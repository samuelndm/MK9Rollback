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
    // Repeating(): from the first press of the second round on (the presses MK9's box sounds again), never before.
    {
        Presser p;
        Expect(!p.Repeating(), "no pick: not repeating");
        p.Accept();
        bool early = false, later = true;
        for (int t = 0; t < RefightVote::kAgainTicks * 4; ++t) {
            p.Tick();
            if (t < RefightVote::kAgainTicks) early = early || p.Repeating();
            else later = later && p.Repeating();
        }
        Expect(!early, "an accept's first press is not a repeat (its sound is the player's choice)");
        Expect(later, "an accept's later presses are repeats, from the second round's first tick on");
        p.Leave();
        bool leaveEarly = false, leaveLater = true;
        for (int t = 0; t < RefightVote::kAgainTicks * 3; ++t) {
            p.Tick();
            if (t < RefightVote::kAgainTicks) leaveEarly = leaveEarly || p.Repeating();
            else leaveLater = leaveLater && p.Repeating();
        }
        Expect(!leaveEarly, "a leave's two first presses are not repeats");
        Expect(leaveLater, "a leave's later rounds are repeats");
        Presser q;
        q.Accept();
        for (int t = 0; t < RefightVote::kAgainTicks * (RefightVote::kMaxPresses + 2); ++t) q.Tick();
        Expect(q.done && !q.Repeating(), "a used-up presser presses nothing, and nothing is held back for it");
        q.Reset();
        Expect(!q.Repeating(), "a reset presser is not repeating");
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

// The same fighters again: which names are a fighter's, the pair's number, and the stage picks' context of a game
// that holds the pair.
void Fighters() {
    using RefightVote::DirectContext;
    using RefightVote::FighterName;
    using RefightVote::PairTag;
    // The names of the field log of 2026-11-14: a costume letter, another one, and the first costume in small letters.
    Expect(FighterName("CHAR_JohnnyCage_B") && FighterName("CHAR_Sektor_C") && FighterName("char_raiden") && FighterName("CHAR_Cyrax_C"),
           "the fighters of a real evening are fighters' names");
    Expect(!FighterName(nullptr) && !FighterName("") && !FighterName("char_") && !FighterName("CHAR"), "nothing, or the start alone, is no fighter");
    Expect(!FighterName("BGND_Pit") && !FighterName("random") && !FighterName("empty_map"), "an arena, \"random\" and the empty map are no fighters");
    Expect(!FighterName("CHAR_Johnny Cage") && !FighterName("CHAR_Raiden\n") && !FighterName("CHAR_R\x01"), "a name with anything but letters, digits and '_' is none");
    char longName[80];
    memset(longName, 'a', sizeof longName);
    memcpy(longName, "char_", 5);
    longName[62] = 0;
    Expect(FighterName(longName), "62 characters still fit the game's 64 bytes");
    longName[62] = 'a', longName[63] = 0;
    Expect(FighterName(longName), "63 too");
    longName[63] = 'a', longName[64] = 0;
    Expect(!FighterName(longName), "64 do not");

    const uint32_t t = PairTag("CHAR_JohnnyCage_B", "CHAR_Sektor_C");
    Expect(t != 0 && PairTag("", "") != 0 && PairTag(nullptr, nullptr) != 0, "a pair's number is never 0");
    Expect(t == PairTag("char_johnnycage_b", "CHAR_SEKTOR_C"), "letter case does not change it");
    Expect(t != PairTag("CHAR_Sektor_C", "CHAR_JohnnyCage_B"), "the two players' order does");
    Expect(t != PairTag("CHAR_JohnnyCage_B", "CHAR_Sektor_B") && t != PairTag("CHAR_JohnnyCage", "CHAR_Sektor_C"), "a costume does");
    Expect(PairTag("char_ab", "c") != PairTag("char_a", "bc"), "the names' ends count");
    // Every pair of the evening's seven fighters in both orders: 49 different numbers.
    static const char* const names[] = {"CHAR_JohnnyCage_B", "CHAR_Sektor_C", "CHAR_Raiden_B", "char_raiden", "CHAR_Cyrax_C", "CHAR_Scorpion", "CHAR_SubZero_B"};
    std::vector<uint32_t> tags;
    bool distinct = true;
    for (const char* a : names)
        for (const char* b : names) {
            const uint32_t x = PairTag(a, b);
            // (char_raiden and CHAR_Raiden_B differ; a name against itself in another letter case would not.)
            for (uint32_t y : tags) distinct = distinct && x != y;
            tags.push_back(x);
        }
    Expect(distinct && tags.size() == 49, "49 pairs give 49 numbers");

    const uint32_t table = 0x5A17C0DEu;
    const uint32_t d = DirectContext(table, t);
    Expect(d != table && d == DirectContext(table, t), "a game with the pair names another context than one without, the same every time");
    Expect(d != DirectContext(table, PairTag("CHAR_JohnnyCage_B", "CHAR_Raiden_B")), "another pair, another context");
    Expect(d != DirectContext(table ^ 1, t), "another stage table, another context");
    bool never = true;
    for (uint32_t i = 0; i < 200000; ++i) never = never && DirectContext(i * 0x9E3779B9u, i + 1) != i * 0x9E3779B9u;
    Expect(never, "never the table's own context");

    // What the context does: two games that hold the same pair read each other's arena pick; a game with the pair and
    // one without, or two with different pairs, read nothing of each other and both know it.
    const auto meet = [&](uint32_t ctxA, uint32_t ctxB, bool* differA, bool* differB) {
        Exchange a, c;
        a.Open(kKindStage, ctxA, 31, 200);
        c.Open(kKindStage, ctxB, 32, 300);
        a.Lock(7);
        c.Lock(7);
        uint8_t b[kSize];
        Packet p;
        for (int i = 0; i < 4; ++i) {
            Build(a.Next(), b, sizeof b);
            Parse(b, sizeof b, &p);
            c.OnPacket(p);
            Build(c.Next(), b, sizeof b);
            Parse(b, sizeof b, &p);
            a.OnPacket(p);
        }
        *differA = a.contextDiffers;
        *differB = c.contextDiffers;
        return a.Confirmed() && c.Confirmed();
    };
    bool da = false, db = false;
    Expect(meet(d, d, &da, &db) && !da && !db, "two games with the same pair agree on the arena as ever");
    Expect(!meet(d, table, &da, &db) && da && db, "a game with the pair and one without: no agreement, and both see why");
    Expect(!meet(d, DirectContext(table, t + 1), &da, &db) && da && db, "two games with different pairs: the same");

    // ... and the verdict on such an exchange: it is opened under the pair's context, so it is judged against the
    // pair's context. v20261114-1451 judged it against the table's own: on 2026-11-14 17:18:55 two games that held the
    // same pair and had locked the same arena each read "the two games list different stages", took the pair back and
    // loaded the select screen's fighters on the game's own arena.
    using RefightVote::ExchangeContext;
    Table tb;
    tb.count = 34;
    for (uint8_t i = 0; i < tb.count; ++i) {
        char n[16];
        snprintf(n, sizeof n, "BGND_%u", (unsigned)i);
        tb.nameHash[i] = NameHash(n);
        tb.selectable[i] = i >= 1 && i <= 28;
    }
    Expect(ExchangeContext(tb.Context(), 0) == tb.Context() && ExchangeContext(tb.Context(), t) == DirectContext(tb.Context(), t),
           "a stage exchange's context: the table's own without a pair, the pair's with one");
    Verdict va, vb, old;
    const auto judged = [&](uint32_t tagA, uint32_t tagB) {
        Exchange a, c;
        a.Open(kKindStage, ExchangeContext(tb.Context(), tagA), 41, 500);
        c.Open(kKindStage, ExchangeContext(tb.Context(), tagB), 42, 600);
        a.Lock(11);
        c.Lock(11);
        uint8_t b[kSize];
        Packet p;
        for (int i = 0; i < 4; ++i) {
            Build(a.Next(), b, sizeof b);
            Parse(b, sizeof b, &p);
            c.OnPacket(p);
            Build(c.Next(), b, sizeof b);
            Parse(b, sizeof b, &p);
            a.OnPacket(p);
        }
        va = Judge(a, tb, true, ExchangeContext(tb.Context(), tagA));
        vb = Judge(c, tb, true, ExchangeContext(tb.Context(), tagB));
        old = Judge(a, tb, true);  // against the table's own context
    };
    const auto stage11 = [](const Verdict& v) { return v.why == Why::kAgreed && v.outcome == Outcome::kStage && v.entry == 11; };
    judged(t, t);
    Expect(stage11(va) && stage11(vb), "two games with the same pair and the same arena: both verdicts are that arena");
    Expect(old.why == Why::kContextDiffers && old.outcome == Outcome::kFallback,
           "the same exchange judged against the table's own context is refused: the fault of v20261114-1451");
    judged(t, 0);
    Expect(va.why == Why::kContextDiffers && vb.why == Why::kContextDiffers, "a game with the pair and one without: no arena for either");
    judged(t, t + 1);
    Expect(va.why == Why::kContextDiffers && vb.why == Why::kContextDiffers, "two games with different pairs: no arena for either");
    judged(0, 0);
    Expect(stage11(va) && stage11(vb) && stage11(old), "without a pair nothing changed: the stage screen's verdict as ever");
}

// ---- Kombat Kodes: the game's session as its code reads (RefightVote.h), and the mod's rule on top of it ----
struct KodeMsg {
    int id, due;
    uint32_t data;
};
struct KodeGame {
    bool host = false;
    uint16_t mine = 0, theirs = 0;  // the session's +0x4C34, +0x4C36
    uint32_t locked = 0;            // +0x4C38
    int box[6] = {0, 0, 0, 0, 0, 0};  // the versus screen's six digits: the host's three, then the other player's
    std::vector<KodeMsg> out;
    int sent13 = 0;
    // the mod on this game
    bool mod = false, stopped = false;
    RefightVote::Kodes kept;
    int writes = 0;

    void Reset() { mine = theirs = 0, locked = 0; }  // 0x008809D0
    void SetLocal(const int d[3], int now) {         // 0x00880790
        if (locked) return;
        const uint16_t w = (uint16_t)(((d[0] & 0xF) << 8) | ((d[1] & 0xF) << 4) | (d[2] & 0xF));
        if (w == mine) return;
        mine = w;
        out.push_back({13, now + Between(0, 5), w});
        ++sent13;
    }
    void Lockdown(int now) {  // 0x008808C0: the host's script, when its load ends
        if (!host) return;
        locked = 1;
        out.push_back({14, now + Between(0, 5), ((uint32_t)mine << 16) | theirs});
    }
    void Take(const KodeMsg& m) {
        if (m.id == 13) {  // 0x00880910
            if (!locked) theirs = (uint16_t)m.data;
        } else {  // 0x00880970: the first word is this game's player's, the second the sender's
            mine = (uint16_t)m.data;
            theirs = (uint16_t)(m.data >> 16);
            locked = 1;
        }
    }
    // The versus screen's tick 0x009273A0. press: 0 none, 1-3 that digit up, -1..-3 that digit down.
    void VersusTick(int press, int now) {
        int d[3];
        for (int i = 0; i < 3; ++i) d[i] = box[(host ? 0 : 3) + i];
        if (press > 0) d[press - 1] = (d[press - 1] + 1) % 10;
        if (press < 0) d[-press - 1] = (d[-press - 1] + 9) % 10;
        SetLocal(d, now);
        const uint16_t first = host ? mine : theirs, second = host ? theirs : mine;  // 0x00880820
        for (int i = 0; i < 3; ++i) {
            box[i] = (first >> (8 - 4 * i)) & 0xF;
            box[3 + i] = (second >> (8 - 4 * i)) & 0xF;
        }
    }
    void ModTick() {  // Refight.cpp, KeepKodes
        if (!mod || stopped) return;
        switch (RefightVote::KodeTick(RefightVote::Kodes{mine, theirs, locked != 0}, kept)) {
        case RefightVote::KodeStep::kHold:
            return;
        case RefightVote::KodeStep::kTheirs:
            stopped = true;
            return;
        case RefightVote::KodeStep::kWrite:
            if (writes >= 8) return;
            mine = kept.mine, theirs = kept.theirs, locked = 1;
            ++writes;
        }
    }
};
// One load of a re-fight: both versus screens start (and zero their sessions) a few ticks in, both players press
// blind, the host locks at tick 240, and every message arrives up to five ticks late.
void KodeLoad(KodeGame& h, KodeGame& c, bool presses) {
    h.host = true;
    const int startH = Between(0, 6), startC = Between(0, 6);
    for (int now = 0; now < 300; ++now) {
        for (KodeGame* g : {&h, &c}) {
            KodeGame* o = g == &h ? &c : &h;
            for (size_t i = 0; i < o->out.size();) {
                if (o->out[i].due > now) {
                    ++i;
                    continue;
                }
                g->Take(o->out[i]);
                o->out.erase(o->out.begin() + (long)i);
            }
        }
        h.ModTick();
        c.ModTick();
        if (now == startH) h.Reset();
        if (now == startC) c.Reset();
        if (now >= startH) h.VersusTick(presses && Rnd() % 7 == 0 ? Between(-3, 3) : 0, now);
        if (now >= startC) c.VersusTick(presses && Rnd() % 7 == 0 ? Between(-3, 3) : 0, now);
        if (now == 240) h.Lockdown(now);
    }
}
void KodeChecks() {
    using namespace RefightVote;
    Expect(KodeValid(0x000) && KodeValid(0x999) && KodeValid(0x123), "three digits are a kode");
    Expect(!KodeValid(0x1000) && !KodeValid(0x0A0) && !KodeValid(0x00F) && !KodeValid(0xB00), "a fourth digit or a hex digit is not");
    char t[6];
    KodeText(0x123, t);
    Expect(!strcmp(t, "1-2-3"), "a kode's text");
    KodeText(0x0A0, t);
    Expect(!strcmp(t, "?-?-?"), "the text of a word that is no kode");
    const Kodes k = KodesOf(0x04560123u, 1);
    Expect(k.mine == 0x123 && k.theirs == 0x456 && k.locked && KodeWords(k) == 0x04560123u, "the session's two words, there and back");
    Expect(KodesKeepable(k) && !KodesKeepable(KodesOf(0x04560123u, 0)) && !KodesKeepable(KodesOf(0x0A000123u, 1)), "kept: locked, three digits each");
    Expect(KodeTick(KodesOf(0, 0), k) == KodeStep::kWrite && KodeTick(KodesOf(0x04560123u, 0), k) == KodeStep::kWrite, "not locked: written");
    Expect(KodeTick(k, k) == KodeStep::kHold, "the kept kodes, locked: nothing to do");
    Expect(KodeTick(KodesOf(0x04560124u, 1), k) == KodeStep::kTheirs && KodeTick(KodesOf(0, 1), k) == KodeStep::kTheirs, "locked with others: left alone");

    const auto same = [](const KodeGame& h, const KodeGame& c) {
        return h.locked && c.locked && h.mine == c.theirs && h.theirs == c.mine && !memcmp(h.box, c.box, sizeof h.box);
    };
    for (int run = 0; run < 400; ++run) {
        const uint16_t kh = (uint16_t)((Between(0, 9) << 8) | (Between(0, 9) << 4) | Between(0, 9));
        const uint16_t kc = (uint16_t)((Between(0, 9) << 8) | (Between(0, 9) << 4) | Between(0, 9));
        {  // both mods hold the last fight's kodes: both games load with them, whatever is pressed
            KodeGame h, c;
            h.mod = c.mod = true;
            h.kept = Kodes{kh, kc, true};
            c.kept = Kodes{kc, kh, true};
            KodeLoad(h, c, true);
            Expect(same(h, c) && h.mine == kh && h.theirs == kc, "both mods: the last fight's kodes on both games");
            Expect(h.box[0] == ((kh >> 8) & 0xF) && h.box[2] == (kh & 0xF) && h.box[3] == ((kc >> 8) & 0xF) && h.box[5] == (kc & 0xF),
                   "both mods: the versus screen holds the last fight's six digits");
            // (A press in the very tick the versus screen zeroes its session travels, and changes nothing: the next
            // tick writes the kept kodes again, and a locked game does not take it.)
            Expect(h.sent13 <= 1 && c.sent13 <= 1, "both mods: no blind press travels after the versus screen's own start");
            Expect(h.writes >= 1 && h.writes <= 2 && c.writes >= 1 && c.writes <= 2 && !h.stopped && !c.stopped,
                   "both mods: one write, and one more after the versus screen's own start");
        }
        {  // the same without a press
            KodeGame h, c;
            h.mod = c.mod = true;
            h.kept = Kodes{kh, kc, true};
            c.kept = Kodes{kc, kh, true};
            KodeLoad(h, c, false);
            Expect(same(h, c) && h.mine == kh && h.theirs == kc, "both mods, nothing pressed: the last fight's kodes on both games");
        }
        {  // only the host's mod holds them: the host's two words reach the other game
            KodeGame h, c;
            h.mod = true;
            h.kept = Kodes{kh, kc, true};
            KodeLoad(h, c, true);
            Expect(same(h, c) && h.mine == kh && h.theirs == kc, "the host's mod alone: its kodes on both games");
        }
        {  // only the other game's mod holds them: the host's lock rules, the two games still end alike
            KodeGame h, c;
            c.mod = true;
            c.kept = Kodes{kc, kh, true};
            KodeLoad(h, c, true);
            Expect(same(h, c), "the other game's mod alone: both games end with the host's six digits");
            Expect(c.writes <= 2, "the other game's mod alone: it does not write over the host's lock");
        }
        {  // the two mods hold different kodes (never seen; a fault somewhere): the host's rule, both games alike
            KodeGame h, c;
            h.mod = c.mod = true;
            h.kept = Kodes{kh, kc, true};
            c.kept = Kodes{(uint16_t)(kc ^ 1), kh, true};
            KodeLoad(h, c, true);
            Expect(same(h, c) && h.mine == kh && h.theirs == kc, "two mods that differ: the host's kodes on both games");
            Expect(c.stopped || c.kept.mine == kc, "two mods that differ: the other game's mod steps back");
        }
        {  // no mod: MK9 as it is (the model's own check)
            KodeGame h, c;
            KodeLoad(h, c, true);
            Expect(same(h, c), "no mod: both games end with the same six digits");
        }
    }
}
}  // namespace

int main() {
    KodeChecks();
    Fighters();
    Rule();
    Presses();
    OtherKinds();
    Exchanges();
    printf("refightvote: %d checks, %d failed\n", gChecks, gFailed);
    if (!gFailed) printf("ALL CHECKS PASSED\n");
    return gFailed ? 1 : 0;
}
