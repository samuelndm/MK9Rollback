// Host-side test of src\net\HelloProto.{h,cpp}: the online mode's hello parsing, build comparison and ready decision.
// Build and run: see run.ps1 beside this file.
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "HelloProto.h"

using namespace HelloProto;

namespace {
int gFailed = 0, gChecks = 0;
void Check(bool ok, const char* what) {
    ++gChecks;
    if (!ok) ++gFailed;
    printf("%s  %s\n", ok ? "PASS" : "FAIL", what);
}
bool Starts(const char* s, const char* prefix) { return !strncmp(s, prefix, strlen(prefix)); }

// The hello of the released build (rc27), exactly as OnlineMode.cpp declared it: sizeof 28, read with
// 'got >= sizeof(Hello) && version == 4' from a 64-byte buffer.
struct Rc27Hello {
    uint32_t magic, version;
    uint8_t localPlayer, engaged, lockstep, delay;
    uint8_t delayFinal;
    uint8_t liveDelay;
    uint8_t ready;
    uint8_t pad[1];
    uint32_t sentMs;
    uint32_t echoMs, echoHeldMs;
};
static_assert(sizeof(Rc27Hello) == 28, "rc27's hello is 28 bytes");

Rc27Hello Rc27(uint8_t player, uint8_t engaged, uint8_t lockstep, uint8_t delay, uint8_t delayFinal, uint8_t live, uint8_t ready) {
    Rc27Hello h{};
    h.magic = 0x4F39524Bu;
    h.version = 4;
    h.localPlayer = player;
    h.engaged = engaged;
    h.lockstep = lockstep;
    h.delay = delay;
    h.delayFinal = delayFinal;
    h.liveDelay = live;
    h.ready = ready;
    h.sentMs = 111111;
    h.echoMs = 222222;
    h.echoHeldMs = 33;
    return h;
}

// What rc27 does with a packet (ReadHellos): false = skipped; else its view of ready / engaged.
bool Rc27Reads(const uint8_t* pkt, size_t size, Rc27Hello* out) {
    char buf[64];
    const int got = (int)(size < sizeof buf ? size : sizeof buf);  // ReadP2PPacket into 64 bytes
    memcpy(buf, pkt, (size_t)got);
    uint32_t magic = 0, version = 0;
    if (got < 8) return false;
    memcpy(&magic, buf, 4);
    memcpy(&version, buf + 4, 4);
    if (magic != 0x4F39524Bu) return false;
    if (version != 4 || got < (int)sizeof(Rc27Hello)) return false;
    memcpy(out, buf, sizeof *out);
    return true;
}

const Identity kMine = {20261110, 1200, 0x5AB648CB, 0xA615E846, 0xD9A634EB};

Fields NewFields(const Identity& id, bool valid, uint8_t lockstep, uint8_t ready, int wifi) {
    Fields f;
    f.localPlayer = 2;
    f.engaged = 0;
    f.lockstep = lockstep;
    f.delay = 2;
    f.delayFinal = 1;
    f.liveDelay = 3;
    f.ready = ready;
    f.sentMs = 5000;
    f.echoMs = 4000;
    f.echoHeldMs = 7;
    f.identityValid = valid;
    f.id = id;
    f.wifi = wifi;
    return f;
}

Parsed PeerWith(const Identity& id, bool valid = true, uint8_t lockstep = 0) {
    uint8_t pkt[kSize];
    Build(NewFields(id, valid, lockstep, 1, 0), pkt, sizeof pkt);
    return Parse(pkt, sizeof pkt);
}

const char* Name(Match m) {
    switch (m) {
    case Match::kUnknown: return "unknown";
    case Match::kSame: return "same";
    case Match::kPeerOlder: return "peer older";
    case Match::kThisOlder: return "this older";
    case Match::kDifferentBuild: return "different build";
    case Match::kContentDiffers: return "content differs";
    case Match::kModeDiffers: return "mode differs";
    case Match::kSwitchesDiffer: return "switches differ";
    }
    return "?";
}

const Local kAllReady = {true, true, true, false, true};
}  // namespace

int main() {
    char line[400];

    printf("-- 1. an rc27 hello (28 bytes, version 4)\n");
    {
        const Rc27Hello h = Rc27(1, 0, 0, 2, 1, 2, 1);
        const Parsed p = Parse(&h, sizeof h);
        Check(p.kind == Kind::kBase && p.version == 4, "parsed as a hello without identity");
        Check(p.f.localPlayer == 1 && p.f.delay == 2 && p.f.delayFinal == 1 && p.f.liveDelay == 2 && p.f.ready == 1 && !p.f.engaged &&
              p.f.sentMs == 111111 && p.f.echoMs == 222222 && p.f.echoHeldMs == 33, "its fields are read as rc27 wrote them");
        Check(!p.f.identityValid && p.f.wifi == -1, "no identity, Wi-Fi unknown");
        const Match m = Compare(kMine, true, false, p);
        Check(m == Match::kPeerOlder, "verdict: the other player's mod is older");
        Check(Compare(kMine, false, false, p) == Match::kPeerOlder, "the same before this game's own prints are computed");
        Describe(m, kMine, p, line, sizeof line);
        printf("      %s\n", line);
        Check(Starts(line, "ONLINE: the other player's mod is older ("), "log line begins as the launcher expects, details in parentheses");
        Check(!Ready(kAllReady, m), "this game does not say ready to it");
        Check(!MayEngage(kAllReady, m, true, true, true), "and does not engage, even though rc27 says ready and engaged");
    }

    printf("-- 2. this build's hello (52 bytes)\n");
    {
        uint8_t pkt[kSize];
        const Fields f = NewFields(kMine, true, 1, 1, 1);
        Check(Build(f, pkt, sizeof pkt) == kSize && kSize == 52 && kSize <= kRc27Buffer, "52 bytes, inside rc27's 64-byte read buffer");
        Check(Build(f, pkt, kSize - 1) == 0, "not written into a buffer that is too small");
        const Parsed p = Parse(pkt, sizeof pkt);
        Check(p.kind == Kind::kFull && p.f.identityValid, "parsed with its identity");
        Check(p.f.id.stampDate == 20261110 && p.f.id.stampTime == 1200 && p.f.id.content == 0x5AB648CB && p.f.id.dll == 0xA615E846 &&
              p.f.id.switches == 0xD9A634EB, "stamp and the three prints round-trip");
        Check(p.f.localPlayer == 2 && p.f.lockstep == 1 && p.f.delay == 2 && p.f.delayFinal == 1 && p.f.liveDelay == 3 && p.f.ready == 1 &&
              p.f.sentMs == 5000 && p.f.echoMs == 4000 && p.f.echoHeldMs == 7, "the first 28 bytes' fields round-trip");
        Check(p.f.wifi == 1, "Wi-Fi: yes");
        Build(NewFields(kMine, true, 0, 0, 0), pkt, sizeof pkt);
        Check(Parse(pkt, sizeof pkt).f.wifi == 0, "Wi-Fi: no");
        Build(NewFields(kMine, true, 0, 0, -1), pkt, sizeof pkt);
        Check(Parse(pkt, sizeof pkt).f.wifi == -1, "Wi-Fi: unknown");
        // The first 28 bytes are rc27's struct, byte for byte.
        const Rc27Hello ref = Rc27(2, 0, 1, 2, 1, 3, 1);
        Fields g = NewFields(kMine, true, 1, 1, 1);
        g.sentMs = ref.sentMs;
        g.echoMs = ref.echoMs;
        g.echoHeldMs = ref.echoHeldMs;
        Build(g, pkt, sizeof pkt);
        Check(!memcmp(pkt, &ref, sizeof ref), "the first 28 bytes equal rc27's struct for the same values");
        Rc27Hello seen{};
        Check(Rc27Reads(pkt, sizeof pkt, &seen) && seen.ready == 1 && seen.delay == 2 && seen.lockstep == 1, "rc27 accepts the longer packet and reads those fields");
    }

    printf("-- 3. what rc27 hears from this build\n");
    {
        // This build's verdict on an rc27 peer is 'peer older', so its hello says ready 0, and it never engages (engaged 0).
        const Rc27Hello theirs = Rc27(1, 0, 0, 2, 1, 2, 1);
        const Match m = Compare(kMine, true, false, Parse(&theirs, sizeof theirs));
        Fields f = NewFields(kMine, true, 0, Ready(kAllReady, m) ? 1 : 0, 0);
        f.engaged = MayEngage(kAllReady, m, true, true, true) ? 1 : 0;
        uint8_t pkt[kSize];
        Build(f, pkt, sizeof pkt);
        Rc27Hello seen{};
        const bool read = Rc27Reads(pkt, sizeof pkt, &seen);
        Check(read && seen.version == 4, "rc27 takes it as a valid hello (not 'another version', not 'no mod')");
        Check(read && !seen.ready && !seen.engaged, "ready 0 and engaged 0: rc27's 'if (!gPeerReady && !gPeerEngaged) return' never passes");
    }

    printf("-- 4. a longer hello of a later build\n");
    {
        uint8_t pkt[200];
        memset(pkt, 0xEE, sizeof pkt);
        Build(NewFields(kMine, true, 0, 1, 1), pkt, sizeof pkt);
        const Parsed a = Parse(pkt, 60), b = Parse(pkt, sizeof pkt);
        Check(a.kind == Kind::kFull && b.kind == Kind::kFull, "60 and 200 bytes: parsed, the tail ignored");
        Check(a.f.id.dll == kMine.dll && b.f.id.switches == kMine.switches && b.f.ready == 1 && b.f.wifi == 1, "the known part reads the same");
        Check(Compare(kMine, true, false, b) == Match::kSame, "same build: same");
    }

    printf("-- 5. truncated and garbage packets\n");
    {
        uint8_t pkt[kSize];
        Build(NewFields(kMine, true, 0, 1, 1), pkt, sizeof pkt);
        bool none = true, malformed = true;
        for (size_t n = 0; n < 8; ++n) none &= Parse(pkt, n).kind == Kind::kNone;
        for (size_t n = 8; n < kBaseSize; ++n) malformed &= Parse(pkt, n).kind == Kind::kMalformed;
        for (size_t n = kBaseSize + 1; n < kSize; ++n) malformed &= Parse(pkt, n).kind == Kind::kMalformed;
        Check(none, "0-7 bytes: not a hello");
        Check(malformed, "8-27 and 29-51 bytes with the magic and version 4: malformed (heard, not used)");
        Check(Parse(nullptr, 0).kind == Kind::kNone && Parse(nullptr, 52).kind == Kind::kNone, "no data: not a hello");
        Check(Compare(kMine, true, false, Parse(pkt, 40)) == Match::kUnknown, "a malformed hello gives no verdict (no line, not ready)");
        uint8_t junk[64];
        for (size_t i = 0; i < sizeof junk; ++i) junk[i] = (uint8_t)(i * 37 + 11);
        Check(Parse(junk, sizeof junk).kind == Kind::kNone, "64 bytes without the magic: not a hello");
        // Random bytes behind a valid header, every length up to 256: nothing out of range comes out.
        srand(12345);
        bool inRange = true;
        int full = 0, base = 0;
        for (int iter = 0; iter < 200000; ++iter) {
            uint8_t r[256];
            const size_t n = (size_t)(rand() % 257);
            for (size_t i = 0; i < n; ++i) r[i] = (uint8_t)rand();
            if (n >= 8 && iter % 4) {
                const uint32_t magic = kMagic, version = iter % 16 == 1 ? (uint32_t)rand() : kVersion;
                memcpy(r, &magic, 4);
                memcpy(r + 4, &version, 4);
            }
            const Parsed p = Parse(r, n);
            full += p.kind == Kind::kFull;
            base += p.kind == Kind::kBase;
            inRange &= p.f.delay <= kMaxDelay && p.f.liveDelay <= kMaxDelay && p.f.localPlayer <= 2 && p.f.engaged <= 1 && p.f.lockstep <= 1 &&
                       p.f.delayFinal <= 1 && p.f.ready <= 1 && p.f.wifi >= -1 && p.f.wifi <= 1;
            if (p.kind != Kind::kBase && p.kind != Kind::kFull) inRange &= p.f.delay == 0 && p.f.ready == 0 && p.f.engaged == 0 && !p.f.identityValid;
            const Match m = Compare(kMine, true, false, p);
            Describe(m, kMine, p, line, sizeof line);  // never past the buffer (the sanitizer-free check: it returns)
        }
        printf("      (200000 random packets: %d parsed as full hellos, %d as 28-byte ones)\n", full, base);
        Check(inRange && full > 1000, "200000 random packets: every parsed value in range");
    }

    printf("-- 6. out-of-range values from the other game\n");
    {
        Rc27Hello h = Rc27(3, 2, 7, 255, 9, 200, 0x80);
        Parsed p = Parse(&h, sizeof h);
        Check(p.f.delay == kMaxDelay && kMaxDelay == 8, "delay 255 -> 8 (the largest the mod proposes or takes)");
        Check(p.f.liveDelay == kMaxDelay, "live delay 200 -> 8");
        Check(p.f.localPlayer == 0, "player 3 -> 0 (unknown)");
        Check(p.f.engaged == 1 && p.f.lockstep == 1 && p.f.delayFinal == 1 && p.f.ready == 1, "flags 2 / 7 / 9 / 0x80 -> 1");
        h = Rc27(255, 0, 0, 9, 0, 8, 0);
        p = Parse(&h, sizeof h);
        Check(p.f.localPlayer == 0 && p.f.delay == 8 && p.f.liveDelay == 8, "player 255 -> 0, delay 9 -> 8, live delay 8 stays");
        h = Rc27(2, 0, 0, 0, 0, 0, 0);
        p = Parse(&h, sizeof h);
        Check(p.f.localPlayer == 2 && p.f.delay == 0 && p.f.liveDelay == 0, "player 2, delay 0, live delay 0 stay");
        uint8_t pkt[kSize];
        Build(NewFields(kMine, true, 0, 1, 1), pkt, sizeof pkt);
        pkt[11] = 0xFF;
        pkt[13] = 0x40;
        pkt[8] = 0x7F;
        pkt[48] = 0xFF;  // every flag bit
        p = Parse(pkt, sizeof pkt);
        Check(p.f.delay == 8 && p.f.liveDelay == 8 && p.f.localPlayer == 0 && p.f.wifi == 1 && p.f.identityValid, "the same in a 52-byte hello; unknown flag bits ignored");
    }

    printf("-- 7. every comparison outcome\n");
    {
        struct Case {
            const char* what;
            Identity peer;
            bool mineValid, peerValid;
            uint8_t peerLockstep;
            bool mineLockstep;
            Match want;
            const char* begins;  // the log line's beginning, null: no line
        };
        Identity older = kMine, newer = kMine, sameDay = kMine, otherDll = kMine, noStamp = kMine, repack = kMine, content = kMine, sw = kMine;
        older.stampDate = 20261101; older.dll = 0x11111111;
        newer.stampDate = 20261201; newer.stampTime = 900; newer.dll = 0x22222222;
        sameDay.stampTime = 1159; sameDay.dll = 0x33333333;
        otherDll.dll = 0x44444444;
        noStamp.stampDate = noStamp.stampTime = 0; noStamp.dll = 0x55555555;
        repack.stampDate = 20261111;  // the same DLL in a later zip
        content.content = 0x66666666;
        sw.switches = 0x77777777;
        Identity mineDev = kMine;
        mineDev.stampDate = mineDev.stampTime = 0;
        const Case cases[] = {
            {"same build, files, mode, switches", kMine, true, true, 0, false, Match::kSame, nullptr},
            {"same, both in Safe mode", kMine, true, true, 1, true, Match::kSame, nullptr},
            {"other DLL, earlier stamp", older, true, true, 0, false, Match::kPeerOlder, "ONLINE: the other player's mod is older ("},
            {"other DLL, later stamp", newer, true, true, 0, false, Match::kThisOlder, "ONLINE: this mod is older than the other player's ("},
            {"other DLL, same day, earlier time", sameDay, true, true, 0, false, Match::kPeerOlder, "ONLINE: the other player's mod is older ("},
            {"other DLL, same stamp", otherDll, true, true, 0, false, Match::kDifferentBuild, "ONLINE: the other player's mod is a different build ("},
            {"other DLL, their stamp unknown", noStamp, true, true, 0, false, Match::kDifferentBuild, "ONLINE: the other player's mod is a different build ("},
            {"same DLL, later stamp (repackaged zip)", repack, true, true, 0, false, Match::kSame, nullptr},
            {"same DLL, other game files", content, true, true, 0, false, Match::kContentDiffers, "ONLINE: the other player's game files differ"},
            {"same DLL and files, Safe mode on their side only", kMine, true, true, 1, false, Match::kModeDiffers, nullptr},
            {"same DLL and files, Safe mode on this side only", kMine, true, true, 0, true, Match::kModeDiffers, nullptr},
            {"same DLL, files and mode, other switches", sw, true, true, 0, false, Match::kSwitchesDiffer, "ONLINE: the other player's mod runs with other settings"},
            {"their prints not computed yet", kMine, true, false, 0, false, Match::kUnknown, nullptr},
            {"this game's prints not computed yet", kMine, false, true, 0, false, Match::kUnknown, nullptr},
        };
        static const char* const kThree[3] = {"ONLINE: the other player's mod is older", "ONLINE: this mod is older than the other player's",
                                              "ONLINE: the other player's mod is a different build"};
        for (const Case& c : cases) {
            const Parsed p = PeerWith(c.peer, c.peerValid, c.peerLockstep);
            const Match m = Compare(kMine, c.mineValid, c.mineLockstep, p);
            const int n = Describe(m, kMine, p, line, sizeof line);
            bool ok = m == c.want && (c.begins ? n > 0 && Starts(line, c.begins) : n == 0);
            int three = 0;
            for (const char* t : kThree) three += Starts(line, t);
            ok &= three <= 1 && (m == Match::kPeerOlder || m == Match::kThisOlder || m == Match::kDifferentBuild) == (three == 1);
            ok &= Ready(kAllReady, m) == (m == Match::kSame);
            char what[200];
            snprintf(what, sizeof what, "%s -> %s%s", c.what, Name(m), m == Match::kSame ? " (ready)" : " (not ready)");
            Check(ok, what);
            if (n) printf("      %s\n", line);
            if (const char* s = StatusText(m)) printf("      status: %s\n", s);
        }
        const Parsed dev = PeerWith(otherDll);
        Check(Compare(mineDev, true, false, dev) == Match::kDifferentBuild, "other DLL, this side's stamp unknown (developer tree) -> different build");
        Check(Compare(mineDev, true, false, PeerWith(mineDev)) == Match::kSame, "two developer trees with the same DLL -> same");
        Check(Compare(kMine, true, false, Parsed{}) == Match::kUnknown, "nothing heard -> unknown");
    }

    printf("-- 8. version 3 and version 5 peers\n");
    {
        uint8_t v3[12] = {};  // an old, shorter hello: only the header is common
        uint8_t v5[64];
        memset(v5, 0xFF, sizeof v5);
        const uint32_t magic = kMagic, three = 3, five = 5;
        memcpy(v3, &magic, 4);
        memcpy(v3 + 4, &three, 4);
        memcpy(v5, &magic, 4);
        memcpy(v5 + 4, &five, 4);
        const Parsed a = Parse(v3, sizeof v3), b = Parse(v5, sizeof v5);
        Check(a.kind == Kind::kOtherVersion && a.version == 3 && b.kind == Kind::kOtherVersion && b.version == 5, "both heard as the mod, another version");
        Check(b.f.delay == 0 && !b.f.ready && !b.f.engaged && !b.f.identityValid, "nothing else is read from another version's bytes");
        const Match ma = Compare(kMine, true, false, a), mb = Compare(kMine, false, true, b);
        Check(ma == Match::kPeerOlder && mb == Match::kThisOlder, "version 3 -> the other is older; version 5 -> this one is older");
        Describe(ma, kMine, a, line, sizeof line);
        printf("      %s\n", line);
        Check(Starts(line, "ONLINE: the other player's mod is older (theirs speaks online version 3, this one 4)"), "version 3 line");
        Describe(mb, kMine, b, line, sizeof line);
        printf("      %s\n", line);
        Check(Starts(line, "ONLINE: this mod is older than the other player's (theirs speaks online version 5, this one 4)"), "version 5 line");
        Check(!Ready(kAllReady, ma) && !Ready(kAllReady, mb) && !MayEngage(kAllReady, mb, true, true, true), "not ready, not engaging, with either");
    }

    printf("-- 9. the ready and engage decisions\n");
    {
        Check(Ready(kAllReady, Match::kSame), "all local conditions and the same build: ready");
        bool only = true;
        for (int m = 0; m <= (int)Match::kSwitchesDiffer; ++m)
            if ((Match)m != Match::kSame) only &= !Ready(kAllReady, (Match)m) && !MayEngage(kAllReady, (Match)m, true, true, true);
        Check(only, "any other verdict (unknown included): not ready, no engagement");
        Local l = kAllReady;
        l.fightActive = false;
        bool each = !Ready(l, Match::kSame);
        l = kAllReady, l.fightState = false, each &= !Ready(l, Match::kSame);
        l = kAllReady, l.delayFinal = false, each &= !Ready(l, Match::kSame);
        l = kAllReady, l.failedThisMatch = true, each &= !Ready(l, Match::kSame);
        l = kAllReady, l.oneOnOne = false, each &= !Ready(l, Match::kSame);
        Check(each, "each local condition alone keeps ready 0");
        Check(MayEngage(kAllReady, Match::kSame, true, true, false) && MayEngage(kAllReady, Match::kSame, true, false, true), "engages when the other says ready, or engaged");
        Check(!MayEngage(kAllReady, Match::kSame, true, false, false), "not while the other says neither");
        Check(!MayEngage(kAllReady, Match::kSame, false, true, true), "not before the other's delay is final");
    }

    printf("-- 10. the 'peer absent' test while engaged (3000 ms)\n");
    {
        const uint64_t E = 100000, A = 3000;
        Check(!PeerAbsent(E + 3000, E, E + 2900, false, false, A), "says neither, exactly 3 s after the engagement: not yet");
        Check(PeerAbsent(E + 3001, E, E + 2900, false, false, A), "says neither, after 3 s: absent (as before)");
        Check(!PeerAbsent(E + 60000, E, E + 59800, true, false, A) && !PeerAbsent(E + 60000, E, E + 59800, false, true, A), "hellos keep coming and say engaged or ready: never absent");
        Check(!PeerAbsent(E + 2000, E, E - 2500, false, true, A), "hellos stopped 2.5 s before the engagement, 2 s after it: not yet");
        Check(PeerAbsent(E + 3001, E, E - 2500, false, true, A), "the same 3 s after the engagement: absent (never before this change)");
        Check(PeerAbsent(E + 3001, E, 0, true, true, A), "no hello on record at all: absent 3 s after the engagement");
        Check(!PeerAbsent(E + 5000, E, E + 2500, true, true, A), "last hello 2.5 s after the engagement said engaged, 2.5 s ago: not yet");
        Check(PeerAbsent(E + 5501, E, E + 2500, true, true, A), "silent for 3 s since that hello: absent");
        Check(!PeerAbsent(E + 900000, E, E + 899900, true, true, A), "an old engagement (re-armed fights) with fresh hellos: not absent");
        Check(!PeerAbsent(E - 1, E, 0, false, false, A), "a clock before the engagement: not absent");
    }

    printf("-- 11. version.txt's stamp\n");
    {
        uint32_t d = 1, t = 1;
        const char a[] = "version=20261110-1200\r\ndll=0123ABCD\r\n";
        Check(ParseStamp(a, sizeof a - 1, &d, &t) && d == 20261110 && t == 1200, "version=20261110-1200 (package.ps1's file)");
        const char b[] = "dll=0123ABCD\nversion=20261109-0005  \n";
        Check(ParseStamp(b, sizeof b - 1, &d, &t) && d == 20261109 && t == 5, "on the second line, trailing spaces");
        const char c[] = "version=20261110-1200";
        Check(ParseStamp(c, sizeof c - 1, &d, &t) && d == 20261110, "no line end");
        const char e[] = "dll=0123ABCD\r\n";
        Check(!ParseStamp(e, sizeof e - 1, &d, &t) && d == 0 && t == 0, "no version line: 0/0");
        const char f[] = "version=2026111-1200\nversion=20261110_1200\nxversion=20261110-1200\nversion=20261310-1200\nversion=20261110-2460\nversion=20261110-12000\n";
        Check(!ParseStamp(f, sizeof f - 1, &d, &t) && d == 0, "malformed lines (short date, wrong separator, not at the line start, month 13, 24:60, long time): none");
        Check(!ParseStamp("", 0, &d, &t), "empty file");
        char s[24];
        Check(!strcmp(FormatStamp(20261110, 1200, s, sizeof s), "20261110-1200") && !strcmp(FormatStamp(20261109, 5, s, sizeof s), "20261109-0005") &&
              !strcmp(FormatStamp(0, 0, s, sizeof s), "none"), "printed back as 20261110-1200 / 20261109-0005 / none");
    }

    printf("-- 12. the texts\n");
    {
        bool fits = true, unique = true;
        for (int m = 0; m <= (int)Match::kSwitchesDiffer; ++m)
            if (const char* s = StatusText((Match)m)) {
                printf("      %-16s %s\n", Name((Match)m), s);
                fits &= strlen(s) <= 63 && Starts(s, "MK9 NETCODE  (") && !strstr(s, "does not run the mod");
            } else {
                unique &= (Match)m == Match::kUnknown || (Match)m == Match::kSame;
            }
        Check(fits, "every status text fits the overlay's 63 characters and none says the opponent has no mod");
        Check(unique, "only 'unknown' and 'same' have no status text of their own");
        const Parsed p = PeerWith(kMine);
        bool clean = true;
        for (int m = 0; m <= (int)Match::kSwitchesDiffer; ++m) {
            Describe((Match)m, kMine, p, line, sizeof line);
            clean &= !strstr(line, "ONLINE off") && !strstr(line, "ONLINE armed for") && !strstr(line, "does not run the mod") &&
                     !strstr(line, "the other player runs ") && (!line[0] || Starts(line, "ONLINE: "));
            char tiny[16];
            const int n = Describe((Match)m, kMine, p, tiny, sizeof tiny);
            clean &= n >= 0 && n < (int)sizeof tiny && strlen(tiny) == (size_t)n;
        }
        Check(clean, "no line collides with another launcher or smoke-test pattern; a short buffer is not overrun");
    }

    printf("\n%d checks, %d failed\n", gChecks, gFailed);
    return gFailed ? 1 : 0;
}
