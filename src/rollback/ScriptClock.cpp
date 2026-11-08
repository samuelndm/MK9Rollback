#include "ScriptClock.h"

#include <windows.h>

#include <cstring>

#include "../debug/Logger.h"
#include "../hook/CodePatch.h"
#include "../hook/HookManager.h"

namespace {
constexpr uintptr_t kRealMs = 0x004D5DA0, kRealUs = 0x004D5DF0;
constexpr uintptr_t kMsSites[] = {0x004931BE, 0x0049322B, 0x00493313};
constexpr uintptr_t kUsSites[] = {0x004931F0, 0x0049329B, 0x00493383};
constexpr uintptr_t kNativeSlot = 0x00DF0F3C, kNativeSeconds = 0x008B7F50;

using ClockFn = uint64_t(__cdecl*)();
volatile bool gActive = false;
bool gInstalled = false;
uint64_t gBaseMs = 0, gBaseUs = 0;
double gBaseSec = 0;
uint32_t gBaseGf = 0;

uint32_t GameFrame() {
    const uint32_t gi = *reinterpret_cast<const uint32_t*>(0x00DEF4B4);
    return gi ? *reinterpret_cast<const uint32_t*>(gi + 0x70) : 0;
}
int64_t Frames() { return (int64_t)(int32_t)(GameFrame() - gBaseGf); }
}  // namespace

extern "C" uint64_t __cdecl MK9R_ClockMs() {
    if (!gActive) return reinterpret_cast<ClockFn>(kRealMs)();
    return gBaseMs + (uint64_t)(Frames() * 50 / 3);  // 1000/60 ms per frame
}
extern "C" uint64_t __cdecl MK9R_ClockUs() {
    if (!gActive) return reinterpret_cast<ClockFn>(kRealUs)();
    return gBaseUs + (uint64_t)(Frames() * 50000 / 3);
}
// Native 191: seconds as float in the executor result slot ([0x00E39DF4]+0x1C).
extern "C" void __cdecl MK9R_NativeSeconds() {
    if (!gActive) {
        static uint32_t sRealCalls = 0;
        if (++sRealCalls <= 3 || sRealCalls % 2000 == 0)
            Logger::Info("SCRIPTCLOCK native 191 (appSeconds) real-clock call #%u at gf %u (outside a session)", sRealCalls, GameFrame());
        reinterpret_cast<void(__cdecl*)()>(kNativeSeconds)();
        return;
    }
    const float s = (float)(gBaseSec + (double)Frames() / 60.0);
    *reinterpret_cast<float*>(*reinterpret_cast<const uint32_t*>(0x00E39DF4) + 0x1C) = s;
    // Whether fight scripts read appSeconds at all (2026-11-04): a value stored before the session came from the real
    // clock, and on a PC up more than ~36 h (gBaseSec 131072 s) it would lie after the virtual one.
    static uint32_t sCalls = 0;
    if (++sCalls <= 5 || sCalls % 2000 == 0) Logger::Info("SCRIPTCLOCK native 191 (appSeconds) call #%u at gf %u: %.3f s", sCalls, GameFrame(), s);
}

namespace ScriptClock {

bool Install() {
    if (gInstalled) return true;
    // E8 rel32 call sites (not 4-byte aligned: CodePatch, expected bytes checked).
    auto retarget = [](uintptr_t site, uintptr_t oldTarget, void* newTarget, const char* name) {
        uint8_t expected[5] = {0xE8}, replacement[5] = {0xE8};
        const int32_t relOld = (int32_t)(oldTarget - (site + 5));
        const int32_t relNew = (int32_t)(reinterpret_cast<uintptr_t>(newTarget) - (site + 5));
        memcpy(expected + 1, &relOld, 4);
        memcpy(replacement + 1, &relNew, 4);
        return CodePatch::Write(site, expected, replacement, 5, name);
    };
    bool ok = true;
    for (uintptr_t s : kMsSites) ok &= retarget(s, kRealMs, reinterpret_cast<void*>(&MK9R_ClockMs), "script stopwatch ms clock");
    for (uintptr_t s : kUsSites) ok &= retarget(s, kRealUs, reinterpret_cast<void*>(&MK9R_ClockUs), "script stopwatch us clock");
    ok &= HookManager::PatchPointer(kNativeSlot, kNativeSeconds, reinterpret_cast<void*>(&MK9R_NativeSeconds), "script native 191 (seconds)");
    gInstalled = ok;
    if (!ok) Logger::Info("SCRIPTCLOCK: a patch site was not as expected; script real-time natives stay non-deterministic");
    return ok;
}

void Anchor(uint32_t gameFrame) {
    gBaseMs = reinterpret_cast<ClockFn>(kRealMs)();
    gBaseUs = reinterpret_cast<ClockFn>(kRealUs)();
    LARGE_INTEGER q;
    QueryPerformanceCounter(&q);
    gBaseSec = (double)q.QuadPart * *reinterpret_cast<const double*>(0x00E42FF0);
    gBaseGf = gameFrame;
}

void AnchorCanonical(uint32_t gameFrame) {
    // Far above any machine's real clock (2^42 ms is ~139 years of uptime): every stopwatch a script started before the
    // netplay start reads as long expired on both peers. Seconds stay at 2^17 so the float keeps ~1/64 s resolution.
    gBaseMs = 1ull << 42;
    gBaseUs = (1ull << 42) * 1000;
    gBaseSec = 131072.0;
    gBaseGf = gameFrame;
    gActive = gInstalled;  // players run MK9's real clock until here (RollbackManager: VirtualClockOutsideSessions)
}

void SetActive(bool on) { gActive = on && gInstalled; }
bool Active() { return gActive; }

}  // namespace ScriptClock
