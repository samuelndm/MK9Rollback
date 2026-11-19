#include "ScoreKeeper.h"

#include <windows.h>

#include <cerrno>
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
constexpr uint32_t kBoardLookMs = 2000;    // outside a fight, how often the board is checked against the opponent and the day
constexpr long kFileCap = 8L << 20;        // a history larger than this is not read (a line is about 100 bytes)

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

// The board against the current opponent: read from the file outside a fight (gBoard.allKnown), each counted match
// added at once (so the choice after the match shows it before its line is written), read again after the line is.
Score::Board gBoard;
bool gBoardTest = false;        // gBoard is the test file's (opponent 0)
char gBoardSince[24] = "";      // the day it was counted for
ULONGLONG gBoardLookAt = 0;     // the next look at the opponent and the day

// This game's score against the last counted match's opponent (the log's "this game" numbers).
uint64_t gTallyOpponent = 0;
bool gHaveTally = false;
Tally gSession;
int gCounted = 0, gNotCounted = 0;
int gWriteFailures = 0, gReadFailures = 0;

uint32_t GameState() {
    const uint32_t gi = *reinterpret_cast<const volatile uint32_t*>(MK9Game::Get().gameInfoPtr);
    return gi ? *reinterpret_cast<const volatile uint32_t*>(gi + 0x4) : 0xFFFFFFFF;
}
uint32_t GameFrame() {
    const uint32_t gi = *reinterpret_cast<const volatile uint32_t*>(MK9Game::Get().gameInfoPtr);
    return gi ? *reinterpret_cast<const volatile uint32_t*>(gi + MK9Game::kGameInfoGameFrame) : 0;
}
bool SessionRuns() { return Rollback::NetplayRunning() || Rollback::NetplayStarting() || Rollback::gIsResimulating; }

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
    Logger::Info("SCORE: a %s fight began: the two fighters' lives are read until the match is over%s", test ? "fight ('score test')" : "one-on-one online",
                 gBoard.on ? "" : " (no score read for this opponent yet: the fight shows none)");
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
    if (!gOpponent && !gTestMatch) gOpponent = Online::Opponent();
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

// Where today began (Score::DayStart: 06:00 of this date, or of the date before when it is earlier than that).
void TodayStart(char* out, size_t n) {
    SYSTEMTIME t, prev;
    GetLocalTime(&t);
    prev = t;
    FILETIME ft;
    if (SystemTimeToFileTime(&t, &ft)) {
        ULARGE_INTEGER u;
        u.LowPart = ft.dwLowDateTime, u.HighPart = ft.dwHighDateTime;
        u.QuadPart -= 24ull * 3600 * 10000000;
        ft.dwLowDateTime = u.LowPart, ft.dwHighDateTime = u.HighPart;
        if (!FileTimeToSystemTime(&ft, &prev)) prev = t;
    }
    Score::DayStart(t.wYear, t.wMonth, t.wDay, t.wHour, prev.wYear, prev.wMonth, prev.wDay, out, n);
}

// The board against one opponent from the file (the whole file is read: it grows by a line per match). A file that is
// not there yet is an empty board. False: not read (gBoard stays as it was). Never while a rollback session runs.
bool LoadBoard(const wchar_t* path, uint64_t opponent, bool test, const char* why) {
    if (!path[0]) return false;
    char since[24];
    TodayStart(since, sizeof since);
    Score::Board b;
    int skipped = 0;
    long size = 0;
    FILE* f = _wfopen(path, L"rb");
    if (!f) {
        if (errno != ENOENT) {
            if (gReadFailures++ < 4) Logger::Info("SCORE: %ls could not be read (errno %d): the score shown is this game's", path, errno);
            return false;
        }
        b.on = 1, b.allKnown = 1, b.opponent = opponent;  // no file yet: nothing played
    } else {
        if (fseek(f, 0, SEEK_END) == 0) size = ftell(f);
        fseek(f, 0, SEEK_SET);
        char* text = size >= 0 && size <= kFileCap ? static_cast<char*>(malloc((size_t)size + 1)) : nullptr;
        const size_t got = text ? fread(text, 1, (size_t)size, f) : 0;
        fclose(f);
        if (!text || got != (size_t)size) {
            free(text);
            if (gReadFailures++ < 4) Logger::Info("SCORE: %ls could not be read (%ld bytes): the score shown is this game's", path, size);
            return false;
        }
        text[got] = 0;
        Score::BoardOf(text, opponent, since, &b, &skipped);
        free(text);
    }
    gBoard = b;
    gBoardTest = test;
    snprintf(gBoardSince, sizeof gBoardSince, "%s", since);
    Logger::Info("SCORE board (%s): against this opponent today (since %s) %d won, %d lost, %d drawn; every day %d - %d - %d; %s %d in a row; "
                 "today's best runs %d and %d; %d fighter(s) today; from %ls (%ld bytes%s)",
                 why, since, b.today.wins, b.today.losses, b.today.draws, b.all.wins, b.all.losses, b.all.draws,
                 b.streak > 0 ? "this player" : b.streak < 0 ? "the opponent" : "nobody", b.streak < 0 ? -b.streak : b.streak, b.bestMine,
                 b.bestTheirs, b.fighters, path, size, skipped ? ", some lines not read" : "");
    return true;
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
    char why[80];
    snprintf(why, sizeof why, "the match's line written%s", how);
    LoadBoard(path, gPendingEntry.opponent, gPendingTest, why);  // read back: the board as the file has it
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
    // The board at once (the choice after the match opens now); the file's own count replaces it once the line is written.
    if (!gBoard.on || gBoard.opponent != gOpponent || gBoardTest != gTestMatch) {
        gBoard = Score::Board{};
        gBoard.on = 1;
        gBoard.opponent = gOpponent;
        gBoardTest = gTestMatch;
    }
    gBoard.Add(e, true);
    char rounds[16];
    if (f.roundsKnown) snprintf(rounds, sizeof rounds, "%d-%d", f.mine, f.theirs);
    else snprintf(rounds, sizeof rounds, "not known");
    Logger::Info("SCORE: the match is over (%s): this player %s, rounds %s (%d - %d - %d drawn read before the last), %s against %s on %s "
                 "(lives at the end %.3f and %.3f; side %d, opponent %s); this game's score against this opponent: %d won, %d lost, %d drawn; "
                 "today %d - %d - %d, %s %d in a row",
                 where, Score::ResultText(f.result), rounds, gJudge.rounds[gSide - 1], gJudge.rounds[2 - gSide], gJudge.drawn, e.myFighter,
                 e.theirFighter, e.arena, gJudge.life[0], gJudge.life[1], gSide, gOpponent ? "known" : "not known", gSession.wins, gSession.losses,
                 gSession.draws, gBoard.today.wins, gBoard.today.losses, gBoard.today.draws,
                 gBoard.streak > 0 ? "this player" : gBoard.streak < 0 ? "the opponent" : "nobody", gBoard.streak < 0 ? -gBoard.streak : gBoard.streak);
    gPending = true;
    gPendingTest = gTestMatch;
    gPendingAt = GetTickCount64();
}

// Outside a fight, now and then: the board for the coming fight's opponent, and again when a new day began.
void LookAtBoard(ULONGLONG now, bool test) {
    if (gInMatch || gPending || now < gBoardLookAt || SessionRuns()) return;
    gBoardLookAt = now + kBoardLookMs;
    const uint64_t opponent = test ? 0 : Online::Opponent();
    if (!test && !opponent) return;
    char since[24];
    TodayStart(since, sizeof since);
    if (gBoard.on && gBoard.allKnown && gBoard.opponent == opponent && gBoardTest == test && !strcmp(since, gBoardSince)) return;
    LoadBoard(test ? gTestPath : gPath, opponent, test, test ? "'score test'" : gBoard.on && gBoard.opponent == opponent ? "a new day" : "the coming match's opponent");
}

// The fight's numbers (debug\ScoreView.h): in a counted match's fight, once this player's side and the board for this
// opponent are known.
void PublishBadge(uint32_t state) {
    ScoreView::Badge b;
    const bool fight = gOn && gInMatch && state == kFightState && gSide;
    const bool ours = gBoard.on && gBoardTest == gTestMatch && (gTestMatch || gBoard.opponent == (gOpponent ? gOpponent : Online::Opponent()));
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
        } else if (!gInMatch) {
            LookAtBoard(now, true);
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
        LookAtBoard(now, false);
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
                     "%d lost, %d drawn; board %s: today %d - %d - %d, every day %d - %d - %d%s, %s %d in a row, %d row(s), %d fighter(s); "
                     "panel line \"%s\"; file %ls",
                     gOn ? "on" : "off ('score 1')", gTest ? ", TEST (every fight counts, into match_history_test.txt)" : "",
                     gInMatch ? "a match is being read" : "no match being read", gCounted, gNotCounted, gSession.wins, gSession.losses,
                     gSession.draws, !gBoard.on ? "none" : gBoardTest ? "of the test file" : gBoard.allKnown ? "from the file" : "this game's only",
                     gBoard.today.wins, gBoard.today.losses, gBoard.today.draws, gBoard.all.wins, gBoard.all.losses, gBoard.all.draws,
                     gBoardSince[0] ? "" : " (no day)", gBoard.streak > 0 ? "this player" : gBoard.streak < 0 ? "the opponent" : "nobody",
                     gBoard.streak < 0 ? -gBoard.streak : gBoard.streak, gBoard.rows, gBoard.fighters, panel, gPath);
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
        gBoardLookAt = 0;  // the test file's board at the next tick outside a fight
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
