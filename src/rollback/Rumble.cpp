#include "Rumble.h"

#include <windows.h>

#include <cstdint>

#include "../debug/Logger.h"
#include "RollbackManager.h"

namespace {
struct Vibration {
    WORD left, right;  // XINPUT_VIBRATION
};
using SetStateFn = DWORD(WINAPI*)(DWORD user, Vibration* v);

SetStateFn gReal = nullptr;
uint32_t* gSlot = nullptr;  // MKKE's stored pointer to XInputSetState
bool gPending[4] = {};
Vibration gLast[4] = {};
CRITICAL_SECTION gLock;
bool gLockInit = false;

DWORD WINAPI HkSetState(DWORD user, Vibration* v) {
    if (Rollback::gIsResimulating && user < 4 && v) {
        EnterCriticalSection(&gLock);
        gLast[user] = *v;
        gPending[user] = true;
        LeaveCriticalSection(&gLock);
        return ERROR_SUCCESS;
    }
    return gReal(user, v);
}
}  // namespace

namespace Rumble {

bool Install() {
    if (gSlot) return true;
    HMODULE x = GetModuleHandleW(L"xinput1_3.dll");
    if (!x) x = GetModuleHandleW(L"xinput1_4.dll");
    if (!x) x = GetModuleHandleW(L"xinput9_1_0.dll");
    if (!x) {
        Logger::Info("RUMBLE: no XInput module loaded; nothing to redirect");
        return false;
    }
    const uint32_t real = (uint32_t)(uintptr_t)GetProcAddress(x, "XInputSetState");
    if (!real) return false;
    // The game's own copy of the pointer: a dword equal to it in MKKE's writable data.
    const uint8_t* base = reinterpret_cast<const uint8_t*>(0x00400000);
    const IMAGE_NT_HEADERS32* nt =
        reinterpret_cast<const IMAGE_NT_HEADERS32*>(base + reinterpret_cast<const IMAGE_DOS_HEADER*>(base)->e_lfanew);
    const IMAGE_SECTION_HEADER* sec = IMAGE_FIRST_SECTION(nt);
    int found = 0;
    for (int i = 0; i < nt->FileHeader.NumberOfSections; ++i) {
        if (!(sec[i].Characteristics & IMAGE_SCN_MEM_WRITE)) continue;
        uint32_t* p = reinterpret_cast<uint32_t*>(0x00400000 + sec[i].VirtualAddress);
        const uint32_t n = sec[i].Misc.VirtualSize / 4;
        for (uint32_t k = 0; k < n; ++k)
            if (p[k] == real) {
                if (!gSlot) gSlot = &p[k];
                ++found;
            }
    }
    if (found != 1) {
        Logger::Info("RUMBLE: %d copies of XInputSetState (%08X) in MKKE's data; not redirected", found, real);
        gSlot = nullptr;
        return false;
    }
    if (!gLockInit) {
        InitializeCriticalSection(&gLock);
        gLockInit = true;
    }
    gReal = reinterpret_cast<SetStateFn>(real);
    InterlockedExchange(reinterpret_cast<volatile LONG*>(gSlot), (LONG)(uintptr_t)&HkSetState);
    Logger::Info("RUMBLE: XInputSetState pointer at %p redirected (resimulated frames do not rumble)", (void*)gSlot);
    return true;
}

void EndResim() {
    if (!gSlot || !gLockInit) return;
    for (DWORD u = 0; u < 4; ++u) {
        EnterCriticalSection(&gLock);
        const bool send = gPending[u];
        Vibration v = gLast[u];
        gPending[u] = false;
        LeaveCriticalSection(&gLock);
        if (send) gReal(u, &v);
    }
}

void Uninstall() {
    if (!gSlot) return;
    InterlockedExchange(reinterpret_cast<volatile LONG*>(gSlot), (LONG)(uintptr_t)gReal);
    gSlot = nullptr;
}

}  // namespace Rumble
