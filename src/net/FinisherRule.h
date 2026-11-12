#pragma once
#include <cstdint>

// The frame at which both games end a rollback session in the finisher phase (OnlineMode.cpp, "the finisher phase").
// Nothing here touches the game: the phase cannot run in the test harness (offline MK9's load handshake answers "done"),
// so the rule is checked by a host-side test instead (finisher_test).
namespace FinisherRule {

// The session ends kHandBack frames after the last frame that started with the fight body running. Only notes at least
// kSettled frames old decide: older than any rollback reaches (GGPO predicts 8 frames at most), so both games read the
// same ones whatever each predicted on the way.
constexpr uint32_t kHandBack = 30, kSettled = 16;

class Notes {
public:
    constexpr Notes() { Clear(); }
    // A session ended: the next one starts without the notes of this one (its last frames were never confirmed, and
    // MK9 numbers the frames of every fight from the same start).
    constexpr void Clear() {
        for (uint32_t& n : note_) n = kNone;
    }
    // Every simulated frame as it starts, live or replayed: did it start in the finisher state with the body stopped?
    void Note(uint32_t gameFrame, bool stopped) { note_[gameFrame & 63] = gameFrame << 1 | (stopped ? 1u : 0u); }

    // Frame F is the one where both games end the session: the body ran at frame F - kHandBack and was stopped at every
    // frame after it through F - kSettled. Asked before frame F starts, on every live frame.
    bool HandBackAt(uint32_t F) const {
        bool stopped = false;
        if (F < kHandBack || !Noted(F - kHandBack, &stopped) || stopped) return false;
        for (uint32_t f = F - kHandBack + 1; f + kSettled <= F; ++f)
            if (!Noted(f, &stopped) || !stopped) return false;
        return true;
    }

private:
    static constexpr uint32_t kNone = 0xFFFFFFFFu;  // no frame's note (GameFrame stays far below 2^31 - 1)
    bool Noted(uint32_t gameFrame, bool* stopped) const {  // false: no note for that frame
        const uint32_t note = note_[gameFrame & 63];
        if (note == kNone || note >> 1 != (gameFrame & 0x7FFFFFFFu)) return false;
        *stopped = note & 1;
        return true;
    }
    uint32_t note_[64] = {};  // by GameFrame % 64: frame << 1 | stopped
};

}  // namespace FinisherRule
