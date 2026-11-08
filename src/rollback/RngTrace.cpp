#include "RngTrace.h"

#include <windows.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <map>
#include <vector>

#include "../debug/CrashLog.h"
#include "../debug/Lab.h"
#include "../debug/Logger.h"
#include "../hook/CodePatch.h"
#include "../hook/HookManager.h"
#include "../hook/ObjectHooks.h"
#include "../hook/GameLoopHook.h"
#include "../debug/Ue3Names.h"
#include "RollbackManager.h"
#include "Snapshot.h"

namespace {
constexpr uintptr_t kRandInt = 0x005F5080;    // mov eax,[0xE7C054] (5 bytes, absolute: no relocation)
constexpr uintptr_t kRandFloat = 0x005F50A0;  // push ebp; mov ebp,esp; sub esp,8 (6 bytes)
const uint8_t kRandIntOrig[5] = {0xA1, 0x54, 0xC0, 0xE7, 0x00};
const uint8_t kRandFloatOrig[6] = {0x55, 0x8B, 0xEC, 0x83, 0xEC, 0x08};

constexpr int kSlots = 64;  // covers the netplay checksum lag (24 frames) for the desync report
struct FrameDraws {
    uint32_t gf = 0xFFFFFFFF;
    std::vector<uint32_t> callers;
};
FrameDraws gOrig[kSlots];      // original (or adopted) timeline, game thread only
std::vector<uint32_t> gResim;  // current resimulated frame
// Script executor ([0x00E39DF4], set by 0x0045C700) for each draw, parallel to the caller lists.
std::vector<uint32_t> gOrigExec[kSlots], gResimExec, *gCurExec = nullptr;
std::vector<uint32_t> gAiExecs;  // executors ever seen drawing from AI natives (test harness)
std::vector<uint32_t>* gCur = nullptr;
volatile LONG gOtherThread = 0;
struct OffDraw {
    uint32_t caller, tid, gf;
};
OffDraw gOffDraw[64];      // the last RNG A draws made off the game thread (ring, gOtherThread counts them all)
bool gRngOnly = false;     // 'rngtrace 1' (before netcode): RNG A draw callers per live frame without the lab's VM traces
bool gRandHooked = false;  // the RandInt/RandFloat hooks are in (lab or rngtrace): the counts above mean something
bool gInstalled = false;

void* MakeTramp(const uint8_t* orig, uint32_t len, uintptr_t resume) {
    uint8_t* t = static_cast<uint8_t*>(CodePatch::AllocExec(16));
    if (!t) return nullptr;
    memcpy(t, orig, len);
    t[len] = 0xE9;
    const int32_t rel = (int32_t)(resume - (reinterpret_cast<uintptr_t>(t) + len + 5));
    memcpy(t + len + 1, &rel, 4);
    FlushInstructionCache(GetCurrentProcess(), t, 16);
    return t;
}

bool PatchJmp(uintptr_t at, const uint8_t* orig, uint32_t len, void* to, const char* name) {
    uint8_t jmp[8] = {0xE9, 0, 0, 0, 0, 0x90, 0x90, 0x90};
    const int32_t rel = (int32_t)(reinterpret_cast<uintptr_t>(to) - (at + 5));
    memcpy(jmp + 1, &rel, 4);
    return CodePatch::Write(at, orig, jmp, len, name);
}
}  // namespace

bool gIdleLog = false;
std::map<uint32_t, uint32_t> gIdleCallers;  // RNG A draws outside fights per caller ('rnglog 1')
extern "C" {
void* gMK9R_RandIntTramp = nullptr;
void* gMK9R_RandFloatTramp = nullptr;
// caller = return address into the function that called RandInt/RandFloat; ebp = that
// function's frame pointer. Draws through the helpers RandomInt (0x00492050, frame at ebp) and
// 0x00493FD3.. are attributed to *their* caller ([ebp+4]) so the trace names the subsystem.
void __cdecl MK9R_OnRand(uint32_t caller, uint32_t ebp) {
    if (GetCurrentThreadId() != ObjectHooks::gGameThread) {
        // Another thread drawing RNG A moves the gameplay stream at a wall-clock moment (2026-11-04: a desync with
        // identical fighters and only the RNG different). Kept for the report (LogOtherThreads).
        const LONG i = InterlockedIncrement(&gOtherThread) - 1;
        uint32_t c = caller;
        if ((c == 0x00492062 || c == 0x00493FE2) && ebp > 0x10000) {
            MEMORY_BASIC_INFORMATION mbi;
            if (VirtualQuery(reinterpret_cast<void*>(ebp + 4), &mbi, sizeof mbi) && mbi.State == MEM_COMMIT)
                c = *reinterpret_cast<const uint32_t*>(ebp + 4);
        }
        const uint32_t gi = *reinterpret_cast<const volatile uint32_t*>(0x00DEF4B4);
        gOffDraw[i & 63] = {c, (uint32_t)GetCurrentThreadId(), gi ? *reinterpret_cast<const volatile uint32_t*>(gi + 0x70) : 0};
        return;
    }
    if ((caller == 0x00492062 || caller == 0x00493FE2) && ebp > 0x10000) {
        MEMORY_BASIC_INFORMATION mbi;
        if (VirtualQuery(reinterpret_cast<void*>(ebp + 4), &mbi, sizeof mbi) && mbi.State == MEM_COMMIT)
            caller = *reinterpret_cast<const uint32_t*>(ebp + 4);
    }
    if (gIdleLog && !GameLoopHook::FoughtThisTick() && gIdleCallers.size() < 512) ++gIdleCallers[caller];
    const uint32_t exec = *reinterpret_cast<const uint32_t*>(0x00E39DF4);
    if (caller >= 0x0088C700 && caller < 0x00890E00 && exec && gAiExecs.size() < 64 &&
        std::find(gAiExecs.begin(), gAiExecs.end(), exec) == gAiExecs.end())
        gAiExecs.push_back(exec);
    if (gCur && gCur->size() < 4096) {
        gCur->push_back(caller);
        if (gCurExec) gCurExec->push_back(exec);
    }
}
}

// ---- MKScript native trace ----------------------------------------------------------------
// Dispatch: 0x0045C9FF mov [esi+8],eax / mov [esi+3Ch],edx / mov [esi+38h],ecx / call edi
// (11 relocation-free bytes; esi = executor, edi = native; natives return through executor
// +0x1C). The stub replays those bytes, calls the native, then records (native, result).
namespace {
struct NativeRec {
    uint32_t native, exec, result, context, obj;
};
std::vector<NativeRec> gNatOrig[kSlots], gNatResim, *gNatCur = nullptr;
std::vector<uint32_t> gContexts;  // script contexts seen at native dispatch (game thread)
std::vector<uint32_t> gContextSeen;  // GameFrame each context last dispatched a native
std::vector<uint32_t> gRecent;
volatile uint32_t gCurGf = 0;
uint32_t gNatOrigGf[kSlots];

// Natives that reach the online engine (getter 0x0093C7D0 or StartSendingGamepadData 0x0081C080) within two calls
// (static scan of the native table 0x00DF0C40, 2026-10-26; sorted by address). A fight script that calls one sends or
// reads online state, and a rollback replays it: the first call of each is logged (ONLINENATIVE) so runs show which
// ones fights really reach, live or in a replay.
struct OnlineNative {
    uint32_t fn;
    uint16_t id;
};
const OnlineNative kOnlineNatives[] = {
    {0x008B6870, 456}, {0x008B68A0, 453}, {0x008B6900, 459}, {0x008B6920, 454}, {0x008B6950, 455},
    {0x008B6E60, 33}, {0x008B70A0, 66}, {0x008B70B0, 67}, {0x008B9670, 339}, {0x008BF950, 1144},
    {0x008BFAF0, 1156}, {0x008BFBC0, 1165}, {0x008BFBD0, 1166}, {0x008BFBE0, 1167}, {0x008BFBF0, 1168},
    {0x008BFC00, 1169}, {0x008BFC10, 1170}, {0x008BFC20, 1171}, {0x008BFC30, 1172}, {0x008BFC50, 1174},
    {0x008BFC80, 1177}, {0x008BFCB0, 1179}, {0x008BFCC0, 1180}, {0x008BFCE0, 1181}, {0x008BFD00, 1182},
    {0x008BFD10, 1183}, {0x008BFD20, 1184}, {0x008BFD50, 1186}, {0x008BFD60, 1187}, {0x008BFD80, 1188},
    {0x008BFDA0, 1189}, {0x008BFDC0, 1190}, {0x008BFDD0, 1191}, {0x008BFDE0, 1192}, {0x008BFDF0, 1193},
    {0x008BFE00, 1194}, {0x008BFE10, 1195}, {0x008BFE20, 1196}, {0x008BFE60, 1204}, {0x008BFE70, 1205},
    {0x008BFE80, 1206}, {0x008BFEA0, 1208}, {0x008BFEC0, 1209}, {0x008BFF00, 1211}, {0x008BFF40, 1213},
    {0x008BFF60, 1214}, {0x008BFF70, 1215}, {0x008BFF80, 1216}, {0x008BFF90, 1217}, {0x008BFFB0, 1218},
    {0x008BFFD0, 1219}, {0x008BFFF0, 1221}, {0x008C0010, 1222}, {0x008C0030, 1223}, {0x008C0040, 1224},
    {0x008C0070, 1225}, {0x008C00A0, 1226}, {0x008C00D0, 1227}, {0x008C0100, 1228}, {0x008C0180, 1235},
    {0x008C0500, 1259}, {0x008C0520, 1260}, {0x008C0540, 1261}, {0x008C0960, 1297}, {0x008C0AB0, 1310},
    {0x008C0E10, 1342}, {0x008C0E20, 1343}, {0x008C0E50, 1346}, {0x008C0E90, 1350}, {0x008C0ED0, 1352},
    {0x008C0F00, 1353}, {0x008C0F20, 1354}, {0x008C0F50, 1355}, {0x008C0F60, 1356}, {0x008C0F80, 1357},
    {0x008C0FA0, 1358}, {0x008C0FC0, 1359}, {0x008C0FE0, 1360}, {0x008C1090, 1367}, {0x008C10B0, 1368},
    {0x008C10F0, 1370}, {0x008C1110, 1371}, {0x008C1120, 1372}, {0x008C1140, 1373}, {0x008C1170, 1375},
    {0x008C1180, 1376}, {0x008C11A0, 1378}, {0x008C11B0, 1379}, {0x008C11C0, 1380}, {0x008C1200, 1383},
    {0x008C1220, 1384}, {0x008C1270, 1386}, {0x008C1290, 1387}, {0x008C12D0, 1392}, {0x008C3240, 1614},
    {0x008C32A0, 1616}, {0x008C32D0, 1617}, {0x008C3330, 1619}, {0x008D6330, 3261}, {0x008D6450, 3268},
    {0x008D65F0, 3275}, {0x008DF890, 4141}, {0x008E1570, 4288}, {0x008E5CA0, 4674}, {0x008E91C0, 4897},
    {0x008E93A0, 4905}, {0x008E95A0, 4915}, {0x008E95D0, 4916}, {0x008E9600, 4917}, {0x008E9630, 4918},
    {0x008E96F0, 4922}, {0x008E98A0, 4931}, {0x008E9900, 4933}, {0x008E9960, 4935}, {0x008E9BA0, 4948},
    {0x008E9BD0, 4949}, {0x008E9C00, 4950}, {0x008E9E40, 4962}, {0x008EA8B0, 5010}, {0x008EA950, 5013},
    {0x008EAA30, 5017}, {0x008EABA0, 5026}, {0x008EB070, 5051}, {0x008EB170, 5056}, {0x008EB3C0, 5068},
    {0x008EB890, 5091}, {0x008EB8C0, 5092}, {0x008EB980, 5095}, {0x008EB9E0, 5097}, {0x008EBA60, 5099},
    {0x008EBAE0, 5102}, {0x008EBB70, 5106}, {0x008EBC00, 5109}, {0x008EFA10, 1404}, {0x008EFAC0, 1405},
};
constexpr size_t kOnlineNativeCount = sizeof(kOnlineNatives) / sizeof(kOnlineNatives[0]);
bool gOnlineSeen[kOnlineNativeCount];
void NoteOnlineNative(uint32_t fn, uint32_t exec) {
    size_t lo = 0, hi = kOnlineNativeCount;
    while (lo < hi) {
        const size_t mid = (lo + hi) / 2;
        if (kOnlineNatives[mid].fn < fn) lo = mid + 1;
        else hi = mid;
    }
    if (lo == kOnlineNativeCount || kOnlineNatives[lo].fn != fn) return;
    // StartSendingGamepadData (1172) zeroes GameFrame online: every call (up to 20) tells whether fight scripts reach it
    // once per fight or once per round, even offline where 0x0081C080 finds no online engine and does nothing.
    static int startCalls = 0;
    const bool start = kOnlineNatives[lo].id == 1172 && startCalls < 20;
    if (gOnlineSeen[lo] && !start) return;
    if (start) ++startCalls;
    gOnlineSeen[lo] = true;
    Logger::Info("ONLINENATIVE %s: id %u native %08X at gf %u (%s, %s) exec %08X", start ? "call" : "first call", kOnlineNatives[lo].id, fn,
                 (uint32_t)gCurGf, gNatCur == &gNatResim ? "replay" : "live", Rollback::GgpoActive() ? "in a GGPO session" : "no session", exec);
}
}
extern "C" {
uint32_t gMK9R_NatInner = 0;  // operand 1 of the instruction: the real native when native == 0x004596D0 (call)
uint32_t gNatObj = 0;         // object operand of the anim-position native 0x008C80F0, resolved before the call
// 0x008C80F0 reads its object operand with 0x00458930(0, [0x00C6876C], [0x00C68760]) (pure read).
void __cdecl MK9R_PreNative() {
    gNatObj = 0;
    const uint32_t n = gMK9R_NatInner;
    // 0x008CE360: answers obj->vt[0x1B0]() for its object operand; 0 in one pass and 1 in the other, then a runscript
    // (traced soak 11410 S=1880).
    if ((n != 0x008C80F0 && n != 0x008E16E0 && n != 0x008E1740 && n != 0x008BC9B0 && n != 0x008CC6C0 && n != 0x008CE360) ||
        GetCurrentThreadId() != ObjectHooks::gGameThread)
        return;
    using ReadOpFn = uint32_t(__cdecl*)(uint32_t, uint32_t, uint32_t);
    // The call opcode 0x004596D0 advances the operand pointer (executor +0x3C) by 4 before it
    // jumps into the native: emulate that while resolving operand 0, then put it back.
    const uint32_t exec = *reinterpret_cast<const uint32_t*>(0x00E39DF4);
    if (!exec) return;
    uint32_t* opnd = reinterpret_cast<uint32_t*>(exec + 0x3C);
    const uint32_t saved = *opnd;
    *opnd = saved + 4;
    gNatObj = reinterpret_cast<ReadOpFn>(0x00458930)(0, *reinterpret_cast<const uint32_t*>(0x00C6876C),
                                                      *reinterpret_cast<const uint32_t*>(0x00C68760));
    *opnd = saved;
}
void __cdecl MK9R_OnNative(uint32_t native, uint32_t exec) {
    if (native == 0x004596D0) native = gMK9R_NatInner | 0x80000000u;  // attribute to the called native (marked)
    if (GetCurrentThreadId() == ObjectHooks::gGameThread) NoteOnlineNative(native & 0x7FFFFFFF, exec);
    // Remember every script context that runs (their data areas are snapshotted, see Snapshot).
    const uint32_t ctx = *reinterpret_cast<const uint32_t*>(0x00E39DF8);
    // Consecutive natives mostly come from one context: the last one found is checked before a search (up to 512).
    static size_t last = 0;
    auto known = [&] { return last < gContexts.size() && gContexts[last] == ctx; };
    if (ctx && GetCurrentThreadId() == ObjectHooks::gGameThread) {
        if (!known()) last = std::find(gContexts.begin(), gContexts.end(), ctx) - gContexts.begin();
        if (last < gContexts.size()) gContextSeen[last] = gCurGf;
    }
    if (gContexts.size() >= 512)  // drop contexts idle for 10 s (destroyed scripts)
        for (size_t i = gContexts.size(); i-- > 0;)
            if ((int32_t)(gCurGf - gContextSeen[i]) > 600) {
                gContexts.erase(gContexts.begin() + i);
                gContextSeen.erase(gContextSeen.begin() + i);
            }
    if (ctx && GetCurrentThreadId() == ObjectHooks::gGameThread && !known() &&
        std::find(gContexts.begin(), gContexts.end(), ctx) == gContexts.end() && gContexts.size() < 512) {
        gContexts.push_back(ctx);
        gContextSeen.push_back((uint32_t)gCurGf);
        static int logged = 0;
        if (logged < 6) {  // which executor field holds the context? (eager enumeration)
            for (uint32_t o = 0; o < 0xD8; o += 4)
                if (*reinterpret_cast<const uint32_t*>(exec + o) == ctx) {
                    Logger::Info("RNGTRACE context %08X found at executor %08X +0x%X", ctx, exec, o);
                    ++logged;
                }
        }
    }
    if (!gNatCur || gNatCur->size() >= 20000 || GetCurrentThreadId() != ObjectHooks::gGameThread) return;
    gNatCur->push_back({native, exec, *reinterpret_cast<const uint32_t*>(exec + 0x1C), *reinterpret_cast<const uint32_t*>(0x00E39DF8), gNatObj});
}
}
extern "C" void MK9R_HkNativeDispatch();
asm(R"(
    .text
    .globl _MK9R_HkNativeDispatch
_MK9R_HkNativeDispatch:
    movl %eax, 8(%esi)
    movl %edx, 0x3c(%esi)
    movl %ecx, 0x38(%esi)
    movl 4(%edx), %eax
    movl %eax, _gMK9R_NatInner
    pushfl
    pushal
    call _MK9R_PreNative
    popal
    popfl
    movl 8(%esi), %eax
    call *%edi
    pushfl
    pushal
    pushl %esi
    pushl %edi
    call _MK9R_OnNative
    addl $8, %esp
    popal
    popfl
    pushl $0x0045CA0A
    ret
)");

// ---- MKScript VM event trace (docs/script_scheduling.md, proposal 1) ----------------------
// The dominant remaining failing-load class: a VM's first native (0x008B6DA0, "get owner") runs on a different frame in
// the replay, so the VM was spawned, bound, restarted, delayed or gated differently. Each of these entry points records
// {site, ecx, first three stack arguments, return address, current executor} per frame; the replay is compared with
// the original like the natives, and the first differing event names the input.
namespace {
struct VmSite {
    uintptr_t at;
    uint8_t orig[8];
    uint32_t len;
    const char* name;
    uint32_t nargs;  // stack arguments recorded (at most 3); the rest of the stack is the caller's and differs
};
const VmSite kVmSites[] = {
    {0x0045CC80, {0x55, 0x8B, 0xEC, 0x6A, 0xFF}, 5, "ctor", 2},                      // ecx = new VM; ret 8
    {0x0045CB20, {0x55, 0x8B, 0xEC, 0x56, 0x8B, 0x75, 0x08}, 7, "bind", 3},          // cdecl (vm, group, ctx, fn, ...)
    {0x0045CB90, {0x55, 0x8B, 0xEC, 0x56, 0x8B, 0xF1}, 6, "restart", 3},             // ecx = vm; ret 0x14
    {0x0045C610, {0x55, 0x8B, 0xEC, 0x53, 0x8B, 0x5D, 0x08}, 7, "runscript", 2},     // ecx, target, ...; ret 8
    {0x00458320, {0x55, 0x8B, 0xEC, 0x83, 0x79, 0x48, 0x00}, 7, "wait", 1},          // ecx = vm, n; ret 4
    {0x004583F0, {0x55, 0x8B, 0xEC, 0xF3, 0x0F, 0x2A, 0x45, 0x08}, 8, "addwait", 1}, // ecx = vm, n; ret 4
    {0x004583A0, {0x55, 0x8B, 0xEC, 0x8B, 0x45, 0x08}, 6, "gate", 1},                // ecx = vm, value; ret 4
    // Not an entry hook (patched separately, see MK9R_HkContact): the MK object tick 0x008A7300 spawns its
    // pending script (+0x1D0, through 0x0089F7F0) once Cast<>(...)->+0x288->+0x14C is set. Records
    // {object, cast result, +0x288, flag} (traced soak 11503 gf 1793: a spawn only in the replay, ctor up 0x0089F825).
    {0x008A7420, {0x8B, 0x80, 0x88, 0x02, 0x00, 0x00}, 6, "contact", 3},
    // The start of that gate chain, 0x008A73B7 cmp dword [esi+0x1BC],0: in soak 11606 (S=902, S=1330) the replay reached
    // the contact check on the first replayed frame and the original did not, so an earlier gate differed right after the
    // load. Records {object, +0x1BC, actor +0x8, actor Base +0x60} and in up[] {+0x14, [+0x1D8]+0xC, actor bDeleteMe word +0x80}.
    {0x008A73B7, {0x83, 0xBE, 0xBC, 0x01, 0x00, 0x00, 0x00}, 7, "tickgate", 3},
    // MKCollisionObj constructor 0x008A45C0 (entry, MK9R_HkVm9): ecx = new object, args (owner, [owner+0x7638], script
    // arg 8, script arg C); ret 0x10. Called from 0x008A4C70 <- native 0x008D94F0, whose owner comes from the current
    // executor (0x00458930). Soak 12808 S=1199: the object made on the first replayed frame stood next to P1
    // (y -321.7) where the original's stood next to P2 (y -263.8); the owner argument says whether the replay made it
    // for the other object.
    {0x008A45C0, {0x55, 0x8B, 0xEC, 0x6A, 0xFF}, 5, "collctor", 3},
    // Not a code hook: a scene-query result delivered to an MK object's +0x1D8 helper (the report vtable tap below).
    // Records {helper, kind 0 bool / 1 ray, result or hit count, +0xC after, caller}; up[0] = +0xC before (not compared).
    {0, {0}, 0, "rayres", 3},
};
constexpr int kCollCtorSite = 9;
constexpr int kRayResSite = 10;
constexpr int kVmSiteCount = sizeof(kVmSites) / sizeof(kVmSites[0]);
constexpr int kVmEntryHooks = 7;  // the first seven sites are function entries with MK9R_HkVm<i> stubs
struct VmRec {
    uint32_t site, ecx, a1, a2, a3, ret, exec;
    uint32_t up[3];  // ctor only: return addresses above it (ebp chain); the (1, 0xCE) VMs come through the generic
                     // wrappers 0x004925BE / 0x0049265E -> 0x0045D8C0, so the spawner is two or three frames up
    uint32_t raw[2] = {0, 0};  // tickgate: actor and Base pointers, logged but not compared (an actor re-spawned in
                               // the replay has another address: soak 11701 showed every such object differing)
    uint32_t helper = 0;       // tickgate: the +0x1D8 helper whose +0xC is up[1] (not compared)
    uint32_t hf[5] = {0, 0, 0, 0, 0};  // tickgate: helper +4 issued, +8 done, +0x30 hit actor, +0x34 / +0x40 the last query's
                                       // start x / end x (not compared): whether a whole result arrived or only +0xC changed
    bool operator==(const VmRec& o) const {
        // up[] beyond the first frame of a ctor can be stale stack, so it is compared only for the tick-gate probe
        return site == o.site && ecx == o.ecx && a1 == o.a1 && a2 == o.a2 && a3 == o.a3 && ret == o.ret && exec == o.exec &&
               (site != 8 || (up[0] == o.up[0] && up[1] == o.up[1] && up[2] == o.up[2]));
    }
};
std::vector<VmRec> gVmOrig[kSlots], gVmResim, *gVmCur = nullptr;
uint32_t gVmOrigGf[kSlots];
uint32_t gVmLogged = 0;
}  // namespace
extern "C" {
void* gMK9R_VmTramp[kVmSiteCount];
void __cdecl MK9R_OnVmEvent(uint32_t site, const uint32_t* sp, uint32_t ecx, uint32_t ebp) {
    if (!gVmCur || gVmCur->size() >= 4096 || GetCurrentThreadId() != ObjectHooks::gGameThread) return;
    const uint32_t na = kVmSites[site].nargs;
    VmRec r{site, ecx, sp[1], na > 1 ? sp[2] : 0, na > 2 ? sp[3] : 0, sp[0], *reinterpret_cast<const uint32_t*>(0x00E39DF4), {0, 0, 0}};
    if (site == 0) {  // ebp is still the caller's frame: [ebp+4] is the caller's return address
        uint32_t f = ebp;
        for (int d = 0; d < 3 && f > 0x10000; ++d) {
            uint32_t ret = 0, next = 0;
            if (!CrashLog::SafeRead32(f + 4, &ret) || !CrashLog::SafeRead32(f, &next)) break;
            r.up[d] = ret;
            if (next <= f) break;
            f = next;
        }
    }
    if (site == kCollCtorSite) {
        // Soak 12904 S=2289: same owner and arguments in both passes, but the object at 11A50400 in the original and
        // 11A52200 in the replay. Where it came from: the 0x1DC bytes go to the 0x800 pool [0xED5414] (6 elements),
        // then the 0x1000 pool [0xED5418], then the master heap. Logged (not compared): both pools' free-list heads
        // (+0x30) and the master heap's used bytes (+0x28).
        const uint32_t p800 = *reinterpret_cast<const uint32_t*>(0x00ED5414), p1000 = *reinterpret_cast<const uint32_t*>(0x00ED5418),
                       master = *reinterpret_cast<const uint32_t*>(0x00ED53F0);
        if (p800) CrashLog::SafeRead32(p800 + 0x30, &r.up[0]);
        if (p1000) CrashLog::SafeRead32(p1000 + 0x30, &r.up[1]);
        if (master) CrashLog::SafeRead32(master + 0x28, &r.up[2]);
    }
    gVmCur->push_back(r);
}
void __cdecl MK9R_OnContact(uint32_t obj, uint32_t cast) {
    if (!gVmCur || gVmCur->size() >= 4096 || GetCurrentThreadId() != ObjectHooks::gGameThread) return;
    uint32_t comp = 0, flag = 0;
    if (CrashLog::SafeRead32(cast + 0x288, &comp) && comp) CrashLog::SafeRead32(comp + 0x14C, &flag);
    gVmCur->push_back({7, obj, cast, comp, flag, 0x008A7420, *reinterpret_cast<const uint32_t*>(0x00E39DF4), {0, 0, 0}});
}
}
namespace {
void RqGate(uint32_t helper, uint32_t obj, uint32_t c);  // the line-check ring below
}
extern "C" void __cdecl MK9R_OnTickGate(uint32_t obj) {
    if (!gVmCur || gVmCur->size() >= 4096 || GetCurrentThreadId() != ObjectHooks::gGameThread) return;
    uint32_t f1bc = 0, actor = 0, base = 0xFFFFFFFF, f14 = 0, rec = 0, recC = 0xFFFFFFFF, flags = 0;
    CrashLog::SafeRead32(obj + 0x1BC, &f1bc);
    CrashLog::SafeRead32(obj + 0x14, &f14);
    if (CrashLog::SafeRead32(obj + 0x8, &actor) && actor) {
        CrashLog::SafeRead32(actor + 0x60, &base);
        CrashLog::SafeRead32(actor + 0x80, &flags);
    }
    if (CrashLog::SafeRead32(obj + 0x1D8, &rec) && rec) CrashLog::SafeRead32(rec + 0xC, &recC);
    // Compared: +0x1BC, actor present, Base present (the gate needs Base null), +0x14, [+0x1D8]+0xC, actor +0x80 flags.
    VmRec r{8, obj, f1bc, actor != 0, base == 0xFFFFFFFF ? 2u : (base != 0), 0x008A73B7, *reinterpret_cast<const uint32_t*>(0x00E39DF4),
            {f14, recC, flags}};
    r.raw[0] = actor;
    r.raw[1] = base;
    r.helper = rec;
    if (rec) {
        static const uint32_t kHf[5] = {0x4, 0x8, 0x30, 0x34, 0x40};
        for (int i = 0; i < 5; ++i) CrashLog::SafeRead32(rec + kHf[i], &r.hf[i]);
        RqGate(rec, obj, recC);
    }
    gVmCur->push_back(r);
}

// ---- MK object line-check results (VMTRACE site "rayres", lab only) ----
// Soak 12 (2026-11-05, relfloor build): the tick gate's [+0x1D8]+0xC (the helper's "hit") differed at the first replayed
// gate in 6 events over 6 sessions, 5 of them 0 in the original and 1 in the replay, with the helper restored (+0xC saved 0,
// live before the load 0); 13155 S=3254 then spawned the object's script in the replay only and desynced. So something
// wrote +0xC during the replayed frame before the gate. Its known writers are the scene-query report's callbacks
// (0x00DED22C, vtable 0x00D549E8: onBooleanQuery 0x006BDA80, onRaycastQuery 0x006BDB30; RayQueries.h), whoever issued
// the query. Tapped here: a result for an MK helper becomes a VM event on the game thread (compared like the others, so a
// result present or different in one pass is the first differing event), and every such result, on any thread, goes into
// a ring that the tick-gate difference prints for its helper.
namespace {
constexpr uintptr_t kRqReport = 0x00DED22C, kRqReportVt = 0x00D549E8, kRqOnBool = 0x006BDA80, kRqOnRay = 0x006BDB30;
constexpr uint32_t kRqHelperVt = 0x00C6A5B4;
// Soak 13 (2026-11-05) logged MK-helper results off the game thread (helper 57C17B84: not 16-aligned, so an embedded
// helper). Both callers of 0x006BDD50 pass type 1 (0x006BDC00), which writes +8/+0xC itself unless the query object's
// vt[1] says the query went asynchronous; then the report callbacks above deliver it. So the ring also records every
// issue at the two call sites (0x008A7564 the MK object tick, 0x006F3E12 an array of embedded helpers) with +0xC before
// and after and whether it was left in flight, and every tick-gate read: a rec+C difference then lists, in order, what
// touched that helper's +0xC after the load.
constexpr uintptr_t kRqIssueFn = 0x006BDD50;
constexpr uintptr_t kRqIssueSite[2] = {0x008A7564, 0x006F3E12};
constexpr LONG kRqRing = 4096;
uint32_t gRqVt[8];
bool gRqTapped = false;
bool gRqIssueTapped[2] = {false, false};
volatile bool gResimNow = false;
enum RqKind : uint32_t { kRqBool = 0, kRqRay = 1, kRqIssue = 2, kRqGate = 3 };
struct RqWrite {
    uint32_t helper, gf, tid, before, after, ret, kind, result;
    bool resim;
    uint32_t x;  // issue: +4/+8 before (bits 0/1) and after (bits 2/3); gate: the MK object
};
RqWrite gRqRing[kRqRing];
LONG gRqHead = 0;
SRWLOCK gRqLock = SRWLOCK_INIT;
volatile LONG gRqGame = 0, gRqOther = 0;
volatile LONG gRqIssues[2] = {0, 0}, gRqAsync[2] = {0, 0}, gRqBusy[2] = {0, 0};
bool gRqOtherLogged = false, gRqAsyncLogged[2] = {false, false};
bool IsMkHelper(uint32_t p) {
    uint32_t vt = 0;
    return p > 0x10000 && !(p & 3) && CrashLog::SafeRead32(p, &vt) && vt == kRqHelperVt;
}
void RqPush(const RqWrite& w) {
    AcquireSRWLockExclusive(&gRqLock);
    gRqRing[gRqHead++ % kRqRing] = w;
    ReleaseSRWLockExclusive(&gRqLock);
}
// "module+offset" for a code address outside the game image (the off-thread callers are in a relocated DLL).
void RqWhere(uint32_t a, char* out, size_t cap) {
    HMODULE m = nullptr;
    char path[MAX_PATH] = "";
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCSTR>((uintptr_t)a), &m) && m && GetModuleFileNameA(m, path, sizeof path)) {
        const char* base = strrchr(path, '\\');
        snprintf(out, cap, "%s+%X", base ? base + 1 : path, (unsigned)(a - (uint32_t)(uintptr_t)m));
    } else {
        snprintf(out, cap, "?");
    }
}
// The newest issue entry for a helper: which call site issued its last query.
uint32_t RqLastIssuer(uint32_t helper) {
    uint32_t site = 0;
    AcquireSRWLockShared(&gRqLock);
    for (LONG i = gRqHead - 1; i >= 0 && i >= gRqHead - kRqRing; --i) {
        const RqWrite& w = gRqRing[i % kRqRing];
        if (w.helper == helper && w.kind == kRqIssue) {
            site = w.ret - 5;
            break;
        }
    }
    ReleaseSRWLockShared(&gRqLock);
    return site;
}
void RqRecord(uint32_t helper, uint32_t kind, uint32_t result, uint32_t before, uint32_t after, uint32_t ret) {
    const DWORD tid = GetCurrentThreadId();
    const bool game = tid == ObjectHooks::gGameThread;
    InterlockedIncrement(game ? &gRqGame : &gRqOther);
    RqPush({helper, gCurGf, (uint32_t)tid, before, after, ret, kind, result, gResimNow, 0});
    if (!game) {
        if (!gRqOtherLogged) {
            gRqOtherLogged = true;
            char where[96];
            RqWhere(ret, where, sizeof where);
            const uint32_t issuer = RqLastIssuer(helper);
            Logger::Info("RAYRES: a line-check result for MK helper %08X arrived on thread %lu, not the game thread (gf %u, +0xC %u -> %u, "
                         "caller %08X = %s; its last issue %s%08X)", helper, tid, (unsigned)gCurGf, before, after, ret, where,
                         issuer ? "from " : "not in the ring ", issuer);
        }
        return;
    }
    if (gVmCur && gVmCur->size() < 4096)
        gVmCur->push_back({(uint32_t)kRayResSite, helper, kind, result, after, ret, *reinterpret_cast<const uint32_t*>(0x00E39DF4),
                           {before, 0, 0}});
}
using RqOnBoolFn = uint32_t(__thiscall*)(void*, uint32_t, uint32_t);
using RqOnRayFn = uint32_t(__thiscall*)(void*, uint32_t, uint32_t, const void*);
// thiscall emulated with fastcall (EDX unused), as RayQueries does.
uint32_t __fastcall RqOnBool(void* self, void*, uint32_t userData, uint32_t result) {
    const uint32_t ret = (uint32_t)(uintptr_t)__builtin_return_address(0);
    if (!IsMkHelper(userData)) return reinterpret_cast<RqOnBoolFn>(kRqOnBool)(self, userData, result);
    uint32_t before = 0xFFFFFFFF, after = 0xFFFFFFFF;
    CrashLog::SafeRead32(userData + 0xC, &before);
    const uint32_t r = reinterpret_cast<RqOnBoolFn>(kRqOnBool)(self, userData, result);
    CrashLog::SafeRead32(userData + 0xC, &after);
    RqRecord(userData, 0, result, before, after, ret);
    return r;
}
uint32_t __fastcall RqOnRay(void* self, void*, uint32_t userData, uint32_t nbHits, const void* hits) {
    const uint32_t ret = (uint32_t)(uintptr_t)__builtin_return_address(0);
    if (!IsMkHelper(userData)) return reinterpret_cast<RqOnRayFn>(kRqOnRay)(self, userData, nbHits, hits);
    uint32_t before = 0xFFFFFFFF, after = 0xFFFFFFFF;
    CrashLog::SafeRead32(userData + 0xC, &before);
    const uint32_t r = reinterpret_cast<RqOnRayFn>(kRqOnRay)(self, userData, nbHits, hits);
    CrashLog::SafeRead32(userData + 0xC, &after);
    RqRecord(userData, 1, nbHits, before, after, ret);
    return r;
}
// The calls to 0x006BDD50 at 0x008A7564 and 0x006F3E12: thiscall(sys, start, end, a3, queryObj, type, helper), ret 0x18.
using RqIssueFn = void(__thiscall*)(void*, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t);
void __fastcall RqIssue(void* sys, void*, uint32_t start, uint32_t end, uint32_t a3, uint32_t a4, uint32_t type, uint32_t helper) {
    const uint32_t ret = (uint32_t)(uintptr_t)__builtin_return_address(0);
    const int s = ret == kRqIssueSite[1] + 5 ? 1 : 0;
    const bool mk = IsMkHelper(helper);
    uint32_t b4 = 0, b8 = 0, bC = 0xFFFFFFFF;
    if (mk) {
        CrashLog::SafeRead32(helper + 4, &b4);
        CrashLog::SafeRead32(helper + 8, &b8);
        CrashLog::SafeRead32(helper + 0xC, &bC);
    }
    reinterpret_cast<RqIssueFn>(kRqIssueFn)(sys, start, end, a3, a4, type, helper);
    if (!mk) return;
    uint32_t f4 = 0, f8 = 0, fC = 0xFFFFFFFF;
    CrashLog::SafeRead32(helper + 4, &f4);
    CrashLog::SafeRead32(helper + 8, &f8);
    CrashLog::SafeRead32(helper + 0xC, &fC);
    const bool busy = b4 && !b8;       // in flight already: the helper's vt[1] refuses a new query
    const bool async = !busy && f4 && !f8;  // issued and not answered inside the call
    InterlockedIncrement(busy ? &gRqBusy[s] : &gRqIssues[s]);
    if (async) InterlockedIncrement(&gRqAsync[s]);
    RqPush({helper, gCurGf, (uint32_t)GetCurrentThreadId(), bC, fC, ret, kRqIssue, type, gResimNow,
            (b4 ? 1u : 0u) | (b8 ? 2u : 0u) | (f4 ? 4u : 0u) | (f8 ? 8u : 0u)});
    if (async && !gRqAsyncLogged[s]) {
        gRqAsyncLogged[s] = true;
        Logger::Info("RAYISSUE: a line check from 0x%08X for helper %08X went asynchronous (left in flight) at gf %u%s, thread %lu",
                     (unsigned)kRqIssueSite[s], helper, (unsigned)gCurGf, gResimNow ? " (replay)" : "", GetCurrentThreadId());
    }
}
void RqGate(uint32_t helper, uint32_t obj, uint32_t c) {
    if (!gRqTapped) return;
    RqPush({helper, gCurGf, (uint32_t)GetCurrentThreadId(), c, c, 0x008A73B7, kRqGate, 0, gResimNow, obj});
}
// The ring's latest entries for one helper (results, issues, gate reads), newest first.
void RqDescribe(uint32_t helper, char* out, size_t cap) {
    int k = snprintf(out, cap, "%ld MK results so far (%ld off the game thread); issues 8A7564 %ld (async %ld, busy %ld), 6F3E12 %ld "
                     "(async %ld, busy %ld); for %08X:", (long)(gRqGame + gRqOther), (long)gRqOther, (long)gRqIssues[0],
                     (long)gRqAsync[0], (long)gRqBusy[0], (long)gRqIssues[1], (long)gRqAsync[1], (long)gRqBusy[1], helper);
    int shown = 0;
    AcquireSRWLockShared(&gRqLock);
    for (LONG i = gRqHead - 1; i >= 0 && i >= gRqHead - kRqRing && shown < 14 && k > 0 && (size_t)k < cap; --i) {
        const RqWrite& w = gRqRing[i % kRqRing];
        if (w.helper != helper) continue;
        ++shown;
        const char* thr = w.tid == ObjectHooks::gGameThread ? "game" : "OTHER";
        if (w.kind == kRqGate)
            k += snprintf(out + k, cap - k, " [gf %u %s gate obj %08X read +C %u]", w.gf, w.resim ? "replay" : "live", w.x, w.before);
        else if (w.kind == kRqIssue)
            k += snprintf(out + k, cap - k, " [gf %u %s %s issue@%06X %s +C %u>%u +4/+8 %u%u>%u%u]", w.gf, w.resim ? "replay" : "live", thr,
                          w.ret - 5, (w.x & 1) && !(w.x & 2) ? "busy" : ((w.x & 4) && !(w.x & 8) ? "async" : "sync"), w.before, w.after,
                          w.x & 1, (w.x >> 1) & 1, (w.x >> 2) & 1, (w.x >> 3) & 1);
        else
            k += snprintf(out + k, cap - k, " [gf %u %s %s tid %u %s %u +C %u>%u by %08X]", w.gf, w.resim ? "replay" : "live", thr, w.tid,
                          w.kind == kRqRay ? "ray" : "bool", w.result, w.before, w.after, w.ret);
    }
    ReleaseSRWLockShared(&gRqLock);
    if (!shown && k > 0 && (size_t)k < cap) snprintf(out + k, cap - k, " none in the last %ld", (long)kRqRing);
}
}  // namespace
extern "C" void MK9R_HkContact();
extern "C" void MK9R_HkTickGate();
asm(R"(
    .text
    .globl _MK9R_HkTickGate
_MK9R_HkTickGate:
    pushfl
    pushal
    pushl %esi
    call _MK9R_OnTickGate
    addl $4, %esp
    popal
    popfl
    cmpl $0, 0x1bc(%esi)
    pushl $0x008A73BE
    ret
)");
asm(R"(
    .text
    .globl _MK9R_HkContact
_MK9R_HkContact:
    pushfl
    pushal
    pushl %eax
    pushl %esi
    call _MK9R_OnContact
    addl $8, %esp
    popal
    popfl
    movl 0x288(%eax), %eax
    pushl $0x008A7426
    ret
)");
#define MK9R_VM_STUB(i)                     \
    ".globl _MK9R_HkVm" #i "\n"             \
    "_MK9R_HkVm" #i ":\n"                   \
    "    pushfl\n"                          \
    "    pushal\n"                          \
    "    pushl %ebp\n"                      \
    "    pushl %ecx\n"                      \
    "    leal 44(%esp), %eax\n"             \
    "    pushl %eax\n"                      \
    "    pushl $" #i "\n"                   \
    "    call _MK9R_OnVmEvent\n"            \
    "    addl $16, %esp\n"                  \
    "    popal\n"                           \
    "    popfl\n"                           \
    "    jmp *_gMK9R_VmTramp+4*" #i "\n"
asm(".text\n" MK9R_VM_STUB(0) MK9R_VM_STUB(1) MK9R_VM_STUB(2) MK9R_VM_STUB(3) MK9R_VM_STUB(4) MK9R_VM_STUB(5)
        MK9R_VM_STUB(6) MK9R_VM_STUB(9));
extern "C" void MK9R_HkVm9();
extern "C" void MK9R_HkVm0();
extern "C" void MK9R_HkVm1();
extern "C" void MK9R_HkVm2();
extern "C" void MK9R_HkVm3();
extern "C" void MK9R_HkVm4();
extern "C" void MK9R_HkVm5();
extern "C" void MK9R_HkVm6();

extern "C" void MK9R_HkRandInt();
extern "C" void MK9R_HkRandFloat();
asm(R"(
    .text
    .globl _MK9R_HkRandInt
_MK9R_HkRandInt:
    pushfl
    pushal
    pushl 8(%esp)
    pushl 40(%esp)
    call _MK9R_OnRand
    addl $8, %esp
    popal
    popfl
    jmp *_gMK9R_RandIntTramp
    .globl _MK9R_HkRandFloat
_MK9R_HkRandFloat:
    pushfl
    pushal
    pushl 8(%esp)
    pushl 40(%esp)
    call _MK9R_OnRand
    addl $8, %esp
    popal
    popfl
    jmp *_gMK9R_RandFloatTramp
)");

namespace RngTrace {

bool Install() {
    if (gInstalled) return true;
    // Players get only the native dispatch hook: its script contexts feed the snapshots' script_data spans. The RNG
    // draw and VM/tick traces below are for replay comparisons ('lab 1', docs/restructure_plan.md 1c).
    static const uint8_t kDispatchOrig[11] = {0x89, 0x46, 0x08, 0x89, 0x56, 0x3C, 0x89, 0x4E, 0x38, 0xFF, 0xD7};
    uint8_t j[11] = {0xE9, 0, 0, 0, 0, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90};
    const int32_t rel = (int32_t)(reinterpret_cast<uintptr_t>(&MK9R_HkNativeDispatch) - (0x0045C9FF + 5));
    memcpy(j + 1, &rel, 4);
    if (!CodePatch::Write(0x0045C9FF, kDispatchOrig, j, 11, "MKScript native dispatch 0x0045C9FF (trace)"))
        Logger::Info("RNGTRACE: native dispatch trace not installed");
    auto hookRand = [] {
        gMK9R_RandIntTramp = MakeTramp(kRandIntOrig, 5, kRandInt + 5);
        gMK9R_RandFloatTramp = MakeTramp(kRandFloatOrig, 6, kRandFloat + 6);
        if (!gMK9R_RandIntTramp || !gMK9R_RandFloatTramp) return false;
        if (!PatchJmp(kRandInt, kRandIntOrig, 5, reinterpret_cast<void*>(&MK9R_HkRandInt), "RandInt 0x005F5080 (trace)"))
            return false;
        if (!PatchJmp(kRandFloat, kRandFloatOrig, 6, reinterpret_cast<void*>(&MK9R_HkRandFloat), "RandFloat 0x005F50A0 (trace)"))
            return false;
        gRandHooked = true;
        return true;
    };
    if (!Lab::On()) {
        if (gRngOnly && !hookRand()) Logger::Info("RNGTRACE: RandInt/RandFloat hooks not installed");
        gInstalled = true;
        return true;
    }
    if (!hookRand()) return false;
    void (*const stubs[kVmEntryHooks])() = {MK9R_HkVm0, MK9R_HkVm1, MK9R_HkVm2, MK9R_HkVm3, MK9R_HkVm4, MK9R_HkVm5, MK9R_HkVm6};
    for (int i = 0; i < kVmEntryHooks; ++i) {
        const VmSite& s = kVmSites[i];
        char name[64];
        snprintf(name, sizeof name, "MKScript VM %s 0x%08X (trace)", s.name, (unsigned)s.at);
        gMK9R_VmTramp[i] = MakeTramp(s.orig, s.len, s.at + s.len);
        if (!gMK9R_VmTramp[i] || !PatchJmp(s.at, s.orig, s.len, reinterpret_cast<void*>(stubs[i]), name))
            Logger::Info("VMTRACE: %s not installed", name);
    }
    if (!PatchJmp(kVmSites[7].at, kVmSites[7].orig, kVmSites[7].len, reinterpret_cast<void*>(&MK9R_HkContact),
                  "MK object tick contact check 0x008A7420 (trace)"))
        Logger::Info("VMTRACE: contact probe 0x008A7420 not installed");
    if (!PatchJmp(kVmSites[8].at, kVmSites[8].orig, kVmSites[8].len, reinterpret_cast<void*>(&MK9R_HkTickGate),
                  "MK object tick gate 0x008A73B7 (trace)"))
        Logger::Info("VMTRACE: tick-gate probe 0x008A73B7 not installed");
    {
        const VmSite& s = kVmSites[kCollCtorSite];
        gMK9R_VmTramp[kCollCtorSite] = MakeTramp(s.orig, s.len, s.at + s.len);
        if (!gMK9R_VmTramp[kCollCtorSite] ||
            !PatchJmp(s.at, s.orig, s.len, reinterpret_cast<void*>(&MK9R_HkVm9), "MKCollisionObj constructor 0x008A45C0 (trace)"))
            Logger::Info("VMTRACE: collision-object constructor probe 0x008A45C0 not installed");
    }
    {
        const uint32_t* vt = reinterpret_cast<const uint32_t*>(kRqReportVt);
        if (vt[0] == kRqOnBool && vt[1] == kRqOnRay) {
            memcpy(gRqVt, vt, sizeof gRqVt);
            gRqVt[0] = (uint32_t)reinterpret_cast<uintptr_t>(&RqOnBool);
            gRqVt[1] = (uint32_t)reinterpret_cast<uintptr_t>(&RqOnRay);
            gRqTapped = HookManager::PatchPointer(kRqReport, kRqReportVt, gRqVt, "scene query report vtable 0x00DED22C (rayres trace)");
        }
        if (!gRqTapped) Logger::Info("VMTRACE: line-check result tap (rayres) not installed");
        // E8 rel32 at both calls to 0x006BDD50 (not 4-byte aligned: CodePatch with the expected bytes).
        for (int s = 0; gRqTapped && s < 2; ++s) {
            uint8_t expected[5] = {0xE8}, replacement[5] = {0xE8};
            const int32_t relOld = (int32_t)(kRqIssueFn - (kRqIssueSite[s] + 5));
            const int32_t relNew = (int32_t)(reinterpret_cast<uintptr_t>(&RqIssue) - (kRqIssueSite[s] + 5));
            memcpy(expected + 1, &relOld, 4);
            memcpy(replacement + 1, &relNew, 4);
            gRqIssueTapped[s] = CodePatch::Write(kRqIssueSite[s], expected, replacement, 5,
                                                 s ? "embedded helper line check issue 0x006F3E12 (rayres trace)"
                                                   : "MK object line check issue 0x008A7564 (rayres trace)");
            if (!gRqIssueTapped[s]) Logger::Info("VMTRACE: line-check issue tap 0x%08X not installed", (unsigned)kRqIssueSite[s]);
        }
    }
    gInstalled = true;
    return true;
}

void SetIdleLog(bool on) {
    if (on) Install();
    gIdleLog = on;
    Logger::Info("RNGLOG %s: RNG A draws outside fights are counted per caller and listed at the next fight start", on ? "on" : "off");
}
void DumpIdle() {
    if (!gIdleLog) return;
    std::vector<std::pair<uint32_t, uint32_t>> v(gIdleCallers.begin(), gIdleCallers.end());
    std::sort(v.begin(), v.end(), [](const auto& a, const auto& b) { return a.second > b.second; });
    char line[600];
    int n = 0;
    for (size_t i = 0; i < v.size() && i < 24 && n < 560; ++i) n += snprintf(line + n, sizeof line - n, " %08X:%u", v[i].first, v[i].second);
    Logger::Info("RNGLOG draws since the last fight (caller:count):%s", n ? line : " none");
    gIdleCallers.clear();
}

void BeginFrame(uint32_t gf, bool resimulating) {
    if (!gInstalled) return;
    gCurGf = gf;  // the script contexts' "dispatched recently" (ScriptContexts) needs it in every mode
    gResimNow = resimulating;
    if (!Lab::On()) {
        // The original/replay native and VM sequences only feed EndResimFrame's comparisons ('lab 1'): recording up to
        // 20000 natives a frame and logging a NATIVETRACE line for nearly every replayed frame (corrected inputs make
        // the replay differ) cost every rollback in a player's game.
        gNatCur = nullptr;
        gVmCur = nullptr;
        gCur = nullptr;
        gCurExec = nullptr;
        if (gRandHooked && !resimulating) {  // 'rngtrace 1': the live frames' RNG A callers only (LogFrameDraws)
            FrameDraws& s = gOrig[gf % kSlots];
            s.gf = gf;
            s.callers.clear();
            gOrigExec[gf % kSlots].clear();
            gCur = &s.callers;
            gCurExec = &gOrigExec[gf % kSlots];
        }
        return;
    }
    if (resimulating) {
        gResim.clear();
        gResimExec.clear();
        gCur = &gResim;
        gCurExec = &gResimExec;
        gNatResim.clear();
        gNatCur = &gNatResim;
        gVmResim.clear();
        gVmCur = &gVmResim;
        return;
    }
    gVmOrig[gf % kSlots].clear();
    gVmOrigGf[gf % kSlots] = gf;
    gVmCur = &gVmOrig[gf % kSlots];
    gNatOrig[gf % kSlots].clear();
    gNatOrigGf[gf % kSlots] = gf;
    gNatCur = &gNatOrig[gf % kSlots];
    FrameDraws& s = gOrig[gf % kSlots];
    s.gf = gf;
    s.callers.clear();
    gOrigExec[gf % kSlots].clear();
    gCur = &s.callers;
    gCurExec = &gOrigExec[gf % kSlots];
}

void AiExecutors(std::vector<uint32_t>& out) { out = gAiExecs; }
const std::vector<uint32_t>& ScriptContexts() {
    // Only contexts that ran recently: the list grows for the whole fight, and a context destroyed
    // long ago names a freed (possibly reused) data block; restoring it corrupted the GMalloc medium
    // heap's free tree (0x0042AF1D / 0x0042AFC2, 2026-10-15).
    gRecent.clear();
    for (size_t i = 0; i < gContexts.size(); ++i)
        // Dispatched this frame or the previous one: a context whose script ended at D is captured
        // last in the snapshot of D, and the quarantine keeps its freed blocks until no snapshot
        // predates the free (D + kRing). A 60-frame window outlived that and restored freed memory.
        if ((int32_t)(gCurGf - gContextSeen[i]) <= 1) gRecent.push_back(gContexts[i]);
    return gRecent;
}
void ClearScriptContexts() {
    gContexts.clear();
    gContextSeen.clear();
}

bool EndResimFrame(uint32_t gf, int k) {
    if (!gInstalled) return true;
    if (!gNatCur && !gVmCur && !gCur) return true;  // nothing recorded for this replayed frame (lab off, BeginFrame)
    FrameDraws& s = gOrig[gf % kSlots];
    bool same = true;
    if (s.gf == gf) {
        const size_t n = s.callers.size() < gResim.size() ? s.callers.size() : gResim.size();
        size_t i = 0;
        while (i < n && s.callers[i] == gResim[i]) ++i;
        if (i < n || s.callers.size() != gResim.size()) {
            same = false;
            Logger::Info("RNGTRACE k=%d gf=%u: draw #%u differs: original %u draws (caller %08X), resim %u draws (caller %08X)",
                         k, gf, (unsigned)i, (unsigned)s.callers.size(), i < s.callers.size() ? s.callers[i] : 0,
                         (unsigned)gResim.size(), i < gResim.size() ? gResim[i] : 0);
            char line[256];
            int ln = 0;
            for (size_t j = i > 3 ? i - 3 : 0; j < i + 6 && j < s.callers.size(); ++j)
                ln += snprintf(line + ln, sizeof line - ln, " %08X", s.callers[j]);
            Logger::Info("RNGTRACE   original around:%s", line);
            ln = 0;
            for (size_t j = i > 3 ? i - 3 : 0; j < i + 6 && j < gResim.size(); ++j)
                ln += snprintf(line + ln, sizeof line - ln, " %08X", gResim[j]);
            Logger::Info("RNGTRACE   resim around:   %s", line);
            const std::vector<uint32_t>& oe = gOrigExec[gf % kSlots];
            const uint32_t eo = i < oe.size() ? oe[i] : 0, er = i < gResimExec.size() ? gResimExec[i] : 0;
            Logger::Info("RNGTRACE   executor at the diverging draw: original %08X, resim %08X", eo, er);
        }
    }
    // VM events: the first one that differs (one report per resimulated frame, at most 200 per process).
    if (gVmOrigGf[gf % kSlots] == gf) {
        const std::vector<VmRec>& o = gVmOrig[gf % kSlots];
        const size_t n = o.size() < gVmResim.size() ? o.size() : gVmResim.size();
        size_t i = 0;
        while (i < n && o[i] == gVmResim[i]) ++i;
        if ((i < n || o.size() != gVmResim.size()) && gVmLogged++ < 200) {
            auto fmt = [](const std::vector<VmRec>& v, size_t i, char* out, size_t cap) {
                if (i >= v.size()) {
                    snprintf(out, cap, "none");
                    return;
                }
                const VmRec& r = v[i];
                const int k = snprintf(out, cap, "%s ecx %08X args %08X %08X %08X ret %08X exec %08X", kVmSites[r.site].name, r.ecx,
                                       r.a1, r.a2, r.a3, r.ret, r.exec);
                if (r.site == 0 && k > 0 && (size_t)k < cap)
                    snprintf(out + k, cap - k, " up %08X %08X %08X", r.up[0], r.up[1], r.up[2]);
                if (r.site == kCollCtorSite && k > 0 && (size_t)k < cap)
                    snprintf(out + k, cap - k, " pool800 head %08X pool1000 head %08X master used %08X", r.up[0], r.up[1], r.up[2]);
                if (r.site == 8 && k > 0 && (size_t)k < cap)
                    snprintf(out + k, cap - k, " +14 %08X rec+C %08X actor+80 %08X | actor %08X base %08X | helper +4 %X +8 %X +30 %08X "
                             "+34 %08X +40 %08X", r.up[0], r.up[1], r.up[2], r.raw[0], r.raw[1], r.hf[0], r.hf[1], r.hf[2], r.hf[3], r.hf[4]);
                if (r.site == kRayResSite && k > 0 && (size_t)k < cap) snprintf(out + k, cap - k, " (+C before %08X)", r.up[0]);
            };
            char a[400], b[400];
            fmt(o, i, a, sizeof a);
            fmt(gVmResim, i, b, sizeof b);
            Logger::Info("VMTRACE k=%d gf=%u: event #%u of %u/%u differs: original %s | resim %s", k, gf, (unsigned)i, (unsigned)o.size(),
                         (unsigned)gVmResim.size(), a, b);
            if (i < n && o[i].site == 8 && gVmResim[i].site == 8 && o[i].up[1] != gVmResim[i].up[1]) {
                char d[300];
                Snapshot::DescribeHelper(gVmResim[i].ecx, gVmResim[i].helper, d, sizeof d);
                Logger::Info("VMTRACE   tick-gate helper: original %08X, resim %08X; %s", o[i].helper, gVmResim[i].helper, d);
                if (gRqTapped) {
                    char w[2400];
                    RqDescribe(gVmResim[i].helper, w, sizeof w);
                    Logger::Info("VMTRACE   helper line-check results (newest first): %s", w);
                }
            } else if (gRqTapped && ((i < o.size() && o[i].site == kRayResSite) || (i < gVmResim.size() && gVmResim[i].site == kRayResSite))) {
                char w[2400];
                RqDescribe(i < gVmResim.size() && gVmResim[i].site == kRayResSite ? gVmResim[i].ecx : o[i].ecx, w, sizeof w);
                Logger::Info("VMTRACE   line-check results (newest first): %s", w);
            }
        }
    }
    gVmOrig[gf % kSlots] = gVmResim;
    gVmOrigGf[gf % kSlots] = gf;
    gVmCur = nullptr;
    // Natives: first record whose native matches but whose result differs (a native reading
    // state the load did not restore), or the first point where the call sequences part.
    {
        // Sound natives (0x008E16E0/0x008E1740 -> 0x00921200 -> 0x00461E10: play by name, returns a
        // handle) differ for cosmetic reasons and would hide the first gameplay-relevant one.
        auto cosmetic = [](const NativeRec& r) {
            const uint32_t n = r.native & 0x7FFFFFFF;
            return n == 0x008E16E0 || n == 0x008E1740;
        };
        // The handle stays in the executor's result slot (+0x1C) and shows up as the "result" of
        // later records: the store op 0x00458AC0 copies it into a variable, and natives that set
        // no result (e.g. the wait 0x008B5E00) report it unchanged (soak29: most first divergences,
        // in rollbacks that ended exact). Such results are normalised per executor.
        auto filter = [&](const std::vector<NativeRec>& in, std::vector<NativeRec>& out) {
            std::vector<std::pair<uint32_t, uint32_t>> handle;  // executor -> last sound-native result
            out.clear();
            for (const NativeRec& r : in) {
                auto it = std::find_if(handle.begin(), handle.end(), [&](const auto& h) { return h.first == r.exec; });
                if (cosmetic(r)) {
                    if (it == handle.end()) handle.push_back({r.exec, r.result});
                    else it->second = r.result;
                    continue;
                }
                NativeRec c = r;
                if (it != handle.end() && c.result == it->second) c.result = 0xC05E71C5;  // "cosmetic"
                out.push_back(c);
            }
        };
        std::vector<NativeRec> o, rs;
        filter(gNatOrig[gf % kSlots], o);
        filter(gNatResim, rs);
        gNatResim.swap(rs);
        if (gNatOrigGf[gf % kSlots] == gf) {
            const size_t n = o.size() < gNatResim.size() ? o.size() : gNatResim.size();
            size_t i = 0;
            while (i < n && o[i].native == gNatResim[i].native && o[i].exec == gNatResim[i].exec && o[i].result == gNatResim[i].result) ++i;
            if (i < n || o.size() != gNatResim.size()) {
                const NativeRec a = i < o.size() ? o[i] : NativeRec{0, 0, 0, 0, 0};
                const NativeRec b = i < gNatResim.size() ? gNatResim[i] : NativeRec{0, 0, 0, 0, 0};
                Logger::Info("NATIVETRACE k=%d gf=%u: call #%u of %u/%u differs: original native %08X exec %08X -> %08X, resim native %08X exec %08X -> %08X",
                             k, gf, (unsigned)i, (unsigned)o.size(), (unsigned)gNatResim.size(), a.native, a.exec, a.result, b.native,
                             b.exec, b.result);
                // The producer: the executor's previous record (a store copies its result).
                for (size_t j = i; j-- > 0;)
                    if (o[j].exec == a.exec) {
                        Logger::Info("NATIVETRACE   previous record of exec %08X: native %08X -> %08X", a.exec, o[j].native, o[j].result);
                        break;
                    }
                // Where do this executor's variables live? (locals [exec+0x10], context data
                // [ctx+0x84], context variable table [ctx+0xA0]) - inside the master image or not?
                const uint32_t m = *reinterpret_cast<const uint32_t*>(0x00ED53F0);
                const uint32_t mb = m ? *reinterpret_cast<const uint32_t*>(m + 0x34) : 0, ms = m ? *reinterpret_cast<const uint32_t*>(m + 0x24) : 0;
                auto where = [&](uint32_t p) { return p >= mb && p < mb + ms ? "image" : (p ? "OUTSIDE" : "null"); };
                const uint32_t loc = b.exec ? *reinterpret_cast<const uint32_t*>(b.exec + 0x10) : 0;
                const uint32_t ctx = b.context;
                const uint32_t data = ctx ? *reinterpret_cast<const uint32_t*>(ctx + 0x84) : 0;
                const uint32_t vars = ctx ? *reinterpret_cast<const uint32_t*>(ctx + 0xA0) : 0;
                Logger::Info("NATIVETRACE   exec %08X locals %08X (%s) | context %08X (%s) data %08X (%s) vartable %08X (%s)", b.exec, loc,
                             where(loc), ctx, where(ctx), data, where(data), vars, where(vars));
                if (b.obj) {
                    char od[160] = "?";
                    Ue3Names::Describe(b.obj, od, sizeof od);
                    const uint32_t vt = *reinterpret_cast<const uint32_t*>(b.obj);
                    Logger::Info("NATIVETRACE   operand object %08X MK %s / UE3 %s vt %08X vt[8] %08X", b.obj, ObjectHooks::TypeOf(b.obj), od,
                                 vt, vt > 0x00400000 && vt < 0x010AE000 ? *reinterpret_cast<const uint32_t*>(vt + 0x20) : 0);
                }
                if (b.obj && (b.native & 0x7FFFFFFF) == 0x008C80F0) {
                    const uint32_t comp = *reinterpret_cast<const uint32_t*>(b.obj + 0x10C);
                    const uint32_t tree = comp ? *reinterpret_cast<const uint32_t*>(comp + 0x280) : 0;
                    char cn[80] = "?", tn[80] = "?";
                    Ue3Names::Describe(comp, cn, sizeof cn);
                    Ue3Names::Describe(tree, tn, sizeof tn);
                    Logger::Info("NATIVETRACE   anim-position object %08X %s: +0x10C %08X (%s, %s) -> +0x280 %08X (%s, %s) +0xD0 %08X", b.obj,
                                 ObjectHooks::TypeOf(b.obj), comp, where(comp), cn, tree, where(tree), tn,
                                 tree ? *reinterpret_cast<const uint32_t*>(tree + 0xD0) : 0);
                    // 0x0043B070 (channel lookup in obj+0x1B0) WRITES the channel record, so only its
                    // pure part is replayed: 0x004355A0(obj, bone) walks the MK anim tree (obj+0x194) to
                    // the node playing on the bone; the position is that node's +0x74.
                    const uint32_t bone = tree ? *reinterpret_cast<const uint32_t*>(tree + 0xD0) : 0xFFFFFFFF;
                    using FindNodeFn = uint32_t(__thiscall*)(uint32_t, uint32_t);
                    const uint32_t node = bone != 0xFFFFFFFF ? reinterpret_cast<FindNodeFn>(0x004355A0)(b.obj, bone) : 0;
                    if (node) {
                        float p = 0;
                        memcpy(&p, reinterpret_cast<const void*>(node + 0x74), 4);
                        const uint32_t ue = *reinterpret_cast<const uint32_t*>(node + 8);
                        char un[120] = "?";
                        Ue3Names::Describe(ue, un, sizeof un);
                        Logger::Info("NATIVETRACE   anim node %08X %s pos(+0x74) %.3f, +8 %08X %s", node, ObjectHooks::TypeOf(node), p, ue, un);
                        Rollback::LogSpansContaining(node + 0x74);
                        if (ue) Rollback::LogSpansContaining(ue);
                    }
                }
            }
        }
        gNatOrig[gf % kSlots] = gNatResim;
        gNatOrigGf[gf % kSlots] = gf;
        gNatCur = nullptr;
    }
    s.gf = gf;
    s.callers = gResim;
    gOrigExec[gf % kSlots] = gResimExec;
    gCur = nullptr;
    gCurExec = nullptr;
    return same;
}

void RebaseFrames(uint32_t oldGf, uint32_t newGf) {
    for (uint32_t& s : gContextSeen) s += newGf - oldGf;  // keeps "dispatched recently" for the snapshot of newGf
    gCurGf = newGf;
    for (FrameDraws& s : gOrig) s.gf = 0xFFFFFFFF;  // per-frame sequences of the old numbering
    for (uint32_t& g : gNatOrigGf) g = 0xFFFFFFFF;
}

void SetRngOnly(bool on) {
    gRngOnly = on;
    Logger::Info("RNGTRACE %d: RNG A callers per live frame %s (takes effect when the rollback patch set installs)", on ? 1 : 0,
                 on ? "recorded" : "only with 'lab 1'");
}
void LogOtherThreads(const char* tag) {
    if (!gRandHooked) return;
    const LONG n = gOtherThread;
    char line[640];
    int k = 0;
    for (LONG i = n > 16 ? n - 16 : 0; i < n && k < 600; ++i) {
        const OffDraw& d = gOffDraw[i & 63];
        k += snprintf(line + k, sizeof line - k, " %08X(t%u gf%u)", d.caller, d.tid, d.gf);
    }
    line[k] = 0;
    Logger::Info("%s: %ld RNG A draw(s) off the game thread so far; last:%s", tag, (long)n, k ? line : " none");
}

void LogFrameDraws(uint32_t gf, const char* tag) {
    if (!gInstalled) return;
    const FrameDraws& s = gOrig[gf % kSlots];
    if (s.gf != gf) {
        Logger::Info("%s gf=%u: RNG A draws no longer recorded", tag, gf);
        return;
    }
    const std::vector<uint32_t>& ex = gOrigExec[gf % kSlots];
    char line[700];
    int n = 0;
    for (size_t i = 0; i < s.callers.size() && i < 24 && n < 660; ++i)
        n += snprintf(line + n, sizeof line - n, " %08X(%08X)", s.callers[i], i < ex.size() ? ex[i] : 0);
    line[n] = 0;
    Logger::Info("%s gf=%u: %u RNG A draws, caller(executor):%s", tag, gf, (unsigned)s.callers.size(), line);
}

}  // namespace RngTrace
