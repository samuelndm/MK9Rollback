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
//
// Near-frame matching (round 3, 2026-11-10): a corrected prediction often keeps a sound but moves it a frame (a hit
// that lands one frame later). Matched on the exact frame only, the original kept playing as "abandoned" and the
// replay started the same sound again: an audible double. A replayed sound now also takes an unconsumed record of the
// same cue up to 'soundnear' frames away inside the replayed window (nearest first), as Skullgirls' sound ledger does
// ("the desired sound is what is playing: nothing restarts"). Presentation only: handles were already machine-local.
namespace SoundDedupe {

bool Install();
// Rollback bracket (game thread). S: the first frame the replay runs, F: the live frame it ends at. Called at every
// load of one boundary (GGPO can load twice in one): the window grows to the lowest S and the records of [S, F) become
// matchable again, so a second replay of the same frames does not play their sounds a second time.
void BeginResim(uint32_t S, uint32_t F);
struct Stats {
    int deduped;    // replayed sounds that were already playing (shifted ones included)
    int shifted;    // of those: matched a frame or two away from where they first played
    int played;     // new in the corrected timeline: started late
    int abandoned;  // played only in the abandoned timeline (not stopped)
    int maxLate;    // the latest start of a new sound, in frames behind its own frame
};
Stats EndResim(uint32_t S, uint32_t F);
void Clear();  // timeline restart

void SetNearFrames(int frames);  // 'soundnear N': 0 = the exact frame only (rc27), default 2, at most 4
// P2P session totals (field logs: how often a correction is heard).
void SessionReset();
void LogSession();

}  // namespace SoundDedupe
