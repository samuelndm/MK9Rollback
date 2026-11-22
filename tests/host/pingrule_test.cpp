// Host test of src\net\PingRule.h: the online mode's input delay from the hello round trips.
//   1. Against the rule it replaced (copied below as OldDelay): Safe mode is unchanged, rollback's delay is never larger,
//      and smaller only where the new rule means it (delay 1 by the median with spikes of one frame).
//   2. The field evenings' readings (second-highest / median of every match start in the logs of 2026-11-14 and
//      2026-11-20 and the install's log of 2026-11-20): all at delay 1, the two "67 / 50" starts included.
//   3. Simulated links: the hello's round trip measured the way the games measure it (sent and read at a fixed point of
//      each 60 Hz tick, the two games' ticks at a random offset, spikes on the way). A link of up to 2 frames gets delay 1
//      however its ticks fall; a link longer than 3 frames never does.
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <random>

#include "PingRule.h"

namespace {
int gChecks = 0, gFails = 0;
void Check(bool ok, const char* what, long a = 0, long b = 0) {
    ++gChecks;
    if (!ok) {
        ++gFails;
        if (gFails <= 40) printf("FAIL: %s (%ld, %ld)\n", what, a, b);
    }
}

// The rule until 2026-11-22 (OnlineMode.cpp's PingEstimate + DelayFromPing), with enough readings.
int OldDelay(const uint32_t* ring, uint32_t count, bool lockstep, uint32_t margin, int cpuFloor) {
    const uint32_t n = count < 16 ? count : 16;
    uint32_t s[16];
    for (uint32_t i = 0; i < n; ++i) s[i] = ring[i];
    std::sort(s, s + n);
    const uint32_t est = (n >= 2 ? s[n - 2] : n ? s[0] : 0) + margin;
    if (lockstep) return est + 10 <= 2u * 2000 / 60 ? 2 : 3;
    if (est <= 55) return cpuFloor > 1 ? cpuFloor : 1;
    static constexpr uint32_t kUpTo[] = {108, 142, 208, 258};
    for (int d = 2; d <= 5; ++d)
        if (est <= kUpTo[d - 2]) return d;
    return 6;
}

// Readings shaped like a match start's 16: seven at 33 (or the median when lower), seven at `median`, two at `second`.
void FieldReadings(uint32_t second, uint32_t median, uint32_t* ring) {
    for (int i = 0; i < 16; ++i) ring[i] = i < 7 ? (median > 33 ? 33 : median) : i < 14 ? median : second;
}

void AgainstOldRule() {
    std::mt19937 rng(20261122);
    const uint32_t steps[] = {16, 17, 33, 34, 49, 50, 51, 66, 67, 83, 84, 100, 117, 133, 150, 167, 200, 233, 250, 267, 300};
    long lowered = 0, cases = 0;
    for (int it = 0; it < 300000; ++it) {
        const uint32_t count = 4 + rng() % 40;
        uint32_t ring[16];
        const bool frames = rng() % 4 != 0;  // mostly whole-frame readings, as the games read them
        const uint32_t n = count < 16 ? count : 16;
        for (uint32_t i = 0; i < n; ++i) ring[i] = frames ? steps[rng() % (rng() % 2 ? 9 : 21)] : rng() % 520;
        const uint32_t margin = rng() % 3 == 0 ? 8 : 0;
        const int floor = rng() % 5 == 0 ? 2 : 1;
        const PingRule::Readings r = PingRule::Summarize(ring, count);
        for (bool lockstep : {false, true}) {
            ++cases;
            const int now = PingRule::DelayFor(r, lockstep, margin, floor);
            const int old = OldDelay(ring, count, lockstep, margin, floor);
            if (lockstep) {
                Check(now == old, "Safe mode's delay differs from the old rule", now, old);
                continue;
            }
            Check(now <= old, "rollback delay larger than the old rule's", now, old);
            Check(now >= floor, "rollback delay under this PC's floor", now, floor);
            if (now < old) {
                ++lowered;
                Check(now == floor && r.median + margin <= PingRule::kDelay1UpToMs &&
                          std::max(r.secondHighest, r.median) + margin <= PingRule::kDelay1SpikesUpToMs,
                      "rollback delay lowered outside the delay-1 rule", now, old);
            } else {
                Check(now == old, "rollback delay differs from the old rule", now, old);
            }
        }
    }
    printf("note: against the old rule: %ld cases, rollback's delay lowered in %ld (always to delay 1 by the median)\n", cases,
           lowered);
}

void FieldEvenings() {
    // second-highest / median of each match start (2026-11-14 afternoon and evening, 2026-11-20, the install's log), as
    // the field logs printed them.
    struct Pair { uint32_t second, median; int count; };
    const Pair field[] = {{50, 33, 10}, {34, 33, 9}, {50, 50, 8}, {49, 33, 6}, {33, 33, 5}, {34, 34, 3}, {67, 50, 2},
                          {33, 32, 2},  {50, 34, 2}, {51, 34, 1}, {49, 32, 1}, {49, 34, 1}, {51, 33, 1}, {50, 32, 1}};
    int starts = 0, wasTwo = 0;
    for (const Pair& p : field) {
        uint32_t ring[16];
        FieldReadings(p.second, p.median, ring);
        const PingRule::Readings r = PingRule::Summarize(ring, 16);
        Check(r.secondHighest == p.second && r.median == p.median, "the field readings were not rebuilt", r.secondHighest, r.median);
        const int now = PingRule::DelayFor(r, false, 0, 1);
        Check(now == 1, "a field match start is not at delay 1", (long)p.second, (long)p.median);
        if (OldDelay(ring, 16, false, 0, 1) != 1) wasTwo += p.count;
        // Between rounds at delay 1 these readings keep delay 1 (LiveProposal asks DelayFor without a margin when going up).
        starts += p.count;
        // At delay 2 they come back down to 1 only with kDownMarginMs (8) to spare: a median of 33 and no 67.
        const int down = PingRule::DelayFor(r, false, 8, 1);
        Check(down == ((p.median + 8 <= 55 && p.second + 8 <= 72) ? 1 : 2), "the way down from delay 2 changed", down, p.median);
        // A PC that does not keep up at delay 1 still proposes 2.
        Check(PingRule::DelayFor(r, false, 0, 2) == 2, "the CPU floor is not kept", (long)p.second, (long)p.median);
    }
    Check(starts == 52 && wasTwo == 2, "the field tally changed", starts, wasTwo);
    printf("note: field evenings: %d match starts, all at delay 1 now; %d were at delay 2 by the old rule\n", starts, wasTwo);
}

// One match start's 16 readings over a link: one-way trips t1 / t2 ms, the other game's ticks `phase` ms after this
// one's, spikes of up to `spikeMs` on `spikePct` % of the legs, a millisecond of noise.
struct Link { double t1, t2, spikePct, spikeMs; };
void ReadingsOver(const Link& l, double phase, std::mt19937& rng, uint32_t* ring) {
    const double P = 1000.0 / 60;
    std::uniform_real_distribution<double> u(0, 1);
    auto leg = [&](double t) { return t + u(rng) * 1.0 + (u(rng) * 100 < l.spikePct ? u(rng) * l.spikeMs : 0); };
    auto nextTick = [&](double at, double offset) { return offset + std::ceil((at - offset) / P) * P; };
    double s = 0;
    for (int i = 0; i < 16; ++i) {
        s += 6 * P;  // a hello every 100 ms, sent at this game's tick
        const double r = nextTick(s + leg(l.t1), phase);   // the other game reads it at its next tick
        const double uSend = r + 6 * P;                     // and echoes it in its next hello (held: measured, taken off)
        const double v = nextTick(uSend + leg(l.t2), 0.0);  // this game reads the echo at its next tick
        ring[i] = (uint32_t)std::lround((r - s) + (v - uSend));
    }
}

void SimulatedLinks() {
    std::mt19937 rng(7);
    std::uniform_real_distribution<double> ph(0, 1000.0 / 60);
    struct Case { const char* name; Link l; double minShareAt1; double maxShareAt1; };
    const Case cases[] = {
        {"LAN, 2 ms", {1, 1, 0, 0}, 1.0, 1.0},
        {"the user's link, 22 ms", {11, 11, 0, 0}, 1.0, 1.0},
        {"22 ms, 5% spikes up to 20 ms", {11, 11, 5, 20}, 0.98, 1.0},
        {"22 ms, uneven legs 4 / 18", {4, 18, 2, 15}, 0.98, 1.0},
        {"30 ms, 5% spikes up to 20 ms", {15, 15, 5, 20}, 0.98, 1.0},
        {"40 ms (2-3 frames)", {20, 20, 2, 15}, 0.0, 1.0},
        {"52 ms (over 3 frames)", {26, 26, 0, 0}, 0.0, 0.0},
        {"60 ms, 10% spikes", {30, 30, 10, 30}, 0.0, 0.0},
        {"100 ms", {50, 50, 5, 30}, 0.0, 0.0},
    };
    for (const Case& c : cases) {
        int atOneNew = 0, atOneOld = 0;
        const int matches = 4000;
        for (int m = 0; m < matches; ++m) {
            uint32_t ring[16];
            ReadingsOver(c.l, ph(rng), rng, ring);
            const PingRule::Readings r = PingRule::Summarize(ring, 16);
            const int now = PingRule::DelayFor(r, false, 0, 1);
            const int old = OldDelay(ring, 16, false, 0, 1);
            atOneNew += now == 1;
            atOneOld += old == 1;
            Check(now <= old, "a simulated link got more delay than by the old rule", now, old);
            if (c.l.t1 + c.l.t2 > 3 * 1000.0 / 60) Check(now >= 2, "a link longer than 3 frames got delay 1", (long)r.median, (long)r.secondHighest);
        }
        const double share = (double)atOneNew / matches;
        Check(share >= c.minShareAt1 && share <= c.maxShareAt1, c.name, atOneNew, atOneOld);
        printf("note: %-32s delay 1 in %4d of %d match starts (old rule %4d)\n", c.name, atOneNew, matches, atOneOld);
    }
}

void Summaries() {
    uint32_t ring[16] = {50, 33};
    PingRule::Readings r = PingRule::Summarize(ring, 0);
    Check(r.n == 0 && r.secondHighest == 0 && r.median == 0, "no readings");
    r = PingRule::Summarize(ring, 1);
    Check(r.n == 1 && r.secondHighest == 50 && r.median == 50 && r.lowest == 50, "one reading");
    r = PingRule::Summarize(ring, 2);
    Check(r.secondHighest == 33 && r.median == 50 && r.lowest == 33, "two readings: the median is the upper one");
    // Two readings: the higher one counts for every step, as before (the old rule took the lower).
    Check(PingRule::DelayFor(r, false, 0, 1) == 1, "two readings 33 / 50");
    uint32_t ring2[16] = {33, 83};
    r = PingRule::Summarize(ring2, 2);
    Check(PingRule::DelayFor(r, false, 0, 1) == 2, "two readings 33 / 83 must not be delay 1");
    // More than 16 taken: only the ring's 16 count.
    uint32_t ring3[16];
    for (uint32_t& v : ring3) v = 33;
    r = PingRule::Summarize(ring3, 500);
    Check(r.n == 16 && r.median == 33, "a full ring");
    // The steps above delay 1, at their edges.
    const struct { uint32_t ms; int delay; } edges[] = {{72, 2}, {108, 2}, {109, 3}, {142, 3}, {143, 4}, {208, 4}, {209, 5}, {258, 5}, {259, 6}, {900, 6}};
    for (const auto& e : edges) {
        for (uint32_t& v : ring3) v = e.ms;
        Check(PingRule::DelayFor(PingRule::Summarize(ring3, 16), false, 0, 1) == e.delay, "a step's edge", (long)e.ms, e.delay);
    }
    for (uint32_t& v : ring3) v = 55;
    Check(PingRule::DelayFor(PingRule::Summarize(ring3, 16), false, 0, 1) == 1, "55 ms is delay 1");
    for (uint32_t& v : ring3) v = 56;
    Check(PingRule::DelayFor(PingRule::Summarize(ring3, 16), false, 0, 1) == 2, "56 ms is delay 2");
    // Safe mode: 2 up to 56, 3 above, by the second-highest (a median of 33 does not change that).
    for (int i = 0; i < 16; ++i) ring3[i] = 33;
    ring3[14] = 56;
    ring3[15] = 90;
    Check(PingRule::DelayFor(PingRule::Summarize(ring3, 16), true, 0, 1) == 2, "Safe mode: second-highest 56 is 2");
    ring3[14] = 57;
    Check(PingRule::DelayFor(PingRule::Summarize(ring3, 16), true, 0, 1) == 3, "Safe mode: second-highest 57 is 3");
}
}  // namespace

int main() {
    Summaries();
    AgainstOldRule();
    FieldEvenings();
    SimulatedLinks();
    printf("%d checks, %d failed\n", gChecks, gFails);
    if (!gFails) printf("ALL CHECKS PASSED\n");
    return gFails ? 1 : 0;
}
