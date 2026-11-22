#pragma once
#include <cstdint>

// What the mod does with the quarantine's held blocks when a rollback session stops in the middle of a fight, and the
// names it gives MK9's disconnect calls. Header-only: tests\host\stopkeep_test.cpp.
//
// 2026-11-20 20:14:59 (field, v20261119-0159): a session stopped "out of sync" mid-fight; the quarantine went off at the
// same boundary and ReleaseAll gave every held block back to MK9's allocator, the parked ones (small-block class
// 625..704 bytes: subsystems keep writing such blocks for many frames after freeing them) included, while the fight went
// on. 50 s later the next stage load's allocation returned 0x74696D45 (the text "Emit" where a free-list link belongs)
// and the render thread crashed at 0x007145BD. About 80 sessions that ended at the match's end (the fight body already
// stopped) never crashed. So at a stop that is not the match's normal end the parked blocks held at that moment are
// kept, never given back (hundreds to ~11,000 blocks of <= 704 bytes: a few MB, once), and a check counts how many of them
// the game writes after the stop. The other held blocks (non-parked: due within 11 frames anyway) go back as before.
// Not at the next ReleaseAll: in a fight that goes on with the quarantine on, that is the fight's end, where MK9 frees
// ~90,000 blocks (~100 MB) in its last frames (harness 2026-11-21).
namespace StopKeep {

// Which parked blocks are kept: GMalloc's (kind 0) and the small-block allocator's (kind 2) both grow on demand, so a few
// MB more cost nothing. MK heap blocks (kind 1) live in a fixed reservation that can run full (2026-10-29: heap #5),
// so they stay in the quarantine and go back as before.
inline bool KeepKind(uint8_t kind) { return kind != 1; }

// A block's content hash for the stale-write check: FNV-1a over its first <= 1 KB, read a dword at a time through 'read'
// (a guarded read in the game: false = unreadable). A block whose size is unknown (0) is hashed over its first 16 bytes,
// where a small-block free list keeps its link. False when any dword could not be read.
template <class Read>
bool HashBlock(uint32_t addr, uint32_t size, Read read, uint32_t* out) {
    uint32_t bytes = size ? size : 16;
    if (bytes > 1024) bytes = 1024;
    uint32_t h = 2166136261u;
    for (uint32_t o = 0; o + 4 <= bytes; o += 4) {
        uint32_t v = 0;
        if (!read(addr + o, &v)) return false;
        for (int b = 0; b < 4; ++b) h = (h ^ ((v >> (8 * b)) & 0xFF)) * 16777619u;
    }
    *out = h;
    return true;
}

// When the kept blocks are checked again: 2, 10, 30 and 90 s after the stop (and at the next timeline restart, logged
// on its own). Each checkpoint once, in order; a late call (a paused or loading game) takes every one it passed in one go.
struct Schedule {
    static constexpr uint32_t kAtMs[4] = {2000, 10000, 30000, 90000};
    uint32_t next = 0;
    // The checkpoint whose time 'elapsedMs' reached (its time in ms), or 0 for none. Call until it returns 0.
    uint32_t Due(uint32_t elapsedMs) {
        if (next >= 4 || elapsedMs < kAtMs[next]) return 0;
        uint32_t at = kAtMs[next];
        while (next < 4 && elapsedMs >= kAtMs[next]) at = kAtMs[next++];  // only the latest of those passed is logged
        return at;
    }
    bool Done() const { return next >= 4; }
};

// MK9's disconnect types (names at 0x0081A6A0, the switch of the type-to-text function there).
inline const char* DisconnectTypeName(uint32_t t) {
    static const char* const kNames[12] = {"DT_None", "DT_LostMatchConnection", "DT_MatchConnectionDesync", "DT_MatchKick",
                                           "DT_MatchAbort", "DT_MatchAbortAcknowledge", "DT_MatchQuit", "DT_MatchQuitAcknowledge",
                                           "DT_RematchDecline", "DT_LostAccountConnection", "DT_LostLinkConnection",
                                           "DT_LostMatchConnectionForced"};
    return t < 12 ? kNames[t] : "INVALID_DISCONNECT_TYPE";
}

// MK9's UI BeginDisconnect(type) (0x00944C90, thiscall) asks for the game's pause at 0x00944D42. Who called it, from its
// return address: the Disconnect(type) wrapper 0x0081C640 (13 call sites), or the link watch 0x00944FC0 (types 10 / 9 / 1
// from the link's state).
constexpr uint32_t kBeginDisconnectPause = 0x00944D42;
inline const char* DisconnectCallerName(uint32_t ret) {
    if (ret == 0x0081C679) return "MK9's Disconnect(type) 0x0081C640";
    if (ret == 0x0094501C || ret == 0x00945048) return "the link watch 0x00944FC0";
    return "another caller";
}

}  // namespace StopKeep
