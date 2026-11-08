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
#include "../rollback/RollbackManager.h"
#include "../rollback/Snapshot.h"
#include "SteamBridge.h"

namespace {
constexpr uintptr_t kGamepadExchange = 0x00885F80;  // thiscall(MKOnlineGamepadData*, connection) -> int, ret 4
const uint8_t kExchangeOrig[7] = {0x55, 0x8B, 0xEC, 0x51, 0x56, 0x8B, 0xF1};  // push ebp; mov ebp,esp; push ecx; push esi; mov esi,ecx
constexpr uintptr_t kIdleMs = 0x004F5E10;  // thiscall(socket) -> ms since the last packet (connection timeouts)
const uint8_t kIdleOrig[6] = {0x53, 0x56, 0x8B, 0xF1, 0x33, 0xDB};  // push ebx; push esi; mov esi,ecx; xor ebx,ebx
constexpr uint32_t kConnToGamepads = 0x4C3C;  // MKOnlineGamepadData inside the connection (call at 0x008868EC)
constexpr int kHelloChannel = 79;
constexpr uint32_t kHelloMagic = 0x4F39524Bu;  // "KR9O"
// 2: lockstep flag and input delay; 3: + the delay proposal's state and a ping echo; 4: + the live proposal and 'ready'
// (a game engages only when the other one is ready too).
constexpr uint32_t kHelloVersion = 4;
constexpr ULONGLONG kHelloEveryMs = 500, kHelloFreshMs = 3000, kGraceMs = 15000;
// GameInfo+0x4 after the last round: MK9's results and rematch screens (8 through the rounds; 11 was seen late in a
// fight, so only 9 ends a session).
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

struct Hello {
    uint32_t magic, version;
    uint8_t localPlayer, engaged, lockstep, delay;  // delay: this game's proposal (the session uses the larger of the two)
    uint8_t delayFinal;                             // the proposal will not change any more this match
    uint8_t liveDelay;                              // the delay the current round trips allow (0: none; between rounds)
    uint8_t ready;                                  // this game would engage now (fight's phase, delay final, not failed)
    uint8_t pad[1];
    uint32_t sentMs;                                // the sender's clock (NowMs) at the send
    uint32_t echoMs, echoHeldMs;                    // the last sentMs read from the other game, and how long it was held
};

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
uint32_t gPing[kPingKeep] = {}, gPingCount = 0;  // round trips in ms (ring; gPingCount counts every sample)
uint64_t gPingPeer = 0;                          // the opponent the samples belong to
uint32_t gEchoMs = 0, gEchoReadAt = 0;           // the other game's last hello clock and when it was read (NowMs)
int gOwnDelay = -1;                              // this game's proposal once final (-1: still measuring)
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

// The second-highest of the kept round trips: one late hello does not decide the delay. 0 without samples.
uint32_t PingEstimate(uint32_t* median) {
    const uint32_t n = gPingCount < kPingKeep ? gPingCount : kPingKeep;
    uint32_t s[kPingKeep];
    memcpy(s, gPing, n * sizeof s[0]);
    std::sort(s, s + n);
    if (median) *median = n ? s[n / 2] : 0;
    return n >= 2 ? s[n - 2] : n ? s[0] : 0;
}

// The smallest delay (frames of 1000/60 ms) the round trip allows, from 2 up to 3 (Safe mode) or 6 (rollback).
// - Lockstep (Safe mode) waits for each input: the two one-way trips together must fit in 2 x delay frames, so 2 frames
//   up to a 56 ms round trip (10 ms kept for jitter), else 3 as before.
// - Rollback guesses late inputs and replays: it keeps a replay to about one frame, so the delay is the one-way trip in
//   frames minus one, at least 2 (delay 1 replayed 4-8 times the frames of delay 2 at 40 and 80 ms one way and crashed
//   once, results\lag_matrix*.csv): 2 frames up to a 100 ms round trip, 3 up to 133 ms. Then 4 up to 200 ms, 5 up to
//   250 ms, 6 beyond (2026-11-06, net15 on 5CD892F7, frames >= 34 ms per 33 s fight: a 133-200 ms round trip at delay
//   4 had 2-3, as smooth as 60 ms; a 300-440 ms one had 118-146 at delay 4, 28-61 at 5, 5-46 at 6, where replays near
//   GGPO's 8-frame prediction limit made frames wait; the cap was 4 until then).
// Too few samples: the fixed delays used before (Safe 3, rollback 2). margin: added to the round trip.
int DelayFromPing(uint32_t margin = 0) {
    const bool lockstep = Rollback::Lockstep();
    if (gPingCount < kPingMin) return lockstep ? 3 : 2;
    const uint32_t est = PingEstimate(nullptr) + margin;
    if (lockstep) return est + 10 <= 2u * 2000 / 60 ? 2 : 3;
    static constexpr uint32_t kRollbackUpTo[] = {3 * 2000 / 60, 4 * 2000 / 60, 200, 250};  // delays 2, 3, 4, 5
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
        Logger::Info("ONLINE: round trip to the other game %u ms (second-highest of %u hellos, median %u ms): input delay %d proposed "
                     "%s (%s)",
                     (unsigned)est, (unsigned)n, (unsigned)median, gOwnDelay, when, Rollback::Lockstep() ? "Safe mode" : "rollback");
}

bool FightActive() {
    const volatile uint8_t* g = gGamepads;
    return g && Readable(g, 0x240) && U32(g, 0x18) != 0;
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
    if (ms > gIdlePeak) gIdlePeak = ms;  // what MK9 would have seen, also when the session hides it
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

// This game's own conditions to engage (the engage path in Tick adds the other game's): the fight's connection phase,
// a final delay proposal, no failure this match, a one-on-one Player/Private match.
bool LocalReady() {
    if (!FightActive() || gFailedThisMatch || GameState() != kFightState || !OwnDelayFinal()) return false;
    uint32_t type = 0, matchType = 0;
    return OneOnOneMatch(&type, &matchType);
}

void SendHello() {
    Hello h{};
    h.magic = kHelloMagic;
    h.version = kHelloVersion;
    h.localPlayer = (uint8_t)Online::LocalPlayer();
    h.engaged = (uint8_t)gEngaged;
    h.lockstep = (uint8_t)(Rollback::Lockstep() ? 1 : 0);
    h.delay = (uint8_t)OwnDelay();
    h.delayFinal = (uint8_t)OwnDelayFinal();
    h.liveDelay = (uint8_t)LiveProposal();
    h.ready = (uint8_t)LocalReady();
    const uint32_t now = NowMs();
    h.sentMs = now ? now : 1;  // 0 means "nothing to echo"
    if (gEchoMs && gPingPeer == gPeer) {
        h.echoMs = gEchoMs;
        h.echoHeldMs = now - gEchoReadAt;
    }
    SteamBridge::SendRaw(gPeer, &h, sizeof h, kHelloChannel);
    gLastHelloTx = GetTickCount64();
}

void ReadHellos() {
    char buf[64];
    uint64_t from = 0;
    uint32_t echoMs = 0, echoHeldMs = 0;  // the newest hello's echo: older ones waited in Steam's queue meanwhile
    for (int i = 0; i < 16; ++i) {
        const int got = SteamBridge::ReadRaw(kHelloChannel, buf, sizeof buf, &from);
        if (got < 0) break;
        uint32_t magic = 0, version = 0;
        if (from != gPeer || got < 8) continue;
        memcpy(&magic, buf, 4);
        memcpy(&version, buf + 4, 4);
        if (magic != kHelloMagic) continue;
        if (version != kHelloVersion || got < (int)sizeof(Hello)) {
            static int warned = 0;
            if (!warned++) Logger::Info("ONLINE: the other player's MK9Rollback speaks online version %u (this one %u); not engaging",
                                        version, kHelloVersion);
            continue;
        }
        Hello h{};
        memcpy(&h, buf, sizeof h);
        if (gPingPeer != gPeer) {  // samples and proposals belong to one opponent
            ResetPing();
            gPingPeer = gPeer;
        }
        echoMs = h.echoMs;
        echoHeldMs = h.echoHeldMs;
        gEchoMs = h.sentMs;
        gEchoReadAt = NowMs();
        if (!gLastHelloRx) Logger::Info("ONLINE: hello from the other player's MK9Rollback (their side %u)", h.localPlayer);
        gLastHelloRx = GetTickCount64();
        gPeerPlayer = h.localPlayer;
        gPeerLockstep = h.lockstep;
        gPeerDelay = h.delay;
        gPeerDelayFinal = h.delayFinal;
        gPeerLive = h.liveDelay;
        gPeerReady = h.ready != 0;
        gPeerEngaged = h.engaged != 0;
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
        // After a session MK9's idle timer would see the whole session as one gap: the usual grace (review 2026-11-01).
        gGraceUntil = wasEngaged ? GetTickCount64() + kGraceMs : 0;
        Logger::Info("ONLINE off: MK9's own netcode for every online fight");
        return;
    }
    if (!Install()) return;
    gDelay = delay < 0 ? 0 : delay > 8 ? 8 : delay;
    gPingDelay = pingDelay;
    gOwnDelay = -1;  // proposed again (the round trips measured with this opponent are kept)
    gFailedThisMatch = false;
    gLastHelloRx = 0;
    gScanLogged = false;
    char delayText[48];
    if (pingDelay) snprintf(delayText, sizeof delayText, "delay from the ping (%s)", Rollback::Lockstep() ? "2-3" : "2-6");
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
    gPeer = peer;
    Logger::Info("ONLINE armed for %llu, %s: rollback replaces MK9's input exchange in online fights against this player "
                 "when their MK9Rollback answers; anyone else keeps MK9's netcode",
                 (unsigned long long)peer, delayText);
}

bool Engaged() { return gEngaged; }

bool ScanRunning() { return gWideState == 1; }

const char* StatusText() {  // game thread (overlay publish); null outside an online fight or while engaged
    if ((!gPeer && !gAuto) || gEngaged || !FightActive()) return nullptr;
    if (gMatchRefused) return "MK9 NETCODE  (rollback off: only 1 vs 1 Player Match / private rooms)";
    if (gFailedThisMatch) return "MK9 NETCODE  (rollback stopped for this match)";
    if (!gPeer) return "MK9 NETCODE  (looking for the opponent's mod...)";
    if (!gLastHelloRx || GetTickCount64() - gLastHelloRx > kHelloFreshMs) return "MK9 NETCODE  (the opponent does not run the mod)";
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

uintptr_t ConnectionAddress(uint32_t* size) {
    const volatile uint8_t* g = gGamepads;
    *size = kConnToGamepads + 0x240;
    return g ? reinterpret_cast<uintptr_t>(g) - kConnToGamepads : 0;
}

void OnSessionStop(const char* why) {
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
    if (gPeer) ReadHellos();
    if (gPingDelay && gOwnDelay < 0 && gPingCount >= kPingFinal) FinalizeDelay("before the fight");
    const bool active = FightActive();
    if (now - gIdleWindowAt >= 1000) {  // MK9's packet gaps, per second
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
        if (Rollback::NetplayRunning() && GameState() == kResultsState) {
            uint32_t median = 0;
            const uint32_t est = PingEstimate(&median);
            Logger::Info("ONLINE: the match is over (results screen): the session ends and MK9's own netcode runs the results and "
                         "rematch screens; the next fight engages again (hello round trip this match: median %u ms, second-highest "
                         "%u ms; delay %d at the end, %u change(s) between rounds in %u stretch(es) with both inputs locked)",
                         (unsigned)median, (unsigned)est, Rollback::SessionDelay(), (unsigned)gDelayChanges, (unsigned)gLockStretches);
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
    if (!active) {
        // The writes around the close are logged by now; the gamepad data may be freed and its memory reused.
        if (gInactiveSince && now - gInactiveSince > 3000) DisarmConnectionWatch();
        // The online match is over: ready for the next one. Long enough to outlast a quiet stretch between rounds (a
        // re-engagement after the next round's start would miss that round).
        if (gInactiveSince && now - gInactiveSince > 20000) {
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
    }
    if (!SteamBridge::SessionActive(gPeer)) {  // the game's own connection is with someone else
        if (gAuto && !gEngaged) gPeer = 0;     // auto: look again
        return;
    }
    if (now - gLastHelloTx >= kHelloEveryMs) SendHello();
    if (gEngaged) {
        if (Rollback::NetplayRunning()) {
            if (gPingDelay) AdaptDelay(now);
        } else if (gLastHelloRx && gPeerLockstep != (Rollback::Lockstep() ? 1 : 0)) {
            // Armed (or waiting at the fight start) against a game that now runs the other mode: the session would start
            // lockstep against rollback. The engage path below reports the mismatch.
            Disengage("the other player's mode changed (Safe/Rollback)");
        } else if (gLastHelloRx > gEngagedAt && !gPeerEngaged && !gPeerReady && now - gEngagedAt > kPeerAbsentMs) {
            // Waiting (armed, or paused at the fight start) while the other game says it is neither engaged nor about to
            // be: it will not join, and this game skips MK9's exchange meanwhile.
            Disengage("the other player's game is not starting the session");
        }
        return;
    }
    if (gFailedThisMatch || !gLastHelloRx || now - gLastHelloRx > kHelloFreshMs) return;
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
    // other's MK9 waits for its pads. Its hello says so within kHelloEveryMs, or at once after its delay is final.
    if (!gPeerReady && !gPeerEngaged) return;
    gEngaged = true;
    gSessionThisMatch = true;
    gEngagedAt = now;
    SteamBridge::SetSharedSession(true);
    gSessionDelay = AgreedDelay();
    gLockedTicks = 0;
    gLockDone = false;
    gLockStretches = gDelayChanges = 0;
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
}

}  // namespace Online
