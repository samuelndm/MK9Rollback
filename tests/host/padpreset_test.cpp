// src\game\PadPreset.h: the controller preset a player chose comes back at the next game start, and nothing else does.
#include <cstdio>
#include <cstring>

#include "PadPreset.h"

using namespace PadPreset;

namespace {
int gChecks = 0, gFailed = 0;

void Check(bool ok, const char* what) {
    ++gChecks;
    if (ok) return;
    ++gFailed;
    printf("FAIL: %s\n", what);
}

bool SameSaved(const Saved& x, const Saved& y) {
    for (int i = 0; i < kPlayers; ++i) {
        if (x.has[i] != y.has[i]) return false;
        if (x.has[i] && !Same(x.pair[i], y.pair[i])) return false;
    }
    return true;
}

// What PadPreset.cpp does with a look's answer: a restore is written into the record, a save goes into the file's data.
Act LookAs(Tracker& t, int i, Pair* record, Saved& saved) {
    Pair p;
    const Act a = t.Look(i, *record, saved, &p);
    if (a == Act::kRestore) *record = p;
    if (a == Act::kSave) {
        saved.has[i] = true;
        saved.pair[i] = p;
    }
    return a;
}

void FileTests() {
    Saved s;
    s.has[0] = true, s.pair[0] = Pair{1, 1};
    s.has[2] = true, s.pair[2] = Pair{-1, 3};
    char text[512];
    const int n = Format(s, text, sizeof text);
    Check(n > 0 && (size_t)n == strlen(text), "Format: returns the text's length");
    Check(strstr(text, "player 1 1 1\n") && strstr(text, "player 3 -1 3\n") && !strstr(text, "player 2") && !strstr(text, "player 4"),
          "Format: one line per player that chose");
    int skipped = -1;
    Check(SameSaved(Parse(text, &skipped), s) && skipped == 0, "Parse(Format(x)) == x, no line skipped");
    Check(SameSaved(Parse("", &skipped), Saved{}) && skipped == 0, "Parse: empty text");
    Check(SameSaved(Parse(nullptr, &skipped), Saved{}) && skipped == 0, "Parse: no text");

    Saved crlf = Parse("# comment\r\n\r\nplayer 2 2 2\r\n  player 1 0 -1\r\n", &skipped);
    Check(crlf.has[1] && Same(crlf.pair[1], Pair{2, 2}) && crlf.has[0] && Same(crlf.pair[0], Pair{0, -1}) && !crlf.has[2] && skipped == 0,
          "Parse: CRLF line ends, blank lines, comments, leading spaces");
    Saved last = Parse("player 1 1 1\nplayer 1 3 3", &skipped);
    Check(last.has[0] && Same(last.pair[0], Pair{3, 3}) && skipped == 0, "Parse: a later line for the same player wins; no final newline");
    Saved bad = Parse("player 1 -2 -2\nplayer 2 4 0\nplayer 0 1 1\nplayer 5 1 1\nplayer 3 1\nplayer 4 1 1 x\nplayers 1 1 1\nhello\n", &skipped);
    Check(SameSaved(bad, Saved{}) && skipped == 8, "Parse: REMOTO, out-of-range presets and players, short, long and foreign lines skipped");
    char longLine[400];
    memset(longLine, ' ', sizeof longLine);
    memcpy(longLine, "player 1 2 2", 12);  // then 300 spaces: its first 95 characters alone would read as a good line
    longLine[312] = '\n';
    memcpy(longLine + 313, "player 2 1 1", 13);
    Saved longOne = Parse(longLine, &skipped);
    Check(!longOne.has[0] && longOne.has[1] && Same(longOne.pair[1], Pair{1, 1}) && skipped == 1, "Parse: an over-long line is skipped, the next one read");

    char small[8];
    Check(Format(s, small, sizeof small) == 0 && small[0] == 0, "Format: too small a buffer gives an empty, terminated text");
    char mid[262];  // the two comment lines and the first player's line, not the second
    const int m = Format(s, mid, sizeof mid);
    Check(m >= 0 && (size_t)m < sizeof mid && mid[m] == 0, "Format: a buffer that holds part of it stays terminated");
}

void TrackerTests() {
    {  // a game start with a saved preset: the record still at 0/0 gets it; it is not saved again
        Saved saved;
        saved.has[0] = true, saved.pair[0] = Pair{1, 1};
        Tracker t;
        Pair rec = kStart;
        Check(LookAs(t, 0, &rec, saved) == Act::kRestore && Same(rec, Pair{1, 1}), "start: the saved preset is put back");
        Check(LookAs(t, 0, &rec, saved) == Act::kNone && Same(rec, Pair{1, 1}), "start: the next look changes nothing");
        rec = Pair{2, 2};
        Check(LookAs(t, 0, &rec, saved) == Act::kSave && Same(saved.pair[0], Pair{2, 2}), "a later choice is kept");
        rec = Pair{0, 0};
        Check(LookAs(t, 0, &rec, saved) == Act::kSave && saved.has[0] && Same(saved.pair[0], kStart), "going back to preset 1 is kept too");
    }
    {  // nothing saved: the start's 0/0 is left alone; preset 1 chosen with nothing saved is nothing to keep
        Saved saved;
        Tracker t;
        Pair rec = kStart;
        Check(LookAs(t, 1, &rec, saved) == Act::kNone && Same(rec, kStart) && !saved.has[1], "start with nothing saved: nothing done");
        rec = Pair{-1, -1};
        Check(LookAs(t, 1, &rec, saved) == Act::kSave && Same(saved.pair[1], Pair{-1, -1}), "PADRAO is kept");
    }
    {  // the mod loaded after the player chose: the first look keeps it (unless it is what the file has)
        Saved saved;
        Tracker t;
        Pair rec{2, 2};
        Check(LookAs(t, 0, &rec, saved) == Act::kSave && Same(saved.pair[0], Pair{2, 2}) && Same(rec, Pair{2, 2}),
              "a choice made before the mod loaded is kept, not overwritten");
        Tracker t2;
        Check(LookAs(t2, 0, &rec, saved) == Act::kNone, "the same choice already in the file: nothing written");
        Saved other;
        other.has[0] = true, other.pair[0] = Pair{3, 3};
        Tracker t3;
        Pair rec3{1, 1};
        Check(LookAs(t3, 0, &rec3, other) == Act::kSave && Same(rec3, Pair{1, 1}) && Same(other.pair[0], Pair{1, 1}),
              "a record the player changed before the mod loaded wins over the file");
    }
    {  // values that are not kept
        Saved saved;
        saved.has[0] = true, saved.pair[0] = Pair{1, 1};
        Tracker t;
        Pair rec = kStart;
        LookAs(t, 0, &rec, saved);
        rec = Pair{-2, -2};
        Check(LookAs(t, 0, &rec, saved) == Act::kNone && Same(saved.pair[0], Pair{1, 1}), "REMOTO is not kept");
        rec = Pair{1, 1};
        Check(LookAs(t, 0, &rec, saved) == Act::kNone, "back from REMOTO to the kept preset: nothing written");
        rec = Pair{7, 1};
        Check(LookAs(t, 0, &rec, saved) == Act::kNone && Same(saved.pair[0], Pair{1, 1}), "a value outside the presets is not kept");
        Tracker t2;
        Pair odd{-2, 0};
        Saved none;
        Check(LookAs(t2, 2, &odd, none) == Act::kNone && !none.has[2], "first look at REMOTO: nothing kept");
        Saved broken;
        broken.has[3] = true, broken.pair[3] = Pair{9, 9};
        Tracker t3;
        Pair r3 = kStart;
        Check(LookAs(t3, 3, &r3, broken) == Act::kNone && Same(r3, kStart), "a saved value outside the presets is never written");
        Check(t3.Look(-1, kStart, broken, nullptr) == Act::kNone && t3.Look(kPlayers, kStart, broken, nullptr) == Act::kNone,
              "a player index outside 0-3 does nothing");
    }
    {  // players are separate: one player's choice is not another's
        Saved saved;
        saved.has[1] = true, saved.pair[1] = Pair{3, 3};
        Tracker t;
        Pair recs[kPlayers] = {kStart, kStart, kStart, kStart};
        for (int i = 0; i < kPlayers; ++i) LookAs(t, i, &recs[i], saved);
        Check(Same(recs[0], kStart) && Same(recs[1], Pair{3, 3}) && Same(recs[2], kStart) && Same(recs[3], kStart),
              "only the player that chose gets a preset back");
    }
}

// Game starts one after another: the file's text carries the choices from one start to the next (Format, Parse); in a
// game the player changes presets between looks (only the value at a look is seen), MK9 shows REMOTO now and then.
// After each start's first look, every record holds the last keepable preset the looks of the games before saw.
uint32_t gRng = 12345;
uint32_t Rand(uint32_t n) {
    gRng = gRng * 1103515245u + 12345u;
    return (gRng >> 8) % n;
}
Pair RandomPair() {
    const int32_t a = (int32_t)Rand(5) - 1;  // -1..3
    return Rand(4) ? Pair{a, a} : Pair{a, (int32_t)Rand(5) - 1};
}

void Simulation() {
    char file[512] = "";
    bool known[kPlayers] = {};
    Pair expect[kPlayers] = {};
    int starts = 0, restores = 0, saves = 0, wrong = 0;
    for (int game = 0; game < 3000; ++game) {
        ++starts;
        Saved saved = Parse(file);
        Tracker t;
        Pair recs[kPlayers] = {kStart, kStart, kStart, kStart};
        // A game whose mod loads late: a player may have chosen before the first look.
        if (Rand(6) == 0) {
            const int i = (int)Rand(kPlayers);
            recs[i] = RandomPair();
        }
        const Pair before[kPlayers] = {recs[0], recs[1], recs[2], recs[3]};
        for (int i = 0; i < kPlayers; ++i) {
            const Act a = LookAs(t, i, &recs[i], saved);
            restores += a == Act::kRestore;
            saves += a == Act::kSave;
            // The first look: a record still at the start's 0/0 has the last kept preset; one the player changed keeps it.
            const Pair want = Same(before[i], kStart) ? (known[i] ? expect[i] : kStart) : before[i];
            if (!Same(recs[i], want)) {
                if (wrong++ < 5)
                    printf("FAIL: game %d player %d: %d/%d after the first look, wanted %d/%d\n", game, i + 1, (int)recs[i].a, (int)recs[i].b,
                           (int)want.a, (int)want.b);
            }
            if (Keepable(recs[i]) && !(Same(recs[i], kStart) && !known[i])) {
                known[i] = known[i] || !Same(recs[i], kStart);
                expect[i] = recs[i];
            }
        }
        const int looks = 1 + (int)Rand(20);
        for (int l = 0; l < looks; ++l) {
            const int changes = (int)Rand(3);
            for (int c = 0; c < changes; ++c) {
                const int i = (int)Rand(kPlayers);
                recs[i] = Rand(10) == 0 ? Pair{-2, -2} : RandomPair();
            }
            for (int i = 0; i < kPlayers; ++i) {
                const Act a = LookAs(t, i, &recs[i], saved);
                restores += a == Act::kRestore;
                saves += a == Act::kSave;
                if (Keepable(recs[i]) && (known[i] || !Same(recs[i], kStart))) {
                    known[i] = true;
                    expect[i] = recs[i];
                }
            }
        }
        Format(saved, file, sizeof file);
    }
    ++gChecks;
    if (wrong) ++gFailed;
    printf("%d game starts, %d presets put back, %d choices kept, %d starts wrong\n", starts, restores, saves, wrong);
}
}  // namespace

int main() {
    FileTests();
    TrackerTests();
    Simulation();
    printf("%d checks, %d failed\n", gChecks, gFailed);
    if (gFailed) return 1;
    printf("ALL CHECKS PASSED\n");
    return 0;
}
