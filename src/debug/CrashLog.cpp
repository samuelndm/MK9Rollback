#include "CrashLog.h"

#include <windows.h>
#include <tlhelp32.h>

#include <algorithm>
#include <vector>

#include <cstdint>
#include <cstdio>

#include "../common/GuardPage.h"
#include "../common/StrAppend.h"
#include "../hook/CodePatch.h"
#include "Logger.h"
#include "Ue3Names.h"

// ---- guarded call of the GMalloc block lookup 0x00405A10(ptr, out) ----
extern "C" {
volatile DWORD MK9R_gProbeActive = 0, MK9R_gProbeThread = 0, MK9R_gProbeEsp = 0, MK9R_gProbeEbp = 0;
int __cdecl MK9R_SafeBlockInfoAsm(const void* ptr, uint32_t* out);
void MK9R_ProbeRecover();
}
#if defined(_MSC_VER) && !defined(__clang__)
__declspec(naked) int __cdecl MK9R_SafeBlockInfoAsm(const void*, uint32_t*) {
    __asm {
        push ebp
        mov ebp, esp
        push ebx
        push esi
        push edi
        mov MK9R_gProbeEsp, esp
        mov MK9R_gProbeEbp, ebp
        mov MK9R_gProbeActive, 1
        push dword ptr [ebp+12]
        push dword ptr [ebp+8]
        mov eax, 0x00405A10
        call eax
        add esp, 8
        mov MK9R_gProbeActive, 0
        movzx eax, al
        pop edi
        pop esi
        pop ebx
        pop ebp
        ret
    }
}
__declspec(naked) void MK9R_ProbeRecover() {
    __asm {
        mov MK9R_gProbeActive, 0
        xor eax, eax
        pop edi
        pop esi
        pop ebx
        pop ebp
        ret
    }
}
#else
asm(R"(
    .text
    .globl _MK9R_SafeBlockInfoAsm
_MK9R_SafeBlockInfoAsm:
    pushl %ebp
    movl %esp, %ebp
    pushl %ebx
    pushl %esi
    pushl %edi
    movl %esp, _MK9R_gProbeEsp
    movl %ebp, _MK9R_gProbeEbp
    movl $1, _MK9R_gProbeActive
    pushl 12(%ebp)
    pushl 8(%ebp)
    movl $0x00405A10, %eax
    call *%eax
    addl $8, %esp
    movl $0, _MK9R_gProbeActive
    movzbl %al, %eax
    popl %edi
    popl %esi
    popl %ebx
    popl %ebp
    ret

    .globl _MK9R_ProbeRecover
_MK9R_ProbeRecover:
    movl $0, _MK9R_gProbeActive
    xorl %eax, %eax
    popl %edi
    popl %esi
    popl %ebx
    popl %ebp
    ret

    .globl _MK9R_SafeRead32
    .globl _MK9R_SafeReadInsn
    .globl _MK9R_SafeReadFail
_MK9R_SafeRead32:
    movl 4(%esp), %ecx
_MK9R_SafeReadInsn:
    movl (%ecx), %eax
    movl 8(%esp), %ecx
    movl %eax, (%ecx)
    movl $1, %eax
    ret
_MK9R_SafeReadFail:
    xorl %eax, %eax
    ret
)");
#endif
// int __cdecl MK9R_SafeRead32(const void* p, uint32_t* out): a fault at its one load resumes at the failure exit
// (any thread, no shared state). For reads that race other threads' decommits (pointer heuristics).
extern "C" {
int __cdecl MK9R_SafeRead32(const void* p, uint32_t* out);
void MK9R_SafeReadInsn();
void MK9R_SafeReadFail();
}

namespace {
PVOID gHandler = nullptr;
const char* volatile gCrumb = "idle";
volatile unsigned gCrumbValue = 0;
volatile LONG gReported = 0;
volatile LONG gHeartbeat = 0;  // game-thread world ticks (Heartbeat)
// The first fatal exception (CRASH? branch): when, and the heartbeat at that moment. The watchdog closes the game if its
// thread does not tick again: in 40 of 40 crash logs MK9's own exception path re-entered the heap the fault had left
// locked and spun forever (0x0042B240..0x0042B28B), the game frozen in front of the desktop.
volatile ULONGLONG gFatalAt = 0;
volatile LONG gFatalBeat = 0;
volatile DWORD gFatalTid = 0;  // a thread inside our own fatal-error handlers (purecall, out of memory)
volatile DWORD gWatchThread = 0;  // game thread id (Heartbeat)
DWORD gSelfLo = 0, gSelfHi = 0;  // MK9Rollback.dll image range (for the stack scan)
CrashLog::RegionNamer gNamer = nullptr;
CrashLog::PtrHistory gHistory = nullptr;
CrashLog::SavedDwords gSaved = nullptr;

bool Readable(uint32_t a, uint32_t n) {
    MEMORY_BASIC_INFORMATION mbi;
    return a >= 0x10000 && VirtualQuery(reinterpret_cast<void*>(a), &mbi, sizeof mbi) && mbi.State == MEM_COMMIT &&
           (mbi.Protect & (PAGE_READWRITE | PAGE_READONLY | PAGE_EXECUTE_READWRITE | PAGE_WRITECOPY)) &&
           !(mbi.Protect & PAGE_GUARD) && a + n <= reinterpret_cast<uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
}

// One pointer: UE3 owner, restored span, allocator history, GObjObjects identity.
void DescribePtr(const char* what, uint32_t p) {
    char d[160] = "?", h[160] = "", s[120] = "";
    Ue3Names::Describe(p, d, sizeof d);
    if (gHistory) gHistory(p, h, sizeof h);
    if (gNamer) {
        uint32_t base = 0;
        if (const char* n = gNamer(p, &base)) snprintf(s, sizeof s, " restored-span %s+0x%X", n, p - base);
    }
    const char* ident = "";
    if (Readable(p, 8)) {
        const uint32_t idx = *reinterpret_cast<const uint32_t*>(p + 4);
        const uint32_t objs = *reinterpret_cast<const uint32_t*>(0x00E7C0EC), cnt = *reinterpret_cast<const uint32_t*>(0x00E7C0F0);
        if (idx < cnt && Readable(objs + 4 * idx, 4))
            ident = *reinterpret_cast<const uint32_t*>(objs + 4 * idx) == p ? " gobj=self" : " gobj=OTHER";
        else
            ident = " gobj=badindex";
    }
    Logger::Info("CRASH?   %s %08X: %s%s%s %s", what, p, d, ident, s, h);
}

// 0x007794A0 (vt 0x00D5A330 slot 70) copies the 0x60-byte entries of the list at [this+0x48]+0x68
// whose +0x30 is 'this' into a local array and reads entry[0]->Outer->+0x1E4.
void DumpTouchList(const CONTEXT* c) {
    const uint32_t ebp = c->Ebp;
    if (!Readable(ebp - 0x2C, 0x10)) return;
    const uint32_t elem = *reinterpret_cast<const uint32_t*>(ebp - 0x20);
    if (!Readable(elem, 0x60)) return;
    const uint32_t* e = reinterpret_cast<const uint32_t*>(elem);
    char line[400];
    int n = 0;
    for (int i = 0; i < 24; ++i) n = AppendF(line, sizeof line, n, " %08X", e[i]);
    Logger::Info("CRASH? 0x7794A0 entry copy %08X:%s", elem, line);
    DescribePtr("entry[0] component", e[0]);
    const uint32_t self = e[12];
    DescribePtr("entry+0x30 this", self);
    if (!Readable(self + 0x48, 4)) return;
    const uint32_t x = *reinterpret_cast<const uint32_t*>(self + 0x48);
    DescribePtr("this+0x48 holder", x);
    if (!Readable(x + 0x68, 8)) return;
    const uint32_t data = *reinterpret_cast<const uint32_t*>(x + 0x68), cnt = *reinterpret_cast<const uint32_t*>(x + 0x6C);
    Logger::Info("CRASH?   holder list data=%08X count=%u", data, cnt);
    DescribePtr("holder list data", data);
    for (uint32_t i = 0; i < cnt && i < 24 && Readable(data + i * 0x60, 0x60); ++i) {
        const uint32_t* r = reinterpret_cast<const uint32_t*>(data + i * 0x60);
        char w[32];
        snprintf(w, sizeof w, "list[%u] +0 (+0x30=%08X)", i, r[12]);
        DescribePtr(w, r[0]);
    }
}

volatile LONG gWatchHits = 0;
}  // namespace
namespace CrashLog {
volatile bool gWatchDistinct = false;  // 'atomwatch': log each distinct writer instead of the first 6 hits
volatile bool gWatchGated = false;     // distinct mode: only hits while gWatchPhase != 0 count
volatile LONG gWatchPhase = 0;         // set by the tracer (e.g. 1 live / 2 replay inside a frame window)
}
namespace {
using CrashLog::gWatchDistinct;

struct TraceRec {
    uint32_t slot, eip, val, ret[3];
};
struct TraceFrameRec {
    uint32_t gf, n;
    TraceRec r[32];
};
TraceFrameRec gTrace[2][128];  // [live, resim][gf % 128]
TraceFrameRec* volatile gTraceCur = nullptr;
volatile LONG gTraceMode = 0;
uintptr_t gTraceAddr[4];
DWORD gTraceThread = 0;
bool gTraceAllThreads = false;

// Change watch (ChangeWatchArm)
volatile LONG gChangeMode = 0;
uintptr_t gChangeAddr[4];
const char* gChangeName[4];
volatile DWORD gChangeLast[4];
volatile LONG gChangeFlips[4], gChangeOther[4];  // logged zero <-> nonzero changes (10 kept), other changes (3 kept)

// "callers: <EBP chain> stack: <code-like values>" of the faulting thread, for watch hits
void DescribeWriter(const CONTEXT* c, char* chain, int chainCap, char* stack, int stackCap) {
    const DWORD* sp = reinterpret_cast<const DWORD*>(c->Esp);
    int n = 0, shown = 0;
    stack[0] = chain[0] = 0;
    for (int i = 0; i < 48 && shown < 8 && n < stackCap - 10; ++i) {
        const DWORD v = sp[i];
        if (v >= 0x00401000 && v < 0x00F9C000) {
            n = AppendF(stack, stackCap, n, " %08lX", v);
            ++shown;
        }
    }
    const NT_TIB* tib = reinterpret_cast<const NT_TIB*>(NtCurrentTeb());
    const uintptr_t lo = reinterpret_cast<uintptr_t>(tib->StackLimit), hi = reinterpret_cast<uintptr_t>(tib->StackBase);
    int cn = 0;
    uintptr_t fp = c->Ebp;
    for (int k = 0; k < 18 && cn < chainCap - 10 && fp >= lo && fp + 8 <= hi && !(fp & 3); ++k) {
        cn = AppendF(chain, chainCap, cn, " %08lX", reinterpret_cast<const DWORD*>(fp)[1]);
        const uintptr_t next = reinterpret_cast<const DWORD*>(fp)[0];
        if (next <= fp) break;
        fp = next;
    }
}

DWORD SetDebugRegisters(const uintptr_t addrs[4], DWORD skipThread, int* armed) {
    DWORD dr7 = 0;
    for (int s = 0; s < 4; ++s)  // Ls; write: RWs=01, LENs=11 (4 bytes)
        if (addrs[s]) dr7 |= (1u << (2 * s)) | (1u << (16 + 4 * s)) | (3u << (18 + 4 * s));
    *armed = 0;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snap == INVALID_HANDLE_VALUE) return dr7;
    THREADENTRY32 te{};
    te.dwSize = sizeof te;
    for (BOOL more = Thread32First(snap, &te); more; more = Thread32Next(snap, &te)) {
        if (te.th32OwnerProcessID != GetCurrentProcessId() || te.th32ThreadID == skipThread) continue;
        HANDLE t = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_SET_CONTEXT, FALSE, te.th32ThreadID);
        if (!t) continue;
        SuspendThread(t);
        CONTEXT c{};
        c.ContextFlags = CONTEXT_DEBUG_REGISTERS;
        if (GetThreadContext(t, &c)) {
            c.Dr0 = addrs[0] & ~3u;
            c.Dr1 = addrs[1] & ~3u;
            c.Dr2 = addrs[2] & ~3u;
            c.Dr3 = addrs[3] & ~3u;
            c.Dr6 = 0;
            c.Dr7 = dr7;
            if (SetThreadContext(t, &c)) ++*armed;
        }
        ResumeThread(t);
        CloseHandle(t);
    }
    CloseHandle(snap);
    return dr7;
}

DWORD WINAPI ChangeWatchProc(void*) {  // a helper thread, so the arming thread (the game thread) is armed too
    int armed = 0;
    const DWORD dr7 = SetDebugRegisters(gChangeAddr, GetCurrentThreadId(), &armed);
    if (!dr7) {
        InterlockedExchange(&gChangeMode, 0);
        Logger::Info("WATCH disarmed on %d threads", armed);
        return 0;
    }
    char what[200] = "";
    int n = 0;
    for (int s = 0; s < 4 && n < (int)sizeof what - 40; ++s)
        if (gChangeAddr[s]) n = AppendF(what, sizeof what, n, " %s %08X = %08lX;", gChangeName[s], (unsigned)gChangeAddr[s], gChangeLast[s]);
    Logger::Info("WATCH armed on %d threads:%s", armed, what);
    return 0;
}

LONG CALLBACK Handler(EXCEPTION_POINTERS* ep) {
    const DWORD code = ep->ExceptionRecord->ExceptionCode;
    // Change watch (ChangeWatchArm): log changes of the watched values, stay armed.
    if (code == EXCEPTION_SINGLE_STEP && gChangeMode && (ep->ContextRecord->Dr6 & 0xF)) {
        CONTEXT* c = ep->ContextRecord;
        for (int s = 0; s < 4; ++s) {
            if (!(c->Dr6 & (1u << s)) || !gChangeAddr[s]) continue;
            const DWORD now = *reinterpret_cast<const volatile DWORD*>(gChangeAddr[s]);  // just written: mapped
            const DWORD was = gChangeLast[s];
            if (now == was) continue;
            gChangeLast[s] = now;
            // MK9's closing check tests these for nonzero: a counter-like field must not use up the budget before its flip.
            if ((was == 0) != (now == 0) ? InterlockedIncrement(&gChangeFlips[s]) > 10 : InterlockedIncrement(&gChangeOther[s]) > 3) continue;
            char chain[200], stack[100];
            DescribeWriter(c, chain, sizeof chain, stack, sizeof stack);
            Logger::Info("WATCH %s: %08lX -> %08lX eip=%08lX thread=%lu eax=%08lX ecx=%08lX edx=%08lX esi=%08lX edi=%08lX callers:%s stack:%s",
                         gChangeName[s], was, now, c->Eip, GetCurrentThreadId(), c->Eax, c->Ecx, c->Edx, c->Esi, c->Edi, chain, stack);
        }
        c->Dr6 = 0;
        c->EFlags |= 0x10000;
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    // Write trace (TraceArm): record every hit under the current frame, stay armed.
    if (code == EXCEPTION_SINGLE_STEP && gTraceMode && (gTraceAllThreads || GetCurrentThreadId() == gTraceThread) && (ep->ContextRecord->Dr6 & 0xF)) {
        CONTEXT* c = ep->ContextRecord;
        TraceFrameRec* f = gTraceCur;
        for (uint32_t s = 0; f && s < 4; ++s) {
            if (!(c->Dr6 & (1u << s)) || f->n >= 32) continue;
            const LONG idx = InterlockedIncrement(reinterpret_cast<volatile LONG*>(&f->n)) - 1;  // any thread may record
            if (idx >= 32) continue;
            TraceRec& r = f->r[idx];
            r.slot = s | (GetCurrentThreadId() == gTraceThread ? 0u : 0x10u);  // 0x10: written by another thread
            r.eip = c->Eip;
            r.val = *reinterpret_cast<const uint32_t*>(gTraceAddr[s]);
            // Callers from the EBP chain (MK's aligned-stack prologues also build {saved ebp, return} frames).
            const NT_TIB* tib = reinterpret_cast<const NT_TIB*>(NtCurrentTeb());
            const uintptr_t lo = reinterpret_cast<uintptr_t>(tib->StackLimit), hi = reinterpret_cast<uintptr_t>(tib->StackBase);
            uintptr_t fp = c->Ebp;
            int k = 0;
            for (; k < 3 && fp >= lo && fp + 8 <= hi && !(fp & 3); ++k) {
                r.ret[k] = reinterpret_cast<const DWORD*>(fp)[1];
                const uintptr_t next = reinterpret_cast<const DWORD*>(fp)[0];
                if (next <= fp) {
                    ++k;
                    break;
                }
                fp = next;
            }
            for (; k < 3; ++k) r.ret[k] = 0;
        }
        c->Dr6 = 0;
        c->EFlags |= 0x10000;
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    // Hardware data breakpoint (CrashLog::WatchWrite): log the writer, then disarm.
    if (code == EXCEPTION_SINGLE_STEP && (ep->ContextRecord->Dr6 & 0xF)) {
        CONTEXT* c = ep->ContextRecord;
        // Distinct-writer mode (gWatchDistinct, 'atomwatch'): each writer EIP is logged once, up to 12, over 5000 hits.
        const LONG phase = CrashLog::gWatchPhase;
        if (gWatchDistinct && CrashLog::gWatchGated && !phase) {  // outside the traced window: ignore
            c->Dr6 = 0;
            c->EFlags |= 0x10000;
            return EXCEPTION_CONTINUE_EXECUTION;
        }
        const LONG hit = InterlockedIncrement(&gWatchHits);
        bool show = hit <= 6;
        if (gWatchDistinct) {
            show = false;
            static volatile LONG sSeenN = 0;
            static DWORD sSeen[12];
            bool known = false;
            // Keyed by writer and phase, so the same writer is shown once live and once in a replay.
            const DWORD key = c->Eip ^ ((DWORD)phase << 28);
            for (LONG i = 0; i < sSeenN && i < 12; ++i) known |= sSeen[i] == key;
            if (!known && sSeenN < 12) {
                sSeen[InterlockedIncrement(&sSeenN) - 1] = key;
                show = true;
            }
            if (hit % 1000 == 0) Logger::Info("WATCH %ld hits, %ld distinct writers", hit, sSeenN);
        }
        if (show) {
            const DWORD* sp = reinterpret_cast<const DWORD*>(c->Esp);
            char line[200];
            int n = 0, shown = 0;
            for (int i = 0; i < 48 && shown < 8; ++i) {
                const DWORD v = sp[i];
                if (v >= 0x00401000 && v < 0x00F9C000) {
                    n = AppendF(line, sizeof line, n, " %08lX", v);
                    ++shown;
                }
            }
            line[n] = 0;
            // Precise callers from the EBP chain (the stack scan above also shows stale values).
            const NT_TIB* tib = reinterpret_cast<const NT_TIB*>(NtCurrentTeb());
            const uintptr_t lo = reinterpret_cast<uintptr_t>(tib->StackLimit), hi = reinterpret_cast<uintptr_t>(tib->StackBase);
            char chain[200];
            int cn = 0;
            uintptr_t fp = c->Ebp;
            for (int k = 0; k < 18 && fp >= lo && fp + 8 <= hi && !(fp & 3); ++k) {
                cn = AppendF(chain, sizeof chain, cn, " %08lX", reinterpret_cast<const DWORD*>(fp)[1]);
                const uintptr_t next = reinterpret_cast<const DWORD*>(fp)[0];
                if (next <= fp) break;
                fp = next;
            }
            chain[cn] = 0;
            Logger::Info("WATCH hit: phase=%ld value=%08lX eip=%08lX [esp]=%08lX thread=%lu eax=%08lX ebx=%08lX ecx=%08lX edx=%08lX esi=%08lX edi=%08lX callers:%s stack:%s",
                         phase, *reinterpret_cast<const DWORD*>(c->Dr0), c->Eip, *reinterpret_cast<const DWORD*>(c->Esp), GetCurrentThreadId(), c->Eax,
                         c->Ebx, c->Ecx, c->Edx, c->Esi, c->Edi, chain, line);
        } else if (!gWatchDistinct || hit > 5000) {
            c->Dr7 = 0;  // enough samples
        }
        c->Dr6 = 0;
        c->EFlags |= 0x10000;  // RF: an execute breakpoint must not re-fire on the same instruction
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    // Guarded 4-byte read (CrashLog::SafeRead32). A guard page fails the read too (the save's ProbeRead, 2026-11-04): left
    // unhandled, STATUS_GUARD_PAGE_VIOLATION on another thread's stack guard would end the process.
    if ((code == EXCEPTION_ACCESS_VIOLATION || code == STATUS_GUARD_PAGE_VIOLATION) &&
        ep->ContextRecord->Eip == reinterpret_cast<DWORD>(&MK9R_SafeReadInsn)) {
        // The read took the page's guard protection away with it (GuardPage.h): put it back, or the thread whose stack
        // guard this is cannot grow its stack any more. The address is the exception's (a read across a page boundary
        // faults on its second page), else the one the read used.
        if (code == STATUS_GUARD_PAGE_VIOLATION)
            GuardPage::Rearm(ep->ExceptionRecord->NumberParameters >= 2 ? ep->ExceptionRecord->ExceptionInformation[1]
                                                                        : ep->ContextRecord->Ecx);
        ep->ContextRecord->Eip = reinterpret_cast<DWORD>(&MK9R_SafeReadFail);
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    // Guarded probe (CrashLog::SafeBlockInfo): resume at its recovery label.
    if (code == EXCEPTION_ACCESS_VIOLATION && MK9R_gProbeActive && GetCurrentThreadId() == MK9R_gProbeThread) {
        CONTEXT* c = ep->ContextRecord;
        c->Esp = MK9R_gProbeEsp;
        c->Ebp = MK9R_gProbeEbp;
        c->Eip = reinterpret_cast<DWORD>(&MK9R_ProbeRecover);
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    if (code != EXCEPTION_ACCESS_VIOLATION && code != EXCEPTION_ILLEGAL_INSTRUCTION &&
        code != EXCEPTION_PRIV_INSTRUCTION && code != EXCEPTION_STACK_OVERFLOW) {
        // Other first-chance exceptions, logged (not handled): rollback-mode games exited at a level change without a
        // CRASH line (2026-10-31 11:03/12:19), e.g. a C++ throw (E06D7363), an integer divide or an FP fault ending in
        // MK9's fatal-error exit. Skips thread naming, debug output, RPC status codes and our own breakpoints.
        static volatile LONG others = 0;
        if (code != 0x406D1388 && code != 0x40010006 && code != 0x4001000A && code != EXCEPTION_SINGLE_STEP && code >= 0x10000 &&
            InterlockedIncrement(&others) <= 8) {
            const CONTEXT* c = ep->ContextRecord;
            char line[200];
            int n = 0;
            const DWORD* sp = reinterpret_cast<const DWORD*>(c->Esp);
            for (int i = 0, shown = 0; i < 96 && shown < 10; ++i) {
                MEMORY_BASIC_INFORMATION mbi;
                if (!VirtualQuery(sp + i, &mbi, sizeof mbi) || mbi.State != MEM_COMMIT) break;
                const DWORD v = sp[i];
                if (v >= 0x00401000 && v < 0x00F9C000) {
                    n = AppendF(line, sizeof line, n, " %08lX", v);
                    ++shown;
                }
            }
            line[n] = 0;
            Logger::Info("EXCEPTION (first chance, passed on) code=%08lX at %08lX thread=%lu crumb=%s(%u) params %lu: %08lX %08lX; "
                         "stack code values:%s",
                         code, (unsigned long)(uintptr_t)ep->ExceptionRecord->ExceptionAddress, GetCurrentThreadId(), gCrumb,
                         gCrumbValue, ep->ExceptionRecord->NumberParameters,
                         (unsigned long)(ep->ExceptionRecord->NumberParameters > 0 ? ep->ExceptionRecord->ExceptionInformation[0] : 0),
                         (unsigned long)(ep->ExceptionRecord->NumberParameters > 1 ? ep->ExceptionRecord->ExceptionInformation[1] : 0), line);
            Logger::Flush();
        }
        return EXCEPTION_CONTINUE_SEARCH;
    }
    // A fault inside our purecall / out-of-memory handlers would go on into MK9's handlers and hang: close now.
    if (gFatalTid && gFatalTid == GetCurrentThreadId()) {
        // Never without a line: the game closed here with nothing in the log. Once only (a fault inside the line's own
        // logging comes back here).
        static volatile LONG said = 0;
        if (!InterlockedExchange(&said, 1))
            Logger::TryInfo("CRASH: exception %08lX at %08lX inside the mod's pure-call / out-of-memory handler (thread %lu); "
                            "closing the game", code, ep->ContextRecord->Eip, GetCurrentThreadId());
        TerminateProcess(GetCurrentProcess(), 0xC0000409);
    }
    // Only faults that can stop the game: on its thread, or in MKKE's own code (a driver or overlay DLL may fault and
    // handle it on its own threads). The watchdog clears the mark if the game thread keeps ticking.
    if (!gFatalAt && (GetCurrentThreadId() == gWatchThread || (ep->ContextRecord->Eip >= 0x00401000 && ep->ContextRecord->Eip < 0x00F9C000))) {
        gFatalBeat = gHeartbeat;
        gFatalAt = GetTickCount64();
    }
    if (InterlockedIncrement(&gReported) > 3) return EXCEPTION_CONTINUE_SEARCH;  // first few only
    const CONTEXT* c = ep->ContextRecord;
    const ULONG_PTR rw = ep->ExceptionRecord->NumberParameters >= 2 ? ep->ExceptionRecord->ExceptionInformation[0] : 0;
    const ULONG_PTR addr = ep->ExceptionRecord->NumberParameters >= 2 ? ep->ExceptionRecord->ExceptionInformation[1] : 0;
    Logger::Info("CRASH? code=%08lX eip=%08lX %s addr=%08lX thread=%lu crumb=%s(%u) eax=%08lX ebx=%08lX ecx=%08lX "
                 "edx=%08lX esi=%08lX edi=%08lX ebp=%08lX esp=%08lX",
                 code, c->Eip, rw == 1 ? "write" : rw == 8 ? "exec" : "read", (unsigned long)addr, GetCurrentThreadId(),
                 gCrumb, gCrumbValue, c->Eax, c->Ebx, c->Ecx, c->Edx, c->Esi, c->Edi, c->Ebp, c->Esp);
    // Return-address scan of the top of the stack (best effort, no unwinding).
    const DWORD* sp = reinterpret_cast<const DWORD*>(c->Esp);
    char line[256];
    int n = 0, shown = 0;
    for (int i = 0; i < 64 && shown < 10; ++i) {
        MEMORY_BASIC_INFORMATION mbi;
        if (!VirtualQuery(sp + i, &mbi, sizeof mbi) || mbi.State != MEM_COMMIT) break;
        const DWORD v = sp[i];
        if ((v >= 0x00401000 && v < 0x00F9C000) || (v >= gSelfLo && v < gSelfHi)) {
            n = AppendF(line, sizeof line, n, " %08lX", v);
            ++shown;
        }
    }
    line[n] = 0;
    Logger::Info("CRASH? stack code-like values:%s", line);
    {  // which module holds eip (other DLLs load at varying bases), plus the module return addresses on the stack
        auto modOf = [](uint32_t a, char* out, size_t sz) -> bool {
            HMODULE m = nullptr;
            char path[MAX_PATH];
            if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                    reinterpret_cast<LPCSTR>((uintptr_t)a), &m) || !m || !GetModuleFileNameA(m, path, sizeof path))
                return false;
            const char* b = strrchr(path, '\\');
            snprintf(out, sz, "%s+%lX", b ? b + 1 : path, (unsigned long)(a - (uint32_t)(uintptr_t)m));
            return true;
        };
        char em[160] = "?";
        modOf(c->Eip, em, sizeof em);
        char ml[700];
        int k = 0, shownm = 0;
        for (int i = 0; i < 512 && shownm < 16; ++i) {
            uint32_t v = 0;
            if (!CrashLog::SafeRead32((uintptr_t)(sp + i), &v)) break;
            char d[160];
            if (v >= 0x10000 && modOf(v, d, sizeof d)) {
                k = AppendF(ml, sizeof ml, k, " %s", d);
                ++shownm;
            }
        }
        ml[k] = 0;
        Logger::Info("CRASH? eip in %s; stack module values:%s", em, ml);
    }
    if (gNamer) {
        const DWORD regs[6] = {c->Eax, c->Ebx, c->Ecx, c->Edx, c->Esi, c->Edi};
        const char* rn[6] = {"eax", "ebx", "ecx", "edx", "esi", "edi"};
        for (int i = 0; i < 6; ++i) {
            uint32_t base = 0;
            const char* n = gNamer(regs[i], &base);
            if (!n) continue;
            Logger::Info("CRASH?   %s=%08lX is inside restored span %s %08X (+0x%lX)", rn[i], regs[i], n, base, regs[i] - base);
            // Restored at the load, or written by the replay since? The registry-walk crash 0x0045B168 (baseline_0917_3,
            // soak 11203) read a garbage name pointer at +0x14 of an object in the restored MK heap.
            constexpr unsigned kN = 12;
            const uint32_t at = regs[i] & ~3u;
            uint32_t saved[kN];
            if (!gSaved || !gSaved(at, saved, kN)) continue;
            char line[400];
            int k = 0;
            for (unsigned d = 0; d < kN; ++d) {
                uint32_t live = 0;
                const bool ok = CrashLog::SafeRead32(at + d * 4, &live);
                k = AppendF(line, sizeof line, k, ok && live == saved[d] ? " %08X" : " %08X>%s", saved[d],
                              ok ? "" : "?");
                if (ok && live != saved[d]) k = AppendF(line, sizeof line, k, "%08X", live);
            }
            Logger::Info("CRASH?     %s +0..+0x2C saved[>live]:%s", rn[i], line);
        }
    }
    {
        // Every heap-like register with its allocator history: the eip=0 crash from 0x008A753D (soak 9509, 2026-10-19)
        // called through ecx = [MK object+0x1D8], a 0x4C-byte GMalloc helper whose vtable slot held heap data, and
        // ecx was the one register left undescribed.
        const DWORD regs[6] = {c->Eax, c->Ebx, c->Ecx, c->Edx, c->Esi, c->Edi};
        const char* rn[6] = {"eax", "ebx", "ecx", "edx", "esi", "edi"};
        for (int i = 0; i < 6; ++i)
            if (regs[i] >= 0x01000000 && !(regs[i] >= 0x00400000 && regs[i] < 0x010AE000)) DescribePtr(rn[i], regs[i]);
    }
    if (c->Eip >= 0x007794A0 && c->Eip < 0x00779800) DumpTouchList(c);
    // UE3 garbage collector, reference token stream (0x005F95C0..0x005F9810): token 3 is a TArray<UObject*> at
    // [ebp+0x10] (the object) + offset, its header at [ebp-0x20], the element index at [ebp-0x18]. Combo soak 18
    // (2026-11-07): 4 of 4 sessions read an element from an array block the quarantine had released (eip 0x005F975D).
    if (c->Eip >= 0x005F95C0 && c->Eip < 0x005F9810 && Readable(c->Ebp - 0x20, 0x34)) {
        const uint32_t obj = *reinterpret_cast<const uint32_t*>(c->Ebp + 0x10);
        const uint32_t hdr = *reinterpret_cast<const uint32_t*>(c->Ebp - 0x20);
        const uint32_t idx = *reinterpret_cast<const uint32_t*>(c->Ebp - 0x18);
        uint32_t data = 0, num = 0, max = 0, cls = 0;
        CrashLog::SafeRead32(hdr, &data);
        CrashLog::SafeRead32(hdr + 4, &num);
        CrashLog::SafeRead32(hdr + 8, &max);
        CrashLog::SafeRead32(obj + 0x30, &cls);
        Logger::Info("CRASH?   GC token: object %08X, array header %08X (+0x%X): data %08X num %u max %u, element %u", obj, hdr,
                     hdr - obj, data, num, max, idx);
        DescribePtr("gc object", obj);
        DescribePtr("gc object class", cls);
        DescribePtr("gc array data", data);
    }
    // 0x00789270(entry): reads the attached actor entry[0]'s Components TArray (+0x3C); caller
    // 0x00764240 walks base+0x68. esi = the attached actor, the base is the caller's esi at [ebp-8].
    if (c->Eip >= 0x00789270 && c->Eip < 0x007892E0) {
        DescribePtr("attached actor (esi)", c->Esi);
        if (Readable(c->Ebp - 8, 4)) {
            const uint32_t base = *reinterpret_cast<const uint32_t*>(c->Ebp - 8);
            DescribePtr("base actor", base);
            if (Readable(base + 0x68, 8)) {
                const uint32_t data = *reinterpret_cast<const uint32_t*>(base + 0x68), cnt = *reinterpret_cast<const uint32_t*>(base + 0x6C);
                Logger::Info("CRASH?   base attached list data=%08X count=%u", data, cnt);
                DescribePtr("base list data", data);
                for (uint32_t i = 0; i < cnt && i < 24 && Readable(data + i * 0x60, 0x60); ++i) {
                    char w[40];
                    snprintf(w, sizeof w, "attached[%u]", i);
                    DescribePtr(w, *reinterpret_cast<const uint32_t*>(data + i * 0x60));
                }
            }
        }
    }
    Logger::Flush();
    return EXCEPTION_CONTINUE_SEARCH;
}
// Pure virtual call (MSVCR90 _purecall -> R6025): log who called and with what before the CRT aborts. The game's
// vtables point at MSVCR90's _purecall, which calls the handler set through _set_purecall_handler.
using PurecallHandlerFn = void(__cdecl*)();
PurecallHandlerFn gPrevPurecall = nullptr;
bool gPurecallSet = false;
void __cdecl PurecallHandler() {
    gFatalTid = GetCurrentThreadId();  // a fault in the descriptions below closes the game too (Handler)
    uint32_t rbx, rsi, rdi, rbp;
    asm volatile("movl %%ebx, %0\n\tmovl %%esi, %1\n\tmovl %%edi, %2\n\tmovl %%ebp, %3" : "=m"(rbx), "=m"(rsi), "=m"(rdi), "=m"(rbp));
    Logger::Info("PURECALL (R6025 follows): thread %lu crumb=%s(%u) ebx=%08X esi=%08X edi=%08X ebp=%08X", GetCurrentThreadId(), gCrumb,
                 gCrumbValue, rbx, rsi, rdi, rbp);
    const DWORD* sp = reinterpret_cast<const DWORD*>(&rbx);
    // Not past the top of this thread's stack (a pure call near a pool thread's stack base read beyond it and faulted).
    const DWORD* top = reinterpret_cast<const DWORD*>(reinterpret_cast<const NT_TIB*>(NtCurrentTeb())->StackBase);
    char line[300];
    int n = 0, shown = 0;
    for (int i = 0; i < 256 && shown < 16 && sp + i < top; ++i) {
        const DWORD v = sp[i];
        if (v >= 0x00401000 && v < 0x00F9C000) {
            n = AppendF(line, sizeof line, n, " %08lX", v);
            ++shown;
        }
    }
    line[n] = 0;
    Logger::Info("PURECALL stack code-like values:%s", line);
    DescribePtr("ebx", rbx);
    DescribePtr("esi", rsi);
    DescribePtr("edi", rdi);
    // The CRT's R6025 box then aborts behind the fullscreen window: 2026-10-31 11:40 the PC had to be powered off.
    // The game is lost either way, so it is closed here.
    Logger::Info("PURECALL: closing the game instead of MK9's R6025 dialog");
    Logger::Flush();
    TerminateProcess(GetCurrentProcess(), 0xC0000409);
    if (gPrevPurecall) gPrevPurecall();
}

// MK9's out-of-memory handler 0x00406160(a1, size, heap, kind, spin), called by its allocators when they cannot get
// memory (MK heap 0x0042B4F5: a1, the grow size, the heap, 3, 1; GMalloc 0x00405415 / 0x00428xxx; the small-block
// allocator 0x0042Cxxx; 0x00B959D7). With spin != 0 and [0x00DE110C] set it shows "out of memory" and then loops
// forever behind the fullscreen window (2026-10-31 20:34, opening a private room: MK heap #5 [0x00ED5400] could not
// grow; the PC locked up and needed a logout). Logged here with the heap's state, the allocation that failed and the
// stack; a call that would hang closes the game instead, the others go on to MK9's handler.
constexpr uintptr_t kMemFatal = 0x00406160;
const uint8_t kMemFatalOrig[5] = {0x55, 0x8B, 0xEC, 0x51, 0x56};  // push ebp; mov ebp, esp; push ecx; push esi
using MemFatalFn = void(__cdecl*)(uint32_t, uint32_t, uint32_t, uint32_t, uint32_t);
uint8_t* gMemFatalTramp = nullptr;
volatile LONG gMemFatalHits = 0;
volatile DWORD gAllocThread = 0;  // the last MK heap allocation (CrashLog::NoteAlloc)
volatile uint32_t gAllocHeap = 0, gAllocSize = 0, gAllocCaller = 0;

void __cdecl MemFatal(uint32_t a1, uint32_t size, uint32_t heap, uint32_t kind, uint32_t spin) {
    gFatalTid = GetCurrentThreadId();  // a fault from here on (our logging, or MK9's NULL allocation) closes the game
    const uint32_t from = (uint32_t)(uintptr_t)__builtin_return_address(0);
    uint32_t forever = 0;
    CrashLog::SafeRead32(0x00DE110C, &forever);
    const bool hangs = spin && forever;
    if (InterlockedIncrement(&gMemFatalHits) <= 4) {
        int idx = 0;
        for (int k = 0; k < 5 && !idx; ++k) {
            uint32_t h = 0;
            if (CrashLog::SafeRead32(0x00ED53F0 + 4 * k, &h) && h && h == heap) idx = k + 1;
        }
        char state[200] = "not an MK heap";
        if (idx) {
            uint32_t res = 0, used = 0, chunk = 0, root = 0, largest = 0;
            CrashLog::SafeRead32(heap + 0x24, &res);
            CrashLog::SafeRead32(heap + 0x28, &used);
            CrashLog::SafeRead32(heap + 0x38, &chunk);
            CrashLog::SafeRead32(heap + 0x34, &root);
            // The free tree's +0x10 side holds the larger blocks (best-fit search in 0x0042B390).
            for (uint32_t n = root, steps = 0; n >= 0x10000 && steps < 4096; ++steps) {
                uint32_t s = 0, next = 0;
                if (!CrashLog::SafeRead32(n + 4, &s) || !CrashLog::SafeRead32(n + 0x10, &next)) break;
                largest = s & ~7u;
                n = next;
            }
            snprintf(state, sizeof state, "MK heap #%d used/reserved %u/%u KB, grow chunk %u KB, largest free block %u KB", idx,
                     used >> 10, res >> 10, chunk >> 10, largest >> 10);
        }
        Logger::Info("OUT OF MEMORY (MK9's handler 0x00406160 called from %08X, thread %lu, crumb %s(%u)): args %08X %08X %08X %u %u; %s",
                     from, GetCurrentThreadId(), gCrumb, gCrumbValue, a1, size, heap, kind, spin, state);
        if (gAllocThread == GetCurrentThreadId())
            Logger::Info("OUT OF MEMORY: this thread's last MK heap request: %u bytes (%u KB) from heap %08X, caller %08X", gAllocSize,
                         gAllocSize >> 10, gAllocHeap, gAllocCaller);
        CrashLog::LogStack("OUT OF MEMORY");
        Logger::Flush();
    }
    if (hangs) {
        Logger::Info("OUT OF MEMORY: MK9 would show its message and freeze the screen here; closing the game instead");
        Logger::Flush();
        TerminateProcess(GetCurrentProcess(), 0xC0000017);  // STATUS_NO_MEMORY
    }
    reinterpret_cast<MemFatalFn>(gMemFatalTramp)(a1, size, heap, kind, spin);
    // MK9's handler came back (a request its caller can do without): this thread has left the fatal path. The mark
    // used to stay for the rest of the run, and the thread's next exception of any kind, one the game handles itself
    // included, closed the game.
    if (gFatalTid == GetCurrentThreadId()) gFatalTid = 0;
}

void InstallMemFatal() {
    if (gMemFatalTramp) return;
    uint8_t* tramp = static_cast<uint8_t*>(CodePatch::AllocExec(16));
    if (!tramp) return;
    memcpy(tramp, kMemFatalOrig, 5);  // relocation-free
    tramp[5] = 0xE9;
    const int32_t back = (int32_t)((kMemFatal + 5) - (reinterpret_cast<uintptr_t>(tramp) + 10));
    memcpy(tramp + 6, &back, 4);
    FlushInstructionCache(GetCurrentProcess(), tramp, 16);
    gMemFatalTramp = tramp;
    uint8_t jmp[5] = {0xE9, 0, 0, 0, 0};
    const int32_t rel = (int32_t)(reinterpret_cast<uintptr_t>(&MemFatal) - (kMemFatal + 5));
    memcpy(jmp + 1, &rel, 4);
    if (!CodePatch::Write(kMemFatal, kMemFatalOrig, jmp, 5, "out-of-memory handler 0x00406160 (log, close instead of freezing)"))
        gMemFatalTramp = nullptr;
}
}  // namespace

namespace CrashLog {
void Install() {
    if (!gPurecallSet) {
        using SetFn = PurecallHandlerFn(__cdecl*)(PurecallHandlerFn);
        if (HMODULE crt = GetModuleHandleA("msvcr90.dll"))
            if (auto set = reinterpret_cast<SetFn>(GetProcAddress(crt, "_set_purecall_handler"))) {
                gPrevPurecall = set(&PurecallHandler);
                gPurecallSet = true;
            }
    }
    MEMORY_BASIC_INFORMATION mbi;
    if (VirtualQuery(reinterpret_cast<void*>(&Handler), &mbi, sizeof mbi)) {
        gSelfLo = reinterpret_cast<DWORD>(mbi.AllocationBase);
        const IMAGE_NT_HEADERS32* nt = reinterpret_cast<const IMAGE_NT_HEADERS32*>(
            gSelfLo + reinterpret_cast<const IMAGE_DOS_HEADER*>(gSelfLo)->e_lfanew);
        gSelfHi = gSelfLo + nt->OptionalHeader.SizeOfImage;
    }
    if (!gHandler) gHandler = AddVectoredExceptionHandler(1, Handler);
    InstallMemFatal();
}
void LogStack(const char* tag) {
    DWORD here = 0;
    const DWORD* sp = &here;
    const DWORD* top = static_cast<const DWORD*>(reinterpret_cast<NT_TIB*>(NtCurrentTeb())->StackBase);
    char line[400];
    int n = 0;
    for (int shown = 0; sp + 1 <= top && shown < 32; ++sp) {
        const DWORD v = *sp;
        if ((v >= 0x00401000 && v < 0x00F9C000) || (v >= gSelfLo && v < gSelfHi)) {
            n = AppendF(line, sizeof line, n, " %08lX", v);
            ++shown;
        }
    }
    line[n] = 0;
    Logger::Info("%s stack code values:%s", tag, n ? line : " none");
}
void NoteAlloc(const void* heap, uint32_t size, const void* caller) {
    gAllocHeap = (uint32_t)(uintptr_t)heap;
    gAllocSize = size;
    gAllocCaller = (uint32_t)(uintptr_t)caller;
    gAllocThread = GetCurrentThreadId();
}
void Uninstall() {
    if (gHandler) RemoveVectoredExceptionHandler(gHandler);
    gHandler = nullptr;
    if (gPurecallSet) {
        using SetFn = PurecallHandlerFn(__cdecl*)(PurecallHandlerFn);
        if (HMODULE crt = GetModuleHandleA("msvcr90.dll"))
            if (auto set = reinterpret_cast<SetFn>(GetProcAddress(crt, "_set_purecall_handler"))) set(gPrevPurecall);
        gPurecallSet = false;
    }
}
// ---- hang watchdog ----
namespace {
bool gCrtDialog = false;  // a C runtime error box is open (LogProcessWindow, watchdog thread)
HANDLE gWatchdog = nullptr;
volatile LONG gWatchStop = 0;

// GetWindowText on a window of a hung thread sends WM_GETTEXT and blocks the watchdog with it
// (soak29: the thread dump after the window list never ran). Bounded send instead.
void WindowText(HWND h, char* out, int n) {
    DWORD_PTR r = 0;
    out[0] = 0;
    if (!SendMessageTimeoutA(h, WM_GETTEXT, (WPARAM)n, (LPARAM)out, SMTO_ABORTIFHUNG | SMTO_BLOCK, 200, &r)) out[0] = 0;
    out[n - 1] = 0;
}
BOOL CALLBACK LogChildWindow(HWND ch, LPARAM) {
    char ccls[64] = {}, text[512] = {};
    GetClassNameA(ch, ccls, sizeof ccls);
    WindowText(ch, text, sizeof text);
    if (text[0]) Logger::Info("HANG?     child '%s': %s", ccls, text);
    return TRUE;
}
BOOL CALLBACK LogProcessWindow(HWND h, LPARAM) {
    DWORD pid = 0;
    GetWindowThreadProcessId(h, &pid);
    if (pid != GetCurrentProcessId() || !IsWindowVisible(h)) return TRUE;
    // A return address directly follows a call: E8 rel32, FF /2 (register, [reg], [reg+d8], [reg+d32], [abs32], SIB forms).
    auto LooksLikeReturn = [](uint32_t r) {
        uint32_t w0 = 0, w1 = 0;
        if (r < 0x10000 || !SafeRead32(r - 8, &w0) || !SafeRead32(r - 4, &w1)) return false;
        uint8_t b[8];
        memcpy(b, &w0, 4);
        memcpy(b + 4, &w1, 4);
        auto ff2 = [&](int at) { return b[at] == 0xFF && ((b[at + 1] >> 3) & 7) == 2; };
        return b[3] == 0xE8 ||                                            // call rel32
               (b[6] == 0xFF && (b[7] & 0xF8) == 0xD0) ||                 // call reg
               (ff2(6) && (b[7] >> 6) == 0 && (b[7] & 7) != 4 && (b[7] & 7) != 5) ||  // call [reg]
               (ff2(5) && (b[6] >> 6) == 1 && (b[6] & 7) != 4) ||         // call [reg+d8]
               (ff2(4) && (b[5] >> 6) == 1 && (b[5] & 7) == 4) ||         // call [sib+d8]
               (ff2(2) && (((b[3] >> 6) == 2 && (b[3] & 7) != 4) || b[3] == 0x15)) ||  // call [reg+d32] / [abs32]
               (ff2(1) && (b[2] >> 6) == 2 && (b[2] & 7) == 4);           // call [sib+d32]
    };
    char cls[64] = {}, title[256] = {};
    GetClassNameA(h, cls, sizeof cls);
    WindowText(h, title, sizeof title);
    const DWORD owner = GetWindowThreadProcessId(h, nullptr);
    Logger::Info("HANG?   window %p class '%s' title '%s' (thread %lu)", (void*)h, cls, title, owner);
    // A C runtime error box (R6025 from PhysX's own statically linked CRT, which our purecall handler never sees; abort,
    // R6016...) ends the game when dismissed, and it opens behind the fullscreen window: the game is closed instead.
    if (!strcmp(cls, "#32770") && strstr(title, "Visual C++ Runtime")) gCrtDialog = true;
    // An error dialog (R6025 from a DLL's own CRT, e.g. PhysXCore): the owner's stack as module+offset for every
    // value inside any loaded module (the regular dump only shows MKKE and MK9Rollback addresses).
    if (!strcmp(cls, "#32770") && owner && owner != GetCurrentThreadId()) {
        HANDLE t = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT, FALSE, owner);
        if (t) {
            // Copied while suspended, analysed after the resume: the module lookups below take the loader lock, which the
            // suspended owner may hold (the watchdog would then wait forever and never resume it).
            static DWORD stack[0x2000];
            uint32_t count = 0;
            CONTEXT c{};
            c.ContextFlags = CONTEXT_CONTROL;
            SuspendThread(t);
            const bool got = GetThreadContext(t, &c) != 0;
            if (got) {
                MEMORY_BASIC_INFORMATION smbi;
                const DWORD* sp = reinterpret_cast<const DWORD*>(c.Esp);
                // Not a guard or no-access page (a stack pointer just moved down can sit in one): see FramePace's Capture.
                if (VirtualQuery(sp, &smbi, sizeof smbi) && smbi.State == MEM_COMMIT && !(smbi.Protect & (PAGE_GUARD | PAGE_NOACCESS))) {
                    const DWORD* end = reinterpret_cast<const DWORD*>(reinterpret_cast<uintptr_t>(smbi.BaseAddress) + smbi.RegionSize);
                    count = end > sp ? (uint32_t)(end - sp) : 0;
                    if (count > 0x2000) count = 0x2000;
                    memcpy(stack, sp, count * 4);
                }
            }
            ResumeThread(t);
            CloseHandle(t);
            if (got && count) {
                char line[900];
                int n = 0, shown = 0;
                for (uint32_t i = 0; i < count && shown < 40; ++i) {
                    HMODULE m = nullptr;
                    if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                            reinterpret_cast<LPCSTR>(static_cast<uintptr_t>(stack[i])), &m) || !m)
                        continue;
                    if (!LooksLikeReturn(stack[i])) continue;  // stale data and constants that point into code
                    char path[MAX_PATH] = {};
                    GetModuleFileNameA(m, path, sizeof path);
                    const char* base = strrchr(path, '\\');
                    base = base ? base + 1 : path;
                    if (!_stricmp(base, "USER32.dll") || !_stricmp(base, "ntdll.dll") || !_stricmp(base, "win32u.dll") ||
                        !_stricmp(base, "KERNELBASE.dll") || !_stricmp(base, "KERNEL32.DLL") || !_stricmp(base, "apphelp.dll") ||
                        !_stricmp(base, "winmmbase.dll") || !_stricmp(base, "GDI32.dll") || !_stricmp(base, "gdi32full.dll"))
                        continue;  // window-system frames of the message box
                    n = AppendF(line, sizeof line, n, " %s+%lX", base,
                                  (unsigned long)(stack[i] - reinterpret_cast<uintptr_t>(m)));
                    ++shown;
                }
                line[n] = 0;
                Logger::Info("HANG?     dialog owner thread %lu eip=%08lX stack modules:%s", owner, c.Eip, line);
            }
        }
    }
    EnumChildWindows(h, LogChildWindow, 0);
    return TRUE;
}

DWORD WINAPI Watchdog(LPVOID) {
    LONG last = gHeartbeat;
    DWORD stale = 0;
    bool reported = false;
    while (!InterlockedCompareExchange(&gWatchStop, 0, 0)) {
        Sleep(500);
        const LONG now = gHeartbeat;
        // The faulting code handled its exception (first-chance) and the game went on: not a crash after all.
        if (gFatalAt && now - gFatalBeat >= 2) gFatalAt = 0;
        // Deadman: a fatal exception was recorded (CRASH? above) and the game thread has not ticked since (one tick of
        // slack for a crash on another thread). MK9 does not come back from there; it spins in its heap lock forever.
        if (gFatalAt && now - gFatalBeat <= 1 && GetTickCount64() - gFatalAt >= 6000) {
            Logger::TryInfo("CRASH: the game thread has not run for %llu s since the crash above; closing the game instead of "
                            "leaving it frozen",
                            (unsigned long long)((GetTickCount64() - gFatalAt) / 1000));
            TerminateProcess(GetCurrentProcess(), 0xC0000005);
        }
        if (now != last) {
            last = now;
            stale = 0;
            reported = false;
            continue;
        }
        if (++stale < 6 || reported || !gWatchThread) continue;  // 3 s without a world tick
        // After a crash the deadman above acts; the dump's blocking log calls could wait forever on a log lock the crashed
        // thread holds, and the CRASH? lines already describe it.
        if (gFatalAt) continue;
        // The dump suspends threads: not while another thread of ours does (CodePatch, FpuGuard); next time then.
        if (!CodePatch::TryLockSuspenders()) continue;
        reported = true;
        // Every thread of the process (the game thread first): a stalled game thread is often
        // waiting on another one (soak9: a render-command fence wait at 0x00661BBA). Each thread
        // is captured while suspended and logged only after it is resumed (the logger's lock
        // could be held by a suspended thread).
        std::vector<DWORD> tids{gWatchThread};
        HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
        if (snap != INVALID_HANDLE_VALUE) {
            THREADENTRY32 te{};
            te.dwSize = sizeof te;
            for (BOOL ok = Thread32First(snap, &te); ok; ok = Thread32Next(snap, &te))
                if (te.th32OwnerProcessID == GetCurrentProcessId() && te.th32ThreadID != GetCurrentThreadId() &&
                    te.th32ThreadID != gWatchThread)
                    tids.push_back(te.th32ThreadID);
            CloseHandle(snap);
        }
        for (size_t k = 0; k < tids.size() && k < 64; ++k) {
            HANDLE t = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT, FALSE, tids[k]);
            if (!t) continue;
            char line[256];
            int n = 0, shown = 0;
            CONTEXT c{};
            c.ContextFlags = CONTEXT_FULL;
            SuspendThread(t);
            const bool got = GetThreadContext(t, &c) != 0;
            if (got) {
                const DWORD* sp = reinterpret_cast<const DWORD*>(c.Esp);
                for (int i = 0; i < 256 && shown < 14; ++i) {
                    MEMORY_BASIC_INFORMATION mbi;
                    if (!VirtualQuery(sp + i, &mbi, sizeof mbi) || mbi.State != MEM_COMMIT) break;
                    const DWORD v = sp[i];
                    if ((v >= 0x00401000 && v < 0x00F9C000) || (v >= gSelfLo && v < gSelfHi)) {
                        n = AppendF(line, sizeof line, n, " %08lX", v);
                        ++shown;
                    }
                }
            }
            ResumeThread(t);
            CloseHandle(t);
            line[n] = 0;
            if (!got) continue;
            if (k == 0) {
                Logger::Info("HANG? game thread %lu stalled >3s: eip=%08lX esp=%08lX ebp=%08lX eax=%08lX ecx=%08lX esi=%08lX "
                             "edi=%08lX crumb=%s(%u) stack:%s",
                             tids[k], c.Eip, c.Esp, c.Ebp, c.Eax, c.Ecx, c.Esi, c.Edi, gCrumb, gCrumbValue, line);
                // Recursion shows as a few return addresses repeated many times: summarise the
                // distinct code addresses in the top 64 KB of the stack with their counts.
                std::vector<std::pair<DWORD, int>> seen;
                const DWORD* sp = reinterpret_cast<const DWORD*>(c.Esp);
                MEMORY_BASIC_INFORMATION smbi;
                if (!VirtualQuery(sp, &smbi, sizeof smbi) || smbi.State != MEM_COMMIT) continue;
                // Stay inside the committed stack region (the summary scan once read past the stack top).
                const DWORD* stackEnd = reinterpret_cast<const DWORD*>(reinterpret_cast<uintptr_t>(smbi.BaseAddress) + smbi.RegionSize);
                for (int i = 0; i < 0x4000 && sp + i < stackEnd; ++i) {
                    const DWORD v = sp[i];
                    if (!((v >= 0x00401000 && v < 0x00F9C000) || (v >= gSelfLo && v < gSelfHi))) continue;
                    bool found = false;
                    for (auto& s : seen)
                        if (s.first == v) {
                            ++s.second;
                            found = true;
                            break;
                        }
                    if (!found && seen.size() < 64) seen.push_back({v, 1});
                }
                std::sort(seen.begin(), seen.end(), [](auto& a, auto& b) { return a.second > b.second; });
                char sum[400];
                sum[0] = 0;  // printed as is when the stack held no code address
                int sn = 0;
                for (size_t i = 0; i < seen.size() && i < 16; ++i)
                    sn = AppendF(sum, sizeof sum, sn, " %08lX x%d", seen[i].first, seen[i].second);
                Logger::Info("HANG? game thread stack summary (64 KB):%s", sum);
                // rc25 SyncTest soaks (b44/b45): two hangs in UE3's purge loop 0x00516E70 (IncrementalPurgeGarbage, called
                // from 0x005F225E at a fight's end), which with no time limit retries the current object until its
                // vt+0x2C (IsReadyForFinishDestroy) says yes. Name that object: the purge state is at 0x00E7C068 (array
                // +0x0C, count +0x10, index +0x18).
                if (c.Eip >= 0x00516E70 && c.Eip < 0x00516FA0) {
                    uint32_t arr = 0, cnt = 0, idx = 0, obj = 0, vt = 0, ready = 0, cls = 0, outer = 0;
                    SafeRead32(0x00E7C068 + 0x0C, &arr);
                    SafeRead32(0x00E7C068 + 0x10, &cnt);
                    SafeRead32(0x00E7C068 + 0x18, &idx);
                    if (arr && idx < cnt) SafeRead32(arr + 4 * idx, &obj);
                    if (obj) {
                        SafeRead32(obj, &vt);
                        if (vt) SafeRead32(vt + 0x2C, &ready);
                        SafeRead32(obj + 0x30, &cls);
                        SafeRead32(obj + 0x28, &outer);
                    }
                    char on[96] = "", cn[96] = "", oun[96] = "";
                    if (obj) Ue3Names::NameOf(obj, on, sizeof on);
                    if (cls) Ue3Names::NameOf(cls, cn, sizeof cn);
                    if (outer) Ue3Names::NameOf(outer, oun, sizeof oun);
                    Logger::Info("HANG? purge loop: object %u of %u, %08X '%s' class %08X '%s' outer %08X '%s', vtable %08X, "
                                 "IsReadyForFinishDestroy %08X", idx, cnt, obj, on, cls, cn, outer, oun, vt, ready);
                    if (obj) {  // its nonzero dwords (fence counters, flags), to compare with the class layout
                        char dw[1100];
                        dw[0] = 0;  // an object that could not be read, or all zeros
                        int dn = 0;
                        for (uint32_t o = 0; o < 0x200 && dn < (int)sizeof dw - 24; o += 4) {
                            uint32_t v = 0;
                            if (SafeRead32(obj + o, &v) && v) dn = AppendF(dw, sizeof dw, dn, " +%X=%08X", o, v);
                        }
                        Logger::Info("HANG? purge object dwords:%s", dw);
                    }
                }
            }
            else if (shown)  // threads with game code on their stack
                Logger::Info("HANG?   thread %lu eip=%08lX esp=%08lX stack:%s", tids[k], c.Eip, c.Esp, line);
        }
        // A stalled game thread inside a window-system call is often showing an error
        // dialog (assert / fatal error message box): log the process's visible windows.
        Logger::Flush();
        EnumWindows(LogProcessWindow, 0);
        Logger::Flush();
        CodePatch::UnlockSuspenders();
        if (gCrtDialog) {
            Logger::TryInfo("HANG? a Visual C++ runtime error box is open (the game ends when it is dismissed): closing the "
                            "game instead of leaving it frozen behind the box");
            TerminateProcess(GetCurrentProcess(), 0xC0000409);
        }
    }
    return 0;
}
}  // namespace

bool WatchWrite(unsigned threadId, uintptr_t address, bool execute) {
    HANDLE t = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_SET_CONTEXT, FALSE, threadId);
    if (!t) return false;
    InterlockedExchange(&gWatchHits, 0);
    SuspendThread(t);
    CONTEXT c{};
    c.ContextFlags = CONTEXT_DEBUG_REGISTERS;
    bool ok = GetThreadContext(t, &c) != 0;
    if (ok) {
        c.Dr0 = address & ~3u;
        // L0; write: RW0=01, LEN0=11 (4 bytes); execute: RW0=00, LEN0=00
        c.Dr7 = address ? (execute ? 1u : (1u | (1u << 16) | (3u << 18))) : 0;
        ok = SetThreadContext(t, &c) != 0;
    }
    ResumeThread(t);
    CloseHandle(t);
    return ok;
}

int WatchWriteAll(uintptr_t address) {
    // Same breakpoint on every thread of the process except the caller (a field that changes with no
    // game-thread writer is written by the render or a worker thread).
    InterlockedExchange(&gWatchHits, 0);
    int armed = 0;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snap == INVALID_HANDLE_VALUE) return 0;
    THREADENTRY32 te{sizeof te};
    for (BOOL ok = Thread32First(snap, &te); ok; ok = Thread32Next(snap, &te)) {
        if (te.th32OwnerProcessID != GetCurrentProcessId() || te.th32ThreadID == GetCurrentThreadId()) continue;
        HANDLE t = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_SET_CONTEXT, FALSE, te.th32ThreadID);
        if (!t) continue;
        SuspendThread(t);
        CONTEXT c{};
        c.ContextFlags = CONTEXT_DEBUG_REGISTERS;
        if (GetThreadContext(t, &c)) {
            c.Dr0 = address & ~3u;
            c.Dr7 = address ? (1u | (1u << 16) | (3u << 18)) : 0;
            if (SetThreadContext(t, &c)) ++armed;
        }
        ResumeThread(t);
        CloseHandle(t);
    }
    CloseHandle(snap);
    return armed;
}

bool TraceArm(unsigned threadId, const uintptr_t addrs[4]) {
    HANDLE t = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_SET_CONTEXT, FALSE, threadId);
    if (!t) return false;
    uint32_t dr7 = 0;
    for (int s = 0; s < 4; ++s) {
        gTraceAddr[s] = addrs[s] & ~3u;
        // Ls; write: RWs=01, LENs=11 (4 bytes)
        if (addrs[s]) dr7 |= (1u << (2 * s)) | (1u << (16 + 4 * s)) | (3u << (18 + 4 * s));
    }
    gTraceThread = threadId;
    InterlockedExchange(&gTraceMode, dr7 ? 1 : 0);
    SuspendThread(t);
    CONTEXT c{};
    c.ContextFlags = CONTEXT_DEBUG_REGISTERS;
    bool ok = GetThreadContext(t, &c) != 0;
    if (ok) {
        c.Dr0 = gTraceAddr[0];
        c.Dr1 = gTraceAddr[1];
        c.Dr2 = gTraceAddr[2];
        c.Dr3 = gTraceAddr[3];
        c.Dr7 = dr7;
        ok = SetThreadContext(t, &c) != 0;
    }
    ResumeThread(t);
    CloseHandle(t);
    if (gTraceAllThreads) {  // every other thread of the process too (pool threads run the actor and anim jobs)
        HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
        THREADENTRY32 te{};
        te.dwSize = sizeof te;
        int armed = 0;
        for (BOOL more = snap != INVALID_HANDLE_VALUE && Thread32First(snap, &te); more; more = Thread32Next(snap, &te)) {
            if (te.th32OwnerProcessID != GetCurrentProcessId() || te.th32ThreadID == threadId || te.th32ThreadID == GetCurrentThreadId())
                continue;
            HANDLE w = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_SET_CONTEXT, FALSE, te.th32ThreadID);
            if (!w) continue;
            SuspendThread(w);
            CONTEXT wc{};
            wc.ContextFlags = CONTEXT_DEBUG_REGISTERS;
            if (GetThreadContext(w, &wc)) {
                wc.Dr0 = gTraceAddr[0];
                wc.Dr1 = gTraceAddr[1];
                wc.Dr2 = gTraceAddr[2];
                wc.Dr3 = gTraceAddr[3];
                wc.Dr7 = dr7;
                if (SetThreadContext(w, &wc)) ++armed;
            }
            ResumeThread(w);
            CloseHandle(w);
        }
        if (snap != INVALID_HANDLE_VALUE) CloseHandle(snap);
        Logger::Info("TRACE armed on %d other threads", armed);
    }
    return ok;
}

void SetTraceAllThreads(bool on) { gTraceAllThreads = on; }

void ChangeWatchArm(const uintptr_t addrs[4], const char* const names[4]) {
    bool any = false;
    for (int s = 0; s < 4; ++s) {
        gChangeAddr[s] = addrs[s] & ~3u;
        gChangeName[s] = names[s] ? names[s] : "?";
        gChangeLast[s] = addrs[s] ? *reinterpret_cast<const volatile DWORD*>(gChangeAddr[s]) : 0;
        InterlockedExchange(&gChangeFlips[s], 0);
        InterlockedExchange(&gChangeOther[s], 0);
        any |= addrs[s] != 0;
    }
    if (any) InterlockedExchange(&gChangeMode, 1);  // before any thread can hit; disarming clears it after the registers
    if (HANDLE t = CreateThread(nullptr, 0, ChangeWatchProc, nullptr, 0, nullptr)) CloseHandle(t);
    else if (!any) InterlockedExchange(&gChangeMode, 0);
}

void TraceFrame(uint32_t gf, bool resim) {
    if (gf == 0xFFFFFFFF) {
        gTraceCur = nullptr;
        return;
    }
    TraceFrameRec* f = &gTrace[resim ? 1 : 0][gf % 128];
    f->gf = gf;
    f->n = 0;
    gTraceCur = f;
}

void TraceDump(uint32_t gf) {
    if (!gTraceMode) return;
    const TraceFrameRec& a = gTrace[0][gf % 128];
    const TraceFrameRec& b = gTrace[1][gf % 128];
    if (a.gf != gf || b.gf != gf) {
        Logger::Info("LOCTRACE gf=%u: no live/resim pair recorded", gf);
        return;
    }
    const uint32_t an = a.n < 32 ? a.n : 32, bn = b.n < 32 ? b.n : 32;
    uint32_t i = 0;
    while (i < an && i < bn && a.r[i].slot == b.r[i].slot && a.r[i].eip == b.r[i].eip && a.r[i].val == b.r[i].val) ++i;
    Logger::Info("LOCTRACE gf=%u: live %u writes, resim %u writes, first difference at #%u", gf, a.n, b.n, i);
    for (int pass = 0; pass < 2; ++pass) {
        const TraceFrameRec& f = pass ? b : a;
        char line[600];
        int n = 0;
        const uint32_t fn = f.n < 32 ? f.n : 32;
        for (uint32_t j = i > 2 ? i - 2 : 0; j < fn && j < i + 4; ++j)
            n = AppendF(line, sizeof line, n, " #%u s%u@%08X=%08X(%08X<%08X<%08X)", j, f.r[j].slot, f.r[j].eip, f.r[j].val,
                          f.r[j].ret[0], f.r[j].ret[1], f.r[j].ret[2]);
        line[n] = 0;
        Logger::Info("LOCTRACE   %s:%s", pass ? "resim" : "live ", line);
    }
}

void TraceLog(uint32_t gf, bool resim, const char* tag) {
    const TraceFrameRec& f = gTrace[resim ? 1 : 0][gf % 128];
    if (f.gf != gf) {
        Logger::Info("%s gf=%u %s: no writes recorded", tag, gf, resim ? "replay" : "original");
        return;
    }
    const uint32_t fn = f.n < 32 ? f.n : 32;
    char line[1200];
    int n = 0;
    for (uint32_t j = 0; j < fn && n < (int)sizeof line - 80; ++j) {
        float v;
        memcpy(&v, &f.r[j].val, 4);
        n = AppendF(line, sizeof line, n, " #%u s%u@%08X=%.3f(%08X<%08X<%08X)", j, f.r[j].slot, f.r[j].eip, v, f.r[j].ret[0],
                      f.r[j].ret[1], f.r[j].ret[2]);
    }
    line[n] = 0;
    Logger::Info("%s gf=%u %s: %u writes:%s", tag, gf, resim ? "replay" : "original", f.n, n ? line : " none");
}

uintptr_t TraceAddr(int slot) { return gTraceMode && slot >= 0 && slot < 4 ? gTraceAddr[slot] : 0; }

void Heartbeat(unsigned gameThreadId) {
    gWatchThread = gameThreadId;
    InterlockedIncrement(&gHeartbeat);
    if (!gWatchdog) gWatchdog = CreateThread(nullptr, 0, Watchdog, nullptr, 0, nullptr);
}

void SetRegionNamer(RegionNamer fn) { gNamer = fn; }
void SetPtrHistory(PtrHistory fn) { gHistory = fn; }
void SetSavedDwords(SavedDwords fn) { gSaved = fn; }

void StopWatchdog() {
    InterlockedExchange(&gWatchStop, 1);
    if (gWatchdog) {
        WaitForSingleObject(gWatchdog, 2000);
        CloseHandle(gWatchdog);
        gWatchdog = nullptr;
    }
}

bool SafeReadAvailable() { return gHandler != nullptr; }
bool SafeRead32(uintptr_t address, uint32_t* out) {
    if (!gHandler) return false;
    return MK9R_SafeRead32(reinterpret_cast<const void*>(address), out) != 0;
}

bool SafeBlockInfo(const void* ptr, unsigned* out) {
    if (!gHandler) return false;  // no recovery without the handler
    MK9R_gProbeThread = GetCurrentThreadId();
    return MK9R_SafeBlockInfoAsm(ptr, out) != 0;
}

void Crumb(const char* what) { gCrumb = what; }
void CrumbFrame(const char* what, unsigned value) {
    gCrumb = what;
    gCrumbValue = value;
}

void LogProcessExit() {
    // DLL_PROCESS_DETACH runs on the thread that called ExitProcess: its stack names the code that ended the game.
    // The loader's shutdown frames sit between here and the caller (a normal quit had none of MKKE's within 8 KB): scan
    // to the stack base.
    DWORD here = 0;
    const DWORD* sp = &here;
    const DWORD* top = static_cast<const DWORD*>(reinterpret_cast<NT_TIB*>(NtCurrentTeb())->StackBase);
    char line[400];
    int n = 0;
    for (int shown = 0; sp + 1 <= top && shown < 32; ++sp) {
        const DWORD v = *sp;
        if ((v >= 0x00401000 && v < 0x00F9C000) || (v >= gSelfLo && v < gSelfHi)) {
            n = AppendF(line, sizeof line, n, " %08lX", v);
            ++shown;
        }
    }
    line[n] = 0;
    Logger::Info("PROCESS EXIT on thread %lu (game thread %lu), crumb %s(%u); stack code values:%s", GetCurrentThreadId(),
                 (unsigned long)gWatchThread, gCrumb, gCrumbValue, n ? line : " none");
}
}  // namespace CrashLog


