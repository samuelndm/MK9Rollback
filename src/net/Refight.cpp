#include "Refight.h"

#include <windows.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "../debug/ArenaPics.h"
#include "../debug/CrashLog.h"
#include "../debug/Logger.h"
#include "../debug/StageScreen.h"
#include "../game/Input.h"
#include "../game/MK9Game.h"
#include "../hook/ObjectHooks.h"
#include "../rollback/RollbackManager.h"
#include "../rollback/ScriptRng.h"
#include "../rollback/SoundDedupe.h"
#include "FlowTrace.h"
#include "OnlineMode.h"
#include "RefightVote.h"
#include "ScoreKeeper.h"
#include "StageFlow.h"
#include "StagePick.h"

namespace {
using namespace StagePick;
using Vote = RefightVote::Outcome;  // (StagePick has an Outcome of its own)

enum Mode { kModeOff = 0, kModeOn = 1, kModeTest = 2 };
// kVoting: the panel has the controls and the picks travel. kOver: MK9's box is over (or, in a test, both picks are
// in): what the picks gave stays on the panel a moment. kLetGo: the controls go back once nothing is held.
enum class Step { kIdle, kVoting, kOver, kLetGo };

constexpr uint32_t kBoxNative = 1350;      // 0x008C0E90 with operand 1: the first call of MK9's rematch screen script
constexpr uint32_t kBoxOverNative = 1351;  // 0x008C0EB0: that script calls it when the two decisions are in
constexpr uint32_t kSettingsNative = 1375; // the select screen's script: both fighters are locked
constexpr uint32_t kFightState = 8, kSelectState = 7, kLoadState = 1;  // GameInfo+0x4
constexpr uint32_t kPreLoadState = 12;  // one tick between the character select and the load
constexpr uint32_t kUp = 1, kDown = 2, kLockButtons = 0x20 | 0x1000, kBackButton = 0x80;
constexpr uint32_t kSendMs = 100, kMoveSendMs = 30;
constexpr uint32_t kQuietMs = 1000;     // no packet of the other panel this long: its cursor is shown as "no signal"
constexpr uint32_t kSilenceMs = 3000;   // nothing heard from the other panel this long after opening: it is not there
constexpr uint32_t kLostMs = 4000;      // heard before, then nothing this long: the panel steps aside
// MK9's rematch screen begins in the very tick the match's rollback session ends (two-PC logs of 2026-11-11: native
// 1350 "in a GGPO session", the session's end 21 ms later), so the panel opens in a later tick: tried every tick this long.
constexpr uint32_t kOpenTryMs = 5000;
// Both picks are in and MK9's own box has not ended: the presses did not reach it. The players get that box.
constexpr uint32_t kBothLockedMs = 8000;
constexpr uint32_t kOverMs = 1200, kTestOverMs = 2500;
constexpr uint32_t kPendingMs = 180000; // a re-fight that reached no fight within this is forgotten
// A re-fight that keeps its fighters: the mod answers MK9's character select for the player. The select screen reads
// no pad while it loads (1.7 s in the field log of 2026-11-14), so "confirm" is pressed again every half second
// (RefightVote::Presser) from this tick on, until both fighters are locked (script native 1375).
constexpr int kAutoAfterTicks = 45;
constexpr int kAutoMaxTicks = 720;      // 12 s of the select screen without both fighters locked: the player gets the screen and the controls
constexpr uint32_t kCoverWaitMs = 5000; // the cover before the character select is up: no longer than this
// The cover of a re-fight that keeps its fighters hides MK9's character select, which shows and names the select
// screen's own fighters and not the fight's. It is asked for with every tick and ends by itself when it is not: a stop
// of the game's thread of up to kCoverKeepMs shows no gap, and nothing can leave it on the screen.
//   'refightload 0' (as in the releases of 2026-11-14 18:18 to 2026-11-15 20:57): it stays until the fight begins,
//   over MK9's versus / loading screen too.
//   'refightload 1' (the players' wish of 2026-11-15: "we can at least show the loading screen with the codes on
//   re-match, better than the black screen"): it stays through the character select and kCoverLoadMs into the
//   load; MK9's versus / loading screen with its Kombat Kodes is seen from then on. Only where that screen shows
//   the re-fight's fighters: by itself it shows copies of the select screen's two body pictures (seen in one offline
//   game on 2026-11-16, Ladder, 'refight direct CHAR_SEKTOR_C CHAR_JOHNNYCAGE_B': the versus screen showed Scorpion and
//   Quan Chi, the fight that loaded was Sektor against Johnny Cage; MK9R_loadscreen\build\load_run. In an online
//   re-fight that would be Scorpion and Sub-Zero). The copies of a re-fight are made of its own fighters' pictures
//   (ScriptRng::MatchPictures has how); where one of the two was not, the cover stays until the fight begins, as with
//   'refightload 0'. In the same game the picture before the load was black by the time the load state began, and
//   the versus screen's fighters slid in 0.6 s after it.
constexpr uint32_t kCoverKeepMs = 3000;
constexpr uint32_t kCoverLoadMs = 1500;  // 'refightload 1', into the load: the last of the character select's picture
constexpr uint32_t kCoverFightMs = 500;  // 'refightload 0', into the fight: the last of the versus screen
constexpr uint32_t kCoverMaxMs = 90000;  // from the choice: no cover after this, whatever the game shows
const char kCoverTitle[] = "REMATCH", kCoverNote[] = "THE SAME FIGHTERS, THE SAME ARENA";
// Under the cover, MK9's character select is not heard either (its announcer names the select screen's fighters, not
// the fight's: "I still can hear the scorpion and sub-zero", 2026-11-14): no sound is started while the game is on that
// screen (rollback\SoundDedupe.h, Quiet). Asked for with every tick, as the cover is; the versus / loading screen and
// everything after it sound as always.
constexpr uint32_t kQuietKeepMs = 1000;
// MK9's online session and its Kombat Kodes (RefightVote.h has the reading of the game's code): the online manager at
// [0x00E924F0] (0 offline), its session at +0x1F4.
constexpr uintptr_t kOnlineManager = 0x00E924F0;
constexpr uint32_t kManagerSession = 0x1F4, kSessionKodes = 0x4C34, kSessionKodeLock = 0x4C38;
constexpr int kKodeWritesMax = 8;  // per load: the versus screen zeroes the session once, at its start
// The two players' records (rollback\ScriptRng.cpp: player p at 0x00E89D7C + p * 0xC8C).
constexpr uintptr_t kPlayerRecord = 0x00E89D7C;
constexpr uint32_t kRecordDwords = 0xC8C / 4;

int gMode = kModeOn;
// 'refightauto 0|1'. 1 (the start): a re-fight both chose keeps its fighters and asks nothing on the character select.
// 0: the character select is the player's, as in the release of 2026-11-14 (only the arena is kept).
bool gAuto = true;
Step gStep = Step::kIdle;
uint64_t gStepAt = 0, gOpenedAt = 0, gSentAt = 0, gHeardAt = 0;
uint32_t gOpenState = 0, gSerial = 0, gPrevMask = 0xFFFF, gPrevDir = 0;
uint64_t gRepeatAt = 0;
Exchange gX, gBot;
uint64_t gBotMoveAt = 0, gBotLockAt = 0;
uint32_t gBotRng = 1;
RefightVote::Presser gPress;
uint8_t gCursor = RefightVote::kRefight;
Vote gOutcome = Vote::kPending;
bool gSendSoon = false, gHeardLogged = false, gTheirLockLogged = false, gConfirmedLogged = false, gBoxOver = false;
int gHandsOff = 0, gLetGoTicks = 0;
bool gWantOpen = false;          // MK9's rematch screen has begun and the panel is not up yet
uint64_t gBoxAt = 0, gBothAt = 0;
const char* gRefused = nullptr;  // why the last try to open was refused (logged when it changes)
char gNote[72] = "";
StageScreen::State gView;

// The last online fight's arena, as a stage table entry (-1: none seen), and its two fighters as the game names them
// (fighter and costume; gLastTag 0: not read, or not names of fighters).
int gLastStage = -1;
char gLastP1[64] = "", gLastP2[64] = "";
uint32_t gLastTag = 0;
// ... and the two fighters' colours (the character select's START: ScriptRng::FightColours), given back with them.
uint32_t gLastColours[2] = {};
bool gLastColoursOk = false;
bool gFightNoted = false;
uint64_t gFightTryAt = 0;

// A re-fight both players chose, on its way through the character select to the next fight.
// gDirect: it keeps its fighters (the game is given the last pair where its script sets the fighters, the mod answers
// the select screen and covers it). gPendingTest: begun by 'refight direct' in any game, for a look at one PC.
bool gPending = false, gSelectSeen = false, gCardOn = false, gCardDone = false, gAutoHold = false, gAutoDone = false;
bool gDirect = false, gPendingTest = false;
bool gCoverUp = false;  // the cover over the whole picture is asked for (StageScreen::Cover)
// 'refightload 0|1|<ms>'. 0: the cover stays until the fight begins. 1: it ends gLoadMs into the load and MK9's versus
// / loading screen is seen, where that screen's two pictures were made of the re-fight's fighters ('refightpics 1').
// Both are 1 at the start since one offline game showed the pictures (2026-11-16, the Ladder; no online match yet).
// With either at 0, or where a picture could not be made, a re-fight is as in the release of 2026-11-15 20:57.
bool gLoadShow = true;
uint32_t gLoadMs = kCoverLoadMs;
bool gLoadShown = false;   // this re-fight's load has begun and its cover was given up for the versus / loading screen
bool gLoadNoPics = false;  // ... has begun, and the cover stays: that screen does not show the re-fight's fighters (said once)
uint64_t gPendingAt = 0, gCoverAt = 0, gCoverAskedAt = 0;
uint32_t gCoverGapMs = 0;  // the longest time between two ticks that asked for the cover (kCoverKeepMs must stay above it)
int gSelectTicks = 0;
uint32_t gPendingState = 0xFFFFFFFF;
RefightVote::Presser gAutoPress;

// 'refightquiet 0|1'. 1 (the start): the hidden character select of a re-fight is not heard.
bool gQuiet = true;
bool gQuietOn = false;
uint32_t gQuietHeld = 0;
char gQuietNames[1800] = "";
uint64_t gQuietTestUntil = 0;  // 'refightquiet <ms>': a look at one PC, no sound is started until then, in any game state

// The Kombat Kodes of the online match that loads or runs, as the game last held them locked since its character
// select (gKodesSeenOk), and those of the last online fight: what a re-fight's load is given again (gLastKodesOk).
RefightVote::Kodes gKodesSeen, gLastKodes, gKeptKodes;  // gKeptKodes: the last fight's, as they stood when the pending re-fight was chosen
bool gKodesSeenOk = false, gLastKodesOk = false, gKeptKodesOk = false;
// 'refightkodes 0|1'. 0 (the start, the players' wish of 2026-11-16: "on the re-match make the MK codes not locked,
// make it editable and reseted"): a rematch's versus screen is MK9's own, it starts at 0-0-0 and each player enters a
// kode there as before any fight. 1: the last fight's kodes, locked (the releases of 2026-11-14 20:40 to 2026-11-16 18:14).
bool gKeepKodes = false;
int gKodeWrites = 0;
bool gKodeTheirsLogged = false, gKodeNoSessionLogged = false, gKodeCapLogged = false;

// The two players' records at the last online fight's start, for a look at what a re-fight leaves different ("the
// skin is still changing", 2026-11-14: the fighters' names and meshes read the same in the log).
uint32_t gRecLast[2][kRecordDwords];
bool gRecLastOk = false, gFightOfRefight = false;

uint32_t GameState() {
    const uint32_t gi = *reinterpret_cast<const volatile uint32_t*>(MK9Game::Get().gameInfoPtr);
    return gi ? *reinterpret_cast<const volatile uint32_t*>(gi + 0x4) : 0xFFFFFFFF;
}
bool OnGameThread() { return GetCurrentThreadId() == ObjectHooks::gGameThread; }

uint32_t FreshNonce() {  // as the stage picks': a coin between two friends, not a secret
    static uint32_t count = 0;
    LARGE_INTEGER q;
    QueryPerformanceCounter(&q);
    uint32_t h = detail::Mix((uint32_t)q.LowPart ^ 0x2F1A77C3u);
    h = detail::Mix(h + (uint32_t)q.HighPart + GetTickCount());
    h = detail::Mix(h ^ GetCurrentProcessId() ^ (++count * 0x9E3779B9u) ^ (uint32_t)reinterpret_cast<uintptr_t>(&q));
    return h ? h : 1;
}
uint32_t NextSerial() {  // larger than every earlier one of this kind, also after the game was started again
    FILETIME ft;
    GetSystemTimeAsFileTime(&ft);
    const uint32_t s = (uint32_t)((((uint64_t)ft.dwHighDateTime << 32) | ft.dwLowDateTime) / 10000000ull);
    gSerial = (int32_t)(s - gSerial) > 0 ? s : gSerial + 1;
    return gSerial;
}

void PublishOff() {
    gView = StageScreen::State{};
    StageScreen::Publish(gView);
}

void Publish(uint64_t now) {
    StageScreen::State& s = gView;
    s = StageScreen::State{};
    s.phase = StageScreen::kVote;
    s.cells = RefightVote::kChoices;
    for (uint8_t i = 0; i < RefightVote::kChoices; ++i) snprintf(s.name[i], sizeof s.name[i], "%s", RefightVote::ChoiceText(i));
    s.myCursor = gX.mine.locked ? gX.mine.choice : gCursor;
    s.myLocked = gX.mine.locked ? 1 : 0;
    const uint8_t theirs = gX.theirs.locked ? gX.theirs.choice : gX.theirs.cursor;
    s.theirCursor = RefightVote::IsChoice(theirs) ? theirs : StageScreen::kNoCell;
    s.peer = gX.theirs.locked ? StageScreen::kPeerLocked : !gX.theirs.nonce ? StageScreen::kPeerSilent
             : now - gHeardAt < kQuietMs ? StageScreen::kPeerChoosing : StageScreen::kPeerQuiet;
    s.test = gMode == kModeTest;
    snprintf(s.note, sizeof s.note, "%s", gNote);
    ScoreKeeper::FillBoard(&s.board);  // the score against this opponent: drawn over the panel and beside it
    StageScreen::Publish(s);
}

void SetStep(Step s, uint64_t now) {
    gStep = s;
    gStepAt = now;
}
void EndPending(const char* why);

// The panel is gone and the game has its controls. A re-fight both players chose, and both know of, goes on from here.
void Finish(uint64_t now, const char* why) {
    const bool agreed = gMode == kModeOn && RefightVote::RefightAgreed(gX);
    Logger::Info("REFIGHT panel closed: %s (this player %s, the other %s: %s%s)", why,
                 gX.mine.locked ? RefightVote::ChoiceText(gX.mine.choice) : "no pick", gX.theirs.locked ? RefightVote::ChoiceText(gX.theirs.choice) : "no pick",
                 RefightVote::OutcomeText(RefightVote::Of(gX.mine, gX.theirs)),
                 RefightVote::Of(gX.mine, gX.theirs) == Vote::kRefight && !agreed && gMode == kModeOn ? ", NOT confirmed by the other game: not acted on" : "");
    gX.Close();
    gBot.Close();
    gPress.Reset();
    Input::SetPanelPass(0);
    if (Input::PanelHold()) Input::SetPanelHold(false);
    SetStep(Step::kIdle, now);
    gNote[0] = 0;
    PublishOff();
    if (!agreed) return;
    gPending = true;
    gPendingAt = now;
    gSelectSeen = gCardOn = gCardDone = gAutoHold = gAutoDone = gPendingTest = gCoverUp = gLoadShown = gLoadNoPics = false;
    gKeptKodes = gLastKodes;
    gKeptKodesOk = gLastKodesOk;
    gKodeWrites = 0;
    gKodeTheirsLogged = gKodeNoSessionLogged = gKodeCapLogged = false;
    gSelectTicks = 0;
    gPendingState = 0xFFFFFFFF;
    // The same fighters too, where the last fight's are known: the game is given them where its script sets the
    // fighters, and the stage screen's exchange settles whether they stand (StageFlow.h).
    const char* noDirect = !gAuto ? "'refightauto 0'" : gLastStage < 0 ? "the last fight's arena is not known" : !gLastTag ? "the last fight's fighters are not known"
                           : !ScriptRng::SetMatchPlayers(gLastP1, gLastP2, gLastColoursOk ? gLastColours : nullptr)
                               ? "the game's code for setting the fighters is not the one this mod knows" : nullptr;
    gDirect = !noDirect;
    StageFlow::SetRefight(gLastStage, gDirect ? gLastTag : 0);
    const char* name = gLastStage >= 0 ? ScriptRng::StageAt((uint32_t)gLastStage) : nullptr;
    if (gDirect)
        Logger::Info("REFIGHT both players chose the same fight again and both games know it: the next fight keeps %.60s and %.60s on %.40s. "
                     "MK9's character select runs under a cover, the mod confirms it for this player, and the stage screen stays shut",
                     gLastP1, gLastP2, name ? name : "?");
    else
        Logger::Info("REFIGHT both players chose the same fight again and both games know it: each player confirms the fighter on the character "
                     "select (%s), and the stage screen %s%.40s",
                     noDirect, gLastStage >= 0 ? "stays shut, this player's pick is the last arena, " : "opens as after any match", name ? name : "");
}

// The panel ends. careful: the player goes on with MK9's own box, so the controls go back only once nothing is held (a
// button still down would be a press on that box).
void Close(const char* why, uint64_t now, bool careful) {
    if (gStep == Step::kIdle) return;
    if (careful && gStep != Step::kLetGo) {
        Logger::Info("REFIGHT the panel steps aside: %s; MK9's own rematch box is the player's now", why);
        gPress.Reset();
        Input::SetPanelPass(0);
        gHandsOff = gLetGoTicks = 0;
        snprintf(gNote, sizeof gNote, "%s", "USE MK9'S OWN BOX");
        SetStep(Step::kLetGo, now);
        return;
    }
    Finish(now, why);
}

bool Open(const char* why, bool byNative) {
    const uint64_t now = GetTickCount64();
    const char* no = nullptr;
    if (gMode == kModeOff) no = "the choice after a match is off ('refight 1' or 'refight test')";
    else if (!OnGameThread()) no = "not on the game thread";
    else if (gStep != Step::kIdle) no = "a panel is already open";
    else if (!StageFlow::Idle() || Input::PanelHold()) no = "another panel of the mod is on the screen";
    else if (Rollback::GgpoActive()) no = "a rollback session runs";
    // The script native is the rematch screen's own first call; a command must never take a fighter's controls.
    else if (!byNative && GameState() == kFightState) no = "a fight is running";
    else if (!StageScreen::CanDraw()) no = "the display cannot draw the panel (no faces, or no frame in the last second)";
    else if (gMode == kModeOn && !Online::OneOnOne()) no = "not in a one-on-one online match";
    // (The hellos pause between a match's phases, so nothing here asks for a fresh one: the other panel's own packets
    // show that it is there, within kSilenceMs.)
    else if (gMode == kModeOn && !Online::SameBuildPeer())
        no = "the two mods of this match have not answered each other as the same build (no mod on the other side, or another build)";
    if (no) {
        // Tried every tick after MK9's rematch screen began: a line when the reason changes.
        if (!byNative || no != gRefused) Logger::Info("REFIGHT panel not opened (%s): %s%s", why, no, byNative ? "; trying again" : "");
        gRefused = no;
        return false;
    }
    gRefused = nullptr;
    gBothAt = 0;
    EndPending("a new choice after a match");  // a re-fight that never reached its fight
    ArenaPics::Request();  // MK9's look for the panel (already read where the stage screen has shown; else read now, the plain panel until then)
    gX.Open(kKindAfterMatch, RefightVote::kContext, FreshNonce(), NextSerial());
    gCursor = RefightVote::kRefight;
    gX.MoveCursor(gCursor);
    gPress.Reset();
    gPrevMask = 0xFFFF;  // what is held now is not a press
    gPrevDir = 0;
    gRepeatAt = 0;
    gOpenedAt = now;
    gSentAt = gHeardAt = 0;
    gSendSoon = gHeardLogged = gTheirLockLogged = gConfirmedLogged = gBoxOver = false;
    gOutcome = Vote::kPending;
    gNote[0] = 0;
    gOpenState = GameState();
    if (gMode == kModeTest) {
        gBot.Open(kKindAfterMatch, RefightVote::kContext, FreshNonce(), NextSerial());
        gBotRng = FreshNonce();
        gBotMoveAt = now + 600;
        gBotLockAt = now + 3500;
    }
    Input::SetPanelHold(true);
    SetStep(Step::kVoting, now);
    const char* name = gLastStage >= 0 ? ScriptRng::StageAt((uint32_t)gLastStage) : nullptr;
    Logger::Info("REFIGHT panel opened (%s): exchange serial %u, mode %s, game state %u, the last online fight's arena %s%.40s", why, gX.serial,
                 gMode == kModeTest ? "test (simulated opponent, nothing is pressed on the game's screen)" : "online", gOpenState,
                 gLastStage >= 0 ? "" : "is not known", name ? name : "");
    Publish(now);
    return true;
}

void Heard(const Packet& p, const char* from) {
    const Exchange::Heard h = gX.OnPacket(p);
    if (h == Exchange::Heard::kTaken) gHeardAt = GetTickCount64();
    if (h == Exchange::Heard::kContextDiffers && !gHeardLogged) {
        gHeardLogged = true;
        Logger::Info("REFIGHT heard %s with other choices (context %08X, this game %08X)", from, p.context, gX.context);
    } else if (h == Exchange::Heard::kTaken && !gHeardLogged) {
        gHeardLogged = true;
        Logger::Info("REFIGHT heard %s: its panel is open (exchange serial %u)", from, p.serial);
    }
    if (gX.theirs.locked && !gTheirLockLogged) {
        gTheirLockLogged = true;
        Logger::Info("REFIGHT %s chose %s", from, RefightVote::ChoiceText(gX.theirs.choice));
    }
    if (!gX.theirs.locked) gTheirLockLogged = false;  // a later exchange of theirs (a LEAVE after another pick) is logged again
    if (gX.Confirmed() && !gConfirmedLogged) {
        gConfirmedLogged = true;
        Logger::Info("REFIGHT confirmed: the other game has this game's choice, both read the two choices alike");
    }
}

void BotTick(uint64_t now) {
    if (!gBot.open || gBot.mine.locked) return;
    const auto rnd = [] {
        gBotRng = detail::Mix(gBotRng + 0x9E3779B9u);
        return gBotRng;
    };
    if (now >= gBotLockAt) gBot.Lock((uint8_t)(rnd() % 2));  // RE-FIGHT or CHANGE: a LEAVE would end the test at once
    else if (now >= gBotMoveAt) {
        gBot.MoveCursor((uint8_t)(rnd() % RefightVote::kChoices));
        gBotMoveAt = now + 450;
    }
}

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

// This player's choice is final in its exchange, and MK9's box gets its answer: the presses begin with the next tick.
void LockMine(uint8_t choice, const char* how) {
    if (!gX.Lock(choice)) return;
    gSendSoon = true;
    if (choice == RefightVote::kLeave) gPress.Leave();
    else gPress.Accept();
    Logger::Info("REFIGHT this player chose %s (%s): MK9's box is answered \"%s\" for this player%s", RefightVote::ChoiceText(choice), how,
                 choice == RefightVote::kLeave ? "decline" : "accept", gMode == kModeTest ? " (a test: nothing is pressed)" : "");
}

void ReadControls(uint64_t now) {
    const uint32_t m = Input::PanelMask() | Input::TakePanelPresses();
    const uint32_t pressed = m & ~gPrevMask;
    gPrevMask = m;
    if (gX.mine.locked) {
        // After a pick: "back" leaves after all. The pick is final in its exchange, so the LEAVE travels as a later
        // exchange of this game (the other game takes a later one in place of the earlier), and MK9's box gets "back"
        // twice: the first takes the accept back, the second declines.
        if ((pressed & kBackButton) && gX.mine.choice != RefightVote::kLeave) {
            RefightVote::Reopen(&gX, FreshNonce(), NextSerial());
            gX.Lock(RefightVote::kLeave);
            gCursor = RefightVote::kLeave;
            gPress.Leave();
            gSendSoon = true;
            gConfirmedLogged = gTheirLockLogged = false;
            Logger::Info("REFIGHT this player leaves after all (\"back\" after the pick): MK9's box is answered \"decline\"");
        }
        return;
    }
    // Up and down act when pressed, and again while held (0.35 s, then every 0.15 s).
    const uint32_t dir = m & (kUp | kDown);
    bool step = (pressed & (kUp | kDown)) != 0;
    if (dir != gPrevDir || step) gRepeatAt = now + 350;
    else if (dir && now >= gRepeatAt) {
        step = true;
        gRepeatAt = now + 150;
    }
    gPrevDir = dir;
    if (step) {
        const uint32_t use = (pressed & (kUp | kDown)) ? (pressed & (kUp | kDown)) : dir;
        const int dy = ((use & kDown) ? 1 : 0) - ((use & kUp) ? 1 : 0);
        gCursor = (uint8_t)((gCursor + RefightVote::kChoices + dy) % RefightVote::kChoices);
        gX.MoveCursor(gCursor);
        gSendSoon = true;
    } else if (pressed & kBackButton) {
        // "back" is MK9's decline; here it only puts the cursor on LEAVE, so one stray press ends no evening.
        gCursor = RefightVote::kLeave;
        gX.MoveCursor(gCursor);
        gSendSoon = true;
    }
    if (pressed & kLockButtons) LockMine(gCursor, "the player's button");
}

void NoteOutcome() {
    const Vote o = RefightVote::Of(gX.mine, gX.theirs);
    if (o == gOutcome) return;
    gOutcome = o;
    const bool iLeave = gX.mine.locked && gX.mine.choice == RefightVote::kLeave;
    snprintf(gNote, sizeof gNote, "%s",
             o == Vote::kRefight ? "REMATCH: THE SAME FIGHTERS, THE SAME ARENA" : o == Vote::kChange ? "BACK TO THE CHARACTER SELECT"
             : o == Vote::kLeave ? (iLeave ? "LEAVING" : "THE OPPONENT LEAVES") : "");
    if (o != Vote::kPending) Logger::Info("REFIGHT the two choices give: %s", RefightVote::OutcomeText(o));
}

// The online session's Kombat Kodes, through guarded reads. False: no online session (an offline game), or not readable.
bool ReadKodes(RefightVote::Kodes* k, uint32_t* session) {
    uint32_t mgr = 0, s = 0, words = 0, flag = 0;
    if (!CrashLog::SafeRead32(kOnlineManager, &mgr) || mgr < 0x10000 || !CrashLog::SafeRead32(mgr + kManagerSession, &s) || s < 0x10000) return false;
    if (!CrashLog::SafeRead32(s + kSessionKodes, &words) || !CrashLog::SafeRead32(s + kSessionKodeLock, &flag)) return false;
    *k = RefightVote::KodesOf(words, flag);
    if (session) *session = s;
    return true;
}
// Every tick: the kodes the game holds locked while an online one-on-one match loads and in the fight's first ticks
// (until NoteFight has taken them). The character select forgets them: the next match is set up.
void NoteKodes(uint32_t state) {
    if (state == kSelectState) {
        gKodesSeenOk = false;
        return;
    }
    if (state != kLoadState && !(state == kFightState && !gFightNoted)) return;
    RefightVote::Kodes k;
    if (!Online::OneOnOne() || !ReadKodes(&k, nullptr) || !k.locked) return;
    gKodesSeen = k;
    gKodesSeenOk = true;
}

// The two players' records at an online fight's start. At the start of a re-fight that kept its fighters: what differs
// from the fight before it (pointers and counters will; a value that names a look would show here).
void NoteRecords(bool refight) {
    static uint32_t rec[2][kRecordDwords];
    bool ok = true;
    for (uint32_t p = 0; p < 2 && ok; ++p)
        for (uint32_t i = 0; i < kRecordDwords && ok; ++i) ok = CrashLog::SafeRead32(kPlayerRecord + p * 0xC8C + i * 4, &rec[p][i]);
    if (refight && ok && gRecLastOk) {
        char line[1500] = "";
        size_t n = 0;
        int differ = 0;
        for (uint32_t p = 0; p < 2; ++p)
            for (uint32_t i = 0; i < kRecordDwords; ++i) {
                if (rec[p][i] == gRecLast[p][i]) continue;
                if (++differ <= 48 && n + 32 < sizeof line)
                    n += (size_t)snprintf(line + n, sizeof line - n, " P%u+%03X %08X>%08X", p + 1, i * 4, gRecLast[p][i], rec[p][i]);
            }
        Logger::Info("REFIGHT records: %d of %u values of the two players' records differ between this re-fight's start and the start of the "
                     "fight before it%s%s",
                     differ, 2 * kRecordDwords, differ ? " (the first 48 at most):" : "", line);
    } else if (refight) {
        Logger::Info("REFIGHT records: not compared (%s)", ok ? "the fight before this re-fight was not read" : "the players' records could not be read");
    }
    if (ok) memcpy(gRecLast, rec, sizeof gRecLast);
    gRecLastOk = ok;
}

// The last online fight's arena, read once per fight from the game's own name of it.
void NoteFight(uint64_t now, uint32_t state) {
    if (state != kFightState) {
        gFightNoted = gFightOfRefight = false;
        return;
    }
    if (gFightNoted || now < gFightTryAt || !Online::OneOnOne()) return;
    gFightTryAt = now + 500;
    char name[80] = "";
    if (!FlowTrace::FightStageName(name, sizeof name)) return;
    gFightNoted = true;
    gLastStage = ScriptRng::StageIndex(name);
    // The fight's two fighters, as the game holds them (fighter and costume): what a re-fight gives the game again.
    char a[64] = "", b[64] = "";
    const bool read = ScriptRng::FightPlayers(a, b, sizeof a), names = read && RefightVote::FighterName(a) && RefightVote::FighterName(b);
    snprintf(gLastP1, sizeof gLastP1, "%s", names ? a : "");
    snprintf(gLastP2, sizeof gLastP2, "%s", names ? b : "");
    gLastTag = names ? RefightVote::PairTag(a, b) : 0;
    gLastColoursOk = names && ScriptRng::FightColours(gLastColours);
    char colours[48] = "not read";
    if (gLastColoursOk) snprintf(colours, sizeof colours, "%u and %u (1: the other colours, START)", gLastColours[0], gLastColours[1]);
    Logger::Info("REFIGHT this online fight's arena is %.60s (stage table entry %d) and its fighters are %.60s and %.60s%s, colours %s: what "
                 "a rematch would use again (the two players' records hold %08X and %08X at +0xB40)",
                 name, gLastStage, names ? a : "?", names ? b : "?",
                 names ? "" : read ? " (what the game holds are not fighters' names)" : " (the game's fighter names could not be read)", colours,
                 ScriptRng::PlayerRecordValue(0), ScriptRng::PlayerRecordValue(1));
    // Its Kombat Kodes, as the game locked them when the load ended: what a re-fight's load is given again.
    gLastKodes = gKodesSeen;
    gLastKodesOk = gKodesSeenOk && RefightVote::KodesKeepable(gKodesSeen);
    char mine[6], theirs[6];
    RefightVote::KodeText(gKodesSeen.mine, mine);
    RefightVote::KodeText(gKodesSeen.theirs, theirs);
    if (gLastKodesOk)
        Logger::Info("REFIGHT this online fight's Kombat Kodes, as the game locked them when its load ended: this player %s, the other player %s: "
                     "what a re-fight's load would be given again",
                     mine, theirs);
    else if (gKodesSeenOk)
        Logger::Info("REFIGHT this online fight's Kombat Kodes are not three digits each in the game's session (%04X and %04X): a re-fight "
                     "leaves the kodes to the game",
                     gKodesSeen.mine, gKodesSeen.theirs);
    else
        Logger::Info("REFIGHT this online fight's Kombat Kodes were not seen locked in the game's session while it loaded: a re-fight leaves "
                     "the kodes to the game");
    // (A re-fight's fight may be noted in the tick before Pending sees it.)
    NoteRecords(gFightOfRefight || (gPending && gDirect && gSelectSeen));
    gFightOfRefight = false;
}

// The hidden character select of a re-fight is not heard: asked for with every tick the cover is up on that screen.
void QuietEnd(const char* why) {
    if (!gQuietOn) return;
    gQuietOn = false;
    SoundDedupe::QuietOff();
    gQuietHeld = SoundDedupe::QuietTake(gQuietNames, sizeof gQuietNames);
    Logger::Info("REFIGHT quiet: over (%s): %u sound(s) of the hidden character select were not started%s%s", why, gQuietHeld,
                 gQuietHeld ? "; the first of them (a / b: the game's play method): " : "", gQuietHeld ? gQuietNames : "");
}
void QuietTick(uint32_t state) {
    if (!gQuiet || (state != kSelectState && state != kPreLoadState)) return QuietEnd(gQuiet ? "the game left the character select" : "'refightquiet 0'");
    SoundDedupe::Quiet(kQuietKeepMs);
    if (gQuietOn) return;
    gQuietOn = true;
    SoundDedupe::QuietTake(nullptr, 0);  // (what an earlier quiet left uncounted)
    Logger::Info("REFIGHT quiet: on (game state %u): no sound is started while the cover is up on MK9's character select; it ends by itself "
                 "%u ms after the mod last asked for it",
                 state, kQuietKeepMs);
}
// 'refightquiet <ms>' (a look at one PC): the same quiet for that long, whatever the game shows, and the same lines.
void QuietTest(uint64_t now, uint32_t state) {
    if (!gQuietTestUntil) return;
    if (now >= gQuietTestUntil) {
        gQuietTestUntil = 0;
        return QuietEnd("the test's time is over");
    }
    SoundDedupe::Quiet(kQuietKeepMs);
    if (gQuietOn) return;
    gQuietOn = true;
    SoundDedupe::QuietTake(nullptr, 0);
    Logger::Info("REFIGHT quiet: on (a test, game state %u): no sound is started for the next %u ms", state, (uint32_t)(gQuietTestUntil - now));
}

// A re-fight that keeps its fighters loads with the last fight's Kombat Kodes too ("the same MK codes chosen in the
// loading screen", 2026-11-14). While its versus / loading screen runs (in sight with 'refightload 1', under the cover
// with 0), the game's session is given the last fight's two words and the "locked" flag whenever it is not locked
// (RefightVote::KodeTick has the rule): what a player types then changes nothing, the versus screen shows and uses the
// six digits it reads from the session, and when the load ends the host's game sends its two words to the other game
// as after any versus screen. A session that is locked with other words is the other game's doing, or MK9's: left
// alone. Only while the game holds the re-fight's fighters: the cover is up, or was given up for this very load.
// Only with 'refightkodes 1': at 0 (the start) the versus screen's kodes are the players', as after any character select.
void KeepKodes(uint64_t now, uint32_t state) {
    if (!gKeepKodes || !(gCoverUp || gLoadShown) || state != kLoadState || !gKeptKodesOk || gKodeTheirsLogged) return;
    RefightVote::Kodes held;
    uint32_t session = 0;
    if (!ReadKodes(&held, &session)) {
        if (!gKodeNoSessionLogged)
            Logger::Info("REFIGHT Kombat Kodes: no online session to give the last fight's kodes to (in an offline game they live in the "
                         "versus screen alone)");
        gKodeNoSessionLogged = true;
        return;
    }
    char mine[6], theirs[6], hm[6], ht[6];
    RefightVote::KodeText(gKeptKodes.mine, mine);
    RefightVote::KodeText(gKeptKodes.theirs, theirs);
    RefightVote::KodeText(held.mine, hm);
    RefightVote::KodeText(held.theirs, ht);
    switch (RefightVote::KodeTick(held, gKeptKodes)) {
    case RefightVote::KodeStep::kHold:
        return;
    case RefightVote::KodeStep::kTheirs:
        gKodeTheirsLogged = true;
        Logger::Info("REFIGHT Kombat Kodes: %u ms after the choice the game holds %s (this player) and %s (the other player), locked, and "
                     "not the last fight's %s and %s: the other game's or MK9's own, left as they are (%d write(s) of the mod before)",
                     (unsigned)(now - gPendingAt), hm, ht, mine, theirs, gKodeWrites);
        return;
    case RefightVote::KodeStep::kWrite:
        break;
    }
    if (gKodeWrites >= kKodeWritesMax) {
        if (!gKodeCapLogged)
            Logger::Info("REFIGHT Kombat Kodes: the game unlocked them %d times in this load; the mod writes them no more", gKodeWrites);
        gKodeCapLogged = true;
        return;
    }
    *reinterpret_cast<volatile uint32_t*>(session + kSessionKodes) = RefightVote::KodeWords(gKeptKodes);
    *reinterpret_cast<volatile uint32_t*>(session + kSessionKodeLock) = 1;
    if (++gKodeWrites <= 3)
        Logger::Info("REFIGHT Kombat Kodes: write %d, %u ms after the choice: the game held %s and %s, not locked; it is given the last "
                     "fight's, locked: this player %s, the other player %s. Nothing typed on the versus screen changes them (%s); when "
                     "the load ends the host's game sends both to the other game, as after any versus screen",
                     gKodeWrites, (unsigned)(now - gPendingAt), hm, ht, mine, theirs,
                     gLoadShown ? "that screen is in sight, or will be when the cover has run out" : "that screen is under the cover");
}
// What the game holds as the re-fight's fight begins.
void KodesAtFight() {
    if (!gDirect) return;
    RefightVote::Kodes held;
    char hm[6] = "", ht[6] = "", mine[6], theirs[6];
    const bool read = ReadKodes(&held, nullptr);
    RefightVote::KodeText(held.mine, hm);
    RefightVote::KodeText(held.theirs, ht);
    RefightVote::KodeText(gKeptKodes.mine, mine);
    RefightVote::KodeText(gKeptKodes.theirs, theirs);
    if (!gKeepKodes) Logger::Info("REFIGHT Kombat Kodes at the rematch's start: %s%s%s%s, as the players entered them on MK9's versus screen ('refightkodes 0')",
                                  read ? hm : "no online session", read ? " (this player) and " : "", read ? ht : "",
                                  read ? (held.locked ? " (the other player), locked" : " (the other player), not locked") : "");
    else if (!gKeptKodesOk) Logger::Info("REFIGHT Kombat Kodes: the last fight's were not known; the game was left its own (%s%s%s%s)", read ? hm : "no online session",
                                    read ? " and " : "", read ? ht : "", read ? (held.locked ? ", locked" : ", not locked") : "");
    else if (!read) Logger::Info("REFIGHT Kombat Kodes: no online session at the re-fight's start; the mod wrote %d time(s)", gKodeWrites);
    else
        Logger::Info("REFIGHT Kombat Kodes at the re-fight's start: the game holds this player %s, the other player %s (%s); the last fight's "
                     "were %s and %s: %s. The mod wrote %d time(s)",
                     hm, ht, held.locked ? "locked" : "not locked", mine, theirs,
                     held.mine == gKeptKodes.mine && held.theirs == gKeptKodes.theirs ? "the same" : "NOT the same", gKodeWrites);
}

void ReleaseAuto() {
    if (!gAutoHold) return;
    gAutoHold = false;
    gAutoDone = true;
    gAutoPress.Reset();
    Input::SetPanelPass(0);
    if (Input::PanelHold()) Input::SetPanelHold(false);
}

// The cover's end. lingerMs: it runs out by itself that much later, over what `over` names; 0: it is gone now.
void CoverEnd(uint64_t now, uint32_t state, const char* why, uint32_t lingerMs, const char* over) {
    if (!gCoverUp) return;
    gCoverUp = false;
    QuietEnd("the cover ended");
    if (lingerMs) StageScreen::Cover(kCoverTitle, kCoverNote, lingerMs);
    else StageScreen::CoverOff();
    char stays[160] = "";
    if (lingerMs) snprintf(stays, sizeof stays, "; it stays for another %u ms, over %s", lingerMs, over);
    Logger::Info("REFIGHT cover: off %u ms after it went up (%s; game state %u)%s; the longest time between two ticks under it was %u ms (a gap "
                 "shows from %u on)",
                 (unsigned)(now - gCoverAt), why, state, stays, gCoverGapMs, kCoverKeepMs);
}
// Every tick of a pending re-fight. The cover is up on the way to the character select, on the select while the mod
// answers it, and from the lock of both fighters into the load while the game holds the re-fight's pair: gLoadMs into
// it with 'refightload 1' (MK9's versus / loading screen is seen from then on), through all of it with 0. Not when the
// pair was given back (the select screen's own fighters then fight, and are seen), not when the player was given the
// select screen, not in any other state of the game.
void CoverTick(uint64_t now, uint32_t state) {
    const bool select = state == kSelectState;
    const char* no = nullptr;
    if (!gDirect) no = "this re-fight does not keep its fighters";
    else if (now - gPendingAt >= kCoverMaxMs) no = "no fight began in time";
    else if (!gSelectSeen && !select) no = gPendingTest ? "a test waits for the character select" : now - gPendingAt >= kCoverWaitMs ? "the character select did not come up in time" : nullptr;
    else if (!select && state != kPreLoadState && state != kLoadState) no = "the game left the character select and the load";
    else if (!ScriptRng::MatchPlayersWritten() && (!select || gAutoDone))
        no = select ? "the game does not hold the re-fight's fighters, and the select screen is the player's" : "the game does not hold the re-fight's fighters";
    if (no) {
        gLoadShown = false;  // (the kodes are kept no further either)
        return CoverEnd(now, state, no, 0, "");
    }
    if (gLoadShown) return;  // MK9's versus / loading screen is in sight: no cover is asked for in this load any more
    if (gLoadShow && gSelectSeen && state == kLoadState) {
        // The load has begun: the character select is behind the game, and its script has made the two pictures of
        // the versus / loading screen. Both of the re-fight's fighters: the cover runs out a moment later, over the
        // change of the picture, and that screen is the players' (its kodes stay the last fight's). Not both: the
        // screen would show the select screen's fighters, and the cover stays until the fight begins.
        const uint32_t pics = ScriptRng::MatchPictures();
        if (pics == 3) {
            gLoadShown = true;
            QuietTick(state);  // (over: the game left the character select)
            return CoverEnd(now, state, "the load has begun, 'refightload 1'", gLoadMs,
                            "the last of the character select's picture; MK9's versus / loading screen is seen from then on, with the re-fight's "
                            "fighters and its Kombat Kodes");
        }
        if (!gLoadNoPics) {
            gLoadNoPics = true;
            Logger::Info("REFIGHT cover: the load has begun and the cover stays until the fight ('refightload 1'): MK9's versus / loading screen "
                         "does not show the re-fight's fighters (its left picture is %s, its right picture %s; %u copies of a picture were asked "
                         "for since the game started, 'refightpics' is %d)",
                         pics & 1 ? "the re-fight's" : "the select screen's", pics & 2 ? "the re-fight's" : "the select screen's",
                         ScriptRng::PictureCalls(), ScriptRng::MatchPicturesOn() ? 1 : 0);
        }
    }
    StageScreen::Cover(kCoverTitle, kCoverNote, kCoverKeepMs);
    QuietTick(state);
    if (gCoverUp) {
        if (now - gCoverAskedAt > gCoverGapMs) gCoverGapMs = (uint32_t)(now - gCoverAskedAt);
        gCoverAskedAt = now;
        return;
    }
    gCoverUp = true;
    gCoverAt = gCoverAskedAt = now;
    gCoverGapMs = 0;
    char until[224];
    if (gLoadShow)
        snprintf(until, sizeof until, "MK9's character select is hidden; its versus / loading screen is seen from %u ms into the load where it shows the re-fight's fighters ('refightload 1')", gLoadMs);
    else snprintf(until, sizeof until, "MK9's character select and its versus / loading screen are hidden until the fight begins ('refightload 0')");
    Logger::Info("REFIGHT cover: up %u ms after the choice (game state %u): %s; the cover ends by itself %u ms after the mod last asked for it",
                 (unsigned)(now - gPendingAt), state, until, kCoverKeepMs);
}

void EndPending(const char* why) {
    if (!gPending) return;
    gPending = false;
    gLoadShown = gLoadNoPics = false;
    CoverEnd(GetTickCount64(), GameState(), why, 0, "");
    ReleaseAuto();
    if (gCardOn && StageFlow::Idle()) PublishOff();
    gCardOn = false;
    StageFlow::SetRefight(-1);
    if (gDirect) ScriptRng::SetMatchPlayers(nullptr, nullptr);  // from here the game's scripts set their fighters themselves again
    gDirect = gPendingTest = false;
    Logger::Info("REFIGHT the re-fight both players chose is over: %s", why);
}

// The pending re-fight's two lines. cover: over the whole picture, in its middle (the game asks the player nothing).
void PublishCard(bool cover, const char* note) {
    gView = StageScreen::State{};
    gView.phase = StageScreen::kCard;
    gView.cover = cover ? 1 : 0;
    snprintf(gView.name[0], sizeof gView.name[0], "REMATCH");
    snprintf(gView.note, sizeof gView.note, "%s", note);
    StageScreen::Publish(gView);
    gCardOn = true;
}
void CardOff() {
    if (!gCardOn) return;
    gCardOn = false;
    PublishOff();
}

// A re-fight both chose, from the choice to its fight. One that keeps its fighters: the game's picture is covered from
// the choice through the character select (CoverTick has how far into the load), and on the character select the mod
// holds the controls and presses "confirm" for this player. Any other: two lines at the foot of the character select,
// which is the player's.
void Pending(uint64_t now, uint32_t state) {
    if (!gPending) return;
    if (gPendingTest && state != gPendingState) {
        Logger::Info("REFIGHT (test) the game's state is %u (it was %u; the character select of an online match is 7)", state, gPendingState);
        gPendingState = state;
    }
    // (Only after the character select: the state under MK9's rematch screen is not known to differ from a fight's.)
    if (state == kFightState && gSelectSeen) {
        KodesAtFight();
        gFightOfRefight = gDirect && !gFightNoted;  // for the fight's note, where it has not been made in this very tick
        CoverEnd(now, state, "its fight has begun", kCoverFightMs, "the last of MK9's versus screen");  // ('refightload 0': up until here)
        return EndPending("its fight has begun");
    }
    if (!gPendingTest && !Online::OneOnOne()) return EndPending("the online match ended");
    if (now - gPendingAt >= kPendingMs) return EndPending("no fight followed in time");
    CoverTick(now, state);
    KeepKodes(now, state);
    if (!StageFlow::Idle()) {  // both fighters are locked: the stage screen (shut, for a re-fight) shows its own lines
        if (gSelectSeen) gCardDone = true;
        gCardOn = false;
        ReleaseAuto();
        return;
    }
    if (state != kSelectState) {
        ReleaseAuto();
        // On the way from MK9's rematch screen to its character select, a re-fight that keeps its fighters is covered
        // already (not in a test, which may begin in any menu).
        if (gDirect && !gPendingTest && !gSelectSeen && now - gPendingAt < kCoverWaitMs) PublishCard(true, "THE SAME FIGHTERS, THE SAME ARENA");
        else CardOff();
        return;
    }
    if (!gSelectSeen) {
        gSelectSeen = true;
        gSelectTicks = 0;
        Logger::Info("REFIGHT the character select of the re-fight is up%s", gDirect ? "; the mod answers it for this player, under a cover" : "");
    }
    ++gSelectTicks;
    const bool driving = gDirect && !gAutoDone;  // the mod answers the select screen
    if (gCardDone) CardOff();  // both fighters are locked and no stage screen followed
    else if (driving) PublishCard(true, "THE SAME FIGHTERS, THE SAME ARENA");
    else PublishCard(false, gDirect ? "THE SAME FIGHTERS AND ARENA. PRESS CONFIRM TO GO ON." : gLastStage >= 0 ? "THE SAME ARENA AGAIN. CONFIRM YOUR FIGHTER." : "CONFIRM YOUR FIGHTER.");
    if (!driving) return;
    if (!gAutoHold) {
        if (Input::PanelHold()) return;  // another panel of the mod has the controls: tried again with the next tick
        Input::SetPanelHold(true);
        gAutoHold = true;
        gAutoPress.Reset();
        Logger::Info("REFIGHT the mod holds the controls on the character select and presses \"confirm\" for this player from tick %d on, every "
                     "%d ticks, until both fighters are locked (%d ticks at most)",
                     kAutoAfterTicks, RefightVote::kAgainTicks, kAutoMaxTicks);
    }
    if (gSelectTicks >= kAutoMaxTicks) {
        Logger::Info("REFIGHT the fighters were not locked within %d ticks of the character select (%d presses of the mod): the player has the "
                     "screen and the controls. The last fight's fighters are still what the game is given",
                     kAutoMaxTicks, gAutoPress.presses);
        ReleaseAuto();
        return;
    }
    uint32_t mask = 0;
    if (gSelectTicks >= kAutoAfterTicks) {
        gAutoPress.Accept();  // (once: a second call changes nothing)
        mask = gAutoPress.Tick();
    }
    Input::SetPanelPass(mask);
}
}  // namespace

namespace Refight {

void Tick() {
    if (!OnGameThread()) return;
    const uint64_t now = GetTickCount64();
    const uint32_t state = GameState();
    NoteKodes(state);
    NoteFight(now, state);
    Pending(now, state);
    QuietTest(now, state);
    // MK9's rematch screen has begun: the panel opens in the first tick that allows it.
    if (gWantOpen && gStep == Step::kIdle) {
        if (gMode != kModeOn || state == kSelectState || state == kLoadState) {
            gWantOpen = false;
        } else if (now - gBoxAt >= kOpenTryMs) {
            gWantOpen = false;
            Logger::Info("REFIGHT the panel did not open within %u s of MK9's rematch screen (%s): MK9's own box is all there is", kOpenTryMs / 1000,
                         gRefused ? gRefused : "no reason noted");
        } else if (Open("MK9's rematch screen has begun", true)) {
            gWantOpen = false;
            Logger::Info("REFIGHT the panel opened %u ms after MK9's rematch screen began", (uint32_t)(now - gBoxAt));
        }
    }
    if (gStep == Step::kIdle) return;
    if (Rollback::GgpoActive()) return Close("a rollback session started", now, false);
    if (gMode == kModeOn) {
        if (!Online::OneOnOne()) return Close("the online match ended", now, false);
        if (state != gOpenState) {
            if (state == kSelectState || state == kLoadState || state == kFightState) return Close("the game left the rematch screen", now, false);
            Logger::Info("REFIGHT the game's state went from %u to %u under the panel", gOpenState, state);
            gOpenState = state;
        }
    } else if (state == kFightState) {
        return Close("a fight began", now, false);
    }
    switch (gStep) {
    case Step::kVoting:
        ReadControls(now);
        SendNow(now);
        NoteOutcome();
        if (gMode == kModeOn) {
            if (gX.contextDiffers) return Close("the other mod offers other choices", now, true);
            if (!gX.theirs.nonce && now - gOpenedAt >= kSilenceMs) return Close("the opponent's mod did not answer", now, true);
            if (gX.theirs.nonce && now - gHeardAt >= kLostMs) return Close("the opponent's mod stopped answering", now, true);
            if (!gX.mine.locked || !gX.theirs.locked) gBothAt = 0;
            else if (!gBothAt) gBothAt = now;
            else if (now - gBothAt >= kBothLockedMs) return Close("both picks are in and MK9's own box has not ended (the presses did not reach it)", now, true);
        } else if (gOutcome != Vote::kPending) {
            SetStep(Step::kOver, now);  // a test has no box of MK9's that ends
        }
        break;
    case Step::kOver:
        gPrevMask = Input::PanelMask();
        SendNow(now);
        NoteOutcome();
        if (now - gStepAt >= (gMode == kModeTest ? kTestOverMs : kOverMs)) return Finish(now, gMode == kModeTest ? "the test run is over" : "MK9's rematch box is over");
        break;
    case Step::kLetGo:
        SendNow(now);
        gHandsOff = Input::PanelMask() ? 0 : gHandsOff + 1;
        if (gHandsOff >= 3 || ++gLetGoTicks >= 60) return Finish(now, "the player goes on with MK9's own box");
        break;
    case Step::kIdle:
        break;
    }
    // MK9's own box under the panel: the presses of this player's choice (none in a test, none once the box is over).
    Input::SetPanelPass(gMode == kModeOn && gStep == Step::kVoting ? gPress.Tick() : 0);
    Publish(now);
}

void OnPacket(const void* data, int size) {
    if (size <= 0 || !OnGameThread()) return;
    Packet p;
    if (Parse(data, (size_t)size, &p) != Parsed::kOk || p.kind != kKindAfterMatch) return;
    if (gMode != kModeOn || gStep == Step::kIdle || !gX.open) return;
    Heard(p, "the other game");
}

void OnNative(uint32_t id, uint32_t firstOperand) {
    if (gMode != kModeOn || !OnGameThread()) return;
    if (id == kBoxNative && firstOperand == 1) {
        // The panel opens from Tick, not from inside the script's call: the match's rollback session may still run in
        // this tick (it ends at the results state, which this very tick sets).
        if (gStep == Step::kIdle) {
            gWantOpen = true;
            ArenaPics::Request();  // the game's lettering and gold pieces, where the stage screen has not read them yet
            gBoxAt = GetTickCount64();
            gRefused = nullptr;
            gBoxOver = false;
            Logger::Info("REFIGHT MK9's rematch screen begins (script native 1350, game state %u, %s)", GameState(),
                         Rollback::GgpoActive() ? "the rollback session still runs in this tick" : "no rollback session");
        }
        return;
    }
    if (id == kBoxOverNative && gWantOpen) {
        gWantOpen = false;
        gBoxOver = true;
        Logger::Info("REFIGHT MK9's rematch box was over before the panel opened (%u ms after it began; %s)", (uint32_t)(GetTickCount64() - gBoxAt),
                     gRefused ? gRefused : "no try was refused");
        return;
    }
    if (id == kBoxOverNative && gStep == Step::kVoting) {
        // The two decisions are in on MK9's side: nothing more is pressed, the panel shows what the picks gave.
        const uint64_t now = GetTickCount64();
        gBoxOver = true;
        gPress.Reset();
        Input::SetPanelPass(0);
        NoteOutcome();
        if (gOutcome == Vote::kPending) snprintf(gNote, sizeof gNote, "%s", "DECIDED ON MK9'S OWN BOX");
        Logger::Info("REFIGHT MK9's rematch box is over (script native 1351) %u ms after the panel opened; the picks so far give: %s",
                     (uint32_t)(now - gOpenedAt), RefightVote::OutcomeText(gOutcome));
        SetStep(Step::kOver, now);
        return;
    }
    // Both fighters are locked on the character select: the mod's own presses there end before the stage screen's tap
    // (which runs next and, for a re-fight, shows its own two lines).
    if (id == kSettingsNative && gPending) {
        if (gAutoHold) {
            Logger::Info("REFIGHT both fighters are locked after %d ticks of the character select and %d presses of the mod: the presses end, "
                         "the controls go back",
                         gSelectTicks, gAutoPress.presses);
            ReleaseAuto();
        }
        if (gSelectSeen) gCardDone = true;
    }
}

bool Direct(const char* p1, const char* p2, const uint32_t* colours, const char* who) {
    if (!OnGameThread() || !p1 || !p2 || !RefightVote::FighterName(p1) || !RefightVote::FighterName(p2)) return false;
    const uint64_t now = GetTickCount64();
    if (gStep != Step::kIdle) Finish(now, who);
    EndPending(who);
    if (!ScriptRng::SetMatchPlayers(p1, p2, colours)) return false;
    ScriptRng::KeepMatchPlayersForTest();  // offline: no stage screen's exchange settles the pair, it stands by itself
    gPending = gPendingTest = gDirect = true;
    gPendingAt = now;
    gSelectSeen = gCardOn = gCardDone = gAutoHold = gAutoDone = gCoverUp = gLoadShown = gLoadNoPics = false;
    gKeptKodesOk = false;  // (an offline fight has no online session to give kodes to)
    gKodeWrites = 0;
    gKodeTheirsLogged = gKodeNoSessionLogged = gKodeCapLogged = false;
    gSelectTicks = 0;
    gPendingState = 0xFFFFFFFF;
    Logger::Info("REFIGHT direct (%s): the next fight a script sets up gets %.60s and %.60s (colours %s); on the next character select the "
                 "mod covers the screen and presses \"confirm\". Game state %u",
                 who, p1, p2, colours ? (colours[0] ? (colours[1] ? "1 and 1" : "1 and 0") : (colours[1] ? "0 and 1" : "0 and 0")) : "left as the script sets them",
                 GameState());
    return true;
}
void DirectEnd(const char* why) {
    if (gPendingTest) EndPending(why);
}
bool LastFightKodes(uint16_t* mine, uint16_t* theirs) {
    if (!gLastKodesOk) return false;
    *mine = gLastKodes.mine;
    *theirs = gLastKodes.theirs;
    return true;
}

bool Command(const char* line) {
    char verb[24] = "", arg[24] = "", p1[64] = "", p2[64] = "";
    int c1 = -1, c2 = -1;  // 'refight direct A B c1 c2': the two fighters' colours too (a look at one PC)
    if (!line || sscanf(line, "%23s %23s %63s %63s %d %d", verb, arg, p1, p2, &c1, &c2) < 1) return false;
    const uint64_t now = GetTickCount64();
    if (!strcmp(verb, "refight")) {
        if (!strcmp(arg, "open")) {
            Open("the command", false);
        } else if (!strcmp(arg, "close")) {
            if (gStep != Step::kIdle) Finish(now, "the command");
        } else if (!strcmp(arg, "direct")) {
            // A look at one PC, in any game: what a re-fight that keeps its fighters does from the choice on, without
            // the choice and without the other game. 'refight direct A B': the next time a script sets the fight's
            // fighters, the game is given A and B (as the game names them, "CHAR_Sektor_C"); without names, the two
            // of the fight the game last set up. On the next character select (game state 7) the mod covers the
            // screen, holds the controls and presses "confirm". 'refight direct 0': over.
            if (gStep != Step::kIdle) Finish(now, "a test of the fighters");
            EndPending("the command");
            if (strcmp(p1, "0") && OnGameThread()) {
                char a[64] = "", b[64] = "";
                const bool given = p1[0] && p2[0];
                if (given) snprintf(a, sizeof a, "%s", p1), snprintf(b, sizeof b, "%s", p2);
                else ScriptRng::FightPlayers(a, b, sizeof a);
                const bool names = RefightVote::FighterName(a) && RefightVote::FighterName(b);
                // Without names, the fight the game last set up is kept with its colours, as an online rematch keeps them;
                // with names, the colours named after them ('refight direct CHAR_JOHNNYCAGE_B CHAR_SEKTOR 1 0'), if any.
                uint32_t colours[2] = {};
                const bool named = given && c1 >= 0 && c2 >= 0;
                if (named) colours[0] = (uint32_t)c1, colours[1] = (uint32_t)c2;
                const bool coloured = named || (!given && ScriptRng::FightColours(colours));
                if (names && ScriptRng::SetMatchPlayers(a, b, coloured ? colours : nullptr)) {
                    // Offline no stage screen opens, whose exchange settles a re-fight's pair (StageFlow gives the
                    // game its select screen's fighters back at the lock): the test's pair stands by itself, so one
                    // PC shows the character select's cover, the load and the fight with the pair.
                    // (Not a command for an online match: the other game would load its select screen's fighters.)
                    const bool keep = true;
                    ScriptRng::KeepMatchPlayersForTest();
                    gPending = gPendingTest = gDirect = true;
                    gPendingAt = now;
                    gSelectSeen = gCardOn = gCardDone = gAutoHold = gAutoDone = gCoverUp = gLoadShown = gLoadNoPics = false;
                    gKeptKodes = gLastKodes;
                    gKeptKodesOk = gLastKodesOk;
                    gKodeWrites = 0;
                    gKodeTheirsLogged = gKodeNoSessionLogged = gKodeCapLogged = false;
                    gSelectTicks = 0;
                    gPendingState = 0xFFFFFFFF;
                    Logger::Info("REFIGHT direct (test): the next fight a script sets up gets %.60s and %.60s (%s; colours %s); on the next "
                                 "character select the mod covers the screen and presses \"confirm\". Game state %u, %u calls of the fighter "
                                 "setter so far; %s",
                                 a, b, given ? "named by the command" : "the fight the game last set up",
                                 coloured ? (colours[0] ? (colours[1] ? "1 and 1" : "1 and 0") : (colours[1] ? "0 and 1" : "0 and 0"))
                                          : "left as the script sets them",  // (any value above 0 is written as given, shown as 1)
                                 GameState(), ScriptRng::PlayerCalls(),
                                 keep ? "a test: the pair stands without a stage screen" : "the stage screen settles whether the pair stands");
                } else {
                    Logger::Info("REFIGHT direct (test) NOT begun: %s", !names ? "two fighters' names are needed (\"CHAR_Sektor_C char_raiden\"), or a fight the game has set up"
                                                                                : "the game's code for setting the fighters is not the one this mod knows");
                }
            }
        } else if (!strcmp(arg, "test") || !strcmp(arg, "0") || !strcmp(arg, "1")) {
            if (gStep != Step::kIdle) Finish(now, "the mode was changed");
            EndPending("the mode was changed");
            gMode = !strcmp(arg, "test") ? kModeTest : atoi(arg) ? kModeOn : kModeOff;
            Logger::Info("REFIGHT %s (%s)", arg,
                         gMode == kModeTest ? "a simulated opponent; 'refight open' shows the panel, nothing is pressed on the game's screen"
                         : gMode == kModeOn ? "the panel opens by itself on MK9's rematch screen in an online one-on-one match between two mods of this build"
                         : "off: MK9's own rematch box only");
        } else {
            Logger::Info("REFIGHT usage: refight 0|1|test|open|close|direct [A B|0]");
        }
        return true;
    }
    if (!strcmp(verb, "refightauto")) {
        gAuto = atoi(arg) != 0;
        Logger::Info("REFIGHT auto %d (%s; it takes effect with the next choice after a match, and both players need the same)", gAuto ? 1 : 0,
                     gAuto ? "a re-fight both chose keeps its fighters: the mod answers the character select for the player, under a cover"
                           : "each player confirms the fighter on the character select of a re-fight; only the arena is kept");
        return true;
    }
    if (!strcmp(verb, "refightkodes")) {
        gKeepKodes = atoi(arg) != 0;
        Logger::Info("REFIGHT kodes %d (%s)", gKeepKodes ? 1 : 0,
                     gKeepKodes ? "a rematch loads with the last fight's Kombat Kodes, locked"
                                : "a rematch's versus screen starts at 0-0-0 and each player enters a kode there, as before any fight");
        return true;
    }
    if (!strcmp(verb, "refightquiet") && atoi(arg) >= 100) {
        gQuietTestUntil = now + (uint64_t)(atoi(arg) > 120000 ? 120000 : atoi(arg));
        return true;
    }
    if (!strcmp(verb, "refightquiet")) {
        gQuiet = atoi(arg) != 0;
        if (!gQuiet) QuietEnd("'refightquiet 0'");
        Logger::Info("REFIGHT quiet %d (%s)", gQuiet ? 1 : 0,
                     gQuiet ? "the hidden character select of a re-fight is not heard: no sound is started while the cover is up on it"
                            : "the hidden character select of a re-fight is heard, as in the release of 2026-11-14 18:18");
        return true;
    }
    if (!strcmp(verb, "refightload")) {
        // 'refightload 0|1': MK9's versus / loading screen of a re-fight is hidden / seen (where it shows the re-fight's
        // fighters). 'refightload <ms>' (100 to 10000): seen, and the cover stays that long into the load. This PC's
        // picture alone: the fight, its fighters and its kodes are the same either way, and the two players need not
        // hold the same.
        const int n = atoi(arg);
        if (n >= 100) gLoadShow = true, gLoadMs = (uint32_t)(n > 10000 ? 10000 : n);
        else gLoadShow = n != 0;
        Logger::Info("REFIGHT load %d (%s; the cover stays %u ms into the load with 1; this PC's picture alone, from the next re-fight's load on)",
                     gLoadShow ? 1 : 0,
                     gLoadShow ? "a re-fight shows MK9's versus / loading screen with its Kombat Kodes where that screen shows the re-fight's "
                                 "fighters; only the character select is hidden"
                               : "a re-fight is covered until its fight begins, as in the releases of 2026-11-14 18:18 to 2026-11-15 20:57",
                     gLoadMs);
        return true;
    }
    if (!strcmp(verb, "refightpics")) {
        // 'refightpics 0|1': the two pictures MK9 keeps for a re-fight's versus / loading screen are the select
        // screen's own / the re-fight's fighters' (ScriptRng::MatchPictures). With 0 the cover stays until the fight.
        ScriptRng::SetMatchPictures(atoi(arg) != 0);
        Logger::Info("REFIGHT pictures %d (%s; this PC's picture alone, from the next re-fight on)", ScriptRng::MatchPicturesOn() ? 1 : 0,
                     ScriptRng::MatchPicturesOn()
                         ? "the pictures of a re-fight's versus / loading screen are made of the re-fight's fighters"
                         : "the pictures of a re-fight's versus / loading screen are the select screen's, and the cover stays over that screen");
        return true;
    }
    if (!strcmp(verb, "refightjump")) {
        // A look at one PC (2026-11-15): MK9's own "go to this script mode" (0x00495A10, cdecl (module, function); script
        // native 171 calls it). MK9's scripts change screens with it: the online rematch screen ends with
        // ("PlayerSelect.mko", "PlayerSelect"), the character select starts the fight with ("versus.mko", "versus_mode"),
        // and versus.mko asks for ("versus.mko", "versus_mode") itself for an offline rematch, with no character select.
        // The routine only notes the request; the engine acts on it in its own tick (0x0080F673).
        // 'refightjump arm M F': a script's next request for the character select becomes a request for M / F
        // (ScriptRng::SetModeJump; every request of a script is logged from then on); 'refightjump arm 0': not any more.
        if (!strcmp(arg, "arm")) {
            const bool off = !p1[0] || !strcmp(p1, "0");
            const bool ok = off ? ScriptRng::SetModeJump(nullptr, nullptr) : ScriptRng::SetModeJump(p1, p2);
            if (off) Logger::Info("REFIGHT jump: no request for the character select is turned any more");
            else if (ok) Logger::Info("REFIGHT jump (a test): a script's next request for the character select becomes one for %s / %s (game state %u)", p1, p2, GameState());
            else Logger::Info("REFIGHT jump: not armed ('refightjump arm <module.mko> <function>'; or the game's call could not be patched)");
            return true;
        }
        if (!arg[0] || !p1[0]) {
            Logger::Info("REFIGHT jump: 'refightjump <module.mko> <function>', for example 'refightjump versus.mko versus_mode'");
            return true;
        }
        Logger::Info("REFIGHT jump (a test): the game is asked for the script mode %s / %s (game state %u)", arg, p1, GameState());
        reinterpret_cast<void(__cdecl*)(const char*, const char*)>(0x00495A10)(arg, p1);
        return true;
    }
    if (!strcmp(verb, "refightstatus")) {
        char mine[6], theirs[6];
        RefightVote::KodeText(gLastKodes.mine, mine);
        RefightVote::KodeText(gLastKodes.theirs, theirs);
        const uint32_t pics = ScriptRng::MatchPictures();
        Logger::Info("REFIGHT status: quiet %d (%s; the last one held back %u sound(s)), load %d (the versus / loading screen of a re-fight is %s; "
                     "cover %u ms into the load; %s), pictures %d (of the pair that stands now: left %s, right %s; %u copies asked for), the last "
                     "online fight's Kombat Kodes %s%s%s%s, %d write(s) of them in the last re-fight's load (kodes %d: %s); the last online "
                     "fight's colours %u and %u (%s)",
                     gQuiet ? 1 : 0, gQuietOn ? "on now" : "off now", gQuietHeld, gLoadShow ? 1 : 0, gLoadShow ? "seen" : "hidden", gLoadMs,
                     gCoverUp ? "the cover is up now" : gLoadShown ? "given up for this load" : "no cover now", ScriptRng::MatchPicturesOn() ? 1 : 0,
                     pics & 1 ? "made" : "not made", pics & 2 ? "made" : "not made", ScriptRng::PictureCalls(), gLastKodesOk ? mine : "not known",
                     gLastKodesOk ? " (this player) and " : "", gLastKodesOk ? theirs : "", gLastKodesOk ? " (the other player)" : "", gKodeWrites,
                     gKeepKodes ? 1 : 0, gKeepKodes ? "kept, locked" : "the players'", gLastColours[0], gLastColours[1],
                     gLastColoursOk ? "read" : "not read");
        const char* name = gLastStage >= 0 ? ScriptRng::StageAt((uint32_t)gLastStage) : nullptr;
        Logger::Info("REFIGHT status: mode %s, auto %d, step %s, this player %s, the other %s, MK9's box %s, presses %d, re-fight under way %s%s%s, "
                     "the last online fight's arena entry %d %.40s, its fighters %.60s and %.60s; game state %u, the game was given a re-fight's "
                     "fighters %s, %u calls of its fighter setter, select screen ticks %d, presses there %d",
                     gMode == kModeTest ? "test" : gMode == kModeOn ? "1" : "0", gAuto ? 1 : 0,
                     gStep == Step::kIdle ? "idle" : gStep == Step::kVoting ? "choosing" : gStep == Step::kOver ? "over" : "letting go",
                     gX.mine.locked ? RefightVote::ChoiceText(gX.mine.choice) : "no pick", gX.theirs.locked ? RefightVote::ChoiceText(gX.theirs.choice) : "no pick",
                     gBoxOver ? "over" : "not over", gPress.presses, gPending ? "yes" : "no", gPending && gDirect ? " (keeps its fighters)" : "",
                     gPending && gPendingTest ? " (a test)" : "", gLastStage, name ? name : "", gLastTag ? gLastP1 : "?", gLastTag ? gLastP2 : "?",
                     GameState(), ScriptRng::MatchPlayersWritten() ? "yes" : "no", ScriptRng::PlayerCalls(), gSelectTicks, gAutoPress.presses);
        return true;
    }
    return false;
}

bool Active() { return gStep != Step::kIdle || gWantOpen; }

void Shutdown() {
    gWantOpen = false;
    gQuietOn = false;
    SoundDedupe::QuietOff();
    ReleaseAuto();
    ScriptRng::SetMatchPlayers(nullptr, nullptr);
    if (gStep != Step::kIdle) {
        Input::SetPanelPass(0);
        if (Input::PanelHold()) Input::SetPanelHold(false);
        gStep = Step::kIdle;
    }
}

}  // namespace Refight
