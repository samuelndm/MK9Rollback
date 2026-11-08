#include "PhysWatch.h"

#include <windows.h>

#include <cstdio>
#include <cstring>
#include <vector>

#include "../debug/Logger.h"
#include "../hook/CodePatch.h"
#include "../hook/GameLoopHook.h"

namespace {
constexpr uintptr_t kTermBodies = 0x006D2900;  // only caller 0x006DD810
// push ebp; mov ebp,esp; push ecx; push ebx; mov ebx,ecx (7 bytes, relocation-free)
const uint8_t kTermOrig[7] = {0x55, 0x8B, 0xEC, 0x51, 0x53, 0x8B, 0xD9};

struct Event {
    uint32_t gf, comp, caller;
    uint8_t kind;  // 0 rigid-body termination, 1 DestroyActor outside the MKObj destructor
};
std::vector<Event> gEvents;  // protected by gLock
CRITICAL_SECTION gLock;
bool gLockInit = false, gInstalled = false;
volatile uint32_t gFrame = 0;
}  // namespace

extern "C" {
void* gMK9R_PhysTermTramp = nullptr;
void __cdecl MK9R_OnPhysTerm(uint32_t comp, uint32_t caller) {
    EnterCriticalSection(&gLock);
    gEvents.push_back({gFrame, comp, caller, 0});
    if (gEvents.size() > 4096) gEvents.erase(gEvents.begin(), gEvents.begin() + 2048);
    LeaveCriticalSection(&gLock);
    Logger::Frame("PHYS term bodies comp=%08X gf=%u from=%08X", comp, gFrame, caller);
}
}

// UWorld::DestroyActor 0x0079D960 (thiscall world, actor, bNetForce, bShouldModifyLevel; ret 0xC).
// Calls from the MKObj destructor site (return 0x00439A75) are ActorDefer's; calls from outside
// MKKE.exe (our own releases) are ignored. Any other destroy during a fight tick is a UE3 actor
// death that a load cannot undo (soak13: a resurrected MKObj's +0x1D8 actor, destroyed through
// GWorld at 0x008A754F in the abandoned timeline, crashed the resimulation at 0x008A753A).
extern "C" {
void* gMK9R_DestroyActorTramp = nullptr;
void __cdecl MK9R_OnDestroyActor(uint32_t actor, uint32_t caller) {
    if (caller == 0x00439A75 || caller < 0x00401000 || caller >= 0x010AE000) return;
    // Any tick, not only fight ticks: a destroy in a non-fight tick inside the window (round
    // transition) also cannot be undone (soak18: DestroyActor through a garbage actor, 0x0078663F).
    if (!gInstalled) return;
    EnterCriticalSection(&gLock);
    gEvents.push_back({gFrame, actor, caller, 1});
    if (gEvents.size() > 4096) gEvents.erase(gEvents.begin(), gEvents.begin() + 2048);
    LeaveCriticalSection(&gLock);
    Logger::Frame("PHYS destroy actor=%08X gf=%u from=%08X", actor, gFrame, caller);
}
}  // extern "C"

extern "C" void MK9R_HkPhysTerm();
asm(R"(
    .text
    .globl _MK9R_HkPhysTerm
_MK9R_HkPhysTerm:
    pushfl
    pushal
    pushl 36(%esp)
    pushl 28(%esp)
    call _MK9R_OnPhysTerm
    addl $8, %esp
    popal
    popfl
    jmp *_gMK9R_PhysTermTramp
    .globl _MK9R_HkDestroyActorEntry
_MK9R_HkDestroyActorEntry:
    pushfl
    pushal
    pushl 36(%esp)
    pushl 44(%esp)
    call _MK9R_OnDestroyActor
    addl $8, %esp
    popal
    popfl
    jmp *_gMK9R_DestroyActorTramp
)");
extern "C" void MK9R_HkDestroyActorEntry();

namespace PhysWatch {

bool Install() {
    if (gInstalled) return true;
    if (!gLockInit) {
        InitializeCriticalSection(&gLock);
        gLockInit = true;
    }
    uint8_t* tramp = static_cast<uint8_t*>(CodePatch::AllocExec(16));
    if (!tramp) return false;
    memcpy(tramp, kTermOrig, 7);
    tramp[7] = 0xE9;
    const int32_t back = (int32_t)((kTermBodies + 7) - (reinterpret_cast<uintptr_t>(tramp) + 12));
    memcpy(tramp + 8, &back, 4);
    FlushInstructionCache(GetCurrentProcess(), tramp, 16);
    gMK9R_PhysTermTramp = tramp;
    uint8_t jmp[7] = {0xE9, 0, 0, 0, 0, 0x90, 0x90};
    const int32_t rel = (int32_t)(reinterpret_cast<uintptr_t>(&MK9R_HkPhysTerm) - (kTermBodies + 5));
    memcpy(jmp + 1, &rel, 4);
    gInstalled = CodePatch::Write(kTermBodies, kTermOrig, jmp, 7, "rigid-body term 0x006D2900 (watch)");
    if (gInstalled) {
        // push ebp; mov ebp,esp; sub esp,0xC; push ebx (7 bytes, relocation-free)
        static const uint8_t kDestroyOrig[7] = {0x55, 0x8B, 0xEC, 0x83, 0xEC, 0x0C, 0x53};
        uint8_t* t2 = static_cast<uint8_t*>(CodePatch::AllocExec(16));
        if (t2) {
            memcpy(t2, kDestroyOrig, 7);
            t2[7] = 0xE9;
            const int32_t back2 = (int32_t)((0x0079D960 + 7) - (reinterpret_cast<uintptr_t>(t2) + 12));
            memcpy(t2 + 8, &back2, 4);
            FlushInstructionCache(GetCurrentProcess(), t2, 16);
            gMK9R_DestroyActorTramp = t2;
            uint8_t j2[7] = {0xE9, 0, 0, 0, 0, 0x90, 0x90};
            const int32_t rel2 = (int32_t)(reinterpret_cast<uintptr_t>(&MK9R_HkDestroyActorEntry) - (0x0079D960 + 5));
            memcpy(j2 + 1, &rel2, 4);
            if (!CodePatch::Write(0x0079D960, kDestroyOrig, j2, 7, "UWorld::DestroyActor entry (watch)"))
                Logger::Info("PHYSWATCH: DestroyActor watch not installed");
        }
    }
    return gInstalled;
}

void SetFrame(uint32_t gf) { gFrame = gf; }

bool EventsSince(uint32_t S, char* why, size_t whyLen, bool (*referenced)(uint32_t actor)) {
    if (!gLockInit) return false;
    bool hit = false;
    EnterCriticalSection(&gLock);
    for (const Event& e : gEvents)
        if (e.gf >= S) {
            if (e.kind == 1 && referenced && !referenced(e.comp)) continue;
            if (e.kind == 1)
                snprintf(why, whyLen, "UE3 actor %08X destroyed at frame %u by %08X (not an MKObj death; cannot be undone)",
                         e.comp, e.gf, e.caller);
            else
                snprintf(why, whyLen, "rigid bodies of component %08X terminated at frame %u (PhysX state is not restorable)",
                         e.comp, e.gf);
            hit = true;
            break;
        }
    LeaveCriticalSection(&gLock);
    return hit;
}

void Trim(uint32_t oldestNeededGf) {
    if (!gLockInit) return;
    EnterCriticalSection(&gLock);
    size_t keep = 0;
    while (keep < gEvents.size() && gEvents[keep].gf < oldestNeededGf) ++keep;
    gEvents.erase(gEvents.begin(), gEvents.begin() + keep);
    LeaveCriticalSection(&gLock);
}

void DropFrom(uint32_t S) {
    if (!gLockInit) return;
    EnterCriticalSection(&gLock);
    for (size_t i = 0; i < gEvents.size();) {
        if (gEvents[i].gf >= S) gEvents.erase(gEvents.begin() + i);
        else ++i;
    }
    LeaveCriticalSection(&gLock);
}

void Clear() {
    if (!gLockInit) return;
    EnterCriticalSection(&gLock);
    gEvents.clear();
    LeaveCriticalSection(&gLock);
}

}  // namespace PhysWatch
