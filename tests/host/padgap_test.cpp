// Host-side test of src\game\PadGap.h: counting the presses the game never read (pressed and released between two samples
// of the controls). No game and no Windows input here: MK9's samples, the pads' reading thread and the presses are
// simulated over a main loop that stands still now and then, as on the evening of 2026-11-20 (waits of 0.3-0.8 s for the
// other PC). What matters most: a press the game did read is never counted as lost.
// Build and run: see run.ps1 beside this file.
#include <windows.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <thread>
#include <vector>

#include "PadGap.h"

using namespace PadGap;

namespace {
int gFailed = 0, gChecks = 0;
void Expect(bool ok, const char* what) {
    ++gChecks;
    if (!ok) ++gFailed;
    printf("%s  %s\n", ok ? "PASS" : "FAIL", what);
}

uint32_t gRng = 0x7A3C91Eu;
uint32_t Rnd() {
    gRng ^= gRng << 13;
    gRng ^= gRng >> 17;
    gRng ^= gRng << 5;
    return gRng;
}
int64_t Rnd(int64_t lo, int64_t hi) { return lo + (int64_t)(Rnd() % (uint32_t)(hi - lo + 1)); }

// ---- the sources of one XInput reading ----
void TestSources() {
    Expect(SourcesOf(0x1000 | 0x0002, 0, 0, 0, 0, 0) == (0x1000u | 0x0002u), "buttons and the d-pad are XInput's button word as it is");
    Expect(SourcesOf(0, 30, 0, 0, 0, 0) == kLT && SourcesOf(0, 29, 0, 0, 0, 0) == 0, "LT counts from 30 of 255 (XInput's threshold)");
    Expect(SourcesOf(0, 0, 25, 0, 0, kRT) == kRT && SourcesOf(0, 0, 19, 0, 0, kRT) == 0, "RT held stays held down to 20, then off");
    Expect(SourcesOf(0, 0, 0, 24000, 0, 0) == kStickRight && SourcesOf(0, 0, 0, 20000, 0, 0) == 0, "the stick counts from 24000");
    Expect(SourcesOf(0, 0, 0, 20000, 0, kStickRight) == kStickRight && SourcesOf(0, 0, 0, 15000, 0, kStickRight) == 0,
           "a stick direction held stays held down to 16000");
    Expect(SourcesOf(0, 0, 0, 0, 30000, 0) == kStickUp && SourcesOf(0, 0, 0, -30000, -30000, 0) == (kStickLeft | kStickDown),
           "XInput's +Y is up; a diagonal is two directions");
}

// ---- edges of the readings ----
void TestEdges() {
    Edges e;
    Press out[8];
    uint32_t lost = 0;
    int n = e.Read(0x1000, 100, 90, out, 8, &lost);
    Expect(n == 0, "a press that begins gives nothing yet");
    n = e.Read(0x1000 | 0x2000, 110, 100, out, 8, &lost);
    Expect(n == 0, "a second button down: still nothing");
    n = e.Read(0x2000, 120, 110, out, 8, &lost);
    Expect(n == 1 && out[0].source == 0x1000 && out[0].downAfter == 90 && out[0].down == 100 && out[0].up == 120,
           "A's release: one press, begun after the reading at 90, seen at 100, released by 120");
    n = e.Read(0, 130, 120, out, 8, &lost);
    Expect(n == 1 && out[0].source == 0x2000 && out[0].downAfter == 100 && out[0].up == 130, "B's release, with its own times");
    e.Read(0xFFFF, 140, 130, out, 8, &lost);
    n = e.Read(0, 150, 140, out, 8, &lost);
    Expect(n == 8 && lost == 8, "16 releases in one reading: 8 written, 8 counted as lost");
}

// ---- the verdict against the samples ----
void TestJudge() {
    Samples s;
    Press p{0x1000, 5, 6, 9};
    Expect(Judge(s, p) == Verdict::NotYet, "no sample yet: not decided");
    s.Add(10);
    Expect(Judge(s, p) == Verdict::Unknown, "a press that ended before the first sample of the count: unknown, not lost");
    s.Add(20);
    s.Add(30);
    Expect(Judge(s, Press{0x1000, 12, 14, 25}) == Verdict::Read, "a sample at 20 fell inside the press: read");
    Expect(Judge(s, Press{0x1000, 21, 22, 28}) == Verdict::Missed, "between the samples at 20 and 30: never read");
    Expect(Judge(s, Press{0x1000, 21, 22, 30}) == Verdict::Read, "released by the reading at 30, the sample at 30 may have seen it: read");
    Expect(Judge(s, Press{0x1000, 20, 22, 28}) == Verdict::Missed, "the sample at 20 was the reading before it began: never read");
    Expect(Judge(s, Press{0x1000, 19, 22, 28}) == Verdict::Read, "it may have begun before the sample at 20: read (counted only when sure)");
    Expect(Judge(s, Press{0x1000, 25, 27, 31}) == Verdict::NotYet, "it ended after the last sample: decided at the next one");
    Expect(Judge(s, Press{0x1000, 31, 33, 40}) == Verdict::NotYet, "begun and ended after the last sample: not decided yet");
    s.Add(500);
    Expect(Judge(s, Press{0x1000, 25, 27, 31}) == Verdict::Read, "the sample at 30 fell inside it: read");
    Expect(Judge(s, Press{0x1000, 31, 33, 40}) == Verdict::Missed, "the next sample came at 500: never read");
    int64_t before = 0, after = 0;
    s.Around(31, 40, &before, &after);
    Expect(before == 30 && after == 500, "the samples around it are 30 and 500 (a 470 gap: the game stood still)");
    for (int i = 0; i < 600; ++i) s.Add(1000 + i * 10);  // 256 kept: 4440 .. 6990
    Expect(Judge(s, Press{0x1000, 900, 950, 6000}) == Verdict::Read, "a long press over many samples: read");
    Expect(Judge(s, Press{0x1000, 4000, 4005, 4500}) == Verdict::Read, "begun before the oldest kept sample, one kept inside: read");
    Expect(Judge(s, Press{0x1000, 600, 650, 3500}) == Verdict::Unknown, "a press older than every kept sample: unknown");
}

// ---- keys ----
void TestKeys() {
    Keys k;
    int64_t held = 0;
    k.Down(0x41, 100);
    k.Sample();
    Expect(k.Up(0x41, 150, &held) == 0 && held == 50, "down, a sample, up: read, held 50");
    k.Down(0x41, 200);
    k.Down(0x41, 230);  // auto-repeat
    Expect(k.Up(0x41, 260, &held) == 1 && held == 60, "down, auto-repeat, up with no sample: never read, held from the first down");
    Expect(k.Up(0x42, 270, &held) == -1, "an up whose down was not counted is no press");
    k.Down(0x43, 300);
    k.Reset();
    Expect(k.Up(0x43, 320, &held) == -1, "a reset forgets keys that were down");
    k.Down(0x44, 400);
    k.Sample();
    k.Sample();
    Expect(k.Up(0x44, 420, &held) == 0, "two samples while down: read");
}

// ---- the ring between the two threads ----
void TestRing() {
    static Ring<64> ring;
    constexpr uint32_t kN = 200000;
    std::thread writer([] {
        for (uint32_t i = 0; i < kN;) {
            if (ring.w.load() - ring.r.load() >= 64) {
                std::this_thread::yield();
                continue;
            }
            ring.Push(Press{1, (int64_t)i, (int64_t)i, (int64_t)i});
            ++i;
        }
    });
    uint32_t next = 0, bad = 0;
    Press p;
    while (next < kN) {
        if (!ring.Pop(&p)) continue;
        if (p.up != (int64_t)next) ++bad;
        ++next;
    }
    writer.join();
    Expect(bad == 0 && ring.dropped.load() == 0 && !ring.Pop(&p), "200000 presses through the ring between two threads: all, in order, once");
    static Ring<4> small;
    for (int i = 0; i < 6; ++i) small.Push(Press{1, i, i, i});
    Expect(small.dropped.load() == 2, "a full ring drops and counts what does not fit");
}

// ---- a simulated evening ----
// Times in microseconds. MK9 samples every 16.7 ms and now and then stands still 40-800 ms; the reading thread reads every
// 2 ms, sometimes late by up to 16 ms; presses of 5-150 ms on several buttons and stick directions.
struct Tap {
    uint32_t source;
    int64_t down, up;
    bool sampled = false;  // the truth: a sample fell while it was held
    int verdict = -1;      // the counter's
};

void TestEvening(uint32_t seed, int64_t lengthUs, bool stalls, int* fpOut, double* rateStill, double* rateShort, int* stillMissed) {
    gRng = seed;
    std::vector<int64_t> samples;
    for (int64_t t = 1000; t < lengthUs;) {
        samples.push_back(t);
        t += (stalls && Rnd() % 100 < 2) ? Rnd(40000, 800000) : 16667;
    }
    const uint32_t sources[] = {0x1000, 0x2000, 0x4000, 0x8000, 0x0100, 0x0200, kLT, kRT, 0x0002, kStickDown, kStickRight};
    std::vector<Tap> taps;
    for (uint32_t src : sources)
        for (int64_t t = Rnd(0, 50000); t < lengthUs - 200000;) {
            const int64_t len = Rnd(5000, 150000);
            taps.push_back(Tap{src, t, t + len});
            t += len + Rnd(20000, 300000);
        }
    for (Tap& tp : taps) {
        auto it = std::lower_bound(samples.begin(), samples.end(), tp.down);
        tp.sampled = it != samples.end() && *it < tp.up;
    }
    std::vector<int64_t> reads;
    for (int64_t t = 0; t < lengthUs;) {
        reads.push_back(t);
        t += Rnd(2000, 2500) + (Rnd() % 100 == 0 ? Rnd(0, 16000) : 0);
    }
    auto heldAt = [&](int64_t t) {
        uint32_t m = 0;
        for (const Tap& tp : taps)
            if (tp.down <= t && t < tp.up) m |= tp.source;
        return m;
    };
    // The two threads' events in time order: a reading pushes ended presses; a sample decides what can be decided.
    Edges edges;
    Samples smp;
    std::vector<Press> waiting;
    std::vector<std::pair<Press, Verdict>> decided;
    size_t ri = 0, si = 0;
    int64_t prevRead = 0;
    while (ri < reads.size() || si < samples.size()) {
        const bool readNext = si >= samples.size() || (ri < reads.size() && reads[ri] < samples[si]);
        if (readNext) {
            Press out[16];
            uint32_t lost = 0;
            const int n = edges.Read(heldAt(reads[ri]), reads[ri], prevRead, out, 16, &lost);
            for (int i = 0; i < n; ++i) waiting.push_back(out[i]);
            prevRead = reads[ri++];
        } else {
            smp.Add(samples[si++]);
            std::vector<Press> keep;
            for (const Press& p : waiting) {
                const Verdict v = Judge(smp, p);
                if (v == Verdict::NotYet) keep.push_back(p);
                else decided.push_back({p, v});
            }
            waiting.swap(keep);
        }
    }
    // Match each decided press to its tap: the same source, begun after downAfter and by down.
    int fp = 0, missedStill = 0, countedStill = 0, missedShort = 0, countedShort = 0;
    for (auto& [p, v] : decided)
        for (Tap& tp : taps)
            if (tp.source == p.source && tp.down > p.downAfter && tp.down <= p.down) {
                tp.verdict = (int)v;
                break;
            }
    for (const Tap& tp : taps) {
        if (tp.verdict == (int)Verdict::Missed && tp.sampled) ++fp;
        if (tp.sampled || tp.verdict < 0 || tp.verdict == (int)Verdict::Unknown) continue;
        // A press the game never read, seen by the reading thread: what kind of gap held it.
        auto it = std::lower_bound(samples.begin(), samples.end(), tp.down);
        const int64_t gap = it == samples.end() || it == samples.begin() ? 0 : *it - *(it - 1);
        const bool still = gap >= 34000;
        (still ? missedStill : missedShort)++;
        if (tp.verdict == (int)Verdict::Missed) (still ? countedStill : countedShort)++;
    }
    *fpOut = fp;
    *rateStill = missedStill ? (double)countedStill / missedStill : 1.0;
    *rateShort = missedShort ? (double)countedShort / missedShort : 1.0;
    *stillMissed = missedStill;
    printf("        seed %08X: %zu presses, %zu samples; never read %d in gaps >= 34 ms (%d counted), %d in ordinary gaps (%d counted); "
           "counted as lost but read: %d\n",
           seed, taps.size(), samples.size(), missedStill, countedStill, missedShort, countedShort, fp);
}

}  // namespace

int main() {
    printf("== pad gap: presses the game never read\n");
    TestSources();
    TestEdges();
    TestJudge();
    TestKeys();
    TestRing();
    int fpAll = 0, stillAll = 0;
    double worstStill = 1.0, worstShort = 1.0;
    for (uint32_t seed : {0x1u, 0xBEEFu, 0x51C3A7Du, 0x2026112u}) {
        int fp = 0, still = 0;
        double rs = 0, rsh = 0;
        TestEvening(seed, 600LL * 1000000, true, &fp, &rs, &rsh, &still);
        fpAll += fp;
        stillAll += still;
        worstStill = std::min(worstStill, rs);
        worstShort = std::min(worstShort, rsh);
    }
    Expect(fpAll == 0, "four simulated 10-minute evenings with stalls: no press the game read is counted as never read");
    Expect(stillAll > 100 && worstStill >= 0.85, "of the presses made while the game stood still, at least 85 % are counted");
    Expect(worstShort >= 0.5, "of the presses shorter than a frame that fell between two samples, at least half are counted");
    int fp = 0, still = 0;
    double rs = 0, rsh = 0;
    TestEvening(0x600DF00Du, 600LL * 1000000, false, &fp, &rs, &rsh, &still);
    Expect(fp == 0 && still == 0, "an evening without stalls: none counted wrongly, none in a stall");
    printf("\n%d checks, %d failed\n", gChecks, gFailed);
    if (!gFailed) printf("ALL CHECKS PASSED\n");
    return gFailed;
}
