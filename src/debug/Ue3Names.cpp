#include "Ue3Names.h"

#include <windows.h>

#include <cstdio>

namespace {
bool Ok(uint32_t a, uint32_t n) {
    MEMORY_BASIC_INFORMATION mbi;
    return a >= 0x10000 && VirtualQuery(reinterpret_cast<void*>(a), &mbi, sizeof mbi) && mbi.State == MEM_COMMIT &&
           (mbi.Protect & (PAGE_READWRITE | PAGE_READONLY | PAGE_EXECUTE_READWRITE)) && !(mbi.Protect & PAGE_GUARD) &&
           a + n <= reinterpret_cast<uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
}
uint32_t R(uint32_t a) { return *reinterpret_cast<const uint32_t*>(a); }
}  // namespace

namespace Ue3Names {

void NameOf(uint32_t obj, char* out, size_t n) {
    snprintf(out, n, "?");
    if (!Ok(obj, 0x38)) return;
    const uint32_t idx = R(obj + 0x28);
    const uint32_t names = R(0x00E7C058), count = R(0x00E7C05C);
    if (!Ok(names, 4) || idx >= count || !Ok(names + 4 * idx, 4)) return;
    const uint32_t e = R(names + 4 * idx);
    if (!Ok(e + 0x10, 64)) return;
    snprintf(out, n, "%.60s_%u", reinterpret_cast<const char*>(e + 0x10), R(obj + 0x2C));
}

uint32_t FindObjectBack(uint32_t addr, uint32_t maxBack) {
    MEMORY_BASIC_INFORMATION mbi;
    if (addr < 0x10000 || !VirtualQuery(reinterpret_cast<void*>(addr), &mbi, sizeof mbi) || mbi.State != MEM_COMMIT ||
        !(mbi.Protect & (PAGE_READWRITE | PAGE_READONLY | PAGE_EXECUTE_READWRITE)) || (mbi.Protect & PAGE_GUARD))
        return 0;
    const uint32_t lo = (uint32_t)reinterpret_cast<uintptr_t>(mbi.BaseAddress);
    const uint32_t hi = lo + (uint32_t)mbi.RegionSize;
    for (uint32_t a = addr & ~3u; a + maxBack > addr && a >= lo; a -= 4) {  // one region query, not one per step
        if (a + 0x38 > hi) continue;
        const uint32_t vt = R(a);
        if (vt < 0x00C00000 || vt >= 0x00DA0000) continue;
        char t[80];
        NameOf(R(a + 0x30), t, sizeof t);
        if (t[0] == '?') continue;
        return a;
    }
    return 0;
}

void Describe(uint32_t addr, char* out, size_t n) {
    const uint32_t o = FindObjectBack(addr, 0x2000);
    if (!o) {
        snprintf(out, n, "?");
        return;
    }
    char name[80], cls[80];
    NameOf(o, name, sizeof name);
    NameOf(R(o + 0x30), cls, sizeof cls);
    snprintf(out, n, "%s:%s+0x%X", cls, name, addr - o);
}

}  // namespace Ue3Names
