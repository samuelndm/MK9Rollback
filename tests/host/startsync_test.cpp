// Host-side test of src\net\StartSync.h: the start alignment of a rollback session's two games. Two simulated games reach
// their frame-0 boundaries at different moments and make the calls RollbackManager.cpp makes (StartSyncBegin, the start
// barrier's wait loop, StartSyncDecide, StartSyncAtFrame) over a simulated link; afterwards their frame grids must
// coincide: the game whose frame 1 came first moved by the difference, the other one did not move.
// Build and run: see run.ps1 beside this file.
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <vector>

#include "StartSync.h"

using namespace StartSync;

namespace {
int gFailed = 0, gChecks = 0;
void Check(bool ok, const char* what) {
    ++gChecks;
    if (!ok) ++gFailed;
    printf("%s  %s\n", ok ? "PASS" : "FAIL", what);
}

uint32_t gSeed = 20261112;
uint32_t Rnd(uint32_t n) {  // 0 .. n-1
    gSeed = gSeed * 1664525u + 1013904223u;
    return (gSeed >> 8) % n;
}

struct Link {  // what the network does to one direction's start datagrams
    int64_t tripUs = 0, jitterUs = 0;
    uint32_t lossPct = 0;
};
struct Arrival {
    int64_t at;
    Packet p;
};

struct Game {
    Session s;
    int64_t frame1 = 0;  // when its frame 1 starts before any shift (the limiter's deadline at the frame-0 boundary)
    int64_t begin = 0;   // its frame-0 boundary
    int64_t shift = 0, waited = 0, next = 0;
    int phase = 0;  // 0 not begun, 1 the barrier's wait loop, 2 the polls after it, 3 the following frames, 4 done
    int extra = 0, frame = 0;
    bool looped = false;
    std::vector<Arrival> inbox;
    Verdict verdict;
};

int Read(Game& g, int64_t now, bool prompt) {
    int n = 0;
    for (size_t i = 0; i < g.inbox.size();) {
        if (g.inbox[i].at > now) {
            ++i;
            continue;
        }
        g.s.OnPacket(g.inbox[i].p, now, prompt);
        g.inbox[i] = g.inbox.back();
        g.inbox.pop_back();
        ++n;
    }
    return n;
}
void Send(Game& g, Game& to, const Link& l, int64_t now) {
    const Packet p = g.s.Make(now);
    if (Rnd(100) >= l.lossPct) to.inbox.push_back({now + l.tripUs + (l.jitterUs ? (int64_t)Rnd((uint32_t)l.jitterUs + 1) : 0), p});
}
bool Decide(Game& g, bool final, int32_t tripGuessUs) {
    if (g.s.Decided()) return true;
    const Verdict v = g.s.Decide(final, g.waited, tripGuessUs);
    if (v.kind == Verdict::kWait) return false;
    g.verdict = v;
    if (v.kind == Verdict::kShift) g.shift += v.shiftUs;
    return true;
}

// One action of game g at g.next, in the order of RollbackManager.cpp's frame-0 boundary and the boundaries after it.
// inputTripUs: the trip of the first GGPO input, whose arrival ends the barrier (the harness delays GGPO's packets but
// not the checksum channel's, so it is a separate number).
void Step(Game& g, Game& o, const Link& out, int64_t inputTripUs, int32_t tripGuessUs) {
    const int64_t now = g.next;
    const auto inputIn = [&] { return o.phase > 0 && o.begin + inputTripUs <= now; };
    const auto poll = [&] {  // StartSyncLoopPoll
        Read(g, now, true);
        if (g.s.WantSend(now)) Send(g, o, out, now);
    };
    const auto iteration = [] { return 1000 + (int64_t)Rnd(401); };  // BarrierWait and the poll: 1.0-1.4 ms
    const auto toFrames = [&] {
        g.phase = 3;
        g.frame = 1;
        const int64_t at = g.frame1 + g.shift;
        g.next = at > now + 3000 ? at : now + 3000;
    };
    const auto afterBarrier = [&] {  // the barrier's end: decide, or up to 3 more polls for a game that waited
        g.waited = g.looped ? now - g.begin : 0;
        if (Decide(g, false, tripGuessUs) || !g.looped) {
            toFrames();
            return;
        }
        g.phase = 2;
        g.extra = 0;
        g.next = now + iteration();
    };
    switch (g.phase) {
    case 0:  // StartSyncBegin
        g.s.Begin(now, g.frame1);
        g.phase = 1;
        Read(g, now, true);
        Send(g, o, out, now);
        Send(g, o, out, now);
        if (inputIn()) afterBarrier();
        else g.next = now + iteration();
        break;
    case 1:
        g.looped = true;
        poll();
        if (inputIn()) afterBarrier();
        else g.next = now + iteration();
        break;
    case 2:
        poll();
        if (Decide(g, false, tripGuessUs) || ++g.extra >= 3) toFrames();
        else g.next = now + iteration();
        break;
    case 3: {  // StartSyncAtFrame
        Read(g, now, false);
        if (g.s.WantSendAtFrame(g.frame)) Send(g, o, out, now);
        Decide(g, g.frame >= kFrames, tripGuessUs);
        if (g.frame >= kFrames) {
            g.phase = 4;
            break;
        }
        ++g.frame;
        const int64_t at = g.frame1 + g.shift + (g.frame - 1) * kFrameUs;
        g.next = at > now + 3000 ? at : now + 3000;
        break;
    }
    }
}

struct Outcome {
    int64_t errorUs;  // (A's frame 1 after its shift) - (B's): 0 = the grids coincide
    int64_t shiftA, shiftB;
    Verdict a, b;
    bool decidedA, decidedB;
};
// aheadUs: B's frame 1 starts this much after A's. lateUs: how late each game's frame-0 boundary is on its own grid.
Outcome Run(int64_t aheadUs, int64_t lateA, int64_t lateB, const Link& aToB, const Link& bToA, int64_t inputTripUs, int32_t tripGuessUs) {
    Game a, b;
    a.frame1 = 1'000'000;
    b.frame1 = a.frame1 + aheadUs;
    a.begin = a.next = a.frame1 - kFrameUs + lateA;
    b.begin = b.next = b.frame1 - kFrameUs + lateB;
    for (int guard = 0; guard < 200000 && (a.phase < 4 || b.phase < 4); ++guard) {
        const bool stepA = b.phase >= 4 || (a.phase < 4 && a.next <= b.next);
        if (stepA) Step(a, b, aToB, inputTripUs, tripGuessUs);
        else Step(b, a, bToA, inputTripUs, tripGuessUs);
    }
    return {(a.frame1 + a.shift) - (b.frame1 + b.shift), a.shift, b.shift, a.verdict, b.verdict, a.s.Decided(), b.s.Decided()};
}

struct Sweep {
    int runs = 0, bad = 0, both = 0, undecided = 0, unknown = 0, exact = 0;
    int64_t worstUs = 0;
};
// Offsets from -spanUs to +spanUs; wantErrorUs: the error the link's asymmetry gives by itself.
Sweep Many(int64_t spanUs, int64_t stepUs, const Link& aToB, const Link& bToA, int64_t inputTripUs, int64_t tolUs, int64_t wantErrorUs,
           bool unknownAllowed, const char* name) {
    Sweep s;
    for (int64_t ahead = -spanUs; ahead <= spanUs; ahead += stepUs) {
        const int64_t off = ahead + (int64_t)Rnd(200);
        const Outcome r = Run(off, Rnd(3001), Rnd(3001), aToB, bToA, inputTripUs, (int32_t)((aToB.tripUs + bToA.tripUs) / 2 + 5000));
        ++s.runs;
        const bool unknown = r.a.kind == Verdict::kUnknown || r.b.kind == Verdict::kUnknown;
        s.unknown += unknown;
        s.both += r.shiftA > 0 && r.shiftB > 0;
        s.undecided += !r.decidedA || !r.decidedB;
        s.exact += r.a.exact && r.b.exact;
        if (unknown && unknownAllowed) continue;
        const int64_t e = llabs(r.errorUs - wantErrorUs);
        if (e > s.worstUs) s.worstUs = e;
        if ((e > tolUs || unknown) && s.bad++ < 5)
            printf("      %s: B's frame 1 %.2f ms after A's: A moved %.2f, B moved %.2f, left %.2f ms apart (kinds %d %d)\n", name, off / 1000.0,
                   r.shiftA / 1000.0, r.shiftB / 1000.0, r.errorUs / 1000.0, (int)r.a.kind, (int)r.b.kind);
    }
    printf("      %s: %d starts, worst %.2f ms from the wanted result, %d outside %.1f ms, %d with both gaps read promptly, %d without "
           "usable timing\n", name, s.runs, s.worstUs / 1000.0, s.bad, tolUs / 1000.0, s.exact, s.unknown);
    return s;
}
bool Clean(const Sweep& s) { return !s.bad && !s.both && !s.undecided; }
}  // namespace

int main() {
    // The rule by hand: A reads B's datagram one trip after it left, and the other way round.
    {
        Session a, b;
        const int64_t d = 13500, aheadB = 20000;  // B's frame 1 is 20 ms after A's; the trip is 13.5 ms
        a.Begin(0, 16000);
        b.Begin(0, 16000 + aheadB);
        const Packet pa = a.Make(1000), pb = b.Make(2000);
        Check(pa.untilUs == 15000 && pa.gapUs == kNone && pb.untilUs == 34000, "a datagram carries the sender's time to its frame 1 and no gap yet");
        a.OnPacket(pb, 2000 + d, true);
        b.OnPacket(pa, 1000 + d, true);
        Check(a.Mine() == aheadB + d && b.Mine() == -aheadB + d, "each game's gap: the other game's frame 1 minus its own, plus the trip");
        Check(a.Decide(false, 40000, 0).kind == Verdict::kWait && !a.Decided(), "one gap alone: wait");
        a.OnPacket(b.Make(20000), 20000 + d, true);
        b.OnPacket(a.Make(21000), 21000 + d, true);
        const Verdict va = a.Decide(false, 40000, 0), vb = b.Decide(false, 0, 0);
        Check(va.kind == Verdict::kShift && va.shiftUs == aheadB && va.tripUs == d && va.exact, "the earlier game moves by the offset; the trip is measured");
        Check(vb.kind == Verdict::kStay && vb.shiftUs == 0 && vb.aheadUs == -aheadB && vb.tripUs == d, "the later game stays");
        Check(a.Decide(true, 40000, 0).kind == Verdict::kWait && a.Decided(), "a verdict is given once");
    }
    // Without the other game's gap.
    {
        Session a;
        a.Begin(0, 16000);
        Check(a.Decide(true, 30000, 13000).kind == Verdict::kUnknown, "nothing read by the last frame: unknown (the caller's own estimate decides)");
        Session b;
        b.Begin(0, 16000);
        Packet p{kMagic, 46000, kNone, 0, 0};  // their frame 1 is 30 ms after this game's, read 13.5 ms after it left
        b.OnPacket(p, 13500, true);
        const Verdict v = b.Decide(true, 44000, 15000);
        Check(v.kind == Verdict::kShift && v.guessed && v.shiftUs == 43500 - 15000, "only its own gap by the last frame: the gap less the trip estimate");
        Session c;
        c.Begin(0, 16000);
        p.untilUs = -20000;  // their frame 1 was 36 ms before this game's
        c.OnPacket(p, 13500, true);
        Check(c.Decide(false, 0, 15000).kind == Verdict::kWait, "a negative gap still waits for the other game's (for the measured trip)");
        const Verdict w = c.Decide(true, 0, 15000);
        Check(w.kind == Verdict::kStay && w.bound && w.shiftUs == 0, "a negative gap alone: this game is the later one and stays");
    }
    // What must not be taken.
    {
        Session a;
        a.Begin(0, 16000);
        Packet p{kMagic, 5'000'000, 13000, 1, 0};  // their frame 1 five seconds away: another start's datagram
        a.OnPacket(p, 13500, true);
        Check(a.Decide(false, 30000, 0).kind == Verdict::kUnknown, "an offset far beyond this game's own wait: unknown, nothing moves");
        Session b;
        b.Begin(0, 16000);
        p = {kMagic, 10000, -40000, 1, 0};  // gaps of 6 and -40 ms: a trip of -17 ms
        b.OnPacket(p, 12000, true);
        Check(b.Decide(false, 30000, 0).kind == Verdict::kUnknown, "gaps that add up to a negative trip: unknown");
        Session c;
        c.Begin(0, 16000);
        p = {0x43394B4D, 10000, 0, 1, 0};
        c.OnPacket(p, 12000, true);
        p = {kMagic, 2'000'000'000, 0, 1, 0};
        c.OnPacket(p, 12000, true);
        Check(c.Reads() == 0 && c.Mine() == kNone, "another magic and a time out of range are not read");
        Session d;
        Check(!d.WantSend(0) && !d.WantSendAtFrame(1) && d.Decide(true, 0, 0).kind == Verdict::kWait, "a session that never began sends and decides nothing");
    }
    // The wait loop's sends: every poll for 100 ms, then every fourth; at once when the gap changed.
    {
        Session a;
        a.Begin(0, 16000);
        int sends = 0;
        for (int64_t t = 0; t < 300000; t += 1100)
            if (a.WantSend(t)) {
                a.Make(t);
                ++sends;
            }
        Check(sends > 130 && sends < 150, "a 300 ms wait: about 91 datagrams in the first 100 ms and 46 after");
        Check(!a.WantSend(300100), "nothing new 0.1 ms after a send");
        a.OnPacket(Packet{kMagic, 10000, kNone, 0, 0}, 300200, true);
        Check(a.WantSend(300200) && a.WantSendAtFrame(9), "a new gap goes out at once, in the loop and at a later frame");
        a.Make(300200);
        Check(!a.WantSendAtFrame(kResendFrames + 1) && a.WantSendAtFrame(kResendFrames), "repeats through frame 5 only");
    }

    const Link wire{13500, 0, 0};
    // The evening of 2026-11-11: a 13.5 ms trip, starts up to 80 ms apart.
    {
        const Sweep s = Many(80000, 170, wire, wire, 13500, 1500, 0, false, "13.5 ms trip");
        Check(Clean(s), "a 13.5 ms trip, starts up to 80 ms apart either way: the grids end within 1.5 ms of each other, one game moves");
        Check(s.exact > s.runs * 9 / 10, "nearly every start decides on two promptly read gaps");
    }
    // The six starts that went wrong on that evening, as the log has them (wait at frame 0 -> offset = wait - trip):
    // 31.6, 15.8, 16.8, 35.2 ms waits; rc29 moved 2, 1, 1, 2 frames where 1, 0, 0, 1 were right.
    {
        bool ok = true;
        for (const int64_t wait : {31600, 15800, 16800, 35200}) {
            const int64_t offset = wait - 13500;
            const Outcome r = Run(offset, 0, 0, wire, wire, 13500, 14000);
            const bool good = llabs(r.shiftA - offset) <= 1500 && r.shiftB == 0 && llabs(r.errorUs) <= 1500;
            if (!good) printf("      a %.1f ms wait: A moved %.2f ms (wanted %.2f), B %.2f\n", wait / 1000.0, r.shiftA / 1000.0, offset / 1000.0, r.shiftB / 1000.0);
            ok &= good;
        }
        Check(ok, "the 2026-11-11 starts (waits of 31.6, 15.8, 16.8, 35.2 ms): the waiting game moves 18.1, 2.3, 3.3, 21.7 ms, not 1-2 frames");
    }
    // Other trips, from two games on one PC to a 160 ms round trip.
    {
        bool ok = true;
        for (const int64_t trip : {100, 2000, 6000, 25000, 40000, 80000}) {
            const Link l{trip, 0, 0};
            char name[48];
            snprintf(name, sizeof name, "%.1f ms trip", trip / 1000.0);
            ok &= Clean(Many(80000, 370, l, l, trip, 1500, 0, false, name));
        }
        Check(ok, "trips of 0.1 to 80 ms: within 1.5 ms, one game moves");
    }
    // The two-instance harness: the checksum channel is loopback (0.1 ms) while GGPO's packets take the netsim's 15 or 60 ms.
    {
        const Link loop{100, 0, 0};
        Check(Clean(Many(80000, 370, loop, loop, 15000, 1500, 0, false, "harness, GGPO 15 ms")), "harness link, GGPO's input 15 ms behind: within 1.5 ms");
        Check(Clean(Many(80000, 370, loop, loop, 60000, 1500, 0, false, "harness, GGPO 60 ms")), "harness link, GGPO's input 60 ms behind: within 1.5 ms");
    }
    // A game that arrives much later than the other (sync retries): the waiting game sends every fourth poll by then.
    {
        const Sweep s = Many(900000, 9100, wire, wire, 13500, 3500, 0, false, "starts up to 0.9 s apart");
        Check(Clean(s), "starts up to 0.9 s apart: within 3.5 ms");
    }
    // Jitter: each datagram up to 2 ms later than the trip.
    {
        const Link j{13500, 2000, 0};
        Check(Clean(Many(80000, 170, j, j, 13500, 3000, 0, false, "2 ms jitter")), "2 ms of jitter: within 3 ms");
    }
    // A trip that differs by direction cannot be seen: the grids end half the difference apart (12 ms and 18 ms: 3 ms).
    {
        const Link ab{12000, 0, 0}, ba{18000, 0, 0};
        Check(Clean(Many(80000, 370, ab, ba, 15000, 1500, 3000, false, "12 / 18 ms trips")),
              "trips of 12 and 18 ms: half the difference apart, as the rule says, and no worse");
    }
    // Lost datagrams. A gap may then come from a once-a-frame read (up to a frame late: half a frame of error), and a
    // game that reads nothing at all leaves it to the caller's estimate; never do both games move.
    {
        const Link l{13500, 1000, 30};
        const Sweep s = Many(80000, 90, l, l, 13500, 10500, 0, true, "30% lost");
        Check(!s.bad && !s.both && !s.undecided, "30% of the datagrams lost: within 10.5 ms (a late-read gap), never both games moving");
        Check(s.unknown < s.runs / 50 && s.exact > s.runs / 2, "and most starts still decide on promptly read gaps");
        const Link half{13500, 1000, 60};
        const Sweep h = Many(80000, 190, half, half, 13500, 12000, 0, true, "60% lost");
        Check(!h.bad && !h.both && !h.undecided, "60% lost: bounded the same way");
    }
    // One game without the exchange ('startsync 0', or its datagrams never arrive): the other reads nothing and says so.
    {
        const Link dead{13500, 0, 100};
        const Outcome r = Run(20000, 0, 0, wire, dead, 13500, 14000);  // B's datagrams never reach A
        Check(r.a.kind == Verdict::kUnknown && r.shiftA == 0, "a silent partner: the waiting game reads nothing, moves nothing itself, reports unknown");
        Check(r.decidedB && r.shiftB == 0, "and the later game, which read the waiting game's datagrams, stays");
    }

    printf("\n%d checks, %d failed\n", gChecks, gFailed);
    if (!gFailed) printf("ALL CHECKS PASSED\n");
    return gFailed ? 1 : 0;
}
