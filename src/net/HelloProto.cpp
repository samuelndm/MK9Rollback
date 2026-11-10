#include "HelloProto.h"

#include <cstdio>
#include <cstring>

namespace HelloProto {
namespace {
void Put32(uint8_t* p, uint32_t v) { memcpy(p, &v, 4); }
uint32_t Get32(const uint8_t* p) {
    uint32_t v;
    memcpy(&v, p, 4);
    return v;
}
uint8_t Flag(uint8_t b) { return b ? 1 : 0; }
uint8_t Delay(uint8_t b) { return b > kMaxDelay ? (uint8_t)kMaxDelay : b; }
bool StampKnown(const Identity& i) { return i.stampDate || i.stampTime; }

bool Digits(const char* p, int n, uint32_t* out) {
    uint32_t v = 0;
    for (int i = 0; i < n; ++i) {
        if (p[i] < '0' || p[i] > '9') return false;
        v = v * 10 + (uint32_t)(p[i] - '0');
    }
    *out = v;
    return true;
}
}  // namespace

size_t Build(const Fields& f, void* out, size_t cap) {
    if (cap < kSize) return 0;
    uint8_t* b = static_cast<uint8_t*>(out);
    memset(b, 0, kSize);
    Put32(b, kMagic);
    Put32(b + 4, kVersion);
    b[8] = f.localPlayer;
    b[9] = f.engaged;
    b[10] = f.lockstep;
    b[11] = f.delay;
    b[12] = f.delayFinal;
    b[13] = f.liveDelay;
    b[14] = f.ready;
    Put32(b + 16, f.sentMs);
    Put32(b + 20, f.echoMs);
    Put32(b + 24, f.echoHeldMs);
    if (f.identityValid) {
        Put32(b + 28, f.id.stampDate);
        Put32(b + 32, f.id.stampTime);
        Put32(b + 36, f.id.content);
        Put32(b + 40, f.id.dll);
        Put32(b + 44, f.id.switches);
    }
    b[48] = (uint8_t)((f.identityValid ? kIdentityValid : 0) | (f.wifi >= 0 ? kWifiKnown : 0) | (f.wifi > 0 ? kOnWifi : 0));
    return kSize;
}

Parsed Parse(const void* data, size_t size) {
    Parsed r;
    const uint8_t* b = static_cast<const uint8_t*>(data);
    if (!b || size < 8 || Get32(b) != kMagic) return r;
    r.version = Get32(b + 4);
    if (r.version != kVersion) {
        r.kind = Kind::kOtherVersion;
        return r;
    }
    if (size < kBaseSize || (size > kBaseSize && size < kSize)) {
        r.kind = Kind::kMalformed;
        return r;
    }
    Fields& f = r.f;
    f.localPlayer = b[8] == 1 || b[8] == 2 ? b[8] : 0;
    f.engaged = Flag(b[9]);
    f.lockstep = Flag(b[10]);
    f.delay = Delay(b[11]);
    f.delayFinal = Flag(b[12]);
    f.liveDelay = Delay(b[13]);
    f.ready = Flag(b[14]);
    f.sentMs = Get32(b + 16);
    f.echoMs = Get32(b + 20);
    f.echoHeldMs = Get32(b + 24);
    if (size == kBaseSize) {
        r.kind = Kind::kBase;
        return r;
    }
    r.kind = Kind::kFull;
    const uint8_t flags = b[48];
    f.identityValid = (flags & kIdentityValid) != 0;
    if (f.identityValid) {
        f.id.stampDate = Get32(b + 28);
        f.id.stampTime = Get32(b + 32);
        f.id.content = Get32(b + 36);
        f.id.dll = Get32(b + 40);
        f.id.switches = Get32(b + 44);
    }
    f.wifi = !(flags & kWifiKnown) ? -1 : (flags & kOnWifi) ? 1 : 0;
    return r;
}

Match Compare(const Identity& mine, bool mineValid, bool mineLockstep, const Parsed& peer) {
    switch (peer.kind) {
    case Kind::kNone:
    case Kind::kMalformed:
        return Match::kUnknown;
    case Kind::kOtherVersion:
        return peer.version < kVersion ? Match::kPeerOlder : Match::kThisOlder;
    case Kind::kBase:
        return Match::kPeerOlder;
    case Kind::kFull:
        break;
    }
    if (!mineValid || !peer.f.identityValid) return Match::kUnknown;
    const Identity& p = peer.f.id;
    if (mine.dll != p.dll) {
        if (StampKnown(mine) && StampKnown(p) && (mine.stampDate != p.stampDate || mine.stampTime != p.stampTime)) {
            const bool peerEarlier = p.stampDate != mine.stampDate ? p.stampDate < mine.stampDate : p.stampTime < mine.stampTime;
            return peerEarlier ? Match::kPeerOlder : Match::kThisOlder;
        }
        return Match::kDifferentBuild;
    }
    if (mine.content != p.content) return Match::kContentDiffers;
    if ((peer.f.lockstep != 0) != mineLockstep) return Match::kModeDiffers;
    if (mine.switches != p.switches) return Match::kSwitchesDiffer;
    return Match::kSame;
}

bool Ready(const Local& l, Match m) {
    return l.fightActive && l.fightState && l.delayFinal && !l.failedThisMatch && l.oneOnOne && m == Match::kSame;
}

bool MayEngage(const Local& l, Match m, bool peerDelayFinal, bool peerReady, bool peerEngaged) {
    return Ready(l, m) && peerDelayFinal && (peerReady || peerEngaged);
}

bool PeerAbsent(uint64_t now, uint64_t engagedAt, uint64_t lastHelloRx, bool peerEngaged, bool peerReady, uint64_t absentMs) {
    if (now < engagedAt || now - engagedAt <= absentMs) return false;
    const uint64_t heard = lastHelloRx > engagedAt ? lastHelloRx : engagedAt;
    if (now >= heard && now - heard > absentMs) return true;  // silent since then
    return lastHelloRx > engagedAt && !peerEngaged && !peerReady;
}

bool ParseStamp(const char* text, size_t size, uint32_t* date, uint32_t* time) {
    *date = *time = 0;
    static const char kKey[] = "version=";
    const size_t keyLen = sizeof kKey - 1, need = keyLen + 8 + 1 + 4;
    size_t at = size >= 3 && !memcmp(text, "\xEF\xBB\xBF", 3) ? 3 : 0;
    while (at < size) {
        size_t end = at;
        while (end < size && text[end] != '\n' && text[end] != '\r') ++end;
        const char* line = text + at;
        size_t len = end - at;
        while (len && (line[len - 1] == ' ' || line[len - 1] == '\t')) --len;
        uint32_t d = 0, t = 0;
        if (len == need && !memcmp(line, kKey, keyLen) && Digits(line + keyLen, 8, &d) && line[keyLen + 8] == '-' &&
            Digits(line + keyLen + 9, 4, &t)) {
            const uint32_t month = d / 100 % 100, day = d % 100;
            if (d >= 20000101 && month >= 1 && month <= 12 && day >= 1 && day <= 31 && t / 100 < 24 && t % 100 < 60) {
                *date = d;
                *time = t;
                return true;
            }
        }
        at = end + 1;
    }
    return false;
}

const char* FormatStamp(uint32_t date, uint32_t time, char* out, size_t cap) {
    if (!date && !time) snprintf(out, cap, "none");
    else snprintf(out, cap, "%08u-%04u", (unsigned)(date % 100000000u), (unsigned)(time % 10000u));
    return out;
}

int Describe(Match m, const Identity& mine, const Parsed& peer, char* out, size_t cap) {
    if (!cap) return 0;
    out[0] = 0;
    char theirs[24], ours[24];
    FormatStamp(peer.f.id.stampDate, peer.f.id.stampTime, theirs, sizeof theirs);
    FormatStamp(mine.stampDate, mine.stampTime, ours, sizeof ours);
    int n = 0;
    switch (m) {
    case Match::kPeerOlder:
        if (peer.kind == Kind::kOtherVersion)
            n = snprintf(out, cap, "ONLINE: the other player's mod is older (theirs speaks online version %u, this one %u); not engaging: "
                                   "MK9's netcode plays until they update", (unsigned)peer.version, (unsigned)kVersion);
        else if (peer.kind == Kind::kBase)
            n = snprintf(out, cap, "ONLINE: the other player's mod is older (theirs is a release from before the build check, this %s); "
                                   "not engaging: MK9's netcode plays until they update", ours);
        else
            n = snprintf(out, cap, "ONLINE: the other player's mod is older (theirs %s, this %s); not engaging: MK9's netcode plays "
                                   "until they update", theirs, ours);
        break;
    case Match::kThisOlder:
        if (peer.kind == Kind::kOtherVersion)
            n = snprintf(out, cap, "ONLINE: this mod is older than the other player's (theirs speaks online version %u, this one %u); "
                                   "not engaging: MK9's netcode plays until this mod is updated", (unsigned)peer.version, (unsigned)kVersion);
        else
            n = snprintf(out, cap, "ONLINE: this mod is older than the other player's (theirs %s, this %s); not engaging: MK9's netcode "
                                   "plays until this mod is updated", theirs, ours);
        break;
    case Match::kDifferentBuild:
        n = snprintf(out, cap, "ONLINE: the other player's mod is a different build (DLL theirs %08X, this %08X; release theirs %s, this "
                               "%s); not engaging: both must use the same zip", (unsigned)peer.f.id.dll, (unsigned)mine.dll, theirs, ours);
        break;
    case Match::kContentDiffers:
        n = snprintf(out, cap, "ONLINE: the other player's game files differ from this game's (Asset files: theirs %08X, this %08X); not "
                               "engaging: rollback needs the same unmodified game files on both PCs",
                     (unsigned)peer.f.id.content, (unsigned)mine.content);
        break;
    case Match::kSwitchesDiffer:
        n = snprintf(out, cap, "ONLINE: the other player's mod runs with other settings than this one (snapshot/FP switches: theirs "
                               "%08X, this %08X); not engaging until they match", (unsigned)peer.f.id.switches, (unsigned)mine.switches);
        break;
    case Match::kUnknown:
    case Match::kSame:
    case Match::kModeDiffers:
        break;
    }
    if (n < 0) n = 0;
    return (size_t)n < cap ? n : (int)cap - 1;
}

const char* StatusText(Match m) {
    switch (m) {
    case Match::kPeerOlder: return "MK9 NETCODE  (the opponent's mod is older: they must update)";
    case Match::kThisOlder: return "MK9 NETCODE  (your mod is older: update it)";
    case Match::kDifferentBuild: return "MK9 NETCODE  (different builds of the mod)";
    case Match::kContentDiffers: return "MK9 NETCODE  (your game files differ from the opponent's)";
    case Match::kModeDiffers: return "MK9 NETCODE  (Safe mode on one side only: pick the same mode)";
    case Match::kSwitchesDiffer: return "MK9 NETCODE  (the two mods run with different settings)";
    case Match::kUnknown:
    case Match::kSame:
        break;
    }
    return nullptr;
}

}  // namespace HelloProto
