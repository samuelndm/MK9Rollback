#pragma once
// A press that falls between two samples of the controls, for a panel of the mod (the stage screen, feature plan 4.2).
//
// MK9 looks at its controls once per pass of its main loop (the sampler 0x00424A50; Input.cpp wraps its one call). While
// the game loads, a pass takes far longer: on 2026-11-13 the controls went unread for 252, 103, 184 and 71 ms in a row
// while the title screen loaded its demo fight, and a key held 120 ms inside the first of these was never seen, by the
// panel or by anything else. Two things are done about it, both only while a panel has the controls (Input::SetPanelHold)
// and neither touching what a fight or the game's own screens read:
//
//  - Keyboard. MK9 takes its keys from the window's key messages (posted WM_KEYDOWN / WM_KEYUP reach its sampler, the
//    way a pad does). A key pressed and released between two samples has both messages taken in one pass. The release of
//    such a key is held back (a message hook on the window's thread turns it into WM_NULL) until the sampler has run once
//    with the key down, then posted to the window again. MK9's own key assignment decides what the key means, as before.
//  - Pads. A thread of the mod reads the XInput controllers every few milliseconds and remembers what was held at any
//    reading since the panel last asked. Its bits are the pad record's (1 up, 2 down, 4 left, 8 right, 0x20 the menus'
//    confirm). A source (d-pad direction, A, a direction of the left stick) is used only once MK9's own sampler has shown
//    the same bit while this thread saw the source held, so nothing is assumed about a controller MK9 reads otherwise:
//    until then, and for every other device, the panel gets what MK9's sampler gives it, as before.
//
// This header holds the logic without Windows (tests\host\presslatch_test.cpp runs it over simulated passes, pauses and
// presses); PressLatch.cpp holds the hook, the thread and the log lines.
#include <atomic>
#include <cstdint>

namespace PressLatch {

// ---- keyboard: a release held back until the key was sampled ----
struct KeyStretch {
    static constexpr int kKeys = 256;
    static constexpr uint32_t kMaxHoldMs = 500;  // a release is never kept longer (no sample came: a screen that reads nothing)
    struct Up {
        uint8_t vk;
        uintptr_t window, lParam;
    };

    static constexpr uint32_t kMaxFlightMs = 1000;  // a release posted again that has not come past by then is taken as lost

    uint32_t samples = 0;     // sampler calls so far
    uint32_t heldBack = 0;    // releases held back so far
    uint32_t expired = 0;     // of them, given back unsampled after kMaxHoldMs
    uint32_t downAt[kKeys] = {};
    bool down[kKeys] = {};      // the game has seen the key go down and not up
    bool pending[kKeys] = {};   // its release is held back
    bool inFlight[kKeys] = {};  // its release was posted again and has not come past yet
    bool drop[kKeys] = {};      // the key went down again before that: the posted release must not reach the game
    uintptr_t window[kKeys] = {}, lParam[kKeys] = {};
    uint32_t pendingSince[kKeys] = {}, flightSince[kKeys] = {};
    int pendingCount = 0;

    // A release posted again never came past (the post failed, or the window is gone): the key is as any other again.
    void PostFailed(uint8_t vk) { inFlight[vk] = drop[vk] = false; }

    // A WM_KEYDOWN is about to reach the game.
    void KeyDown(uint8_t vk, uint32_t nowMs) {
        Forget(vk, nowMs);
        if (inFlight[vk]) {
            drop[vk] = true;  // the game keeps the key down: for it the two presses are one long one
            downAt[vk] = samples;
        } else if (pending[vk]) {
            pending[vk] = false;  // pressed again before the release went out: one press, still down
            --pendingCount;
        } else if (!down[vk]) {
            down[vk] = true;
            downAt[vk] = samples;
        }
    }
    // A WM_KEYUP is about to reach the game. True: hold it back (the caller makes it a WM_NULL).
    bool KeyUp(uint8_t vk, uintptr_t wnd, uintptr_t lp, uint32_t nowMs) {
        Forget(vk, nowMs);
        if (inFlight[vk]) {  // the first release to come past stands for the one posted again
            inFlight[vk] = false;
            if (drop[vk]) {
                drop[vk] = false;
                return true;
            }
            down[vk] = false;
            return false;
        }
        if (pending[vk]) return true;  // a second release of a key whose release is held: nothing new
        if (down[vk] && downAt[vk] == samples) {  // no sample has seen it down
            pending[vk] = true;
            ++pendingCount;
            ++heldBack;
            window[vk] = wnd;
            lParam[vk] = lp;
            pendingSince[vk] = nowMs;
            return true;
        }
        down[vk] = false;
        return false;
    }
    // The sampler has run. Fills out[] with the releases to post now; returns their number (the rest, should there be
    // more than max, go out at the next sample).
    int Sample(uint32_t nowMs, Up* out, int max) {
        ++samples;
        return Release(out, max, false, nowMs);
    }
    // Releases held longer than kMaxHoldMs (no sample came). Same output.
    int Expire(uint32_t nowMs, Up* out, int max) { return pendingCount ? Release(out, max, true, nowMs) : 0; }
    // The panel gives the controls back: call until it returns less than max, post them all, then Reset.
    int TakeAll(uint32_t nowMs, Up* out, int max) { return Release(out, max, false, nowMs); }
    void Reset() {
        const uint32_t s = samples, h = heldBack, e = expired;
        *this = KeyStretch{};
        samples = s;
        heldBack = h;
        expired = e;
    }

private:
    void Forget(uint8_t vk, uint32_t nowMs) {
        if (inFlight[vk] && (uint32_t)(nowMs - flightSince[vk]) >= kMaxFlightMs) inFlight[vk] = drop[vk] = false;
    }
    int Release(Up* out, int max, bool onlyOld, uint32_t nowMs) {
        int n = 0;
        for (int vk = 0; vk < kKeys && pendingCount > 0 && n < max; ++vk) {
            if (!pending[vk]) continue;
            if (onlyOld) {
                if ((uint32_t)(nowMs - pendingSince[vk]) < kMaxHoldMs) continue;
                ++expired;
            }
            pending[vk] = false;
            --pendingCount;
            inFlight[vk] = true;
            drop[vk] = false;
            flightSince[vk] = nowMs;
            out[n++] = Up{(uint8_t)vk, window[vk], lParam[vk]};
        }
        return n;
    }
};

// ---- pads: what was held at any reading since the panel last asked ----
// Sources: what the reading thread tells apart. Their order is the XInput button word's for the d-pad.
enum : uint32_t {
    kPadUp = 0x001, kPadDown = 0x002, kPadLeft = 0x004, kPadRight = 0x008, kPadA = 0x010,
    kStickUp = 0x020, kStickDown = 0x040, kStickLeft = 0x080, kStickRight = 0x100,
};
constexpr int kSources = 9;
// The pad record's bit each source stands for (1 up, 2 down, 4 left, 8 right, 0x20 the menus' confirm).
constexpr uint32_t kMaskOf[kSources] = {1, 2, 4, 8, 0x20, 1, 2, 4, 8};
constexpr int kStickOn = 24000, kStickOff = 16000;  // of 32767: on beyond the first, off again inside the second

// One controller's sources from its XInput state (wButtons, sThumbLX, sThumbLY) and what it showed at the reading before.
inline uint32_t SourcesOf(uint16_t buttons, int lx, int ly, uint32_t before) {
    uint32_t s = (buttons & 0xFu) | ((buttons & 0x1000u) ? kPadA : 0u);
    if (lx >= kStickOn || ((before & kStickRight) && lx > kStickOff)) s |= kStickRight;
    if (lx <= -kStickOn || ((before & kStickLeft) && lx < -kStickOff)) s |= kStickLeft;
    if (ly >= kStickOn || ((before & kStickUp) && ly > kStickOff)) s |= kStickUp;  // XInput: +Y is up
    if (ly <= -kStickOn || ((before & kStickDown) && ly < -kStickOff)) s |= kStickDown;
    return s;
}

// Which sources MK9's own sampler has agreed with. Sampler's thread only.
struct Trust {
    static constexpr int kAgree = 2;    // samples in a row with the source held and its bit in MK9's sample: known
    static constexpr int kDiffer = 12;  // samples in a row with the source held and the bit missing: not known any more
    uint8_t agree[kSources] = {}, differ[kSources] = {};
    uint32_t known = 0;

    // One sample: the sources the reading thread saw held at its last reading, and MK9's sampled mask.
    // Returns the sources that became known; *lost gets those that stopped being known.
    uint32_t Observe(uint32_t held, uint32_t sampled, uint32_t* lost) {
        uint32_t gained = 0;
        *lost = 0;
        for (int i = 0; i < kSources; ++i) {
            const uint32_t bit = 1u << i;
            if (!(held & bit)) {
                agree[i] = differ[i] = 0;
            } else if (sampled & kMaskOf[i]) {
                differ[i] = 0;
                if (agree[i] < 255) ++agree[i];
                if (agree[i] >= kAgree && !(known & bit)) {
                    known |= bit;
                    gained |= bit;
                }
            } else {
                agree[i] = 0;
                if (differ[i] < 255) ++differ[i];
                if (differ[i] >= kDiffer && (known & bit)) {
                    known &= ~bit;
                    *lost |= bit;
                }
            }
        }
        return gained;
    }
    // The pad record bits of the known sources among these.
    uint32_t Mask(uint32_t sources) const {
        uint32_t m = 0;
        for (int i = 0; i < kSources; ++i)
            if (sources & known & (1u << i)) m |= kMaskOf[i];
        return m;
    }
};

// Written by the reading thread, taken by the game thread.
struct Latch {
    std::atomic<uint32_t> seen{0}, held{0};
    void Read(uint32_t sourcesNow) {
        held.store(sourcesNow, std::memory_order_relaxed);
        seen.fetch_or(sourcesNow, std::memory_order_relaxed);
    }
    // Everything held at any reading since the last take; what is held now stays in for the next one.
    uint32_t Take() { return seen.exchange(held.load(std::memory_order_relaxed), std::memory_order_relaxed); }
    uint32_t Held() const { return held.load(std::memory_order_relaxed); }
    void Clear() {
        held.store(0, std::memory_order_relaxed);
        seen.store(0, std::memory_order_relaxed);
    }
};

// ---- the part with Windows (PressLatch.cpp) ----
void SetWanted(bool on);              // 'stagelatch 0|1'; off at start (PressLatch.cpp). Off: the panel reads MK9's samples alone
bool Wanted();
void Begin();                         // a panel takes the controls (game thread)
void End();                           // it gives them back: held releases go out, the reading thread rests
void OnSample(uint32_t sampledMask);  // after every sample of the controls under the panel (the sampler's thread)
uint32_t TakePad();                   // pad record bits the known pad sources showed since the last call (game thread)
void NotePadAdded(uint32_t bits);     // the caller found bits in TakePad that no sample had: counted, the first ones logged
void Shutdown();                      // the DLL unloads: hook off, held releases out, thread ended
void Describe(char* out, int n);      // one line for 'stagestatus'

}  // namespace PressLatch
