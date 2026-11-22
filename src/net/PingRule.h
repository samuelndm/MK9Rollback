#pragma once
#include <algorithm>
#include <cstdint>

// The online mode's input delay from the hello round trips (OnlineMode.cpp, "Input delay from the ping"). Nothing here
// touches the game: the rule is checked by the host test pingrule_test, which also replays the field evenings' readings.
//
// What a reading is. Hellos are sent and read at a fixed point of each game's tick (the Steam bridge forwards at the
// frame boundaries, and the session's inputs travel the same way), so a round trip reads as whole frames: 33, 50, 67 ms.
// Which of two neighbouring steps a link reads depends on where the two games' ticks fall against each other: for a round
// trip T the reading is ceil(T / frame) frames, or one frame more. Before a fight the two games' ticks are not aligned
// yet (StartSync aligns them at the session's start), so a short link reads either step all through a match start.
// Field evidence, the user's link (true round trip about 22 ms; 47 matches of 2026-11-14 and 2026-11-20 at delay 1):
// the readings were 33 or 50 (median 50 in 8 matches) with now and then a 67, and how many rollbacks a match had did not
// follow the reading at all (median-50 matches: 25, 0, 0, 0, 0, 0, 1, 0; median-33 matches up to 46).
//
// Until 2026-11-22 delay 1 needed the SECOND-HIGHEST reading at or under kDelay1UpToMs: two readings of 67 among 16
// (median 50) put a whole match at delay 2, 17 ms more input lag (2026-11-14 17:28; 2 of the logs' 52 match starts read so).
// Now rollback's delay 1 is decided by the typical reading (the median) with one more frame allowed for the spikes; the
// higher delays and Safe mode (lockstep waits for every input, so a spike stalls it) keep the cautious second-highest.
namespace PingRule {

constexpr uint32_t kKeep = 16;  // round trips kept (OnlineMode's ring)

// Rollback delay 1: the median at most 3 frames plus a margin (a reading of 50 is a short link whose ticks fell apart),
// and the second-highest at most one frame above that (a spike of one frame on top: 67). A link whose second-highest
// reads 83 or more has jumps of two frames and more, and takes the old rule.
constexpr uint32_t kDelay1UpToMs = 55;
constexpr uint32_t kDelay1SpikesUpToMs = 72;
// Delays 2..5: the second-highest up to these (half a frame above the 6, 8, 12 and 15 frames they stand for: a link of
// exactly 8 frames read 133 or 134 ms and flipped between 3 and 4 from match to match); beyond: kMaxRollbackDelay.
constexpr uint32_t kRollbackUpTo[] = {108, 142, 208, 258};
constexpr int kMaxRollbackDelay = 6;
// Safe mode (lockstep): 2 frames while both trips fit in 2 frames with 10 ms kept for jitter, else 3.
constexpr uint32_t kLockstepJitterMs = 10;

struct Readings {
    uint32_t n = 0;              // readings kept (at most kKeep)
    uint32_t secondHighest = 0;  // 0 without readings; the only one with one reading
    uint32_t median = 0;         // the upper median (s[n / 2])
    uint32_t lowest = 0;
};

// ring: the kept round trips (the first min(count, kKeep) entries are filled); count: every reading taken so far.
inline Readings Summarize(const uint32_t* ring, uint32_t count) {
    Readings r;
    r.n = count < kKeep ? count : kKeep;
    if (!r.n) return r;
    uint32_t s[kKeep];
    std::copy(ring, ring + r.n, s);
    std::sort(s, s + r.n);
    r.secondHighest = r.n >= 2 ? s[r.n - 2] : s[0];
    r.median = s[r.n / 2];
    r.lowest = s[0];
    return r;
}

// The smallest delay (frames of 1000/60 ms) the readings allow, with margin ms added to every reading (OnlineMode's
// kDownMarginMs when asking whether a smaller delay than the running one would fit). cpuFloor: the smallest rollback
// delay this PC runs without replay hitches (OnlineMode's gCpuFloor). Called with enough readings only (kPingMin).
inline int DelayFor(const Readings& r, bool lockstep, uint32_t margin, int cpuFloor) {
    const uint32_t high = std::max(r.secondHighest, r.median) + margin;  // with 2 readings the median is the higher one
    if (lockstep) return high + kLockstepJitterMs <= 2u * 2000 / 60 ? 2 : 3;
    const int floor = cpuFloor > 1 ? cpuFloor : 1;
    if (r.median + margin <= kDelay1UpToMs && high <= kDelay1SpikesUpToMs) return floor;
    for (int d = 2; d <= 5; ++d)
        if (high <= kRollbackUpTo[d - 2]) return std::max(d, floor);
    return std::max(kMaxRollbackDelay, floor);
}

}  // namespace PingRule
