#include "FpuGuard.h"
#include <windows.h>
#include <tlhelp32.h>
#include <cstring>
#include <set>
#include "../debug/Logger.h"
#include "../hook/CodePatch.h"

namespace FpuGuard {
namespace {
uint32_t gCorrections = 0;
using QueryFn = LONG(WINAPI*)(HANDLE, ULONG, PVOID, ULONG, PULONG);
const QueryFn gQuery = reinterpret_cast<QueryFn>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtQueryInformationThread"));
const uintptr_t gImageLo = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
const uintptr_t gImageHi = gImageLo + reinterpret_cast<const IMAGE_NT_HEADERS*>(
                                          gImageLo + reinterpret_cast<const IMAGE_DOS_HEADER*>(gImageLo)->e_lfanew)->OptionalHeader.SizeOfImage;
std::set<DWORD> gReported;
std::set<DWORD> gForeignSeen;  // threads not started by the game, inspected once (never changed, so never suspended again)
}

void PinGameThread() {
    uint16_t cw;
    uint32_t csr;
    asm volatile("fnstcw %0" : "=m"(cw));
    asm volatile("stmxcsr %0" : "=m"(csr));
    if (cw == kX87 && (csr & kMxcsrMask) == kMxcsrControl) return;
    if (gCorrections++ < 20) Logger::Info("FPUGUARD game thread: x87cw %04X mxcsr %08X -> %04X / control %04X", cw, csr, kX87, kMxcsrControl);
    const uint16_t ncw = kX87;
    const uint32_t ncsr = (csr & ~kMxcsrMask) | kMxcsrControl;
    asm volatile("fnclex\n\tfldcw %0" : : "m"(ncw));
    asm volatile("ldmxcsr %0" : : "m"(ncsr));
}

bool gPinInSessions = false;

int PinAllThreads() {
    PinGameThread();
    CodePatch::LockSuspenders();  // not while another thread of ours suspends the others (both would stay suspended)
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snap == INVALID_HANDLE_VALUE) {
        CodePatch::UnlockSuspenders();
        return -1;
    }
    int fixed = 0;
    THREADENTRY32 te{};
    te.dwSize = sizeof te;
    for (BOOL ok = Thread32First(snap, &te); ok; ok = Thread32Next(snap, &te)) {
        if (te.th32OwnerProcessID != GetCurrentProcessId() || te.th32ThreadID == GetCurrentThreadId()) continue;
        HANDLE t = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_SET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, te.th32ThreadID);
        if (!t) continue;
        // Only threads the game started (start address in MKKE.exe: its task pool runs anim jobs) are pinned. Others
        // (audio, Steam, drivers) keep their own environment: two of them run with x87 0x037F and reset it themselves
        // (fpu_1 2026-10-18), and changing it under their code could perturb them; they are reported once.
        uintptr_t start = 0;
        if (gQuery) gQuery(t, 9 /* ThreadQuerySetWin32StartAddress */, &start, sizeof start, nullptr);
        const bool gameThread = start >= gImageLo && start < gImageHi;
        // Inspected already: do not suspend it again (audio, driver and Steam threads were suspended every 10 s, the
        // render thread's driver work included).
        if (!gameThread && !gForeignSeen.insert(te.th32ThreadID).second) {
            CloseHandle(t);
            continue;
        }
        // Nothing that can take a lock (logging, allocation) runs while the thread is suspended: it may hold that lock.
        bool changed = false, foreignOdd = false;
        uint16_t was = 0;
        uint32_t wasCsr = 0;
        if (SuspendThread(t) != (DWORD)-1) {
            CONTEXT c{};
            c.ContextFlags = CONTEXT_FLOATING_POINT | CONTEXT_EXTENDED_REGISTERS;
            if (GetThreadContext(t, &c)) {
                uint32_t csr = 0;
                memcpy(&csr, c.ExtendedRegisters + 24, 4);  // FXSAVE layout: FCW at +0, MXCSR at +24
                uint16_t fcw = 0;
                memcpy(&fcw, c.ExtendedRegisters, 2);
                const uint16_t cw = (uint16_t)c.FloatSave.ControlWord;
                if ((cw != kX87 || fcw != kX87 || (csr & kMxcsrMask) != kMxcsrControl) && !gameThread) {
                    was = fcw;
                    wasCsr = csr;
                    foreignOdd = true;
                } else if (cw != kX87 || fcw != kX87 || (csr & kMxcsrMask) != kMxcsrControl) {
                    was = fcw;
                    wasCsr = csr;
                    c.FloatSave.ControlWord = (c.FloatSave.ControlWord & 0xFFFF0000u) | kX87;
                    memcpy(c.ExtendedRegisters, &kX87, 2);
                    csr = (csr & ~kMxcsrMask) | kMxcsrControl;
                    memcpy(c.ExtendedRegisters + 24, &csr, 4);
                    changed = SetThreadContext(t, &c) != 0;
                    (void)cw;
                }
            }
            ResumeThread(t);
        }
        CloseHandle(t);
        if (changed && fixed++ < 8) Logger::Info("FPUGUARD game thread %lu (start %08X): x87cw %04X mxcsr %08X -> canonical", te.th32ThreadID, (unsigned)start, was, wasCsr);
        if (foreignOdd && gReported.insert(te.th32ThreadID).second && gReported.size() < 16)
            Logger::Info("FPUGUARD thread %lu (start %08X, not the game's): x87cw %04X mxcsr %08X, left as is", te.th32ThreadID, (unsigned)start, was, wasCsr);
    }
    CloseHandle(snap);
    CodePatch::UnlockSuspenders();
    return fixed;
}

uint32_t Corrections() { return gCorrections; }
}  // namespace FpuGuard
