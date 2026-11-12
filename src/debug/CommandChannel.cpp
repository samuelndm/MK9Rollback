#include "CommandChannel.h"
#include "AllocWatch.h"
#include "AnimInputTrace.h"
#include "Overlay.h"
#include "../common/StrAppend.h"
#include "../common/MemQuery.h"

#include <windows.h>
#include <tlhelp32.h>

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <deque>
#include <map>
#include <string>
#include <vector>

#include "../game/Fighter.h"
#include "../game/Input.h"
#include "../game/MK9Game.h"
#include "../game/MKHeap.h"
#include "../hook/CodePatch.h"
#include "../hook/ObjectHooks.h"
#include "../hook/Ue3Refs.h"
#include "../rollback/CinemaRestore.h"
#include "../rollback/FpuGuard.h"
#include "../rollback/FramePace.h"
#include "../rollback/FreeQuarantine.h"
#include "../rollback/ParallelMem.h"
#include "../rollback/OnlineKeep.h"
#include "../rollback/RngTrace.h"
#include "../rollback/ScriptRng.h"
#include "../rollback/RenderTag.h"
#include "../rollback/SoundDedupe.h"
#include "../rollback/RollbackManager.h"
#include "../rollback/Snapshot.h"
#include "../rollback/StateHash.h"
#include "CrashLog.h"
#include "Lab.h"
#include "Ue3Names.h"
#include "Logger.h"
#include "../net/OnlineMode.h"
#include "../net/SteamBridge.h"

namespace {
wchar_t gPath[MAX_PATH];
wchar_t gTakenPath[MAX_PATH];  // the command file while the poll thread reads it
HANDLE gThread = nullptr;
volatile LONG gStop = 0;
CRITICAL_SECTION gLock;
std::vector<std::string> gPending;  // filled by poller, drained by game thread

struct Step {
    uint32_t mask;
    uint32_t frames;
};
std::deque<Step> gQueue[4];
std::deque<Step> gHwQueue[6];  // raw hardware pads (hwhold)
struct WatchArg {
    unsigned tid;
    uintptr_t addr;
    bool exec;
} gWatchArg;
DWORD WINAPI ArmWatch(LPVOID p) {
    const WatchArg* a = static_cast<const WatchArg*>(p);
    Logger::Info("WATCH arm %08X on thread %u: %s", (unsigned)a->addr, a->tid,
                 CrashLog::WatchWrite(a->tid, a->addr, a->exec) ? "ok" : "failed");
    return 0;
}
DWORD WINAPI ArmWatchAll(LPVOID p) {
    const WatchArg* a = static_cast<const WatchArg*>(p);
    Logger::Info("WATCH arm %08X on all threads: %d armed", (unsigned)a->addr, CrashLog::WatchWriteAll(a->addr));
    return 0;
}
struct TraceArg {
    unsigned tid;
    uintptr_t addrs[4];
} gTraceArg;
DWORD WINAPI ArmTrace(LPVOID p) {
    const TraceArg* a = static_cast<const TraceArg*>(p);
    Logger::Info("LOCTRACE arm %08X %08X %08X %08X on thread %u: %s", (unsigned)a->addrs[0], (unsigned)a->addrs[1],
                 (unsigned)a->addrs[2], (unsigned)a->addrs[3], a->tid, CrashLog::TraceArm(a->tid, a->addrs) ? "ok" : "failed");
    return 0;
}
uint32_t gStepRemaining = 0;  // fight frames still to run before re-pausing
bool gStepping = false;
// Test harness: destroy the AIDrone MK objects (the only writers of the AI pads, found with a hardware write
// breakpoint at 0x008895FF) so both fighters follow scripted pad input. (Destroying the AI's script executors too did
// not stop AI-native draws and crashed a resimulated tick once; only the drones are destroyed.)
bool gAutoKillAi = false;
void KillAi(bool verbose) {
    std::vector<uint32_t> drones;
    ObjectHooks::ForEachListed([](uint32_t obj, const char* type, void* ctx) {
        if (strstr(type, "AIDrone")) static_cast<std::vector<uint32_t>*>(ctx)->push_back(obj);
    }, &drones);
    for (uint32_t o : drones) {
        const uint32_t dtor = *reinterpret_cast<const uint32_t*>(*reinterpret_cast<const uint32_t*>(o) + 4);
        Logger::Info("KILLAI destroying AIDrone %08X via vt[1]=%08X at gf=%u", o, dtor, *reinterpret_cast<const uint32_t*>(*reinterpret_cast<const uint32_t*>(0x00DEF4B4) + 0x70));
        reinterpret_cast<void*(__thiscall*)(void*, int)>(dtor)(reinterpret_cast<void*>(o), 1);
    }
    if (verbose || !drones.empty()) Logger::Info("KILLAI %u drones destroyed", (unsigned)drones.size());
}
}  // namespace
// Netplay start (both peers, the same frame, before its snapshot): both fighters' inputs come from the session, so
// an AI drone driving a pad is never wanted there.
int CommandChannel::KillAiDrones() {
    std::vector<uint32_t> drones;
    ObjectHooks::ForEachListed([](uint32_t obj, const char* type, void* ctx) {
        if (strstr(type, "AIDrone")) static_cast<std::vector<uint32_t>*>(ctx)->push_back(obj);
    }, &drones);
    if (!drones.empty()) KillAi(false);
    return (int)drones.size();
}
int CommandChannel::CountAiDrones() {
    int n = 0;
    ObjectHooks::ForEachListed([](uint32_t, const char* type, void* ctx) {
        if (strstr(type, "AIDrone")) ++*static_cast<int*>(ctx);
    }, &n);
    return n;
}
namespace {
int gPendingPause = -1, gPendingGamePause = -1;  // -1 none, else value for the pause setter at end of tick
struct PendingDump {
    uint32_t framesLeft;  // fight frames to skip before dumping
    std::string label;
};
std::vector<PendingDump> gDumps;
std::vector<PendingDump> gScheduled;  // "at <n> <cmd>": framesLeft counts fight frames; runs at EngineTick entry
Snapshot::State gSlots[4];
Snapshot::StateAB gSlotsAB[2];
bool gObjHooks = false;
int gWiProbe = 0;
uint8_t gWiCopy[0x800];
uintptr_t gWiAddr = 0;
int gCounterScan = 0;  // 'counterscan' state (CounterScan, near RunFrame)

uintptr_t WorldInfoPtr() {
    const uintptr_t world = *reinterpret_cast<const uint32_t*>(0x00E86AB0);
    const uintptr_t level = world ? *reinterpret_cast<const uint32_t*>(world + 0x48) : 0;
    const uintptr_t actors = level ? *reinterpret_cast<const uint32_t*>(level + 0x38) : 0;
    return actors ? *reinterpret_cast<const uint32_t*>(actors) : 0;
}

// Tick-to-tick diff of the UE3 objects referenced at +8 by live MK anim nodes:
// which offsets change per frame (aggregated over all nodes of each MK type).
int gAnimProbe = 0;
struct AnimCopy {
    uint32_t mkobj, ue3;
    const char* type;
    uint8_t bytes[0x140];
};
std::vector<AnimCopy> gAnimCopies;

void AnimProbe() {
    if (gAnimProbe == 1) {
        gAnimCopies.clear();
        ObjectHooks::ForEachLive(
            [](uint32_t obj, const char* type, void*) {
                if (strncmp(type, "MKAnimNode", 10)) return;
                const uint32_t ue3 = *reinterpret_cast<const uint32_t*>(obj + 8);
                if (ue3 < 0x01000000 || ue3 >= 0xFFFE0000) return;
                AnimCopy c{obj, ue3, type, {}};
                memcpy(c.bytes, reinterpret_cast<const void*>(ue3), sizeof c.bytes);
                gAnimCopies.push_back(c);
            },
            nullptr);
        Logger::Info("ANIMPROBE captured %u MK anim nodes with UE3 counterparts", (unsigned)gAnimCopies.size());
        gAnimProbe = gAnimCopies.empty() ? 0 : 2;
    } else if (gAnimProbe == 2) {
        // key: short type name + offset -> (count, example delta)
        std::map<std::string, std::pair<int, float>> agg;
        for (const AnimCopy& c : gAnimCopies) {
            if (*reinterpret_cast<const uint32_t*>(c.mkobj + 4) == 0xFFFFFFFF) continue;
            for (uint32_t o = 0; o < sizeof c.bytes; o += 4) {
                uint32_t a, b;
                memcpy(&a, c.bytes + o, 4);
                memcpy(&b, reinterpret_cast<const void*>(c.ue3 + o), 4);
                if (a == b) continue;
                float fa, fb;
                memcpy(&fa, &a, 4);
                memcpy(&fb, &b, 4);
                char key[96];
                snprintf(key, sizeof key, "%.*s +0x%03X", (int)strcspn(c.type, "<"), c.type, o);
                auto& e = agg[key];
                ++e.first;
                e.second = fb - fa;
            }
        }
        for (auto& [k, v] : agg) Logger::Info("ANIMPROBE %s changed in %d nodes (example d=%.6f)", k.c_str(), v.first, v.second);
        gAnimProbe = 0;
    }
}

// Tick-to-tick diff of WorldInfo: which dwords change every frame and by how much.
void WorldInfoProbe() {
    if (gWiProbe == 1) {
        gWiAddr = WorldInfoPtr();
        if (gWiAddr) memcpy(gWiCopy, reinterpret_cast<const void*>(gWiAddr), sizeof gWiCopy);
        Logger::Info("WIPROBE WorldInfo at %08X (vtable %08X)", gWiAddr, gWiAddr ? *reinterpret_cast<uint32_t*>(gWiAddr) : 0);
        gWiProbe = gWiAddr ? 2 : 0;
    } else if (gWiProbe == 2) {
        for (uint32_t o = 0; o < sizeof gWiCopy; o += 4) {
            uint32_t a, b;
            memcpy(&a, gWiCopy + o, 4);
            memcpy(&b, reinterpret_cast<const void*>(gWiAddr + o), 4);
            if (a == b) continue;
            float fa, fb;
            memcpy(&fa, &a, 4);
            memcpy(&fb, &b, 4);
            Logger::Info("WIPROBE +0x%03X: %08X -> %08X  (f %.6f -> %.6f, d %.6f)", o, a, b, fa, fb, fb - fa);
        }
        gWiProbe = 0;
    }
}
wchar_t gDir[MAX_PATH];
constexpr uintptr_t kPauseFlag = 0x00E43054;
constexpr uintptr_t kRngState = 0x00E7C054;  // LCG state used by 0x005F5080 / 0x005F50A0

using SetPauseFn = void(__cdecl*)(int);  // 0x00407AB0, writes 0x00E43054
constexpr uintptr_t kSetPause = 0x00407AB0;

// Pause guard: every pause of the game goes through 0x00407AB0 (11 call sites: the engine's request 0x0080E6E0, the
// script native at 0x008C441C, the UI at 0x00944D42, MK's online code 0x00884xxx/0x00886xxx, ...). During a P2P session
// a local pause stopped this game's GGPO frames and the peer gave up after 5 s. While the session runs, the game's own
// pause-on requests are refused; the DLL's pauses (netplay start/stop handshake, 'pause' command) go to the original.
const uint8_t kSetPauseOrig[6] = {0x55, 0x8B, 0xEC, 0x83, 0xEC, 0x0C};  // push ebp; mov ebp,esp; sub esp,0xC
uint8_t* gSetPauseTramp = nullptr;
void CallSetPause(int on) { reinterpret_cast<SetPauseFn>(gSetPauseTramp ? reinterpret_cast<uintptr_t>(gSetPauseTramp) : kSetPause)(on); }
void __cdecl SetPauseGuard(int on) {
    if (on && Rollback::NetplayRunning()) {
        static int refused = 0;
        if (refused++ < 20)
            Logger::Info("PAUSE: the game asked to pause during the netplay session (call from %08X); refused, the session goes on",
                         (unsigned)(uintptr_t)__builtin_return_address(0) - 5);
        return;
    }
    CallSetPause(on);
}

DWORD WINAPI Poll(LPVOID) {
    while (!InterlockedCompareExchange(&gStop, 0, 0)) {
        Sleep(50);
        // Taken by rename first (round 3): read in place and removed afterwards, a file the launcher moved in between
        // the read and the remove was deleted unread (an 'online' command lost, the window still saying "Ready"). The
        // rename is atomic: the launcher's next file is a new one. It replaces a leftover of an earlier read (a game
        // closed inside one), whose commands were for that game.
        if (!MoveFileExW(gPath, gTakenPath, MOVEFILE_REPLACE_EXISTING)) continue;  // nothing waiting, or still being written
        FILE* f = _wfopen(gTakenPath, L"r");
        if (!f) continue;
        std::vector<std::string> lines;
        char buf[256];
        while (fgets(buf, sizeof buf, f)) {
            size_t n = strcspn(buf, "\r\n");
            buf[n] = 0;
            if (n) lines.emplace_back(buf);
        }
        fclose(f);
        _wremove(gTakenPath);
        EnterCriticalSection(&gLock);
        for (auto& l : lines) gPending.push_back(l);
        LeaveCriticalSection(&gLock);
    }
    return 0;
}

void Execute(const std::string& line) {
    char verb[16] = {};
    sscanf(line.c_str(), "%15s", verb);
    int player = 0, frames = 0, value = 0;
    unsigned mask = 0;
    if (!strcmp(verb, "hwhold") && sscanf(line.c_str(), "hwhold %d %x %d", &player, &mask, &frames) == 3 && player >= 0 &&
        player < 6 && frames > 0) {
        gHwQueue[player].push_back({mask, (uint32_t)frames});
    } else if (!strcmp(verb, "hold") && sscanf(line.c_str(), "hold %d %x %d", &player, &mask, &frames) == 3 &&
        player >= 1 && player <= 4 && frames > 0) {
        gQueue[player - 1].push_back({mask, (uint32_t)frames});
    } else if (!strcmp(verb, "release") && sscanf(line.c_str(), "release %d", &player) == 1 && player >= 1 &&
               player <= 4) {
        gQueue[player - 1].clear();
        Input::ReleasePlayerInput(player - 1);
    } else if (!strcmp(verb, "pause") && sscanf(line.c_str(), "pause %d", &value) == 1) {
        gPendingPause = value ? 1 : 0;  // applied at the end of this tick (frame boundary)
    } else if (!strcmp(verb, "gamepause") && sscanf(line.c_str(), "gamepause %d", &value) == 1) {
        gPendingGamePause = value ? 1 : 0;  // test: the pause setter called the way the game calls it (pause guard)
    } else if (!strcmp(verb, "step") && sscanf(line.c_str(), "step %d", &value) == 1 && value > 0) {
        if (!*reinterpret_cast<const uint32_t*>(kPauseFlag)) {
            Logger::Info("CMD step refused: game is not paused");
            return;
        }
        gStepRemaining += (uint32_t)value;
    } else if (!strcmp(verb, "rngset") && sscanf(line.c_str(), "rngset %x", &mask) == 1) {
        *reinterpret_cast<volatile uint32_t*>(kRngState) = mask;  // game thread, before this tick's fight update
    } else if ((!strcmp(verb, "save") || !strcmp(verb, "load")) && sscanf(line.c_str(), "%*s %d", &value) == 1 &&
               value >= 0 && value < 4) {
        const uint32_t gf = *reinterpret_cast<const uint32_t*>(*reinterpret_cast<const uint32_t*>(0x00DEF4B4) + 0x70);
        if (verb[0] == 's') {
            Snapshot::Save(&gSlots[value], gf);
            size_t bytes = 0, keeps = 0;
            for (auto& s : gSlots[value].spans) { bytes += s.size; keeps += s.keepOffsets.size(); }
            Logger::Info("SNAPSHOT save slot %d gf=%u spans=%u bytes=%u pointer-kept=%u", value, gf,
                         (unsigned)gSlots[value].spans.size(), (unsigned)bytes, (unsigned)keeps);
        } else if (gSlots[value].spans.empty()) {
            Logger::Info("SNAPSHOT load slot %d: empty", value);
            return;
        } else {
            const int n = Snapshot::Load(gSlots[value]);
            Logger::Info("SNAPSHOT load slot %d at gf=%u -> gf=%u, restored %d/%u spans", value, gf,
                         gSlots[value].gameFrame, n, (unsigned)gSlots[value].spans.size());
        }
    } else if (!strcmp(verb, "objhooks") && sscanf(line.c_str(), "objhooks %d", &value) == 1) {
        if (value && !gObjHooks) {
            gObjHooks = ObjectHooks::Install();
        } else if (!value && gObjHooks) {
            ObjectHooks::Uninstall();
            gObjHooks = false;
        }
        Logger::Info("OBJHOOKS %s", gObjHooks ? "installed" : "off");
    } else if (!strcmp(verb, "rbmode") && sscanf(line.c_str(), "rbmode %d", &value) == 1) {
        // Lockstep needs no object tracking (it never restores); rollback does (Rollback::Enable installs it then).
        Rollback::Enable(value != 0);
        gObjHooks = ObjectHooks::Installed();
    } else if (!strcmp(verb, "netcode")) {
        // 'netcode safe|rollback': the launcher's mode in one command (lockstep at delay N or rollback), with the patch
        // set of that mode only. Replaces 'objhooks 1' + 'rbmode 1' + 'ggpo lockstep N' for players.
        char mode[16] = {};
        sscanf(line.c_str(), "netcode %15s", mode);
        const bool safe = !strcmp(mode, "safe");
        if (!safe && strcmp(mode, "rollback")) {
            Logger::Info("NETCODE: unknown mode '%s' (safe|rollback)", mode);
        } else {
            Rollback::SetLockstep(safe);
            Rollback::Enable(true);
            gObjHooks = ObjectHooks::Installed();
        }
    } else if (!strcmp(verb, "lab") && sscanf(line.c_str(), "lab %d", &value) == 1) {
        Lab::gOn = value != 0;
        Rollback::gDiag = value != 0;
        Logger::Info("LAB %d: diagnostics %s (patch sets are chosen at install: send before objhooks/rbmode/netcode)", value != 0,
                     value ? "on" : "off");
    } else if (!strcmp(verb, "rollback") && sscanf(line.c_str(), "rollback %d", &value) == 1) {
        Rollback::RequestRollback(value);
    } else if (!strcmp(verb, "fastprobe") && sscanf(line.c_str(), "fastprobe %d", &value) == 1) {
        Snapshot::gFastProbe = value != 0;
        Logger::Info("FASTPROBE %d: the save's skel phase checks reads with %s", value != 0, value ? "guarded loads" : "VirtualQuery");
    } else if (!strcmp(verb, "diffmerge") && sscanf(line.c_str(), "diffmerge %d", &value) == 1) {
        Snapshot::gDiffMerge = value < 0 ? 0 : value > 2 ? 2 : value;
        Logger::Info("DIFFMERGE %d: the load's pointer-safe span merge %s", Snapshot::gDiffMerge,
                     Snapshot::gDiffMerge == 0   ? "classifies every dword (before rc25)"
                     : Snapshot::gDiffMerge == 1 ? "classifies only the dwords that differ"
                                                 : "classifies only the differing dwords and checks each span against the full merge");
    } else if (!strcmp(verb, "heaplock") && sscanf(line.c_str(), "heaplock %d", &value) == 1) {
        Snapshot::gHeapLock = value != 0;
    } else if (!strcmp(verb, "namemaps") && sscanf(line.c_str(), "namemaps %d", &value) == 1) {
        Snapshot::gNameMaps = value != 0;
    } else if (!strcmp(verb, "abtest")) {
        char nm[32] = {};
        sscanf(line.c_str(), "abtest %31s", nm);
        Rollback::AbTest(nm);
    } else if (!strcmp(verb, "fixseed") && sscanf(line.c_str(), "fixseed %x", &mask) == 1) {
        Rollback::SetFixSeed(mask);
    } else if (!strcmp(verb, "complistrebase") && sscanf(line.c_str(), "complistrebase %d", &value) == 1) {
        Snapshot::gCompListRebase = value != 0;
    } else if (!strcmp(verb, "complist") && sscanf(line.c_str(), "complist %d", &value) == 1) {
        Snapshot::gCompList = value != 0;
    } else if (!strcmp(verb, "bonerot") && sscanf(line.c_str(), "bonerot %d", &value) == 1) {
        Snapshot::gBoneRotation = value != 0;
    } else if (!strcmp(verb, "propskel") && sscanf(line.c_str(), "propskel %d", &value) == 1) {
        Snapshot::gPropSkelLoad = value != 0;
        Logger::Info("PROPSKEL %d: prop component poses %s at loads", value != 0, value ? "restored" : "kept live");
    } else if (!strcmp(verb, "overlay") && sscanf(line.c_str(), "overlay %d", &value) == 1) {
        // In-game status line: 0 off, 1 while netplay is armed / running / just stopped (default), 2 verbose, always.
        Overlay::SetLevel(value);
        if (value > 0) Overlay::EnsureInstalled();
        Logger::Info("OVERLAY level %d", Overlay::Level());
    } else if (!strcmp(verb, "huddemo") && sscanf(line.c_str(), "huddemo %d", &value) == 1) {
        // The HUD display's canned states (Hud::Demo), for screenshots without an online match: 0 = off. The counters
        // are logged first: the draw time of the state that was on screen until now.
        Overlay::LogCounters();
        Overlay::SetDemo(value);
        if (value > 0) Overlay::EnsureInstalled();
        Logger::Info("HUDDEMO %d (0: off, the published state is shown)", Overlay::Demo());
    } else if (!strcmp(verb, "hudshot")) {
        // The next finished frame as a BMP (the harness looks at the display's place; works with the PC locked).
        char path[MAX_PATH] = "";
        if (sscanf(line.c_str(), "hudshot %259[^\r\n]", path) == 1) {
            Overlay::EnsureInstalled();
            Overlay::RequestShot(path);
            Logger::Info("HUDSHOT wanted");
        }
    } else if (!strcmp(verb, "framelatency") && sscanf(line.c_str(), "framelatency %d", &value) == 1) {
        Overlay::SetFrameLatency(value);
        Logger::Info("FRAMELATENCY %d (0: the game's own; applied at the next drawn frame)", Overlay::FrameLatencyWanted());
    } else if (!strcmp(verb, "learnfields") && sscanf(line.c_str(), "learnfields %d", &value) == 1) {
        Snapshot::gLearnFields = value;
    } else if (!strcmp(verb, "poscache") && sscanf(line.c_str(), "poscache %d", &value) == 1) {
        Snapshot::gPositiveCache = value != 0;
    } else if (!strcmp(verb, "heapcache") && sscanf(line.c_str(), "heapcache %d", &value) == 1) {
        Snapshot::SetCommittedCap(value);
    } else if (!strcmp(verb, "slotnt") && sscanf(line.c_str(), "slotnt %d", &value) == 1) {
        ParallelMem::SetSlotStreaming(value != 0);
    } else if (!strcmp(verb, "floatfields") && sscanf(line.c_str(), "floatfields %d", &value) == 1) {
        Snapshot::gFloatFields = value != 0;
    } else if (!strcmp(verb, "strictvt") && sscanf(line.c_str(), "strictvt %d", &value) == 1) {
        Snapshot::gStrictVtable = value != 0;
    } else if (!strcmp(verb, "foreignvt") && sscanf(line.c_str(), "foreignvt %d", &value) == 1) {
        Snapshot::gForeignVtables = value != 0;
    } else if (!strcmp(verb, "coldcheck") && sscanf(line.c_str(), "coldcheck %d", &value) == 1) {
        Snapshot::gColdCheck = value != 0;
        Logger::Info("COLDCHECK %d: pointer targets on pages outside the working set are %s", value != 0,
                     value ? "kept live unread" : "read");
    } else if (!strcmp(verb, "trimws") && sscanf(line.c_str(), "trimws %d", &value) == 1) {
        // Test aid: empty this process's working set every <value> ms on the game thread (0: off), like Windows trimming
        // a minimized game; the pages come back as soft faults, or from the disk once repurposed.
        Snapshot::gTrimWsMs = value > 0 ? value : 0;
        Logger::Info("TRIMWS %d ms", Snapshot::gTrimWsMs);
    } else if (!strcmp(verb, "pawncomps") && sscanf(line.c_str(), "pawncomps %d", &value) == 1) {
        Snapshot::gPawnComps = value != 0;
    } else if (!strcmp(verb, "tierecam") && sscanf(line.c_str(), "tierecam %d", &value) == 1) {
        Snapshot::gTierECamera = value != 0;
    } else if (!strcmp(verb, "cinewatch") && sscanf(line.c_str(), "cinewatch %d", &value) == 1) {
        Snapshot::gCineWatch = value != 0 && Lab::On();  // the cinematic objects and fighter slots per frame (frames log)
        Logger::Info("CINEWATCH %s", Snapshot::gCineWatch ? "on" : "off (needs 'lab 1')");
    } else if (!strcmp(verb, "cinema") && sscanf(line.c_str(), "cinema %d", &value) == 1) {
        CinemaRestore::gRestore = value != 0;  // the cinematic objects restored at loads (saves and deferral run either way)
        Logger::Info("CINEMA restore %s", CinemaRestore::gRestore ? "on" : "off");
    } else if (!strcmp(verb, "cinemadeadok") && sscanf(line.c_str(), "cinemadeadok %d", &value) == 1) {
        CinemaRestore::gDeadOk = value != 0;  // object fields dangling at the save already do not block the restore
        Logger::Info("CINEMADEADOK %d", value != 0 ? 1 : 0);
    } else if (!strcmp(verb, "cinemadeadrefs") && sscanf(line.c_str(), "cinemadeadrefs %d", &value) == 1) {
        CinemaRestore::gDeadRefs = value != 0;  // dead references cleared at loads instead of blocking the restore
        Logger::Info("CINEMADEADREFS %d", value != 0 ? 1 : 0);
    } else if (!strcmp(verb, "demodamage") && sscanf(line.c_str(), "demodamage %d", &value) == 1) {
        // Test harness: damage in attract-demo fights. Every harness fight had full life throughout (2026-11-07): no KO,
        // low-health or round-end state ever ran under rollback in the harness. The damage function 0x008988F0 adds the
        // damage to life at 0x00898A4D unless bit 0 of [[0xDEF4B4]+0x68] is set (test at 0x00898A47, jne at 0x00898A4B);
        // in attract demos it is (the demo flow clears it at 0x009568F6; other readers 0x004071C0, 0x004072C8, 0x00810A8C
        // skip pause / fight-flow handling), and bit 0 of +0x10 (the mode table's override) is already set there
        // (000C0001). Only that one jump is patched (2 NOPs), so the rest of the demo flow is unchanged. Code, not state:
        // both passes of a replay run it the same way.
        static const uint8_t kJne[2] = {0x75, 0x28}, kNops[2] = {0x90, 0x90};
        const uint32_t opts = *reinterpret_cast<const uint32_t*>(0x00DEF4B4);
        const uint32_t mode = opts ? *reinterpret_cast<const uint32_t*>(opts + 8) : 0xFFFFFFFF;
        const uint32_t f68 = opts ? *reinterpret_cast<const uint32_t*>(opts + 0x68) : 0;
        const bool ok = value ? CodePatch::Write(0x00898A4B, kJne, kNops, 2, "attract-demo damage (demodamage)")
                              : CodePatch::Restore(0x00898A4B);
        Logger::Info("DEMODAMAGE %d: %s (game mode %u, options +0x68 %08X)", value ? 1 : 0, ok ? "done" : "no change", mode, f68);
    } else if (!strcmp(verb, "netplaymeter")) {
        // Test harness (online X-ray tests): 'netplaymeter 1' = both meters full at a fixed frame of each local netplay
        // session, written inside the frame loop (live and replays); 'netplaymeter -1' = off.
        float f = -1.0f;
        if (sscanf(line.c_str(), "netplaymeter %f", &f) == 1) Rollback::SetNetplayMeter(f > 1.0f ? 1.0f : f);
    } else if (!strcmp(verb, "netplaylife")) {
        // Test harness (KOs under rollback): 'netplaylife 0.3' = both fighters' life 0.3 at the same frame; -1 = off.
        float f = -1.0f;
        if (sscanf(line.c_str(), "netplaylife %f", &f) == 1) Rollback::SetNetplayLife(f > 1.0f ? 1.0f : f);
    } else if (!strcmp(verb, "protectlate") && sscanf(line.c_str(), "protectlate %d", &value) == 1) {
        FreeQuarantine::SetProtectLate(value != 0);  // requeued blocks the cinema restore points at again stay live
    } else if (!strcmp(verb, "memthreads") && sscanf(line.c_str(), "memthreads %d", &value) == 1) {
        ParallelMem::SetHelpers(value);  // helpers of the saves' and loads' big copies and scans: 0 none, -1 from the CPU count
    } else if (!strcmp(verb, "levelredo") && sscanf(line.c_str(), "levelredo %d", &value) == 1) {
        CinemaRestore::gLevelRedo = value != 0;  // levels added after a load point re-added at their next lookup
        Logger::Info("LEVELREDO %d", value != 0 ? 1 : 0);
    } else if (!strcmp(verb, "kismettrace") && sscanf(line.c_str(), "kismettrace %d", &value) == 1) {
        CinemaRestore::Trace(value);  // the next N frames: Kismet/cinema object changes per save, first pass vs replay
        Logger::Info("KISMETTRACE %d frames", value);
    } else if (!strcmp(verb, "sessiondelay") && sscanf(line.c_str(), "sessiondelay %d", &value) == 1) {
        // The running P2P session's local input delay, mid-session (the online mode's between-rounds change, for tests).
        if (!Rollback::SetSessionDelay(value)) Logger::Info("SESSIONDELAY %d: no running P2P session (or a delay outside 1-8)", value);
    } else if (!strcmp(verb, "onlinekeep") && sscanf(line.c_str(), "onlinekeep %d", &value) == 1) {
        // MK9's online memory (MK heap #5) across loads: 0 off, 1 measured in online sessions (default), 2 kept live in
        // online sessions, 3 kept live in every session (lab).
        OnlineKeep::SetMode(value == 3 && !Lab::On() ? 2 : value);
        const char* const what[4] = {"off: MK heap #5 rewound with every load", "measured in online sessions (still rewound)",
                                     "kept live in online sessions", "kept live in every session"};
        Logger::Info("ONLINEKEEP mode %d (%s)", OnlineKeep::Mode(), what[OnlineKeep::Mode()]);
    } else if (!strcmp(verb, "poke") && Lab::On()) {
        // 'poke <hex address> <hex dword>' (lab): a test write, e.g. a full meter: PlayerInfo (0x00E89D7C + 0xC8C * p)
        // + 0xC14 = 3F800000 (1.0f).
        unsigned addr = 0, v = 0;
        if (sscanf(line.c_str(), "poke %x %x", &addr, &v) == 2 && addr >= 0x10000 && MemQuery::Writable(addr, 4)) {
            *reinterpret_cast<volatile uint32_t*>(addr) = v;
            Logger::Info("POKE %08X = %08X", addr, v);
        } else {
            Logger::Info("POKE refused (address not writable): %s", line.c_str());
        }
    } else if (!strcmp(verb, "tierenodes") && sscanf(line.c_str(), "tierenodes %d", &value) == 1) {
        Snapshot::gTierENodes = value != 0;
    } else if (!strcmp(verb, "compspan") && sscanf(line.c_str(), "compspan %d", &value) == 1) {
        Snapshot::gCompSpan = value != 0;
    } else if (!strcmp(verb, "memcache") && sscanf(line.c_str(), "memcache %d", &value) == 1) {
        MemQuery::gEnabled = value != 0;
    } else if (!strcmp(verb, "heapdefer") && sscanf(line.c_str(), "heapdefer %d", &value) == 1) {
        FreeQuarantine::SetHeapDefer(value != 0);
    } else if (!strcmp(verb, "allocwatch") && sscanf(line.c_str(), "allocwatch %d", &value) == 1) {
        if (value) AllocWatch::Install();  // ntdll inline hooks: large reservations by caller chain (MEMORY report)
        else AllocWatch::Uninstall();
    } else if (!strcmp(verb, "unfreeall") && sscanf(line.c_str(), "unfreeall %d", &value) == 1) {
        FreeQuarantine::SetUnfreeAll(value != 0);
    } else if (!strcmp(verb, "ggpo")) {
        int n = 0;
        unsigned lp = 0, rp = 0;
        int player = 1, delay = 0;
        char ip[32] = {};
        if (sscanf(line.c_str(), "ggpo lockstep %d", &n) == 1) {
            Rollback::SetLockstep(n != 0);
            if (!n && Rollback::Enabled()) Rollback::Enable(true);  // a lockstep-only patch set gets the rollback modules
            gObjHooks = ObjectHooks::Installed();
        }
        else if (sscanf(line.c_str(), "ggpo synctest %d", &n) == 1 && n > 0) Rollback::GgpoCommand(n);
        else if (sscanf(line.c_str(), "ggpo p2p %u %31s %u %d %d", &lp, ip, &rp, &player, &delay) >= 4)
            Rollback::GgpoP2PCommand((unsigned short)lp, ip, (unsigned short)rp, player, delay);
        else if (sscanf(line.c_str(), "ggpo netplay %u %31s %u %d %d", &lp, ip, &rp, &player, &delay) >= 4)
            Rollback::NetplayArm((unsigned short)lp, ip, (unsigned short)rp, player, delay);
        else if (unsigned long long peer = 0; sscanf(line.c_str(), "ggpo steam %llu %d %d", &peer, &player, &delay) >= 2)
            Rollback::GgpoSteamCommand(peer, player, delay, false);
        else if (unsigned long long peer = 0; sscanf(line.c_str(), "ggpo steamnetplay %llu %d %d", &peer, &player, &delay) >= 2)
            Rollback::GgpoSteamCommand(peer, player, delay, true);
        else Rollback::GgpoCommand(0);
    } else if (!strcmp(verb, "steamid")) {
        Logger::Info("STEAM: interfaces %s, this account's SteamID64 = %llu", SteamBridge::Available() ? "available" : "unavailable",
                     (unsigned long long)SteamBridge::SelfId());
    } else if (!strcmp(verb, "online")) {  // 'online <SteamID64> [delay|ping]' / 'online off': rollback in MK9's online fights
        unsigned long long peer = 0;
        int delay = 1;
        char arg[16] = {};
        if (!strncmp(line.c_str(), "online auto", 11)) {  // 'online auto [delay|ping]': any opponent running the mod
            sscanf(line.c_str(), "online auto %15s", arg);
            peer = Online::kAutoPeer;
        } else {
            sscanf(line.c_str(), "online %llu %15s", &peer, arg);
        }
        const bool ping = !strcmp(arg, "ping");  // the delay follows the measured round trip (the launcher's default)
        if (arg[0] && !ping) delay = atoi(arg);
        Online::Command(peer, delay, ping);
    } else if (!strcmp(verb, "onlinescan")) {
        Online::TestScan();
    } else if (!strcmp(verb, "watchchange")) {
        // 'watchchange <hex address>': the online mode's change watch on any address (test: the host frame E89BDC);
        // 'watchchange 0' disarms.
        unsigned addr = 0;
        sscanf(line.c_str(), "watchchange %x", &addr);
        const uintptr_t a[4] = {addr, 0, 0, 0};
        const char* const names[4] = {"test address", nullptr, nullptr, nullptr};
        CrashLog::ChangeWatchArm(a, names);
    } else if (!strcmp(verb, "fakerollback")) {
        int count = 0;
        if (sscanf(line.c_str(), "fakerollback %d %x %d", &value, &mask, &count) != 3) {
            Logger::Info("CMD ignored: '%s'", line.c_str());
            return;
        }
        Rollback::RequestFakeRollback(value, mask, count);
    } else if (!strcmp(verb, "whatis") && sscanf(line.c_str(), "whatis %x", &mask) == 1) {
        // Walk back from an address to the nearest MK object header (vtable whose vt[0]
        // is "mov eax, desc; ret"), report its type chain and the offset, and dump 0x40 bytes.
        for (uint32_t a = mask & ~3u; a + 0x20000 > mask && a > 0x10000; a -= 4) {
            const uint32_t vt = *reinterpret_cast<const uint32_t*>(a);
            if (vt < 0x00C40000 || vt >= 0x00DA0000) continue;
            const uint32_t fn = *reinterpret_cast<const uint32_t*>(vt);
            if (fn < 0x00401000 || fn >= 0x00F9C000) continue;
            const uint8_t* p = reinterpret_cast<const uint8_t*>(fn);
            if (p[0] != 0xB8 || p[5] != 0xC3) continue;
            uint32_t desc;
            memcpy(&desc, p + 1, 4);
            if (desc < 0x00DE0000 || desc >= 0x00E00000) continue;
            const char* name = reinterpret_cast<const char*>(*reinterpret_cast<const uint32_t*>(desc));
            Logger::Info("WHATIS %08X: inside %s object at %08X (+0x%X), id=%08X, type size 0x%X", mask, name, a,
                         mask - a, *reinterpret_cast<const uint32_t*>(a + 4), *reinterpret_cast<const uint32_t*>(desc + 4));
            break;
        }
        for (uint32_t o = 0; o < 0x40; o += 16) {
            const uint32_t* q = reinterpret_cast<const uint32_t*>((mask & ~3u) + o);
            Logger::Info("  %08X: %08X %08X %08X %08X", (mask & ~3u) + o, q[0], q[1], q[2], q[3]);
        }
    } else if (!strcmp(verb, "spanlist")) {
        // Current snapshot coverage -> <dll dir>\spans.txt ("base size name" per line), used by
        // memscan to subtract covered memory from a frame's write set.
        static Snapshot::StateAB probe;
        char why[160] = {};
        if (!Snapshot::SaveAB(&probe, 0, why, sizeof why)) {
            Logger::Info("SPANLIST failed: %s", why);
            return;
        }
        wchar_t path[MAX_PATH];
        swprintf(path, MAX_PATH, L"%ls\\spans.txt", gDir);
        if (FILE* f = _wfopen(path, L"w")) {
            for (auto& s : probe.spans) fprintf(f, "%08X %08X %s\n", (unsigned)s.base, s.size, s.name);
            fclose(f);
        }
        Logger::Info("SPANLIST wrote %u spans", (unsigned)probe.spans.size());
    } else if (!strcmp(verb, "loadonly") && sscanf(line.c_str(), "loadonly %d", &value) == 1 && value > 0 && value < 12) {
        Rollback::RequestLoadOnly(value);
    } else if (!strcmp(verb, "ownranges")) {
        wchar_t path[MAX_PATH];
        swprintf(path, MAX_PATH, L"%ls\\ownranges.txt", gDir);
        if (FILE* f = _wfopen(path, L"w")) {
            Snapshot::WriteOwnRanges(f);
            Logger::Info("OWNRANGES written (%ld bytes)", ftell(f));
            fclose(f);
        }
    } else if (!strcmp(verb, "describefile")) {
        // Reads describe_in.txt (one hex address per line), writes describe_out.txt with the owner.
        wchar_t in[MAX_PATH], outp[MAX_PATH];
        swprintf(in, MAX_PATH, L"%ls\\describe_in.txt", gDir);
        swprintf(outp, MAX_PATH, L"%ls\\describe_out.txt", gDir);
        std::vector<std::pair<uint32_t, const char*>> objs;  // listed MK objects
        ObjectHooks::ForEachListed([](uint32_t obj, const char* type, void* ctx) {
            static_cast<std::vector<std::pair<uint32_t, const char*>>*>(ctx)->push_back({obj, type});
        }, &objs);
        std::sort(objs.begin(), objs.end());
        FILE* fi = _wfopen(in, L"r");
        FILE* fo = _wfopen(outp, L"w");
        char buf[64];
        unsigned lastChunk = 0, described = 0;
        while (fi && fo && fgets(buf, sizeof buf, fi) && described < 20000) {
            unsigned a;
            if (sscanf(buf, "%x", &a) != 1 || (a >> 8) == lastChunk) continue;  // one per 256 bytes
            lastChunk = a >> 8;
            ++described;
            char desc[200] = "?";
            if (a >= 0x00400000 && a < 0x010AE000) {
                snprintf(desc, sizeof desc, "image");
            } else {
                auto it = std::upper_bound(objs.begin(), objs.end(), std::make_pair(a, (const char*)nullptr),
                                           [](const auto& x, const auto& y) { return x.first < y.first; });
                bool mk = false;
                if (it != objs.begin()) {
                    --it;
                    const uint32_t vt = *reinterpret_cast<const uint32_t*>(it->first);
                    const uint8_t* fn = reinterpret_cast<const uint8_t*>(*reinterpret_cast<const uint32_t*>(vt));
                    uint32_t d = 0, size = 0;
                    memcpy(&d, fn + 1, 4);
                    size = *reinterpret_cast<const uint32_t*>(d + 4);
                    if (a < it->first + size) {
                        snprintf(desc, sizeof desc, "MK %s+0x%X", it->second, a - it->first);
                        mk = true;
                    }
                }
                if (!mk) Ue3Names::Describe(a, desc, sizeof desc);
            }
            fprintf(fo, "%08X %s\n", a, desc);
        }
        if (fi) fclose(fi);
        if (fo) fclose(fo);
    } else if (!strcmp(verb, "findclass")) {
        char prefix[64] = {};
        if (sscanf(line.c_str(), "findclass %63s", prefix) == 1) {
            const uint32_t data = *reinterpret_cast<const uint32_t*>(0x00E7C0EC), count = *reinterpret_cast<const uint32_t*>(0x00E7C0F0);
            int shown = 0;
            for (uint32_t i = 0; i < count && shown < 30; ++i) {
                const uint32_t o = *reinterpret_cast<const uint32_t*>(data + 4 * i);
                if (!o) continue;
                char cls[80];
                Ue3Names::NameOf(*reinterpret_cast<const uint32_t*>(o + 0x30), cls, sizeof cls);
                if (strncmp(cls, prefix, strlen(prefix))) continue;
                char name[80], o1[80], o2[80], o3[80];
                const uint32_t p1 = *reinterpret_cast<const uint32_t*>(o + 0x24);
                const uint32_t p2 = p1 ? *reinterpret_cast<const uint32_t*>(p1 + 0x24) : 0;
                const uint32_t p3 = p2 ? *reinterpret_cast<const uint32_t*>(p2 + 0x24) : 0;
                Ue3Names::NameOf(o, name, sizeof name);
                Ue3Names::NameOf(p1, o1, sizeof o1);
                Ue3Names::NameOf(p2, o2, sizeof o2);
                Ue3Names::NameOf(p3, o3, sizeof o3);
                uint32_t info[2] = {0, 0};
                CrashLog::SafeBlockInfo(reinterpret_cast<const void*>(o), info);
                Logger::Info("FINDCLASS %08X %s (%s) block %u <- %08X %s <- %08X %s <- %08X %s", o, name, cls, info[1], p1, o1, p2, o2,
                             p3, o3);
                ++shown;
            }
            uint32_t comps[2];
            for (int p = 0; p < 2; ++p) {
                const uint32_t ob = *reinterpret_cast<const uint32_t*>(0x00E89D7C + p * 0xC8C + 0x1C);
                comps[p] = ob ? *reinterpret_cast<const uint32_t*>(ob + 0x10C) : 0;
            }
            Logger::Info("FINDCLASS fighter skel comps %08X %08X", comps[0], comps[1]);
        }
    } else if (!strcmp(verb, "animtime")) {
        // Fighter animation position as scripts read it (native 0x008C80F0 -> 0x00442370):
        // channel = 0x0043B070(fighter, [[[fighter+0x10C]+0x280]+0xD0]); node = [channel+8] or
        // [channel+0xC]; time = float [node+0x74].
        for (int p = 0; p < 2; ++p) {
            const uint32_t ob = *reinterpret_cast<const uint32_t*>(0x00E89D7C + p * 0xC8C + 0x1C);
            if (!ob) continue;
            const uint32_t comp = *reinterpret_cast<const uint32_t*>(ob + 0x10C);
            const uint32_t mesh = comp ? *reinterpret_cast<const uint32_t*>(comp + 0x280) : 0;
            const uint32_t idx = mesh ? *reinterpret_cast<const uint32_t*>(mesh + 0xD0) : 0;
            using LookupFn = uint32_t(__thiscall*)(uint32_t, uint32_t);
            const uint32_t ch = reinterpret_cast<LookupFn>(0x0043B070)(ob, idx);
            const uint32_t a = ch ? *reinterpret_cast<const uint32_t*>(ch + 8) : 0, b = ch ? *reinterpret_cast<const uint32_t*>(ch + 0xC) : 0;
            char da[160] = "?", db[160] = "?", dc[160] = "?";
            Ue3Names::Describe(a, da, sizeof da);
            Ue3Names::Describe(b, db, sizeof db);
            Ue3Names::Describe(ch, dc, sizeof dc);
            float ta = 0, tb = 0;
            if (a) memcpy(&ta, reinterpret_cast<const void*>(a + 0x74), 4);
            if (b) memcpy(&tb, reinterpret_cast<const void*>(b + 0x74), 4);
            Logger::Info("ANIMTIME P%d fighter %08X idx %u channel %08X (%s, MK %s) node8 %08X (%s, MK %s) t=%.4f node12 %08X (%s) t=%.4f", p + 1,
                         ob, idx, ch, dc, ObjectHooks::TypeOf(ch), a, da, ObjectHooks::TypeOf(a), ta, b, db, tb);
        }
    } else if (!strcmp(verb, "classa")) {
        std::vector<std::pair<uintptr_t, uint32_t>> cells;
        Ue3Refs::CollectCells(cells);
        int shown = 0;
        for (auto& cl : cells) {
            if (cl.second != 12 || shown >= 12) continue;  // list headers at instance+0xB4
            const uint32_t inst = (uint32_t)cl.first - 0xB4;
            char name[80], cls[80];
            Ue3Names::NameOf(inst, name, sizeof name);
            Ue3Names::NameOf(*reinterpret_cast<const uint32_t*>(inst + 0x30), cls, sizeof cls);
            uint32_t info[2] = {0, 0};
            CrashLog::SafeBlockInfo(reinterpret_cast<const void*>(inst), info);
            Logger::Info("CLASSA %08X %s class %s block %u outer %08X", inst, name, cls, info[1],
                         *reinterpret_cast<const uint32_t*>(inst + 0x24));
            ++shown;
        }
    } else if (!strcmp(verb, "killobj") && sscanf(line.c_str(), "killobj %x", &mask) == 1) {
        // Test harness: destroy one live MK object (listed in the global node list) via vt[1].
        bool found = false;
        ObjectHooks::ForEachListed([](uint32_t obj, const char*, void* ctx) {
            if (obj == *static_cast<uint32_t*>(static_cast<void**>(ctx)[0])) *static_cast<bool*>(static_cast<void**>(ctx)[1]) = true;
        }, std::array<void*, 2>{&mask, &found}.data());
        if (found) {
            const uint32_t dtor = *reinterpret_cast<const uint32_t*>(*reinterpret_cast<const uint32_t*>(mask) + 4);
            Logger::Info("KILLOBJ %08X via vt[1]=%08X", mask, dtor);
            reinterpret_cast<void*(__thiscall*)(void*, int)>(dtor)(reinterpret_cast<void*>(mask), 1);
        } else {
            Logger::Info("KILLOBJ %08X: not a live listed MK object", mask);
        }
    } else if (!strcmp(verb, "killai")) {
        // 'killai 2': from now on at every EngineTick exit (two-instance netplay: drones appear after the session
        // start, at the same simulated frame in both games); 'killai 0' stops that.
        // A plain 'killai' (mode 1) leaves the automatic mode as it is: it used to turn it off, so from round 2 on the
        // drones the game re-created were only removed by the harness's next command, on different frames in the two
        // games (two-instance regression 2026-10-25: gf 100000 on A, 100009 on B; fight 2 split at frame 1).
        const int mode = sscanf(line.c_str(), "killai %d", &value) == 1 ? value : 1;
        if (mode == 2) gAutoKillAi = true;
        else if (mode == 0) gAutoKillAi = false;
        if (mode) KillAi(true);
    } else if (!strcmp(verb, "listtypes")) {
        std::map<std::string, int> counts;
        ObjectHooks::ForEachListed([](uint32_t, const char* type, void* ctx) { (*static_cast<std::map<std::string, int>*>(ctx))[type]++; },
                                   &counts);
        for (auto& [t, n] : counts) Logger::Info("TYPES %4d %s", n, t.c_str());
    } else if (!strcmp(verb, "skelprobe")) {
        // For every live MK object whose type chain contains MKSkeletalObj: is
        // [[obj+0x10C]+0xB4] -> [holder] an MK object (component-owned MK anim tree)?
        int shown = 0;
        ObjectHooks::ForEachListed(
            [](uint32_t obj, const char* type, void* ctx) {
                int& shown = *static_cast<int*>(ctx);
                if (!strstr(type, "MKSkeletalObj") || shown >= 12) return;
                const uint32_t comp = *reinterpret_cast<const uint32_t*>(obj + 0x10C);
                uint32_t holder = 0, tree = 0, treeVt = 0;
                MEMORY_BASIC_INFORMATION mbi;
                if (comp > 0x10000 && VirtualQuery(reinterpret_cast<void*>(comp), &mbi, sizeof mbi) && mbi.State == MEM_COMMIT) {
                    holder = *reinterpret_cast<const uint32_t*>(comp + 0xB4);
                    if (holder > 0x10000 && VirtualQuery(reinterpret_cast<void*>(holder), &mbi, sizeof mbi) && mbi.State == MEM_COMMIT) {
                        tree = *reinterpret_cast<const uint32_t*>(holder);
                        if (tree > 0x10000 && VirtualQuery(reinterpret_cast<void*>(tree), &mbi, sizeof mbi) && mbi.State == MEM_COMMIT)
                            treeVt = *reinterpret_cast<const uint32_t*>(tree);
                    }
                }
                Logger::Info("SKELPROBE %s obj=%08X comp(+0x10C)=%08X holder(+0xB4)=%08X tree=%08X treeVt=%08X", type, obj,
                             comp, holder, tree, treeVt);
                ++shown;
            },
            &shown);
    } else if (!strcmp(verb, "gmalloc")) {
        const uint32_t gm = *reinterpret_cast<const uint32_t*>(0x00E3EFC8);
        const uint32_t vt = gm ? *reinterpret_cast<const uint32_t*>(gm) : 0;
        Logger::Info("GMALLOC obj=%08X vtable=%08X", gm, vt);
        for (int i = 0; vt && i < 16; ++i)
            Logger::Info("GMALLOC vt[%2d] +0x%02X = %08X", i, i * 4, *reinterpret_cast<const uint32_t*>(vt + 4 * i));
        for (int i = 0; gm && i < 12; ++i)
            Logger::Info("GMALLOC obj+0x%02X = %08X", i * 4, *reinterpret_cast<const uint32_t*>(gm + 4 * i));
    } else if (!strcmp(verb, "animprobe")) {
        gAnimProbe = 1;
    } else if (!strcmp(verb, "wiprobe")) {
        gWiProbe = 1;  // next RunFrame copies WorldInfo, the one after diffs it
    } else if (!strcmp(verb, "peek") && sscanf(line.c_str(), "peek %x %d", &mask, &value) == 2 && value > 0 &&
               value <= 1024) {
        // Dump dwords; tag each value that points into the master-heap ranges ("M") or another
        // committed allocation ("C", with its allocation base).
        MKHeap::Region mr[8];
        const int nm = MKHeap::MasterRanges(mr, 8);
        for (int i = 0; i < value; ++i) {
            const uint32_t a = mask + 4 * i;
            MEMORY_BASIC_INFORMATION mbi;
            if (!VirtualQuery(reinterpret_cast<void*>(a), &mbi, sizeof mbi) || mbi.State != MEM_COMMIT) break;
            const uint32_t v = *reinterpret_cast<const uint32_t*>(a);
            const char* tag = "";
            uint32_t allocBase = 0;
            for (int k = 0; k < nm; ++k)
                if (v >= mr[k].base && v < mr[k].base + mr[k].size) tag = "M";
            if (!*tag && v >= 0x10000 && VirtualQuery(reinterpret_cast<void*>(v), &mbi, sizeof mbi) &&
                mbi.State == MEM_COMMIT && v >= 0x01000000) {
                tag = "C";
                allocBase = (uint32_t)(uintptr_t)mbi.AllocationBase;
            }
            if (v) Logger::Info("PEEK %08X +%03X = %08X %s %08X", a, 4 * i, v, tag, allocBase);
        }
    } else if ((!strcmp(verb, "uobj") || !strcmp(verb, "uobjback")) && sscanf(line.c_str(), "%*s %x", &mask) == 1) {
        // UE3 object identity: Name FName at +0x28 (0x00411A90), GNames data [0x00E7C058]
        // with the ANSI string at entry +0x10 (0x005F51D0); Outer +0x24, Class +0x30 (layout
        // hypothesis: verified when the class's own name resolves).
        auto ok = [](uint32_t a, uint32_t n) {
            MEMORY_BASIC_INFORMATION mbi;
            return a >= 0x10000 && VirtualQuery(reinterpret_cast<void*>(a), &mbi, sizeof mbi) && mbi.State == MEM_COMMIT &&
                   (mbi.Protect & (PAGE_READWRITE | PAGE_READONLY)) &&
                   a + n <= reinterpret_cast<uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
        };
        auto nameOf = [&](uint32_t obj, char* out, size_t n) {
            snprintf(out, n, "?");
            if (!ok(obj, 0x38)) return;
            const uint32_t idx = *reinterpret_cast<const uint32_t*>(obj + 0x28);
            const uint32_t names = *reinterpret_cast<const uint32_t*>(0x00E7C058);
            const uint32_t count = *reinterpret_cast<const uint32_t*>(0x00E7C05C);
            if (!ok(names, 4) || idx >= count) return;
            const uint32_t e = *reinterpret_cast<const uint32_t*>(names + 4 * idx);
            if (!ok(e + 0x10, 64)) return;
            snprintf(out, n, "%.60s_%u", reinterpret_cast<const char*>(e + 0x10), *reinterpret_cast<const uint32_t*>(obj + 0x2C));
        };
        if (!strcmp(verb, "uobjback")) {
            // Walk back (<= 0x2000) to the nearest UObject header: vtable in the image and a
            // Class whose own name resolves.
            const uint32_t from = mask;
            for (uint32_t a = from & ~3u; a + 0x2000 > from && a > 0x10000; a -= 4) {
                if (!ok(a, 0x38)) break;
                const uint32_t vt = *reinterpret_cast<const uint32_t*>(a);
                if (vt < 0x00C00000 || vt >= 0x00DA0000) continue;
                const uint32_t cl = *reinterpret_cast<const uint32_t*>(a + 0x30);
                char t[80];
                nameOf(cl, t, sizeof t);
                if (t[0] == '?' || !ok(a + 0x28, 8)) continue;
                mask = a;
                Logger::Info("UOBJBACK %08X -> object at %08X (+0x%X)", from, a, from - a);
                break;
            }
        }
        char name[80], cls[80], outer[80];
        nameOf(mask, name, sizeof name);
        const uint32_t c = ok(mask, 0x38) ? *reinterpret_cast<const uint32_t*>(mask + 0x30) : 0;
        const uint32_t o = ok(mask, 0x38) ? *reinterpret_cast<const uint32_t*>(mask + 0x24) : 0;
        nameOf(c, cls, sizeof cls);
        nameOf(o, outer, sizeof outer);
        Logger::Info("UOBJ %08X vt=%08X name=%s class=%08X:%s outer=%08X:%s", mask,
                     ok(mask, 4) ? *reinterpret_cast<const uint32_t*>(mask) : 0, name, c, cls, o, outer);
    } else if (!strcmp(verb, "tarrays") && sscanf(line.c_str(), "tarrays %x %x", &mask, (unsigned*)&value) == 2 &&
               value > 0 && value <= 0x4000) {
        // Candidate TArray {data, num, max} triples inside [mask, mask+value): data committed
        // outside the image, 0 < num <= max <= 0x10000; with the guarded GMalloc block size.
        for (uint32_t o = 0; o + 12 <= (uint32_t)value; o += 4) {
            const uint32_t* t = reinterpret_cast<const uint32_t*>(mask + o);
            const uint32_t p = t[0], num = t[1], max = t[2];
            if (!p || (p & 3) || num == 0 || num > max || max > 0x10000 || p < 0x01000000) continue;
            MEMORY_BASIC_INFORMATION mbi;
            if (!VirtualQuery(reinterpret_cast<void*>(p), &mbi, sizeof mbi) || mbi.State != MEM_COMMIT) continue;
            uint32_t info[2] = {0, 0};
            const bool blk = CrashLog::SafeBlockInfo(reinterpret_cast<const void*>(p), info);
            Logger::Info("TARRAY %08X +%03X data=%08X num=%u max=%u block=%s%u elem~%u", mask, o, p, num, max,
                         blk ? "" : "?", info[1], blk && max ? info[1] / max : 0);
        }
    } else if (!strcmp(verb, "scriptdata") && sscanf(line.c_str(), "scriptdata %d", &value) == 1) {
        Snapshot::gScriptData = value != 0;
    } else if (!strcmp(verb, "tiere") && sscanf(line.c_str(), "tiere %d", &value) == 1) {
        Snapshot::gTierEOn = value != 0;
    } else if (!strcmp(verb, "scratch") && sscanf(line.c_str(), "scratch %d", &value) == 1) {
        Snapshot::gScriptScratch = value != 0;
    } else if (!strcmp(verb, "animarrays")) {
        // Where do MK anim blend nodes keep their arrays (+0x08, children +0x3C, bone masks +0x4C)?
        // Which MK heap (headers [0xED53F0..0xED5400], region base +0x34 / size +0x24) holds the data,
        // is it inside the restored master image, and is it a live GMalloc block?
        MKHeap::Region mr[8];
        const int nmr = MKHeap::MasterRanges(mr, 8);
        auto rd = [](uint32_t a) { return *reinterpret_cast<const uint32_t*>(a); };
        auto heapOf = [&](uint32_t p) -> int {
            for (int h = 0; h < 5; ++h) {
                const uint32_t hp = rd(0x00ED53F0 + 4 * h);
                if (!hp) continue;
                const uint32_t base = rd(hp + 0x34), size = rd(hp + 0x24);
                if (p >= base && p < base + size) return h;
            }
            return -1;
        };
        int shown = 0, guard = 0;
        for (uint32_t node = rd(0x00DE8214); node && guard < 20000 && shown < 8; node = rd(node + 0xC), ++guard) {
            const uint32_t obj = rd(node + 4);
            if (rd(node + 0x18) || !obj || rd(obj + 4) != rd(node + 8)) continue;
            const char* type = ObjectHooks::TypeOf(obj);
            if (!type || !strstr(type, "MKAnimNodeBlend")) continue;
            ++shown;
            for (uint32_t off : {0x08u, 0x3Cu, 0x4Cu}) {
                const uint32_t p = rd(obj + off), num = rd(obj + off + 4), max = rd(obj + off + 8);
                bool inMaster = false;
                for (int r = 0; r < nmr; ++r) inMaster |= p >= mr[r].base && p < mr[r].base + mr[r].size;
                uint32_t info[2] = {0, 0};
                const bool blk = p && CrashLog::SafeBlockInfo(reinterpret_cast<const void*>(p), info);
                Logger::Info("ANIMARRAYS %08X %.40s +0x%02X data=%08X num=%u max=%u heap=%d master=%d gmalloc=%s size=%u", obj, type, off, p,
                             num, max, p ? heapOf(p) : -2, inMaster, blk ? "yes" : "no", info[1]);
            }
        }
    } else if (!strcmp(verb, "fpu")) {
        // x87 control word / MXCSR per thread: tasks of the per-object async manager run on pool
        // threads or on the game thread (0x00661A70 pops and runs them while waiting); a different
        // precision or rounding mode would make results depend on the thread that ran them.
        HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
        if (snap != INVALID_HANDLE_VALUE) {
            THREADENTRY32 te{};
            te.dwSize = sizeof te;
            for (BOOL ok = Thread32First(snap, &te); ok; ok = Thread32Next(snap, &te)) {
                if (te.th32OwnerProcessID != GetCurrentProcessId()) continue;
                if (te.th32ThreadID == GetCurrentThreadId()) {
                    unsigned short cw;
                    unsigned int csr;
                    asm volatile("fnstcw %0" : "=m"(cw));
                    asm volatile("stmxcsr %0" : "=m"(csr));
                    Logger::Info("FPU thread %lu (game, self) x87cw=%04X mxcsr=%08X", te.th32ThreadID, cw, csr);
                    continue;
                }
                HANDLE t = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT, FALSE, te.th32ThreadID);
                if (!t) continue;
                CONTEXT c{};
                c.ContextFlags = CONTEXT_FLOATING_POINT | CONTEXT_EXTENDED_REGISTERS | CONTEXT_CONTROL;
                SuspendThread(t);
                const bool got = GetThreadContext(t, &c) != 0;
                ResumeThread(t);
                CloseHandle(t);
                if (!got) continue;
                uint32_t csr = 0;
                memcpy(&csr, c.ExtendedRegisters + 24, 4);  // FXSAVE layout: MXCSR at +24
                char stk[160];
                int sn = 0, shown = 0;
                const DWORD* sp = reinterpret_cast<const DWORD*>(c.Esp);
                for (int i = 0; i < 512 && shown < 6; ++i) {
                    MEMORY_BASIC_INFORMATION mbi;
                    if (!VirtualQuery(sp + i, &mbi, sizeof mbi) || mbi.State != MEM_COMMIT) break;
                    if (sp[i] >= 0x00401000 && sp[i] < 0x00F9C000) { sn = AppendF(stk, sizeof stk, sn, " %08lX", sp[i]); ++shown; }
                }
                stk[sn] = 0;
                Logger::Info("FPU thread %lu x87cw=%04X mxcsr=%08X eip=%08lX game-code:%s", te.th32ThreadID, (unsigned)(c.FloatSave.ControlWord & 0xFFFF),
                             csr, c.Eip, stk);
            }
            CloseHandle(snap);
        }
    } else if (!strcmp(verb, "inspan") && sscanf(line.c_str(), "inspan %x", &mask) == 1) {
        Rollback::LogSpansContaining(mask);
    } else if (!strcmp(verb, "contacts")) {
        // The 0x60-byte pair lists searched by 0x007896F0 ([actor+0x60]+0x68) and 0x00789640
        // ([obj+0x48]+0x68): owner of the holder, every entry's +0 and +0x30 objects.
        auto ok = [](uint32_t a, uint32_t n) {
            MEMORY_BASIC_INFORMATION mbi;
            return a >= 0x10000 && VirtualQuery(reinterpret_cast<void*>(a), &mbi, sizeof mbi) && mbi.State == MEM_COMMIT &&
                   (mbi.Protect & (PAGE_READWRITE | PAGE_READONLY)) && a + n <= reinterpret_cast<uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
        };
        auto rd = [](uint32_t a) { return *reinterpret_cast<const uint32_t*>(a); };
        for (int p = 0; p < 2; ++p) {
            const uint32_t info = (uint32_t)MK9Game::Get().playerInfoBase + p * MK9Game::kPlayerInfoStride;
            const uint32_t obj = ok(info + 0x1C, 8) ? rd(info + 0x1C) : 0;
            const uint32_t actor = ok(obj + 8, 4) ? rd(obj + 8) : 0;
            char d[160];
            Ue3Names::Describe(actor, d, sizeof d);
            Logger::Info("CONTACTS p%d mkobj=%08X actor=%08X %s +0x60=%08X", p, obj, actor, d, ok(actor + 0x60, 4) ? rd(actor + 0x60) : 0);
            for (uint32_t off : {0x60u}) {
                const uint32_t h = ok(actor + off, 4) ? rd(actor + off) : 0;
                if (!ok(h + 0x68, 8)) continue;
                Ue3Names::Describe(h, d, sizeof d);
                const uint32_t data = rd(h + 0x68), cnt = rd(h + 0x6C);
                Logger::Info("CONTACTS   actor+0x%X holder=%08X %s list data=%08X count=%u", off, h, d, data, cnt);
                for (uint32_t i = 0; i < cnt && i < 32 && ok(data + i * 0x60, 0x60); ++i) {
                    const uint32_t* r = reinterpret_cast<const uint32_t*>(data + i * 0x60);
                    char a0[120], a1[120];
                    Ue3Names::Describe(r[0], a0, sizeof a0);
                    Ue3Names::Describe(r[12], a1, sizeof a1);
                    Logger::Info("CONTACTS     [%u] +0 %08X %s | +0x30 %08X %s | %08X %08X %08X %08X", i, r[0], a0, r[12], a1, r[1], r[2], r[13], r[14]);
                }
            }
        }
        // Objects of the class whose vt slot 70 is 0x007794A0 (vt 0x00D5A330): holder [o+0x48].
        const uint32_t objs = rd(0x00E7C0EC), count = rd(0x00E7C0F0);
        int shown = 0;
        for (uint32_t i = 0; i < count && shown < 12; ++i) {
            const uint32_t o = rd(objs + 4 * i);
            if (!o || !ok(o, 0x50) || rd(o) != 0x00D5A330) continue;
            ++shown;
            char d[160], hd[160];
            Ue3Names::Describe(o, d, sizeof d);
            const uint32_t h = rd(o + 0x48);
            Ue3Names::Describe(h, hd, sizeof hd);
            Logger::Info("CONTACTS vtD5A330 %08X %s outer=%08X +0x48=%08X %s", o, d, rd(o + 0x24), h, hd);
            if (!ok(h + 0x68, 8)) continue;
            const uint32_t data = rd(h + 0x68), cnt = rd(h + 0x6C);
            Logger::Info("CONTACTS   list data=%08X count=%u max=%u", data, cnt, ok(h + 0x70, 4) ? rd(h + 0x70) : 0);
            for (uint32_t k = 0; k < cnt && k < 8 && ok(data + k * 0x60, 0x60); ++k) {
                const uint32_t* r = reinterpret_cast<const uint32_t*>(data + k * 0x60);
                char a0[120], a1[120];
                Ue3Names::Describe(r[0], a0, sizeof a0);
                Ue3Names::Describe(r[12], a1, sizeof a1);
                Logger::Info("CONTACTS     [%u] +0 %08X %s | +0x30 %08X %s | %08X %08X %08X %08X", k, r[0], a0, r[12], a1, r[1], r[2], r[13], r[14]);
            }
        }
    } else if (!strcmp(verb, "skel") && sscanf(line.c_str(), "skel %d", &value) == 1) {
        Snapshot::gSkelArrays = value != 0;
    } else if (!strcmp(verb, "listscan")) {
        // Every MK node {vt 0xC66B14 live / 0xD4D980 free, obj +4, id +8, next +0xC, prev +0x10,
        // list +0x14, dead +0x18} lives in one pool (base [0xE394E4], 0x1C stride). Collect the
        // lists live nodes belong to; flag image-resident lists no Tier A span covers.
        const uint32_t base = *reinterpret_cast<const uint32_t*>(0x00E394E4);
        std::map<uint32_t, int> lists;
        int live = 0, freeN = 0;
        for (uint32_t n = base; n; n += 0x1C) {
            MEMORY_BASIC_INFORMATION mbi;
            if (!VirtualQuery(reinterpret_cast<void*>(n), &mbi, sizeof mbi) || mbi.State != MEM_COMMIT) break;
            const uint32_t vt = *reinterpret_cast<const uint32_t*>(n);
            if (vt == 0x00D4D980) { ++freeN; continue; }
            if (vt != 0x00C66B14) break;
            ++live;
            ++lists[*reinterpret_cast<const uint32_t*>(n + 0x14)];
        }
        Logger::Info("LISTSCAN pool %08X: %d live nodes, %d free, %u distinct lists", base, live, freeN, (unsigned)lists.size());
        for (auto& [l, cnt] : lists) {
            const bool image = l >= 0x00400000 && l < 0x010AE000;
            const char* cov = image ? Snapshot::CoveredStatic(l, 0x14) : "heap";
            Logger::Info("LISTSCAN list %08X nodes=%d %s%s", l, cnt, cov ? cov : "UNCOVERED", image ? " (image)" : "");
        }
    } else if (!strcmp(verb, "heaptree")) {
        // MK heap headers form a tree: +4 parent, +8 first child, +0xC next sibling; base +0x34,
        // reservation +0x24. Walk from the master [0xED53F0] (depth-first, bounded).
        struct Item { uint32_t h; int depth; };
        std::vector<Item> stack{{*reinterpret_cast<const uint32_t*>(0x00ED53F0), 0}};
        int count = 0;
        while (!stack.empty() && count < 256) {
            const Item it = stack.back();
            stack.pop_back();
            MEMORY_BASIC_INFORMATION mbi;
            if (!it.h || !VirtualQuery(reinterpret_cast<void*>(it.h), &mbi, sizeof mbi) || mbi.State != MEM_COMMIT) continue;
            const uint32_t* h = reinterpret_cast<const uint32_t*>(it.h);
            ++count;
            Logger::Info("HEAPTREE %*s%08X vt=%08X parent=%08X base=%08X res=%X size38=%X used28=%X", it.depth * 2, "", it.h,
                         h[0], h[1], h[0x34 / 4], h[0x24 / 4], h[0x38 / 4], h[0x28 / 4]);
            if (h[3]) stack.push_back({h[3], it.depth});
            if (h[2]) stack.push_back({h[2], it.depth + 1});
        }
        Logger::Info("HEAPTREE %d headers", count);
    } else if (!strcmp(verb, "modules")) {
        HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, GetCurrentProcessId());
        MODULEENTRY32W me{};
        me.dwSize = sizeof me;
        for (BOOL ok = snap != INVALID_HANDLE_VALUE && Module32FirstW(snap, &me); ok; ok = Module32NextW(snap, &me))
            Logger::Info("MODULE %08X-%08X %ls", (unsigned)(uintptr_t)me.modBaseAddr,
                         (unsigned)((uintptr_t)me.modBaseAddr + me.modBaseSize), me.szModule);
        if (snap != INVALID_HANDLE_VALUE) CloseHandle(snap);
    } else if ((!strcmp(verb, "hwbp") || !strcmp(verb, "hwexec")) && sscanf(line.c_str(), "%*s %x", &mask) == 1) {
        // Runs on the game thread: arm the breakpoint from a helper thread.
        gWatchArg = {GetCurrentThreadId(), mask, !strcmp(verb, "hwexec")};
        HANDLE h = CreateThread(nullptr, 0, ArmWatch, &gWatchArg, 0, nullptr);
        if (h) CloseHandle(h);
    } else if (!strcmp(verb, "loctrace") && sscanf(line.c_str(), "loctrace %d", &value) == 1) {
        // Write trace on both fighters' actor Location Y/Z (+0xE0/+0xE4); armed from a helper thread.
        for (int p = 0; p < 2; ++p) {
            const uint32_t obj = *reinterpret_cast<const uint32_t*>(0x00E89D7C + p * 0xC8C + 0x1C);
            const uint32_t actor = (value && obj && obj != 0xDEADBEEF) ? *reinterpret_cast<const uint32_t*>(obj + 8) : 0;
            // 1: actor Location Y/Z. 2: SkeletalMeshComponent (obj+0x10C) root-motion Z (+0x5E8) and actor Z.
            const uint32_t comp = (value == 2 && obj && obj != 0xDEADBEEF) ? *reinterpret_cast<const uint32_t*>(obj + 0x10C) : 0;
            gTraceArg.addrs[2 * p] = value == 2 ? (comp ? comp + 0x5E8 : 0) : (actor ? actor + 0xE0 : 0);
            gTraceArg.addrs[2 * p + 1] = actor ? actor + 0xE4 : 0;
        }
        gTraceArg.tid = GetCurrentThreadId();
        HANDLE h = CreateThread(nullptr, 0, ArmTrace, &gTraceArg, 0, nullptr);
        if (h) CloseHandle(h);
    } else if (!strcmp(verb, "betweenprobe") && sscanf(line.c_str(), "betweenprobe %d", &value) == 1) {
        Rollback::SetBetweenProbe(value);
    } else if (!strcmp(verb, "traceall") && sscanf(line.c_str(), "traceall %d", &value) == 1) {
        CrashLog::SetTraceAllThreads(value != 0);
    } else if (!strcmp(verb, "classprops") && sscanf(line.c_str(), "classprops %x", &mask) == 1) {
        // UE3 reflection walk: the class's property chain, then its super classes. Layout found at runtime: the
        // children slot is the first class dword (0x38..0x7C) naming a *Property object, the next-field slot the
        // first property dword (0x38..0x48) naming another *Property or 0. Each property is printed with dwords
        // +0x3C..+0x6C so the Offset slot can be read off a known field (Actor Location = 0xDC).
        auto isProp = [](uint32_t o) {
            if (o < 0x01000000 || (o & 3) || !CrashLog::SafeRead32(o + 0x30, &o)) return false;
            char cn[96];
            Ue3Names::NameOf(o, cn, sizeof cn);
            return strstr(cn, "Property") != nullptr;
        };
        for (uint32_t cls = mask, depth = 0; cls && depth < 6; ++depth) {
            char cname[96];
            Ue3Names::NameOf(cls, cname, sizeof cname);
            uint32_t childOff = 0, first = 0, v = 0;
            for (uint32_t k = 0x38; k < 0x80 && !childOff; k += 4)
                if (CrashLog::SafeRead32(cls + k, &v) && isProp(v)) childOff = k, first = v;
            uint32_t size = 0, super = 0;
            CrashLog::SafeRead32(cls + 0x40, &size);
            CrashLog::SafeRead32(cls + 0x38, &super);
            Logger::Info("CLASSPROPS %s %08X: size %X, children at +%X, super %08X", cname, cls, size, childOff, super);
            uint32_t nextOff = 0;
            // Next field: prefer a slot naming another property (UField +0x38 is 0 for these, SuperField).
            if (first)
                for (uint32_t j = 0x38; j <= 0x48 && !nextOff; j += 4)
                    if (CrashLog::SafeRead32(first + j, &v) && v && isProp(v) && v != first) nextOff = j;
            if (first && !nextOff) nextOff = 0x3C;
            int count = 0;
            for (uint32_t p = first; p && nextOff && count < 400 && isProp(p); ++count) {
                char pn[96], pc[96];
                Ue3Names::NameOf(p, pn, sizeof pn);
                uint32_t pcls = 0;
                CrashLog::SafeRead32(p + 0x30, &pcls);
                Ue3Names::NameOf(pcls, pc, sizeof pc);
                uint32_t w[13] = {};
                for (int i = 0; i < 13; ++i) CrashLog::SafeRead32(p + 0x3C + 4 * i, &w[i]);
                // +0x58 is the field Offset (Actor.Components = 0x3C), +0x44 ArrayDim, +0x48 ElementSize, +0x4C flags.
                Logger::Info("CLASSPROPS   +%03X %-28s %-22s dim %u elem %X flags %08X", w[7], pn, pc, w[2], w[3], w[4]);
                if (!CrashLog::SafeRead32(p + nextOff, &p)) break;
            }
            cls = super;
        }
    } else if (!strcmp(verb, "findprop")) {
        // UE3 reflection probe: objects named <name> (e.g. a UProperty), their class and outer, and the dwords after
        // the UObject header (where UProperty keeps ArrayDim/ElementSize/flags/Offset).
        char want[64] = {};
        if (sscanf(line.c_str(), "findprop %63s", want) == 1) {
            const uint32_t objs = *reinterpret_cast<const uint32_t*>(0x00E7C0EC), cnt = *reinterpret_cast<const uint32_t*>(0x00E7C0F0);
            int found = 0;
            for (uint32_t i = 0; objs && i < cnt && i < 0x200000 && found < 12; ++i) {
                const uint32_t o = *reinterpret_cast<const uint32_t*>(objs + 4 * i);
                if (!o) continue;
                char nm[96];
                Ue3Names::NameOf(o, nm, sizeof nm);
                const size_t wl = strlen(want);
                if (strncmp(nm, want, wl) || (nm[wl] && nm[wl] != '_')) continue;
                ++found;
                char cls[96], outer[96];
                Ue3Names::NameOf(*reinterpret_cast<const uint32_t*>(o + 0x30), cls, sizeof cls);
                Ue3Names::NameOf(*reinterpret_cast<const uint32_t*>(o + 0x24), outer, sizeof outer);
                const uint32_t* w = reinterpret_cast<const uint32_t*>(o + 0x3C);
                Logger::Info("FINDPROP %s = %08X class %s outer %s: %08X %08X %08X %08X %08X %08X %08X %08X %08X %08X %08X %08X", nm, o, cls,
                             outer, w[0], w[1], w[2], w[3], w[4], w[5], w[6], w[7], w[8], w[9], w[10], w[11]);
            }
            if (!found) Logger::Info("FINDPROP %s: not found", want);
        }
    } else if (!strcmp(verb, "loctraceactor") && sscanf(line.c_str(), "loctraceactor %d %x", &value, &mask) == 2) {
        // Write trace (live vs resim) on four dwords of a fighter's actor from the given offset.
        const uint32_t obj = *reinterpret_cast<const uint32_t*>(0x00E89D7C + (value & 1) * 0xC8C + 0x1C);
        const uint32_t actor = obj && obj != 0xDEADBEEF ? *reinterpret_cast<const uint32_t*>(obj + 8) : 0;
        for (int i = 0; i < 4; ++i) gTraceArg.addrs[i] = actor ? actor + mask + 4 * i : 0;
        gTraceArg.tid = GetCurrentThreadId();
        HANDLE h = CreateThread(nullptr, 0, ArmTrace, &gTraceArg, 0, nullptr);
        if (h) CloseHandle(h);
    } else if (!strcmp(verb, "traceanimpos")) {
        unsigned off = 0x74;
        sscanf(line.c_str(), "traceanimpos %x", &off);
        // Write trace (live vs resim per frame) on the anim position (+0x74) of the first four MKAnimNodeSequence nodes.
        struct Pick {
            int n;
            uint32_t obj[4];
        } pick{0, {0, 0, 0, 0}};
        ObjectHooks::ForEachLive(
            [](uint32_t o, const char* t, void* c) {
                Pick* p = static_cast<Pick*>(c);
                if (p->n < 4 && !strncmp(t, "MKAnimNodeSequence", 18)) p->obj[p->n++] = o;
            },
            &pick);
        for (int i = 0; i < 4; ++i) gTraceArg.addrs[i] = pick.obj[i] ? pick.obj[i] + off : 0;
        Logger::Info("TRACEANIMPOS nodes %08X %08X %08X %08X", pick.obj[0], pick.obj[1], pick.obj[2], pick.obj[3]);
        gTraceArg.tid = GetCurrentThreadId();
        HANDLE h = CreateThread(nullptr, 0, ArmTrace, &gTraceArg, 0, nullptr);
        if (h) CloseHandle(h);
    } else if (!strcmp(verb, "affinity") && sscanf(line.c_str(), "affinity %x", &mask) == 1) {
        // Experiment: restrict the whole process to the given CPU mask (1 = one core): do task-system races explain
        // the live-vs-replay differences after the fight update?
        DWORD_PTR proc = 0, sys = 0;
        GetProcessAffinityMask(GetCurrentProcess(), &proc, &sys);
        const BOOL ok = SetProcessAffinityMask(GetCurrentProcess(), mask ? mask : sys);
        Logger::Info("AFFINITY %08lX -> %08X (system %08lX): %s", (unsigned long)proc, mask ? mask : (unsigned)sys, (unsigned long)sys,
                     ok ? "ok" : "failed");
    } else if (!strcmp(verb, "hwbpactor") && sscanf(line.c_str(), "hwbpactor %d %x", &value, &mask) == 2) {
        // Write watch on a fighter's UE3 actor field: hwbpactor <player 0|1> <offset hex>.
        const uint32_t obj = *reinterpret_cast<const uint32_t*>(0x00E89D7C + (value & 1) * 0xC8C + 0x1C);
        const uint32_t actor = obj && obj != 0xDEADBEEF ? *reinterpret_cast<const uint32_t*>(obj + 8) : 0;
        uint32_t blk[2] = {0, 0};
        if (actor) CrashLog::SafeBlockInfo(reinterpret_cast<const void*>(actor), blk);
        Logger::Info("HWBPACTOR P%d actor %08X +%X = %08X (GMalloc block size %X)", (value & 1) + 1, actor, mask,
                     actor ? *reinterpret_cast<const uint32_t*>(actor + mask) : 0u, blk[1]);
        if (actor) {
            gWatchArg = {GetCurrentThreadId(), actor + mask, false};
            HANDLE h = CreateThread(nullptr, 0, ArmWatch, &gWatchArg, 0, nullptr);
            if (h) CloseHandle(h);
        }
    } else if (!strcmp(verb, "hwbpall") && sscanf(line.c_str(), "hwbpall %x", &mask) == 1) {
        gWatchArg = {0, mask, false};  // write watch on every thread (render/worker writers)
        HANDLE h = CreateThread(nullptr, 0, ArmWatchAll, &gWatchArg, 0, nullptr);
        if (h) CloseHandle(h);
    } else if (!strcmp(verb, "atomwatch") && sscanf(line.c_str(), "atomwatch %d %d", &value, &mask) == 2) {
        // Distinct writers of fighter <value>'s bone atom <mask> (+0x10 of the 40-byte atom: translation X).
        uint32_t obj = 0, comp = 0, data = 0, num = 0;
        CrashLog::SafeRead32(0x00E89D7C + value * 0xC8C + 0x1C, &obj);
        if (obj) CrashLog::SafeRead32(obj + 0x10C, &comp);
        if (comp) CrashLog::SafeRead32(comp + 0x2C8, &data);
        if (comp) CrashLog::SafeRead32(comp + 0x2CC, &num);
        if (data && (uint32_t)mask < num) {
            CrashLog::gWatchDistinct = true;
            gWatchArg = {0, data + (uint32_t)mask * 40 + 0x10, false};
            Logger::Info("ATOMWATCH P%d comp %08X atoms %08X (%u bones): watching %08X", value + 1, comp, data, num, data + (uint32_t)mask * 40 + 0x10);
            HANDLE h = CreateThread(nullptr, 0, ArmWatchAll, &gWatchArg, 0, nullptr);
            if (h) CloseHandle(h);
        } else {
            Logger::Info("ATOMWATCH: no atoms (comp %08X data %08X num %u)", comp, data, num);
        }
    } else if (!strcmp(verb, "blockinfo") && sscanf(line.c_str(), "blockinfo %x", &mask) == 1) {
        uint32_t info[2] = {0, 0};
        const bool ok = CrashLog::SafeBlockInfo(reinterpret_cast<const void*>(mask), info);
        Logger::Info("BLOCKINFO %08X: %s {%X %X}", mask, ok ? "GMalloc block" : "no block", info[0], info[1]);
    } else if (!strcmp(verb, "padinfo")) {
        // Pad objects of both players ([PlayerInfo + 0xB40]; the snapshot captures 0x5F8 bytes of each).
        for (int i = 0; i < 2; ++i) {
            const uint32_t info = MK9Game::Get().playerInfoBase + i * MK9Game::kPlayerInfoStride;
            const uint32_t pad = *reinterpret_cast<const uint32_t*>(info + 0xB40);
            uint32_t bi[2] = {0, 0};
            const bool ok = pad && CrashLog::SafeBlockInfo(reinterpret_cast<const void*>(pad), bi);
            Logger::Info("PADINFO P%d pad %08X: %s {%X %X}", i + 1, pad, ok ? "GMalloc block" : "no block", bi[0], bi[1]);
        }
    } else if (!strcmp(verb, "animtrace") && sscanf(line.c_str(), "animtrace %d", &value) == 1) {
        AnimInputTrace::SetEnabled(value != 0);
    } else if (!strcmp(verb, "counterscan")) {
        gCounterScan = 1;
    } else if (!strcmp(verb, "ctxprobe")) {
        Snapshot::CtxProbe();
    } else if (!strcmp(verb, "hwbpactorall")&& sscanf(line.c_str(), "hwbpactorall %d %x", &value, &mask) == 2) {
        // As hwbpactor, but on every thread of the process (render/worker writers).
        const uint32_t obj = *reinterpret_cast<const uint32_t*>(0x00E89D7C + (value & 1) * 0xC8C + 0x1C);
        const uint32_t actor = obj && obj != 0xDEADBEEF ? *reinterpret_cast<const uint32_t*>(obj + 8) : 0;
        // 'hwbpactorall P OFF 1': each distinct writer once (up to 12) instead of the first 6 hits.
        int distinct = 0;
        sscanf(line.c_str(), "hwbpactorall %*d %*x %d", &distinct);
        if (actor) {
            if (distinct) CrashLog::gWatchDistinct = true;
            gWatchArg = {0, actor + mask, false};
            HANDLE h = CreateThread(nullptr, 0, ArmWatchAll, &gWatchArg, 0, nullptr);
            if (h) CloseHandle(h);
        }
        Logger::Info("HWBPACTORALL P%d actor %08X +%X%s (game thread %lu)", (value & 1) + 1, actor, mask, distinct ? " distinct writers" : "",
                     GetCurrentThreadId());
    } else if (!strcmp(verb, "hwbpchar") && sscanf(line.c_str(), "hwbpchar %d %x", &value, &mask) == 2) {
        // Write watch on a fighter's MK character object field: hwbpchar <player 0|1> <offset hex>.
        const uint32_t obj = *reinterpret_cast<const uint32_t*>(0x00E89D7C + (value & 1) * 0xC8C + 0x1C);
        Logger::Info("HWBPCHAR P%d obj %08X +%X = %08X", (value & 1) + 1, obj, mask, obj ? *reinterpret_cast<const uint32_t*>(obj + mask) : 0u);
        if (obj && obj != 0xDEADBEEF) {
            gWatchArg = {GetCurrentThreadId(), obj + mask, false};
            HANDLE h = CreateThread(nullptr, 0, ArmWatch, &gWatchArg, 0, nullptr);
            if (h) CloseHandle(h);
        }
    } else if (!strcmp(verb, "watchanimpos") && sscanf(line.c_str(), "watchanimpos %d", &value) == 1) {
        // Write watch on the anim position (+0x74) of the value-th live MKAnimNodeSequence: who advances it.
        struct Pick {
            int want, seen;
            uint32_t obj;
        } pick{value, 0, 0};
        unsigned off = 0x74;
        sscanf(line.c_str(), "watchanimpos %*d %x", &off);
        ObjectHooks::ForEachLive(
            [](uint32_t o, const char* t, void* c) {
                Pick* p = static_cast<Pick*>(c);
                if (p->obj || strncmp(t, "MKAnimNodeSequence", 18)) return;
                if (p->seen++ == p->want) p->obj = o;
            },
            &pick);
        Logger::Info("WATCHANIMPOS: node #%d = %08X (pos %08X)", value, pick.obj,
                     pick.obj ? *reinterpret_cast<const uint32_t*>(pick.obj + 0x74) : 0u);
        if (pick.obj) {
            gWatchArg = {GetCurrentThreadId(), pick.obj + off, false};
            HANDLE h = CreateThread(nullptr, 0, ArmWatch, &gWatchArg, 0, nullptr);
            if (h) CloseHandle(h);
        }
    } else if (!strcmp(verb, "deepdiff") && sscanf(line.c_str(), "deepdiff %d", &value) == 1) {
        Rollback::SetDeepDiff(value != 0);
        Rollback::SetDeepEvery(value == 2);  // 2: every load's first frame
    } else if (!strcmp(verb, "qmenus") && sscanf(line.c_str(), "qmenus %d", &value) == 1) {
        Rollback::SetQuarantineMenus(value != 0);
    } else if (!strcmp(verb, "netplayload") && sscanf(line.c_str(), "netplayload %d", &value) == 1) {
        Rollback::SetNetplayFromLoad(value != 0);
    } else if (!strcmp(verb, "netplaydemo") && sscanf(line.c_str(), "netplaydemo %d", &value) == 1) {
        Rollback::SetNetplayDemoFights(value != 0);
    } else if (!strcmp(verb, "netplayrearm") && sscanf(line.c_str(), "netplayrearm %d", &value) == 1) {
        Rollback::SetNetplayAutoRearm(value != 0);
    } else if (!strcmp(verb, "netsim")) {
        // Test only: GGPO's own network simulator (udp_proto.cpp reads these when a session starts). Every
        // packet this game sends waits 2/3..1 x delay ms; oop% of them arrive out of order, up to 10 x delay + 1 s late.
        // Patched GGPO (tools/build_ggpo.ps1): loss% of the packets are dropped, and for spike ms of every period ms no
        // packet leaves (a stalled link; the held ones then go out together), or with drop 1 the window's packets are lost.
        int delay = 0, oop = 0, loss = 0, spike = 0, every = 0, drop = 0;
        if (sscanf(line.c_str(), "netsim %d %d %d %d %d %d", &delay, &oop, &loss, &spike, &every, &drop) >= 1 && delay >= 0 && delay <= 1000 &&
            oop >= 0 && oop <= 50 && loss >= 0 && loss <= 50 && spike >= 0 && spike <= 2000 && every >= 0 && every <= 120000) {
            char d[16], o[16], l[16], s[16], e[16];
            snprintf(d, sizeof d, "%d", delay);
            snprintf(o, sizeof o, "%d", oop);
            snprintf(l, sizeof l, "%d", loss);
            snprintf(s, sizeof s, "%d", spike);
            snprintf(e, sizeof e, "%d", every);
            SetEnvironmentVariableA("ggpo.network.delay", delay ? d : nullptr);
            SetEnvironmentVariableA("ggpo.oop.percent", oop ? o : nullptr);
            SetEnvironmentVariableA("ggpo.loss.percent", loss ? l : nullptr);
            SetEnvironmentVariableA("ggpo.spike.ms", spike && every ? s : nullptr);
            SetEnvironmentVariableA("ggpo.spike.every", spike && every ? e : nullptr);
            SetEnvironmentVariableA("ggpo.spike.drop", spike && every && drop ? "1" : nullptr);
            Logger::Info("NETSIM: sends delayed %d ms (jitter down to 2/3), %d%% out of order, %d%% lost, %s %d ms every %d ms, "
                         "from the next session start", delay, oop, loss, drop ? "all lost for" : "stalled", spike && every ? spike : 0,
                         spike && every ? every : 0);
        } else {
            Logger::Info("NETSIM: usage 'netsim <delay ms 0..1000> [out-of-order %% 0..50] [loss %% 0..50] [stall ms] [stall period ms] [drop 0|1]'");
        }
    } else if (!strcmp(verb, "netsimtimed") && sscanf(line.c_str(), "netsimtimed %d", &value) == 1) {
        // Test only, both games of a one-PC pair: netsim's delay is kept by the receiver until each packet's due time
        // instead of by the sender until one of its polls (once a frame: 'netsim 15' was exactly a frame, on the
        // sender's frame grid). Needed to measure anything under a frame (tools/build_ggpo.ps1, timed netsim).
        SetEnvironmentVariableA("ggpo.network.timed", value ? "1" : nullptr);
        Logger::Info("NETSIMTIMED %d: netsim packets %s, from the next session start", value ? 1 : 0,
                     value ? "leave at once and are held by the receiver until their due time (both games need it)"
                           : "wait in the sender's queue for one of its polls (stock)");
    } else if (!strcmp(verb, "lazysave") && sscanf(line.c_str(), "lazysave %d", &value) == 1) {
        Rollback::SetLazySave(value != 0);
    } else if (!strcmp(verb, "skipconfirmed") && sscanf(line.c_str(), "skipconfirmed %d", &value) == 1) {
        Rollback::SetSkipConfirmed(value != 0);
    } else if (!strcmp(verb, "sparsesave") && sscanf(line.c_str(), "sparsesave %d", &value) == 1) {
        Rollback::SetSparseSave(value != 0);
    } else if (!strcmp(verb, "sparselive") && sscanf(line.c_str(), "sparselive %d", &value) == 1) {
        Rollback::SetSparseLive(value != 0);
    } else if (!strcmp(verb, "qmargin") && sscanf(line.c_str(), "qmargin %d", &value) == 1) {
        Rollback::SetQuarantineMargin(value);
    } else if (!strcmp(verb, "balance") && sscanf(line.c_str(), "balance %d", &value) == 1) {
        Rollback::SetBalance(value != 0);
    } else if (!strcmp(verb, "balancecfg")) {
        int settle = 45, samples = 6, give = 3;
        if (sscanf(line.c_str(), "balancecfg %d %d %d", &settle, &samples, &give) >= 2) Rollback::SetBalanceTuning(settle, samples, give);
    } else if (!strcmp(verb, "qualityms") && sscanf(line.c_str(), "qualityms %d", &value) == 1) {
        Rollback::SetQualityInterval(value);
    } else if (!strcmp(verb, "netretry") && sscanf(line.c_str(), "netretry %d", &value) == 1) {
        Rollback::SetRunningRetry(value);
    } else if (!strcmp(verb, "firstretry") && sscanf(line.c_str(), "firstretry %d", &value) == 1) {
        Rollback::SetFirstInputRetry(value);
    } else if (!strcmp(verb, "paysmooth") && sscanf(line.c_str(), "paysmooth %d", &value) == 1) {
        Rollback::SetPaySmooth(value);
        int small = 0;
        if (sscanf(line.c_str(), "paysmooth %d %d", &value, &small) == 2) Rollback::SetPaySmoothSmall(small);
    } else if (!strcmp(verb, "ggposync") && sscanf(line.c_str(), "ggposync %d", &value) == 1) {
        Rollback::SetGgpoTimesync(value != 0);
    } else if (!strcmp(verb, "repoll") && sscanf(line.c_str(), "repoll %d", &value) == 1) {
        Rollback::SetRepoll(value != 0);
    } else if (!strcmp(verb, "phasesync") && sscanf(line.c_str(), "phasesync %d", &value) == 1) {
        Rollback::SetPhaseSync(value != 0);
    } else if (!strcmp(verb, "soundnear") && sscanf(line.c_str(), "soundnear %d", &value) == 1) {
        SoundDedupe::SetNearFrames(value);
    } else if (!strcmp(verb, "replayskip") && sscanf(line.c_str(), "replayskip %d", &value) == 1) {
        RenderTag::SetReplaySkip(value != 0);
    } else if (!strcmp(verb, "fastreadable") && sscanf(line.c_str(), "fastreadable %d", &value) == 1) {
        Rollback::SetFastReadable(value != 0);
    } else if (!strcmp(verb, "renderlate") && sscanf(line.c_str(), "renderlate %d", &value) == 1) {
        Rollback::SetRenderLate(value);
    } else if (!strcmp(verb, "stallsample") && sscanf(line.c_str(), "stallsample %d", &value) == 1) {
        FramePace::SetStallSample(value);
    } else if (!strcmp(verb, "limiterwait") && sscanf(line.c_str(), "limiterwait %d", &value) == 1) {
        FramePace::SetLimiterWait(value != 0);
    } else if (!strcmp(verb, "ecoqos") && sscanf(line.c_str(), "ecoqos %d", &value) == 1) {
        FramePace::SetPowerThrottling(value != 0);
    } else if (!strcmp(verb, "fpupinsession") && sscanf(line.c_str(), "fpupinsession %d", &value) == 1) {
        FpuGuard::gPinInSessions = value != 0;
        Logger::Info("FPUPINSESSION %d: the all-thread FPU walk %s", value != 0 ? 1 : 0,
                     value != 0 ? "also runs every 600 ticks during sessions" : "runs once at a session's start, not during it");
    } else if (!strcmp(verb, "canonclock") && sscanf(line.c_str(), "canonclock %d", &value) == 1) {
        Rollback::SetCanonClock(value != 0);
    } else if (!strcmp(verb, "startbarrier") && sscanf(line.c_str(), "startbarrier %d", &value) == 1) {
        Rollback::SetStartBarrier(value != 0);
    } else if (!strcmp(verb, "startsync") && sscanf(line.c_str(), "startsync %d", &value) == 1) {
        Rollback::SetStartSync(value != 0);
    } else if (!strcmp(verb, "desyncstop") && sscanf(line.c_str(), "desyncstop %d", &value) == 1) {
        Rollback::SetDesyncStop(value);
    } else if (!strcmp(verb, "handshakepoll") && sscanf(line.c_str(), "handshakepoll %d", &value) == 1) {
        Rollback::SetHandshakePoll(value != 0);
    } else if (!strcmp(verb, "phasegrow") && sscanf(line.c_str(), "phasegrow %d", &value) == 1) {
        Rollback::SetPhaseGrowAfter(value);
    } else if (!strcmp(verb, "waitshift") && sscanf(line.c_str(), "waitshift %d", &value) == 1) {
        Rollback::SetWaitShift(value != 0);
    } else if (!strcmp(verb, "earlyinput") && sscanf(line.c_str(), "earlyinput %d", &value) == 1) {
        Rollback::SetEarlyInput(value != 0);
    } else if (!strcmp(verb, "latesample") && sscanf(line.c_str(), "latesample %d", &value) == 1) {
        Rollback::SetLateSample(value != 0);
    } else if (!strcmp(verb, "skiplive") && sscanf(line.c_str(), "skiplive %d", &value) == 1) {
        Rollback::SetSkipLive(value != 0);
    } else if (!strcmp(verb, "tickspin") && sscanf(line.c_str(), "tickspin %d", &value) == 1) {
        Rollback::SetTickSpin(value);
    } else if (!strcmp(verb, "stallhold") && sscanf(line.c_str(), "stallhold %d", &value) == 1) {
        int maxMs = 150;
        if (sscanf(line.c_str(), "stallhold %d %d", &value, &maxMs) < 2) maxMs = 150;
        Rollback::SetStallHold(value == 1 ? 60 : value, maxMs);  // 'stallhold 1' = the defaults (60 since rc18)
    } else if (!strcmp(verb, "xraylock") && sscanf(line.c_str(), "xraylock %d", &value) == 1) {
        Rollback::SetXrayLock(value, -1, 0, -1);
    } else if (!strcmp(verb, "xraylockcine") && sscanf(line.c_str(), "xraylockcine %d", &value) == 1) {
        Rollback::SetXrayLock(-1, value, 0, -1);
    } else if (!strcmp(verb, "xraylockwindow") && sscanf(line.c_str(), "xraylockwindow %d", &value) == 1) {
        Rollback::SetXrayLock(-1, -1, value, -1);
    } else if (!strcmp(verb, "xrayfreeze") && sscanf(line.c_str(), "xrayfreeze %d", &value) == 1) {
        Rollback::SetXrayLock(-1, -1, 0, value);
    } else if (!strcmp(verb, "freezeevery")) {
        int ms = 0, every = 0;
        if (sscanf(line.c_str(), "freezeevery %d %d", &ms, &every) == 2) Rollback::SetFreeze(ms, every);
    } else if (!strcmp(verb, "stages")) {
        ScriptRng::LogStages();
    } else if (!strcmp(verb, "forcestage") && sscanf(line.c_str(), "forcestage %d", &value) == 1) {
        ScriptRng::ForceStage(value);
    } else if (!strcmp(verb, "pickvariety") && sscanf(line.c_str(), "pickvariety %d", &value) == 1 && value >= 0) {
        ScriptRng::SetPickVariety((uint32_t)value);
    } else if (!strcmp(verb, "listeners")) {
        Rollback::LogEventListeners("command");
    } else if (!strcmp(verb, "netplaygf") && sscanf(line.c_str(), "netplaygf %d", &value) == 1 && value >= 0) {
        Rollback::SetNetplayGf((uint32_t)value);
    } else if (!strcmp(verb, "rnglog") && sscanf(line.c_str(), "rnglog %d", &value) == 1) {
        RngTrace::SetIdleLog(value != 0);
    } else if (!strcmp(verb, "scriptseed") && sscanf(line.c_str(), "scriptseed %x", &mask) == 1) {
        ScriptRng::SetAttractSeed(mask);
    } else if (!strcmp(verb, "scriptrng") && sscanf(line.c_str(), "scriptrng %d", &value) == 1) {
        Rollback::SetScriptRng(value != 0);
    } else if (!strcmp(verb, "rngtrace") && sscanf(line.c_str(), "rngtrace %d", &value) == 1) {
        RngTrace::SetRngOnly(value != 0);
    } else if (!strcmp(verb, "crowdrng") && sscanf(line.c_str(), "crowdrng %d", &value) == 1) {
        Rollback::SetCrowdRng(value != 0);
    } else if (!strcmp(verb, "attachall") && sscanf(line.c_str(), "attachall %d", &value) == 1) {
        Rollback::SetAttachAll(value != 0);
    } else if (!strcmp(verb, "mkattach") && sscanf(line.c_str(), "mkattach %d", &value) == 1) {
        Rollback::SetMkAttach(value != 0);
    } else if (!strcmp(verb, "compsync") && sscanf(line.c_str(), "compsync %d", &value) == 1) {
        Rollback::SetCompSync(value != 0);
    } else if (!strcmp(verb, "deferggpo") && sscanf(line.c_str(), "deferggpo %d", &value) == 1) {
        Rollback::SetDeferOnlyGgpo(value != 0);
    } else if (!strcmp(verb, "idlesaves") && sscanf(line.c_str(), "idlesaves %d", &value) == 1) {
        Rollback::SetIdleSaves(value != 0);
    } else if (!strcmp(verb, "phaseaudit") && sscanf(line.c_str(), "phaseaudit %d", &value) == 1) {
        Rollback::SetPhaseAudit(value != 0);
    } else if (!strcmp(verb, "oomtest")) {
        // Test of the out-of-memory handling: 64 MB from MK heap #5's allocator 0x0081A4B0 (cdecl size), which cannot
        // grow that far; MK9's handler 0x00406160 is reached as on 2026-10-31 20:34 and the game must close, logged.
        Logger::Info("OOMTEST: asking MK heap #5's allocator for 64 MB");
        Logger::Flush();
        reinterpret_cast<void*(__cdecl*)(uint32_t)>(0x0081A4B0)(64u << 20);
        Logger::Info("OOMTEST: the allocation returned (not expected)");
    } else if (!strcmp(verb, "tickparity") && sscanf(line.c_str(), "tickparity %d", &value) == 1) {
        Rollback::SetTickParity(value != 0);
    } else if (!strcmp(verb, "resimsleep") && sscanf(line.c_str(), "resimsleep %d", &value) == 1) {
        Rollback::SetResimSleep(value);
    } else if (!strcmp(verb, "forcelod") && sscanf(line.c_str(), "forcelod %d", &value) == 1) {
        Rollback::SetForceLod(value != 0);
    } else if (!strcmp(verb, "rbtwice") && sscanf(line.c_str(), "rbtwice %d", &value) == 1) {
        Rollback::SetTwice(value != 0);
    } else if (!strcmp(verb, "animtagfix") && sscanf(line.c_str(), "animtagfix %d", &value) == 1) {
        Rollback::SetAnimTagFix(value != 0);
    } else if (!strcmp(verb, "diag") && sscanf(line.c_str(), "diag %d", &value) == 1) {
        Rollback::gDiag = value != 0;
    } else if (!strcmp(verb, "qmode") && sscanf(line.c_str(), "qmode %d", &value) == 1) {
        FreeQuarantine::SetMode(value);
    } else if (!strcmp(verb, "freecallers")) {
        FreeQuarantine::LogTopCallers(40);
    } else if (!strcmp(verb, "heapinfo")) {
        MKHeap::LogInfo();
    } else if ((!strcmp(verb, "saveab") || !strcmp(verb, "loadab") || !strcmp(verb, "loadabforce")) &&
               sscanf(line.c_str(), "%*s %d", &value) == 1 && value >= 0 && value < 2) {
        const uint32_t gf = *reinterpret_cast<const uint32_t*>(*reinterpret_cast<const uint32_t*>(0x00DEF4B4) + 0x70);
        char why[160] = {};
        if (!strcmp(verb, "saveab")) {
            if (Snapshot::SaveAB(&gSlotsAB[value], gf, why, sizeof why)) {
                if (gObjHooks) ObjectHooks::Mark();
                size_t bytes = 0;
                for (auto& s : gSlotsAB[value].spans) bytes += s.size;
                Logger::Info("SNAPSHOT-AB save slot %d gf=%u spans=%u bytes=%u ids=%08X", value, gf,
                             (unsigned)gSlotsAB[value].spans.size(), (unsigned)bytes, gSlotsAB[value].idCounter);
            } else {
                gSlotsAB[value].spans.clear();
                Logger::Info("SNAPSHOT-AB save slot %d REFUSED: %s", value, why);
            }
        } else if (gSlotsAB[value].spans.empty()) {
            Logger::Info("SNAPSHOT-AB load slot %d: empty", value);
        } else if (gObjHooks && (int32_t)*reinterpret_cast<const uint32_t*>(0x00DE8628) == -1) {
            // Churn-aware load (docs/determinism.md, Gate D experiment 4):
            //  0. refuse if an actor-owning object that existed at the save has died since and
            //     its type is not verified safe to tear down without its (now destroyed) actor;
            //  1. destroy actor-owning objects born since the save via their own deleting dtor;
            //  2. restore Tier A+B;
            //  3. tear down resurrected effect objects whose actors died (actor ptr nulled first);
            //  4. restart birth/death tracking relative to the restored state.
            if (!ObjectHooks::CanLoad(why, sizeof why)) {
                Logger::Info("SNAPSHOT-AB load slot %d REFUSED at gf=%u: %s", value, gf, why);
            } else {
                const int killedBorn = ObjectHooks::DestroyBornSinceMark();
                const int n = Snapshot::LoadAB(gSlotsAB[value], true, why, sizeof why);
                FreeQuarantine::OnLoad(gSlotsAB[value].gameFrame);
                const int resurrected = ObjectHooks::RetireResurrected();
                Ue3Refs::FixDangling(gSlotsAB[value].gameFrame);
                ObjectHooks::Mark();
                Logger::Info("SNAPSHOT-AB load slot %d at gf=%u -> gf=%u, restored %d spans; post-save MKObj destroyed=%d, "
                             "resurrected actorless effects retired=%d",
                             value, gf, gSlotsAB[value].gameFrame, n, killedBorn, resurrected);
            }
        } else {
            const int n = Snapshot::LoadAB(gSlotsAB[value], verb[6] == 'f', why, sizeof why);
            if (n < 0)
                Logger::Info("SNAPSHOT-AB load slot %d REFUSED at gf=%u: %s", value, gf, why);
            else
                Logger::Info("SNAPSHOT-AB load slot %d at gf=%u -> gf=%u, restored %d spans%s", value, gf,
                             gSlotsAB[value].gameFrame, n, verb[6] == 'f' ? " (FORCED)" : "");
        }
        Logger::Frame("CMD %s -> %s", line.c_str(), why[0] ? why : "ok");
    } else if (!strcmp(verb, "clear")) {
        gScheduled.clear();
        gDumps.clear();
        for (auto& q : gQueue) q.clear();
        gStepRemaining = 0;
    } else if (!strcmp(verb, "at")) {
        int consumed = 0;
        if (sscanf(line.c_str(), "at %d %n", &value, &consumed) != 1 || value < 0 || !consumed) {
            Logger::Info("CMD ignored: '%s'", line.c_str());
            return;
        }
        gScheduled.push_back({(uint32_t)value, line.substr(consumed)});
        return;  // logged when it runs
    } else if (!strcmp(verb, "dumpafter")) {
        char label[96] = {};
        if (sscanf(line.c_str(), "dumpafter %d %95s", &value, label) != 2 || value < 0) {
            Logger::Info("CMD ignored: '%s'", line.c_str());
            return;
        }
        gDumps.push_back({(uint32_t)value, label});
    } else if (!strcmp(verb, "mark")) {
        Logger::Frame("MARK %s", line.c_str() + 4);
    } else {
        Logger::Info("CMD ignored: '%s'", line.c_str());
        return;
    }
    Logger::Info("CMD %s", line.c_str());
    Logger::Frame("CMD %s", line.c_str());
}
}  // namespace

namespace CommandChannel {

bool Start(const wchar_t* directory) {
    InitializeCriticalSection(&gLock);
    swprintf(gPath, MAX_PATH, L"%ls\\MK9Rollback_cmd.txt", directory);
    swprintf(gTakenPath, MAX_PATH, L"%ls\\MK9Rollback_cmd.reading", directory);
    wcsncpy(gDir, directory, MAX_PATH - 1);
    // A stale command file was removed before the log was opened (InitThread): what is there now came from the
    // launcher for this game.
    gThread =CreateThread(nullptr, 0, Poll, nullptr, 0, nullptr);
    return gThread != nullptr;
}

void Stop() {
    InterlockedExchange(&gStop, 1);
    if (gThread) {
        WaitForSingleObject(gThread, 2000);
        CloseHandle(gThread);
        gThread = nullptr;
    }
}

void RequestPause(int on) { gPendingPause = on ? 1 : 0; }

bool InstallPauseGuard() {
    if (gSetPauseTramp) return true;
    uint8_t* tramp = static_cast<uint8_t*>(CodePatch::AllocExec(16));
    if (!tramp) return false;
    memcpy(tramp, kSetPauseOrig, 6);  // relocation-free
    tramp[6] = 0xE9;
    const int32_t back = (int32_t)((kSetPause + 6) - (reinterpret_cast<uintptr_t>(tramp) + 11));
    memcpy(tramp + 7, &back, 4);
    FlushInstructionCache(GetCurrentProcess(), tramp, 16);
    uint8_t jmp[6] = {0xE9, 0, 0, 0, 0, 0x90};
    const int32_t rel = (int32_t)(reinterpret_cast<uintptr_t>(&SetPauseGuard) - (kSetPause + 5));
    memcpy(jmp + 1, &rel, 4);
    gSetPauseTramp = tramp;  // set first: our own pauses go to the original from now on
    if (!CodePatch::Write(kSetPause, kSetPauseOrig, jmp, 6, "pause setter 0x00407AB0 (netplay pause guard)")) {
        Logger::Info("PAUSE guard: 0x00407AB0 not as expected; a local pause still stalls netplay");
        gSetPauseTramp = nullptr;
        return false;
    }
    return true;
}

// 'counterscan': dwords of MKKE's writable sections that grow by a constant 1..4 per live frame over three
// consecutive frames (frame counters / per-frame tags), each marked restored (Tier A) or not. A counter that live
// frames advance but a replay neither restores nor advances in step is a candidate for the live-vs-replay residual
// (replays agree with each other but not with live, rbtwice).
std::vector<uint32_t> gCsCopy[3];
std::vector<std::pair<uint32_t, uint32_t>> gCsRanges;
void CounterScan() {
    if (Rollback::gIsResimulating) return;
    if (gCsRanges.empty()) {
        const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(0x00400000);
        const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS32*>(0x00400000 + dos->e_lfanew);
        const IMAGE_SECTION_HEADER* sh = IMAGE_FIRST_SECTION(nt);
        for (int i = 0; i < nt->FileHeader.NumberOfSections; ++i)
            if (sh[i].Characteristics & IMAGE_SCN_MEM_WRITE)
                gCsRanges.push_back({0x00400000 + sh[i].VirtualAddress, sh[i].Misc.VirtualSize & ~3u});
    }
    std::vector<uint32_t>& c = gCsCopy[gCounterScan - 1];
    c.clear();
    for (auto& r : gCsRanges) {
        const uint32_t* p = reinterpret_cast<const uint32_t*>(r.first);
        c.insert(c.end(), p, p + r.second / 4);
    }
    if (++gCounterScan <= 3) return;
    gCounterScan = 0;
    int shown = 0, total = 0;
    size_t idx = 0;
    for (auto& r : gCsRanges)
        for (uint32_t o = 0; o < r.second; o += 4, ++idx) {
            const uint32_t a = gCsCopy[0][idx], b = gCsCopy[1][idx], d = gCsCopy[2][idx];
            const uint32_t s = b - a;
            if (s < 1 || s > 4 || d - b != s) continue;
            ++total;
            if (shown++ < 150)
                Logger::Info("COUNTERSCAN %08X = %08X step %u %s", r.first + o, d, s,
                             Snapshot::TierACovers(r.first + o, 4) ? "restored" : "NOT restored");
        }
    Logger::Info("COUNTERSCAN: %d per-frame counters in MKKE's writable sections", total);
}

void RunFrame() {
    if (gCounterScan) CounterScan();
    std::vector<std::string> todo;
    EnterCriticalSection(&gLock);
    todo.swap(gPending);
    LeaveCriticalSection(&gLock);
    for (auto& l : todo) Execute(l);
    if (gWiProbe) WorldInfoProbe();
    if (gAnimProbe) AnimProbe();
    for (size_t i = 0; i < gScheduled.size();) {
        if (gScheduled[i].framesLeft == 0) {
            const std::string cmd = gScheduled[i].label;
            gScheduled.erase(gScheduled.begin() + i);
            Execute(cmd);
        } else {
            ++i;
        }
    }

    // While paused no fight frame runs, so scripted steps must not be consumed
    // (the current mask is still applied to keep the pad stable).
    const bool paused = *reinterpret_cast<const uint32_t*>(kPauseFlag) != 0;
    const bool ggpo = Rollback::GgpoActive();
    // 'hold' masks may name directions relative to the opponent (combo scripts, tools\combo_inputs.ps1): 0x10000 forward,
    // 0x20000 back, resolved here on the live frame from the fighters' horizontal coordinate (pos[1]; 4 = left = towards
    // smaller values, 8 = right, xray_probe 2026-11-02). Only the resolved mask reaches the game, GGPO and the replays.
    auto resolve = [](int p, uint32_t m) -> uint32_t {
        constexpr uint32_t kFwd = 0x10000, kBack = 0x20000;
        if (!(m & (kFwd | kBack))) return m;
        const uint32_t rel = m & (kFwd | kBack);
        m &= ~(kFwd | kBack);
        if (p > 1) return m;
        const Fighter::State me = Fighter::Read(p), op = Fighter::Read(1 - p);
        if (!me.valid || !op.valid) return m;
        const uint32_t toward = me.pos[1] > op.pos[1] ? 4u : 8u, away = toward == 4u ? 8u : 4u;
        return m | (rel == kFwd ? toward : rel == kBack ? away : 0u);
    };
    for (int p = 0; p < 4; ++p) {
        auto& q = gQueue[p];
        if (ggpo && p < 2) {
            // GGPO session: the fighter pads are applied once per frame after synchronization (RecordInputs);
            // here the scripted mask is only the wanted local input, and the pad is never given to the sampler.
            if (!q.empty()) {
                Input::SetWantedInput(p, resolve(p, q.front().mask));
                if (!paused && --q.front().frames == 0) q.pop_front();
            }
            continue;
        }
        if (q.empty()) {
            if (Input::IsOverridden(p)) Input::ReleasePlayerInput(p);
            continue;
        }
        if (!Input::OverridePlayerInput(p, {resolve(p, q.front().mask)})) {
            Logger::Info("INPUT P%d has no pad; dropping %u queued steps", p + 1, (unsigned)q.size());
            q.clear();
            continue;
        }
        if (!paused && --q.front().frames == 0) q.pop_front();
    }
    for (int p = 0; p < 6; ++p) {  // hardware pads count engine ticks (menus run while unpaused)
        auto& q = gHwQueue[p];
        if (q.empty()) {
            if (Input::HwPadOverridden(p)) Input::ReleaseHwPad(p);
            continue;
        }
        Input::OverrideHwPad(p, q.front().mask);
        if (--q.front().frames == 0) q.pop_front();
    }
}

// Frame advance. Unpausing at the END of a tick lets the next main-loop
// iteration advance the game-frame counter (0x00404E12) normally before the
// fight update; re-pausing at the end of the tick that ran the Nth fight frame.
bool ObjectHooksActive() { return gObjHooks; }

void EndFrame(bool fightRan) {
    if (gAutoKillAi) KillAi(false);
    if (gObjHooks) {
        ObjectHooks::FlushFrame(*reinterpret_cast<const uint32_t*>(*reinterpret_cast<const uint32_t*>(0x00DEF4B4) + 0x70));
    }
    if (gPendingPause >= 0) {
        // Pausing mid-iteration would leave the game-frame counter already advanced
        // for a frame whose fight update is then skipped; the end of EngineTick is
        // the boundary where the next iteration sees a consistent flag.
        CallSetPause(gPendingPause);
        Logger::Frame("PAUSE %d applied at end of tick", gPendingPause);
        gPendingPause = -1;
    }
    if (gPendingGamePause >= 0) {
        reinterpret_cast<SetPauseFn>(kSetPause)(gPendingGamePause);  // through the (guarded) entry, as the game calls it
        Logger::Info("CMD gamepause %d: pause flag now %u", gPendingGamePause, *reinterpret_cast<const uint32_t*>(kPauseFlag));
        gPendingGamePause = -1;
    }
    if (gStepping && fightRan && --gStepRemaining == 0) {
        CallSetPause(1);
        gStepping = false;
        Logger::Frame("STEP done (re-paused)");
    } else if (!gStepping && gStepRemaining > 0 && *reinterpret_cast<const uint32_t*>(kPauseFlag)) {
        CallSetPause(0);
        gStepping = true;
        Logger::Frame("STEP begin (%u frames)", gStepRemaining);
    }
}

void OnFightFrame(uint32_t gameFrame) {
    if (Rollback::gIsResimulating) return;  // test schedules count real (presented) frames only
    for (auto& s : gScheduled)
        if (s.framesLeft) --s.framesLeft;
    for (size_t i = 0; i < gDumps.size();) {
        if (gDumps[i].framesLeft == 0) {
            const bool ok = StateHash::Dump(gDir, gDumps[i].label.c_str(), gameFrame);
            Logger::Frame("DUMP %s gf=%u %s", gDumps[i].label.c_str(), gameFrame, ok ? "ok" : "FAILED");
            gDumps.erase(gDumps.begin() + i);
        } else {
            --gDumps[i].framesLeft;
            ++i;
        }
    }
}

void Shutdown() {
    gDumps.clear();
    gScheduled.clear();
    gStepRemaining = 0;
    gStepping = false;
    for (auto& q : gQueue) q.clear();
    Input::ReleaseAll();
}

}  // namespace CommandChannel




