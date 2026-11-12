#include "ScriptRng.h"

#include <windows.h>

#include <cstdio>
#include <cstring>

#include "../common/StrAppend.h"
#include "../debug/Logger.h"
#include "../hook/CodePatch.h"

namespace {
constexpr uintptr_t kCallSite = 0x008B879C;  // in native 0x008B8790: call 0x00493C90
constexpr uintptr_t kRealRand = 0x00493C90;
constexpr uintptr_t kFloatCallSite = 0x008B7FF6;  // in native 0x008B7FD0: call 0x00887F50
constexpr uintptr_t kRealRandFloat = 0x00887F50;
constexpr uintptr_t kExecState = 0x0049FFD0;  // thiscall on the executor [0x00E39DF4]
constexpr uintptr_t kRngA = 0x00E7C054;
constexpr uintptr_t kPlayerInfo = 0x00E89D7C, kPlayerInfoStride = 0xC8C;

volatile bool gActive = false;
bool gInstalled = false;
ScriptRng::State gState = {0, 0};
uint32_t gReseeds = 0, gDraws = 0;
uint32_t gLockSeed = 0;  // 'scriptseed': every fighter change restarts the stream from this seed (two-instance tests)

uint32_t FightKey() {
    const uint32_t a = *reinterpret_cast<const uint32_t*>(kPlayerInfo + 0x20);
    const uint32_t b = *reinterpret_cast<const uint32_t*>(kPlayerInfo + kPlayerInfoStride + 0x20);
    return a ^ (b << 1) ^ 0x5C21D7u;
}
void NextSeed() {
    const uint32_t key = FightKey();
    if (key != gState.key) {
        gState.key = key;
        gState.seed = gLockSeed ? gLockSeed ^ 0x5bd1e995u : *reinterpret_cast<const uint32_t*>(kRngA) ^ 0x9E3779B9u;
        if (gReseeds++ < 20) Logger::Info("SCRIPTRNG: seeded %08X for fight key %08X", gState.seed, key);
    }
    gState.seed = gState.seed * 0x0BB38435u + 0x3619636Bu;
    if (++gDraws % 500 == 1) Logger::Info("SCRIPTRNG: %u draws", gDraws);
}
}  // namespace

extern "C" int __cdecl MK9R_ScriptRand(int n) {
    if (!gActive) return reinterpret_cast<int(__cdecl*)(int)>(kRealRand)(n);
    // 0x00493C90: an executor in state 2, 3 or 4 gets 0 without a draw.
    const uint32_t exec = *reinterpret_cast<const uint32_t*>(0x00E39DF4);
    if (exec) {
        const int st = reinterpret_cast<int(__thiscall*)(uint32_t)>(kExecState)(exec);
        if (st == 2 || st == 3 || st == 4) return 0;
    }
    NextSeed();
    // Same scaling as the original: ((seed & 0x7FFFFFFF) * n) >> 31 (64-bit multiply, n zero-extended).
    const uint64_t prod = (uint64_t)(gState.seed & 0x7FFFFFFFu) * (uint64_t)(uint32_t)n;
    return (int)(uint32_t)(prod >> 31);
}

// Script random float (native 0x008B7FD0 -> 0x00887F50, its only caller, call at 0x008B7FF6): args (lo, hi, scale,
// offset) as floats; u = 1.mantissa(seed) in [1,2), x = (u - 1) * scale * K - scale + offset, clamped to [lo, hi],
// returned on the x87 stack. K is the constant at 0x00C60C94. Every step is rounded to float as the SSE original does
// (each op on float operands in x87 extended precision, stored to float, rounds the same: 64 >= 2*24+2 bits).
extern "C" float __cdecl MK9R_ScriptRandFloat(float lo, float hi, float scale, float offset) {
    if (!gActive) return reinterpret_cast<float(__cdecl*)(float, float, float, float)>(kRealRandFloat)(lo, hi, scale, offset);
    NextSeed();
    uint32_t bits = (gState.seed & 0x7FFFFFu) | 0x3F800000u;
    float u;
    memcpy(&u, &bits, 4);
    float k;
    memcpy(&k, reinterpret_cast<const void*>(0x00C60C94), 4);
    volatile float x = u - (float)(int)u;
    x = x * scale;
    x = x * k;
    x = x - scale;
    x = x + offset;
    float r = x;
    r = r > lo ? r : lo;  // maxss x, lo (lo if unordered)
    r = r < hi ? r : hi;  // minss r, hi
    return r;
}

namespace ScriptRng {

bool Install() {
    if (gInstalled) return true;
    auto callBytes = [](uintptr_t site, uintptr_t target, uint8_t out[5]) {
        out[0] = 0xE8;
        const int32_t rel = (int32_t)(target - (site + 5));
        memcpy(out + 1, &rel, 4);
    };
    uint8_t origInt[5], origFloat[5], newInt[5], newFloat[5];
    callBytes(kCallSite, kRealRand, origInt);
    callBytes(kFloatCallSite, kRealRandFloat, origFloat);
    callBytes(kCallSite, reinterpret_cast<uintptr_t>(&MK9R_ScriptRand), newInt);
    callBytes(kFloatCallSite, reinterpret_cast<uintptr_t>(&MK9R_ScriptRandFloat), newFloat);
    // Both or neither (one stream): check both sites before patching either.
    const bool sitesOk = !memcmp(reinterpret_cast<const void*>(kCallSite), origInt, 5) && !memcmp(reinterpret_cast<const void*>(kFloatCallSite), origFloat, 5);
    gInstalled = sitesOk && CodePatch::Write(kCallSite, origInt, newInt, 5, "script RandomInt 0x008B879C (private stream)") &&
                 CodePatch::Write(kFloatCallSite, origFloat, newFloat, 5, "script RandomFloat 0x008B7FF6 (private stream)");
    if (!gInstalled) Logger::Info("SCRIPTRNG: a call site was not as expected; script random natives stay on GSRandSeed");
    return gInstalled;
}

void SetActive(bool on) {
    gActive = on && gInstalled;
    Logger::Info("SCRIPTRNG %s (%u draws, %u seeds so far)", gActive ? "on" : "off", gDraws, gReseeds);
}
bool Active() { return gActive; }
State* StatePtr() { return &gState; }
uint32_t CurrentKey() { return FightKey(); }

namespace {
uint32_t gAttractSeed = 0;
constexpr uintptr_t kRunScript = 0x00495A10;                         // cdecl (module, function)
constexpr uintptr_t kAttractCalls[2] = {0x008155F8, 0x0081561A};      // ("attract.mko", "StartAttractMode")
}  // namespace
extern "C" void __cdecl MK9R_OnAttractStart() {
    if (!gAttractSeed) return;
    // Same RNG A, RNG B and script stream in every instance when the attract script starts, so two games pick the
    // same demo fight (two-instance netplay test); the fight then gets the netplay seed as usual.
    *reinterpret_cast<uint32_t*>(kRngA) = gAttractSeed;
    *reinterpret_cast<uint32_t*>(0x00E5352C) = gAttractSeed ^ 0x9E3779B9u;
    gState.seed = gAttractSeed ^ 0x5bd1e995u;
    gState.key = FightKey();
    Logger::Info("SCRIPTSEED: attract script start, RNG A/B and script stream seeded from %08X", gAttractSeed);
}
asm(R"(
    .text
    .globl _MK9R_HkAttractStart
_MK9R_HkAttractStart:
    pushal
    pushfl
    call _MK9R_OnAttractStart
    popfl
    popal
    movl $0x00495A10, %eax
    jmp *%eax
)");
extern "C" void MK9R_HkAttractStart();

// The attract script picks each demo fight with two natives: random stage 0x008D69F0 (call 0x008D6A1E -> 0x00810530,
// table 0x00E9234C) and random character 0x008D6AB0 (call 0x008D6ADE -> 0x008105C0, roster 0x00E92358). Both draw RNG
// A, which thousands of timing-dependent idle draws (0x0080F6C5) advance between fights (rnglog, 2026-10-18). With
// 'scriptseed', RNG A is set from the seed and the pick's index within the coming fight right before each pick, so
// every instance picks the same fights from its second demo on (the index restarts at each fight start).
namespace {
uint32_t gPickIndex = 0;
uint32_t gPickVariety = 0;  // 'pickvariety N': other fighters for the same seed (only the picks' RNG A changes)
}
extern "C" void __cdecl MK9R_OnAttractPick() {
    if (!gAttractSeed) return;
    *reinterpret_cast<uint32_t*>(kRngA) = gAttractSeed + 0x9E3779B9u * ++gPickIndex + 0x7F4A7C15u * gPickVariety;
}
// The stage table: [0x00E9234C] = entries of 24 bytes, count [0x00E92350]; +0 the stage's FName index (string at
// GNames[index] + 0x10, GNames = [0x00E7C058]), +0xC nonzero when the stage can be picked. 0x00810530 retries picks
// until the name is one of the 26 at 0x00DEF5E8 (the stages random picks may use) with +0xC set, copies the name into
// its first argument and returns 1 (thiscall, two stack arguments, ret 8). The attract script's stage native is
// 0x008D6A30 -> 0x00812670 (call 0x008D6A5E), which retries 0x00810530 forever while the name is STThrone, GorosLair or
// KahnsColiseum and up to 15 times while it is in the recently-played history; 0x008D6A70 -> 0x00812540 (call
// 0x008D6A9E) is another picker, same convention. 'forcestage' lets each picker pick as usual and then overwrites the
// name it returned (the pickers' history bookkeeping stays theirs; no forced name can trap a retry loop).
extern "C" {
volatile int MK9R_ForceStageOn = 0;
}
namespace {
volatile int gForceStage = -1;
const char* StageName(uint32_t i, uint32_t* flag) {
    const uint32_t tab = *reinterpret_cast<const uint32_t*>(0x00E9234C), n = *reinterpret_cast<const uint32_t*>(0x00E92350);
    const uint32_t names = *reinterpret_cast<const uint32_t*>(0x00E7C058);
    if (!tab || !names || i >= n || IsBadReadPtr(reinterpret_cast<const void*>(tab + i * 24), 24)) return nullptr;
    const uint32_t fn = *reinterpret_cast<const uint32_t*>(tab + i * 24);
    if (flag) *flag = *reinterpret_cast<const uint32_t*>(tab + i * 24 + 0xC);
    if (IsBadReadPtr(reinterpret_cast<const void*>(names + fn * 4), 4)) return nullptr;
    const uint32_t entry = *reinterpret_cast<const uint32_t*>(names + fn * 4);
    if (!entry || IsBadReadPtr(reinterpret_cast<const void*>(entry + 0x10), 4)) return nullptr;
    return reinterpret_cast<const char*>(entry + 0x10);
}
}  // namespace
extern "C" void __cdecl MK9R_ForceStageAfter(char* out, uint32_t picker) {
    if (!MK9R_ForceStageOn || !out) return;
    const char* name = StageName((uint32_t)gForceStage, nullptr);
    if (!name) return;
    char was[64];
    snprintf(was, sizeof was, "%s", out);
    strcpy(out, name);  // as the pickers copy it
    Logger::Info("FORCESTAGE: attract pick -> stage %d %s (picker %08X had %s)", gForceStage, name, picker, was);
}
// Pick, then overwrite: called instead of a picker (thiscall, two stack arguments, ret 8; ecx untouched until the call).
#define MK9R_STAGE_AFTER(NAME, TARGET, PRE)                                                                                   \
    asm(".text\n .globl _" #NAME "\n_" #NAME ":\n" PRE " pushl 8(%esp)\n pushl 8(%esp)\n movl $" #TARGET                    \
        ", %eax\n call *%eax\n pushl %eax\n pushl $" #TARGET "\n pushl 12(%esp)\n call _MK9R_ForceStageAfter\n addl $8, %esp\n" \
        " popl %eax\n ret $8\n");
MK9R_STAGE_AFTER(MK9R_HkPickStage, 0x00810530, " pushal\n pushfl\n call _MK9R_OnAttractPick\n popfl\n popal\n")
MK9R_STAGE_AFTER(MK9R_HkPickStage2, 0x00812670, "")
MK9R_STAGE_AFTER(MK9R_HkPickStage3, 0x00812540, "")
extern "C" void MK9R_HkPickStage2();
extern "C" void MK9R_HkPickStage3();
// SetSelectedBackground 0x00811DD0 (cdecl, the stage name; "random" picks through 0x00810530 at 0x00811E7F): copies the
// name into the fight's stage buffer [0x00E89B8C]+0x3C and derives the stored FName (+0x7C) from it. The attract start
// (0x0081563C) and the script native at 0x008BB530 call it; the forced name replaces the argument before the call.
extern "C" const char* __cdecl MK9R_ForcedStageName(const char* orig) {
    // Only a fight stage ("BGND_..." or "random"): the attract flow also sets "empty_map" between demos.
    if (!orig || IsBadStringPtrA(orig, 64) || (_strnicmp(orig, "BGND_", 5) && _stricmp(orig, "random"))) return nullptr;
    const char* name = MK9R_ForceStageOn ? StageName((uint32_t)gForceStage, nullptr) : nullptr;
    if (name)
        Logger::Info("FORCESTAGE: SetSelectedBackground(%s) -> stage %d %s", orig && !IsBadStringPtrA(orig, 64) ? orig : "?", gForceStage,
                     name);
    return name;
}
asm(R"(
    .text
    .globl _MK9R_HkSetBgnd
_MK9R_HkSetBgnd:
    cmpl $0, _MK9R_ForceStageOn
    je 1f
    pushl %ecx
    pushl %edx
    pushl 12(%esp)
    call _MK9R_ForcedStageName
    addl $4, %esp
    popl %edx
    popl %ecx
    testl %eax, %eax
    je 1f
    movl %eax, 4(%esp)
1:
    movl $0x00811DD0, %eax
    jmp *%eax
)");
extern "C" void MK9R_HkSetBgnd();
asm(R"(
    .text
    .globl _MK9R_HkPickChar
_MK9R_HkPickChar:
    pushal
    pushfl
    call _MK9R_OnAttractPick
    popfl
    popal
    movl $0x008105C0, %eax
    jmp *%eax
)");
extern "C" void MK9R_HkPickStage();
extern "C" void MK9R_HkPickChar();

void OnFightStart() { gPickIndex = 0; }

void SetPickVariety(uint32_t v) {
    gPickVariety = v;
    Logger::Info("PICKVARIETY %u: attract fighter picks offset (scriptseed %08X)", v, gAttractSeed);
}

void ForceStage(int index) {
    const char* name = index >= 0 ? StageName((uint32_t)index, nullptr) : nullptr;
    if (index >= 0 && !name) {
        Logger::Info("FORCESTAGE %d: no such stage (table of %u)", index, *reinterpret_cast<const uint32_t*>(0x00E92350));
        return;
    }
    // The stage natives' picker calls (0x008D6A1E is 'scriptseed''s; they never ran in attract) and every call of
    // SetSelectedBackground 0x00811DD0, which sets the demos' stages.
    static bool sites = false;
    if (index >= 0 && !sites) {
        sites = true;
        const struct { uintptr_t site, target; void (*stub)(); } s[5] = {{0x008D6A5E, 0x00812670, &MK9R_HkPickStage2},
                                                                         {0x008D6A9E, 0x00812540, &MK9R_HkPickStage3},
                                                                         {0x008BB53C, 0x00811DD0, &MK9R_HkSetBgnd},
                                                                         {0x0081563C, 0x00811DD0, &MK9R_HkSetBgnd},
                                                                         {0x008143D7, 0x00811DD0, &MK9R_HkSetBgnd}};
        for (const auto& p : s) {
            uint8_t orig[5] = {0xE8}, call[5] = {0xE8};
            const int32_t ro = (int32_t)(p.target - (p.site + 5));
            const int32_t rn = (int32_t)(reinterpret_cast<uintptr_t>(p.stub) - (p.site + 5));
            memcpy(orig + 1, &ro, 4);
            memcpy(call + 1, &rn, 4);
            CodePatch::Write(p.site, orig, call, 5, "attract stage picker (forcestage)");
        }
    }
    gForceStage = index;
    MK9R_ForceStageOn = index >= 0;
    Logger::Info("FORCESTAGE %d%s%s%s", index, name ? " (" : " (random picks)", name ? name : "", name ? ")" : "");
}

void LogStages() {
    const uint32_t n = *reinterpret_cast<const uint32_t*>(0x00E92350);
    char line[2000];
    int k = snprintf(line, sizeof line, "STAGES %u:", n);
    for (uint32_t i = 0; i < n && i < 80 && k < (int)sizeof line - 64; ++i) {
        uint32_t flag = 0;
        const char* name = StageName(i, &flag);
        bool excluded = false;
        for (int x = 0; x < 26 && name; ++x) {
            const char* e = *reinterpret_cast<const char* const*>(0x00DEF5E8 + 4 * x);
            if (e && !IsBadReadPtr(e, 4) && !_stricmp(e, name)) excluded = true;
        }
        k = AppendF(line, sizeof line, k, " %u=%s%s%s", i, name ? name : "?", flag ? "" : "(off)", excluded ? "(x)" : "");
    }
    Logger::Info("%s", line);
}

uint32_t StageCount() { return *reinterpret_cast<const uint32_t*>(0x00E92350); }
const char* StageAt(uint32_t index) { return StageName(index, nullptr); }
int StageIndex(const char* name) {
    const uint32_t n = StageCount();
    for (uint32_t i = 0; name && i < n && i < 256; ++i) {
        const char* s = StageName(i, nullptr);
        if (s && !_strnicmp(s, name, 64)) return (int)i;
    }
    return -1;
}

void SetAttractSeed(uint32_t seed) {
    static bool hooked = false;
    if (seed && !hooked) {
        hooked = true;
        for (uintptr_t site : kAttractCalls) {
            uint8_t orig[5] = {0xE8}, call[5] = {0xE8};
            const int32_t ro = (int32_t)(kRunScript - (site + 5));
            const int32_t rn = (int32_t)(reinterpret_cast<uintptr_t>(&MK9R_HkAttractStart) - (site + 5));
            memcpy(orig + 1, &ro, 4);
            memcpy(call + 1, &rn, 4);
            if (!CodePatch::Write(site, orig, call, 5, "StartAttractMode script call (scriptseed)")) hooked = false;
        }
        const struct { uintptr_t site, target; void (*stub)(); } picks[2] = {{0x008D6A1E, 0x00810530, &MK9R_HkPickStage},
                                                                             {0x008D6ADE, 0x008105C0, &MK9R_HkPickChar}};
        for (const auto& p : picks) {
            uint8_t orig[5] = {0xE8}, call[5] = {0xE8};
            const int32_t ro = (int32_t)(p.target - (p.site + 5));
            const int32_t rn = (int32_t)(reinterpret_cast<uintptr_t>(p.stub) - (p.site + 5));
            memcpy(orig + 1, &ro, 4);
            memcpy(call + 1, &rn, 4);
            if (!CodePatch::Write(p.site, orig, call, 5, "attract random fight pick (scriptseed)")) hooked = false;
        }
    }
    gAttractSeed = seed;
    gLockSeed = seed;
    gState.key = ~FightKey();  // restart the stream at the next draw
    if (seed && Install()) gActive = true;
    Logger::Info("SCRIPTSEED %08X (0 = off)%s", seed, hooked ? "" : ": attract call sites not patched");
}

}  // namespace ScriptRng
