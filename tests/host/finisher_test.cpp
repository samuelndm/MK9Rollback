// Host-side test of src\net\FinisherRule.h: the frame at which both games hand a session back to MK9's netcode in the
// finisher phase. Two simulated games run the same fight with different rollback histories (what each notes for its
// unconfirmed frames is wrong at random, then replayed) and must end the session at the same frame.
// Build and run: see run.ps1 beside this file.
#include <cstdint>
#include <cstdio>
#include <initializer_list>
#include <utility>
#include <vector>

#include "FinisherRule.h"

using FinisherRule::kHandBack;
using FinisherRule::kSettled;

namespace {
int gFailed = 0, gChecks = 0;
void Check(bool ok, const char* what) {
    ++gChecks;
    if (!ok) ++gFailed;
    printf("%s  %s\n", ok ? "PASS" : "FAIL", what);
}

uint32_t gSeed = 20261111;
uint32_t Rnd(uint32_t n) {  // 0 .. n-1
    gSeed = gSeed * 1664525u + 1013904223u;
    return (gSeed >> 8) % n;
}

constexpr uint32_t kPredict = 8;  // GGPO's prediction window: a game simulates at most this many frames ahead of the
                                  // other player's inputs
static_assert(kSettled > kPredict, "the rule must read confirmed frames only");

using Fight = std::vector<uint8_t>;  // by frame: it starts in the finisher state with the body stopped (real inputs)

// The rule on the fight itself: what both games must answer.
int Expected(const Fight& fight, uint32_t first) {
    for (uint32_t F = first + kHandBack; F < fight.size(); ++F) {
        bool all = !fight[F - kHandBack];
        for (uint32_t f = F - kHandBack + 1; all && f + kSettled <= F; ++f) all = fight[f] != 0;
        if (all) return (int)F;
    }
    return -1;
}

// One game of a session. Frames below 'confirmed' ran on both players' real inputs; the ones from there to 'live' ran on
// a prediction, so their notes may be wrong until the inputs arrive and the frames are replayed.
struct Game {
    FinisherRule::Notes notes;
    uint32_t live, confirmed;
    int endedAt = -1;
    uint32_t wrongNotes = 0, replayed = 0;

    explicit Game(uint32_t first) : live(first), confirmed(first) {}

    bool Guess(const Fight& fight, uint32_t f, uint32_t wrongPct) {
        const bool wrong = Rnd(100) < wrongPct;
        wrongNotes += wrong;
        return (fight[f] != 0) != wrong;
    }
    // One tick of the game: the rule is asked before the frame boundary; then inputs arrive (a rollback replays every
    // unconfirmed frame), then the next frame is simulated unless the game waits (prediction window full, or a stall).
    void Tick(const Fight& fight, uint32_t wrongPct, uint32_t stallPct, bool askAfterReplay) {
        if (endedAt >= 0 || live >= fight.size()) return;
        if (!askAfterReplay && notes.HandBackAt(live)) {
            endedAt = (int)live;
            return;
        }
        const uint32_t arrived = confirmed + Rnd(4);  // inputs for 0-3 more frames
        const uint32_t upTo = arrived < live ? arrived : live;
        if (upTo > confirmed) {
            for (uint32_t f = confirmed; f < live; ++f, ++replayed) notes.Note(f, f < upTo ? fight[f] != 0 : Guess(fight, f, wrongPct));
            confirmed = upTo;
        }
        if (askAfterReplay && notes.HandBackAt(live)) {
            endedAt = (int)live;
            return;
        }
        if (live - confirmed >= kPredict || Rnd(100) < stallPct) return;
        notes.Note(live, Guess(fight, live, wrongPct));
        ++live;
    }
};

struct Result {
    int a, b;
    uint32_t wrong, replayed;
};
Result Play(const Fight& fight, uint32_t first, uint32_t wrongPct, uint32_t stallA, uint32_t stallB) {
    Game a(first), b(first);
    const auto done = [&](const Game& g) { return g.endedAt >= 0 || g.live >= fight.size(); };
    for (uint32_t tick = 0; tick < (fight.size() - first) * 100 && !(done(a) && done(b)); ++tick) {
        a.Tick(fight, wrongPct, stallA, false);
        b.Tick(fight, wrongPct, stallB, true);
    }
    if (!done(a) || !done(b)) return {-2, -2, 0, 0};  // a game that never got through the fight: the test's own fault
    return {a.endedAt, b.endedAt, a.wrongNotes + b.wrongNotes, a.replayed + b.replayed};
}

// 'len' frames from 'first' (frames below it are never simulated); the body stopped in the given [from, to) stretches.
Fight Make(uint32_t first, uint32_t len, std::initializer_list<std::pair<uint32_t, uint32_t>> stopped) {
    Fight fight(first + len, 0);
    for (auto [from, to] : stopped)
        for (uint32_t f = from; f < to && f < fight.size(); ++f) fight[f] = 1;
    return fight;
}

bool Both(const Fight& fight, uint32_t first, int want, uint32_t wrongPct = 50) {
    const Result r = Play(fight, first, wrongPct, 10, 30);
    if (r.a != want || r.b != want) printf("      wanted %d, the two games ended at %d and %d\n", want, r.a, r.b);
    return r.a == want && r.b == want;
}
}  // namespace

int main() {
    const uint32_t first = 100001;  // MK9's GameFrame at a fight's first frame

    // The plain case: the body stops at S and stays stopped.
    {
        const uint32_t S = first + 5000;
        const Fight fight = Make(first, 6000, {{S, ~0u}});
        Check(Expected(fight, first) == (int)(S + kHandBack - 1), "the reference: a stop at S hands back at S + 29");
        Check(Both(fight, first, (int)S + 29, 0), "stop at S, no wrong predictions: both games end at S + 29");
        Check(Both(fight, first, (int)S + 29, 50), "stop at S, half of the unconfirmed notes wrong: both games end at S + 29");
        Check(Both(fight, first, (int)S + 29, 100), "stop at S, every unconfirmed note wrong: both games end at S + 29");
    }
    // The stop's length: 14 frames stopped decide (F - 29 through F - 16), 13 do not.
    {
        const uint32_t S = first + 300;
        Check(Both(Make(first, 600, {{S, S + 13}}), first, -1), "a 13-frame stop that ends: no hand-back");
        Check(Both(Make(first, 600, {{S, S + 14}}), first, (int)S + 29), "a 14-frame stop: hand-back at S + 29, running again or not");
        Check(Both(Make(first, 600, {{S, S + 6}, {S + 40, ~0u}}), first, (int)S + 69), "a short stop, then the real one: only the real one counts");
    }
    // The session's first frames: nothing is noted before them.
    {
        Check(Both(Make(first, 400, {{first, ~0u}}), first, -1), "stopped from the session's first frame on: no hand-back (no running frame was seen)");
        Check(Both(Make(first, 400, {{first + 1, ~0u}}), first, (int)first + 30), "stopped from the second frame on: hand-back 29 frames later");
        Check(Both(Make(0, 400, {{1, ~0u}}), 0, 30), "the same with GameFrame counted from 0");
        Check(Both(Make(first, 400, {}), first, -1), "a fight whose body never stops: no hand-back");
    }
    // Notes of an earlier session with the same frame numbers do not decide anything after Clear().
    {
        FinisherRule::Notes notes;
        bool any = false;
        for (uint32_t F = 0; F < 200; ++F) any |= notes.HandBackAt(F);
        Check(!any, "no notes: no hand-back at any frame");
        for (uint32_t f = first; f < first + 20; ++f) notes.Note(f, false);
        for (uint32_t f = first + 20; f < first + 60; ++f) notes.Note(f, true);
        Check(notes.HandBackAt(first + 49) && !notes.HandBackAt(first + 48) && !notes.HandBackAt(first + 50),
              "noted by hand: the stop at +20 hands back at +49 and at no neighbour");
        Check(!notes.HandBackAt(first + 49 + 64) && !notes.HandBackAt(first + 49 - 64), "a frame 64 away reads no other frame's note");
        notes.Clear();
        any = false;
        for (uint32_t F = first; F < first + 200; ++F) any |= notes.HandBackAt(F);
        Check(!any, "after Clear(): the earlier session's notes are gone");
    }
    // Random fights: stops of random lengths at random frames, random prediction errors and stalls, different on each game.
    {
        int bad = 0, handBacks = 0;
        uint64_t wrong = 0, replayed = 0;
        const int kFights = 4000;
        for (int i = 0; i < kFights; ++i) {
            const uint32_t start = Rnd(2) ? first : Rnd(200000);
            const bool shortOnly = Rnd(5) == 0;  // one fight in five: no stop long enough
            Fight fight(start + 260, 0);
            for (uint32_t f = start + Rnd(120); f < fight.size();) {  // alternate stops and runs
                const uint32_t stop = shortOnly ? 1 + Rnd(13) : Rnd(4) ? 1 + Rnd(40) : 400;
                for (uint32_t k = 0; k < stop && f < fight.size(); ++k) fight[f++] = 1;
                f += 1 + Rnd(50);
            }
            const int want = Expected(fight, start);
            const Result r = Play(fight, start, Rnd(101), Rnd(60), Rnd(60));
            handBacks += want >= 0;
            wrong += r.wrong;
            replayed += r.replayed;
            if ((r.a != want || r.b != want) && bad++ < 5) printf("      fight %d: wanted %d, the two games ended at %d and %d\n", i, want, r.a, r.b);
        }
        printf("      %d fights, %d with a hand-back; %llu unconfirmed notes were wrong, %llu frames replayed\n", kFights, handBacks,
               (unsigned long long)wrong, (unsigned long long)replayed);
        Check(!bad, "4000 random fights: both games end the session at the frame the fight itself gives");
        Check(handBacks > kFights / 4 && handBacks < kFights && wrong > 100000 && replayed > 100000,
              "the random fights cover both outcomes, wrong predictions and replays");
    }

    printf("\n%d checks, %d failed\n", gChecks, gFailed);
    return gFailed ? 1 : 0;
}
