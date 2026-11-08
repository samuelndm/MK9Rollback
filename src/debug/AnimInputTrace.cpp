#include "AnimInputTrace.h"

#include <windows.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <map>
#include <vector>
#include <string>

#include "Ue3Names.h"

#include "../hook/CodePatch.h"
#include "../game/MKHeap.h"
#include <new>
#include "../hook/ObjectHooks.h"
#include "../rollback/RollbackManager.h"
#include "../rollback/Snapshot.h"
#include "CrashLog.h"
#include "Logger.h"

// Live-vs-replay inputs of the fighters' skeleton update ('animtrace 1|0').
// Replays agree with each other but not with live frames (rbtwice), and the MK image is identical at FightUpdate
// exit, so the first divergence is inside the post-fight-update animation evaluation. This hook sits at the start of
// that evaluation: USkeletalMeshComponent skeleton update 0x007741E0 (thiscall, ecx = component; it first resets
// the root-motion delta +0x5D0..). The first call per frame per fighter copies the component block; live copies are
// kept per GameFrame, and each replayed call is compared against the live copy of the same frame. Per-offset counts
// of differing dwords are logged every 200 comparisons. The hook runs on whichever thread takes the job.
namespace {
constexpr uintptr_t kSkelUpdate = 0x007741E0;
const uint8_t kOrig[6] = {0x55, 0x8B, 0xEC, 0x83, 0xEC, 0x60};  // push ebp; mov ebp, esp; sub esp, 0x60
constexpr uint32_t kMax = 0x800;
constexpr int kRing = 64;

struct Rec {
    uint32_t gf = 0xFFFFFFFF;
    uint32_t size = 0;
    uint8_t bytes[kMax];
};
Rec gLive[kRing][2];
uint32_t gInputsMatchedGf[2] = {~0u, ~0u};
// Per replayed frame and fighter, what differed from live (latest replay of that frame): 1 component input, 2 node,
// 4 root motion, 8 MoveActor inputs, 16 MoveActor result. Printed with each SyncTest desync (Describe).
uint8_t gFlags[kRing][2];
uint32_t gOrderGf[kRing];   // replayed frame whose skeleton-update order was compared
bool gOrderDiff[kRing];      // ... and the other fighter went first (Describe 'o')
uint32_t gFlagsGf[kRing][2];
void Flag(uint32_t gf, int p, uint8_t f) {
    if (gFlagsGf[gf % kRing][p] != gf) {
        gFlagsGf[gf % kRing][p] = gf;
        gFlags[gf % kRing][p] = 0;
    }
    gFlags[gf % kRing][p] |= f;
}  // gf whose replayed skeleton-update inputs (component, and nodes on first frames) matched live
// Anim tree nodes below the component (AnimTree comp+0x280 and the named-node table comp+0x290 {FName, node}, count
// +0x294): the component itself matched live on first replayed frames, so the next level of inputs is compared too.
constexpr int kNodes = 192;
constexpr uint32_t kNodeMax = 0x400;
struct NodeRec {
    uint32_t gf = 0xFFFFFFFF;
    int n = 0;
    uint32_t ptr[kNodes], size[kNodes];
    uint8_t bytes[kNodes][kNodeMax];
};
NodeRec gLiveNodes[kRing][2];
void CaptureNodes(uint32_t comp, NodeRec& r) {
    r.n = 0;
    uint32_t tree = 0, tab = 0, cnt = 0;
    CrashLog::SafeRead32(comp + 0x280, &tree);
    CrashLog::SafeRead32(comp + 0x290, &tab);
    CrashLog::SafeRead32(comp + 0x294, &cnt);
    auto add = [&](uint32_t o) {
        if (!o || r.n >= kNodes) return;
        for (int i = 0; i < r.n; ++i)
            if (r.ptr[i] == o) return;
        uint32_t info[2] = {0, 0};
        if (!CrashLog::SafeBlockInfo(reinterpret_cast<const void*>(o), info) || info[1] < 0x40) return;
        const uint32_t sz = (info[1] > kNodeMax ? kNodeMax : info[1]) & ~3u;
        r.ptr[r.n] = o;
        r.size[r.n] = sz;
        memcpy(r.bytes[r.n], reinterpret_cast<const void*>(o), sz);
        ++r.n;
    };
    add(tree);
    for (uint32_t i = 0; tab && i < cnt && i < 256; ++i) {
        uint32_t node = 0;
        // Every ticked node: the table entry heads a chain (+0x78), as TickAnimNodes 0x7627F0 walks it and the snapshot
        // restores it (unnamed sequence nodes are only in the chain).
        if (!CrashLog::SafeRead32(tab + i * 0xC + 8, &node)) continue;
        for (int guard = 0; node && guard < 512 && r.n < kNodes; ++guard) {
            add(node);
            if (!CrashLog::SafeRead32(node + 0x78, &node)) break;
        }
    }
    static int sCapLogged = 0;
    if (r.n >= kNodes && !sCapLogged++) Logger::Info("ANIMTRACE nodes: capture full at %d nodes (comp %08X)", kNodes, comp);
}
// Leg-IK binding inputs per fighter: the bound elements (comp TArray +0x384, 0x60 each: +0xC node, +0x20 strength,
// +0x24) and the weights TArray +0x39C ({float, float} each) that 0x0077BA40 turns into element strengths. Soak 13119
// S=3309 (2026-11-05): SkelControlPinLimb +0x40 on the first replayed frames was the abandoned future's value (live 1.0
// replay 0.9, live 0.9 replay 0.3, live 0.3 replay 0 and unbound) although bindstate restored that state: which of these
// already held the future's value (IKDIFF lines, at the load and at the first replayed skeleton update).
struct IkRec {
    uint32_t gf = 0xFFFFFFFF;
    uint32_t n384 = 0, n39c = 0, n3a8 = 0;
    uint32_t el[8][2];  // +0x20, +0x24
    uint32_t w[16];
};
IkRec gLiveIk[kRing][2], gLoadIk[2];
// The fighters' table nodes' +0x40 right after the last load, with the snapshot's saved value and span: traced soak 4
// (2026-11-05) found the elements and weights equal but node +0x40 at the future's value, and +0x40 never kept live by
// the pointer test. Restored and rewritten before the next skeleton update, or not restored at all?
// Also the node's strength source [node +0xC0] +0x20 (0x007601B0 copies it into +0x40 before each skeleton update):
// soak 7 (2026-11-05, 13131 gf 2614) had +0x40 restored right and the future's value at the first replayed update, with
// IKTRACE watching another node's source. Is the source in a snapshot span, and what did the load leave in it?
struct LoadNode { uint32_t node, after, saved; const char* span; uint32_t bound, srcAfter, srcSaved; const char* srcSpan; };
LoadNode gLoadNodes[2][8];
int gLoadNodeN[2];
uint32_t gIkWatch = 0;  // write-trace slot 3 (IkWatchAddr, below)
void CaptureLoadNodes(uint32_t comp, int p) {
    gLoadNodeN[p] = 0;
    uint32_t tab = 0, cnt = 0;
    if (!comp || !CrashLog::SafeRead32(comp + 0x290, &tab) || !CrashLog::SafeRead32(comp + 0x294, &cnt) || !tab) return;
    for (uint32_t i = 0; i < cnt && i < 32 && gLoadNodeN[p] < 8; ++i) {
        uint32_t node = 0;
        if (!CrashLog::SafeRead32(tab + i * 0xC + 8, &node) || !node) continue;
        LoadNode& ln = gLoadNodes[p][gLoadNodeN[p]++];
        ln.node = node;
        ln.after = ln.saved = ln.srcAfter = ln.srcSaved = 0xDEADDEAD;
        ln.bound = 0;
        ln.srcSpan = nullptr;
        CrashLog::SafeRead32(node + 0x40, &ln.after);
        ln.span = Snapshot::SavedDword(node + 0x40, &ln.saved);
        if (CrashLog::SafeRead32(node + 0xC0, &ln.bound) && ln.bound && !(ln.bound & 3) && CrashLog::SafeRead32(ln.bound + 0x20, &ln.srcAfter))
            ln.srcSpan = Snapshot::SavedDword(ln.bound + 0x20, &ln.srcSaved);
        else
            ln.bound = 0;
    }
}
void CaptureIk(uint32_t comp, uint32_t gf, IkRec& r) {
    r.gf = gf;
    r.n384 = r.n39c = r.n3a8 = 0;
    uint32_t d384 = 0, d39c = 0;
    if (!comp || !CrashLog::SafeRead32(comp + 0x384, &d384) || !CrashLog::SafeRead32(comp + 0x388, &r.n384) ||
        !CrashLog::SafeRead32(comp + 0x39C, &d39c) || !CrashLog::SafeRead32(comp + 0x3A0, &r.n39c) || !CrashLog::SafeRead32(comp + 0x3AC, &r.n3a8))
        return;
    for (uint32_t e = 0; e < r.n384 && e < 8; ++e) {
        r.el[e][0] = r.el[e][1] = 0;
        CrashLog::SafeRead32(d384 + e * 0x60 + 0x20, &r.el[e][0]);
        CrashLog::SafeRead32(d384 + e * 0x60 + 0x24, &r.el[e][1]);
    }
    for (uint32_t k = 0; k < r.n39c * 2 && k < 16; ++k) {
        r.w[k] = 0;
        CrashLog::SafeRead32(d39c + k * 4, &r.w[k]);
    }
}
int FormatIk(const IkRec& r, char* out, int len) {
    int n = snprintf(out, len, "n %u/%u/%u el", r.n384, r.n39c, r.n3a8);
    float f[2];
    for (uint32_t e = 0; e < r.n384 && e < 8 && n < len - 24; ++e) {
        memcpy(f, r.el[e], sizeof f);
        n += snprintf(out + n, len - n, " %.3f/%.3f", f[0], f[1]);
    }
    n += snprintf(out + n, len - n, " w");
    for (uint32_t k = 0; k + 1 < r.n39c * 2 && k + 1 < 16 && n < len - 24; k += 2) {
        memcpy(f, &r.w[k], sizeof f);
        n += snprintf(out + n, len - n, " %.3f/%.3f", f[0], f[1]);
    }
    return n;
}
uint32_t gReplaySeen[kRing][2];  // replay pass of the last compared call (first call per replayed frame and pass)
uint32_t gPass[2] = {1, 1};      // replay pass per fighter: +1 at each first replayed frame after a load
DWORD WINAPI ArmWatchThread(LPVOID a) {  // hardware watches are armed from another thread (all threads but the caller)
    const uintptr_t addr = *static_cast<uintptr_t*>(a);
    Logger::Info("WATCH arm %08X on all threads: %d armed", (unsigned)addr, CrashLog::WatchWriteAll(addr));
    return 0;
}
int gStepFirstLive[kRing], gStepFirstReplay[kRing];  // fighter whose anim step ran first
uint32_t gStepFirstLiveGf[kRing], gStepFirstReplayGf[kRing];
// Page hashes of the MK master image (4 KB pages) at FightUpdate entry (0) and exit (1), live per frame and compared
// on every replay: with the anim-step diff they show in which phase of a frame the MK state first diverged.
constexpr uint32_t kPages = 4096;  // up to 16 MB
struct PageRec {
    uint32_t gf = 0xFFFFFFFF;
    uint32_t h[kPages];
};
PageRec gPageLive[2][kRing];
uint32_t gPageDiff[2][kRing], gPageDiffGf[2][kRing], gPageFirst[2][kRing];
// Per replayed frame, at P1's anim step (0), at FightUpdate exit (1) and after the MK object tick 0x0045CAF0 (2, between
// the two): the noise-filtered MK image diff, printed on an 'r' frame.
char gImgLine[7][kRing][600];
uint32_t gImgLineGf[7][kRing], gImgLineN[7][kRing];
// Skeleton updates per fighter per frame instance (live or replay); final counts in a histogram per mode.
uint32_t gSkelCalls[2], gSkelCallsGf[2] = {~0u, ~0u};
bool gSkelCallsResim[2];
uint32_t gSkelHist[2][5];  // [replay][calls 0..3, 4+]
CRITICAL_SECTION gLock;
bool gInstalled = false;
volatile bool gOn = false;
std::map<uint32_t, uint32_t> gDiffs;  // offset -> comparisons in which that dword differed
uint32_t gCompares = 0, gDiffering = 0, gLogged = 0;

uint32_t GameFrame() { return *reinterpret_cast<const uint32_t*>(*reinterpret_cast<const uint32_t*>(0x00DEF4B4) + 0x70); }
uint32_t FighterComp(int p) {
    uint32_t obj = 0, actor = 0, comp = 0;
    if (!CrashLog::SafeRead32(0x00E89D7C + p * 0xC8C + 0x1C, &obj) || !obj || obj == 0xDEADBEEF) return 0;
    if (!CrashLog::SafeRead32(obj + 8, &actor) || !actor || !CrashLog::SafeRead32(actor + 0x244, &comp)) return 0;  // Pawn.Mesh
    return comp;
}
}  // namespace

extern "C" {
void* gMK9R_SkelUpdateTramp = nullptr;
void __cdecl MK9R_OnSkelUpdate(uint32_t comp) {
    if (!gOn || !comp) return;
    int p = -1;
    for (int i = 0; i < 2; ++i)
        if (FighterComp(i) == comp) p = i;
    if (p < 0) return;
    const uint32_t gf = GameFrame();
    const bool resim = Rollback::gIsResimulating;
    if (gSkelCallsGf[p] != gf || gSkelCallsResim[p] != resim) {
        if (gSkelCallsGf[p] != ~0u) {
            ++gSkelHist[gSkelCallsResim[p]][gSkelCalls[p] > 4 ? 4 : gSkelCalls[p]];
            static uint32_t sHistN = 0;
            if (++sHistN % 2000 == 0)
                Logger::Info("ANIMTRACE SKELCALLS per frame: live 1:%u 2:%u 3:%u 4+:%u, replay 1:%u 2:%u 3:%u 4+:%u", gSkelHist[0][1], gSkelHist[0][2],
                             gSkelHist[0][3], gSkelHist[0][4], gSkelHist[1][1], gSkelHist[1][2], gSkelHist[1][3], gSkelHist[1][4]);
        }
        gSkelCallsGf[p] = gf;
        gSkelCallsResim[p] = resim;
        gSkelCalls[p] = 0;
    }
    ++gSkelCalls[p];
    uint32_t info[2] = {0, 0};
    const uint32_t size = CrashLog::SafeBlockInfo(reinterpret_cast<const void*>(comp), info) && info[1] >= 0x100 && info[1] <= kMax
                              ? info[1] & ~3u
                              : 0x600;
    EnterCriticalSection(&gLock);
    // Which fighter's skeleton update starts first in this frame instance (live vs replay). The jobs run on worker
    // threads; if one fighter's evaluation reads the other's state, the order changes the result. Flag 32 'o'.
    {
        static uint32_t sLastGf = ~0u;
        static bool sLastResim = false;
        static int sLiveFirst[kRing];
        static uint32_t sLiveFirstGf[kRing];
        static uint32_t sOrderCmp = 0, sOrderDiff = 0;
        if (gf != sLastGf || resim != sLastResim) {  // first skeleton update of a new frame instance
            sLastGf = gf;
            sLastResim = resim;
            if (!resim) {
                sLiveFirst[gf % kRing] = p;
                sLiveFirstGf[gf % kRing] = gf;
            } else if (sLiveFirstGf[gf % kRing] == gf) {
                ++sOrderCmp;
                gOrderGf[gf % kRing] = gf;
                gOrderDiff[gf % kRing] = sLiveFirst[gf % kRing] != p;
                if (gOrderDiff[gf % kRing]) ++sOrderDiff;
                if (sOrderCmp % 500 == 0)
                    Logger::Info("ANIMTRACE ORDER %u replayed frames compared, %u with the other fighter's skeleton update first", sOrderCmp, sOrderDiff);
            }
        }
    }
    // Where do the updates run (thread, engine phase) and with which bound-anim count, live vs replay?
    static uint32_t sLastGf[2][2] = {{~0u, ~0u}, {~0u, ~0u}};
    static int sCallLogs = 0;
    if (sLastGf[resim][p] != gf) {
        sLastGf[resim][p] = gf;
        if (sCallLogs < 80 && (gf % 50) < 5) {
            ++sCallLogs;
            Logger::Info("ANIMTRACE call gf=%u %s P%d thread %s phase %u count +3AC=%u", gf, resim ? "replay" : "live", p + 1,
                         GetCurrentThreadId() == ObjectHooks::gGameThread ? "game" : "worker", (unsigned)ObjectHooks::gPhase,
                         *reinterpret_cast<const uint32_t*>(comp + 0x3AC));
        }
    }
    // Contents of the bone arrays the MK anim tree evaluation (0x7743FF) takes as inputs: +0x2C8 (bone atoms, 40 B),
    // +0x318, +0x378 (28 B per bone), +0x39C (8 B per bone). The component compare above sees only their pointers.
    {
        static const uint32_t kArrOff[4] = {0x2C8, 0x318, 0x378, 0x39C};
        struct ArrRec {
            uint32_t gf = 0xFFFFFFFF;
            uint32_t h[4], n[4];
        };
        static ArrRec sArr[kRing][2];
        static uint32_t sArrSeen[kRing][2], sArrCmp = 0, sArrDiff[4] = {0, 0, 0, 0};
        uint32_t h[4], cnt[4];
        for (int a = 0; a < 4; ++a) {
            uint32_t data = 0, num = 0;
            CrashLog::SafeRead32(comp + kArrOff[a], &data);
            CrashLog::SafeRead32(comp + kArrOff[a] + 4, &num);
            cnt[a] = num;
            uint32_t info[2] = {0, 0};
            uint32_t bytes = data && CrashLog::SafeBlockInfo(reinterpret_cast<const void*>(data), info) ? info[1] : 0;
            if (bytes > 0x10000) bytes = 0x10000;
            uint32_t x = 2166136261u;
            const uint32_t used = a == 0 ? num * 40 : a == 2 ? num * 28 : a == 3 ? num * 8 : bytes;
            const uint32_t len = used < bytes ? used : bytes;
            for (uint32_t i = 0; i + 4 <= len; i += 4) x = (x ^ *reinterpret_cast<const uint32_t*>(data + i)) * 16777619u;
            h[a] = x;
        }
        ArrRec& lr = sArr[gf % kRing][p];
        if (!resim) {
            if (lr.gf != gf) {
                lr.gf = gf;
                memcpy(lr.h, h, sizeof h);
                memcpy(lr.n, cnt, sizeof cnt);
            }
        } else if (lr.gf == gf && sArrSeen[gf % kRing][p] != gPass[p] + 1) {
            sArrSeen[gf % kRing][p] = gPass[p] + 1;
            ++sArrCmp;
            for (int a = 0; a < 4; ++a)
                if (lr.h[a] != h[a]) {
                    ++sArrDiff[a];
                    static int sArrLogs = 0;
                    if (sArrLogs++ < 60)
                        Logger::Info("ANIMTRACE ARRAY gf=%u P%d comp+0x%X: contents differ at skeleton-update entry (num live %u replay %u)", gf, p + 1,
                                     kArrOff[a], lr.n[a], cnt[a]);
                }
            if (sArrCmp % 500 == 0)
                Logger::Info("ANIMTRACE ARRAY %u replayed compared; differing +2C8 %u, +318 %u, +378 %u, +39C %u", sArrCmp, sArrDiff[0], sArrDiff[1],
                             sArrDiff[2], sArrDiff[3]);
        }
    }
    Rec& live = gLive[gf % kRing][p];
    if (!resim) {
        if (live.gf != gf) {  // first call of this live frame
            live.gf = gf;
            live.size = size;
            memcpy(live.bytes, reinterpret_cast<const void*>(comp), size);
            NodeRec& ln = gLiveNodes[gf % kRing][p];
            ln.gf = gf;
            CaptureNodes(comp, ln);
            CaptureIk(comp, gf, gLiveIk[gf % kRing][p]);
        }
    }
    // First replayed frame after a load (S+1): its entry state is the restored S plus one frame's work.
    static bool sPrevResim[2] = {false, false};
    static uint32_t sPrevGf[2] = {0, 0};
    // A new frame is a first replayed frame when the previous one was live or a later/equal frame (a new load).
    const bool newFrame = sPrevGf[p] != gf || sPrevResim[p] != resim;
    static bool sFirstOfPass[2] = {false, false};
    if (newFrame) sFirstOfPass[p] = resim && (!sPrevResim[p] || gf <= sPrevGf[p]);
    const bool first = sFirstOfPass[p];
    if (newFrame && first) ++gPass[p];
    if (newFrame) {
        sPrevResim[p] = resim;
        sPrevGf[p] = gf;
    }
    if (!resim) {
    } else if (live.gf == gf && gReplaySeen[gf % kRing][p] != gPass[p]) {
        gReplaySeen[gf % kRing][p] = gPass[p];
        gFlagsGf[gf % kRing][p] = gf;
        gFlags[gf % kRing][p] = 0;
        const uint8_t* now = reinterpret_cast<const uint8_t*>(comp);
        int n = 0, nTask = 0;
        char line[400];
        int ln = 0;
        for (uint32_t o = 0; o + 4 <= live.size; o += 4) {
            uint32_t a, b;
            memcpy(&a, live.bytes + o, 4);
            memcpy(&b, now + o, 4);
            if (a == b) continue;
            ++gDiffs[o]; if (o >= 0x658 && o < 0x680) ++nTask;
            if (++n <= 8 && ln < 340) ln += snprintf(line + ln, sizeof line - ln, " +%X:%08X>%08X", o, a, b);
        }
        bool nodesAny = false;
        // Node level, first replayed frames only: same node objects, which offsets differ.
        static uint32_t sNodeCmp = 0, sNodeDiff = 0;
        static std::map<std::string, uint32_t> sNodeOffs;  // "Class+off" -> count
        const NodeRec& lnr = gLiveNodes[gf % kRing][p];
        if (lnr.gf == gf) {
            static NodeRec rn;
            CaptureNodes(comp, rn);
            ++sNodeCmp;
            bool any = false;
            for (int i = 0; i < lnr.n; ++i) {
                int j = 0;
                while (j < rn.n && rn.ptr[j] != lnr.ptr[i]) ++j;
                if (j == rn.n) continue;
                const uint32_t sz = lnr.size[i] < rn.size[j] ? lnr.size[i] : rn.size[j];
                for (uint32_t o = 0x3C; o + 4 <= sz; o += 4) {  // after the UObject header
                    uint32_t a, b;
                    memcpy(&a, lnr.bytes[i] + o, 4);
                    memcpy(&b, rn.bytes[j] + o, 4);
                    if (a == b) continue;
                    any = true;
                    uint32_t cls = 0;
                    CrashLog::SafeRead32(lnr.ptr[i] + 0x30, &cls);
                    char cn[64];
                    Ue3Names::NameOf(cls, cn, sizeof cn);
                    char key[96];
                    snprintf(key, sizeof key, "%s+%X", cn, o);
                    ++sNodeOffs[key];
                    static int sNodeLogs = 0;
                    if (sNodeLogs++ < 40)
                        Logger::Info("ANIMTRACE NODE gf=%u P%d node #%d %08X %s +%X: live %08X replay %08X", gf, p + 1, i, lnr.ptr[i], cn, o, a, b);
                }
            }
            if (lnr.n != rn.n) {
                static int sCountLogs = 0;
                if (sCountLogs++ < 10) Logger::Info("ANIMTRACE NODE gf=%u P%d: %d nodes live, %d replay", gf, p + 1, lnr.n, rn.n);
            }
            if (any) ++sNodeDiff;
            nodesAny = any;
            const IkRec& li = gLiveIk[gf % kRing][p];
            static int sIkLogs = 0;
            if (any && first && li.gf == gf && sIkLogs < 120) {
                static IkRec ri;
                CaptureIk(comp, gf, ri);
                char a[300], b[300], c[300];
                FormatIk(li, a, sizeof a);
                FormatIk(ri, b, sizeof b);
                FormatIk(gLoadIk[p], c, sizeof c);
                ++sIkLogs;
                Logger::Info("IKDIFF gf=%u P%d first replayed skeleton update: live %s | replay %s | after the load (gf %u) %s", gf, p + 1, a, b,
                             gLoadIk[p].gf, c);
                // Each table node's +0x40: the snapshot's saved value (span), after the load, now, and the original's.
                char nl[1400];
                int nn = 0;
                for (int k = 0; k < gLoadNodeN[p] && nn < 1250; ++k) {
                    const LoadNode& ld = gLoadNodes[p][k];
                    uint32_t now = 0, lv = 0xDEADDEAD, srcNow = 0xDEADDEAD;
                    CrashLog::SafeRead32(ld.node + 0x40, &now);
                    for (int i = 0; i < lnr.n; ++i)
                        if (lnr.ptr[i] == ld.node && lnr.size[i] >= 0x44) memcpy(&lv, lnr.bytes[i] + 0x40, 4);
                    nn += snprintf(nl + nn, sizeof nl - nn, " %08X saved %08X(%s) load %08X now %08X live %08X", ld.node, ld.saved,
                                   ld.span ? ld.span : "no span", ld.after, now, lv);
                    if (ld.bound) {
                        CrashLog::SafeRead32(ld.bound + 0x20, &srcNow);
                        nn += snprintf(nl + nn, sizeof nl - nn, " src %08X saved %08X(%s) load %08X now %08X", ld.bound + 0x20, ld.srcSaved,
                                       ld.srcSpan ? ld.srcSpan : "no span", ld.srcAfter, srcNow);
                    }
                    nn += snprintf(nl + nn, sizeof nl - nn, ";");
                }
                Logger::Info("IKDIFF gf=%u P%d node +0x40:%s", gf, p + 1, nn ? nl : " no table nodes at the load");
                // Slot 3's writes this frame (the strength source), original and replay so far (s0-s2: LOCTRACE's actor).
                if (gIkWatch) {
                    uint32_t v = 0;
                    CrashLog::SafeRead32(gIkWatch, &v);
                    Logger::Info("IKTRACE gf=%u: watching %08X (now %08X)", gf, gIkWatch, v);
                    CrashLog::TraceLog(gf, false, "IKTRACE");
                    CrashLog::TraceLog(gf, true, "IKTRACE");
                }
            }
            if (sNodeCmp % 100 == 0) {
                std::string s;
                for (auto& kv : sNodeOffs)
                    if (kv.second >= 2) s += " " + kv.first + ":" + std::to_string(kv.second);
                Logger::Info("ANIMTRACE NODES first replayed frames: %u compared, %u with differing nodes;%s", sNodeCmp, sNodeDiff,
                             s.substr(0, 700).c_str());
            }
        }
        // Inputs matched: no differing component dword outside the task-graph bookkeeping +0x658..+0x67F, no node difference.
        if (n == nTask && !nodesAny) gInputsMatchedGf[p] = gf;
        if (n > nTask) Flag(gf, p, 1);
        if (nodesAny) Flag(gf, p, 2);
        ++gCompares;
        static uint32_t sFirst = 0, sFirstDiff = 0, sFirstCount = 0;
        if (first) {
            ++sFirst;
            if (n) ++sFirstDiff;
            uint32_t a, b;
            memcpy(&a, live.bytes + 0x3AC, 4);
            memcpy(&b, now + 0x3AC, 4);
            if (a != b) ++sFirstCount;
        }
        if (n) {
            ++gDiffering;
            static int sFirstLogged = 0;
            if (first && sFirstLogged++ < 40)
                Logger::Info("ANIMTRACE FIRST gf=%u P%d comp %08X: %d dwords differ:%s", gf, p + 1, comp, n, line);
            else if (gLogged++ < 30)
                Logger::Info("ANIMTRACE gf=%u P%d comp %08X%s: %d dwords differ at skeleton-update entry:%s", gf, p + 1, comp,
                             first ? " (first replayed frame)" : "", n, line);
        }
        if (gCompares % 200 == 0)
            Logger::Info("ANIMTRACE first replayed frames: %u compared, %u differed, %u with a different bound-anim count +3AC", sFirst,
                         sFirstDiff, sFirstCount);
        if (gCompares % 200 == 0) {
            char top[600];
            int tn = 0, shown = 0;
            for (auto it = gDiffs.begin(); it != gDiffs.end() && shown < 40; ++it)
                if (it->second * 20 >= gDiffering || it->second >= 3) {
                    tn += snprintf(top + tn, sizeof top - tn, " +%X:%u", it->first, it->second);
                    ++shown;
                }
            top[tn] = 0;
            Logger::Info("ANIMTRACE %u replayed skeleton updates compared, %u differed from live; offsets:%s", gCompares, gDiffering, top);
        }
    }
    LeaveCriticalSection(&gLock);
}
}

// Root motion (0x00762C10, EngineTick 0x0080F896: Location += rotation x comp.RootMotionDelta +0x5E0..+0x5E8). The
// delta is the animation evaluation's output: compared live vs replay per frame, split by whether that frame's
// skeleton-update inputs matched (gInputsMatched), so "different inputs" and "same inputs, different result" separate.
namespace {
struct RmRec {
    uint32_t gf = 0xFFFFFFFF;
    uint32_t d[3];
};
RmRec gRmLive[kRing][2];
uint32_t gRmSeen[kRing][2];
uint32_t gRmCmp = 0, gRmDiff = 0, gRmDiffSameInputs = 0, gRmCmpSameInputs = 0;
}  // namespace
extern "C" {
void* gMK9R_RootMotionTramp = nullptr;
void __cdecl MK9R_OnRootMotion(uint32_t comp) {
    if (!gOn || !comp) return;
    int p = -1;
    for (int i = 0; i < 2; ++i)
        if (FighterComp(i) == comp) p = i;
    if (p < 0) return;
    const uint32_t gf = GameFrame();
    const bool resim = Rollback::gIsResimulating;
    uint32_t d[3];
    memcpy(d, reinterpret_cast<const void*>(comp + 0x5E0), sizeof d);
    EnterCriticalSection(&gLock);
    // The bone atoms (+0x2C8, 40 B per bone) just after this frame's evaluation, and how many skeleton updates ran
    // for this fighter in this frame instance (gSkelCalls, counted in MK9R_OnSkelUpdate), live vs replay: at the
    // next skeleton-update entry the atoms differed on every replayed frame after the first.
    {
        uint32_t data = 0, num = 0;
        CrashLog::SafeRead32(comp + 0x2C8, &data);
        CrashLog::SafeRead32(comp + 0x2CC, &num);
        uint32_t info[2] = {0, 0};
        uint32_t len = data && CrashLog::SafeBlockInfo(reinterpret_cast<const void*>(data), info) ? info[1] : 0;
        if (len > num * 40) len = num * 40;
        uint32_t x = 2166136261u;
        for (uint32_t i = 0; i + 4 <= len; i += 4) x = (x ^ *reinterpret_cast<const uint32_t*>(data + i)) * 16777619u;
        struct AtomRec {
            uint32_t gf = 0xFFFFFFFF;
            uint32_t h, calls;
        };
        static AtomRec sAt[kRing][2];
        static uint32_t sAtSeen[kRing][2], sAtCmp = 0, sAtDiff = 0, sCallsDiff = 0;
        const uint32_t calls = gSkelCallsGf[p] == gf && gSkelCallsResim[p] == resim ? gSkelCalls[p] : 0;
        AtomRec& la = sAt[gf % kRing][p];
        if (!resim) {
            if (la.gf != gf) {
                la.gf = gf;
                la.h = x;
                la.calls = calls;
            }
        } else if (la.gf == gf && sAtSeen[gf % kRing][p] != gPass[p] + 7) {
            sAtSeen[gf % kRing][p] = gPass[p] + 7;
            ++sAtCmp;
            if (la.h != x) ++sAtDiff;
            if (la.calls != calls) ++sCallsDiff;
            static int sAtLogs = 0;
            if ((la.h != x || la.calls != calls) && sAtLogs++ < 40)
                Logger::Info("ANIMTRACE ATOMS gf=%u P%d after evaluation: %s, skeleton updates live %u replay %u", gf, p + 1,
                             la.h != x ? "DIFFER" : "same", la.calls, calls);
            if (sAtCmp % 500 == 0)
                Logger::Info("ANIMTRACE ATOMS %u replayed compared after evaluation: %u differ; %u with a different skeleton-update count", sAtCmp, sAtDiff,
                             sCallsDiff);
        }
    }
    RmRec& live = gRmLive[gf % kRing][p];
    if (!resim) {
        if (live.gf != gf) {
            live.gf = gf;
            memcpy(live.d, d, sizeof d);
        }
    } else if (live.gf == gf && gRmSeen[gf % kRing][p] != gPass[p]) {
        gRmSeen[gf % kRing][p] = gPass[p];
        const bool same = !memcmp(live.d, d, sizeof d);
        const bool inputsMatched = gInputsMatchedGf[p] == gf;
        ++gRmCmp;
        if (inputsMatched) ++gRmCmpSameInputs;
        if (!same) {
            Flag(gf, p, 4);
            ++gRmDiff;
            if (inputsMatched) ++gRmDiffSameInputs;
            static int logs = 0;
            if (logs++ < 30) {
                float a[3], b[3];
                memcpy(a, live.d, sizeof a);
                memcpy(b, d, sizeof b);
                if (inputsMatched && gImgLineGf[0][gf % kRing] == gf) {
                    Logger::Info("ANIMTRACE ROOTMOTION gf=%u: MK image at P1's anim step: %u non-noise dwords differ:%s", gf, gImgLineN[0][gf % kRing], gImgLine[0][gf % kRing]);
                    // FightUpdate exit of the previous frame and of this one (the fight logic runs before the anim jobs),
                    // and the MK object tick that follows it.
                    for (uint32_t g = gf - 1; g <= gf; ++g)
                        for (int pt = 1; pt <= 2; ++pt)
                            if (gImgLineGf[pt][g % kRing] == g)
                                Logger::Info("ANIMTRACE ROOTMOTION gf=%u: MK image at %s of frame %u: %u non-noise dwords differ:%s", gf,
                                             pt == 1 ? "FightUpdate exit" : "MK object tick exit", g, gImgLineN[pt][g % kRing], gImgLine[pt][g % kRing]);
                    // Where it started: the earlier replayed frames of this pass, and the MK object holding the first address.
                    char hist[600];
                    int hn = 0;
                    for (uint32_t g = gf - 4; g <= gf; ++g)
                        hn += snprintf(hist + hn, sizeof hist - hn, " %u:exit %s objtick %s step %s[pages in %s out %s]", g,
                                       gImgLineGf[1][g % kRing] == g ? std::to_string(gImgLineN[1][g % kRing]).c_str() : "-",
                                       gImgLineGf[2][g % kRing] == g ? std::to_string(gImgLineN[2][g % kRing]).c_str() : "-",
                                       gImgLineGf[0][g % kRing] == g ? std::to_string(gImgLineN[0][g % kRing]).c_str() : "-",
                                       gPageDiffGf[0][g % kRing] == g ? std::to_string(gPageDiff[0][g % kRing]).c_str() : "-",
                                       gPageDiffGf[1][g % kRing] == g ? std::to_string(gPageDiff[1][g % kRing]).c_str() : "-");
                    uint32_t first = 0;
                    sscanf(gImgLine[0][gf % kRing], " %x:", &first);
                    struct Find {
                        uint32_t addr, best;
                        const char* type;
                    } fd{first, 0, "?"};
                    ObjectHooks::ForEachLive(
                        [](uint32_t o, const char* ty, void* c) {
                            Find* f = static_cast<Find*>(c);
                            if (o <= f->addr && o > f->best && f->addr - o < 0x4000) {
                                f->best = o;
                                f->type = ty;
                            }
                        },
                        &fd);
                    // Writers of the first differing address from now on (distinct EIPs, all threads, live and replay).
                    // Ungated, the first differing address showed MK heap chunk headers / script VM scratch writers from any
                    // time (writer runs 04:52); now only writes between FightUpdate exit and P1's anim step count
                    // (gWatchPhase 1 live, 2 replay), the window where the image diverges (exitcmp runs 05:10).
                    constexpr bool kWatchFirstDiff = true;
                    static bool sArmed = false;
                    if (kWatchFirstDiff && !sArmed && first >= 0x10000) {
                        sArmed = true;
                        static uintptr_t sAddr;
                        sAddr = first & ~3u;
                        CrashLog::gWatchDistinct = true;
                        CrashLog::gWatchGated = true;
                        Logger::Info("ANIMTRACE ROOTMOTION: watching writes to %08X (all threads, distinct writers)", (unsigned)sAddr);
                        HANDLE th = CreateThread(nullptr, 0, ArmWatchThread, &sAddr, 0, nullptr);

                        if (th) CloseHandle(th);
                    }                    Logger::Info("ANIMTRACE ROOTMOTION gf=%u: anim step first live P%d replay P%d", gf,
                                 gStepFirstLiveGf[gf % kRing] == gf ? gStepFirstLive[gf % kRing] + 1 : 0,
                                 gStepFirstReplayGf[gf % kRing] == gf ? gStepFirstReplay[gf % kRing] + 1 : 0);
                    Logger::Info("ANIMTRACE ROOTMOTION gf=%u: image diff per frame%s; first address %08X in MK object %08X %s +0x%X", gf, hist, first, fd.best,
                                 fd.type ? fd.type : "?", fd.best ? first - fd.best : 0);
                }
                Logger::Info("ANIMTRACE ROOTMOTION gf=%u P%d: live (%.4f %.4f %.4f) replay (%.4f %.4f %.4f), skeleton inputs %s", gf, p + 1, a[0], a[1],
                             a[2], b[0], b[1], b[2], inputsMatched ? "MATCHED" : "differed");
            }
        }
        if (gRmCmp % 200 == 0)
            Logger::Info("ANIMTRACE ROOTMOTION %u replayed compared, %u differ; with matching skeleton inputs: %u compared, %u differ", gRmCmp, gRmDiff,
                         gRmCmpSameInputs, gRmDiffSameInputs);
    }
    LeaveCriticalSection(&gLock);
}
}

// UWorld::MoveActor 0x007A6F00 (thiscall: [esp+4] actor, [esp+8] FVector* delta; ret value = moved) for the fighters,
// called by MK's position update (0x0043369A). Live vs replay per frame and call index: start Location + delta in,
// Location out. Same inputs with a different result = the sweep met different collision state (other fighter's
// collision primitives and the octree are not part of the snapshot).
namespace {
struct MaRec {
    uint32_t in[6];   // start Location, delta
    uint32_t out[3];  // Location after
};
struct MaFrame {
    uint32_t gf = 0xFFFFFFFF;
    int n = 0;
    MaRec r[8];
};
MaFrame gMaLive[kRing][2], gMaCur[2];
bool gMaCurResim[2] = {false, false};
uint32_t gMaStackRet[16], gMaStackActor[16];
int gMaDepth = 0;
uint32_t gMaCmp = 0, gMaSameIn = 0, gMaSameInDiffOut = 0, gMaDiffIn = 0;
int FighterOfActor(uint32_t actor) {
    for (int i = 0; i < 2; ++i) {
        uint32_t obj = 0, a = 0;
        if (CrashLog::SafeRead32(0x00E89D7C + i * 0xC8C + 0x1C, &obj) && obj && obj != 0xDEADBEEF && CrashLog::SafeRead32(obj + 8, &a) && a == actor)
            return i;
    }
    return -1;
}
void MaFlush(int p) {  // a finished frame's records: keep (live) or compare (replay)
    MaFrame& c = gMaCur[p];
    if (c.gf == 0xFFFFFFFF) return;
    if (!gMaCurResim[p]) {
        gMaLive[c.gf % kRing][p] = c;
    } else {
        const MaFrame& l = gMaLive[c.gf % kRing][p];
        if (l.gf == c.gf) {
            const int n = l.n < c.n ? l.n : c.n;
            for (int k = 0; k < n; ++k) {
                ++gMaCmp;
                const bool sameIn = !memcmp(l.r[k].in, c.r[k].in, sizeof l.r[k].in);
                const bool sameOut = !memcmp(l.r[k].out, c.r[k].out, sizeof l.r[k].out);
                if (sameIn) {
                    ++gMaSameIn;
                    if (!sameOut) {
                        Flag(c.gf, p, 16);
                        ++gMaSameInDiffOut;
                        static int logs = 0;
                        if (logs++ < 30) {
                            float a[3], b[3], s[3], d[3];
                            memcpy(s, l.r[k].in, 12);
                            memcpy(d, l.r[k].in + 3, 12);
                            memcpy(a, l.r[k].out, 12);
                            memcpy(b, c.r[k].out, 12);
                            Logger::Info("ANIMTRACE MOVEACTOR gf=%u P%d call %d: same start (%.3f %.3f %.3f) + delta (%.4f %.4f %.4f) -> live (%.3f %.3f %.3f) replay (%.3f %.3f %.3f)",
                                         c.gf, p + 1, k, s[0], s[1], s[2], d[0], d[1], d[2], a[0], a[1], a[2], b[0], b[1], b[2]);
                        }
                    }
                } else {
                    ++gMaDiffIn;
                    Flag(c.gf, p, 8);
                    static int dlogs = 0;
                    if (dlogs++ < 30) {
                        float s0[3], s1[3], d0[3], d1[3];
                        memcpy(s0, l.r[k].in, 12);
                        memcpy(s1, c.r[k].in, 12);
                        memcpy(d0, l.r[k].in + 3, 12);
                        memcpy(d1, c.r[k].in + 3, 12);
                        const bool startSame = !memcmp(l.r[k].in, c.r[k].in, 12);
                        Logger::Info("ANIMTRACE MOVEACTOR gf=%u P%d call %d: %s; start live (%.4f %.4f %.4f) replay (%.4f %.4f %.4f); delta live (%.4f %.4f %.4f) replay (%.4f %.4f %.4f); root motion %s",
                                     c.gf, p + 1, k, startSame ? "same start, different delta (MK target)" : "different start", s0[0], s0[1], s0[2], s1[0], s1[1],
                                     s1[2], d0[0], d0[1], d0[2], d1[0], d1[1], d1[2], gRmSeen[c.gf % kRing][p] == c.gf ? "compared" : "?");
                    }
                }
                if (gMaCmp % 300 == 0)
                    Logger::Info("ANIMTRACE MOVEACTOR %u replayed calls compared: %u same inputs (%u of them with a different result), %u different inputs", gMaCmp,
                                 gMaSameIn, gMaSameInDiffOut, gMaDiffIn);
            }
        }
    }
    c.gf = 0xFFFFFFFF;
    c.n = 0;
}
}  // namespace
extern "C" void MK9R_MoveActorRet();
extern "C" {
void* gMK9R_MoveActorTramp = nullptr;
// Entry: returns the return address to use (ours when the actor is a fighter).
uint32_t __cdecl MK9R_OnMoveActorEnter(uint32_t actor, uint32_t deltaPtr, uint32_t ret) {
    if (!gOn || GetCurrentThreadId() != ObjectHooks::gGameThread || gMaDepth >= 16) return ret;
    const int p = FighterOfActor(actor);
    if (p < 0) return ret;
    const uint32_t gf = GameFrame();
    const bool resim = Rollback::gIsResimulating;
    if (gMaCur[p].gf != gf || gMaCurResim[p] != resim) {
        MaFlush(p);
        gMaCur[p].gf = gf;
        gMaCurResim[p] = resim;
    }
    MaFrame& c = gMaCur[p];
    if (c.n < 8) {
        memcpy(c.r[c.n].in, reinterpret_cast<const void*>(actor + 0xDC), 12);
        uint32_t d[3] = {0, 0, 0};
        for (int i = 0; i < 3; ++i) CrashLog::SafeRead32(deltaPtr + 4 * i, &d[i]);
        memcpy(c.r[c.n].in + 3, d, 12);
    }
    gMaStackRet[gMaDepth] = ret;
    gMaStackActor[gMaDepth] = actor;
    ++gMaDepth;
    return reinterpret_cast<uint32_t>(&MK9R_MoveActorRet);
}
// Exit: records the result, returns the caller's return address.
uint32_t __cdecl MK9R_OnMoveActorExit() {
    --gMaDepth;
    const uint32_t actor = gMaStackActor[gMaDepth];
    const int p = FighterOfActor(actor);
    if (p >= 0) {
        MaFrame& c = gMaCur[p];
        if (c.n < 8) memcpy(c.r[c.n].out, reinterpret_cast<const void*>(actor + 0xDC), 12);
        if (c.n < 8) ++c.n;
    }
    return gMaStackRet[gMaDepth];
}
}

asm(R"(
    .text
    .globl _MK9R_HkMoveActor
_MK9R_HkMoveActor:
    pushfl
    pushal
    movl 36(%esp), %eax     # return address (8 pushal regs + flags = 36 bytes)
    movl 40(%esp), %edx     # actor
    movl 44(%esp), %ecx     # delta pointer
    pushl %eax
    pushl %ecx
    pushl %edx
    call _MK9R_OnMoveActorEnter
    addl $12, %esp
    movl %eax, 36(%esp)     # possibly redirected return
    popal
    popfl
    jmp *_gMK9R_MoveActorTramp

    .globl _MK9R_MoveActorRet
_MK9R_MoveActorRet:
    pushl $0                # slot for the real return address
    pushfl
    pushal
    call _MK9R_OnMoveActorExit
    movl %eax, 36(%esp)
    popal
    popfl
    ret
)");
extern "C" void MK9R_HkMoveActor();

// Animation time step (component tick 0x00773ED0 -> call 0x004CBA60 at 0x00773F90, cdecl (mkTree, dt * rate, comp)):
// rate is vt[2]() of an MK object reached from the owner's MK object +0xA0 (class 0xDE8260), 1.0 otherwise. The MK
// anim tree and TickAnimNodes advance by it, so a replay with a different rate gives a different root motion from
// identical node state. Compared live vs replay per fighter (Describe 'd').
namespace {
struct StepRec {
    uint32_t gf = 0xFFFFFFFF;
    uint32_t step;
    uint32_t host;  // HostFrame [0xDEF4B4]+0x6C at the call: the MK anim nodes tick once per stamp (+0x2C)
    uint32_t tree;
    uint32_t bytes[0x40];  // first 0x100 bytes of the MK anim tree object at the call
};
void ReadTree(uint32_t tree, uint32_t* out) {
    for (int i = 0; i < 0x40; ++i)
        if (!tree || !CrashLog::SafeRead32(tree + 4 * i, &out[i])) out[i] = 0xCDCDCDCD;
}
const char* WhereMk(uint32_t p) {
    const uint32_t m = *reinterpret_cast<const uint32_t*>(0x00ED53F0);
    const uint32_t mb = m ? *reinterpret_cast<const uint32_t*>(m + 0x34) : 0, ms = m ? *reinterpret_cast<const uint32_t*>(m + 0x24) : 0;
    return p >= mb && p < mb + ms ? "image" : (p ? "OUTSIDE" : "null");
}
StepRec gStepLive[kRing][2];
uint32_t gStepSeen[kRing][2];
uint32_t gStepCmp = 0, gStepDiff = 0;
}  // namespace
extern "C" {
// The whole MK heap image at P1's anim step, live vs replay (ring of 8 live copies; synctest distance 4): what the
// animation evaluation may read in MK memory when the component, nodes, step and tree root all matched.
namespace {
constexpr int kImgRing = 6;
uint8_t* gImg[7][kImgRing];
uint32_t gImgGf[7][kImgRing], gImgTotal = 0;
MKHeap::Region gImgReg[8];
int gImgN = 0;
uint32_t gImgSeen[7][kRing];
std::map<uint32_t, uint32_t> gImgAddrDiff[7];  // address -> replayed frames on which it differed
uint32_t gImgCmp[7];
// Full copies of the MK master image live, compared on replay; pt 0 = P1's anim step, 1 = FightUpdate exit.
void ImageCompare(int pt, uint32_t gf, bool resim) {
    MKHeap::Region r[8];
    const int n = MKHeap::MasterRanges(r, 8);
    if (n <= 0) return;
    uint32_t total = 0;
    for (int i = 0; i < n; ++i) total += r[i].size;
    if (total > 0x2000000) return;
    if (n != gImgN || memcmp(r, gImgReg, n * sizeof r[0])) {
        for (auto& row : gImg)
            for (auto& x : row) {
                delete[] x;
                x = nullptr;
            }
        for (auto& row : gImgGf)
            for (auto& g : row) g = 0xFFFFFFFF;
        memcpy(gImgReg, r, n * sizeof r[0]);
        gImgN = n;
        gImgTotal = total;
        Logger::Info("ANIMTRACE IMAGE: %d master ranges, %u bytes (first %08X+%X)", n, total, (unsigned)r[0].base, r[0].size);
    }
    const int slot = gf % kImgRing;
    if (!resim) {
        if (gImgGf[pt][slot] == gf) return;
        if (!gImg[pt][slot]) gImg[pt][slot] = new (std::nothrow) uint8_t[gImgTotal];
        if (!gImg[pt][slot]) return;
        uint32_t at = 0;
        for (int i = 0; i < gImgN; ++i, at += gImgReg[i - 1].size) memcpy(gImg[pt][slot] + at, reinterpret_cast<const void*>(gImgReg[i].base), gImgReg[i].size);
        gImgGf[pt][slot] = gf;
        return;
    }
    if (gImgGf[pt][slot] != gf || gImgSeen[pt][gf % kRing] == gPass[0]) return;
    gImgSeen[pt][gf % kRing] = gPass[0];
    uint32_t nd = 0, at = 0;
    char line[600];
    int ln = 0;
    for (int k = 0; k < gImgN; ++k) {
        const uint32_t* a = reinterpret_cast<const uint32_t*>(gImg[pt][slot] + at);
        const uint32_t* c = reinterpret_cast<const uint32_t*>(gImgReg[k].base);
        for (uint32_t i = 0; i < gImgReg[k].size / 4; ++i) {
            if (a[i] == c[i]) continue;
            const uint32_t addr = (uint32_t)(gImgReg[k].base + i * 4);
            // Noise: addresses that differ on more than a fifth of replayed frames (counters written between ticks).
            uint32_t& cnt = gImgAddrDiff[pt][addr];
            ++cnt;
            if (gImgCmp[pt] >= 100 && cnt * 5 > gImgCmp[pt]) continue;
            if (++nd <= 16 && ln < 520) ln += snprintf(line + ln, sizeof line - ln, " %08X:%08X>%08X", addr, a[i], c[i]);
        }
        at += gImgReg[k].size;
    }
    line[ln] = 0;
    // What the noise filter hides: once per point, after 1000 compares, the addresses it treats as noise.
    if (gImgCmp[pt] == 1000) {
        std::vector<std::pair<uint32_t, uint32_t>> noise;
        for (const auto& kv : gImgAddrDiff[pt])
            if (kv.second * 5 > gImgCmp[pt]) noise.push_back({kv.second, kv.first});
        std::sort(noise.begin(), noise.end(), [](const auto& a, const auto& b) { return a.second < b.second; });
        std::string s;
        for (size_t q = 0; q < noise.size() && s.size() < 1500; ++q) {
            char b[40];
            snprintf(b, sizeof b, " %08X:%u", noise[q].second, noise[q].first);
            s += b;
        }
        Logger::Info("ANIMTRACE IMAGE noise at point %d: %u addresses differ on >20%% of %u replayed frames:%s", pt, (unsigned)noise.size(),
                     gImgCmp[pt], s.c_str());
    }
    memcpy(gImgLine[pt][gf % kRing], line, ln + 1);
    gImgLineGf[pt][gf % kRing] = gf;
    gImgLineN[pt][gf % kRing] = nd;
    static uint32_t sCmp[7], sDiff[7];
    static int sLogs = 0;
    ++sCmp[pt];
    ++gImgCmp[pt];
    if (nd && gImgCmp[pt] > 100) {
        ++sDiff[pt];
        if (pt == 0) Flag(gf, 0, 128);
        if (pt == 0 && sLogs++ < 100) Logger::Info("ANIMTRACE IMAGE gf=%u: %u non-noise dwords differ at P1's anim step:%s", gf, nd, line);
    }
    if (sCmp[pt] % 250 == 0)
        Logger::Info("ANIMTRACE IMAGE %u replayed frames compared, %u with MK image differences at %s", sCmp[pt], sDiff[pt],
                     pt == 0 ? "P1's anim step" : pt == 1 ? "FightUpdate exit" : pt == 2 ? "the MK object tick exit" : pt == 3 ? "FightUpdate entry" : pt == 4 ? "WorldTick entry" : pt == 5 ? "EngineTick entry" : "EngineTick exit");
}}  // namespace
void __cdecl MK9R_OnAnimStep(uint32_t tree, uint32_t stepBits, uint32_t comp) {
    if (!gOn || !comp) return;
    int p = -1;
    for (int i = 0; i < 2; ++i)
        if (FighterComp(i) == comp) p = i;
    if (p < 0) return;
    const uint32_t gf = GameFrame();
    const bool resim = Rollback::gIsResimulating;
    EnterCriticalSection(&gLock);
    // Which fighter's anim step ran first in this frame instance, live vs replay (the MK anim trees advance here).
    {
        static uint32_t sGf = ~0u;
        static bool sResim = false;
        if (gf != sGf || resim != sResim) {
            sGf = gf;
            sResim = resim;
            if (!resim) {
                gStepFirstLive[gf % kRing] = p;
                gStepFirstLiveGf[gf % kRing] = gf;
            } else {
                gStepFirstReplay[gf % kRing] = p;
                gStepFirstReplayGf[gf % kRing] = gf;
            }
        }
    }
    if (p == 0) {
        CrashLog::gWatchPhase = 0;  // end of the FightUpdate-exit -> P1 anim step window
        ImageCompare(0, gf, resim);
    }
    StepRec& live = gStepLive[gf % kRing][p];
    if (!resim) {
        if (live.gf != gf) {
            live.gf = gf;
            live.step = stepBits;
            live.host = *reinterpret_cast<const uint32_t*>(*reinterpret_cast<const uint32_t*>(0x00DEF4B4) + 0x6C);
            live.tree = tree;
            ReadTree(tree, live.bytes);
            static int sWhere = 0;
            if (sWhere < 6 && gf % 600 < 2) {
                ++sWhere;
                Logger::Info("ANIMTRACE MKTREE gf=%u P%d tree %08X (%s) +0xC %u +0x14 %u +0x2C %u HostFrame %u", gf, p + 1, tree, WhereMk(tree),
                             live.bytes[3], live.bytes[5], live.bytes[11],
                             *reinterpret_cast<const uint32_t*>(*reinterpret_cast<const uint32_t*>(0x00DEF4B4) + 0x6C));
            }
        }
    } else if (live.gf == gf && gStepSeen[gf % kRing][p] != gPass[p]) {
        gStepSeen[gf % kRing][p] = gPass[p];
        ++gStepCmp;
        {
            const uint32_t host = *reinterpret_cast<const uint32_t*>(*reinterpret_cast<const uint32_t*>(0x00DEF4B4) + 0x6C);
            static uint32_t sHostDiff = 0;
            static int sHostLogs = 0;
            if (host != live.host) {
                ++sHostDiff;
                if (sHostLogs++ < 30) Logger::Info("ANIMTRACE HOSTFRAME gf=%u P%d at the anim step: live %u replay %u", gf, p + 1, live.host, host);
            }
            if (gStepCmp % 500 == 0) Logger::Info("ANIMTRACE HOSTFRAME %u replayed compared, %u with a different HostFrame at the anim step", gStepCmp, sHostDiff);
        }
        // The MK anim tree object itself, before this frame's advance.
        static uint32_t sTreeCmp = 0, sTreeDiff = 0;
        static std::map<uint32_t, uint32_t> sTreeOffs;
        ++sTreeCmp;
        if (tree == live.tree) {
            uint32_t now[0x40];
            ReadTree(tree, now);
            bool any = false;
            for (int i = 0; i < 0x40; ++i)
                if (now[i] != live.bytes[i]) {
                    any = true;
                    ++sTreeOffs[i * 4];
                    static int sTreeLogs = 0;
                    if (sTreeLogs++ < 40)
                        Logger::Info("ANIMTRACE MKTREE gf=%u P%d tree %08X +%X: live %08X replay %08X", gf, p + 1, tree, i * 4, live.bytes[i], now[i]);
                }
            if (any) {
                ++sTreeDiff;
                Flag(gf, p, 128);
            }
        } else {
            static int sTreeMoved = 0;
            if (sTreeMoved++ < 10) Logger::Info("ANIMTRACE MKTREE gf=%u P%d: tree %08X live, %08X replay", gf, p + 1, live.tree, tree);
        }
        if (sTreeCmp % 500 == 0) {
            std::string s;
            for (auto& kv : sTreeOffs) s += " +" + std::to_string(kv.first) + ":" + std::to_string(kv.second);
            Logger::Info("ANIMTRACE MKTREE %u replayed compared, %u differ; decimal offsets:%s", sTreeCmp, sTreeDiff, s.substr(0, 500).c_str());
        }
        if (live.step != stepBits) {
            ++gStepDiff;
            Flag(gf, p, 64);
            static int logs = 0;
            if (logs++ < 40) {
                float a, b;
                memcpy(&a, &live.step, 4);
                memcpy(&b, &stepBits, 4);
                Logger::Info("ANIMTRACE STEP gf=%u P%d: live dt*rate %.6f (%08X) replay %.6f (%08X), ratio %.4f, MK tree %08X", gf, p + 1, a,
                             live.step, b, stepBits, a != 0.0f ? b / a : 0.0f, tree);
            }
        }
        if (gStepCmp % 500 == 0) {
            uint32_t same = 0, diff = 0;
            for (int g = 0; g < kRing; ++g)
                if (gStepFirstLiveGf[g] == gStepFirstReplayGf[g] && gStepFirstLiveGf[g] != 0) (gStepFirstLive[g] == gStepFirstReplay[g] ? same : diff)++;
            Logger::Info("ANIMTRACE STEP %u replayed compared, %u with a different dt*rate; recent order same %u, different %u", gStepCmp, gStepDiff, same, diff);
        }
    }
    LeaveCriticalSection(&gLock);
}
// Hit tests of the MK object tick 0x008A7300 -> 0x008A46E0 (the post-anim pass over the MK tick list, EngineTick
// 0x0080F78F): four cdecl tests, their result starts script 0xCE (0x0045D960) on a hit. A replay started that
// script where live did not (spawn_1 2026-10-18, load 3188: MKScriptVM born in the replay only, every anim input
// matched). Records each call's arguments, pointed-to vectors, target bytes and result, live vs replay.
struct HitRec {
    uint32_t site, ord, args[6], vec[2][4], target[16], result;  // ord: n-th call of this site in the frame
};
struct HitFrame {
    uint32_t gf = 0xFFFFFFFF, n = 0;
    HitRec r[128];  // sites 4/5 alone make ~13 calls a frame; 32 records dropped later hit tests (soak 10201..10212)
};
// Site 7, the hit check itself (0x008A46E0, called per object at 0x008A73A9): the object's gate fields before the call.
// +0x194 selects the test family (switch at 0x008A493B; untraced tests 8..12 below).
uint32_t gHitObj7[6];
void __cdecl MK9R_PreHitCheck(uint32_t obj) {
    gHitObj7[0] = obj;
    const uint32_t offs[5] = {0x4, 0x194, 0x104, 0x188, 0x18C};
    for (int i = 0; i < 5; ++i) gHitObj7[1 + i] = 0xEEEEEEEE, CrashLog::SafeRead32(obj + offs[i], &gHitObj7[1 + i]);
}
// Site 13, the early-out 0x0089E220 at 0x008A491F (thiscall, 3 stack arguments popped by the callee; nonzero = no
// test): ecx and the arguments, copied before the call.
uint32_t gHitArgs13[4];
void __cdecl MK9R_SaveArgs13(uint32_t ecx, const uint32_t* a) {
    gHitArgs13[0] = ecx;
    for (int i = 0; i < 3; ++i) gHitArgs13[1 + i] = a[i];
}
// Location write trace (soak 12704 S=867: a fighter-attached collision actor's Location was restored at the load,
// yet at the first replayed hit check it held the abandoned future's position, and the replay then trailed live by
// one frame). Each load arms the CrashLog write trace on the Location of a restored MK actor based on a fighter
// (re-armed only when that actor changes, so live frames are recorded too); the first replayed check that finds it
// moved since the load logs both passes' writes of that frame (LOCMOVED).
uint32_t gLocActor = 0, gLocLoadSeq = 0, gLocCheckedSeq = 0;
// The last actor a live hit check (site 7) saw with a Base: in soak 13011 S=2063 the moved actor's Base (636407C0) was
// not a fighter actor (an attached SkeletalMeshActor), so the fighter-only pick never armed (0 LOCLOAD/LOCMOVED lines).
uint32_t gLastHitActor = 0;
uintptr_t gLocAddrs[4];
DWORD WINAPI LocArmThread(LPVOID) {  // TraceArm suspends the game thread, which waits for this thread
    CrashLog::TraceArm(ObjectHooks::gGameThread, gLocAddrs);
    return 0;
}
// Write-trace slot 3: a fighter SkelControlPinLimb's strength source, [node +0xC0] +0x20 (0x007601B0 copies it into
// node +0x40 every frame). Traced soak 5 (2026-11-05, IKDIFF): node +0x40 was restored (saved = after the load = the
// original's) and then held the abandoned future's value at the first replayed skeleton update; the bound objects
// are 0x48 apart, not bindstate's 0x60 elements. Who writes the source in the replayed frame (IKTRACE)?
uint32_t IkWatchAddr() {
    // Prefer (soak 7, 13131: the first PinLimb's source never differed) a PinLimb whose source after the load is not the
    // node's restored +0x40: no snapshot span holds the source first, then any such node.
    uint32_t best = 0;
    int bestRank = 0;
    for (int p = 0; p < 2; ++p)
        for (int k = 0; k < gLoadNodeN[p]; ++k) {
            const LoadNode& ld = gLoadNodes[p][k];
            uint32_t cls = 0;
            char cn[64] = "";
            if (!ld.bound || ld.srcAfter == ld.after || !CrashLog::SafeRead32(ld.node + 0x30, &cls)) continue;
            Ue3Names::NameOf(cls, cn, sizeof cn);
            if (strncmp(cn, "SkelControlPinLimb", 18)) continue;
            static int sLogged = 0;
            if (sLogged++ < 40)
                Logger::Info("IKSRC load gf=%u P%d node %08X +0x40 %08X (saved %08X %s), source %08X holds %08X (saved %08X %s)", GameFrame(), p + 1,
                             ld.node, ld.after, ld.saved, ld.span ? ld.span : "no span", ld.bound + 0x20, ld.srcAfter, ld.srcSaved,
                             ld.srcSpan ? ld.srcSpan : "no span");
            const int rank = ld.srcSpan ? 1 : 2;
            if (rank > bestRank) bestRank = rank, best = ld.bound + 0x20;
        }
    if (best) return best;
    for (int p = 0; p < 2; ++p) {
        uint32_t comp = FighterComp(p), tab = 0, cnt = 0;
        if (!comp || !CrashLog::SafeRead32(comp + 0x290, &tab) || !CrashLog::SafeRead32(comp + 0x294, &cnt) || !tab) continue;
        for (uint32_t i = 0; i < cnt && i < 32; ++i) {
            uint32_t node = 0, bound = 0, cls = 0;
            if (!CrashLog::SafeRead32(tab + i * 0xC + 8, &node) || !node || !CrashLog::SafeRead32(node + 0xC0, &bound) || !bound || (bound & 3) ||
                !CrashLog::SafeRead32(node + 0x30, &cls))
                continue;
            char cn[64] = "";
            Ue3Names::NameOf(cls, cn, sizeof cn);
            uint32_t probe = 0;
            if (!strncmp(cn, "SkelControlPinLimb", 18) && CrashLog::SafeRead32(bound + 0x20, &probe)) return bound + 0x20;
        }
    }
    return 0;
}
// The inputs of an attached actor's placement (its motion controller 0x0078D1B0 -> 0x0078D0B0 -> 0x0078B240 ->
// 0x00789060): its record in Base->Attached (+0x68 data, +0x6C count, 0x60 each, +0 the actor) with the relative
// location (+0x04..+0x0C) and the target (+0x1C: +0x20 type byte, 0 socket / 1 bone / 2 actor, +0x30 the component,
// +0x28 the bone name); the component's SpaceBases (+0x2F4 data, +0x2F8 count; the triple buffer +0x300/+0x30C) and its
// LocalToWorld translation (+0xA0..+0xA8). Soak 7 (2026-11-05, 13129 S=2712 and 13130 S=2156): a hit box kept its
// restored Location at the first replayed check while the original had followed its base prop (LOCSTILL), then trailed
// by one frame and missed: was the base's pose at that check the original's (the order of the updates differs) or not?
struct PlaceCtx {
    uint32_t rec = 0, type = 0, comp = 0, sb = 0, sbN = 0, sbHash = 0, m1 = 0, m2 = 0, aflags = 0, bflags = 0;
    float l2w[3] = {0, 0, 0}, rel[3] = {0, 0, 0};
};
uint32_t AttachRecord(uint32_t actor, uint32_t base) {
    uint32_t data = 0, cnt = 0, a = 0;
    if (!actor || !base || !CrashLog::SafeRead32(base + 0x68, &data) || !CrashLog::SafeRead32(base + 0x6C, &cnt) || !data || cnt > 64) return 0;
    for (uint32_t i = 0; i < cnt; ++i)
        if (CrashLog::SafeRead32(data + i * 0x60, &a) && a == actor) return data + i * 0x60;
    return 0;
}
void CapturePlace(uint32_t actor, uint32_t base, PlaceCtx& c) {
    c = PlaceCtx{};
    if (actor) CrashLog::SafeRead32(actor + 0x80, &c.aflags);
    if (base) CrashLog::SafeRead32(base + 0x80, &c.bflags);
    c.rec = AttachRecord(actor, base);
    if (!c.rec) return;
    uint32_t w = 0;
    if (CrashLog::SafeRead32(c.rec + 0x20, &w)) c.type = w & 0xFF;
    for (int k = 0; k < 3; ++k) CrashLog::SafeRead32(c.rec + 4 + 4 * k, reinterpret_cast<uint32_t*>(&c.rel[k]));
    if (!CrashLog::SafeRead32(c.rec + 0x30, &c.comp) || !c.comp || (c.comp & 3)) {
        c.comp = 0;
        return;
    }
    for (int k = 0; k < 3; ++k) CrashLog::SafeRead32(c.comp + 0xA0 + 4 * k, reinterpret_cast<uint32_t*>(&c.l2w[k]));
    CrashLog::SafeRead32(c.comp + 0x2F4, &c.sb);
    CrashLog::SafeRead32(c.comp + 0x2F8, &c.sbN);
    CrashLog::SafeRead32(c.comp + 0x300, &c.m1);
    CrashLog::SafeRead32(c.comp + 0x30C, &c.m2);
    unsigned blk[2] = {0, 0};
    uint32_t h = 2166136261u;
    // A live GMalloc block at least that large: committed, read directly (every based check in both passes).
    if (c.sb && c.sbN && c.sbN <= 512 && CrashLog::SafeBlockInfo(reinterpret_cast<const void*>(c.sb), blk) && blk[1] >= c.sbN * 0x40) {
        const uint32_t* m = reinterpret_cast<const uint32_t*>(c.sb);
        for (uint32_t i = 0; i < c.sbN * 16; ++i) h = (h ^ m[i]) * 16777619u;
    } else {
        h = 0;
    }
    c.sbHash = h;
}
int FormatPlace(const PlaceCtx& c, char* out, size_t n) {
    if (!c.rec) return snprintf(out, n, "no record (flags %08X, base %08X)", c.aflags, c.bflags);
    return snprintf(out, n, "flags %08X base %08X rec %08X type %u rel %.3f,%.3f,%.3f comp %08X sb %08X/%u hash %08X m1 %08X m2 %08X l2w %.3f,%.3f,%.3f",
                    c.aflags, c.bflags, c.rec, c.type, c.rel[0], c.rel[1], c.rel[2], c.comp, c.sb, c.sbN, c.sbHash, c.m1, c.m2, c.l2w[0], c.l2w[1],
                    c.l2w[2]);
}
// Slots 0-2: the actor's Location X, and, when its record targets a component, that component's SpaceBases header
// (rewritten by each pose update / buffer rotation) and LocalToWorld X, else the actor's Location Y and Z: the order of
// the base's pose update and the actor's placement in each pass (LOCSTILL/LOCMOVED/LOCTRACE print every write).
uint32_t gLocComp = 0;
void LocArm(uint32_t actor) {
    uint32_t base = 0, comp = 0;
    if (actor && CrashLog::SafeRead32(actor + 0x60, &base) && base) {
        PlaceCtx c;
        CapturePlace(actor, base, c);
        comp = c.comp;
    }
    // Soak 8 (2026-11-05, Jax v Jade): with the base component's pose and LocalToWorld equal in both passes at the first
    // replayed check, the hit box's placement write was simply missing in the replay. Order of the ticks: slot 1 the hit
    // box's flags +0x80 (bit 0x40 flips every tick), slot 2 its Base's flags, slot 3 the base component's LocalToWorld
    // X (the IK source watch is off in this build; IKSRC lines still name it).
    gLocAddrs[0] = actor ? actor + 0xDC : 0;
    gLocAddrs[1] = actor ? actor + 0x80 : 0;
    gLocAddrs[2] = base ? base + 0x80 : 0;
    gLocComp = comp;
    static int sArms = 0;
    if (actor && sArms++ < 60)
        Logger::Info("LOCARM gf=%u %s: actor %08X base %08X comp %08X: s0 actor X, s1 actor +0x80, s2 base +0x80, s3 %s", GameFrame(),
                     Rollback::gIsResimulating ? "replay" : "live", actor, base, comp, comp ? "base comp LocalToWorld X" : "IK source");
    gLocAddrs[3] = comp ? comp + 0xA0 : gIkWatch;
    HANDLE h = CreateThread(nullptr, 0, LocArmThread, nullptr, 0, nullptr);
    if (!h) return;
    WaitForSingleObject(h, 2000);
    CloseHandle(h);
    gLocActor = actor;
}
// The original pass's Location at each live hit check, by (frame, MK object id): LOCSTILL compares the first replayed
// check with it (traced soak 13119 S=1966, 2026-11-05: the restored hit box did not move on the first replayed frame
// while the original moved it ~29 units, so the replay trailed by one frame and hit a frame late; no LOCMOVED).
struct LiveLoc {
    uint32_t gf = 0xFFFFFFFF, id = 0;
    float loc[3] = {0, 0, 0};
    PlaceCtx place;  // the placement inputs at that check (actors with a Base)
};
uint32_t gLocSeenGf = 0;  // the last live frame whose hit checks saw the armed actor
uint32_t LiveLocSlot(uint32_t gf, uint32_t id);
LiveLoc gLiveLoc[256];
// LOCPLACE: the placement inputs at the original's check of this frame and now (replay); the slots' armed addresses.
void LogPlace(uint32_t gf, uint32_t id, uint32_t act, uint32_t base) {
    const LiveLoc& ll = gLiveLoc[LiveLocSlot(gf, id)];
    PlaceCtx now;
    CapturePlace(act, base, now);
    char a[320], b[320];
    if (ll.gf == gf && ll.id == id) FormatPlace(ll.place, a, sizeof a);
    else snprintf(a, sizeof a, "no live check recorded");
    FormatPlace(now, b, sizeof b);
    Logger::Info("LOCPLACE gf=%u actor %08X: original %s | replay %s | armed on actor %08X comp %08X", gf, act, a, b, gLocActor, gLocComp);
}
uint32_t LiveLocSlot(uint32_t gf, uint32_t id) { return ((gf * 2654435761u) ^ id ^ (id >> 8)) & 255; }
HitFrame gHitLive[kRing];
uint32_t gHitReplayGf = 0xFFFFFFFF, gHitReplayN[16] = {}, gHitPass = 0xFFFFFFFF;
uint32_t gHitCmp = 0, gHitDiff = 0;
void __cdecl MK9R_OnHit(uint32_t site, const uint32_t* args, uint32_t result) {
    if (!gOn) return;
    const uint32_t gf = GameFrame();
    const bool resim = Rollback::gIsResimulating;
    HitRec h{};
    h.site = site;
    h.result = result;
    if (site < 4 || (site >= 8 && site <= 12)) {  // cdecl tests (target, &vec, &vec or float, floats...)
        for (int i = 0; i < 6; ++i) h.args[i] = args[i];
        for (int v = 0; v < 2; ++v)
            for (int i = 0; i < 4; ++i) CrashLog::SafeRead32(args[1 + v] + 4 * i, &h.vec[v][i]);
        for (int i = 0; i < 16; ++i) CrashLog::SafeRead32(args[0] + 4 * i, &h.target[i]);
    } else if (site == 5) {
        // 0x008A9510 (thiscall, 7 stack arguments copied before the call): args[0] is the move-request entry; vec0[0]
        // its address, vec0[1..3]/vec1 its +0x04..+0x1F, tgt its +0x60..+0x9F (active +0x6C, type +0x70, parameters).
        for (int i = 0; i < 6; ++i) h.args[i] = args[1 + i];
        h.vec[0][0] = args[0];
        for (int v = 0; v < 2; ++v)
            for (int i = v ? 0 : 1; i < 4; ++i) CrashLog::SafeRead32(args[0] + 0x10 * v + 4 * i, &h.vec[v][i]);
        for (int i = 0; i < 16; ++i) CrashLog::SafeRead32(args[0] + 0x60 + 4 * i, &h.target[i]);
        static uint32_t seen[8], nSeen = 0;
        if (nSeen < 8 && std::find(seen, seen + nSeen, args[0]) == seen + nSeen) {
            seen[nSeen++] = args[0];
            uint32_t blk[2] = {0, 0}, w[8] = {};
            const bool inBlock = CrashLog::SafeBlockInfo(reinterpret_cast<const void*>(args[0]), blk);
            for (int k = 0; k < 8; ++k) CrashLog::SafeRead32(args[0] + 4 * k, &w[k]);
            Logger::Info("ANIMTRACE MOVEREQ entry %08X: GMalloc %d %08X/%X, +0: %08X %08X %08X %08X %08X %08X %08X %08X (%s)", args[0], inBlock,
                         blk[0], blk[1], w[0], w[1], w[2], w[3], w[4], w[5], w[6], w[7], ObjectHooks::TypeOf(args[0]));
        }
    } else if (site == 6) {
        // 0x0045D960 at 0x008A4C11 (start of hit script 0xCE): cdecl (owner, 1, 0xCE, 2, 0xD6B1F4); result the executor.
        for (int i = 0; i < 5; ++i) h.args[i] = args[i];
    } else if (site == 7) {
        // 0x008A46E0: args are the object and its gates (id, +0x194, +0x104, +0x188, +0x18C) before the call; the result
        // is +0x104 after it (set on a hit).
        for (int i = 0; i < 6; ++i) h.args[i] = args[i];
        h.result = 0xEEEEEEEE;
        CrashLog::SafeRead32(args[0] + 0x104, &h.result);
        CrashLog::SafeRead32(args[0] + 0x170, &h.vec[0][0]);
        CrashLog::SafeRead32(args[0] + 0x1B8, &h.vec[0][1]);
        CrashLog::SafeRead32(args[0] + 0xF8, &h.vec[0][2]);
        CrashLog::SafeRead32(args[0] + 0x110, &h.vec[0][3]);
        // Trajectory of every checked object, both passes (soak 10401, load 1132: at the hit check of frame S the
        // object's Location was ~4 frames of travel apart live vs replay; which pass is off?).
        uint32_t act = 0, w[11] = {};
        if (CrashLog::SafeRead32(args[0] + 0x8, &act) && act) {
            const uint32_t offs[11] = {0xDC, 0xE0, 0xE4, 0xF4, 0xF8, 0xFC, 0x60, 0x98, 0x58, 0x80, 0x4};
            for (int k = 0; k < 11; ++k) CrashLog::SafeRead32(act + offs[k], &w[k]);
            if (!resim && w[6] && !(w[8] & 0xFF)) gLastHitActor = act;
        }
        float f[8];
        memcpy(f, w, sizeof f);
        if (!resim && act) {
            LiveLoc& ll = gLiveLoc[LiveLocSlot(gf, args[1])];
            ll.gf = gf;
            ll.id = args[1];
            memcpy(ll.loc, f, sizeof ll.loc);
            if (w[6] && !(w[8] & 0xFF)) CapturePlace(act, w[6], ll.place);
            else ll.place = PlaceCtx{};
            // Keep the trace on a based actor the live checks see, so the original pass of a later load's frame is
            // recorded too (soak 7: the hit box was born after the previous load, the original pass traced another).
            if (act == gLocActor) gLocSeenGf = gf;
            else if (w[6] && !(w[8] & 0xFF) && ll.place.rec && gLocSeenGf + 1 < gf) {
                LocArm(act);
                gLocSeenGf = gf;
            }
        }
        // Replay: what the last load did with this actor's mkactor span (soak 12611 S=2362: the first replayed check saw
        // the abandoned future's Location, then trailed live by one frame).
        char at[64] = "";
        if (Rollback::gIsResimulating && act) {
            float al[3];
            const int st = Snapshot::MkActorAtLoad(act, al);
            if (st == 0) snprintf(at, sizeof at, " atload=%.3f,%.3f,%.3f", al[0], al[1], al[2]);
            else snprintf(at, sizeof at, " atload=%s%d", st < 0 ? "none" : "skip", st);
            if (st == 0 && act == gLocActor && gLocCheckedSeq != gLocLoadSeq) {
                gLocCheckedSeq = gLocLoadSeq;  // the first replayed check of this actor since the load
                if (memcmp(al, f, sizeof al)) {
                    static int sLogged = 0;
                    if (sLogged++ < 40) {
                        char bd[80];
                        Ue3Names::Describe(w[6], bd, sizeof bd);
                        Logger::Info("LOCMOVED gf=%u actor %08X: restored %.3f,%.3f,%.3f, at the hit check %.3f,%.3f,%.3f (base %08X %s)",
                                     GameFrame(), act, al[0], al[1], al[2], f[0], f[1], f[2], w[6], bd);
                        LogPlace(gf, args[1], act, w[6]);
                        // the replayed frame's trace slot may be keyed one lower than GameFrame() during the tick
                        for (uint32_t g = GameFrame() - 1; g <= GameFrame(); ++g) {
                            CrashLog::TraceLog(g, true, "LOCMOVED");
                            CrashLog::TraceLog(g, false, "LOCMOVED");
                        }
                    }
                } else {
                    // Not moved since the load: did the original move it by this check?
                    const LiveLoc& ll = gLiveLoc[LiveLocSlot(gf, args[1])];
                    if (ll.gf == gf && ll.id == args[1] && memcmp(ll.loc, f, sizeof ll.loc)) {
                        static int sStill = 0;
                        if (sStill++ < 40) {
                            char bd[80];
                            Ue3Names::Describe(w[6], bd, sizeof bd);
                            Logger::Info("LOCSTILL gf=%u actor %08X: restored %.3f,%.3f,%.3f, not moved at the replayed check; the original was at "
                                         "%.3f,%.3f,%.3f (base %08X %s)",
                                         GameFrame(), act, al[0], al[1], al[2], ll.loc[0], ll.loc[1], ll.loc[2], w[6], bd);
                            LogPlace(gf, args[1], act, w[6]);
                            for (uint32_t g = GameFrame() - 1; g <= GameFrame(); ++g) {
                                CrashLog::TraceLog(g, true, "LOCSTILL");
                                CrashLog::TraceLog(g, false, "LOCSTILL");
                            }
                        }
                    }
                }
            }
        }
        // The base's own Location and base (soak 13110 S=2593/2969: hit boxes based on a prop mesh that P1 attaches and
        // detaches; the original hit, the replay did not): is the prop where the original had it?
        uint32_t bw[4] = {0, 0, 0, 0};
        if (w[6]) {
            CrashLog::SafeRead32(w[6] + 0xDC, &bw[0]);
            CrashLog::SafeRead32(w[6] + 0xE0, &bw[1]);
            CrashLog::SafeRead32(w[6] + 0xE4, &bw[2]);
            CrashLog::SafeRead32(w[6] + 0x60, &bw[3]);
        }
        float bf[3];
        memcpy(bf, bw, sizeof bf);
        Logger::Frame("HITPOS %s gf=%u obj=%08X id=%08X sw=%u actor=%08X idx=%08X loc=%.3f,%.3f,%.3f vel=%.3f,%.3f,%.3f base=%08X ct=%.4f phys=%u fl=%08X res=%u bloc=%.3f,%.3f,%.3f bbase=%08X%s",
                      Rollback::gIsResimulating ? "R" : "L", GameFrame(), args[0], args[1], args[2], act, w[10], f[0], f[1], f[2], f[3], f[4], f[5], w[6],
                      f[7], w[8] & 0xFF, w[9], h.result, bf[0], bf[1], bf[2], bw[3], at);
    } else if (site == 13) {
        for (int i = 0; i < 4; ++i) h.args[i] = args[i];
    }  // site 4, 0x008AB6F0 (no arguments): the result only
    EnterCriticalSection(&gLock);
    if (!resim) {
        HitFrame& f = gHitLive[gf % kRing];
        if (f.gf != gf) {
            f.gf = gf;
            f.n = 0;
        }
        for (uint32_t k = 0; k < f.n; ++k) h.ord += f.r[k].site == site;
        if (f.n < 128) f.r[f.n++] = h;
    } else {
        if (gHitReplayGf != gf || gHitPass != gPass[0]) {
            gHitReplayGf = gf;
            gHitPass = gPass[0];
            memset(gHitReplayN, 0, sizeof gHitReplayN);
        }
        const HitFrame& f = gHitLive[gf % kRing];
        const uint32_t i = h.ord = gHitReplayN[site & 15]++;
        const HitRec* lp = nullptr;
        for (uint32_t k = 0; f.gf == gf && k < f.n && !lp; ++k)
            if (f.r[k].site == site && f.r[k].ord == i) lp = &f.r[k];
        if (lp) {
            const HitRec& l = *lp;
            ++gHitCmp;
            // Arguments pointing into this thread's stack (the caller's locals) move with the resim call depth.
            const uintptr_t sp = reinterpret_cast<uintptr_t>(&h);
            for (int a = 0; a < 6; ++a)
                if (h.args[a] - (sp - 0x100000) < 0x200000 && l.args[a] - (sp - 0x100000) < 0x200000) h.args[a] = l.args[a];
            // Site 7: an object given back by ActorDefer has a new actor and component (+0x170); only the id, the gates
            // and the result count. Site 13: ecx is that component.
            if (site == 7) h.vec[0][0] = l.vec[0][0];
            if (site == 13) h.args[0] = l.args[0];
            if (memcmp(&l, &h, sizeof h)) {
                ++gHitDiff;
                static int logs[16] = {};
                if (logs[site & 15]++ < 40) {
                    char s[700];
                    int n = snprintf(s, sizeof s, "ANIMTRACE HIT gf=%u call %u site %u: result live %u replay %u;", gf, i, site, l.result, h.result);
                    if (site == 5) {
                        uint32_t blk[2] = {0, 0};
                        const bool inBlock = CrashLog::SafeBlockInfo(reinterpret_cast<const void*>(h.vec[0][0]), blk);
                        n += snprintf(s + n, sizeof s - n, " entry %08X (GMalloc %d %08X/%X, vt %08X %s)", h.vec[0][0], inBlock, blk[0], blk[1],
                                      *reinterpret_cast<const uint32_t*>(h.vec[0][0]), ObjectHooks::TypeOf(h.vec[0][0]));
                    }
                    for (int a = 0; a < 6 && n < 600; ++a)
                        if (l.args[a] != h.args[a]) n += snprintf(s + n, sizeof s - n, " arg%d %08X>%08X", a, l.args[a], h.args[a]);
                    for (int v = 0; v < 2; ++v)
                        for (int k = 0; k < 4 && n < 600; ++k)
                            if (l.vec[v][k] != h.vec[v][k]) n += snprintf(s + n, sizeof s - n, " vec%d[%d] %08X>%08X", v, k, l.vec[v][k], h.vec[v][k]);
                    for (int k = 0; k < 16 && n < 600; ++k)
                        if (l.target[k] != h.target[k]) n += snprintf(s + n, sizeof s - n, " tgt+%X %08X>%08X", k * 4, l.target[k], h.target[k]);
                    Logger::Info("%s", s);
                }
            }
            if (gHitCmp % 500 == 0) Logger::Info("ANIMTRACE HIT %u replayed hit tests compared, %u differ", gHitCmp, gHitDiff);
        } else if (f.gf == gf && site != 4 && site != 5) {
            static int extra = 0;
            if (extra++ < 300) Logger::Info("ANIMTRACE HIT gf=%u: replay made call %u of site %u (result %u), live did not", gf, i, site, result);
        }
    }
    LeaveCriticalSection(&gLock);
}
uint32_t gHitRet[16];
uint32_t gHitArgs5[7];
void __cdecl MK9R_SaveArgs5(const uint32_t* a) { memcpy(gHitArgs5, a, sizeof gHitArgs5); }
// After the MK object tick 0x0045CAF0 (EngineTick call at 0x0080F710, right after FightUpdatePlayers): splits the
// FightUpdate-exit -> P1 anim step window into the MK object/script tick and the world/actor tick 0x0080F570.
void __cdecl MK9R_OnAfterObjTick() {
    if (!gOn) return;
    EnterCriticalSection(&gLock);
    ImageCompare(2, GameFrame(), Rollback::gIsResimulating);
    LeaveCriticalSection(&gLock);
}
}
asm(R"(
    .text
    .globl _MK9R_HkObjTick
_MK9R_HkObjTick:
    movl $0x0045CAF0, %eax  # no arguments, result unused by the caller (0x0080F715 calls on)
    call *%eax
    pushfl
    pushal
    call _MK9R_OnAfterObjTick
    popal
    popfl
    ret
)");
extern "C" void MK9R_HkObjTick();
// Hit-test call-site stubs: drop the return address (kept per site), call the real cdecl test with the caller's
// arguments still in place, report (site, args, result), then return with the result in eax.
#define MK9R_HIT_STUB(N, TARGET)                \
    asm(".text\n"                               \
        ".globl _MK9R_HkHit" #N "\n"            \
        "_MK9R_HkHit" #N ":\n"                  \
        "    popl _gHitRet+4*" #N "\n"          \
        "    movl $" #TARGET ", %eax\n"         \
        "    call *%eax\n"                      \
        "    pushl %eax\n"                      \
        "    leal 4(%esp), %ecx\n"              \
        "    pushl %eax\n"                      \
        "    pushl %ecx\n"                      \
        "    pushl $" #N "\n"                   \
        "    call _MK9R_OnHit\n"                \
        "    addl $12, %esp\n"                  \
        "    popl %eax\n"                       \
        "    pushl _gHitRet+4*" #N "\n"         \
        "    ret\n");                           \
    extern "C" void MK9R_HkHit##N();
MK9R_HIT_STUB(0, 0x008B2F90)
MK9R_HIT_STUB(1, 0x008B2C80)
MK9R_HIT_STUB(2, 0x008B0B10)
MK9R_HIT_STUB(3, 0x008B0910)
// The fighter's move-request dispatch 0x008AB6F0 (called from 0x008AD095; no arguments, plain ret): starts a queued
// move; live and replay took different paths there (owner_1 2026-10-18, load 1379: live 0x008AB99A -> 0x008A9510,
// replay the fallback 0x008AAA70, P1 meter 0.02 lower in the replay).
MK9R_HIT_STUB(4, 0x008AB6F0)
// Its type-handler 0x008A9510 (call at 0x008AB995): thiscall with 7 stack arguments popped by the callee, so they are
// copied before the call.
asm(R"(
    .text
    .globl _MK9R_HkHit5
_MK9R_HkHit5:
    popl _gHitRet+20
    pushal
    leal 32(%esp), %eax
    pushl %eax
    call _MK9R_SaveArgs5
    addl $4, %esp
    popal
    movl $0x008A9510, %eax
    call *%eax
    pushl %eax
    pushl %eax
    pushl $_gHitArgs5
    pushl $5
    call _MK9R_OnHit
    addl $12, %esp
    popl %eax
    pushl _gHitRet+20
    ret
)");
extern "C" void MK9R_HkHit5();
// The hit script start in 0x008A46E0 (call 0x0045D960 at 0x008A4C11, cdecl, 5 arguments).
MK9R_HIT_STUB(6, 0x0045D960)
// The hit check 0x008A46E0 itself (thiscall, no stack arguments, call at 0x008A73A9): the object's gates before the
// call, +0x104 after it. Live-only hit scripts came with every compared test equal (soak 10201..10212, 3 loads).
asm(R"(
    .text
    .globl _MK9R_HkHit7
_MK9R_HkHit7:
    popl _gHitRet+28
    pushal
    pushl %ecx
    call _MK9R_PreHitCheck
    addl $4, %esp
    popal
    movl $0x008A46E0, %eax
    call *%eax
    pushl %eax
    pushl %eax
    pushl $_gHitObj7
    pushl $7
    call _MK9R_OnHit
    addl $12, %esp
    popl %eax
    pushl _gHitRet+28
    ret
)");
extern "C" void MK9R_HkHit7();
// The tests behind the +0x194 switch at 0x008A493B (soak 10301..10308: the hit check's result differed live vs replay,
// sites 0..3 not called in those frames).
MK9R_HIT_STUB(8, 0x008B0B10)   // at 0x008A4965
MK9R_HIT_STUB(9, 0x008B2F90)   // at 0x008A4A22
MK9R_HIT_STUB(10, 0x008B2C80)  // at 0x008A4A38
MK9R_HIT_STUB(11, 0x008B3430)  // at 0x008A4A8F
MK9R_HIT_STUB(12, 0x008B3180)  // at 0x008A4AA5
asm(R"(
    .text
    .globl _MK9R_HkHit13
_MK9R_HkHit13:
    popl _gHitRet+52
    pushal
    leal 32(%esp), %eax
    pushl %eax
    pushl %ecx
    call _MK9R_SaveArgs13
    addl $8, %esp
    popal
    movl $0x0089E220, %eax
    call *%eax
    pushl %eax
    pushl %eax
    pushl $_gHitArgs13
    pushl $13
    call _MK9R_OnHit
    addl $12, %esp
    popl %eax
    pushl _gHitRet+52
    ret
)");
extern "C" void MK9R_HkHit13();
asm(R"(
    .text
    .globl _MK9R_HkAnimStep
_MK9R_HkAnimStep:
    pushfl
    pushal
    pushl 48(%esp)          # comp     (ret + 8 regs + flags = 36; args at 40/44/48)
    pushl 48(%esp)          # dt*rate  (shifted by the push)
    pushl 48(%esp)          # MK tree
    call _MK9R_OnAnimStep
    addl $12, %esp
    popal
    popfl
    movl $0x004CBA60, %eax  # eax is a scratch register at this call (cdecl)
    jmp *%eax
)");
extern "C" void MK9R_HkAnimStep();

asm(R"(
    .text
    .globl _MK9R_HkRootMotion
_MK9R_HkRootMotion:
    pushfl
    pushal
    pushl %ecx
    call _MK9R_OnRootMotion
    addl $4, %esp
    popal
    popfl
    jmp *_gMK9R_RootMotionTramp
)");
extern "C" void MK9R_HkRootMotion();

asm(R"(
    .text
    .globl _MK9R_HkSkelUpdate
_MK9R_HkSkelUpdate:
    pushfl
    pushal
    pushl %ecx
    call _MK9R_OnSkelUpdate
    addl $4, %esp
    popal
    popfl
    jmp *_gMK9R_SkelUpdateTramp
)");
extern "C" void MK9R_HkSkelUpdate();

namespace AnimInputTrace {
void ImagePoint(int at) {
    if (!gOn || at < 0 || at > 1) return;
    MKHeap::Region r[8];
    const int n = MKHeap::MasterRanges(r, 8);
    if (n <= 0) return;
    const uint32_t gf = GameFrame();
    const bool resim = Rollback::gIsResimulating;
    static uint32_t h[kPages];
    uint32_t np = 0;
    for (int k = 0; k < n; ++k)
        for (uint32_t off = 0; off < r[k].size && np < kPages; off += 0x1000, ++np) {
            const uint32_t* d = reinterpret_cast<const uint32_t*>(r[k].base + off);
            const uint32_t len = (r[k].size - off < 0x1000 ? r[k].size - off : 0x1000) / 4;
            uint32_t x = 0x9E3779B9u;
            for (uint32_t i = 0; i < len; ++i) x = ((x << 5) | (x >> 27)) ^ d[i];
            h[np] = x;
        }
    EnterCriticalSection(&gLock);
    ImageCompare(at == 1 ? 1 : 3, gf, resim);  // 3: FightUpdate entry
    CrashLog::gWatchPhase = at == 1 ? (resim ? 2 : 1) : 0;
    PageRec& lr = gPageLive[at][gf % kRing];
    if (!resim) {
        lr.gf = gf;
        memcpy(lr.h, h, np * 4);
    } else if (lr.gf == gf) {
        uint32_t d = 0, first = 0xFFFFFFFF;
        for (uint32_t i = 0; i < np; ++i)
            if (lr.h[i] != h[i]) {
                if (first == 0xFFFFFFFF) first = i;
                ++d;
            }
        gPageDiff[at][gf % kRing] = d;
        gPageDiffGf[at][gf % kRing] = gf;
        gPageFirst[at][gf % kRing] = first == 0xFFFFFFFF ? 0 : (uint32_t)(r[0].base + first * 0x1000);
    }
    LeaveCriticalSection(&gLock);
}
// Frame-boundary image points: 4 = WorldTick entry (the snapshot boundary; right after a load in a replay),
// 5 = EngineTick entry (after the UE3 world tick, before the pre-FightUpdate calls), 6 = EngineTick exit (before
// the rollback manager's between-frame work: deferred deaths, quarantine, the next save).
void ImagePointAt(int pt) {
    if (!gOn || pt < 4 || pt > 6) return;
    EnterCriticalSection(&gLock);
    ImageCompare(pt, GameFrame(), Rollback::gIsResimulating);
    LeaveCriticalSection(&gLock);
}
}  // namespace AnimInputTrace
namespace AnimInputTrace {
int Describe(uint32_t S, uint32_t gf, char* out, int len) {
    if (!gOn || len <= 0) return 0;
    for (int p = 0; p < 2; ++p) MaFlush(p);  // the last replayed frame's MoveActor records
    int n = snprintf(out, len, "ANIMTRACE frames %u..%u since the load (c comp, n nodes, r root motion, m MoveActor in, M MoveActor out, d anim dt*rate, t MK anim tree):", S + 1, gf);
    for (uint32_t g = S; g <= gf && g - S <= 12 && n < len - 24; ++g) {
        char f[2][8];
        for (int p = 0; p < 2; ++p) {
            const uint8_t x = gFlagsGf[g % kRing][p] == g ? gFlags[g % kRing][p] : 0xFF;
            int k = 0;
            if (x == 0xFF) f[p][k++] = '?';
            else {
                if (x & 1) f[p][k++] = 'c';
                if (x & 2) f[p][k++] = 'n';
                if (x & 4) f[p][k++] = 'r';
                if (x & 8) f[p][k++] = 'm';
                if (x & 16) f[p][k++] = 'M';
                if (x & 64) f[p][k++] = 'd';
                if (x & 128) f[p][k++] = 't';
                if (gOrderGf[g % kRing] == g && gOrderDiff[g % kRing]) f[p][k++] = 'o';
                if (!k) f[p][k++] = '-';
            }
            f[p][k] = 0;
        }
        n += snprintf(out + n, len - n, " %u:%s/%s", g, f[0], f[1]);
        // Non-noise MK image dwords differing at FightUpdate entry / exit / MK object tick exit / P1's anim step.
        if (n < len - 48) {
            auto cnt = [&](int pt) { return gImgLineGf[pt][g % kRing] == g ? (int)gImgLineN[pt][g % kRing] : -1; };
            n += snprintf(out + n, len - n, "[img %d/%d/%d/%d/%d/%d/%d]", cnt(4), cnt(5), cnt(3), cnt(1), cnt(2), cnt(0), cnt(6));
        }
    }
    // The earliest point (frame order: entry, exit, object tick, anim step) where the image diverged: its addresses.
    static int sFirstLogs = 0;
    static const int kOrder[7] = {4, 5, 3, 1, 2, 0, 6};
    static const char* kName[7] = {"P1 anim step", "FightUpdate exit", "MK object tick exit", "FightUpdate entry", "WorldTick entry", "EngineTick entry", "EngineTick exit"};
    bool done = false;
    for (uint32_t g = S; g <= gf && g - S <= 12 && !done && sFirstLogs < 60; ++g)
        for (int q = 0; q < 7 && !done; ++q) {
            const int pt = kOrder[q];
            if (gImgLineGf[pt][g % kRing] != g || gImgLineN[pt][g % kRing] <= 8) continue;
            done = true;
            ++sFirstLogs;
            Logger::Info("ANIMTRACE FIRSTDIFF load S=%u: frame %u at %s, %u dwords:%s", S, g, kName[pt], gImgLineN[pt][g % kRing], gImgLine[pt][g % kRing]);
            // Owners: the listed MK object with the highest start at or below each of the first addresses.
            struct Near { uint32_t addr, obj; const char* type; } nr[6]{};
            int nn = 0;
            const char* p = gImgLine[pt][g % kRing];
            uint32_t a = 0;
            int used = 0;
            while (nn < 6 && sscanf(p, " %x:%*x>%*x%n", &a, &used) == 1) {
                p += used;
                if (nn && a - nr[nn - 1].addr < 0x40) continue;  // same record
                nr[nn++] = {a, 0, nullptr};
            }
            // The constructor-tracked set: the global list 0x00DE8214 lacks script VMs and anim nodes.
            auto nearest = [](uint32_t obj, const char* type, void* c) {
                Near* v = static_cast<Near*>(c);
                for (int k = 0; k < 6 && v[k].addr; ++k)
                    if (obj <= v[k].addr && obj > v[k].obj) v[k].obj = obj, v[k].type = type;
            };
            // It misses objects created before the hooks were installed, so the list is walked too.
            if (ObjectHooks::Installed()) ObjectHooks::ForEachLive(nearest, nr);
            ObjectHooks::ForEachListed(nearest, nr);
            char o[600];
            int on = 0;
            for (int k = 0; k < nn && on < 520; ++k)
                on += snprintf(o + on, sizeof o - on, " %08X=%s+%X", nr[k].addr, nr[k].type ? nr[k].type : "?", nr[k].obj ? nr[k].addr - nr[k].obj : 0);
            Logger::Info("ANIMTRACE FIRSTDIFF owners:%s", o);
        }
    return n;
}
bool SetEnabled(bool on) {
    if (on && !gInstalled) {
        InitializeCriticalSection(&gLock);
        uint8_t* t = static_cast<uint8_t*>(CodePatch::AllocExec(16));
        if (!t) return false;
        memcpy(t, kOrig, 6);
        t[6] = 0xE9;
        const int32_t rel = (int32_t)((kSkelUpdate + 6) - (reinterpret_cast<uintptr_t>(t) + 11));
        memcpy(t + 7, &rel, 4);
        FlushInstructionCache(GetCurrentProcess(), t, 16);
        gMK9R_SkelUpdateTramp = t;
        uint8_t jmp[6] = {0xE9, 0, 0, 0, 0, 0x90};
        const int32_t r2 = (int32_t)(reinterpret_cast<uintptr_t>(&MK9R_HkSkelUpdate) - (kSkelUpdate + 5));
        memcpy(jmp + 1, &r2, 4);
        if (!CodePatch::Write(kSkelUpdate, kOrig, jmp, 6, "skeleton update 0x007741E0 (animtrace)")) return false;
        // Root motion 0x00762C10: push ebx; mov ebx, esp; sub esp, 8 (6 bytes, relocatable).
        static const uint8_t kRmOrig[6] = {0x53, 0x8B, 0xDC, 0x83, 0xEC, 0x08};
        uint8_t* t2 = static_cast<uint8_t*>(CodePatch::AllocExec(16));
        if (t2) {
            memcpy(t2, kRmOrig, 6);
            t2[6] = 0xE9;
            const int32_t rr = (int32_t)((0x00762C10 + 6) - (reinterpret_cast<uintptr_t>(t2) + 11));
            memcpy(t2 + 7, &rr, 4);
            FlushInstructionCache(GetCurrentProcess(), t2, 16);
            gMK9R_RootMotionTramp = t2;
            uint8_t j2[6] = {0xE9, 0, 0, 0, 0, 0x90};
            const int32_t r3 = (int32_t)(reinterpret_cast<uintptr_t>(&MK9R_HkRootMotion) - (0x00762C10 + 5));
            memcpy(j2 + 1, &r3, 4);
            if (!CodePatch::Write(0x00762C10, kRmOrig, j2, 6, "root motion 0x00762C10 (animtrace)")) Logger::Info("ANIMTRACE: root-motion hook not installed");
        }
        // UWorld::MoveActor 0x007A6F00: same 6-byte prologue as root motion.
        uint8_t* t3 = static_cast<uint8_t*>(CodePatch::AllocExec(16));
        if (t3) {
            memcpy(t3, kRmOrig, 6);
            t3[6] = 0xE9;
            const int32_t r4 = (int32_t)((0x007A6F00 + 6) - (reinterpret_cast<uintptr_t>(t3) + 11));
            memcpy(t3 + 7, &r4, 4);
            FlushInstructionCache(GetCurrentProcess(), t3, 16);
            gMK9R_MoveActorTramp = t3;
            uint8_t j3[6] = {0xE9, 0, 0, 0, 0, 0x90};
            const int32_t r5 = (int32_t)(reinterpret_cast<uintptr_t>(&MK9R_HkMoveActor) - (0x007A6F00 + 5));
            memcpy(j3 + 1, &r5, 4);
            if (!CodePatch::Write(0x007A6F00, kRmOrig, j3, 6, "UWorld::MoveActor 0x007A6F00 (animtrace)")) Logger::Info("ANIMTRACE: MoveActor hook not installed");
        }
        // Animation time step: the call 0x004CBA60 at 0x00773F90 (E8 rel32) is redirected to a stub that records the
        // arguments and jumps on to 0x004CBA60.
        {
            constexpr uintptr_t kStepCall = 0x00773F90;
            const int32_t origRel = (int32_t)(0x004CBA60 - (kStepCall + 5));
            uint8_t orig[5] = {0xE8};
            memcpy(orig + 1, &origRel, 4);
            uint8_t call[5] = {0xE8};
            const int32_t nr = (int32_t)(reinterpret_cast<uintptr_t>(&MK9R_HkAnimStep) - (kStepCall + 5));
            memcpy(call + 1, &nr, 4);
            if (!CodePatch::Write(kStepCall, orig, call, 5, "anim step call 0x00773F90 (animtrace)")) Logger::Info("ANIMTRACE: anim-step hook not installed");
        }
        // MK object tick: the call 0x0045CAF0 at 0x0080F710 (E8 rel32) goes through a stub that compares the image after it.
        {
            constexpr uintptr_t kObjCall = 0x0080F710;
            const int32_t origRel = (int32_t)(0x0045CAF0 - (kObjCall + 5));
            uint8_t orig[5] = {0xE8};
            memcpy(orig + 1, &origRel, 4);
            uint8_t call[5] = {0xE8};
            const int32_t nr = (int32_t)(reinterpret_cast<uintptr_t>(&MK9R_HkObjTick) - (kObjCall + 5));
            memcpy(call + 1, &nr, 4);
            if (!CodePatch::Write(kObjCall, orig, call, 5, "MK object tick call 0x0080F710 (animtrace)")) Logger::Info("ANIMTRACE: object-tick hook not installed");
        }
        // Hit tests in 0x008A46E0 (E8 rel32 call sites).
        {
            struct Site {
                uintptr_t at, target;
                void (*stub)();
            };
            const Site sites[14] = {{0x008A4B64, 0x008B2F90, &MK9R_HkHit0},
                                   {0x008A4BB3, 0x008B2C80, &MK9R_HkHit1},
                                   {0x008A4BD5, 0x008B0B10, &MK9R_HkHit2},
                                   {0x008A4BE1, 0x008B0910, &MK9R_HkHit3},
                                   {0x008AD095, 0x008AB6F0, &MK9R_HkHit4},
                                   {0x008AB995, 0x008A9510, &MK9R_HkHit5},
                                   {0x008A4C11, 0x0045D960, &MK9R_HkHit6},
                                   {0x008A73A9, 0x008A46E0, &MK9R_HkHit7},
                                   {0x008A4965, 0x008B0B10, &MK9R_HkHit8},
                                   {0x008A4A22, 0x008B2F90, &MK9R_HkHit9},
                                   {0x008A4A38, 0x008B2C80, &MK9R_HkHit10},
                                   {0x008A4A8F, 0x008B3430, &MK9R_HkHit11},
                                   {0x008A4AA5, 0x008B3180, &MK9R_HkHit12},
                                   {0x008A491F, 0x0089E220, &MK9R_HkHit13}};
            for (const Site& s : sites) {
                uint8_t orig[5] = {0xE8}, call[5] = {0xE8};
                const int32_t ro = (int32_t)(s.target - (s.at + 5));
                const int32_t rn = (int32_t)(reinterpret_cast<uintptr_t>(s.stub) - (s.at + 5));
                memcpy(orig + 1, &ro, 4);
                memcpy(call + 1, &rn, 4);
                if (!CodePatch::Write(s.at, orig, call, 5, "MK hit test call (animtrace)")) Logger::Info("ANIMTRACE: hit-test hook %08X not installed", (unsigned)s.at);
            }
        }
        gInstalled = true;
    }
    gOn = on;
    Logger::Info("ANIMTRACE %s", on ? "on" : "off");
    return true;
}

// End of replayed frame gf: live hit-test calls the replay did not repeat (with their live results). A hit live and no
// test at all in the replay was invisible to the per-call comparison (soak 10101..10104: a hit script 0xCE started
// live only from 0x008A4C11, every compared test equal).
void HitReplayFrameDone(uint32_t gf) {
    if (!gOn) return;
    EnterCriticalSection(&gLock);
    const HitFrame& pf = gHitLive[gf % kRing];
    uint32_t rep[16] = {};
    if (gHitReplayGf == gf && gHitPass == gPass[0]) memcpy(rep, gHitReplayN, sizeof rep);
    if (pf.gf == gf) {
        uint32_t liveN[16] = {};
        for (uint32_t k = 0; k < pf.n; ++k) ++liveN[pf.r[k].site & 15];
        for (uint32_t st = 0; st < 16; ++st)
            if (liveN[st] > rep[st] && st != 4 && st != 5) {  // 4/5: routinely live-only with result 0 (soak 10201..10212)
                static int missing = 0;
                if (missing++ < 300) {
                    char s[300];
                    int n = snprintf(s, sizeof s, "ANIMTRACE HIT gf=%u site %u: live made %u calls, the replay %u; unmatched live results:", gf,
                                     st, liveN[st], rep[st]);
                    for (uint32_t k = 0; k < pf.n && n < 280; ++k)
                        if ((pf.r[k].site & 15) == st && pf.r[k].ord >= rep[st]) n += snprintf(s + n, sizeof s - n, " #%u=%u", pf.r[k].ord, pf.r[k].result);
                    Logger::Info("%s", s);
                }
            }
    }
    LeaveCriticalSection(&gLock);
}
// After every load (RollbackManager): keep the Location write trace on a restored MK actor with a Base (Physics
// none), preferring the last one a live hit check saw, then the one already armed, then one based on a fighter.
// See LOCMOVED in MK9R_OnHit.
void OnLoadDone() {
    if (!gOn) return;
    ++gLocLoadSeq;
    for (int p = 0; p < 2; ++p) {
        CaptureIk(FighterComp(p), GameFrame(), gLoadIk[p]);
        CaptureLoadNodes(FighterComp(p), p);
    }
    uint32_t a[64], fa[2] = {0, 0};
    const int n = Snapshot::MkActorsRestored(a, 64);
    for (int p = 0; p < 2; ++p) {
        uint32_t obj = 0;
        if (CrashLog::SafeRead32(0x00E89D7C + p * 0xC8C + 0x1C, &obj) && obj && obj != 0xDEADBEEF) CrashLog::SafeRead32(obj + 8, &fa[p]);
    }
    uint32_t pick = 0, hit = 0, armed = 0, fighterBased = 0;
    for (int k = 0; k < n; ++k) {
        uint32_t base = 0, phys = 0;
        if (!CrashLog::SafeRead32(a[k] + 0x60, &base) || !CrashLog::SafeRead32(a[k] + 0x58, &phys)) continue;
        if (!base || (phys & 0xFF)) continue;
        // The load's own later steps (MKATTACH re-attach, RepairAttachBase, SyncFighterComponents) run untraced:
        // did one of them move the actor after its span was written?
        float al[3], now[3];
        if (Snapshot::MkActorAtLoad(a[k], al) == 0 && CrashLog::SafeRead32(a[k] + 0xDC, reinterpret_cast<uint32_t*>(&now[0])) &&
            CrashLog::SafeRead32(a[k] + 0xE0, reinterpret_cast<uint32_t*>(&now[1])) && CrashLog::SafeRead32(a[k] + 0xE4, reinterpret_cast<uint32_t*>(&now[2])) &&
            memcmp(al, now, sizeof al)) {
            static int sLogged = 0;
            if (sLogged++ < 40)
                Logger::Info("LOCLOAD gf=%u actor %08X (base %08X): span wrote %.3f,%.3f,%.3f, after the load's later steps %.3f,%.3f,%.3f", GameFrame(),
                             a[k], base, al[0], al[1], al[2], now[0], now[1], now[2]);
        }
        if (a[k] == gLastHitActor) hit = a[k];
        if (a[k] == gLocActor) armed = a[k];
        if (!fighterBased && (base == fa[0] || base == fa[1])) fighterBased = a[k];
        if (!pick) pick = a[k];
    }
    pick = hit ? hit : armed ? armed : fighterBased ? fighterBased : pick;
    const uint32_t ik = IkWatchAddr();
    if ((pick && pick != gLocActor) || ik != gIkWatch) {
        if (ik != gIkWatch) {
            static int sIkArms = 0;
            if (sIkArms++ < 20) Logger::Info("IKTRACE arm: PinLimb strength source %08X (was %08X) at gf %u", ik, gIkWatch, GameFrame());
        }
        gIkWatch = ik;
        LocArm(pick ? pick : gLocActor);
    }
}
}  // namespace AnimInputTrace
