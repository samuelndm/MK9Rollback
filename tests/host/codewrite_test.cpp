// src\hook\CodePatch.cpp and src\common\ThreadWalk.h without the game: code is patched in this process while 48 threads
// run, half of them calling the patched function.
//
// What it guards: every other thread is suspended for a write and running again after it; a write whose expected bytes
// differ is refused; a write over an instruction a thread is executing is refused instead of made; callers that never
// pause still only ever see a whole function; Restore and RestoreAll put the first bytes back; both ways of walking the
// threads (NtGetNextThread and the Toolhelp snapshot) reach the same threads; and 56 writes, as many as arming the mod
// makes in one game frame, finish well inside half a second (with a system-wide snapshot per write they froze the game
// for 3 s at every launch, 2026-11-11).
//
// The program's name must not contain "patch", "setup", "install" or "update": Windows takes such a 32-bit program
// without a manifest for an installer and asks for administrator rights before it starts.
#include <windows.h>

#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>

#include "../../src/common/ThreadWalk.h"
#include "../../src/hook/CodePatch.h"

namespace Logger {
char gLast[512];
void Info(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(gLast, sizeof gLast, fmt, ap);
    va_end(ap);
}
}  // namespace Logger

namespace {
int gFailed = 0, gChecks = 0;
void Check(bool ok, const char* what) {
    ++gChecks;
    if (ok) return;
    ++gFailed;
    printf("FAIL: %s (last log line: %s)\n", what, Logger::gLast);
}

using Fn = int (*)();
uint8_t* gCode = nullptr;
volatile LONG gStop = 0, gStopHot = 0;
const int kThreads = 48, kHot = 3;
volatile LONG gCount[kThreads + kHot];
volatile LONG gBadValue = 0;

void CallOnce(int i) {
    const int v = reinterpret_cast<Fn>(reinterpret_cast<void*>(gCode))();
    if (v != 1 && v != 2) InterlockedIncrement(&gBadValue);
    InterlockedIncrement(&gCount[i]);
}

DWORD WINAPI Caller(void* arg) {
    while (!gStop) {
        CallOnce((int)(intptr_t)arg);
        Sleep(1);
    }
    return 0;
}

DWORD WINAPI Idler(void* arg) {
    while (!gStop) {
        Sleep(1);
        InterlockedIncrement(&gCount[(int)(intptr_t)arg]);
    }
    return 0;
}

// Calls without a pause: at any moment such a thread is likely inside the function, so writes have to wait for it.
DWORD WINAPI HotCaller(void* arg) {
    while (!gStopHot) CallOnce((int)(intptr_t)arg);
    return 0;
}

// True when every thread's counter moves within two seconds: none was left suspended.
bool AllRunning(int count = kThreads) {
    LONG before[kThreads + kHot];
    for (int i = 0; i < count; ++i) before[i] = gCount[i];
    for (int waited = 0; waited < 2000; waited += 10) {
        Sleep(10);
        bool all = true;
        for (int i = 0; i < count; ++i) all = all && gCount[i] != before[i];
        if (all) return true;
    }
    return false;
}

int WalkCount() {
    int n = 0;
    ThreadWalk::Others(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT, [&](HANDLE, DWORD) {
        ++n;
        return false;
    });
    return n;
}

double Ms(LARGE_INTEGER a, LARGE_INTEGER b) {
    LARGE_INTEGER f;
    QueryPerformanceFrequency(&f);
    return (b.QuadPart - a.QuadPart) * 1000.0 / f.QuadPart;
}
}  // namespace

int main() {
    setvbuf(stdout, nullptr, _IONBF, 0);
    gCode = static_cast<uint8_t*>(CodePatch::AllocExec(64));
    const uint8_t one[] = {0xB8, 0x01, 0x00, 0x00, 0x00}, two[] = {0xB8, 0x02, 0x00, 0x00, 0x00};  // mov eax, 1 / 2
    memcpy(gCode, one, 5);
    gCode[5] = 0xC3;  // ret
    const uint8_t nops[] = {0x90, 0x90, 0x90}, ints[] = {0xCC, 0xCC, 0xCC};
    memcpy(gCode + 16, nops, 3);
    gCode[32] = 0xEB;  // jmp $: a thread that never leaves these two bytes
    gCode[33] = 0xFE;
    FlushInstructionCache(GetCurrentProcess(), gCode, 64);
    const Fn fn = reinterpret_cast<Fn>(reinterpret_cast<void*>(gCode));
    const uintptr_t at = reinterpret_cast<uintptr_t>(gCode);

    HANDLE threads[kThreads];
    for (int i = 0; i < kThreads; ++i)
        threads[i] = CreateThread(nullptr, 0, i < kThreads / 2 ? Caller : Idler, (void*)(intptr_t)i, 0, nullptr);
    Check(AllRunning(), "the test's threads run");

    // Both walks reach at least the test's threads, and the same number of them.
    const int fast = WalkCount();
    const int how = ThreadWalk::gHow;
    ThreadWalk::gHow = 2;
    const int slow = WalkCount();
    ThreadWalk::gHow = how;
    printf("threads walked: %d (NtGetNextThread%s), %d (Toolhelp)\n", fast, how == 1 ? "" : " not available", slow);
    Check(fast >= kThreads && slow >= kThreads, "a walk reaches every thread of the test");
    Check(fast == slow, "both walks reach the same threads");

    // The two bytes at +5 and +6 are C3 00, not 'two': refused, nothing written.
    Check(!CodePatch::Write(at + 5, two, one, 2, "wrong expected bytes"), "a write whose expected bytes differ is refused");
    Check(fn() == 1 && gCode[5] == 0xC3, "a refused write changes nothing");

    Check(CodePatch::Write(at, one, two, 5, "one to two"), "a write succeeds while 24 threads call the function");
    Check(fn() == 2, "the patched function returns the new value");
    Check(AllRunning(), "every thread runs again after a write");

    // As many writes as arming makes (56), timed.
    LARGE_INTEGER t0, t1;
    QueryPerformanceCounter(&t0);
    bool all = true;
    for (int i = 0; i < 56; ++i) all = CodePatch::Write(at, i % 2 ? one : two, i % 2 ? two : one, 5, "flip") && all;
    QueryPerformanceCounter(&t1);
    const double ms = Ms(t0, t1);
    printf("56 writes: %.1f ms\n", ms);
    Check(all, "56 writes in a row succeed");
    Check(ms < 500, "56 writes take less than 500 ms");
    Check(fn() == 2 && AllRunning(), "every thread runs again after 56 writes");

    // The same through the Toolhelp route (taken when NtGetNextThread is missing or a walk failed).
    ThreadWalk::gHow = 2;
    Check(CodePatch::Write(at, two, one, 5, "toolhelp route"), "a write succeeds through the Toolhelp walk");
    Check(fn() == 1 && AllRunning(), "the Toolhelp walk resumes every thread");
    ThreadWalk::gHow = how;

    // Callers that never pause: a write waits until none of them is on the bytes, so each call returns 1 or 2 and
    // nothing else. A write may run out of attempts here (reported, not a failure); it must then have written nothing.
    HANDLE hot[kHot];
    for (int i = 0; i < kHot; ++i) hot[i] = CreateThread(nullptr, 0, HotCaller, (void*)(intptr_t)(kThreads + i), 0, nullptr);
    int made = 0, gaveUp = 0;
    bool whole = true;
    uint8_t now = 1;
    for (int i = 0; i < 20; ++i) {
        const bool ok = CodePatch::Write(at, now == 1 ? one : two, now == 1 ? two : one, 5, "flip under hot callers");
        if (ok) {
            now = now == 1 ? 2 : 1;
            ++made;
        } else {
            ++gaveUp;
        }
        whole = whole && gCode[0] == 0xB8 && gCode[1] == now && fn() == now;
    }
    printf("under %d callers that never pause: %d writes made, %d gave up\n", kHot, made, gaveUp);
    Check(made > 0, "writes get through between calls of threads that never pause");
    Check(whole, "the bytes are either the old or the new instruction after every attempt");
    Check(AllRunning(kThreads + kHot), "every thread, the hot callers included, runs again");
    InterlockedExchange(&gStopHot, 1);
    WaitForMultipleObjects(kHot, hot, TRUE, 5000);
    Check(!gBadValue, "no caller ever saw a half-written function");

    // A thread executing the bytes to be replaced: the write must be refused, not made.
    HANDLE spinner = CreateThread(nullptr, 0, reinterpret_cast<LPTHREAD_START_ROUTINE>(reinterpret_cast<void*>(gCode + 32)), nullptr,
                                  CREATE_SUSPENDED, nullptr);
    SetThreadPriority(spinner, THREAD_PRIORITY_IDLE);
    ResumeThread(spinner);
    Sleep(100);
    const uint8_t spin[] = {0xEB, 0xFE}, retn[] = {0xC3, 0x90};
    Check(!CodePatch::Write(at + 32, spin, retn, 2, "under a running thread"), "a write over an instruction a thread is in is refused");
    Check(gCode[32] == 0xEB && gCode[33] == 0xFE, "the refused write left the bytes alone");
    Check(AllRunning(), "every thread runs again after the refused write");
    TerminateThread(spinner, 0);
    CloseHandle(spinner);

    // Restore(address) undoes the one patch there; RestoreAll undoes the rest, newest first, down to the first bytes.
    Check(CodePatch::Write(at + 16, nops, ints, 3, "for Restore") && memcmp(gCode + 16, ints, 3) == 0, "a second address is patched");
    Check(CodePatch::Restore(at + 16) && memcmp(gCode + 16, nops, 3) == 0, "Restore puts that address's bytes back");
    Check(!CodePatch::Restore(at + 16), "Restore of an address without a patch fails");
    Check(CodePatch::RestoreAll() && memcmp(gCode, one, 5) == 0 && fn() == 1, "RestoreAll ends on the original bytes");
    Check(AllRunning(), "every thread runs again after the restores");

    // No thread is left with a suspend count.
    bool clean = true;
    for (int i = 0; i < kThreads; ++i) {
        clean = clean && SuspendThread(threads[i]) == 0;
        ResumeThread(threads[i]);
    }
    Check(clean, "no thread keeps a suspend count");

    InterlockedExchange(&gStop, 1);
    WaitForMultipleObjects(kThreads, threads, TRUE, 5000);
    printf("%d checks, %d failed\n", gChecks, gFailed);
    printf(gFailed ? "FAILED\n" : "ALL CHECKS PASSED\n");
    return gFailed ? 1 : 0;
}
