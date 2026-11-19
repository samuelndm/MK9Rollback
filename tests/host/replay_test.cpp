// src\net\ReplayFile.h: replays of online matches without the game. The recorder's memory under rollbacks (a replayed
// frame overwrites what the live frame or an earlier replay wrote, so the recording ends with the inputs the session
// settled on), its limits, the file's bytes written and read back, every cut-off and every damaged copy refused without a
// read outside the bytes, the comparison of a playback with its recording (checksums, exact and rounded state samples),
// and, given a path, a file made by tools\replay_from_log.ps1 from a small made-up log (the PowerShell writer and the
// C++ reader must agree on every byte).
//   replay_test.exe                 the checks
//   replay_test.exe <file.mk9r>     also the made-up log's file (run.ps1 makes it first)
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "ReplayFile.h"

using namespace ReplayFile;

namespace {
int gChecks = 0, gFailed = 0;

void Check(bool ok, const char* what) {
    ++gChecks;
    if (ok) return;
    ++gFailed;
    if (gFailed < 60) printf("FAIL: %s\n", what);
}

uint32_t gRng = 12345;
uint32_t Rand() {
    gRng = gRng * 1664525u + 1013904223u;
    return gRng >> 8;
}

// ~1 MB each: static, as in the DLL.
Replay gA, gB, gC;

bool VecWrite(void* ctx, const void* p, size_t n) {
    auto* v = static_cast<std::vector<uint8_t>*>(ctx);
    v->insert(v->end(), static_cast<const uint8_t*>(p), static_cast<const uint8_t*>(p) + n);
    return true;
}
std::vector<uint8_t> Bytes(const Replay& r, bool* ok = nullptr) {
    std::vector<uint8_t> v;
    Sink s;
    s.ctx = &v;
    s.write = &VecWrite;
    const bool w = Write(r, s);
    if (ok) *ok = w;
    return v;
}
void FixFnv(std::vector<uint8_t>& v) {
    if (v.size() < 4) return;
    const uint32_t h = Fnv(kFnvStart, v.data(), v.size() - 4);
    for (int i = 0; i < 4; ++i) v[v.size() - 4 + i] = (uint8_t)(h >> (8 * i));
}

State StateOf(uint32_t seed) {
    State s{};
    s.life[0] = (seed % 1000) / 1000.0f;
    s.life[1] = ((seed / 7) % 1000) / 1000.0f;
    for (int p = 0; p < 2; ++p)
        for (int c = 0; c < 3; ++c) s.pos[p][c] = (float)((int)((seed >> (p * 3 + c)) % 2000) - 1000) * 0.371f;
    s.meter[0] = ((seed / 3) % 300) / 100.0f;
    s.meter[1] = ((seed / 5) % 300) / 100.0f;
    s.rng = seed * 2654435761u;
    return s;
}

void FillSetup(Replay& r) {
    CopyText(r.setup.p1, kText, "CHAR_JOHNNYCAGE_B");
    CopyText(r.setup.p2, kText, "CHAR_NIGHTWOLF");
    CopyText(r.setup.arena, kText, "BGND_Desert");
    r.setup.arenaIndex = 4;
    r.setup.colours[0] = 1;
    r.setup.colours[1] = 0;
    r.setup.kodes[0] = 0x900;
    r.setup.kodes[1] = 0x123;
    r.setup.localPlayer = 1;
    r.setup.delay = 1;
    r.setup.mode = 1;
    r.setup.sub = 8;
    r.setup.netplayGf = 100000;
    r.setup.startGf = 100001;
    r.setup.env[0] = 0x5AB648CB;
    r.setup.env[1] = 0x53FAF5F9;
    r.setup.env[2] = 0x55C958AB;
    r.setup.release[0] = 20261116;
    r.setup.release[1] = 2336;
    CopyText(r.setup.time, kTimeText, "2026-11-16 21:34:44");
    r.flags = kFromStart | kOnline | kColoursKnown | kKodesKnown;
}

// A session of `frames` frames as RollbackManager records it: the true inputs of both players; the live frame of the
// remote player is a guess (the last input seen) until a rollback replays the window with the true inputs. Rollbacks of
// 1-8 frames at random, some windows replayed twice (a second load in one poll), and the checksum of each frame recorded
// live and again by every replay that passes it (a guessed frame's checksum is wrong until then).
void Session(Replay& r, uint32_t frames, std::vector<uint32_t> truth[2], std::vector<uint32_t>& trueCs) {
    Clear(r);
    FillSetup(r);
    truth[0].assign(frames, 0);
    truth[1].assign(frames, 0);
    trueCs.assign(frames + 1, 0);
    uint32_t a = 0, b = 0;
    for (uint32_t i = 0; i < frames; ++i) {
        if (Rand() % 6 == 0) a = Rand() & 0xFF;
        if (Rand() % 5 == 0) b = Rand() & 0x3FF;
        truth[0][i] = a;
        truth[1][i] = b;
    }
    // checksum of frame n = a hash of the inputs before it (a stand-in for the game's state)
    auto csOf = [&](uint32_t n, const uint32_t* in1, const uint32_t* in2) {
        uint32_t h = 2166136261u;
        for (uint32_t i = 0; i < n; ++i) h = (h ^ in1[i] ^ (in2[i] << 11)) * 16777619u;
        return h;
    };
    for (uint32_t n = 0; n <= frames; ++n) trueCs[n] = csOf(n, truth[0].data(), truth[1].data());
    std::vector<uint32_t> seen[2] = {std::vector<uint32_t>(frames, 0), std::vector<uint32_t>(frames, 0)};
    uint32_t confirmed = 0;  // remote inputs known for frames < confirmed
    for (uint32_t f = 0; f < frames; ++f) {
        // the remote input of a few frames back arrives; the live frame uses the last known one as its guess
        const uint32_t lag = 1 + Rand() % 8;
        const uint32_t newConfirmed = f + 1 > lag ? f + 1 - lag : 0;
        if (newConfirmed > confirmed) {
            bool wrong = false;
            uint32_t from = confirmed;
            for (uint32_t i = confirmed; i < newConfirmed; ++i) {
                if (seen[1][i] != truth[1][i] && !wrong) wrong = true, from = i;
                seen[1][i] = truth[1][i];
            }
            confirmed = newConfirmed;
            if (wrong) {
                // the rollback: frames from..f-1 replayed with corrected (or still guessed) inputs
                const int passes = Rand() % 4 == 0 ? 2 : 1;
                for (int pass = 0; pass < passes; ++pass)
                    for (uint32_t g = from; g < f; ++g) {
                        const uint32_t guess = g < confirmed ? truth[1][g] : (confirmed ? truth[1][confirmed - 1] : 0);
                        seen[1][g] = guess;
                        PutInputs(r, g, truth[0][g], guess);
                        PutChecksum(r, g + 1, csOf(g + 1, truth[0].data(), seen[1].data()), nullptr);
                    }
            }
        }
        const uint32_t guess = f < confirmed ? truth[1][f] : (confirmed ? truth[1][confirmed - 1] : 0);
        seen[1][f] = guess;
        seen[0][f] = truth[0][f];
        // the save of frame f (state at its start), then its inputs at the engine tick
        if (f == 0) PutChecksum(r, 0, csOf(0, truth[0].data(), seen[1].data()), nullptr);
        PutInputs(r, f, truth[0][f], guess);
        PutChecksum(r, f + 1, csOf(f + 1, truth[0].data(), seen[1].data()), nullptr);
    }
    // the session's last frames are confirmed before it ends (GGPO's final polls): one more rollback over the open window
    for (uint32_t g = confirmed; g < frames; ++g) {
        seen[1][g] = truth[1][g];
        PutInputs(r, g, truth[0][g], truth[1][g]);
        PutChecksum(r, g + 1, csOf(g + 1, truth[0].data(), seen[1].data()), nullptr);
    }
    Finish(r, frames, "match over");
}

void TestRecorder() {
    std::vector<uint32_t> truth[2], trueCs;
    for (int round = 0; round < 40; ++round) {
        const uint32_t frames = 200 + Rand() % 3000;
        Session(gA, frames, truth, trueCs);
        bool inputs = gA.frames == frames, cs = true;
        for (uint32_t i = 0; i < frames; ++i)
            inputs = inputs && (gA.have[i] & kHaveInputs) && gA.mask[i][0] == truth[0][i] && gA.mask[i][1] == truth[1][i];
        for (uint32_t i = 0; i <= frames; ++i) cs = cs && (gA.have[i] & kHaveChecksum) && gA.cs[i] == trueCs[i];
        Check(inputs, "recorder: after the session's rollbacks every frame holds the true inputs of both players");
        Check(cs, "recorder: every frame's checksum is the one of the true inputs (a replay overwrote the guessed frame's)");
        Check(MissingInputs(gA) == 0 && Checksums(gA) == frames + 1, "recorder: no frame missing");
        Check(!strcmp(gA.stopWhy, "match over") && !(gA.flags & kTruncated), "recorder: the stop's reason, not cut");
    }
    // a frame without inputs (the session's frame never synchronized here) is counted and written as 0
    Clear(gA);
    PutInputs(gA, 0, 1, 2);
    PutInputs(gA, 2, 5, 6);
    Check(gA.frames == 3 && MissingInputs(gA) == 1, "recorder: a gap is counted");
    // the limits
    Clear(gA);
    Check(PutInputs(gA, kMaxFrames - 1, 7, 8) && gA.frames == kMaxFrames, "recorder: the last frame that fits");
    Check(!PutInputs(gA, kMaxFrames, 7, 8) && (gA.flags & kTruncated) && gA.frames == kMaxFrames, "recorder: a frame past the limit is refused and marks the cut");
    Check(PutChecksum(gA, kMaxFrames, 9, nullptr) && !PutChecksum(gA, kMaxFrames + 1, 9, nullptr), "recorder: checksums up to the state after the last frame");
    Clear(gA);
    Finish(gA, kMaxFrames + 5, "x");
    Check(gA.flags & kTruncated, "recorder: a session longer than the limit is marked cut");
    // state samples: every 60th frame, overwritten by a replay, in order
    Clear(gA);
    for (uint32_t f = 0; f <= 600; ++f) {
        const State s = StateOf(f);
        PutChecksum(gA, f, f * 3, &s);
    }
    const State late = StateOf(999);
    PutChecksum(gA, 120, 1, &late);
    bool states = gA.states == 11;
    for (uint32_t i = 0; i < gA.states; ++i) states = states && gA.stateKind[i] == kStateExact && gA.stateFrame[i] == i * kStateEvery;
    Check(states && !memcmp(&gA.state[2], &late, sizeof late), "recorder: a state every 60 frames, a replay's wins");
    // Clear forgets the last recording
    Clear(gA);
    Check(gA.frames == 0 && gA.states == 0 && MissingInputs(gA) == 0 && Checksums(gA) == 0 && gA.setup.arenaIndex == -1, "recorder: Clear");
}

bool SameReplay(const Replay& a, const Replay& b) {
    if (a.flags != b.flags || a.frames != b.frames || memcmp(&a.setup, &b.setup, sizeof a.setup) || strcmp(a.stopWhy, b.stopWhy)) return false;
    for (uint32_t i = 0; i < a.frames; ++i) {
        const bool ka = a.have[i] & kHaveInputs, kb = b.have[i] & kHaveInputs;
        if (ka != kb || (ka && (a.mask[i][0] != b.mask[i][0] || a.mask[i][1] != b.mask[i][1]))) return false;
    }
    for (uint32_t i = 0; i <= a.frames; ++i) {
        if ((a.have[i] & 3) != (b.have[i] & 3)) return false;
        if ((a.have[i] & kHaveChecksum) && a.cs[i] != b.cs[i]) return false;
    }
    // b (read back) lists the samples a has, in order
    uint32_t j = 0;
    for (uint32_t i = 0; i < a.states; ++i) {
        if (!a.stateKind[i]) continue;
        if (j >= b.states || b.stateFrame[j] != a.stateFrame[i] || b.stateKind[j] != a.stateKind[i] || memcmp(&b.state[j], &a.state[i], sizeof(State)))
            return false;
        ++j;
    }
    return j == b.states;
}

void TestFile() {
    // a recorded session: written, read back, the same
    std::vector<uint32_t> truth[2], trueCs;
    Session(gA, 2500, truth, trueCs);
    for (uint32_t f = 0; f <= 2500; f += 60) {
        const State s = StateOf(f + 17);
        PutChecksum(gA, f, gA.cs[f], &s);
    }
    bool wrote = false;
    std::vector<uint8_t> bytes = Bytes(gA, &wrote);
    Check(wrote && bytes.size() == FileBytes(gA.frames, 42), "file: written, its size as FileBytes says");
    Check(Read(bytes.data(), bytes.size(), &gB) == Parsed::kOk && SameReplay(gA, gB), "file: read back, the same recording");
    // a recording with gaps and a sample left out (kind 0) writes only what it has
    Clear(gC);
    FillSetup(gC);
    gC.flags |= kRebuilt;
    PutInputs(gC, 0, 1, 2);
    PutInputs(gC, 5, 3, 4);
    PutInputs(gC, 150, 9, 10);  // 151 frames: the samples at 0 and 120 lie inside
    PutChecksum(gC, 0, 77, nullptr);
    gC.stateFrame[0] = 0, gC.stateKind[0] = kStateApprox, gC.state[0] = StateOf(3);
    gC.stateFrame[1] = 60, gC.stateKind[1] = 0;
    gC.stateFrame[2] = 120, gC.stateKind[2] = kStateApprox, gC.state[2] = StateOf(4);
    gC.states = 3;
    Finish(gC, 151, "the peer disconnected");
    std::vector<uint8_t> small = Bytes(gC);
    Check(Read(small.data(), small.size(), &gB) == Parsed::kOk && SameReplay(gC, gB) && gB.states == 2 && MissingInputs(gB) == 148,
          "file: gaps and missing samples survive the round trip");
    // texts that fill their field are cut, never unterminated
    char longName[200];
    memset(longName, 'A', sizeof longName - 1);
    longName[sizeof longName - 1] = 0;
    CopyText(gC.setup.p1, kText, longName);
    CopyText(gC.stopWhy, kText, longName);
    small = Bytes(gC);
    Check(Read(small.data(), small.size(), &gB) == Parsed::kOk && strlen(gB.setup.p1) == kText - 1, "file: a long name is cut to its field");
    // every shorter copy refused; every longer one too
    int cutOk = 0;
    for (size_t n = 0; n < small.size(); ++n)
        if (Read(small.data(), n, &gB) == Parsed::kOk) ++cutOk;
    Check(cutOk == 0, "file: every cut-off copy is refused");
    std::vector<uint8_t> longer = small;
    longer.push_back(0);
    Check(Read(longer.data(), longer.size(), &gB) == Parsed::kSize, "file: a byte added is refused");
    Check(Read(nullptr, 0, &gB) == Parsed::kShort, "file: nothing");
    // every bit of every byte flipped: refused (the check value, or the header's own checks)
    int flipOk = 0, flips = 0;
    for (size_t i = 0; i < small.size(); ++i)
        for (int bit = 0; bit < 8; ++bit) {
            std::vector<uint8_t> d = small;
            d[i] ^= (uint8_t)(1 << bit);
            ++flips;
            if (Read(d.data(), d.size(), &gB) == Parsed::kOk) ++flipOk;
        }
    Check(flipOk == 0, "file: every single flipped bit is refused");
    printf("note: %d damaged copies of a %u-byte replay refused\n", flips, (unsigned)small.size());
    // damaged with a correct check value (a writer's mistake, not the disk's): the structure's own checks
    struct Case {
        size_t at;
        uint8_t v;
        Parsed want;
    };
    const size_t texts = 5 * 4, have = kHeaderBytes + gC.frames * 8 + (gC.frames + 1) * 4, st = have + gC.frames + 1;
    const Case cases[] = {{0, 'X', Parsed::kMagic},           {4, 2, Parsed::kVersion},
                          {15, 0xFF, Parsed::kTooLong},       {12, 7, Parsed::kSize},
                          {texts + kText - 1, 'Z', Parsed::kText},  // p1 without its end
                          {kHeaderBytes - 1, 'Z', Parsed::kText},   // stopWhy without its end
                          {have, 4, Parsed::kHave},           {have + gC.frames, kHaveInputs, Parsed::kHave},
                          {st + 4, 3, Parsed::kStates},       {kHeaderBytes, 0x55, Parsed::kOk}};
    for (const Case& c : cases) {
        std::vector<uint8_t> d = small;
        if (c.at >= d.size()) {
            Check(false, "file: a damage case outside the file");
            continue;
        }
        d[c.at] = c.v;
        FixFnv(d);
        const Parsed got = Read(d.data(), d.size(), &gB);
        char what[160];
        snprintf(what, sizeof what, "file: byte %u set to %u reads as '%s' (wanted '%s')", (unsigned)c.at, c.v, ParsedText(got), ParsedText(c.want));
        Check(got == c.want, what);
    }
    // the second sample before the first: refused
    {
        std::vector<uint8_t> d = small;
        d[st + kStateBytes] = 0;  // frame 120 -> 0 (low byte 120 -> 0): not after frame 0
        FixFnv(d);
        Check(Read(d.data(), d.size(), &gB) == Parsed::kStates, "file: samples out of order are refused");
    }
    // random structural damage with a correct check value: a refusal or a replay whose counts stay in bounds
    int fuzzOk = 0, fuzzBad = 0;
    for (int i = 0; i < 20000; ++i) {
        std::vector<uint8_t> d = small;
        const int edits = 1 + Rand() % 6;
        for (int e = 0; e < edits; ++e) d[Rand() % d.size()] = (uint8_t)Rand();
        if (Rand() % 4 == 0) d.resize(Rand() % (d.size() + 40), 0x5A);
        FixFnv(d);
        if (Read(d.data(), d.size(), &gB) == Parsed::kOk) {
            ++fuzzOk;
            if (gB.frames > kMaxFrames || gB.states > kMaxStates || !memchr(gB.setup.p1, 0, kText) || !memchr(gB.stopWhy, 0, kText)) ++fuzzBad;
        }
    }
    Check(fuzzBad == 0, "file: 20000 random damages with a correct check value: whatever is read stays in bounds");
    printf("note: 20000 random damages: %d still read as a replay (their numbers changed), the rest refused\n", fuzzOk);
    // the largest replay
    Clear(gC);
    for (uint32_t f = 0; f < kMaxFrames; ++f) PutInputs(gC, f, f, ~f);
    for (uint32_t f = 0; f <= kMaxFrames; ++f) {
        const State s = StateOf(f);
        PutChecksum(gC, f, f * 7, &s);
    }
    std::vector<uint8_t> big = Bytes(gC, &wrote);
    Check(wrote && big.size() == FileBytes(kMaxFrames, kMaxFrames / kStateEvery + 1), "file: the largest replay's size");
    Check(Read(big.data(), big.size(), &gB) == Parsed::kOk && SameReplay(gC, gB), "file: the largest replay read back");
    printf("note: the largest replay (%u frames) is %u KB; a 140 s match about %u KB\n", kMaxFrames, (unsigned)(big.size() / 1024),
           (unsigned)(FileBytes(8400, 140) / 1024));
    // a sink that fails: Write says so
    Sink bad;
    bad.write = [](void*, const void*, size_t) { return false; };
    Check(!Write(gA, bad), "file: a failed write is reported");
}

void TestCompare() {
    std::vector<uint32_t> truth[2], trueCs;
    Session(gA, 1200, truth, trueCs);
    for (uint32_t f = 0; f <= 1200; f += 60) {
        const State s = StateOf(f);
        PutChecksum(gA, f, gA.cs[f], &s);
    }
    // the same fight: every frame the same
    Compare c;
    bool same = true;
    for (uint32_t n = 0; n <= gA.frames; ++n) {
        const Compare::Verdict v = c.Frame(gA, n, trueCs[n], StateOf(n));
        same = same && v == Compare::kSame;
    }
    Check(same && c.compared == 1201 && c.same == 1201 && c.statesCompared == 21 && c.statesSame == 21 && c.firstOff == 0xFFFFFFFF,
          "compare: a playback of the same fight is the same on every frame");
    // out of step from frame 700: the first one says so, later ones are "off"
    c.Reset();
    int first = 0, offs = 0;
    for (uint32_t n = 0; n <= gA.frames; ++n) {
        const Compare::Verdict v = c.Frame(gA, n, n >= 700 ? trueCs[n] ^ 1 : trueCs[n], StateOf(n));
        first += v == Compare::kFirstOff;
        offs += v == Compare::kOff;
    }
    Check(first == 1 && c.firstOff == 700 && offs == 500 && c.differ == 501 && c.same == 700, "compare: out of step from frame 700");
    // a state sample alone differs (checksum the same: a field the checksum does not cover would not, but a sample must)
    c.Reset();
    State odd = StateOf(300);
    odd.rng ^= 4;
    Compare::Verdict at300 = Compare::kNothing;
    for (uint32_t n = 0; n <= 400; ++n) {
        const Compare::Verdict v = c.Frame(gA, n, trueCs[n], n == 300 ? odd : StateOf(n));
        if (n == 300) at300 = v;
    }
    Check(at300 == Compare::kFirstOff && c.firstOff == 300 && c.statesSame == c.statesCompared - 1, "compare: a differing state sample is out of step");
    // back in step after differing frames
    c.Reset();
    Compare::Verdict back = Compare::kNothing;
    for (uint32_t n = 0; n <= 100; ++n) {
        const Compare::Verdict v = c.Frame(gA, n, n >= 40 && n < 50 ? 0 : trueCs[n], StateOf(n));
        if (n == 50) back = v;
    }
    Check(back == Compare::kBack && c.differ == 10, "compare: back in step is said once");
    // a playback that skips frames (no call for some n) still finds the next sample
    c.Reset();
    for (uint32_t n = 0; n <= 1200; n += 7) c.Frame(gA, n, trueCs[n], StateOf(n));
    Check(c.statesCompared == 3 && c.statesSame == 3, "compare: samples at frames a playback reaches (0, 420, 840 of every 7th)");
    // a replay rebuilt from logs: no checksums, rounded samples
    Clear(gB);
    gB.frames = 1000;
    const uint32_t at[4] = {0, 1, 300, 600};
    for (int i = 0; i < 4; ++i) {
        State s = StateOf(at[i]);
        // as a log line prints them: lives and meters to 4 decimals, positions to 3
        for (int p = 0; p < 2; ++p) {
            s.life[p] = std::round(s.life[p] * 10000.0f) / 10000.0f;
            s.meter[p] = std::round(s.meter[p] * 10000.0f) / 10000.0f;
            for (int k = 0; k < 3; ++k) s.pos[p][k] = std::round(s.pos[p][k] * 1000.0f) / 1000.0f;
        }
        gB.stateFrame[i] = at[i];
        gB.stateKind[i] = kStateApprox;
        gB.state[i] = s;
    }
    gB.states = 4;
    c.Reset();
    for (uint32_t n = 0; n <= 1000; ++n) c.Frame(gB, n, 0, StateOf(n));
    Check(c.compared == 0 && c.statesCompared == 4 && c.statesSame == 4, "compare: rounded samples match the exact state within their rounding");
    c.Reset();
    for (uint32_t n = 0; n <= 1000; ++n) {
        State s = StateOf(n);
        if (n == 300) s.pos[1][1] += 0.002f;  // beyond a position's rounding
        if (n == 600) s.life[0] += 0.0002f;   // beyond a life's
        c.Frame(gB, n, 0, s);
    }
    Check(c.statesSame == 2 && c.firstOff == 300 && c.lastOff == 600, "compare: rounded samples catch a move past their rounding");
    // SampleAt
    uint8_t kind = 0;
    Check(Compare::SampleAt(gB, 300, &kind) == &gB.state[2] && kind == kStateApprox && !Compare::SampleAt(gB, 301, nullptr), "compare: SampleAt");
    char k[6];
    KodeText(0x900, k);
    Check(!strcmp(k, "9-0-0"), "kodes as text");
}

// A file tools\replay_from_log.ps1 made from tests\host\replay_log_*.txt (run.ps1 writes them): its numbers are known.
void TestLogFile(const char* path) {
    FILE* f = fopen(path, "rb");
    Check(f != nullptr, "log file: tools\\replay_from_log.ps1 wrote a file");
    if (!f) return;
    std::vector<uint8_t> d;
    uint8_t buf[65536];
    size_t n;
    while ((n = fread(buf, 1, sizeof buf, f)) > 0) d.insert(d.end(), buf, buf + n);
    fclose(f);
    const Parsed p = Read(d.data(), d.size(), &gA);
    char what[200];
    snprintf(what, sizeof what, "log file: read as a replay (%s)", ParsedText(p));
    Check(p == Parsed::kOk, what);
    if (p != Parsed::kOk) return;
    const Setup& u = gA.setup;
    Check(!strcmp(u.p1, "CHAR_JOHNNYCAGE_B") && !strcmp(u.p2, "CHAR_NIGHTWOLF") && !strcmp(u.arena, "BGND_Desert") && u.arenaIndex == 4,
          "log file: fighters and arena from the REFIGHT line");
    Check((gA.flags & kKodesKnown) && u.kodes[0] == 0x900 && u.kodes[1] == 0, "log file: kodes by side (this game was player 1: 9-0-0, the other 0-0-0)");
    Check(!(gA.flags & kColoursKnown) && (gA.flags & kRebuilt) && (gA.flags & kFromStart) && (gA.flags & kOnline), "log file: flags");
    Check(u.testMeter == -1.0f && u.testLife == -1.0f, "log file: no test settings");
    Check(u.localPlayer == 1 && u.delay == 1 && u.netplayGf == 100000 && u.startGf == 100001 && u.mode == 1, "log file: side, delay, frame numbers, mode");
    Check(gA.frames == 12 && !strcmp(gA.stopWhy, "match over"), "log file: 12 frames (the stop line's count), stopped at the match's end");
    // frames 0..5 logged (frame 3 replayed with another P2 input: the replay's wins), 6..9 a gap (the next logged frame's
    // prev for all of them), 10 logged, 11 after the last line (0, unknown)
    const uint32_t want[12][2] = {{0, 0}, {0x8000, 8}, {0x8000, 8}, {0x20, 0x4}, {0x20, 0x24}, {0, 0x24},
                                  {0x44, 0x2}, {0x44, 0x2}, {0x44, 0x2}, {0x44, 0x2}, {0x44, 0x6}, {0, 0}};
    bool masks = true;
    for (uint32_t i = 0; i < 12; ++i) masks = masks && gA.mask[i][0] == want[i][0] && gA.mask[i][1] == want[i][1];
    Check(masks, "log file: the masks (a replayed line wins, a gap gets the held input after it, the end is neutral)");
    Check(MissingInputs(gA) == 5 && (gA.have[0] & kHaveInputs) && !(gA.have[7] & kHaveInputs) && !(gA.have[11] & kHaveInputs),
          "log file: the guessed frames are marked as not recorded");
    Check(Checksums(gA) == 0 && gA.states == 2 && gA.stateFrame[0] == 0 && gA.stateFrame[1] == 3 && gA.stateKind[0] == kStateApprox,
          "log file: no checksums, two rounded samples (frames 0 and 3)");
    const State& s = gA.state[1];
    Check(std::fabs(s.life[0] - 0.8725f) < 1e-6f && std::fabs(s.life[1] - 0.97f) < 1e-6f && std::fabs(s.pos[0][1] + 398.813f) < 1e-3f &&
              std::fabs(s.pos[1][1] - 17.267f) < 1e-3f && std::fabs(s.meter[0] - 0.4226f) < 1e-6f && s.rng == 0xF5A4E99Fu,
          "log file: the sample's numbers");
}
}  // namespace

int main(int argc, char** argv) {
    TestRecorder();
    TestFile();
    TestCompare();
    if (argc > 1) TestLogFile(argv[1]);
    printf("replay_test: %d checks, %d failed\n", gChecks, gFailed);
    if (!gFailed) printf("ALL CHECKS PASSED\n");
    return gFailed ? 1 : 0;
}
