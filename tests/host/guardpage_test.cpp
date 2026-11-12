// src\common\GuardPage.h, with the same kind of guarded read as CrashLog::SafeRead32 (one load instruction whose fault
// a vectored handler turns into "read failed").
//
// A thread's stack ends in a guard page; Windows grows the stack when the thread itself touches it. When ANOTHER thread
// reads that page, the reader gets STATUS_GUARD_PAGE_VIOLATION and the page loses its guard protection. A handler that
// only swallows the exception leaves that thread without a way to grow its stack: its next deep call faults.
//
//   guardpage_test            the read is followed by GuardPage::Rearm (what the DLL's handler does now): the page is a
//                             guard page again and the thread grows its stack 128 KB past it. Then the same without Rearm in
//                             a child process, to show the failure this prevents (reported, not counted as a check: it
//                             describes Windows, not our code).
//   guardpage_test norearm    that child: reads the guard page, does not put it back, lets the thread go deep.
#include <windows.h>

#include <cstdint>
#include <cstdio>
#include <cstring>

#include "../../src/common/GuardPage.h"

extern "C" {
int __cdecl T_SafeRead32(const void* p, uint32_t* out);
void T_SafeReadInsn();
void T_SafeReadFail();
}
__asm__(R"(
    .text
    .globl _T_SafeRead32
    .globl _T_SafeReadInsn
    .globl _T_SafeReadFail
_T_SafeRead32:
    movl 4(%esp), %ecx
_T_SafeReadInsn:
    movl (%ecx), %eax
    movl 8(%esp), %ecx
    movl %eax, (%ecx)
    movl $1, %eax
    ret
_T_SafeReadFail:
    xorl %eax, %eax
    ret
)");

namespace {
bool gRearm = true;
volatile LONG gGuardFaults = 0;

LONG CALLBACK Handler(EXCEPTION_POINTERS* ep) {
    const DWORD code = ep->ExceptionRecord->ExceptionCode;
    if ((code == EXCEPTION_ACCESS_VIOLATION || code == STATUS_GUARD_PAGE_VIOLATION) &&
        ep->ContextRecord->Eip == reinterpret_cast<DWORD>(&T_SafeReadInsn)) {
        if (code == STATUS_GUARD_PAGE_VIOLATION) {
            InterlockedIncrement(&gGuardFaults);
            if (gRearm)
                GuardPage::Rearm(ep->ExceptionRecord->NumberParameters >= 2 ? ep->ExceptionRecord->ExceptionInformation[1]
                                                                            : ep->ContextRecord->Ecx);
        }
        ep->ContextRecord->Eip = reinterpret_cast<DWORD>(&T_SafeReadFail);
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

int gFailed = 0, gChecks = 0;
void Check(bool ok, const char* what) {
    ++gChecks;
    if (ok) return;
    ++gFailed;
    printf("FAIL: %s\n", what);
}

bool IsGuard(uintptr_t a) {
    MEMORY_BASIC_INFORMATION mbi;
    return VirtualQuery(reinterpret_cast<void*>(a), &mbi, sizeof mbi) && mbi.State == MEM_COMMIT && (mbi.Protect & PAGE_GUARD);
}

HANDLE gReady, gGo;
volatile uintptr_t gGuardAt = 0;  // the highest guard page of the worker's stack
volatile int gGuardPages = 0;     // how many guard pages lie there in a row (more than one on some systems)

// 2 KB of stack per level. The buffer is handed to a function the compiler cannot see through, and the recursion goes
// through a volatile pointer: otherwise the buffer shrinks to the two bytes used and the recursion becomes a loop.
void Touch(char* p, size_t n) {
    p[0] = 1;
    p[n - 1] = 2;
}
void (*volatile gTouch)(char*, size_t) = Touch;
int Deep(int n);
int (*volatile gDeep)(int) = Deep;
__attribute__((noinline)) int Deep(int n) {
    char pad[2048];
    gTouch(pad, sizeof pad);
    if (!n) return pad[0];
    return gDeep(n - 1) + pad[0] + pad[2047];
}

DWORD WINAPI Worker(void*) {
    // The lowest committed page of this stack is at the TEB's stack limit; the guard page lies right below it.
    const uintptr_t limit = reinterpret_cast<uintptr_t>(reinterpret_cast<const NT_TIB*>(NtCurrentTeb())->StackLimit);
    const uintptr_t g = limit - 0x1000;
    gGuardAt = IsGuard(g) ? g : 0;
    int pages = 0;
    while (pages < 64 && IsGuard(g - 0x1000u * pages)) ++pages;
    gGuardPages = pages;
    SetEvent(gReady);
    WaitForSingleObject(gGo, INFINITE);
    // Deep enough to use up what is committed already (it can be hundreds of KB) and go 128 KB further: through the
    // guard region and well past it.
    const uintptr_t sp = reinterpret_cast<uintptr_t>(__builtin_frame_address(0));
    const int levels = (int)((sp - limit) / 2048) + 64;
    const int r = Deep(levels);
    const uintptr_t now = reinterpret_cast<uintptr_t>(reinterpret_cast<const NT_TIB*>(NtCurrentTeb())->StackLimit);
    return now < limit ? 1000 + (r & 1) : 999;  // 999: the stack did not grow at all (the scenario did not run)
}

// Reads another thread's stack guard page with the guarded read, then lets that thread go deep. Returns its exit code
// (1000 or 1001 when it came back), 0 when the scenario could not be set up.
DWORD Scenario(bool* wasGuard, bool* readFailed, bool* guardAfter) {
    gReady = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    gGo = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    HANDLE t = CreateThread(nullptr, 0, Worker, nullptr, 0, nullptr);
    WaitForSingleObject(gReady, 5000);
    const uintptr_t g = gGuardAt;
    *wasGuard = g != 0;
    if (!g) {
        SetEvent(gGo);
        WaitForSingleObject(t, 5000);
        return 0;
    }
    // Every page of the guard region is read, as a scan over that stack or pointer-shaped values landing there would.
    *readFailed = true;
    *guardAfter = true;
    for (int i = 0; i < gGuardPages; ++i) {
        uint32_t v = 0;
        *readFailed = !T_SafeRead32(reinterpret_cast<const void*>(g - 0x1000u * i + 0x800), &v) && *readFailed;
        *guardAfter = IsGuard(g - 0x1000u * i) && *guardAfter;
    }
    SetEvent(gGo);
    WaitForSingleObject(t, 10000);
    DWORD code = 0;
    GetExitCodeThread(t, &code);
    CloseHandle(t);
    return code;
}
}  // namespace

int wmain(int argc, wchar_t** argv) {
    setvbuf(stdout, nullptr, _IONBF, 0);
    AddVectoredExceptionHandler(1, Handler);
    bool wasGuard = false, readFailed = false, guardAfter = false;
    if (argc > 1 && !wcscmp(argv[1], L"norearm")) {
        SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);  // the expected crash without a dialog
        gRearm = false;
        const DWORD code = Scenario(&wasGuard, &readFailed, &guardAfter);
        return code >= 1000 ? 0 : 3;  // 0: the thread survived without the fix
    }

    {  // a guard page of our own: the read fails, and the page is a guard page again afterwards
        uint8_t* p = static_cast<uint8_t*>(VirtualAlloc(nullptr, 0x3000, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
        DWORD old = 0;
        VirtualProtect(p + 0x1000, 0x1000, PAGE_READWRITE | PAGE_GUARD, &old);
        uint32_t v = 0;
        Check(T_SafeRead32(p + 0x10, &v), "a plain page reads");
        Check(!T_SafeRead32(p + 0x1010, &v) && gGuardFaults == 1, "a guard page fails the guarded read");
        Check(IsGuard(reinterpret_cast<uintptr_t>(p) + 0x1000), "the page is a guard page again after the read");
        // Across the boundary: the first page is plain, the fault is on the second.
        Check(!T_SafeRead32(p + 0x0FFE, &v) && IsGuard(reinterpret_cast<uintptr_t>(p) + 0x1000), "a read across the boundary re-arms the page it faulted on");
        Check(!IsGuard(reinterpret_cast<uintptr_t>(p)) && !IsGuard(reinterpret_cast<uintptr_t>(p) + 0x2000), "the neighbouring pages are left alone");
        Check(!T_SafeRead32(reinterpret_cast<const void*>(0x10), &v), "an unmapped address fails the guarded read");
        Check(!GuardPage::Rearm(0x10), "Rearm of an unmapped address reports failure");
        VirtualFree(p, 0, MEM_RELEASE);
    }

    // Another thread's stack guard, then that thread grows its stack past it.
    const DWORD code = Scenario(&wasGuard, &readFailed, &guardAfter);
    printf("the worker's stack has %d guard page(s); after the read its thread ended with %lu\n", (int)gGuardPages, code);
    Check(wasGuard, "the worker's stack guard page was found");
    Check(readFailed, "the guarded read of another thread's stack guard fails");
    Check(guardAfter, "that page is a guard page again");
    Check(code >= 1000, "the worker grows its stack through the page afterwards");

    // The same without Rearm, in a child (it is expected to die).
    wchar_t self[MAX_PATH], cmd[MAX_PATH + 32];
    GetModuleFileNameW(nullptr, self, MAX_PATH);
    swprintf(cmd, MAX_PATH + 32, L"\"%ls\" norearm", self);
    STARTUPINFOW si{};
    si.cb = sizeof si;
    PROCESS_INFORMATION pi{};
    if (CreateProcessW(self, cmd, nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi)) {
        WaitForSingleObject(pi.hProcess, 20000);
        DWORD child = 0;
        GetExitCodeProcess(pi.hProcess, &child);
        printf("without the re-arm: the child process ended with %08lX (%s)\n", child,
               child == 0 ? "its thread survived: this Windows does not show the failure"
                          : child == 0xC0000005 || child == 0xC00000FD ? "its thread could not grow its stack, as expected" : "unexpected");
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
    } else {
        printf("without the re-arm: the child process could not be started (%lu)\n", GetLastError());
    }

    printf("%d checks, %d failed\n", gChecks, gFailed);
    printf(gFailed ? "FAILED\n" : "ALL CHECKS PASSED\n");
    return gFailed ? 1 : 0;
}
