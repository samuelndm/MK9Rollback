#include "Refight.h"

#include <windows.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "../debug/ArenaPics.h"
#include "../debug/Logger.h"
#include "../debug/StageScreen.h"
#include "../game/Input.h"
#include "../game/MK9Game.h"
#include "../hook/ObjectHooks.h"
#include "../rollback/RollbackManager.h"
#include "../rollback/ScriptRng.h"
#include "FlowTrace.h"
#include "OnlineMode.h"
#include "RefightVote.h"
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
constexpr int kAutoAfterTicks = 90;     // the character select has been up this long before the mod confirms a fighter
constexpr int kAutoPresses = 3;

int gMode = kModeOn;
bool gAuto = false;  // 'refightauto'
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

// The last online fight's arena, as a stage table entry (-1: none seen).
int gLastStage = -1;
bool gFightNoted = false;
uint64_t gFightTryAt = 0;

// A re-fight both players chose, on its way through the character select to the next fight.
bool gPending = false, gSelectSeen = false, gCardOn = false, gCardDone = false, gAutoHold = false, gAutoDone = false;
uint64_t gPendingAt = 0;
int gSelectTicks = 0;
RefightVote::Presser gAutoPress;

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
    StageScreen::Publish(s);
}

void SetStep(Step s, uint64_t now) {
    gStep = s;
    gStepAt = now;
}

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
    gSelectSeen = gCardOn = gCardDone = gAutoHold = gAutoDone = false;
    gSelectTicks = 0;
    StageFlow::SetRefight(gLastStage);
    const char* name = gLastStage >= 0 ? ScriptRng::StageAt((uint32_t)gLastStage) : nullptr;
    Logger::Info("REFIGHT both players chose the same fight again and both games know it: on the character select %s, and the stage screen "
                 "%s%.40s",
                 gAuto ? "the mod confirms this player's fighter ('refightauto 1')" : "each player confirms the fighter",
                 gLastStage >= 0 ? "stays shut, this player's pick is the last arena, " : "opens as after any match (the last fight's arena is not known)",
                 name ? name : "");
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
    // A re-fight that never reached its fight is over with this new choice.
    if (gPending) {
        gPending = false;
        StageFlow::SetRefight(-1);
    }
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
             o == Vote::kRefight ? "RE-FIGHT: THE SAME FIGHTERS, THE SAME ARENA" : o == Vote::kChange ? "BACK TO THE CHARACTER SELECT"
             : o == Vote::kLeave ? (iLeave ? "LEAVING" : "THE OPPONENT LEAVES") : "");
    if (o != Vote::kPending) Logger::Info("REFIGHT the two choices give: %s", RefightVote::OutcomeText(o));
}

// The last online fight's arena, read once per fight from the game's own name of it.
void NoteFight(uint64_t now, uint32_t state) {
    if (state != kFightState) {
        gFightNoted = false;
        return;
    }
    if (gFightNoted || now < gFightTryAt || !Online::OneOnOne()) return;
    gFightTryAt = now + 500;
    char name[80] = "";
    if (!FlowTrace::FightStageName(name, sizeof name)) return;
    gFightNoted = true;
    gLastStage = ScriptRng::StageIndex(name);
    Logger::Info("REFIGHT this online fight's arena is %.60s (stage table entry %d): what a re-fight would use again", name, gLastStage);
}

void ReleaseAuto() {
    if (!gAutoHold) return;
    gAutoHold = false;
    gAutoDone = true;
    gAutoPress.Reset();
    Input::SetPanelPass(0);
    if (Input::PanelHold()) Input::SetPanelHold(false);
}

void EndPending(const char* why) {
    if (!gPending) return;
    gPending = false;
    ReleaseAuto();
    if (gCardOn && StageFlow::Idle()) PublishOff();
    gCardOn = false;
    StageFlow::SetRefight(-1);
    Logger::Info("REFIGHT the re-fight both players chose is over: %s", why);
}

// A re-fight both chose: two lines on the character select until both fighters are locked (the stage screen's own two
// lines follow), and with 'refightauto 1' the "confirm" presses for this player.
void Pending(uint64_t now, uint32_t state) {
    if (!gPending) return;
    // (Only after the character select: the state under MK9's rematch screen is not known to differ from a fight's.)
    if (state == kFightState && gSelectSeen) return EndPending("its fight has begun");
    if (!Online::OneOnOne()) return EndPending("the online match ended");
    if (now - gPendingAt >= kPendingMs) return EndPending("no fight followed in time");
    if (!StageFlow::Idle()) {  // both fighters are locked: the stage screen (shut, for a re-fight) shows its own lines
        if (gSelectSeen) gCardDone = true;
        gCardOn = false;
        ReleaseAuto();
        return;
    }
    if (state != kSelectState) {
        if (gCardOn) {
            gCardOn = false;
            PublishOff();
        }
        ReleaseAuto();
        return;
    }
    if (!gSelectSeen) {
        gSelectSeen = true;
        gSelectTicks = 0;
        Logger::Info("REFIGHT the character select of the re-fight is up%s", gAuto ? "; the mod will confirm this player's fighter" : "");
    }
    ++gSelectTicks;
    if (!gCardDone) {
        gView = StageScreen::State{};
        gView.phase = StageScreen::kCard;
        snprintf(gView.name[0], sizeof gView.name[0], "RE-FIGHT");
        snprintf(gView.note, sizeof gView.note, "%s%s", gLastStage >= 0 ? "THE SAME ARENA AGAIN. " : "",
                 gAuto && !gAutoDone ? "YOUR FIGHTER IS CONFIRMED FOR YOU." : "CONFIRM YOUR FIGHTER.");
        StageScreen::Publish(gView);
        gCardOn = true;
    }
    if (!gAuto || gAutoDone) return;
    if (!gAutoHold) {
        if (gSelectTicks < kAutoAfterTicks || Input::PanelHold()) return;
        Input::SetPanelHold(true);
        gAutoHold = true;
        gAutoPress.Reset();
        gAutoPress.Accept();
        Logger::Info("REFIGHT 'refightauto 1': the mod presses \"confirm\" on the character select for this player (%d presses at most)", kAutoPresses);
    }
    const uint32_t mask = gAutoPress.Tick();
    Input::SetPanelPass(mask);
    if (gAutoPress.presses >= kAutoPresses && !mask) {
        Logger::Info("REFIGHT the mod's presses on the character select are done: the player has the controls again");
        ReleaseAuto();
    }
}
}  // namespace

namespace Refight {

void Tick() {
    if (!OnGameThread()) return;
    const uint64_t now = GetTickCount64();
    const uint32_t state = GameState();
    NoteFight(now, state);
    Pending(now, state);
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
    // Both fighters are locked on the character select: the mod's own presses there end before the stage screen's tap.
    if (id == kSettingsNative && gAutoHold) {
        Logger::Info("REFIGHT both fighters are locked: the mod's presses on the character select end");
        ReleaseAuto();
    }
}

bool Command(const char* line) {
    char verb[24] = "", arg[24] = "";
    if (!line || sscanf(line, "%23s %23s", verb, arg) < 1) return false;
    const uint64_t now = GetTickCount64();
    if (!strcmp(verb, "refight")) {
        if (!strcmp(arg, "open")) {
            Open("the command", false);
        } else if (!strcmp(arg, "close")) {
            if (gStep != Step::kIdle) Finish(now, "the command");
        } else if (!strcmp(arg, "test") || !strcmp(arg, "0") || !strcmp(arg, "1")) {
            if (gStep != Step::kIdle) Finish(now, "the mode was changed");
            EndPending("the mode was changed");
            gMode = !strcmp(arg, "test") ? kModeTest : atoi(arg) ? kModeOn : kModeOff;
            Logger::Info("REFIGHT %s (%s)", arg,
                         gMode == kModeTest ? "a simulated opponent; 'refight open' shows the panel, nothing is pressed on the game's screen"
                         : gMode == kModeOn ? "the panel opens by itself on MK9's rematch screen in an online one-on-one match between two mods of this build"
                         : "off: MK9's own rematch box only");
        } else {
            Logger::Info("REFIGHT usage: refight 0|1|test|open|close");
        }
        return true;
    }
    if (!strcmp(verb, "refightauto")) {
        gAuto = atoi(arg) != 0;
        Logger::Info("REFIGHT auto %d (%s)", gAuto ? 1 : 0,
                     gAuto ? "on the character select of a re-fight both chose, the mod presses \"confirm\" for this player"
                           : "each player confirms the fighter on the character select of a re-fight");
        return true;
    }
    if (!strcmp(verb, "refightstatus")) {
        const char* name = gLastStage >= 0 ? ScriptRng::StageAt((uint32_t)gLastStage) : nullptr;
        Logger::Info("REFIGHT status: mode %s, auto %d, step %s, this player %s, the other %s, MK9's box %s, presses %d, re-fight under way %s, "
                     "the last online fight's arena entry %d %.40s",
                     gMode == kModeTest ? "test" : gMode == kModeOn ? "1" : "0", gAuto ? 1 : 0,
                     gStep == Step::kIdle ? "idle" : gStep == Step::kVoting ? "choosing" : gStep == Step::kOver ? "over" : "letting go",
                     gX.mine.locked ? RefightVote::ChoiceText(gX.mine.choice) : "no pick", gX.theirs.locked ? RefightVote::ChoiceText(gX.theirs.choice) : "no pick",
                     gBoxOver ? "over" : "not over", gPress.presses, gPending ? "yes" : "no", gLastStage, name ? name : "");
        return true;
    }
    return false;
}

bool Active() { return gStep != Step::kIdle || gWantOpen; }

void Shutdown() {
    gWantOpen = false;
    ReleaseAuto();
    if (gStep != Step::kIdle) {
        Input::SetPanelPass(0);
        if (Input::PanelHold()) Input::SetPanelHold(false);
        gStep = Step::kIdle;
    }
}

}  // namespace Refight
