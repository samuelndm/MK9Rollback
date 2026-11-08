#pragma once
#include <cstdint>

// Milestone 11, sound: script sound natives (0x008E16E0 / 0x008E1740) play through the global
// sound object [0x00E91FF0] (vt 0x00D5E420), slot +0x20 = 0x00921200 (thiscall, 7 stack args,
// ret 0x1C; arg2 == 0 -> 0x00461E10 play by cue name in arg1). The slot is swapped (data write,
// HookManager) for a wrapper:
//  - live frames: play, record {gf, key, handle};
//  - resimulated frame g: a matching unconsumed record of the original timeline is not played
//    again, its handle is returned (script variables stay identical to the original timeline);
//    otherwise the sound is new in the corrected timeline and plays (late).
// Original records of the resimulated window that were never matched belonged to the abandoned
// timeline only (counted; stopping them is not implemented yet).
namespace SoundDedupe {

bool Install();
// Rollback bracket (game thread): counts per rollback.
void BeginResim();
struct Stats {
    int deduped, played, abandoned;
};
Stats EndResim(uint32_t S, uint32_t F);
void Clear();  // timeline restart

}  // namespace SoundDedupe
