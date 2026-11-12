#include "OnlineMode.h"

#include <windows.h>

#include <algorithm>
#include <cstring>

#include "../debug/CrashLog.h"
#include "../debug/Lab.h"
#include "../debug/Logger.h"
#include "../game/Input.h"
#include "../game/MK9Game.h"
#include "../hook/CodePatch.h"
#include "../rollback/FramePace.h"
#include "../rollback/RollbackManager.h"
#include "../rollback/Snapshot.h"
#include "EnvFingerprint.h"
#include "FinisherRule.h"
#include "HelloProto.h"
#include "SteamBridge.h"

namespace {
constexpr uintptr_t kGamepadExchange = 0x00885F80;  // thiscall(MKOnlineGamepadData*, connection) -> int, ret 4
const uint8_t kExchangeOrig[7] = {0x55, 0x8B, 0xEC, 0x51, 0x56, 0x8B, 0xF1};  // push ebp; mov ebp,esp; push ecx; push esi; mov esi,ecx
constexpr uintptr_t kIdleMs = 0x004F5E10;  // thiscall(socket) -> ms since the last packet (connection timeouts)
const uint8_t kIdleOrig[6] = {0x53, 0x56, 0x8B, 0xF1, 0x33, 0xDB};  // push ebx; push esi; mov esi,ecx; xor ebx,ebx
constexpr uint32_t kConnToGamepads = 0x4C3C;  // MKOnlineGamepadData inside the connection (call at 0x008868EC)
constexpr int kHelloChannel = 79;
// The hello's bytes and the decisions taken from them: HelloProto.h (version 4, magic "KR9O").
// 2: lockstep flag and input delay; 3: + the delay proposal's state and a ping echo; 4: + the live proposal and 'ready'
// (a game engages only when the other one is ready too); still 4, 24 bytes longer (2026-11-10): + this build's
// identity and the Wi-Fi flag. The releases up to rc27 read the first 28 bytes of it and never see 'ready' from a
// build with an identity, so they keep MK9's netcode against it.
constexpr ULONGLONG kHelloEveryMs = 500, kHelloFastMs = 100, kHelloFreshMs = 3000, kGraceMs = 15000;
// GameInfo+0x4 after the last round: MK9's results and rematch screens (8 through the rounds). 11 is the finisher
// phase of a match with blood on, which ends the session by a rule of its own (FinisherRule.h, used below).
constexpr uint32_t kResultsState = 9;
// The session ends there with this reason: a normal end, like "timeline restart" (RollbackManager's GgpoStop).
constexpr const char* kMatchOver = "match over";

// The online message send (cdecl type, data, size; 6 callers): e.g. native 1182 -> 0x008808C0 sends message 14 with
// two 16-bit connection values (fight scripts call it about 300-450 frames into a fight). A rollback replays script
// natives, and a replay must not send what the original pass already sent; connection memory is not snapshotted, so
// skipping the replay's send leaves everything as the original pass left it.
constexpr uintptr_t kOnlineManager = 0x00E924F0;  // online manager; 0 when the game is not online
constexpr uintptr_t kSendMessage = 0x0081BCD0;
const uint8_t kSendOrig[5] = {0x55, 0x8B, 0xEC, 0x6A, 0xFF};  // push ebp; mov ebp,esp; push -1
using SendFn = int(__cdecl*)(int type, const void* data, int size);
uint8_t* gSendTramp = nullptr;
uint32_t gReplaySends = 0;

using ExchangeFn = int(__thiscall*)(void* self, int arg);
using IdleFn = uint32_t(__thiscall*)(void* self);
uint8_t* gExchangeTramp = nullptr;
uint8_t* gIdleTramp = nullptr;

uint64_t gPeer = 0;
bool gAuto = false;  // 'online auto': gPeer is found in the game's connection, per match
ULONGLONG gLastScan = 0;
bool gScanLogged = false;  // per match
int gDelay = 1;  // the fixed input delay (unused with gPingDelay)

// Input delay from the ping ('online ... ping', the launcher's default). Each hello echoes the other game's last clock
// value, so every hello read gives a round trip. That includes both games' read latency (hellos are read once per
// tick), which the session has as well (the Steam bridge forwards at the frame boundaries). A game proposes a delay once
// it has kPingFinal round trips (or at the fight's connection, with what it has); the session uses the larger of the
// two proposals, so both games run the same delay. GGPO would also accept a different delay per side.
bool gPingDelay = false;
constexpr uint32_t kPingKeep = 16, kPingMin = 4, kPingFinal = 8;
constexpr int kMaxRollbackDelay = 6;  // DelayFromPing's largest rollback delay (4 before 2026-11-06)
// Everything this game proposes fits what a hello may carry (the other game's values are cut to it when read).
static_assert(kMaxRollbackDelay <= HelloProto::kMaxDelay, "the ping's proposals must fit the hello's delay range");
uint32_t gPing[kPingKeep] = {}, gPingCount = 0;  // round trips in ms (ring; gPingCount counts every sample)
uint64_t gPingPeer = 0;                          // the opponent the samples belong to
uint32_t gEchoMs = 0, gEchoReadAt = 0;           // the other game's last hello clock and when it was read (NowMs)
uint32_t gLateHelloReads = 0;                    // hellos read late (ReadHellos), not used for the round trip
int gOwnDelay = -1;                             // this game's proposal once final (-1: still measuring)
int gPeerDelayFinal = 0;
int gSessionDelay = 0;                           // the agreed delay of the current engagement
// Between rounds the session's delay follows the round trips measured during the match: both fighters' input locked
// (PlayerInfo+0x944, set by the game in the round intros and after a KO; command queries return 0 then) for
// kLockedTicksMin ticks, once per such stretch, the larger of both games' live proposals. A larger delay repeats the last
// input for the added frames, a smaller one drops one input per frame (GGPO, patched to send the repeated frames).
constexpr int kLockedTicksMin = 30;
constexpr uint32_t kDownMarginMs = 8;  // a smaller delay only with this much round trip to spare (no flapping at a step)
int gPeerLive = 0;                     // the other game's live proposal (0: none, e.g. an older build)
// One game engaged alone skips MK9's exchange while the other's MK9 waits for its pads (MK9 drops the match after ~12 s):
// review 2026-11-01. A game engages only when the other's latest hello says ready (or engaged), and gives up the wait at
// the fight start when the other says neither for kPeerAbsentMs.
bool gPeerReady = false, gPeerEngaged = false;
ULONGLONG gEngagedAt = 0;
constexpr ULONGLONG kPeerAbsentMs = 3000;
bool gModeMismatchLogged = false;     // per match: Safe/Rollback differ (re-checked every tick, not latched)
const volatile uint8_t* gMatchGamepads = nullptr;  // the connection's gamepad data of the current match
int gLockedTicks = 0;
bool gLockDone = false;
uint32_t gLockStretches = 0, gDelayChanges = 0;  // this engagement
bool gEngaged = false, gFailedThisMatch = false;
volatile uint8_t* gGamepads = nullptr;  // last MKOnlineGamepadData seen by the exchange
uint32_t gActiveCalls = 0, gSkipped = 0;  // game thread only
ULONGLONG gLastHelloTx = 0, gLastHelloRx = 0, gGraceUntil = 0, gInactiveSince = 0;
// MK9's exchange got the opponent's data (result 1) at this tick (GetTickCount), and a session of ours ran in this match:
// IdleHook counts the gap from that success when MK9's own timer is older.
volatile DWORD gLastExOk = 0;
bool gSessionThisMatch = false;
int gPeerPlayer = 0;
int gPeerLockstep = -1, gPeerDelay = -1;  // the other player's mode, from its hello (-1: not heard yet)
bool gWasActive = false;
uint32_t gRearms = 0;  // sessions re-armed at a fight end (this match)
bool gMatchRefused = false;  // this match is not one-on-one Player/Private (logged once per match)
bool gStateWaitLogged = false;  // "not in the fight's phase yet" (logged once per connection phase)
constexpr uint32_t kFightState = 8;  // GameInfo+0x4 in online matches: 7 character select, 1 stage load, 8 fight
constexpr uint32_t kLoadState = 1;

// MK9's wait for the other player's stage load. From this game's own "loaded" (script native 1182) the fight script
// counts 2400 ticks, 40 s at 60 ticks per second, and then gives up (natives 1209/1217/1144; the match disconnects).
// The load wait is not paced by MK9's frame limiter: 2026-11-11 the user's PC ran it at 430-625 ticks per second, which
// left the other player's PC 3.8-5.6 s to finish loading after this one. Three starts in a row failed that way (18:42:26:
// 1182 at gf 2772, the give-up natives at gf 5172, 3.84 s later), and the one that worked at 18:46:37 had used 1578 of
// the 2400 ticks. 2026-10-31 20:46 was the same count (gf 497 -> 2897 in 6.1 s; read as the virtual script clock then,
// but players have had MK9's real clock outside sessions since). From 1182 until the load state ends, the ticks are
// held to 60 per second: the wait gets the 40 s its count stands for. Only this game's own wait changes; nothing is
// sent, and a game without the change still gives up on its own count.
constexpr ULONGLONG kLoadWaitMaxMs = 60000;
bool gLoadWait = false;
ULONGLONG gLoadWaitSince = 0;
uint32_t gLoadWaitGf = 0, gLoadWaitTicks = 0;
int64_t gLoadWaitNext = 0;                 // performance-counter value of the next tick's earliest start
uint32_t gTicksThisSecond = 0, gTickRate = 0;  // engine ticks per second (Tick's one-second window)

// The other game's build, from its hellos (HelloProto.h). Two games with the same protocol version but different DLLs,
// game files or switches used to engage and stop at the session's environment check (frame 24, in mid-fight, falling
// back to MK9's netcode from a state the two games no longer shared): every match after one player updated. Now a game
// says 'ready', and engages, only against the same build, files, mode and switches; anything else is said once per
// match and MK9's netcode plays. Per opponent (gHelloPeer).
HelloProto::Parsed gPeerHello;       // the opponent's latest hello with a meaning (kind kNone: none yet)
uint64_t gHelloPeer = 0;             // whose gPeerHello, gAnyHelloRx and gPeerWifi are
ULONGLONG gAnyHelloRx = 0;           // the last hello of any version or size from the opponent's mod (0: none this match)
HelloProto::Identity gOwnId;         // this game's side (UpdateVerdict, every tick with an opponent)
bool gOwnIdValid = false;            // false until EnvFingerprint's background pass is through
HelloProto::Match gVerdict = HelloProto::Match::kUnknown;
bool gBuildLineLogged = false, gContentLineLogged = false, gSwitchLineLogged = false;  // per match
// Connection facts for the status line (read by the overlay through LocalOnWifi/PeerOnWifi/LinkRelayed every tick).
volatile LONG gLocalWifi = -1;  // this PC's default route: 1 Wi-Fi, 0 not, -1 unknown (WifiProbe's thread)
volatile LONG gWifiState = 0;   // 0 not looked at this match, 1 the look runs, 2 done
int gPeerWifi = -1;             // the opponent's, from its hello (-1: not said)
bool gLinkRelayed = false;      // Steam's P2P session with the opponent goes through a relay (read with every hello sent)
bool gLinkLogged = false;       // per match
ULONGLONG gPeerKnownAt = 0;     // the opponent of this connection phase has been known since (0: not known)
bool gMatchModeLogged = false;  // per match

uint32_t GameState() {
    const uint32_t gi = *reinterpret_cast<const volatile uint32_t*>(MK9Game::Get().gameInfoPtr);
    return gi ? *reinterpret_cast<const volatile uint32_t*>(gi + 0x4) : 0xFFFFFFFF;
}

// Connection end report. 2026-10-31, first engaged two-PC test: MK9's connection closed at the end of the character
// select in both attempts, after the mod armed and before any fight frame; nothing in the logs said why. Kept here:
// MK9's packet gaps (the idle-time function its timeouts read), the exchange's results and MK9's last sent messages.
volatile uint32_t gIdlePeak = 0;  // longest gap MK9's connection reported during the current second (ms)
uint32_t gIdlePeaks[8] = {}, gIdlePeakPos = 0, gIdleLogs = 0;
ULONGLONG gIdleWindowAt = 0;
// MK9 asks the idle time of more than one socket, and one of them can be a socket nobody ever writes to: 2026-11-11 one
// game reported 2,210,859 ms and counting, a line every second of every fight. A gap counts for the report only on a
// socket that has had a packet (has ever answered less than a second). A socket that received and then stopped still
// counts, whenever it stopped (2026-11-03: 166 s, counted from a session's start, closed a connection).
void* gIdleReceived[8] = {};  // the sockets that have had a packet, the latest 8
uint32_t gIdleReceivedPos = 0;

bool IdleSocketReceived(void* self, uint32_t ms) {
    for (void* s : gIdleReceived)
        if (s == self) return true;
    if (ms >= 1000) return false;
    gIdleReceived[gIdleReceivedPos++ % 8] = self;
    return true;
}
int gExResults[16] = {};
uint32_t gExPos = 0, gExZero = 0, gExOne = 0, gExOther = 0;
struct SentMsg {
    int type, size;
    ULONGLONG at;
};
SentMsg gSentMsgs[8] = {};
uint32_t gSentPos = 0;
// What closes the connection: hardware write watches (every thread) on the gamepad data's active flag and the three
// flags of MK9's "connection closing" check 0x0081C430 (online manager +0x20, +0x1B0, [+0x1F4]+0x78; any nonzero makes
// the exchange return 0). Each change is logged with its writer. Armed while the online data is active.
bool gWatchArmed = false;

// snprintf at buf + n that never runs past the buffer
template <typename... A>
void Append(char* buf, size_t cap, int& n, const char* fmt, A... a) {
    if (n < 0 || (size_t)n + 1 >= cap) return;
    const int w = snprintf(buf + n, cap - n, fmt, a...);
    if (w > 0) n = (size_t)n + w + 1 < cap ? n + w : (int)cap - 1;
}

bool Readable(const volatile void* p, size_t n) {
    MEMORY_BASIC_INFORMATION mbi{};
    if (!VirtualQuery(const_cast<const void*>(p), &mbi, sizeof mbi) || mbi.State != MEM_COMMIT) return false;
    if (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) return false;
    return reinterpret_cast<uintptr_t>(p) + n <= reinterpret_cast<uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
}

void ArmConnectionWatch() {
    uintptr_t a[4] = {};
    const char* const names[4] = {"gamepads+0x18 (online data active)", "manager+0x20", "manager+0x1B0", "[manager+0x1F4]+0x78"};
    const volatile uint8_t* g = gGamepads;
    if (g && Readable(g + 0x18, 4)) a[0] = reinterpret_cast<uintptr_t>(g) + 0x18;
    const uint32_t mgr = *reinterpret_cast<const volatile uint32_t*>(kOnlineManager);
    if (mgr && Readable(reinterpret_cast<void*>(mgr), 0x1F8)) {
        a[1] = mgr + 0x20;
        a[2] = mgr + 0x1B0;
        const uint32_t sub = *reinterpret_cast<const volatile uint32_t*>(mgr + 0x1F4);
        if (sub && Readable(reinterpret_cast<void*>(sub + 0x78), 4)) a[3] = sub + 0x78;
    }
    CrashLog::ChangeWatchArm(a, names);
    gWatchArmed = true;
}

void DisarmConnectionWatch() {
    if (!gWatchArmed) return;
    const uintptr_t none[4] = {};
    const char* const names[4] = {};
    CrashLog::ChangeWatchArm(none, names);
    gWatchArmed = false;
}

uint32_t U32(const volatile uint8_t* base, uint32_t off) { return *reinterpret_cast<const volatile uint32_t*>(base + off); }

// Milliseconds from the performance counter: GetTickCount's 15.6 ms steps are too coarse for a 30 ms round trip.
uint32_t NowMs() {
    static LARGE_INTEGER f{};
    if (!f.QuadPart) QueryPerformanceFrequency(&f);
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    return (uint32_t)((c.QuadPart / f.QuadPart) * 1000 + (c.QuadPart % f.QuadPart) * 1000 / f.QuadPart);
}

void ResetPing() {
    gPingCount = 0;
    gEchoMs = gEchoReadAt = 0;
    gOwnDelay = -1;
    gPeerDelay = -1;
    gPeerDelayFinal = 0;
    gPeerLive = 0;
    gPeerReady = gPeerEngaged = false;
}

// Another opponent, or another match: nothing heard from this one's mod yet, and its lines may be logged again.
void ResetPeerHello() {
    gPeerHello = HelloProto::Parsed{};
    gAnyHelloRx = 0;
    gVerdict = HelloProto::Match::kUnknown;
    gPeerWifi = -1;
    gBuildLineLogged = gContentLineLogged = gSwitchLineLogged = false;
}

// A hello of any version or size from the current opponent's mod arrived in this match.
bool HeardPeer() { return gAnyHelloRx && gHelloPeer == gPeer; }

// Another build of the mod or other game files: nothing a setting changes during this match.
bool OtherBuild(HelloProto::Match m) {
    return m == HelloProto::Match::kPeerOlder || m == HelloProto::Match::kThisOlder || m == HelloProto::Match::kDifferentBuild ||
           m == HelloProto::Match::kContentDiffers;
}

// This game's identity and the comparison with the opponent's latest hello: once per tick, after the hellos were read.
// The content and DLL prints come from EnvFingerprint's background pass (started when the mod is armed); the switches
// print is computed here every time, so a hello always carries the switches of the moment it is sent and a command that
// changes one (the launcher's Safe mode checkbox) shows in the next hello and the next comparison. The session's own
// check at frame 24 still has the last word on the values the session really starts with.
void UpdateOwnId() {
    // Not while the background pass runs (Get would wait for it on the game thread). Without one (its thread could not
    // be created) the pass runs here, once.
    if (!EnvFingerprint::Ready() && EnvFingerprint::Busy()) {
        gOwnIdValid = false;
        return;
    }
    const EnvFingerprint::Print& p = EnvFingerprint::Get();
    const EnvFingerprint::Stamp s = EnvFingerprint::ReleaseStamp();
    gOwnId.stampDate = s.date;
    gOwnId.stampTime = s.time;
    gOwnId.content = p.v[EnvFingerprint::kContent];
    gOwnId.dll = p.v[EnvFingerprint::kDll];
    gOwnId.switches = p.v[EnvFingerprint::kSwitches];
    gOwnIdValid = true;
}

void UpdateVerdict() {
    UpdateOwnId();
    const HelloProto::Match was = gVerdict;
    gVerdict = HeardPeer() ? HelloProto::Compare(gOwnId, gOwnIdValid, Rollback::Lockstep(), gPeerHello) : HelloProto::Match::kUnknown;
    // 'ready' depends on it: the other game hears at the next tick, not at the next hello period.
    if (gVerdict == HelloProto::Match::kSame && was != gVerdict) gLastHelloTx = 0;
}

// The verdicts that keep MK9's netcode, each once per match (the launcher shows the three build lines).
void ReportVerdict() {
    bool* logged = nullptr;
    switch (gVerdict) {
    case HelloProto::Match::kPeerOlder:
    case HelloProto::Match::kThisOlder:
    case HelloProto::Match::kDifferentBuild:
        logged = &gBuildLineLogged;
        break;
    case HelloProto::Match::kContentDiffers:
        logged = &gContentLineLogged;
        break;
    case HelloProto::Match::kSwitchesDiffer:
        logged = &gSwitchLineLogged;
        break;
    default:  // unknown, the same, or Safe mode on one side (the engage path's own line)
        return;
    }
    if (*logged) return;
    // The line names this build too: an older opponent is known before this game's own prints are (a moment after the
    // mod was armed).
    if (!gOwnIdValid && EnvFingerprint::Busy()) return;
    char line[400];
    if (!HelloProto::Describe(gVerdict, gOwnId, gPeerHello, line, sizeof line)) return;
    *logged = true;
    Logger::Info("%s", line);
}

// This PC's default route, looked up off the game thread (GetAdaptersAddresses can take tens of milliseconds).
DWORD WINAPI WifiProbe(void*) {
    InterlockedExchange(&gLocalWifi, SteamBridge::DefaultRouteOnWifi());
    InterlockedExchange(&gWifiState, 2);
    return 0;
}

void StartWifiProbe() {  // once per match (MatchReset), and when the mod is armed
    if (gWifiState != 0) return;
    InterlockedExchange(&gWifiState, 1);
    if (HANDLE t = CreateThread(nullptr, 0, WifiProbe, nullptr, 0, nullptr)) CloseHandle(t);
    else InterlockedExchange(&gWifiState, 2);  // stays unknown
}

const char* WifiText(int wifi) { return wifi > 0 ? "on Wi-Fi" : wifi == 0 ? "not on Wi-Fi" : "unknown"; }

// The second-highest of the kept round trips: one late hello does not decide the delay. 0 without samples.
uint32_t PingEstimate(uint32_t* median) {
    const uint32_t n = gPingCount < kPingKeep ? gPingCount : kPingKeep;
    uint32_t s[kPingKeep];
    memcpy(s, gPing, n * sizeof s[0]);
    std::sort(s, s + n);
    if (median) *median = n ? s[n / 2] : 0;
    return n >= 2 ? s[n - 2] : n ? s[0] : 0;
}

// The smallest delay (frames of 1000/60 ms) the round trip allows, from 2 up to 3 (Safe mode) or from 1 up to 6
// (rollback; 1 since rc20, below).
// - Lockstep (Safe mode) waits for each input: the two one-way trips together must fit in 2 x delay frames, so 2 frames
//   up to a 56 ms round trip (10 ms kept for jitter), else 3 as before.
// - Rollback guesses late inputs and replays: it keeps a replay to about one frame, so the delay is the one-way trip in
//   frames minus one, at least 2 (delay 1 replayed 4-8 times the frames of delay 2 at 40 and 80 ms one way and crashed
//   once, results\lag_matrix*.csv): 2 frames up to a 100 ms round trip, 3 up to 133 ms. Then 4 up to 200 ms, 5 up to
//   250 ms, 6 beyond (2026-11-06, net15 on 5CD892F7, frames >= 34 ms per 33 s fight: a 133-200 ms round trip at delay
//   4 had 2-3, as smooth as 60 ms; a 300-440 ms one had 118-146 at delay 4, 28-61 at 5, 5-46 at 6, where replays near
//   GGPO's 8-frame prediction limit made frames wait; the cap was 4 until then).
// Too few samples: the fixed delays used before (Safe 3, rollback 2). margin: added to the round trip.
// Delay 1 (2026-11-08, rc19): the field test's link (hello round trip 33-50 ms, GGPO ping 33-50) ran at delay 2 all night
// and the player still felt it ("not like the new fighting games, which feel offline"; a Cyrax bomb combo timed by eye
// dropped). b34/b35 (two games on one PC): delay 1 at a 20-30 ms and a 40-60 ms simulated round trip had the hitches of
// delay 2, but on a PC slowed by 6 ms a tick the replays it adds cost ~119 hitches a minute (delay 2: 5-40). So delay 1 up
// to a 50 ms round trip, and only while this PC keeps up: gCpuFloor.
// Hellos are sent and read at a fixed point of each game's tick, so a round trip reads as whole frames (33, 50, 67 ms;
// the field log's second-highest was 49-51 in 14 of 19 matches): the limit is 3 frames plus a margin, not 50 exactly.
constexpr uint32_t kDelay1UpToMs = 55;
// The smallest rollback delay this PC runs without replay hitches: 1 until a round at delay 1 had kCpuHitchesPerK or more
// live frames >= 25 ms with a replay per 1000 frames (FramePace::TakeReplayHitches), then 2 for the rest of the game's run
// (a PC does not get faster; the next rounds and matches propose 2, and the larger proposal wins).
int gCpuFloor = 1;
constexpr uint32_t kCpuHitchesPerK = 3;
int DelayFromPing(uint32_t margin = 0) {
    const bool lockstep = Rollback::Lockstep();
    if (gPingCount < kPingMin) return lockstep ? 3 : 2;
    const uint32_t est = PingEstimate(nullptr) + margin;
    if (lockstep) return est + 10 <= 2u * 2000 / 60 ? 2 : 3;
    if (est <= kDelay1UpToMs) return gCpuFloor > 1 ? gCpuFloor : 1;
    // Half a frame above the 6, 8, 12 and 15 frames the limits were (round 3, timing audit): a hello round trip reads as
    // whole frames give or take a millisecond, so a link of exactly 8 frames read 133 or 134 ms and its delay flipped
    // between 3 and 4 from match to match (the same for 100, 200 and 250 ms). kDelay1UpToMs got the same margin in rc20.
    static constexpr uint32_t kRollbackUpTo[] = {108, 142, 208, 258};  // delays 2, 3, 4, 5
    for (int d = 2; d <= 5; ++d)
        if (est <= kRollbackUpTo[d - 2]) return d;
    return kMaxRollbackDelay;
}

// The delay the last round trips allow now (0: not adapting); below the running session's only with kDownMarginMs spare.
int LiveProposal() {
    if (!gPingDelay || gPingCount < kPingMin) return 0;
    const int up = DelayFromPing();
    return !gSessionDelay || up >= gSessionDelay ? up : std::min(gSessionDelay, DelayFromPing(kDownMarginMs));
}

int OwnDelay() { return !gPingDelay ? gDelay : gOwnDelay >= 0 ? gOwnDelay : DelayFromPing(); }
bool OwnDelayFinal() { return !gPingDelay || gOwnDelay >= 0; }
int AgreedDelay() { return gPeerDelay > OwnDelay() ? gPeerDelay : OwnDelay(); }

void FinalizeDelay(const char* when) {
    gOwnDelay = DelayFromPing();
    gLastHelloTx = 0;  // tell the other game at the next tick
    uint32_t median = 0;
    const uint32_t est = PingEstimate(&median);
    const uint32_t n = gPingCount < kPingKeep ? gPingCount : kPingKeep;
    if (gPingCount < kPingMin)
        Logger::Info("ONLINE: input delay %d proposed %s (only %u round trip(s) measured: the usual delay)", gOwnDelay, when,
                     (unsigned)gPingCount);
    else
        Logger::Info("ONLINE: round trip to the other game %u ms (second-highest of %u hellos, median %u ms; %u read late and "
                     "not counted): input delay %d proposed %s (%s)",
                     (unsigned)est, (unsigned)n, (unsigned)median, (unsigned)gLateHelloReads, gOwnDelay, when,
                     Rollback::Lockstep() ? "Safe mode" : "rollback");
}

bool FightActive() {
    const volatile uint8_t* g = gGamepads;
    return g && Readable(g, 0x240) && U32(g, 0x18) != 0;
}

uint32_t GameFrameNow() {
    const uint32_t gi = *reinterpret_cast<const volatile uint32_t*>(MK9Game::Get().gameInfoPtr);
    return gi ? *reinterpret_cast<const volatile uint32_t*>(gi + 0x70) : 0;
}

void StopLoadWait(const char* why) {
    if (!gLoadWait) return;
    gLoadWait = false;
    Logger::Info("ONLINE: the wait for the other player's load is over after %.1f s and %u ticks (%s)",
                 (GetTickCount64() - gLoadWaitSince) / 1000.0, (unsigned)gLoadWaitTicks, why);
}

// Every tick of the wait (see kLoadWaitMaxMs): ends it, or sleeps until 1/60 s after the previous tick's start.
void PaceLoadWait(bool active) {
    if (!gLoadWait) return;
    if (active) return StopLoadWait("the fight's connection started");
    if (GameState() != kLoadState) return StopLoadWait("the load state ended");
    if (Rollback::NetplayStarting() || Rollback::NetplayRunning()) return StopLoadWait("a session started");
    if (GameFrameNow() < gLoadWaitGf) return StopLoadWait("the level changed");
    if (GetTickCount64() - gLoadWaitSince > kLoadWaitMaxMs) return StopLoadWait("60 s passed");
    ++gLoadWaitTicks;
    LARGE_INTEGER f, q;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&q);
    const int64_t step = f.QuadPart / 60;
    // The first tick, or one that itself took several steps: from now, nothing is caught up.
    if (!gLoadWaitNext || q.QuadPart - gLoadWaitNext > 4 * step) gLoadWaitNext = q.QuadPart;
    gLoadWaitNext += step;
    const int64_t left = gLoadWaitNext - q.QuadPart;
    if (left <= 0) return;
    static const HANDLE timer = CreateWaitableTimerExW(nullptr, nullptr, 0x00000002 /* CREATE_WAITABLE_TIMER_HIGH_RESOLUTION */,
                                                       TIMER_MODIFY_STATE | SYNCHRONIZE);
    LARGE_INTEGER due;
    due.QuadPart = -(left * 10000000 / f.QuadPart);  // relative, 100 ns units
    if (timer && due.QuadPart < 0 && SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE))
        WaitForSingleObject(timer, 50);
    else
        Sleep((DWORD)(left * 1000 / f.QuadPart));  // before Windows 10 1803: the system timer's steps (a slower, longer wait)
}

// ---- the finisher phase (game state 0xB, GS_FATALITY; matches with blood on) ----
// After "Finish Him" the fight body stops (GameInfo+0x10 bit 0x400) and MK9's scripts load what follows, a fatality's
// cinematic or the winner's ending, through its OnlineAsyncLoad handshake on the gamepad data: natives 1174
// StartAsyncLoad (0x0087FEF0: +0x244 in progress), 1175 LocalAsyncLoadComplete (0x0087FFD0: +0x248 this game's load is
// done; on the host first a countdown of 5 exchanges, +0x258), 1176 IsAsyncLoadComplete (0x008800E0: +0x24C) and 1177
// EndAsyncLoad (0x00880140). +0x24C is set in one place, 0x00881CF0, the first call of the exchange 0x00885F80, from
// flag bit 1 of the current outgoing and incoming gamepad records; the flag is put into the outgoing record by the
// exchange's send path only (0x008858B7), and the host's countdown runs there too. With the exchange skipped, 1176
// says "not yet" on both games until the session ends.
// Two-PC evening of 2026-11-11: every match with blood on waited exactly 900 ticks (15 s) between the fight body's stop
// and the results screen, and a fatality's input left the picture frozen for exactly 1800 ticks, after which the results
// screen never ended (2026-11-03 the same): the scripts' own limits on that wait.
// So the session ends FinisherRule::kHandBack frames after the fight body stopped, and MK9's exchange runs the handshake
// and the rest of the match, as it does from the results screen on. Both games must end it at the same frame (the
// exchange resumes on both from where both left it): whether a frame started with the body stopped is noted for every
// simulated frame, live or replayed (NoteFrameStart), and only settled notes decide (FinisherRule.h, checked by the host
// test finisher_test: this phase cannot run in the harness).
constexpr uint32_t kFinisherState = 0xB, kBodyStopped = 0x400;
FinisherRule::Notes gFinisherNotes;

bool FinisherStopped() {
    const uint32_t gi = *reinterpret_cast<const volatile uint32_t*>(MK9Game::Get().gameInfoPtr);
    return gi && *reinterpret_cast<const volatile uint32_t*>(gi + 0x4) == kFinisherState &&
           (*reinterpret_cast<const volatile uint32_t*>(gi + 0x10) & kBodyStopped);
}
// The handshake's fields, for the log line of a session's end and of the results state.
int AsyncLoadText(char* out, size_t cap) {
    const volatile uint8_t* g = gGamepads;
    if (!g || !Readable(g, 0x260)) return snprintf(out, cap, "no gamepad data");
    return snprintf(out, cap, "in progress %u, this game's load done %u, both done %u, host countdown %d", (unsigned)U32(g, 0x244),
                    (unsigned)U32(g, 0x248), (unsigned)U32(g, 0x24C), (int)U32(g, 0x258));
}

// Every tick of a running session (see kLockedTicksMin).
void AdaptDelay(ULONGLONG now) {
    if (!(Input::ReadPad(0).inputLock && Input::ReadPad(1).inputLock)) {
        gLockedTicks = 0;
        gLockDone = false;
        return;
    }
    if (gLockDone || ++gLockedTicks < kLockedTicksMin) return;
    gLockDone = true;
    ++gLockStretches;
    uint32_t frames = 0;
    const uint32_t replayHitches = FramePace::TakeReplayHitches(&frames);
    if (gCpuFloor < 2 && Rollback::SessionDelay() == 1 && frames >= 600 && replayHitches * 1000 >= kCpuHitchesPerK * frames) {
        gCpuFloor = 2;
        Logger::Info("ONLINE: %u of %u live frames since the last round took >= 25 ms with a replay at delay 1: this PC proposes "
                     "delay 2 or more from now on", (unsigned)replayHitches, (unsigned)frames);
    }
    const int own = LiveProposal();
    if (!own || !gPeerLive || !gLastHelloRx || now - gLastHelloRx > kHelloFreshMs) return;
    const int agreed = std::max(own, gPeerLive);
    const int was = Rollback::SessionDelay();  // the session's own value ('sessiondelay' may have changed it)
    if (!was || agreed == was) return;
    if (!Rollback::SetSessionDelay(agreed)) return;
    gSessionDelay = agreed;
    ++gDelayChanges;
    uint32_t median = 0;
    const uint32_t est = PingEstimate(&median);
    Logger::Info("ONLINE: input delay %d -> %d between rounds (hello round trip second-highest %u ms, median %u ms; this game "
                 "proposed %d, the other %d)",
                 was, agreed, (unsigned)est, (unsigned)median, own, gPeerLive);
}

int __fastcall ExchangeHook(uint8_t* self, void*, int arg) {
    gGamepads = self;
    if (U32(self, 0x18)) ++gActiveCalls;
    if (Online::SkipNative()) {
        ++gSkipped;
        return 0;
    }
    const int r = reinterpret_cast<ExchangeFn>(gExchangeTramp)(self, arg);
    gExResults[gExPos++ % 16] = r;
    if (r == 0) {
        ++gExZero;
    } else if (r == 1) {
        ++gExOne;
        gLastExOk = GetTickCount();
    } else {
        ++gExOther;
    }
    return r;
}

int __cdecl SendHook(int type, const void* data, int size) {
    // Offline there is no online manager and the original returns 0 without sending: always let it run, so a replay
    // gets the same result as the original pass (native 1222/1223 at 0x008C0054 stores it as its script result).
    // Online the original pass sent and returned 1, which the skipped replay call returns too.
    if (Rollback::gIsResimulating && *reinterpret_cast<const volatile uint32_t*>(kOnlineManager)) {
        if (gReplaySends++ < 20) Logger::Info("ONLINE: a rollback replay tried to send online message %d (%d bytes); not sent again", type, size);
        return 1;
    }
    gSentMsgs[gSentPos++ % 8] = {type, size, GetTickCount64()};
    return reinterpret_cast<SendFn>(gSendTramp)(type, data, size);
}

uint32_t __fastcall IdleHook(void* self, void*) {
    const uint32_t ms = reinterpret_cast<IdleFn>(gIdleTramp)(self);
    // What MK9 would have seen, also when the session hides it.
    if (IdleSocketReceived(self, ms) && ms > gIdlePeak) gIdlePeak = ms;
    // The grace after a session does not need gPeer (an 'online auto' re-arm outside a fight forgets the opponent until
    // it is found again).
    if (Online::SkipNative() || GetTickCount64() < gGraceUntil) return 0;
    // After a session this timer can stay at the session's start while MK9's exchange runs again: 2026-11-03 one game read
    // 166 s at the rematch's character select (exchange results all 1) and its connection closed 34 ms after the grace
    // ran out. Once a session ran in this match, the gap counts from the exchange's last success when that is newer; a
    // peer that really stops still times out, from its last data.
    if (gSessionThisMatch && gLastExOk) {
        const DWORD sinceOk = GetTickCount() - gLastExOk;
        if (ms > sinceOk) return sinceOk;
    }
    return ms;
}

uint8_t* Detour(uintptr_t at, const uint8_t* orig, uint32_t len, void* hook, const char* name) {
    uint8_t* tramp = static_cast<uint8_t*>(CodePatch::AllocExec(len + 5));
    if (!tramp) return nullptr;
    memcpy(tramp, orig, len);  // relocation-free prologues
    tramp[len] = 0xE9;
    const int32_t back = (int32_t)((at + len) - (reinterpret_cast<uintptr_t>(tramp) + len + 5));
    memcpy(tramp + len + 1, &back, 4);
    FlushInstructionCache(GetCurrentProcess(), tramp, len + 5);
    uint8_t jmp[8] = {0xE9, 0, 0, 0, 0, 0x90, 0x90, 0x90};
    const int32_t rel = (int32_t)(reinterpret_cast<uintptr_t>(hook) - (at + 5));
    memcpy(jmp + 1, &rel, 4);
    return CodePatch::Write(at, orig, jmp, len, name) ? tramp : nullptr;
}

void LogLayout(const char* why) {
    const volatile uint8_t* g = gGamepads;
    if (!g || !Readable(g, 0x240)) return;
    const uint32_t localList = U32(g, 0x224), localCount = U32(g, 0x228), remoteList = U32(g, 0x230), remoteCount = U32(g, 0x234);
    char pads[160] = {};
    int n = 0;
    for (uint32_t i = 0; i < localCount && i < 4 && Readable(reinterpret_cast<void*>(localList), (i + 1) * 4); ++i)
        n += snprintf(pads + n, sizeof pads - n, " L%u=%08X", i, reinterpret_cast<const uint32_t*>(localList)[i]);
    for (uint32_t i = 0; i < remoteCount && i < 4 && Readable(reinterpret_cast<void*>(remoteList), (i + 1) * 4); ++i)
        n += snprintf(pads + n, sizeof pads - n, " R%u=%08X", i, reinterpret_cast<const uint32_t*>(remoteList)[i]);
    const uintptr_t conn = reinterpret_cast<uintptr_t>(g) - kConnToGamepads;
    Logger::Info("ONLINE %s: gamepads %08X (connection %08X, in Tier A: %d) active %u frame +0x20 %u +0x24 %u; pads%s; "
                 "fighter pads P1 %08X P2 %08X",
                 why, (unsigned)reinterpret_cast<uintptr_t>(g), (unsigned)conn, Snapshot::TierACovers((uint32_t)conn, 4) ? 1 : 0,
                 U32(g, 0x18), U32(g, 0x20), U32(g, 0x24), pads, Input::ReadPad(0).pad, Input::ReadPad(1).pad);
}

// The match description (GameContext, dumped by 0x0081B8A0 at 0x0093B5A8): online manager [0x00E924F0] + 0x73C;
// +0x00 Mode (None/Looking/Found/Cancel), +0x04 Type (None/Ranked/Player/Private), +0x10 MatchType (DM/KOTH).
// Rollback replaces the exchange of exactly two games: a King of the Hill room has spectators that receive the
// fighters' pads through the same exchange (they would get nothing, and the idle-time hook keeps their timeouts from
// firing), and ranked matches are never touched. Anything unreadable or unexpected: not engaged.
constexpr uint32_t kGameContext = 0x73C;
bool OneOnOneMatch(uint32_t* type, uint32_t* matchType) {
    *type = *matchType = 0xFFFFFFFF;
    const uint32_t mgr = *reinterpret_cast<const volatile uint32_t*>(kOnlineManager);
    if (!mgr || !Readable(reinterpret_cast<void*>(mgr + kGameContext), 0x18)) return false;
    const volatile uint8_t* ctx = reinterpret_cast<const volatile uint8_t*>(mgr + kGameContext);
    *type = U32(ctx, 0x04);
    *matchType = U32(ctx, 0x10);
    // Type 0 (None) too: the first real two-PC test (2026-10-29 18:55, a 1 vs 1 room joined by invite) read type 0 for
    // the whole match, so every fight was refused. Ranked (1) stays refused; King of the Hill is match type 1.
    return (*type == 0 || *type == 2 || *type == 3) && *matchType == 0;  // None (invited room), Player or Private; DM
}

// The match mode, with the fields OneOnOneMatch decides on: logged once per match in the fight's phase. Nothing is
// refused on it yet. docs\reverse_engineering.md puts MatchMode at GameContext +0x14 with the names 1V1, TAG, TAG_2P
// (0x0081A660) but no values, and its layout of the fields before it (+0x08 GameType) is not the one read here at
// runtime (+0x04 Type). The name function itself (static, 2026-11-10) returns "1V1" for 0, "TAG" for 1, "TAG_2P" for 2;
// that the dword at +0x14 is its argument is not confirmed by a real tag match, and a wrong guess would refuse every
// match. These lines from real 1 vs 1 and tag rooms settle it.
void LogMatchMode() {
    const uint32_t mgr = *reinterpret_cast<const volatile uint32_t*>(kOnlineManager);
    if (!mgr || !Readable(reinterpret_cast<void*>(mgr + kGameContext), 0x18)) {
        Logger::Info("ONLINE: match description (GameContext) not readable in this match");
        return;
    }
    const volatile uint8_t* ctx = reinterpret_cast<const volatile uint8_t*>(mgr + kGameContext);
    Logger::Info("ONLINE: match description (GameContext): match mode (+0x14) %u (the game's names: 0 1V1, 1 TAG, 2 TAG_2P; not "
                 "checked yet); mode (+0x00) %u, type (+0x04) %u, +0x08 %u, +0x0C %u, match type (+0x10) %u",
                 U32(ctx, 0x14), U32(ctx, 0x00), U32(ctx, 0x04), U32(ctx, 0x08), U32(ctx, 0x0C), U32(ctx, 0x10));
}

// This game's own conditions to engage (the engage path in Tick adds the other game's): the fight's connection phase,
// a final delay proposal, no failure this match, a one-on-one Player/Private match.
HelloProto::Local LocalState() {
    HelloProto::Local l;
    l.fightActive = FightActive();
    l.fightState = GameState() == kFightState;
    l.delayFinal = OwnDelayFinal();
    l.failedThisMatch = gFailedThisMatch;
    uint32_t type = 0, matchType = 0;
    l.oneOnOne = l.fightActive && l.fightState && OneOnOneMatch(&type, &matchType);
    return l;
}

// The hello's 'ready': those, and the other game's build is known to be this one's (gVerdict).
bool LocalReady() { return HelloProto::Ready(LocalState(), gVerdict); }

void SendHello() {
    HelloProto::Fields f;
    f.localPlayer = (uint8_t)Online::LocalPlayer();
    f.engaged = gEngaged ? 1 : 0;
    f.lockstep = Rollback::Lockstep() ? 1 : 0;
    f.delay = (uint8_t)OwnDelay();
    f.delayFinal = OwnDelayFinal() ? 1 : 0;
    f.liveDelay = (uint8_t)LiveProposal();
    f.ready = LocalReady() ? 1 : 0;
    const uint32_t now = NowMs();
    f.sentMs = now ? now : 1;  // 0 means "nothing to echo"
    if (gEchoMs && gPingPeer == gPeer) {
        f.echoMs = gEchoMs;
        f.echoHeldMs = now - gEchoReadAt;
    }
    f.identityValid = gOwnIdValid;
    f.id = gOwnId;
    f.wifi = (int)gLocalWifi;
    uint8_t pkt[HelloProto::kSize];
    const size_t size = HelloProto::Build(f, pkt, sizeof pkt);
    SteamBridge::SendRaw(gPeer, pkt, (uint32_t)size, kHelloChannel);
    gLastHelloTx = GetTickCount64();
    gLinkRelayed = SteamBridge::UsingRelay(gPeer);  // one Steam call per hello, not per tick (LinkRelayed)
}

// Hellos are read once a tick. A tick that comes more than kLateReadMs after the previous one (a stall, a stall hold, an
// X-ray lock wait, a long rollback frame, a loading screen) reads hellos that arrived some time before, unknown how long:
// a round trip through that read held the stall (2026-11-09 timing audit: two such samples in 8 s turned 50 ms into 67,
// delay 2, for the next round). Such a read takes no sample and is not echoed (the other game's sample would hold it).
constexpr uint32_t kLateReadMs = 25;
void ReadHellos() {
    char buf[256];  // a later build may send more than this one reads; Steam cuts what does not fit
    uint64_t from = 0;
    uint32_t echoMs = 0, echoHeldMs = 0;  // the newest hello's echo: older ones waited in Steam's queue meanwhile
    static uint32_t prevCall = 0;
    const uint32_t callAt = NowMs();
    const bool lateRead = prevCall && callAt - prevCall > kLateReadMs;
    prevCall = callAt;
    bool readAny = false;
    for (int i = 0; i < 16; ++i) {
        const int got = SteamBridge::ReadRaw(kHelloChannel, buf, sizeof buf, &from);
        if (got < 0) break;
        if (from != gPeer) continue;
        // Everything below is the opponent's to choose: Parse checks the lengths and brings every value into the range
        // this mod sends (a delay of 255 went through AgreedDelay into GGPO, whose input queue asserts: the game closed).
        const HelloProto::Parsed h = HelloProto::Parse(buf, (size_t)got);
        if (h.kind == HelloProto::Kind::kNone) continue;
        if (gHelloPeer != gPeer) {
            ResetPeerHello();
            gHelloPeer = gPeer;
        }
        // The opponent's mod is there, whatever it speaks: not "no mod" (a hello of another version used to be dropped
        // here with one line per process, and 5 s later the log and the status line said the opponent had no mod).
        gAnyHelloRx = GetTickCount64();
        if (h.kind == HelloProto::Kind::kMalformed) continue;
        gPeerHello = h;
        gPeerWifi = h.f.wifi;
        if (h.kind == HelloProto::Kind::kOtherVersion) continue;  // an older or newer protocol: UpdateVerdict says which
        if (gPingPeer != gPeer) {  // samples and proposals belong to one opponent
            ResetPing();
            gPingPeer = gPeer;
        }
        echoMs = h.f.echoMs;
        echoHeldMs = h.f.echoHeldMs;
        gEchoMs = lateRead ? 0 : h.f.sentMs;  // 0: the next hello carries no echo
        gEchoReadAt = NowMs();
        readAny = true;
        // Not for another build (the launcher would say rollback can take over, after the line that says it cannot).
        if (!gLastHelloRx && !OtherBuild(HelloProto::Compare(gOwnId, gOwnIdValid, Rollback::Lockstep(), h)))
            Logger::Info("ONLINE: hello from the other player's MK9Rollback (their side %u)", h.f.localPlayer);
        gLastHelloRx = GetTickCount64();
        gPeerPlayer = h.f.localPlayer;
        gPeerLockstep = h.f.lockstep;
        gPeerDelay = h.f.delay;
        gPeerDelayFinal = h.f.delayFinal;
        gPeerLive = h.f.liveDelay;
        gPeerReady = h.f.ready != 0;
        gPeerEngaged = h.f.engaged != 0;
    }
    if (readAny && lateRead) {
        ++gLateHelloReads;
        return;
    }
    if (echoMs && echoHeldMs < 10000) {
        const uint32_t rtt = NowMs() - echoMs - echoHeldMs;
        if (rtt < 2000) gPing[gPingCount++ % kPingKeep] = rtt;
    }
}

// Fallback when the connection and the online manager hold no matching SteamID: one background pass over the whole
// process's committed read-write memory (steamclient's own session table holds the remote ID), through
// ReadProcessMemory so a page freed meanwhile only fails the read. The game thread confirms the candidates.
constexpr int kWideMax = 64;
uint64_t gWide[kWideMax];
volatile LONG gWideCount = 0, gWideState = 0;  // 0 idle, 1 running, 2 done
uint32_t gNarrowMisses = 0;                      // per match

DWORD WINAPI WideScan(void* selfp) {
    const uint64_t self = *static_cast<uint64_t*>(selfp);
    delete static_cast<uint64_t*>(selfp);
    // ~850 MB read in ~2 s during the character select: below the game's threads (MK9's packet gaps of 445/799 ms on the
    // other PC while its scan ran, 2026-10-31 11:38).
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
    static uint32_t buf[0x4000];  // 64 KB
    const ULONGLONG t0 = GetTickCount64();
    uint64_t scanned = 0;
    int n = 0;
    for (uintptr_t a = 0x10000; a < 0x7FFE0000 && n < kWideMax;) {
        MEMORY_BASIC_INFORMATION mi{};
        if (!VirtualQuery(reinterpret_cast<void*>(a), &mi, sizeof mi)) break;
        const uintptr_t end = reinterpret_cast<uintptr_t>(mi.BaseAddress) + mi.RegionSize;
        const bool rw = mi.State == MEM_COMMIT && !(mi.Protect & (PAGE_GUARD | PAGE_NOACCESS)) &&
                        (mi.Protect & (PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READWRITE));
        for (uintptr_t p = a; rw && p < end && n < kWideMax; p += sizeof buf) {
            const SIZE_T want = end - p < sizeof buf ? end - p : sizeof buf;
            SIZE_T got = 0;
            if (!ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<void*>(p), buf, want, &got)) continue;
            scanned += got;
            for (SIZE_T i = 1; i < got / 4 && n < kWideMax; ++i) {
                if (buf[i] != 0x01100001u || !buf[i - 1]) continue;
                const uint64_t id = (uint64_t)0x01100001u << 32 | buf[i - 1];
                bool seen = id == self;
                for (int k = 0; k < n && !seen; ++k) seen = gWide[k] == id;
                if (!seen) gWide[n++] = id;
            }
        }
        if (end <= a) break;
        a = end;
    }
    Logger::Info("ONLINE auto: process-memory pass read %llu MB in %llu ms, %d SteamID-shaped value(s) other than this account",
                 (unsigned long long)(scanned >> 20), (unsigned long long)(GetTickCount64() - t0), n);
    InterlockedExchange(&gWideCount, n);
    InterlockedExchange(&gWideState, 2);
    return 0;
}

// 'online auto': the opponent of the game's current online connection. Read only: SteamID64-shaped dwords in the
// connection object (gamepad data - 0x4C3C, through the end of the gamepad data) and the online manager's first 8 KB,
// each confirmed with Steam's P2P session state (the game's connection runs over Steam P2P).
uint64_t FindGamePeer() {
    const volatile uint8_t* g = gGamepads;
    const uint64_t self = SteamBridge::SelfId();
    if (!g || !self) return 0;
    const struct {
        uintptr_t at;
        uint32_t size;
        const char* name;
    } ranges[2] = {{reinterpret_cast<uintptr_t>(g) - kConnToGamepads, kConnToGamepads + 0x240, "connection"},
                   {*reinterpret_cast<const volatile uint32_t*>(kOnlineManager), 0x2000, "online manager"}};
    int candidates = 0;
    uint64_t tried[8] = {};
    for (const auto& r : ranges) {
        if (!r.at || !Readable(reinterpret_cast<const void*>(r.at), r.size)) continue;
        const volatile uint8_t* p = reinterpret_cast<const volatile uint8_t*>(r.at);
        for (uint32_t off = 0; off + 8 <= r.size; off += 4) {
            if (U32(p, off + 4) != 0x01100001u || !U32(p, off)) continue;
            const uint64_t id = (uint64_t)0x01100001u << 32 | U32(p, off);
            if (id == self) continue;
            bool seen = false;
            for (int i = 0; i < candidates && i < 8; ++i) seen |= tried[i] == id;
            if (seen) continue;
            if (candidates < 8) tried[candidates] = id;
            ++candidates;
            if (SteamBridge::SessionActive(id)) {
                Logger::Info("ONLINE auto: the game's online opponent is %llu (%s +0x%X, Steam P2P session active)", (unsigned long long)id,
                             r.name, off);
                return id;
            }
        }
    }
    if (gWideState == 2) {  // the fallback pass finished: confirm its candidates (once per pass)
        const LONG wn = gWideCount;
        InterlockedExchange(&gWideState, 3);
        for (LONG i = 0; i < wn; ++i)
            if (SteamBridge::SessionActive(gWide[i])) {
                Logger::Info("ONLINE auto: the game's online opponent is %llu (process memory, %ld candidate(s), Steam P2P session active)",
                             (unsigned long long)gWide[i], wn);
                return gWide[i];
            }
        Logger::Info("ONLINE auto: the process-memory pass found %ld SteamID-shaped value(s), none with an active Steam P2P session", wn);
    } else if (gWideState == 0 && ++gNarrowMisses >= 4) {  // 2 s without a match in the connection: one wide pass
        InterlockedExchange(&gWideState, 1);
        if (HANDLE t = CreateThread(nullptr, 0, WideScan, new uint64_t(self), 0, nullptr)) CloseHandle(t);
        else InterlockedExchange(&gWideState, 3);
    }
    if (!gScanLogged) {
        gScanLogged = true;
        Logger::Info("ONLINE auto: no opponent found yet in the game's connection (%d SteamID-shaped value(s), none with an active "
                     "Steam P2P session); searching every 0.5 s while the online fight runs",
                     candidates);
    }
    return 0;
}

void DescribePeer(char* out, unsigned cap) {
    if (gPeer) SteamBridge::DescribeSession(gPeer, out, cap);
    else snprintf(out, cap, "opponent not identified");
}

const char* HelloAge(char* out, unsigned cap) {
    if (gLastHelloRx) snprintf(out, cap, "%.1f s ago", (GetTickCount64() - gLastHelloRx) / 1000.0);
    else snprintf(out, cap, "never");
    return out;
}

// When MK9's online data goes inactive (its connection was destroyed: a match's normal end, or a drop).
void LogConnectionEnd() {
    const ULONGLONG now = GetTickCount64();
    char p2p[200], age[32];
    DescribePeer(p2p, sizeof p2p);
    const char* mod = !gEngaged ? "not engaged"
                      : Rollback::NetplayRunning() ? "engaged, session running"
                      : Rollback::NetplayStarting() ? "engaged, waiting for the peer's session"
                                                    : "engaged, waiting for the fight";
    Logger::Info("ONLINE connection end: mod %s; the opponent's mod last answered %s; Steam P2P with the opponent: %s", mod,
                 HelloAge(age, sizeof age), p2p);
    char peaks[128], ex[256];
    int n = 0, m = 0;
    for (uint32_t i = 0; i < 8; ++i) Append(peaks, sizeof peaks, n, " %u", gIdlePeaks[(gIdlePeakPos + i) % 8]);
    for (uint32_t i = 0; i < 16; ++i) Append(ex, sizeof ex, m, " %d", gExResults[(gExPos + i) % 16]);
    Logger::Info("ONLINE connection end: MK9's longest packet gap per second (ms, last 8 s):%s, this second %u; last 16 "
                 "exchange results:%s (this match: 0 x%u, 1 x%u, other x%u)",
                 n ? peaks : " -", gIdlePeak, m ? ex : " -", gExZero, gExOne, gExOther);
    char sent[320];
    int k = 0;
    for (uint32_t i = 0; i < 8; ++i) {
        const SentMsg& s = gSentMsgs[(gSentPos + i) % 8];
        if (s.at) Append(sent, sizeof sent, k, " type %d (%d B) %.1f s before;", s.type, s.size, (now - s.at) / 1000.0);
    }
    Logger::Info("ONLINE connection end: MK9's last online messages sent:%s", k ? sent : " none");
}

void Disengage(const char* why) {
    if (!gEngaged) return;
    gEngaged = false;
    Rollback::NetplayDisarm();
    // A running session too ('online off', the match-end reset): GGPO would keep driving the pads while MK9's exchange
    // runs again. Stopped while the bridge still knows the Steam session is MK9's (it must not close it).
    Rollback::NetplayStop(why);
    SteamBridge::SetSharedSession(false);
    Logger::Info("ONLINE: rollback disengaged (%s); %u native exchanges were skipped, %u fight(s) re-armed", why, (unsigned)gSkipped,
                 (unsigned)gRearms);
    gRearms = 0;
    gSessionDelay = 0;
}

// The end of an online match (the connection's data inactive for 20 s, or a different connection): ready for the next
// one, which proposes its delay from its own round trips.
void MatchReset(const char* why) {
    Disengage(why);
    gFailedThisMatch = false;
    gSessionThisMatch = false;
    gLastExOk = 0;
    gMatchRefused = false;
    gModeMismatchLogged = false;
    gLastHelloRx = 0;
    gInactiveSince = 0;
    ResetPing();  // also the delay proposals (gOwnDelay, the other game's)
    ResetPeerHello();  // the next match hears, compares and reports again
    gLinkLogged = gMatchModeLogged = false;
    gLinkRelayed = false;
    InterlockedCompareExchange(&gWifiState, 0, 2);  // looked at again in the next match (a look still running finishes first)
    if (gAuto) {  // the next match may be against someone else
        gPeer = 0;
        gPeerPlayer = 0;
        gScanLogged = false;
        gNarrowMisses = 0;
        if (gWideState == 3) InterlockedExchange(&gWideState, 0);  // a running pass finishes first
    }
}
}  // namespace

namespace Online {

bool Install() {
    if (gExchangeTramp && gIdleTramp && gSendTramp) return true;
    if (!gExchangeTramp)
        gExchangeTramp = Detour(kGamepadExchange, kExchangeOrig, sizeof kExchangeOrig, reinterpret_cast<void*>(&ExchangeHook),
                                "online gamepad exchange 0x00885F80");
    if (!gIdleTramp)
        gIdleTramp = Detour(kIdleMs, kIdleOrig, sizeof kIdleOrig, reinterpret_cast<void*>(&IdleHook), "connection idle time 0x004F5E10");
    if (!gSendTramp)
        gSendTramp = Detour(kSendMessage, kSendOrig, sizeof kSendOrig, reinterpret_cast<void*>(&SendHook), "online message send 0x0081BCD0");
    if (!gExchangeTramp || !gIdleTramp || !gSendTramp)
        Logger::Info("ONLINE: hooks not installed (code not as expected); online rollback unavailable");
    return gExchangeTramp && gIdleTramp && gSendTramp;
}

void Command(uint64_t peer, int delay, bool pingDelay) {
    if (!peer) {
        DisarmConnectionWatch();
        const bool wasEngaged = gEngaged;
        Disengage("online mode off");
        gPeer = 0;
        gAuto = false;
        gLinkRelayed = false;
        // After a session MK9's idle timer would see the whole session as one gap: the usual grace (review 2026-11-01).
        gGraceUntil = wasEngaged ? GetTickCount64() + kGraceMs : 0;
        Logger::Info("ONLINE off: MK9's own netcode for every online fight");
        return;
    }
    if (!Install()) return;
    // What the hellos say about this build and this PC, each on a thread of its own and long before a fight: the game
    // files listed and the DLL read (EnvFingerprint, once per run; it used to happen on the game thread at the first
    // session's frame 24), and the default route's adapter.
    EnvFingerprint::Prepare();
    StartWifiProbe();
    gDelay = delay < 0 ? 0 : delay > HelloProto::kMaxDelay ? HelloProto::kMaxDelay : delay;
    gPingDelay = pingDelay;
    gOwnDelay = -1;  // proposed again (the round trips measured with this opponent are kept)
    gFailedThisMatch = false;
    gLastHelloRx = 0;
    // Re-armed while engaged and waiting (the launcher's checkbox): the wait for the other game's next hello starts over,
    // or the forgotten hello above would read as a silent opponent at once (HelloProto::PeerAbsent).
    if (gEngaged) gEngagedAt = GetTickCount64();
    gScanLogged = false;
    char delayText[48];
    if (pingDelay) snprintf(delayText, sizeof delayText, "delay from the ping (%s)", Rollback::Lockstep() ? "2-3" : "1-6");
    else snprintf(delayText, sizeof delayText, "delay %d", gDelay);
    if (peer == kAutoPeer) {
        // Re-armed during a match (the launcher's Safe mode checkbox): keep its opponent. The opponent search's memory
        // pass runs once per match, so a forgotten opponent would not be found again for the rematch.
        if (!gEngaged && !FightActive()) {
            gPeer = 0;
            gNarrowMisses = 0;
            if (gWideState == 3) InterlockedExchange(&gWideState, 0);  // a running pass finishes first
        }
        gAuto = true;
        Logger::Info("ONLINE armed for any opponent, %s: in one-on-one Player/Private online fights rollback replaces MK9's input "
                     "exchange when the opponent's MK9Rollback answers; anyone else keeps MK9's netcode",
                     delayText);
        return;
    }
    gAuto = false;
    // Never 'engaged' in a hello to someone this game did not engage with (their game could take it for the go-ahead).
    if (gEngaged && peer != gPeer) Disengage("armed for another player");
    gPeer = peer;
    Logger::Info("ONLINE armed for %llu, %s: rollback replaces MK9's input exchange in online fights against this player "
                 "when their MK9Rollback answers; anyone else keeps MK9's netcode",
                 (unsigned long long)peer, delayText);
}

bool Engaged() { return gEngaged; }

// Every thread this file or EnvFingerprint starts: the auto mode's memory pass, the Wi-Fi look, the build prints.
bool ScanRunning() { return gWideState == 1 || gWifiState == 1 || EnvFingerprint::Busy(); }

bool LocalOnWifi() { return gLocalWifi == 1; }
bool PeerOnWifi() { return gPeerWifi == 1 && HeardPeer(); }
bool LinkRelayed() { return gLinkRelayed; }

const char* StatusText() {  // game thread (overlay publish); null outside an online fight or while engaged
    if ((!gPeer && !gAuto) || gEngaged || !FightActive()) return nullptr;
    if (gMatchRefused) return "MK9 NETCODE  (rollback off: only 1 vs 1 Player Match / private rooms)";
    if (gFailedThisMatch) return "MK9 NETCODE  (rollback stopped for this match)";
    if (!gPeer) return "MK9 NETCODE  (looking for the opponent's mod...)";
    // "No mod" only when no hello of any version or size came from the opponent in this match.
    if (!HeardPeer()) return "MK9 NETCODE  (the opponent does not run the mod)";
    // Another build, other game files or other settings (gVerdict: this tick's, Tick runs before the overlay's publish).
    if (const char* why = HelloProto::StatusText(gVerdict)) return why;
    if (GetTickCount64() - gAnyHelloRx > kHelloFreshMs) return "MK9 NETCODE  (the opponent's mod stopped answering)";
    return "MK9 NETCODE  (both run the mod: rollback starts at the next fight)";
}

void TestScan() {  // 'onlinescan': the fallback pass on demand (its log line gives size and time); results unused
    if (gWideState == 1) return;
    const uint64_t self = SteamBridge::SelfId();
    InterlockedExchange(&gWideState, 1);
    if (HANDLE t = CreateThread(nullptr, 0, WideScan, new uint64_t(self), 0, nullptr)) CloseHandle(t);
    else InterlockedExchange(&gWideState, 0);
}

bool SkipNative() { return gEngaged && (Rollback::NetplayStarting() || Rollback::NetplayRunning()); }

void NoteFrameStart(uint32_t gameFrame) { gFinisherNotes.Note(gameFrame, FinisherStopped()); }

void OnLoadedNative() {
    const uint32_t gameFrame = GameFrameNow();
    // Only where Tick runs (the online mode is on) and MK9 is loading an online fight: offline fights call 1182 too
    // (about 300-450 frames in), with no online manager and outside the load state.
    if ((!gPeer && !gAuto) || gLoadWait) return;
    if (!*reinterpret_cast<const volatile uint32_t*>(kOnlineManager) || GameState() != kLoadState || FightActive()) return;
    if (Rollback::NetplayStarting() || Rollback::NetplayRunning()) return;
    gLoadWait = true;
    gLoadWaitSince = GetTickCount64();
    gLoadWaitGf = gameFrame;
    gLoadWaitTicks = 0;
    gLoadWaitNext = 0;
    Logger::Info("ONLINE: this game has loaded the fight (native 1182 at gf %u) and waits for the other player's: MK9's script "
                 "gives up after 2400 ticks, and the load ran at %u ticks per second (%.1f s); held to 60 per second from "
                 "here (40 s)",
                 (unsigned)gameFrame, (unsigned)gTickRate, gTickRate ? 2400.0 / gTickRate : 0.0);
}

uintptr_t ConnectionAddress(uint32_t* size) {
    const volatile uint8_t* g = gGamepads;
    *size = kConnToGamepads + 0x240;
    return g ? reinterpret_cast<uintptr_t>(g) - kConnToGamepads : 0;
}

void OnSessionStop(const char* why) {
    gFinisherNotes.Clear();
    if (!gEngaged) return;
    gGraceUntil = GetTickCount64() + kGraceMs;
    // A session ends at every timeline restart. In an online match that is the normal end of a fight: MK9's
    // StartSendingGamepadData (0x00884770, script native 1172) zeroes GameFrame, reseeds the RNG with the connection's
    // shared seed and resets the gamepad rings at the next fight, and the gamepad data stays active (+0x18) until the
    // connection is destroyed. Re-arm here, synchronously: the restart handler only waits for the next fight
    // (gNetplayAwaitFight) when netplay is armed at that moment. Without this, Tick took the stopped session for a
    // failure and left every later fight of the lobby (rematches, King of the Hill) to MK9's netcode.
    if (gPeer && why && !strcmp(why, "timeline restart")) {
        // Only while the fight's connection is still up. After the last round MK9 goes back to the character select or
        // the menus: an engagement carried there skipped the state-8 check of the next connection phase (armed through
        // a rematch's character select and load). The next fight's phase engages again on its own.
        if (!FightActive()) {
            Disengage("the online fight's connection ended");
            return;
        }
        // A mode changed during the session (the launcher's checkbox) applies now, on either side.
        if (gPeerLockstep != (Rollback::Lockstep() ? 1 : 0)) {
            Disengage("the two games no longer run the same mode (Safe/Rollback changed during the match)");
            return;
        }
        // So does a switch a command changed during the session (gVerdict: the last tick's hellos, this tick's switches
        // at the latest in the next hello): the next session's environment check would stop it in mid-fight.
        if (HelloProto::Mismatch(gVerdict)) {
            Disengage("the two games no longer run the same build and settings");
            return;
        }
        gSessionDelay = AgreedDelay();
        Rollback::GgpoSteamCommand(gPeer, 1, gSessionDelay, true);  // the side is taken from the game at the fight start
        ++gRearms;
        Logger::Info("ONLINE: the fight ended; rollback re-armed for the next fight of this match (%u so far)", (unsigned)gRearms);
    } else if (why && strcmp(why, "restart") && strcmp(why, "timeline restart")) {
        // Any other stop while engaged is a failure (the peer lost or never joined, no synchronized input in time, a busy
        // script VM, an environment or frame-0 mismatch...). Several of them leave netplay armed, so the game stayed
        // engaged and the next fight started a session the other game never joined (review 2026-11-01). MK9's netcode
        // for the rest of this match; the rematch tries again.
        gFailedThisMatch = true;
        char text[160];
        snprintf(text, sizeof text, "the rollback session stopped: %s", why);
        Disengage(text);
    }
}

int LocalPlayer() {
    const volatile uint8_t* g = gGamepads;
    if (!g || !Readable(g, 0x240)) return 0;
    const uint32_t list = U32(g, 0x224), count = U32(g, 0x228);
    if (!count || count > 4 || !Readable(reinterpret_cast<void*>(list), count * 4)) return 0;
    int side = 0;
    for (int p = 0; p < 2; ++p) {
        const uint32_t pad = Input::ReadPad(p).pad;
        for (uint32_t i = 0; pad && i < count; ++i)
            if (reinterpret_cast<const uint32_t*>(list)[i] == pad) side = side ? -1 : p + 1;
    }
    return side > 0 ? side : 0;
}

void Tick() {
    if (!gPeer && !gAuto) return;
    const ULONGLONG now = GetTickCount64();
    if (gPeer) {
        UpdateOwnId();  // ReadHellos compares a first hello with it
        ReadHellos();
        UpdateVerdict();
        ReportVerdict();
    }
    if (gPingDelay && gOwnDelay < 0 && gPingCount >= kPingFinal) FinalizeDelay("before the fight");
    const bool active = FightActive();
    ++gTicksThisSecond;
    PaceLoadWait(active);
    // For the launcher (rc25): the status line already says when the opponent has no mod; the log says it once per match,
    // after the opponent has been known for 5 s without a hello from their MK9Rollback: none of any version or size (a
    // mod of another version or build has its own line, ReportVerdict), and none earlier in this match either.
    static bool noModLogged = false;
    if (!active || !gPeer) {
        gPeerKnownAt = 0;
        if (!active) noModLogged = false;
    } else if (!gPeerKnownAt) {
        gPeerKnownAt = now;
    } else if (!noModLogged && !gEngaged && !gMatchRefused && now - gPeerKnownAt >= 5000 && !HeardPeer()) {
        noModLogged = true;
        Logger::Info("ONLINE: the opponent does not run the mod (no hello from their MK9Rollback): MK9's own netcode plays this match");
    }
    // The fight's phase of a match: the match mode beside the fields the one-on-one check reads, once.
    if (active && !gMatchModeLogged && GameState() == kFightState) {
        gMatchModeLogged = true;
        LogMatchMode();
    }
    if (now - gIdleWindowAt >= 1000) {  // MK9's packet gaps, per second
        gTickRate = (uint32_t)((uint64_t)gTicksThisSecond * 1000 / (now - gIdleWindowAt));
        gTicksThisSecond = 0;
        gIdleWindowAt = now;
        const uint32_t peak = gIdlePeak;
        gIdlePeak = 0;
        gIdlePeaks[gIdlePeakPos++ % 8] = peak;
        if (peak >= 1000 && active && gIdleLogs++ < 20)
            Logger::Info("ONLINE: MK9's connection went %u ms without a packet from the opponent", peak);
    }
    if (active != gWasActive) {
        gWasActive = active;
        if (active) {
            gInactiveSince = 0;
            gStateWaitLogged = false;
            gExZero = gExOne = gExOther = gIdleLogs = 0;
            LogLayout("online fight data active");
            StartWifiProbe();  // the first phase of a match: Wi-Fi or not, on its own thread (nothing in later phases)
            if (Lab::On()) ArmConnectionWatch();  // debug registers on every thread: lab only
            // A new connection phase: an engagement from the previous one (its session ended after its data went
            // inactive) must pass this phase's checks (state 8, fresh hello, same mode) again.
            if (gEngaged && !Rollback::NetplayStarting() && !Rollback::NetplayRunning()) {
                Disengage("a new connection phase began");
                gLastHelloRx = 0;
            }
            // Another connection object than this match's: a new match (another room or opponent), also when it came
            // within the 20 s that end a match below (a refusal or failure of the last match must not carry over).
            if (gMatchGamepads && gGamepads != gMatchGamepads) {
                Logger::Info("ONLINE: a new online connection (gamepad data %08X, the last match's %08X): a new match",
                             (unsigned)reinterpret_cast<uintptr_t>(gGamepads), (unsigned)reinterpret_cast<uintptr_t>(gMatchGamepads));
                MatchReset("a new online connection");
            }
            gMatchGamepads = gGamepads;
        } else {
            gInactiveSince = now;
            Logger::Info("ONLINE: the online fight data went inactive (%u exchanges seen, %u skipped)", (unsigned)gActiveCalls,
                         (unsigned)gSkipped);
            LogConnectionEnd();
            // Engaged before this game's session started: the character select's connection phase ended and MK9 now
            // loads the fight, which gets a phase of its own. Armed through that load, MK9 dropped the match every
            // time (2026-10-31 10:59, 11:02, 11:38: one game never reached native 1182, its "loaded" message 14, and
            // the host's PLAYER_REMOVE_REQ ended it, Disconnect(0xB) from 0x0091B980); engaged only at the fight's
            // phase (12:15), the whole match played. Stand down for the load; the hellos meet again at the fight's
            // phase within milliseconds and the session starts at its first fight frame.
            if (gEngaged && !Rollback::NetplayStarting() && !Rollback::NetplayRunning()) {
                Disengage("the character select ended before the fight; re-engaging when the fight's connection starts");
                gLastHelloRx = 0;  // a hello from this new phase
            }
        }
    }
    if (gEngaged) {
        // Before the Steam session check below, so neither waits on it (review 2026-11-01).
        // The match is over: MK9's results and rematch screens run on its own exchange (the rematch vote waits for it).
        // With the session running on, neither player's "yes" got anywhere (2026-11-01 19:08). Both games reach state 9
        // on the same frame (lockstep: 1118 ticks after the last fight frame on both PCs), and this runs before that
        // tick's frame boundary, so neither waits for an input the other will not send. Not a failure: the rematch's
        // fight engages again at its own connection phase (state 8, fresh hello).
        // The finisher phase ends it earlier (FinisherRule, above): at the frame both games agree on.
        const bool results = GameState() == kResultsState;
        if (Rollback::NetplayRunning() && (results || gFinisherNotes.HandBackAt(GameFrameNow()))) {
            uint32_t median = 0;
            const uint32_t est = PingEstimate(&median);
            char finisher[160] = "results screen";
            if (!results) {
                char load[120];
                AsyncLoadText(load, sizeof load);
                snprintf(finisher, sizeof finisher, "the fight is decided, gf %u, %u frames after the fight body stopped; MK9's load handshake: %s",
                         (unsigned)GameFrameNow(), (unsigned)FinisherRule::kHandBack - 1, load);
            }
            Logger::Info("ONLINE: the match is over (%s): the session ends and MK9's own netcode runs %sthe results and "
                         "rematch screens; the next fight engages again (hello round trip this match: median %u ms, second-highest "
                         "%u ms; delay %d at the end, %u change(s) between rounds in %u stretch(es) with both inputs locked)",
                         finisher, results ? "" : "the finisher, ", (unsigned)median, (unsigned)est, Rollback::SessionDelay(),
                         (unsigned)gDelayChanges, (unsigned)gLockStretches);
            gStateWaitLogged = true;  // (the hellos that follow arrive outside the fight's state: nothing to say about that)
            Disengage(kMatchOver);
            gGraceUntil = now + kGraceMs;  // MK9's exchange starts again on both games: its idle timer counts from now
            gLastHelloRx = 0;               // the next engagement needs a hello from after this point
            gOwnDelay = -1;                 // a rematch proposes again from the latest round trips (both games stop here)
            gPeerDelayFinal = 0;
            return;
        }
        if (!Rollback::NetplayArmed() && !Rollback::NetplayStarting() && !Rollback::NetplayRunning()) {
            gFailedThisMatch = true;  // timed out or lost the peer: MK9's netcode for the rest of this match
            Disengage("the rollback session could not continue");
        }
    }
    // The results screen: a failure of this match does not carry into the rematch (both games come here).
    if (!gEngaged && GameState() == kResultsState) {
        if (gFailedThisMatch) {
            gFailedThisMatch = false;
            Logger::Info("ONLINE: results screen: rollback stopped during this match; the next fight tries it again");
        }
        gModeMismatchLogged = false;
    }
    // The results state, once a second while it lasts (about 2 s after a normal ending). 2026-11-11: after a fatality
    // that did not play (MK9's finisher phase ran into its 1800-tick limit on both PCs) both games stayed in this state
    // for 50 s, until the players quit, and the logs said nothing about that time. What MK9's own exchange returned in
    // the second (0: no data from the other game yet; MK9 pauses the game then, its lockstep pause), the pause flag, the
    // game's flags (GameInfo+0x10) and the longest gap between the other game's packets.
    {
        static ULONGLONG since = 0, logAt = 0;
        static uint32_t lines = 0, zero0 = 0, one0 = 0, other0 = 0;
        if (GameState() == kResultsState && *reinterpret_cast<const volatile uint32_t*>(kOnlineManager)) {
            if (!since) {
                since = logAt = now;
                lines = 0;
                zero0 = gExZero, one0 = gExOne, other0 = gExOther;
            } else if (now - logAt >= 1000 && lines < 90) {
                logAt = now;
                ++lines;
                const uint32_t gi = *reinterpret_cast<const volatile uint32_t*>(MK9Game::Get().gameInfoPtr);
                Logger::Info("ONLINE results state +%u s: MK9's exchange in this second: %u without the other game's data, %u "
                             "with, %u other; pause flag %u; game flags %08X; data %s; longest packet gap %u ms",
                             (unsigned)((now - since) / 1000), (unsigned)(gExZero - zero0), (unsigned)(gExOne - one0),
                             (unsigned)(gExOther - other0), (unsigned)*reinterpret_cast<const volatile uint32_t*>(MK9Game::Get().pauseFlag),
                             gi ? (unsigned)*reinterpret_cast<const volatile uint32_t*>(gi + 0x10) : 0u, active ? "active" : "inactive",
                             (unsigned)gIdlePeaks[(gIdlePeakPos + 7) % 8]);
                zero0 = gExZero, one0 = gExOne, other0 = gExOther;
            }
        } else {
            since = 0;
        }
    }
    if (!active) {
        // The writes around the close are logged by now; the gamepad data may be freed and its memory reused.
        if (gInactiveSince && now - gInactiveSince > 3000) DisarmConnectionWatch();
        // The online match is over: ready for the next one. Long enough to outlast a quiet stretch between rounds (a
        // re-engagement after the next round's start would miss that round). Not while MK9 loads the online fight or
        // waits for the other player's load (up to 40 s now): the match is still on, and the fight's phase needs the
        // opponent it knows (in auto mode a reset here cost a new search, ~2 s, at the fight's start).
        const bool loading = gLoadWait || (GameState() == kLoadState && *reinterpret_cast<const volatile uint32_t*>(kOnlineManager));
        if (gInactiveSince && now - gInactiveSince > 20000 && !loading) {
            if (gPeer) {  // still connected to them through Steam, and still hearing their mod?
                char p2p[200], age[32];
                DescribePeer(p2p, sizeof p2p);
                Logger::Info("ONLINE: 20 s after the connection end: the opponent's mod last answered %s; Steam P2P with the opponent: %s",
                             HelloAge(age, sizeof age), p2p);
            }
            MatchReset("the online match ended");
            gMatchGamepads = nullptr;
        }
        return;
    }
    if (gAuto && !gPeer) {
        if (now - gLastScan < 500) return;
        gLastScan = now;
        if (!(gPeer = FindGamePeer())) return;
        UpdateVerdict();  // for this opponent (nothing heard yet), before the hello below says 'ready'
    }
    if (!SteamBridge::SessionActive(gPeer)) {  // the game's own connection is with someone else
        gLinkRelayed = false;
        if (gAuto && !gEngaged) gPeer = 0;     // auto: look again
        return;
    }
    // Twice a second; ten times a second while this game's delay proposal still lacks round trips. 2026-11-11 18:46: the
    // players left the character select 1 s after the opponent was found, with 3 round trips measured, and the match ran
    // at the fallback delay 2 on a 33 ms link (delay 1 in the matches before and after).
    const ULONGLONG every = gPingDelay && gOwnDelay < 0 && gPingCount < kPingFinal ? kHelloFastMs : kHelloEveryMs;
    if (now - gLastHelloTx >= every) SendHello();
    // The connection, once per match: when the opponent's hello said its side, or 5 s after the opponent was known (no
    // mod, or one that does not say). The overlay reads the same three facts every tick (LocalOnWifi, ...).
    if (!gLinkLogged && gWifiState != 1 && gPeerKnownAt) {
        const int peerWifi = HeardPeer() ? gPeerWifi : -1;
        if (peerWifi >= 0 || now - gPeerKnownAt >= 5000) {
            gLinkLogged = true;
            Logger::Info("ONLINE: connection: this PC %s, the opponent's PC %s%s; Steam P2P %s", WifiText((int)gLocalWifi),
                         WifiText(peerWifi), peerWifi >= 0 ? "" : HeardPeer() ? " (their mod does not say)" : " (no mod heard)",
                         gLinkRelayed ? "through Steam's relay" : "direct");
        }
    }
    if (gEngaged) {
        if (Rollback::NetplayRunning()) {
            if (gPingDelay) AdaptDelay(now);
        } else if (gLastHelloRx && gPeerLockstep != (Rollback::Lockstep() ? 1 : 0)) {
            // Armed (or waiting at the fight start) against a game that now runs the other mode: the session would start
            // lockstep against rollback. The engage path below reports the mismatch.
            Disengage("the other player's mode changed (Safe/Rollback)");
        } else if (HelloProto::Mismatch(gVerdict) && gVerdict != HelloProto::Match::kModeDiffers) {
            // The same for a switch a command changed after the engagement, on either side: the session's environment
            // check would stop it at frame 24, in mid-fight. (The mode: the branch above, from the next hello.)
            Disengage("the two games no longer run the same build and settings");
        } else if (HelloProto::PeerAbsent(now, gEngagedAt, gLastHelloRx, gPeerEngaged, gPeerReady, kPeerAbsentMs)) {
            // Waiting (armed, or paused at the fight start) while the other game says it is neither engaged nor about to
            // be, or says nothing at all any more (its hellos stopped, also when that was just before this game engaged:
            // the old test needed a hello from after the engagement and so never fired then). It will not join, and this
            // game skips MK9's exchange meanwhile.
            const bool saysNo = gLastHelloRx > gEngagedAt && !gPeerEngaged && !gPeerReady && now - gLastHelloRx <= kPeerAbsentMs;
            Disengage(saysNo ? "the other player's game is not starting the session"
                             : "the other player's mod stopped answering before the session started");
        }
        return;
    }
    if (gFailedThisMatch || !gLastHelloRx || now - gLastHelloRx > kHelloFreshMs) return;
    // Another build of the mod, or other game files: no session in this match, whatever else holds (ReportVerdict said
    // which, once; the status line says it too).
    if (OtherBuild(gVerdict)) return;
    // Only in the fight's own connection phase (after the stage load), never in the character select's. 2026-10-31: in
    // all four failed matches both mods met and armed during the character select (10:59, 11:02, 11:38, and 20:46 even
    // though the 12:35 change stood down again before the load), and one game's load script then stopped before natives
    // 1192/1193 and 1182 ("loaded", message 14); the host removed that player after ~12 s. In the one match that
    // played (12:15) neither armed before the fight's phase (state 8); the hellos met there within milliseconds and
    // the session started at the first fight frame, ~5 s later.
    if (const uint32_t state = GameState(); state != kFightState) {
        if (!gStateWaitLogged) {
            gStateWaitLogged = true;
            Logger::Info("ONLINE: the other player's MK9Rollback answered in game state %u (character select: 7); engaging only in "
                         "the fight's connection (state %u), after the stage load",
                         state, kFightState);
        }
        return;
    }
    // Both games must run the same netcode: a lockstep game and a rollback game in one GGPO session wait on different
    // things. The delays need not match: the session takes the larger proposal.
    // Not latched: one game latched while the other's checkbox cleared it, and that one then engaged alone (review
    // 2026-11-01). Checked every tick, logged once per match.
    if (gPeerLockstep != (Rollback::Lockstep() ? 1 : 0)) {
        if (!gModeMismatchLogged) {
            gModeMismatchLogged = true;
            Logger::Info("ONLINE: the other player runs %s, this game %s: both must pick the same mode; MK9's netcode plays until "
                         "they do",
                         gPeerLockstep == 1 ? "Safe mode" : "Rollback", Rollback::Lockstep() ? "Safe mode" : "Rollback");
        }
        return;
    }
    // The other game's build, files and switches are known and the same as this game's: not yet known in the first
    // moments (this game's prints still being computed, or the other's), other switches reported by ReportVerdict.
    if (gVerdict != HelloProto::Match::kSame) return;
    // Both proposals final, so both games take the same larger one. A game decides at the latest here, at the fight's
    // connection, and sends its hello at the next tick; the fight intro leaves seconds for that.
    if (!OwnDelayFinal()) FinalizeDelay("at the fight's connection");
    if (!gPeerDelayFinal) return;
    uint32_t type = 0, matchType = 0;
    if (!OneOnOneMatch(&type, &matchType)) {
        if (!gMatchRefused) {
            gMatchRefused = true;
            Logger::Info("ONLINE: not engaging in this match (GameContext type %u, match type %u: only one-on-one Player/Private "
                         "matches, type 0/2/3 and match type 0; ranked is type 1); MK9's netcode plays it",
                         type, matchType);
        }
        return;
    }
    // The other game must be ready too (its own checks passed): a game engaged alone skips MK9's exchange while the
    // other's MK9 waits for its pads. Its hello says so within kHelloEveryMs, or at once after its delay is final. One
    // function decides (HelloProto, tested on the host): this game's own conditions and the same build once more, the
    // other game's final delay, and its 'ready' or 'engaged'. A release without the build identity never hears 'ready'
    // or 'engaged' from this one, and this one never engages against it.
    if (!HelloProto::MayEngage(LocalState(), gVerdict, gPeerDelayFinal != 0, gPeerReady, gPeerEngaged)) return;
    gEngaged = true;
    gSessionThisMatch = true;
    gEngagedAt = now;
    SteamBridge::SetSharedSession(true);
    gSessionDelay = AgreedDelay();
    gLockedTicks = 0;
    gLockDone = false;
    gLockStretches = gDelayChanges = 0;
    FramePace::TakeReplayHitches(nullptr);  // the CPU check counts this engagement's frames only
    Rollback::GgpoSteamCommand(gPeer, 1, gSessionDelay, true);  // the side is taken from the game at the fight start
    // StartSendingGamepadData already zeroed GameFrame (the timeline restart) before the hellos met: wait for this
    // timeline's first fight frame if it has not come yet, otherwise for the next fight.
    const bool thisFight = Rollback::NetplayAwaitThisTimeline();
    LogLayout("engaging");
    Logger::Info("ONLINE: the session starts at the %s", thisFight ? "first fight frame of this fight" : "next fight start");
    if (gPeerPlayer && gPeerPlayer == Online::LocalPlayer())
        Logger::Info("ONLINE: both games report side %d; the fight start will fail the frame-0 check if they really agree", gPeerPlayer);
    Logger::Info("ONLINE: both games run MK9Rollback in a one-on-one %s match (GameContext type %u); rollback delay %d",
                 type == 3 ? "private" : type == 2 ? "player" : "invited", type, gSessionDelay);
    Logger::Info("ONLINE: input delay %d agreed (this game proposed %d, the other %d)", gSessionDelay, OwnDelay(), gPeerDelay);
    char p2p[200];
    DescribePeer(p2p, sizeof p2p);
    Logger::Info("ONLINE: Steam P2P with the opponent: %s", p2p);
    // 'engaged' goes out now, not at the next hello period (up to 500 ms): the other game may be waiting for it.
    SendHello();
}

}  // namespace Online
