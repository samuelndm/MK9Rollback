#pragma once
#include <cstdint>

// MKScript RandomInt (native 0x008B8790 -> 0x00493C90) draws from UE3's GSRandSeed 0x00E5352C (rngB), the
// stream that particles, FX, sound variation and other UE3 code also advance (about 40 functions). A replay
// that makes a different number of those cosmetic draws (sounds deduplicated, effects spawned differently)
// shifted the script's numbers, and NATIVETRACE showed 0x008B8790 results differing live vs replay
// (padfull_1, 2026-10-18). Two PCs would disagree the same way in lockstep.
//
// While active, the native's call at 0x008B879C goes to a copy of 0x00493C90 that draws from a private
// seed with the same LCG (0x0BB38435, 0x3619636B) and the same executor-state check; likewise the script
// random float (native 0x008B7FD0, call at 0x008B7FF6 -> 0x00887F50, its only caller), same stream. The seed is in the
// snapshot (Tier A "scriptrng"). It is seeded from the gameplay RNG A (0x00E7C054) whenever the pair of
// fighter MK ids changes, i.e. once per fight, identically on both peers; a rollback restores both the
// seed and the key, so it never reseeds.
namespace ScriptRng {

struct State {
    uint32_t seed;
    uint32_t key;  // fighter MK ids the seed belongs to
};

bool Install();  // CodePatch of the call site; idempotent
void SetActive(bool on);
bool Active();
State* StatePtr();  // for the snapshot
uint32_t CurrentKey();  // the fighter key the stream would be seeded for now
// Two-instance tests: seed RNG A, RNG B and the script stream from 'seed' whenever the attract script starts
// (StartAttractMode), so every instance picks the same demo fight. 0 = off.
void SetAttractSeed(uint32_t seed);
void OnFightStart();  // restarts the attract pick index (combat start)
// Two-instance tests on every stage: the attract script's random stage pick (0x00810530, table [0x00E9234C]) returns
// entry 'index' of the stage table instead ('forcestage -1': random again). Needs 'scriptseed' (the pick's hook).
void ForceStage(int index);
void SetPickVariety(uint32_t v);  // 'pickvariety N': other attract fighters for the same scriptseed
void LogStages();  // 'stages': the table's entries (index, name, selectable flag, attract exclusion)

}  // namespace ScriptRng
