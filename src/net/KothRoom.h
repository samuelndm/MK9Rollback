#pragma once
#include <cstdint>
#include <cstring>

// King of the Hill rooms: MK9's own online mode where the winner stays and the others wait their turn and watch. The
// online mode (OnlineMode.cpp) takes rollback into such a room only while it holds just the two players, and treats
// MK9's connection timers there per connection. This header holds the decisions; it reads no game memory and has a
// host-side test (tests\host\koth_test.cpp). OnlineMode.cpp reads the game.
//
// Why a check of its own (2026-11-17, from the log of 2026-11-14 and MKKE.exe):
//  - MK9's game states have names (0x00406610): 0 GS_INIT, 1 GS_LOADING_FIGHT, 2 GS_ATTRACT, 3 GS_MAINMENU, 4 GS_ONLINE,
//    5 GS_KOTH, 6 GS_PROFILES, 7 GS_PSELECT, 8 GS_PLAY, 9 GS_GAME_OVER, 10 GS_MOVIE, 11 GS_FATALITY, 12 GS_LADDER. A King
//    of the Hill room goes to GS_KOTH (5) before its first match and back to it after every match (results 9 -> 5 -> 7
//    -> 1 -> 8 -> 9 -> 5); a one-on-one room never does (it goes back to 4).
//  - The match description the one-on-one check reads (GameContext, online manager +0x73C: type, match type 0 DM /
//    1 KOTH) was all zeros in that King of the Hill room, an invited one: the value of a one-on-one room. So the room was
//    taken for one-on-one, and nothing checked who else was in it.
//  - MKOnlineSession (online manager +0x1F4) keeps its connections in four arrays of pointers (data, count, capacity)
//    at +0x40D0, +0x40DC, +0x40E8 ("IncomingConnections") and +0x40F4 ("Connections"; MKOnlineSession::AddConnection
//    0x00915C20 adds there); its dump 0x0091A1F0 lists them. A connection's name is the text at its +0x1778
//    (0x0081AF30 on +0x1338); this player's own name is the session's +0x4B38 (MK9's own spectator test 0x00911D50
//    compares it with the two fighters' names, at most 64 characters). Whether MK9 keeps a connection for the local
//    player in those arrays is not known, so one with this player's name is not counted. Names are compared, never
//    logged.
//  - Watchers receive the fight through the exchange the mod replaces (MKOnlineGamepadData keeps their frames:
//    GetSpectatorOutgoingDataSinceFrame 0x00881190): with a third player in the room a rollback fight would leave them
//    nothing to watch, and a stage the mod chose would not be theirs. Rollback with watchers is not built.
namespace KothRoom {

constexpr uint32_t kKothState = 5;
// The states a room is left for: attract, main menu, the online menus, profiles. Not 12: despite its name (GS_LADDER)
// MK9 passes through it for one tick between the character select and the load of every online fight (Refight.cpp's
// kPreLoadState).
inline bool MenuState(uint32_t s) { return s == 2 || s == 3 || s == 4 || s == 6; }

enum class Change : uint8_t { kNone, kEntered, kLeft };

// In a King of the Hill room: from MK9's King of the Hill state (or a match description that says King of the Hill)
// until MK9 is back in its menus or offline.
struct Room {
    bool in = false;
    Change Update(uint32_t gameState, bool online, bool contextKoth) {
        if (in) {
            if (!online || MenuState(gameState)) {
                in = false;
                return Change::kLeft;
            }
            return Change::kNone;
        }
        if (online && !MenuState(gameState) && (gameState == kKothState || contextKoth)) {
            in = true;
            return Change::kEntered;
        }
        return Change::kNone;
    }
};

// ---- who is in the room ----
constexpr uint32_t kLists = 4;
constexpr uint32_t kMaxPerList = 16;  // a room holds 8 players; more entries than this: not read as a room
constexpr uint32_t kNameMax = 64;

struct Members {
    bool read = false;       // the session, its four arrays and every entry could be read
    uint32_t entries = 0;    // pointers in the four arrays together
    uint32_t distinct = 0;   // different connections among them (one can sit in two arrays while it moves)
    uint32_t self = 0;       // of those, ones with this player's name
    uint32_t unnamed = 0;    // ones whose name was empty or could not be read
};

enum class Verdict : uint8_t { kUnknown, kTwoPlayers, kOthers };

inline const char* VerdictText(Verdict v) {
    return v == Verdict::kTwoPlayers ? "just the two players" : v == Verdict::kOthers ? "more players than the two" : "not known";
}

// Two players: everything read, every connection named, exactly one that is not this player. More: two or more.
// Anything else (nothing read, a nameless connection, no other player at all) is not known, which keeps MK9's netcode.
inline Verdict Judge(const Members& m) {
    if (!m.read || m.unnamed) return Verdict::kUnknown;
    const uint32_t others = m.distinct >= m.self ? m.distinct - m.self : 0;
    return others == 1 ? Verdict::kTwoPlayers : others >= 2 ? Verdict::kOthers : Verdict::kUnknown;
}

// Counts the session's connections through three readers, so that the test can feed it memory of its own:
//   list(i, &data, &count)       array i (0-3): its data pointer and count; false when not readable
//   entry(data, k, &conn)        its k-th pointer; false when not readable
//   name(conn, out[kNameMax])    the connection's name, cut at kNameMax; false when not readable
//   own                          this player's name (kNameMax bytes, need not end in a zero)
template <class ListFn, class EntryFn, class NameFn>
Members Count(ListFn list, EntryFn entry, NameFn name, const char* own) {
    Members m;
    if (!own || !own[0]) return m;  // this player's name unknown: nothing can be told apart
    uintptr_t seen[kLists * kMaxPerList];
    uint32_t nSeen = 0;
    for (uint32_t i = 0; i < kLists; ++i) {
        uintptr_t data = 0;
        uint32_t count = 0;
        if (!list(i, &data, &count) || count > kMaxPerList || (count && !data)) return m;
        for (uint32_t k = 0; k < count; ++k) {
            uintptr_t conn = 0;
            if (!entry(data, k, &conn)) return m;
            ++m.entries;
            if (!conn) {
                ++m.unnamed;  // a hole in MK9's array: not a room this check knows
                continue;
            }
            bool dup = false;
            for (uint32_t s = 0; s < nSeen && !dup; ++s) dup = seen[s] == conn;
            if (dup) continue;
            seen[nSeen++] = conn;
            ++m.distinct;
            char n[kNameMax];
            if (!name(conn, n) || !n[0]) {
                ++m.unnamed;
                continue;
            }
            if (!strncmp(n, own, kNameMax)) ++m.self;
        }
    }
    m.read = true;
    return m;
}

// The verdict the online mode uses. "Two players" must hold for kTwoHoldMs before it is used (an array read while MK9
// changes it can miss an entry); anything else counts at once, so a third player is never overlooked for long.
constexpr uint64_t kTwoHoldMs = 300;
struct Sticky {
    Verdict shown = Verdict::kUnknown, last = Verdict::kUnknown;
    uint64_t lastSince = 0;
    Verdict Feed(Verdict v, uint64_t nowMs) {
        if (v != last) {
            last = v;
            lastSince = nowMs;
        }
        if (v != Verdict::kTwoPlayers) shown = v;
        else if (nowMs - lastSince >= kTwoHoldMs) shown = v;
        return shown;
    }
    void Reset() { *this = Sticky{}; }
};

// ---- MK9's connection timers in the room ----
// MK9 asks each of its connections how long it has been since the last packet (0x004F5E10) and closes one that stays
// silent too long. While a rollback fight runs the mod skips MK9's exchange, and one of MK9's connections to the
// opponent can then go silent for the rest of the room: logs of 2026-11-03 to 2026-11-16 show such silences of 20 s
// to 37 minutes that began in rollback fights, in one-on-one rooms too, while fights, rematches and the votes went on.
// The mod hides them: 0 during the fight and for a grace of 15 s after it, then, in a one-on-one room, at most the time
// since MK9's exchange last got the opponent's data, which MK9's rematch screen keeps fresh. A King of the Hill room has
// no rematch screen: after the results it waits in GS_KOTH while MK9's exchange gets nothing (2026-11-14: one second
// with data, then none), so that time grew and, 15 s after the second match, MK9 was shown a connection silent for 6 s
// and more, 12 s by the time the game closed in MK9's web-services job (a stale object). Here each connection is kept
// apart instead:
//   - starved: one that went silent during a rollback fight of this game (its silence began at most a second before
//     this game engaged, or later) and that was already there before it engaged. MK9 is shown at most the time since
//     the opponent's game was last heard from (its mod's hellos, which keep coming while the room lasts, or MK9's
//     exchange): hidden while the opponent's game runs, and timing out as MK9's own would once it stops.
//   - any other (a later player's, one silent since before the fight): MK9's own value.
// A connection that hears a packet again is not starved any more.
constexpr uint32_t kSilentMs = 1000;        // MK9's value from which a connection counts as silent
constexpr uint64_t kEngageMarginMs = 1000;  // a silence that began this much before the engagement still counts
constexpr uint32_t kBookSize = 16;
struct IdleBook {
    struct Entry {
        const void* self = nullptr;
        uint64_t firstSeen = 0, lastUsed = 0;
        bool starved = false;
    };
    Entry e[kBookSize];
    uint32_t starvedMarks = 0;  // connections marked starved in this room (for the log)
    bool lastWasStarved = false;

    Entry& Find(const void* self, uint64_t now) {
        Entry* oldest = &e[0];
        for (Entry& x : e) {
            if (x.self == self) {
                x.lastUsed = now;
                return x;
            }
            if (!x.self) {
                oldest = &x;
                break;
            }
            if (x.lastUsed < oldest->lastUsed) oldest = &x;
        }
        *oldest = Entry{};
        oldest->self = self;
        oldest->firstSeen = oldest->lastUsed = now;
        return *oldest;
    }
    // What MK9 is shown for this connection. now: the ms clock; real: MK9's own value; hidden: a rollback fight skips
    // MK9's exchange, or the grace after one runs; engagedAt: when this game last engaged in this room (0: never);
    // lastHeard: the latest sign of the opponent's game (0: none in this room).
    uint32_t Show(const void* self, uint64_t now, uint32_t real, bool hidden, uint64_t engagedAt, uint64_t lastHeard) {
        Entry& x = Find(self, now);
        lastWasStarved = false;
        if (real < kSilentMs) {
            x.starved = false;
            return hidden ? 0 : real;
        }
        const uint64_t silentSince = now >= real ? now - real : 0;
        if (hidden) {
            if (!x.starved && engagedAt && x.firstSeen < engagedAt && silentSince + kEngageMarginMs >= engagedAt) {
                x.starved = true;
                ++starvedMarks;
            }
            return 0;
        }
        if (!x.starved) return real;
        lastWasStarved = true;
        if (!lastHeard) return real;
        const uint64_t since = now > lastHeard ? now - lastHeard : 0;
        return since < real ? (uint32_t)since : real;
    }
    void Clear() { *this = IdleBook{}; }
};

}  // namespace KothRoom
