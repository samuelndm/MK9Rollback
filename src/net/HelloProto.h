#pragma once
#include <cstddef>
#include <cstdint>

// The online mode's hello datagram (Steam P2P channel 79, OnlineMode.cpp) and the decisions taken from it. Nothing here
// touches the game, Steam or Windows: the two-PC paths cannot run in the test harness, so these functions are checked by
// a host-side test instead (hello_test).
//
// Wire layout, little endian, protocol version 4:
//    0 u32 magic "KR9O"        4 u32 version
//    8 u8 localPlayer          9 u8 engaged        10 u8 lockstep       11 u8 delay
//   12 u8 delayFinal          13 u8 liveDelay      14 u8 ready          15 u8 0
//   16 u32 sentMs             20 u32 echoMs        24 u32 echoHeldMs
//   -- 28 bytes: the whole hello of the releases up to rc27, which read exactly these and ignore the rest --
//   28 u32 stampDate          32 u32 stampTime     (version.txt's version=YYYYMMDD-HHMM as two decimal numbers; 0: none)
//   36 u32 content            40 u32 dll           44 u32 switches      (EnvFingerprint's three prints)
//   48 u8 flags (kIdentityValid | kWifiKnown | kOnWifi)                 49 u8[3] 0
//   -- 52 bytes; a later build may append --
// rc27 reads a hello into a 64-byte buffer and takes any packet of 28 bytes or more with version 4, so the added part
// keeps the version, and the packet stays under 64 bytes. A 28-byte version-4 hello means "a build before the identity".
namespace HelloProto {

constexpr uint32_t kMagic = 0x4F39524Bu;  // "KR9O"
constexpr uint32_t kVersion = 4;
constexpr size_t kBaseSize = 28, kSize = 52, kRc27Buffer = 64;
// The largest input delay a game proposes or takes: 'online <id> N' clamps N to 0-8 and the running session's delay
// (Rollback::SetSessionDelay) takes 1-8; the ping's proposals stay below (Safe mode 2-3, rollback 1-6). A larger value
// in a hello is not this mod's: GGPO's input queue asserts on a large delay, which ends the process.
constexpr int kMaxDelay = 8;

enum Flags : uint8_t { kIdentityValid = 1, kWifiKnown = 2, kOnWifi = 4 };

// What two games must share to run one session: the release (to say which side is older), and the three prints the
// session's environment check compares at frame 24 (EnvFingerprint).
struct Identity {
    uint32_t stampDate = 0, stampTime = 0;  // 0/0: unknown (a developer tree has no version.txt)
    uint32_t content = 0, dll = 0, switches = 0;
};

struct Fields {
    uint8_t localPlayer = 0;  // 1 or 2; 0: unknown
    uint8_t engaged = 0, lockstep = 0;
    uint8_t delay = 0;       // this game's proposal (the session uses the larger of the two)
    uint8_t delayFinal = 0;  // the proposal will not change any more this match
    uint8_t liveDelay = 0;   // the delay the current round trips allow (0: none; between rounds)
    uint8_t ready = 0;       // this game would engage now
    uint32_t sentMs = 0;                  // the sender's clock at the send
    uint32_t echoMs = 0, echoHeldMs = 0;  // the last sentMs read from the other game, and how long it was held
    // The part after byte 28:
    bool identityValid = false;  // the prints are computed (false in the first moments after the mod was armed)
    Identity id;
    int wifi = -1;  // this PC's default route: 1 Wi-Fi, 0 not, -1 unknown
};

// Writes the 52-byte hello; returns its size (0 when cap is too small).
size_t Build(const Fields& f, void* out, size_t cap);

enum class Kind : uint8_t {
    kNone,          // not this mod's hello (under 8 bytes or another magic): as if nothing had arrived
    kMalformed,     // this mod's magic and version 4, but a length no build sends (under 28, or 29-51): heard, not used
    kOtherVersion,  // this mod's magic with another protocol version: only 'version' means anything
    kBase,          // exactly the 28 bytes of rc27 and before: the fields without an identity
    kFull,          // 52 bytes or more
};

struct Parsed {
    Kind kind = Kind::kNone;
    uint32_t version = 0;
    Fields f;  // validated: flags 0/1, localPlayer 0-2, delays 0-kMaxDelay
};

// Any byte string of any length. Peer-controlled values are brought into the ranges the mod itself sends.
Parsed Parse(const void* data, size_t size);

enum class Match : uint8_t {
    kUnknown,         // not known yet: no usable hello, or a side whose prints are still being computed
    kSame,
    kPeerOlder,       // an older protocol version, a hello without identity, or another DLL with an earlier release stamp
    kThisOlder,
    kDifferentBuild,  // another DLL, and the stamps do not order the two (equal, or one unknown)
    kContentDiffers,  // the same DLL, other game files
    kModeDiffers,     // the same DLL and files, Safe mode on one side only (OnlineMode's mode mismatch)
    kSwitchesDiffer,  // the same DLL, files and mode, other determinism switches
};

// The same DLL print is the same build whatever the stamps say (a zip repackaged from the same binaries). With
// different DLLs, two known and different stamps say which side is older.
Match Compare(const Identity& mine, bool mineValid, bool mineLockstep, const Parsed& peer);
inline bool Mismatch(Match m) { return m != Match::kUnknown && m != Match::kSame; }

// This game's own conditions to engage (OnlineMode's LocalReady).
struct Local {
    bool fightActive = false;   // the connection's gamepad data is active
    bool fightState = false;    // the fight's own connection phase (game state 8)
    bool delayFinal = false;
    bool failedThisMatch = false;
    bool oneOnOne = false;      // a one-on-one Player/Private match
};
// The hello's 'ready': never while the other game's identity is missing or differs, so neither game engages then (a
// game engages only when the other one says ready or engaged).
bool Ready(const Local& l, Match m);
bool MayEngage(const Local& l, Match m, bool peerDelayFinal, bool peerReady, bool peerEngaged);

// Engaged and waiting for the session: the other game is not coming. Either its latest hello, from after this game
// engaged, says neither engaged nor ready and absentMs have passed since the engagement; or nothing was heard from it for
// absentMs counted from the later of the engagement and its last hello (hellos that stopped before the engagement used
// to leave this game waiting for the join timeout).
bool PeerAbsent(uint64_t now, uint64_t engagedAt, uint64_t lastHelloRx, bool peerEngaged, bool peerReady, uint64_t absentMs);

// "version=YYYYMMDD-HHMM" on a line of its own in version.txt's text (tools\package.ps1); false when there is none.
bool ParseStamp(const char* text, size_t size, uint32_t* date, uint32_t* time);
// "20261110-1200", or "none" for 0/0.
const char* FormatStamp(uint32_t date, uint32_t time, char* out, size_t cap);

// The log line of a verdict that keeps MK9's netcode, details in parentheses; returns its length, 0 when there is
// nothing to say (kUnknown, kSame, and kModeDiffers, which OnlineMode reports itself). The launcher matches the
// beginnings (tools\launcher_lib.ps1):
//   "ONLINE: the other player's mod is older", "ONLINE: this mod is older than the other player's",
//   "ONLINE: the other player's mod is a different build".
int Describe(Match m, const Identity& mine, const Parsed& peer, char* out, size_t cap);
// The status line for it (at most 63 characters: Overlay's text field), or null.
const char* StatusText(Match m);

}  // namespace HelloProto
