#include "Ue3Refs.h"
#include "../common/MemQuery.h"

#include <windows.h>

#include <cstring>
#include <unordered_map>

#include "../debug/CrashLog.h"
#include "../debug/Logger.h"
#include "CodePatch.h"
#include "HookManager.h"

#include <cstdio>
#include <vector>

namespace {
constexpr uintptr_t kCtorA = 0x005DA820;    // cdecl InternalConstructor(void* mem)
constexpr uint32_t kVtableA = 0x00D3EAC0;
constexpr uint32_t kListHeadA = 0xB4;   // doubly linked list (head +0xB4, tail +0xB8, count +0xBC) of nodes {MK object +0, next +4, prev +8} (remove: 0x007613C0)
constexpr uint32_t kListCountA = 0xBC;  // node count (teardown loops while != 0)
// push ebp; mov ebp,esp; push esi; mov esi,[ebp+8]  (7 bytes, relocation-free)
const uint8_t kCtorAOrig[7] = {0x55, 0x8B, 0xEC, 0x56, 0x8B, 0x75, 0x08};

CRITICAL_SECTION gLock;
bool gLockInit = false;
std::unordered_map<uint32_t, uint32_t> gInstances;  // obj -> GameFrame at construction; protected by gLock
bool gInstalled = false;
// Class-A teardowns (vtable slot 10 at 0x00D3EAE8 -> 0x00762390, thiscall, plain ret) with the
// GameFrame they happened at: a destroyed UE3 object cannot be resurrected by a load.
struct Teardown {
    uint32_t obj, gf, bornGf;
};
std::vector<Teardown> gTeardowns;  // protected by gLock
using TeardownFn = void(__thiscall*)(void* self);
constexpr uintptr_t kTeardownSlotA = 0x00D3EAE8;
constexpr uintptr_t kTeardownA = 0x00762390;

uint32_t CurrentGf() {
    const uint32_t gi = *reinterpret_cast<const uint32_t*>(0x00DEF4B4);
    return gi ? *reinterpret_cast<const uint32_t*>(gi + 0x70) : 0;
}

void __fastcall HkTeardownA(void* self, void* /*edx*/) {
    const uint32_t obj = reinterpret_cast<uint32_t>(self);
    EnterCriticalSection(&gLock);
    auto it = gInstances.find(obj);
    const uint32_t born = it != gInstances.end() ? it->second : 0;
    gTeardowns.push_back({obj, CurrentGf(), born});
    if (gTeardowns.size() > 4096) gTeardowns.erase(gTeardowns.begin(), gTeardowns.begin() + 2048);
    if (it != gInstances.end()) gInstances.erase(it);
    LeaveCriticalSection(&gLock);
    reinterpret_cast<TeardownFn>(kTeardownA)(self);
}

uint32_t Read32(uintptr_t a) { return *reinterpret_cast<const uint32_t*>(a); }

// Cached inside a snapshot save/load (MemQuery::Scope, game thread); plain VirtualQuery otherwise.
bool Readable(uintptr_t p, size_t n) { return MemQuery::Readable(p, n); }
// A fault-recovering read (no syscall; FixDangling ran a VirtualQuery per instance and per list node, ~2 ms of every
// load, 2026-11-04); VirtualQuery as before without the crash handler.
bool SafeR(uint32_t a, uint32_t& v) {
    if (a < 0x10000) return false;
    if (CrashLog::SafeRead32(a, &v)) return true;
    uint32_t probe = 0;
    if (CrashLog::SafeRead32(0x00E7C0EC, &probe) || !Readable(a, 4)) return false;  // handler present: a real fault
    v = Read32(a);
    return true;
}

// A live MK object: vtable in the image whose vt[0] is "mov eax, desc; ret", id != -1.
bool ValidMkObject(uint32_t obj) {
    uint32_t vt = 0, id = 0;
    if (!SafeR(obj, vt) || !SafeR(obj + 4, id)) return false;
    if (vt < 0x00C40000 || vt >= 0x00DA0000) return false;
    const uint8_t* fn = reinterpret_cast<const uint8_t*>(Read32(vt));
    if (reinterpret_cast<uintptr_t>(fn) < 0x00401000 || reinterpret_cast<uintptr_t>(fn) >= 0x00F9C000) return false;
    return fn[0] == 0xB8 && fn[5] == 0xC3 && id != 0xFFFFFFFF;
}
}  // namespace

extern "C" {
void* gMK9R_CtorATramp = nullptr;
void __cdecl MK9R_OnCtorA(uint32_t mem) {
    if (!mem) return;
    const uint32_t gi = *reinterpret_cast<const uint32_t*>(0x00DEF4B4);
    const uint32_t gf = gi ? *reinterpret_cast<const uint32_t*>(gi + 0x70) : 0;
    EnterCriticalSection(&gLock);
    gInstances[mem] = gf;
    LeaveCriticalSection(&gLock);
}
}

#if defined(_MSC_VER) && !defined(__clang__)
__declspec(naked) static void HkCtorA() {
    __asm {
        pushfd
        pushad
        push dword ptr [esp+40]  // arg0 (mem): ret addr at +36 after pushfd/pushad
        call MK9R_OnCtorA
        add esp, 4
        popad
        popfd
        jmp dword ptr [gMK9R_CtorATramp]
    }
}
#else
extern "C" void MK9R_HkCtorA();
asm(R"(
    .text
    .globl _MK9R_HkCtorA
_MK9R_HkCtorA:
    pushfl
    pushal
    pushl 40(%esp)
    call _MK9R_OnCtorA
    addl $4, %esp
    popal
    popfl
    jmp *_gMK9R_CtorATramp
)");
static void (*const HkCtorA)() = MK9R_HkCtorA;
#endif

namespace Ue3Refs {

bool Install() {
    if (gInstalled) return true;
    if (!gLockInit) {
        InitializeCriticalSection(&gLock);
        gLockInit = true;
    }
    uint8_t* tramp = static_cast<uint8_t*>(CodePatch::AllocExec(16));
    if (!tramp) return false;
    memcpy(tramp, kCtorAOrig, 7);
    tramp[7] = 0xE9;
    const int32_t back = (int32_t)((kCtorA + 7) - (reinterpret_cast<uintptr_t>(tramp) + 12));
    memcpy(tramp + 8, &back, 4);
    FlushInstructionCache(GetCurrentProcess(), tramp, 16);
    gMK9R_CtorATramp = tramp;
    uint8_t jmp[7] = {0xE9, 0, 0, 0, 0, 0x90, 0x90};
    const int32_t rel = (int32_t)(reinterpret_cast<uintptr_t>(HkCtorA) - (kCtorA + 5));
    memcpy(jmp + 1, &rel, 4);
    gInstalled = CodePatch::Write(kCtorA, kCtorAOrig, jmp, 7, "UE3 class A InternalConstructor");
    if (gInstalled) {
        // Instances constructed before this hook (injection or rollback mode enabled mid-fight)
        // are found through the MK objects that point at them: owner at MK object +0x80
        // (0x004CFEB5 unlinks the object from that owner's list in its destructor).
        int found = 0;
        EnterCriticalSection(&gLock);
        int guard = 0;
        for (uint32_t node = Read32(0x00DE8214); node && guard < 20000 && Readable(node, 0x1C);
             node = Read32(node + 0xC), ++guard) {
            const uint32_t obj = Read32(node + 4);
            if (Read32(node + 0x18) || !ValidMkObject(obj) || !Readable(obj + 0x80, 4)) continue;
            const uint32_t owner = Read32(obj + 0x80);
            if (owner && !(owner & 3) && Readable(owner, 0xCC) && Read32(owner) == kVtableA &&
                gInstances.emplace(owner, 0).second)
                ++found;
        }
        LeaveCriticalSection(&gLock);
        Logger::Info("UE3REFS: %d pre-existing class-A instances found through MK object owners", found);
    }
    if (gInstalled &&
        !HookManager::PatchPointer(kTeardownSlotA, kTeardownA, reinterpret_cast<void*>(&HkTeardownA), "UE3 class A teardown@vt+0x28"))
        Logger::Info("UE3REFS: teardown hook failed (rollbacks across class-A teardowns not detected)");
    return gInstalled;
}

void Uninstall() { gInstalled = false; }  // CodePatch::RestoreAll() restores the bytes

void CollectCells(std::vector<std::pair<uintptr_t, uint32_t>>& out) {
    if (!gLockInit) return;
    // Readability by fault-recovering reads of each range's first and last dword (rc25 save audit): the per-walk
    // VirtualQuery cache held 16 regions and stopped growing, so every list node outside them asked VirtualQuery
    // (~3-20 us each in this process: the call walks the page table of its multi-MB region), 0.9 ms of a save on a
    // prop-heavy stage (699 cells). The ranges (0xCC-byte objects, 12-byte nodes) are GMalloc blocks, so "first and last
    // dword readable" is "committed and accessible" as before; FixDangling below reads them the same way.
    // Without the crash handler SafeR falls back to MemQuery (VirtualQuery) as before.
    auto Readable = [](uintptr_t p, size_t n) {
        uint32_t v = 0;
        return p >= 0x10000 && SafeR((uint32_t)p, v) && SafeR((uint32_t)((p + n - 1) & ~uintptr_t(3)), v);
    };
    EnterCriticalSection(&gLock);
    for (auto it = gInstances.begin(); it != gInstances.end();) {
        const uint32_t obj = it->first;
        if (!Readable(obj, 0xCC) || Read32(obj) != kVtableA) {
            it = gInstances.erase(it);  // destroyed (or memory reused)
            continue;
        }
        // List header (head, tail, count) and every node {value, next, prev} (0x765F40 walks it,
        // 0x007613C0 unlinks through next/prev).
        out.emplace_back(obj + kListHeadA, 12);
        int guard = 0;
        for (uint32_t node = Read32(obj + kListHeadA); node && guard < 4096 && Readable(node, 12);
             node = Read32(node + 4), ++guard)
            out.emplace_back(node, 12);
        ++it;
    }
    LeaveCriticalSection(&gLock);
}

int FixDangling(uint32_t S) {
    if (!gLockInit) return 0;
    int fixed = 0;
    EnterCriticalSection(&gLock);
    for (auto& [obj, bornGf] : gInstances) {
        uint32_t vt = 0, last = 0;  // [obj, obj+0xCC) readable: its first and last dwords (at most two pages)
        if (!SafeR(obj, vt) || vt != kVtableA || !SafeR(obj + 0xC8, last)) continue;
        uint32_t* head = reinterpret_cast<uint32_t*>(obj + kListHeadA);
        uint32_t* count = reinterpret_cast<uint32_t*>(obj + kListCountA);
        if (*count == 0) continue;
        if (bornGf >= S) {
            // Abandoned-timeline object: its MK partners do not exist in the restored state.
            // A zero count keeps its teardown loop (0x00762390) from deleting anything.
            *count = 0;
            ++fixed;
            continue;
        }
        // Unlink nodes whose value is not a valid live MK object (each MK object's destructor
        // unlinks itself via 0x00765F40; a node for a vanished object would never go away).
        // Relink the survivors keeping next/prev/tail consistent (removed nodes leak; they may be
        // in the free quarantine).
        uint32_t* tail = reinterpret_cast<uint32_t*>(obj + kListHeadA + 4);
        uint32_t removed = 0, kept = 0, prevKept = 0;
        int guard = 0;
        uint32_t nv = 0, np = 0;
        for (uint32_t node = *head; node && guard++ < 4096 && SafeR(node, nv) && SafeR(node + 8, np);) {
            const uint32_t next = Read32(node + 4);
            const uint32_t value = nv;
            if (value && !ValidMkObject(value)) {
                ++removed;
            } else {
                if (prevKept) *reinterpret_cast<uint32_t*>(prevKept + 4) = node;
                else *head = node;
                *reinterpret_cast<uint32_t*>(node + 8) = prevKept;
                prevKept = node;
                ++kept;
            }
            node = next;
        }
        if (removed) {
            if (prevKept) *reinterpret_cast<uint32_t*>(prevKept + 4) = 0;
            else *head = 0;
            *tail = prevKept;
            *count = kept;
            ++fixed;
        }
    }
    LeaveCriticalSection(&gLock);
    return fixed;
}
bool TornDownSince(uint32_t S, char* why, size_t whyLen) {
    if (!gLockInit) return false;
    bool hit = false;
    EnterCriticalSection(&gLock);
    for (const Teardown& t : gTeardowns) {
        if (t.gf >= S && t.bornGf < S) {
            snprintf(why, whyLen, "UE3 class-A object %08X (holds MK objects) torn down at frame %u", t.obj, t.gf);
            hit = true;
            break;
        }
    }
    LeaveCriticalSection(&gLock);
    return hit;
}

void OnTimelineRestart() {
    if (!gLockInit) return;
    EnterCriticalSection(&gLock);
    for (auto& kv : gInstances) kv.second = 0;  // older than any snapshot of the new timeline
    gTeardowns.clear();
    LeaveCriticalSection(&gLock);
}

uint32_t TrackedCount() {
    if (!gLockInit) return 0;
    EnterCriticalSection(&gLock);
    const uint32_t n = (uint32_t)gInstances.size();
    LeaveCriticalSection(&gLock);
    return n;
}

}  // namespace Ue3Refs





