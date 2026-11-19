#pragma once
// Replays of online matches (the user's wish of 2026-11-16: "check as well if we can add replays of the online matchs").
//
// A replay is the inputs, not a film: both fighters' 32-bit input masks for every frame of an online match's rollback
// session, and the setup the session started from (fighters and colours, arena, Kombat Kodes, the session's fixed start).
// Played back, an offline fight is set up the same way, started from the same fixed state an online session starts from
// (rollback\RollbackManager.cpp: GameFrame renumbered to 100000, RNG A / RNG B and the script stream seeded, the fighters'
// input history cleared) and driven by the recorded masks, one per frame, as the session drove it. The gameplay checksum
// of every frame (the one the two games compare during a session: lives, positions, meters, RNG A) is recorded too, so
// the playback says frame by frame whether it is still the same fight, and where it went out of step.
//
// What is here, without the game (tests\host\replay_test.cpp): the recording's memory (Replay: fixed arrays, nothing
// allocated while a fight runs; a rollback's replay of a frame overwrites what the live frame or an earlier replay wrote,
// so the recording ends with the corrected inputs), the file's bytes (Write / Read), and the frame-by-frame comparison
// of a playback with its recording (Compare). net\ReplayKeeper.cpp reads the game, writes and reads the files and logs;
// rollback\RollbackManager.cpp records the session and drives the playback.
//
// The file (<the DLL's folder>\replays\<date>_<time>.mk9r), little-endian:
//   u32 magic 'MK9R', u32 version, u32 flags, u32 frames, u32 states,
//   the setup (Setup, field by field, texts as fixed zero-padded fields), char stopWhy[64],
//   frames x {u32 P1 mask, u32 P2 mask}, (frames + 1) x u32 checksum, (frames + 1) x u8 have,
//   states x {u32 frame, u8 kind, 11 x u32: life P1 P2, position P1 xyz, position P2 xyz, meter P1 P2, RNG A},
//   u32 FNV-1a of every byte before it.
// Frame n's inputs are applied at the engine tick of session frame n; checksum n is the state at the start of frame n
// (after frame n-1 ran), the value GGPO's save of frame n carries; checksum `frames` is the state after the last frame.
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace ReplayFile {

constexpr uint32_t kMagic = 0x52394B4Du;  // the bytes 'M' 'K' '9' 'R'
constexpr uint32_t kVersion = 1;
constexpr uint32_t kMaxFrames = 65536;    // 18 minutes at 60 frames a second; a longer session is cut (kTruncated)
constexpr uint32_t kStateEvery = 60;      // a recording keeps the whole state of every 60th frame (for the out-of-step line)
constexpr uint32_t kMaxStates = kMaxFrames / kStateEvery + 8;
constexpr size_t kText = 64;
constexpr size_t kTimeText = 24;

// Replay::have, per frame.
enum : uint8_t { kHaveInputs = 1, kHaveChecksum = 2 };
// Replay::flags.
enum : uint32_t {
    kFromStart = 1,      // the session began at a fight's first frame with the fixed start (a netplay start): it can be played back
    kOnline = 2,         // an online match (MK9's online mode engaged), not a test session
    kTruncated = 4,      // the session ran longer than kMaxFrames, or a frame was out of range: the recording is cut
    kRebuilt = 8,        // made from the logs (tools\replay_from_log.ps1): inputs of frames the frames log lacks are guessed
    kColoursKnown = 16,  // Setup::colours were read
    kKodesKnown = 32,    // Setup::kodes were read
};
// The kind of a state sample.
enum : uint8_t { kStateExact = 1, kStateApprox = 2 /* from a log line's rounded numbers */ };

struct State {
    float life[2];
    float pos[2][3];
    float meter[2];
    uint32_t rng;
};

struct Setup {
    char p1[kText], p2[kText];  // the fighters as the game names them (fighter and costume, "CHAR_JOHNNYCAGE_B")
    char arena[kText];          // "BGND_Desert"
    int32_t arenaIndex;         // its stage table entry (-1: not known)
    uint32_t colours[2];        // each fighter's "other colours" flag (the select screen's START)
    uint16_t kodes[2];          // the Kombat Kodes of player 1's and player 2's side (MK9's three digits, RefightVote::Kodes)
    uint32_t localPlayer;       // 1 or 2: the side the recording game played (0 not known)
    uint32_t delay;             // the session's input delay
    uint32_t mode, sub;         // GameInfo +0x8 and +0x4 at the start
    uint32_t netplayGf;         // GameFrame of the start's first fight frame (100000)
    uint32_t startGf;           // GameFrame of frame 0
    uint32_t env[3];            // the environment prints (EnvFingerprint: content, DLL, switches)
    uint32_t release[2];        // version.txt's stamp (date, time), 0 0 in a developer tree
    float testMeter, testLife;  // 'netplaymeter' / 'netplaylife' of a test session (both meters / lives set at frame 7), < 0: off
    char time[kTimeText];       // local time the session began, "2026-11-16 21:34:44"
};

struct Replay {
    uint32_t flags = 0;
    Setup setup{};
    char stopWhy[kText] = "";
    uint32_t frames = 0;  // inputs of frames 0 .. frames-1
    uint32_t mask[kMaxFrames][2];
    uint32_t cs[kMaxFrames + 1];
    uint8_t have[kMaxFrames + 1];
    // State samples, ascending by frame. A recording keeps sample i for frame i * kStateEvery (kind 0: none yet); a file
    // read back lists only the samples it has.
    uint32_t states = 0;
    uint32_t stateFrame[kMaxStates];
    uint8_t stateKind[kMaxStates];
    State state[kMaxStates];
};

inline void CopyText(char* dst, size_t cap, const char* src) {
    if (!cap) return;
    size_t n = 0;
    if (src)
        for (; n + 1 < cap && src[n]; ++n) dst[n] = src[n];
    memset(dst + n, 0, cap - n);
}

// A new recording (or the playback buffer before a read). The masks and checksums are not cleared: `have` says which are set.
inline void Clear(Replay& r) {
    r.flags = 0;
    r.setup = Setup{};
    r.setup.arenaIndex = -1;
    r.setup.testMeter = r.setup.testLife = -1.0f;
    memset(r.stopWhy, 0, sizeof r.stopWhy);
    r.frames = 0;
    memset(r.have, 0, sizeof r.have);
    r.states = 0;
    memset(r.stateKind, 0, sizeof r.stateKind);
}

// The recorder (game thread, inside the session's frames: a few stores, no allocation). Frame `frame`'s two masks, live or
// from a rollback's replay of it (a later call for the same frame wins). False: past kMaxFrames (the recording is cut there).
inline bool PutInputs(Replay& r, uint32_t frame, uint32_t m1, uint32_t m2) {
    if (frame >= kMaxFrames) {
        r.flags |= kTruncated;
        return false;
    }
    r.mask[frame][0] = m1;
    r.mask[frame][1] = m2;
    r.have[frame] |= kHaveInputs;
    if (frame + 1 > r.frames) r.frames = frame + 1;
    return true;
}
// Frame `frame`'s gameplay checksum (the state at its start), with the whole state where one is given; a state is kept
// for every kStateEvery-th frame.
inline bool PutChecksum(Replay& r, uint32_t frame, uint32_t cs, const State* s) {
    if (frame > kMaxFrames) {
        r.flags |= kTruncated;
        return false;
    }
    r.cs[frame] = cs;
    r.have[frame] |= kHaveChecksum;
    if (s && frame % kStateEvery == 0) {
        const uint32_t i = frame / kStateEvery;
        if (i < kMaxStates) {
            r.state[i] = *s;
            r.stateKind[i] = kStateExact;
            r.stateFrame[i] = frame;
            if (i + 1 > r.states) r.states = i + 1;
        }
    }
    return true;
}
// The end of the session: `frames` is the number of frames the session ran (its last ones may hold predicted inputs only
// if it stopped abruptly; the count is what the session itself says).
inline void Finish(Replay& r, uint32_t sessionFrames, const char* why) {
    if (sessionFrames < r.frames) r.frames = sessionFrames;
    if (sessionFrames > kMaxFrames) r.flags |= kTruncated;
    CopyText(r.stopWhy, sizeof r.stopWhy, why);
}
// Frames among 0 .. frames-1 without recorded inputs, and checksums missing among 0 .. frames.
inline uint32_t MissingInputs(const Replay& r) {
    uint32_t n = 0;
    for (uint32_t i = 0; i < r.frames; ++i) n += !(r.have[i] & kHaveInputs);
    return n;
}
inline uint32_t Checksums(const Replay& r) {
    uint32_t n = 0;
    for (uint32_t i = 0; i <= r.frames && i <= kMaxFrames; ++i) n += (r.have[i] & kHaveChecksum) ? 1 : 0;
    return n;
}

// ---- the file's bytes ----

inline uint32_t Fnv(uint32_t h, const void* p, size_t n) {
    const uint8_t* b = static_cast<const uint8_t*>(p);
    for (size_t i = 0; i < n; ++i) h = (h ^ b[i]) * 16777619u;
    return h;
}
constexpr uint32_t kFnvStart = 2166136261u;

// Where the bytes go: write(ctx, bytes, n) returns false when they could not be written (the rest is not tried).
struct Sink {
    void* ctx = nullptr;
    bool (*write)(void* ctx, const void* p, size_t n) = nullptr;
    uint32_t fnv = kFnvStart;
    size_t bytes = 0;
    bool ok = true;
    void Put(const void* p, size_t n) {
        if (!ok || !n) return;
        ok = write(ctx, p, n);
        fnv = Fnv(fnv, p, n);
        bytes += n;
    }
    void U32(uint32_t v) {
        const uint8_t b[4] = {(uint8_t)v, (uint8_t)(v >> 8), (uint8_t)(v >> 16), (uint8_t)(v >> 24)};
        Put(b, 4);
    }
    void U16(uint16_t v) {
        const uint8_t b[2] = {(uint8_t)v, (uint8_t)(v >> 8)};
        Put(b, 2);
    }
    void U8(uint8_t v) { Put(&v, 1); }
    void F32(float f) {
        uint32_t u;
        memcpy(&u, &f, 4);
        U32(u);
    }
    void Text(const char* s, size_t cap) {
        char buf[kText > kTimeText ? kText : kTimeText] = {};
        CopyText(buf, cap, s);
        Put(buf, cap);
    }
};

constexpr size_t kSetupBytes = 3 * kText + 4 + 8 + 4 + 4 * 6 + 12 + 8 + 8 + kTimeText;
constexpr size_t kHeaderBytes = 5 * 4 + kSetupBytes + kText;
constexpr size_t kStateBytes = 4 + 1 + 11 * 4;
inline size_t FileBytes(uint32_t frames, uint32_t states) {
    return kHeaderBytes + (size_t)frames * 8 + ((size_t)frames + 1) * 5 + (size_t)states * kStateBytes + 4;
}

inline void PutState(Sink& s, const State& st) {
    s.F32(st.life[0]);
    s.F32(st.life[1]);
    for (int p = 0; p < 2; ++p)
        for (int c = 0; c < 3; ++c) s.F32(st.pos[p][c]);
    s.F32(st.meter[0]);
    s.F32(st.meter[1]);
    s.U32(st.rng);
}

// The whole file. False: the sink failed, or the recording holds nothing that can be written.
inline bool Write(const Replay& r, Sink& s) {
    if (r.frames > kMaxFrames || r.states > kMaxStates) return false;
    uint32_t states = 0;
    for (uint32_t i = 0; i < r.states; ++i) states += r.stateKind[i] ? 1 : 0;
    s.U32(kMagic);
    s.U32(kVersion);
    s.U32(r.flags);
    s.U32(r.frames);
    s.U32(states);
    const Setup& u = r.setup;
    s.Text(u.p1, kText);
    s.Text(u.p2, kText);
    s.Text(u.arena, kText);
    s.U32((uint32_t)u.arenaIndex);
    s.U32(u.colours[0]);
    s.U32(u.colours[1]);
    s.U16(u.kodes[0]);
    s.U16(u.kodes[1]);
    s.U32(u.localPlayer);
    s.U32(u.delay);
    s.U32(u.mode);
    s.U32(u.sub);
    s.U32(u.netplayGf);
    s.U32(u.startGf);
    for (uint32_t e : u.env) s.U32(e);
    s.U32(u.release[0]);
    s.U32(u.release[1]);
    s.F32(u.testMeter);
    s.F32(u.testLife);
    s.Text(u.time, kTimeText);
    s.Text(r.stopWhy, kText);
    for (uint32_t i = 0; i < r.frames; ++i) {
        const bool known = r.have[i] & kHaveInputs;
        s.U32(known ? r.mask[i][0] : 0);
        s.U32(known ? r.mask[i][1] : 0);
    }
    for (uint32_t i = 0; i <= r.frames; ++i) s.U32((r.have[i] & kHaveChecksum) ? r.cs[i] : 0);
    for (uint32_t i = 0; i <= r.frames; ++i) s.U8(r.have[i] & (kHaveInputs | kHaveChecksum));
    for (uint32_t i = 0; i < r.states; ++i) {
        if (!r.stateKind[i]) continue;
        s.U32(r.stateFrame[i]);
        s.U8(r.stateKind[i]);
        PutState(s, r.state[i]);
    }
    const uint32_t sum = s.fnv;
    s.U32(sum);
    return s.ok;
}

enum class Parsed { kOk, kShort, kMagic, kVersion, kTooLong, kSize, kText, kHave, kStates, kDamaged };
inline const char* ParsedText(Parsed p) {
    switch (p) {
    case Parsed::kOk: return "read";
    case Parsed::kShort: return "the file is shorter than a replay's header";
    case Parsed::kMagic: return "not a replay of this mod (no MK9R at its start)";
    case Parsed::kVersion: return "a replay of another version of this mod's format";
    case Parsed::kTooLong: return "more frames or state samples than a replay can hold";
    case Parsed::kSize: return "its size does not fit the frames it says it holds (cut off, or bytes added)";
    case Parsed::kText: return "a name in it is not a terminated text";
    case Parsed::kHave: return "a frame's marks are not ones this format writes";
    case Parsed::kStates: return "its state samples are not in order or name a frame past the end";
    case Parsed::kDamaged: return "its check value does not match its bytes (the file is damaged)";
    }
    return "?";
}

struct Reader {
    const uint8_t* p;
    size_t n, at = 0;
    uint32_t U32() {
        const uint8_t* b = p + at;
        at += 4;
        return b[0] | (b[1] << 8) | (b[2] << 16) | ((uint32_t)b[3] << 24);
    }
    uint16_t U16() {
        const uint8_t* b = p + at;
        at += 2;
        return (uint16_t)(b[0] | (b[1] << 8));
    }
    uint8_t U8() { return p[at++]; }
    float F32() {
        const uint32_t u = U32();
        float f;
        memcpy(&f, &u, 4);
        return f;
    }
    bool Text(char* out, size_t cap) {  // a fixed field of `cap` bytes that holds a terminated text
        memcpy(out, p + at, cap);
        at += cap;
        return memchr(out, 0, cap) != nullptr;
    }
};

// The file's bytes into `out` (its earlier contents are lost, also on a failure). Every length is checked before a byte
// is read: any input, damaged or not, reads nothing outside `data`.
inline Parsed Read(const uint8_t* data, size_t n, Replay* out) {
    Clear(*out);
    if (!data || n < kHeaderBytes + 4) return Parsed::kShort;
    Reader rd{data, n};
    if (rd.U32() != kMagic) return Parsed::kMagic;
    if (rd.U32() != kVersion) return Parsed::kVersion;
    const uint32_t flags = rd.U32(), frames = rd.U32(), states = rd.U32();
    if (frames > kMaxFrames || states > kMaxStates) return Parsed::kTooLong;
    if (n != FileBytes(frames, states)) return Parsed::kSize;
    if (Fnv(kFnvStart, data, n - 4) != (data[n - 4] | (data[n - 3] << 8) | (data[n - 2] << 16) | ((uint32_t)data[n - 1] << 24)))
        return Parsed::kDamaged;
    Replay& r = *out;
    r.flags = flags;
    Setup& u = r.setup;
    bool texts = rd.Text(u.p1, kText) && rd.Text(u.p2, kText) && rd.Text(u.arena, kText);
    u.arenaIndex = (int32_t)rd.U32();
    u.colours[0] = rd.U32();
    u.colours[1] = rd.U32();
    u.kodes[0] = rd.U16();
    u.kodes[1] = rd.U16();
    u.localPlayer = rd.U32();
    u.delay = rd.U32();
    u.mode = rd.U32();
    u.sub = rd.U32();
    u.netplayGf = rd.U32();
    u.startGf = rd.U32();
    for (uint32_t& e : u.env) e = rd.U32();
    u.release[0] = rd.U32();
    u.release[1] = rd.U32();
    u.testMeter = rd.F32();
    u.testLife = rd.F32();
    texts = rd.Text(u.time, kTimeText) && texts;
    texts = rd.Text(r.stopWhy, kText) && texts;
    if (!texts) {
        Clear(r);
        return Parsed::kText;
    }
    for (uint32_t i = 0; i < frames; ++i) {
        r.mask[i][0] = rd.U32();
        r.mask[i][1] = rd.U32();
    }
    for (uint32_t i = 0; i <= frames; ++i) r.cs[i] = rd.U32();
    for (uint32_t i = 0; i <= frames; ++i) {
        r.have[i] = rd.U8();
        if (r.have[i] & ~(kHaveInputs | kHaveChecksum) || (i == frames && (r.have[i] & kHaveInputs))) {
            Clear(r);
            return Parsed::kHave;
        }
    }
    for (uint32_t i = 0; i < states; ++i) {
        r.stateFrame[i] = rd.U32();
        r.stateKind[i] = rd.U8();
        State& s = r.state[i];
        s.life[0] = rd.F32();
        s.life[1] = rd.F32();
        for (int p = 0; p < 2; ++p)
            for (int c = 0; c < 3; ++c) s.pos[p][c] = rd.F32();
        s.meter[0] = rd.F32();
        s.meter[1] = rd.F32();
        s.rng = rd.U32();
        if (r.stateFrame[i] > frames || (r.stateKind[i] != kStateExact && r.stateKind[i] != kStateApprox) ||
            (i && r.stateFrame[i] <= r.stateFrame[i - 1])) {
            Clear(r);
            return Parsed::kStates;
        }
    }
    r.frames = frames;
    r.states = states;
    return Parsed::kOk;
}

// ---- a playback against its recording ----

// Two states the same? Exact samples: every bit. Samples from a log line ("NETPLAY STATE frame N: life 0.8725 ...",
// four decimals for lives and meters, three for positions): within that rounding, the RNG exactly.
inline bool SameState(const State& rec, uint8_t kind, const State& live) {
    if (kind == kStateExact) return !memcmp(&rec, &live, sizeof rec);
    auto within = [](float a, float b, float tol) { return (a > b ? a - b : b - a) <= tol; };
    for (int p = 0; p < 2; ++p) {
        if (!within(rec.life[p], live.life[p], 0.000075f) || !within(rec.meter[p], live.meter[p], 0.000075f)) return false;
        for (int c = 0; c < 3; ++c)
            if (!within(rec.pos[p][c], live.pos[p][c], 0.00075f)) return false;
    }
    return rec.rng == live.rng;
}

// The playback's count, frame by frame: Frame(n, checksum, state) at the start of each played frame n (the point where the
// recording took checksum n). What it returns says what to log: kSame / kNothing (no checksum or sample recorded for n) /
// kFirstOff (the first frame that differs) / kOff (a later one) / kBack (same again after differing frames).
struct Compare {
    enum Verdict { kNothing, kSame, kFirstOff, kOff, kBack };
    uint32_t compared = 0, same = 0, differ = 0;          // checksums
    uint32_t statesCompared = 0, statesSame = 0;          // state samples (the only check of a replay rebuilt from the logs)
    uint32_t firstOff = 0xFFFFFFFF;                       // the first frame whose checksum or sample differs
    uint32_t lastOff = 0xFFFFFFFF;
    uint32_t stateCursor = 0;
    bool off = false;                                     // the last compared frame differed
    void Reset() { *this = Compare{}; }
    // `rec` the recording; n the frame about to be played (n <= rec.frames).
    Verdict Frame(const Replay& rec, uint32_t n, uint32_t liveCs, const State& live) {
        bool any = false, bad = false;
        if (n <= rec.frames && n <= kMaxFrames && (rec.have[n] & kHaveChecksum)) {
            any = true;
            ++compared;
            if (rec.cs[n] == liveCs) ++same;
            else ++differ, bad = true;
        }
        while (stateCursor < rec.states && (!rec.stateKind[stateCursor] || rec.stateFrame[stateCursor] < n)) ++stateCursor;
        if (stateCursor < rec.states && rec.stateFrame[stateCursor] == n) {
            any = true;
            ++statesCompared;
            if (SameState(rec.state[stateCursor], rec.stateKind[stateCursor], live)) ++statesSame;
            else bad = true;
        }
        if (!any) return kNothing;
        if (bad) {
            lastOff = n;
            const bool first = firstOff == 0xFFFFFFFF;
            if (first) firstOff = n;
            off = true;
            return first ? kFirstOff : kOff;
        }
        const bool back = off;
        off = false;
        return back ? kBack : kSame;
    }
    // The recorded state sample at frame n (null: none).
    static const State* SampleAt(const Replay& rec, uint32_t n, uint8_t* kind) {
        for (uint32_t i = 0; i < rec.states; ++i)
            if (rec.stateKind[i] && rec.stateFrame[i] == n) {
                if (kind) *kind = rec.stateKind[i];
                return &rec.state[i];
            }
        return nullptr;
    }
};

inline void KodeText(uint16_t k, char out[6]) {  // MK9's three digits of a side ("9-0-0"), as RefightVote::KodeText
    snprintf(out, 6, "%u-%u-%u", (k >> 8) & 0xF, (k >> 4) & 0xF, k & 0xF);
}

}  // namespace ReplayFile
