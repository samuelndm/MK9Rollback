#pragma once
#include <cstdint>

#include "StagePick.h"

// The choice after an online match (docs\feature_parity_plan.md 4.3): each player picks RE-FIGHT, CHANGE FIGHTER OR
// ARENA, or LEAVE on a panel of the mod, sees the other's pick, and the pair says what comes next. The picks travel in
// the stage picks' packet (StagePick.h, kind 2) with the same exchange, so a lost datagram costs nothing and a locked
// pick never changes. Nothing here touches the game, Steam or Windows: tests\host\refightvote_test.cpp runs it.
//
// What the mod does with a pick is small, because MK9 has its own rematch box under the panel (its script
// MKOnlineEngine_RematchScreen, read 2026-11-13): each game answers that box for its own player, and MK9 itself
// carries the answer to the other game, ends the match when one declines and goes to the character select when both
// accept. So two games can never disagree about whether there is another match: that is MK9's own exchange, not the
// mod's. The mod's packets only add what MK9's box cannot say: whether both asked for the same fight again.
//
// Each game's script reads two pads there: its own player's, and the pad MK9's netcode fills with the other player's
// buttons (pad record 10; the mod's presses go to the six hardware records only, so they are this player's alone and
// reach the other game the way a real press does).
//
// MK9's box, as its script reads the pads (state per player: 1 undecided, 2 declined, 3 accepted; one decline or two
// accepts end it):
//   the menus' "confirm" (pad bit 0x20; native 0x008B8080 names it):  1 -> 3
//   the menus' "back"    (pad bit 0x80; native 0x008B8060 names it):  1 -> 2,  3 -> 1 (the accept is taken back)
// A press is a press only when the button was up the tick before, so every press here is followed by ticks of nothing.
namespace RefightVote {

enum Choice : uint8_t { kRefight = 0, kChange = 1, kLeave = 2, kChoices = 3 };
inline bool IsChoice(uint8_t c) { return c < kChoices; }
inline const char* ChoiceText(uint8_t c) {
    return c == kRefight ? "RE-FIGHT" : c == kChange ? "CHANGE FIGHTER OR ARENA" : c == kLeave ? "LEAVE" : "?";
}
// Both mods must mean the same three choices by the same three numbers: the exchange's context.
constexpr uint32_t kContext = 0x52463031u;  // "RF01"

enum class Outcome : uint8_t {
    kPending,  // a pick is missing
    kRefight,  // both asked for the same fight again: same fighters, same arena, no stage screen
    kChange,   // another match, through the character select and the stage screen as after any match
    kLeave,    // one of them leaves: no other match
};
// The pair's meaning, the same on both sides whichever order they are given in. A LEAVE needs no second pick.
inline Outcome Of(const StagePick::Side& mine, const StagePick::Side& theirs) {
    const bool m = mine.locked && IsChoice(mine.choice), t = theirs.locked && IsChoice(theirs.choice);
    if ((m && mine.choice == kLeave) || (t && theirs.choice == kLeave)) return Outcome::kLeave;
    if (!m || !t) return Outcome::kPending;
    return mine.choice == kRefight && theirs.choice == kRefight ? Outcome::kRefight : Outcome::kChange;
}
inline const char* OutcomeText(Outcome o) {
    switch (o) {
    case Outcome::kPending: return "a pick is missing";
    case Outcome::kRefight: return "the same fight again";
    case Outcome::kChange: return "another match, through the character select";
    case Outcome::kLeave: return "no other match";
    }
    return "?";
}
// A re-fight is acted on (the stage screen stays shut and the last arena is used) only when the other game has shown
// that it knows this game's pick too: then it computes the same outcome.
inline bool RefightAgreed(const StagePick::Exchange& x) { return x.Confirmed() && Of(x.mine, x.theirs) == Outcome::kRefight; }

// A pick is final in its exchange, so a player who leaves after another pick opens a later exchange of this game (the
// other game takes a later one in place of the earlier). What is known of the other side is its own exchange and stays;
// what it had heard of this side's earlier exchange is void.
inline void Reopen(StagePick::Exchange* x, uint32_t nonce, uint32_t serial) {
    const StagePick::Exchange old = *x;
    x->Open(old.kind, old.context, nonce, serial);
    x->theirs = old.theirs;
    x->theirSerial = old.theirSerial;
    x->theirMoves = old.theirMoves;
    x->theirSeq = old.theirSeq;
    x->theirsOpen = old.theirsOpen;
}

// ---- MK9's own box: which presses a pick is ----
constexpr uint32_t kAcceptButton = 0x20, kDeclineButton = 0x80;
constexpr int kHoldTicks = 3;    // a press is held this many ticks
constexpr int kGapTicks = 9;     // and followed by at least this many of nothing
constexpr int kAgainTicks = 30;  // the presses are made again this many ticks later while the box is still there
constexpr int kMaxPresses = 40;  // per pick (an accept is tried for 20 s, a leave for 10)

// The presses that bring MK9's box to what the player picked, tick by tick, for as long as the box is there (the box
// does not read its pads yet when it begins: it loads its own picture first, 0.4 s in the two-PC logs of 2026-11-11).
// Accept: one press of "confirm", made again every half second (a second accept does nothing to a player who has
// accepted). Leave: "back" twice, because
// the first one only takes an accept back when the player had accepted before, made again likewise (a box that is over
// reads nothing). Never mixed: once leaving, no accept is pressed again.
struct Presser {
    uint8_t want = 0;  // 0 nothing, 1 accept, 2 leave
    int tick = 0;      // ticks into the current round of presses
    int presses = 0;   // presses begun for the current pick
    bool done = false; // the pick has had all its presses
    void Accept() {
        if (want) return;  // a leave is never turned into an accept, and an accept is not begun twice
        want = 1;
        tick = presses = 0;
        done = false;
    }
    void Leave() {
        if (want == 2) return;
        want = 2;
        tick = presses = 0;
        done = false;
    }
    void Reset() { *this = Presser{}; }
    // One engine tick under the box. Returns the mask the game's pads read this tick (0: nothing held).
    uint32_t Tick() {
        if (!want || done) return 0;
        const int t = tick++;
        if (t >= kAgainTicks - 1) tick = 0;
        const int second = kHoldTicks + kGapTicks;
        const bool first = t < kHoldTicks, again = want == 2 && t >= second && t < second + kHoldTicks;
        if (!first && !again) return 0;
        if (t == 0 || t == second) {
            if (presses >= kMaxPresses) {
                done = true;
                return 0;
            }
            ++presses;
        }
        return want == 2 ? kDeclineButton : kAcceptButton;
    }
};

}  // namespace RefightVote
