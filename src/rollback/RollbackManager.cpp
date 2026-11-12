#include "RollbackManager.h"

#include <winsock2.h>  // WSAStartup for GGPO p2p (before windows.h)
#include <windows.h>
#include <psapi.h>

#include <cmath>
#include <cstdio>

#include <cstring>
#include <algorithm>
#include <map>
#include <string>
#include <unordered_map>
#include <vector>

#include "../common/StrAppend.h"
#include "../debug/AllocWatch.h"
#include "../debug/AnimInputTrace.h"
#include "../debug/Overlay.h"
#include "../debug/CommandChannel.h"
#include "../debug/CrashLog.h"
#include "../debug/Lab.h"
#include "../debug/Logger.h"
#include "../debug/Ue3Names.h"
#include "../game/Fighter.h"
#include "../game/Input.h"
#include "../game/MKHeap.h"
#include "../hook/CodePatch.h"
#include "../hook/GameLoopHook.h"
#include "../hook/ObjectHooks.h"
#include "../hook/Ue3Refs.h"
#include "ActorDefer.h"
#include "CinemaRestore.h"
#include "PhysWatch.h"
#include "RngTrace.h"
#include "ParticleDone.h"
#include "HandleGuard.h"
#include "ActorGuard.h"
#include "ScriptLinkGuard.h"
#include "RenderTag.h"
#include "SoundDedupe.h"
#include "StateHash.h"
#include "ggponet.h"
#include <cstdlib>
#include "ScriptClock.h"
#include "ScriptRng.h"
#include "FpuGuard.h"
#include "../net/EnvFingerprint.h"
#include "Rumble.h"
#include "CrowdRng.h"
#include "FramePace.h"
#include "RayQueries.h"
#include "FreeQuarantine.h"
#include "Snapshot.h"
#include "OnlineKeep.h"
#include "../net/HelloProto.h"
#include "../net/OnlineMode.h"
#include "../net/StartSync.h"
#include "../net/SteamBridge.h"

extern "C" int ggpo_prediction_frames_override;  // patched GGPO (tools/build_ggpo.ps1); kept 0
extern "C" int ggpo_last_confirmed_frame;         // patched GGPO: last frame with every player's input received
// Patched GGPO: right before each poll's rollback check, the oldest frame any player's input is still owed for (the
// minimum over connected queues of the last frame received/added). A rollback can only load that frame or a later one
// (the first mispredicted frame follows the last received one; a disconnect rewinds to the peer's last frame), so a
// replay need not save the frames before it.
extern "C" int ggpo_rollback_safe_frame;
// Patched GGPO (sparse replay saves, 2026-11-06): a rollback loads the newest frame at or before its target that this
// callback calls loadable (and whose inputs GGPO still keeps: >= its last confirmed frame), then replays from there;
// ggpo_fallback_loads counts the loads that went back, ggpo_fallback_misses the ones that found nothing loadable.
extern "C" int (*ggpo_frame_loadable)(unsigned char* buf, int len);
extern "C" int ggpo_fallback_loads, ggpo_fallback_misses;
extern "C" int ggpo_quality_report_interval;  // patched GGPO: quality report period in ms ('qualityms N', stock 1000)
extern "C" int ggpo_running_retry_interval;   // patched GGPO: unacked inputs resent after N ms without a new frame ('netretry N', stock 200)
// Patched GGPO (round 3): a side drops input packets until its own handshake is done, so the frame-0 input of the game
// that ran first was lost and came again after 'netretry' (34 ms): the later game waited for it at frame 0 and the
// start barrier's WaitShift took that wait for a lead. Until the peer's first input arrives the inputs are sent again
// every N+1 ms ('firstretry N', 0 = rc27).
extern "C" int ggpo_first_input_retry_ms;
// Patched GGPO (rc25, tools/build_ggpo.ps1):
extern "C" int ggpo_no_disconnect_rewind;     // 1: no load + replay to the peer's last frame at a disconnect (the session stops anyway)
extern "C" int ggpo_bind_loopback;            // 1: GGPO's socket binds 127.0.0.1 only (Steam bridge sessions)
extern "C" int ggpo_rtt_min_ms;               // the lowest of the last 5 quality-report round trips (ms; -1 none yet)
extern "C" int ggpo_sync_rtt_ms;              // the handshake's round trip (ms; -1 none): the only one known at frame 0
extern "C" int ggpo_bye_on_close;             // 1: ggpo_close_session tells the peer the session is over (GgpoStop)
extern "C" int ggpo_bind_failed;              // the WSA error of the session socket's bind, 0 when it worked
extern "C" void (*ggpo_after_send)(void);     // called after each datagram GGPO sends (the bridge forwards it at once)

namespace Rollback {
void LogEventListeners(const char* tag);
bool InstallRollbackModules();
}

namespace {
constexpr uint32_t kActorBytes = 0x28C;  // MKPawn PropertiesSize (Snapshot.cpp kActorSpan)
constexpr int kRing = 12;  // snapshots kept (max rollback depth kRing-1)
constexpr int kInputRing = 128;
constexpr uintptr_t kGameInfoPtr = 0x00DEF4B4;

// The frame n frames before gf, 0 in a timeline's first n frames. The plain subtraction wraps there to about four
// billion, and a release cutoff of that size let go of every block and actor held for a rollback (SyncTest and local
// sessions near frame 0; the netplay start renumbers its frames to 100000).
constexpr uint32_t FramesBack(uint32_t gf, uint32_t n) { return gf > n ? gf - n : 0; }

struct InputFrame {
    uint32_t gf;
    bool valid[2];  // pad was driven by us this frame
    uint32_t mask[2];
};

Snapshot::StateAB gRing[kRing];
bool gHave[kRing] = {};
uint32_t gEpochOf[kRing] = {};
uint32_t gEpoch = 1;  // bumped on every timeline restart (GameFrame reset / jump)
InputFrame gInputs[kInputRing] = {};
Rollback::TickFn gTick = nullptr;
Rollback::TickFn gWorldTick = nullptr;
bool gEnabled = false;
int gPendingFrames = 0;
bool gPendingFake = false;
bool gPendingLoadOnly = false;  // diagnostic: restore snapshot S and stop (no resimulation)
uint32_t gFakeMask = 0;
int gFakeCount = 0;
int gGameplayExact = 0, gGameplayMismatch = 0;

uint32_t& GameFrame() { return *reinterpret_cast<uint32_t*>(*reinterpret_cast<uint32_t*>(kGameInfoPtr) + 0x70); }
uint32_t& HostFrame() { return *reinterpret_cast<uint32_t*>(*reinterpret_cast<uint32_t*>(kGameInfoPtr) + 0x6C); }

Snapshot::StateAB* SlotFor(uint32_t gf) {
    Snapshot::StateAB& s = gRing[gf % kRing];
    return (gHave[gf % kRing] && s.gameFrame == gf && gEpochOf[gf % kRing] == gEpoch) ? &s : nullptr;
}

uint32_t FighterActor(int p);
void RecordAttach(uint32_t gf);
void RecordAttachAll(uint32_t gf);
void RecordMkAttach(uint32_t gf);
void RecordMotionCtl(uint32_t gf, std::vector<uint32_t>* blocks);
void RecordMkMotion(uint32_t gf, std::vector<uint32_t>* blocks);
void RecordPendRel(uint32_t gf);
int RestoreAttach(uint32_t S);

// Save() beside SaveAB (SAVE EXTRAS every 600 saves; a P2P live save cost ~4.8 ms against SAVE PROFILE's ~3.5, 2026-11-04).
struct SaveExtras {
    int64_t t[5] = {};
    int n = 0;
    void Report() {
        if (++n < 600) return;
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        auto ms = [&](int i) { return t[i] * 1000.0 / f.QuadPart / n; };
        Logger::Info("SAVE EXTRAS (ms/save, %d saves): saveab %.3f records %.3f mkattach %.3f cinema+motion %.3f blocks %.3f", n, ms(0),
                     ms(1), ms(2), ms(3), ms(4));
        *this = SaveExtras{};
    }
} gSaveExtras;
// The last refusal was the map's teardown (the MK object heap already gone): the session stops at this timeline's
// restart, so no load can ask for the frame. Not a failed save (gate regression 2026-11-04: the fight's last frame, gf
// 101966, refused once after the deferred live save moved it behind the boundary's poll).
bool gSaveRefusedTeardown = false;
bool Save(uint32_t gf) {
    char why[160] = {};
    int64_t tp = FramePace::Now();
    auto mark = [&](int i) {
        const int64_t now = FramePace::Now();
        gSaveExtras.t[i] += now - tp;
        tp = now;
    };
    OnlineKeep::BeforeSave();
    gSaveRefusedTeardown = false;
    if (!Snapshot::SaveAB(&gRing[gf % kRing], gf, why, sizeof why)) {
        gHave[gf % kRing] = false;
        gSaveRefusedTeardown = !strcmp(why, "object heap not discovered") || !strcmp(why, "master heap ranges not found");
        static uint32_t lastWarn = 0;
        if (gf - lastWarn > 60) {
            Logger::Info("ROLLBACK save gf=%u refused: %s", gf, why);
            lastWarn = gf;
        }
        return false;
    }
    gHave[gf % kRing] = true;
    gEpochOf[gf % kRing] = gEpoch;
    mark(0);
    OnlineKeep::OnSave(gf);
    RayQueries::OnSave(gf);  // the MK object line checks pending at this frame (re-issued by a load to it)
    RecordAttach(gf);
    RecordAttachAll(gf);
    mark(1);
    RecordMkAttach(gf);
    mark(2);
    RecordPendRel(gf);
    // Tell the quarantine which heap blocks this snapshot restores by content.
    static std::vector<uint32_t> blocks;
    blocks.clear();
    RecordMotionCtl(gf, &blocks);
    RecordMkMotion(gf, &blocks);  // after RecordMkAttach: the attached MK actors' controllers
    CinemaRestore::Save(gf, &blocks);
    mark(3);
    for (const Snapshot::Span& s : gRing[gf % kRing].spans)
        if (!strcmp(s.name, "xblock") || !strncmp(s.name, "skel", 4) || !strcmp(s.name, "ue3cell")) blocks.push_back((uint32_t)s.base);
        else if (!strcmp(s.name, "ue3anim")) blocks.push_back((uint32_t)s.base - 0x3C);  // object start
        else if (!strcmp(s.name, "script_module") || !strcmp(s.name, "script_data") || !strcmp(s.name, "actor0") || !strcmp(s.name, "actor1"))
            blocks.push_back((uint32_t)s.base);
        // MK object helpers (+0x1D8): an object destroyed in the abandoned future freed its helper for real, and the
        // object restored from the image then pointed at a freed (or reused) block (soak 13104 S=3405, 2026-11-04).
        else if (Snapshot::gMkHelperRevive && !strcmp(s.name, "mkhelper"))
            blocks.push_back((uint32_t)s.base);
        // MK skeletal objects' name-map blocks: a rehash or the object's death in the abandoned future defers their
        // frees, so a load can always put back the image's header with the saved blocks (LoadAB's NAMEMAP decision).
        else if (!strncmp(s.name, "namemap_", 8))
            blocks.push_back((uint32_t)s.base);
    FreeQuarantine::SetRestoreBlocks(gf % kRing, blocks);
    mark(4);
    gSaveExtras.Report();
    return true;
}

void ApplyRecordedInputs(uint32_t gf) {
    const InputFrame& in = gInputs[gf % kInputRing];
    if (in.gf != gf) return;
    for (int p = 0; p < 2; ++p)
        if (in.valid[p]) Input::OverridePlayerInput(p, {in.mask[p]});
}


// Gameplay-level state (the Gate E criterion): raw heap bytes are not comparable because
// worker-thread pool frees reorder free lists, so compare what the fight is made of.
struct GameplayState {
    float life[2], pos[2][3];
    float meter[2];
    uint32_t rng, gameFrame;
};
GameplayState CaptureGameplay() {
    GameplayState g{};
    for (int p = 0; p < 2; ++p) {
        const Fighter::State f = Fighter::Read(p);
        g.life[p] = f.life;
        memcpy(g.pos[p], f.pos, sizeof g.pos[p]);
        const uintptr_t info = 0x00E89D7C + p * 0xC8C;
        memcpy(&g.meter[p], reinterpret_cast<const void*>(info + 0xC14), 4);
    }
    g.rng = *reinterpret_cast<const uint32_t*>(0x00E7C054);
    g.gameFrame = GameFrame();
    return g;
}

// UE3 attachments of the fighters: actor+0x60 is Base (SetBase is vt+0x190, MK detach 0x00433E40)
// and base+0x68 is a TArray of 0x60-byte records {attached actor, relative location, ...,
// +0x30 base skeletal component}. Soak 26/27 crashes 0x007795AF, 0x007892AB and 0x0043E117 all
// read such a record whose actor is dead or a Base whose list lacks the actor. Logs each new
// inconsistency once (frame, record, actor identity) so it can be tied to a load or a tick.
// rc25 (save/load cost audit): one guarded read per page of the range instead of a VirtualQuery, as the saves' 'fastprobe'
// since rc21. VirtualQuery walks the region forward to report its size: in the multi-MB GMalloc regions that hold the
// fighters, actors and anim nodes it took 3-20 us a call (9 us on the slower field PC), and these helpers run on every
// live and replayed frame (FighterActor, WorldTickedBit, ForceFighterLod, render fields) and many times per load
// (AnimTagAudit per anim node, attach repair, motion controllers, pending releases). Same answer: a page that reads is
// committed and accessible; a guard page fails the read (CrashLog's handler). 'fastreadable 0': VirtualQuery as before.
bool gFastReadable = true;
bool Readable(uint32_t a, uint32_t n) {
    if (a < 0x10000) return false;
    if (gFastReadable && CrashLog::SafeReadAvailable()) {
        const uint32_t last = a + (n ? n - 1 : 0);
        if (last < a) return false;
        uint32_t v;
        for (uint32_t p = a;; p = (p | 0xFFFu) + 1) {  // the first address of each page of the range
            if (!CrashLog::SafeRead32(p & ~3u, &v)) return false;
            if ((p >> 12) == (last >> 12)) return true;
        }
    }
    MEMORY_BASIC_INFORMATION mbi;
    return VirtualQuery(reinterpret_cast<void*>(a), &mbi, sizeof mbi) && mbi.State == MEM_COMMIT &&
           !(mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS)) && a + n <= reinterpret_cast<uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
}
bool Live(uint32_t a) {
    if (!Readable(a, 0x84)) return false;
    const uint32_t idx = *reinterpret_cast<const uint32_t*>(a + 4);
    const uint32_t objs = *reinterpret_cast<const uint32_t*>(0x00E7C0EC), cnt = *reinterpret_cast<const uint32_t*>(0x00E7C0F0);
    return idx < cnt && *reinterpret_cast<const uint32_t*>(objs + 4 * idx) == a;
}
// Every live actor whose Base (+0x60) is a fighter must be in that fighter's attachment list (+0x68, 0x60-byte
// records, actor first): the detach path 0x00793DC0 looks the record up and dereferences it unchecked (st2 2026-10-18,
// crash 0x00793E0E read [0+0x20] in a resim: an effect actor based on a fighter whose kept-live list no longer had it;
// once before in noai-correct1, 2026-10-15). After a load, such an actor is detached (Base = 0): the state it would
// have after SetBase(None), without its side effects. Fighters themselves are RestoreAttach's ('attachrepair 0|1').
bool gAttachRepair = true;
bool gScriptRngWanted = true;  // script RandomInt on a private, snapshotted stream ('scriptrng 0|1', ScriptRng.h)
bool gCrowdRngWanted = true;   // background crowds off the gameplay RNG ('crowdrng 0|1', CrowdRng.h)
int gQuarantineMargin = 8;     // 'qmargin N': freed blocks stay quarantined N frames beyond the snapshot ring
// What the frame boundary last switched the free quarantine to. Cleared wherever else the quarantine is switched off
// (Enable, SetLockstep): the boundary compares against it, and "safe" then "rollback" read in one batch of commands
// with a session armed left the quarantine off for the whole match while this still said on.
bool gQuarantineOn = false;
uint32_t gAttachRepaired = 0;
bool AttachListRoom(uint32_t base, uint32_t need);
int RepairAttachBase(uint32_t S) {
    if (!gAttachRepair) return 0;
    const uint32_t objs = *reinterpret_cast<const uint32_t*>(0x00E7C0EC), cnt = *reinterpret_cast<const uint32_t*>(0x00E7C0F0);
    uint32_t fighter[2] = {0, 0};
    for (int p = 0; p < 2; ++p) {
        const uintptr_t info = 0x00E89D7C + p * 0xC8C;
        const uint32_t obj = *reinterpret_cast<const uint32_t*>(info + 0x1C);
        if (!obj || obj == 0xDEADBEEF || !Readable(obj, 0x10)) continue;
        const uint32_t a = *reinterpret_cast<const uint32_t*>(obj + 8);
        if (Live(a)) fighter[p] = a;
    }
    if (!objs || !cnt || cnt > 0x200000 || (!fighter[0] && !fighter[1]) || !Readable(objs, cnt * 4)) return 0;
    // The Actor class, from a fighter's class chain (SuperStruct +0x38), by name.
    static uint32_t sActorClass = 0;
    if (!sActorClass) {
        std::string chain;
        for (uint32_t c = *reinterpret_cast<const uint32_t*>((fighter[0] ? fighter[0] : fighter[1]) + 0x30), g = 0; c && g < 16 && Readable(c, 0x40);
             c = *reinterpret_cast<const uint32_t*>(c + 0x38), ++g) {
            char nm[64];
            Ue3Names::NameOf(c, nm, sizeof nm);
            chain += std::string(" ") + nm;
            if (!strcmp(nm, "Actor") || !strcmp(nm, "Actor_0")) sActorClass = c;  // NameOf appends the FName number
        }
        Logger::Info("ATTACHREPAIR: Actor class %08X (fighter class chain:%s)", sActorClass, chain.substr(0, 300).c_str());
        if (!sActorClass) gAttachRepair = false;  // fail closed: never guess the class
        if (!sActorClass) return 0;
    }
    // Every read below is a fault-recovering one (CrashLog::SafeRead32: a plain load, no syscall): the checks asked
    // VirtualQuery per object, per class link and per list record, ~23000 objects, and that was 31.8 of the 45.9 ms a load
    // cost (LOAD PROFILE 2026-11-04). Without the crash handler (SafeRead32 unavailable) VirtualQuery as before.
    uint32_t probe = 0;
    const bool guarded = CrashLog::SafeRead32(0x00E7C0EC, &probe);
    auto rd = [guarded](uint32_t a, uint32_t& v) {
        if (guarded) return a >= 0x10000 && CrashLog::SafeRead32(a, &v);
        if (!Readable(a, 4)) return false;
        v = *reinterpret_cast<const uint32_t*>(a);
        return true;
    };
    // A live object's class (+0x30) is a UClass for as long as its package stays loaded: whether it derives from Actor
    // is answered once per class, and the answers are dropped when a new timeline starts (a map change can unload a
    // stage package and give another class the same address; a stale "Actor" would zero +0x60 of a non-actor).
    static std::unordered_map<uint32_t, bool> actorClass;
    static uint32_t lastS = 0;
    if (S < lastS) actorClass.clear();
    lastS = S;
    auto isActor = [&](uint32_t o) {
        uint32_t c = 0;
        if (!rd(o + 0x30, c) || !c) return false;
        const auto it = actorClass.find(c);
        if (it != actorClass.end()) return it->second;
        bool r = false;
        for (uint32_t k = c, g = 0; k && g < 16; ++g) {  // the class chain (SuperStruct +0x38)
            if (k == sActorClass) {
                r = true;
                break;
            }
            if (!rd(k + 0x38, k)) break;
        }
        actorClass.emplace(c, r);
        return r;
    };
    auto live = [&](uint32_t a) {  // Live(): readable through +0x84 and listed in GObjObjects at its index
        uint32_t idx = 0, end = 0;
        return a >= 0x10000 && rd(a + 4, idx) && rd(a + 0x80, end) && idx < cnt && *reinterpret_cast<const uint32_t*>(objs + 4 * idx) == a;
    };
    // Any base actor, not only fighters: the same lookup crashed for an effect attached to another effect
    // (0x0043E117 -> 0x007896F0 null+4, writer_2 2026-10-18).
    int fixed = 0;
    for (uint32_t i = 0; i < cnt; ++i) {
        const uint32_t o = *reinterpret_cast<const uint32_t*>(objs + 4 * i);
        if (!o || o == fighter[0] || o == fighter[1]) continue;
        uint32_t base = 0, flags = 0;
        if (!rd(o + 0x60, base) || !base || base == o || !rd(o + 0x80, flags)) continue;  // readable through +0x84
        const int p = base == fighter[0] ? 0 : base == fighter[1] ? 1 : -1;
        if (!isActor(o) || (flags & 0x20)) continue;  // bDeleteMe: being destroyed
        const bool baseLive = p >= 0 || (live(base) && isActor(base));
        uint32_t n = 0;
        bool listed = false;
        if (baseLive) {
            uint32_t data = 0;
            if (!rd(base + 0x68, data) || !rd(base + 0x6C, n)) n = 0;
            for (uint32_t k = 0; k < n && k < 64 && data; ++k) {
                uint32_t v = 0;
                if (!rd(data + k * 0x60, v)) break;
                listed |= v == o;
            }
        }
        if (listed) continue;
        *reinterpret_cast<uint32_t*>(o + 0x60) = 0;
        ++fixed;
        ++gAttachRepaired;
        static int logged = 0;
        if (logged++ < 20)
            Logger::Info("ATTACHREPAIR S=%u: actor %08X based on %08X (%s) but not in its list (count %u): Base cleared", S, o, base,
                         p == 0 ? "P1" : p == 1 ? "P2" : baseLive ? "actor" : "not a live actor", n);
    }
    return fixed;
}

// The restore audits (Attach/TaskReg/List/Heap) only log; their callers ignore the results. Lab only: each walks a
// whole MK list or heap tree, the heap ones under the game's spinlocks (docs/restructure_plan.md, 1c).
int AttachAudit(const char* when) {
    if (!Lab::On()) return 0;
    static uint32_t seen[64];
    static int nSeen = 0;
    int bad = 0;
    for (int p = 0; p < 2; ++p) {
        const uintptr_t info = 0x00E89D7C + p * 0xC8C;
        const uint32_t obj = *reinterpret_cast<const uint32_t*>(info + 0x1C);
        if (!obj || obj == 0xDEADBEEF || *reinterpret_cast<const uint32_t*>(obj + 4) != *reinterpret_cast<const uint32_t*>(info + 0x20)) continue;
        const uint32_t actor = *reinterpret_cast<const uint32_t*>(obj + 8);
        if (!Live(actor)) continue;
        const uint32_t base = *reinterpret_cast<const uint32_t*>(actor + 0x60);
        auto report = [&](uint32_t key, const char* fmt, uint32_t a, uint32_t b) {
            ++bad;
            for (int i = 0; i < nSeen && i < 64; ++i)  // nSeen counts on past the 64 kept (the oldest are replaced)
                if (seen[i] == key) return;
            seen[nSeen++ & 63] = key;
            char m[200];
            snprintf(m, sizeof m, fmt, a, b);  // a format may use fewer than both
            Logger::Info("ATTACH %s gf=%u p%d actor=%08X: %s", when, GameFrame(), p, actor, m);
        };
        if (base) {
            bool found = false;
            if (!Live(base)) {
                report(actor ^ 0x60, "Base %08X is not a live object", base, 0);
            } else {
                const uint32_t data = *reinterpret_cast<const uint32_t*>(base + 0x68), n = *reinterpret_cast<const uint32_t*>(base + 0x6C);
                for (uint32_t i = 0; i < n && i < 64 && Readable(data + i * 0x60, 4); ++i) found |= *reinterpret_cast<const uint32_t*>(data + i * 0x60) == actor;
                if (!found) report(actor ^ 0x61, "Base %08X does not list it (count %u)", base, n);
            }
        }
        const uint32_t data = *reinterpret_cast<const uint32_t*>(actor + 0x68), n = *reinterpret_cast<const uint32_t*>(actor + 0x6C);
        if (n && !data) report(actor ^ 0x68, "attached list null data with count %u", n, 0);
        for (uint32_t i = 0; i < n && i < 64 && Readable(data + i * 0x60, 4); ++i) {
            const uint32_t a = *reinterpret_cast<const uint32_t*>(data + i * 0x60);
            if (!Live(a)) {
                uint32_t freed = 0, released = 0, caller = 0;
                const bool rel = FreeQuarantine::RecentlyReleased(reinterpret_cast<const void*>(a), 0x10, &freed, &released, &caller);
                report(a, rel ? "attached[%u] %08X dead (quarantine-released)" : "attached[%u] %08X dead", i, a);
            } else if (*reinterpret_cast<const uint32_t*>(a + 0x60) != actor) {
                report(a ^ 1, "attached[%u] %08X has another Base", i, a);
            }
        }
    }
    return bad;
}

// Per-object async work: the world tick (0x007AEC1C) calls 0x0071E960 on the manager at
// [0x00E8436C], which queues one task per registered object (bucket TArray at +0, stride 0x18;
// bucket +0xC/+0x10 = item array/count; item -> object, task on object+0x40), then waits for all
// of them (0x0070E050 -> 0x00661A70). A task stuck on a worker hangs the game thread (soak29
// gf 2006). Layout from static analysis (unverified): each problem is logged once.
int TaskRegAudit(const char* when) {
    if (!Lab::On()) return 0;
    // Interpretation of an item, chosen once from the first pass (whichever makes most items
    // live UObjects): 0 = the object, 1 = pointer to the object, 2 = the object's +0x40 sub-object.
    static int mode = -1;
    static bool disabled = false;
    if (disabled) return 0;
    static uint32_t seen[64];
    static int nSeen = 0;
    const uint32_t mgr = *reinterpret_cast<const uint32_t*>(0x00E8436C);
    if (!Readable(mgr, 0x3C)) return 0;
    const uint32_t buckets = *reinterpret_cast<const uint32_t*>(mgr), nb = *reinterpret_cast<const uint32_t*>(mgr + 4);
    if (nb > 4096 || (nb && !Readable(buckets, nb * 0x18))) return 0;
    auto objectOf = [](uint32_t item, int m) -> uint32_t {
        if (m == 0) return item;
        if (m == 1) return Readable(item, 4) ? *reinterpret_cast<const uint32_t*>(item) : 0;
        return item - 0x40;
    };
    int bad = 0, items = 0, liveBy[3] = {0, 0, 0};
    for (uint32_t b = 0; b < nb; ++b) {
        const uint32_t arr = *reinterpret_cast<const uint32_t*>(buckets + b * 0x18 + 0xC);
        const uint32_t n = *reinterpret_cast<const uint32_t*>(buckets + b * 0x18 + 0x10);
        if (n > 100000 || (n && !Readable(arr, n * 4))) continue;
        for (uint32_t i = 0; i < n; ++i) {
            ++items;
            const uint32_t item = *reinterpret_cast<const uint32_t*>(arr + 4 * i);
            if (mode < 0) {
                for (int m = 0; m < 3; ++m) liveBy[m] += Live(objectOf(item, m));
                continue;
            }
            const uint32_t o = objectOf(item, mode);
            const bool live = Live(o);
            const bool deleted = live && (*reinterpret_cast<const uint32_t*>(o + 0x80) & 0x20);
            if (live && !deleted) continue;
            ++bad;
            bool dup = false;
            for (int s = 0; s < nSeen && s < 64 && !dup; ++s) dup = seen[s] == item;
            if (dup) continue;
            seen[nSeen++ & 63] = item;
            uint32_t freed = 0, released = 0, caller = 0;
            const bool rel = FreeQuarantine::RecentlyReleased(reinterpret_cast<const void*>(o), 0x10, &freed, &released, &caller);
            Logger::Info("TASKREG %s gf=%u bucket %u item %u (%08X): object %08X %s%s", when, GameFrame(), b, i, item, o,
                         live ? "bDeleteMe" : "not a live object", rel ? " (quarantine-released)" : "");
        }
    }
    if (mode < 0) {
        int best = 0;
        for (int m = 1; m < 3; ++m)
            if (liveBy[m] > liveBy[best]) best = m;
        Logger::Info("TASKREG shape at %s: manager %08X, %u buckets, %d items; live as object %d, via pointer %d, as +0x40 %d",
                     when, mgr, nb, items, liveBy[0], liveBy[1], liveBy[2]);
        if (items == 0 || liveBy[best] * 2 <= items) {
            disabled = true;
            Logger::Info("TASKREG items do not resolve to UObjects: audit disabled");
        } else {
            mode = best;
        }
    }
    return bad;
}
// Does snapshot gRefSnap hold the value (aligned dword) anywhere? Used to let a rollback cross a
// UE3 actor destroy that no restored byte refers to (PhysWatch; ~3 ms for the master image).
const Snapshot::StateAB* gRefSnap = nullptr;
bool SnapshotReferences(uint32_t value) {
    if (!gRefSnap) return true;
    for (const std::vector<uint8_t>& b : gRefSnap->bytes) {
        const uint32_t* p = reinterpret_cast<const uint32_t*>(b.data());
        const size_t n = b.size() / 4;
        for (size_t i = 0; i < n; ++i)
            if (p[i] == value) return true;
    }
    return false;
}

// Fighter-to-fighter attachment (grabs/throws): actor+0x60 Base, base+0x68 TArray of 0x60-byte records
// {attached actor, relative location, ..., +0x30 base component}. The actor merge keeps these live (restoring
// them by value crashed), so a load left the abandoned timeline's grab state in place (GGPO synctest: P1's list
// held P2 in the replay but not in the original). Recorded per saved frame and re-established after a load.
struct AttachRec {
    uint32_t gf;
    uint32_t actor[2];
    bool attached[2];       // fighter p based on the other fighter at the save
    uint8_t record[2][0x60];  // its record in the other fighter's list
};
AttachRec gAttach[kRing];

int FindRecord(uint32_t base, uint32_t actor) {
    const uint32_t data = *reinterpret_cast<const uint32_t*>(base + 0x68), n = *reinterpret_cast<const uint32_t*>(base + 0x6C);
    for (uint32_t i = 0; i < n && i < 64 && data; ++i)
        if (*reinterpret_cast<const uint32_t*>(data + i * 0x60) == actor) return (int)i;
    return -1;
}

void RecordAttach(uint32_t gf) {
    AttachRec& r = gAttach[gf % kRing];
    r.gf = gf;
    for (int p = 0; p < 2; ++p) r.actor[p] = FighterActor(p);
    for (int p = 0; p < 2; ++p) {
        r.attached[p] = false;
        const uint32_t self = r.actor[p], other = r.actor[1 - p];
        if (!self || !other || *reinterpret_cast<const uint32_t*>(self + 0x60) != other) continue;
        const int i = FindRecord(other, self);
        if (i < 0) continue;
        memcpy(r.record[p], reinterpret_cast<const void*>(*reinterpret_cast<const uint32_t*>(other + 0x68) + i * 0x60), 0x60);
        r.attached[p] = true;
    }
}

// SetBase(NULL) exactly as the MK detach 0x00433E40 calls it (vt+0x190).
void DetachActor(uint32_t actor) {
    using SetBaseFn = void(__thiscall*)(uint32_t, uint32_t, uint32_t, uint32_t, float, float, float, uint32_t, uint32_t);
    const uint32_t vt = *reinterpret_cast<const uint32_t*>(actor);
    const SetBaseFn setBase = reinterpret_cast<SetBaseFn>(*reinterpret_cast<const uint32_t*>(vt + 0x190));
    setBase(actor, 0, 0, 0, 0.0f, 0.0f, *reinterpret_cast<const float*>(0x00C5A698), 0, 0);
}

// Every actor attached to a fighter (effects on bones, projectiles before launch): the fighter's attachment list
// (+0x68) is kept live by the pointer-safe merge, so after a load it listed the abandoned future's effects and
// lacked those attached at S (ACTORARRAY diagnostic). Saved per frame, reconciled at load ('attachall 0|1').
bool gAttachAll = false;  // twice7: a null-function crash in a resim and a replay pair that disagreed; off
struct FullAttachRec {
    uint32_t gf = 0xFFFFFFFF;
    uint32_t base[2];
    uint32_t n[2];
    uint8_t rec[2][16][0x60];
};
FullAttachRec gFullAttach[kRing];
// A fault-recovering read (CrashLog::SafeRead32, no syscall); VirtualQuery without the crash handler.
bool SafeRd(uint32_t a, uint32_t& v) {
    if (a < 0x10000) return false;
    if (CrashLog::SafeRead32(a, &v)) return true;
    uint32_t probe = 0;
    if (CrashLog::SafeRead32(0x00E7C0EC, &probe) || !Readable(a, 4)) return false;  // handler present: a real fault
    v = *reinterpret_cast<const uint32_t*>(a);
    return true;
}
// [a, a+n) readable: one guarded read per page and the last dword (replaces a VirtualQuery in per-save paths).
bool SafeRange(uint32_t a, uint32_t n) {
    uint32_t v = 0;
    if (!n) return a >= 0x10000;
    for (uint32_t p = a; p < a + n; p = (p & ~0xFFFu) + 0x1000)
        if (!SafeRd(p, v)) return false;
    return SafeRd((a + n - 4) & ~3u, v);
}
bool LiveActorObject(uint32_t a) {
    // A VirtualQuery per call made RecordMkAttach (every save) and the load-side attachment restores slow (2026-11-04).
    if (!a || (a & 3) || !SafeRange(a, 0x100)) return false;
    const uint32_t idx = *reinterpret_cast<const uint32_t*>(a + 4), objs = *reinterpret_cast<const uint32_t*>(0x00E7C0EC),
                   cnt = *reinterpret_cast<const uint32_t*>(0x00E7C0F0);
    if (!objs || idx >= cnt || *reinterpret_cast<const uint32_t*>(objs + 4 * idx) != a) return false;
    const uint32_t vt = *reinterpret_cast<const uint32_t*>(a);
    return vt >= 0x00C55000 && vt < 0x00DE064B;
}
void RecordAttachAll(uint32_t gf) {
    FullAttachRec& r = gFullAttach[gf % kRing];
    r.gf = gf;
    for (int p = 0; p < 2; ++p) {
        r.base[p] = FighterActor(p);
        r.n[p] = 0;
        if (!r.base[p]) continue;
        const uint32_t data = *reinterpret_cast<const uint32_t*>(r.base[p] + 0x68), num = *reinterpret_cast<const uint32_t*>(r.base[p] + 0x6C);
        // An empty list whose storage was released (data 0, num 0) is a valid record of "nothing attached": before
        // 2026-10-26 it counted as unusual, so attachfuture skipped exactly the loads whose list was empty at S (soak
        // 11201-11212: 11205 S=1829 and S=2537, P2 0 at the save, 1 SkeletalMeshActor live, z 0 -> 4.156 in the replay).
        if (!data && !num) continue;
        if (!data || num > 16 || !Readable(data, num * 0x60)) {
            r.base[p] = 0;  // unusual: leave this fighter's list alone at load
            continue;
        }
        memcpy(r.rec[p], reinterpret_cast<const void*>(data), num * 0x60);
        r.n[p] = num;
    }
}
int RestoreAttachAll(uint32_t S) {
    if (!gAttachAll) return 0;
    const FullAttachRec& r = gFullAttach[S % kRing];
    if (r.gf != S) return 0;
    int changes = 0;
    for (int p = 0; p < 2; ++p) {
        const uint32_t base = FighterActor(p), other = FighterActor(1 - p);
        if (!base || base != r.base[p]) continue;
        auto savedHas = [&](uint32_t a) {
            for (uint32_t i = 0; i < r.n[p]; ++i)
                if (*reinterpret_cast<const uint32_t*>(r.rec[p][i]) == a) return true;
            return false;
        };
        // 1. Live attachments that did not exist at S (other than the other fighter, handled by RestoreAttach).
        for (int guard = 0; guard < 32; ++guard) {
            const uint32_t data = *reinterpret_cast<const uint32_t*>(base + 0x68), num = *reinterpret_cast<const uint32_t*>(base + 0x6C);
            uint32_t victim = 0;
            for (uint32_t i = 0; i < num && i < 64 && data; ++i) {
                const uint32_t a = *reinterpret_cast<const uint32_t*>(data + i * 0x60);
                if (a && a != other && !savedHas(a) && LiveActorObject(a) && *reinterpret_cast<const uint32_t*>(a + 0x60) == base) {
                    victim = a;
                    break;
                }
            }
            if (!victim) break;
            DetachActor(victim);
            ++changes;
        }
        // 2. The list in its saved order: records of still-live actors (the other fighter's record is RestoreAttach's).
        uint32_t data = *reinterpret_cast<const uint32_t*>(base + 0x68);
        uint32_t& num = *reinterpret_cast<uint32_t*>(base + 0x6C);
        const uint32_t max = *reinterpret_cast<const uint32_t*>(base + 0x70);
        uint8_t rebuilt[32][0x60];
        uint32_t m = 0;
        for (uint32_t i = 0; i < r.n[p] && m < 32; ++i) {
            const uint32_t a = *reinterpret_cast<const uint32_t*>(r.rec[p][i]);
            if (a == other || !LiveActorObject(a)) continue;
            const uint32_t cur = *reinterpret_cast<const uint32_t*>(a + 0x60);
            if (cur && cur != base) continue;  // now based on something else: its list owns it
            memcpy(rebuilt[m++], r.rec[p][i], 0x60);
        }
        // Keep live records that are neither in the rebuilt set nor detachable (e.g. the other fighter).
        for (uint32_t i = 0; i < num && i < 64 && data && m < 32; ++i) {
            const uint32_t a = *reinterpret_cast<const uint32_t*>(data + i * 0x60);
            bool have = false;
            for (uint32_t k = 0; k < m; ++k) have |= *reinterpret_cast<const uint32_t*>(rebuilt[k]) == a;
            if (!have) memcpy(rebuilt[m++], reinterpret_cast<const void*>(data + i * 0x60), 0x60);
        }
        if (!m && !data) continue;
        if (!AttachListRoom(base, m)) {
            Logger::Info("ATTACHALL P%d: list has no room (need %u, max %u); left live", p + 1, m, max);
            continue;
        }
        data = *reinterpret_cast<const uint32_t*>(base + 0x68);
        if (m != num || memcmp(reinterpret_cast<const void*>(data), rebuilt, m * 0x60)) {
            memcpy(reinterpret_cast<void*>(data), rebuilt, m * 0x60);
            num = m;
            for (uint32_t k = 0; k < m; ++k) {
                const uint32_t a = *reinterpret_cast<const uint32_t*>(rebuilt[k]);
                if (a != other && LiveActorObject(a)) *reinterpret_cast<uint32_t*>(a + 0x60) = base;
            }
            ++changes;
        }
    }
    return changes;
}

// Only the first half of 'attachall': actors attached to a fighter now but not in its list at S (attached in the
// abandoned future; the merge keeps the list live) are detached with the engine's SetBase(NULL), the call RestoreAttach
// uses. No list rebuild or re-basing (the part 'attachall' did too when it crashed, twice7, before effect and MK actors
// had their own state restored). The dominant remaining class after the motion-control fix: 3 of 4 failing loads in
// soak 10901-10912 (P2 list 0 live vs 4 in the replay; P1 7 vs 12). 'attachfuture 0|1' via abtest.
bool gAttachFuture = true;
uint32_t gAttachFutureDetached = 0;
int RestoreAttachFuture(uint32_t S) {
    if (!gAttachFuture) return 0;
    const FullAttachRec& r = gFullAttach[S % kRing];
    if (r.gf != S) return 0;
    int changes = 0;
    for (int p = 0; p < 2; ++p) {
        const uint32_t base = FighterActor(p), other = FighterActor(1 - p);
        if (!base || base != r.base[p]) continue;
        auto savedHas = [&](uint32_t a) {
            for (uint32_t i = 0; i < r.n[p]; ++i)
                if (*reinterpret_cast<const uint32_t*>(r.rec[p][i]) == a) return true;
            return false;
        };
        for (int guard = 0; guard < 32; ++guard) {
            const uint32_t data = *reinterpret_cast<const uint32_t*>(base + 0x68), num = *reinterpret_cast<const uint32_t*>(base + 0x6C);
            uint32_t victim = 0;
            for (uint32_t i = 0; i < num && i < 64 && data; ++i) {
                const uint32_t a = *reinterpret_cast<const uint32_t*>(data + i * 0x60);
                if (a && a != other && !savedHas(a) && LiveActorObject(a) && *reinterpret_cast<const uint32_t*>(a + 0x60) == base &&
                    !(*reinterpret_cast<const uint32_t*>(a + 0x80) & 0x20)) {
                    victim = a;
                    break;
                }
            }
            if (!victim) break;
            char cls[64] = "?";
            const uint32_t c = *reinterpret_cast<const uint32_t*>(victim + 0x30);
            if (c) Ue3Names::NameOf(c, cls, sizeof cls);
            const uint32_t owner = *reinterpret_cast<const uint32_t*>(victim + 0x9C + 4);
            DetachActor(victim);
            ++changes;
            if (gAttachFutureDetached++ < 20)
                Logger::Info("ATTACHFUTURE S=%u: P%d had %08X (%s, MK owner %08X %s) attached since after the save; detached", S, p + 1, victim, cls, owner,
                             owner ? ObjectHooks::TypeOf(owner) : "-");
            if (FindRecord(base, victim) >= 0) {  // the engine left the record: stop rather than loop
                Logger::Info("ATTACHFUTURE S=%u: %08X still listed by P%d after SetBase(NULL); stopping", S, victim, p + 1);
                break;
            }
        }
    }
    return changes;
}

// Room for `need` 0x60-byte records in an actor's attachment list (+0x68 data, +0x6C num, +0x70 max). The live list can
// have released its storage since the save (max 0 after the throw ended): it is grown the way the engine grows a
// TArray, GMalloc->Realloc(data, bytes, 8) through the vtable (0x00401BED ...). SyncTest soak seed 9202 frame 752: P1
// could not be re-attached to P2 (num 0 max 0) and the replayed throw put P1 elsewhere (posz 0 -> 8.4).
bool AttachListRoom(uint32_t base, uint32_t need) {
    uint32_t& data = *reinterpret_cast<uint32_t*>(base + 0x68);
    const uint32_t num = *reinterpret_cast<const uint32_t*>(base + 0x6C);
    uint32_t& max = *reinterpret_cast<uint32_t*>(base + 0x70);
    if (data && need <= max) return true;
    if (need > 32 || num > max) return false;
    const uint32_t gm = *reinterpret_cast<const uint32_t*>(0x00E3EFC8);
    if (!gm || *reinterpret_cast<const uint32_t*>(gm) != 0x00C5ABB4) return false;  // fail closed: unknown allocator
    using ReallocFn = void*(__thiscall*)(void* self, void* ptr, uint32_t size, uint32_t align);
    const auto realloc = reinterpret_cast<ReallocFn>(*reinterpret_cast<const uint32_t*>(0x00C5ABB4 + 0xC));
    const uint32_t newMax = need < 4 ? 4 : need;
    void* p = realloc(reinterpret_cast<void*>(gm), data ? reinterpret_cast<void*>(data) : nullptr, newMax * 0x60, 8);
    if (!p) return false;
    data = reinterpret_cast<uint32_t>(p);
    max = newMax;
    static int logged = 0;
    if (logged++ < 10) Logger::Info("ATTACH restore: list of %08X grown to %u records (needed %u)", base, newMax, need);
    return true;
}

int RestoreAttach(uint32_t S) {
    const AttachRec& r = gAttach[S % kRing];
    if (r.gf != S) return 0;
    int changes = 0;
    for (int p = 0; p < 2; ++p) {
        const uint32_t self = FighterActor(p), other = FighterActor(1 - p);
        if (!self || !other || self != r.actor[p] || other != r.actor[1 - p]) continue;
        const bool liveAttached = *reinterpret_cast<const uint32_t*>(self + 0x60) == other;
        if (liveAttached && !r.attached[p]) {
            DetachActor(self);
            ++changes;
        } else if (r.attached[p]) {
            int i = FindRecord(other, self);
            if (i < 0) {
                uint32_t& num = *reinterpret_cast<uint32_t*>(other + 0x6C);
                if (!AttachListRoom(other, num + 1)) {
                    Logger::Info("ATTACH restore: P%d's list has no room to re-attach P%d (num %u max %u)", 2 - p, p + 1, num,
                                 *reinterpret_cast<const uint32_t*>(other + 0x70));
                    continue;
                }
                i = (int)num++;
            }
            memcpy(reinterpret_cast<void*>(*reinterpret_cast<const uint32_t*>(other + 0x68) + i * 0x60), r.record[p], 0x60);
            *reinterpret_cast<uint32_t*>(self + 0x60) = other;
            ++changes;
        }
    }
    return changes;
}

// Attachments of the other MK objects' actors (projectiles, hit boxes, effects: obj+0x08) at each save. A collision
// object that moves with its base died after S (its hit); the MKObj destructor detached the actor (Base 0, record gone
// from the base's list) before ActorDefer postponed its destruction. At the load the actor came back with Base 0 (the
// mkactor span keeps pointers live), stood still at its spawn point and missed the hit live made (soak 10506 S=2382:
// live Base 69D25F00 and y 317.6 -> 376.8, replay Base 0 and y 317.6; same in 10401, 10507, 10508). Re-attached at
// load when it is detached now and its saved base is a live actor ('mkattach 0|1').
bool gMkAttach = true;
struct MkAttachRec {
    uint32_t actor, index, base;
    uint8_t rec[0x60];
};
struct MkAttachFrame {
    uint32_t gf = 0xFFFFFFFF, n = 0;
    MkAttachRec r[48];
};
MkAttachFrame gMkAttachRing[kRing];
uint32_t gMkReattached = 0;
// The record's own data, for an actor still listed at the load (soak 12704 S=867, 12611 S=2362): a fighter-attached
// collision actor's Location was restored, but the record kept the future's relative transform, and the engine
// re-derived Location from it before the MK tick (bone-attach update 0x007794A0 -> 0x0078D0B0 -> MoveActor; MK's
// SetPosition 0x00433580 writes the relative location through 0x00727C40, its GetPosition 0x00433410 reads record
// +0x04). The record is in the base's AttachedActors block, kept live by the merge. With the same targets (+0x1C
// actor, +0x30 component, +0x34, +0x48), restore +0x04..+0x1B (relative location, rotation) and +0x4C..+0x5B
// (mode flag, extra term, controller left out: +0x5C) ('mkattrec 0|1').
bool gMkAttRec = true;
// 'qmenus 1': the free quarantine also outside sessions (the behaviour before 2026-10-29 19:18), to reproduce the
// menu freeze against the gated default.
bool gQuarantineMenus = false;
uint32_t gMkAttRecRestored = 0;
void RecordMkAttach(uint32_t gf) {
    MkAttachFrame& f = gMkAttachRing[gf % kRing];
    f.gf = gf;
    f.n = 0;
    if (!gMkAttach) return;
    static std::vector<uint32_t> actors;
    actors.clear();
    ObjectHooks::ForEachListed([](uint32_t obj, const char* type, void* c) {
        if (!type || !strstr(type, "MKObj")) return;
        uint32_t a = 0;
        if (CrashLog::SafeRead32(obj + 0x08, &a) && a) static_cast<std::vector<uint32_t>*>(c)->push_back(a);
    }, &actors);
    const uint32_t f0 = FighterActor(0), f1 = FighterActor(1);
    for (uint32_t a : actors) {
        if (f.n >= 48) break;
        if (a == f0 || a == f1 || !LiveActorObject(a)) continue;
        const uint32_t base = *reinterpret_cast<const uint32_t*>(a + 0x60);
        if (!base || base == a || !LiveActorObject(base)) continue;
        const uint32_t data = *reinterpret_cast<const uint32_t*>(base + 0x68), num = *reinterpret_cast<const uint32_t*>(base + 0x6C);
        if (!data || num > 64 || !SafeRange(data, num * 0x60)) continue;
        for (uint32_t k = 0; k < num; ++k)
            if (*reinterpret_cast<const uint32_t*>(data + k * 0x60) == a) {
                MkAttachRec& r = f.r[f.n++];
                r.actor = a;
                r.index = *reinterpret_cast<const uint32_t*>(a + 4);
                r.base = base;
                memcpy(r.rec, reinterpret_cast<const void*>(data + k * 0x60), 0x60);
                break;
            }
    }
}
int RestoreMkAttach(uint32_t S) {
    const MkAttachFrame& f = gMkAttachRing[S % kRing];
    if (!gMkAttach || f.gf != S) return 0;
    int changes = 0;
    for (uint32_t i = 0; i < f.n; ++i) {
        const MkAttachRec& r = f.r[i];
        if (!LiveActorObject(r.actor) || *reinterpret_cast<const uint32_t*>(r.actor + 4) != r.index ||
            (*reinterpret_cast<const uint32_t*>(r.actor + 0x80) & 0x20) || !LiveActorObject(r.base) ||
            (*reinterpret_cast<const uint32_t*>(r.base + 0x80) & 0x20))
            continue;
        uint32_t& cur = *reinterpret_cast<uint32_t*>(r.actor + 0x60);
        if (cur && cur != r.base) continue;  // based on something else now: that list owns it
        if (const int k = FindRecord(r.base, r.actor); k >= 0) {
            if (!cur) cur = r.base;
            uint8_t* live = reinterpret_cast<uint8_t*>(*reinterpret_cast<const uint32_t*>(r.base + 0x68) + k * 0x60);
            const auto same = [&](uint32_t o) { return !memcmp(live + o, r.rec + o, 4); };
            if (gMkAttRec && same(0x1C) && same(0x30) && same(0x34) && same(0x48) &&
                (memcmp(live + 0x04, r.rec + 0x04, 0x18) || memcmp(live + 0x4C, r.rec + 0x4C, 0x10))) {
                float a[3], b[3];
                memcpy(a, live + 0x04, sizeof a);
                memcpy(b, r.rec + 0x04, sizeof b);
                memcpy(live + 0x04, r.rec + 0x04, 0x18);
                memcpy(live + 0x4C, r.rec + 0x4C, 0x10);
                if (gMkAttRecRestored++ < 30 || gMkAttRecRestored % 500 == 0)
                    Logger::Info("MKATTREC S=%u: actor %08X record on %08X restored (relative %.3f,%.3f,%.3f -> %.3f,%.3f,%.3f; %u so far)", S, r.actor,
                                 r.base, a[0], a[1], a[2], b[0], b[1], b[2], gMkAttRecRestored);
            }
            continue;
        }
        uint32_t& num = *reinterpret_cast<uint32_t*>(r.base + 0x6C);
        if (!AttachListRoom(r.base, num + 1)) {
            Logger::Info("MKATTACH S=%u: base %08X has no room to re-attach %08X (num %u)", S, r.base, r.actor, num);
            continue;
        }
        const bool wasDetached = !cur;
        memcpy(reinterpret_cast<void*>(*reinterpret_cast<const uint32_t*>(r.base + 0x68) + num * 0x60), r.rec, 0x60);
        ++num;
        cur = r.base;
        ++changes;
        if (gMkReattached++ < 20)
            Logger::Info("MKATTACH S=%u: actor %08X re-attached to its saved base %08X (was %s)", S, r.actor, r.base, wasDetached ? "detached" : "based, unlisted");
    }
    return changes;
}

// The fighters' Actor.MotionControl (+0x13C, 4 owned controller pointers; set by 0x00786200, which deletes the old
// one; cleared by 0x00785380). The one seen is class 0x00D5A85C (12 bytes {vt, ref, owned block +8}, built by
// 0x00793BA0 from the attachment code 0x00793C50; its methods follow the actor's base, 0x0078D1B0). The actor span
// keeps pointers live, so a load inside a grab kept the future's empty slot: P1/P2 +0x13C live 434769C0 / 4574FEE0 /
// 59C8AAE0, replay 0 at the first replayed frame's FightUpdate entry, then a 1-ulp Location drift (all three drift
// failures of soaks 10601-10712). Saved per frame with the controller's bytes and its owned block's content; both
// blocks are registered with the quarantine, so frees after the save are deferred and handed back at a load. At load
// each slot gets its saved controller again; a controller of the abandoned future is deleted the game's way (vt[0](1)).
// Slots holding another class are left alone ('motionctl 0|1' via abtest).
bool gMotionCtl = true;
constexpr uint32_t kMotionVt = 0x00D5A85C;
constexpr uint32_t kMotionSubMax = 0x200;
constexpr uint32_t kMotionCtlMax = 16;
// The cinematic controllers (see below) are restored too since 2026-11-02 ('abtest cineslots'): an X-ray puts
// 0x00D58B08 into both fighters' slot 2 (SetPhysics 7) on its first frame, and with the cinematic objects now restored
// (CinemaRestore) a load before that frame must also take it out again, and one inside the X-ray put it back.
bool gCineSlots = true;
bool gMotionRefresh = true;  // after a cinematic slot change, the setter's refresh ('abtest motionrefresh')
bool CineMotionVt(uint32_t vt) { return vt == 0x00C66E40 || vt == 0x00D58B08 || vt == 0x00D58B18; }
struct MotionFrame {
    uint32_t gf = 0xFFFFFFFF;
    uint32_t actor[2];
    uint32_t slot[2][4];
    bool known[2][4];  // slot was empty or held a controller of a known class whose blocks were saved
    uint8_t ctl[2][4][kMotionCtlMax];
    uint32_t ctlSize[2][4];
    uint32_t subPtr[2][4], subSize[2][4];
    uint8_t sub[2][4][kMotionSubMax];
};
MotionFrame gMotionRing[kRing];
uint32_t gMotionRestored = 0, gMotionDeleted = 0, gMotionUnknown = 0, gMotionLiveLogs = 0;
// The other classes (all stateless, deleting destructor 0x0052EC10; the dispatcher 0x00786320 runs only the lowest
// non-empty slot): 0x00C66E40 in slot 1, installed by the cinematic event track (0x004BB0F8..0x004BB1C2: X-rays,
// throws), moves the fighter through 0x006C5960; 0x00D58B08/0x00D58B18 in slot 2 (SetPhysics 0x007940C0); 0x00D5A150 in
// slot 3 (root motion of the mesh component, whose state the fightercomp span restores). They are left live: restoring
// slot 1 alone would also need the cinematic objects (SeqAct_Cinema, CinemaGroupInst*, CinemaTrackInst*), which no
// snapshot covers (load_refusals.md section 3): a replay across an X-ray start runs against a cinematic already started.
// Each class change per slot is logged (the first 10 lines used to be spent at the fight start), and every load that
// leaves a different one in place, so a desync can be matched to a cinematic (2026-11-01 19:11, an X-ray's first frame).
uint32_t gMotionLoggedVt[2][4];
void RecordMotionCtl(uint32_t gf, std::vector<uint32_t>* blocks) {
    MotionFrame& f = gMotionRing[gf % kRing];
    f.gf = gf;
    for (int p = 0; p < 2; ++p) {
        const uint32_t a = FighterActor(p);
        f.actor[p] = gMotionCtl ? a : 0;
        for (int k = 0; k < 4; ++k) {
            f.slot[p][k] = f.subPtr[p][k] = f.subSize[p][k] = 0;
            f.known[p][k] = false;
            if (!f.actor[p]) continue;
            const uint32_t c = *reinterpret_cast<const uint32_t*>(a + 0x13C + 4 * k);
            f.slot[p][k] = c;
            if (!c || (Readable(c, 4) && *reinterpret_cast<const uint32_t*>(c) == kMotionVt)) gMotionLoggedVt[p][k] = 0;  // log the next other class
            if (!c) {
                f.known[p][k] = true;
                continue;
            }
            unsigned info[2] = {0, 0};
            if (gCineSlots && Readable(c, 4) && CineMotionVt(*reinterpret_cast<const uint32_t*>(c)) &&
                CrashLog::SafeBlockInfo(reinterpret_cast<const void*>(c), info) && info[1] >= 4 && info[1] <= kMotionCtlMax && Readable(c, info[1])) {
                const uint32_t vt = *reinterpret_cast<const uint32_t*>(c);
                if (vt != gMotionLoggedVt[p][k] && gMotionUnknown++ < 60) {
                    gMotionLoggedVt[p][k] = vt;
                    Logger::Info("MOTIONCTL gf=%u: P%d slot %d holds %08X (vt %08X), a cinematic controller (%u bytes): restored at loads", gf,
                                 p + 1, k, c, vt, info[1]);
                }
                memcpy(f.ctl[p][k], reinterpret_cast<const void*>(c), info[1]);  // stateless: the vtable and the block
                f.ctlSize[p][k] = info[1];
                f.known[p][k] = true;
                blocks->push_back(c);
                continue;
            }
            if (!Readable(c, 12) || *reinterpret_cast<const uint32_t*>(c) != kMotionVt || !CrashLog::SafeBlockInfo(reinterpret_cast<const void*>(c), info) ||
                info[1] < 12) {
                const uint32_t vt = Readable(c, 4) ? *reinterpret_cast<const uint32_t*>(c) : 0;
                if (vt != gMotionLoggedVt[p][k] && gMotionUnknown++ < 60) {
                    gMotionLoggedVt[p][k] = vt;
                    Logger::Info("MOTIONCTL gf=%u: P%d slot %d holds %08X (vt %08X), not the known controller; left live at loads", gf, p + 1,
                                 k, c, vt);
                }
                continue;
            }
            const uint32_t s = *reinterpret_cast<const uint32_t*>(c + 8);
            unsigned sinfo[2] = {0, 0};
            if (s && (!CrashLog::SafeBlockInfo(reinterpret_cast<const void*>(s), sinfo) || sinfo[1] > kMotionSubMax || !Readable(s, sinfo[1]))) continue;
            memcpy(f.ctl[p][k], reinterpret_cast<const void*>(c), 12);
            f.ctlSize[p][k] = 12;
            f.subPtr[p][k] = s;
            f.subSize[p][k] = s ? sinfo[1] : 0;
            if (s) memcpy(f.sub[p][k], reinterpret_cast<const void*>(s), sinfo[1]);
            f.known[p][k] = true;
            blocks->push_back(c);
            if (s) blocks->push_back(s);
        }
    }
}
int RestoreMotionCtl(uint32_t S) {
    const MotionFrame& f = gMotionRing[S % kRing];
    if (!gMotionCtl || f.gf != S) return 0;
    int changes = 0;
    for (int p = 0; p < 2; ++p) {
        const uint32_t a = FighterActor(p);
        if (!a || a != f.actor[p]) continue;
        bool cineChanged = false;
        for (int k = 0; k < 4; ++k) {
            uint32_t& cur = *reinterpret_cast<uint32_t*>(a + 0x13C + 4 * k);
            const uint32_t sv = f.slot[p][k];
            if (!f.known[p][k]) {  // another class at the save: left live (see gMotionLoggedVt)
                if (cur != sv && gMotionLiveLogs++ < 40)
                    Logger::Info("MOTIONCTL S=%u: P%d slot %d differs from the save (saved %08X, now %08X vt %08X); left live", S, p + 1, k, sv,
                                 cur, cur && Readable(cur, 4) ? *reinterpret_cast<const uint32_t*>(cur) : 0);
                continue;
            }
            if (cur != sv) {
                if (cur) {
                    const uint32_t cvt = Readable(cur, 4) ? *reinterpret_cast<const uint32_t*>(cur) : 0;
                    const bool cine = gCineSlots && CineMotionVt(cvt);
                    if (!cine && (!Readable(cur, 12) || cvt != kMotionVt)) {
                        if (gMotionLiveLogs++ < 40)
                            Logger::Info("MOTIONCTL S=%u: P%d slot %d now holds %08X (vt %08X), another class than the save's; left live", S, p + 1,
                                         k, cur, cvt);
                        continue;
                    }
                    const uint32_t future = cur;
                    cur = 0;  // the slot no longer owns it before its destructor runs
                    using DelFn = void*(__thiscall*)(void*, int);
                    reinterpret_cast<DelFn>(*reinterpret_cast<const uint32_t*>(cvt))(reinterpret_cast<void*>(future), 1);  // vt[0]
                    ++gMotionDeleted;
                    cineChanged |= cine;
                }
                cineChanged |= sv && CineMotionVt(*reinterpret_cast<const uint32_t*>(f.ctl[p][k]));
                ++changes;
            }
            if (!sv) continue;
            memcpy(reinterpret_cast<void*>(sv), f.ctl[p][k], f.ctlSize[p][k]);
            if (f.subSize[p][k]) memcpy(reinterpret_cast<void*>(f.subPtr[p][k]), f.sub[p][k], f.subSize[p][k]);
            cur = sv;
        }
        // The setter (0x00786200) ends by refreshing a fighter field from the lowest non-empty slot (its vt[3] fills 8
        // bytes, 0x00799530 writes them to [0x00785310(actor)] +0x20..+0x2C); a cinematic controller is the lowest
        // one while it is in place. 0x00786290 stores a slot and refreshes, without deleting: stored back unchanged.
        if (cineChanged && gMotionRefresh) {
            int low = 0;
            while (low < 3 && !*reinterpret_cast<const uint32_t*>(a + 0x13C + 4 * low)) ++low;
            using StoreFn = void(__thiscall*)(void*, int, uint32_t);
            reinterpret_cast<StoreFn>(0x00786290)(reinterpret_cast<void*>(a), low, *reinterpret_cast<const uint32_t*>(a + 0x13C + 4 * low));
        }
    }
    if (changes && (gMotionRestored += changes) && (gMotionRestored <= 10 || gMotionRestored % 200 == 0))
        Logger::Info("MOTIONCTL S=%u: %d fighter motion-control slot(s) put back to the save (%u so far, %u future controllers deleted)", S, changes,
                     gMotionRestored, gMotionDeleted);
    return changes;
}

// The other MK objects' attached actors (hit boxes, projectiles, props on a fighter: the actors RecordMkAttach lists)
// get the same attachment controller in a motion-control slot when attached (SetBase -> 0x00793DC0 -> 0x00793C50 ->
// 0x00786200), and lose it when detached. Traced soaks 7-9 (2026-11-05: 13119 S=1966, 13129 S=2712, 13130 S=2156,
// 13132 S=3160, all "the original hit, the replay missed"): the hit box was detached in the abandoned future, the load
// re-attached its record (MKATTACH) but not its controller, so on the first replayed frame the hit box ticked before its
// base (write trace: its bTicked write came first, the base's after) and did not follow it; the replay then trailed by
// one frame (LOCSTILL: the base's pose and LocalToWorld were the original's at the check). Saved per frame as the
// fighters' (controller bytes and its owned block, both quarantine restore blocks); at load a slot that is empty again
// gets its saved controller back through the engine's slot store 0x00786290 (stores and refreshes, deletes nothing),
// and a slot still holding the saved controller gets the saved bytes ('abtest mkmotion').
bool gMkMotion = true;
struct MkMotionRec {
    uint32_t actor, index, base;
    uint32_t slot[4];
    bool known[4];
    uint8_t ctl[4][12];
    uint32_t subPtr[4], subSize[4];
    uint8_t sub[4][kMotionSubMax];
};
struct MkMotionFrame {
    uint32_t gf = 0xFFFFFFFF, n = 0;
    MkMotionRec r[48];
};
MkMotionFrame gMkMotionRing[kRing];
uint32_t gMkMotionPut = 0, gMkMotionBytes = 0, gMkMotionLeft = 0;
void RecordMkMotion(uint32_t gf, std::vector<uint32_t>* blocks) {
    MkMotionFrame& m = gMkMotionRing[gf % kRing];
    m.gf = gf;
    m.n = 0;
    const MkAttachFrame& f = gMkAttachRing[gf % kRing];
    if (!gMkMotion || f.gf != gf) return;
    for (uint32_t i = 0; i < f.n && m.n < 48; ++i) {
        MkMotionRec& r = m.r[m.n];
        r.actor = f.r[i].actor;
        r.index = f.r[i].index;
        r.base = f.r[i].base;
        bool any = false;
        for (int k = 0; k < 4; ++k) {
            r.slot[k] = r.subPtr[k] = r.subSize[k] = 0;
            r.known[k] = false;
            uint32_t c = 0, vt = 0, s = 0;
            if (!CrashLog::SafeRead32(r.actor + 0x13C + 4 * k, &c)) break;
            r.slot[k] = c;
            if (!c) {
                r.known[k] = true;
                continue;
            }
            unsigned info[2] = {0, 0}, sinfo[2] = {0, 0};
            if (!CrashLog::SafeRead32(c, &vt) || vt != kMotionVt || !CrashLog::SafeBlockInfo(reinterpret_cast<const void*>(c), info) || info[1] < 12 ||
                !CrashLog::SafeRead32(c + 8, &s))
                continue;  // another class (or not a GMalloc block): left live
            if (s && (!CrashLog::SafeBlockInfo(reinterpret_cast<const void*>(s), sinfo) || sinfo[1] > kMotionSubMax)) continue;
            memcpy(r.ctl[k], reinterpret_cast<const void*>(c), 12);
            r.subPtr[k] = s;
            r.subSize[k] = s ? sinfo[1] : 0;
            if (s) memcpy(r.sub[k], reinterpret_cast<const void*>(s), sinfo[1]);
            r.known[k] = any = true;
            blocks->push_back(c);
            if (s) blocks->push_back(s);
        }
        if (any) ++m.n;  // only actors with a controller of the known class
    }
}
int RestoreMkMotion(uint32_t S) {
    const MkMotionFrame& m = gMkMotionRing[S % kRing];
    if (!gMkMotion || m.gf != S) return 0;
    int changes = 0;
    for (uint32_t i = 0; i < m.n; ++i) {
        const MkMotionRec& r = m.r[i];
        // The same live actor, still (or again, MKATTACH) on its saved base, not being destroyed.
        if (!LiveActorObject(r.actor) || *reinterpret_cast<const uint32_t*>(r.actor + 4) != r.index ||
            (*reinterpret_cast<const uint32_t*>(r.actor + 0x80) & 0x20) || *reinterpret_cast<const uint32_t*>(r.actor + 0x60) != r.base)
            continue;
        for (int k = 0; k < 4; ++k) {
            const uint32_t sv = r.slot[k];
            if (!r.known[k] || !sv) continue;
            uint32_t& cur = *reinterpret_cast<uint32_t*>(r.actor + 0x13C + 4 * k);
            if (cur && cur != sv) {  // another controller since: the actor's own business (no delete here)
                if (gMkMotionLeft++ < 20)
                    Logger::Info("MKMOTION S=%u: actor %08X slot %d holds %08X, the save had %08X; left live", S, r.actor, k, cur, sv);
                continue;
            }
            // The saved blocks: the quarantine deferred their frees and handed them back at this load (OnLoad).
            unsigned info[2] = {0, 0}, sinfo[2] = {0, 0};
            if (!CrashLog::SafeBlockInfo(reinterpret_cast<const void*>(sv), info) || info[1] < 12 ||
                (r.subSize[k] && (!CrashLog::SafeBlockInfo(reinterpret_cast<const void*>(r.subPtr[k]), sinfo) || sinfo[1] < r.subSize[k]))) {
                if (gMkMotionLeft++ < 20)
                    Logger::Info("MKMOTION S=%u: actor %08X slot %d: the saved controller %08X (block %u bytes) is not live; slot left %08X", S, r.actor,
                                 k, sv, info[1], cur);
                continue;
            }
            memcpy(reinterpret_cast<void*>(sv), r.ctl[k], 12);
            if (r.subSize[k]) memcpy(reinterpret_cast<void*>(r.subPtr[k]), r.sub[k], r.subSize[k]);
            if (cur == sv) {
                ++gMkMotionBytes;
                continue;
            }
            using StoreFn = void(__thiscall*)(void*, int, uint32_t);
            reinterpret_cast<StoreFn>(0x00786290)(reinterpret_cast<void*>(r.actor), k, sv);  // store + refresh, no delete
            ++changes;
            if (gMkMotionPut++ < 30 || gMkMotionPut % 200 == 0)
                Logger::Info("MKMOTION S=%u: actor %08X (base %08X) slot %d: saved controller %08X put back (%u so far; %u restored in place)", S,
                             r.actor, r.base, k, sv, gMkMotionPut, gMkMotionBytes);
        }
    }
    return changes;
}

// The engine's pending-release list (singleton from 0x0074C9A0([0x00E86AB0]); TArray of 0x10-byte records at +0x54,
// count +0x58: {object, countdown, kind, 0}). 0x0074FA10 queues a record (kind 1: GMalloc free, countdown 6), a tick
// (0x00753880) counts down and 0x0074FB20 performs the expired ones. The list is live across loads, so a record
// queued in the abandoned timeline still fired in the replay: the destructor 0x00895BA0 of the class 0x00D6A9B0 queues
// its +0x1D8 block, a load restored the object with +0x1D8 set, the replay kept it alive, the free came 6 updates
// later and the quarantine released the block 12 frames after that: 0x008A753A read a zeroed vtable through +0x1D8
// (lag_ns40_d1, 2026-10-28; quarantine "written after free" on the same block just before).
// At a load, kind-1 records the save does not have are removed: their blocks are alive at S. Kinds 0 and 2 (physics
// bodies, actors) are left to PhysWatch. A kind-1 record of the save that the abandoned timeline already performed is
// not re-queued: the quarantine hands its block back and it leaks (re-queuing would double-free a block the quarantine
// did not hold). 'pendrel 0|1' via abtest.
bool gPendRel = true;
constexpr int kPendRelMax = 128;
struct PendRelFrame {
    uint32_t gf = 0xFFFFFFFF;
    int n = -1;  // -1: list not read (too long or unreadable): no restore from this save
    uint32_t rec[kPendRelMax], obj[kPendRelMax], kind[kPendRelMax];
};
PendRelFrame gPendRelRing[kRing];
uint32_t gPendRelRemoved = 0, gPendRelLeaked = 0;
uintptr_t PendRelList() {
    using GetFn = uintptr_t(__cdecl*)(uint32_t);
    const uintptr_t m = reinterpret_cast<GetFn>(0x0074C9A0)(*reinterpret_cast<const uint32_t*>(0x00E86AB0));
    return m && Readable(m + 0x54, 12) ? m : 0;
}
void RecordPendRel(uint32_t gf) {
    PendRelFrame& f = gPendRelRing[gf % kRing];
    f.gf = gf;
    f.n = -1;
    const uintptr_t m = gPendRel ? PendRelList() : 0;
    if (!m) return;
    const uint32_t arr = *reinterpret_cast<const uint32_t*>(m + 0x54);
    const int n = *reinterpret_cast<const int*>(m + 0x58);
    if (n < 0 || n > kPendRelMax || (n && !Readable(arr, n * 4))) return;
    for (int i = 0; i < n; ++i) {
        const uint32_t r = reinterpret_cast<const uint32_t*>(arr)[i];
        if (!Readable(r, 12)) return;
        f.rec[i] = r;
        f.obj[i] = *reinterpret_cast<const uint32_t*>(r);
        f.kind[i] = *reinterpret_cast<const uint32_t*>(r + 8);
    }
    f.n = n;
}
int RestorePendRel(uint32_t S) {
    const PendRelFrame& f = gPendRelRing[S % kRing];
    const uintptr_t m = gPendRel && f.gf == S && f.n >= 0 ? PendRelList() : 0;
    if (!m) return 0;
    uint32_t* arr = *reinterpret_cast<uint32_t**>(m + 0x54);
    int& n = *reinterpret_cast<int*>(m + 0x58);
    if (n < 0 || n > 4 * kPendRelMax || (n && !Readable(reinterpret_cast<uintptr_t>(arr), n * 4))) return 0;
    auto saved = [&](uint32_t r, uint32_t obj) {
        for (int j = 0; j < f.n; ++j)
            if (f.rec[j] == r && f.obj[j] == obj) return true;
        return false;
    };
    int removed = 0;
    for (int i = 0; i < n;) {
        const uint32_t r = arr[i];
        if (!Readable(r, 12) || *reinterpret_cast<const uint32_t*>(r + 8) != 1 || saved(r, *reinterpret_cast<const uint32_t*>(r))) {
            ++i;
            continue;
        }
        if (gPendRelRemoved < 20)
            Logger::Info("PENDREL S=%u: release of %08X queued after the save (countdown %d) removed", S, *reinterpret_cast<const uint32_t*>(r),
                         *reinterpret_cast<const int*>(r + 4));
        memmove(&arr[i], &arr[i + 1], (n - i - 1) * 4);
        --n;
        void* gm = *reinterpret_cast<void**>(0x00E3EFC8);  // the record itself, freed the way 0x0074FB20 frees it
        using FreeFn = void(__thiscall*)(void*, void*);
        reinterpret_cast<FreeFn>((*reinterpret_cast<uint32_t* const*>(gm))[4])(gm, reinterpret_cast<void*>(r));
        ++removed;
        ++gPendRelRemoved;
    }
    for (int j = 0; j < f.n; ++j) {  // performed in the abandoned timeline: diagnostic count only
        if (f.kind[j] != 1) continue;
        bool live = false;
        for (int i = 0; i < n && !live; ++i) live = arr[i] == f.rec[j];
        if (!live && gPendRelLeaked++ < 10)
            Logger::Info("PENDREL S=%u: release of %08X performed after the save is not re-queued (block leaks)", S, f.obj[j]);
    }
    return removed;
}

// MK heap free-tree audit (diagnostic for the 0x0042AF1D crashes: a free chunk holding a reset MK reference
// {0x00D4D980, 0, -1} or an MK id in its tree links). Red-black tree rooted at [heap+0x34]; a node is a free
// chunk: [+4] size|flags with flags 0, +8 parent, +0xC/+0x10 children, +0x14 color. Logs the first broken
// node per session with the frame and the last load, then stops checking.
uint32_t gLastLoadS = 0, gLastLoadAt = 0;
// The global MK object list (header 0xDE8210: head +4, tail +8; nodes {?, obj +4, id +8, next +0xC}), walked every
// frame by 0x0042D950 from 0x0042DE00 (EngineTick). A cycle hung the game there (synctest47, eip 0x0042D96D):
// find the first frame where it is broken and whether a load or a frame broke it.
bool gListAuditBroken = false;
bool OneListAudit(const char* when, uint32_t header, const char* name, bool* broken);
bool ListAudit(const char* when) {
    if (!Lab::On()) return true;
    // The global object list, and the script VM's executor list 0xE39FB0 (walked by the same 0x0042D950 from
    // 0x0045CAF0 right after the fight update): a free-tree crash 0x0042AF1D had this list's header as a heap tree
    // link (synctest58), i.e. a node linked in it had been freed.
    static bool brokenFb = false, brokenFa = false;
    OneListAudit(when, 0x00E39FB0, "script list E39FB0", &brokenFb);
    (void)brokenFa;  // 0xE39FA4 is not a head/tail list (its +8 holds an MK id)
    return OneListAudit(when, 0x00DE8210, "global MK object list", &gListAuditBroken);
}
// Milestone 11 check: MK objects (global list 0xDE8210, node +4 = object) by type, and whether each lies inside the
// snapshotted master ranges (state that rolls back with Tier B). Screen shake (ScreenShakePdata, desc 0xDE8400) is
// reported by name.
void TypeCensus() {
    MKHeap::Region mr[8];
    const int nmr = MKHeap::MasterRanges(mr, 8);
    std::map<std::string, std::pair<int, int>> types;  // type -> {count, outside the master ranges}
    int total = 0, outside = 0;
    const uint32_t head = *reinterpret_cast<const uint32_t*>(0x00DE8210 + 4);
    for (uint32_t node = head; node && total < 200000; node = *reinterpret_cast<const uint32_t*>(node + 0xC)) {
        if (!Readable(node, 0x1C)) break;
        const uint32_t obj = reinterpret_cast<const uint32_t*>(node)[1];
        if (!obj || !Readable(obj, 8)) continue;
        bool in = false;
        for (int i = 0; i < nmr && !in; ++i) in = obj >= mr[i].base && obj < mr[i].base + mr[i].size;
        auto& t = types[ObjectHooks::TypeOf(obj)];
        ++t.first;
        if (!in) {
            ++t.second;
            ++outside;
        }
        ++total;
    }
    char line[900];
    int n = 0;
    for (auto& [name, c] : types)
        if (c.second || name.find("Shake") != std::string::npos)
            n = AppendF(line, sizeof line, n, " %s:%d(out %d)", name.substr(0, 48).c_str(), c.first, c.second);
    line[n] = 0;
    Logger::Info("TYPECENSUS gf=%u: %d MK objects in %d types, %d outside the master ranges; shake/outside:%s", GameFrame(), total,
                 (int)types.size(), outside, line);
    // List headers: every MK list node (vt 0x00C66B14, 0x1C bytes, +0x14 = its list header) in the master image; a
    // static header that Tier A does not cover entirely ({?, head, tail, ?, pending}, 0x14) is rewound only partly by a
    // load (synctest111: 0xE3B694, of which only the head 0xE3B698 was restored, cross-linked the global list).
    std::map<uint32_t, int> headers;
    for (int i = 0; i < nmr; ++i)
        for (uint32_t a = mr[i].base; a + 0x1C <= mr[i].base + mr[i].size; a += 4)
            if (*reinterpret_cast<const uint32_t*>(a) == 0x00C66B14) {
                const uint32_t h = *reinterpret_cast<const uint32_t*>(a + 0x14);
                if (h >= 0x00401000 && h < 0x01000000) ++headers[h];
            }
    n = 0;
    int uncovered = 0;
    for (auto& [h, c] : headers)
        if (!Snapshot::TierACovers(h, 0x14)) {
            ++uncovered;
            n = AppendF(line, sizeof line, n, " %08X(%d nodes)", h, c);
        }
    line[n] = 0;
    Logger::Info("LISTHEADERS gf=%u: %d static list headers referenced by nodes, %d not fully restored by Tier A:%s", GameFrame(),
                 (int)headers.size(), uncovered, line);
}

bool OneListAudit(const char* when, uint32_t header, const char* name, bool* broken) {
    if (*broken) return true;
    const uint32_t head = *reinterpret_cast<const uint32_t*>(header + 4), tail = *reinterpret_cast<const uint32_t*>(header + 8);
    uint32_t slow = head, fast = head, last = 0;
    int n = 0;
    const char* bad = nullptr;
    for (uint32_t node = head; node; node = *reinterpret_cast<const uint32_t*>(node + 0xC)) {
        if (!Readable(node, 0x1C)) {
            bad = "unreadable node";
            break;
        }
        last = node;
        // Membership: a node names its list header at +0x14 (synctest109: a tick-group node of 0xE39EF0 was
        // chained into the global list).
        if (header == 0x00DE8210 && *reinterpret_cast<const uint32_t*>(node + 0x14) != header) {
            bad = "node of another list chained in";
            break;
        }
        if (++n > 200000) {
            bad = "too long";
            break;
        }
        // Floyd: fast advances two nodes per step
        for (int s = 0; s < 2 && fast; ++s) fast = Readable(fast, 0x10) ? *reinterpret_cast<const uint32_t*>(fast + 0xC) : 0;
        slow = *reinterpret_cast<const uint32_t*>(slow + 0xC);
        if (fast && fast == slow) {
            bad = "cycle";
            break;
        }
    }
    if (!bad && last != tail) bad = "tail does not end the list";
    if (!bad) return true;
    *broken = true;
    Logger::Info("LISTAUDIT %s at gf=%u (last load S=%u at gf=%u): %s broken: %s (head %08X tail %08X, %d nodes walked, last %08X)",
                 when, GameFrame(), gLastLoadS, gLastLoadAt, name, bad, head, tail, n, last);
    // The node where the walk ended and the object it names: which object type's node lost its link.
    if (last && Readable(last, 0x1C)) {
        const uint32_t* w = reinterpret_cast<const uint32_t*>(last);
        const uint32_t obj = w[1];
        Logger::Info("LISTAUDIT   last node %08X: %08X %08X %08X %08X %08X %08X %08X; obj %08X type %s", last, w[0], w[1], w[2], w[3], w[4],
                     w[5], w[6], obj, obj && Readable(obj, 8) ? ObjectHooks::TypeOf(obj) : "?");
        const uint32_t tailObj = Readable(tail, 0x1C) ? reinterpret_cast<const uint32_t*>(tail)[1] : 0;
        Logger::Info("LISTAUDIT   tail node %08X: obj %08X type %s", tail, tailObj, tailObj && Readable(tailObj, 8) ? ObjectHooks::TypeOf(tailObj) : "?");
    }
    return false;
}

bool gHeapAuditBroken = false;
bool HeapAudit(const char* when) {
    if (gHeapAuditBroken || !Lab::On()) return true;
    // Readable node test with a per-audit cache of 4 KB pages (VirtualQuery per node would be too slow; a
    // VirtualQuery result always covers the whole 4 KB page it was asked about).
    static uint32_t okPages[1024];
    int nOk = 0;
    auto inImage = [&](uint32_t p) {
        if (p < 0x01000000 || p >= 0xFFFE0000 || (p & 7)) return false;
        const uint32_t pg = p >> 12;
        if (((p + 0x17) >> 12) != pg) return Readable(p, 0x18);
        for (int i = 0; i < nOk; ++i)
            if (okPages[i] == pg) return true;
        if (!Readable(p, 0x18)) return false;
        if (nOk < 1024) okPages[nOk++] = pg;
        return true;
    };
    // The five MK heaps plus GMalloc's medium-block heap (0x00E0AC18, same class, never restored).
    uint32_t heaps[6] = {};
    int nh = 0;
    for (uintptr_t a = 0x00ED53F0; a <= 0x00ED5400; a += 4) heaps[nh++] = *reinterpret_cast<const uint32_t*>(a);
    heaps[nh++] = 0x00E0AC18;
    MKHeap::Region mr[8];
    const int mrn = MKHeap::MasterRanges(mr, 8);
    int outside[5] = {0, 0, 0, 0, 0};
    for (int hi = 0; hi < nh; ++hi) {
        const uint32_t heap = heaps[hi];
        if (!heap || !Readable(heap + 0x34, 4)) continue;
        // Other threads use these heaps: walk under the heap's own spinlock ([heap+0x30]).
        volatile LONG* lk = reinterpret_cast<volatile LONG*>(heap + 0x30);
        bool locked = false;
        for (int spin = 0; spin < 200000 && !locked; ++spin) {
            locked = InterlockedCompareExchange(lk, 1, 0) == 0;
            if (!locked) YieldProcessor();
        }
        if (!locked) continue;
        struct Unlock {
            volatile LONG* l;
            ~Unlock() { InterlockedExchange(l, 0); }
        } unlock{lk};
        static uint32_t stack[4096];
        int sp = 0, visited = 0;
        const uint32_t root = *reinterpret_cast<const uint32_t*>(heap + 0x34);
        if (root) stack[sp++] = root;
        while (sp > 0 && visited < 200000) {
            const uint32_t n = stack[--sp];
            ++visited;
            const char* bad = nullptr;
            if (!inImage(n)) bad = "node outside the image";
            else if (*reinterpret_cast<const uint32_t*>(n + 4) & 7) bad = "node is not a free chunk";
            else if (*reinterpret_cast<const uint8_t*>(n + 0x14) > 1) bad = "bad color";
            uint32_t kids[2] = {0, 0};
            if (!bad)
                for (int c = 0; c < 2; ++c) {
                    kids[c] = *reinterpret_cast<const uint32_t*>(n + 0xC + 4 * c);
                    if (kids[c] && (!inImage(kids[c]) || *reinterpret_cast<const uint32_t*>(kids[c] + 8) != n)) bad = "child link broken";
                }
            if (bad) {
                gHeapAuditBroken = true;
                const uint32_t* w = reinterpret_cast<const uint32_t*>(inImage(n) ? n : 0);
                Logger::Info("HEAPAUDIT %s gf=%u: heap %08X node %08X: %s (last load S=%u at gf %u)%s", when, GameFrame(), heap, n, bad,
                             gLastLoadS, gLastLoadAt, w ? "" : " [unreadable]");
                if (w)
                    Logger::Info("HEAPAUDIT   node dwords: %08X %08X %08X %08X %08X %08X %08X %08X", w[0], w[1], w[2], w[3], w[4], w[5], w[6],
                                 w[7]);
                return false;
            }
            // Restored heaps (hi < 5): a free node outside the master ranges lives in a segment the heap grew into
            // (parent allocator [heap+0x18] vt+0x14 -> 0x00429E90). Tier B does not restore such a segment, so a
            // restored tree linking into it cannot be consistent (suspected cause of the 0x0042AF1D crashes).
            if (hi < 5 && mrn > 0) {
                bool in = false;
                for (int i = 0; i < mrn && !in; ++i) in = n >= mr[i].base && n < mr[i].base + mr[i].size;
                if (!in) ++outside[hi];
            }
            for (int c = 0; c < 2; ++c)
                if (kids[c] && sp < 4096) stack[sp++] = kids[c];
        }
    }
    static int segReports = 0;
    const int total = outside[0] + outside[1] + outside[2] + outside[3] + outside[4];
    if (total && segReports < 20) {
        ++segReports;
        Logger::Info("HEAPSEG %s gf=%u: free nodes outside the master image per heap: %d %d %d %d %d (heaps %08X %08X %08X %08X %08X)", when,
                     GameFrame(), outside[0], outside[1], outside[2], outside[3], outside[4], heaps[0], heaps[1], heaps[2], heaps[3],
                     heaps[4]);
    }
    return true;
}

// Anim node tick tags (USkeletalMeshComponent::TickAnimNodes 0x7627F0): the component increments its
// TickTag (+0x470) each tick and ticks a node of its tick array (+0x290/+0x294, 0xC-byte entries, head at
// +8, chain +0x78) only if node+0x70 != TickTag. A node whose tag was not restored keeps a tag from the
// abandoned future and is skipped on the frame the restored TickTag reaches it (synctest25: a fighter's
// root-motion update ran on different frames in the live and replayed passes). Before the load such a
// node's tag was <= TickTag, so clamping a newer tag to TickTag gives the original tick decisions.
bool gAnimTagFix = true;
// The fighters' skeletal LOD (PredictedLODLevel +0x43C, chosen in 0x76DCB0) follows ForcedLodModel +0x438 when set,
// else the render thread's desired LOD ([comp+0x2B4]+4): render timing then decided which bones a frame evaluated,
// and two replays of the same window differed in comp +0x444 (rbtwice 08:55). Forced to LOD 0 ('forcelod 0|1').
bool gForceLod = true;
void ForceFighterLod() {
    if (!gForceLod) return;
    for (int p = 0; p < 2; ++p) {
        const uint32_t obj = *reinterpret_cast<const uint32_t*>(0x00E89D7C + p * 0xC8C + 0x1C);
        if (!obj || obj == 0xDEADBEEF || !Readable(obj + 0x10C, 4)) continue;
        const uint32_t comp = *reinterpret_cast<const uint32_t*>(obj + 0x10C);
        if (!comp || !Readable(comp, 0x440) || *reinterpret_cast<const uint32_t*>(comp) != 0x00D5A388) continue;
        int32_t* forced = reinterpret_cast<int32_t*>(comp + 0x438);
        if (*forced != 1) *forced = 1;
    }
}
int gResimSleep = 0;  // 'resimsleep N': ms after each resimulated frame (tests races with async work that live frames' pacing gap hides)
bool gTwice = false;
bool gDeferOnlyGgpo = true;
bool gIdleSaves = false;  // 'idlesaves 1': rollback mode snapshots (and audits) every tick, also without a session
bool gTickAudits = false;  // this tick snapshots (OnTickEntry): the phase audits run only then (and in replays)
bool gReleaseDeferredAll = false;  // this tick took no snapshot: FightTickDone destroys every deferred actor
int gTicksWithoutFight = 0;  // consecutive ticks whose fight update did not run  // 'rbtwice 1': every local rollback replays its window twice and compares the replays
int gTwiceSame = 0, gTwiceDiff = 0;
int gAnimTagLogged = 0;
void AnimTagAudit(uint32_t S) {
    int stale = 0, nodes = 0;
    for (int p = 0; p < 2; ++p) {
        const uint32_t obj = *reinterpret_cast<const uint32_t*>(0x00E89D7C + p * 0xC8C + 0x1C);
        if (!obj || obj == 0xDEADBEEF || !Readable(obj + 0x10C, 4)) continue;
        const uint32_t comp = *reinterpret_cast<const uint32_t*>(obj + 0x10C);
        if (!comp || !Readable(comp, 0x760) || *reinterpret_cast<const uint32_t*>(comp) != 0x00D5A388) continue;
        const int32_t tag = *reinterpret_cast<const int32_t*>(comp + 0x470);
        const uint32_t arr = *reinterpret_cast<const uint32_t*>(comp + 0x290);
        const int32_t n = *reinterpret_cast<const int32_t*>(comp + 0x294);
        if (n <= 0 || n > 256 || !Readable(arr, n * 0xC)) continue;
        for (int32_t i = 0; i < n; ++i) {
            uint32_t node = *reinterpret_cast<const uint32_t*>(arr + i * 0xC + 8);
            for (int guard = 0; node && guard < 512 && Readable(node, 0x7C); ++guard) {
                ++nodes;
                int32_t* t = reinterpret_cast<int32_t*>(node + 0x70);
                if (*t - tag > 0) {
                    ++stale;
                    if (gAnimTagFix) *t = tag;
                }
                node = *reinterpret_cast<const uint32_t*>(node + 0x78);
            }
        }
    }
    if (stale && gAnimTagLogged < 30) {
        ++gAnimTagLogged;
        Logger::Info("ANIMTAG load S=%u: %d of %d anim nodes had a tick tag newer than their component's%s", S, stale, nodes,
                     gAnimTagFix ? " (clamped)" : "");
    }
}

// The fighters' components (collision cylinder, mesh) cache their world transform; the snapshot restores the
// actor's Location but not those caches, so until the actor moves again collision against the other fighter used
// the abandoned future's transform. After a load each fighter actor is moved to its own restored Location with its
// SetLocation (vtable +0xB8 = 0x007225C0, the setter root motion uses; it ends in 0x00790230, which updates every
// attached component), so the component state follows from the restored actor exactly as in a live frame.
bool gCompSync = false;  // 'compsync 0|1'. twice6: replays stopped agreeing with each other (5/59 pairs differed), off
void SyncFighterComponents() {
    if (!gCompSync) return;
    using SetLocationFn = int(__thiscall*)(void* actor, const float* loc);
    for (int p = 0; p < 2; ++p) {
        const uint32_t actor = FighterActor(p);
        if (!actor || !Readable(actor, 0xF0)) continue;
        const uint32_t vt = *reinterpret_cast<const uint32_t*>(actor);
        if (vt < 0x00C55000 || vt >= 0x00DE064B || !Readable(vt + 0xB8, 4)) continue;
        const uint32_t fn = *reinterpret_cast<const uint32_t*>(vt + 0xB8);
        if (fn < 0x00401000 || fn >= 0x00C55000) continue;
        float loc[3];
        memcpy(loc, reinterpret_cast<const void*>(actor + 0xDC), sizeof loc);
        reinterpret_cast<SetLocationFn>(fn)(reinterpret_cast<void*>(actor), loc);
    }
}

// The restore half of a rollback (also GGPO's load_game_state): undo post-S object churn, load
// the snapshot, then the bookkeeping that keeps the abandoned timeline's frees/deaths consistent.
struct LoadResult {
    int restored = 0, destroyed = 0, unfreed = 0, actorsBack = 0, retired = 0;
};
// Load cost profile (2026-11-04: a P2P load cost ~48 ms, the frame-pacing hitch of every rollback): per phase, printed
// every 100 loads.
struct LoadProf {
    static constexpr int kN = 9;
    int64_t t[kN] = {}, maxTotal = 0;
    int loads = 0;
    int64_t last = 0, start = 0;
    void Begin() { last = start = FramePace::Now(); }
    void Mark(int i) {
        const int64_t n = FramePace::Now();
        t[i] += n - last;
        last = n;
    }
    void End() {
        const int64_t total = FramePace::Now() - start;
        if (total > maxTotal) maxTotal = total;
        if (++loads < 100) return;
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        auto ms = [&](int64_t v) { return v * 1000.0 / f.QuadPart; };
        int64_t sum = 0;
        for (int64_t v : t) sum += v;
        Logger::Info("LOAD PROFILE (ms/load, %d loads): prerestore %.2f loadab %.2f quarantine %.2f cinema %.2f motion %.2f "
                     "actordefer %.2f attach %.2f postrestore %.2f dangling %.2f, total %.2f (max %.2f)",
                     loads, ms(t[0]) / loads, ms(t[1]) / loads, ms(t[2]) / loads, ms(t[3]) / loads, ms(t[4]) / loads, ms(t[5]) / loads,
                     ms(t[6]) / loads, ms(t[7]) / loads, ms(t[8]) / loads, ms(sum) / loads, ms(maxTotal));
        *this = LoadProf{};
    }
};
LoadProf gLoadProf;
bool LoadSnapshot(const Snapshot::StateAB& snap, LoadResult& r, char* why, size_t whyLen) {
    const uint32_t S = snap.gameFrame;
    gLoadProf.Begin();
    HeapAudit("pre-load");
    ListAudit("pre-load");
    CrashLog::CrumbFrame("rollback:PreRestore", S);
    r.destroyed = ObjectHooks::PreRestore(S);  // sets ActorDefer bypass owners itself
    gLoadProf.Mark(0);
    CrashLog::CrumbFrame("rollback:LoadAB", S);
    r.restored = Snapshot::LoadAB(snap, true, why, whyLen);  // OnlineKeep inside (after PreRestore's frees, under the heap locks)
    gLoadProf.Mark(1);
    {
        static std::vector<std::pair<uintptr_t, uint32_t>> ranges;  // every restored span (scanned for references)
        ranges.clear();
        for (const auto& sp : snap.spans)
            if (sp.base && sp.size >= 4) ranges.push_back({sp.base, sp.size});
        r.unfreed = FreeQuarantine::OnLoad(S, &ranges);
    }
    gLoadProf.Mark(2);
    CinemaRestore::OnLoad(S);  // likewise (instances and their arrays); before PhysWatch drops the destroys it makes
    // The cinema restore runs after the quarantine's requeue scan: blocks it requeued that the restored cinematic
    // objects point at again stay live (combo soak 18, 2026-11-07: GC crashes reading a released object-array block).
    FreeQuarantine::ProtectReferenced(S, CinemaRestore::LastRestoredRanges(), "the cinema restore");
    // The fighters' IK lists put back by LoadAB may name what the cinema restore could not bring back (an X-ray's IK
    // track instance collected since S): removed before the replay's first tick walks them.
    Snapshot::ScrubCompLists(S, CinemaRestore::LastGoneRanges());
    gLoadProf.Mark(3);
    RestoreMotionCtl(S);  // after the quarantine handed the saved controllers' blocks back
    RestorePendRel(S);    // likewise: blocks queued for release after S are alive again
    RayQueries::OnLoad(S);  // MK object line checks pending: drop the abandoned future's, re-issue the saved frame's
    ListAudit("post-load");  // the restored lists themselves (before any engine code runs)
    gLoadProf.Mark(4);
    r.actorsBack = ActorDefer::OnLoad(S);
    PhysWatch::DropFrom(S);
    gLoadProf.Mark(5);
    if (const int a = RestoreAttach(S)) Logger::Frame("ATTACH restore S=%u: %d fighter attachment change(s)", S, a);
    if (const int a = RestoreAttachAll(S)) Logger::Frame("ATTACHALL restore S=%u: %d change(s)", S, a);
    if (const int a = RestoreAttachFuture(S)) Logger::Frame("ATTACHFUTURE restore S=%u: %d future attachment(s) detached", S, a);
    if (const int a = RestoreMkAttach(S)) Logger::Frame("MKATTACH restore S=%u: %d actor(s) re-attached", S, a);
    if (const int a = RestoreMkMotion(S)) Logger::Frame("MKMOTION restore S=%u: %d controller(s) put back", S, a);
    RepairAttachBase(S);  // after every attachment restore and the actor give-back
    AttachAudit("post-load");
    TaskRegAudit("post-load");
    AnimTagAudit(S);
    SyncFighterComponents();
    gLoadProf.Mark(6);
    CrashLog::CrumbFrame("rollback:PostRestore", S);
    r.retired = ObjectHooks::PostRestore(S);
    gLoadProf.Mark(7);
    const int dangling = Ue3Refs::FixDangling(S);
    if (dangling) Logger::Info("ROLLBACK: cleared %d dangling UE3->MK references", dangling);
    gLastLoadS = S;
    gLastLoadAt = GameFrame();
    HeapAudit("post-load");
    ListAudit("post-load");
    AnimInputTrace::OnLoadDone();  // animtrace only: Location trace of a fighter-attached MK actor (LOCLOAD/LOCMOVED)
    gLoadProf.Mark(8);
    gLoadProf.End();
    return r.restored >= 0;
}

// Render-owned actor fields (LastRenderTime +0xAC, the 'recently rendered' bit 0x40 of +0x80) are kept live by
// the actor merge, but the renderer does not run between resimulated frames: replay the values the original
// frame had at its WorldTick entry (GGPO synctest: the first replayed frame's position differed in the last
// bits while only +0xAC differed beside it, 2026-10-15).
struct RenderRec {
    uint32_t gf;
    uint32_t actor[2], time[2], flags[2];
    uint32_t comp[2], compTime[2];  // fighter SkeletalMeshComponent (Pawn.Mesh +0x244) and its LastRenderTime (+0x228)
};
// 'abtest comprender': also replay the mesh component's render-written LastRenderTime. A/B 2026-10-17: ON 39 errors vs
// OFF 26 over ~430 loads per arm, no help. Off.
bool gCompRenderTime = false;
RenderRec gRender[64];
bool gTickedFromWorld = true;  // 'tickparity 0|1': replayed bTicked follows the world's parity (see ReplayRenderFields)
uint32_t FighterActor(int p) {
    const uintptr_t info = 0x00E89D7C + p * 0xC8C;
    const uint32_t obj = *reinterpret_cast<const uint32_t*>(info + 0x1C);
    if (!obj || obj == 0xDEADBEEF || !Readable(obj + 8, 4)) return 0;
    const uint32_t a = *reinterpret_cast<const uint32_t*>(obj + 8);
    return a && Readable(a, 0x100) ? a : 0;
}
void RecordRenderFields(uint32_t gf) {
    RenderRec& r = gRender[gf % 64];
    r.gf = gf;
    for (int p = 0; p < 2; ++p) {
        r.actor[p] = FighterActor(p);
        r.time[p] = r.actor[p] ? *reinterpret_cast<const uint32_t*>(r.actor[p] + 0xAC) : 0;
        r.flags[p] = r.actor[p] ? *reinterpret_cast<const uint32_t*>(r.actor[p] + 0x80) & 0x40 : 0;
        const uint32_t comp = r.actor[p] ? *reinterpret_cast<const uint32_t*>(r.actor[p] + 0x244) : 0;
        r.comp[p] = comp && Readable(comp + 0x228, 4) ? comp : 0;
        r.compTime[p] = r.comp[p] ? *reinterpret_cast<const uint32_t*>(comp + 0x228) : 0;
    }
}
// Bit 0x40 of actor +0x80 is not a render flag: it is bTicked, set by the actor tick prologue 0x007A8F90 from the
// world's tick parity (GWorld +0x130 bit 0, flipped every world tick) so that an actor ticks once per world tick.
// GWorld is not rolled back, so a replayed frame needs bTicked == the world's current parity, exactly as every actor
// that ticked in the previous frame has it at a live frame boundary. Replaying the live frame's recorded bit gave the
// wrong parity after an odd rollback distance (the fighter's actor tick was skipped or doubled in the replay).
uint32_t WorldTickedBit(uint32_t fallback) {
    const uint32_t world = *reinterpret_cast<const uint32_t*>(0x00E86AB0);
    if (!world || !Readable(world + 0x130, 4)) return fallback;
    return (*reinterpret_cast<const uint32_t*>(world + 0x130) & 1u) << 6;
}
void ReplayRenderFields(uint32_t gf) {
    const RenderRec& r = gRender[gf % 64];
    if (r.gf != gf) return;
    for (int p = 0; p < 2; ++p) {
        if (!r.actor[p] || FighterActor(p) != r.actor[p]) continue;
        *reinterpret_cast<uint32_t*>(r.actor[p] + 0xAC) = r.time[p];  // LastRenderTime: render-owned, replayed
        uint32_t& f = *reinterpret_cast<uint32_t*>(r.actor[p] + 0x80);
        f = (f & ~0x40u) | (gTickedFromWorld ? WorldTickedBit(r.flags[p]) : r.flags[p]);
        // The renderer writes the mesh component's LastRenderTime between live frames; the component span restores
        // the value of S, so replayed frames saw an older render time than the live frames had.
        if (gCompRenderTime && r.comp[p] && *reinterpret_cast<const uint32_t*>(r.actor[p] + 0x244) == r.comp[p])
            *reinterpret_cast<uint32_t*>(r.comp[p] + 0x228) = r.compTime[p];
    }
}

// One resimulated frame g (the caller saves g first if it needs it, and advances the frame
// counters afterwards). masks == nullptr: the recorded inputs of g; else {P1, P2} masks.
// HostFrame (GameInfo+0x6C) is kept live by the snapshot; a resimulated frame g gets the value the original
// frame g had at its WorldTick entry instead of the live counter advanced from F.
bool gHostReplay = true;
uint32_t gHostAt[256], gHostAtGf[256];
void ResimFrame(void* engine, float dt, uint32_t g, int k, const uint32_t* masks) {
    if (gHostReplay && gHostAtGf[g % 256] == g) HostFrame() = gHostAt[g % 256];
    ForceFighterLod();
    FreeQuarantine::SetFrame(g);
    ActorDefer::SetFrame(g);
    CinemaRestore::SetFrame(g);
    PhysWatch::SetFrame(g);
    RngTrace::BeginFrame(g, true);
    CrowdRng::BeginFrame();
    CrashLog::TraceFrame(g, true);
    ReplayRenderFields(g);
    ObjectHooks::gPhase = 1;
    CrashLog::CrumbFrame("rollback:resim WorldTick", g);
    FpuGuard::PinGameThread();  // same FP environment on every replayed frame
        AnimInputTrace::ImagePointAt(4);
    gWorldTick(engine, dt);  // vt+0xBC: snapshot boundary is its entry, so it runs for every g
    // Frame g's input is applied where the live frame applies and records it: at EngineTick
    // entry (CommandChannel::RunFrame + RecordInputs). WorldTick still sees frame g-1's mask
    // (restored with the pad span at S); applying it before WorldTick shifted inputs by one tick.
    if (masks) {
        for (int p = 0; p < 2; ++p) Input::OverridePlayerInput(p, {masks[p]});
    } else {
        ApplyRecordedInputs(g);
    }
    ObjectHooks::gPhase = 2;
    CrashLog::CrumbFrame("rollback:resim EngineTick", g);
    AnimInputTrace::ImagePointAt(5);
    // Deaths before the fight update are immediate in a live tick (ActorDefer bypass while FoughtThisTick is false); the
    // replay kept the previous frame's true and deferred them instead.
    GameLoopHook::ClearFoughtThisTick();
    gTick(engine, dt);  // vt+0xC0
    AnimInputTrace::ImagePointAt(6);
    AnimInputTrace::HitReplayFrameDone(g);
    Rollback::OnEngineTickExit();
    if (gResimSleep > 0) Sleep(gResimSleep);
    ObjectHooks::gPhase = 0;
    ObjectHooks::FlushFrame(g);  // stamp this resimulated frame's births/deaths with g
    CrashLog::TraceFrame(0xFFFFFFFF, false);
    RngTrace::EndResimFrame(g, k);
}

bool DoRollback(void* engine, float dt, int k, bool loadOnly = false) {
    const uint32_t F = GameFrame();
    const uint32_t S = F - k;
    Snapshot::StateAB* snap = SlotFor(S);
    char why[200] = {};
    if (k <= 0 || k >= kRing || !snap) {
        Logger::Info("ROLLBACK %d refused: no snapshot for gf=%u", k, S);
        return false;
    }
    if (!ObjectHooks::CanLoadTo(S, why, sizeof why) || Ue3Refs::TornDownSince(S, why, sizeof why) ||
        (gRefSnap = snap, PhysWatch::EventsSince(S, why, sizeof why, &SnapshotReferences))) {
        Logger::Info("ROLLBACK %d refused: %s", k, why);
        return false;
    }
    const uint32_t hostAtF = HostFrame();
    // Exactness probe: live state at F entry before vs. after resimulating back to F.
    uint32_t preSpans[48], postSpans[48], preAll = 0, postAll = 0;
    const char* names[48] = {};
    const bool diag = Rollback::gDiag;
    const int nSpans = diag ? Snapshot::HashAB(preSpans, names, 48, &preAll) : 0;
    const GameplayState gpPre = CaptureGameplay();
    static Snapshot::StateAB pre;  // byte-level copy for diagnosing mismatches
    if (diag) Snapshot::SaveAB(&pre, F, why, sizeof why);
    const LARGE_INTEGER t0 = [] { LARGE_INTEGER t; QueryPerformanceCounter(&t); return t; }();

    if (diag) {
        char when[32];
        snprintf(when, sizeof when, "pre-load S=%u", S);
        Snapshot::CheckFreeLists(*snap, when, 8);
    }
    LoadResult lr;
    if (!LoadSnapshot(*snap, lr, why, sizeof why)) {
        Logger::Info("ROLLBACK %d: LoadAB failed after cleanup: %s", k, why);
        return false;
    }
    const int restored = lr.restored, destroyed = lr.destroyed, unfreed = lr.unfreed, actorsBack = lr.actorsBack,
              retired = lr.retired;
    if (loadOnly) {
        Logger::Info("LOADONLY: restored snapshot gf=%u (%d spans, %d un-freed, %d actors back); now at gf=%u, no resimulation",
                     S, restored, unfreed, actorsBack, GameFrame());
        return true;
    }

    SoundDedupe::BeginResim(S, F);
    Rollback::gIsResimulating = true;
    LARGE_INTEGER tLoaded;
    QueryPerformanceCounter(&tLoaded);
    double msSave = 0, msTick = 0;
    for (uint32_t g = S; g < F; ++g) {
        LARGE_INTEGER a, b, cc;
        QueryPerformanceCounter(&a);
        if (g != S) Save(g);  // keep the ring on the corrected timeline
        QueryPerformanceCounter(&b);
        ResimFrame(engine, dt, g, k, nullptr);
        QueryPerformanceCounter(&cc);
        {
            LARGE_INTEGER fq;
            QueryPerformanceFrequency(&fq);
            msSave += (b.QuadPart - a.QuadPart) * 1000.0 / fq.QuadPart;
            msTick += (cc.QuadPart - b.QuadPart) * 1000.0 / fq.QuadPart;
        }
        // What the main loop does between iterations (0x00404DFC..0x00404E12).
        ++HostFrame();
        ++GameFrame();
    }
    if (gTwice) {
        // Self-consistency: replay the same window again from S. Replays that differ from each other point at
        // nondeterminism (threads, unrestored state touched by the replay itself); identical replays that differ
        // from the live frames point at what only live execution does.
        const GameplayState gpA = CaptureGameplay();
        // Fighter char objects, actors and skeletal components after pass A (no SaveAB here: a capture in the
        // middle of a resimulation, HashAB's SaveAB(gf 0), preceded a crash in pass B).
        struct Region {
            const char* name;
            uint32_t addr, size;
        } regs[6];
        static std::vector<uint8_t> copyA[6];
        for (int p = 0; p < 2; ++p) {
            const uint32_t obj = *reinterpret_cast<const uint32_t*>(0x00E89D7C + p * 0xC8C + 0x1C);
            const bool ok = obj && obj != 0xDEADBEEF && Readable(obj, 0xC580);
            const uint32_t actor = ok ? *reinterpret_cast<const uint32_t*>(obj + 8) : 0;
            const uint32_t comp = ok ? *reinterpret_cast<const uint32_t*>(obj + 0x10C) : 0;
            regs[p * 3] = {p ? "char2" : "char1", ok ? obj : 0u, 0xC580};
            regs[p * 3 + 1] = {p ? "actor2" : "actor1", actor && Readable(actor, kActorBytes) ? actor : 0u, kActorBytes};
            regs[p * 3 + 2] = {p ? "comp2" : "comp1", comp && Readable(comp, 0x700) ? comp : 0u, 0x700};
        }
        for (int r = 0; r < 6; ++r)
            if (regs[r].addr)
                copyA[r].assign(reinterpret_cast<const uint8_t*>(regs[r].addr), reinterpret_cast<const uint8_t*>(regs[r].addr) + regs[r].size);
        LoadResult lr2;
        if (LoadSnapshot(*snap, lr2, why, sizeof why)) {
            for (uint32_t g = S; g < F; ++g) {
                if (g != S) Save(g);
                ResimFrame(engine, dt, g, k, nullptr);
                ++HostFrame();
                ++GameFrame();
            }
            const GameplayState gpB = CaptureGameplay();
            char d[600];
            int dn = 0;
            for (int r = 0; r < 6; ++r) {
                if (!regs[r].addr || copyA[r].size() != regs[r].size || !Readable(regs[r].addr, regs[r].size)) continue;
                int total = 0, shown = 0;
                char part[200];
                int pn = 0;
                for (uint32_t o = 0; o < regs[r].size; o += 4) {
                    uint32_t x, y;
                    memcpy(&x, copyA[r].data() + o, 4);
                    memcpy(&y, reinterpret_cast<const void*>(regs[r].addr + o), 4);
                    if (x == y) continue;
                    ++total;
                    if (shown++ < 4) pn = AppendF(part, sizeof part, pn, " +%X:%08X>%08X", o, x, y);
                }
                part[pn] = 0;
                if (total) dn = AppendF(d, sizeof d, dn, " %s(%d):%s", regs[r].name, total, part);
            }
            d[dn] = 0;
            const bool same = !memcmp(&gpA, &gpB, sizeof gpA);
            ++(same ? gTwiceSame : gTwiceDiff);
            Logger::Info("TWICE k=%d S=%u: replays %s (gameplay); spans differing:%s  [same %d, differ %d]", k, S,
                         same ? "identical" : "DIFFER", dn ? d : " none", gTwiceSame, gTwiceDiff);
        }
    }
    Rollback::gIsResimulating = false;
    Rumble::EndResim();
    const SoundDedupe::Stats snd = SoundDedupe::EndResim(S, F);
    LARGE_INTEGER tCore;
    QueryPerformanceCounter(&tCore);
    CrashLog::Crumb("rollback:post");
    HostFrame() = hostAtF;  // HostFrame is a wall-clock-like counter; it ends where it was
    HeapAudit("post-resim");
    ListAudit("post-resim");
    const GameplayState gpPost = CaptureGameplay();
    if (!memcmp(&gpPre, &gpPost, sizeof gpPre)) {
        Logger::Info("ROLLBACK gameplay exact: life/pos/meter/RNG/GameFrame identical after resimulating %d frames", k);
        ++gGameplayExact;
    } else {
        char diff[512];
        int n = snprintf(diff, sizeof diff, "ROLLBACK gameplay MISMATCH k=%d:", k);
        for (int p = 0; p < 2; ++p) {
            if (gpPre.life[p] != gpPost.life[p])
                n = AppendF(diff, sizeof diff, n, " P%d life %.4f->%.4f", p + 1, gpPre.life[p], gpPost.life[p]);
            if (gpPre.meter[p] != gpPost.meter[p])
                n = AppendF(diff, sizeof diff, n, " P%d meter %.4f->%.4f", p + 1, gpPre.meter[p], gpPost.meter[p]);
            for (int a = 0; a < 3; ++a)
                if (gpPre.pos[p][a] != gpPost.pos[p][a])
                    n = AppendF(diff, sizeof diff, n, " P%d pos%c %.3f->%.3f", p + 1, "xyz"[a], gpPre.pos[p][a],
                                  gpPost.pos[p][a]);
        }
        if (gpPre.rng != gpPost.rng) n = AppendF(diff, sizeof diff, n, " rng %08X->%08X", gpPre.rng, gpPost.rng);
        if (gpPre.gameFrame != gpPost.gameFrame)
            snprintf(diff + n, sizeof diff - n, " gf %u->%u", gpPre.gameFrame, gpPost.gameFrame);
        Logger::Info("%s", diff);
        // Which fighter-object fields diverged (pre = live state at F before the rollback, kept
        // in the 'pre' snapshot's master-heap ranges). Pointer-like dwords are skipped.
        for (int p = 0; diag && p < 2; ++p) {
            const uintptr_t info = 0x00E89D7C + p * 0xC8C;
            const uintptr_t obj = *reinterpret_cast<const uint32_t*>(info + 0x1C);
            if (!obj) continue;
            for (size_t i = 0; i < pre.spans.size(); ++i) {
                const Snapshot::Span& s = pre.spans[i];
                if (obj < s.base || obj + 0xC580 > s.base + s.size) continue;
                const uint8_t* before = pre.bytes[i].data() + (obj - s.base);
                int shown = 0, total = 0;
                char line[256] = "";
                int ln = 0;
                for (uint32_t o = 0; o < 0xC580; o += 4) {
                    uint32_t a, b;
                    memcpy(&a, before + o, 4);
                    memcpy(&b, reinterpret_cast<const void*>(obj + o), 4);
                    if (a == b) continue;
                    const bool ptrA = a >= 0x01000000 && a < 0x30000000, ptrB = b >= 0x01000000 && b < 0x30000000;
                    if (ptrA || ptrB) continue;
                    ++total;
                    if (shown < 24) {
                        ln = AppendF(line, sizeof line, ln, " +%X:%08X>%08X", o, a, b);
                        if (++shown % 6 == 0) {
                            Logger::Info("ROLLBACK diverged P%d char%s", p + 1, line);
                            ln = 0;
                        }
                    }
                }
                if (ln) Logger::Info("ROLLBACK diverged P%d char%s", p + 1, line);
                Logger::Info("ROLLBACK diverged P%d char: %d non-pointer dwords differ", p + 1, total);
                break;
            }
        }
        ++gGameplayMismatch;
    }
    if (!diag) {
    } else if (Snapshot::HashAB(postSpans, names, 48, &postAll), preAll == postAll) {
        Logger::Info("ROLLBACK exactness: state at F identical before/after resimulation (%08X)", preAll);
    } else {
        for (int i = 0; i < nSpans && i < 48; ++i)
            if (preSpans[i] != postSpans[i])
                Logger::Info("ROLLBACK exactness: span %s differs (%08X -> %08X)", names[i], preSpans[i], postSpans[i]);
        // Byte-level: first differing dwords per span (pointer-kept and HostFrame dwords skipped).
        for (size_t i = 0; i < pre.spans.size(); ++i) {
            const Snapshot::Span& s = pre.spans[i];
            const uint8_t* before = pre.bytes[i].data();
            int shown = 0, total = 0;
            for (uint32_t o = 0; o + 4 <= s.size; o += 4) {
                if (!strcmp(s.name, "gameinfo_all") && o == 0x6C) continue;
                bool kept = false;
                for (uint32_t k : s.keepOffsets) kept |= (k == o);
                if (kept) continue;
                uint32_t a, b;
                memcpy(&a, before + o, 4);
                memcpy(&b, reinterpret_cast<const void*>(s.base + o), 4);
                if (a == b) continue;
                ++total;
                if (shown++ < 12)
                    Logger::Frame("EXACT %s %08X(+%X): %08X -> %08X", s.name, (unsigned)(s.base + o), o, a, b);
            }
            if (total) Logger::Frame("EXACT %s: %d differing dwords", s.name, total);
        }
    }

    LARGE_INTEGER t1, f;
    QueryPerformanceCounter(&t1);
    QueryPerformanceFrequency(&f);
    Logger::Info("ROLLBACK core (restore + resimulation): %.3f ms = load %.3f + saves %.3f + ticks %.3f (+ rest)",
                 (tCore.QuadPart - t0.QuadPart) * 1000.0 / f.QuadPart, (tLoaded.QuadPart - t0.QuadPart) * 1000.0 / f.QuadPart,
                 msSave, msTick);
    Logger::Info("ROLLBACK %d frames: gf %u -> %u -> %u, %d spans, post-snapshot MKObj destroyed=%d, retired=%d, "
                 "un-freed=%d, actors back=%d, quarantine=%u, deferred actors=%u, sounds deduped=%d new=%d abandoned=%d, %.3f ms",
                 k, F, S, GameFrame(), restored, destroyed, retired, unfreed, actorsBack, FreeQuarantine::Pending(),
                 ActorDefer::Pending(), snd.deduped, snd.played, snd.abandoned,
                 (t1.QuadPart - t0.QuadPart) * 1000.0 / f.QuadPart);
    Logger::Frame("ROLLBACK done k=%d S=%u F=%u", k, S, F);
    return true;
}

// ---- GGPO (Milestone 12), SyncTest backend only: no network. -------------------------------
// GGPO drives saves/loads/resimulation through the callbacks below. The live frame closes the
// previous frame with ggpo_advance_frame at WorldTick entry (the snapshot boundary: GGPO's save of
// frame G happens there) and adds/synchronizes both players' inputs at EngineTick entry, where
// inputs are applied and recorded today. SyncTest rolls back every 'check distance' frames inside
// ggpo_advance_frame and compares the gameplay checksum of each replayed frame with the original.
GGPOSession* gGgpo = nullptr;
GGPOPlayerHandle gGgpoPlayer[2] = {GGPO_INVALID_HANDLE, GGPO_INVALID_HANDLE};
int gGgpoPendingStart = 0;      // check distance requested by the command ('ggpo synctest N')
bool gGgpoLoadFailed = false;   // a load found no snapshot: stop the session
const char* gGgpoStopWhy = "a load found no snapshot";
bool gPeerLost = false;  // GGPO reported the peer disconnected: stop the session at the boundary
bool gGgpoIsP2P = false;        // peer-to-peer session (else SyncTest)
// Lockstep netplay ('ggpo lockstep 1', then the usual p2p/netplay/steam commands): GGPO's prediction window is 1, so
// a frame only runs once the remote input for it has arrived. Nothing is ever predicted or rolled back, so no
// snapshots are taken and the free quarantine is off: none of the state save/restore risks apply. The cost is
// input delay (the frame delay, default 2) and a stall when the remote's inputs are late.
bool gLockstep = false;
bool gLockstepDeferred = false, gLockstepNext = false;  // 'ggpo lockstep' during a P2P session: applied at its stop
bool gRollbackModules = false;  // the rollback patch set is installed (InstallRollbackModules)
uint32_t gFixSeed = 0;
// In-fight A/B test of a load-side toggle ('abtest <name>|off'): even GGPO loads run with it on, odd loads with it
// off, and each SyncTest error is charged to the arm of the load whose replay produced it. Both arms see the same
// fight, so the comparison is not swamped by the fight-to-fight spread.
bool* gAbToggle = nullptr;
const char* gAbName = "";
int gAbLoads[2] = {0, 0}, gAbErrors[2] = {0, 0}, gAbParity = 0;
bool gAbSaved = false;
bool gGgpoFrameOpen = false;    // inputs added for the current frame; ggpo_advance_frame is due
uint32_t gGgpoLoadedS = 0xFFFFFFFF, gGgpoLiveF = 0, gLiveSavedGf = 0xFFFFFFFF;
void* gGgpoEngine = nullptr;
float gGgpoDt = 0;
bool gGgpoSaveOk = false;
int gGgpoLoads = 0, gGgpoReplayed = 0, gGgpoSyncErrors = 0, gGgpoSaveFails = 0, gGgpoDesyncLogged = 0;

uint32_t GameplayChecksum() {
    const GameplayState g = CaptureGameplay();
    return StateHash::XXH32(&g, sizeof g, 0);
}

// Smoothness (2026-11-04). A P2P boundary saved frame F at OnTickEntry and then, when its poll rolled back, the replay
// saved F again: one save (~3.4 ms) wasted on every frame that rolled back. The live save now waits for the boundary's
// first poll and is skipped when a replay saved F ('lazysave 0': save at OnTickEntry as before). Replays also saved every
// frame they ran, although no rollback can load a frame before ggpo_rollback_safe_frame ('skipconfirmed 0': save all).
// Sparse replay saves (2026-11-06, net9: on a 100 ms link a rollback took 46-68 ms, 12-41 of them saving the replayed
// frames at ~6 ms each, while replaying one frame costs ~1.5 ms): a P2P replay skips the saves of odd frames after the
// safe frame. A later rollback to such a frame loads the even frame before it (patched GGPO, GgpoFrameLoadable) and
// replays one frame more. The frame before an odd target is never skipped: it is even, and it is at or after the safe
// frame of every earlier replay (a target follows the last received frame). 'sparsesave 0': save them all.
bool gLazySave = true, gSkipConfirmed = true, gSparseSave = true;
int gSparseSkips = 0;  // this session's replay saves skipped as odd frames
// Sparse live saves ('sparselive', 2026-11-06): the live save (~4 ms every frame) is skipped on odd GGPO frames too, and
// so is a replay's save of an odd live frame. The same rule holds (even frames are always saved), so a rollback to an
// odd frame loads the frame before and replays one frame more (~1.5 ms, about half the rollbacks). Needs 'sparsesave'.
// Off by default (net12, 2026-11-06): correct (0 load misses, no new desync), but it moves cost from ordinary frames,
// which have slack, into rollback frames, which are the longest: about half the rollbacks replay a frame more, and on a
// slower PC a replayed frame is the expensive part (ns30_d2_spinB6, B +6 ms per frame: fight 1 frames >= 34 ms 42 with
// it, 6 without; ns100_d3 a little worse than net10). Sparse replay saves keep their gain because their fallback is rare.
bool gSparseLive = false;
int gSparseLiveSkips = 0;
int gLivePendingGgpo = -1;              // the GGPO frame number of the deferred live save's request...
uint32_t gLivePendingGgpoGf = 0xFFFFFFFF;  // ...made at this gf
bool gStartBarrier = true;  // 'startbarrier 0': rollback frame 0 runs without waiting for the peer's first input
bool gEarlyInput = true;    // 'earlyinput 0': the local input is added after the poll and the live save (before rc21)
bool gLateSample = true;    // 'latesample 0': controllers sampled at the main loop's call, before MK9's frame wait
bool gSkipLive = true;      // 'skiplive 0': save every live frame, even one whose remote input is already in
uint32_t gConfirmedLiveSkips = 0;  // this session's live saves not taken (frame already confirmed)
uint32_t gEarlyAdds = 0;    // boundaries whose input was added (and sent) at their start
bool gCanonClock = false;   // 'canonclock 1': the world clocks get the same value on both peers at the netplay start
bool gBalance = true;       // 'balance 0': only GGPO's own time-sync events make this game wait for the peer
// 'balancecfg <settle> <samples> <max give>'. 24 4 3 since net4 (2026-11-04, with 'qualityms 200'): against 45 6 3 over the
// network matrix frames >= 34 ms -22%, presents >= 34 ms -19%, rollbacks/min 144 -> 129, depth 1.60 -> 1.49.
int gBalanceSettle = 24, gBalanceSamples = 4, gBalanceMaxGive = 3;
int gBalanceEvents = 0, gBalanceFrames = 0;  // this session's frame-advantage corrections and frames given
// GGPO's own time sync ('ggposync 0|1', default 0 since rc25 while 'balance' is on). GGPO averages 40 frames of its
// estimated advantage and asks for a wait at >= 3 frames, rechecked every poll until it fires and then every 240 frames.
// Its window is not reset after a stall hold or an X-ray lock, so right after a hold had evened the games it still saw
// the run-up and asked for 3-4 more frames (2026-11-09 audit of every results\ log: 76 sessions with holds averaged 2.95
// TIMESYNC events, the 788 without 0.36); BALANCE paid its own give on top, the leader overshot and the other game rolled
// back deeper for about a second. BALANCE samples the same estimate and restarts after holds, so with it on GGPO's events
// are only counted (logged as TIMESYNC ... not paid).
bool gGgpoTimesync = false;
int gTimesyncIgnored = 0;  // this session's GGPO time-sync events left to BALANCE
// Re-poll ('repoll 0|1', rc25, timing audit): when the boundary's first poll lacked the other player's input for this
// frame, the live save (2.6-5 ms) runs before the frame is synchronized; an input that arrived during it was used only a
// frame later, through a one-frame rollback. One more poll after the save picks it up.
bool gRepoll = true;
uint32_t gRepolls = 0, gRepollHits = 0;  // this session's re-polls, and those that brought the frame's input
// Sub-frame phase alignment ('phasesync 0|1', rc25, timing audit). With the input sent at the boundary's start, the
// other game's input for frame G leaves at its boundary G-1 and is in at this game's first poll of G only when the
// one-way trip is shorter than a frame plus how far this game's frame grid trails the other's; the same holds the other
// way with the sign flipped. MK9's limiter keeps each game's grid phase forever (its fmod only drops whole periods) and
// BALANCE / GGPO see whole frames only, so the phase came from each process's history: at delay 1 on a ~22 ms link both
// games get the inputs in time only while the grids are within ~5.7 ms, and otherwise one game rolled back on nearly
// every input change of the other (harness splits 68/271, 115/230, 122/232 rollbacks a minute; the field hello read 33
// or 50 ms by match). Each game counts, per 120-frame window, the boundaries whose first poll already had the other
// player's input (frames paying a debt, holding, X-ray-locked or right after a wait are not counted), sends the share
// to the other game on the checksum link, and the game with the clearly smaller share (its grid leads) moves its frame
// deadline later 0.5 ms per frame, by a step that halves on each overshoot (PhaseDecide), the actuator 'paysmooth' uses. Only the leader slows down,
// so the two never pull against each other; at most kPhaseMaxMs per session (clock drift between PCs is ~0.1 ms/s).
bool gPhaseSync = true;
constexpr int kPhaseWindow = 120, kPhaseMinSamples = 60, kPhaseDeadband = 150;  // frames, frames, per mille
constexpr double kPhasePerFrameMs = 0.5, kPhaseMaxMs = 150.0;
constexpr uint32_t kPhaseMagic = 0x50394B4D;  // 'MK9P': {magic, window, on-time per mille, samples}
int gPhaseOnTime = 0, gPhaseSamples = 0, gPhaseWindowAt = -1;
int gPhaseMyWindow = -1, gPhaseMyQ = -1, gPhasePeerWindow = -1, gPhasePeerQ = -1, gPhaseDecided = -1;
int gPhaseLastWaits = 0, gPhaseQuietUntil = 0;  // barrier waits seen at the last sample; no samples before this frame
double gPhaseDueMs = 0, gPhaseShiftedMs = 0;
int gPhaseCorrections = 0, gPhaseLogs = 0;
// Small debts paid more gently (rc25): 'paysmooth N S' pays debts of up to 2 frames S ms per frame (default 1).
int gPaySmoothSmallMs = 1;
// Status line (rc25): GGPO reported the other game stopped responding (until it responds again or the session stops).
bool gOvPeerInterrupted = false;
// Stall hold (StallHold, below BarrierWait): 'stallhold 0' off, 'stallhold <min ms> <max ms>' its thresholds.
bool gStallHold = true;
// Min 60 since rc18 (batch 32): with GGPO's 34 ms resend (rc17) the game whose packets were lost waited 82-117 ms after a
// 300 ms drop, under the old 100 ms threshold in about half the drops, and then rolled back up to 8 frames (5 and 12
// rollback hitches in two runs); from 60 ms it held after every drop (avg 57 ms) and had 3. No hold in jittery play.
int gStallHoldMinMs = 60, gStallHoldMaxMs = 150;
int gStallHolds = 0;          // this session's holds
double gStallHeldMs = 0;      // and the time they added
// Wait shift (WaitShift, below StallHold): 'waitshift 0' leaves the limiter's catch-up after a wait as it was (rc27).
bool gWaitShift = true;
int gWaitShifts = 0, gWaitShiftFrames = 0;  // this session
// Start alignment (StartSyncBegin, below WaitShift; src/net/StartSync.h): 'startsync 0' leaves the start barrier's shift
// as it was through rc29 (whole frames, the one-way trip guessed from the handshake's round trip).
bool gStartSync = true;
StartSync::Session gSs;
double gSsWaitedMs = 0;  // this game's wait at the start barrier (0: the other game's first input was already in)
int gSsFrameAt = -1;     // the last frame whose boundary read the other game's start datagrams
int gSsMovedUs = 0, gSsTripUs = StartSync::kNone;  // this session: the deadline moved at the start; the measured one-way trip
// Handshake polls ('handshakepoll 0|1', round 3; off). Until GGPO runs the game is paused and the boundary polls once a
// frame: each game answers the other's sync packets up to a frame late and reads the answers up to a frame late, so the
// handshake's round trip (ggpo_sync_rtt_ms, from which the start barrier's WaitShift takes the one-way trip) reads 0-2
// frames more than the wire's (the start barrier takes a frame off: its comment). With 'handshakepoll 1' the paused wait
// polls every millisecond for kHandshakePollMs of each frame and the round trip is the wire's (27-31 ms where the frame
// polls read 29-39). Measured on the timed netsim (batch r3d, 6 fights each, 'netsim 15' at delay 1): the handshake is
// two frames shorter, but the frame-0 wait then no longer tells which game is ahead (26-28 ms on every start; the shift
// of one frame was given back by a BALANCE frame 1.2 s later in 3 of 5 starts, against 0 of 6 with frame polls), so it
// stays off until the start is timed some other way (docs/todo.md, round 3 follow-ups).
bool gHandshakePoll = false;
constexpr double kHandshakePollMs = 12.0;
bool gBalanceRestart = false;  // the frame-advantage sampler starts over (its samples predate the hold)
int gFreezeMs = 0, gFreezeEveryMs = 0;  // 'freezeevery MS EVERY' (test): this game stops MS ms every EVERY ms in a session
ULONGLONG gFreezeNext = 0;
// X-ray lock (XrayLockNow, below StallHold): 'xraylock 0|1', 'xraylockcine 0|1', 'xraylockwindow N', 'xrayfreeze 0|1'.
bool gXrayLock = true, gXrayLockCine = false, gXrayFreeze = true;
int gXrayLockWindow = 60;
bool gXrayCine = false;      // an X-ray cinematic (began inside a press window) is running
int gXrayCineFrame = 0;      // its first live frame (C)
int gXrayCineGone = 0;       // boundaries since its controllers were last seen
uint32_t gXrayHeld = 0;      // the input added before C, added again until the cinematic ends
uint32_t gXrayLastAdded = 0; // the last local input this game added
int gXrayCines = 0, gXrayFrozenFrames = 0;  // this session's
int gXrayLockUntil = -1;          // GGPO frame: frames up to it run on confirmed inputs (a flip-stance press, full meter)
bool gXrayLockOn = false;         // the previous boundary's decision
const char* gXrayLockWhy = "";
int gXrayLockStartFrame = 0, gXrayLockFrameWaits = 0;
double gXrayLockStartWaitMs = 0;
int gXrayLocks = 0, gXrayLockFrames = 0, gXrayLockCineFrames = 0, gXrayLockWaits = 0;  // this session's
double gXrayLockWaitMs = 0;
uint32_t gXrayLockLogs = 0;
uint32_t gLivePending = 0xFFFFFFFF;  // the live frame whose save is deferred to the boundary's first poll
bool gDeferLive = false;             // inside that poll (and the ggpo_advance_frame before it)
int gTickSpinMs = 0;                 // 'tickspin N' (test): N ms of busy CPU per live and replayed session frame (a slower PC)
void TickSpin() {
    if (gTickSpinMs <= 0) return;
    const int64_t t0 = FramePace::Now();
    LARGE_INTEGER f;
    QueryPerformanceFrequency(&f);
    const int64_t end = t0 + f.QuadPart * gTickSpinMs / 1000;
    while (FramePace::Now() < end) YieldProcessor();
    FramePace::Add(FramePace::kExtra, FramePace::Now() - t0);
}
bool SaveLiveNow(uint32_t gf) {
    const int64_t t = FramePace::Now();
    const bool ok = Save(gf);
    FramePace::Add(FramePace::kLiveSave, FramePace::Now() - t);
    gLivePending = 0xFFFFFFFF;
    gLiveSavedGf = ok ? gf : 0xFFFFFFFF;
    return ok;
}
// The live frame's remote input is in, so no rollback can load it ('skiplive'). ggpo_rollback_safe_frame is the last
// frame received from the other player (the local queue is always further on), and a rollback loads the first
// mispredicted frame, which follows it: the live frame F is safe from F <= safe on. Until rc25 the test was F < safe,
// which at delay 1 also needs the input for F+1, sampled by the other game at its own boundary F: it could not have
// arrived, so the skip almost never fired (b38: 0-62 of 1966 frames; 2026-11-09 timing audit). With 'sparselive' an odd
// F+1 is not saved and a rollback to it loads F, so F < safe stays the rule there. A disconnect's rewind to the peer's
// last frame is turned off (ggpo_no_disconnect_rewind): the session stops at the disconnect anyway.
bool LiveFrameConfirmed(int ggpoFrame) {
    if (!gSkipLive || gLockstep || ggpoFrame < 0) return false;
    return gSparseLive ? ggpoFrame < ggpo_rollback_safe_frame : ggpoFrame <= ggpo_rollback_safe_frame;
}

bool __cdecl GgpoBeginGame(const char*) { return true; }
void CsRecord(uint32_t gf, uint32_t cs, const GameplayState& g);
uint32_t CsStartGf();
bool __cdecl GgpoSaveState(unsigned char** buffer, int* len, int* checksum, int ggpoFrame) {
    const uint32_t gf = GameFrame();
    // Live frame: the snapshot taken at this frame's WorldTick entry (OnTickEntry) is the state GGPO
    // means, including SyncTest's frame-0 save requested mid-frame from ggpo_synchronize_input.
    // Replays save the corrected frame.
    if (gLockstep) {
        gGgpoSaveOk = true;  // lockstep: never loaded
    } else if (!Rollback::gIsResimulating) {
        if (gLiveSavedGf == gf) gGgpoSaveOk = true;
        else if (gLivePending == gf) {
            gLivePendingGgpo = ggpoFrame;  // the boundary may skip an odd one (sparselive)
            gLivePendingGgpoGf = gf;
            gGgpoSaveOk = gDeferLive ? true : SaveLiveNow(gf);  // deferred: the boundary saves it
        } else gGgpoSaveOk = Save(gf);
    } else if (gGgpoIsP2P && gSkipConfirmed && ggpoFrame < ggpo_rollback_safe_frame && gf != gGgpoLiveF) {
        gHave[gf % kRing] = false;  // the slot holds this frame's mispredicted state: not loadable
        FramePace::SkippedSave();
        gGgpoSaveOk = true;
    } else if (gGgpoIsP2P && gSkipConfirmed && gf == gGgpoLiveF && LiveFrameConfirmed(ggpoFrame)) {
        // rc25 (save audit): a replay's save of the live frame F whose remote input is already in, the frames the live
        // save skips too ('skiplive', below). It was always taken: one whole save (2.6-5 ms) on the frames that had a
        // rollback, the longest ones. A deferred live save of F is left pending: the boundary's test (same safe frame)
        // skips and counts it; without one (a replay inside a later poll), the stale live save is made unloadable here.
        gHave[gf % kRing] = false;
        if (gf != gLivePending) {
            FramePace::SkippedSave();
            ++gConfirmedLiveSkips;
        }
        gGgpoSaveOk = true;
    } else if (gGgpoIsP2P && gSparseSave && ggpo_frame_loadable && (ggpoFrame & 1) && ggpoFrame > ggpo_rollback_safe_frame &&
               (gf != gGgpoLiveF || (gSparseLive && !gLockstep))) {
        gHave[gf % kRing] = false;  // odd replayed frame: a rollback to it loads the frame before (GgpoFrameLoadable)
        FramePace::SkippedSave();
        if (gf == gGgpoLiveF) {
            if (gf == gLivePending) gLivePending = 0xFFFFFFFF;  // the odd live frame: no live save either
            ++gSparseLiveSkips;
        } else {
            ++gSparseSkips;
        }
        gGgpoSaveOk = true;
    } else {
        if (gf == gLivePending) gLivePending = 0xFFFFFFFF;  // the replay's save of F replaces the deferred live one
        const int64_t t = FramePace::Now();
        gGgpoSaveOk = Save(gf);
        FramePace::Add(FramePace::kReplaySave, FramePace::Now() - t);
    }
    // A P2P save asked for after the game reset GameFrame for the next map (gf below the session's frame 0) cannot be
    // loaded: the session stops at the timeline restart on the next tick. Seen under simulated lag (lag_ns80_d2_oop5).
    if (!gGgpoSaveOk && !(gGgpoIsP2P && gf < CsStartGf()) && !gSaveRefusedTeardown) ++gGgpoSaveFails;
    uint32_t* token = static_cast<uint32_t*>(malloc(sizeof(uint32_t)));
    *token = gf;
    *buffer = reinterpret_cast<unsigned char*>(token);
    *len = sizeof(uint32_t);
    // Per-field report of what desynchronizes: the live save of gf keeps its gameplay state; a replay
    // save of the same gf compares against it (first 40 differences per session).
    static GameplayState liveGp[64];
    static uint32_t liveGf[64];
    const GameplayState gp = CaptureGameplay();
    // Fighter actor bytes (0x300 each) per frame, diffed on a desync to name the fields that differ.
    static uint8_t liveActor[64][2][0x300];
    // Read only by the SyncTest reports below: P2P skips their six VirtualQuery calls a save (rc21, save audit).
    uint32_t actors[2] = {0, 0};
    for (int p = 0; p < 2 && !gGgpoIsP2P; ++p) {
        const uintptr_t info = 0x00E89D7C + p * 0xC8C;
        const uint32_t obj = *reinterpret_cast<const uint32_t*>(info + 0x1C);
        if (obj && obj != 0xDEADBEEF && Readable(obj + 8, 4)) actors[p] = *reinterpret_cast<const uint32_t*>(obj + 8);
        if (actors[p] && !Readable(actors[p], kActorBytes)) actors[p] = 0;
    }
    // MK character objects (0xC580 bytes; restored with the master image): which MK fields differ first.
    static uint8_t liveChar[16][2][0xC580];
    uint32_t chars[2] = {0, 0};
    for (int p = 0; p < 2 && !gGgpoIsP2P; ++p) {
        const uintptr_t info = 0x00E89D7C + p * 0xC8C;
        const uint32_t obj = *reinterpret_cast<const uint32_t*>(info + 0x1C);
        if (obj && obj != 0xDEADBEEF && Readable(obj, 0xC580)) chars[p] = obj;
    }
    if (!Rollback::gIsResimulating && !gGgpoIsP2P) {  // ~100 KB a frame, read only by the SyncTest reports below
        liveGp[gf % 64] = gp;
        liveGf[gf % 64] = gf;
        for (int p = 0; p < 2; ++p) {
            if (actors[p]) memcpy(liveActor[gf % 64][p], reinterpret_cast<const void*>(actors[p]), kActorBytes);
            if (chars[p]) memcpy(liveChar[gf % 16][p], reinterpret_cast<const void*>(chars[p]), 0xC580);
        }
    } else if (!gGgpoIsP2P && liveGf[gf % 64] == gf && !memcmp(&liveGp[gf % 64], &gp, sizeof gp) && gf == gGgpoLoadedS + 1 && (gGgpoReplayed & 63) == 0) {
        Logger::Info("BONEHASH control (no desync) gf=%u:", gf);
        Rollback::BoneHashDump(gf - 1);
    } else if (!gGgpoIsP2P && liveGf[gf % 64] == gf && memcmp(&liveGp[gf % 64], &gp, sizeof gp) && gGgpoDesyncLogged < 40) {
        ++gGgpoDesyncLogged;
        // First replayed frame of a check only (gf == loaded + 1): later frames inherit the difference.
        for (int p = 0; p < 2 && gf == gGgpoLoadedS + 1; ++p) {
            if (!chars[p]) continue;
            char line[400] = "";
            int ln = 0, shown = 0, total = 0;
            for (uint32_t o = 0; o < 0xC580; o += 4) {
                uint32_t a, b;
                memcpy(&a, liveChar[gf % 16][p] + o, 4);
                memcpy(&b, reinterpret_cast<const void*>(chars[p] + o), 4);
                if (a == b) continue;
                ++total;
                if (shown++ < 14) ln = AppendF(line, sizeof line, ln, " +%X:%08X>%08X", o, a, b);
            }
            if (total) Logger::Info("GGPO DESYNC   char P%d: %d dwords differ:%s", p + 1, total, line);
            if (total && Readable(chars[p] + 0xB40, 4)) Rollback::LogSpansContaining(*reinterpret_cast<const uint32_t*>(chars[p] + 0xB40));  // input history
        }
        for (int p = 0; p < 2; ++p) {
            if (!actors[p]) continue;
            char line[300] = "";
            int ln = 0, shown = 0, total = 0;
            for (uint32_t o = 0; o < kActorBytes; o += 4) {
                uint32_t a, b;
                memcpy(&a, liveActor[gf % 64][p] + o, 4);
                memcpy(&b, reinterpret_cast<const void*>(actors[p] + o), 4);
                if (a == b) continue;
                ++total;
                if (shown++ < 10) ln = AppendF(line, sizeof line, ln, " +%X:%08X>%08X", o, a, b);
            }
            if (total) Logger::Info("GGPO DESYNC   actor P%d: %d dwords differ:%s", p + 1, total, line);
            // Attachment records (Base +0x60, list +0x68: {attached actor, rel loc, ..., +0x30 base comp}).
            uint32_t la, lc, ra, rc;
            memcpy(&la, liveActor[gf % 64][p] + 0x68, 4);
            memcpy(&lc, liveActor[gf % 64][p] + 0x6C, 4);
            ra = *reinterpret_cast<const uint32_t*>(actors[p] + 0x68);
            rc = *reinterpret_cast<const uint32_t*>(actors[p] + 0x6C);
            if (la != ra || lc != rc) {
                char names[260];
                int nn = 0;
                for (uint32_t i = 0; i < rc && i < 4 && Readable(ra + i * 0x60, 4); ++i) {
                    char d[80];
                    Ue3Names::Describe(*reinterpret_cast<const uint32_t*>(ra + i * 0x60), d, sizeof d);
                    nn = AppendF(names, sizeof names, nn, " [%s]", d);
                }
                names[nn] = 0;
                Logger::Info("GGPO DESYNC   actor P%d attached: original count %u, replay count %u:%s", p + 1, lc, rc, names);
            }
        }
        const GameplayState& a = liveGp[gf % 64];
        char d[400];
        int n = snprintf(d, sizeof d, "GGPO DESYNC gf=%u (loaded %u):", gf, gGgpoLoadedS);
        for (int p = 0; p < 2; ++p) {
            if (a.life[p] != gp.life[p]) n = AppendF(d, sizeof d, n, " P%d life %.4f->%.4f", p + 1, a.life[p], gp.life[p]);
            if (a.meter[p] != gp.meter[p]) n = AppendF(d, sizeof d, n, " P%d meter %.4f->%.4f", p + 1, a.meter[p], gp.meter[p]);
            for (int c = 0; c < 3; ++c)
                if (a.pos[p][c] != gp.pos[p][c]) n = AppendF(d, sizeof d, n, " P%d pos%c %.3f->%.3f", p + 1, "xyz"[c], a.pos[p][c], gp.pos[p][c]);
        }
        if (a.rng != gp.rng) snprintf(d + n, sizeof d - n, " rng %08X->%08X", a.rng, gp.rng);
        Logger::Info("%s", d);
        {
            char t[900];
            if (AnimInputTrace::Describe(gGgpoLoadedS, gf, t, sizeof t) > 0) Logger::Info("%s", t);
        }
        CrashLog::TraceDump(gf - 1);  // the frame whose execution produced gf
        Rollback::BoneHashDump(gf - 1);
    }
    *checksum = (int)StateHash::XXH32(&gp, sizeof gp, 0);
    if (gGgpoIsP2P) {  // machine-independent: GameFrame differs between peers
        GameplayState x = gp;
        x.gameFrame = 0;
        CsRecord(gf, StateHash::XXH32(&x, sizeof x, 0), x);
        const uint32_t rel = gf - CsStartGf();
        if (!Rollback::gIsResimulating && gf >= CsStartGf() && (rel < 4 || rel % 300 == 0))
            Logger::Info("NETPLAY STATE frame %u: life %.4f %.4f pos %.3f %.3f %.3f / %.3f %.3f %.3f meter %.4f %.4f rng %08X", rel, x.life[0], x.life[1],
                         x.pos[0][0], x.pos[0][1], x.pos[0][2], x.pos[1][0], x.pos[1][1], x.pos[1][2], x.meter[0], x.meter[1], x.rng);
    }
    return true;
}
bool __cdecl GgpoLoadState(unsigned char* buffer, int) {
    const uint32_t S = *reinterpret_cast<const uint32_t*>(buffer);
    if (gLockstep) {
        // Lockstep runs a frame only once the remote input for it is confirmed (the barrier in GgpoBoundaryP2P), so
        // GGPO never mispredicts. It still rewinds when the peer disconnects (DisconnectPlayerQueue: back to the peer's
        // last frame, i.e. the last frame run), and the replay would run that frame again with the same inputs: keep
        // the state, GgpoAdvanceFrame only steps GGPO's count. Stopping here left GGPO's frame count short instead:
        // "GGPO Assertion Failed" (sync.cpp:166) and exit(0) (lockstep_ns60, 2026-10-30).
        Logger::Info("LOCKSTEP: GGPO rewinds to gf=%u from gf=%u (the peer disconnected); those frames ran on confirmed "
                     "inputs, nothing is replayed", S, GameFrame());
        return true;
    }
    if (gGgpoLoadFailed) return false;  // the session is being stopped already (no snapshot, or a replay started the next map)
    CrashLog::TraceFrame(0xFFFFFFFF, false);  // the load's own writes are not a frame's
    Snapshot::StateAB* snap = SlotFor(S);
    char why[200] = {};
    if (!snap) {
        Logger::Info("GGPO load gf=%u: no snapshot (ring lost it); the session is stopped", S);
        gGgpoLoadFailed = true;  // replays would run on unrestored state
        return false;
    }
    if (Lab::On() && (!ObjectHooks::CanLoadTo(S, why, sizeof why) || Ue3Refs::TornDownSince(S, why, sizeof why) ||
                      (gRefSnap = snap, PhysWatch::EventsSince(S, why, sizeof why, &SnapshotReferences))))  // log only
        Logger::Info("GGPO load gf=%u would be refused by the local engine (%s); loading anyway", S, why);
    SoundDedupe::BeginResim(S, gGgpoLiveF);  // at every load: a second one in this boundary replays its frames again
    if (gGgpoLoadedS == 0xFFFFFFFF) Rollback::gIsResimulating = true;
    if (gAbToggle) {
        gAbParity = gGgpoLoads & 1;
        *gAbToggle = gAbParity == 0;
        ++gAbLoads[gAbParity];
    }
    LoadResult lr;
    const int64_t tl = FramePace::Now();
    if (!LoadSnapshot(*snap, lr, why, sizeof why)) Logger::Info("GGPO load gf=%u: LoadAB failed: %s", S, why);
    FramePace::Add(FramePace::kLoad, FramePace::Now() - tl);
    if (S < gGgpoLoadedS) gGgpoLoadedS = S;
    ++gGgpoLoads;
    return true;
}
// Patched GGPO asks this before a rollback load (sparse replay saves): does the ring still hold this frame's state?
int GgpoFrameLoadable(unsigned char* buffer, int len) {
    return buffer && len >= (int)sizeof(uint32_t) && SlotFor(*reinterpret_cast<const uint32_t*>(buffer)) ? 1 : 0;
}
bool __cdecl GgpoLogState(char*, unsigned char*, int) { return true; }
void __cdecl GgpoFreeBuffer(void* buffer) { free(buffer); }
void NetplayFrameEvents(uint32_t F);  // below gNetplayGf
void XrayLockNote(const uint32_t masks[2], bool replay);  // below StallHold
bool __cdecl GgpoAdvanceFrame(int) {
    if (gLockstep) {  // the rewind loaded nothing (GgpoLoadState): GGPO's frame count catches up, nothing runs
        ggpo_advance_frame(gGgpo);
        return true;
    }
    if (gGgpoLoadFailed) {
        // No state to replay from (the session is stopped at the boundary). GGPO's frame count must still catch up, as in
        // lockstep: its rollback loop asserts it (sync.cpp:166, "GGPO Assertion Failed" + exit(0)) right after this
        // returns. The save it asks for is the live frame's own (GameFrame was not advanced).
        ggpo_advance_frame(gGgpo);
        return true;
    }
    uint32_t in[2] = {0, 0};
    int disconnected = 0;
    ggpo_synchronize_input(gGgpo, in, sizeof in, &disconnected);
    if (gGgpoIsP2P) XrayLockNote(in, true);  // the peer's press can first show up in a replay
    const uint32_t g = GameFrame();
    const int64_t tr = FramePace::Now();
    ResimFrame(gGgpoEngine, gGgpoDt, g, (int)(gGgpoLiveF - gGgpoLoadedS), in);
    if (gGgpoIsP2P) TickSpin();
    FramePace::Add(FramePace::kReplayTick, FramePace::Now() - tr);
    const bool restarted = GameFrame() != g;
    ++HostFrame();
    ++GameFrame();
    ++gGgpoReplayed;
    if (restarted) {
        // The replayed frame started the next map (MK9 zeroes GameFrame there): the fight's timeline is over, as when a
        // live tick does it and the next tick stops the session. The frames GGPO still wants replayed belong to a
        // timeline that no longer exists: they ran as ticks of the new map under the replay's rules, their saves
        // could not be loaded, and a second rollback in the same poll ended the session as "a load found no
        // snapshot", an abnormal stop at an ordinary fight end (2 field logs; the restart was then seen at gf >= 2
        // in 83 of 2415 session ends). Nothing more is replayed; the session stops at the boundary.
        Logger::Info("GGPO replay of gf=%u started the next map (gf is now %u): the session stops as at a timeline restart, "
                     "%d frame(s) left unreplayed", g, GameFrame(), (int)(gGgpoLiveF - g) - 1);
        gGgpoLoadFailed = true;
        gGgpoStopWhy = "timeline restart";
        ggpo_advance_frame(gGgpo);
        return true;
    }
    NetplayFrameEvents(GameFrame());  // before the next frame's save, as in the live tick
    ggpo_advance_frame(gGgpo);  // saves the next frame through GgpoSaveState
    return true;
}
// ---- peer-to-peer (Milestone 13): every GGPO call happens at the frame boundary (WorldTick entry),
// where snapshots are taken, because P2P rolls back inside ggpo_idle. Local input is the mask the
// player/harness produced in the previous frame (one frame of built-in delay). ----
struct P2PConfig {
    unsigned short localPort = 0, remotePort = 0;
    char ip[32] = {};
    int player = 1, delay = 0;
    uint64_t steamPeer = 0;  // nonzero: GGPO runs over the Steam P2P bridge (SteamBridge)
};
P2PConfig gP2P;
bool gP2PPending = false, gGgpoRunning = false, gSyncValid = false;
// 'ggpo netplay ...': at the next fight start, pause, seed RNG A identically on both peers, start the P2P
// session and unpause once GGPO reports both running, so both games leave the same frame from the same state.
bool gNetplayArmed = false, gNetplayWaitRunning = false, gNetplayAwaitFight = false;
bool gNetplaySeedPin = false;  // RNG seeds re-applied every tick from the netplay start to the first unpaused tick
ULONGLONG gNetplayWaitSince = 0;  // when this game paused at its fight start to wait for the peer
constexpr ULONGLONG kNetplayJoinTimeoutMs = 60000;
bool gNetplayFromLoad = false, gNetplayStartNow = false;  // 'netplayload 1'
bool gNetplayDemoFights = false;  // 'netplaydemo 1': the two-instance tests play the attract demos as netplay fights
// 'netplayrearm 1': the online mode's re-arm path (Online::OnSessionStop) for local netplay, so a two-instance run that
// arms once tests it: re-armed inside GgpoStop("timeline restart"), before the restart handler checks gNetplayArmed.
bool gNetplayAutoRearm = false;
// No fight update yet since the last timeline restart. MK9's StartSendingGamepadData zeroes GameFrame (a timeline
// restart) before the online mode can engage (the hello needs a round trip), so an armed session would otherwise wait
// for the restart after the next one: the first online fight of a match ran on MK9's netcode. NetplayAwaitThisTimeline.
bool gFoughtThisTimeline = false;
constexpr uint32_t kNetplaySeed = 0x2F6B9A41;
uint32_t gNetplayGf = 100000;  // GameFrame at the netplay start ('netplaygf N', 0 = keep each game's own); a multiple of 4
// 'netplaymeter F' (test harness, 2026-11-07): both players' super meters set to F (1.0 = full) at frame gNetplayGf + 8
// of every local P2P session, before that frame's save: in the live tick and in every replay that passes the frame, so
// both games and all replays see the same write. For online X-ray tests: 'poke' is lab-only (refused there), reached the
// two games on different frames, and the attract script sets the meter (0.3333) during the first frame. Not in MK9
// online fights (Online::Engaged): only one side would write it.
// 'netplaylife F' the same way for both fighters' life (character object +0x78C0; 1.0 = full): a low value lets the
// harness's 33 s attract fights reach KOs and round ends under rollback (combo2: life 0.42-0.65 at the end, no KO).
float gNetplayMeter = -1.0f, gNetplayLife = -1.0f;  // < 0: off
constexpr uint32_t kNetplayMeterRel = 8;
void NetplayFrameEvents(uint32_t F) {
    if (gGgpo && gGgpoIsP2P) Online::NoteFrameStart(F);  // every frame of a session, as it starts (live or replayed)
    if ((gNetplayMeter < 0 && gNetplayLife < 0) || !gGgpo || !gGgpoIsP2P || !gNetplayGf || F != gNetplayGf + kNetplayMeterRel ||
        Online::Engaged())
        return;
    for (int p = 0; p < 2; ++p) {
        if (gNetplayMeter >= 0) *reinterpret_cast<float*>(0x00E89D7C + 0xC8C * p + 0xC14) = gNetplayMeter;  // PlayerInfo +0xC14
        if (gNetplayLife >= 0) {
            const Fighter::State f = Fighter::Read(p);
            if (f.obj) *reinterpret_cast<float*>(f.obj + 0x78C0) = gNetplayLife;
        }
    }
    Logger::Info("NETPLAYMETER: meters %.4f, life %.4f set at gf=%u%s", gNetplayMeter, gNetplayLife, F,
                 Rollback::gIsResimulating ? " (replay)" : "");
}
uint32_t gLocalRaw = 0, gSyncMasks[2] = {0, 0};
bool gLocalScripted = false;  // gLocalRaw holds a scripted (harness) mask taken at the last RecordInputs
int gTimesyncFrames = 0, gBarrierWaits = 0, gP2PFrames = 0;
int gTimesyncDebt = 0, gTimesyncEvents = 0;  // frames still to give the remote; GGPO time-sync events this session
// 'paysmooth N': the debt paid by moving the frame limiter's deadline N ms per frame (0: 16 ms waits). Default 2 since
// net2 (2026-11-04): frames >= 34 ms -36%, presents >= 34 ms -32%, rollback depth 2.32 -> 1.96 over the network matrix.
int gPaySmoothMs = 2;
double gPaidMs = 0, gShiftedMs = 0;  // part of the frame being paid; deadline moved this session (ms)
int gGgpoLocal = 0, gGgpoDelay = 0;         // the running P2P session's local player index and its input delay

// Cross-machine desync detection (GGPO P2P has none): each peer sends the gameplay checksum of a frame that can no
// longer be rolled back (24 frames behind; GGPO's prediction window is 8) on a side UDP socket (GGPO port + 100),
// and compares the peer's values with its own. Frames are counted from the session start (gf differs per machine).
SOCKET gCsSock = INVALID_SOCKET;
sockaddr_in gCsPeer{};
uint32_t gCsStartGf = 0;
struct CsRec {
    uint32_t rel = 0xFFFFFFFF, cs = 0;
    GameplayState g{};  // for the first desync's report
};
CsRec gCs[128];
int gCsMatched = 0, gCsMismatched = 0, gCsLogged = 0;
constexpr uint32_t kCsMagic = 0x43394B4D;  // 'MK9C'
constexpr uint32_t kCsLag = 24;
constexpr uint32_t kEnvMagic = 0x45394B4D;  // 'MK9E': environment fingerprint
bool gEnvChecked = false, gEnvMismatch = false;
bool gStartMismatch = false;  // frame 0 differs: the fights did not start from the same state; the session stops
// Out of sync for good (round 3): the checksums were only counted and logged, so two players went on playing two
// different fights until the round ended. In a real online match (Online::Engaged; test sessions keep running for the
// diagnosis) the session now stops once gDesyncStopFrames compared frames in a row differed ('desyncstop N', 0 = never):
// the status line says why and MK9's own netcode takes over. A simulation that differs for 2 s does not come back.
int gDesyncStopFrames = 120, gCsMismatchRun = 0;
bool gDesyncStop = false;
uint32_t CsStartGf() { return gCsStartGf; }
void CsRecord(uint32_t gf, uint32_t cs, const GameplayState& g) {
    if (gCsSock == INVALID_SOCKET || gf < gCsStartGf) return;
    const uint32_t rel = gf - gCsStartGf;
    gCs[rel % 128] = {rel, cs, g};  // a replay of the frame overwrites the predicted value
}
void CsOpen(const char* ip, unsigned short localPort, unsigned short remotePort) {
    gCsSock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (gCsSock == INVALID_SOCKET) return;
    sockaddr_in local{};
    local.sin_family = AF_INET;
    local.sin_port = htons((unsigned short)(localPort + 100));
    // A session over the Steam bridge (and two games on one PC) talks to 127.0.0.1 only: bound there the socket cannot be
    // reached from the network, and connected to the peer's port it takes datagrams from nowhere else (round 3: it bound
    // every interface and acted on whatever arrived: checksums, the environment print, phase reports). A direct-IP session
    // keeps the open bind: its peer's packets may come from a translated port.
    const bool loopback = inet_addr(ip) == htonl(INADDR_LOOPBACK);
    local.sin_addr.s_addr = htonl(loopback ? INADDR_LOOPBACK : INADDR_ANY);
    u_long nb = 1;
    if (bind(gCsSock, reinterpret_cast<sockaddr*>(&local), sizeof local) || ioctlsocket(gCsSock, FIONBIO, &nb)) {
        closesocket(gCsSock);
        gCsSock = INVALID_SOCKET;
        Logger::Info("NETPLAY checksum channel: could not bind port %u", localPort + 100);
        return;
    }
    gCsPeer = {};
    gCsPeer.sin_family = AF_INET;
    gCsPeer.sin_port = htons((unsigned short)(remotePort + 100));
    gCsPeer.sin_addr.s_addr = inet_addr(ip);
    if (loopback && connect(gCsSock, reinterpret_cast<const sockaddr*>(&gCsPeer), sizeof gCsPeer))
        Logger::Info("NETPLAY checksum channel: connect failed (%d); datagrams from other local ports are not filtered", WSAGetLastError());
    // No "connection reset" on a read after a datagram found the peer's port not open yet (SIO_UDP_CONNRESET off, as on
    // the Steam bridge's sockets): the start alignment reads this socket from frame 0 on, in a loop that a failed read ends.
    BOOL resetReports = FALSE;
    DWORD ioctlBytes = 0;
    WSAIoctl(gCsSock, 0x9800000C /* SIO_UDP_CONNRESET */, &resetReports, sizeof resetReports, nullptr, 0, &ioctlBytes, nullptr, nullptr);
    gCsMismatchRun = 0;
    gDesyncStop = false;
    gCsStartGf = GameFrame();
    for (CsRec& r : gCs) r = {};
    gCsMatched = gCsMismatched = gCsLogged = 0;
    gEnvChecked = gEnvMismatch = false;
    Logger::Info("NETPLAY checksum channel: port %u -> %s:%u, frame 0 = gf %u", localPort + 100, ip, remotePort + 100, gCsStartGf);
}
void CsClose() {
    if (gCsSock == INVALID_SOCKET) return;
    closesocket(gCsSock);
    gCsSock = INVALID_SOCKET;
    Logger::Info("NETPLAY checksums: %d frames matched the peer, %d did not", gCsMatched, gCsMismatched);
}

// Sub-frame phase alignment (gPhaseSync, with the settings): once both games' input-in-time shares of the same window are
// known, the game whose share is clearly the smaller one (its frame grid leads) moves its deadline later.
// Step search (b44, rc25 first build): a share is close to all-or-nothing in the phase (at ns15 d1 both games get the
// inputs in time only within ~2 ms of balance), so a step sized from the share difference (1-6 ms) jumped past the balance
// every time: the shares flipped between ~50 and ~970 per mille window after window and each game corrected 7 times a
// fight. Now the step halves whenever the leading game changes from one decision to the next (it overshot) and grows by
// half while the same game keeps leading (not there yet): a bisection on the balance point. Both games see the same pair
// of shares, so their steps shrink and grow together.
// Round 3 (r3smoke): the step grew at the very next decision after a halving, so each change of leader shrank the swing
// by a quarter only (3.0 4.5 | 2.2 3.4 | 1.7 2.5 | 1.3 ... over 30 s, 13-27 ms moved a fight, rollbacks on both games
// while it swung). It now grows only from the third decision in a row with the same leader (gPhaseGrowAfter): after an
// overshoot the same step is tried once more, then the search is taken to be behind (a drift, a changed route).
constexpr double kPhaseStepStartMs = 3.0, kPhaseStepMinMs = 0.5, kPhaseStepMaxMs = 6.0;
int gPhaseGrowAfter = 2;  // decisions with the same leader after which the step grows ('phasegrow N'; 1 = rc27)
double gPhaseStep = kPhaseStepStartMs;
int gPhaseLastLeader = 0;  // 1 this game, -1 the other, 0 none yet
int gPhaseLeaderRun = 0;   // decisions in a row that repeated gPhaseLastLeader
void PhaseDecide() {
    if (gPhaseMyWindow < 0 || gPhaseMyWindow != gPhasePeerWindow || gPhaseDecided == gPhaseMyWindow) return;
    gPhaseDecided = gPhaseMyWindow;
    const int lead = gPhasePeerQ - gPhaseMyQ;  // > 0: this game's inputs reach the other in time more often than the reverse
    const int leader = lead > kPhaseDeadband ? 1 : lead < -kPhaseDeadband ? -1 : 0;
    if (leader && gPhaseLastLeader && leader != gPhaseLastLeader) {
        gPhaseStep = gPhaseStep / 2 < kPhaseStepMinMs ? kPhaseStepMinMs : gPhaseStep / 2;  // overshot: halve
        gPhaseLeaderRun = 0;
    } else if (leader && leader == gPhaseLastLeader && ++gPhaseLeaderRun >= gPhaseGrowAfter) {
        gPhaseStep = gPhaseStep * 1.5 > kPhaseStepMaxMs ? kPhaseStepMaxMs : gPhaseStep * 1.5;  // short of it: grow
    }
    if (leader) gPhaseLastLeader = leader;
    double ms = 0;
    if (leader == 1 && gPhaseShiftedMs + gPhaseDueMs < kPhaseMaxMs) {
        ms = gPhaseStep;
        gPhaseDueMs += ms;
        ++gPhaseCorrections;
    }
    if (gPhaseLogs < 40 && (ms > 0 || gPhaseLogs < 12)) {
        ++gPhaseLogs;
        Logger::Info("PHASE window %d (gf=%u): the other player's input was in at this game's first poll %d/1000, this game's at "
                     "theirs %d/1000%s", gPhaseMyWindow, GameFrame(), gPhaseMyQ, gPhasePeerQ,
                     ms > 0 ? " -> this game's frames start later" : "");
        if (ms > 0) Logger::Info("PHASE   moving the frame deadline %.1f ms (%.1f ms this session)", ms, gPhaseShiftedMs + gPhaseDueMs);
    }
}
void PhaseOnPeer(int window, int q, int samples) {
    if (!gPhaseSync || samples < kPhaseMinSamples || q < 0 || q > 1000 || window < gPhasePeerWindow) return;
    // A report for a window this session cannot have reached is an earlier session's (online sessions share Steam's
    // connection, which is not closed between fights): taken, it would hide every real report below its number.
    if (window > (gP2PFrames + 16) / kPhaseWindow) return;
    gPhasePeerWindow = window;
    gPhasePeerQ = q;
    PhaseDecide();
}
// At the boundary's first poll of a live frame: was the other player's input for it already in? Counted only on quiet
// frames: no debt being paid, no phase shift under way, no X-ray lock, no wait for the peer in the previous boundary and
// not within 60 frames of a stall hold. At each window's end this game's share goes to the other game.
void PhaseSample(bool onTime) {
    if (!gPhaseSync || gLockstep || !gGgpoRunning || gP2PFrames <= 0) return;
    static int lastFrame = -1;
    if (gP2PFrames == lastFrame) return;  // a boundary that advanced no frame (paused)
    lastFrame = gP2PFrames;
    const int window = gP2PFrames / kPhaseWindow;
    if (window != gPhaseWindowAt) {
        if (gPhaseWindowAt >= 0 && gPhaseSamples >= kPhaseMinSamples) {
            gPhaseMyWindow = gPhaseWindowAt;
            gPhaseMyQ = gPhaseOnTime * 1000 / gPhaseSamples;
            if (gCsSock != INVALID_SOCKET) {
                const uint32_t pkt[4] = {kPhaseMagic, (uint32_t)gPhaseMyWindow, (uint32_t)gPhaseMyQ, (uint32_t)gPhaseSamples};
                sendto(gCsSock, reinterpret_cast<const char*>(pkt), sizeof pkt, 0, reinterpret_cast<const sockaddr*>(&gCsPeer), sizeof gCsPeer);
            }
            PhaseDecide();
        }
        gPhaseWindowAt = window;
        gPhaseOnTime = gPhaseSamples = 0;
    }
    const bool quiet = gTimesyncDebt == 0 && gPhaseDueMs <= 0 && !gXrayLockOn && gBarrierWaits == gPhaseLastWaits &&
                       gP2PFrames >= gPhaseQuietUntil;
    gPhaseLastWaits = gBarrierWaits;
    if (!quiet) return;
    ++gPhaseSamples;
    if (onTime) ++gPhaseOnTime;
}
void PhaseReset() {
    gPhaseOnTime = gPhaseSamples = 0;
    gPhaseWindowAt = gPhaseMyWindow = gPhaseMyQ = gPhasePeerWindow = gPhasePeerQ = gPhaseDecided = -1;
    gPhaseLastWaits = 0;
    gPhaseQuietUntil = 60;  // the start's waits and catch-up are not the steady phase
    gPhaseDueMs = gPhaseShiftedMs = 0;
    gPhaseCorrections = gPhaseLogs = 0;
    gPhaseStep = kPhaseStepStartMs;
    gPhaseLastLeader = gPhaseLeaderRun = 0;
}

void CsPump(uint32_t liveGf) {
    if (gCsSock == INVALID_SOCKET || liveGf < gCsStartGf + kCsLag) return;
    const uint32_t rel = liveGf - gCsStartGf - kCsLag;
    if (rel % 600 == 599) {
        Logger::Info("NETPLAY checksums so far: %d frames matched the peer, %d did not", gCsMatched, gCsMismatched);
        // Connection quality for the reports of real online sessions: GGPO's view of the remote player.
        GGPONetworkStats ns{};
        // The other index of gGgpoPlayer. The running session's own side (gGgpoLocal, 0 or 1), here and below: gP2P is the
        // configuration for the next start, and an arm command arriving during a session rewrites it.
        const int remote = 1 - gGgpoLocal;
        if (gGgpo && GGPO_SUCCEEDED(ggpo_get_network_stats(gGgpo, gGgpoPlayer[remote], &ns)))
            Logger::Info("NETSTATS ping %d ms, send queue %d, %d kbps sent, frames behind: local %d remote %d; %d loads, %d replayed so far",
                         ns.network.ping, ns.network.send_queue_len, ns.network.kbps_sent, ns.timesync.local_frames_behind,
                         ns.timesync.remote_frames_behind, gGgpoLoads, gGgpoReplayed);
        Overlay::LogCounters();
        FramePace::Report(liveGf);
    }
    const CsRec& mine = gCs[rel % 128];
    if (mine.rel == rel) {
        const uint32_t pkt[3] = {kCsMagic, rel, mine.cs};
        sendto(gCsSock, reinterpret_cast<const char*>(pkt), sizeof pkt, 0, reinterpret_cast<const sockaddr*>(&gCsPeer), sizeof gCsPeer);
    }
    // Environment fingerprint (content, DLL build, switches): every 30 frames for the first 600, so a lost datagram is
    // repeated. Peers that differ stop the session instead of desyncing later.
    if (rel < 600 && rel % 30 == 0) {
        const EnvFingerprint::Print& p = EnvFingerprint::Get();
        uint32_t pkt[1 + EnvFingerprint::kFields] = {kEnvMagic};
        memcpy(pkt + 1, p.v, sizeof p.v);
        sendto(gCsSock, reinterpret_cast<const char*>(pkt), sizeof pkt, 0, reinterpret_cast<const sockaddr*>(&gCsPeer), sizeof gCsPeer);
    }
    if (rel == 600 && !gEnvChecked) Logger::Info("NETPLAY environment: no fingerprint from the peer in 600 frames (older DLL?)");
    for (int n = 0; n < 16; ++n) {
        uint32_t in[16];
        sockaddr_in from{};
        int fromLen = sizeof from;
        const int got = recvfrom(gCsSock, reinterpret_cast<char*>(in), sizeof in, 0, reinterpret_cast<sockaddr*>(&from), &fromLen);
        if (got < 0) break;  // nothing waiting
        // A direct-IP session's socket takes datagrams from the whole network (CsOpen): only the peer's address counts
        // (its port may be a translated one). Anyone else's "environment differs" or "frame 0 differs" stopped the session.
        if (from.sin_addr.s_addr != gCsPeer.sin_addr.s_addr) continue;
        if (got == (int)(4 * (1 + EnvFingerprint::kFields)) && in[0] == kEnvMagic) {
            const EnvFingerprint::Print& p = EnvFingerprint::Get();
            if (!gEnvChecked) {
                gEnvChecked = true;
                bool same = true;
                for (int f = 0; f < EnvFingerprint::kFields; ++f)
                    if (p.v[f] != in[1 + f]) {
                        same = false;
                        Logger::Info("NETPLAY ENVIRONMENT MISMATCH: %s differs (local %08X, peer %08X)", EnvFingerprint::FieldName(f), p.v[f], in[1 + f]);
                    }
                if (same) Logger::Info("NETPLAY environment matches the peer (content %08X, dll %08X, switches %08X)", p.v[0], p.v[1], p.v[2]);
                else gEnvMismatch = true;  // stopped by the tick (fail closed: the session would desync)
            }
            continue;
        }
        if (got == 16 && in[0] == kPhaseMagic) {  // the other game's input-in-time share for a window (phase alignment)
            PhaseOnPeer((int)in[1], (int)in[2], (int)in[3]);
            continue;
        }
        // A start-alignment datagram that came after this game stopped reading them (frame StartSync::kFrames): nothing
        // to do with it, and it must not end this read as an unknown datagram does.
        if (got == (int)sizeof(StartSync::Packet) && in[0] == StartSync::kMagic) continue;
        if (got != 12 || in[0] != kCsMagic) break;
        const CsRec& own = gCs[in[1] % 128];
        if (own.rel != in[1]) continue;  // not (or no longer) recorded here
        if (own.cs == in[2]) {
            ++gCsMatched;
            gCsMismatchRun = 0;
        } else {
            ++gCsMismatched;
            if (++gCsMismatchRun == gDesyncStopFrames && gDesyncStopFrames > 0 && Online::Engaged()) {
                Logger::Info("NETPLAY: the two games went out of sync (%d compared frames in a row differ, the last one frame %u): "
                             "the rollback session is stopped", gCsMismatchRun, in[1]);
                gDesyncStop = true;
            }
            if (gCsLogged++ < 10)
                Logger::Info("NETPLAY DESYNC: frame %u (gf %u) local checksum %08X, peer %08X", in[1], in[1] + gCsStartGf, own.cs, in[2]);
            if (gCsLogged == 1) {  // the first one: what this peer had (the peer logs its side); compare the two logs
                const GameplayState& x = own.g;
                Logger::Info("NETPLAY DESYNC   local frame %u: life %.4f %.4f pos %.3f %.3f %.3f / %.3f %.3f %.3f meter %.4f %.4f rng %08X",
                             in[1], x.life[0], x.life[1], x.pos[0][0], x.pos[0][1], x.pos[0][2], x.pos[1][0], x.pos[1][1],
                             x.pos[1][2], x.meter[0], x.meter[1], x.rng);
                for (uint32_t k = 3; k >= 1; --k)  // the state at frame r comes from frame r-1's execution
                    if (in[1] >= k) RngTrace::LogFrameDraws(in[1] + gCsStartGf - k, "NETPLAY DESYNC   draws");
                RngTrace::LogOtherThreads("NETPLAY DESYNC   other threads");
                Rollback::LogEventListeners("first desync (now)");
            }
            if (in[1] == 0) {  // no rollback can fix a different start; continuing would only desync further
                Logger::Info("NETPLAY: frame 0 differs from the peer: pick the same characters (P1 left, P2 right), stage "
                             "and settings on both PCs. The session is stopped.");
                gStartMismatch = true;
            }
        }
    }
}

bool __cdecl GgpoOnEvent(GGPOEvent* e) {
    switch (e->code) {
        case GGPO_EVENTCODE_RUNNING:
            gGgpoRunning = true;
            FramePace::SessionStart();
            Logger::Info("GGPO running");
            if (gNetplayWaitRunning) {
                CommandChannel::RequestPause(0);
                gNetplayWaitRunning = false;
                Logger::Info("NETPLAY: both peers running at gf=%u, unpaused", GameFrame());
            }
            break;
        case GGPO_EVENTCODE_CONNECTED_TO_PEER: Logger::Info("GGPO connected to peer"); break;
        case GGPO_EVENTCODE_SYNCHRONIZED_WITH_PEER: Logger::Info("GGPO synchronized with peer"); break;
        case GGPO_EVENTCODE_CONNECTION_INTERRUPTED:
            Logger::Info("GGPO: the peer stopped responding");
            gOvPeerInterrupted = true;
            break;
        case GGPO_EVENTCODE_CONNECTION_RESUMED:
            Logger::Info("GGPO: the peer responds again");
            gOvPeerInterrupted = false;
            break;
        case GGPO_EVENTCODE_DISCONNECTED_FROM_PEER:
            // GGPO would go on with a neutral input for the lost player: end the session instead (at the boundary,
            // outside this callback).
            Logger::Info("GGPO disconnected from peer");
            gPeerLost = true;
            break;
        case GGPO_EVENTCODE_TIMESYNC:
            ++gTimesyncEvents;
            if (gBalance && !gGgpoTimesync && !gLockstep) {  // BALANCE decides (gGgpoTimesync)
                if (gTimesyncIgnored++ < 4)
                    Logger::Info("GGPO TIMESYNC gf=%u: %d frame(s) ahead by GGPO's 40-frame average; not paid (BALANCE decides)",
                                 GameFrame(), e->u.timesync.frames_ahead);
            } else {
                gTimesyncFrames = e->u.timesync.frames_ahead;
            }
            break;
        default: Logger::Frame("GGPO event %d", (int)e->code); break;
    }
    return true;
}

// In-game status line (Overlay, docs\status_overlay_plan.md): the depth of each P2P load, per live frame (60-frame
// ring), the last stop reason other than a normal end, and GGPO's network view refreshed every 30 live frames.
uint32_t gOvDepthGf[60], gOvDepth[60];
uint16_t gOvLastDepth = 0;
char gOvStopWhy[64] = "";
DWORD gOvStopAt = 0;
int gOvPing = -1, gOvBehindL = 0, gOvBehindR = 0, gOvKbps = 0, gOvQueue = 0;

void GgpoStop(const char* why) {
    if (!gGgpo) return;
    // Not the fight's normal end ("match over": the online mode at MK9's results screen).
    const bool abnormal = gGgpoIsP2P && strcmp(why, "timeline restart") && strcmp(why, "restart") && strcmp(why, "match over");
    if (abnormal) {
        strncpy(gOvStopWhy, why, sizeof gOvStopWhy - 1);
        gOvStopWhy[sizeof gOvStopWhy - 1] = 0;
        gOvStopAt = GetTickCount();
    }
    // A session that stops mid-fight says so (round 3): the other game stops with it within a frame or two instead of
    // standing at its prediction limit until the 3 s disconnect timeout. Not at a normal end, where both games stop by
    // themselves: the one a few frames behind would take the flag for a lost opponent and disarm.
    ggpo_bye_on_close = abnormal ? 1 : 0;
    ggpo_close_session(gGgpo);
    gGgpo = nullptr;
    gGgpoFrameOpen = false;
    OnlineKeep::OnSessionEnd();
    if (gGgpoIsP2P) {
        Input::SetLocalCapture(false);
        Input::ReleaseAll();  // the fighters' pads go back to the local controllers (they held the last network input)
        CsClose();
        ggpo_after_send = nullptr;  // the closed session's last sends are through (ggpo_close_session flushes)
        SteamBridge::Stop();
        gGgpoIsP2P = gGgpoRunning = gSyncValid = false;
        gOvPeerInterrupted = false;
        // A mode chosen during the session, before the online mode re-arms the next fight with it (it compares the peer's).
        if (gLockstepDeferred) {
            Rollback::SetLockstep(gLockstepNext);
            if (gEnabled && !gLockstep && !gRollbackModules && !Rollback::InstallRollbackModules()) {
                // Rollback would be advertised (hello) but every start refused: stay in lockstep instead.
                Logger::Info("ROLLBACK: rollback sessions unavailable; staying in lockstep");
                Rollback::SetLockstep(true);
            }
        }
        Online::OnSessionStop(why);
        if (gNetplayAutoRearm && !Online::Engaged() && !strcmp(why, "timeline restart")) {
            gNetplayArmed = true;  // gP2P keeps the configuration of the session that just ended
            Logger::Info("NETPLAY re-armed automatically (netplayrearm): player %d, delay %d, starts at the next fight start", gP2P.player,
                         gP2P.delay);
        }
        FramePace::SessionReport("session");
        RngTrace::LogOtherThreads("RNGTRACE session end");
        gLivePending = 0xFFFFFFFF;
        Logger::Info("GGPO p2p stopped (%s): %d frames, %d loads, %d replayed frames, %d barrier waits, %d failed saves, %d time-sync "
                     "event(s) (%d frame(s) still to give), %d balance correction(s) (%d frame(s)), frame deadline moved %.0f ms "
                     "(paysmooth %d); sparse saves: %d replay saves skipped, %d live saves skipped, %d loads went back a frame, "
                     "%d found none; stall holds %d (%.0f ms); X-ray locks %d (%d frames, %d of them for a cinematic, %d "
                     "waited for the peer, %.0f ms), X-ray cinematics %d (%d frames of held input); "
                     "guards (cumulative): "
                     "%u collected actor/component read(s), %u repeated script relocation(s), %u emptied HUD slot(s), "
                     "%u fighter IK list entries removed, %u IK list entries skipped in the fight tick, %u render data deletes "
                     "dropped, %u late frees kept live",
                     why, gP2PFrames, gGgpoLoads, gGgpoReplayed, gBarrierWaits, gGgpoSaveFails, gTimesyncEvents, gTimesyncDebt,
                     gBalanceEvents, gBalanceFrames, gShiftedMs, gPaySmoothMs, gSparseSkips, gSparseLiveSkips, ggpo_fallback_loads,
                     ggpo_fallback_misses, gStallHolds, gStallHeldMs, gXrayLocks, gXrayLockFrames, gXrayLockCineFrames,
                     gXrayLockWaits, gXrayLockWaitMs, gXrayCines, gXrayFrozenFrames,
                     ActorGuard::Hits(), ScriptLinkGuard::Skips(), HandleGuard::NullHits(), Snapshot::gCompListScrubbed,
                     HandleGuard::IkNodeSkips(), HandleGuard::RenderDataKept(), FreeQuarantine::LateProtected());
        Logger::Info("EARLYINPUT this session: %u of %d frames' local inputs added and sent at the boundary's start; %u live "
                     "saves not taken (the other player's input for the frame was already in)",
                     (unsigned)gEarlyAdds, gP2PFrames, (unsigned)gConfirmedLiveSkips);
        Logger::Info("TIMING this session: phase alignment %d correction(s), frame deadline moved %.1f ms (last shares: this game "
                     "%d/1000, the other %d/1000); %u re-poll(s) after a live save, %u brought the frame's input; GGPO time-sync "
                     "events left to BALANCE %d; lowest round trip %d ms; %d wait(s) for the peer moved the deadline %d frame(s) "
                     "(waitshift %d)",
                     gPhaseCorrections, gPhaseShiftedMs, gPhaseMyQ, gPhasePeerQ, (unsigned)gRepolls, (unsigned)gRepollHits,
                     gTimesyncIgnored, ggpo_rtt_min_ms, gWaitShifts, gWaitShiftFrames, (int)gWaitShift);
        if (gSs.Begun())
            Logger::Info("STARTSYNC this session: the deadline moved %.1f ms at the start; one-way trip %.1f ms (-1: not measured); "
                         "%u datagram(s) read, %u sent, decided %d (startsync %d)", gSsMovedUs / 1000.0,
                         gSsTripUs == StartSync::kNone ? -1.0 : gSsTripUs / 1000.0, gSs.Reads(), gSs.Sends(), (int)gSs.Decided(),
                         (int)gStartSync);
        SoundDedupe::LogSession();
        RenderTag::LogSession();
        {
            unsigned maxMs = 0, slow = 0;
            Logger::IoStats(&maxMs, &slow);
            Logger::Info("LOGIO since the last session: the log files' longest write took %u ms, %u took 5 ms or more (on the "
                         "logger's own thread: the game thread no longer waits for them)", maxMs, slow);
        }
        gEarlyAdds = 0;
        gConfirmedLiveSkips = 0;
        Input::SetDeferSampling(false);
        Input::LogSamplingCounts();
        gTimesyncDebt = 0;
        return;
    }
    if (gAbToggle)
        Logger::Info("ABTEST %s (cumulative): ON %d loads %d errors (%.2f/load), OFF %d loads %d errors (%.2f/load)", gAbName, gAbLoads[0],
                     gAbErrors[0], gAbLoads[0] ? (double)gAbErrors[0] / gAbLoads[0] : 0.0, gAbLoads[1], gAbErrors[1],
                     gAbLoads[1] ? (double)gAbErrors[1] / gAbLoads[1] : 0.0);
    Logger::Info("GGPO synctest stopped (%s): %d loads, %d replayed frames, %d sync errors, %d failed saves", why, gGgpoLoads,
                 gGgpoReplayed, gGgpoSyncErrors, gGgpoSaveFails);
}

bool GgpoStartP2P(const P2PConfig& c) {
    GgpoStop("restart");
    if (!gLockstep && !gRollbackModules) {  // a lockstep-only patch set: replays would run without churn/free tracking
        Logger::Info("GGPO p2p: a rollback session needs the rollback patch set ('netcode rollback'); not started");
        return false;
    }
    WSADATA wd;
    WSAStartup(MAKEWORD(2, 2), &wd);
    if (c.steamPeer) {  // GGPO's remote is the local bridge (c.ip 127.0.0.1, c.remotePort); checksums on a second link
        // Channel 77 is not drained at the start: GGPO checks its own session numbers, and the other game's first sync
        // request may already be waiting there (dropping it costs a retry). 78 carries checksums and phase reports with
        // no session number: packets a session that stopped early left there are dropped.
        const SteamBridge::LinkSpec links[2] = {{c.remotePort, c.localPort, 77, true},
                                                {(unsigned short)(c.remotePort + 100), (unsigned short)(c.localPort + 100), 78}};
        if (!SteamBridge::Start(c.steamPeer, links, 2)) return false;
    }
    GGPOSessionCallbacks cb{};
    cb.begin_game = GgpoBeginGame;
    cb.save_game_state = GgpoSaveState;
    cb.load_game_state = GgpoLoadState;
    cb.log_game_state = GgpoLogState;
    cb.free_buffer = GgpoFreeBuffer;
    cb.advance_frame = GgpoAdvanceFrame;
    cb.on_event = GgpoOnEvent;
    char name[] = "MK9Rollback";
    ggpo_prediction_frames_override = 0;
    ggpo_last_confirmed_frame = -1;
    ggpo_frame_loadable = gLockstep ? nullptr : &GgpoFrameLoadable;  // lockstep keeps no states (it never mispredicts)
    ggpo_fallback_loads = ggpo_fallback_misses = 0;
    gSparseSkips = gSparseLiveSkips = 0;
    gLivePendingGgpoGf = 0xFFFFFFFF;
    // rc25 GGPO switches (globals that persist: set for every session). Over the Steam bridge GGPO talks to 127.0.0.1 only,
    // so its socket is not exposed on the network (no firewall prompt); a direct-IP session binds every interface as before.
    // Each datagram GGPO sends is forwarded to Steam at once instead of at the boundary's next Pump (quality replies and
    // resends made inside a poll waited behind its rollback and the live save: 3-60 ms added to the measured round trip).
    ggpo_bind_loopback = c.steamPeer ? 1 : 0;
    ggpo_no_disconnect_rewind = 1;
    ggpo_after_send = c.steamPeer ? &SteamBridge::PumpOut : nullptr;
    ggpo_bind_failed = 0;
    const bool started = ggpo_start_session(&gGgpo, &cb, name, 2, sizeof(uint32_t), c.localPort) == GGPO_OK;
    if (!started || ggpo_bind_failed) {
        // GGPO itself goes on without a socket when the port could not be bound (another program holds it): every
        // packet would be lost and the fight would wait 15 s for a peer that cannot be heard.
        if (started) {
            Logger::Info("GGPO p2p: UDP port %u could not be opened (Windows error %d: in use by another program?); the "
                         "session is not started", c.localPort, ggpo_bind_failed);
            ggpo_bye_on_close = 0;
            ggpo_close_session(gGgpo);
        } else {
            Logger::Info("GGPO p2p: session could not start on port %u", c.localPort);
        }
        gGgpo = nullptr;
        ggpo_after_send = nullptr;
        if (c.steamPeer) SteamBridge::Stop();
        return false;
    }
    ggpo_set_disconnect_timeout(gGgpo, 3000);
    ggpo_set_disconnect_notify_start(gGgpo, 1000);
    for (int p = 0; p < 2; ++p) {
        GGPOPlayer pl{};
        pl.size = sizeof pl;
        pl.player_num = p + 1;
        if (p + 1 == c.player) {
            pl.type = GGPO_PLAYERTYPE_LOCAL;
        } else {
            pl.type = GGPO_PLAYERTYPE_REMOTE;
            strncpy(pl.u.remote.ip_address, c.ip, sizeof pl.u.remote.ip_address - 1);
            pl.u.remote.port = c.remotePort;
        }
        ggpo_add_player(gGgpo, &pl, &gGgpoPlayer[p]);
    }
    // Lockstep needs a frame delay >= 1: the first frames' remote inputs would otherwise be predicted.
    const int delay = gLockstep && c.delay < 1 ? 2 : c.delay;
    if (delay > 0) ggpo_set_frame_delay(gGgpo, gGgpoPlayer[c.player - 1], delay);
    gGgpoLocal = c.player - 1;
    gGgpoDelay = delay > 0 ? delay : 0;
    gTimesyncDebt = gTimesyncEvents = gBalanceEvents = gBalanceFrames = 0;
    gTimesyncFrames = gTimesyncIgnored = 0;  // an event raised by the last session's final polls is not this one's debt
    gRepolls = gRepollHits = 0;
    gOvPeerInterrupted = false;
    PhaseReset();
    SoundDedupe::SessionReset();
    RenderTag::SessionReset();
    gPaidMs = gShiftedMs = 0;
    gStallHolds = 0;
    gStallHeldMs = 0;
    gWaitShifts = gWaitShiftFrames = 0;
    gSs = StartSync::Session();
    gSsWaitedMs = 0;
    gSsFrameAt = -1;
    gSsMovedUs = 0;
    gSsTripUs = StartSync::kNone;
    gXrayLockUntil = -1;
    gXrayLockOn = false;
    gXrayLocks = gXrayLockFrames = gXrayLockCineFrames = gXrayLockWaits = 0;
    gXrayLockWaitMs = 0;
    gXrayCine = false;
    gXrayLastAdded = gXrayHeld = 0;
    gXrayCines = gXrayFrozenFrames = 0;
    gBalanceRestart = false;
    gFreezeNext = 0;
    gGgpoIsP2P = true;
    // Where a >= 120 ms frame sat, and whether the whole process had stopped. 250 ms until rc28: the 150-230 ms hitches
    // near each match's end in the two-PC evening of 2026-11-11 (outside boundary, world tick and engine tick) left no sample.
    FramePace::DefaultStallSample(120);
    Overlay::RequestDisplayLog();
    // Sessions without the netplay renumber ('netplaygf 0', plain 'ggpo p2p|steam') get the shared virtual clock here; the
    // players' netplay start already switched to it (AnchorCanonical in OnTickEntry).
    if (!ScriptClock::Active()) ScriptClock::AnchorCanonical(GameFrame());
    Input::SetLocalCapture(true);
    gGgpoRunning = gSyncValid = gGgpoFrameOpen = gGgpoLoadFailed = gPeerLost = false;
    // The checksum channel's verdicts too, here and not only in CsOpen: that returns early when its port cannot be
    // bound, and it never cleared the frame-0 verdict. A verdict left over from the last session stops this one at
    // its first boundary with the old reason.
    gEnvChecked = gEnvMismatch = gStartMismatch = gDesyncStop = false;
    gCsMismatchRun = 0;
    gGgpoLoads = gGgpoReplayed = gGgpoSyncErrors = gGgpoSaveFails = gBarrierWaits = gP2PFrames = 0;
    // Per session as well: the X-ray lock's log budget (40 lines, spent for good by the first long session of a
    // launch) and the overlay's ring of recent rollbacks (sessions share the frame numbering, so the last fight's
    // entries were counted again when this one passed the same frame numbers).
    gXrayLockLogs = 0;
    memset(gOvDepthGf, 0, sizeof gOvDepthGf);
    memset(gOvDepth, 0, sizeof gOvDepth);
    gOvLastDepth = 0;
    Logger::Info("GGPO p2p started at gf=%u (%s): local port %u, player %d, remote %s:%u, frame delay %d", GameFrame(),
                 gLockstep ? "LOCKSTEP" : "rollback", c.localPort, c.player, c.ip, c.remotePort, delay);
    CsOpen(c.ip, c.localPort, c.remotePort);
    return true;
}

// The local player's input delay in the running P2P session (the online mode, between rounds). The patched GGPO sends
// the frames a larger delay pads (tools/build_ggpo.ps1); a smaller one drops one local input per frame of difference.
// The session's own local player and delay (gGgpoLocal/gGgpoDelay), not gP2P: commands can rewrite that configuration
// while a session runs.
bool SetSessionDelayImpl(int delay) {
    if (!gGgpo || !gGgpoIsP2P || !gGgpoRunning || gGgpoLocal < 0 || gGgpoLocal > 1 || delay < 1 || delay > 8) return false;
    if (!GGPO_SUCCEEDED(ggpo_set_frame_delay(gGgpo, gGgpoPlayer[gGgpoLocal], delay))) return false;
    Logger::Info("GGPO p2p frame delay %d -> %d at gf=%u", gGgpoDelay, delay, GameFrame());
    gGgpoDelay = gP2P.delay = delay;
    // Both timing controllers measured the old delay: the frame-advantage samples and the share of inputs in time
    // describe frames that were sent a different number of frames ahead.
    gBalanceRestart = true;
    if (gPhaseQuietUntil < gP2PFrames + 60) gPhaseQuietUntil = gP2PFrames + 60;
    return true;
}

// One poll interval of the boundary's waits for the remote input: 1 ms. Sleep(1) waits a whole system timer tick (15.6 ms:
// MKKE never raises the timer resolution), so a lockstep frame started up to a tick after its input had arrived
// (lockstep_ns60: ~8 ms per wait, waiting on 93% of the frames). A high-resolution waitable timer (Windows 10 1803+) has
// ~0.5 ms precision without changing the process-wide timer resolution; older systems keep Sleep(1).
void BarrierWait() {
    static HANDLE timer = CreateWaitableTimerExW(nullptr, nullptr, 0x00000002 /* CREATE_WAITABLE_TIMER_HIGH_RESOLUTION */,
                                                 TIMER_MODIFY_STATE | SYNCHRONIZE);
    const int64_t t = FramePace::Now();
    LARGE_INTEGER due;
    due.QuadPart = -10000;  // relative, 100 ns units
    if (timer && SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE)) WaitForSingleObject(timer, 20);
    else Sleep(1);
    FramePace::Add(FramePace::kWait, FramePace::Now() - t);
}

// Stall hold (2026-11-07 field session, two PCs over Steam, 13 matches): the friend's game froze ~830 ms four times and
// their packets stopped for ~600 ms ten times. Each time the other game ran to GGPO's prediction limit (8 frames) and
// waited; after it, the game that came out ahead stayed 4-8 frames ahead for 1-2 s (BALANCE gives at most 3 frames per
// 40 frames, paid 2 ms per frame), so every input change of the other player rolled it back 4-8 frames: 17 frames of
// 40-55 ms (load 6-15, replay 7-19, replay saves 12-23 ms) right after the 26 stalls, none anywhere else. Now, when this
// boundary waited >= gStallHoldMinMs for the peer, it keeps waiting (up to gStallHoldMaxMs more) until the peer's
// confirmed input is within `target` frames of this frame: the screen is frozen already, a little longer costs nothing
// visible, and the frames that follow run without the deep rollbacks. target = frames the input needs to arrive minus
// the input delay, plus 1 (ping 50 ms at delay 2: 1). Globals with the balance settings above.
double QpcMs(int64_t ticks) {
    static LARGE_INTEGER f = [] { LARGE_INTEGER q; QueryPerformanceFrequency(&q); return q; }();
    return ticks * 1000.0 / (double)f.QuadPart;
}
void StallHold(double waitedMs) {
    if (!gStallHold || gLockstep || waitedMs < gStallHoldMinMs) return;
    const int remote = 1 - gGgpoLocal;
    GGPONetworkStats ns{};
    int ping = 0;
    if (GGPO_SUCCEEDED(ggpo_get_network_stats(gGgpo, gGgpoPlayer[remote], &ns)))
        ping = ns.network.ping;
    // rc25: the lowest recent round trip (GGPO's latest sample right after a stall can hold the stall itself).
    if (ggpo_rtt_min_ms >= 0) ping = ggpo_rtt_min_ms;
    const int oneWay = (int)std::ceil(ping / 2.0 / (1000.0 / 60));  // frames, rounded up (rc24 subtracted 1 ms, not a frame)
    const int target = (oneWay > gGgpoDelay ? oneWay - gGgpoDelay : 0) + 1;
    const int gap0 = gP2PFrames - ggpo_last_confirmed_frame;
    gPhaseQuietUntil = gP2PFrames + 60;  // the phase measure skips the frames around a stall
    if (gap0 <= target) return;
    FramePace::Note("stallhold");
    const int64_t t0 = FramePace::Now();
    while (gP2PFrames - ggpo_last_confirmed_frame > target && QpcMs(FramePace::Now() - t0) < gStallHoldMaxMs) {
        BarrierWait();
        SteamBridge::Pump();
        ggpo_idle(gGgpo, 0);
    }
    const double held = QpcMs(FramePace::Now() - t0);
    ++gStallHolds;
    gStallHeldMs += held;
    gBalanceRestart = true;
    if (gStallHolds <= 20)
        Logger::Info("STALLHOLD gf=%u: waited %.0f ms for the peer; frame gap %d (target %d at ping %d ms, delay %d): held %.0f ms "
                     "more, gap now %d", GameFrame(), waitedMs, gap0, target, ping, gGgpoDelay, held,
                     gP2PFrames - ggpo_last_confirmed_frame);
}

// Wait shift (round 3, 2026-11-10 timing audit). MK9's limiter keeps a fixed 1/60 s grid (the comment at 'paysmooth'
// in GgpoBoundaryP2P): the frames after a late one start without a wait until the grid is caught up, whole for a frame
// up to 50 ms late, the remainder of 33 ms beyond. So the time a boundary spent waiting for the peer was taken back
// within a few frames. At the start barrier that undid the barrier: in all 10 fights of the ns15 stage batch (b46) the
// game that waited at frame 0 (38-52 ms) ran its next frames 4-5 ms apart and was 1-2 frames ahead again 50 ms later
// ("BALANCE ... 1.50 frames ahead" at 0.7 s, every rollback of the fight's first 10 s on the other game). After a
// prediction-limit wait or a stall hold the same sprint took back part of what the hold had evened. A boundary that
// waited now moves the limiter's deadline (0x00E0AB10) by the wait in whole frames, so the grid itself starts later
// and nothing is caught up; whole frames keep the sub-frame phase the phase alignment works on. At the start barrier
// the wait ends one network trip after the other game's frame 0 began, so that trip is not part of the offset between
// the games and is taken off first (the caller's estimate). Timing only: both games simulate the same frames.
void WaitShift(double waitedMs, double oneWayMs, const char* what) {
    if (!gWaitShift || gLockstep) return;
    const int frames = (int)std::floor((waitedMs - oneWayMs) * 60 / 1000 + 0.5);
    if (frames <= 0) return;
    *reinterpret_cast<volatile double*>(0x00E0AB10) += frames / 60.0;
    ++gWaitShifts;
    gWaitShiftFrames += frames;
    gBalanceRestart = true;  // the frame-advantage samples describe the games before the shift
    if (gWaitShifts <= 12)
        Logger::Info("WAITSHIFT gf=%u (%s): this boundary waited %.1f ms for the peer (one-way trip %.1f ms); the frame "
                     "deadline moves %d frame(s) later, nothing is caught up", GameFrame(), what, waitedMs, oneWayMs, frames);
}

// The start barrier's shift without the other game's timing: through rc29 the only one, now for 'startsync 0' and for a
// start in which nothing usable arrived from the other game. The handshake's round trip (no quality report yet at frame
// 0, and those are read at one poll a frame). Polled once a frame it is 0-2 frames more than the wire's, a frame on
// average (r3smoke: 50 and 53 ms on the harness link whose trip is one frame; half of that left a frame of lead, which
// the phase alignment took 8 s to remove): less that frame, half of it is within half a frame of the trip. Polled every
// millisecond on both games ('handshakepoll 1'; the two games of a session are the same build) it is the wire's.
// Field evening 2026-11-11: the frame polls read the wire's own 27-28 ms in 6 of 14 starts, the guess was then 5-6 ms
// for a 13.5 ms trip and the shift one frame too many (StartSync.h).
void StartBarrierGuess(double waitedMs) {
    const int rtt = ggpo_sync_rtt_ms >= 0 ? ggpo_sync_rtt_ms : ggpo_rtt_min_ms;
    const double pollMs = gHandshakePoll && ggpo_sync_rtt_ms >= 0 ? 0 : 1000.0 / 60;
    const double oneWay = rtt > pollMs ? (rtt - pollMs) / 2 : 0;
    WaitShift(waitedMs, oneWay, "start barrier");
}

// Start alignment (2026-11-12; the rule and its reasons: src/net/StartSync.h). From its frame-0 boundary each game tells
// the other, on the checksum channel, how long until its own frame 1; from the two games' readings the one whose frame 1
// comes first moves its deadline later by the measured difference, to the millisecond, instead of by whole frames of
// (wait - guessed trip). Timing only: both games simulate the same frames.
// The frame limiter's clock in microseconds (FramePace.cpp, "limiter wait": the deadline 0x00E0AB10 is in seconds of
// QueryPerformanceCounter x [0x00E42FF0]).
int64_t LimiterNowUs() {
    LARGE_INTEGER q;
    QueryPerformanceCounter(&q);
    return (int64_t)((double)q.QuadPart * *reinterpret_cast<const double*>(0x00E42FF0) * 1e6);
}
// The other game's start datagrams waiting on the checksum socket, all taken as read at one time. Another kind of
// datagram from the peer ends the read and stays for CsPump (none is sent before the sender's frame 24, but a game can
// be that far ahead of this one's frame 12 at a long frame delay).
int StartSyncRead(bool prompt) {
    if (gCsSock == INVALID_SOCKET) return 0;
    const int64_t now = LimiterNowUs();
    int n = 0, resets = 0;
    for (int k = 0; k < 1024; ++k) {
        uint32_t in[16];
        sockaddr_in from{};
        int fromLen = sizeof from;
        const int got = recvfrom(gCsSock, reinterpret_cast<char*>(in), sizeof in, MSG_PEEK, reinterpret_cast<sockaddr*>(&from), &fromLen);
        if (got < 0) {
            // "Connection reset" (a system that reports it although CsOpen turned the reports off): the report of an
            // earlier datagram that found no listener; taken off the queue, the datagrams behind it are fine.
            if (WSAGetLastError() != WSAECONNRESET || ++resets > 8) break;
            recvfrom(gCsSock, reinterpret_cast<char*>(in), sizeof in, 0, nullptr, nullptr);
            continue;
        }
        const bool peer = from.sin_addr.s_addr == gCsPeer.sin_addr.s_addr;
        if (peer && (got != (int)sizeof(StartSync::Packet) || in[0] != StartSync::kMagic)) break;
        recvfrom(gCsSock, reinterpret_cast<char*>(in), sizeof in, 0, nullptr, nullptr);
        if (!peer) continue;  // a direct-IP session's socket takes datagrams from the whole network (CsPump)
        StartSync::Packet p;
        memcpy(&p, in, sizeof p);
        gSs.OnPacket(p, now, prompt);
        ++n;
    }
    return n;
}
void StartSyncSend() {
    const StartSync::Packet p = gSs.Make(LimiterNowUs());
    sendto(gCsSock, reinterpret_cast<const char*>(&p), sizeof p, 0, reinterpret_cast<const sockaddr*>(&gCsPeer), sizeof gCsPeer);
    SteamBridge::PumpOut();  // over the Steam bridge: on its way now, not at the next poll (the time in it is this moment's)
}
// Decides as soon as both games' gaps are known, or at `final` with what there is, and moves the deadline. true: decided.
bool StartSyncDecide(bool final) {
    if (!gSs.Begun() || gSs.Decided()) return true;
    int rtt = ggpo_sync_rtt_ms;
    if (ggpo_rtt_min_ms >= 0 && (rtt < 0 || ggpo_rtt_min_ms < rtt)) rtt = ggpo_rtt_min_ms;
    const StartSync::Verdict v = gSs.Decide(final, (int64_t)(gSsWaitedMs * 1000), rtt > 0 ? rtt * 500 : 0);
    if (v.kind == StartSync::Verdict::kWait) return false;
    char gaps[96];
    if (gSs.Mine() == StartSync::kNone) snprintf(gaps, sizeof gaps, "own gap none, theirs %s", gSs.Peer() == StartSync::kNone ? "none" : "known");
    else if (gSs.Peer() == StartSync::kNone) snprintf(gaps, sizeof gaps, "own gap %.1f ms%s, theirs none", gSs.Mine() / 1000.0, gSs.MinePrompt() ? "" : " (read late)");
    else snprintf(gaps, sizeof gaps, "gaps %.1f%s and %.1f ms", gSs.Mine() / 1000.0, gSs.MinePrompt() ? "" : " (read late)", gSs.Peer() / 1000.0);
    if (v.kind == StartSync::Verdict::kUnknown) {
        Logger::Info("STARTSYNC gf=%u (frame %d): no usable timing from the other game (%s; %u datagram(s) read, %u sent); this "
                     "game waited %.1f ms at frame 0%s", GameFrame(), gP2PFrames, gaps, gSs.Reads(), gSs.Sends(), gSsWaitedMs,
                     gSsWaitedMs > 0 ? ": the wait and the handshake's round trip decide (rc29)" : "");
        if (gSsWaitedMs > 0) StartBarrierGuess(gSsWaitedMs);
        return true;
    }
    const bool move = v.kind == StartSync::Verdict::kShift && gWaitShift;
    if (move) {
        *reinterpret_cast<volatile double*>(0x00E0AB10) += v.shiftUs / 1e6;
        gSsMovedUs = v.shiftUs;
        gBalanceRestart = true;  // the frame-advantage samples describe the games before the shift
    }
    gSsTripUs = v.tripUs;
    char trip[112];
    if (v.tripUs != StartSync::kNone)
        snprintf(trip, sizeof trip, "one-way trip %.1f ms measured (round trip %.1f)%s", v.tripUs / 1000.0, v.tripUs / 500.0,
                 v.exact ? "" : ", a gap read late");
    else if (v.guessed) snprintf(trip, sizeof trip, "their gap did not arrive: trip taken as %.1f ms from the round trips", rtt > 0 ? rtt / 2.0 : 0.0);
    else snprintf(trip, sizeof trip, "their gap did not arrive: this game's own says it is the later one");
    Logger::Info("STARTSYNC gf=%u (frame %d): the other game's frame 1 starts %s%.1f ms %s this game's (%s; %s; %u datagram(s) read, "
                 "%u sent; this game waited %.1f ms at frame 0): %s", GameFrame(), gP2PFrames, v.bound ? "more than " : "",
                 (v.aheadUs < 0 ? -v.aheadUs : v.aheadUs) / 1000.0, v.aheadUs < 0 ? "before" : "after", trip, gaps, gSs.Reads(),
                 gSs.Sends(), gSsWaitedMs,
                 move ? "this game's frame deadline moves that much later, nothing is caught up"
                      : v.kind == StartSync::Verdict::kShift ? "not moved (waitshift 0)" : "this game's frames stay where they are");
    return true;
}
// Frame 0's boundary, right after this game's first input left.
void StartSyncBegin() {
    gSs = StartSync::Session();
    gSsWaitedMs = 0;
    gSsFrameAt = 0;
    if (gCsSock == INVALID_SOCKET) return;
    if (!(*reinterpret_cast<const double*>(0x00E42FF0) > 0)) return;
    const int64_t now = LimiterNowUs();
    const int64_t frame1 = (int64_t)(*reinterpret_cast<const volatile double*>(0x00E0AB10) * 1e6);
    // The limiter keeps its deadline within three frames of now (the 1/60 s grid); anything else is not that wait.
    if (frame1 - now > 100000 || now - frame1 > 100000) {
        Logger::Info("STARTSYNC gf=%u: the frame deadline is %.0f ms from now, not a 60 Hz grid: the start barrier's own "
                     "estimate decides (rc29)", GameFrame(), (frame1 - now) / 1000.0);
        return;
    }
    gSs.Begin(now, frame1);
    // What the other game sent while it waited for this one: all of it, the newest counts (the bridge passes 64 a call).
    for (int round = 0; round < 8; ++round) {
        SteamBridge::Pump();
        if (!StartSyncRead(true)) break;
    }
    StartSyncSend();
    StartSyncSend();  // twice: the datagrams are unreliable, and the waiting game decides when it reads one of these
}
// In the start barrier's wait loop, after each 1 ms poll.
void StartSyncLoopPoll() {
    if (!gSs.Begun()) return;
    StartSyncRead(true);
    if (gSs.WantSend(LimiterNowUs())) StartSyncSend();
}
// At the boundary of frames 1 .. StartSync::kFrames, after the boundary's first poll: the other game's gap when it was
// still on the way at frame 0 (both games waited), and the datagrams repeated for lost ones.
void StartSyncAtFrame() {
    if (!gSs.Begun() || gP2PFrames == gSsFrameAt) return;  // not twice for a boundary that advanced no frame
    gSsFrameAt = gP2PFrames;
    StartSyncRead(false);
    if (gSs.WantSendAtFrame(gP2PFrames)) StartSyncSend();
    StartSyncDecide(gP2PFrames >= StartSync::kFrames);
}

// X-ray lock (2026-11-08, rc14). The X-ray desyncs of the online matrix (batch 22: all four, one-frame shifts with the
// attacker's game behind) and the crashes of batches 22-23 (IK list, render data deletes, motion controllers, cinematic
// object variables, all at loads during X-rays) come from loads that land on or inside an X-ray: a rollback across the
// start replays it in a world that already has the X-ray level, which starts it a frame late on that game only, and a
// load inside the cinematic must rebuild cinematic objects that no snapshot owns. A frame that ran on confirmed inputs
// is never rolled back, so while an X-ray can start or a cinematic runs, a P2P frame runs only once the peer's input for
// it has arrived (the lockstep barrier):
//  - after a flip-stance press (0x4000; X-ray = block + flip stance, 0xC000) by a player whose meter is full: the local
//    player's as it is added (it applies `delay` frames later), the peer's once GGPO hands it over (live or in a replay),
//    for `window` frames from the last such press;
//  - while a fighter's slot 1 or 2 holds a cinematic controller (X-rays, throws: CineMotionVt).
// A correction can then only reach frames before the lock, which are before the local press; the peer's press is
// covered from the frame its input arrives (a replay before that may still run a buffered start on a prediction).
// The cost is a wait only when the peer's input is late: none while the frame delay covers the trip, a few frames when
// the press finds this game running ahead of the peer's input. Lockstep and rollback run the same simulation, so the
// lock changes timing only and the two games need not agree on it.
// rc15 (batch 26: lockstep runs at 60 fps only while the round trip fits in 2 x delay frames, and the ping rule's delay
// is the one-way trip minus one, so at a ~185 ms round trip / delay 4 the rc14 locks ran at ~40 fps: 32-60 frames >= 34 ms
// a minute, 75-90% of the locked frames in the cinematic; and the cinematic began 30-40 frames after the press, once
// the X-ray connected, once 4 unlocked frames after a 30-frame window): the press window is 60 frames and lasts until
// the X-ray cinematic begins (a cinematic controller in a fighter's slot 1 or 2 while the window is open, at the frame C
// both games see first), then `delay + 4` frames more. From C both games add the input they added before C ('xrayfreeze',
// default on: the X-ray is a cinematic, the players' input does nothing), so every input of the cinematic is the
// confirmed one and GGPO's prediction (the last input) is right: no rollback, no wait. When the controllers have been
// gone for 2 frames each game adds its live input again; the first change applies at end + delay, after the cinematic.
// Throws and other cinematics (no X-ray press before them) are neither locked nor frozen ('xraylockcine 1': lock every
// cinematic, the rc14 rule).
float PlayerMeter(int p) { return *reinterpret_cast<const float*>(0x00E89D7C + 0xC8C * p + 0xC14); }  // PlayerInfo +0xC14
void XrayLockNote(const uint32_t masks[2], bool replay) {
    if (!gXrayLock || gLockstep || !gGgpoRunning || gXrayCine) return;  // the meter is spent in the cinematic anyway
    for (int p = 0; p < 2; ++p) {
        if (!(masks[p] & 0x4000) || PlayerMeter(p) < 0.995f) continue;
        if (gXrayLockUntil < gP2PFrames) gXrayLockWhy = p == gGgpoLocal ? (replay ? "own press, replayed" : "own press")
                                                                              : (replay ? "peer's press, in a replay" : "peer's press");
        gXrayLockUntil = gP2PFrames + gXrayLockWindow;
    }
}
bool CineControllerNow() {
    for (int p = 0; p < 2; ++p) {
        const uint32_t a = FighterActor(p);
        if (!a) continue;
        for (int k = 1; k <= 2; ++k) {
            const uint32_t c = *reinterpret_cast<const uint32_t*>(a + 0x13C + 4 * k);
            if (c && Readable(c, 4) && CineMotionVt(*reinterpret_cast<const uint32_t*>(c))) return true;
        }
    }
    return false;
}
// Before the local input is added (each live boundary): follows the X-ray cinematic and holds the input through it.
void XrayLockPre() {
    if (!gXrayLock || gLockstep || !gGgpoRunning) {
        gXrayCine = false;
        return;
    }
    const bool cineNow = CineControllerNow();
    if (gXrayCine) {
        gXrayCineGone = cineNow ? 0 : gXrayCineGone + 1;
        if (gXrayCineGone >= 2 || gP2PFrames - gXrayCineFrame > 1800) {  // over (or stuck: 30 s)
            gXrayCine = false;
            if (gXrayLockLogs++ < 40)
                Logger::Info("XRAYFREEZE gf=%u: the X-ray cinematic of frame %d is over after %d frames; live input again", GameFrame(),
                             gXrayCineFrame, gP2PFrames - gXrayCineFrame);
        }
    } else if (cineNow && gP2PFrames <= gXrayLockUntil) {
        gXrayCine = true;
        gXrayCineFrame = gP2PFrames;
        gXrayCineGone = 0;
        gXrayHeld = gXrayLastAdded;
        gXrayLockUntil = -1;  // the press window ends here; the lock runs on to C + delay + 4
        ++gXrayCines;
        if (gXrayLockLogs++ < 40)
            Logger::Info("XRAYFREEZE gf=%u: X-ray cinematic from frame %d; %s", GameFrame(), gP2PFrames,
                         gXrayFreeze ? "this game's input is held until it ends" : "input not held (xrayfreeze 0)");
    }
    if (gXrayCine && gXrayFreeze) {
        gLocalRaw = gXrayHeld;
        ++gXrayFrozenFrames;
    }
}
// Decided once per boundary, before frame gP2PFrames is synchronized.
bool XrayLockNow() {
    if (!gXrayLock || gLockstep) return false;
    const int local = gGgpoLocal;
    if (local < 0 || local > 1) return false;
    gXrayLastAdded = gLocalRaw;  // added at this boundary (the held one in the cinematic)
    uint32_t own[2] = {0, 0};
    own[local] = gLocalRaw;  // this boundary's input (frame + delay)
    XrayLockNote(own, false);
    if (gP2PFrames > 0) {  // the previous frame's inputs: the peer's is its last received one (GGPO predicts by repeating it)
        uint32_t peer[2] = {0, 0};
        peer[1 - local] = gSyncMasks[1 - local];
        XrayLockNote(peer, false);
    }
    bool on = gXrayCine ? gP2PFrames <= gXrayCineFrame + gGgpoDelay + 4 : gP2PFrames <= gXrayLockUntil;
    bool cine = gXrayCine && on;
    if (!on && gXrayLockCine && CineControllerNow()) on = cine = true;
    if (on) {
        ++gXrayLockFrames;
        if (cine) ++gXrayLockCineFrames;
        if (!gXrayLockOn) {
            ++gXrayLocks;
            gXrayLockStartFrame = gP2PFrames;
            gXrayLockFrameWaits = 0;
            gXrayLockStartWaitMs = 0;
            if (gXrayLockLogs++ < 40)
                Logger::Info("XRAYLOCK gf=%u: on (%s); frame %d, the peer's input confirmed to %d", GameFrame(), cine ? "cinematic" : gXrayLockWhy,
                             gP2PFrames, ggpo_last_confirmed_frame);
        }
    } else if (gXrayLockOn) {
        gBalanceRestart = true;  // the frame-advantage samples of the lock describe its waits
        if (gXrayLockLogs++ < 40)
            Logger::Info("XRAYLOCK gf=%u: off after %d frames (%d frame(s) waited for the peer, %.0f ms)", GameFrame(),
                         gP2PFrames - gXrayLockStartFrame, gXrayLockFrameWaits, gXrayLockStartWaitMs);
    }
    gXrayLockOn = on;
    return on;
}
void XrayLockWaited(double ms) {
    ++gXrayLockWaits;
    ++gXrayLockFrameWaits;
    gXrayLockWaitMs += ms;
    gXrayLockStartWaitMs += ms;
}

// WorldTick entry of live frame F (P2P): close F-1, let GGPO poll (rollbacks happen here), then add this
// frame's local input and synchronize; the masks are applied at EngineTick entry (RecordInputs).
void GgpoBoundaryP2P(void* engine, float dt) {
    const int64_t boundaryT0 = FramePace::Now();
    gGgpoEngine = engine;
    gGgpoDt = dt;
    gGgpoLiveF = GameFrame();
    gGgpoLoadedS = 0xFFFFFFFF;
    const uint32_t hostAtF = HostFrame();
    // The lazy live save (OnTickEntry deferred it): GGPO's save request for F inside ggpo_advance_frame is answered
    // without a copy; a rollback in this poll saves F in its replay, otherwise F is saved right after the poll (nothing
    // ran in between: the state is still F's boundary state).
    if (gFreezeMs > 0 && gFreezeEveryMs > 0 && gGgpoRunning && gP2PFrames > 0) {  // 'freezeevery' (test): a stalled PC
        const ULONGLONG now = GetTickCount64();
        if (!gFreezeNext) gFreezeNext = now + gFreezeEveryMs;
        if (now >= gFreezeNext) {
            gFreezeNext = now + gFreezeEveryMs;
            Logger::Info("FREEZE gf=%u: this game stops %d ms (test)", GameFrame(), gFreezeMs);
            Sleep(gFreezeMs);
        }
    }
    gDeferLive = true;
    if (gGgpoFrameOpen) {
        ggpo_advance_frame(gGgpo);
        gGgpoFrameOpen = false;
        ++gP2PFrames;
    }
    // Early send (rc21, 2026-11-09 latency audit): GGPO sends a local input the moment it is added. Added after the poll,
    // the rollback and the live save (as before), it left 3.5-4 ms after the boundary's start on a fast PC, 5-8 ms on a
    // slower one, plus a rollback's load and replay on the frames that had one: the other game received it that much
    // later and rolled back more at delay 1. Added here, before anything else of the boundary, it leaves at once; the
    // loop below then finds it added. The pause test and the X-ray hold are evaluated as they are below.
    Input::SampleDeferredNow();  // late sampling: this pass's controller sample, taken after MK9's frame wait
    // LATENCY: the input sampled for GGPO frame gP2PFrames is applied `delay` frames later; that frame's picture gets
    // this sample's time at the end of its own boundary (below).
    static int64_t sampledAt[64];
    sampledAt[gP2PFrames & 63] = Input::LocalSampleQpc();  // scripted runs too: the pad is sampled at the same point
    bool earlyAdded = false;
    if (gEarlyInput && gGgpoRunning && *reinterpret_cast<const uint32_t*>(0x00E43054) == 0) {
        // GGPO saves frame 0 inside the first add (Sync::AddLocalInput): checksum frame 0 must start here, as at the
        // later add below, or that save is recorded as frame 1 and the start check (frame 0 differs) never runs
        // (rc21 two-instance batch b37: PitDay fights started from different states ran desynced).
        if (gP2PFrames == 0 && !gGgpoFrameOpen) gCsStartGf = GameFrame();
        if (!gLocalScripted) gLocalRaw = Input::LocalHardwareMask() & ~0x0F00u;
        XrayLockPre();
        earlyAdded = GGPO_SUCCEEDED(ggpo_add_local_input(gGgpo, gGgpoPlayer[gGgpoLocal], &gLocalRaw, sizeof gLocalRaw));
        if (earlyAdded) {
            SteamBridge::Pump();
            ++gEarlyAdds;
            // Start alignment: frame 0's input has just left; this game's time to its frame 1 goes after it, with what
            // the other game's datagrams say if it got here first (StartSyncBegin).
            if (gP2PFrames == 0 && gStartSync && gStartBarrier && gWaitShift && !gLockstep) StartSyncBegin();
        }
    }
    SteamBridge::Pump();
    ggpo_idle(gGgpo, 0);
    if (gGgpoRunning && gP2PFrames > 0 && gP2PFrames <= StartSync::kFrames) StartSyncAtFrame();
    // The paused wait for the session (gHandshakePoll): the handshake's packets answered and read within a millisecond.
    while (gHandshakePoll && gNetplayWaitRunning && !gGgpoRunning && QpcMs(FramePace::Now() - boundaryT0) < kHandshakePollMs) {
        BarrierWait();
        SteamBridge::Pump();
        ggpo_idle(gGgpo, 0);
    }
    gDeferLive = false;
    PhaseSample(ggpo_last_confirmed_frame >= gP2PFrames);  // the other player's input for this frame already in?
    if (gLivePending == gGgpoLiveF && !gGgpoLoadFailed && gSparseLive && gSparseSave && ggpo_frame_loadable && !gLockstep &&
        gLivePendingGgpoGf == gGgpoLiveF && (gLivePendingGgpo & 1)) {
        gHave[gGgpoLiveF % kRing] = false;  // odd live frame (sparselive): a rollback to it loads the frame before
        gLivePending = 0xFFFFFFFF;
        gLiveSavedGf = 0xFFFFFFFF;
        FramePace::SkippedSave();
        ++gSparseLiveSkips;
    }
    // Confirmed live frame (rc21, save audit): once this poll has the other player's input for F, no rollback can load F,
    // as for the replayed frames above (LiveFrameConfirmed: F <= ggpo_rollback_safe_frame since rc25). With the input
    // sent at the boundary's start that is most frames at a short round trip: their save (2.5-5 ms) is not taken, which
    // also takes it off the time between the controller sample and this frame's picture. 'skiplive 0': save every one.
    if (gLivePending == gGgpoLiveF && !gGgpoLoadFailed && gSkipConfirmed && gGgpoIsP2P && gLivePendingGgpoGf == gGgpoLiveF &&
        LiveFrameConfirmed(gLivePendingGgpo)) {
        gHave[gGgpoLiveF % kRing] = false;
        gLivePending = 0xFFFFFFFF;
        gLiveSavedGf = 0xFFFFFFFF;
        FramePace::SkippedSave();
        ++gConfirmedLiveSkips;
    }
    bool savedLive = false;
    if (gLivePending == gGgpoLiveF && !gGgpoLoadFailed) {
        savedLive = true;
        if (!SaveLiveNow(gGgpoLiveF) && !gSaveRefusedTeardown) ++gGgpoSaveFails;
    }
    // Re-poll (gRepoll): the save took 2.6-5 ms; the other player's input for this frame may have come in meanwhile.
    if (gRepoll && savedLive && gGgpoRunning && !gLockstep && !gGgpoLoadFailed && ggpo_last_confirmed_frame < gP2PFrames) {
        SteamBridge::Pump();
        ggpo_idle(gGgpo, 0);
        ++gRepolls;
        if (ggpo_last_confirmed_frame >= gP2PFrames) ++gRepollHits;
    }
    if (gTimesyncFrames > 0) {  // the remote is behind: give it time (simple time sync)
        // Not in lockstep: the barrier below already waits for the remote's input every frame.
        if (!gLockstep) gTimesyncDebt += gTimesyncFrames > 4 ? 4 : gTimesyncFrames;
        gTimesyncFrames = 0;
    }
    // Frame-advantage balance (2026-11-04). GGPO's own time sync only asks for a wait when this game is >= 3 frames
    // ahead with the local input unchanged over its last frames: in the stage sweep one game stayed 2-5 frames ahead for
    // whole fights (NETSTATS "frames behind: local 1 remote 9") and ran every rollback (Graveyard 116 against 0,
    // FleshPits 0 against 113), one >= 34 ms frame each. GGPO's estimates are sampled every 15 frames; once 8 samples
    // taken after the last payment (plus 60 frames for the peer's quality report to see it) put this game >= 1 frame
    // ahead, it gives that many frames (at most 3) through the same debt.
    // Tuned 2026-11-04 (rng1 Courtyard): one game ran ~0.4 frames/s faster in wall time (the other 59.6 fps) and stayed
    // 2-3 frames ahead although it gave 12 frames, with depth 4-5 rollbacks: samples every 10 frames, a decision after
    // 6 (the peer's quality report, which carries its view, comes every 60 frames), 45 frames to settle after paying.
    if (gBalance && !gLockstep && gGgpoRunning && gP2PFrames > 0 && gP2PFrames % 10 == 0) {
        static int samples = 0, sumDiff = 0, lastFrame = -1, settleUntil = 0;
        if (gP2PFrames < lastFrame) samples = sumDiff = settleUntil = 0;  // a new session
        if (gBalanceRestart) {  // a stall hold just evened the games: older samples describe the stall
            gBalanceRestart = false;
            samples = sumDiff = 0;
            settleUntil = gP2PFrames + gBalanceSettle;
        }
        const bool fresh = gP2PFrames != lastFrame;  // a boundary that advanced no frame samples nothing
        lastFrame = gP2PFrames;
        GGPONetworkStats ns{};
        const int remote = 1 - gGgpoLocal;
        if (!fresh) {
        } else if (gTimesyncDebt > 0) {
            settleUntil = gP2PFrames + gBalanceSettle;
            samples = sumDiff = 0;
        } else if (gP2PFrames >= settleUntil && ggpo_rtt_min_ms >= 0 &&
                   GGPO_SUCCEEDED(ggpo_get_network_stats(gGgpo, gGgpoPlayer[remote], &ns))) {
            sumDiff += ns.timesync.remote_frames_behind - ns.timesync.local_frames_behind;  // 2x frames ahead
            if (++samples >= gBalanceSamples) {
                const float ahead = sumDiff / (2.0f * samples);
                if (ahead >= 1.0f) {
                    const int give = ahead >= (float)gBalanceMaxGive ? gBalanceMaxGive : (int)ahead;
                    gTimesyncDebt += give;
                    // One actuator at a time (timing audit, b46 fight 2: a frame given and a 3 ms phase step within 18 ms
                    // for a lead under 6 ms, then the other game gave a frame back): a whole frame replaces the phase
                    // step that was under way; the phase measure starts again once the frame is paid.
                    gPhaseDueMs = 0;
                    if (gPhaseQuietUntil < gP2PFrames + 60) gPhaseQuietUntil = gP2PFrames + 60;
                    ++gBalanceEvents;
                    gBalanceFrames += give;
                    if (gBalanceEvents <= 8)
                        Logger::Info("BALANCE gf=%u: %.2f frames ahead of the peer (%d samples); giving %d frame(s)", GameFrame(), ahead,
                                     samples, give);
                }
                samples = sumDiff = 0;
            }
        }
    }
    // Paid one frame at a time, every 20th frame: Sleep(frames * 16) froze the game for 3-5 frames in one piece (62-78 ms
    // at the 15.6 ms timer). GGPO asks again at most every 240 frames (RECOMMENDATION_INTERVAL), so 4 frames are paid
    // well before. 1 ms high-resolution waits, the bridge and GGPO polled in between as in the barrier.
    // MK9's frame limiter (TimedEngineFrame 0x004025A0) keeps a fixed 1/60 s grid: after its wait the next deadline
    // (double 0xE0AB10) is now + fmod(deadline + period - now, 2 periods) (the factor is 2 with the flag 0xDECAFC = 1,
    // 32 without). A 16 ms wait inside the tick only made this frame late; the next frame then ran without a wait and
    // the grid caught up, so little time reached the peer (regression r2 fight 1, 2026-11-04: >= 21 frames given, 14
    // grid slots lost in 33 s, A ahead all fight). 'paysmooth N' moves the deadline itself N ms per frame instead: each
    // frame starts N ms later, with no long frame, until 1/60 s has been paid per frame owed.
    static int paidAt = -1;  // a boundary that advanced no frame (paused) must not pay again
    // Sub-frame phase alignment (PhaseDecide): the same actuator, kPhasePerFrameMs a frame until the decided shift is made.
    static int phaseAt = -1;
    if (gPhaseDueMs > 0 && gP2PFrames != phaseAt && !gLockstep) {
        phaseAt = gP2PFrames;
        const double step = gPhaseDueMs < kPhasePerFrameMs ? gPhaseDueMs : kPhasePerFrameMs;
        *reinterpret_cast<volatile double*>(0x00E0AB10) += step / 1000.0;
        gPhaseDueMs -= step;
        gPhaseShiftedMs += step;
    }
    if (gTimesyncDebt > 0 && gPaySmoothMs > 0 && gP2PFrames != paidAt) {
        paidAt = gP2PFrames;
        // rc25: debts of up to 2 frames (most BALANCE gives) are paid gPaySmoothSmallMs a frame: a 6% longer frame instead
        // of 12%, the same total.
        const int payMs = gTimesyncDebt <= 2 && gPaySmoothSmallMs > 0 && gPaySmoothSmallMs < gPaySmoothMs ? gPaySmoothSmallMs : gPaySmoothMs;
        *reinterpret_cast<volatile double*>(0x00E0AB10) += payMs / 1000.0;
        gPaidMs += payMs;
        gShiftedMs += payMs;
        if (gPaidMs >= 1000.0 / 60) {
            gPaidMs -= 1000.0 / 60;
            --gTimesyncDebt;
        }
    } else if (gTimesyncDebt > 0 && gPaySmoothMs <= 0 && gP2PFrames % (gTimesyncDebt >= 2 ? 10 : 20) == 0 && gP2PFrames != paidAt) {
        --gTimesyncDebt;
        paidAt = gP2PFrames;
        FramePace::Note("paywait");
        LARGE_INTEGER f, t0, t;
        QueryPerformanceFrequency(&f);
        QueryPerformanceCounter(&t0);
        do {
            BarrierWait();
            SteamBridge::Pump();
            ggpo_idle(gGgpo, 0);
            QueryPerformanceCounter(&t);
        } while ((t.QuadPart - t0.QuadPart) * 1000 < f.QuadPart * 16);
    }
    gSyncValid = false;
    // A paused tick runs no fight update (the unpause after GGPO's 'running' takes effect at the end of the tick): it
    // must not become a GGPO frame. A replay of it would run a full frame (two-instance test: 3 RNG draws in the replay
    // of frame 0, 0 live). GGPO's frame 0 is the first unpaused frame, which is also checksum frame 0.
    // An input added early belongs to this frame: GGPO asserts on a second add for the same frame, so the frame must be
    // synchronized now even if the pause flag changed since (the netplay pause guard refuses pauses in a session).
    const bool earlyDone = earlyAdded;
    const bool paused = !earlyDone && *reinterpret_cast<const uint32_t*>(0x00E43054) != 0;
    if (gGgpoRunning && !paused && gP2PFrames == 0 && !gGgpoFrameOpen) gCsStartGf = GameFrame();
    // Not for a session this poll has already ended (gGgpoLoadFailed: a load without a snapshot, or a replay that
    // started the next map): the caller stops it right after this boundary. Adding an input could wait here up to 5 s
    // for a peer that has closed its own session at the same fight end, and then replaced the reason with "no
    // synchronized input", an abnormal stop.
    if (gGgpoRunning && !paused && !gGgpoLoadFailed) {
        const int local = gGgpoLocal;
        // Wait for the remote input (lockstep) or until GGPO accepts the local one (rollback: too far ahead of the
        // remote). Either way a frame never runs unsynchronized: it would get the pad instead of GGPO's inputs and shift
        // the frame numbering against the peer (two-instance test with a slowed instance, 2026-10-18).
        // 5 s (GGPO's disconnect timeout is 3 s). A count of Sleep(1) calls was meant as 5 s but each one waits a whole
        // system timer tick (15.6 ms; MKKE never raises the timer resolution): 78 s.
        const ULONGLONG deadline = GetTickCount64() + 5000;
        // The player's controllers as sampled in this main-loop pass (the sampler call 0x404E26 runs right before this
        // boundary). Read at EngineTick entry instead (RecordInputs), the input reached GGPO one pass later: a frame of
        // input latency for nothing. A scripted mask (harness) still comes from RecordInputs.
        if (!earlyAdded) {
            if (!gLocalScripted) gLocalRaw = Input::LocalHardwareMask() & ~0x0F00u;
            XrayLockPre();  // in an X-ray cinematic: the input held from before it
        }
        int64_t thresholdSince = 0;  // the first prediction-limit wait of this boundary (stall hold)
        while (GetTickCount64() < deadline) {
            // Added at the boundary's start (early send): accepted then, so not again.
            const GGPOErrorCode r =
                earlyAdded ? GGPO_OK : ggpo_add_local_input(gGgpo, gGgpoPlayer[local], &gLocalRaw, sizeof gLocalRaw);
            earlyAdded = false;
            if (GGPO_SUCCEEDED(r)) {
                if (thresholdSince) {
                    StallHold(QpcMs(FramePace::Now() - thresholdSince));
                    WaitShift(QpcMs(FramePace::Now() - thresholdSince), 0, "prediction limit");
                }
                // Rollback: frame 0 too (start barrier, 2026-11-04). The two games reach GGPO's 'running' up to ~0.8 s
                // apart (sync retries); the first one ran its frames 0-7 on predicted inputs, stopped at the prediction
                // limit until the other started, then rolled back to frame 0 and paid time-sync debt (every two-instance
                // start). Waiting here for the peer's first input starts both within one network trip of each other.
                const bool xrayLock = XrayLockNow();
                if (gLockstep || (gP2PFrames == 0 && gStartBarrier) || xrayLock) {
                    // Synchronize frame gP2PFrames only once the remote input for it has arrived: GGPO then never
                    // hands out a predicted input, so it never rolls back. (Under the X-ray lock a correction of an
                    // earlier, predicted frame can still roll back inside this wait, as in the prediction-limit wait.)
                    const int64_t tw = FramePace::Now();
                    bool waited = false;
                    if (xrayLock && ggpo_last_confirmed_frame < gP2PFrames) FramePace::Note("xraylock");
                    while (ggpo_last_confirmed_frame < gP2PFrames && GetTickCount64() < deadline) {
                        ++gBarrierWaits;
                        waited = true;
                        BarrierWait();
                        SteamBridge::Pump();
                        ggpo_idle(gGgpo, 0);
                        if (gP2PFrames == 0) StartSyncLoopPoll();  // start alignment: read within this millisecond, sent on every poll
                    }
                    if (xrayLock && waited) XrayLockWaited(QpcMs(FramePace::Now() - tw));
                    if (ggpo_last_confirmed_frame < gP2PFrames) break;  // stopped below (gSyncValid stays false)
                    if (!gLockstep && !xrayLock && gP2PFrames == 0) {
                        const double waitedMs = waited ? QpcMs(FramePace::Now() - boundaryT0) : 0;
                        if (gSs.Begun()) {
                            // Start alignment. The game that waited decides here when it has both games' gaps (the other
                            // game's datagram left with its first input; read a poll later than that input it gets up to
                            // 3 polls more). A game whose partner's gap is still on the way decides at one of its next
                            // boundaries (StartSyncAtFrame), without waiting here.
                            gSsWaitedMs = waitedMs;
                            for (int k = 0; !StartSyncDecide(false) && waited && k < 3; ++k) {
                                BarrierWait();
                                SteamBridge::Pump();
                                ggpo_idle(gGgpo, 0);
                                StartSyncLoopPoll();
                            }
                        } else if (waited) {
                            StartBarrierGuess(waitedMs);
                        }
                    }
                }
                int disconnected = 0;
                if (GGPO_SUCCEEDED(ggpo_synchronize_input(gGgpo, gSyncMasks, sizeof gSyncMasks, &disconnected))) {
                    gSyncValid = true;
                    gGgpoFrameOpen = true;
                }
                break;
            }
            if (r != GGPO_ERRORCODE_PREDICTION_THRESHOLD) break;
            if (!thresholdSince) thresholdSince = FramePace::Now();
            ++gBarrierWaits;  // too far ahead of the remote: wait for its inputs (still at the boundary)
            BarrierWait();
            SteamBridge::Pump();
            ggpo_idle(gGgpo, 0);
        }
        if (!gSyncValid) {  // running this frame unsynchronized would desynchronize the peers for good
            Logger::Info("%s: no synchronized input for gf=%u after the wait (%d waits); the session is stopped",
                         gLockstep ? "LOCKSTEP" : "P2P", GameFrame(), gBarrierWaits);
            gGgpoLoadFailed = true;  // stopped at the boundary by the caller
            gGgpoStopWhy = "no synchronized input from the peer in time";
        }
        if (gSyncValid && gP2PFrames == 0 && gNetplaySeedPin) {
            // GGPO's frame 0 is this snapshot (seeded). Re-applying the seeds at the next boundary would give the live
            // frame 1 a state no replay of frame 0 produces (two-instance, slowed instance: frame 2 RNG three draws
            // from the seed instead of two).
            gNetplaySeedPin = false;
            Logger::Info("NETPLAY: seeds released with GGPO frame 0 at gf=%u", GameFrame());
        }
        if (gP2PFrames < 3)
            Logger::Info("P2P boundary: GGPO frame %d at gf=%u, pause flag %u, synchronized %d, inputs %X %X", gP2PFrames,
                         GameFrame(), *reinterpret_cast<const uint32_t*>(0x00E43054), (int)gSyncValid, gSyncMasks[0],
                         gSyncMasks[1]);
    }
    if (gGgpoLoadedS != 0xFFFFFFFF) {
        const uint32_t depth = gGgpoLiveF - gGgpoLoadedS;  // frames rolled back by this load
        gOvLastDepth = (uint16_t)(depth < 0xFFFF ? depth : 0xFFFF);
        gOvDepthGf[gGgpoLiveF % 60] = gGgpoLiveF;
        gOvDepth[gGgpoLiveF % 60] = depth;
        FramePace::Rollback(depth);
        Rollback::gIsResimulating = false;
        Rumble::EndResim();
        SoundDedupe::EndResim(gGgpoLoadedS, gGgpoLiveF);
        HostFrame() = hostAtF;
        HeapAudit("post-resim");
    ListAudit("post-resim");
    }
    CsPump(gGgpoLiveF);
    SteamBridge::Pump();
    Input::SetDeferSampling(gLateSample && gGgpoRunning);  // the next pass samples at its boundary
    RenderTag::SetStamp(gSyncValid && gGgpoFrameOpen && gP2PFrames >= gGgpoDelay ? sampledAt[(gP2PFrames - gGgpoDelay) & 63] : 0);
    // One verdict stops the session and all of them are cleared with it: a second one raised in the same boundary (a
    // different environment usually comes with a different frame 0) stayed set and stopped the next session.
    const bool envMismatch = gEnvMismatch, startMismatch = gStartMismatch, desyncStop = gDesyncStop, peerLost = gPeerLost;
    gEnvMismatch = gStartMismatch = gDesyncStop = gPeerLost = false;
    if (envMismatch) {
        GgpoStop("environment mismatch with the peer (see NETPLAY ENVIRONMENT MISMATCH)");
    } else if (startMismatch) {
        GgpoStop("the peers started the fight from different states (frame 0 differs)");
    } else if (desyncStop) {
        GgpoStop("the two games went out of sync");
    } else if (peerLost) {
        gNetplayArmed = false;  // the next fight would wait for a peer that is gone
        // The other game counts the same differing frames and may reach the limit a few frames earlier: its bye
        // (GgpoStop) then arrives before this game's own stop. Both screens give the same reason.
        const bool desync = gDesyncStopFrames > 0 && gCsMismatchRun >= gDesyncStopFrames / 2 && Online::Engaged();
        if (desync)
            Logger::Info("NETPLAY: the two games went out of sync (%d compared frames in a row differ; the other game "
                         "stopped its session first): the rollback session is stopped", gCsMismatchRun);
        else
            Logger::Info("NETPLAY: the other player disconnected at gf=%u; the session is stopped and netplay disarmed "
                         "(the fight continues locally)", GameFrame());
        GgpoStop(desync ? "the two games went out of sync" : "the peer disconnected");
    }
}

bool GgpoStart(int checkDistance) {
    GgpoStop("restart");
    if (!gRollbackModules) {
        Logger::Info("GGPO synctest: needs the rollback patch set ('netcode rollback' or 'rbmode 1' outside lockstep); not started");
        return false;
    }
    GGPOSessionCallbacks cb{};
    cb.begin_game = GgpoBeginGame;
    cb.save_game_state = GgpoSaveState;
    cb.load_game_state = GgpoLoadState;
    cb.log_game_state = GgpoLogState;
    cb.free_buffer = GgpoFreeBuffer;
    cb.advance_frame = GgpoAdvanceFrame;
    cb.on_event = GgpoOnEvent;
    char name[] = "MK9Rollback";
    if (ggpo_start_synctest(&gGgpo, &cb, name, 2, sizeof(uint32_t), checkDistance) != GGPO_OK) {
        Logger::Info("GGPO synctest could not start");
        gGgpo = nullptr;
        return false;
    }
    for (int p = 0; p < 2; ++p) {
        GGPOPlayer pl{};
        pl.size = sizeof pl;
        pl.type = GGPO_PLAYERTYPE_LOCAL;
        pl.player_num = p + 1;
        ggpo_add_player(gGgpo, &pl, &gGgpoPlayer[p]);
    }
    gGgpoLoads = gGgpoReplayed = gGgpoSyncErrors = gGgpoSaveFails = gGgpoDesyncLogged = 0;
    gGgpoFrameOpen = false;
    ggpo_idle(gGgpo, 0);  // SyncTest accepts inputs once it is 'running'
    Logger::Info("GGPO synctest started at gf=%u (check distance %d)", GameFrame(), checkDistance);
    return true;
}

// WorldTick entry of live frame F: close frame F-1 (GGPO saves F, and SyncTest may replay).
void GgpoBoundary(void* engine, float dt) {
    if (!gGgpo || !gGgpoFrameOpen) return;
    gGgpoEngine = engine;
    gGgpoDt = dt;
    gGgpoLiveF = GameFrame();
    gGgpoLoadedS = 0xFFFFFFFF;
    const uint32_t hostAtF = HostFrame();
    ggpo_idle(gGgpo, 0);
    ggpo_advance_frame(gGgpo);
    gGgpoFrameOpen = false;
    if (gGgpoLoadedS != 0xFFFFFFFF) {
        const uint32_t depth = gGgpoLiveF - gGgpoLoadedS;  // frames rolled back by this load
        gOvLastDepth = (uint16_t)(depth < 0xFFFF ? depth : 0xFFFF);
        gOvDepthGf[gGgpoLiveF % 60] = gGgpoLiveF;
        gOvDepth[gGgpoLiveF % 60] = depth;
        Rollback::gIsResimulating = false;
        Rumble::EndResim();
        SoundDedupe::EndResim(gGgpoLoadedS, gGgpoLiveF);
        HostFrame() = hostAtF;
        HeapAudit("post-resim");
    ListAudit("post-resim");
        if (GameFrame() != gGgpoLiveF)
            Logger::Info("GGPO: replay ended at gf=%u, live frame is %u", GameFrame(), gGgpoLiveF);
    }
}
}  // namespace

extern "C" void MK9R_GgpoSyncError(const char* msg) {
    if (gAbToggle) ++gAbErrors[gAbParity];
    ++gGgpoSyncErrors;
    Logger::Info("GGPO SYNC ERROR: %s", msg);
}
// Patched GGPO's Platform::AssertFailed (tools/build_ggpo.ps1): GGPO calls exit(0) right after it; its message box would
// open behind the fullscreen game first and leave the screen frozen until dismissed.
extern "C" void MK9R_GgpoAssert(const char* msg) {
    Logger::TryInfo("GGPO ASSERTION FAILED: %s; closing the game", msg);
    TerminateProcess(GetCurrentProcess(), 0xC0000409);
}

namespace Rollback {

bool gIsResimulating = false;
bool gDiag = false;  // 'lab 1' / 'diag 1'
void SetAnimTagFix(bool on) { gAnimTagFix = on; }
void SetTwice(bool on) { gTwice = on; }
void SetResimSleep(int ms) { gResimSleep = ms; }
void SetLazySave(bool on) {
    gLazySave = on;
    Logger::Info("LAZYSAVE %d: the P2P live save %s", on ? 1 : 0, on ? "waits for the boundary's first poll" : "is taken at tick entry");
}
void SetSkipConfirmed(bool on) {
    gSkipConfirmed = on;
    Logger::Info("SKIPCONFIRMED %d: replays %s", on ? 1 : 0, on ? "skip the saves of frames no rollback can load" : "save every frame");
}
void SetSparseSave(bool on) {
    gSparseSave = on;
    Logger::Info("SPARSESAVE %d: P2P replays %s", on ? 1 : 0,
                 on ? "skip the saves of odd frames (a rollback to one loads the frame before)" : "save every loadable frame");
}
void SetSparseLive(bool on) {
    gSparseLive = on;
    Logger::Info("SPARSELIVE %d: P2P live saves of odd frames are %s", on ? 1 : 0, on ? "skipped (with sparsesave on)" : "taken");
}
void SetQuarantineMargin(int frames) {
    gQuarantineMargin = frames < 0 ? 0 : frames > 60 ? 60 : frames;
    Logger::Info("QMARGIN %d: freed blocks stay quarantined %d frame(s) beyond the snapshot ring", gQuarantineMargin, gQuarantineMargin);
}
void SetBalance(bool on) {
    gBalance = on;
    Logger::Info("BALANCE %d: %s", on ? 1 : 0, on ? "a game >= 1 frame ahead of its peer waits for it" : "GGPO's time sync only");
}
void SetBalanceTuning(int settleFrames, int samples, int maxGive) {
    gBalanceSettle = settleFrames < 0 ? 0 : settleFrames > 300 ? 300 : settleFrames;
    gBalanceSamples = samples < 1 ? 1 : samples > 30 ? 30 : samples;
    gBalanceMaxGive = maxGive < 1 ? 1 : maxGive > 6 ? 6 : maxGive;
    Logger::Info("BALANCECFG: settle %d frames after paying, decide after %d samples (every 10 frames), give at most %d frame(s)",
                 gBalanceSettle, gBalanceSamples, gBalanceMaxGive);
}
// The frame limiter marks a frame late (0x00E844A5 = 1) when it starts more than 0.05 of a period (0.83 ms) after its
// deadline (mulsd [0x00C5A6B8] at 0x004026B9), and a late frame is not rendered (0x00666DEE, with the flag 0xDECAFC = 1).
// A game thread near its budget was slightly late on almost every frame: cpu1 spinB10 (2026-11-04, game B with 10 ms of
// extra CPU per frame) simulated 59.4 frames/s but presented 6-46/s. 'renderlate N': the threshold in percent of a
// period (5 = stock); the instruction reads gRenderLateFactor instead of the constant. Default 50 since rl50b
// (2026-11-04): ns60_d2 presents >= 20 ms 81/92 -> 32/34 per minute and frames >= 20 ms 107/133 -> 49/47 (A/B), ns30_d2
// similar or better, simulation unchanged; an overloaded game (spinB10) presents 23 -> 36-44/s but with more gaps >= 50 ms
// (31 -> 65-117 per minute).
double gRenderLateFactor = 0.50;
bool gRenderLatePatched = false;
void InstallRenderLate() {
    if (gRenderLatePatched) return;
    static const uint8_t orig[8] = {0xF2, 0x0F, 0x59, 0x05, 0xB8, 0xA6, 0xC5, 0x00};  // mulsd xmm0, [0x00C5A6B8]
    uint8_t code[8] = {0xF2, 0x0F, 0x59, 0x05, 0, 0, 0, 0};
    const uint32_t a = (uint32_t)reinterpret_cast<uintptr_t>(&gRenderLateFactor);
    memcpy(code + 4, &a, 4);
    gRenderLatePatched = CodePatch::Write(0x004026B9, orig, code, 8, "frame limiter late threshold 0x004026B9 (renderlate)");
    if (!gRenderLatePatched) Logger::Info("RENDERLATE: 0x004026B9 not as expected; the stock 5%% threshold stays");
}
void SetRenderLate(int pct) {
    pct = pct < 1 ? 1 : pct > 100 ? 100 : pct;
    InstallRenderLate();
    gRenderLateFactor = pct / 100.0;
    Logger::Info("RENDERLATE %d: a frame that starts more than %d%% of a period after its deadline is not rendered%s", pct, pct,
                 gRenderLatePatched ? "" : " (patch not applied)");
}
void SetPaySmooth(int ms) {
    gPaySmoothMs = ms < 0 ? 0 : ms > 8 ? 8 : ms;
    Logger::Info("PAYSMOOTH %d: %s", gPaySmoothMs,
                 gPaySmoothMs ? "frames owed to the peer are paid by moving the frame limiter's deadline (ms per frame)"
                              : "frames owed to the peer are paid by 16 ms waits");
}
void SetPaySmoothSmall(int ms) {
    gPaySmoothSmallMs = ms < 0 ? 0 : ms > 8 ? 8 : ms;
    Logger::Info("PAYSMOOTH small debts (up to 2 frames): %d ms per frame (0: as larger ones)", gPaySmoothSmallMs);
}
void SetGgpoTimesync(bool on) {
    gGgpoTimesync = on;
    Logger::Info("GGPOSYNC %d: GGPO's own time-sync requests are %s", on ? 1 : 0,
                 on ? "paid (as before rc25)" : "only counted while BALANCE is on");
}
void SetRepoll(bool on) {
    gRepoll = on;
    Logger::Info("REPOLL %d: %s", on ? 1 : 0,
                 on ? "a live save made before the frame's remote input was in is followed by one more poll" : "no extra poll");
}
void SetPhaseSync(bool on) {
    gPhaseSync = on;
    if (!on) gPhaseDueMs = 0;
    Logger::Info("PHASESYNC %d: sub-frame phase alignment with the other game %s", on ? 1 : 0, on ? "on" : "off");
}
void SetQualityInterval(int ms) {
    ggpo_quality_report_interval = ms < 50 ? 50 : ms > 1000 ? 1000 : ms;
    Logger::Info("QUALITYMS %d: GGPO sends its frame advantage and ping every %d ms", ggpo_quality_report_interval,
                 ggpo_quality_report_interval);
}
void SetRunningRetry(int ms) {
    ggpo_running_retry_interval = ms < 17 ? 17 : ms > 1000 ? 1000 : ms;
    Logger::Info("NETRETRY %d: GGPO resends the unacknowledged inputs after %d ms without a new input frame from the peer (stock 200)",
                 ggpo_running_retry_interval, ggpo_running_retry_interval);
}
void SetFirstInputRetry(int ms) {
    ggpo_first_input_retry_ms = ms < 0 ? 0 : ms > 34 ? 34 : ms;
    if (ggpo_first_input_retry_ms) {
        Logger::Info("FIRSTRETRY %d: until the other game's first input arrives, GGPO sends this game's inputs again every %d ms",
                     ggpo_first_input_retry_ms, ggpo_first_input_retry_ms + 1);
    } else {
        Logger::Info("FIRSTRETRY 0: the first inputs are sent again only after 'netretry' (rc27)");
    }
}
void SetCanonClock(bool on) {
    gCanonClock = on;
    Logger::Info("CANONCLOCK %d: the world clocks %s at the netplay start", on ? 1 : 0, on ? "are set to the same value" : "are left as they are");
}
void SetFastReadable(bool on) {
    gFastReadable = on;
    Logger::Info("FASTREADABLE %d: the rollback's readability checks use %s", on ? 1 : 0,
                 on ? "guarded reads (one per page)" : "VirtualQuery");
}
void SetSkipLive(bool on) {
    gSkipLive = on;
    Logger::Info("SKIPLIVE %d: live saves of frames whose remote input is already in are %s", on ? 1 : 0, on ? "skipped" : "taken");
}
void SetLateSample(bool on) {
    gLateSample = on;
    if (!on) Input::SetDeferSampling(false);
    Logger::Info("LATESAMPLE %d: controllers sampled %s", on ? 1 : 0,
                 on ? "at the P2P boundary's start (after MK9's frame wait)" : "at the main loop's call (before MK9's frame wait)");
}
void SetEarlyInput(bool on) {
    gEarlyInput = on;
    Logger::Info("EARLYINPUT %d: the local input is added %s", on ? 1 : 0,
                 on ? "at the boundary's start, before the poll and the live save" : "after the poll and the live save");
}
void SetDesyncStop(int frames) {
    gDesyncStopFrames = frames < 0 ? 0 : frames;
    if (gDesyncStopFrames) Logger::Info("DESYNCSTOP: an online match's rollback session stops after %d compared frames in a row differ", gDesyncStopFrames);
    else Logger::Info("DESYNCSTOP off: checksum differences are only logged (rc27)");
}
void SetHandshakePoll(bool on) {
    gHandshakePoll = on;
    Logger::Info("HANDSHAKEPOLL %s", on ? "on: the paused wait for the session polls every millisecond"
                                        : "off: one poll a frame until the session runs (rc27)");
}
void SetPhaseGrowAfter(int decisions) {
    gPhaseGrowAfter = decisions < 1 ? 1 : decisions > 8 ? 8 : decisions;
    Logger::Info("PHASEGROW %d: the phase search's step grows from decision %d in a row with the same leader%s", gPhaseGrowAfter,
                 gPhaseGrowAfter + 1, gPhaseGrowAfter == 1 ? " (rc27)" : "");
}
void SetWaitShift(bool on) {
    gWaitShift = on;
    Logger::Info("WAITSHIFT %s", on ? "on: a boundary that waited for the peer moves the frame deadline by the wait (whole frames)"
                                    : "off: the limiter catches up after a wait (rc27)");
}
void SetStartSync(bool on) {
    gStartSync = on;
    Logger::Info("STARTSYNC %s", on ? "on: at a session's start the two games measure how far apart their frames start and the "
                                      "earlier one moves by that much"
                                    : "off: the start barrier's wait and the handshake's round trip decide, in whole frames (rc29)");
}
void SetStartBarrier(bool on) {
    gStartBarrier = on;
    Logger::Info("STARTBARRIER %d: rollback frame 0 %s", on ? 1 : 0, on ? "waits for the peer's first input" : "runs at once");
}
void SetTickSpin(int ms) {
    gTickSpinMs = ms < 0 ? 0 : ms > 30 ? 30 : ms;
    Logger::Info("TICKSPIN %d ms of CPU per live and replayed session frame (test: a slower PC)", gTickSpinMs);
}
void SetStallHold(int minMs, int maxMs) {
    gStallHold = minMs > 0 && maxMs > 0;
    if (gStallHold) {
        gStallHoldMinMs = minMs;
        gStallHoldMaxMs = maxMs > 500 ? 500 : maxMs;
    }
    Logger::Info("STALLHOLD %s", gStallHold ? "on" : "off");
    if (gStallHold) Logger::Info("STALLHOLD: after a wait of >= %d ms for the peer, up to %d ms more until the frame gap is normal",
                                 gStallHoldMinMs, gStallHoldMaxMs);
}
void SetXrayLock(int on, int cine, int window, int freeze) {
    if (on >= 0) gXrayLock = on != 0;
    if (cine >= 0) gXrayLockCine = cine != 0;
    if (window > 0) gXrayLockWindow = window > 600 ? 600 : window;
    if (freeze >= 0) gXrayFreeze = freeze != 0;
    Logger::Info("XRAYLOCK %s: up to %d frames after a flip-stance press with a full meter, until the X-ray cinematic + delay + 4; "
                 "input %s in the X-ray cinematic%s",
                 gXrayLock ? "on" : "off", gXrayLockWindow, gXrayFreeze ? "held" : "live",
                 gXrayLockCine ? "; also locked while a fighter holds any cinematic controller" : "");
}
void SetFreeze(int ms, int everyMs) {
    gFreezeMs = ms < 0 ? 0 : ms > 3000 ? 3000 : ms;
    gFreezeEveryMs = everyMs < 1000 ? 0 : everyMs;
    gFreezeNext = 0;
    Logger::Info("FREEZEEVERY: %s", gFreezeMs && gFreezeEveryMs ? "this game stops for a while in each P2P session (test)" : "off");
}
void SetTickParity(bool on) { gTickedFromWorld = on; }
void SetDeferOnlyGgpo(bool on) { gDeferOnlyGgpo = on; }
void SetIdleSaves(bool on) {
    gIdleSaves = on;
    Logger::Info("IDLESAVES %d: rollback mode snapshots %s", on ? 1 : 0,
                 on ? "every tick, also without a session (old behaviour)" : "only in a session or at its start");
}
void SetCompSync(bool on) { gCompSync = on; }
void SetAttachAll(bool on) { gAttachAll = on; }
void SetMkAttach(bool on) {
    gMkAttach = on;
    Logger::Info("MKATTACH %d: other MK objects' actors %s re-attached to their saved base at load", on ? 1 : 0, on ? "are" : "are not");
}
bool GgpoActive() { return gGgpo != nullptr; }
bool NetplayRunning() { return gGgpo && gGgpoIsP2P && gGgpoRunning; }
bool SetSessionDelay(int delay) { return SetSessionDelayImpl(delay); }
int SessionDelay() { return gGgpo && gGgpoIsP2P && gGgpoRunning ? gGgpoDelay : 0; }
bool NetplayStarting() { return gNetplayWaitRunning; }
bool NetplayArmed() { return gNetplayArmed; }
void NetplayDisarm() {
    if (!gNetplayArmed && !gNetplayWaitRunning) return;
    const bool waiting = gNetplayWaitRunning, awaiting = gNetplayAwaitFight;
    gNetplayArmed = gNetplayAwaitFight = gNetplayWaitRunning = gNetplaySeedPin = gP2PPending = false;
    if (waiting) {
        GgpoStop("netplay disarmed");
        CommandChannel::RequestPause(0);
    }
    // Armed in a fight, RecordInputs held both pads neutral for the session's start; nothing releases them without one.
    if ((waiting || awaiting) && !gGgpo) Input::ReleaseAll();
    Logger::Info("NETPLAY disarmed");
}
void NetplayStop(const char* why) {
    if (gGgpo && gGgpoIsP2P) GgpoStop(why);
}
void SetForceLod(bool on) { gForceLod = on; }
// Bone state at FightUpdatePlayers entry (where the scripts' hit checks read bone positions, 0x8B0CD0),
// live and resimulated passes per frame: does a desync start before the fight update or inside it?
namespace {
struct BoneHash {
    uint32_t gf;
    uint32_t h[2][6];  // per fighter: component body, atoms, three matrix arrays, actor
};
BoneHash gBoneHash[3][2][128];  // [FightUpdate entry, EngineTick exit, FightUpdate exit][live, resim][gf % 128]
uint8_t gBoneComp[3][2][128][2][0x700];  // component bytes at the same points
uint8_t gBoneActor[3][2][128][2][0x300];  // fighter actor bytes at the same points
void CaptureBones(int at);
}  // namespace
// Deep diff ('deepdiff 1'): every 16th live frame copies the MK master image at FightUpdate entry and EngineTick
// exit; when a replay runs that frame, the same points are diffed and the differing dwords grouped by MK object.
namespace {
bool gDeepDiff = false, gDeepEvery = false;
struct DeepCopy {
    uint32_t gf = 0xFFFFFFFF;
    std::vector<std::pair<MKHeap::Region, std::vector<uint8_t>>> ranges;
    size_t firstX = 0;
};
DeepCopy gDeep[3];  // [FightUpdate entry, EngineTick exit, FightUpdate exit]
int gDeepReports = 0;
struct DeepWatch {
    unsigned tid;
    uintptr_t addr;
} gDeepWatch;
DWORD WINAPI DeepTraceThread(LPVOID p) {
    struct Arg {
        unsigned tid;
        uintptr_t addrs[4];
    };
    const Arg* a = static_cast<const Arg*>(p);
    Logger::Info("DEEPDIFF trace armed: %s", CrashLog::TraceArm(a->tid, a->addrs) ? "ok" : "failed");
    return 0;
}
DWORD WINAPI DeepWatchThread(LPVOID p) {
    const DeepWatch* x = static_cast<const DeepWatch*>(p);
    Logger::Info("DEEPDIFF watch armed: %s", CrashLog::WatchWrite(x->tid, x->addr) ? "ok" : "failed");
    return 0;
}
void DeepCapture(int at) {
    if (!gDeepDiff || !gEnabled) return;
    const uint32_t gf = GameFrame();
    MKHeap::Region mr[8];
    const int nmr = MKHeap::MasterRanges(mr, 8);
    DeepCopy& d = gDeep[at];
    if (!gIsResimulating) {
        // One frame in 16, in phase with the loads (SyncTest loads every 4th frame) so that it is replayed as the
        // first frame after a load.
        if (gf % 4 != gLastLoadS % 4 || (!gDeepEvery && (gf / 4) % 4)) return;
        d.gf = gf;
        d.ranges.clear();
        for (int i = 0; i < nmr; ++i)
            d.ranges.push_back({mr[i], std::vector<uint8_t>(reinterpret_cast<const uint8_t*>(mr[i].base),
                                                            reinterpret_cast<const uint8_t*>(mr[i].base) + mr[i].size)});
        d.firstX = d.ranges.size();  // the anim nodes' / channels' GMalloc blocks (xblocks) follow
        static std::vector<std::pair<uintptr_t, uint32_t>> xb;
        Snapshot::ExternalBlocks(xb);
        for (const auto& b : xb)
            if (Readable(b.first, b.second))
                d.ranges.push_back({{b.first, b.second}, std::vector<uint8_t>(reinterpret_cast<const uint8_t*>(b.first),
                                                                            reinterpret_cast<const uint8_t*>(b.first) + b.second)});
        return;
    }
    if (d.gf != gf || gDeepReports >= (gDeepEvery ? 120 : 24)) return;
    // Only the first frame after a load: later replayed frames inherit whatever diverged before them.
    if (gf != gLastLoadS) return;
    if (at == 1) d.gf = 0xFFFFFFFF;  // once per live copy (EngineTick exit is the last point of the frame)
    // Live MK objects, sorted, to name differing addresses (object start .. next object start).
    static std::vector<std::pair<uint32_t, const char*>> objs;
    objs.clear();
    ObjectHooks::ForEachLive([](uint32_t o, const char* t, void*) { objs.push_back({o, t}); }, nullptr);
    std::sort(objs.begin(), objs.end());
    std::map<std::pair<uint32_t, const char*>, std::pair<int, std::string>> per;  // object -> count, first offsets
    int total = 0, xTotal = 0, xBlocks = 0;
    static std::vector<std::pair<uintptr_t, uint32_t>> xnow;
    Snapshot::ExternalBlocks(xnow);
    for (size_t ri = 0; ri < d.ranges.size(); ++ri) {
        const auto& r = d.ranges[ri];
        if (!Readable(r.first.base, r.first.size)) continue;
        const bool isX = ri >= d.firstX;
        if (isX) {  // compare an xblock only if the replay still uses the same block
            bool same = false;
            for (const auto& b : xnow) same |= b.first == r.first.base && b.second == r.first.size;
            if (!same) continue;
            ++xBlocks;
        }
        const uint32_t* now = reinterpret_cast<const uint32_t*>(r.first.base);
        const uint32_t* was = reinterpret_cast<const uint32_t*>(r.second.data());
        for (uint32_t k = 0; k < r.first.size / 4; ++k) {
            if (now[k] == was[k]) continue;
            ++total;
            if (isX) ++xTotal;
            const uint32_t a = (uint32_t)r.first.base + k * 4;
            auto it = std::upper_bound(objs.begin(), objs.end(), std::make_pair(a, (const char*)~0u));
            std::pair<uint32_t, const char*> key{0, "(no object)"};
            if (it != objs.begin() && a - std::prev(it)->first < 0xD000) key = *std::prev(it);
            auto& e = per[key];
            if (e.first++ < 4) {
                char b[48];
                snprintf(b, sizeof b, " +%X:%08X>%08X", key.first ? a - key.first : a, was[k], now[k]);
                e.second += b;
            }
        }
    }
    // 'deepdiff 2' samples every load's first frame; only diffs beyond the known between-tick counters are reported.
    if (gDeepEvery && (total < 8 || at == 1)) return;  // EngineTick exit: per-frame heap pointers in MK anim nodes
    ++gDeepReports;
    Logger::Info("DEEPDIFF gf=%u at %s (replay vs live): %d dwords differ in %u objects (%d of them in %d compared xblocks)", gf,
                 at == 2 ? "FightUpdate exit" : at ? "EngineTick exit" : "FightUpdate entry", total, (unsigned)per.size(), xTotal,
                 xBlocks);
    // First differing dword outside MK objects at FightUpdate entry: watch its writers (once per session).
    static bool armed = false;
    if (at == 0 && !armed) {
        for (const auto& r : d.ranges) {
            const uint32_t* now = reinterpret_cast<const uint32_t*>(r.first.base);
            const uint32_t* was = reinterpret_cast<const uint32_t*>(r.second.data());
            for (uint32_t k = 0; k < r.first.size / 4 && !armed; ++k) {
                if (now[k] == was[k]) continue;
                const uint32_t a = (uint32_t)r.first.base + k * 4;
                // Float-like values only (the millisecond stamps written by the main loop's 0x0081C7C0 are known).
                if ((was[k] >> 24) < 0x38 || (was[k] >> 24) > 0x46) continue;
                armed = true;
                gDeepWatch = {GetCurrentThreadId(), a};
                Logger::Info("DEEPDIFF watching writes to %08X (%08X live, %08X replay)", a, was[k], now[k]);
                HANDLE h = CreateThread(nullptr, 0, DeepWatchThread, &gDeepWatch, 0, nullptr);
                if (h) CloseHandle(h);
            }
            if (armed) break;
        }
    }
    if (at == 1 && total) CrashLog::TraceDump(gf);  // traced writes of this frame, live vs replay (traceanimpos)
    // First float-like dword that differs at EngineTick exit: trace its writes (live vs replay, with callers) from now
    // on; later first-frame comparisons dump them (once per session).
    static bool traced = false;
    if (at == 1 && total && !traced) {
        for (size_t ri = 0; ri < d.firstX && !traced; ++ri) {
            const auto& r = d.ranges[ri];
            const uint32_t* now = reinterpret_cast<const uint32_t*>(r.first.base);
            const uint32_t* was = reinterpret_cast<const uint32_t*>(r.second.data());
            for (uint32_t k = 0; k < r.first.size / 4; ++k) {
                if (now[k] == was[k] || (was[k] >> 24) < 0x40 || (was[k] >> 24) > 0x45) continue;
                traced = true;
                static struct {
                    unsigned tid;
                    uintptr_t addrs[4];
                } targ;
                targ.tid = GetCurrentThreadId();
                targ.addrs[0] = (uint32_t)r.first.base + k * 4;
                targ.addrs[1] = targ.addrs[2] = targ.addrs[3] = 0;
                Logger::Info("DEEPDIFF tracing writes to %08X (%08X live, %08X replay)", (unsigned)targ.addrs[0], was[k], now[k]);
                HANDLE h = CreateThread(nullptr, 0, DeepTraceThread, &targ, 0, nullptr);
                if (h) CloseHandle(h);
                break;
            }
        }
    }
    int shown = 0;
    for (const auto& [k, v] : per) {
        if (shown++ >= 14) break;
        char tn[48];
        snprintf(tn, sizeof tn, "%.40s", k.second ? k.second : "?");
        Logger::Info("DEEPDIFF   %08X %s: %d%s", k.first, tn, v.first, v.second.c_str());
    }
}
}  // namespace
void SetDeepDiff(bool on) { gDeepDiff = on; }
void SetDeepEvery(bool on) { gDeepEvery = on; gDeepDiff = gDeepDiff || on; }
// 0x0042AF1D: the object heap's free tree is intact at tick entry and corrupt within the same frame; auditing at the
// phase boundaries names the phase that breaks it ('phaseaudit 0|1'). Only on ticks that snapshot and in replays: run on
// every rollback-mode tick, they slowed a fight's load ~10x and the fight was drawn almost black (2026-10-31).
bool gPhaseAudit = true;
void SetPhaseAudit(bool on) { gPhaseAudit = on; }
void PhaseAudit(const char* where) {
    if (!gPhaseAudit || !gEnabled || gLockstep || !(gTickAudits || gIsResimulating)) return;
    const std::string w = gIsResimulating ? std::string("resim ") + where : std::string(where);
    HeapAudit(w.c_str());
    ListAudit(w.c_str());
}
// The fighters' pad records (PlayerInfo +0xB40 -> 0x60 bytes: cur/prev masks, press/release rings) at FightUpdate
// entry, live vs every replay of the same frame: a replayed frame whose MK state jumped (rmimg2 2026-10-18: 0 ->
// 250+ differing dwords within one frame) with an input applied differently would show here ('padtrace 0|1').
bool gPadTrace = true;
void PadTrace() {
    if (!gPadTrace || !gEnabled || !Lab::On()) return;
    struct PadRec {
        uint32_t gf = 0xFFFFFFFF;
        uint8_t b[2][0x60];
    };
    static PadRec sLive[64];
    static uint32_t sCmp = 0, sDiff = 0;
    const uint32_t gf = GameFrame();
    uint8_t now[2][0x60] = {};
    for (int p = 0; p < 2; ++p) {
        const uintptr_t info = 0x00E89D7C + p * 0xC8C;
        const uint32_t pad = *reinterpret_cast<const uint32_t*>(info + 0xB40);
        if (pad && Readable(pad, 0x60)) memcpy(now[p], reinterpret_cast<const void*>(pad), 0x60);
    }
    PadRec& r = sLive[gf % 64];
    if (!gIsResimulating) {
        r.gf = gf;
        memcpy(r.b, now, sizeof now);
        return;
    }
    if (r.gf != gf) return;
    ++sCmp;
    for (int p = 0; p < 2; ++p)
        if (memcmp(r.b[p], now[p], 0x60)) {
            ++sDiff;
            static int logs = 0;
            if (logs++ < 40) {
                char line[300] = "";
                int n = 0;
                for (uint32_t o = 0; o < 0x60 && n < 260; o += 4) {
                    uint32_t a, b;
                    memcpy(&a, r.b[p] + o, 4);
                    memcpy(&b, now[p] + o, 4);
                    if (a != b) n = AppendF(line, sizeof line, n, " +%X:%08X>%08X", o, a, b);
                }
                Logger::Info("PADTRACE gf=%u P%d pad differs at FightUpdate entry (S=%u):%s", gf, p + 1, gLastLoadS, line);
            }
        }
    if (sCmp % 1000 == 0) Logger::Info("PADTRACE %u replayed frames compared, %u fighter pads differed", sCmp, sDiff);
}
void OnFightUpdateEntry() {
    PadTrace();
    AnimInputTrace::ImagePoint(0);
    PhaseAudit("FightUpdate entry");
    CaptureBones(0);
    DeepCapture(0);
}
void OnEngineTickExit() {
    PhaseAudit("EngineTick exit");
    CaptureBones(1);
    DeepCapture(1);
}
void OnFightUpdateExit() {
    AnimInputTrace::ImagePoint(1);
    PhaseAudit("FightUpdate exit");
    CaptureBones(2);
    DeepCapture(2);
}
namespace {
void CaptureBones(int at) {
    if (!gEnabled || !Lab::On()) return;  // read only by the SyncTest report (BoneHashDump)
    const uint32_t gf = GameFrame();
    BoneHash& b = gBoneHash[at][gIsResimulating ? 1 : 0][gf % 128];
    b.gf = gf;
    memset(b.h, 0, sizeof b.h);
    static const uint32_t kArr[4][2] = {{0x2C8, 40}, {0x2F4, 64}, {0x300, 64}, {0x30C, 64}};
    for (int p = 0; p < 2; ++p) {
        const uint32_t obj = *reinterpret_cast<const uint32_t*>(0x00E89D7C + p * 0xC8C + 0x1C);
        if (!obj || obj == 0xDEADBEEF || !Readable(obj + 0x10C, 4)) continue;
        const uint32_t comp = *reinterpret_cast<const uint32_t*>(obj + 0x10C);
        if (!comp || !Readable(comp, 0x700)) continue;
        b.h[p][0] = StateHash::XXH32(reinterpret_cast<const void*>(comp + 0x3C), 0x700 - 0x3C, 0);
        memcpy(gBoneComp[at][gIsResimulating ? 1 : 0][gf % 128][p], reinterpret_cast<const void*>(comp), 0x700);
        const uint32_t actor = *reinterpret_cast<const uint32_t*>(obj + 8);
        if (actor && Readable(actor, kActorBytes)) {
            memcpy(gBoneActor[at][gIsResimulating ? 1 : 0][gf % 128][p], reinterpret_cast<const void*>(actor), kActorBytes);
            b.h[p][5] = StateHash::XXH32(reinterpret_cast<const void*>(actor + 0x3C), kActorBytes - 0x3C, 0);
        }
        for (int k = 0; k < 4; ++k) {
            const uint32_t data = *reinterpret_cast<const uint32_t*>(comp + kArr[k][0]);
            const uint32_t num = *reinterpret_cast<const uint32_t*>(comp + kArr[k][0] + 4);
            if (data && num && num < 1024 && Readable(data, num * kArr[k][1]))
                b.h[p][k + 1] = StateHash::XXH32(reinterpret_cast<const void*>(data), num * kArr[k][1], num);
        }
    }
}
}  // namespace
void BoneHashDump(uint32_t gf) {
  for (int at : {0, 2, 1}) {
    const BoneHash& a = gBoneHash[at][0][gf % 128];
    const BoneHash& r = gBoneHash[at][1][gf % 128];
    if (a.gf != gf || r.gf != gf) continue;
    static const char* kName[6] = {"comp", "atoms", "m0", "m1", "m2", "actor"};
    char line[300];
    int n = 0;
    for (int p = 0; p < 2; ++p)
        for (int k = 0; k < 6; ++k)
            if (a.h[p][k] != r.h[p][k]) n = AppendF(line, sizeof line, n, " P%d.%s", p + 1, kName[k]);
    line[n] = 0;
    Logger::Info("BONEHASH gf=%u at %s: %s", gf, at == 2 ? "FightUpdate exit" : at ? "EngineTick exit" : "FightUpdate entry", n ? line : "identical");
    for (int p = 0; p < 2; ++p) {
        if (a.h[p][5] == r.h[p][5]) continue;
        char d[600];
        int dn = 0, total = 0;
        for (uint32_t o = 0x3C; o < kActorBytes; o += 4) {
            uint32_t x, y;
            memcpy(&x, gBoneActor[at][0][gf % 128][p] + o, 4);
            memcpy(&y, gBoneActor[at][1][gf % 128][p] + o, 4);
            if (x == y) continue;
            if (total++ < 16) dn = AppendF(d, sizeof d, dn, " +%X:%08X>%08X", o, x, y);
        }
        d[dn] = 0;
        Logger::Info("BONEHASH   P%d actor %d dwords differ:%s", p + 1, total, d);
    }
    for (int p = 0; p < 2; ++p) {
        if (a.h[p][0] == r.h[p][0]) continue;
        char d[600];
        int dn = 0, total = 0;
        for (uint32_t o = 0x3C; o < 0x700; o += 4) {
            uint32_t x, y;
            memcpy(&x, gBoneComp[at][0][gf % 128][p] + o, 4);
            memcpy(&y, gBoneComp[at][1][gf % 128][p] + o, 4);
            if (x == y) continue;
            if (total++ < 16) dn = AppendF(d, sizeof d, dn, " +%X:%08X>%08X", o, x, y);
        }
        d[dn] = 0;
        Logger::Info("BONEHASH   P%d comp %d dwords differ:%s", p + 1, total, d);
    }
  }
}
// Memory-growth attribution: committed private memory per allocation base, diffed against the previous
// call. Logs the bases that grew most (heap segments marked) and the new bases by size.
namespace {
// Live UObjects per class (GObjObjects data [0x00E7C0EC], count [0x00E7C0F0]; Class at +0x30), diffed against the
// previous call: objects created in abandoned timelines that nothing destroys show up as steadily growing classes.
std::unordered_map<uint32_t, uint32_t> gPrevCensus;
void ObjectCensus() {
    uint32_t objs = 0, cnt = 0;
    if (!CrashLog::SafeRead32(0x00E7C0EC, &objs) || !CrashLog::SafeRead32(0x00E7C0F0, &cnt) || !objs || cnt > 0x400000) return;
    std::unordered_map<uint32_t, uint32_t> cur;
    uint32_t live = 0;
    for (uint32_t i = 0; i < cnt; ++i) {
        uint32_t o = 0, cls = 0;
        if (!CrashLog::SafeRead32(objs + 4 * i, &o) || !o) continue;
        if (!CrashLog::SafeRead32(o + 0x30, &cls)) continue;
        ++cur[cls];
        ++live;
    }
    if (!gPrevCensus.empty()) {
        std::vector<std::pair<int, uint32_t>> d;
        for (auto& [c, k] : cur) {
            auto it = gPrevCensus.find(c);
            const int delta = (int)k - (it == gPrevCensus.end() ? 0 : (int)it->second);
            if (delta) d.push_back({delta, c});
        }
        std::sort(d.begin(), d.end(), [](auto& x, auto& y) { return x.first > y.first; });
        char line[700];
        int n = 0;
        for (size_t i = 0; i < d.size() && i < 10 && d[i].first > 0; ++i) {
            char nm[64];
            Ue3Names::NameOf(d[i].second, nm, sizeof nm);
            n = AppendF(line, sizeof line, n, " %s+%d(%u)", nm, d[i].first, cur[d[i].second]);
        }
        line[n] = 0;
        Logger::Info("CENSUS: %u live UObjects (array %u); grew:%s", live, cnt, line);
    } else {
        Logger::Info("CENSUS: %u live UObjects (array %u)", live, cnt);
    }
    gPrevCensus.swap(cur);
}

std::map<uintptr_t, uint32_t> gPrevRegions;
void RegionDiff() {
    std::map<uintptr_t, uint32_t> cur;
    std::map<uintptr_t, uint32_t> curType;
    MEMORY_BASIC_INFORMATION mbi;
    for (uintptr_t a = 0x10000; a < 0xFFFF0000u && VirtualQuery(reinterpret_cast<void*>(a), &mbi, sizeof mbi);) {
        if (mbi.State == MEM_COMMIT && mbi.Type == MEM_PRIVATE) {
            cur[reinterpret_cast<uintptr_t>(mbi.AllocationBase)] += (uint32_t)mbi.RegionSize;
            curType[reinterpret_cast<uintptr_t>(mbi.AllocationBase)] = mbi.AllocationProtect << 16 | (mbi.Protect & 0xFFFF);
        }
        const uintptr_t next = reinterpret_cast<uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
        if (next <= a) break;
        a = next;
    }
    HANDLE heaps[64];
    const DWORD nh = GetProcessHeaps(64, heaps);
    auto heapIdx = [&](uintptr_t b) -> int {
        for (DWORD i = 0; i < nh && i < 64; ++i)
            if (reinterpret_cast<uintptr_t>(heaps[i]) == b) return (int)i;
        return -1;
    };
    if (!gPrevRegions.empty()) {
        std::vector<std::pair<int64_t, uintptr_t>> grew;
        int64_t newBytes = 0, oldGrowth = 0, gone = 0;
        uint32_t newCount = 0;
        std::map<uint32_t, uint32_t> newBySize;  // size -> count
        for (auto& [b, s] : cur) {
            auto it = gPrevRegions.find(b);
            if (it == gPrevRegions.end()) {
                newBytes += s;
                ++newCount;
                ++newBySize[s];
                grew.push_back({(int64_t)s, b});
            } else if (s != it->second) {
                oldGrowth += (int64_t)s - it->second;
                grew.push_back({(int64_t)s - it->second, b});
            }
        }
        for (auto& [b, s] : gPrevRegions)
            if (!cur.count(b)) gone += s;
        Logger::Info("REGIONS: %u bases; new %u (%lld KB), existing grew %lld KB, released %lld KB", (unsigned)cur.size(), newCount,
                     newBytes >> 10, oldGrowth >> 10, gone >> 10);
        std::sort(grew.begin(), grew.end(), [](auto& x, auto& y) { return x.first > y.first; });
        for (size_t i = 0; i < grew.size() && i < 8; ++i) {
            const uintptr_t b = grew[i].second;
            const bool isNew = !gPrevRegions.count(b);
            uint32_t w[4] = {};
            for (int k = 0; k < 4; ++k) CrashLog::SafeRead32(b + k * 4, &w[k]);
            Logger::Info("REGIONS   %08X %s %+lld KB (now %u KB)%s prot %08X first %08X %08X %08X %08X", (unsigned)b,
                         isNew ? "new" : "grew", grew[i].first >> 10, cur[b] >> 10, heapIdx(b) >= 0 ? " [process heap]" : "",
                         curType[b], w[0], w[1], w[2], w[3]);
        }
        std::vector<std::pair<uint32_t, uint32_t>> sizes(newBySize.begin(), newBySize.end());
        std::sort(sizes.begin(), sizes.end(), [](auto& x, auto& y) { return (uint64_t)x.first * x.second > (uint64_t)y.first * y.second; });
        char line[300];
        int n = 0;
        for (size_t i = 0; i < sizes.size() && i < 6; ++i)
            n = AppendF(line, sizeof line, n, " %uKBx%u", sizes[i].first >> 10, sizes[i].second);
        line[n] = 0;
        if (n) Logger::Info("REGIONS   new by size:%s", line);
    }
    gPrevRegions.swap(cur);
    AllocWatch::Report();
    ObjectCensus();
}
}  // namespace

// Between-tick probe: fighter component/actor/char bytes at EngineTick exit vs the next WorldTick entry. Offsets
// that change there are written outside the game tick (render, main loop) and are not reproduced by a replay.
namespace {
struct ProbeRegion {
    const char* name;
    uint32_t addr, size;
    std::vector<uint8_t> copy;
};
int gBetweenFrames = 0;
ProbeRegion gProbe[6];
std::map<std::pair<int, uint32_t>, std::pair<int, std::pair<uint32_t, uint32_t>>> gBetweenHits;
void ProbeTargets(ProbeRegion* r) {
    for (int p = 0; p < 2; ++p) {
        const uint32_t obj = *reinterpret_cast<const uint32_t*>(0x00E89D7C + p * 0xC8C + 0x1C);
        const bool ok = obj && obj != 0xDEADBEEF && Readable(obj, 0xC580);
        const uint32_t actor = ok ? *reinterpret_cast<const uint32_t*>(obj + 8) : 0;
        const uint32_t comp = ok ? *reinterpret_cast<const uint32_t*>(obj + 0x10C) : 0;
        r[p * 3 + 0] = {p ? "comp2" : "comp1", comp && Readable(comp, 0x800) ? comp : 0u, 0x800, {}};
        r[p * 3 + 1] = {p ? "actor2" : "actor1", actor && Readable(actor, kActorBytes) ? actor : 0u, kActorBytes, {}};
        r[p * 3 + 2] = {p ? "char2" : "char1", ok ? obj : 0u, 0xC580, {}};
    }
}
}  // namespace
void SetBetweenProbe(int frames) {
    gBetweenFrames = frames;
    gBetweenHits.clear();
    for (ProbeRegion& r : gProbe) r.addr = 0;
}
void BetweenProbeExit() {
    if (gBetweenFrames <= 0) return;
    ProbeTargets(gProbe);
    for (ProbeRegion& r : gProbe)
        if (r.addr) r.copy.assign(reinterpret_cast<const uint8_t*>(r.addr), reinterpret_cast<const uint8_t*>(r.addr) + r.size);
}
void BetweenProbeEntry() {
    if (gBetweenFrames <= 0) return;
    ProbeRegion now[6];
    ProbeTargets(now);
    for (int i = 0; i < 6; ++i) {
        if (!gProbe[i].addr || gProbe[i].addr != now[i].addr || gProbe[i].copy.size() != gProbe[i].size) continue;
        for (uint32_t o = 0; o < gProbe[i].size; o += 4) {
            uint32_t a, b;
            memcpy(&a, gProbe[i].copy.data() + o, 4);
            memcpy(&b, reinterpret_cast<const void*>(gProbe[i].addr + o), 4);
            if (a == b) continue;
            auto& h = gBetweenHits[{i, o}];
            ++h.first;
            h.second = {a, b};
        }
    }
    for (ProbeRegion& r : gProbe) r.addr = 0;
    if (--gBetweenFrames == 0) {
        Logger::Info("BETWEEN probe: %u offsets changed between EngineTick exit and the next WorldTick entry", (unsigned)gBetweenHits.size());
        int shown = 0;
        for (const auto& [k, v] : gBetweenHits)
            if (shown++ < 80)
                Logger::Info("BETWEEN   %s +0x%X: %d frames (last %08X -> %08X)", gProbe[k.first].name ? gProbe[k.first].name : "?", k.second,
                             v.first, v.second.first, v.second.second);
    }
}

void SetOriginalTicks(TickFn worldTick, TickFn engineTick) {
    gWorldTick = worldTick;
    gTick = engineTick;
}

// The patch sets per mode (docs/restructure_plan.md, phase 1b). Lockstep never restores a snapshot: it needs the game
// loop, the sampler and pause hooks and the cross-machine determinism patches (script clock, script RNG stream), none of
// the snapshot engine's tracking. Rollback adds object churn and free tracking, the deferrals and the replay guards;
// 'lab 1' adds the rigid-body watch (its events only refuse local test rollbacks; GGPO loads just log them).
bool RollbackWanted() { return !(gLockstepDeferred ? gLockstepNext : gLockstep); }
bool InstallRollbackModules() {
    if (gRollbackModules) return true;
    if (!ObjectHooks::Installed() && !ObjectHooks::Install()) {
        Logger::Info("ROLLBACK modules refused: object hooks could not be installed");
        return false;
    }
    if (!Ue3Refs::Install()) Logger::Info("ROLLBACK: UE3 reference tracking unavailable");
    if (!FreeQuarantine::Install()) {
        Logger::Info("ROLLBACK modules refused: free quarantine could not be installed");
        return false;
    }
    if (!RngTrace::Install()) Logger::Info("ROLLBACK: RNG trace unavailable");
    if (Lab::On() && !PhysWatch::Install()) Logger::Info("ROLLBACK: rigid-body watch unavailable");
    if (!SoundDedupe::Install()) Logger::Info("ROLLBACK: sound dedupe unavailable");
    Rumble::Install();
    ParticleDone::Install();
    HandleGuard::Install();
    ActorGuard::Install();
    ScriptLinkGuard::Install();
    InstallRenderLate();
    if (!RenderTag::Install()) Logger::Info("ROLLBACK: replayed frames stay drawn (replayskip unavailable)");
    // RayQueries: after RngTrace (chains to its lab tap). Uninstalled 2026-11-04 on a wrong premise (RayQueries.h); soak 14
    // (2026-11-06) showed every MK object line check answering one frame later through the report.
    if (!RayQueries::Install()) Logger::Info("ROLLBACK: MK object line-check tracking unavailable (rayqfix inactive)");
    if (!ActorDefer::Install()) Logger::Info("ROLLBACK: actor deferral unavailable (effect deaths will refuse rollbacks)");
    CinemaRestore::Install();
    gRollbackModules = true;
    return true;
}

// The virtual script clock (ScriptClock.h) outside sessions: only for lab tests and local rollbacks ('idlesaves 1').
// Players get MK9's real clock in the menus and loads: the virtual clock advances 1/60 s per GameFrame, and MK9's online
// stage-load wait ticks every 2.5-4.5 ms, so its script timers ran 4-7 times too fast and differently on each PC
// (2026-10-31 20:46: native 1182 'loaded' at gf 497, the give-up natives 1209/1217/1144 exactly 2400 frames = 40 virtual
// seconds later, after 6.1 real seconds; the match then disconnected). A session switches to the shared virtual clock
// at its start (AnchorCanonical) and back at the next timeline restart, as before.
bool VirtualClockOutsideSessions() { return Lab::On() || gIdleSaves; }

void Enable(bool on) {
    // Sent again while on (the launcher re-sends its mode on a checkbox change or a Play press): only the patch set may
    // grow. Re-anchoring the script clock to local time and clearing the ring would undo a running session's shared
    // start (RM ScriptClock::AnchorCanonical) and stop its next load.
    if (on && gEnabled) {
        // During a session the mode switch waits for its stop (SetLockstep), and so do the rollback modules: installing
        // them suspends every thread of the game several times, a hitch in the middle of a fight (GgpoStop installs them).
        if (gGgpo && gGgpoIsP2P && RollbackWanted() && !gRollbackModules)
            Logger::Info("ROLLBACK: the rollback patch set is installed when the running session stops");
        else if (RollbackWanted() && !InstallRollbackModules()) {
            Logger::Info("ROLLBACK: rollback sessions unavailable; staying in lockstep");
            SetLockstep(true);  // not advertised as rollback (hello) with every start refused
        }
        Logger::Info("ROLLBACK mode already on (%s patch set)", gRollbackModules ? "rollback" : "lockstep");
        return;
    }
    if (on && RollbackWanted() && !InstallRollbackModules()) {
        Logger::Info("ROLLBACK mode refused: the rollback modules could not be installed");
        on = false;
    }
    if (on) Input::InstallSamplerHook();
    // In Safe mode too (outside 'lab 1' this is the script native dispatch hook alone): the online mode learns from
    // native 1182 that this game has loaded the fight and holds the wait for the other player's load to 60 ticks per
    // second (Online::OnLoadedNative). It was part of the rollback patch set only, so a Safe-mode game still gave up
    // on a slower PC a few seconds after its own load, and its log had no ONLINEFLOW lines at all.
    if (on) RngTrace::Install();
    if (on) CommandChannel::InstallPauseGuard();
    if (on && ScriptClock::Install()) ScriptClock::Anchor(GameFrame());
    ScriptClock::SetActive(on && VirtualClockOutsideSessions());
    if (on) ScriptRng::Install();
    ScriptRng::SetActive(on && gScriptRngWanted);
    if (on) CrowdRng::Install();
    CrowdRng::SetActive(on && gCrowdRngWanted);
    gEnabled = on;
    if (!on) {  // on: per frame at the boundary, only while a session is armed or running
        FreeQuarantine::SetActive(false);
        gQuarantineOn = false;
    }
    ActorDefer::SetActive(on && !gLockstep);
    CinemaRestore::SetActive(on && !gLockstep);
    if (!on) {
        FreeQuarantine::ReleaseAll();
        ActorDefer::ReleaseAll();
        CinemaRestore::ReleaseAll();
    }
    for (bool& h : gHave) h = false;
    if (on) ObjectHooks::StartTracking(GameFrame());
    Logger::Info("ROLLBACK mode %s (ring %d, %s patch set%s)", on ? "ON" : "OFF", kRing, gRollbackModules ? "rollback" : "lockstep",
                 Lab::On() ? ", lab diagnostics" : "");
}
bool Enabled() { return gEnabled; }
void SetScriptRng(bool on) {
    gScriptRngWanted = on;
    if (on) ScriptRng::Install();
    ScriptRng::SetActive(gEnabled && on);
}
void SetCrowdRng(bool on) {
    gCrowdRngWanted = on;
    if (on && gEnabled) CrowdRng::Install();
    CrowdRng::SetActive(gEnabled && on);
    Logger::Info("CROWDRNG %d: background crowds draw %s", on ? 1 : 0, CrowdRng::Active() ? "their own stream" : "the gameplay RNG");
}

void SetLockstep(bool on) {
    if (gGgpo && gGgpoIsP2P) {  // a running session keeps its mode: the barrier, saves and GGPO's callbacks all read it
        gLockstepDeferred = on != gLockstep;
        gLockstepNext = on;
        Logger::Info("NETCODE mode: %s from the next session (a %s session is running)", on ? "LOCKSTEP" : "rollback",
                     gLockstep ? "LOCKSTEP" : "rollback");
        return;
    }
    gLockstepDeferred = false;
    gLockstep = on;
    // Nothing is ever restored in lockstep: frees go straight through and deaths are immediate.
    if (on) {  // off -> back on at the boundary once a session is armed
        FreeQuarantine::SetActive(false);
        gQuarantineOn = false;
        FreeQuarantine::ReleaseAll();
        ActorDefer::ReleaseAll();
        CinemaRestore::ReleaseAll();
    }
    ActorDefer::SetActive(gEnabled && !on);
    CinemaRestore::SetActive(gEnabled && !on);
    Logger::Info("NETCODE mode: %s (applies to the next p2p/netplay/steam session)", on ? "LOCKSTEP (delay-based, no rollback)" : "rollback");
}
bool Lockstep() { return gLockstep; }
void AbTest(const char* name) {
    if (gAbToggle) *gAbToggle = gAbSaved;
    gAbToggle = nullptr;
    struct T {
        const char* n;
        bool* p;
    } const ts[] = {{"complistrebase", &Snapshot::gCompListRebase}, {"bonerot", &Snapshot::gBoneRotation},
                    {"floatfields", &Snapshot::gFloatFields},       {"strictvt", &Snapshot::gStrictVtable},
                    {"foreignvt", &Snapshot::gForeignVtables},      {"namemaps", &Snapshot::gNameMaps},
                    {"animtagfix", &gAnimTagFix},   {"mkattach", &gMkAttach}, {"mkattrec", &gMkAttRec}, {"motionctl", &gMotionCtl}, {"mkmotion", &gMkMotion}, {"bindpins", &Snapshot::gBindPins}, {"cineslots", &gCineSlots}, {"motionrefresh", &gMotionRefresh}, {"cinema", &CinemaRestore::gRestore}, {"levelvis", &CinemaRestore::gLevelVis}, {"levelready", &CinemaRestore::gLevelReady}, {"kismet", &CinemaRestore::gKismet}, {"levelundo", &CinemaRestore::gLevelUndo}, {"levelredo", &CinemaRestore::gLevelRedo},{"cinepartial", &CinemaRestore::gPartial},{"cinemadeadok", &CinemaRestore::gDeadOk},{"cinemadeadrefs", &CinemaRestore::gDeadRefs}, {"compscrub", &Snapshot::gCompListScrub},{"pendrel", &gPendRel},{"attachfuture", &gAttachFuture},                 {"forcelod", &gForceLod},        {"comprender", &gCompRenderTime}, {"skelbind", &Snapshot::gSkelBind}, {"skelbound", &Snapshot::gSkelBoundLoad}, {"compweights", &Snapshot::gCompWeightsLoad}, {"bindstate", &Snapshot::gBindStateLoad}, {"listrescue", &Snapshot::gListRescue}, {"pinfloats", &Snapshot::gPinFloats}, {"compfloats", &Snapshot::gCompFloats}, {"complivedata", &Snapshot::gCompLiveData}, {"reflectdata", &Snapshot::gReflectData}, {"listrebase", &Snapshot::gListRebase}, {"attachall", &gAttachAll}, {"fxactors", &Snapshot::gFxActorsLoad}, {"mkactors", &Snapshot::gMkActorsLoad}, {"mkhelper", &Snapshot::gMkHelpersLoad}, {"mkhelperrevive", &Snapshot::gMkHelperRevive}, {"rayqfix", &RayQueries::gFix},{"movereq", &Snapshot::gMoveReqLoad}, {"costdmg", &Snapshot::gCostDmgLoad}, {"attachrepair", &gAttachRepair}, {"padfull", &Snapshot::gPadFullLoad}, {"padhw", &Snapshot::gPadHwLive}, {"mkbases", &Snapshot::gMkBases}, {"propskel", &Snapshot::gPropSkelLoad}};
    for (const T& x : ts)
        if (!strcmp(name, x.n)) {
            gAbToggle = x.p;
            gAbName = x.n;
            gAbSaved = *x.p;
            gAbLoads[0] = gAbLoads[1] = gAbErrors[0] = gAbErrors[1] = 0;
        }
    Logger::Info("ABTEST %s", gAbToggle ? gAbName : "off");
}
void SetQuarantineMenus(bool on) {
    gQuarantineMenus = on;
    Logger::Info("QMENUS %d: the free quarantine runs %s", on ? 1 : 0, on ? "always (pre-fix behaviour)" : "only while a session is armed or running");
}
void SetNetplayFromLoad(bool on) {
    gNetplayFromLoad = on;
    Logger::Info("NETPLAYLOAD %d: netplay sessions start at the %s", on ? 1 : 0, on ? "timeline restart (map load)" : "first fight frame");
}
void SetNetplayDemoFights(bool on) {
    gNetplayDemoFights = on;
    Logger::Info("NETPLAYDEMO %d: CPU-vs-CPU fights (attract demos) %s", on ? 1 : 0, on ? "start netplay sessions (tests)" : "never start a netplay session");
}
bool NetplayAwaitThisTimeline() {
    if (!gNetplayArmed || gNetplayAwaitFight || gFoughtThisTimeline) return gNetplayAwaitFight;
    gNetplayAwaitFight = true;
    Logger::Info("NETPLAY: armed before this timeline's first fight frame (gf=%u): the session starts at it", GameFrame());
    return true;
}
void SetNetplayAutoRearm(bool on) {
    gNetplayAutoRearm = on;
    Logger::Info("NETPLAYREARM %d: %s", on ? 1 : 0,
                 on ? "a netplay session that ends at a timeline restart is re-armed with the same configuration for the next fight"
                    : "every netplay session is armed by its own command");
}
// Script event listeners (node list 0xE0B4F0: node +4 listener object, +8 its id, +0xC next, +0x18 disabled; listener
// +0x10 event id, +0xC function index), in list order. The two peers' lists should be identical apart from addresses.
void LogEventListeners(const char* tag) {
    uint32_t node = *reinterpret_cast<const uint32_t*>(0x00E0B4F0 + 4);
    char line[900];
    int n = 0, count = 0;
    for (; node && count < 256 && Readable(node, 0x1C); node = *reinterpret_cast<const uint32_t*>(node + 0xC), ++count) {
        const uint32_t obj = *reinterpret_cast<const uint32_t*>(node + 4);
        const uint32_t flag = *reinterpret_cast<const uint32_t*>(node + 0x18);
        if (!obj || obj == 0xDEADBEEF || !Readable(obj, 0x14)) {
            if (n < 840) n = AppendF(line, sizeof line, n, " [dead]");
            continue;
        }
        const uint32_t ev = *reinterpret_cast<const uint32_t*>(obj + 0x10), fn = *reinterpret_cast<const uint32_t*>(obj + 0xC);
        if (n < 840) n = AppendF(line, sizeof line, n, " [ev %X fn %u%s %.24s]", ev, fn, flag ? " off" : "", ObjectHooks::TypeOf(obj));
    }
    line[n] = 0;
    Logger::Info("LISTENERS %s gf=%u: %d:%s", tag, GameFrame(), count, line);
}

// Netplay start: the script event listener list (0xE0B4F0) starts empty on both peers. Whether a script registered
// listeners before the first fight frame depends on each game's own pre-session history: in two-instance runs B
// started with the two listeners of Toasty.mko's toasty_init (events 0x4C and 0x10, init 0x897830 via native
// 0x8D6150), A with none, and the first hit with event 0x10 (frame 1412) ran a handler that draws RNG A on B only.
// Every run without them started empty, so empty is the canonical start; listeners registered during the session
// are the same on both peers. Cost: an easter-egg popup armed before the start does not appear in that fight.
// Unlinked with the game's own 0x42D6E0 (the listener objects stay; unlinked they are never called).
int ClearEventListeners() {
    using UnlinkFn = void(__thiscall*)(void* list, uint32_t node);
    int removed = 0;
    uint32_t node = *reinterpret_cast<const uint32_t*>(0x00E0B4F0 + 4);
    for (int guard = 0; node && guard < 256 && Readable(node, 0x1C); ++guard) {
        const uint32_t next = *reinterpret_cast<const uint32_t*>(node + 0xC);
        reinterpret_cast<UnlinkFn>(0x0042D6E0)(reinterpret_cast<void*>(0x00E0B4F0), node);
        ++removed;
        node = next;
    }
    return removed;
}

uint32_t NetplayGf() { return gNetplayGf; }
void SetNetplayMeter(float value) {
    gNetplayMeter = value;
    if (value < 0) Logger::Info("NETPLAYMETER off");
    else Logger::Info("NETPLAYMETER %.4f: both meters set at frame %u of each local netplay session", value, gNetplayGf + kNetplayMeterRel);
}
void SetNetplayLife(float value) {
    gNetplayLife = value;
    if (value < 0) Logger::Info("NETPLAYLIFE off");
    else Logger::Info("NETPLAYLIFE %.4f: both fighters' life set at frame %u of each local netplay session", value, gNetplayGf + kNetplayMeterRel);
}
void SetNetplayGf(uint32_t gf) {
    gNetplayGf = gf & ~3u;  // same GameFrame & 3 (player update order) on both peers
    Logger::Info("NETPLAYGF %u: %s", gNetplayGf, gNetplayGf ? "GameFrame and the script clock are renumbered at the netplay start" : "each game keeps its own GameFrame");
}

void SetFixSeed(uint32_t seed) {
    gFixSeed = seed;
    Logger::Info("FIXSEED %08X (0 = off): applied at every timeline restart", seed);
}

// Once per live tick (OnTickEntry; resimulated frames never come through here): the status line's stats.
void PublishOverlay() {
    if (Rollback::gIsResimulating || Overlay::Level() <= 0) return;
    static uint32_t ticks = 0;
    ++ticks;
    if (ticks % 600 == 0) {
        FramePace::Note("overlaycheck");
        Overlay::EnsureInstalled();
    }
    Overlay::Stats s;
    const uint32_t gf = GameFrame();
    if (gGgpo && gGgpoIsP2P) {
        s.mode = !gGgpoRunning ? Overlay::kWaiting : gLockstep ? Overlay::kLockstep : Overlay::kRollback;
        if (gGgpoRunning && ticks % 30 == 0) {
            GGPONetworkStats ns{};
            const int remote = 1 - gGgpoLocal;
            if (GGPO_SUCCEEDED(ggpo_get_network_stats(gGgpo, gGgpoPlayer[remote], &ns))) {
                gOvPing = ns.network.ping;
                gOvBehindL = ns.timesync.local_frames_behind;
                gOvBehindR = ns.timesync.remote_frames_behind;
                gOvKbps = ns.network.kbps_sent;
                gOvQueue = ns.network.send_queue_len;
            }
        }
    } else if (gGgpo) {
        s.mode = Overlay::kSyncTest;
    } else if (gOvStopAt && GetTickCount() - gOvStopAt < 12000) {  // 5 s until rc25: easy to miss mid-round
        s.mode = Overlay::kStopped;
        s.stopAgeS = (uint8_t)((GetTickCount() - gOvStopAt) / 1000);
    } else if (gNetplayArmed || gNetplayWaitRunning || gP2PPending) {
        s.mode = Overlay::kWaiting;
    } else if (const char* why = Online::StatusText()) {  // an online fight on MK9's netcode: say why
        s.mode = Overlay::kInfo;
        snprintf(s.stopWhy, sizeof s.stopWhy, "%s", why);
    }
    if (!gGgpo || !gGgpoIsP2P) gOvPing = -1, gOvLastDepth = 0;
    s.ping = (int16_t)gOvPing;
    s.behindLocal = (int16_t)gOvBehindL;
    s.behindRemote = (int16_t)gOvBehindR;
    s.kbps = gOvKbps;
    s.sendQueue = gOvQueue;
    s.delay = (int16_t)(gLockstep && gP2P.delay < 1 ? 2 : gP2P.delay);
    s.lastDepth = gOvLastDepth;
    uint32_t maxD = 0, loads = 0;
    for (int k = 0; k < 60; ++k)
        if (gOvDepthGf[k] && gf - gOvDepthGf[k] < 60) {
            ++loads;
            if (gOvDepth[k] > maxD) maxD = gOvDepth[k];
        }
    s.maxDepth = (uint16_t)(maxD < 0xFFFF ? maxD : 0xFFFF);
    s.loadsPerSec = (uint16_t)loads;
    s.loads = gGgpoLoads;
    s.replayed = gGgpoReplayed;
    s.mismatched = gCsMismatched;
    s.barrierWaits = gBarrierWaits;
    // rc25 connection-quality inputs (the overlay picks the wording and the colour).
    const bool running = gGgpo && gGgpoIsP2P && gGgpoRunning;
    s.rttMs = (int16_t)(running && ggpo_rtt_min_ms >= 0 ? (ggpo_rtt_min_ms < 32767 ? ggpo_rtt_min_ms : 32767) : -1);
    if (running) FramePace::Quality(&s.netStallMs, &s.netStallAgoS, &s.localHitchMs, &s.localHitchAgoS);
    s.peerInterrupted = running && gOvPeerInterrupted ? 1 : 0;
    if (s.mode != Overlay::kInfo) memcpy(s.stopWhy, gOvStopWhy, sizeof s.stopWhy);
    if (Online::Engaged()) {
        s.wifi = (uint8_t)((Online::LocalOnWifi() ? 1 : 0) | (Online::PeerOnWifi() ? 2 : 0));
        s.relayed = Online::LinkRelayed() ? 1 : 0;
    }
    // The HUD display (Hud.h): where the game is, which side this PC plays, and what the next fight will run on.
    const uint32_t gi = *reinterpret_cast<const uint32_t*>(kGameInfoPtr);
    const uint32_t state = gi ? *reinterpret_cast<const uint32_t*>(gi + 0x4) : 0xFFFFFFFFu;
    s.phase = state == 7 ? Overlay::kPhaseSelect : state == 1 ? Overlay::kPhaseLoad : state == 8 ? Overlay::kPhaseFight
              : state == 9 ? Overlay::kPhaseResults : state == 0xB ? Overlay::kPhaseFinisher : Overlay::kPhaseOther;
    s.hudHidden = s.phase == Overlay::kPhaseFinisher ? 1 : 0;  // the game takes its HUD away for a finisher
    s.localSide = running ? (uint8_t)(gGgpoLocal + 1) : 0;
    if (s.phase == Overlay::kPhaseSelect || s.phase == Overlay::kPhaseLoad) {
        int rtt = -1;
        const char* why = nullptr;
        s.pre = (uint8_t)Online::PreFight(&rtt, &why);
        s.preRttMs = (int16_t)rtt;
        if (s.pre == Overlay::kPreBlocked && why) snprintf(s.stopWhy, sizeof s.stopWhy, "%s", why);
    }
    Overlay::Publish(s);
}

bool OnTickEntry(void* engine, float dt) {
    // Frame pacing (PACE lines): the interval between live, unpaused P2P boundaries. 'tickspin N' adds N ms of CPU work
    // to each such frame here (and to each replayed one): a slower PC for the two-instance tests.
    if (gGgpo && gGgpoIsP2P && gGgpoRunning && !*reinterpret_cast<const uint32_t*>(0x00E43054)) {
        FramePace::LiveFrame(GameFrame());
        TickSpin();
    } else {
        FramePace::Gap();
    }
    BetweenProbeEntry();
    Online::Tick();
    PublishOverlay();
    if (gEnabled) ForceFighterLod();
    gTickAudits = false;
    if (!gEnabled || !gTick) return false;
    // Births/deaths since the last engine tick (between frames: render, UI, main loop)
    // happened BEFORE this boundary: stamp them F-1, never F.
    ObjectHooks::FlushFrame(GameFrame() - 1);
    // A running lockstep session never saves or loads, so a busy script VM does not concern it; skipping its boundary
    // would run this frame on the previous frame's inputs (gSyncValid stays set), one frame the peer never runs.
    const bool lockstepRunning = gGgpo && gGgpoIsP2P && gLockstep;
    if ((int32_t)*reinterpret_cast<const uint32_t*>(MKHeap::kScriptDepth) != -1 && !lockstepRunning) {
        static uint32_t lastWarn = 0;
        if (GameFrame() - lastWarn > 60) {
            Logger::Info("ROLLBACK skip gf=%u: script depth %d at tick entry", GameFrame(),
                         (int)*reinterpret_cast<const uint32_t*>(MKHeap::kScriptDepth));
            lastWarn = GameFrame();
        }
        // A running rollback session cannot save or load here and must not run the frame unsynchronized either. One still
        // waiting for the peer (paused, no frame yet) just skips the tick as before.
        if (gGgpo && gGgpoIsP2P && gGgpoRunning) GgpoStop("the script VM was busy at the frame boundary");
        return false;
    }
    // GameFrame is reset (or jumps, e.g. attract demos) at fight/round start: every
    // gf-keyed structure starts over BEFORE any pending rollback can look at the ring.
    static uint32_t lastF = 0;
    {
        const uint32_t F0 = GameFrame();
        if (F0 < lastF || F0 > lastF + kRing) {
            for (bool& h : gHave) h = false;
            ++gEpoch;
            ObjectHooks::RestartTracking(F0);
            FreeQuarantine::ReleaseAll();
            FreeQuarantine::NewTimeline();
            ActorDefer::DropAll();
            CinemaRestore::DropAll();
            SoundDedupe::Clear();
            ParticleDone::Clear();
            ScriptClock::Anchor(GameFrame());
            ScriptClock::SetActive(VirtualClockOutsideSessions());  // the session (if any) stops below; the next one re-anchors
            PhysWatch::Clear();
            RngTrace::ClearScriptContexts();
            FreeQuarantine::ClearRestoreBlocks();
            RayQueries::Clear();
            Ue3Refs::OnTimelineRestart();
            gPendingFrames = 0;
            gFoughtThisTimeline = false;
            if (gP2PPending || gNetplayWaitRunning) {
                // The fight start that asked for the session is gone (lag_ns120_d3, 2026-10-27: GameFrame went to 0 between
                // the start request and the session start; the session then ran on the next map's load, life 0 at frame 0,
                // and the join wait later cancelled netplay). Unpause; an armed game waits for the next fight start.
                Logger::Info("NETPLAY: the timeline restarted (gf=%u) before the session was running: that start is cancelled", F0);
                gP2PPending = gNetplayWaitRunning = gNetplaySeedPin = false;
                CommandChannel::RequestPause(0);
            }
            GgpoStop("timeline restart");
            gListAuditBroken = gHeapAuditBroken = false;  // audits report once per timeline
            if (gFixSeed) {  // reproducible test fights ('fixseed <hex>'): both RNGs from a fixed seed at every timeline start
                *reinterpret_cast<uint32_t*>(0x00E7C054) = gFixSeed;
                *reinterpret_cast<uint32_t*>(0x00E5352C) = gFixSeed ^ 0x9E3779B9u;
                Logger::Info("FIXSEED: RNG A/B seeded from %08X at gf=%u", gFixSeed, F0);
            }
            // GameFrame also resets at menu/attract map loads: the session starts at the first frame of
            // this timeline that follows a fight update and has a snapshot (the same frame on both peers).
            if (gNetplayArmed) gNetplayAwaitFight = true;
            // 'netplayload 1': start at the timeline restart itself (map load), so the intro also runs in lockstep;
            // the first fight frame depends on how long each game's intro took (two-instance test, 2026-10-18).
            if (gNetplayArmed && gNetplayFromLoad) gNetplayStartNow = true;
            Logger::Info("ROLLBACK timeline restart at gf=%u (previous %u), %d ticks without a fight update before it", F0, lastF,
                     gTicksWithoutFight);
        }
        lastF = F0;
        FreeQuarantine::SetFrame(F0);
        ActorDefer::SetFrame(F0);
        CinemaRestore::SetFrame(F0);
        PhysWatch::SetFrame(F0);
    }
    // Attract mode: a game left at the title screen plays CPU-vs-CPU demo fights. Armed at the title (the launcher's
    // Local Versus mode), the first demo started the session, paused the game and waited 60 s for a peer that was still
    // in its menus: both players saw a frozen title screen (2026-10-31 11:58, Raiden vs Raiden). A fight whose two
    // fighters are both CPU-controlled (an AIDrone each) is not a netplay fight: wait for the next one. The two-instance
    // tests play the demos on purpose ('netplaydemo 1').
    if (gNetplayAwaitFight && (GameLoopHook::FoughtThisTick() || gNetplayStartNow) && !gNetplayDemoFights) {
        const int cpu = CommandChannel::CountAiDrones();
        if (cpu >= 2) {
            gNetplayAwaitFight = gNetplayStartNow = false;
            Input::ReleaseAll();  // the await's neutral pads (RecordInputs) go back to the game
            Logger::Info("NETPLAY: this fight is CPU against CPU (%d AI drones: an attract demo), not a netplay fight; the session "
                         "starts at the next fight",
                         cpu);
        }
    }
    // Netplay start: the same clocks on both peers. Each game reaches the first fight frame at its own GameFrame (the
    // intro waits on loading and audio: 588 vs 808 with one instance slowed, 2026-10-18), and gameplay keeps absolute
    // GameFrame stamps (input rings, fighter timers, script gates such as 0x00495AF0 'n frames since the last roll',
    // the update order GameFrame & 3). MK9's own online start (0x00884770) zeroes GameFrame for the same reason.
    // A large value makes every stamp from before the start "long ago" on both peers; the script clock gets fixed
    // bases likewise. Every gf-keyed structure starts over, as at a timeline restart.
    if (gNetplayGf && gNetplayAwaitFight && (GameLoopHook::FoughtThisTick() || gNetplayStartNow) && GameFrame() != gNetplayGf) {
        const uint32_t old = GameFrame();
        // The fighters' own stamps get MK9's online-start rebase (0x0087FA70: x > GF ? x - GF : 0, before GF = 0),
        // onto the new number: pending deadlines keep their remaining frames, past stamps become "now".
        int rebased = 0;
        auto rebase = [&](uintptr_t a) {
            if (!Readable(a, 4)) return;
            uint32_t& x = *reinterpret_cast<uint32_t*>(a);
            x = (x > old ? x - old : 0) + gNetplayGf;
            ++rebased;
        };
        for (int p = 0; p < 2; ++p) {
            const uintptr_t ch = *reinterpret_cast<const uint32_t*>(0x00E89D7C + p * 0xC8C + 0x1C);
            if (!ch || ch == 0xDEADBEEF || !Readable(ch, 0x7600)) continue;
            rebase(ch + 0x75F8);
            for (uint32_t i = 0; i < 0x3C; ++i) {
                const uintptr_t r = ch + 0x30F4 + i * 0x124;
                rebase(r - 8);
                rebase(r);
                rebase(r - 4);
            }
            const uint32_t n = *reinterpret_cast<const uint32_t*>(ch + 0x75EC);
            for (uint32_t i = 0; i < n && i < 64; ++i) {
                const uintptr_t e = *reinterpret_cast<const uint32_t*>(ch + 0x7524 + i * 4);
                if (!e || !Readable(e + 0x38, 12)) continue;
                rebase(e + 0x38);
                rebase(e + 0x40);
                rebase(e + 0x3C);
            }
        }
        GameFrame() = gNetplayGf;
        for (bool& h : gHave) h = false;
        ++gEpoch;
        ObjectHooks::RestartTracking(gNetplayGf);
        FreeQuarantine::ReleaseAll();
        FreeQuarantine::NewTimeline();
        ActorDefer::ReleaseAll();
        CinemaRestore::ReleaseAll();
        CinemaRestore::DropAll();  // the saves (keyed by the old frame numbers)
        SoundDedupe::Clear();
            ParticleDone::Clear();
        PhysWatch::Clear();
        FreeQuarantine::ClearRestoreBlocks();
        RayQueries::Clear();
        Ue3Refs::OnTimelineRestart();
        RngTrace::RebaseFrames(old, gNetplayGf);
        ScriptClock::AnchorCanonical(gNetplayGf);
        gPendingFrames = 0;
        lastF = gNetplayGf;
        FreeQuarantine::SetFrame(gNetplayGf);
        ActorDefer::SetFrame(gNetplayGf);
        CinemaRestore::SetFrame(gNetplayGf);
        PhysWatch::SetFrame(gNetplayGf);
        Logger::Info("NETPLAY: GameFrame %u renumbered to %u (%d fighter stamps rebased), script clock on fixed bases",
                     old, gNetplayGf, rebased);
        LogEventListeners("netplay start");
        if (const int cleared = ClearEventListeners()) {
            Logger::Info("NETPLAY: %d script event listeners registered before the start were unlinked (canonical empty list)", cleared);
            LogEventListeners("after clearing");
        }
    }
    if (gGgpoPendingStart) {
        GgpoStart(gGgpoPendingStart);
        gGgpoPendingStart = 0;
    }
    if (gP2PPending) {
        gP2PPending = false;
        if (!GgpoStartP2P(gP2P) && gNetplayWaitRunning) {
            // Port taken, Steam unavailable: this game paused at its fight start and skipped MK9's exchange for the whole
            // join timeout before giving up.
            gNetplayWaitRunning = gNetplaySeedPin = gNetplayArmed = false;
            CommandChannel::RequestPause(0);
            Input::ReleaseAll();  // the await's neutral pads (RecordInputs) go back to the controllers
            Logger::Info("NETPLAY: the session could not start; netplay is cancelled and the fight continues on %s",
                         Online::Engaged() ? "MK9's netcode" : "this PC only");
        }
    }
    if (gGgpo) gPendingFrames = 0;  // GGPO decides when to roll back
    RecordRenderFields(GameFrame());  // the original values of this live frame (before any rollback)
    bool rolled = false;
    if (gPendingFrames > 0) {
        const int k = gPendingFrames;
        gPendingFrames = 0;
        if (gPendingFake) {
            const uint32_t F = GameFrame();
            for (int i = 0; i < gFakeCount && i < k; ++i) {
                InputFrame& in = gInputs[(F - k + i) % kInputRing];
                if (in.gf == F - k + i) {
                    Logger::Frame("FAKE correction gf=%u P1 %08X -> %08X", in.gf, in.mask[0], gFakeMask);
                    in.mask[0] = gFakeMask;
                    in.valid[0] = true;
                }
            }
            gPendingFake = false;
        }
        rolled = DoRollback(engine, dt, k, gPendingLoadOnly);
        gPendingLoadOnly = false;
        if (rolled) ReplayRenderFields(GameFrame());
    }
    const uint32_t F = GameFrame();
    lastF = F;
    CrashLog::CrumbFrame("rollback:Save", F);
    LARGE_INTEGER s0, s1, fq;
    QueryPerformanceCounter(&s0);
    // Actor deferral only while frames are being snapshotted (a fight): deaths in the
    // end-of-fight teardown must really destroy their actors (soak6: deferred actors left in
    // a world being torn down crashed the next level start at 0x0071C53C/0x0071C517).
    if (gDiag) Snapshot::WatchBin(29, F);
    // Snapshots and the restore audits only where a load can follow: a GGPO session, one starting (pending, the join
    // wait) or this tick's netplay start (frame F's snapshot). Taken every tick in rollback mode before 2026-10-31, menus
    // and fights without a session included: the audits walk every MK heap under its spinlock (GMalloc's medium heap
    // too) and the saves read the fight's objects from the first fighter on, through the fight's load and intro; attract
    // demo fights entered that way were drawn almost black (screenshots: brightness 0 with rollback mode, normal with
    // the mod passive or in Safe mode, and normal when rollback mode was switched on after the fight had started; a
    // player's online practice fight went black too). Lockstep never restores: no snapshots, no audits.
    // 'idlesaves 1' brings the old every-tick saves back (local 'rollback N' tests outside a session need them).
    // The logs of those dark fights: the fight's load wait got 200-290 engine ticks at 27-41 ms each, against ~4500 at
    // 2.5 ms in Safe mode (same wall time, same objects created); the gate alone left it at 27 ms, the phase audits
    // (PhaseAudit, 3 heap+list walks per tick) running on, so they follow this gate too.
    const bool netplayStart = gNetplayAwaitFight && (GameLoopHook::FoughtThisTick() || gNetplayStartNow);
    const bool snapshotting = !gLockstep && (gIdleSaves || gGgpo || gP2PPending || gNetplayWaitRunning || netplayStart);
    gTickAudits = snapshotting;
    if (snapshotting) {
        AttachAudit("tick");
        TaskRegAudit("tick");
        HeapAudit("tick");
        ListAudit("tick");
    }
    // Netplay start: every random stream the simulation reads gets the same seed on both peers, before frame F's
    // snapshot (a rollback to F must restore the seeded state). The script stream was seeded from RNG A when the
    // fighters were created, while the two machines' RNG A still differed (two-instance test, 2026-10-18).
    // The game's pause (0x00407AB0) keeps EngineTick running, and its idle draw (caller 0x0080F6C5) advances RNG A once
    // per tick: a peer that waited longer for the other left the pause with a different RNG (two-instance lockstep,
    // 2026-10-18: every checksum differed from frame 0). The seeds are re-applied at every tick until the first
    // unpaused one.
    if (GameLoopHook::FoughtThisTick()) gFoughtThisTimeline = true;
    if (netplayStart || gNetplaySeedPin) {
        *reinterpret_cast<uint32_t*>(0x00E7C054) = kNetplaySeed;
        *reinterpret_cast<uint32_t*>(0x00E5352C) = kNetplaySeed ^ 0x9E3779B9u;
        ScriptRng::State* sr = ScriptRng::StatePtr();
        sr->seed = kNetplaySeed ^ 0x5bd1e995u;
        sr->key = ScriptRng::CurrentKey();
        // 'canonclock 1' (test, 2026-11-04): the UE3 world clocks too (WorldInfo +0x284 TimeSeconds, +0x288
        // RealTimeSeconds). They count ticks since the level loaded, so they differed by each game's load and join wait.
        if (gCanonClock) {
            if (const uintptr_t wi = Snapshot::WorldInfoActor()) {
                float* clk = reinterpret_cast<float*>(wi + 0x284);
                if (netplayStart) Logger::Info("NETPLAY: world clocks %.4f %.4f set to 64 at gf=%u", clk[0], clk[1], F);
                clk[0] = clk[1] = 64.0f;
            }
        }
        // The fighters' input history too (prev records, press/release rings, last-buttons lists): it came from the
        // unsynchronized time before the start (between rounds) and split round 2 at its first frame.
        const int pads = Input::CanonicalizePadsForNetplay();
        if (netplayStart) Logger::Info("NETPLAY: input history of %d fighter pad(s) cleared to the neutral state", pads);
        // AI drones the game created for this round: removed here, on both peers before frame F's snapshot, instead of
        // by an outside command that reached the two games on different frames (fight 2 split at frame 1).
        if (const int ai = CommandChannel::KillAiDrones())
            Logger::Info("NETPLAY: %d AI drone(s) removed at the start (gf=%u); both fighters follow the session's input", ai, F);
        if (gNetplaySeedPin && !gNetplayWaitRunning && !*reinterpret_cast<const uint32_t*>(0x00E43054)) {
            gNetplaySeedPin = false;  // first unpaused tick: it starts from the seeds
            Logger::Info("NETPLAY: seeds held through the pause, released at gf=%u", F);
        }
    }
    NetplayFrameEvents(F);  // before frame F's save (live or lazy)
    // Lazy live save in a running P2P rollback session (after its frame 0): GgpoBoundaryP2P saves F after its first poll
    // unless a replay there saved F (gLazySave).
    const bool lazy = snapshotting && gLazySave && gGgpo && gGgpoIsP2P && gGgpoRunning && !gLockstep && !netplayStart && gP2PFrames > 0;
    bool saved;
    if (lazy) {
        gLivePending = F;
        gLiveSavedGf = 0xFFFFFFFF;
        saved = true;
    } else {
        const int64_t ts = FramePace::Now();
        saved = snapshotting ? Save(F) : false;
        if (gGgpo && gGgpoIsP2P) FramePace::Add(FramePace::kLiveSave, FramePace::Now() - ts);
        gLiveSavedGf = saved ? F : 0xFFFFFFFF;
    }
    gHostAt[F % 256] = HostFrame();
    gHostAtGf[F % 256] = F;
    if (netplayStart && (saved || gLockstep) && Online::Engaged()) {
        // MK9 online fight: the game decided the sides; this game drives the fighter whose pad is one of its local pads.
        const int side = Online::LocalPlayer();
        if (side) gP2P.player = side;
        Logger::Info("ONLINE: this game controls player %d in the online fight%s", gP2P.player,
                     side ? "" : " (side not found in the game's pad lists; keeping the armed side)");
    }
    if (netplayStart && (saved || gLockstep)) {
        gNetplayAwaitFight = false;
        gNetplayStartNow = false;
        CommandChannel::RequestPause(1);
        gP2PPending = true;  // started at the next boundary, while paused
        gNetplayWaitRunning = true;
        gNetplayWaitSince = GetTickCount64();
        gNetplaySeedPin = true;
        Logger::Info("NETPLAY: first fight frame gf=%u: pausing, RNG A/B and script stream seeded, starting the session", F);
    }
    // MK9 starts an online fight on both PCs together: a peer still missing after 15 s (GGPO's handshake included, which
    // retries every 2 s on a lost packet) will not come, and meanwhile this game skips MK9's exchange, so the other one's
    // MK9 sees no pads and drops the match well before 60 s.
    const ULONGLONG joinTimeoutMs = Online::Engaged() ? 15000 : kNetplayJoinTimeoutMs;
    if (gNetplayWaitRunning && GetTickCount64() - gNetplayWaitSince > joinTimeoutMs) {
        // The other game never reached its fight start (or the session could not start): don't leave this one paused.
        gNetplayWaitRunning = gNetplaySeedPin = gNetplayArmed = gP2PPending = false;
        Logger::Info("NETPLAY: the other player did not join within %u s; netplay is cancelled (re-arm it to try again) "
                     "and the fight continues locally", (unsigned)(joinTimeoutMs / 1000));
        GgpoStop("the peer did not join");
        CommandChannel::RequestPause(0);
    }
    if (gGgpo) {
        if (gGgpoIsP2P) GgpoBoundaryP2P(engine, dt);  // P2P: rollbacks inside ggpo_idle
        else GgpoBoundary(engine, dt);  // closes frame F-1; SyncTest may replay up to F here
        if (gGgpoLoadFailed) {  // outside any GGPO call now
            GgpoStop(gGgpoStopWhy);
            Rollback::gIsResimulating = false;
            gGgpoLoadFailed = false;
            gGgpoStopWhy = "a load found no snapshot";
        }
        rolled = gGgpoLoadedS != 0xFFFFFFFF;
        if (rolled) ReplayRenderFields(F);
    }
    if (gLivePending == F) {  // not saved by the boundary (no P2P boundary ran): as before, now
        if (gGgpo && !SaveLiveNow(F) && !gSaveRefusedTeardown) ++gGgpoSaveFails;
        gLivePending = 0xFFFFFFFF;
    }
    RngTrace::BeginFrame(F, false);
    CrowdRng::BeginFrame();
    CrashLog::TraceFrame(F, false);
    // Deferring effect-actor destruction keeps actors ticking with a freed MK owner; only worth it while a rollback
    // can actually happen. Two fight-start crashes (0x0042AF1D, the global MK list broken one frame after four
    // deferrals, synctest58/74) came before the fight's GGPO session had started. 'deferggpo 0' = defer always.
    ActorDefer::SetActive(saved && (gGgpo || !gDeferOnlyGgpo));
    CinemaRestore::SetActive(saved && (gGgpo || !gDeferOnlyGgpo));
    // The object scan's first pass classifies every live object (62 ms at a session's first save, soak 14001): done
    // ahead in fight ticks without a snapshot (intros, fights before a session), so the first save only diffs.
    if (!saved && gEnabled && !gLockstep && GameLoopHook::FoughtThisTick()) CinemaRestore::Prewarm();
    // No loadable future for them: destroyed after this tick's fight update (FightTickDone), where the game destroys effect
    // actors itself; here, at the boundary, pool threads may still walk the world lists (soak23, see below).
    if (!saved) gReleaseDeferredAll = true;
    QueryPerformanceCounter(&s1);
    QueryPerformanceFrequency(&fq);
    if (gGgpo && gGgpoIsP2P) FramePace::Add(FramePace::kBoundary, s1.QuadPart - s0.QuadPart);
    static double saveMsAcc = 0;
    static int saveCount = 0;
    saveMsAcc += (s1.QuadPart - s0.QuadPart) * 1000.0 / fq.QuadPart;
    if (++saveCount == 600) {
        FramePace::Note("memreport");
        Logger::Info("ROLLBACK save cost: %.3f ms/frame average over 600 frames", saveMsAcc / saveCount);
        // Memory watch (an hour-long soak ran the 32-bit process out of address space, 3.3 GB private).
        PROCESS_MEMORY_COUNTERS_EX pmc{};
        pmc.cb = sizeof pmc;
        K32GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&pmc), sizeof pmc);
        MEMORYSTATUSEX ms{};
        ms.dwLength = sizeof ms;
        GlobalMemoryStatusEx(&ms);
        Logger::Info("MEMORY gf=%u: private %u MB, address space used %u MB; quarantine %u blocks (~%u MB)", GameFrame(),
                     (unsigned)(pmc.PrivateUsage >> 20), (unsigned)((ms.ullTotalVirtual - ms.ullAvailVirtual) >> 20),
                     FreeQuarantine::Pending(), (unsigned)(FreeQuarantine::PendingBytesApprox() >> 20));
        // Outside the lab too once it is large (one walk per 600 frames): a field log then names what piles up.
        if (FreeQuarantine::Pending() > (Lab::On() ? 500u : 20000u)) FreeQuarantine::LogCensus();
        {  // MK heaps [0xED53F0..0xED5400]: used +0x28 / reserved +0x24 (2026-10-29: #5 ran full in the menus)
            // with the base (+0x34) and whether the block range lies inside the master's region (frees of blocks
            // outside it are deferred by the GMalloc hooks: suspected menu freeze cause)
            char hs[320] = "";
            int k = 0;
            uint32_t mBase = 0, mRes = 0, m = 0;
            CrashLog::SafeRead32(0x00ED53F0, &m);
            if (m) {
                CrashLog::SafeRead32(m + 0x34, &mBase);
                CrashLog::SafeRead32(m + 0x24, &mRes);
            }
            for (int h = 0; h < 5; ++h) {
                uint32_t hp = 0, res = 0, used = 0, base = 0;
                CrashLog::SafeRead32(0x00ED53F0 + 4 * h, &hp);
                if (hp) {
                    CrashLog::SafeRead32(hp + 0x24, &res);
                    CrashLog::SafeRead32(hp + 0x28, &used);
                    CrashLog::SafeRead32(hp + 0x34, &base);
                }
                const bool inMaster = h == 0 || (base >= mBase && base + res <= mBase + mRes);
                if (k < (int)sizeof hs)
                    k = AppendF(hs, sizeof hs, k, " #%d %u/%u KB @%08X%s", h + 1, used >> 10, res >> 10, base, inMaster ? "" : " OUTSIDE-MASTER");
            }
            Logger::Info("MKHEAPS used/reserved:%s", hs);
        }
        if (Lab::On()) {  // the address-space walk and the object censuses (~10 ms on the game thread)
            RegionDiff();
            TypeCensus();
        }
        saveMsAcc = 0;
        saveCount = 0;
    }
    CrashLog::Crumb("game");
    ObjectHooks::TrimHistory(FramesBack(F, kRing - 1));
    {  // The free quarantine only while a session can load: a GGPO session, or netplay armed / starting. 2026-10-29
       // both players' games froze in MK9's menus with rollback mode on (MK heap #5 allocation failure at 18:41) and
       // ran 11 minutes of online fights without a freeze in Safe mode, whose only menu-time difference is this.
        const bool want = gEnabled && !gLockstep &&
                          (gQuarantineMenus || gGgpo || gNetplayArmed || gNetplayAwaitFight || gNetplayWaitRunning || gP2PPending);
        if (want != gQuarantineOn) {
            gQuarantineOn = want;
            FreeQuarantine::SetActive(want);
            if (!want) FreeQuarantine::ReleaseAll();
            Logger::Info("QUARANTINE %s (%s)", want ? "on" : "off", want ? "a rollback session is armed or running" : "no rollback session");
        }
    }
    FreeQuarantine::SetFrame(F);
    AllocWatch::SetFrame(F);
    // gQuarantineMargin frames past the ring: a safety margin for state that still points at a block freed just outside
    // the window. (Added for the 2026-11-04 regression crash, which turned out to be a garbage-collected light component
    // read by a resurrected effect's destructor, not a released block: ActorGuard.)
    FreeQuarantine::Release(FramesBack(F, (kRing - 1) + (uint32_t)gQuarantineMargin));
    ActorDefer::SetFrame(F);  // releases happen after the fight update (ReleaseDeferredActors), not here
    CinemaRestore::SetFrame(F);
    PhysWatch::SetFrame(F);
    PhysWatch::Trim(FramesBack(F, kRing - 1));
    return rolled;
}

// Deferred actors are really destroyed inside the tick, right after the fight update, where the game
// destroys effect actors itself. At the frame boundary async work queued by the previous tick may
// still be walking world object lists on the pool threads (soak23: pool-thread garbage read at
// 0x007795AF through a destroyed object's Outer).
void LogSpansContaining(uint32_t addr) {
    const Snapshot::StateAB* best = nullptr;
    for (int i = 0; i < kRing; ++i)
        if (gHave[i] && gEpochOf[i] == gEpoch && (!best || gRing[i].gameFrame > best->gameFrame)) best = &gRing[i];
    if (!best) {
        Logger::Info("INSPAN %08X: no snapshot", addr);
        return;
    }
    int hits = 0;
    for (const Snapshot::Span& s : best->spans)
        if (addr >= s.base && addr < s.base + s.size) {
            Logger::Info("INSPAN %08X: gf=%u span %s %08X+0x%X (size 0x%X)", addr, best->gameFrame, s.name, (unsigned)s.base,
                         addr - (unsigned)s.base, s.size);
            ++hits;
        }
    if (!hits) Logger::Info("INSPAN %08X: gf=%u in no span (%u spans)", addr, best->gameFrame, (unsigned)best->spans.size());
}

void ReleaseDeferredActors() {
    if (!gEnabled || gIsResimulating) return;
    // Deferred actors live (ticking, owner freed) only as long as a load can still reach their death: GGPO SyncTest
    // goes back the check distance (4), P2P the prediction window (8); local rollbacks use the whole ring.
    const uint32_t oldest = FramesBack(GameFrame(), gGgpo ? (gGgpoIsP2P ? 10 : 6) : (kRing - 1));
    ActorDefer::Release(oldest);
    CinemaRestore::Release(oldest);
}

void FightTickDone(bool fought) {
    if (!gEnabled || gIsResimulating) return;
    if (Snapshot::gCineWatch && (fought || gTicksWithoutFight < 600)) {  // 'cinewatch 1': live ticks of a fight and after it
        const uint32_t actors[2] = {FighterActor(0), FighterActor(1)};
        Snapshot::CineWatch(GameFrame(), actors);
    }
    // Fight over (no fight update for a few ticks): destroy every deferred actor now, while its world still exists.
    // One left for the level teardown ('drop at timeline restart') was destroyed by the engine with its MK owner
    // already freed; the global MK object list broke a few hundred frames into the next fight (0x0042AF1D,
    // synctest58/74/75).
    gTicksWithoutFight = fought ? 0 : gTicksWithoutFight + 1;
    // Cinematic teardowns are deferred only in fights. The cinematic can tick before this tick's fight update, so the
    // previous tick's value decides; the first tick without a fight still defers, and releases them right here.
    CinemaRestore::SetFighting(fought);
    if (gTicksWithoutFight == 1) {
        Logger::Frame("ACTORDEFER fight over at gf=%u: releasing all deferred actors", GameFrame());
        ActorDefer::ReleaseAll();
        CinemaRestore::ReleaseAll();
    } else if (gReleaseDeferredAll) {
        ActorDefer::ReleaseAll();  // this tick had no snapshot (OnTickEntry)
        CinemaRestore::ReleaseAll();
    }
    gReleaseDeferredAll = false;
}

void RecordInputs() {
    if (!gEnabled || gIsResimulating) return;
    const uint32_t gf = GameFrame();
    InputFrame& in = gInputs[gf % kInputRing];
    in.gf = gf;
    if (gGgpo) {
        // Exactly one OverridePlayerInput per fighter pad per frame, as in a resimulated frame (see Input.h).
        uint32_t want[2];
        for (int p = 0; p < 2; ++p) want[p] = Input::TakeWantedInput(p);
        if (gGgpoIsP2P) {
            // Added at the next boundary. Scripted input (harness) wins; otherwise the player's own controllers/keyboard
            // (every local hardware pad), without the menu/aux bits 0x0F00 (not fight commands; 0x200 is likely Start, and
            // a pause would stall both games). The boundary reads the hardware again, from its own pass's sampling.
            gLocalScripted = want[gGgpoLocal] != 0;
            gLocalRaw = gLocalScripted ? want[gGgpoLocal] : (Input::LocalHardwareMask() & ~0x0F00u);
            for (int p = 0; p < 2; ++p) {
                in.valid[p] = gSyncValid;
                in.mask[p] = gSyncValid ? gSyncMasks[p] : 0;
                if (gSyncValid) Input::OverridePlayerInput(p, {gSyncMasks[p]});
            }
        } else {
            for (int p = 0; p < 2; ++p) ggpo_add_local_input(gGgpo, gGgpoPlayer[p], &want[p], sizeof(uint32_t));
            uint32_t sync[2] = {want[0], want[1]};
            int disconnected = 0;
            const bool ok = GGPO_SUCCEEDED(ggpo_synchronize_input(gGgpo, sync, sizeof sync, &disconnected));
            for (int p = 0; p < 2; ++p) {
                const uint32_t m = ok ? sync[p] : want[p];
                Input::OverridePlayerInput(p, {m});
                in.mask[p] = m;
                in.valid[p] = true;
            }
            if (ok) gGgpoFrameOpen = true;
        }
        return;
    }
    // Netplay armed: the fight's first update runs before the session can start (the boundary comes after it in the
    // tick), so it must not see each machine's own pads (two-instance test 2026-10-18: one peer's P1 jumped on its AI
    // input before frame 0). Both pads are neutral on every fight update until the session takes over. Twice: the first
    // call moves the last held mask into prev (a previous fight's input was still there: prev 0x8018 on one peer), the
    // second leaves prev = cur = 0 on both.
    if (gNetplayAwaitFight && GameLoopHook::FightBodyWillRun())
        for (int k = 0; k < 2; ++k)
            for (int p = 0; p < 2; ++p) Input::OverridePlayerInput(p, {0});
    for (int p = 0; p < 2; ++p) {
        in.valid[p] = Input::IsOverridden(p);
        in.mask[p] = Input::ReadPlayerInput(p).mask;
    }
}

// What a session armed by hand ('ggpo p2p' / 'ggpo netplay': the launcher's direct-IP mode, the test scripts) was started
// with, unchecked until rc30: an address GGPO cannot parse ends in its assert (the game closes), the checksum channel
// uses each port + 100, and GGPO's input queue holds 128 frames. Four decimal numbers 0-255 with dots, no leading
// zeros: what GGPO's inet_pton takes.
bool DottedQuad(const char* s) {
    for (int part = 0; part < 4; ++part) {
        const char first = *s;
        int v = 0, digits = 0;
        for (; *s >= '0' && *s <= '9'; ++s) {
            v = v * 10 + (*s - '0');
            if (++digits > 3 || v > 255) return false;
        }
        if (!digits || (digits > 1 && first == '0')) return false;
        if (part < 3 && *s++ != '.') return false;
    }
    return *s == 0;
}
bool P2PArgsOk(const char* verb, unsigned short localPort, const char* ip, unsigned short remotePort, int* delay) {
    if (!DottedQuad(ip) || !localPort || !remotePort || localPort > 65435 || remotePort > 65435) {
        Logger::Info("%s refused: it needs an IPv4 address (four numbers, like 192.168.0.12) and ports from 1 to 65435; got "
                     "'%.40s', local port %u, remote port %u", verb, ip, localPort, remotePort);
        return false;
    }
    if (*delay < 0 || *delay > HelloProto::kMaxDelay) {
        const int asked = *delay;
        *delay = asked < 0 ? 0 : HelloProto::kMaxDelay;
        Logger::Info("%s: delay %d is outside 0-%d, %d is used", verb, asked, HelloProto::kMaxDelay, *delay);
    }
    return true;
}

void GgpoP2PCommand(unsigned short localPort, const char* ip, unsigned short remotePort, int player, int delay) {
    if (!P2PArgsOk("GGPO p2p", localPort, ip, remotePort, &delay)) return;
    gP2P.localPort = localPort;
    gP2P.remotePort = remotePort;
    strncpy(gP2P.ip, ip, sizeof gP2P.ip - 1);
    gP2P.player = player == 2 ? 2 : 1;
    gP2P.delay = delay;
    gP2P.steamPeer = 0;
    gP2PPending = true;  // started at the next frame boundary
}

// Same over Steam P2P (Milestone 15): GGPO on 7000 talks to the local bridge on 7050 (checksums 7100 <-> 7150).
void GgpoSteamCommand(uint64_t peer, int player, int delay, bool netplay) {
    if (netplay) NetplayArm(7000, "127.0.0.1", 7050, player, delay);
    else GgpoP2PCommand(7000, "127.0.0.1", 7050, player, delay);
    gP2P.steamPeer = peer;
}

void NetplayArm(unsigned short localPort, const char* ip, unsigned short remotePort, int player, int delay) {
    if (!P2PArgsOk("NETPLAY arm", localPort, ip, remotePort, &delay)) return;
    gP2P.localPort = localPort;
    gP2P.remotePort = remotePort;
    strncpy(gP2P.ip, ip, sizeof gP2P.ip - 1);
    gP2P.player = player == 2 ? 2 : 1;
    gP2P.delay = delay;
    gP2P.steamPeer = 0;
    gNetplayArmed = true;
    Logger::Info("NETPLAY armed: player %d, local port %u, remote %s:%u, delay %d (starts at the next fight start)", gP2P.player,
                 localPort, ip, remotePort, delay);
}

void GgpoCommand(int checkDistance) {
    if (checkDistance > 0) {
        gGgpoPendingStart = checkDistance;  // started at the next frame boundary
    } else {
        GgpoStop("command");
    }
}

void RequestRollback(int frames) {
    gPendingFrames = frames;
    gPendingFake = false;
}

void RequestLoadOnly(int frames) {
    gPendingFrames = frames;
    gPendingFake = false;
    gPendingLoadOnly = true;
}

void RequestFakeRollback(int frames, uint32_t mask, int count) {
    gPendingFrames = frames;
    gPendingFake = true;
    gFakeMask = mask;
    gFakeCount = count;
}

}  // namespace Rollback










