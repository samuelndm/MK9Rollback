#include "ScriptLinkGuard.h"

#include <windows.h>

#include <cstring>
#include <unordered_map>

#include "../debug/Logger.h"
#include "../hook/CodePatch.h"

namespace {
constexpr uintptr_t kSite = 0x0045CEF5;  // lea ecx, [edi+0xC]; dec edx; add [edi+4], edx
bool gInstalled = false;
SRWLOCK gLock = SRWLOCK_INIT;
std::unordered_map<uint32_t, uint32_t>* gDone = nullptr;  // &entry+4 -> the pointer the relocation produced
volatile uint32_t gSkips = 0, gRelocs = 0;
}  // namespace

// cdecl, replacing the add: entry = edi, delta = [module+0xA8] - 1.
extern "C" void __cdecl MK9R_ScriptReloc(uint32_t entry, uint32_t delta) {
    uint32_t* const p = reinterpret_cast<uint32_t*>(entry + 4);
    AcquireSRWLockExclusive(&gLock);
    if (!gDone) gDone = new std::unordered_map<uint32_t, uint32_t>();
    if (gDone->size() > 2000000) gDone->clear();
    auto it = gDone->find(entry + 4);
    const bool again = it != gDone->end() && it->second == *p;
    if (!again) {
        *p += delta;
        (*gDone)[entry + 4] = *p;
        ++gRelocs;
    }
    ReleaseSRWLockExclusive(&gLock);
    if (again) {
        const uint32_t n = ++gSkips;
        if (n <= 5 || n % 1000 == 0)
            Logger::Info("SCRIPTLINK: import entry %08X already relocated (+4 = %08X, base delta %08X): not added again (%u skip(s))",
                         entry, *p, delta, n);
    }
}

extern "C" void MK9R_HkScriptReloc();
asm(R"(
    .text
    .globl _MK9R_HkScriptReloc
_MK9R_HkScriptReloc:
    decl %edx
    pushl %eax
    pushl %edx
    pushl %edx
    pushl %edi
    call _MK9R_ScriptReloc
    addl $8, %esp
    popl %edx
    popl %eax
    leal 0xc(%edi), %ecx
    ret
)");

namespace ScriptLinkGuard {

bool Install() {
    if (gInstalled) return true;
    static const uint8_t kOrig[7] = {0x8D, 0x4F, 0x0C, 0x4A, 0x01, 0x57, 0x04};
    uint8_t code[7] = {0xE8, 0, 0, 0, 0, 0x90, 0x90};
    const int32_t rel = (int32_t)(reinterpret_cast<uintptr_t>(&MK9R_HkScriptReloc) - (kSite + 5));
    memcpy(code + 1, &rel, 4);
    gInstalled = CodePatch::Write(kSite, kOrig, code, 7, "script module link import relocation (idempotent)");
    if (!gInstalled) Logger::Info("SCRIPTLINK: 0x0045CEF5 not as expected; a relink after a load can still double a relocation");
    return gInstalled;
}

uint32_t Skips() { return gSkips; }

}  // namespace ScriptLinkGuard
