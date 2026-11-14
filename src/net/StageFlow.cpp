#include "StageFlow.h"

#include <windows.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "../debug/ArenaPics.h"
#include "../debug/Logger.h"
#include "../debug/Overlay.h"
#include "../debug/StageScreen.h"
#include "../game/Input.h"
#include "../game/MK9Game.h"
#include "../game/PressLatch.h"
#include "../hook/ObjectHooks.h"
#include "../rollback/RollbackManager.h"
#include "../rollback/ScriptRng.h"
#include "FlowTrace.h"
#include "OnlineMode.h"
#include "StageHold.h"
#include "StagePick.h"

namespace {
using namespace StagePick;

enum Mode { kModeOff = 0, kModeOn = 1, kModeTest = 2 };
enum class Step { kIdle, kPicking, kRoulette, kResult, kLetGo, kDecided };

constexpr uint32_t kPickMs = 20000;      // the countdown; at its end a player who has not locked gets the cursor's cell
constexpr uint32_t kGraceMs = 3000;      // after it: how long the other game's pick may still arrive
// A panel that opens by itself (below): MK9 does not wait for it. In the two-PC logs of 2026-11-11 the stage load began
// 2.1-2.7 s after the last fighter was locked and the fight 6-13 s after it, so this countdown is short, and the panel
// ends when this game has loaded the match (script native 1182), before the fight's phase and its session take the pads.
constexpr uint32_t kAutoPickMs = 5000;
constexpr uint32_t kAutoNative = 1375;    // SendGameSettings 0x008C1170: the select screen's script runs it in the frame the last fighter is locked
constexpr uint32_t kLoadedNative = 1182;  // this game has loaded the match
constexpr uint32_t kRetryMs = 30000;      // a refused opening is not tried again sooner (a native may run every tick)
// A panel that opened by itself and holds the game (StageHold.h): MK9 stays on the character select while the players
// pick. The script that waits there gives up after a count of its own (not read for this wait; 2400 ticks for the load
// wait), so the countdown stays short and the hold has an end of its own.
constexpr uint32_t kHeldPickMs = 8000;    // the countdown of a panel that holds the game ('stagetime N' changes it)
constexpr uint32_t kHeldGraceMs = 1500;   // after it: how long the other game's pick may still arrive
constexpr uint32_t kSureWaitMs = 1500;    // after the result: how long the panel waits for the other game's "I hold and have both picks"
constexpr uint32_t kHoldSlackMs = 2000;   // beyond the longest regular run (countdown, grace, roulette, result, that wait)
constexpr uint32_t kLoadWatchMs = 45000;  // "loaded" must follow a hold within this, or the hold is taken for broken
constexpr uint32_t kResultMs = 1600;     // the result stays on the panel
constexpr uint32_t kBannerMs = 4000;     // the two lines while the match loads
constexpr uint32_t kKeepMs = 60000;      // a result nobody asked for is forgotten
constexpr uint32_t kSendMs = 100;        // a packet this often while the exchange is open (32 bytes: 2.6 kbit/s)
constexpr uint32_t kMoveSendMs = 30;     // after a cursor move or the lock: the next packet this soon, so the other screen follows
constexpr uint32_t kQuietMs = 1000;      // no packet of the other panel this long: its cursor is shown as "no signal", not as live
constexpr uint32_t kSilenceMs = 3000;   // nothing heard from the other panel this long after opening: it is not there
constexpr uint32_t kFightState = 8;      // GameInfo+0x4 in an online match (7 character select, 1 stage load, 8 fight)
constexpr uint32_t kUp = 1, kDown = 2, kLeft = 4, kRight = 8, kLockButtons = 0x20 | 0x1000;
// The lock is the button MK9's own menus confirm with. Read in the game on 2026-11-12 (keyboard and scripted pad): bit
// 0x20 on a hardware pad confirms in the menus (the game's hint names it: pad A, keys G and numpad 4), the Enter key
// arrives as 0x1000, bit 0x80 is the menus' "back" (pad B, keys J and numpad 6), the space bar 0x8000.

// On from the start since 2026-11-13 (the players' wish: the screen shows by itself once both have chosen their
// fighters). It opens only in an online one-on-one match whose two mods have answered each other as the same build;
// everywhere else nothing of it runs. 'stagescreen 0' switches it off, 'stagetrigger 0' leaves it to the command.
int gMode = kModeOn;
bool gOverride = false;
uint32_t gTrigger = kAutoNative;
// This panel opened on the trigger. It came after the game's settings message (the same native sends it), so nothing
// of its draw goes into that message. Where the game's script then waits for the other game, the panel holds it there
// (gHold) and the drawn arena is written into both games through the game's own stage setter; where it does not, the
// draw is shown and logged only (the trial run of 2026-11-13 15:19) and the panel may stay through the versus and
// loading screens.
bool gAuto = false;
uint32_t gPickMs = kPickMs, gGraceMs = kGraceMs;
uint64_t gTriedAt = 0;
// The hold of such a panel, and what came of it. All on the game thread.
StageHold::Hold gHold;
uint32_t gHeldPickMs = kHeldPickMs;
bool gHoldSaid = false;        // this hold's end has its log line
uint64_t gLoadWatchUntil = 0;  // a hold ended: "loaded" (native 1182) is expected before this
uint64_t gHoldEndedAt = 0;
int gWritten = -1;             // the stage table entry written into the game for the match that loads now (-1: none)
bool gWriteTest = false;       // written by 'stagewrite' (a test of the write alone, in any game): kept until a fight begins
uint64_t gWrittenAt = 0;
uint32_t HoldMaxMs() { return gHeldPickMs + kHeldGraceMs + StageScreen::kRouletteMs + kResultMs + kSureWaitMs + kHoldSlackMs; }
// A re-fight both players chose after the last match (Refight.cpp): the next panel that opens by itself shows no grid.
// It locks the last fight's arena for this player at once and goes through the same exchange, hold and write, so the
// two games still end on one arena whatever the other one shows (a grid, should it not know of the re-fight).
constexpr uint32_t kRefightKeepMs = 300000;  // a re-fight that reached no character select within this is forgotten
constexpr uint32_t kSilentResultMs = 500;    // such a panel has no result to read: the write follows this soon
int gRefightEntry = -1;
uint64_t gRefightSetAt = 0;
bool gSilent = false;  // the open panel is that one

Step gStep = Step::kIdle;
uint64_t gStepAt = 0, gOpenedAt = 0, gSentAt = 0, gHeardAt = 0, gBannerUntil = 0, gDecidedAt = 0;
Exchange gX;
Table gTable;
uint8_t gCellChoice[StageScreen::kMaxCells];  // cell -> choice (a stage table entry, or kRandom for the last cell)
char gCellPic[StageScreen::kMaxCells][ArenaPics::kNameLen];  // cell -> its picture's name ("" for RANDOM), taken when the panel opens
int gPicsSeen = ArenaPics::kNotAsked;                        // the reading's last state that was logged
uint8_t gCursor = StageScreen::kNoCell;
StageScreen::State gView;
uint32_t gSerial = 0, gPrevMask = 0xFFFF, gPrevDir = 0, gOpenState = 0;
uint64_t gRepeatAt = 0;
int gHandsOff = 0, gLetGoTicks = 0;
bool gTheirLockLogged = false, gHeardLogged = false, gConfirmedLogged = false, gOpenedThisMatch = false, gFightChecked = false;
bool gSendSoon = false;  // this player's cursor moved or locked since the last packet
uint8_t gUse = StageScreen::kUsePending;
char gNote[72] = "";
Verdict gVerdict;  // what the panel showed (Judge without the confirmation)
volatile LONG gReceivedFlag = 0, gReceivedIndex = 0;

// The simulated opponent of 'stagescreen test': a second exchange whose packets go through Build and Parse both ways.
Exchange gBot;
uint64_t gBotMoveAt = 0, gBotLockAt = 0;
uint32_t gBotRng = 1;

uint32_t GameState() {
    const uint32_t gi = *reinterpret_cast<const volatile uint32_t*>(MK9Game::Get().gameInfoPtr);
    return gi ? *reinterpret_cast<const volatile uint32_t*>(gi + 0x4) : 0xFFFFFFFF;
}
bool OnGameThread() { return GetCurrentThreadId() == ObjectHooks::gGameThread; }

// A number the other game cannot predict and this game does not repeat: the high-resolution clock, the tick count, the
// process and a count, mixed. A coin between two friends, not a secret.
uint32_t FreshNonce() {
    static uint32_t count = 0;
    LARGE_INTEGER q;
    QueryPerformanceCounter(&q);
    uint32_t h = detail::Mix((uint32_t)q.LowPart ^ 0x51ED270Bu);
    h = detail::Mix(h + (uint32_t)q.HighPart + GetTickCount());
    h = detail::Mix(h ^ GetCurrentProcessId() ^ (++count * 0x9E3779B9u) ^ (uint32_t)reinterpret_cast<uintptr_t>(&q));
    return h ? h : 1;
}
uint32_t NextSerial() {  // larger than every earlier one, also after the game was started again (seconds since 1601)
    FILETIME ft;
    GetSystemTimeAsFileTime(&ft);
    const uint64_t t = (((uint64_t)ft.dwHighDateTime << 32) | ft.dwLowDateTime) / 10000000ull;
    const uint32_t s = (uint32_t)t;
    gSerial = (int32_t)(s - gSerial) > 0 ? s : gSerial + 1;
    return gSerial;
}

const char* ChoiceName(uint8_t choice, char* buf, size_t n) {
    if (choice == kRandom) snprintf(buf, n, "RANDOM");
    else if (choice == kNoChoice) snprintf(buf, n, "nothing");
    else {
        const char* name = ScriptRng::StageAt(choice);
        snprintf(buf, n, "entry %u %.40s", (unsigned)choice, name ? name : "?");
    }
    return buf;
}
uint8_t CellOf(uint8_t choice) {
    for (int i = 0; i < gView.cells; ++i)
        if (gCellChoice[i] == choice) return (uint8_t)i;
    return StageScreen::kNoCell;
}
uint8_t ChoiceOf(uint8_t cell) { return cell < gView.cells ? gCellChoice[cell] : kNoChoice; }

// The stage table as it is now: the pick's table (names hashed, selectable marks) and the panel's cells. False: the
// table cannot serve a panel (not readable, fewer than two stages to pick, more than the panel holds).
bool ReadTable(Table* t, StageScreen::State* view, uint8_t* cellChoice, const char** why) {
    *t = Table{};
    const uint32_t count = ScriptRng::StageCount();
    if (count < 2 || count > kMaxEntries) {
        *why = "the stage table has no usable size";
        return false;
    }
    int cells = 0;
    t->count = (uint8_t)count;
    for (uint32_t i = 0; i < count; ++i) {
        const char* name = ScriptRng::StageAt(i);
        if (!name) {
            *why = "a stage table entry cannot be read";
            return false;
        }
        t->nameHash[i] = NameHash(name);
        t->selectable[i] = ScriptRng::StageSelectable(i);
        if (!t->selectable[i]) continue;
        if (cells >= StageScreen::kMaxCells - 1) {
            *why = "more stages than the panel holds";
            return false;
        }
        StageScreen::DisplayName(name, view->name[cells], sizeof view->name[cells]);
        ArenaPics::PictureName(name, gCellPic[cells], sizeof gCellPic[cells]);
        cellChoice[cells++] = (uint8_t)i;
    }
    if (cells < 2) {
        *why = "fewer than two stages can be picked";
        return false;
    }
    snprintf(view->name[cells], sizeof view->name[cells], "RANDOM");
    gCellPic[cells][0] = 0;
    cellChoice[cells] = kRandom;
    view->randomCell = (uint8_t)cells;
    view->cells = (uint8_t)(cells + 1);
    return true;
}

void SetStep(Step s, uint64_t now) {
    gStep = s;
    gStepAt = now;
}

void PublishOff() {
    gView.phase = StageScreen::kOff;
    StageScreen::Publish(gView);
}

void PublishView(uint64_t now) {
    StageScreen::State& s = gView;
    // Picture tiles as soon as the game's pictures are read (and the list again, should the display refuse them).
    // The names were taken from the stage table when the panel opened: nothing of the game is read here.
    const bool tiles = ArenaPics::Usable();
    if (tiles != (s.tiles != 0)) {
        s.tiles = tiles ? 1 : 0;
        for (int i = 0; i < s.cells; ++i) s.pic[i] = tiles ? (uint8_t)(ArenaPics::SlotOfPicture(gCellPic[i]) + 1) : 0;
    }
    s.phase = gStep == Step::kPicking ? StageScreen::kPicking : gStep == Step::kRoulette ? StageScreen::kRoulette
              : gStep == Step::kResult ? StageScreen::kResult : StageScreen::kOff;
    if (s.phase == StageScreen::kOff && gBannerUntil) s.phase = StageScreen::kBanner;
    s.myCursor = gCursor;
    s.myLocked = gX.mine.locked ? 1 : 0;
    s.theirCursor = CellOf(gX.theirs.locked ? gX.theirs.choice : gX.theirs.cursor);
    s.peer = gX.theirs.locked ? StageScreen::kPeerLocked : !gX.theirs.nonce ? StageScreen::kPeerSilent
             : now - gHeardAt < kQuietMs ? StageScreen::kPeerChoosing : StageScreen::kPeerQuiet;
    const uint64_t end = gOpenedAt + gPickMs;
    s.secondsLeft = gStep == Step::kPicking ? (int16_t)(now < end ? (end - now + 999) / 1000 : 0) : -1;
    s.winner = gVerdict.winner == Winner::kMine ? StageScreen::kMine : gVerdict.winner == Winner::kTheirs ? StageScreen::kTheirs
               : gVerdict.winner == Winner::kBoth ? StageScreen::kBoth : StageScreen::kNoWinner;
    const Result r = Decide(gX.mine, gX.theirs);
    s.resultCell = gVerdict.winner == Winner::kNone ? StageScreen::kNoCell : CellOf(r.choice);
    s.use = gUse;
    s.test = gMode == kModeTest;
    s.seed = SharedSeed(gX.mine, gX.theirs);
    s.phaseSinceMs = gStepAt;
    snprintf(s.note, sizeof s.note, "%s", gNote);
    if (gSilent && s.phase != StageScreen::kOff && s.phase != StageScreen::kBanner) {
        // A re-fight: two lines at the foot of the screen instead of the grid; the banner that follows is the usual one.
        static StageScreen::State card;
        card = StageScreen::State{};
        card.phase = StageScreen::kCard;
        snprintf(card.name[0], sizeof card.name[0], "RE-FIGHT");
        const uint8_t cell = CellOf(gX.mine.choice);
        snprintf(card.note, sizeof card.note, "THE SAME ARENA: %.40s", cell < s.cells ? s.name[cell] : "?");
        StageScreen::Publish(card);
        return;
    }
    StageScreen::Publish(s);
}

void LetGoOfControls() {
    if (Input::PanelHold()) Input::SetPanelHold(false);
}

// The hold ends (once per hold: its log line, and the watch for the match's "loaded"). From here the game's own answer
// reaches its script again and the game goes on to load.
void HoldOver(uint64_t now, const char* why) {
    if (!gHold.on || gHoldSaid) return;
    gHold.End(now);
    gHoldSaid = true;
    gHoldEndedAt = now;
    gLoadWatchUntil = now + kLoadWatchMs;
    char theirs[64];
    if (gHold.theirsAt) snprintf(theirs, sizeof theirs, "\"over\" (0) from %u ms on", (uint32_t)(gHold.theirsAt - gHold.since));
    else snprintf(theirs, sizeof theirs, "never \"over\" (the other game had not arrived)");
    Logger::Info("STAGEHOLD over after %u ms (%s): %u questions of the script (native 1260, sync wait 2) were answered \"still waiting\" (1), %.0f a "
                 "second; the game's own answer was %s; the game goes on",
                 (uint32_t)(gHold.overAt - gHold.since), why, gHold.held,
                 gHold.overAt > gHold.since ? gHold.held * 1000.0 / (double)(gHold.overAt - gHold.since) : 0.0, theirs);
}

// The exchange is over without a stage both games use: the game's own choice stands. A banner says so when a panel
// was on the screen.
void Stop(const char* why, uint64_t now, bool banner, bool fallback) {
    if (gStep == Step::kIdle) return;
    HoldOver(now, why);
    Logger::Info(fallback ? "STAGEPICK ended: %s; the game's own arena choice stands" : "STAGEPICK closed: %s", why);
    const bool shown = gStep != Step::kDecided;
    gX.Close();
    gBot.Close();
    LetGoOfControls();
    SetStep(Step::kIdle, now);
    gVerdict = Verdict{};
    gUse = StageScreen::kUseGameChose;
    snprintf(gNote, sizeof gNote, "%s", why);
    for (char* p = gNote; *p; ++p)
        if (*p >= 'a' && *p <= 'z') *p = (char)(*p - 32);
    gBannerUntil = fallback && banner && shown ? now + kBannerMs : 0;
    PublishView(now);
}
// End: a fallback (the picks were not used). Close: the exchange has done its work, or was only a test.
void End(const char* why, uint64_t now, bool banner) { Stop(why, now, banner, true); }
void Close(const char* why, uint64_t now) { Stop(why, now, false, false); }

bool Open(const char* why, bool byTrigger = false) {
    const uint64_t now = GetTickCount64();
    const char* no = nullptr;
    int rtt = -1;
    const char* pre = nullptr;
    if (gMode == kModeOff) no = "the stage screen is off ('stagescreen 1' or 'stagescreen test')";
    else if (!OnGameThread()) no = "not on the game thread";
    else if (gStep != Step::kIdle) no = "a panel is already open";
    else if (Rollback::GgpoActive()) no = "a rollback session runs";
    // Also for the test command: on 2026-11-12 'stagescreen open' in an offline fight opened the panel and took the
    // fighter's controls.
    else if (GameState() == kFightState) no = "a fight is running";
    else if (!StageScreen::CanDraw()) no = "the display cannot draw the panel (no faces, or no frame in the last second)";
    else if (gMode == kModeOn && !Online::OneOnOne()) no = "not in a one-on-one online match";
    // (Or they did so earlier in this match and the hellos pause right now, as they do between a match's phases: the
    // other panel's own packets then show within kSilenceMs whether it is there.)
    else if (gMode == kModeOn && Online::PreFight(&rtt, &pre) != Overlay::kPreReady && !Online::SameBuildPeer())
        no = "the two mods have not answered each other as the same build (no mod, another build, or too early)";
    gView = StageScreen::State{};
    if (!no) ArenaPics::Request();  // the first panel starts the reading thread (the commands that arm the panel did already)
    if (!no) ReadTable(&gTable, &gView, gCellChoice, &no);
    if (no) {
        Logger::Info("STAGEPICK not opened (%s): %s; the game's own arena choice stands", why, no);
        return false;
    }
    gX.Open(kKindStage, gTable.Context(), FreshNonce(), NextSerial());
    gAuto = byTrigger;
    gPickMs = gAuto ? kAutoPickMs : kPickMs;
    gGraceMs = kGraceMs;
    gHold.Reset();  // it begins when the game's script asks for the wait under this panel (OnSyncPoll)
    gHoldSaid = false;
    gX.apply = gOverride && gMode == kModeOn && !gAuto;
    // A re-fight both players chose: this panel is not shown and locks the last fight's arena at once.
    gSilent = false;
    uint8_t again = kNoChoice;
    if (byTrigger && gRefightEntry >= 0) {
        const uint8_t e = (uint8_t)gRefightEntry;
        const char* name = ScriptRng::StageAt(e);
        gRefightEntry = -1;
        if (now - gRefightSetAt >= kRefightKeepMs) Logger::Info("REFIGHT the re-fight is too old to be used: the stage screen opens as after any match");
        else if (!gTable.Pickable(e) || CellOf(e) == StageScreen::kNoCell)
            Logger::Info("REFIGHT the last fight's arena (entry %u %.40s) is not one a player may pick: the stage screen opens as after any match",
                         (unsigned)e, name ? name : "?");
        else gSilent = true, again = e;
    }
    gCursor = gSilent ? CellOf(again) : gView.randomCell;  // a player who only confirms takes the "random" ticket
    gX.MoveCursor(gSilent ? again : kRandom);
    gPrevMask = 0xFFFF;  // what is held now is not a press
    gPrevDir = 0;
    gRepeatAt = 0;
    gOpenedAt = now;
    gSentAt = gHeardAt = gBannerUntil = 0;
    gTheirLockLogged = gHeardLogged = gConfirmedLogged = gFightChecked = gSendSoon = false;
    gVerdict = Verdict{};
    gUse = StageScreen::kUsePending;
    gNote[0] = 0;
    gOpenedThisMatch = true;
    gOpenState = GameState();
    if (gMode == kModeTest) {
        gBot.Open(kKindStage, gTable.Context(), FreshNonce(), NextSerial());
        gBotRng = FreshNonce();
        gBotMoveAt = now + 500;
        gBotLockAt = now + 4000;
    }
    if (gSilent) {
        // No panel, so no controls are taken: the pick is made here and now.
        gX.Lock(again);
        gSendSoon = true;
        const char* name = ScriptRng::StageAt(again);
        Logger::Info("REFIGHT the stage screen stays shut: this player's pick is the last fight's arena, entry %u %.40s", (unsigned)again,
                     name ? name : "?");
    } else {
        Input::SetPanelHold(true);
    }
    SetStep(Step::kPicking, now);
    Logger::Info("STAGEPICK opened (%s): %d stages and RANDOM, table context %08X, exchange serial %u, mode %s, override %s, game state %u, "
                 "countdown %u s%s",
                 why, gView.cells - 1, gX.context, gX.serial, gMode == kModeTest ? "test (simulated opponent)" : "online",
                 gX.apply ? "on" : "off (trial run)", GameState(), gPickMs / 1000,
                 !gAuto ? "" : gHold.wanted && !gHold.broken
                     ? "; opened by itself: the game is held on this screen once its script asks whether the other game is ready to load"
                     : "; opened by itself with the hold off: the game's own arena loads, the panel ends when the match is loaded");
    PublishView(now);
    return true;
}

void Heard(const Packet& p, const char* from) {
    const Exchange::Heard h = gX.OnPacket(p);
    if (h == Exchange::Heard::kTaken) gHeardAt = GetTickCount64();
    if (h == Exchange::Heard::kContextDiffers && !gHeardLogged) {
        gHeardLogged = true;
        Logger::Info("STAGEPICK heard %s with another stage table (context %08X, this game %08X)", from, p.context, gX.context);
    } else if (h == Exchange::Heard::kTaken && !gHeardLogged) {
        gHeardLogged = true;
        Logger::Info("STAGEPICK heard %s: its panel is open (exchange serial %u, override %s)", from, p.serial, p.apply ? "on" : "off");
    }
    if (gX.theirs.locked && !gTheirLockLogged) {
        gTheirLockLogged = true;
        char name[64];
        Logger::Info("STAGEPICK %s locked %s", from, ChoiceName(gX.theirs.choice, name, sizeof name));
    }
    if (gX.Confirmed() && !gConfirmedLogged) {
        gConfirmedLogged = true;
        Logger::Info("STAGEPICK confirmed: the other game has this game's pick, both can compute the same result");
    }
}

void BotTick(uint64_t now) {
    if (!gBot.open) return;
    const auto rnd = [] {
        gBotRng = detail::Mix(gBotRng + 0x9E3779B9u);
        return gBotRng;
    };
    const auto anyCell = [&] { return ChoiceOf((uint8_t)(rnd() % gView.cells)); };
    if (!gBot.mine.locked && now >= gBotLockAt) gBot.Lock(anyCell());
    else if (!gBot.mine.locked && now >= gBotMoveAt) {
        gBot.MoveCursor(anyCell());
        gBotMoveAt = now + 450;
    }
}

// This game's state to the other one (and, in a test, to the simulated opponent and back), through the real bytes.
// Every packet is the whole state, so one that is lost is made good by the next: at the latest 100 ms later, 30 ms
// after a cursor move or the lock.
void SendNow(uint64_t now) {
    if (now - gSentAt < (gSendSoon ? kMoveSendMs : kSendMs)) return;
    gSendSoon = false;
    gSentAt = now;
    uint8_t pkt[kSize];
    Build(gX.Next(), pkt, sizeof pkt);
    if (gMode == kModeTest) {
        Packet p;
        if (Parse(pkt, sizeof pkt, &p) == Parsed::kOk) gBot.OnPacket(p);
        BotTick(now);
        Build(gBot.Next(), pkt, sizeof pkt);
        if (Parse(pkt, sizeof pkt, &p) == Parsed::kOk) Heard(p, "the simulated opponent");
        return;
    }
    Online::SendToPeer(pkt, (uint32_t)sizeof pkt);
}

void LockMine(const char* how) {
    const uint8_t choice = ChoiceOf(gCursor);
    if (!gX.Lock(choice)) return;
    gSendSoon = true;
    char name[64];
    Logger::Info("STAGEPICK this player locked %s (%s)", ChoiceName(choice, name, sizeof name), how);
}

void ReadControls(uint64_t now) {
    // What is held at the last sample, and what any sample or the pads' reading showed since the last tick: a press that
    // was over before this tick still counts once (seen lost on 2026-11-13 while the game loaded and read nothing).
    const uint32_t m = Input::PanelMask() | Input::TakePanelPresses();
    const uint32_t pressed = m & ~gPrevMask;
    gPrevMask = m;
    if (gX.mine.locked) return;
    // A direction acts when it is pressed, and again while it is held (0.35 s, then every 0.12 s).
    const uint32_t dir = m & (kUp | kDown | kLeft | kRight);
    bool step = (pressed & (kUp | kDown | kLeft | kRight)) != 0;
    if (dir != gPrevDir || step) gRepeatAt = now + 350;
    else if (dir && now >= gRepeatAt) {
        step = true;
        gRepeatAt = now + 120;
    }
    gPrevDir = dir;
    if (step) {
        const uint32_t use = (pressed & 0xF) ? (pressed & 0xF) : dir;
        const int dx = ((use & kRight) ? 1 : 0) - ((use & kLeft) ? 1 : 0), dy = ((use & kDown) ? 1 : 0) - ((use & kUp) ? 1 : 0);
        gCursor = StageScreen::Move(gView.cells, gView.tiles != 0, gCursor, dx, dy);
        gX.MoveCursor(ChoiceOf(gCursor));
        gSendSoon = true;  // the other player sees the cursor move
    }
    if (pressed & kLockButtons) LockMine("the player's button");
}

void LogVerdict(const Verdict& v, const char* when) {
    char mine[64], theirs[64];
    Logger::Info("STAGEPICK %s: this player %s, the other %s -> %s%s%s (%s)", when, ChoiceName(gX.mine.choice, mine, sizeof mine),
                 ChoiceName(gX.theirs.choice, theirs, sizeof theirs),
                 v.outcome == Outcome::kStage ? "stage table entry " : v.outcome == Outcome::kGameChoice ? "the game's own choice" : "no agreed stage",
                 v.outcome == Outcome::kStage ? (ScriptRng::StageAt(v.entry) ? ScriptRng::StageAt(v.entry) : "?") : "",
                 v.winner == Winner::kMine ? ", this player's pick" : v.winner == Winner::kTheirs ? ", the other player's pick"
                 : v.winner == Winner::kBoth ? ", both players' pick" : "",
                 WhyText(v.why));
}

void SetBanner(uint8_t use, const char* note, uint64_t now) {
    gUse = use;
    snprintf(gNote, sizeof gNote, "%s", note);
    gBannerUntil = now + kBannerMs;
}

// The panel that held the game gives it back: is the drawn arena written? Asked once (StageHold.h says why then).
void Settle(uint64_t now) {
    if (!gHold.on) {  // never held: the trial run of 2026-11-13 15:19, the game's own arena loads
        SetBanner(gUse == StageScreen::kUseTrial ? StageScreen::kUseTrial : StageScreen::kUseGameChose,
                  gUse == StageScreen::kUseTrial ? "" : "THIS GAME DID NOT WAIT FOR THE PICKS", now);
        Logger::Info("STAGEPICK the draw is shown only: this game was not held on the select screen (its script asked for no sync wait 2 "
                     "under the panel%s), the game's own arena loads",
                     !gHold.wanted ? "; 'stagehold 0'" : gHold.broken ? "; the hold is off after a match that did not load" : "");
        return;
    }
    const Verdict v = Judge(gX, gTable, true);
    const StageHold::Use use = StageHold::Decide(gHold, gX, v);
    LogVerdict(v, "the wait ends");
    bool wrote = false;
    if (use == StageHold::Use::kWrite) {
        char was[80] = "", is[80] = "";
        const bool had = FlowTrace::FightStageName(was, sizeof was);
        // Both ways: the name now (should the script have set its arena before the wait), and every stage the script
        // sets from here until the fight (should it set it after the wait).
        wrote = ScriptRng::SetMatchStage(v.entry) && ScriptRng::WriteStage(v.entry);
        const bool has = FlowTrace::FightStageName(is, sizeof is);
        const char* name = ScriptRng::StageAt(v.entry);
        if (wrote) {
            gWritten = v.entry;
            gWrittenAt = now;
            Logger::Info("STAGEPICK wrote the agreed arena into this game: entry %u %.40s (the game held %.40s, now %.40s); the other game "
                         "said that it holds too and has both picks, and writes the same by its own draw",
                         (unsigned)v.entry, name ? name : "?", had ? was : "?", has ? is : "?");
        } else {
            ScriptRng::SetMatchStage(-1);
            Logger::Info("STAGEPICK could NOT write the agreed arena (entry %u): the game's own arena loads here", (unsigned)v.entry);
        }
    }
    if (wrote) SetBanner(StageScreen::kUseApplied, "", now);
    else {
        gX.apply = false;  // said to the other game for as long as packets go
        gSendSoon = true;
        const char* note = use == StageHold::Use::kGameChoice ? "RANDOM WON" : use == StageHold::Use::kWrite ? "THE ARENA COULD NOT BE SET"
                           : use == StageHold::Use::kPeerNotHeld ? "THE OTHER GAME DID NOT WAIT FOR THE PICKS"
                           : use == StageHold::Use::kNotHeld ? "THE WAIT ENDED BEFORE THE DRAW" : "THE OTHER GAME DID NOT CONFIRM THE DRAW";  // kNoStage, kPeerNotSure
        SetBanner(StageScreen::kUseGameChose, note, now);
        Logger::Info("STAGEPICK the agreed arena is not written (%s): the game's own arena loads", StageHold::UseText(use));
    }
    HoldOver(now, wrote ? "the arena is written" : StageHold::UseText(use));
}

// The panel holds the game, the other game says it holds too and a stage is drawn, but the other game's "I know you
// have both picks" has not arrived yet: the panel waits a little longer for it before it asks (Settle).
bool AwaitsPeer() {
    return gAuto && gHold.Holding() && gX.apply && gX.theirsApply && gVerdict.outcome == Outcome::kStage && !(gX.Confirmed() && gX.theirsSure);
}

// The settings message came in (noted by FlowTrace's tap on whichever thread; looked at here on the game thread).
void TakeReceived(uint64_t now) {
    if (!InterlockedExchange(&gReceivedFlag, 0)) return;
    const uint32_t index = (uint32_t)gReceivedIndex;
    if (gStep == Step::kIdle || !gX.open) return;
    if (gAuto) {  // sent by the native that opened the panel: no pick was in
        const char* got = ScriptRng::StageAt(index);
        Logger::Info("STAGEPICK the settings message arrived with index %u (%.40s) under the panel that opened by itself: the picks come "
                     "after it, the mod changes nothing of it",
                     index, got ? got : "no such entry");
        return;
    }
    const Verdict v = Judge(gX, gTable, false);
    const Check c = CheckReceived(v, index);
    const char* name = ScriptRng::StageAt(index);
    Logger::Info("STAGEPICK the settings message arrived with index %u (%.40s); this game %s; the sender's override is %s", index,
                 name ? name : "no such entry",
                 c == Check::kSame ? "computed the same stage" : c == Check::kDiffers ? "computed ANOTHER stage: the message's index is what loads"
                 : "expected nothing (no agreed stage, or \"random\" won)",
                 gX.theirsApply ? "on" : "off");
    LogVerdict(v, "at the received settings message");
    if (!gX.theirsApply) SetBanner(StageScreen::kUseTrial, "", now);
    else if (c == Check::kSame) SetBanner(StageScreen::kUseApplied, "", now);
    else if (v.outcome == Outcome::kGameChoice) SetBanner(StageScreen::kUseGameChose, "RANDOM WON", now);
    else SetBanner(StageScreen::kUseGameChose, c == Check::kDiffers ? "THE TWO GAMES DID NOT AGREE ON THE DRAW" : "", now);
}

// The fight begins: the stage the game really loaded, against what the panel drew. The one check that does not
// depend on how the stage travels between the two games.
void CheckFight() {
    if (gFightChecked || gVerdict.winner == Winner::kNone) return;
    gFightChecked = true;
    char stage[80] = "";
    const bool read = FlowTrace::FightStageName(stage, sizeof stage);
    const char* drawn = gVerdict.outcome == Outcome::kStage ? ScriptRng::StageAt(gVerdict.entry) : nullptr;
    Logger::Info("STAGEPICK the fight's stage is %s; the draw was %s%s: %s", read ? stage : "not readable",
                 gVerdict.outcome == Outcome::kStage ? "" : "the game's own choice", drawn ? drawn : "",
                 !read || !drawn ? "nothing to compare" : ScriptRng::StageIndex(stage) == (int)gVerdict.entry ? "the SAME stage" : "ANOTHER stage");
}
}  // namespace

namespace StageFlow {

void Tick() {
    if (!OnGameThread()) return;
    const uint64_t now = GetTickCount64();
    const uint32_t state = GameState();
    if (const int pics = ArenaPics::State(); pics != gPicsSeen && pics != ArenaPics::kLoading) {  // the reading thread has ended: its one log line
        gPicsSeen = pics;
        char line[720];
        ArenaPics::Describe(line, sizeof line);
        Logger::Info("ARENAPICS: %s", line);
    }
    if (state == kFightState) {
        CheckFight();
        gOpenedThisMatch = false;
    }
    TakeReceived(now);
    // An arena this mod wrote: checked against the stage the fight starts on, and from then on (or when the match is
    // gone, or after a minute and a half) the game's scripts set their stages themselves again.
    if (gWritten >= 0 && (state == kFightState || (!gWriteTest && !Online::OneOnOne()) || now - gWrittenAt >= (gWriteTest ? 600000u : 90000u))) {
        gWriteTest = false;
        char stage[80] = "";
        const bool read = state == kFightState && FlowTrace::FightStageName(stage, sizeof stage);
        const char* name = ScriptRng::StageAt((uint32_t)gWritten);
        Logger::Info("STAGEPICK the written arena was entry %d %.40s; %s%.40s%s", gWritten, name ? name : "?",
                     state != kFightState ? "no fight followed" : read ? "the fight's stage is " : "the fight's stage is not readable", read ? stage : "",
                     !read ? "" : ScriptRng::StageIndex(stage) == gWritten ? ": the SAME stage" : ": ANOTHER stage");
        ScriptRng::SetMatchStage(-1);
        gWritten = -1;
    }
    // A hold ended and no "loaded" followed: the match most likely broke on it. No second match is risked on it.
    if (gLoadWatchUntil && (now >= gLoadWatchUntil || !Online::OneOnOne())) {
        Logger::Info("STAGEHOLD: no loaded match followed the hold within %u ms (%s): the hold stays off until the game is started again "
                     "or 'stagehold 1'; the panel still opens, as a trial run",
                     (uint32_t)(now - gHoldEndedAt), now >= gLoadWatchUntil ? "time" : "the online match ended");
        gLoadWatchUntil = 0;
        gHold.broken = true;
    }
    // The hold's own end (StageHold.h): the panel is taken off and the game's own arena loads.
    if (gHold.Holding() && now - gHold.since >= HoldMaxMs()) return End("the wait for the picks took too long", now, true);
    if (gStep == Step::kIdle) {
        if (gBannerUntil && now >= gBannerUntil) {
            gBannerUntil = 0;
            PublishOff();
        }
        return;
    }
    const bool panel = gStep != Step::kDecided;  // the panel is on the screen (a result that is kept is not)
    if (Rollback::GgpoActive()) return End("a rollback session started", now, false);
    // The panel belongs to the screens before a fight (in every mode: it must never hold a fighter's controls); a result
    // waits through the loading screen for the message.
    if (state == kFightState) return panel ? End("the fight began", now, false) : Close("the fight began", now);
    if (gMode == kModeOn) {
        // The game's screen changed under an open panel: it went on without the picks.
        // (A panel that opened by itself stays through the versus and loading screens: OnNative ends it at "loaded".)
        if (panel && !gAuto && state != gOpenState) return End("the game went on to another screen", now, true);
        if (!Online::OneOnOne()) return panel ? End("the online match ended", now, false) : Close("the online match ended", now);
        if (gStep == Step::kPicking && !gX.theirs.nonce && !gX.contextDiffers && now >= gOpenedAt + kSilenceMs)
            return End("the opponent's mod did not answer", now, true);
    }
    switch (gStep) {
    case Step::kPicking: {
        ReadControls(now);
        if (!gX.mine.locked && now >= gOpenedAt + gPickMs) LockMine("the countdown ended");
        SendNow(now);
        if (gX.contextDiffers) return End("the two games list different stages", now, true);
        if (gX.Ready()) {
            gVerdict = Judge(gX, gTable, false);
            LogVerdict(gVerdict, "both picks are in");
            if (gVerdict.winner == Winner::kNone) return End(WhyText(gVerdict.why), now, true);
            if (gMode == kModeTest || (!gX.apply && !gX.theirsApply)) gUse = StageScreen::kUseTrial;  // neither game will write it
            SetStep(gVerdict.winner == Winner::kBoth ? Step::kResult : Step::kRoulette, now);
        } else if (now >= gOpenedAt + gPickMs + gGraceMs) {
            return End(gX.theirs.nonce ? "the opponent did not pick in time" : "the opponent's mod did not answer", now, true);
        }
        break;
    }
    case Step::kRoulette:
        gPrevMask = Input::PanelMask();
        SendNow(now);
        if (now - gStepAt >= StageScreen::kRouletteMs) SetStep(Step::kResult, now);
        break;
    case Step::kResult:
        gPrevMask = Input::PanelMask();
        SendNow(now);
        if (now - gStepAt >= (gSilent && gVerdict.winner == Winner::kBoth ? kSilentResultMs : kResultMs)) {
            gHandsOff = gLetGoTicks = 0;
            SetStep(Step::kLetGo, now);
        }
        break;
    case Step::kLetGo:
        // The game gets the controls back once nothing is held (a button still down would be a press on its screen),
        // or after a second.
        SendNow(now);
        gHandsOff = Input::PanelMask() ? 0 : gHandsOff + 1;
        if ((gHandsOff >= 3 || ++gLetGoTicks >= 60) && (!AwaitsPeer() || now - gStepAt >= kSureWaitMs)) {
            LetGoOfControls();
            gDecidedAt = now;
            SetStep(Step::kDecided, now);
            if (gMode == kModeTest) SetBanner(StageScreen::kUseTrial, "", now);
            else if (gAuto) Settle(now);
        }
        break;
    case Step::kDecided:
        // The exchange stays open, so the other game can still confirm and the settings message can be judged.
        SendNow(now);
        if (gMode == kModeTest && (!gBannerUntil || now >= gBannerUntil)) return Close("the test run is over", now);
        if (now - gDecidedAt >= kKeepMs) return Close("no settings message within a minute of the draw", now);
        break;
    case Step::kIdle:
        break;
    }
    if (gBannerUntil && now >= gBannerUntil && gStep == Step::kDecided && gMode != kModeTest) gBannerUntil = 0;
    PublishView(now);
}

bool Idle() { return gStep == Step::kIdle && !gBannerUntil; }

void SetRefight(int entry) {
    if (!OnGameThread()) return;
    gRefightEntry = entry >= 0 && entry < (int)kMaxEntries ? entry : -1;
    gRefightSetAt = GetTickCount64();
}

void OnPacket(const void* data, int size) {
    if (size <= 0 || !OnGameThread()) return;
    Packet p;
    const Parsed r = Parse(data, (size_t)size, &p);
    static bool otherLogged = false;
    if (r == Parsed::kOtherVersion && !otherLogged) {
        otherLogged = true;
        Logger::Info("STAGEPICK the other mod sends a pick packet of another version: not used");
    }
    if (r != Parsed::kOk || gMode != kModeOn || gStep == Step::kIdle) return;
    Heard(p, "the other game");
}

void OnNative(uint32_t id) {
    if (gMode != kModeOn) return;
    // An online select screen begins (1261, 455 and 453 run in its first frame): the game's arena pictures are read now,
    // on a thread of their own, so a panel that opens by itself has them.
    if ((id == 1261 || id == 455 || id == 453) && gTrigger && ArenaPics::State() == ArenaPics::kNotAsked && Online::OneOnOne()) ArenaPics::Request();
    // This game has loaded the match: the panel that stayed through the loading screen gives the controls back here,
    // before the fight's phase and its session.
    if (id == kLoadedNative && gLoadWatchUntil) {
        Logger::Info("STAGEHOLD: this game loaded the match %u ms after the hold ended", (uint32_t)(GetTickCount64() - gHoldEndedAt));
        gLoadWatchUntil = 0;
    }
    if (id == kLoadedNative && gAuto && gStep != Step::kIdle) {
        const uint64_t at = GetTickCount64();
        if (gStep == Step::kDecided) Close("the match is loaded", at);
        else End("the match was loaded before the draw", at, false);
        return;
    }
    if (!gTrigger || id != gTrigger || gStep != Step::kIdle || gOpenedThisMatch) return;
    const uint64_t now = GetTickCount64();
    if (gTriedAt && now - gTriedAt < kRetryMs) return;
    gTriedAt = now;
    char why[40];
    snprintf(why, sizeof why, "script native %u", id);
    Open(why, true);
}

bool OnSyncPoll(uint32_t type, uint32_t gameAnswer) {
    if (gMode != kModeOn || !gAuto || gStep == Step::kIdle || !OnGameThread()) return false;
    const bool waits = gStep == Step::kPicking || gStep == Step::kRoulette || gStep == Step::kResult || gStep == Step::kLetGo;
    const bool was = gHold.on;
    const uint64_t now = GetTickCount64();
    const bool hold = gHold.OnPoll(type, gameAnswer, waits, now, HoldMaxMs());
    if (gHold.on && !was) {
        // The game waits for this panel now: the players get the longer countdown, and the other game is told that this
        // one holds and will write an arena both have agreed on.
        gPickMs = gHeldPickMs;
        gGraceMs = kHeldGraceMs;
        gX.apply = true;
        gSendSoon = true;
        ScriptRng::InstallStageSites();
        Logger::Info("STAGEHOLD began %u ms after the panel opened: the game's script asks whether sync wait 2 (the other game is ready to "
                     "load) still runs, the game's own answer is %u (0 over, 1 waiting), and it reads 1 until the panel gives the game "
                     "back; countdown %u s, the hold ends by itself after %u ms or %u answers",
                     (uint32_t)(now - gOpenedAt), gameAnswer, gPickMs / 1000, HoldMaxMs(), StageHold::kMaxAnswers);
    }
    if (!hold && gHold.on && gHold.over && !gHoldSaid) {  // its own limit, reached between two ticks
        gX.apply = false;
        gSendSoon = true;
        HoldOver(now, "the hold's own limit");
    }
    return hold;
}

bool OnSettingsSend(uint8_t* bytes16) {
    if (!OnGameThread() || gStep == Step::kIdle || !gX.open || gMode != kModeOn) return false;
    if (gAuto) {  // the native that opened the panel is the one that sends this message: no pick is in
        Logger::Info("STAGEPICK the settings message goes out with the game's index %u as the panel opens by itself: the picks come after "
                     "it, the mod changes nothing of it",
                     SettingsIndex(bytes16));
        return false;
    }
    const uint64_t now = GetTickCount64();
    const Verdict v = Judge(gX, gTable, true);
    LogVerdict(v, "this game builds the settings message");
    const uint32_t before = SettingsIndex(bytes16);
    if (!gX.apply) {
        Logger::Info("STAGEPICK the settings message keeps the game's index %u: the override is off (trial run)", before);
        SetBanner(StageScreen::kUseTrial, "", now);
        return false;
    }
    if (v.outcome == Outcome::kGameChoice) {
        Logger::Info("STAGEPICK the settings message keeps the game's index %u: \"random\" won", before);
        SetBanner(StageScreen::kUseGameChose, "RANDOM WON", now);
        return false;
    }
    if (!ApplyToSettings(v, bytes16)) {
        Logger::Info("STAGEPICK the settings message keeps the game's index %u: %s", before, WhyText(v.why));
        char note[72];
        snprintf(note, sizeof note, "%s", WhyText(v.why));
        SetBanner(StageScreen::kUseGameChose, note, now);
        for (char* p = gNote; *p; ++p)
            if (*p >= 'a' && *p <= 'z') *p = (char)(*p - 32);
        return false;
    }
    Logger::Info("STAGEPICK the settings message's index %u -> %u (the agreed stage)", before, (unsigned)v.entry);
    SetBanner(StageScreen::kUseApplied, "", now);
    return true;
}

void NoteSettingsReceived(uint32_t index) {
    InterlockedExchange(&gReceivedIndex, (LONG)index);
    InterlockedExchange(&gReceivedFlag, 1);
}

void Shutdown() {
    LetGoOfControls();
    gStep = Step::kIdle;
    gMode = kModeOff;
    StageScreen::SetDemo(0);
    StageScreen::State off;
    StageScreen::Publish(off);
}

bool Command(const char* line) {
    char verb[16] = "", arg[16] = "";
    const int n = sscanf(line, "%15s %15s", verb, arg);
    if (n < 1 || strncmp(verb, "stage", 5) || !strcmp(verb, "stages")) return false;
    const uint64_t now = GetTickCount64();
    if (!strcmp(verb, "stagescreen") && n == 2) {
        if (!strcmp(arg, "open")) {
            Open("command");
        } else if (!strcmp(arg, "close")) {
            Close("closed by command", now);
        } else {
            const int mode = !strcmp(arg, "test") ? kModeTest : atoi(arg) == 1 ? kModeOn : kModeOff;
            if (mode != gMode && gStep != Step::kIdle) End("the mode changed", now, false);
            gMode = mode;
            if (mode != kModeOff) Overlay::EnsureInstalled();  // the drawing patch, should a restore have taken it out
            if (mode != kModeOff) ArenaPics::Request();        // the game's arena pictures, read on a thread of their own
            Logger::Info("STAGESCREEN %s", mode == kModeTest ? "test: a simulated opponent ('stagescreen open' shows the panel)"
                                           : mode == kModeOn ? "on: the panel may open in an online one-on-one match between the same builds"
                                                             : "off");
        }
    } else if (!strcmp(verb, "stageoverride") && n == 2) {
        gOverride = atoi(arg) == 1;
        Logger::Info("STAGEOVERRIDE %d (%s)", gOverride ? 1 : 0,
                     gOverride ? "an agreed stage is written into the settings message this game sends"
                               : "trial run: the draw is shown, the game's own arena choice stays");
    } else if (!strcmp(verb, "stagetrigger") && n == 2) {
        gTrigger = (uint32_t)strtoul(arg, nullptr, 10);
        gTriedAt = 0;
        Logger::Info("STAGETRIGGER %u (%s)", gTrigger,
                     gTrigger ? "the panel opens when this online script native runs; the default 1375 runs in the frame the last fighter is locked"
                              : "none: the panel opens by command only");
    } else if (!strcmp(verb, "stagehold") && n == 2) {
        gHold.wanted = atoi(arg) != 0;
        if (gHold.wanted) gHold.broken = false;
        else if (gHold.Holding()) End("the hold was switched off", now, true);
        Logger::Info("STAGEHOLD %d (%s)", gHold.wanted ? 1 : 0,
                     gHold.wanted ? "a panel that opens by itself keeps the game on the character select until the draw, and both games then "
                                    "load the drawn arena"
                                  : "the panel opens as a trial run: the game goes on under it and loads its own arena");
    } else if (!strcmp(verb, "stagewrite") && n == 2) {
        // A test of the arena write alone, in any game (offline too): the two calls Settle makes when both games agreed,
        // from the same place in the tick. The next fight's line says whether its stage is the written one.
        const int entry = atoi(arg);
        char was[80] = "", is[80] = "";
        const bool had = FlowTrace::FightStageName(was, sizeof was);
        const bool wrote = entry >= 0 && OnGameThread() && ScriptRng::SetMatchStage(entry) && ScriptRng::WriteStage((uint32_t)entry);
        const bool has = FlowTrace::FightStageName(is, sizeof is);
        if (!wrote) ScriptRng::SetMatchStage(-1);
        gWritten = wrote ? entry : -1;
        gWrittenAt = now;
        gWriteTest = wrote;
        const char* name = entry >= 0 ? ScriptRng::StageAt((uint32_t)entry) : nullptr;
        Logger::Info("STAGEWRITE %d (test): %s; entry name %.40s; the game held %.40s, now holds %.40s; game state %u", entry,
                     wrote ? "the game's stage setter was called and every later stage a script sets becomes this entry, until a fight begins"
                           : entry < 0 ? "off: the scripts set their stages themselves" : "NOT written (no such entry, or no fight record)",
                     name ? name : "-", had ? was : "?", has ? is : "?", GameState());
    } else if (!strcmp(verb, "stagetime") && n == 2) {
        const int s = atoi(arg);
        gHeldPickMs = (uint32_t)(s < 3 ? 3 : s > 15 ? 15 : s) * 1000;
        Logger::Info("STAGETIME %u (the countdown, in seconds, of a panel that holds the game; 3 to 15, 8 at the start; with it the hold "
                     "lasts %u ms at the most)",
                     gHeldPickMs / 1000, HoldMaxMs());
    } else if (!strcmp(verb, "stagedemo") && n == 2) {
        StageScreen::SetDemo(atoi(arg));
        if (StageScreen::DemoShown() > 0) Overlay::EnsureInstalled();
        if (StageScreen::DemoShown() > 0) ArenaPics::Request();
        Logger::Info("STAGEDEMO %d (0: off, the published state is shown)", StageScreen::DemoShown());
    } else if (!strcmp(verb, "stagepics") && n == 2) {
        ArenaPics::SetWanted(atoi(arg) != 0);
        char line[720];
        ArenaPics::Describe(line, sizeof line);
        Logger::Info("STAGEPICS %d (%s): %s", atoi(arg) != 0 ? 1 : 0, atoi(arg) != 0 ? "picture tiles where the game's pictures could be read" : "the list of names", line);
    } else if (!strcmp(verb, "stagelook") && n == 2) {
        ArenaPics::SetLookWanted(atoi(arg) != 0);
        Logger::Info("STAGELOOK %d (%s)", atoi(arg) != 0 ? 1 : 0,
                     atoi(arg) != 0 ? "MK9's own lettering and frame pieces where they could be read from the game" : "the plain picture tiles");
    } else if (!strcmp(verb, "stagelatch") && n == 2) {
        PressLatch::SetWanted(atoi(arg) != 0);
        Logger::Info("STAGELATCH %d (%s; it takes effect when a panel next takes the controls)", atoi(arg) != 0 ? 1 : 0,
                     atoi(arg) != 0 ? "a press that falls between two samples of the controls reaches the panel"
                                    : "the panel reads MK9's samples alone, as before 2026-11-13");
    } else if (!strcmp(verb, "stagestatus")) {
        char pics[720], look[220];
        ArenaPics::Describe(pics, sizeof pics);
        StageSkin::Describe(look, sizeof look);  // written by the rendering thread: read here for the log only
        look[sizeof look - 1] = 0;
        Logger::Info("STAGESTATUS pictures: %s; the panel shows %s; last lettering: %s; the texture was made %d times", pics,
                     !ArenaPics::Usable() ? "the list of names" : ArenaPics::ArtOf() ? "picture tiles in MK9's look" : "plain picture tiles", look,
                     Overlay::SheetsMade());
        static const char* const kSteps[] = {"idle", "picking", "roulette", "result", "letting go of the controls", "decided"};
        Logger::Info("STAGESTATUS mode %s, override %d, trigger %u, step %s%s, the panel %s be drawn, controls %s, cursor cell %u, mask %04X, game "
                     "state %u, ready %d, confirmed %d",
                     gMode == kModeTest ? "test" : gMode == kModeOn ? "on" : "off", gOverride ? 1 : 0, gTrigger, kSteps[(int)gStep],
                     gStep != Step::kIdle && gAuto ? " (opened by itself)" : "",
                     StageScreen::CanDraw() ? "can" : "cannot", Input::PanelHold() ? "with the panel" : "with the game", (unsigned)gCursor,
                     Input::PanelMask(), GameState(), gX.Ready() ? 1 : 0, gX.Confirmed() ? 1 : 0);
        Logger::Info("STAGESTATUS hold %s%s, countdown %u s; this panel's hold: %s, %u answers; written arena: %d (-1 none); forced stage "
                     "entry now: %d",
                     gHold.wanted ? "on" : "off", gHold.broken ? " (off after a match that did not load)" : "", gHeldPickMs / 1000,
                     gHold.Holding() ? "holding" : gHold.on ? "over" : "not begun", gHold.held, gWritten, ScriptRng::MatchStage());
        char latch[420];
        PressLatch::Describe(latch, sizeof latch);
        Logger::Info("STAGESTATUS press latch %s", latch);
        // The display's counters too (draws, the largest frame, the draw time since the last report): outside a netplay
        // session nothing else writes them but 'huddemo', and a look at the panel in one game wants the draw time.
        Overlay::LogCounters();
    } else {
        Logger::Info("STAGE command not understood: %s", line);
    }
    return true;
}

}  // namespace StageFlow
