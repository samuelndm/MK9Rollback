#pragma once

// The button preset each player plays with ("AJUSTE DOS COMANDOS": PADRÃO, 1-4), kept across game starts by the mod.
// MK9 saves the presets' buttons in its profile but not which preset a player chose: its four player records (static,
// 0x00E89D7C + i * 0xC8C, built once by the game's static initializer 0x00C42980 -> 0x00815680 -> 0x008A1580) start
// every game with +0xC48 = +0xC4C = 0, preset 1. Only two menus write the fields: the pause menu (pause_menu.mko
// ShowPresetSelect, then native 64916:62 = 0x0087F040 copies each player's preset into its pads) and the character
// select's player options (PlayerSelect.mko; the fight's start applies it, gamelogic.mko -> 0x0087F040).
// Values: 0-3 = presets 1-4, -1 = PADRÃO (the device's default), -2 = REMOTO (an online opponent's pad; never kept).
// +0xC48 is the preset of the pad the player signed in with (0x0044CC00), +0xC4C of the other one; the pause menu
// writes both when the player has one pad.
//
// The decisions are here, without the game (tests\host\padpreset_test.cpp); PadPreset.cpp reads and writes the records,
// the file and the log.
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace PadPreset {

constexpr int kPlayers = 4;

struct Pair {
    int32_t a = 0, b = 0;  // record +0xC48, +0xC4C
};
inline bool Same(Pair x, Pair y) { return x.a == y.a && x.b == y.b; }
constexpr Pair kStart{0, 0};  // what the record constructor writes at the game's start
inline bool Keepable(int32_t v) { return v >= -1 && v <= 3; }
inline bool Keepable(Pair p) { return Keepable(p.a) && Keepable(p.b); }

// What the file holds: the last preset pair each player chose (has[i] false: that player never chose while the mod ran).
struct Saved {
    bool has[kPlayers] = {};
    Pair pair[kPlayers] = {};
};

// The file's text: two comment lines, then "player <1-4> <a> <b>" per player that chose. Returns the length written
// (always terminated; 0 when n is too small for anything).
inline int Format(const Saved& s, char* out, size_t n) {
    if (!out || n == 0) return 0;
    out[0] = 0;
    int len = snprintf(out, n,
                       "# MK9 Rollback: the controller preset (AJUSTE DOS COMANDOS) each player last chose; the mod sets it again when the game starts.\n"
                       "# player <1-4> <preset of the pad the player signed in with> <of the other pad>: 0-3 = presets 1-4, -1 = PADRAO\n");
    if (len < 0 || (size_t)len >= n) {
        out[0] = 0;
        return 0;
    }
    for (int i = 0; i < kPlayers; ++i) {
        if (!s.has[i]) continue;
        const int k = snprintf(out + len, n - (size_t)len, "player %d %d %d\n", i + 1, (int)s.pair[i].a, (int)s.pair[i].b);
        if (k < 0 || (size_t)k >= n - (size_t)len) {
            out[len] = 0;
            return len;
        }
        len += k;
    }
    return len;
}

// Reads Format's text (or a person's edit of it). Lines that are not "player <1-4> <a> <b>" with keepable values are
// skipped and counted in *skipped; a later line for the same player wins. Never reads past the terminator.
inline Saved Parse(const char* text, int* skipped = nullptr) {
    Saved s;
    int bad = 0;
    for (const char* p = text; p && *p;) {
        const char* end = strchr(p, '\n');
        const size_t n = end ? (size_t)(end - p) : strlen(p);
        const char* from = p;
        p = end ? end + 1 : p + n;
        size_t k = 0;
        while (k < n && (from[k] == ' ' || from[k] == '\t')) ++k;
        if (k == n || from[k] == '\r' || from[k] == '#') continue;  // blank line or comment, of any length
        char line[96];
        if (n - k >= sizeof line) {  // longer than any player line Format writes: not read (a cut copy could look good)
            ++bad;
            continue;
        }
        memcpy(line, from + k, n - k);
        line[n - k] = 0;
        int player = 0, a = 0, b = 0;
        char tail = 0;
        const int got = sscanf(line, "player %d %d %d %c", &player, &a, &b, &tail);
        if (got != 3 || player < 1 || player > kPlayers || !Keepable(Pair{a, b})) {
            ++bad;
            continue;
        }
        s.has[player - 1] = true;
        s.pair[player - 1] = Pair{a, b};
    }
    if (skipped) *skipped = bad;
    return s;
}

enum class Act { kNone, kRestore, kSave };

// One look at a player's record, outside fights and netplay sessions. The first look of a game is the restore: a
// record that still holds the constructor's 0/0 gets the saved pair (MK9 itself never sets it; only the player's
// menus do). Every later change of the record is the player's choice and is saved, unless it is a value that is not
// kept (REMOTO, garbage). A first look at a record the player already changed (the mod loaded after the choice) saves it.
struct Tracker {
    bool seen[kPlayers] = {};
    Pair last[kPlayers] = {};

    // *out: for kRestore the pair to write into the record; for kSave the pair to keep.
    Act Look(int i, Pair now, const Saved& saved, Pair* out) {
        if (i < 0 || i >= kPlayers) return Act::kNone;
        if (!seen[i]) {
            seen[i] = true;
            if (Same(now, kStart) && saved.has[i] && !Same(saved.pair[i], kStart) && Keepable(saved.pair[i])) {
                last[i] = saved.pair[i];
                if (out) *out = saved.pair[i];
                return Act::kRestore;
            }
            last[i] = now;
            if (Same(now, kStart) || !Keepable(now) || (saved.has[i] && Same(saved.pair[i], now))) return Act::kNone;
            if (out) *out = now;
            return Act::kSave;
        }
        if (Same(now, last[i])) return Act::kNone;
        last[i] = now;
        if (!Keepable(now) || (saved.has[i] && Same(saved.pair[i], now))) return Act::kNone;
        if (!saved.has[i] && Same(now, kStart)) return Act::kNone;  // back to preset 1 with nothing kept: nothing to keep
        if (out) *out = now;
        return Act::kSave;
    }
};

// PadPreset.cpp, the game's side (not in the host test).
void SetFolder(const wchar_t* dir);  // where controller_presets.txt is kept (the DLL's folder); before the hooks go live
void Tick();                         // every engine tick on the game thread (GameLoopHook); looks twice a second

}  // namespace PadPreset
