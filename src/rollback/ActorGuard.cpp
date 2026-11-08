#include "ActorGuard.h"

#include <windows.h>

#include <cstring>

#include "../debug/CrashLog.h"
#include "../debug/Logger.h"
#include "../hook/CodePatch.h"
#include "../hook/ObjectHooks.h"
#include "ActorDefer.h"
#include "RollbackManager.h"

namespace {
constexpr uintptr_t kGObjObjects = 0x00E7C0EC, kGObjCount = 0x00E7C0F0;
bool gInstalled = false;
volatile uint32_t gHits = 0;

uint32_t GameFrame() {
    uint32_t p = 0, gf = 0;
    if (!CrashLog::SafeRead32(0x00DEF4B4, &p) || !CrashLog::SafeRead32(p + 0x70, &gf)) return 0;
    return gf;
}

bool Readable(uintptr_t p, size_t n) {
    MEMORY_BASIC_INFORMATION mbi;
    if (p < 0x10000 || !VirtualQuery(reinterpret_cast<void*>(p), &mbi, sizeof mbi)) return false;
    if (mbi.State != MEM_COMMIT || (mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS))) return false;
    return p + n <= reinterpret_cast<uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
}

bool Peek32(uintptr_t a, uint32_t* v) {
    if (a < 0x10000) return false;
    if (CrashLog::SafeReadAvailable()) return CrashLog::SafeRead32(a, v);
    if (!Readable(a, 4)) return false;
    *v = *reinterpret_cast<const uint32_t*>(a);
    return true;
}

// Registered at its GObjObjects slot: the collector's ~UObject clears the slot (0x005F8850).
bool LiveUObject(uint32_t o) {
    uint32_t idx = 0, objs = 0, cnt = 0, slot = 0;
    if (!Peek32(o + 4, &idx) || !Peek32(kGObjObjects, &objs) || !Peek32(kGObjCount, &cnt)) return false;
    return objs && idx < cnt && Peek32(objs + 4 * idx, &slot) && slot == o;
}

uint32_t Vt(uint32_t o) {
    uint32_t v = 0;
    Peek32(o, &v);
    return v;
}

void Report(const char* where, uint32_t obj, uint32_t actor, uint32_t comp) {
    const uint32_t n = ++gHits;
    if (n > 20 && n % 100) return;
    uint32_t flags = 0, id = 0, owns = 0, ref[3] = {0, 0, 0};
    Peek32(actor + 0x80, &flags);
    Peek32(obj + 4, &id);
    Peek32(obj + 0x2C, &owns);
    for (int k = 0; k < 3; ++k) Peek32(actor + 0x9C + 4 * k, &ref[k]);
    // Seen 2026-11-04: the dying object deleted by a purged actor's +0x9C handle (garbage collection 0x007AF2FC ->
    // 0x005F2220 -> 0x00516E70 -> actor dtor 0x00408160 -> handle 0x0042D810), its component destructed earlier in the
    // same purge.
    Logger::Info("ACTORGUARD %s (#%u) gf=%u%s: MK object %08X (vt %08X id %08X, owns actor %u, born gf %u, deferred %d) "
                 "actor %08X (vt %08X, %s, +0x80 %08X, +0x9C {%08X %08X %08X})%s",
                 where, n, GameFrame(), Rollback::gIsResimulating ? " resim" : "", obj, Vt(obj), id, owns,
                 ObjectHooks::BirthGf(obj), ActorDefer::Deferred(obj, id) ? 1 : 0, actor, Vt(actor),
                 LiveUObject(actor) ? "live" : "collected", flags, ref[0], ref[1], ref[2],
                 comp ? "" : "; skipped the un-root, DestroyActor and back-reference reset");
    if (comp)
        Logger::Info("ACTORGUARD   component +0x1CC %08X (vt %08X, %s): no brightness change or reattach", comp, Vt(comp),
                     LiveUObject(comp) ? "live" : "collected");
}
}  // namespace

// cdecl, from the light destructor 0x0043D176 (replaces mov eax, [eax+0x1CC]): the component, or 0 when it or its
// actor was collected.
extern "C" uint32_t __cdecl MK9R_LightCompCheck(uint32_t obj, uint32_t actor) {
    uint32_t comp = 0;
    if (!Peek32(actor + 0x1CC, &comp)) return 0;
    if (!comp || (LiveUObject(comp) && LiveUObject(actor))) return comp;
    Report("light destructor", obj, actor, comp);
    return 0;
}

// cdecl, from the MKObj base destructor 0x00439A36 (replaces mov ecx, [esi+8]; test ecx, ecx): the actor, or 0 when it
// was collected.
extern "C" uint32_t __cdecl MK9R_MkObjActorCheck(uint32_t obj, uint32_t actor) {
    if (LiveUObject(actor)) return actor;
    Report("MK object destructor", obj, actor, 0);
    return 0;
}

extern "C" void MK9R_HkLightComp();
extern "C" void MK9R_HkMkObjActor();
asm(R"(
    .text
    .globl _MK9R_HkLightComp
_MK9R_HkLightComp:
    pushl %ecx
    pushl %edx
    pushl %eax
    pushl %esi
    call _MK9R_LightCompCheck
    addl $8, %esp
    popl %edx
    popl %ecx
    ret

    .globl _MK9R_HkMkObjActor
_MK9R_HkMkObjActor:
    movl 0x8(%esi), %ecx
    testl %ecx, %ecx
    je 1f
    pushl %eax
    pushl %edx
    pushl %ecx
    pushl %esi
    call _MK9R_MkObjActorCheck
    addl $8, %esp
    movl %eax, %ecx
    popl %edx
    popl %eax
    testl %ecx, %ecx
1:
    ret
)");

namespace {
bool CallPatch(uintptr_t at, const uint8_t* orig, size_t len, void (*to)(), const char* name) {
    uint8_t code[16];
    memset(code, 0x90, sizeof(code));
    code[0] = 0xE8;
    const int32_t rel = (int32_t)(reinterpret_cast<uintptr_t>(to) - (at + 5));
    memcpy(code + 1, &rel, 4);
    return CodePatch::Write(at, orig, code, (uint32_t)len, name);
}
}  // namespace

namespace ActorGuard {

bool Install() {
    if (gInstalled) return true;
    // mov eax, [eax+0x1CC] (test eax, eax; je 0x0043D19C follow and stay)
    static const uint8_t kLight[6] = {0x8B, 0x80, 0xCC, 0x01, 0x00, 0x00};
    // mov ecx, [esi+8]; test ecx, ecx (je 0x00439A83 follows and stays: the stub ends with the test)
    static const uint8_t kMkObj[5] = {0x8B, 0x4E, 0x08, 0x85, 0xC9};
    const bool a = CallPatch(0x0043D176, kLight, 6, &MK9R_HkLightComp, "light effect destructor component read (collected guard)");
    const bool b = CallPatch(0x00439A36, kMkObj, 5, &MK9R_HkMkObjActor, "MKObj destructor actor read (collected guard)");
    if (!a) Logger::Info("ACTORGUARD: 0x0043D176 not as expected; a collected light component can still crash a replay");
    if (!b) Logger::Info("ACTORGUARD: 0x00439A36 not as expected; a collected MK object actor can still crash a replay");
    gInstalled = a && b;
    return gInstalled;
}

uint32_t Hits() { return gHits; }

}  // namespace ActorGuard
