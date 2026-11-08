#include "RayQueries.h"

#include <windows.h>

#include <algorithm>
#include <cstring>
#include <unordered_map>
#include <utility>
#include <vector>

#include "../debug/CrashLog.h"
#include "../debug/Logger.h"
#include "../hook/CodePatch.h"
#include "../hook/HookManager.h"
#include "../hook/ObjectHooks.h"
#include "Snapshot.h"

namespace RayQueries {
bool gFix = true;
}

namespace {
constexpr uintptr_t kIssueSite = 0x008A7564, kIssueFn = 0x006BDD50;
constexpr uintptr_t kReport = 0x00DED22C, kReportVt = 0x00D549E8;
constexpr uintptr_t kOnBool = 0x006BDA80, kOnRay = 0x006BDB30;
constexpr uint32_t kHelperVt = 0x00C6A5B4;
constexpr uint32_t kHelperSize = 0x4C;
constexpr int kVtCopy = 8;
constexpr uint32_t kSaveSlots = 32;  // > the snapshot ring

struct Pend {
    uint32_t live = 0;     // issued in the current timeline, not delivered yet
    uint32_t stale = 0;    // pending at a load: issued in the abandoned future, dropped when they arrive
    uint32_t gf = 0;       // GameFrame of the last issue
    uint32_t staleAt = 0;  // the load frame that made them stale (they arrive during it; later, they never will)
    uint32_t seq = 0;      // issue order of the last issue (re-issues at a load follow the original's order)
};
// A re-issued query's userData is a proxy, a copy of the restored helper: its result is told apart from the abandoned
// future's (whose userData is the helper itself) whatever order PhysX reports them in, and is forwarded to the helper.
// 0x006BDC00 writes its provisional +8 / +0xC into the proxy, so the restored helper is not touched.
constexpr int kProxies = 64;
struct alignas(16) ProxyMem {
    uint8_t b[0x80];
};
struct ProxyInfo {
    uint32_t helper = 0;  // the real helper
    uint32_t state = 0;   // 0 free, 1 pending (forward its result), 2 stale (drop it)
    uint32_t gf = 0;      // the load frame
};
ProxyMem gProxyMem[kProxies];
ProxyInfo gProxyInfo[kProxies];
std::unordered_map<uint32_t, Pend> gPend;
SRWLOCK gLock = SRWLOCK_INIT;
// Per saved frame: the helpers whose query was pending at that save (issued the frame before, delivered during it).
struct SavedPend {
    uint32_t gf = 0xFFFFFFFF;
    std::vector<uint32_t> helpers;
};
SavedPend gSaved[kSaveSlots];
uint32_t gVt[kVtCopy];
uintptr_t gIssueNext = kIssueFn;  // what the call site called before us (0x006BDD50 or a lab trace wrapper)
uintptr_t gNextBool = kOnBool, gNextRay = kOnRay;
bool gInstalled = false;
volatile bool gReissuing = false;
uint32_t gIssued = 0, gDelivered = 0, gUntracked = 0, gUntrackedOff = 0, gDropped = 0, gReissued = 0, gLoads = 0,
         gStaleAtLoad = 0, gOffThread = 0, gNoSave = 0;
uint32_t gProxyFwd = 0, gProxyDrop = 0, gProxyGone = 0, gProxyStale = 0, gProxyNoSlot = 0, gBoth = 0, gStaleExpired = 0;
uint32_t gLatN[4] = {0, 0, 0, 0};  // delivery latency in frames 0/1/2/3+
uint32_t gLogged = 0;
uint32_t gSeq = 0;
std::vector<std::pair<uint32_t, uint32_t>> gSortBuf;  // (seq, helper), OnSave only (game thread)

uint32_t GameFrame() {
    const uint32_t gi = *reinterpret_cast<const uint32_t*>(0x00DEF4B4);
    return gi ? *reinterpret_cast<const uint32_t*>(gi + 0x70) : 0;
}
bool IsHelper(uint32_t p) {
    uint32_t vt = 0;
    return p > 0x10000 && !(p & 3) && CrashLog::SafeRead32(p, &vt) && vt == kHelperVt;
}
bool InFlight(uint32_t helper) {
    uint32_t f4 = 0, f8 = 1;
    CrashLog::SafeRead32(helper + 4, &f4);
    CrashLog::SafeRead32(helper + 8, &f8);
    return f4 && !f8;  // 0x004A5B50 (helper vt[1]) returns 0 exactly then: 0x006BDC00 issues nothing
}

using IssueFn = void(__thiscall*)(void*, const float*, const float*, uint32_t, uint32_t, uint32_t, uint32_t);
// thiscall(sys, start, end, a3, queryObj, type, helper), ret 0x18, emulated with fastcall (EDX unused).
void __fastcall IssueWrap(void* sys, void*, const float* start, const float* end, uint32_t a3, uint32_t a4, uint32_t type,
                          uint32_t helper) {
    const bool mk = IsHelper(helper);
    const bool busy = mk && InFlight(helper);
    reinterpret_cast<IssueFn>(gIssueNext)(sys, start, end, a3, a4, type, helper);
    if (!mk || busy) return;  // not an MK helper, or a query still in flight (nothing issued)
    // 0x006BDC00 writes a provisional "done, no hit" (+8 = 1, +0xC = 0) inside the call; the scene query itself runs
    // with the next physics step and its result comes through the report, during the next frame before the tick gate.
    AcquireSRWLockExclusive(&gLock);
    Pend& p = gPend[helper];
    ++p.live;
    p.gf = GameFrame();
    p.seq = ++gSeq;
    ++gIssued;
    ReleaseSRWLockExclusive(&gLock);
}

int ProxyIndex(uint32_t p) {
    const uintptr_t base = reinterpret_cast<uintptr_t>(&gProxyMem[0]);
    if (p < base || p >= base + sizeof gProxyMem) return -1;
    const uintptr_t off = p - base;
    return off % sizeof(ProxyMem) ? -1 : (int)(off / sizeof(ProxyMem));
}

// true: drop this result (it answers a query issued in an abandoned timeline); else deliver it to *to
bool Arrive(uint32_t userData, uint32_t* to) {
    *to = userData;
    const int pi = ProxyIndex(userData);
    if (pi >= 0) {
        AcquireSRWLockExclusive(&gLock);
        ProxyInfo& x = gProxyInfo[pi];
        const uint32_t st = x.state, h = x.helper;
        x.state = 0;
        if (st == 1) ++gProxyFwd; else ++gProxyDrop;
        ReleaseSRWLockExclusive(&gLock);
        if (st != 1) return true;
        if (!IsHelper(h)) {
            ++gProxyGone;
            return true;
        }
        *to = h;
        return false;
    }
    const uint32_t helper = userData;
    const uint32_t gf = GameFrame();
    const bool onGame = GetCurrentThreadId() == ObjectHooks::gGameThread;
    bool drop = false, tracked = false, stale = false;
    AcquireSRWLockExclusive(&gLock);
    auto it = gPend.find(helper);
    if (it == gPend.end()) {
        ++gUntracked;
        if (!onGame) ++gUntrackedOff;
    } else {
        tracked = true;
        if (!onGame) ++gOffThread;
        Pend& p = it->second;
        if (p.stale && (int32_t)(gf - p.staleAt) > 1) {  // the abandoned future's results come during the load frame
            gStaleExpired += p.stale;
            p.stale = 0;
        }
        if (p.stale) {
            --p.stale;
            ++gDropped;
            stale = true;
            drop = RayQueries::gFix;
        } else if (p.live) {
            --p.live;
            ++gDelivered;
            const uint32_t lat = gf - p.gf;
            ++gLatN[lat < 3 ? lat : 3];
        }
        if (!p.stale && !p.live) gPend.erase(it);
    }
    ReleaseSRWLockExclusive(&gLock);
    if (tracked && stale && gLogged < 12) {
        ++gLogged;
        Logger::Info("RAYQ stale result for helper %08X at gf %u (%s thread): %s", helper, gf, onGame ? "game" : "OTHER",
                     drop ? "dropped" : "delivered (rayqfix off)");
    }
    return drop;
}

using OnBoolFn = uint32_t(__thiscall*)(void*, uint32_t, uint32_t);
using OnRayFn = uint32_t(__thiscall*)(void*, uint32_t, uint32_t, const void*);
uint32_t __fastcall OnBoolWrap(void* self, void*, uint32_t userData, uint32_t result) {
    uint32_t to = userData;
    if (IsHelper(userData) && Arrive(userData, &to)) return 0;  // NX_SQR_CONTINUE, as the game's own handler returns
    return reinterpret_cast<OnBoolFn>(gNextBool)(self, to, result);
}
uint32_t __fastcall OnRayWrap(void* self, void*, uint32_t userData, uint32_t nbHits, const void* hits) {
    uint32_t to = userData;
    if (IsHelper(userData) && Arrive(userData, &to)) return 0;
    return reinterpret_cast<OnRayFn>(gNextRay)(self, to, nbHits, hits);
}

// A free proxy slot (or the oldest stale one), marked pending for helper h; -1 when all 64 are pending.
int TakeProxy(uint32_t h, uint32_t S) {
    int best = -1;
    for (int i = 0; i < kProxies; ++i) {
        const ProxyInfo& x = gProxyInfo[i];
        if (x.state == 0) {
            best = i;
            break;
        }
        if (x.state == 2 && (best < 0 || S - x.gf > S - gProxyInfo[best].gf)) best = i;
    }
    if (best >= 0) gProxyInfo[best] = {h, 1, S};  // a stale one taken here never got its result
    return best;
}
}  // namespace

namespace RayQueries {

bool Install() {
    if (gInstalled) return true;
    // Chained after whatever is there already (RngTrace's lab tap installs first and calls the originals itself).
    uint8_t cur[5];
    memcpy(cur, reinterpret_cast<const void*>(kIssueSite), 5);
    if (cur[0] != 0xE8) {
        Logger::Info("RAYQ: 0x%08X is not a call (%02X); not installed", (unsigned)kIssueSite, cur[0]);
        return false;
    }
    int32_t rel = 0;
    memcpy(&rel, cur + 1, 4);
    gIssueNext = kIssueSite + 5 + rel;
    const uint32_t curVt = *reinterpret_cast<const uint32_t*>(kReport);
    const uint32_t* vt = reinterpret_cast<const uint32_t*>((uintptr_t)curVt);
    if (curVt != kReportVt && (gIssueNext == kIssueFn)) {
        Logger::Info("RAYQ: report vtable is %08X, not %08X, with no trace installed; not installed", curVt, (unsigned)kReportVt);
        return false;
    }
    memcpy(gVt, vt, sizeof gVt);
    gNextBool = vt[0];
    gNextRay = vt[1];
    gVt[0] = (uint32_t)reinterpret_cast<uintptr_t>(&OnBoolWrap);
    gVt[1] = (uint32_t)reinterpret_cast<uintptr_t>(&OnRayWrap);
    uint8_t replacement[5] = {0xE8};
    const int32_t relNew = (int32_t)(reinterpret_cast<uintptr_t>(&IssueWrap) - (kIssueSite + 5));
    memcpy(replacement + 1, &relNew, 4);
    if (!CodePatch::Write(kIssueSite, cur, replacement, 5, "MK object line check issue 0x008A7564 (rayq)")) return false;
    if (!HookManager::PatchPointer(kReport, curVt, gVt, "scene query report vtable 0x00DED22C (rayq)")) {
        CodePatch::Write(kIssueSite, replacement, cur, 5, "MK object line check issue 0x008A7564 (rayq undo)");
        return false;
    }
    gInstalled = true;
    Logger::Info("RAYQ installed: MK object line checks counted per helper (issue -> %08X, report -> %08X/%08X), rayqfix %d",
                 (unsigned)gIssueNext, (unsigned)gNextBool, (unsigned)gNextRay, gFix ? 1 : 0);
    return true;
}

void OnSave(uint32_t gf) {
    if (!gInstalled) return;
    SavedPend& s = gSaved[gf % kSaveSlots];
    s.gf = gf;
    s.helpers.clear();
    gSortBuf.clear();
    AcquireSRWLockShared(&gLock);
    for (const auto& kv : gPend)
        if (kv.second.live) gSortBuf.emplace_back(kv.second.seq, kv.first);
    ReleaseSRWLockShared(&gLock);
    // In issue order: PhysX runs the batch and reports in that order, so the replay's results come as the original's did
    // (soak 15: the map's order swapped two helpers' results in the load frame, 45 VMTRACE differences, harmless).
    std::sort(gSortBuf.begin(), gSortBuf.end());
    for (const auto& e : gSortBuf) s.helpers.push_back(e.second);
}

void OnLoad(uint32_t S) {
    if (!gInstalled) return;
    if (++gLoads % 600 == 0) Report();
    // Every query still pending answers a frame of the abandoned future: its result would land in the restored helper
    // during the first replayed frame, before the tick gate.
    AcquireSRWLockExclusive(&gLock);
    for (auto& kv : gPend) {
        gStaleAtLoad += kv.second.live;
        kv.second.stale += kv.second.live;
        kv.second.live = 0;
        if (kv.second.stale) kv.second.staleAt = S;
    }
    for (auto& x : gProxyInfo)
        if (x.state == 1) {  // re-issued for a timeline now abandoned too
            x.state = 2;
            ++gProxyStale;
        }
    ReleaseSRWLockExclusive(&gLock);
    if (!gFix) return;
    // The original's queries pending at the save (issued in S-1, answered during S): issue them again from copies of the
    // restored helpers (start +0x34, end +0x40), as the tick does at 0x008A7543..0x008A7564, so S gets the same answers;
    // each copy's result is forwarded to its helper (Arrive).
    const SavedPend& s = gSaved[S % kSaveSlots];
    if (s.gf != S) {
        ++gNoSave;
        return;
    }
    const uint32_t gi = *reinterpret_cast<const uint32_t*>(0x00E86AB0);
    uint32_t sys = 0;
    if (!gi || !CrashLog::SafeRead32(gi + 0x154, &sys) || !sys) return;
    const auto& restored = Snapshot::RestoredHelpers();
    gReissuing = true;
    for (uint32_t h : s.helpers) {
        bool isRestored = false;
        for (uint32_t r : restored)
            if (r == h) {
                isRestored = true;
                break;
            }
        if (!isRestored || !IsHelper(h) || InFlight(h)) continue;
        AcquireSRWLockExclusive(&gLock);
        const int pi = TakeProxy(h, S);
        auto it = gPend.find(h);
        if (it != gPend.end() && it->second.stale) ++gBoth;  // the future's result for h arrives too
        ReleaseSRWLockExclusive(&gLock);
        if (pi < 0) {
            ++gProxyNoSlot;
            continue;
        }
        uint8_t* px = gProxyMem[pi].b;
        memset(px, 0, sizeof gProxyMem[pi].b);
        memcpy(px, reinterpret_cast<const void*>((uintptr_t)h), kHelperSize);
        reinterpret_cast<IssueFn>(gIssueNext)(reinterpret_cast<void*>((uintptr_t)sys), reinterpret_cast<const float*>(px + 0x34),
                                              reinterpret_cast<const float*>(px + 0x40), 0, 0, 1,
                                              (uint32_t)reinterpret_cast<uintptr_t>(px));
        ++gReissued;
    }
    gReissuing = false;
}

void Clear() {
    static uint32_t reportedAt = 0;
    if (gLoads != reportedAt) {  // the counters at each session end (a session rarely reaches 600 loads)
        reportedAt = gLoads;
        Report();
    }
    AcquireSRWLockExclusive(&gLock);
    gPend.clear();
    for (auto& x : gProxyInfo)
        if (x.state == 1) x.state = 2;
    ReleaseSRWLockExclusive(&gLock);
    for (auto& s : gSaved) {
        s.gf = 0xFFFFFFFF;
        s.helpers.clear();
    }
}

void Report() {
    if (!gInstalled) return;
    Logger::Info("RAYQ: %u MK object line checks issued, %u answered (latency 0/1/2/3+ frames %u/%u/%u/%u), %u answers off the "
                 "game thread, %u results for other users (%u off the game thread); %u loads found %u pending, %u stale results %s, "
                 "%u queries re-issued (%u loads without a save record; %u with the future's query for the same helper; "
                 "results forwarded %u, dropped %u, helper gone %u; %u re-issues abandoned by a load, %u without a slot, "
                 "%u stale never came)",
                 gIssued, gDelivered, gLatN[0], gLatN[1], gLatN[2], gLatN[3], gOffThread, gUntracked, gUntrackedOff, gLoads, gStaleAtLoad,
                 gDropped, gFix ? "dropped" : "delivered (rayqfix off)", gReissued, gNoSave, gBoth, gProxyFwd, gProxyDrop,
                 gProxyGone, gProxyStale, gProxyNoSlot, gStaleExpired);
}

}  // namespace RayQueries
