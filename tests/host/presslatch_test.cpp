// Host-side test of src\game\PressLatch.h: a press that falls between two samples of the controls still reaches a panel
// of the mod. No game and no Windows input here: the key messages, MK9's key table, its sampler, the panel's reader and
// the pads' reading thread are simulated, with the pauses of the main loop that lost a press on 2026-11-13.
// Build and run: see run.ps1 beside this file.
#include <windows.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "PressLatch.h"

using namespace PressLatch;

namespace {
int gFailed = 0, gChecks = 0;
void Expect(bool ok, const char* what) {
    ++gChecks;
    if (!ok) ++gFailed;
    printf("%s  %s\n", ok ? "PASS" : "FAIL", what);
}

uint32_t gRng = 0x51C3A7Du;
uint32_t Rnd() {
    gRng ^= gRng << 13;
    gRng ^= gRng >> 17;
    gRng ^= gRng << 5;
    return gRng;
}
int Rnd(int lo, int hi) { return lo + (int)(Rnd() % (uint32_t)(hi - lo + 1)); }

constexpr uint8_t kKey = 0x27;  // the right arrow
constexpr uintptr_t kWindow = 0x1234, kLParam = 0x014D0001;

bool Clean(const KeyStretch& k) {
    if (k.pendingCount) return false;
    for (int i = 0; i < KeyStretch::kKeys; ++i)
        if (k.pending[i] || k.inFlight[i] || k.drop[i]) return false;
    return true;
}

// ---- MK9's main loop with the keyboard: pump the key messages, sample, sometimes tick ----
struct Press {
    int down, up;  // ms
};
struct LoopResult {
    int edges = 0;          // presses the panel's reader saw begin
    int sampledDown = 0;    // samples with the key down
    bool tableUpAtEnd = false, clean = false;
    uint32_t heldBack = 0, expired = 0;
    int longestPass = 0;
};
struct Msg {
    int t;
    bool down;
    uint32_t order;
};

// stretch: the hook's logic is in the path. pauseChance: per pass, in 1/1000. skipTick: per pass, in 1/1000 (a pass that
// samples and runs no tick of the panel).
LoopResult RunKeyboard(const std::vector<Press>& presses, bool stretch, int pauseChance, int pauseMin, int pauseMax, int skipTick) {
    std::vector<Msg> queue;
    uint32_t order = 0;
    for (const Press& p : presses) {
        queue.push_back(Msg{p.down, true, order++});
        queue.push_back(Msg{p.up, false, order++});
    }
    KeyStretch ks;
    LoopResult r;
    bool table = false;       // MK9's own idea of the key
    uint32_t seen = 0, prev = 0;
    const int end = presses.back().up + 3000;
    size_t at = 0;
    std::vector<Msg> posted;  // releases posted again: taken after everything that was already in the queue at their time
    for (int now = 0; now < end;) {
        // pump: everything in the queue by now, in the order it was queued
        std::vector<Msg> take;
        while (at < queue.size() && queue[at].t <= now) take.push_back(queue[at++]);
        for (const Msg& m : posted) take.push_back(m);
        posted.clear();
        std::stable_sort(take.begin(), take.end(), [](const Msg& a, const Msg& b) { return a.t != b.t ? a.t < b.t : a.order < b.order; });
        for (const Msg& m : take) {
            KeyStretch::Up ups[8];
            bool held = false;
            if (stretch) {
                if (m.down) ks.KeyDown(kKey, (uint32_t)now);
                else held = ks.KeyUp(kKey, kWindow, kLParam, (uint32_t)now);
                const int n = ks.Expire((uint32_t)now, ups, 8);
                for (int i = 0; i < n; ++i) posted.push_back(Msg{now, false, order++});
            }
            if (!held) table = m.down;
        }
        // sample
        const uint32_t sampled = table ? 8u : 0u;
        if (sampled) ++r.sampledDown;
        seen |= sampled;
        if (stretch) {
            KeyStretch::Up ups[16];
            const int n = ks.Sample((uint32_t)now, ups, 16);
            for (int i = 0; i < n; ++i) {
                if (ups[i].vk != kKey || ups[i].window != kWindow || ups[i].lParam != kLParam) r.edges = -1000000;  // the release as it came
                posted.push_back(Msg{now, false, order++});
            }
        }
        // tick of the panel
        if ((int)(Rnd() % 1000) >= skipTick) {
            const uint32_t m = sampled | seen;
            seen = 0;
            if (m & ~prev) ++r.edges;
            prev = m;
        }
        int pass = 16 + (int)(Rnd() % 2);
        if ((int)(Rnd() % 1000) < pauseChance) pass = Rnd(pauseMin, pauseMax);
        r.longestPass = std::max(r.longestPass, pass);
        now += pass;
    }
    r.tableUpAtEnd = !table;
    r.clean = Clean(ks);
    r.heldBack = ks.heldBack;
    r.expired = ks.expired;
    return r;
}

std::vector<Press> Taps(int count, int minHeld, int maxHeld, int minApart, int maxApart) {
    std::vector<Press> v;
    int t = 500;
    for (int i = 0; i < count; ++i) {
        const int held = Rnd(minHeld, maxHeld);
        v.push_back(Press{t, t + held});
        t += held + Rnd(minApart, maxApart);
    }
    return v;
}

// ---- the pads: a reading thread every 4 ms, MK9's sampler at each pass, the panel's reader ----
struct PadResult {
    int edges = 0;
    uint32_t knownAtEnd = 0;
    int addedBetweenSamples = 0;
};
// source: what the reading thread sees while the button is down. mk9Bit: what MK9's sampler shows for it (0: MK9 does not
// read this source). learn: this many long, undisturbed presses come first.
PadResult RunPad(const std::vector<Press>& presses, uint32_t source, uint32_t mk9Bit, bool latchOn, int pauseChance, int pauseMin, int pauseMax) {
    Latch latch;
    Trust trust;
    PadResult r;
    uint32_t seen = 0, prev = 0;
    const int end = presses.back().up + 2000;
    size_t at = 0;
    int nextPass = 0, nextRead = 0;
    for (int now = 0; now < end; ++now) {
        while (at < presses.size() && presses[at].up <= now) ++at;
        const bool held = at < presses.size() && presses[at].down <= now;
        if (now >= nextRead) {
            latch.Read(held ? source : 0);
            nextRead = now + 4 + (int)(Rnd() % 3);  // 4 to 6 ms
        }
        if (now >= nextPass) {
            const uint32_t sampled = held ? mk9Bit : 0u;
            seen |= sampled;
            uint32_t lost = 0;
            trust.Observe(latch.Held(), sampled, &lost);
            const uint32_t pad = latchOn ? trust.Mask(latch.Take()) : 0u;
            if (pad & ~seen & ~sampled & ~prev) ++r.addedBetweenSamples;  // a press no sample had (not the tail of one)
            const uint32_t m = sampled | seen | pad;
            seen = 0;
            if (m & ~prev) ++r.edges;
            prev = m;
            int pass = 16 + (int)(Rnd() % 2);
            if ((int)(Rnd() % 1000) < pauseChance) pass = Rnd(pauseMin, pauseMax);
            nextPass = now + pass;
        }
    }
    r.knownAtEnd = trust.known;
    return r;
}

volatile LONG gStop = 0, gCycles = 0;
Latch gShared;
DWORD WINAPI Hammer(LPVOID) {
    while (!gStop) {
        gShared.Read(kPadRight);
        gShared.Read(0);
        InterlockedIncrement(&gCycles);
    }
    gShared.Read(0);
    return 0;
}
}  // namespace

int main() {
    setvbuf(stdout, nullptr, _IOFBF, 1 << 16);

    printf("== 1. a key's release, held back until the key was sampled\n");
    {
        KeyStretch k;
        KeyStretch::Up ups[32];
        k.KeyDown(kKey, 0);
        Expect(k.Sample(16, ups, 32) == 0, "a key that is down at a sample: nothing to post");
        Expect(!k.KeyUp(kKey, kWindow, kLParam, 100) && !k.down[kKey] && k.heldBack == 0, "its release later goes to the game as it comes");
    }
    {
        KeyStretch k;
        KeyStretch::Up ups[32];
        k.KeyDown(kKey, 0);
        Expect(k.KeyUp(kKey, kWindow, kLParam, 5) && k.heldBack == 1 && k.down[kKey], "pressed and released between two samples: the release is held back, the game has the key down");
        Expect(k.KeyUp(kKey, kWindow, kLParam, 6), "a second release of that key changes nothing");
        const int n = k.Sample(16, ups, 32);
        Expect(n == 1 && ups[0].vk == kKey && ups[0].window == kWindow && ups[0].lParam == kLParam, "after the next sample the release is posted, to the same window with the same lParam");
        Expect(k.inFlight[kKey] && !k.pending[kKey], "and is on its way");
        Expect(!k.KeyUp(kKey, kWindow, kLParam, 17) && !k.down[kKey] && Clean(k), "when it comes past it goes to the game: the key is up, nothing is left over");
        Expect(k.Sample(33, ups, 32) == 0, "the sample after posts nothing");
    }
    {
        KeyStretch k;
        KeyStretch::Up ups[32];
        k.KeyDown(kKey, 0);
        k.KeyUp(kKey, kWindow, kLParam, 30);
        k.KeyDown(kKey, 60);
        Expect(!k.pending[kKey] && k.down[kKey], "down, up, down in one gap: the held release is dropped, the key is simply down");
        Expect(k.KeyUp(kKey, kWindow, kLParam, 90) && k.heldBack == 2, "its second release in the same gap is held back in turn");
        Expect(k.Sample(100, ups, 32) == 1 && !k.KeyUp(kKey, kWindow, kLParam, 116) && !k.down[kKey] && Clean(k), "one release goes out after the sample and ends the press");
    }
    {
        KeyStretch k;
        KeyStretch::Up ups[32];
        k.KeyDown(kKey, 0);
        k.KeyUp(kKey, kWindow, kLParam, 5);
        k.Sample(16, ups, 32);    // the release is on its way
        k.KeyDown(kKey, 17);      // pressed again, queued before the posted release
        Expect(k.KeyUp(kKey, kWindow, kLParam, 17) && k.down[kKey] && !k.inFlight[kKey], "pressed again before the posted release came past: that release does not reach the game (the key is down)");
        k.Sample(33, ups, 32);
        Expect(!k.KeyUp(kKey, kWindow, kLParam, 200) && !k.down[kKey] && Clean(k), "the real release ends it");
    }
    {
        KeyStretch k;
        KeyStretch::Up ups[32];
        k.KeyDown(kKey, 0);
        k.KeyUp(kKey, kWindow, kLParam, 5);
        k.Sample(16, ups, 32);
        k.KeyDown(kKey, 17);
        Expect(k.KeyUp(kKey, kWindow, kLParam, 17), "posted release on its way, then down and up again in one pass: the first release to come past is dropped");
        Expect(k.KeyUp(kKey, kWindow, kLParam, 17) && k.pending[kKey], "the next one is held back, because no sample has seen the second press");
        Expect(k.Sample(33, ups, 32) == 1 && !k.KeyUp(kKey, kWindow, kLParam, 34) && !k.down[kKey] && Clean(k), "and goes out after the sample: the key is up, nothing is left over");
    }
    {
        KeyStretch k;
        KeyStretch::Up ups[32];
        k.KeyDown(kKey, 1000);
        k.KeyUp(kKey, kWindow, kLParam, 1010);
        Expect(k.Expire(1010 + KeyStretch::kMaxHoldMs - 1, ups, 32) == 0, "no sample comes: the release is kept for half a second");
        Expect(k.Expire(1010 + KeyStretch::kMaxHoldMs, ups, 32) == 1 && k.expired == 1, "then given back unsampled");
        Expect(!k.KeyUp(kKey, kWindow, kLParam, 1600) && !k.down[kKey] && Clean(k), "and the key is up");
    }
    {
        KeyStretch k;
        KeyStretch::Up ups[32];
        Expect(!k.KeyUp(kKey, kWindow, kLParam, 0) && k.heldBack == 0, "a key that went down before the hook was there: its release passes");
        k.KeyDown(kKey, 10);
        k.KeyUp(kKey, kWindow, kLParam, 12);
        k.Sample(16, ups, 32);  // posted, and it never comes past
        k.KeyDown(kKey, 16 + KeyStretch::kMaxFlightMs);
        Expect(!k.inFlight[kKey] && !k.drop[kKey], "a posted release that has not come past after a second is taken as lost");
        k.Sample(1100, ups, 32);
        Expect(!k.KeyUp(kKey, kWindow, kLParam, 1200) && !k.down[kKey] && Clean(k), "and the next real release is not swallowed for it");
        k.KeyDown(kKey, 2000);
        k.KeyUp(kKey, kWindow, kLParam, 2001);
        k.Sample(2016, ups, 32);
        k.PostFailed(kKey);
        k.KeyDown(kKey, 2020);
        k.Sample(2033, ups, 32);
        Expect(!k.KeyUp(kKey, kWindow, kLParam, 2040) && Clean(k), "a release whose post failed does not swallow a later one either");
    }
    {
        KeyStretch k;
        KeyStretch::Up ups[32];
        for (int vk = 0x41; vk < 0x41 + 20; ++vk) {
            k.KeyDown((uint8_t)vk, 0);
            k.KeyUp((uint8_t)vk, kWindow + (uintptr_t)vk, kLParam, 1);
        }
        const int first = k.Sample(16, ups, 16);
        bool own = first == 16;
        for (int i = 0; i < first; ++i) own = own && ups[i].window == kWindow + ups[i].vk;
        Expect(own && k.pendingCount == 4, "twenty keys tapped in one gap, sixteen releases per sample: sixteen go out, each to its own window");
        Expect(k.Sample(33, ups, 16) == 4 && k.pendingCount == 0, "the other four after the next sample");
        k.KeyDown(0x30, 40);
        k.KeyUp(0x30, kWindow, kLParam, 41);
        Expect(k.TakeAll(50, ups, 16) == 1 && ups[0].vk == 0x30, "the panel closes with a release held: it goes out at once");
        const uint32_t held = k.heldBack;
        k.Reset();
        Expect(Clean(k) && !k.down[0x30] && k.heldBack == held, "and the state starts anew, the counts kept");
    }

    printf("\n== 2. MK9's main loop with the keyboard: pump, sample, tick\n");
    {
        // The pauses of 2026-11-13: up to 252 ms; here up to 400.
        const std::vector<Press> taps = Taps(3000, 15, 150, 900, 1500);
        const uint32_t seed = gRng;
        const LoopResult without = RunKeyboard(taps, false, 60, 60, 400, 100);
        gRng = seed;
        const LoopResult with = RunKeyboard(taps, true, 60, 60, 400, 100);
        printf("3000 taps of 15 to 150 ms, a pause of 60 to 400 ms in 6 of 100 passes, no tick in 1 of 10: without the latch %d reach "
               "the panel (%d lost), with it %d (%u releases held back, %u given back unsampled)\n", without.edges, 3000 - without.edges,
               with.edges, with.heldBack, with.expired);
        Expect(without.edges < 3000 - 30, "without the latch such taps are lost (the fault is in the simulation)");
        Expect(with.edges == 3000, "with it every tap reaches the panel exactly once");
        Expect(with.heldBack > 30 && with.expired == 0, "by holding releases back, none longer than half a second");
        Expect(with.tableUpAtEnd && with.clean, "the game has the key up at the end and nothing is left over");
    }
    {
        const std::vector<Press> holds = Taps(1500, 200, 1200, 900, 1500);
        const uint32_t seed = gRng;
        const LoopResult without = RunKeyboard(holds, false, 60, 60, 400, 100);
        gRng = seed;
        const LoopResult with = RunKeyboard(holds, true, 60, 60, 400, 100);
        Expect(with.edges == 1500 && with.tableUpAtEnd && with.clean, "keys held 0.2 to 1.2 s: every press once, with the latch");
        Expect(with.sampledDown >= without.sampledDown, "and never fewer samples with the key down than without it");
    }
    {
        // Bursts of two or three quick taps: they may run together, they must not multiply or leave the key down.
        std::vector<Press> bursts;
        int t = 500, pressCount = 0, burstCount = 0;
        for (int b = 0; b < 2000; ++b) {
            const int n = Rnd(2, 3);
            for (int i = 0; i < n; ++i) {
                const int held = Rnd(10, 80);
                bursts.push_back(Press{t, t + held});
                t += held + Rnd(10, 80);
                ++pressCount;
            }
            ++burstCount;
            t += Rnd(900, 1500);
        }
        const LoopResult with = RunKeyboard(bursts, true, 60, 60, 400, 100);
        printf("%d bursts of two or three taps (10 to 80 ms down, 10 to 80 ms apart; %d presses): %d reach the panel\n", burstCount, pressCount, with.edges);
        Expect(with.edges >= burstCount && with.edges <= pressCount, "bursts of quick taps: at least one press per burst, never more presses than were made");
        Expect(with.tableUpAtEnd && with.clean, "the key is up at the end and nothing is left over");
    }
    {
        // Pauses longer than the half second a release is kept: taps may be lost as before, nothing may stick.
        const std::vector<Press> taps = Taps(1500, 15, 150, 2500, 3500);
        const LoopResult with = RunKeyboard(taps, true, 80, 300, 1500, 100);
        printf("pauses of 0.3 to 1.5 s: %d of 1500 taps reach the panel, %u releases held back, %u given back unsampled\n", with.edges, with.heldBack, with.expired);
        Expect(with.edges <= 1500 && with.edges >= 1400 && with.tableUpAtEnd && with.clean, "pauses of up to 1.5 s: no press counted twice, the key up at the end, nothing left over");
    }

    printf("\n== 3. the pads' sources\n");
    {
        Expect(SourcesOf(0x0001, 0, 0, 0) == kPadUp && SourcesOf(0x0002, 0, 0, 0) == kPadDown && SourcesOf(0x0004, 0, 0, 0) == kPadLeft &&
               SourcesOf(0x0008, 0, 0, 0) == kPadRight, "the d-pad's four bits of the XInput button word");
        Expect(SourcesOf(0x1000, 0, 0, 0) == kPadA && SourcesOf(0x2000 | 0x4000 | 0x8000 | 0x0010 | 0x0020 | 0x0300, 0, 0, 0) == 0, "A; B, X, Y, start, back and the shoulders are not sources");
        Expect(SourcesOf(0, 23999, 0, 0) == 0 && SourcesOf(0, 24000, 0, 0) == kStickRight && SourcesOf(0, -24000, 0, 0) == kStickLeft, "the stick counts from 24000 of 32767");
        Expect(SourcesOf(0, 0, 24000, 0) == kStickUp && SourcesOf(0, 0, -24000, 0) == kStickDown, "up is +Y");
        Expect(SourcesOf(0, 16001, 0, kStickRight) == kStickRight && SourcesOf(0, 16000, 0, kStickRight) == 0 && SourcesOf(0, 16001, 0, 0) == 0,
               "and stays until it is back inside 16000 (it does not flicker at the edge)");
        Expect(SourcesOf(0x1008, -32768, 32767, 0) == (kPadRight | kPadA | kStickLeft | kStickUp), "several at once");
        Expect(kMaskOf[0] == 1 && kMaskOf[1] == 2 && kMaskOf[2] == 4 && kMaskOf[3] == 8 && kMaskOf[4] == 0x20 && kMaskOf[5] == 1 && kMaskOf[6] == 2 &&
               kMaskOf[7] == 4 && kMaskOf[8] == 8, "their pad record bits: 1 up, 2 down, 4 left, 8 right, 0x20 confirm");
    }
    {
        Trust t;
        uint32_t lost = 0;
        Expect(t.Mask(0x1FF) == 0, "nothing is used before MK9's sampler has agreed");
        Expect(t.Observe(kPadRight, 8, &lost) == 0 && t.Mask(kPadRight) == 0, "one agreeing sample is not enough");
        Expect(t.Observe(kPadRight, 8, &lost) == kPadRight && t.Mask(kPadRight) == 8, "two in a row are: d-pad right is known");
        Expect(t.Mask(kStickRight) == 0 && t.Mask(kPadLeft | kPadA) == 0, "that says nothing about the stick or another button");
        t.Observe(kPadRight, 0, &lost);
        t.Observe(0, 0, &lost);
        for (int i = 0; i < Trust::kDiffer - 1; ++i) t.Observe(kPadRight, 0, &lost);
        Expect(t.Mask(kPadRight) == 8, "held but missing from eleven samples: still used");
        t.Observe(kPadRight, 0, &lost);
        Expect(lost == kPadRight && t.Mask(kPadRight) == 0, "from twelve in a row: not used any more");
        t.Observe(kPadRight, 8, &lost);
        Expect(t.Observe(kPadRight, 8, &lost) == kPadRight && t.Mask(kPadRight) == 8, "until the two agree again");
        t.Observe(kPadA, 0x20, &lost);
        Expect(t.Observe(kPadA, 0x20, &lost) == kPadA && t.Mask(kPadA) == 0x20, "A is known by the menus' confirm bit 0x20");
        t.Observe(kPadA, 0x1000, &lost);
        Expect(t.agree[4] == 0, "and by no other bit");
        t.Observe(kStickUp | kPadRight, 8, &lost);
        t.Observe(kStickUp | kPadRight, 8, &lost);
        Expect(t.Mask(kStickUp) == 0, "a stick direction MK9 does not show stays unused while another source agrees");
    }
    {
        Latch l;
        l.Read(kPadRight);
        l.Read(0);
        Expect(l.Take() == kPadRight && l.Take() == 0, "held at one reading and let go: the next take has it, the one after not");
        l.Read(kPadA);
        Expect(l.Take() == kPadA && l.Take() == kPadA && l.Held() == kPadA, "still held and no new reading: every take has it");
        l.Read(0);
        l.Take();
        Expect(l.Take() == 0, "let go: gone after the take that follows");
        l.Read(kPadUp);
        l.Clear();
        Expect(l.Take() == 0 && l.Held() == 0, "cleared with the panel");
    }
    {
        HANDLE h = CreateThread(nullptr, 0, Hammer, nullptr, 0, nullptr);
        const DWORD until = GetTickCount() + 300;
        long takes = 0, withBit = 0, edges = 0;
        uint32_t prev = 0;
        while ((LONG)(GetTickCount() - until) < 0) {
            const uint32_t s = gShared.Take();
            ++takes;
            if (s & kPadRight) ++withBit;
            if (s & ~prev) ++edges;
            prev = s;
        }
        InterlockedExchange(&gStop, 1);
        WaitForSingleObject(h, 2000);
        CloseHandle(h);
        gShared.Take();
        printf("two threads for 0.3 s: %ld press-and-release cycles written, %ld takes, %ld with the bit, %ld beginnings\n", gCycles, takes, withBit, edges);
        Expect(gCycles > 1000 && takes > 1000 && withBit > 0 && edges <= gCycles + 1, "a writer and a taker at full speed: presses are seen, never more beginnings than presses");
        Expect(gShared.Take() == 0 && gShared.Held() == 0, "and nothing is left when the writer has stopped");
    }

    printf("\n== 4. a pad in MK9's main loop\n");
    {
        // Three long presses first (MK9's sampler and the reading agree on them), then taps under pauses.
        std::vector<Press> presses;
        int t = 500;
        for (int i = 0; i < 3; ++i) {
            presses.push_back(Press{t, t + 250});
            t += 1250;
        }
        for (const Press& p : Taps(3000, 15, 150, 900, 1500)) presses.push_back(Press{p.down + t, p.up + t});
        const int total = (int)presses.size();
        const uint32_t seed = gRng;
        const PadResult without = RunPad(presses, kPadRight, 8, false, 60, 60, 400);
        gRng = seed;
        const PadResult with = RunPad(presses, kPadRight, 8, true, 60, 60, 400);
        printf("%d d-pad presses (3 long ones, then taps of 15 to 150 ms), pauses of 60 to 400 ms: without the latch %d reach the panel "
               "(%d lost), with it %d (%d added between samples)\n", total, without.edges, total - without.edges, with.edges, with.addedBetweenSamples);
        Expect(without.edges < total - 30, "without the latch a tap inside a pause is lost (the fault is in the simulation)");
        Expect(with.edges == total, "with it every press reaches the panel exactly once");
        Expect(with.knownAtEnd == kPadRight && with.addedBetweenSamples > 30, "d-pad right became known from MK9's own samples, and presses were added between samples");
    }
    {
        // The very first press is a tap inside a pause: nothing is known yet, so it is lost as before. No guess is made.
        Latch l;
        Trust tr;
        for (int now = 990; now < 1060; now += 4) l.Read(now >= 1000 && now < 1040 ? kPadA : 0);
        uint32_t lost = 0;
        tr.Observe(l.Held(), 0, &lost);
        Expect(tr.Mask(l.Take()) == 0, "a first press that MK9's sampler never saw is not given to the panel: nothing is assumed about a pad");
    }
    {
        // A source MK9 does not read (say, a stick it ignores on this screen): never used.
        const std::vector<Press> presses = Taps(400, 15, 600, 900, 1500);
        const PadResult with = RunPad(presses, kStickRight, 0, true, 60, 60, 400);
        Expect(with.edges == 0 && with.knownAtEnd == 0 && with.addedBetweenSamples == 0, "a source that MK9's sampler never shows gives the panel nothing");
    }
    {
        // A source MK9 shows under another bit than the one assumed here: never used either.
        const std::vector<Press> presses = Taps(400, 15, 600, 900, 1500);
        const PadResult with = RunPad(presses, kPadA, 0x10, true, 60, 60, 400);
        Expect(with.knownAtEnd == 0 && with.addedBetweenSamples == 0, "A under another bit than 0x20: the latch adds nothing (the panel gets MK9's sample alone)");
    }

    printf("\n%d checks, %d failed\n", gChecks, gFailed);
    if (!gFailed) printf("ALL CHECKS PASSED\n");
    return gFailed ? 1 : 0;
}
