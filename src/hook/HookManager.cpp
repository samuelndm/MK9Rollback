#include "HookManager.h"

#include <windows.h>

#include <vector>

#include "../debug/Logger.h"

namespace {
struct Patch {
    const char* name;
    uintptr_t address;   // aligned 4-byte location written
    uint32_t original;   // value before patching
    uint32_t installed;  // value we wrote
};

std::vector<Patch> gPatches;

bool AtomicWrite32(uintptr_t address, uint32_t value) {
    DWORD old = 0;
    if (!VirtualProtect(reinterpret_cast<void*>(address), 4, PAGE_EXECUTE_READWRITE, &old)) return false;
    InterlockedExchange(reinterpret_cast<volatile LONG*>(address), (LONG)value);
    DWORD ignored = 0;
    VirtualProtect(reinterpret_cast<void*>(address), 4, old, &ignored);
    FlushInstructionCache(GetCurrentProcess(), reinterpret_cast<void*>(address), 4);
    return true;
}
}  // namespace

namespace HookManager {

bool PatchCall(uintptr_t callSite, uintptr_t expectedTarget, void* newTarget, const char* name) {
    const uint8_t* p = reinterpret_cast<const uint8_t*>(callSite);
    const uintptr_t relAddr = callSite + 1;
    if (p[0] != 0xE8) {
        Logger::Info("HOOK %s: refused, byte at 0x%08X is %02X, not E8", name, callSite, p[0]);
        return false;
    }
    if (relAddr & 3) {
        Logger::Info("HOOK %s: refused, rel32 at 0x%08X is not 4-byte aligned", name, relAddr);
        return false;
    }
    const uint32_t original = *reinterpret_cast<const uint32_t*>(relAddr);
    const uintptr_t currentTarget = callSite + 5 + (int32_t)original;
    if (currentTarget != expectedTarget) {
        Logger::Info("HOOK %s: refused, call target 0x%08X != expected 0x%08X", name, currentTarget, expectedTarget);
        return false;
    }
    const uint32_t installed = (uint32_t)(reinterpret_cast<uintptr_t>(newTarget) - (callSite + 5));
    if (!AtomicWrite32(relAddr, installed)) {
        Logger::Info("HOOK %s: VirtualProtect failed (%lu)", name, GetLastError());
        return false;
    }
    gPatches.push_back({name, relAddr, original, installed});
    Logger::Info("HOOK %s: CALL 0x%08X -> 0x%08X (was 0x%08X)", name, callSite,
                 (uintptr_t)newTarget, expectedTarget);
    return true;
}

bool PatchPointer(uintptr_t slot, uintptr_t expectedValue, void* newValue, const char* name) {
    if (slot & 3) {
        Logger::Info("HOOK %s: refused, slot 0x%08X not aligned", name, slot);
        return false;
    }
    const uint32_t original = *reinterpret_cast<const uint32_t*>(slot);
    if (original != expectedValue) {
        Logger::Info("HOOK %s: refused, slot 0x%08X holds 0x%08X != expected 0x%08X", name, slot, original,
                     expectedValue);
        return false;
    }
    const uint32_t installed = (uint32_t)reinterpret_cast<uintptr_t>(newValue);
    if (!AtomicWrite32(slot, installed)) {
        Logger::Info("HOOK %s: VirtualProtect failed (%lu)", name, GetLastError());
        return false;
    }
    gPatches.push_back({name, slot, original, installed});
    Logger::Info("HOOK %s: slot 0x%08X 0x%08X -> 0x%08X", name, slot, original, installed);
    return true;
}

bool RemoveAll() {
    bool ok = true;
    for (auto it = gPatches.rbegin(); it != gPatches.rend(); ++it) {
        const uint32_t now = *reinterpret_cast<const uint32_t*>(it->address);
        if (now != it->installed) {
            Logger::Info("UNHOOK %s: 0x%08X holds 0x%08X, expected our 0x%08X; restoring anyway", it->name,
                         it->address, now, it->installed);
            ok = false;
        }
        if (!AtomicWrite32(it->address, it->original)) {
            Logger::Info("UNHOOK %s: restore FAILED", it->name);
            ok = false;
            continue;
        }
        Logger::Info("UNHOOK %s: restored 0x%08X = 0x%08X", it->name, it->address, it->original);
    }
    gPatches.clear();
    return ok;
}

}  // namespace HookManager
