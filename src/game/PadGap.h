#pragma once
// Presses the game never read (2026-11-21, the user after the evening of 2026-11-20: "I felt sometimes that my inputs
// where been eaten"). That evening's logs showed every sampled press reaching the fight; what a log could not show is a
// press that began and ended between two samples of the controls: MK9 looks at its controllers once per pass of its main
// loop (in a rollback session at the frame boundary), so a tap made while the game stands still (waiting for the other
// PC, an Alt+Tab, a slow frame) is never read by anything. This counts them, and changes nothing the game reads:
//
//  - Pads. A thread of the mod reads the XInput controllers every 2 ms and notes every press of a button, a trigger or a
//    direction of the left stick with the time it began and ended. At each sample of the controls the game thread takes
//    the presses that have ended: one with no sample between its start and its end was never read.
//  - Keyboard. MK9 takes its keys from the window's key messages; a key whose down and up messages both reach the game
//    with no sample in between was never read (a message hook on the window's thread that only counts).
//
// Only while a rollback session has the controls ('padgap 1', the default); 'padgap 2' also outside sessions (offline
// fights and menus, for comparison), 'padgap 0' off. PADGAP lines in the log.
//
// This header holds the logic without Windows (tests\host\padgap_test.cpp); PadGap.cpp holds the thread, the hook and
// the log lines.
#include <atomic>
#include <cstdint>

namespace PadGap {

// ---- what the reading thread tells apart ----
// Bits 0-15: XInput's button word as it is (d-pad 1/2/4/8, Start 0x10, Back 0x20, the stick buttons 0x40/0x80, LB 0x100,
// RB 0x200, A 0x1000, B 0x2000, X 0x4000, Y 0x8000). Then the triggers and the left stick's directions.
enum : uint32_t {
    kLT = 1u << 16, kRT = 1u << 17,
    kStickUp = 1u << 18, kStickDown = 1u << 19, kStickLeft = 1u << 20, kStickRight = 1u << 21,
};
constexpr uint32_t kStick = kStickUp | kStickDown | kStickLeft | kStickRight;
constexpr int kTrigOn = 30, kTrigOff = 20;          // of 255 (XInput's own threshold is 30); off again below the second
constexpr int kStickOn = 24000, kStickOff = 16000;  // of 32767, as PressLatch.h

inline uint32_t SourcesOf(uint16_t buttons, int lt, int rt, int lx, int ly, uint32_t before) {
    uint32_t s = buttons;
    if (lt >= kTrigOn || ((before & kLT) && lt >= kTrigOff)) s |= kLT;
    if (rt >= kTrigOn || ((before & kRT) && rt >= kTrigOff)) s |= kRT;
    if (lx >= kStickOn || ((before & kStickRight) && lx > kStickOff)) s |= kStickRight;
    if (lx <= -kStickOn || ((before & kStickLeft) && lx < -kStickOff)) s |= kStickLeft;
    if (ly >= kStickOn || ((before & kStickUp) && ly > kStickOff)) s |= kStickUp;  // XInput: +Y is up
    if (ly <= -kStickOn || ((before & kStickDown) && ly < -kStickOff)) s |= kStickDown;
    return s;
}

// A press that has ended, in QueryPerformanceCounter ticks of the reading thread. It began after `downAfter` (the
// reading before the one that first showed it) and ended at or before `up` (the reading that first showed it released).
struct Press {
    uint32_t source;  // one bit
    int64_t downAfter, down, up;
};

// The reading thread's side: one reading of all controllers (OR of the sources held) at time t, the reading before at
// tPrev. Writes the presses that ended at this reading to out (at most max; the rest are counted in *lost).
struct Edges {
    uint32_t held = 0;
    int64_t downAfter[32] = {}, down[32] = {};
    int Read(uint32_t now, int64_t t, int64_t tPrev, Press* out, int max, uint32_t* lost) {
        const uint32_t pressed = now & ~held, released = held & ~now;
        int n = 0;
        for (int i = 0; i < 32; ++i) {
            const uint32_t bit = 1u << i;
            if (pressed & bit) {
                downAfter[i] = tPrev;
                down[i] = t;
            }
            if (released & bit) {
                if (n < max) out[n++] = Press{bit, downAfter[i], down[i], t};
                else ++*lost;
            }
        }
        held = now;
        return n;
    }
    void Reset() { *this = Edges{}; }
};

// One writer (the reading thread), one reader (the game thread).
template <int N>
struct Ring {
    Press items[N];
    std::atomic<uint32_t> w{0}, r{0};
    std::atomic<uint32_t> dropped{0};
    void Push(const Press& p) {
        const uint32_t wi = w.load(std::memory_order_relaxed);
        if (wi - r.load(std::memory_order_acquire) >= (uint32_t)N) {
            dropped.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        items[wi % N] = p;
        w.store(wi + 1, std::memory_order_release);
    }
    bool Pop(Press* p) {
        const uint32_t ri = r.load(std::memory_order_relaxed);
        if (ri == w.load(std::memory_order_acquire)) return false;
        *p = items[ri % N];
        r.store(ri + 1, std::memory_order_release);
        return true;
    }
};

// The game thread's side: the times of the last samples of the controls, and whether a press was read.
struct Samples {
    static constexpr int kKeep = 256;
    int64_t t[kKeep] = {};
    uint32_t n = 0;
    void Add(int64_t at) { t[n++ % kKeep] = at; }
    int64_t Last() const { return n ? t[(n - 1) % kKeep] : 0; }
    int64_t Oldest() const { return n == 0 ? 0 : n <= (uint32_t)kKeep ? t[0] : t[n % kKeep]; }
    // A sample after `from` and at or before `to` (the game may have read the press then).
    bool AnyIn(int64_t from, int64_t to) const {
        const uint32_t k = n < (uint32_t)kKeep ? n : (uint32_t)kKeep;
        for (uint32_t i = 0; i < k; ++i) {
            const int64_t s = t[(n - 1 - i) % kKeep];
            if (s > from && s <= to) return true;
            if (s <= from) break;  // older ones are earlier still
        }
        return false;
    }
    // The last sample at or before `at` and the first after `at2` (0 when not kept / not yet).
    void Around(int64_t at, int64_t at2, int64_t* before, int64_t* after) const {
        *before = *after = 0;
        const uint32_t k = n < (uint32_t)kKeep ? n : (uint32_t)kKeep;
        for (uint32_t i = 0; i < k; ++i) {
            const int64_t s = t[(n - 1 - i) % kKeep];
            if (s > at2) *after = s;
            if (s <= at) {
                *before = s;
                break;
            }
        }
    }
};

enum class Verdict { Read, Missed, Unknown, NotYet };
// A press that has ended, against the samples so far. NotYet: it ended after the last sample (decided at a later one).
// Read: a sample fell between its start and its end. Unknown: none of the samples kept did, but it began before the
// oldest of them (the count began, or the kept ones ran out, while it was held). Missed: no sample in between.
inline Verdict Judge(const Samples& s, const Press& p) {
    if (!s.n || p.up > s.Last()) return Verdict::NotYet;
    if (s.AnyIn(p.downAfter, p.up)) return Verdict::Read;
    return p.downAfter < s.Oldest() ? Verdict::Unknown : Verdict::Missed;
}

// ---- keyboard: a key whose down and up both came with no sample in between ----
struct Keys {
    static constexpr int kKeys = 256;
    uint32_t samples = 0;
    bool down[kKeys] = {};
    uint32_t downAt[kKeys] = {};
    int64_t downT[kKeys] = {};
    void Sample() { ++samples; }
    void Down(uint8_t vk, int64_t t) {
        if (down[vk]) return;  // auto-repeat
        down[vk] = true;
        downAt[vk] = samples;
        downT[vk] = t;
    }
    // 1: the key went down and up again with no sample of the controls in between (never read); 0: a sample saw it down;
    // -1: no down of it was counted (it went down before the count began). *heldT: how long it was down.
    int Up(uint8_t vk, int64_t t, int64_t* heldT) {
        if (!down[vk]) return -1;
        down[vk] = false;
        *heldT = t - downT[vk];
        return downAt[vk] == samples ? 1 : 0;
    }
    void Reset() {
        const uint32_t s = samples;
        *this = Keys{};
        samples = s;
    }
};

// ---- the part with Windows (PadGap.cpp) ----
void SetMode(int mode);  // 'padgap 0|1|2': off / rollback sessions (the default) / also outside sessions
int Mode();
// Every sample of the controls (game thread), with the time just before the game read them, whether a rollback session
// has the controls (Input's capture) and whether a panel of the mod has them (its presses are the panel's business).
void OnSample(int64_t qpcBefore, bool session, bool panel);
void Shutdown();  // the DLL unloads: the hook comes off, the thread ends

}  // namespace PadGap
