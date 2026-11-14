// Host test of src\net\StageHold.h: the hold of the stage screen that opens by itself, and the rule for writing the
// drawn arena into both games. No game. The two simulated games below follow the timing StageFlow.cpp uses (a question
// of the script every tick, a packet every 100 ms and 30 ms after a lock, the decision asked once when the panel gives
// the game back); the hold, the exchange, the draw and the decision are the mod's own code.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "StageHold.h"
#include "StagePick.h"

using namespace StagePick;

namespace {
int gFailed = 0, gChecks = 0;
void Expect(bool ok, const char* what) {
    ++gChecks;
    if (!ok && ++gFailed <= 20) printf("FAILED: %s\n", what);
}
uint32_t gRng = 0x2C9277B5u;
uint32_t Rnd() {
    gRng = detail::Mix(gRng + 0x9E3779B9u);
    return gRng;
}
int Between(int lo, int hi) { return lo + (int)(Rnd() % (uint32_t)(hi - lo + 1)); }

constexpr int kTickMs = 16, kSendMs = 100, kSoonMs = 30, kRouletteMs = 1800, kResultMs = 1600, kLetGoMs = 48, kSureWaitMs = 1500;
constexpr int kTrialPickMs = 5000, kTrialGraceMs = 3000, kHeldPickMs = 8000, kHeldGraceMs = 1500;
constexpr uint32_t kMaxMs = kHeldPickMs + kHeldGraceMs + kRouletteMs + kResultMs + kSureWaitMs + 2000;

struct Game {
    Exchange x;
    StageHold::Hold h;
    Table t;
    int openAt = 0, lockAfter = 0, askFrom = 0;  // when the panel opens, when the player locks (after opening), from when the script asks
    bool asks = true;                            // this game's script asks for sync wait 2 at all
    uint8_t pick = kRandom;
    int pickMs = kTrialPickMs, graceMs = kTrialGraceMs;
    int readyAt = -1, sentAt = -100000, doneAt = -1;
    bool opened = false, soon = false, done = false, ended = false, theirsApplyAtDone = false;
    StageHold::Use use = StageHold::Use::kNoStage;
    uint8_t wrote = kNoChoice;
    uint32_t heldAnswers = 0, ownAnswers = 0;
};
struct Wire {
    int at;
    bool toA;
    uint8_t b[kSize];
};
struct Link {
    int kind = 0;       // 0: loses packets, never more than 'burst' in a row each way; 1: loses any number; 2: dies
    int lossPct = 0, burst = 3, diesAt = 1 << 30;
    bool diesToA = true, diesToB = true;
    int lostRow[2] = {0, 0};
    std::vector<Wire> wires;
};

Table MakeTable() {
    Table t;
    t.count = 30;
    for (int i = 0; i < t.count; ++i) {
        t.nameHash[i] = detail::Mix(0xA5000000u + (uint32_t)i);
        t.selectable[i] = i != 7;
    }
    return t;
}

void Send(Link& l, Game& g, bool toA, int now) {
    const int d = toA ? 0 : 1;
    bool lost = false;
    if (l.kind == 2) lost = now >= l.diesAt && (toA ? l.diesToA : l.diesToB);
    else {
        lost = (int)(Rnd() % 100) < l.lossPct;
        if (l.kind == 0 && lost && l.lostRow[d] >= l.burst) lost = false;
    }
    l.lostRow[d] = lost ? l.lostRow[d] + 1 : 0;
    if (lost) return;
    Wire w;
    w.at = now + Between(4, 150);
    w.toA = toA;
    Build(g.x.Next(), w.b, sizeof w.b);
    l.wires.push_back(w);
    if (Rnd() % 16 == 0) {  // now and then twice, the second one later
        w.at += Between(1, 120);
        l.wires.push_back(w);
    }
}

void Step(Game& g, Link& l, bool isA, int now) {
    if (!g.opened) {
        if (now < g.openAt) return;
        g.opened = true;
        g.x.Open(kKindStage, g.t.Context(), Rnd() | 1, 1000 + (uint32_t)now);
        g.x.MoveCursor(kRandom);
    }
    if (g.ended) return;
    // The script's question, as RngTrace passes it on: the other game is long there, the game's own answer is "over".
    if (g.asks && now >= g.askFrom) {
        const bool was = g.h.on;
        const bool held = g.h.OnPoll(StageHold::kSyncType, StageHold::kAnswerOver, !g.done, (uint64_t)now, kMaxMs);
        if (g.h.on && !was) {
            g.pickMs = kHeldPickMs;
            g.graceMs = kHeldGraceMs;
            g.x.apply = true;
            g.soon = true;
        }
        if (held) ++g.heldAnswers;
        else ++g.ownAnswers;
        if (!held && g.h.over && !g.done) g.x.apply = false;  // the hold's own limit
    }
    if (!g.done) {
        const int lockAt = g.openAt + (g.lockAfter < g.pickMs ? g.lockAfter : g.pickMs);
        if (!g.x.mine.locked && now >= lockAt && g.x.Lock(g.pick)) g.soon = true;
    }
    if (now - g.sentAt >= (g.soon ? kSoonMs : kSendMs)) {
        g.soon = false;
        g.sentAt = now;
        Send(l, g, !isA, now);
    }
    if (g.done) {
        if (now >= g.doneAt + 6000) g.ended = true;  // "loaded": the exchange closes
        return;
    }
    const auto fallBack = [&] {
        g.h.End((uint64_t)now);
        g.x.Close();
        g.ended = true;
    };
    if (g.x.contextDiffers) return fallBack();
    if (g.h.Holding() && (uint64_t)now - g.h.since >= kMaxMs) return fallBack();
    if (g.x.Ready()) {
        const Verdict shown = Judge(g.x, g.t, false);
        if (shown.winner == Winner::kNone) return fallBack();
        if (g.readyAt < 0) g.readyAt = now;
        const int wait = (shown.winner == Winner::kBoth ? 0 : kRouletteMs) + kResultMs + kLetGoMs;
        if (now < g.readyAt + wait) return;
        // The other game's "I hold and know you have both picks" is missing: the panel waits a little longer for it.
        const bool awaits = g.h.Holding() && g.x.apply && g.x.theirsApply && shown.outcome == Outcome::kStage && !(g.x.Confirmed() && g.x.theirsSure);
        if (awaits && now < g.readyAt + wait + kSureWaitMs) return;
        const Verdict v = Judge(g.x, g.t, true);
        g.use = StageHold::Decide(g.h, g.x, v);
        g.theirsApplyAtDone = g.x.theirsApply;
        if (g.use == StageHold::Use::kWrite) g.wrote = v.entry;
        else g.x.apply = false;
        g.h.End((uint64_t)now);
        g.done = true;
        g.doneAt = now;
    } else if (now >= g.openAt + g.pickMs + g.graceMs) {
        fallBack();
    }
}

struct Tally {
    int trials = 0, bothWrote = 0, neither = 0, oneOnly = 0, differ = 0;
};
// One match start. Returns through the tally; the hard rules are checked in every trial.
void Trial(int kind, bool aAsks, bool bAsks, Tally* tally, bool sameTable = true) {
    Game a, b;
    a.t = b.t = MakeTable();
    if (!sameTable) b.t.selectable[3] = false;
    Link l;
    l.kind = kind;
    l.lossPct = kind == 0 ? Between(0, 45) : kind == 1 ? Between(50, 97) : 0;
    l.burst = Between(1, 4);
    if (kind == 2) {
        l.diesAt = Between(200, 14000);
        const int which = Between(0, 2);
        l.diesToA = which != 1;
        l.diesToB = which != 0;
    }
    a.openAt = Between(0, 40);
    b.openAt = Between(0, 220);  // the same frame of the select screen, as far apart as the two PCs' clocks of it
    a.asks = aAsks;
    b.asks = bAsks;
    a.askFrom = a.openAt + Between(0, 120);
    b.askFrom = b.openAt + Between(0, 120);
    const auto anyPick = [] {
        const int r = Between(0, 9);
        return (uint8_t)(r == 0 ? kRandom : r == 1 ? 5 : Between(0, 29) == 7 ? 8 : Between(8, 29));
    };
    a.pick = anyPick();
    b.pick = Between(0, 3) == 0 ? a.pick : anyPick();
    a.lockAfter = Between(0, 3) == 0 ? 100000 : Between(300, 9000);  // some never press: the countdown locks
    b.lockAfter = Between(0, 3) == 0 ? 100000 : Between(300, 9000);
    const int phaseA = Between(0, kTickMs - 1), phaseB = Between(0, kTickMs - 1);
    for (int now = 0; now < 23000; ++now) {
        for (size_t i = 0; i < l.wires.size();) {
            if (l.wires[i].at > now) {
                ++i;
                continue;
            }
            Packet p;
            Game& to = l.wires[i].toA ? a : b;
            if (to.opened && !to.ended && Parse(l.wires[i].b, kSize, &p) == Parsed::kOk) to.x.OnPacket(p);
            l.wires[i] = l.wires.back();
            l.wires.pop_back();
        }
        if (now % kTickMs == phaseA) Step(a, l, true, now);
        if (now % kTickMs == phaseB) Step(b, l, false, now);
    }
    const bool wa = a.wrote != kNoChoice, wb = b.wrote != kNoChoice;
    ++tally->trials;
    if (wa && wb) ++tally->bothWrote;
    else if (wa || wb) ++tally->oneOnly;
    else ++tally->neither;
    if (wa && wb && a.wrote != b.wrote) ++tally->differ;
    // Whatever the link did:
    Expect(!(wa && wb) || a.wrote == b.wrote, "two games that write, write the same arena");
    Expect(!wa || (a.h.on && a.theirsApplyAtDone && a.asks && b.asks), "a game writes only while it holds and the other said it holds");
    Expect(!wb || (b.h.on && b.theirsApplyAtDone && a.asks && b.asks), "a game writes only while it holds and the other said it holds (B)");
    Expect(!wa || (IsEntry(a.wrote) && a.t.Pickable(a.wrote)), "what is written is a stage the table offers");
    Expect(!wb || (IsEntry(b.wrote) && b.t.Pickable(b.wrote)), "what is written is a stage the table offers (B)");
    Expect(a.h.held <= StageHold::kMaxAnswers && b.h.held <= StageHold::kMaxAnswers, "the hold never answers more often than its limit");
    Expect(!a.h.on || (a.h.over && a.h.overAt - a.h.since <= kMaxMs + kTickMs), "a hold that began has ended, within its time");
    Expect(!b.h.on || (b.h.over && b.h.overAt - b.h.since <= kMaxMs + kTickMs), "a hold that began has ended, within its time (B)");
    Expect(a.asks || (!a.h.on && a.heldAnswers == 0), "a game whose script never asks is never held");
    if (!sameTable) Expect(!wa && !wb, "two games with different stage tables write nothing");
    if (!aAsks || !bAsks) Expect(!wa && !wb, "one game that does not hold: neither writes");
    if (kind == 0 && aAsks && bAsks && sameTable) {
        // A link that loses packets but never more than four in a row: both games come to the same end.
        Expect(wa == wb, "over a link that loses up to four packets in a row, both games write or neither does");
        Expect(a.done && b.done, "over such a link both panels come to their draw");
        if (a.done && b.done) {
            const Verdict va = Judge(a.x, a.t, true);
            Expect(va.outcome != Outcome::kStage || (wa && wb && a.wrote == va.entry), "an agreed stage is written by both");
            Expect(va.outcome != Outcome::kGameChoice || (!wa && !wb && a.use == StageHold::Use::kGameChoice), "\"random\" won: nothing is written");
        }
    }
}

void HoldRules() {
    StageHold::Hold h;
    Expect(!h.OnPoll(1, 0, true, 100, 5000) && !h.OnPoll(4, 1, true, 100, 5000) && !h.on, "other sync waits are never held");
    Expect(!h.OnPoll(2, 1, false, 100, 5000) && !h.on, "no panel waits: the game's own answer stands");
    h.wanted = false;
    Expect(!h.OnPoll(2, 1, true, 100, 5000) && !h.on, "'stagehold 0': never held");
    h.wanted = true;
    h.broken = true;
    Expect(!h.OnPoll(2, 1, true, 100, 5000) && !h.on, "a hold that broke a match stays off");
    h.broken = false;
    // What the game's native answers (read in its script and code): 0 the wait is over, 1 it still runs. A held question
    // must read 1; the build of 2026-11-13 16:28 wrote 0 and so ended the game's wait at its first question.
    Expect(StageHold::kAnswerOver == 0 && StageHold::kAnswerWaiting == 1, "native 1260: 0 is \"over\", 1 is \"still waiting\"");
    Expect(StageHold::kMaxAnswers < 1200, "the hold ends before half the script's count of 2400 ticks");
    Expect(h.OnPoll(2, StageHold::kAnswerWaiting, true, 1000, 5000) && h.on && h.since == 1000 && h.Holding() && !h.theirsAt,
           "the first question begins the hold; the other game is not there yet");
    Expect(h.OnPoll(2, StageHold::kAnswerOver, true, 1500, 5000) && h.theirsAt == 1500 && h.held == 2,
           "the game's own \"over\" is noted, the answer stays \"still waiting\"");
    Expect(h.OnPoll(2, StageHold::kAnswerWaiting, true, 1600, 5000) && h.theirsAt == 1500, "the first \"over\" is the one that is kept");
    Expect(h.OnPoll(2, 1, true, 5999, 5000), "held until its time");
    Expect(!h.OnPoll(2, 1, true, 6000, 5000) && h.over && h.overAt == 6000 && !h.Holding(), "its time ends it");
    Expect(!h.OnPoll(2, 1, true, 6001, 5000) && h.held == 4, "an ended hold never holds again");
    h.Reset();
    Expect(!h.on && !h.over && h.wanted && !h.held, "a new panel starts a new hold");
    for (uint32_t i = 0; i < StageHold::kMaxAnswers; ++i) h.OnPoll(2, 1, true, 10, 1u << 30);
    Expect(h.Holding() && h.held == StageHold::kMaxAnswers, "held for its number of answers");
    Expect(!h.OnPoll(2, 1, true, 11, 1u << 30) && h.over, "the number of answers ends it (a game that ticks faster than 60 a second)");
    h.Reset();
    h.OnPoll(2, 1, true, 10, 5000);
    h.End(50);
    h.End(70);
    Expect(h.over && h.overAt == 50, "ended once");

    // The decision.
    Table t = MakeTable();
    Exchange a, b;
    a.Open(kKindStage, t.Context(), 111, 1);
    b.Open(kKindStage, t.Context(), 222, 1);
    a.Lock(12);
    b.Lock(12);
    a.apply = b.apply = true;
    a.OnPacket(b.Out());
    b.OnPacket(a.Out());
    a.OnPacket(b.Out());
    StageHold::Hold on, off;
    on.OnPoll(2, 1, true, 1, 5000);
    Verdict v = Judge(a, t, true);
    Expect(v.outcome == Outcome::kStage && StageHold::Decide(on, a, v) == StageHold::Use::kWrite, "held, agreed, confirmed, the other holds: write");
    Expect(a.theirsSure && a.theirsApply && !b.theirsSure, "the third packet carries \"I hold and know you have both picks\"");
    Expect(StageHold::Decide(off, a, v) == StageHold::Use::kNotHeld, "not held: no write");
    Exchange s = a;
    s.theirsSure = false;
    Expect(StageHold::Decide(on, s, v) == StageHold::Use::kPeerNotSure, "the other game has not said that it is sure: no write");
    // The two flags follow the order the packets were sent in, not the order they arrive in.
    Exchange late = a, peer = b;
    const Packet earlier = peer.Next();
    peer.apply = false;
    const Packet later = peer.Next();
    late.OnPacket(later);
    Expect(!late.theirsApply && !late.theirsSure, "\"I do not hold any more\" is taken");
    late.OnPacket(earlier);
    Expect(!late.theirsApply && !late.theirsSure, "a packet from before it, arriving after it, does not bring the flags back");
    uint8_t raw[kSize];
    Packet back;
    Build(earlier, raw, sizeof raw);
    Expect(Parse(raw, sizeof raw, &back) == Parsed::kOk && back.sure && back.apply && back.seq == earlier.seq, "the flags and the count pass through the bytes");
    raw[7] = kOpen | kSure;
    Expect(Parse(raw, sizeof raw, &back) == Parsed::kOk && !back.sure, "\"sure\" without \"I hold\" is not read as sure");
    StageHold::Hold ended = on;
    ended.End(9);
    Expect(StageHold::Decide(ended, a, v) == StageHold::Use::kNotHeld, "a hold that ended before the draw: no write");
    Exchange c = a;
    c.theirsApply = false;
    Expect(StageHold::Decide(on, c, v) == StageHold::Use::kPeerNotHeld, "the other game does not hold: no write");
    c = a;
    c.apply = false;
    Expect(StageHold::Decide(on, c, v) == StageHold::Use::kPeerNotHeld, "this game does not say it holds: no write");
    c = a;
    c.echoed = Side{};
    Expect(StageHold::Decide(on, c, Judge(c, t, true)) == StageHold::Use::kNoStage, "not confirmed by the other game: no write");
    Exchange r1, r2;
    r1.Open(kKindStage, t.Context(), 5, 1);
    r2.Open(kKindStage, t.Context(), 6, 1);
    r1.Lock(kRandom);
    r2.Lock(kRandom);
    r1.apply = r2.apply = true;
    r1.OnPacket(r2.Out());
    r2.OnPacket(r1.Out());
    r1.OnPacket(r2.Out());
    Expect(StageHold::Decide(on, r1, Judge(r1, t, true)) == StageHold::Use::kGameChoice, "\"random\" won: the game's own choice");
    Expect(StageHold::Decide(on, a, Verdict{}) == StageHold::Use::kNoStage, "no verdict: no write");
    for (int u = 0; u < 6; ++u) Expect(strlen(StageHold::UseText((StageHold::Use)u)) > 3, "every outcome has its words");
}
}  // namespace

int main() {
    HoldRules();
    Tally lossy, oneHolds, tables, heavy, dying;
    for (int i = 0; i < 1500; ++i) Trial(0, true, true, &lossy);
    for (int i = 0; i < 400; ++i) Trial(0, i % 2 == 0, i % 2 == 1, &oneHolds);
    for (int i = 0; i < 100; ++i) Trial(0, false, false, &oneHolds);
    for (int i = 0; i < 200; ++i) Trial(0, true, true, &tables, false);
    for (int i = 0; i < 1500; ++i) Trial(1, true, true, &heavy);
    for (int i = 0; i < 1500; ++i) Trial(2, true, true, &dying);
    const auto line = [](const char* what, const Tally& t) {
        printf("note: %-56s %4d starts: both write %4d, neither %4d, ONE ONLY %3d, two different arenas %d\n", what, t.trials, t.bothWrote,
               t.neither, t.oneOnly, t.differ);
    };
    line("a link that loses up to 45 %, at most 4 in a row:", lossy);
    line("one game or neither holds:", oneHolds);
    line("two different stage tables:", tables);
    line("a link that loses 50-97 % without a bound:", heavy);
    line("a link that dies (one way or both) during the picking:", dying);
    Expect(lossy.oneOnly == 0 && lossy.differ == 0 && lossy.bothWrote > lossy.trials / 2, "the usual link: never one game alone, mostly both");
    Expect(oneHolds.bothWrote == 0 && oneHolds.oneOnly == 0, "one game that does not hold: nothing is written anywhere");
    Expect(tables.bothWrote == 0 && tables.oneOnly == 0, "different stage tables: nothing is written anywhere");
    Expect(heavy.differ == 0 && dying.differ == 0, "no link makes two games write two different arenas");
    printf("stagehold: %d checks, %d failed\n", gChecks, gFailed);
    if (!gFailed) printf("ALL CHECKS PASSED\n");
    return gFailed ? 1 : 0;
}
