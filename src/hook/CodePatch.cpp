#include "CodePatch.h"

#include <windows.h>
#include <tlhelp32.h>

#include <cstring>
#include <vector>

#include "../debug/Logger.h"

namespace {
struct Patch {
    const char* name;
    uintptr_t address;
    std::vector<uint8_t> original, replacement;
};
std::vector<Patch> gPatches;

// Constructed at DLL load, before InitThread can patch anything.
struct SuspendLock {
    CRITICAL_SECTION cs;
    SuspendLock() { InitializeCriticalSection(&cs); }
} gSuspendLock;
struct SuspendScope {
    SuspendScope() { EnterCriticalSection(&gSuspendLock.cs); }
    ~SuspendScope() { LeaveCriticalSection(&gSuspendLock.cs); }
};

// Suspends all other threads; returns their handles. If any thread's EIP is in
// [lo, hi), resumes everything and returns false so the caller can retry.
bool SuspendOthers(uintptr_t lo, uintptr_t hi, std::vector<HANDLE>* out) {
    const DWORD self = GetCurrentThreadId(), pid = GetCurrentProcessId();
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snap == INVALID_HANDLE_VALUE) return false;
    THREADENTRY32 te{sizeof te};
    bool clear = true;
    for (BOOL ok = Thread32First(snap, &te); ok; ok = Thread32Next(snap, &te)) {
        if (te.th32OwnerProcessID != pid || te.th32ThreadID == self) continue;
        HANDLE t = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT, FALSE, te.th32ThreadID);
        if (!t) continue;
        if (SuspendThread(t) == (DWORD)-1) {
            CloseHandle(t);
            continue;
        }
        out->push_back(t);
        CONTEXT ctx{};
        ctx.ContextFlags = CONTEXT_CONTROL;
        if (GetThreadContext(t, &ctx) && ctx.Eip >= lo && ctx.Eip < hi) clear = false;
    }
    CloseHandle(snap);
    return clear;
}

void ResumeAll(std::vector<HANDLE>& threads) {
    for (HANDLE t : threads) {
        ResumeThread(t);
        CloseHandle(t);
    }
    threads.clear();
}

bool WriteSuspended(uintptr_t address, const uint8_t* bytes, uint32_t length) {
    for (int attempt = 0; attempt < 50; ++attempt) {
        // No heap allocation may happen while others are suspended (one of them
        // could hold the process heap lock), so reserve up front.
        std::vector<HANDLE> threads;
        threads.reserve(1024);
        if (SuspendOthers(address, address + length, &threads)) {
            DWORD old = 0, ignored = 0;
            bool ok = VirtualProtect(reinterpret_cast<void*>(address), length, PAGE_EXECUTE_READWRITE, &old) != 0;
            if (ok) {
                memcpy(reinterpret_cast<void*>(address), bytes, length);
                VirtualProtect(reinterpret_cast<void*>(address), length, old, &ignored);
                FlushInstructionCache(GetCurrentProcess(), reinterpret_cast<void*>(address), length);
            }
            ResumeAll(threads);
            return ok;
        }
        ResumeAll(threads);
        Sleep(2);
    }
    return false;
}
}  // namespace

namespace CodePatch {

bool Write(uintptr_t address, const uint8_t* expected, const uint8_t* replacement, uint32_t length, const char* name) {
    // InitThread (overlay) and the game thread (launcher commands at the first ticks) can patch at the same time.
    SuspendScope lock;
    if (memcmp(reinterpret_cast<const void*>(address), expected, length) != 0) {
        Logger::Info("CODEPATCH %s: refused, bytes at 0x%08X differ from expected", name, address);
        return false;
    }
    if (!WriteSuspended(address, replacement, length)) {
        Logger::Info("CODEPATCH %s: could not find a safe moment to write", name);
        return false;
    }
    gPatches.push_back({name, address, std::vector<uint8_t>(expected, expected + length),
                        std::vector<uint8_t>(replacement, replacement + length)});
    Logger::Info("CODEPATCH %s: 0x%08X (%u bytes) patched", name, address, length);
    return true;
}

bool RestoreAll() {
    SuspendScope lock;
    bool ok = true;
    for (auto it = gPatches.rbegin(); it != gPatches.rend(); ++it) {
        if (!WriteSuspended(it->address, it->original.data(), (uint32_t)it->original.size())) {
            Logger::Info("CODEPATCH %s: restore FAILED", it->name);
            ok = false;
            continue;
        }
        Logger::Info("CODEPATCH %s: restored", it->name);
    }
    gPatches.clear();
    return ok;
}

bool Restore(uintptr_t address) {
    SuspendScope lock;
    for (auto it = gPatches.begin(); it != gPatches.end(); ++it) {
        if (it->address != address) continue;
        if (!WriteSuspended(it->address, it->original.data(), (uint32_t)it->original.size())) {
            Logger::Info("CODEPATCH %s: restore FAILED", it->name);
            return false;
        }
        Logger::Info("CODEPATCH %s: restored", it->name);
        gPatches.erase(it);
        return true;
    }
    return false;
}

void LockSuspenders() { EnterCriticalSection(&gSuspendLock.cs); }
bool TryLockSuspenders() { return TryEnterCriticalSection(&gSuspendLock.cs) != 0; }
void UnlockSuspenders() { LeaveCriticalSection(&gSuspendLock.cs); }

void* AllocExec(uint32_t size) {
    return VirtualAlloc(nullptr, size, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
}

}  // namespace CodePatch
