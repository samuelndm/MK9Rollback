#pragma once
#include <cstdint>

#include "StagePick.h"

// The wait after the last fighter is locked (docs\feature_parity_plan.md 4.2): how the stage screen keeps MK9 on the
// character select while the two players pick, and when the drawn arena is written into the game. No game, no clock of
// its own: the caller gives the time, so the host test (tests\host\stagehold_test.cpp) runs the same code.
//
// The hold. MK9's select screen script ends with a wait for the other game: it begins sync wait 2 (ST_BeginGameLoading)
// and asks script native 1260 (0x008C0520 -> 0x0081B460 -> session 0x00911C20) whether that wait still runs, until the
// answer is 0. While a panel that opened by itself is undecided, the mod answers 1 ("still waiting") in the game's
// place. Nothing is sent and nothing of the game's is changed: the game's own wait simply lasts longer, on each PC by
// itself, and ends when the panel lets go. The script gives a wait up after a count of its own ticks (native 1230,
// asked without the wait's type: 2400 where it was read, in the two-PC logs of 2026-11-11) and asks the session to
// send its request again at half that count, so the hold ends by itself after a time and after a number of answers,
// whichever comes first, and both stay under half the script's count.
//
// Which answer means what (read 2026-11-13 in the game's script and its code, both ways the same; the build of
// 16:28 that day had it the other way round and so ended the game's wait at its first question instead of holding it):
//   - the script MKOnline_CheckIsSyncRequestComplete(type) is "r = native 1260(type); return !r", and
//     MKOnline_SyncTimeoutWait leaves its loop with success when that returns true;
//   - the session's 0x00911C20(type) answers 1 without a record of the type and while the record's list of games it
//     still waits for (+4, count at +8) is not empty; the handler of the other game's confirmation (the text
//     "ConnectionSyncConfirmHandler sending %s [ACK]") empties that list and calls the session's "sync complete"
//     handler exactly when 0x00911C20 then answers 0; without an online session 0x0081B460 answers 0.
//
// The write. The arena is written on both PCs or on neither only if both know the same: a game writes when it is
// holding, both picks are in, the other game has shown that it has this game's pick (Exchange::Confirmed), and the
// other game's latest packet says that it is holding too (the packet's "apply" flag) and that it knows this game has
// both picks (its "sure" flag). It is asked once, when the panel gives the game back: seconds after both picks were
// in, with a packet in each direction every 100 ms until then, and up to 1.5 s later while the other game's "sure" is
// missing. Two games cannot be certain of each other over a link that may fail: one writes and the other does not if
// the link dies between the packet that makes one of them sure and the next one. The test counts how often that
// happens over links that lose most packets and over links that die.
namespace StageHold {

constexpr uint32_t kSyncType = 2;      // ST_BeginGameLoading
constexpr uint32_t kAnswerOver = 0;     // native 1260: the wait of that type is over (or there is no online session)
constexpr uint32_t kAnswerWaiting = 1;  // native 1260: the wait still runs (or was never begun); what a held question reads
constexpr uint32_t kMaxAnswers = 1100;  // under half the script's count of ticks (2400 where it was read): the hold ends here at the latest

struct Hold {
    bool wanted = true;    // 'stagehold 0|1'
    bool broken = false;   // a held match did not load: off until the game is started again or 'stagehold 1'
    bool on = false;       // this panel's hold began (the game asked, and was answered "still waiting")
    bool over = false;     // this panel's hold has ended: every later question gets the game's own answer
    uint64_t since = 0;    // when it began
    uint64_t overAt = 0;
    uint32_t held = 0;     // answers given in the game's place
    uint64_t theirsAt = 0;  // when the game's own answer first was "over" under the hold (the other game was there); 0: never

    void Reset() {  // a new panel
        on = over = false;
        since = overAt = theirsAt = 0;
        held = 0;
    }
    bool Holding() const { return on && !over; }
    void End(uint64_t now) {
        if (!Holding()) return;
        over = true;
        overAt = now;
    }
    // The game asks whether sync wait 'type' still runs and would answer 'gameAnswer' (kAnswerOver / kAnswerWaiting).
    // panelWaits: a panel that opened by itself is on the screen and has not given the game back. True: the answer
    // must read kAnswerWaiting.
    bool OnPoll(uint32_t type, uint32_t gameAnswer, bool panelWaits, uint64_t now, uint32_t maxMs) {
        if (type != kSyncType || !wanted || broken || over || !panelWaits) return false;
        if (!on) {
            on = true;
            since = now;
        }
        if (now - since >= maxMs || held >= kMaxAnswers) {
            End(now);
            return false;
        }
        if (gameAnswer == kAnswerOver && !theirsAt) theirsAt = now;
        ++held;
        return true;
    }
};

enum class Use : uint8_t {
    kWrite,        // this game writes the agreed stage, and the other one has said it will
    kGameChoice,   // "random" won: the game's own choice stands on both
    kNoStage,      // nothing agreed and confirmed (the verdict says why)
    kNotHeld,      // this game is not holding: its own arena may be past changing
    kPeerNotHeld,  // the other game does not say that it holds
    kPeerNotSure,  // the other game holds but has not said that it knows this game has both picks
};
// Asked once, when the panel gives the game back. v: Judge with the confirmation.
inline Use Decide(const Hold& h, const StagePick::Exchange& x, const StagePick::Verdict& v) {
    if (v.outcome == StagePick::Outcome::kGameChoice) return Use::kGameChoice;
    if (v.outcome != StagePick::Outcome::kStage || !StagePick::IsEntry(v.entry)) return Use::kNoStage;
    if (!h.Holding()) return Use::kNotHeld;
    if (!x.apply || !x.theirsApply) return Use::kPeerNotHeld;
    if (!x.theirsSure) return Use::kPeerNotSure;
    return Use::kWrite;
}
inline const char* UseText(Use u) {
    switch (u) {
    case Use::kWrite: return "both games write the agreed arena";
    case Use::kGameChoice: return "\"random\" won";
    case Use::kNoStage: return "nothing agreed and confirmed";
    case Use::kNotHeld: return "this game was not waiting";
    case Use::kPeerNotHeld: return "the other game was not waiting";
    case Use::kPeerNotSure: return "the other game did not confirm in time";
    }
    return "?";
}

}  // namespace StageHold
