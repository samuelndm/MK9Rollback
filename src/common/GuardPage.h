#pragma once
#include <windows.h>

#include <cstdint>

// A PAGE_GUARD page loses that protection at the access that raises STATUS_GUARD_PAGE_VIOLATION. A handler that
// swallows the exception for a probing read (CrashLog::SafeRead32 follows pointer-shaped values anywhere, another
// thread's stack guard included) has to put the protection back: without its guard page that thread's stack no longer
// grows, and its next deep call dies on an access violation one page further down.
namespace GuardPage {

// Puts PAGE_GUARD back on the committed page that holds addr. True when the page has it afterwards.
inline bool Rearm(uintptr_t addr) {
    MEMORY_BASIC_INFORMATION mbi;
    if (!VirtualQuery(reinterpret_cast<void*>(addr), &mbi, sizeof mbi) || mbi.State != MEM_COMMIT) return false;
    if (mbi.Protect & PAGE_GUARD) return true;
    if (mbi.Protect & PAGE_NOACCESS) return false;  // PAGE_GUARD cannot be combined with it
    DWORD old = 0;
    return VirtualProtect(reinterpret_cast<void*>(addr & ~static_cast<uintptr_t>(0xFFF)), 0x1000, mbi.Protect | PAGE_GUARD, &old) != 0;
}

}  // namespace GuardPage
