#pragma once
// The score between the two players of an online match, kept over the matches of an evening, and the history of
// matches (the user's wish of 2026-11-16: "count the wins and loses between matchs and a history as well").
//
// Who won is read from the two fighters' life (character object + 0x78C0: 1.0 full, 0 knocked out; game\Fighter.h),
// once per game frame while an online one-on-one fight runs. The mod knows no counter of won rounds in MK9 (the round
// flow is MK9's script, docs\state_ownership.md), and in an online match GameFrame runs on through all its rounds. So:
//   - A round ended where both lives are full again after they had last stood unchanged, not both full, for
//     kStillFrames game frames: the knock-out or the end of the round's time, then the winner's pose. The lives as they
//     stood then say who won it (the lower one lost, two equal ones drew); the new round's lives may be filled in one
//     frame or over several. Lives that never stood still are no round's end: a rollback that takes back a hit
//     predicted wrong moves them back within a few frames, the Health Recovery kode moves them all the time, and frames
//     in which the game does not advance (a session waiting for the other game's input) are not counted.
//   - The match is over at MK9's results screen. Its winner won the last round, so the lives as they stand at the end
//     say it (the loser's at 0 after a knock-out, the lower one after the time ran out). They must say it clearly: one
//     life at 0, or both unchanged for kStillFrames, and not both full; else the match is not counted (left in the
//     middle, or not read). The rounds before it are counted for the history line; a count that does not agree with
//     the winner (the winner's rounds not more than the loser's) is written as unknown.
//
// The history is a text file beside the DLL, one line per counted match (FormatEntry / ParseEntry): it outlives the
// game and an update of the mod, and the Play Online window reads it for its list (tools\launcher_lib.ps1).
//
// The decisions and the file's text are here, without the game (tests\host\score_test.cpp); Score.cpp reads the game,
// writes the file and the log.
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace Score {

enum Result : uint8_t { kUndecided = 0, kWin, kLoss, kDraw };  // this player's side of it
inline const char* ResultText(Result r) { return r == kWin ? "WIN" : r == kLoss ? "LOSS" : r == kDraw ? "DRAW" : "?"; }

constexpr float kFull = 0.999f;     // a life at least this high is full
constexpr float kKnockedOut = 0.0005f;
constexpr float kTie = 0.0005f;     // two lives closer than this are equal
constexpr int kStillFrames = 30;    // half a second of game frames without a change: the round's end stood

// One match, frame by frame.
struct Judge {
    bool have = false;
    float life[2] = {1.0f, 1.0f};   // the last frame's lives, player 1 and player 2
    uint32_t frame = 0;             // the last frame's GameFrame
    int still = 0;                  // game frames the lives have not changed (up to a large cap)
    // The lives as they last stood still for kStillFrames, not both full: a round's end once both are full again (the
    // new round's lives may be filled in one frame or over several, one fighter's before the other's).
    bool settledHave = false;
    float settled[2] = {1.0f, 1.0f};
    int rounds[2] = {0, 0}, drawn = 0;
    uint32_t frames = 0;            // frames sampled

    void Start() { *this = Judge{}; }

    static bool Full(float a, float b) { return a >= kFull && b >= kFull; }
    // 1 or 2: the side with the higher life; 0: equal.
    static int Higher(float a, float b) {
        const float d = a - b;
        return d > kTie ? 1 : d < -kTie ? 2 : 0;
    }

    // One sample at a game frame: both fighters' lives. Returns the round judged at this frame: 1 or 2 the side that
    // won it, 3 a drawn round; 0 none. A sample of a frame already seen is ignored (the game did not advance); one that
    // is no life value (not a number, far outside 0..1) too.
    int Sample(uint32_t gameFrame, float p1, float p2) {
        if (!(p1 == p1) || !(p2 == p2) || p1 < -0.5f || p1 > 1.5f || p2 < -0.5f || p2 > 1.5f) return 0;
        p1 = p1 < 0 ? 0 : p1 > 1 ? 1 : p1;
        p2 = p2 < 0 ? 0 : p2 > 1 ? 1 : p2;
        if (have && gameFrame == frame) return 0;
        int judged = 0;
        if (Full(p1, p2) && settledHave) {
            const int w = Higher(settled[0], settled[1]);
            if (w) ++rounds[w - 1];
            else ++drawn;
            judged = w ? w : 3;
            settledHave = false;
        }
        // A frame that went back (a timeline restart: offline rounds start at GameFrame 0) counts as no frame of
        // stillness; one that moved on counts once, however far it went.
        const bool same = have && p1 == life[0] && p2 == life[1];
        still = same && gameFrame > frame ? (still < 1000000 ? still + 1 : still) : same ? still : 0;
        life[0] = p1, life[1] = p2;
        frame = gameFrame;
        have = true;
        ++frames;
        if (still >= kStillFrames && !Full(p1, p2)) {
            settled[0] = p1, settled[1] = p2;
            settledHave = true;
        }
        return judged;
    }

    struct Final {
        Result result = kUndecided;
        int mine = 0, theirs = 0;    // rounds won, the last one counted; valid when roundsKnown
        bool roundsKnown = false;
        int winnerSide = 0;          // 1 or 2; 0 a draw or undecided
        const char* why = "";        // kUndecided: the reason
    };
    // The match is over as the lives stand now. side: the fighter this player played, 1 or 2.
    Final Finish(int side) const {
        Final f;
        const auto undecided = [&f](const char* why) {
            f.why = why;
            return f;
        };
        if (side != 1 && side != 2) return undecided("this player's side is not known");
        if (!have) return undecided("no life was read in the fight");
        if (Full(life[0], life[1])) return undecided("both lives are full at the end");
        const bool out = life[0] <= kKnockedOut || life[1] <= kKnockedOut;
        if (!out && still < kStillFrames) return undecided("the lives were still moving at the end");
        const int w = Higher(life[0], life[1]);
        int r[2] = {rounds[0], rounds[1]};
        if (w) ++r[w - 1];
        f.winnerSide = w;
        f.result = !w ? kDraw : w == side ? kWin : kLoss;
        f.mine = r[side - 1];
        f.theirs = r[2 - side];
        f.roundsKnown = w && r[w - 1] > r[2 - w] && r[w - 1] <= 9;
        return f;
    }
};

// ---- the history file ----
// One line per counted match, fields apart by a tab:
//   time (local, "2026-11-16 00:41:38"), WIN / LOSS / DRAW, rounds ("2-1", this player's first; "?" unknown),
//   this player's fighter and the opponent's as the game names them ("CHAR_JohnnyCage_B"; "?" unknown), the arena
//   ("BGND_PitDay"; "?"), the opponent's SteamID64 (0 unknown), this player's side (1 or 2).
// Lines starting with '#' are comments; FileHeader is what a new file begins with.
constexpr int kNameCap = 64;
struct Entry {
    char time[20] = "";
    Result result = kUndecided;
    bool roundsKnown = false;
    int mine = 0, theirs = 0;
    char myFighter[kNameCap] = "", theirFighter[kNameCap] = "", arena[kNameCap] = "";
    uint64_t opponent = 0;
    int side = 0;
};

inline const char* FileHeader() {
    return "# MK9 Rollback: your online matches, one line per match that reached MK9's results screen (written by the mod).\n"
           "# time\tresult\trounds (yours first)\tyour fighter\tthe opponent's fighter\tarena\tthe opponent's Steam ID\tyour side\n";
}

namespace detail {
// A field as written: no tab, no line end, never empty ("?"), at most cap - 1 characters.
inline void Field(char* out, size_t cap, const char* s) {
    size_t n = 0;
    for (; s && *s && n + 1 < cap; ++s) out[n++] = *s == '\t' || *s == '\r' || *s == '\n' ? ' ' : *s;
    if (!n && cap > 1) out[n++] = '?';
    if (cap) out[n] = 0;
}
}  // namespace detail

// The line for an entry, ending in '\n'. Returns its length (0: out too small; out is always terminated).
inline int FormatEntry(const Entry& e, char* out, size_t n) {
    if (!out || !n) return 0;
    out[0] = 0;
    if (e.result == kUndecided) return 0;
    char t[20], a[kNameCap], b[kNameCap], c[kNameCap], rounds[16];
    detail::Field(t, sizeof t, e.time);
    detail::Field(a, sizeof a, e.myFighter);
    detail::Field(b, sizeof b, e.theirFighter);
    detail::Field(c, sizeof c, e.arena);
    if (e.roundsKnown) snprintf(rounds, sizeof rounds, "%d-%d", e.mine, e.theirs);
    else snprintf(rounds, sizeof rounds, "?");
    const int k = snprintf(out, n, "%s\t%s\t%s\t%s\t%s\t%s\t%llu\t%d\n", t, ResultText(e.result), rounds, a, b, c,
                           (unsigned long long)e.opponent, e.side);
    if (k < 0 || (size_t)k >= n) {
        out[0] = 0;
        return 0;
    }
    return k;
}

// One line of the file (with or without its line end). False for a comment, a blank line, and anything that is not
// eight fields in FormatEntry's form.
inline bool ParseEntry(const char* line, Entry* e) {
    if (!line || !e) return false;
    *e = Entry{};
    const char* f[8];
    size_t len[8];
    int count = 0;
    const char* p = line;
    while (*p == ' ') ++p;
    if (!*p || *p == '#' || *p == '\r' || *p == '\n') return false;
    for (;;) {
        const char* start = p;
        while (*p && *p != '\t' && *p != '\r' && *p != '\n') ++p;
        if (count == 8) return false;  // a ninth field
        f[count] = start, len[count] = (size_t)(p - start);
        ++count;
        if (*p != '\t') break;
        ++p;
    }
    if (count != 8) return false;
    for (int i = 0; i < 8; ++i)
        if (!len[i]) return false;
    const auto copy = [&](int i, char* out, size_t cap) {
        if (len[i] >= cap) return false;
        memcpy(out, f[i], len[i]);
        out[len[i]] = 0;
        return true;
    };
    char result[8], rounds[16], opp[24], side[4];
    if (!copy(0, e->time, sizeof e->time) || !copy(1, result, sizeof result) || !copy(2, rounds, sizeof rounds) ||
        !copy(3, e->myFighter, sizeof e->myFighter) || !copy(4, e->theirFighter, sizeof e->theirFighter) ||
        !copy(5, e->arena, sizeof e->arena) || !copy(6, opp, sizeof opp) || !copy(7, side, sizeof side))
        return false;
    e->result = !strcmp(result, "WIN") ? kWin : !strcmp(result, "LOSS") ? kLoss : !strcmp(result, "DRAW") ? kDraw : kUndecided;
    if (e->result == kUndecided) return false;
    if (strcmp(rounds, "?")) {
        int a = -1, b = -1, used = 0;
        if (sscanf(rounds, "%d-%d%n", &a, &b, &used) != 2 || rounds[used] || a < 0 || b < 0 || a > 99 || b > 99) return false;
        e->roundsKnown = true, e->mine = a, e->theirs = b;
    }
    for (const char* d = opp; *d; ++d)
        if (*d < '0' || *d > '9') return false;
    e->opponent = strtoull(opp, nullptr, 10);
    if (strcmp(side, "1") && strcmp(side, "2")) return false;
    e->side = side[0] - '0';
    return true;
}

// Wins, losses and draws.
struct Tally {
    int wins = 0, losses = 0, draws = 0;
    int Matches() const { return wins + losses + draws; }
    void Add(Result r) {
        if (r == kWin) ++wins;
        else if (r == kLoss) ++losses;
        else if (r == kDraw) ++draws;
    }
};

// The score of a day and of every day. A day begins at 06:00 local time, so an evening that goes on past midnight is
// one day ("today" at 01:00 is the evening that began the day before). DayStart's text compares with an entry's time
// as text: since[] = "2026-11-15 06:00:00" counts every entry from then on.
struct Totals {
    Tally today, all;
};
inline void Count(const Entry& e, uint64_t opponent, const char* since, Totals* t) {
    if (opponent && e.opponent != opponent) return;
    t->all.Add(e.result);
    if (since && strcmp(e.time, since) >= 0) t->today.Add(e.result);
}
// The day's start for a local time: that date at 06:00, or the date before ('prevY/M/D', the caller's calendar) when
// the time is earlier than 06:00.
inline void DayStart(int y, int mo, int d, int h, int prevY, int prevMo, int prevD, char* out, size_t n) {
    if (h < 6) y = prevY, mo = prevMo, d = prevD;
    snprintf(out, n, "%04d-%02d-%02d 06:00:00", y, mo, d);
}

// The totals of the file's text against one opponent (opponent 0: against everyone), today counted from 'since' (null:
// no day). Lines that are not entries are skipped and counted in *skipped (comments and blank lines are not).
inline Totals TallyOf(const char* text, uint64_t opponent, const char* since, int* skipped = nullptr) {
    Totals t;
    int bad = 0;
    for (const char* p = text; p && *p;) {
        const char* end = strchr(p, '\n');
        const size_t n = end ? (size_t)(end - p) : strlen(p);
        char line[512];
        const char* from = p;
        p = end ? end + 1 : p + n;
        size_t k = 0;
        while (k < n && from[k] == ' ') ++k;
        if (k == n || from[k] == '#' || from[k] == '\r') continue;
        if (n >= sizeof line) {
            ++bad;
            continue;
        }
        memcpy(line, from, n);
        line[n] = 0;
        Entry e;
        if (!ParseEntry(line, &e)) {
            ++bad;
            continue;
        }
        Count(e, opponent, since, &t);
    }
    if (skipped) *skipped = bad;
    return t;
}

// The line on the choice after a match: "SCORE  YOU 3 - 2 OPP" (today's, or this game's when the file could not be
// read), the draws when there are any, and the total against this opponent over every day when it differs from
// today's. Capitals, digits and signs only (the panel's lettering has nothing else). Returns false (out empty) when
// there is nothing to say yet.
inline bool PanelLine(const Tally& session, const Tally& all, bool allKnown, char* out, size_t n) {
    if (!out || !n) return false;
    out[0] = 0;
    if (!session.Matches()) return false;
    int k = snprintf(out, n, "SCORE  YOU %d - %d OPP", session.wins, session.losses);
    if (k > 0 && (size_t)k < n && session.draws) k += snprintf(out + k, n - (size_t)k, "  DRAWS %d", session.draws);
    if (k > 0 && (size_t)k < n && allKnown && all.Matches() > session.Matches())
        k += snprintf(out + k, n - (size_t)k, "      ALL TIME %d - %d", all.wins, all.losses);
    if (k < 0 || (size_t)k >= n) {  // cut: say no more than fits whole
        snprintf(out, n, "SCORE %d - %d", session.wins, session.losses);
    }
    return out[0] != 0;
}

// ---- the board: what the panel after a match and the fight's display show (the user's wish of 2026-11-16: "show it
// better in the screen after a match is finished and on the fighting screen as well! Also show the win streak if
// possible! And put the history match in the game as well!") ----

// A fighter as a player reads it, in capitals (the panels' lettering has no small letters): "CHAR_JOHNNYCAGE_B" ->
// "JOHNNY CAGE". The costume letter is dropped; a name the table does not know is the code's own words.
inline void FighterDisplay(const char* code, char* out, size_t n) {
    if (!out || !n) return;
    out[0] = 0;
    if (!code || !*code || !strcmp(code, "?")) {
        snprintf(out, n, "?");
        return;
    }
    const char* s = code;
    if ((s[0] == 'C' || s[0] == 'c') && (s[1] == 'H' || s[1] == 'h') && (s[2] == 'A' || s[2] == 'a') && (s[3] == 'R' || s[3] == 'r') && s[4] == '_') s += 5;
    char body[kNameCap];
    snprintf(body, sizeof body, "%s", s);
    size_t len = strlen(body);
    if (len > 2 && body[len - 2] == '_' && ((body[len - 1] >= 'A' && body[len - 1] <= 'Z') || (body[len - 1] >= 'a' && body[len - 1] <= 'z')))
        body[len -= 2] = 0;  // the costume: "_B"
    char key[kNameCap];
    size_t k = 0;
    for (size_t i = 0; i < len && k + 1 < sizeof key; ++i)
        if (body[i] != '_' && body[i] != ' ' && body[i] != '-') key[k++] = (char)(body[i] >= 'A' && body[i] <= 'Z' ? body[i] + 32 : body[i]);
    key[k] = 0;
    static const char* const kNames[][2] = {
        {"baraka", "BARAKA"},           {"cyberzero", "CYBER SUB-ZERO"}, {"cybersubzero", "CYBER SUB-ZERO"}, {"cyrax", "CYRAX"},
        {"ermac", "ERMAC"},             {"freddykrueger", "FREDDY KRUEGER"}, {"freddy", "FREDDY KRUEGER"}, {"goro", "GORO"},
        {"jade", "JADE"},               {"jax", "JAX"},                  {"johnnycage", "JOHNNY CAGE"},     {"kabal", "KABAL"},
        {"kano", "KANO"},               {"kenshi", "KENSHI"},            {"kintaro", "KINTARO"},           {"kitana", "KITANA"},
        {"kratos", "KRATOS"},           {"kunglao", "KUNG LAO"},         {"liukang", "LIU KANG"},          {"mileena", "MILEENA"},
        {"nightwolf", "NIGHTWOLF"},     {"noob", "NOOB SAIBOT"},         {"noobsaibot", "NOOB SAIBOT"},     {"quanchi", "QUAN CHI"},
        {"raiden", "RAIDEN"},           {"rain", "RAIN"},                {"reptile", "REPTILE"},           {"scorpion", "SCORPION"},
        {"sektor", "SEKTOR"},           {"shangtsung", "SHANG TSUNG"},   {"shaokahn", "SHAO KAHN"},        {"sheeva", "SHEEVA"},
        {"sindel", "SINDEL"},           {"skarlet", "SKARLET"},          {"smoke", "SMOKE"},               {"sonya", "SONYA"},
        {"stryker", "STRYKER"},         {"subzero", "SUB-ZERO"},
    };
    for (const auto& m : kNames)
        if (!strcmp(key, m[0])) {
            snprintf(out, n, "%s", m[1]);
            return;
        }
    size_t o = 0;
    for (size_t i = 0; i < len && o + 1 < n; ++i) out[o++] = body[i] == '_' ? ' ' : (body[i] >= 'a' && body[i] <= 'z') ? (char)(body[i] - 32) : body[i];
    out[o] = 0;
    if (!o) snprintf(out, n, "?");
}

constexpr int kBoardRows = 8, kBoardFighters = 6, kShortName = 24;
// The score against one opponent: today's and every day's, the results in a row, the last matches and today's record
// of each fighter this player chose. Plain data (it is copied to the rendering thread whole).
struct Board {
    uint8_t on = 0;                    // filled for an opponent
    uint8_t allKnown = 0;              // 'all' and the rows come from the file (else from this game's matches only)
    uint64_t opponent = 0;
    Tally today, all;
    int streak = 0;                    // the latest results in a row: > 0 this player's wins, < 0 the opponent's; a draw ends it
    int bestMine = 0, bestTheirs = 0;  // today's longest run of wins, each player
    int runMine = 0, runTheirs = 0;    // today's runs as they stand (for the best)
    struct Row {
        uint8_t result = kUndecided, roundsKnown = 0, mine = 0, theirs = 0, today = 0;
        char when[8] = "";             // "22:47" today, "15 NOV" an earlier day
        char me[kShortName] = "", them[kShortName] = "";
    };
    int rows = 0;                      // row[0] is the newest
    Row row[kBoardRows];
    struct Fighter {
        char name[kShortName] = "";
        Tally t;
    };
    int fighters = 0;                  // today's, the most played first (the first chosen first among equals)
    Fighter fighter[kBoardFighters];

    // One more match against this opponent, after every earlier one. today: it was played today (Count's rule).
    void Add(const Entry& e, bool isToday) {
        if (e.result == kUndecided) return;
        on = 1;
        all.Add(e.result);
        streak = e.result == kWin ? (streak > 0 ? streak + 1 : 1) : e.result == kLoss ? (streak < 0 ? streak - 1 : -1) : 0;
        if (isToday) {
            today.Add(e.result);
            runMine = e.result == kWin ? runMine + 1 : 0;
            runTheirs = e.result == kLoss ? runTheirs + 1 : 0;
            if (runMine > bestMine) bestMine = runMine;
            if (runTheirs > bestTheirs) bestTheirs = runTheirs;
            char name[kShortName];
            FighterDisplay(e.myFighter, name, sizeof name);
            int at = -1;
            for (int i = 0; i < fighters; ++i)
                if (!strcmp(fighter[i].name, name)) at = i;
            if (at < 0 && fighters < kBoardFighters) {
                at = fighters++;
                fighter[at] = Fighter{};
                snprintf(fighter[at].name, sizeof fighter[at].name, "%s", name);
            }
            if (at >= 0) {
                fighter[at].t.Add(e.result);
                while (at > 0 && fighter[at].t.Matches() > fighter[at - 1].t.Matches()) {  // the most played first
                    const Fighter f = fighter[at - 1];
                    fighter[at - 1] = fighter[at];
                    fighter[at] = f;
                    --at;
                }
            }
        }
        for (int i = kBoardRows - 1; i > 0; --i) row[i] = row[i - 1];
        if (rows < kBoardRows) ++rows;
        Row& r = row[0];
        r = Row{};
        r.result = e.result;
        r.roundsKnown = e.roundsKnown ? 1 : 0;
        r.mine = (uint8_t)(e.mine < 0 ? 0 : e.mine > 99 ? 99 : e.mine);
        r.theirs = (uint8_t)(e.theirs < 0 ? 0 : e.theirs > 99 ? 99 : e.theirs);
        r.today = isToday ? 1 : 0;
        static const char* const kMonths[] = {"JAN", "FEB", "MAR", "APR", "MAY", "JUN", "JUL", "AUG", "SEP", "OCT", "NOV", "DEC"};
        const int month = strlen(e.time) >= 16 ? (e.time[5] - '0') * 10 + (e.time[6] - '0') : 0;
        if (strlen(e.time) < 16) snprintf(r.when, sizeof r.when, "?");
        else if (isToday || month < 1 || month > 12) snprintf(r.when, sizeof r.when, "%.5s", e.time + 11);
        else snprintf(r.when, sizeof r.when, "%.2s %s", e.time + 8, kMonths[month - 1]);
        FighterDisplay(e.myFighter, r.me, sizeof r.me);
        FighterDisplay(e.theirFighter, r.them, sizeof r.them);
    }
};

// The board of the file's text against one opponent (exactly that ID: 0 is the unknown ones and the test file's),
// today counted from 'since' (null: no day). Lines that are not entries are skipped and counted in *skipped.
inline void BoardOf(const char* text, uint64_t opponent, const char* since, Board* b, int* skipped = nullptr) {
    *b = Board{};
    b->opponent = opponent;
    b->on = 1;
    b->allKnown = 1;
    int bad = 0;
    for (const char* p = text; p && *p;) {
        const char* end = strchr(p, '\n');
        const size_t n = end ? (size_t)(end - p) : strlen(p);
        const char* from = p;
        p = end ? end + 1 : p + n;
        size_t k = 0;
        while (k < n && from[k] == ' ') ++k;
        if (k == n || from[k] == '#' || from[k] == '\r') continue;
        char line[512];
        if (n >= sizeof line) {
            ++bad;
            continue;
        }
        memcpy(line, from, n);
        line[n] = 0;
        Entry e;
        if (!ParseEntry(line, &e)) {
            ++bad;
            continue;
        }
        if (e.opponent == opponent) b->Add(e, since && strcmp(e.time, since) >= 0);
    }
    if (skipped) *skipped = bad;
}

// The fight's display for one side: that player's wins today and the wins in a row it is on (0 when the other side
// is on a run). side: the fighter this player plays (1 or 2); which: the side asked for (1 or 2).
inline int WinsOf(const Board& b, int side, int which) { return which == side ? b.today.wins : b.today.losses; }
inline int RunOf(const Board& b, int side, int which) {
    const int mine = b.streak > 0 ? b.streak : 0, theirs = b.streak < 0 ? -b.streak : 0;
    return which == side ? mine : theirs;
}

}  // namespace Score
