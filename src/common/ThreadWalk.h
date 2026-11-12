#pragma once
#include <windows.h>
#include <tlhelp32.h>

// Walks the other threads of this process: fn(handle, threadId) gets each one's handle, opened with `access`, and
// returns true to keep it (the caller closes it later) or false to have it closed here.
//
// NtGetNextThread lists this process's threads only. A Toolhelp snapshot lists every thread in the system (about 6000
// on a desktop PC: 30-50 ms per snapshot); each of arming's ~56 code patches took one, which froze the game thread for
// 3 s at every launch and tripped the hang watchdog (2026-11-11 field logs). A thread started during the walk is
// appended to the process's list and is still reached, which a snapshot taken before it cannot do.
//
// Nothing here allocates from the process heap, so fn may suspend the threads it is handed.
namespace ThreadWalk {

using NtGetNextThreadFn = LONG(NTAPI*)(HANDLE, HANDLE, ACCESS_MASK, ULONG, ULONG, PHANDLE);

// 0 not looked up yet, 1 NtGetNextThread, 2 Toolhelp (no such export, or a walk ended on an unexpected status).
inline int gHow = 0;
inline NtGetNextThreadFn gNext = nullptr;

// False when no snapshot could be taken: no thread was handed to fn (a caller that suspends the threads it gets must not
// take that for "there are no other threads").
template <class Fn>
bool OthersToolhelp(ACCESS_MASK access, Fn&& fn) {
    const DWORD self = GetCurrentThreadId(), pid = GetCurrentProcessId();
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snap == INVALID_HANDLE_VALUE) return false;
    THREADENTRY32 te{};
    te.dwSize = sizeof te;
    for (BOOL ok = Thread32First(snap, &te); ok; ok = Thread32Next(snap, &te)) {
        if (te.th32OwnerProcessID != pid || te.th32ThreadID == self) continue;
        HANDLE t = OpenThread(access, FALSE, te.th32ThreadID);
        if (t && !fn(t, te.th32ThreadID)) CloseHandle(t);
    }
    CloseHandle(snap);
    return true;
}

// Returns false when the walk could not be completed (some threads were handed to fn, others were not reached, or on
// the Toolhelp route none at all): the caller undoes what fn did and calls again; that call takes the Toolhelp route.
template <class Fn>
bool Others(ACCESS_MASK access, Fn&& fn) {
    if (!gHow) {
        gNext = reinterpret_cast<NtGetNextThreadFn>(
            reinterpret_cast<void*>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtGetNextThread")));
        gHow = gNext ? 1 : 2;
    }
    if (gHow == 2) return OthersToolhelp(access, fn);
    const DWORD self = GetCurrentThreadId();
    HANDLE cur = nullptr;
    bool kept = false;
    for (;;) {
        HANDLE next = nullptr;
        const LONG status = gNext(GetCurrentProcess(), cur, access | THREAD_QUERY_LIMITED_INFORMATION, 0, 0, &next);
        if (cur && !kept) CloseHandle(cur);
        if (status < 0) {
            if ((ULONG)status == 0x8000001Aul) return true;  // STATUS_NO_MORE_ENTRIES
            gHow = 2;  // e.g. a thread that cannot be opened with this access: Toolhelp skips such a thread
            return false;
        }
        cur = next;
        const DWORD id = GetThreadId(cur);
        kept = id != self && fn(cur, id);
    }
}

}  // namespace ThreadWalk
