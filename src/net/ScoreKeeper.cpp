#include "ScoreKeeper.h"

#include <windows.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwchar>

#include "../debug/Logger.h"
#include "../debug/ScoreView.h"
#include "../game/Fighter.h"
#include "../game/MK9Game.h"
#include "../hook/GameLoopHook.h"
#include "../rollback/RollbackManager.h"
#include "../rollback/ScriptRng.h"
#include "FlowTrace.h"
#include "OnlineMode.h"
#include "Score.h"

// (Score.h's FormatEntry, BoardOf and the rest are the host-tested part; this file is what reads the game and the file
// and writes the file and the log.)

// The score of an evening's online matches and the history file (Score.h says how a match's winner is read).
namespace ScoreKeeper {
namespace {
using Score::Judge;
using Score::Tally;

constexpr uint32_t kSelectState = 7, kLoadState = 1, kFightState = 8, kResultsState = 9, kFinisherState = 0xB, kPreLoadState = 12;
constexpr uint32_t kOneOnOneLookMs = 500;  // in a fight, how often the match is looked at until it is known to be one-on-one
constexpr uint32_t kNamesLookMs = 500;     // and the fighters' and the arena's names, until read
constexpr uint32_t kTestIdleMs = 6000;     // 'score test': no fight update this long after a fight: the match is over
constexpr uint32_t kWriteWaitMs = 30000;   // a decided match waits this long at most for the session to end before its line is written

bool gOn = true;      // 'score 0|1'
bool gTest = false;   // 'score test 0|1': every fight counts, offline too, into match_history_test.txt (a look at one PC)
wchar_t gPath[MAX_PATH] = L"", gTestPath[MAX_PATH] = L"";

// The match being read.
bool gInMatch = false;
bool gTestMatch = false;    // begun in test mode
Judge gJudge;
int gSide = 0;              // this player's fighter, 1 or 2 (0 not known yet)
uint64_t gOpponent = 0;
char gFighter[2][Score::kNameCap] = {}, gArena[Score::kNameCap] = "";
bool gNamesRead = false, gArenaRead = false;
ULONGLONG gNamesAt = 0, gOneOnOneAt = 0, gLastFightAt = 0;

// A decided match whose line waits for the session to end (no file work while a rollback session runs).
bool gPending = false;
Score::Entry gPendingEntry;
bool gPendingTest = false;
ULONGLONG gPendingAt = 0;

// The room's board (ScoreKeeper.h): in memory only, each counted match added at once (so the choice after the match
// shows it before its line is written). Every match of the room is "today" in it (Board::Add(e, true)).
Score::Board gBoard;
bool gBoardTest = false;        // gBoard is a 'score test' room (opponent 0)
uint32_t gRoomEnds = 0;         // Online::MatchEnds() when the room began
bool gRoomOver = false;         // a player chose LEAVE after the room's last match
int gRooms = 0;                 // rooms begun in this game

// This game's score against the last counted match's opponent (the log's "this game" numbers).
uint64_t gTallyOpponent = 0;
bool gHaveTally = false;
Tally gSession;
int gCounted = 0, gNotCounted = 0;
int gWriteFailures = 0;

uint32_t GameState() {
    const uint32_t gi = *reinterpret_cast<const volatile uint32_t*>(MK9Game::Get().gameInfoPtr);
    return gi ? *reinterpret_cast<const volatile uint32_t*>(gi + 0x4) : 0xFFFFFFFF;
}
uint32_t GameFrame() {
    const uint32_t gi = *reinterpret_cast<const volatile uint32_t*>(MK9Game::Get().gameInfoPtr);
    return gi ? *reinterpret_cast<const volatile uint32_t*>(gi + MK9Game::kGameInfoGameFrame) : 0;
}
bool SessionRuns() { return Rollback::NetplayRunning() || Rollback::NetplayStarting() || Rollback::gIsResimulating; }

// The room's board is this opponent's, and the players have not been apart since its last match.
bool SameRoom(uint64_t opponent, bool test) {
    return gBoard.on && !gRoomOver && gBoardTest == test && gBoard.opponent == opponent && (test || gRoomEnds == Online::MatchEnds());
}
// Before a counted match's numbers are shown or added: a new room where the last one is over.
void EnsureRoom(uint64_t opponent, bool test) {
    if (SameRoom(opponent, test)) return;
    const char* why = !gBoard.on                    ? "the first counted match in this game"
                      : gBoardTest != test          ? "'score test' changed"
                      : gRoomOver                   ? "a player chose LEAVE after the last match"
                      : gBoard.opponent != opponent ? "another opponent"
                                                    : "MK9's online match had ended since the last match (the players were apart)";
    if (gBoard.on)
        Logger::Info("SCORE room %d is over (%d won, %d lost, %d drawn in %d match(es)): %s", gRooms, gBoard.today.wins, gBoard.today.losses,
                     gBoard.today.draws, gBoard.today.Matches(), why);
    gBoard = Score::Board{};
    gBoard.on = 1;
    gBoard.allKnown = 1;
    gBoard.opponent = opponent;
    gBoardTest = test;
    gRoomEnds = Online::MatchEnds();
    gRoomOver = false;
    ++gRooms;
    Logger::Info("SCORE room %d begins (%s): the score, the last matches and the fighters start at nothing; every match is still written to "
                 "match_history.txt",
                 gRooms, test ? "'score test'" : opponent ? "this opponent" : "the opponent is not known");
}

void StartMatch(ULONGLONG now, bool test) {
    if (gInMatch && gJudge.frames > 60) {
        ++gNotCounted;
        Logger::Info("SCORE: the last match (%u frames read) ended without MK9's results screen: not counted", (unsigned)gJudge.frames);
    }
    gJudge.Start();
    gInMatch = true;
    gTestMatch = test;
    gSide = 0;
    gOpponent = 0;
    gFighter[0][0] = gFighter[1][0] = gArena[0] = 0;
    gNamesRead = gArenaRead = false;
    gNamesAt = 0;
    gLastFightAt = now;
    Logger::Info("SCORE: a %s fight began: the two fighters' lives are read until the match is over", test ? "fight ('score test')" : "one-on-one online");
    if (test) EnsureRoom(0, true);  // (an online match's room is settled once its opponent is known: SampleFight)
}

void ReadNames(ULONGLONG now) {
    if ((gNamesRead && gArenaRead) || now < gNamesAt) return;
    gNamesAt = now + kNamesLookMs;
    if (!gNamesRead) {
        char a[Score::kNameCap] = "", b[Score::kNameCap] = "";
        if (ScriptRng::FightPlayers(a, b, sizeof a)) {
            snprintf(gFighter[0], sizeof gFighter[0], "%s", a);
            snprintf(gFighter[1], sizeof gFighter[1], "%s", b);
            gNamesRead = true;
        }
    }
    if (!gArenaRead) {
        char s[80] = "";
        if (FlowTrace::FightStageName(s, sizeof s)) {
            snprintf(gArena, sizeof gArena, "%s", s);
            gArenaRead = true;
        }
    }
}

// One tick of a fight: the lives at this game frame, this player's side and the opponent as soon as they are known.
void SampleFight(ULONGLONG now) {
    gLastFightAt = now;
    if (!gSide) gSide = gTestMatch ? 1 : Online::LocalPlayer();
    if (!gOpponent && !gTestMatch && (gOpponent = Online::Opponent()) != 0) EnsureRoom(gOpponent, false);  // the fight's numbers from its start
    ReadNames(now);
    const Fighter::State p1 = Fighter::Read(0), p2 = Fighter::Read(1);
    if (!p1.valid || !p2.valid) return;
    const int round = gJudge.Sample(GameFrame(), p1.life, p2.life);
    if (round)
        Logger::Info("SCORE: round %d over: %s (lives %.3f and %.3f; rounds so far %d - %d, %d drawn)", gJudge.rounds[0] + gJudge.rounds[1] + gJudge.drawn,
                     round == 3 ? "drawn" : round == 1 ? "player 1 won it" : "player 2 won it", gJudge.life[0], gJudge.life[1], gJudge.rounds[0],
                     gJudge.rounds[1], gJudge.drawn);
}

void LocalTime(char* out, size_t n) {
    SYSTEMTIME t;
    GetLocalTime(&t);
    snprintf(out, n, "%04u-%02u-%02u %02u:%02u:%02u", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
}

// Appends the line (a new file gets its header first). False: not written.
bool Append(const wchar_t* path, const char* line) {
    FILE* f = _wfopen(path, L"ab");
    if (!f) return false;
    bool ok = true;
    if (fseek(f, 0, SEEK_END) == 0 && ftell(f) == 0) ok = fputs(Score::FileHeader(), f) >= 0;
    ok = ok && fputs(line, f) >= 0;
    ok = (fclose(f) == 0) && ok;
    return ok;
}

void WritePending(const char* how) {
    gPending = false;
    const wchar_t* path = gPendingTest ? gTestPath : gPath;
    char line[400];
    if (!path[0] || !Score::FormatEntry(gPendingEntry, line, sizeof line)) return;
    if (!Append(path, line)) {
        const DWORD e = GetLastError();
        if (gWriteFailures++ < 4) Logger::Info("SCORE: %ls could not be written (error %lu): this match is in the score until the game closes, not in the history", path, e);
        return;
    }
    Logger::Info("SCORE: the match's line is written to %ls%s", path, how);
}

void FinishMatch(const char* where) {
    gInMatch = false;
    const Judge::Final f = gJudge.Finish(gSide);
    if (f.result == Score::kUndecided) {
        ++gNotCounted;
        Logger::Info("SCORE: the match is over (%s) but not counted: %s (lives %.3f and %.3f, unchanged for %d frames, %u frames read, "
                     "this player's side %d)",
                     where, f.why, gJudge.life[0], gJudge.life[1], gJudge.still, (unsigned)gJudge.frames, gSide);
        return;
    }
    ++gCounted;
    if (!gHaveTally || gOpponent != gTallyOpponent) {  // another opponent than the last match's: this game's score starts over
        gHaveTally = true;
        gTallyOpponent = gOpponent;
        gSession = Tally{};
    }
    gSession.Add(f.result);
    Score::Entry& e = gPendingEntry;
    e = Score::Entry{};
    LocalTime(e.time, sizeof e.time);
    e.result = f.result;
    e.roundsKnown = f.roundsKnown;
    e.mine = f.mine;
    e.theirs = f.theirs;
    snprintf(e.myFighter, sizeof e.myFighter, "%s", gNamesRead ? gFighter[gSide - 1] : "?");
    snprintf(e.theirFighter, sizeof e.theirFighter, "%s", gNamesRead ? gFighter[2 - gSide] : "?");
    snprintf(e.arena, sizeof e.arena, "%s", gArenaRead ? gArena : "?");
    e.opponent = gOpponent;
    e.side = gSide;
    // The room's board at once (the choice after the match opens now).
    EnsureRoom(gOpponent, gTestMatch);
    gBoard.Add(e, true);
    char rounds[16];
    if (f.roundsKnown) snprintf(rounds, sizeof rounds, "%d-%d", f.mine, f.theirs);
    else snprintf(rounds, sizeof rounds, "not known");
    Logger::Info("SCORE: the match is over (%s): this player %s, rounds %s (%d - %d - %d drawn read before the last), %s against %s on %s "
                 "(lives at the end %.3f and %.3f; side %d, opponent %s); this game's score against this opponent: %d won, %d lost, %d drawn; "
                 "room %d: %d - %d - %d, %s %d in a row",
                 where, Score::ResultText(f.result), rounds, gJudge.rounds[gSide - 1], gJudge.rounds[2 - gSide], gJudge.drawn, e.myFighter,
                 e.theirFighter, e.arena, gJudge.life[0], gJudge.life[1], gSide, gOpponent ? "known" : "not known", gSession.wins, gSession.losses,
                 gSession.draws, gRooms, gBoard.today.wins, gBoard.today.losses, gBoard.today.draws,
                 gBoard.streak > 0 ? "this player" : gBoard.streak < 0 ? "the opponent" : "nobody", gBoard.streak < 0 ? -gBoard.streak : gBoard.streak);
    gPending = true;
    gPendingTest = gTestMatch;
    gPendingAt = GetTickCount64();
}

// The fight's numbers (debug\ScoreView.h): in a counted match's fight, once this player's side and the room of this
// opponent are known.
void PublishBadge(uint32_t state) {
    ScoreView::Badge b;
    const bool fight = gOn && gInMatch && state == kFightState && gSide;
    const bool ours = fight && SameRoom(gTestMatch ? 0 : gOpponent ? gOpponent : Online::Opponent(), gTestMatch);
    if (fight && ours) {
        b.on = 1;
        b.mySide = (uint8_t)gSide;
        for (int which = 1; which <= 2; ++which) {
            const int w = Score::WinsOf(gBoard, gSide, which), r = Score::RunOf(gBoard, gSide, which);
            b.wins[which - 1] = (uint16_t)(w < 0 ? 0 : w > 9999 ? 9999 : w);
            b.run[which - 1] = (uint16_t)(r < 0 ? 0 : r > 999 ? 999 : r);
        }
    }
    ScoreView::Publish(b);
}

void Step(ULONGLONG now, uint32_t state) {
    // The line of a decided match: once no rollback session runs (MK9's results screen comes after the session's end).
    if (gPending) {
        if (!SessionRuns()) WritePending("");
        else if (now - gPendingAt > kWriteWaitMs) WritePending(" (a rollback session was still running)");
    }
    if (gTest) {
        // Any fight with a fight update in the last tick (offline too). A new match begins with both lives full.
        if (GameLoopHook::FoughtThisTick()) {
            if (!gInMatch) {
                const Fighter::State p1 = Fighter::Read(0), p2 = Fighter::Read(1);
                if (!p1.valid || !p2.valid || !Judge::Full(p1.life, p2.life)) return;
                StartMatch(now, true);
            }
            SampleFight(now);
        } else if (gInMatch && now - gLastFightAt >= kTestIdleMs) {
            FinishMatch("'score test': no fight update for 6 s");
        }
        return;
    }
    if (state == kFightState || state == kFinisherState) {
        if (!gInMatch) {
            if (state != kFightState || now < gOneOnOneAt) return;
            gOneOnOneAt = now + kOneOnOneLookMs;
            if (!Online::OneOnOne()) return;
            StartMatch(now, false);
        }
        SampleFight(now);
    } else if (state == kResultsState) {
        if (gInMatch) FinishMatch("MK9's results screen");
    } else if (state == kSelectState || state == kLoadState || state == kPreLoadState) {
        gOneOnOneAt = 0;
        if (gInMatch && gJudge.frames > 60) {
            ++gNotCounted;
            Logger::Info("SCORE: the match (%u frames read) left the fight for the character select or a load without MK9's results "
                         "screen: not counted",
                         (unsigned)gJudge.frames);
        }
        gInMatch = false;
    }
}
}  // namespace

void SetFolder(const wchar_t* dir) {
    if (!dir || !*dir) return;
    swprintf(gPath, MAX_PATH, L"%ls\\match_history.txt", dir);
    swprintf(gTestPath, MAX_PATH, L"%ls\\match_history_test.txt", dir);
}

void Tick() {
    const uint32_t state = GameState();
    if (gOn) Step(GetTickCount64(), state);
    PublishBadge(state);
}

bool FillBoard(Score::Board* out) {
    if (!out) return false;
    *out = Score::Board{};
    if (!gOn || !gBoard.on) return false;
    *out = gBoard;
    return true;
}

void RoomOver(const char* why) {
    if (!gBoard.on || gRoomOver) return;
    gRoomOver = true;
    Logger::Info("SCORE room %d ends: %s (%d won, %d lost, %d drawn in %d match(es)); its board stays on this panel, the next match starts a "
                 "new room",
                 gRooms, why ? why : "a player left", gBoard.today.wins, gBoard.today.losses, gBoard.today.draws, gBoard.today.Matches());
}

bool PanelLine(char* out, size_t n) {
    if (!out || !n) return false;
    out[0] = 0;
    if (!gOn || !gBoard.on) return false;
    return Score::PanelLine(gBoard.today, gBoard.all, gBoard.allKnown != 0, out, n);
}

bool Command(const char* line) {
    char verb[16] = "", arg[16] = "";
    const int got = sscanf(line, "%15s %15s", verb, arg);
    if (got < 1 || strcmp(verb, "score")) return false;
    if (got == 1 || !strcmp(arg, "status")) {
        char panel[96];
        PanelLine(panel, sizeof panel);
        Logger::Info("SCORE status: %s%s; %s; %d match(es) counted in this game, %d not; against the last match's opponent this game: %d won, "
                     "%d lost, %d drawn; room %d%s: %d - %d - %d, %s %d in a row, %d row(s), %d fighter(s); online match ends %u (the room's %u); "
                     "panel line \"%s\"; file %ls",
                     gOn ? "on" : "off ('score 1')", gTest ? ", TEST (every fight counts, into match_history_test.txt)" : "",
                     gInMatch ? "a match is being read" : "no match being read", gCounted, gNotCounted, gSession.wins, gSession.losses,
                     gSession.draws, gRooms, !gBoard.on ? " (none yet)" : gRoomOver ? " (over: a player left)" : gBoardTest ? " ('score test')" : "",
                     gBoard.today.wins, gBoard.today.losses, gBoard.today.draws, gBoard.streak > 0 ? "this player" : gBoard.streak < 0 ? "the opponent" : "nobody",
                     gBoard.streak < 0 ? -gBoard.streak : gBoard.streak, gBoard.rows, gBoard.fighters, (unsigned)Online::MatchEnds(), (unsigned)gRoomEnds,
                     panel, gPath);
        return true;
    }
    if (!strcmp(arg, "0") || !strcmp(arg, "1")) {
        gOn = arg[0] == '1';
        if (!gOn) gInMatch = gPending = false;
        Logger::Info("SCORE %s", gOn ? "on: online one-on-one matches are counted and written to match_history.txt" : "off: no match is counted");
        return true;
    }
    if (!strcmp(arg, "test")) {
        char v[8] = "";
        sscanf(line, "%*s %*s %7s", v);
        gTest = !strcmp(v, "1");
        gInMatch = false;
        Logger::Info("SCORE test %s", gTest ? "on: EVERY fight counts (offline too, this PC's player is player 1), into match_history_test.txt"
                                            : "off: online one-on-one matches only");
        return true;
    }
    if (!strcmp(arg, "demo")) {
        int n = 0;
        sscanf(line, "%*s %*s %d", &n);
        ScoreView::SetDemo(n);
        Logger::Info("SCORE demo %d: %s", ScoreView::DemoShown(), ScoreView::DemoShown() ? "the fight's numbers show a canned state wherever the game is" : "off");
        return true;
    }
    Logger::Info("SCORE: unknown command '%s' (score [status] | score 0|1 | score test 0|1 | score demo N)", line);
    return true;
}

}  // namespace ScoreKeeper
