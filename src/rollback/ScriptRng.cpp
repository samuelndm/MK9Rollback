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
volatile int gForceStage = -1;  // 'forcestage' (tests)
// The stage screen's agreed arena for the online match that is about to load (-1: none). It goes before the command's.
volatile int gMatchStage = -1;
int Forced() {
    const int m = gMatchStage;
    return m >= 0 ? m : gForceStage;
}
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
    const int forced = Forced();
    const char* name = StageName((uint32_t)forced, nullptr);
    if (!name) return;
    char was[64];
    snprintf(was, sizeof was, "%s", out);
    strcpy(out, name);  // as the pickers copy it
    Logger::Info("FORCESTAGE: attract pick -> stage %d %s (picker %08X had %s)", forced, name, picker, was);
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
    const int forced = Forced();
    const char* name = MK9R_ForceStageOn ? StageName((uint32_t)forced, nullptr) : nullptr;
    if (name)
        Logger::Info("FORCESTAGE: SetSelectedBackground(%s) -> stage %d %s%s", orig && !IsBadStringPtrA(orig, 64) ? orig : "?", forced, name,
                     gMatchStage >= 0 ? " (the stage screen's agreed arena)" : "");
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

// The stage natives' picker calls (0x008D6A1E is 'scriptseed''s; they never ran in attract) and every call of
// SetSelectedBackground 0x00811DD0, which sets the demos' stages and the stage a script names (natives 671 and 672).
// While nothing is forced the stubs go straight on to the game's functions.
void InstallStageSites() {
    static bool sites = false;
    if (sites) return;
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

bool SetMatchStage(int index) {
    if (index >= 0 && !StageName((uint32_t)index, nullptr)) return false;
    if (index >= 0) InstallStageSites();
    gMatchStage = index >= 0 ? index : -1;
    MK9R_ForceStageOn = Forced() >= 0;
    return true;
}
int MatchStage() { return gMatchStage; }

// SetSelectedBackground 0x00811DD0 as a script's stage native calls it (cdecl, the name). Read in the disassembly: it
// prints a debug line, copies the name into the fight's stage buffer [0x00E89B8C]+0x3C (64 bytes), stores the name's
// FName at +0x7C / +0x80 and sets +0x8D8 to 1; it starts no load. It reads [0x00E89B8C] without a check.
bool WriteStage(uint32_t index) {
    const char* name = StageName(index, nullptr);
    if (!name || !*reinterpret_cast<const uint32_t*>(0x00E89B8C)) return false;
    reinterpret_cast<void(__cdecl*)(const char*)>(0x00811DD0)(name);
    return true;
}

// ---- the fighters of a re-fight ----
// SetSelectedPlayers 0x008142E0 (cdecl, two names; read in the game's code on 2026-11-14): it prints
// "SetSelectedPlayers(pchr1:%s, pchr2:%s)", clears both players' second fighter and copies each name into its player's
// record through 0x008141A0 (player, slot, name). The record of player p is 0x00E89D7C + p * 0xC8C, and the name of its
// fighter in slot s lies at +0x964 + s * 0xEC (64 bytes). It is called from three places: 0x008143E4 (in 0x008143D0,
// script native 672: the arena and both fighters, run by the select screen's script when both players are done),
// 0x008BB520 (script native 670) and 0x0081564B (the attract demos: left alone). The game's settings message carries
// no fighter: each game sets its own from what its select screen holds, so two games that are given the same pair here
// load the same fight.
// The fighter's colours are not in its name. The character select's START on a fighter picks its other colours (a
// costume's second palette: Johnny Cage's black suit against the blue one), and the select screen's script writes that
// choice into each player's record itself: PlayerSelect.mko, PSelectSetSettings (read 2026-11-16), for player p the
// value at the script's selection + 120 is stored at the record (script native 17544:25 (p)) + 2404 + 128, that is
// +0x964 + 0x80 = +0x9E4 (+0xEC further for a tag partner), after FixupAltPaletteSelections and before natives 670 /
// 672 set the names. A re-fight's hidden select is confirmed without START, so it wrote 0 there: in the two-PC log of
// 2026-11-16 21:37 and 21:42 "P1+9E4 00000001>00000000" and the same for P2 at each re-fight's start, the fighters'
// names the same. The game reads the flag after the select (nothing of the mod's earlier work kept it), so a re-fight
// gives the game the last fight's two flags back together with its names; where the pair does not stand, the select
// screen's own flags go back with its names.
namespace {
constexpr uintptr_t kSetPlayers = 0x008142E0, kSetPlayersBy672 = 0x008143E4, kSetPlayersBy670 = 0x008BB520;
constexpr uintptr_t kFighterName = 0x964;  // in a player's record: its first fighter's name
constexpr uintptr_t kFighterColours = 0x964 + 0x80;  // ... and its colours: 0 its first palette, 1 the other (START)
constexpr size_t kFighterLen = 64;
bool gPlayersArmed = false, gPlayersWritten = false, gPlayersKept = false;
char gWantA[kFighterLen] = "", gWantB[kFighterLen] = "", gHadA[kFighterLen] = "", gHadB[kFighterLen] = "";
uint32_t gWantColours[2] = {}, gHadColours[2] = {};
bool gWantColoursOk = false, gHadColoursOk = false;
uint32_t gPlayerCalls = 0;

bool ReadableName(const char* s) { return s && !IsBadStringPtrA(s, kFighterLen) && s[0] && strnlen(s, kFighterLen) < kFighterLen; }
uint32_t RealSetPlayers(const char* p1, const char* p2) {
    return reinterpret_cast<uint32_t(__cdecl*)(const char*, const char*)>(kSetPlayers)(p1, p2);
}
volatile uint32_t* ColoursAt(int player) {
    const uintptr_t at = kPlayerInfo + (uintptr_t)player * kPlayerInfoStride + kFighterColours;
    return IsBadWritePtr(reinterpret_cast<void*>(at), 4) ? nullptr : reinterpret_cast<volatile uint32_t*>(at);
}
bool WriteColours(const uint32_t v[2]) {
    volatile uint32_t *a = ColoursAt(0), *b = ColoursAt(1);
    if (!a || !b) return false;
    *a = v[0];
    *b = v[1];
    return true;
}
}  // namespace
bool FightColours(uint32_t out[2]) {
    volatile uint32_t *a = ColoursAt(0), *b = ColoursAt(1);
    if (!out || !a || !b) return false;
    out[0] = *a;
    out[1] = *b;
    return true;
}
// Called in place of SetSelectedPlayers by the two script natives. Not armed: straight on to the game's function.
// Armed: the game's function runs first with the script's own names, exactly as in a game that is not armed (0x008141A0
// draws a fighter itself for the name at 0x00D5DFEC, so that draw is made at the same moment on both PCs whichever of
// them is armed), the two names it left in the records are kept, and then it runs again with the armed pair: every
// field the first call wrote is written again by the second.
extern "C" uint32_t __cdecl MK9R_HkSetPlayers(const char* p1, const char* p2) {
    ++gPlayerCalls;
    const uint32_t r = RealSetPlayers(p1, p2);
    char a[kFighterLen] = "", b[kFighterLen] = "";
    if (!gPlayersArmed || !ScriptRng::FightPlayers(a, b, sizeof a)) return r;  // (a script that names no fighter is left alone)
    memcpy(gHadA, a, sizeof gHadA);
    memcpy(gHadB, b, sizeof gHadB);
    gHadColoursOk = FightColours(gHadColours);  // (the script wrote them just before it set the names)
    gPlayersWritten = true;  // (a pair that was kept stays kept: a later call of the script gets the same pair)
    RealSetPlayers(gWantA, gWantB);
    const bool coloured = gWantColoursOk && WriteColours(gWantColours);
    char had[24] = "not read", given[40] = "not known: left as the script set them";
    if (gHadColoursOk) snprintf(had, sizeof had, "%u and %u", gHadColours[0], gHadColours[1]);
    if (coloured) snprintf(given, sizeof given, "%u and %u", gWantColours[0], gWantColours[1]);
    Logger::Info("REFIGHT fighters: the script set %.60s and %.60s (call %u, named %.40s and %.40s; colours %s); the game is given the "
                 "re-fight's %.60s and %.60s (colours %s)",
                 gHadA, gHadB, gPlayerCalls, ReadableName(p1) ? p1 : "?", ReadableName(p2) ? p2 : "?", had, gWantA, gWantB, given);
    return r;
}

// ---- the pictures of a re-fight's versus / loading screen ----
// Read in the game's scripts and code on 2026-11-16. MK9's versus / loading screen has no fighter pictures of its own.
// When a character select ends, the last thing its script does before it asks for the fight (PlayerSelect.mko,
// SaveLoadingScreenRenders; the Ladder does the same on its tower screen, fg_utils.mko p_setup_versus_screen_images) is
// to ask the select screen's two body pictures for a copy each, named "BodyRenderBackLeft" and "BodyRenderBackRight":
// script native 4039 (0x008E00C0) calls 0x004D5EC0 (thiscall on the picture's image object, the name), which copies the
// image's texture into the engine's transient package through 0x006A07A0 (thiscall on the texture, the name). The script
// keeps each copy in its player's record (+0x964 + 0xA0, held against the engine's clean-up), and the loading screen's
// script (fg_utils.mko, LoadVersusImages) shows the two copies by those names. So that screen shows what the select
// screen showed, whatever fighters the game was given afterwards: Scorpion and Quan Chi before a fight of Sektor against
// Johnny Cage in the one game it was looked at (2026-11-16), and Scorpion and Sub-Zero in a re-fight, whose hidden
// select screen is confirmed where its cursors start.
// For a re-fight whose pair stands (the stage screen's exchange has settled it by then: the script sets the fighters,
// sends its settings and waits before it comes to the copies), each copy is made of the body picture of the re-fight's
// fighter instead. The picture's name is the game's own: 0x00814530 (cdecl: side, the fighter's name, out, size), the
// routine behind script native 1414 with which the Ladder names its opponents' pictures ("ui_ps_vs_jpegs.BD_<fighter>
// .jpeg", "_f" before the ending for a fighter with a picture of its own on the right, a fighter's own package for an
// added one; it cuts a costume's ending off the name it is given, in place). The picture is looked up among the loaded
// ones as the game's image code looks it up (0x0060D080, cdecl: the class at [0x00E590C0], 0, the name without its
// ending, 0, 0, 1; it loads nothing and answers null for a picture that is not in memory). Not found, or no copy: the
// select screen's own picture is copied, as without the mod, and MatchPictures says so (the mod's cover then stays over
// the versus / loading screen).
namespace {
constexpr uintptr_t kSaveRenderBy4039 = 0x008E00EA, kSaveRender = 0x004D5EC0, kCopyPicture = 0x006A07A0;
constexpr uintptr_t kPictureName = 0x00814530, kFindLoaded = 0x0060D080, kPictureClass = 0x00E590C0;
// 'refightpics 0|1'; 1 at the start since one offline game showed it (2026-11-16, the Ladder: two re-fights with the
// right pictures, added fighters among them, and a fight with no re-fight left as it is). No online match yet.
bool gPicsOn = true;
uint32_t gPicsGiven = 0, gPicCalls = 0;  // gPicsGiven: bit 0 the left picture, bit 1 the right, since the pair was armed

// The loaded body picture of `fighter` as the select screen shows it on `side` (0 left, 1 right); null: not in memory.
// `shown` gets the picture's name, for the log.
void* LoadedBodyPicture(int side, const char* fighter, char* shown, size_t cap) {
    char name[kFighterLen], pic[128] = "";
    snprintf(name, sizeof name, "%s", fighter);
    if (strlen(name) <= 5) return nullptr;  // ("CHAR_" and a name: the routine reads it from the sixth letter on)
    reinterpret_cast<void(__cdecl*)(uint32_t, char*, char*, uint32_t)>(kPictureName)((uint32_t)side, name, pic, 64);
    pic[sizeof pic - 1] = 0;
    snprintf(shown, cap, "%s", pic);
    char* dot = strrchr(pic, '.');
    void* cls = *reinterpret_cast<void**>(kPictureClass);
    if (!dot || dot == pic || !cls) return nullptr;
    *dot = 0;                               // (0x00600130 in the game's image code: a picture is looked up without its ending)
    if (!strchr(pic, '.')) return nullptr;  // (a package and a name, as every name of the routine is)
    return reinterpret_cast<void*(__cdecl*)(void*, void*, const char*, const char*, uint32_t, uint32_t)>(kFindLoaded)(cls, nullptr, pic, nullptr, 0, 1);
}
}  // namespace
// Called in place of 0x004D5EC0 by script native 4039: the copy of a picture that a script keeps under a name.
extern "C" void* __fastcall MK9R_HkSaveRender(void* image, void* /*edx*/, const char* name) {
    using CopyFn = void*(__thiscall*)(void*, const char*);
    ++gPicCalls;
    int side = -1;
    if (gPicsOn && gPlayersArmed && gPlayersKept && ReadableName(name))
        side = !_stricmp(name, "BodyRenderBackLeft") ? 0 : !_stricmp(name, "BodyRenderBackRight") ? 1 : -1;
    if (side >= 0) {
        const char* fighter = side ? gWantB : gWantA;
        char pic[96] = "";
        void* picture = LoadedBodyPicture(side, fighter, pic, sizeof pic);
        void* copy = picture ? reinterpret_cast<CopyFn>(kCopyPicture)(picture, name) : nullptr;
        if (copy) {
            gPicsGiven |= 1u << side;
            Logger::Info("REFIGHT pictures: MK9's script keeps the select screen's %s body picture for its versus / loading screen (%s); the copy "
                         "is made of %.90s instead, the picture of the re-fight's %.60s",
                         side ? "right" : "left", name, pic, fighter);
            return copy;
        }
        Logger::Info("REFIGHT pictures: the picture of the re-fight's %.60s (%.90s) is %s; the select screen's own %s picture is kept for the "
                     "versus / loading screen (%s), as without the mod",
                     fighter, pic[0] ? pic : "no name", picture ? "loaded, but no copy of it was made" : "not among the loaded ones",
                     side ? "right" : "left", name);
    }
    return reinterpret_cast<CopyFn>(kSaveRender)(image, name);
}
namespace {
// The call of script native 672 is the one a re-fight needs; 670's is taken along where it is found, and so is the
// picture copy's (without it a re-fight's versus / loading screen keeps the select screen's pictures).
bool InstallPlayerSites() {
    static int state = 0;  // 1 in, -1 refused
    if (state) return state > 0;
    const auto patch = [](uintptr_t site, uintptr_t was, uintptr_t now, const char* name) {
        uint8_t orig[5] = {0xE8}, call[5] = {0xE8};
        const int32_t ro = (int32_t)(was - (site + 5));
        const int32_t rn = (int32_t)(now - (site + 5));
        memcpy(orig + 1, &ro, 4);
        memcpy(call + 1, &rn, 4);
        return CodePatch::Write(site, orig, call, 5, name);
    };
    const uintptr_t players = reinterpret_cast<uintptr_t>(&MK9R_HkSetPlayers);
    state = patch(kSetPlayersBy672, kSetPlayers, players, "SetSelectedPlayers call of script native 672 (re-fight)") ? 1 : -1;
    if (state > 0) {
        patch(kSetPlayersBy670, kSetPlayers, players, "SetSelectedPlayers call of script native 670 (re-fight)");
        patch(kSaveRenderBy4039, kSaveRender, reinterpret_cast<uintptr_t>(&MK9R_HkSaveRender),
              "picture copy of script native 4039 (re-fight's versus / loading screen)");
    }
    return state > 0;
}
}  // namespace

bool FightPlayers(char* p1, char* p2, size_t cap) {
    const char* a = reinterpret_cast<const char*>(kPlayerInfo + kFighterName);
    const char* b = reinterpret_cast<const char*>(kPlayerInfo + kPlayerInfoStride + kFighterName);
    if (!p1 || !p2 || cap < kFighterLen || IsBadReadPtr(a, kFighterLen) || IsBadReadPtr(b, kFighterLen)) return false;
    memcpy(p1, a, kFighterLen - 1);
    memcpy(p2, b, kFighterLen - 1);
    p1[kFighterLen - 1] = p2[kFighterLen - 1] = 0;
    return p1[0] && p2[0];
}

bool SetMatchPlayers(const char* p1, const char* p2, const uint32_t* colours) {
    if (!p1 || !p2) {
        gPlayersArmed = false;
        gPicsGiven = 0;
        return true;
    }
    if (!p1[0] || !p2[0] || strlen(p1) >= kFighterLen || strlen(p2) >= kFighterLen || !InstallPlayerSites()) return false;
    snprintf(gWantA, sizeof gWantA, "%s", p1);
    snprintf(gWantB, sizeof gWantB, "%s", p2);
    gWantColoursOk = colours != nullptr;
    gWantColours[0] = colours ? colours[0] : 0;
    gWantColours[1] = colours ? colours[1] : 0;
    gHadColoursOk = false;
    gPlayersArmed = true;
    gPlayersWritten = gPlayersKept = false;
    gPicsGiven = 0;
    return true;
}
uint32_t MatchPictures() { return gPlayersArmed && gPlayersKept ? gPicsGiven : 0; }
void SetMatchPictures(bool on) { gPicsOn = on; }
bool MatchPicturesOn() { return gPicsOn; }
uint32_t PictureCalls() { return gPicCalls; }
bool MatchPlayersWritten() { return gPlayersArmed && gPlayersWritten; }
void KeepMatchPlayers() {
    if (gPlayersArmed && gPlayersWritten) gPlayersKept = true;
}
void KeepMatchPlayersForTest() {
    if (gPlayersArmed) gPlayersKept = true;
}
bool UndoMatchPlayers(const char* why) {
    if (gPlayersKept) return false;
    const bool armed = gPlayersArmed, written = gPlayersWritten;
    gPlayersArmed = gPlayersWritten = false;
    if (!written) {
        if (armed) Logger::Info("REFIGHT fighters: the re-fight's pair is not used (%s); the game had not been given it", why ? why : "?");
        return false;
    }
    RealSetPlayers(gHadA, gHadB);
    const bool coloured = gHadColoursOk && WriteColours(gHadColours);
    Logger::Info("REFIGHT fighters: the re-fight's pair does not stand (%s): the game is given back %.60s and %.60s, the names its select "
                 "screen had set%s",
                 why ? why : "?", gHadA, gHadB, coloured ? ", and its colours" : "");
    return true;
}
uint32_t PlayerCalls() { return gPlayerCalls; }

// ---- a re-fight without the character select (a test, 2026-11-15) ----
// MK9's scripts change screens through script native 171 (0x008B7C00), which calls 0x00495A10 (cdecl: module, function)
// at 0x008B7C10. That routine only notes the request (two names at 0x00E3B8A8 / 0x00E3B8B0, a flag at 0x00E3B85C); the
// engine ends the running scripts and starts the named function in its own tick (0x0080F673). Read in the game's
// scripts: the online rematch screen (MKOnlineEngine_RematchScreen) ends with ("PlayerSelect.mko", "PlayerSelect"); the
// character select starts a fight with ("versus.mko", "versus_mode"); and MK9's own offline rematch (versus.mko,
// p_versus_loop) asks for ("versus.mko", "versus_mode") at once, with the fighters and the arena the game holds and no
// character select. Armed, a script's next request for the character select is turned into the armed one.
namespace {
constexpr uintptr_t kModeJumpBy171 = 0x008B7C10;
bool gJumpArmed = false, gJumpTaken = false;
char gJumpModule[48] = "", gJumpFunction[64] = "";
uint32_t gJumpCalls = 0;
}  // namespace
extern "C" void __cdecl MK9R_HkModeJump(const char* module, const char* function) {
    ++gJumpCalls;
    const bool readable = ReadableName(module) && ReadableName(function);
    if (gJumpArmed && readable && !_stricmp(module, "PlayerSelect.mko") && !_stricmp(function, "PlayerSelect")) {
        gJumpArmed = false;
        gJumpTaken = true;
        Logger::Info("REFIGHT jump: a script asks for the character select (%s / %s, request %u); the game is asked for %s / %s instead", module,
                     function, gJumpCalls, gJumpModule, gJumpFunction);
        module = gJumpModule;
        function = gJumpFunction;
    } else {
        Logger::Info("REFIGHT jump: a script asks for %.47s / %.63s (request %u%s)", readable ? module : "?", readable ? function : "?", gJumpCalls,
                     gJumpArmed ? "; the armed one waits for a request for the character select" : "");
    }
    reinterpret_cast<void(__cdecl*)(const char*, const char*)>(kRunScript)(module, function);
}
bool SetModeJump(const char* module, const char* function) {
    if (!module || !function) {
        gJumpArmed = false;
        return true;
    }
    static int state = 0;  // 1 in, -1 refused
    if (!state) {
        uint8_t orig[5] = {0xE8}, call[5] = {0xE8};
        const int32_t ro = (int32_t)(kRunScript - (kModeJumpBy171 + 5));
        const int32_t rn = (int32_t)(reinterpret_cast<uintptr_t>(&MK9R_HkModeJump) - (kModeJumpBy171 + 5));
        memcpy(orig + 1, &ro, 4);
        memcpy(call + 1, &rn, 4);
        state = CodePatch::Write(kModeJumpBy171, orig, call, 5, "script native 171's request for a script mode (re-fight)") ? 1 : -1;
    }
    if (state < 0 || !module[0] || !function[0] || strlen(module) >= sizeof gJumpModule || strlen(function) >= sizeof gJumpFunction) return false;
    snprintf(gJumpModule, sizeof gJumpModule, "%s", module);
    snprintf(gJumpFunction, sizeof gJumpFunction, "%s", function);
    gJumpArmed = true;
    gJumpTaken = false;
    return true;
}
bool ModeJumpTaken() { return gJumpTaken; }
uint32_t PlayerRecordValue(int player) {
    return player == 0 || player == 1 ? *reinterpret_cast<const uint32_t*>(kPlayerInfo + (uintptr_t)player * kPlayerInfoStride + 0xB40) : 0;
}

void ForceStage(int index) {
    const char* name = index >= 0 ? StageName((uint32_t)index, nullptr) : nullptr;
    if (index >= 0 && !name) {
        Logger::Info("FORCESTAGE %d: no such stage (table of %u)", index, *reinterpret_cast<const uint32_t*>(0x00E92350));
        return;
    }
    if (index >= 0) InstallStageSites();
    gForceStage = index;
    MK9R_ForceStageOn = Forced() >= 0;
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
bool StageSelectable(uint32_t index) {
    uint32_t flag = 0;
    return StageName(index, &flag) && flag != 0;
}
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
