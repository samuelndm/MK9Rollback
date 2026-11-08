#include "HandleGuard.h"

#include <windows.h>

#include <cstring>

#include "../debug/CrashLog.h"
#include "../debug/Logger.h"
#include "../hook/CodePatch.h"

namespace {
constexpr uintptr_t kLookup = 0x00B8FFD0;
const uint8_t kOrig[6] = {0x55, 0x8B, 0xEC, 0x8B, 0x4D, 0x08};  // push ebp; mov ebp, esp; mov ecx, [ebp+8]
bool gInstalled = false;
}  // namespace

extern "C" volatile uint32_t MK9R_gHandleNullHits = 0;
extern "C" void MK9R_HandleNull(uint32_t handle) {
    if (MK9R_gHandleNullHits++ < 10) Logger::Info("HANDLEGUARD: HUD handle %u names an emptied library slot; answered 0", handle);
}

// cdecl uint32_t lookup(int handle): the original's checks (table present, handle != -1, 0 <= handle < count), then
// the entry, which may now be null.
extern "C" void MK9R_HandleLookup();
asm(R"(
    .text
    .globl _MK9R_HandleLookup
_MK9R_HandleLookup:
    movl 4(%esp), %ecx
    movl 0x00FE2004, %eax
    testl %eax, %eax
    je 1f
    cmpl $-1, %ecx
    je 1f
    testl %ecx, %ecx
    jl 1f
    cmpl 0x48(%eax), %ecx
    jge 1f
    movl 0x44(%eax), %eax
    movl (%eax,%ecx,4), %eax
    testl %eax, %eax
    je 2f
    movl 0xa4(%eax), %eax
    ret
2:
    pushl %ecx
    call _MK9R_HandleNull
    addl $4, %esp
1:
    xorl %eax, %eax
    ret
)");

extern "C" volatile uint32_t MK9R_gReleaseNullHits = 0;
extern "C" void MK9R_ReleaseNull(uint32_t table, uint32_t handle) {
    if (MK9R_gReleaseNullHits++ < 10)
        Logger::Info("HANDLEGUARD: release of HUD handle %u (table +0x%X) found the slot already emptied; skipped", handle,
                     table);
}

// cdecl void release44(int handle), 0x00B91480: the original's checks, then, with a filled slot, the original from
// 0x00B914B3 with its frame (ebp, edi = table, eax = handle) as it would have built it; an emptied slot returns.
// cdecl void release5C(int handle), 0x00B90070: the same, resuming at 0x00B900A3 with ecx = table, edi = handle.
extern "C" void MK9R_HandleRelease44();
extern "C" void MK9R_HandleRelease5C();
asm(R"(
    .text
    .globl _MK9R_HandleRelease44
_MK9R_HandleRelease44:
    movl 4(%esp), %eax
    movl 0x00FE2004, %ecx
    testl %ecx, %ecx
    je 1f
    cmpl $-1, %eax
    je 1f
    testl %eax, %eax
    jl 1f
    cmpl 0x48(%ecx), %eax
    jge 1f
    movl 0x44(%ecx), %edx
    cmpl $0, (%edx,%eax,4)
    je 2f
    pushl %ebp
    movl %esp, %ebp
    pushl %edi
    movl %ecx, %edi
    movl $0x00B914B3, %edx
    jmp *%edx
2:
    pushl %eax
    pushl $0x44
    call _MK9R_ReleaseNull
    addl $8, %esp
1:
    ret

    .globl _MK9R_HandleRelease5C
_MK9R_HandleRelease5C:
    movl 4(%esp), %eax
    movl 0x00FE2004, %ecx
    testl %ecx, %ecx
    je 1f
    cmpl $-1, %eax
    je 1f
    testl %eax, %eax
    jl 1f
    cmpl 0x60(%ecx), %eax
    jge 1f
    movl 0x5c(%ecx), %edx
    cmpl $0, (%edx,%eax,4)
    je 2f
    pushl %ebp
    movl %esp, %ebp
    pushl %edi
    movl %eax, %edi
    movl $0x00B900A3, %edx
    jmp *%edx
2:
    pushl %eax
    pushl $0x5C
    call _MK9R_ReleaseNull
    addl $8, %esp
1:
    ret
)");

extern "C" volatile uint32_t MK9R_gAnimNodeNullHits = 0;
extern "C" void MK9R_AnimNodeNull(uint32_t fighter) {
    if (MK9R_gAnimNodeNullHits++ < 10)
        Logger::Info("HANDLEGUARD: fighter %08X has no MK anim node on its UE3 tree's bone (0x0043B1A0 = 0 at 0x0044B949); "
                     "took the null-safe branch 0x0044B990", fighter);
}

// 0x0044B950 mov ecx, [esi+0x34C]; cmp ecx, [eax+0x80]; jne 0x0044B990, with eax = 0x0043B1A0(fighter): the MK anim node
// playing on the bone the UE3 anim tree names ([[comp+0x280]+0xD0] -> 0x004355A0, which returns 0 when no node of the
// MK tree plays on it). Soak 13110 (2026-10-30, gf 794 just after a load to 789) crashed here reading [0+0x80]: the
// UE3 tree is not restored by a load and named a bone the restored MK tree had no node on. A null node now takes the
// not-equal branch, whose 0x00442260 does the same lookup through 0x0043B070 and already handles null.
extern "C" void MK9R_AnimNodeCheck();
asm(R"(
    .text
    .globl _MK9R_AnimNodeCheck
_MK9R_AnimNodeCheck:
    movl 0x34c(%esi), %ecx
    testl %eax, %eax
    je 1f
    cmpl 0x80(%eax), %ecx
    jne 2f
    pushl $0x0044B95E
    ret
1:
    pushal
    pushl %esi
    call _MK9R_AnimNodeNull
    addl $4, %esp
    popal
2:
    pushl $0x0044B990
    ret
)");

extern "C" volatile uint32_t MK9R_gIkNodeNullHits = 0;
extern "C" void MK9R_IkNodeNull(uint32_t comp, uint32_t elem, uint32_t index) {
    const uint32_t n = ++MK9R_gIkNodeNullHits;
    if (n <= 10 || n % 100 == 0)
        Logger::Info("HANDLEGUARD: fighter comp %08X +0x3A8[%u] = %08X has no bound object and no node (+8 = +0xC = 0): skipped "
                     "at 0x0077D567 (%u so far)", comp, index, elem, n);
}

// The fight tick's walk of the fighter component's IK list (comp +0x3A8, 0x0077D535..0x0077D650): an element with +8 == 0
// and both weights (+0x20, +0x24) above the threshold looks its node's bone up by name, [[e+0xC]+0xA0], with no null test.
// An X-ray's CinemaTrackInstIK (element at +0x48) whose node was cleared while the element stayed listed crashed both games
// at 0x0077D56A (batch 21 xray3b, batch 22 xray4c after the rc10 load scrub: the entry was still bound at the load). The
// game never lists such an element outside rollback (it would crash there too), so it is skipped like an element whose
// weights are below the threshold: eax = index, ecx = comp at 0x0077D646.
extern "C" void MK9R_IkNodeCheck();
asm(R"(
    .text
    .globl _MK9R_IkNodeCheck
_MK9R_IkNodeCheck:
    cmpl $0, 0xc(%esi)
    je 1f
    movl 0xc(%esi), %eax
    movl 0xa0(%eax), %ecx
    pushl $0x0077D570
    ret
1:
    pushal
    pushl %eax
    pushl %esi
    pushl %ecx
    call _MK9R_IkNodeNull
    addl $12, %esp
    popal
    pushl $0x0077D646
    ret
)");

// Per-tick render data of a component class (vtable 0x00D3B5B4, tick 0x007CE0D0 -> 0x007C81F0): every tick replaces the
// 0x200-byte object at comp +0x268 (ctor 0x007041B0 stores comp at +0xFC) and queues a render command {vt 0x00D5AFA4, old,
// new} whose Execute 0x007B0A90 detaches old, attaches new and deletes old (GMalloc free returning to 0x007B0ACB) on the
// render thread. A command queued in the abandoned timeline can run after a load put old back at comp +0x268 (the cinema
// restore): old was freed while installed, the quarantine released it 20 frames later and the component's next tick read
// it (xray4b, 2026-11-08: 0x006F2D4D in a replayed EngineTick and 0x007145BD on the render thread, PitDay; batch 21's
// 0x0070451E looks the same). When old is installed again, the command is dropped: old stays, the abandoned new leaks.
extern "C" volatile LONG MK9R_gRenderDataKept = 0;
extern "C" volatile uint32_t MK9R_gRenderDataGuard = 1;
extern "C" int __cdecl MK9R_RenderDataKeep(uint32_t cmd) {
    if (!MK9R_gRenderDataGuard) return 0;
    uint32_t oldObj = 0, newObj = 0, comp = 0, cur = 0;
    if (!CrashLog::SafeRead32(cmd + 4, &oldObj) || !oldObj || !CrashLog::SafeRead32(cmd + 8, &newObj) || !newObj ||
        !CrashLog::SafeRead32(newObj + 0xFC, &comp) || !comp || !CrashLog::SafeRead32(comp + 0x268, &cur) || cur != oldObj)
        return 0;
    const LONG n = InterlockedIncrement(&MK9R_gRenderDataKept);
    if (n <= 10 || n % 100 == 0)
        Logger::Info("HANDLEGUARD: render data %08X of comp %08X is installed again (a load put it back): its delete command "
                     "(new %08X) dropped, thread %lu (%ld so far)", oldObj, comp, newObj, GetCurrentThreadId(), n);
    return 1;
}
extern "C" void MK9R_RenderDataExecute();
asm(R"(
    .text
    .globl _MK9R_RenderDataExecute
_MK9R_RenderDataExecute:
    pushl %ecx
    pushl %ecx
    call _MK9R_RenderDataKeep
    addl $4, %esp
    popl %ecx
    testl %eax, %eax
    jne 1f
    pushl %esi
    movl %ecx, %esi
    movl 0x4(%esi), %ecx
    pushl $0x007B0A96
    ret
1:
    movl $0xc, %eax
    ret
)");

// A SeqAct_Cinema's first object variable (0x007A0B78: GetObjectVars 0x00730BF0 -> [element] -> IsA(Actor) walking
// [obj+0x30] class -> +0x38 supers) read a variable whose object was no longer a UObject: stall gapB_nohold B, Graveyard
// (2026-11-08, 0x007A0BAC, [obj+0x30] = FFFFFFFF), SeqVar_Object_16 itself badindex and its object released by the
// quarantine, after a run of "now collected" loads that restored nothing. An object that is not live in GObjObjects
// (index at +4) is taken as none, which the function already handles (0x007A0BC3: no actor).
extern "C" volatile uint32_t MK9R_gObjVarDead = 0;
extern "C" int __cdecl MK9R_ObjVarLive(uint32_t obj) {
    uint32_t objs = 0, cnt = 0, idx = 0, slot = 0;
    if (!(obj & 3) && CrashLog::SafeRead32(0x00E7C0EC, &objs) && CrashLog::SafeRead32(0x00E7C0F0, &cnt) && objs &&
        CrashLog::SafeRead32(obj + 4, &idx) && idx < cnt && CrashLog::SafeRead32(objs + 4 * idx, &slot) && slot == obj)
        return 1;
    const uint32_t n = ++MK9R_gObjVarDead;
    if (n <= 10 || n % 100 == 0)
        Logger::Info("HANDLEGUARD: a cinematic action's object variable names %08X, not a live object: taken as none at 0x007A0B7E "
                     "(%u so far)", obj, n);
    return 0;
}
extern "C" void MK9R_KismetObjVar();
asm(R"(
    .text
    .globl _MK9R_KismetObjVar
_MK9R_KismetObjVar:
    movl (%edx), %ebx
    testl %ebx, %ebx
    je 2f
    pushl %eax
    pushl %ecx
    pushl %edx
    pushl %ebx
    call _MK9R_ObjVarLive
    addl $4, %esp
    popl %edx
    popl %ecx
    testl %eax, %eax
    popl %eax
    je 2f
    pushl $0x007A0B84
    ret
2:
    pushl $0x007A0BC3
    ret
)");

// A latent action's activation (0x00414694..0x004146C7) walks its Targets (+0xE0 data, +0xE4 count): a non-null target
// gets vt+0x38 (true: skipped) and otherwise the action is added to the target's LatentActions (+0x1A0); Targets is then
// emptied. Five crashes on Graveyard's stage cinematic (SeqAct_Cinema_7: net13, batch 20 x2, batch 22, batch 28 slowB)
// called vt+0x38 of an object the collector had purged (caller 0x00647FA3) and the quarantine released; each followed a
// load that found a spawned group's actor destroyed since S ("1 lost"), whose restored references outlived the actor.
// A target that is not a live UObject now counts as true (skipped), as the loop already does for a destroyed one.
extern "C" volatile uint32_t MK9R_gTargetDead = 0;
extern "C" int __cdecl MK9R_TargetLive(uint32_t obj) {
    uint32_t objs = 0, cnt = 0, idx = 0, slot = 0;
    if (!(obj & 3) && CrashLog::SafeRead32(0x00E7C0EC, &objs) && CrashLog::SafeRead32(0x00E7C0F0, &cnt) && objs &&
        CrashLog::SafeRead32(obj + 4, &idx) && idx < cnt && CrashLog::SafeRead32(objs + 4 * idx, &slot) && slot == obj)
        return 1;
    const uint32_t n = ++MK9R_gTargetDead;
    if (n <= 10 || n % 100 == 0)
        Logger::Info("HANDLEGUARD: a latent action's target %08X is not a live object: skipped at 0x004146A1 (%u so far)", obj, n);
    return 0;
}
extern "C" void MK9R_KismetTarget();
asm(R"(
    .text
    .globl _MK9R_KismetTarget
_MK9R_KismetTarget:
    pushl %edi
    call _MK9R_TargetLive
    addl $4, %esp
    testl %eax, %eax
    je 1f
    movl (%edi), %edx
    movl 0x38(%edx), %eax
    movl %edi, %ecx
    call *%eax
    pushl $0x004146AA
    ret
1:
    movl $1, %eax
    pushl $0x004146AA
    ret
)");

namespace {
bool Redirect(uintptr_t at, const uint8_t* orig, size_t len, void (*to)(), const char* name) {
    uint8_t jmp[16];
    memset(jmp, 0x90, sizeof(jmp));
    jmp[0] = 0xE9;
    const int32_t rel = (int32_t)(reinterpret_cast<uintptr_t>(to) - (at + 5));
    memcpy(jmp + 1, &rel, 4);
    return CodePatch::Write(at, orig, jmp, len, name);
}
}  // namespace

namespace HandleGuard {

bool Install() {
    if (gInstalled) return true;
    gInstalled = Redirect(kLookup, kOrig, 6, &MK9R_HandleLookup, "HUD handle lookup 0x00B8FFD0 (null-slot guard)");
    if (!gInstalled) Logger::Info("HANDLEGUARD: 0x00B8FFD0 not as expected; an emptied HUD slot can still crash a replay");
    // push ebp; mov ebp, esp; mov eax, [ebp+8]
    static const uint8_t kRel44[6] = {0x55, 0x8B, 0xEC, 0x8B, 0x45, 0x08};
    // push ebp; mov ebp, esp; mov ecx, [0x00FE2004]
    static const uint8_t kRel5C[9] = {0x55, 0x8B, 0xEC, 0x8B, 0x0D, 0x04, 0x20, 0xFE, 0x00};
    if (!Redirect(0x00B91480, kRel44, 6, &MK9R_HandleRelease44, "HUD handle release 0x00B91480 (null-slot guard)"))
        Logger::Info("HANDLEGUARD: 0x00B91480 not as expected; releasing an emptied HUD slot can still crash a replay");
    if (!Redirect(0x00B90070, kRel5C, 9, &MK9R_HandleRelease5C, "HUD handle release 0x00B90070 (null-slot guard)"))
        Logger::Info("HANDLEGUARD: 0x00B90070 not as expected; releasing an emptied HUD slot can still crash a replay");
    // mov ecx, [esi+0x34C]; cmp ecx, [eax+0x80] (jne 0x0044B990 follows at 0x0044B95C and stays)
    static const uint8_t kAnimNode[12] = {0x8B, 0x8E, 0x4C, 0x03, 0x00, 0x00, 0x3B, 0x88, 0x80, 0x00, 0x00, 0x00};
    if (!Redirect(0x0044B950, kAnimNode, 12, &MK9R_AnimNodeCheck, "fighter anim node check 0x0044B950 (null-node guard)"))
        Logger::Info("HANDLEGUARD: 0x0044B950 not as expected; a missing MK anim node can still crash a replay");
    // mov eax, [esi+0xC]; mov ecx, [eax+0xA0]
    static const uint8_t kIkNode[9] = {0x8B, 0x46, 0x0C, 0x8B, 0x88, 0xA0, 0x00, 0x00, 0x00};
    if (!Redirect(0x0077D567, kIkNode, 9, &MK9R_IkNodeCheck, "fighter IK list walk 0x0077D567 (null-node guard)"))
        Logger::Info("HANDLEGUARD: 0x0077D567 not as expected; an unbound IK list entry can still crash a fight tick");
    // push esi; mov esi, ecx; mov ecx, [esi+4]
    static const uint8_t kRenderExec[6] = {0x56, 0x8B, 0xF1, 0x8B, 0x4E, 0x04};
    if (!Redirect(0x007B0A90, kRenderExec, 6, &MK9R_RenderDataExecute, "render data delete command 0x007B0A90 (reinstalled guard)"))
        Logger::Info("HANDLEGUARD: 0x007B0A90 not as expected; an abandoned render data delete can still free restored data");
    // mov ebx, [edx]; test ebx, ebx; je 0x007A0BC3
    static const uint8_t kObjVar[6] = {0x8B, 0x1A, 0x85, 0xDB, 0x74, 0x3F};
    if (!Redirect(0x007A0B7E, kObjVar, 6, &MK9R_KismetObjVar, "cinematic action object variable 0x007A0B7E (dead-object guard)"))
        Logger::Info("HANDLEGUARD: 0x007A0B7E not as expected; a dead Kismet object variable can still crash a stage cinematic");
    // mov edx, [edi]; mov eax, [edx+0x38]; mov ecx, edi; call eax
    static const uint8_t kTarget[9] = {0x8B, 0x17, 0x8B, 0x42, 0x38, 0x8B, 0xCF, 0xFF, 0xD0};
    if (!Redirect(0x004146A1, kTarget, 9, &MK9R_KismetTarget, "latent action target loop 0x004146A1 (dead-object guard)"))
        Logger::Info("HANDLEGUARD: 0x004146A1 not as expected; a purged Kismet target can still crash a stage cinematic");
    return gInstalled;
}

uint32_t NullHits() {
    return MK9R_gHandleNullHits + MK9R_gReleaseNullHits + MK9R_gAnimNodeNullHits + MK9R_gIkNodeNullHits + MK9R_gObjVarDead +
           MK9R_gTargetDead;
}
uint32_t IkNodeSkips() { return MK9R_gIkNodeNullHits; }
uint32_t RenderDataKept() { return (uint32_t)MK9R_gRenderDataKept; }
void SetRenderDataGuard(bool on) { MK9R_gRenderDataGuard = on ? 1 : 0; }

}  // namespace HandleGuard
