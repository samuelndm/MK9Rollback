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

// The import table's page is read-only: writable for the one store (it stays readable, so a thread calling through the
// pointer meanwhile reads the old or the new value).
bool Store(uint32_t* slot, uint32_t value) {
    DWORD old = 0;
    if (!VirtualProtect(slot, sizeof *slot, PAGE_READWRITE, &old)) return false;
    InterlockedExchange(reinterpret_cast<volatile LONG*>(slot), (LONG)value);
    VirtualProtect(slot, sizeof *slot, old, &old);
    return true;
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
    // The game's pointer to it: MKKE imports XInputSetState by ordinal (XINPUT1_3.dll #3) and calls it through the
    // import thunk `jmp [0x00C5581C]`, so the pointer is its import table entry, in the read-only .rdata. Until rc30
    // only the writable sections were searched: every log said "0 copies ... not redirected" and replayed frames
    // made real XInputSetState calls (a driver call each). Searched now: the import address table and, as before,
    // the writable data (a copy the game might keep). Exactly one must be found.
    const uint8_t* base = reinterpret_cast<const uint8_t*>(0x00400000);
    const IMAGE_NT_HEADERS32* nt =
        reinterpret_cast<const IMAGE_NT_HEADERS32*>(base + reinterpret_cast<const IMAGE_DOS_HEADER*>(base)->e_lfanew);
    const IMAGE_SECTION_HEADER* sec = IMAGE_FIRST_SECTION(nt);
    int found = 0;
    uint32_t* slot = nullptr;
    auto scan = [&](uint32_t rva, uint32_t bytes) {
        uint32_t* p = reinterpret_cast<uint32_t*>(0x00400000 + rva);
        for (uint32_t k = 0; k < bytes / 4; ++k)
            if (p[k] == real) {
                if (!slot) slot = &p[k];
                ++found;
            }
    };
    const IMAGE_DATA_DIRECTORY& iat = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IAT];
    if (nt->OptionalHeader.NumberOfRvaAndSizes > IMAGE_DIRECTORY_ENTRY_IAT && iat.VirtualAddress && iat.Size <= 0x10000)
        scan(iat.VirtualAddress, iat.Size);
    for (int i = 0; i < nt->FileHeader.NumberOfSections; ++i)
        if (sec[i].Characteristics & IMAGE_SCN_MEM_WRITE) scan(sec[i].VirtualAddress, sec[i].Misc.VirtualSize);
    if (found != 1) {
        Logger::Info("RUMBLE: %d copies of XInputSetState (%08X) in MKKE's import table and data; not redirected", found, real);
        return false;
    }
    if (!gLockInit) {
        InitializeCriticalSection(&gLock);
        gLockInit = true;
    }
    gReal = reinterpret_cast<SetStateFn>(real);
    if (!Store(slot, (uint32_t)(uintptr_t)&HkSetState)) {
        Logger::Info("RUMBLE: the pointer at %p could not be written (error %lu); not redirected", (void*)slot, GetLastError());
        return false;
    }
    gSlot = slot;
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

void Uninstall() {  // MK9R_Unload: the game's pointer must not stay on this module's code
    if (!gSlot) return;
    Store(gSlot, (uint32_t)(uintptr_t)gReal);
    gSlot = nullptr;
}

}  // namespace Rumble
