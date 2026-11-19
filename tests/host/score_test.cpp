// src\net\Score.h: who won an online match, read from the two fighters' lives frame by frame, and the history file's
// lines. Rounds by knock-out and by time, a hit that a rollback takes back, a session that waits for the other game,
// the Health Recovery kode, matches left in the middle; the file's lines written, read back and refused; the panel's
// line; and 3000 random matches with all of that mixed in.
#include <cmath>
#include <cstdio>
#include <cstring>

#include "Score.h"

using namespace Score;

namespace {
int gChecks = 0, gFailed = 0;

void Check(bool ok, const char* what) {
    ++gChecks;
    if (ok) return;
    ++gFailed;
    printf("FAIL: %s\n", what);
}

// A fight as the game shows it: one Sample per game frame.
struct Fight {
    Judge j;
    uint32_t gf = 100000;
    int judgedP1 = 0, judgedP2 = 0, judgedDraw = 0;
    float a = 1.0f, b = 1.0f;
    void Frame() {
        const int r = j.Sample(gf++, a, b);
        if (r == 1) ++judgedP1;
        if (r == 2) ++judgedP2;
        if (r == 3) ++judgedDraw;
    }
    void Hold(int frames) {
        for (int i = 0; i < frames; ++i) Frame();
    }
    // The lives move to (ta, tb) over 'frames' frames (hits: they go down in steps).
    void Move(float ta, float tb, int frames) {
        const float da = (ta - a) / frames, db = (tb - b) / frames;
        for (int i = 0; i < frames; ++i) {
            a += da, b += db;
            if (i == frames - 1) a = ta, b = tb;
            Frame();
        }
    }
    void NewRound() {
        a = b = 1.0f;
        Hold(90);  // the round's intro: both full
    }
};

void RoundTests() {
    {  // 2-1 for player 1: KO, KO, KO
        Fight f;
        f.Hold(200);  // the match's intro
        f.Move(0.55f, 0.0f, 400);
        f.Hold(150);  // the knock-out and the winner's pose
        f.NewRound();
        f.Move(0.0f, 0.3f, 500);
        f.Hold(150);
        f.NewRound();
        f.Move(0.12f, 0.0f, 600);
        f.Hold(200);  // Finish Him, the results screen comes
        Check(f.judgedP1 == 1 && f.judgedP2 == 1 && f.judgedDraw == 0, "2-1: two rounds judged before the last, one for each");
        const Judge::Final w = f.j.Finish(1), l = f.j.Finish(2);
        Check(w.result == kWin && w.roundsKnown && w.mine == 2 && w.theirs == 1, "2-1: player 1 won 2-1");
        Check(l.result == kLoss && l.roundsKnown && l.mine == 1 && l.theirs == 2, "2-1: seen from player 2, lost 1-2");
    }
    {  // a round by time: the lower life loses; equal lives draw
        Fight f;
        f.Hold(100);
        f.Move(0.4f, 0.7f, 900);
        f.Hold(120);  // time is up: the lives stand
        f.NewRound();
        Check(f.judgedP2 == 1 && f.judgedP1 == 0, "time: the higher life (player 2) won the round");
        f.Move(0.5f, 0.5f, 900);
        f.Hold(120);
        f.NewRound();
        Check(f.judgedDraw == 1, "time: equal lives drew the round");
        f.Move(0.0f, 0.2f, 300);
        f.Hold(100);
        const Judge::Final r = f.j.Finish(2);
        Check(r.result == kWin && r.roundsKnown && r.mine == 2 && r.theirs == 0, "time, draw, KO: player 2 won 2-0 (one round drawn)");
    }
    {  // the new round's lives filled over a second, one fighter's before the other's
        Fight f;
        f.Hold(100);
        f.Move(0.0f, 0.35f, 300);
        f.Hold(120);
        f.Move(0.5f, 0.35f, 20);  // player 1's bar fills first
        f.Move(1.0f, 0.7f, 30);
        f.Move(1.0f, 1.0f, 15);
        f.Hold(60);
        Check(f.judgedP2 == 1 && f.judgedP1 == 0 && f.judgedDraw == 0, "lives filled over several frames: the round is judged by the lives that stood");
        f.Move(0.0f, 0.9f, 300);  // the next round goes on as usual
        f.Hold(100);
        Check(f.j.Finish(2).result == kWin && f.j.Finish(2).mine == 2 && f.j.Finish(2).theirs == 0, "and the match after it: 2-0");
    }
    {  // a lull in the middle of a round stands still too: no round's end while the lives are not full again
        Fight f;
        f.Hold(100);
        f.Move(0.7f, 0.5f, 100);
        f.Hold(200);  // nobody moves for a while
        f.Move(0.7f, 0.2f, 100);
        f.Hold(200);
        Check(f.judgedP1 + f.judgedP2 + f.judgedDraw == 0, "a lull is no round's end");
    }
    {  // the round's first hit was predicted wrong: a rollback takes it back within a few frames
        Fight f;
        f.Hold(100);
        f.a = 1.0f, f.b = 0.85f;
        f.Hold(6);
        f.b = 1.0f;
        f.Hold(10);
        Check(f.judgedP1 + f.judgedP2 + f.judgedDraw == 0, "a hit taken back by a rollback is no round's end");
    }
    {  // that hit, and then the session waits a second for the other game: the frame does not advance
        Fight f;
        f.Hold(100);
        f.b = 0.85f;
        f.Hold(3);
        for (int i = 0; i < 120; ++i) f.j.Sample(f.gf, f.a, f.b);  // the same frame, tick after tick
        f.b = 1.0f;
        f.Hold(10);
        Check(f.judgedP1 + f.judgedP2 + f.judgedDraw == 0, "a wait for the other game's input is no frame of stillness");
    }
    {  // the Health Recovery kode: both lives climb back to full by themselves, then stand there
        Fight f;
        f.Hold(100);
        f.Move(0.7f, 0.8f, 200);
        f.Move(1.0f, 1.0f, 300);  // recovering, a little each frame
        f.Hold(200);
        f.Move(0.9f, 1.0f, 30);   // fighting again
        Check(f.judgedP1 + f.judgedP2 + f.judgedDraw == 0, "lives that recover to full are no round's end");
    }
    {  // a match left in the middle, or not read
        Fight f;
        f.Hold(100);
        f.Move(0.6f, 0.4f, 200);
        Check(f.j.Finish(1).result == kUndecided, "the lives still moving at the end: not decided");
        Fight g;
        g.Hold(500);
        Check(g.j.Finish(1).result == kUndecided, "both lives full at the end: not decided");
        Judge none;
        Check(none.Finish(1).result == kUndecided, "nothing read: not decided");
        Fight h;
        h.Move(0.0f, 0.5f, 100);
        Check(h.j.Finish(0).result == kUndecided && h.j.Finish(3).result == kUndecided, "this player's side not known: not decided");
        Check(h.j.Finish(2).result == kWin, "a knock-out decides at once, still or not");
    }
    {  // a double knock-out in the last round
        Fight f;
        f.Hold(100);
        f.Move(0.0f, 0.0f, 300);
        f.Hold(60);
        const Judge::Final r = f.j.Finish(1);
        Check(r.result == kDraw && !r.roundsKnown, "double knock-out at the end: a draw, rounds not given");
    }
    {  // a count that does not agree with the winner is not given
        Fight f;
        f.Hold(100);
        f.Move(0.3f, 0.0f, 100);
        f.Hold(60);
        f.NewRound();
        f.Move(0.3f, 0.0f, 100);
        f.Hold(60);
        f.NewRound();
        f.Move(0.0f, 0.3f, 100);  // player 2 wins the last round, after player 1 had two
        f.Hold(60);
        const Judge::Final r = f.j.Finish(2);
        Check(r.result == kWin && !r.roundsKnown, "rounds 2-1 against the last round's winner: the result stands, the rounds are not given");
    }
    {  // what is no life value is not read
        Judge j;
        j.Sample(1, NAN, 0.5f);
        j.Sample(2, 5.0f, 0.5f);
        j.Sample(3, 0.5f, -3.0f);
        Check(!j.have && j.frames == 0, "not a number, far outside 0..1: ignored");
        j.Sample(4, -0.01f, 1.01f);
        Check(j.have && j.life[0] == 0.0f && j.life[1] == 1.0f, "a little outside 0..1: clamped");
    }
    {  // offline: every round starts again at GameFrame 0
        Fight f;
        f.gf = 0;
        f.Hold(100);
        f.Move(0.0f, 0.5f, 200);
        f.Hold(100);
        f.gf = 0;  // the round's reset
        f.NewRound();
        Check(f.judgedP2 == 1, "a round end where GameFrame starts over (offline) is judged too");
    }
}

void FileTests() {
    Entry e;
    snprintf(e.time, sizeof e.time, "2026-11-16 00:41:38");
    e.result = kWin;
    e.roundsKnown = true, e.mine = 2, e.theirs = 1;
    snprintf(e.myFighter, sizeof e.myFighter, "CHAR_JOHNNYCAGE");
    snprintf(e.theirFighter, sizeof e.theirFighter, "CHAR_KunglAO\tx");  // a tab in a name must not make a field
    snprintf(e.arena, sizeof e.arena, "BGND_PitDay");
    e.opponent = 76561197960265729ull;
    e.side = 1;
    char line[400];
    const int n = FormatEntry(e, line, sizeof line);
    Check(n > 0 && (size_t)n == strlen(line) && line[n - 1] == '\n', "FormatEntry: one line, its length returned");
    Check(!strcmp(line, "2026-11-16 00:41:38\tWIN\t2-1\tCHAR_JOHNNYCAGE\tCHAR_KunglAO x\tBGND_PitDay\t76561197960265729\t1\n"), "FormatEntry: the fields");
    Entry back;
    Check(ParseEntry(line, &back) && back.result == kWin && back.roundsKnown && back.mine == 2 && back.theirs == 1 &&
              !strcmp(back.myFighter, "CHAR_JOHNNYCAGE") && !strcmp(back.theirFighter, "CHAR_KunglAO x") && !strcmp(back.arena, "BGND_PitDay") &&
              back.opponent == 76561197960265729ull && back.side == 1 && !strcmp(back.time, e.time),
          "ParseEntry(FormatEntry(x)) == x");
    Entry u = e;
    u.roundsKnown = false;
    u.result = kDraw;
    u.myFighter[0] = 0;
    u.opponent = 0;
    u.side = 2;
    FormatEntry(u, line, sizeof line);
    Check(strstr(line, "\tDRAW\t?\t?\t") != nullptr && strstr(line, "\t0\t2\n") != nullptr, "FormatEntry: rounds and a name not known are '?'");
    Check(ParseEntry(line, &back) && back.result == kDraw && !back.roundsKnown && !strcmp(back.myFighter, "?") && back.opponent == 0 && back.side == 2,
          "ParseEntry: '?' rounds and names, opponent 0");
    Entry nope;
    nope.result = kUndecided;
    Check(FormatEntry(nope, line, sizeof line) == 0 && line[0] == 0, "FormatEntry: an undecided match has no line");
    Check(FormatEntry(e, line, 20) == 0 && line[0] == 0, "FormatEntry: a buffer too small gives nothing");
    Check(ParseEntry("t\tLOSS\t0-2\ta\tb\tc\t5\t2\r\n", &back) && back.result == kLoss && back.mine == 0 && back.theirs == 2, "ParseEntry: CRLF");
    const char* bad[] = {
        "# a comment",
        "",
        "\r\n",
        "t\tWIN\t2-1\ta\tb\tc\t5",              // seven fields
        "t\tWIN\t2-1\ta\tb\tc\t5\t1\tx",        // nine
        "t\tWON\t2-1\ta\tb\tc\t5\t1",           // not a result
        "t\tWIN\t2-x\ta\tb\tc\t5\t1",           // rounds
        "t\tWIN\t2-1-0\ta\tb\tc\t5\t1",
        "t\tWIN\t-1-2\ta\tb\tc\t5\t1",
        "t\tWIN\t2-1\ta\tb\tc\t5x\t1",          // opponent
        "t\tWIN\t2-1\ta\tb\tc\t5\t3",           // side
        "t\tWIN\t2-1\t\tb\tc\t5\t1",            // an empty field
    };
    bool none = true;
    for (const char* b : bad) none = none && !ParseEntry(b, &back);
    Check(none, "ParseEntry: comments, blank lines and every malformed line refused");

    char text[2048];
    snprintf(text, sizeof text,
             "%s"
             "t\tWIN\t2-0\ta\tb\tc\t5\t1\n"
             "t\tLOSS\t1-2\ta\tb\tc\t5\t1\n"
             "t\tWIN\t?\ta\tb\tc\t7\t2\n"
             "\n"
             "garbage line\n"
             "t\tDRAW\t?\ta\tb\tc\t5\t1\r\n"
             "t\tWIN\t2-1\ta\tb\tc\t5\t1",  // no final line end
             FileHeader());
    int skipped = -1;
    const Tally five = TallyOf(text, 5, nullptr, &skipped).all;
    Check(five.wins == 2 && five.losses == 1 && five.draws == 1 && skipped == 1, "TallyOf: one opponent; the header, a blank line not skipped, garbage skipped");
    const Tally all = TallyOf(text, 0, nullptr).all;
    Check(all.wins == 3 && all.Matches() == 5, "TallyOf: opponent 0 is everyone");
    Check(TallyOf(text, 5, nullptr).today.Matches() == 0, "TallyOf: no day given, nothing today");
    Check(TallyOf(nullptr, 0, nullptr).all.Matches() == 0 && TallyOf("", 5, "x").all.Matches() == 0, "TallyOf: no text");
    char longText[1200];
    memset(longText, 'x', 700);
    snprintf(longText + 700, sizeof longText - 700, "\nt\tWIN\t2-0\ta\tb\tc\t5\t1\n");
    Check(TallyOf(longText, 5, nullptr, &skipped).all.wins == 1 && skipped == 1, "TallyOf: a line longer than any entry is skipped, the next one read");
    // The header's own lines are comments.
    Check(!ParseEntry(FileHeader(), &back) && TallyOf(FileHeader(), 0, nullptr, &skipped).all.Matches() == 0 && skipped == 0,
          "the file's header reads as comments");

    // Today: from 06:00, so an evening past midnight is one day.
    const char* days =
        "2026-11-14 23:00:00\tWIN\t2-0\ta\tb\tc\t5\t1\n"
        "2026-11-15 05:59:59\tWIN\t2-0\ta\tb\tc\t5\t1\n"   // the night of the 14th
        "2026-11-15 06:00:00\tLOSS\t0-2\ta\tb\tc\t5\t1\n"  // the 15th begins
        "2026-11-15 22:10:00\tWIN\t2-1\ta\tb\tc\t5\t1\n"
        "2026-11-16 00:41:38\tWIN\t2-1\ta\tb\tc\t5\t1\n"   // still the evening of the 15th
        "2026-11-16 00:45:00\tLOSS\t1-2\ta\tb\tc\t7\t2\n";
    char since[24];
    DayStart(2026, 11, 16, 0, 2026, 11, 15, since, sizeof since);
    Check(!strcmp(since, "2026-11-15 06:00:00"), "DayStart: at 00:41 the day began yesterday at 06:00");
    DayStart(2026, 11, 16, 6, 2026, 11, 15, since, sizeof since);
    Check(!strcmp(since, "2026-11-16 06:00:00"), "DayStart: from 06:00 on, today");
    DayStart(2026, 1, 1, 3, 2025, 12, 31, since, sizeof since);
    Check(!strcmp(since, "2025-12-31 06:00:00"), "DayStart: across a year's end");
    DayStart(2026, 11, 16, 0, 2026, 11, 15, since, sizeof since);
    const Totals t5 = TallyOf(days, 5, since);
    Check(t5.today.wins == 2 && t5.today.losses == 1 && t5.all.wins == 4 && t5.all.losses == 1, "TallyOf: today's score against one opponent, and every day's");
    const Totals t0 = TallyOf(days, 0, since);
    Check(t0.today.Matches() == 4 && t0.all.Matches() == 6, "TallyOf: today against everyone");
}

void PanelTests() {
    char out[72];
    Tally s, a;
    Check(!PanelLine(s, a, true, out, sizeof out) && out[0] == 0, "PanelLine: nothing before the first counted match");
    s.wins = 3, s.losses = 2;
    a = s;
    Check(PanelLine(s, a, true, out, sizeof out) && !strcmp(out, "SCORE  YOU 3 - 2 OPP"), "PanelLine: the score; no total while it is this evening's");
    a.wins = 41, a.losses = 38;
    Check(PanelLine(s, a, true, out, sizeof out) && !strcmp(out, "SCORE  YOU 3 - 2 OPP      ALL TIME 41 - 38"), "PanelLine: the total over every evening");
    Check(PanelLine(s, a, false, out, sizeof out) && !strcmp(out, "SCORE  YOU 3 - 2 OPP"), "PanelLine: no total while the file is not read");
    s.draws = 1;
    Check(PanelLine(s, a, true, out, sizeof out) && !strcmp(out, "SCORE  YOU 3 - 2 OPP  DRAWS 1      ALL TIME 41 - 38"), "PanelLine: draws");
    s.wins = 99999, s.losses = 99999, a.wins = 999999, a.losses = 999999;
    Check(PanelLine(s, a, true, out, sizeof out) && strlen(out) < sizeof out, "PanelLine: large numbers fit the panel's state");
    char small[16];
    Check(PanelLine(s, a, true, small, sizeof small) && strlen(small) < sizeof small, "PanelLine: a small buffer is cut, never overrun");
    bool lettering = true;
    s = Tally{}, a = Tally{};
    s.wins = 12, s.losses = 11, s.draws = 1, a.wins = 140, a.losses = 138;
    PanelLine(s, a, true, out, sizeof out);
    for (const char* p = out; *p; ++p) lettering = lettering && *p >= 0x20 && *p <= 0x5F;
    Check(lettering, "PanelLine: capitals, digits and signs only (the panel's lettering)");
}

// Random matches: rounds by knock-out or by time, with rollback blips, waits for the other game and the lives' noise of
// a fight. The judge must name the match's winner every time, and its rounds whenever it gives them.
uint32_t gRng = 12345;
uint32_t Next() {
    gRng ^= gRng << 13, gRng ^= gRng >> 17, gRng ^= gRng << 5;
    return gRng;
}
float Unit() { return (float)(Next() % 10000) / 10000.0f; }

void RandomMatches() {
    int wrongResult = 0, wrongRounds = 0, roundsGiven = 0;
    for (int m = 0; m < 3000; ++m) {
        Fight f;
        f.gf = (m & 1) ? 100000 : 0;
        const int toWin = 2 + (int)(Next() % 2);  // two or three rounds to win
        int won[2] = {0, 0};
        f.Hold(60 + (int)(Next() % 200));
        int lastWinner = 0;
        while (won[0] < toWin && won[1] < toWin) {
            const int winner = 1 + (int)(Next() % 2);
            const bool byTime = Next() % 4 == 0;
            // the fight: many small moves down, some blips that a rollback takes back, some waits
            // The winner's life at the end; the loser's is 0 (knock-out) or lower than the winner's (time).
            const float high = 0.05f + 0.9f * Unit(), low = byTime ? high * (0.1f + 0.8f * Unit()) : 0.0f;
            const float ta = winner == 1 ? high : low, tb = winner == 2 ? high : low;
            const int steps = 5 + (int)(Next() % 20);
            for (int s = 0; s < steps; ++s) {
                const float ga = f.a + (ta - f.a) / (float)(steps - s), gb = f.b + (tb - f.b) / (float)(steps - s);
                f.Move(ga, gb, 10 + (int)(Next() % 60));
                if (Next() % 5 == 0) {  // a hit predicted wrong, taken back
                    const float keep = f.b;
                    f.b = keep * 0.8f;
                    f.Hold(2 + (int)(Next() % 7));
                    f.b = keep;
                    f.Frame();
                }
                if (Next() % 7 == 0)  // the session waits: the same frame for a while
                    for (int i = 0, n = (int)(Next() % 90); i < n; ++i) f.j.Sample(f.gf - 1, f.a, f.b);
            }
            f.Hold(60 + (int)(Next() % 150));  // the end stands: knock-out or time, then the pose
            ++won[winner - 1];
            lastWinner = winner;
            if (won[0] < toWin && won[1] < toWin) {
                if (m % 2 == 0) f.gf = 0;  // offline: GameFrame starts over at the round
                f.NewRound();
            }
        }
        const int side = 1 + (int)(Next() % 2);
        const Judge::Final r = f.j.Finish(side);
        if (r.result != (lastWinner == side ? kWin : kLoss)) ++wrongResult;
        if (r.roundsKnown) {
            ++roundsGiven;
            if (r.mine != won[side - 1] || r.theirs != won[2 - side]) ++wrongRounds;
        }
    }
    printf("random matches: 3000, result wrong in %d, rounds given in %d, wrong in %d\n", wrongResult, roundsGiven, wrongRounds);
    Check(wrongResult == 0, "random matches: every winner named");
    Check(wrongRounds == 0 && roundsGiven == 3000, "random matches: every match's rounds given, and right");
}
// The board (what the panel after a match and the fight's numbers show): names, today's and every day's score, the
// wins in a row, the last matches, today's fighters, and the fight's numbers by side.
void BoardTests() {
    char n[32];
    const struct {
        const char *code, *shown;
    } names[] = {{"CHAR_JOHNNYCAGE_B", "JOHNNY CAGE"}, {"CHAR_NIGHTWOLF", "NIGHTWOLF"},     {"CHAR_REPTILE_C", "REPTILE"},
                 {"CHAR_FREDDYKRUEGER", "FREDDY KRUEGER"}, {"CHAR_SubZero_A", "SUB-ZERO"}, {"char_cyber_sub_zero", "CYBER SUB-ZERO"},
                 {"CHAR_NEW_GUY_B", "NEW GUY"},        {"?", "?"},                          {"", "?"},
                 {"CHAR_X", "X"},                       {"PLAIN", "PLAIN"}};
    bool namesOk = true;
    for (const auto& k : names) {
        FighterDisplay(k.code, n, sizeof n);
        if (strcmp(n, k.shown)) {
            namesOk = false;
            printf("  FighterDisplay(\"%s\") = \"%s\", wanted \"%s\"\n", k.code, n, k.shown);
        }
    }
    Check(namesOk, "FighterDisplay: the table, costumes dropped, unknown names from the code's words");
    FighterDisplay("CHAR_FREDDYKRUEGER", n, 6);
    Check(!strcmp(n, "FREDD"), "FighterDisplay: cut to the buffer");

    // A file: two earlier days and an evening against opponent 7, with lines of other opponents and a bad line between.
    const char* text =
        "# header\n"
        "2026-11-14 22:00:00\tWIN\t2-0\tCHAR_SCORPION\tCHAR_KANO\tBGND_Pit\t7\t1\n"
        "2026-11-15 21:00:00\tLOSS\t1-2\tCHAR_SCORPION\tCHAR_KANO\tBGND_Pit\t7\t1\n"
        "2026-11-15 21:05:00\tLOSS\t0-2\tCHAR_SCORPION\tCHAR_KANO\tBGND_Pit\t7\t2\n"
        "2026-11-16 05:59:59\tWIN\t2-1\tCHAR_JADE\tCHAR_KANO\tBGND_Pit\t7\t1\n"   // the evening before (a day starts at 06:00)
        "2026-11-16 20:00:00\tWIN\t2-0\tCHAR_JOHNNYCAGE_B\tCHAR_NIGHTWOLF\tBGND_Desert\t7\t1\n"
        "2026-11-16 20:03:00\tWIN\t2-1\tCHAR_JOHNNYCAGE_B\tCHAR_NIGHTWOLF\tBGND_Desert\t9\t1\n"  // another opponent
        "garbage line\n"
        "2026-11-16 20:06:00\tWIN\t2-1\tCHAR_JOHNNYCAGE_B\tCHAR_NIGHTWOLF\tBGND_Desert\t7\t1\n"
        "2026-11-16 20:09:00\tWIN\t2-0\tCHAR_CYRAX\tCHAR_REPTILE_C\tBGND_Subway\t7\t2\n"
        "2026-11-16 20:12:00\tLOSS\t0-2\tCHAR_CYRAX\tCHAR_REPTILE_C\tBGND_Subway\t7\t2\n"
        "2026-11-16 20:15:00\tDRAW\t?\tCHAR_CYRAX\tCHAR_REPTILE_C\tBGND_Subway\t7\t2\n"
        "2026-11-16 20:18:00\tWIN\t2-1\tCHAR_JOHNNYCAGE_B\tCHAR_REPTILE_C\tBGND_Subway\t7\t1\n"
        "2026-11-16 20:21:00\tWIN\t2-0\tCHAR_JOHNNYCAGE\tCHAR_REPTILE_C\tBGND_Subway\t7\t1\n";
    Board b;
    int skipped = -1;
    BoardOf(text, 7, "2026-11-16 06:00:00", &b, &skipped);
    Check(b.on && b.allKnown && b.opponent == 7 && skipped == 1, "BoardOf: filled for the opponent, the bad line counted");
    Check(b.today.wins == 5 && b.today.losses == 1 && b.today.draws == 1, "BoardOf: today since 06:00 (5 won, 1 lost, 1 drawn)");
    Check(b.all.wins == 7 && b.all.losses == 3 && b.all.draws == 1, "BoardOf: every day, this opponent only");
    Check(b.streak == 2, "BoardOf: the last two won: a run of 2 (the draw before them ended the earlier one)");
    Check(b.bestMine == 3 && b.bestTheirs == 1, "BoardOf: today's best runs (3 won in a row, 1 lost)");
    Check(b.rows == kBoardRows && !strcmp(b.row[0].when, "20:21") && b.row[0].result == kWin && b.row[0].mine == 2 && b.row[0].theirs == 0 &&
              !strcmp(b.row[0].me, "JOHNNY CAGE") && !strcmp(b.row[0].them, "REPTILE") && b.row[0].today,
          "BoardOf: the newest row first, with its names, rounds and time");
    Check(b.row[2].result == kDraw && !b.row[2].roundsKnown, "BoardOf: a drawn match's row, its rounds not known");
    Check(b.row[6].today && !strcmp(b.row[6].when, "20:00") && !b.row[7].today && !strcmp(b.row[7].when, "16 NOV") && b.row[7].result == kWin,
          "BoardOf: an earlier day's row says its date (05:59 belongs to the evening before)");
    Check(b.fighters == 2 && !strcmp(b.fighter[0].name, "JOHNNY CAGE") && b.fighter[0].t.wins == 4 && b.fighter[0].t.losses == 0 &&
              !strcmp(b.fighter[1].name, "CYRAX") && b.fighter[1].t.wins == 1 && b.fighter[1].t.losses == 1 && b.fighter[1].t.draws == 1,
          "BoardOf: today's fighters, the most played first, costumes counted as one fighter");
    Check(WinsOf(b, 1, 1) == 5 && WinsOf(b, 1, 2) == 1 && WinsOf(b, 2, 2) == 5 && WinsOf(b, 2, 1) == 1, "WinsOf: this player's wins on its side");
    Check(RunOf(b, 1, 1) == 2 && RunOf(b, 1, 2) == 0 && RunOf(b, 2, 2) == 2 && RunOf(b, 2, 1) == 0, "RunOf: the run on the side of the one on it");

    Board other;
    BoardOf(text, 9, "2026-11-16 06:00:00", &other);
    Check(other.today.wins == 1 && other.all.Matches() == 1 && other.rows == 1 && other.streak == 1, "BoardOf: another opponent's board");
    Board none;
    BoardOf(text, 5, "2026-11-16 06:00:00", &none);
    Check(none.on && none.today.Matches() == 0 && none.rows == 0 && none.fighters == 0 && none.streak == 0, "BoardOf: an opponent never met: an empty board");
    Board empty;
    BoardOf("", 7, nullptr, &empty);
    Check(empty.on && empty.all.Matches() == 0, "BoardOf: an empty file");

    // Runs, one match at a time: losses turn the run, a draw ends it, today's best is kept.
    Board r;
    Entry e;
    snprintf(e.time, sizeof e.time, "2026-11-16 21:00:00");
    const char seq[] = "WWWLLDWLLLLW";
    const int wantStreak[] = {1, 2, 3, -1, -2, 0, 1, -1, -2, -3, -4, 1};
    bool runsOk = true;
    for (int i = 0; seq[i]; ++i) {
        e.result = seq[i] == 'W' ? kWin : seq[i] == 'L' ? kLoss : kDraw;
        r.Add(e, true);
        if (r.streak != wantStreak[i]) runsOk = false;
    }
    Check(runsOk, "Board::Add: the run after each result");
    Check(r.bestMine == 3 && r.bestTheirs == 4 && r.today.Matches() == 12 && r.rows == kBoardRows, "Board::Add: today's best runs, the rows capped");
    Entry undecided;
    r.Add(undecided, true);
    Check(r.today.Matches() == 12, "Board::Add: an undecided match changes nothing");
    // Earlier days' matches count for every day and the run, not for today.
    Board d;
    e.result = kWin;
    for (int i = 0; i < 3; ++i) d.Add(e, false);
    Check(d.all.wins == 3 && d.today.Matches() == 0 && d.streak == 3 && d.bestMine == 0 && d.fighters == 0, "Board::Add: an earlier day's match");
    // More fighters than the board holds: the first six kept.
    Board f;
    const char* many[] = {"CHAR_A", "CHAR_B", "CHAR_C", "CHAR_D", "CHAR_E", "CHAR_F", "CHAR_G"};
    for (const char* m : many) {
        snprintf(e.myFighter, sizeof e.myFighter, "%s", m);
        f.Add(e, true);
    }
    Check(f.fighters == kBoardFighters && f.today.Matches() == 7, "Board::Add: at most six fighters, every match counted");
    // A row of a short time text, and of a month out of range.
    Board t;
    snprintf(e.time, sizeof e.time, "bad");
    t.Add(e, false);
    snprintf(e.time, sizeof e.time, "2026-13-01 10:00:00");
    t.Add(e, false);
    Check(!strcmp(t.row[1].when, "?") && !strcmp(t.row[0].when, "10:00"), "Board::Add: a time it cannot read");
}
}  // namespace

int main() {
    RoundTests();
    FileTests();
    PanelTests();
    RandomMatches();
    BoardTests();
    printf("%d checks, %d failed\n", gChecks, gFailed);
    if (!gFailed) printf("ALL CHECKS PASSED\n");
    return gFailed ? 1 : 0;
}
