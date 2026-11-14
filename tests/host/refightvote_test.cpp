// Host test of src\net\RefightVote.h: the choice after an online match. No game. The rule of the two picks, the
// presses that answer MK9's own rematch box (run against a model of that box written from its script), and the picks'
// exchange between two simulated games over links that lose, delay and reorder datagrams, with the timing Refight.cpp
// uses (a packet every 100 ms, 30 ms after a pick).
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "RefightVote.h"
#include "StagePick.h"

using namespace StagePick;
using Vote = RefightVote::Outcome;  // (StagePick has an Outcome of its own)

namespace {
int gFailed = 0, gChecks = 0;
void Expect(bool ok, const char* what) {
    ++gChecks;
    if (!ok && ++gFailed <= 20) printf("FAILED: %s\n", what);
}
uint32_t gRng = 0x51F0C3A7u;
uint32_t Rnd() {
    gRng = detail::Mix(gRng + 0x9E3779B9u);
    return gRng;
}
int Between(int lo, int hi) { return lo + (int)(Rnd() % (uint32_t)(hi - lo + 1)); }

Side Locked(uint8_t c) {
    Side s;
    s.nonce = 7;
    s.choice = s.cursor = c;
    s.locked = true;
    return s;
}

void Rule() {
    const Side none;
    for (uint8_t a = 0; a < RefightVote::kChoices; ++a) {
        for (uint8_t b = 0; b < RefightVote::kChoices; ++b) {
            const Vote o = RefightVote::Of(Locked(a), Locked(b));
            Expect(o == RefightVote::Of(Locked(b), Locked(a)), "the pair means the same in both games");
            const Vote want = a == RefightVote::kLeave || b == RefightVote::kLeave ? Vote::kLeave
                                 : a == RefightVote::kRefight && b == RefightVote::kRefight ? Vote::kRefight : Vote::kChange;
            Expect(o == want, "the table of the two picks");
        }
        Expect(RefightVote::Of(Locked(a), none) == (a == RefightVote::kLeave ? Vote::kLeave : Vote::kPending), "one pick alone decides only as a LEAVE");
        Expect(RefightVote::Of(none, Locked(a)) == (a == RefightVote::kLeave ? Vote::kLeave : Vote::kPending), "the other's pick alone likewise");
    }
    Expect(RefightVote::Of(none, none) == Vote::kPending, "no pick: nothing decided");
    // A value no build sends (a stage entry, "random") is no pick.
    Expect(RefightVote::Of(Locked(5), Locked(RefightVote::kRefight)) == Vote::kPending, "a number that is no choice is no pick");
    Expect(RefightVote::Of(Locked(kRandom), Locked(RefightVote::kLeave)) == Vote::kLeave, "a LEAVE stands next to a number that is no choice");
    Side unlocked = Locked(RefightVote::kLeave);
    unlocked.locked = false;
    Expect(RefightVote::Of(unlocked, none) == Vote::kPending, "a cursor on LEAVE is no LEAVE");
    for (uint8_t c = 0; c < 6; ++c) Expect(RefightVote::ChoiceText(c) && RefightVote::ChoiceText(c)[0], "every number has a text");
    Expect(strlen(RefightVote::ChoiceText(RefightVote::kChange)) < 40, "the longest row's text fits a name field");
}

// MK9's own box as its script MKOnlineEngine_RematchScreen reads one player's pad (RefightVote.h has the reading):
// state 1 undecided, 2 declined, 3 accepted, 4 shown as declined; a press is a button that was up the tick before.
struct Box {
    int state = 1;
    uint32_t prev = 0;
    bool reads = true;  // false: the box is still loading its picture and reads nothing
    int accepts = 0, declines = 0;
    void Tick(uint32_t mask) {
        const uint32_t pressed = mask & ~prev;
        prev = mask;
        if (!reads || state == 2 || state == 4) return;
        if (pressed & RefightVote::kAcceptButton) {
            ++accepts;
            if (state == 1) state = 3;
        } else if (pressed & RefightVote::kDeclineButton) {
            ++declines;
            state = state == 1 ? 2 : 1;  // from "accepted" the first press only takes the accept back
        }
    }
};

void Presses() {
    using RefightVote::Presser;
    {  // an accept: the confirm button for kHoldTicks ticks, then nothing until the round is made again
        Presser p;
        p.Accept();
        bool ok = true;
        for (int t = 0; t < RefightVote::kAgainTicks * 3; ++t) {
            const uint32_t m = p.Tick();
            const int in = t % RefightVote::kAgainTicks;
            ok = ok && m == (in < RefightVote::kHoldTicks ? RefightVote::kAcceptButton : 0u);
        }
        Expect(ok, "an accept is 'confirm' held three ticks, once per round");
        Expect(p.presses == 3, "three rounds, three presses");
    }
    {  // a leave: 'back' twice with a gap, per round; never 'confirm'
        Presser p;
        p.Leave();
        bool ok = true, sawAccept = false;
        const int second = RefightVote::kHoldTicks + RefightVote::kGapTicks;
        for (int t = 0; t < RefightVote::kAgainTicks * 2; ++t) {
            const uint32_t m = p.Tick();
            const int in = t % RefightVote::kAgainTicks;
            const bool down = in < RefightVote::kHoldTicks || (in >= second && in < second + RefightVote::kHoldTicks);
            ok = ok && m == (down ? RefightVote::kDeclineButton : 0u);
            sawAccept = sawAccept || (m & RefightVote::kAcceptButton);
        }
        Expect(ok, "a leave is 'back' twice per round, with ticks of nothing between");
        Expect(!sawAccept, "a leave never presses 'confirm'");
        Expect(p.presses == 4, "two rounds, four presses");
    }
    {  // nothing wanted, nothing pressed
        Presser p;
        bool any = false;
        for (int t = 0; t < 300; ++t) any = any || p.Tick();
        Expect(!any, "no pick: nothing is pressed");
    }
    {  // the budget: at most kMaxPresses, then nothing for good
        Presser p;
        p.Accept();
        int downs = 0;
        uint32_t prev = 0;
        for (int t = 0; t < RefightVote::kAgainTicks * (RefightVote::kMaxPresses + 3); ++t) {
            const uint32_t m = p.Tick();
            if (m && !prev) ++downs;
            prev = m;
        }
        Expect(downs == RefightVote::kMaxPresses, "an accept is pressed kMaxPresses times at the most");
        Expect(p.done && !p.Tick(), "and then nothing more");
        p.Leave();  // a leave after that still has its presses
        Box b;
        b.state = 3;
        for (int t = 0; t < 60; ++t) b.Tick(p.Tick());
        Expect(b.state == 2, "a leave after a used-up accept still declines");
    }
    {  // Accept twice is one accept; Accept after Leave is nothing
        Presser p;
        p.Accept();
        for (int t = 0; t < 20; ++t) p.Tick();
        const int tick = p.tick;
        p.Accept();
        Expect(p.tick == tick && p.want == 1, "a second accept does not begin again");
        p.Leave();
        p.Accept();
        Expect(p.want == 2, "a leave is never turned into an accept");
        p.Reset();
        Expect(!p.want && !p.Tick(), "a reset presser presses nothing");
    }
    // Against the box: every moment of the pick, a box that begins to read late, a leave after an accept.
    for (int late = 0; late <= 400; late += 7) {
        {
            Box b;
            b.reads = false;
            Presser p;
            p.Accept();
            for (int t = 0; t < 700; ++t) {
                if (t == late) b.reads = true;
                b.Tick(p.Tick());
            }
            Expect(b.state == 3, "an accept reaches a box that reads late");
            Expect(b.declines == 0, "an accept never declines");
        }
        {
            Box b;
            b.reads = false;
            Presser p;
            p.Leave();
            for (int t = 0; t < 700; ++t) {
                if (t == late) b.reads = true;
                b.Tick(p.Tick());
            }
            Expect(b.state == 2, "a leave reaches a box that reads late");
            Expect(b.accepts == 0, "a leave never accepts");
        }
        {  // accepted first (the pick RE-FIGHT or CHANGE), then "back" on the panel 'late' ticks later
            Box b;
            Presser p;
            p.Accept();
            for (int t = 0; t < 700; ++t) {
                if (t == late) p.Leave();
                b.Tick(p.Tick());
            }
            Expect(b.state == 2, "a leave after an accept declines: the first 'back' takes the accept back, the second declines");
        }
    }
    // A press begun in the tick a leave replaces an accept: the box must still see a release before the first 'back'
    // counts or not, and end declined either way (checked above for every 'late'); and the two buttons are never down
    // in one tick.
    {
        Presser p;
        p.Accept();
        bool both = false;
        for (int t = 0; t < 400; ++t) {
            if (t == 1) p.Leave();
            const uint32_t m = p.Tick();
            both = both || m == (RefightVote::kAcceptButton | RefightVote::kDeclineButton);
        }
        Expect(!both, "the two buttons are never down together");
    }
}

// ---- two games' panels over a link ----
struct Game {
    Exchange x;
    uint8_t pick = RefightVote::kRefight;
    int openAt = 0, lockAt = 0, leaveAt = -1;  // leaveAt: "back" after the pick (-1: never)
    int sentAt = -100000;
    bool opened = false, locked = false, left = false, soon = false;
    uint32_t serial = 0;
};
struct Wire {
    int at;
    bool toA;
    uint8_t b[kSize];
};

void Step(Game& g, int now, uint32_t nonceSeed) {
    if (!g.opened && now >= g.openAt) {
        g.opened = true;
        g.serial = 1000 + nonceSeed % 50;
        g.x.Open(kKindAfterMatch, RefightVote::kContext, detail::Mix(nonceSeed) | 1, g.serial);
        g.x.MoveCursor(RefightVote::kRefight);
    }
    if (!g.opened) return;
    if (!g.locked && now >= g.lockAt) {
        g.locked = true;
        g.x.Lock(g.pick);
        g.soon = true;
    }
    if (g.locked && !g.left && g.leaveAt >= 0 && now >= g.leaveAt && g.x.mine.choice != RefightVote::kLeave) {
        g.left = true;
        RefightVote::Reopen(&g.x, detail::Mix(nonceSeed + 0x1234567u) | 1, ++g.serial);
        g.x.Lock(RefightVote::kLeave);
        g.soon = true;
    }
}

void Exchanges() {
    int runs = 0, agreedBoth = 0, agreedOne = 0, leaves = 0;
    for (int run = 0; run < 6000; ++run) {
        Game a, b;
        a.pick = (uint8_t)(Rnd() % RefightVote::kChoices);
        b.pick = (uint8_t)(Rnd() % RefightVote::kChoices);
        if (run % 3 == 0) a.pick = b.pick = RefightVote::kRefight;  // the case the mod acts on, often
        a.openAt = Between(0, 300);
        b.openAt = Between(0, 300);
        a.lockAt = a.openAt + Between(50, 4000);
        b.lockAt = b.openAt + Between(50, 4000);
        if (run % 5 == 1) a.leaveAt = a.lockAt + Between(100, 3000);
        if (run % 11 == 3) b.leaveAt = b.lockAt + Between(100, 3000);
        const int lossPct = run % 4 == 0 ? 0 : Between(0, 60), maxDelay = Between(1, 250);
        std::vector<Wire> wires;
        const uint32_t seedA = Rnd(), seedB = Rnd();
        const int endAt = 12000;
        for (int now = 0; now <= endAt; now += 16) {
            Step(a, now, seedA);
            Step(b, now, seedB);
            for (int side = 0; side < 2; ++side) {
                Game& g = side ? b : a;
                if (!g.opened || now - g.sentAt < (g.soon ? 30 : 100)) continue;
                g.soon = false;
                g.sentAt = now;
                Wire w;
                w.toA = side == 1;
                w.at = now + Between(1, maxDelay);
                Build(g.x.Next(), w.b, sizeof w.b);
                if (Between(0, 99) >= lossPct) wires.push_back(w);
            }
            for (size_t i = 0; i < wires.size();) {
                if (wires[i].at > now) {
                    ++i;
                    continue;
                }
                Packet p;
                if (Parse(wires[i].b, kSize, &p) == Parsed::kOk) (wires[i].toA ? a : b).x.OnPacket(p);
                wires[i] = wires.back();
                wires.pop_back();
            }
        }
        ++runs;
        const uint8_t finalA = a.left ? (uint8_t)RefightVote::kLeave : a.pick, finalB = b.left ? (uint8_t)RefightVote::kLeave : b.pick;
        const bool refight = finalA == RefightVote::kRefight && finalB == RefightVote::kRefight;
        const bool agA = RefightVote::RefightAgreed(a.x), agB = RefightVote::RefightAgreed(b.x);
        // Never acted on unless both really chose it at the end.
        Expect(!agA || refight, "game A acts on a re-fight only when both chose it");
        Expect(!agB || refight, "game B acts on a re-fight only when both chose it");
        // What each side holds of the other is the other's own pick, never something else.
        if (a.x.theirs.locked) Expect(a.x.theirs.choice == b.pick || a.x.theirs.choice == finalB, "A holds a pick B made");
        if (b.x.theirs.locked) Expect(b.x.theirs.choice == a.pick || b.x.theirs.choice == finalA, "B holds a pick A made");
        // A side that is confirmed reads the two picks as the other side would with the same packets.
        if (a.x.Confirmed() && b.x.Confirmed()) Expect(RefightVote::Of(a.x.mine, a.x.theirs) == RefightVote::Of(b.x.mine, b.x.theirs), "two confirmed games read the pair alike");
        // Over a link that loses nothing, both know within the run.
        if (lossPct == 0) {
            Expect(a.x.Confirmed() && b.x.Confirmed(), "a link that loses nothing confirms both games");
            Expect(agA == refight && agB == refight, "and both act on a re-fight exactly when both chose it");
            if (a.left || b.left) Expect(RefightVote::Of(a.x.mine, a.x.theirs) == Vote::kLeave && RefightVote::Of(b.x.mine, b.x.theirs) == Vote::kLeave, "a LEAVE after a pick reaches both games");
        }
        if (agA && agB) ++agreedBoth;
        else if (agA || agB) ++agreedOne;
        if (finalA == RefightVote::kLeave || finalB == RefightVote::kLeave) ++leaves;
    }
    printf("note: %d exchanges, both games act on a re-fight in %d, one alone in %d (its confirmation was lost; the stage picks then settle the arena), a LEAVE in %d\n",
           runs, agreedBoth, agreedOne, leaves);
    Expect(agreedBoth > 500, "the re-fight both chose is reached often enough to have been tested");
}

void OtherKinds() {
    // The two kinds share the packet and never mix: a stage exchange drops a choice packet and the other way round.
    Exchange stage, vote;
    stage.Open(kKindStage, 0x1111, 5, 10);
    vote.Open(kKindAfterMatch, RefightVote::kContext, 6, 11);
    vote.Lock(RefightVote::kRefight);
    stage.Lock(3);
    uint8_t b[kSize];
    Packet p;
    Build(vote.Next(), b, sizeof b);
    Expect(IsPacket(b, sizeof b), "a choice packet carries the picks' magic");
    Expect(Parse(b, sizeof b, &p) == Parsed::kOk && p.kind == kKindAfterMatch, "a choice packet parses as kind 2");
    Expect(stage.OnPacket(p) == Exchange::Heard::kIgnored && !stage.theirs.nonce && !stage.contextDiffers, "a stage exchange drops it");
    Build(stage.Next(), b, sizeof b);
    Expect(Parse(b, sizeof b, &p) == Parsed::kOk && p.kind == kKindStage, "a stage packet parses as kind 1");
    Expect(vote.OnPacket(p) == Exchange::Heard::kIgnored && !vote.theirs.nonce && !vote.contextDiffers, "a choice exchange drops it");
    // Another set of choices on the other side: nothing of theirs is used.
    Exchange other;
    other.Open(kKindAfterMatch, RefightVote::kContext + 1, 9, 12);
    other.Lock(RefightVote::kRefight);
    Build(other.Next(), b, sizeof b);
    Parse(b, sizeof b, &p);
    Expect(vote.OnPacket(p) == Exchange::Heard::kContextDiffers && vote.contextDiffers, "another context is told apart");
    Expect(!RefightVote::RefightAgreed(vote) && RefightVote::Of(vote.mine, vote.theirs) == Vote::kPending, "and gives no outcome");
    // Reopen keeps what is known of the other side and voids what it had heard of this one.
    Exchange a, c;
    a.Open(kKindAfterMatch, RefightVote::kContext, 21, 100);
    c.Open(kKindAfterMatch, RefightVote::kContext, 22, 200);
    a.Lock(RefightVote::kRefight);
    c.Lock(RefightVote::kRefight);
    for (int i = 0; i < 3; ++i) {
        Build(a.Next(), b, sizeof b);
        Parse(b, sizeof b, &p);
        c.OnPacket(p);
        Build(c.Next(), b, sizeof b);
        Parse(b, sizeof b, &p);
        a.OnPacket(p);
    }
    Expect(RefightVote::RefightAgreed(a) && RefightVote::RefightAgreed(c), "two RE-FIGHTs over a clean link: both act on it");
    RefightVote::Reopen(&a, 23, 101);
    Expect(a.open && !a.mine.locked && a.theirs.locked && a.theirs.nonce == 22 && !a.echoed.locked, "a reopened exchange keeps the other's pick, not its echo");
    Expect(!RefightVote::RefightAgreed(a), "and is not agreed any more");
    a.Lock(RefightVote::kLeave);
    Expect(RefightVote::Of(a.mine, a.theirs) == Vote::kLeave, "the LEAVE decides at once on its own side");
    Build(a.Next(), b, sizeof b);
    Parse(b, sizeof b, &p);
    Expect(c.OnPacket(p) == Exchange::Heard::kTaken && c.theirs.choice == RefightVote::kLeave, "the other game takes the later exchange");
    Expect(!RefightVote::RefightAgreed(c) && RefightVote::Of(c.mine, c.theirs) == Vote::kLeave, "and no longer acts on a re-fight");
    // A leftover packet of the earlier exchange arrives after it: dropped.
    Exchange old;
    old.Open(kKindAfterMatch, RefightVote::kContext, 21, 100);
    old.Lock(RefightVote::kRefight);
    Build(old.Next(), b, sizeof b);
    Parse(b, sizeof b, &p);
    c.OnPacket(p);
    Expect(c.theirs.choice == RefightVote::kLeave, "a leftover of the earlier exchange does not bring the RE-FIGHT back");
}
}  // namespace

int main() {
    Rule();
    Presses();
    OtherKinds();
    Exchanges();
    printf("refightvote: %d checks, %d failed\n", gChecks, gFailed);
    if (!gFailed) printf("ALL CHECKS PASSED\n");
    return gFailed ? 1 : 0;
}
