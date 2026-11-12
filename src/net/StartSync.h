#pragma once
#include <cstdint>

// Start alignment of a rollback session's two games (2026-11-12, netcode audit).
//
// The two games reach GGPO's frame 0 at different moments. The one that gets there first waits for the other's first
// input (the start barrier, RollbackManager.cpp), and after the wait its frame grid has to start as much later as the
// other game's does: otherwise it stays ahead and every input change of the other player rolls it back. Through rc29
// that amount was the wait less the network's one-way trip, in whole frames, with the trip guessed as half of (the
// handshake's round trip less one frame of poll latency). On the first two-PC evening (2026-11-11, 14 starts) the
// handshake read the wire's own round trip in 6 starts (27-28 ms; the guess: 5-6 ms for a 13.5 ms trip), the waiting
// game moved one frame too many, the other game ran a frame ahead until BALANCE or the phase search caught it, and 159
// of that game's 279 rollbacks of the evening fell into those six starts' first seconds.
//
// Here the two games measure the offset instead. From its frame-0 boundary on each game sends small datagrams on the
// checksum channel that say how long, at the moment of sending, until its own frame 1 starts (the frame limiter's
// deadline; negative once past it). A game that reads one knows
//     gap = (the sender's time to its frame 1) - (its own time to its frame 1) = (their frame 1 - my frame 1) + trip
// and sends the smallest gap it has seen along in its own datagrams. With both games' gaps
//     their frame 1 - my frame 1 = (my gap - their gap) / 2          trip = (my gap + their gap) / 2
// whatever the trip is, as long as it is the same both ways (it is one Steam connection). The game whose frame 1 comes
// first moves its deadline later by exactly that difference; the other one stays. No clock is shared: each game only
// subtracts times of its own clock.
//
// How exact a gap is depends on how soon after its arrival a datagram is read. The waiting game polls every millisecond
// (the barrier's loop) and sends on every poll, so (1) it reads the other game's first datagram within a millisecond,
// and (2) the game that arrives second finds, at its frame-0 boundary, a row of datagrams of which the newest left one
// send interval before the last one that could have arrived. Both gaps are then good to about a millisecond with no
// extra wait for either game. When both games wait (they arrived within one trip of each other) each reads the other
// in its loop, and the one whose partner's gap is still on the way when its barrier ends decides a frame or two later:
// the gap is a number, not a time, so it can be read by the ordinary once-a-frame poll. A gap that was itself taken by
// a once-a-frame poll (only after lost datagrams) can read up to a frame too large; the smallest one counts.
//
// Nothing here touches the game or the network: RollbackManager.cpp does the reading and sending, and a host-side test
// (tests\host\startsync_test.cpp) runs two simulated games through the same calls.
namespace StartSync {

constexpr uint32_t kMagic = 0x53394B4D;  // 'MK9S'
constexpr int32_t kNone = INT32_MIN;
constexpr int64_t kFrameUs = 16667;
constexpr int64_t kMaxUs = 10'000'000;  // a time more than 10 s from a frame-0 boundary is not a start's
constexpr int64_t kBurstUs = 900;       // in the wait loop: a datagram on every 1 ms poll ...
constexpr int64_t kBurstForUs = 100'000;  // ... for the first 100 ms of the wait,
constexpr int64_t kSlowUs = 3'900;      // then every fourth poll (a peer that takes seconds to arrive)
constexpr int kResendFrames = 5;        // after the barrier: one datagram a frame through this frame (lost ones)
constexpr int kFrames = 12;             // the frame at which a game stops waiting for the other game's gap
constexpr int64_t kMinShiftUs = 500;    // a smaller difference is not moved
constexpr int64_t kNoiseUs = 3000;      // the two gaps may add up to this much less than nothing (poll and send jitter)

struct Packet {  // 20 bytes: no other datagram of the checksum channel has this size and magic
    uint32_t magic;
    int32_t untilUs;  // from this send to the sender's frame 1 (negative once past it)
    int32_t gapUs;    // the sender's smallest gap so far; kNone before its first read
    uint32_t flags;   // bit 0: that gap is as good as a read within a millisecond or a send interval of the arrival
    uint32_t reads;   // datagrams of the other game the sender has read (the log)
};
static_assert(sizeof(Packet) == 20, "five 32-bit fields");

struct Verdict {
    enum Kind { kWait, kShift, kStay, kUnknown } kind = kWait;
    int32_t shiftUs = 0;       // kShift: this game's frame deadline moves this much later
    int32_t aheadUs = 0;       // their frame 1 minus this game's (kShift, kStay); with `bound`, at most this
    int32_t tripUs = kNone;    // the measured one-way trip (both gaps known)
    bool exact = false;        // both gaps came from prompt reads
    bool guessed = false;      // the other game's gap never arrived: this game's gap less the caller's trip estimate
    bool bound = false;        // kStay without the other game's gap: this game's gap is negative, so it is the later one
};

class Session {
public:
    // This game's frame-0 boundary: the time now and the start of its frame 1, both on one clock of this game's.
    void Begin(int64_t nowUs, int64_t frame1Us) {
        *this = Session();
        begun_ = true;
        t0_ = nowUs;
        frame1_ = frame1Us;
    }
    bool Begun() const { return begun_; }
    bool Decided() const { return decided_; }
    uint32_t Reads() const { return reads_; }
    uint32_t Sends() const { return sends_; }
    int32_t Mine() const { return mine_; }
    int32_t Peer() const { return peer_; }
    bool MinePrompt() const { return minePrompt_; }

    // A datagram of the other game, read at nowUs. prompt: by the wait loop's 1 ms poll, or by the frame-0 boundary's
    // first read (the other game is then still sending on every poll); not prompt: by a later once-a-frame poll.
    void OnPacket(const Packet& p, int64_t nowUs, bool prompt) {
        if (!begun_ || p.magic != kMagic || p.untilUs > kMaxUs || p.untilUs < -kMaxUs) return;
        ++reads_;
        const int64_t gap = (int64_t)p.untilUs - (frame1_ - nowUs);
        if (gap > -kMaxUs && gap < kMaxUs) {
            // A gap is the offset plus the trip plus however late the read was, so the smallest is the best one, and once
            // one prompt read has been made the smallest is at least as good as that one, whichever read it came from.
            if (prompt) minePrompt_ = true;
            if (mine_ == kNone || gap < mine_) {
                mine_ = (int32_t)gap;
                unsent_ = true;  // the other game decides with it: it goes out at once
            }
        }
        if (p.gapUs != kNone && p.gapUs > -kMaxUs && p.gapUs < kMaxUs && (peer_ == kNone || p.gapUs < peer_)) {
            peer_ = p.gapUs;
            peerPrompt_ = p.flags & 1;
        }
    }

    // In the barrier's wait loop, after each poll's reads: send now?
    bool WantSend(int64_t nowUs) const {
        if (!begun_) return false;
        if (!sends_ || unsent_) return true;
        return nowUs - lastSend_ >= (nowUs - t0_ < kBurstForUs ? kBurstUs : kSlowUs);
    }
    // At the boundary of a later frame (1, 2, ...), after its reads.
    bool WantSendAtFrame(int frame) const { return begun_ && (unsent_ || frame <= kResendFrames); }
    Packet Make(int64_t nowUs) {
        int64_t until = frame1_ - nowUs;
        if (until > kMaxUs) until = kMaxUs;
        if (until < -kMaxUs) until = -kMaxUs;
        const Packet p{kMagic, (int32_t)until, mine_, minePrompt_ ? 1u : 0u, reads_};
        ++sends_;
        lastSend_ = nowUs;
        unsent_ = false;
        return p;
    }

    // kWait until there is something to decide with; any other answer is given once. waitedUs: how long this game
    // waited at its start barrier (it cannot be further ahead than that plus its own lateness at the boundary, which
    // the limiter keeps under three frames: a larger answer is another start's datagram or a broken clock, and counts
    // as nothing). final: the caller stops reading (frame kFrames); a game that then has only its own gap takes
    // tripGuessUs for the trip.
    Verdict Decide(bool final, int64_t waitedUs, int32_t tripGuessUs) {
        Verdict v;
        if (!begun_ || decided_) return v;
        const int64_t most = waitedUs + 4 * kFrameUs;
        if (mine_ != kNone && peer_ != kNone) {
            const int64_t trip = ((int64_t)mine_ + peer_) / 2, ahead = ((int64_t)mine_ - peer_) / 2;
            if (trip < -kNoiseUs || ahead > most) {
                v.kind = Verdict::kUnknown;
            } else {
                v.kind = ahead >= kMinShiftUs ? Verdict::kShift : Verdict::kStay;
                v.shiftUs = v.kind == Verdict::kShift ? (int32_t)ahead : 0;
                v.aheadUs = (int32_t)ahead;
                v.tripUs = (int32_t)(trip < 0 ? 0 : trip);
                v.exact = minePrompt_ && peerPrompt_;
            }
        } else if (!final) {
            return v;
        } else if (mine_ == kNone) {
            v.kind = Verdict::kUnknown;
        } else if (mine_ < 0) {  // their frame 1 + trip is before this game's frame 1: this game is the later one
            v.kind = Verdict::kStay;
            v.aheadUs = mine_;
            v.bound = true;
        } else {
            const int64_t ahead = (int64_t)mine_ - (tripGuessUs > 0 ? tripGuessUs : 0);
            if (ahead > most) {
                v.kind = Verdict::kUnknown;
            } else {
                v.kind = ahead >= kMinShiftUs ? Verdict::kShift : Verdict::kStay;
                v.shiftUs = v.kind == Verdict::kShift ? (int32_t)ahead : 0;
                v.aheadUs = (int32_t)ahead;
                v.guessed = true;
            }
        }
        decided_ = true;
        return v;
    }

private:
    bool begun_ = false, decided_ = false, unsent_ = false, minePrompt_ = false, peerPrompt_ = false;
    int64_t t0_ = 0, frame1_ = 0, lastSend_ = 0;
    int32_t mine_ = kNone, peer_ = kNone;
    uint32_t reads_ = 0, sends_ = 0;
};

}  // namespace StartSync
