#include "CrowdRng.h"

#include <cstring>

#include "../debug/Logger.h"
#include "../hook/CodePatch.h"

namespace {
constexpr uintptr_t kRandN = 0x00492050;  // int __cdecl rand(n): (RandInt() * n) >> 31, RNG A
// Every call of rand(n) in the crowd manager's code (0x008F8A00-0x008FA2A0): 0x008F9260 picks a random list entry,
// 0x008F9C70 is the per-character state timer, 0x008F9E70 the crowd update. Then the crowd's setup natives
// (0x008BB680 -> 0x008FB060 places the background characters through the world's actors, 258 draws on Courtyard at
// one frame of the intro: two-instance 'netplayload 1' trace, 2026-11-04; 0x008BB660 -> 0x008FB390). Both call crowd
// class methods (0x008F83F0, 0x008F9190, 0x008F92C0, 0x008F9C20); their setup ran on different frames in the two games.
constexpr uintptr_t kSites[] = {0x008F927E, 0x008F9CAB, 0x008F9CD3, 0x008F9CF6, 0x008F9D19, 0x008F9D41, 0x008F9D60, 0x008F9D72,
                                0x008F9E05, 0x008F9FFA, 0x008FA026, 0x008FA03E, 0x008FA068, 0x008FA07E, 0x008FA098,
                                0x008FB30C, 0x008FB606, 0x008FB62C};
volatile bool gActive = false;
bool gInstalled = false;
uint32_t gIdx = 0;

uint32_t GameFrame() {
    const uint32_t gi = *reinterpret_cast<const uint32_t*>(0x00DEF4B4);
    return gi ? *reinterpret_cast<const uint32_t*>(gi + 0x70) : 0;
}
uint32_t Mix(uint32_t x) {
    x ^= x >> 16;
    x *= 0x7FEB352Du;
    x ^= x >> 15;
    x *= 0x846CA68Bu;
    x ^= x >> 16;
    return x;
}
}  // namespace

extern "C" int __cdecl MK9R_CrowdRand(int n) {
    if (!gActive) return reinterpret_cast<int(__cdecl*)(int)>(kRandN)(n);
    const uint32_t v = Mix(GameFrame() * 0x9E3779B1u ^ Mix(++gIdx + 0x68E31DA4u)) & 0x7FFFFFFFu;
    return (int)(((int64_t)v * n) >> 31);  // the original's range: [0, n) for n > 0
}

namespace CrowdRng {

bool Install() {
    if (gInstalled) return true;
    bool ok = true;
    for (uintptr_t site : kSites) {
        uint8_t expected[5] = {0xE8}, replacement[5] = {0xE8};
        const int32_t relOld = (int32_t)(kRandN - (site + 5));
        const int32_t relNew = (int32_t)(reinterpret_cast<uintptr_t>(&MK9R_CrowdRand) - (site + 5));
        memcpy(expected + 1, &relOld, 4);
        memcpy(replacement + 1, &relNew, 4);
        ok &= CodePatch::Write(site, expected, replacement, 5, "crowd rand(n)");
    }
    gInstalled = ok;
    if (!ok) Logger::Info("CROWDRNG: a crowd call site was not as expected; background crowds keep drawing the gameplay RNG");
    return ok;
}
void SetActive(bool on) { gActive = on && gInstalled; }
bool Active() { return gActive; }
void BeginFrame() { gIdx = 0; }

}  // namespace CrowdRng
