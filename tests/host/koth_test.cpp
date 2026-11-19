// src\net\KothRoom.h: King of the Hill rooms. When a game is in one (MK9's states as the 2026-11-14 room went through
// them), who is in it (the session's four connection arrays read through fake memory: a connection for this player,
// one in two arrays, holes, nameless ones, arrays that cannot be read), the verdict's hold, and what MK9 is shown of its
// connection timers: the 2026-11-14 evening replayed (a connection that went silent 65 s into the second rollback fight
// and stayed so), a player who comes later, silences from before a fight, 4000 random rooms.
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "KothRoom.h"

using namespace KothRoom;

namespace {
int gChecks = 0, gFailed = 0;

void Check(bool ok, const char* what) {
    ++gChecks;
    if (ok) return;
    ++gFailed;
    printf("FAIL: %s\n", what);
}

uint32_t gRand = 12345;
uint32_t Rand() {
    gRand = gRand * 1103515245u + 12345u;
    return gRand >> 8;
}

// ---- the room ----
void RoomTests() {
    Room r;
    // 2026-11-14: a one-on-one room (4 between matches), then the King of the Hill room.
    const uint32_t oneOnOne[] = {4, 7, 12, 1, 8, 9, 4, 7, 1, 8, 9, 4};
    for (uint32_t s : oneOnOne) Check(r.Update(s, true, false) == Change::kNone && !r.in, "a one-on-one room is not King of the Hill");
    Check(r.Update(5, true, false) == Change::kEntered && r.in, "GS_KOTH enters the room");
    const uint32_t koth[] = {5, 7, 12, 1, 8, 11, 9, 5, 7, 1, 8, 9, 5, 7, 0};
    for (uint32_t s : koth) Check(r.Update(s, true, false) == Change::kNone && r.in, "the room's own states keep it");
    Check(r.Update(4, true, false) == Change::kLeft && !r.in, "the online menus leave the room");
    Check(r.Update(4, true, true) == Change::kNone && !r.in, "a match description in the menus does not enter");
    Check(r.Update(7, true, true) == Change::kEntered && r.in, "a match description that says King of the Hill enters");
    Check(r.Update(7, false, true) == Change::kLeft && !r.in, "offline leaves the room");
    Check(r.Update(5, false, false) == Change::kNone && !r.in, "GS_KOTH offline is not a room");
    const uint32_t menus[] = {2, 3, 6};
    for (uint32_t s : menus) {
        Room q;
        q.Update(5, true, false);
        Check(q.Update(s, true, false) == Change::kLeft, "attract, main menu, profiles leave the room");
    }
}

// ---- who is in the room: fake memory ----
struct FakeConn {
    uintptr_t addr;
    const char* name;   // nullptr: not readable
};
struct FakeSession {
    uint32_t count[kLists] = {};
    uintptr_t data[kLists] = {};
    uintptr_t entries[kLists][kMaxPerList + 4] = {};
    bool listReadable[kLists] = {true, true, true, true};
    bool entryReadable = true;
    FakeConn conns[16];
    int nConns = 0;
    void Add(int list, uintptr_t conn) {
        data[list] = 0x1000 + list * 0x100;
        entries[list][count[list]++] = conn;
    }
    void Conn(uintptr_t addr, const char* name) { conns[nConns++] = FakeConn{addr, name}; }
    Members CountAs(const char* own) {
        return Count(
            [&](uint32_t i, uintptr_t* d, uint32_t* c) {
                if (!listReadable[i]) return false;
                *d = data[i];
                *c = count[i];
                return true;
            },
            [&](uintptr_t d, uint32_t k, uintptr_t* conn) {
                if (!entryReadable) return false;
                for (uint32_t i = 0; i < kLists; ++i)
                    if (data[i] == d) {
                        *conn = entries[i][k];
                        return true;
                    }
                return false;
            },
            [&](uintptr_t conn, char* out) {
                for (int i = 0; i < nConns; ++i)
                    if (conns[i].addr == conn) {
                        if (!conns[i].name) return false;
                        memset(out, 0, kNameMax);
                        strncpy(out, conns[i].name, kNameMax);  // a 64-character name has no end inside the 64
                        return true;
                    }
                return false;
            },
            own);
    }
};

void MemberTests() {
    char me[kNameMax] = "Player One";
    {
        FakeSession s;
        s.Conn(0xA000, "Friend");
        s.Add(3, 0xA000);
        const Members m = s.CountAs(me);
        Check(m.read && m.entries == 1 && m.distinct == 1 && m.self == 0 && Judge(m) == Verdict::kTwoPlayers,
              "one connection, the opponent's: two players");
    }
    {
        FakeSession s;  // MK9 may keep a connection for the local player: not counted
        s.Conn(0xA000, "Player One");
        s.Conn(0xB000, "Friend");
        s.Add(3, 0xA000);
        s.Add(3, 0xB000);
        const Members m = s.CountAs(me);
        Check(m.self == 1 && Judge(m) == Verdict::kTwoPlayers, "a connection with this player's name is not another player");
    }
    {
        FakeSession s;
        s.Conn(0xA000, "Friend");
        s.Conn(0xB000, "Watcher");
        s.Add(3, 0xA000);
        s.Add(3, 0xB000);
        Check(Judge(s.CountAs(me)) == Verdict::kOthers, "two other connections: more players than the two");
    }
    {
        FakeSession s;  // a player still connecting
        s.Conn(0xA000, "Friend");
        s.Conn(0xC000, "Newcomer");
        s.Add(3, 0xA000);
        s.Add(2, 0xC000);
        Check(Judge(s.CountAs(me)) == Verdict::kOthers, "a player in the incoming array counts");
    }
    {
        FakeSession s;  // one connection in two arrays while it moves
        s.Conn(0xA000, "Friend");
        s.Add(2, 0xA000);
        s.Add(3, 0xA000);
        const Members m = s.CountAs(me);
        Check(m.entries == 2 && m.distinct == 1 && Judge(m) == Verdict::kTwoPlayers, "a connection in two arrays counts once");
    }
    {
        FakeSession s;
        Check(Judge(s.CountAs(me)) == Verdict::kUnknown, "no connection at all: not known");
        s.Conn(0xA000, "Player One");
        s.Add(3, 0xA000);
        Check(Judge(s.CountAs(me)) == Verdict::kUnknown, "only this player: not known");
    }
    {
        FakeSession s;
        s.Conn(0xA000, nullptr);
        s.Add(3, 0xA000);
        Check(Judge(s.CountAs(me)) == Verdict::kUnknown, "a name that cannot be read: not known");
        FakeSession t;
        t.Conn(0xA000, "");
        t.Add(3, 0xA000);
        Check(Judge(t.CountAs(me)) == Verdict::kUnknown, "an empty name: not known");
        FakeSession u;
        u.Conn(0xA000, "Friend");
        u.Add(3, 0xA000);
        u.Add(3, 0);
        Check(Judge(u.CountAs(me)) == Verdict::kUnknown, "a hole in the array: not known");
    }
    {
        FakeSession s;
        s.Conn(0xA000, "Friend");
        s.Add(3, 0xA000);
        s.listReadable[1] = false;
        Check(!s.CountAs(me).read && Judge(s.CountAs(me)) == Verdict::kUnknown, "an array that cannot be read: not known");
        s.listReadable[1] = true;
        s.entryReadable = false;
        Check(Judge(s.CountAs(me)) == Verdict::kUnknown, "an entry that cannot be read: not known");
    }
    {
        FakeSession s;
        s.Conn(0xA000, "Friend");
        s.Add(3, 0xA000);
        s.count[3] = kMaxPerList + 1;  // garbage count
        Check(Judge(s.CountAs(me)) == Verdict::kUnknown, "a count no room has: not known");
        s.count[3] = 1;
        s.data[3] = 0;
        Check(Judge(s.CountAs(me)) == Verdict::kUnknown, "a count without data: not known");
    }
    {
        FakeSession s;
        s.Conn(0xA000, "Friend");
        s.Add(3, 0xA000);
        Check(Judge(s.CountAs("")) == Verdict::kUnknown, "this player's name unknown: not known");
        Check(Judge(s.CountAs(nullptr)) == Verdict::kUnknown, "no name of this player: not known");
    }
    {
        // 64-character names: compared over 64 characters, no end needed
        char longMe[kNameMax];
        memset(longMe, 'x', sizeof longMe);
        char other[kNameMax + 1];
        memset(other, 'x', kNameMax);
        other[kNameMax] = 0;
        FakeSession s;
        s.Conn(0xA000, other);  // the same 64 characters: this player
        s.Conn(0xB000, "Friend");
        s.Add(3, 0xA000);
        s.Add(3, 0xB000);
        Check(Judge(s.CountAs(longMe)) == Verdict::kTwoPlayers, "64-character names compare without an end");
    }
}

void StickyTests() {
    Sticky s;
    Check(s.Feed(Verdict::kTwoPlayers, 1000) == Verdict::kUnknown, "two players is not used at once");
    Check(s.Feed(Verdict::kTwoPlayers, 1000 + kTwoHoldMs - 1) == Verdict::kUnknown, "nor before the hold");
    Check(s.Feed(Verdict::kTwoPlayers, 1000 + kTwoHoldMs) == Verdict::kTwoPlayers, "after the hold it is");
    Check(s.Feed(Verdict::kOthers, 2000) == Verdict::kOthers, "a third player counts at once");
    Check(s.Feed(Verdict::kTwoPlayers, 2010) == Verdict::kOthers, "and the two again only after the hold");
    Check(s.Feed(Verdict::kTwoPlayers, 2010 + kTwoHoldMs) == Verdict::kTwoPlayers, "after it");
    Check(s.Feed(Verdict::kUnknown, 3000) == Verdict::kUnknown, "not known counts at once");
    // A flicker: a read that misses the third player for one tick does not make two players.
    s.Reset();
    uint64_t t = 0;
    for (int i = 0; i < 200; ++i, t += 16) {
        const Verdict v = (i % 10 == 3) ? Verdict::kTwoPlayers : Verdict::kOthers;
        Check(s.Feed(v, t) == Verdict::kOthers, "one missed read never shows two players");
    }
}

// ---- MK9's connection timers ----
// A connection as MK9 sees it: when it last got a packet (0: none since the start).
struct Sock {
    const void* id;
    uint64_t lastPacket;
    uint32_t Real(uint64_t now) const { return (uint32_t)(now - lastPacket); }
};

void IdleTests() {
    // 2026-11-14, times in ms from the room's start. Socket A: the opponent's connection that went silent 65 s into the
    // second rollback fight; B: the opponent's other one, alive throughout.
    IdleBook book;
    const void* A = reinterpret_cast<const void*>(0x11AA0001);
    const void* B = reinterpret_cast<const void*>(0x11AA0002);
    Sock a{A, 0}, b{B, 0};
    const uint64_t engage1 = 38000, end1 = 130000, engage2 = 174000, silentA = 239000, end2 = 336000, crashAt = 357000;
    uint64_t lastHeard = 0;
    uint32_t maxShownAfter = 0, maxB = 0;
    bool sawStarved = false, sawLive = false;
    for (uint64_t t = 1; t <= crashAt + 120000; t += 16) {
        if (t < silentA || t > crashAt + 60000) a.lastPacket = t - (t % 33);  // packets every ~33 ms (and again at the end)
        b.lastPacket = t - (t % 33);
        const uint64_t engagedAt = t >= engage2 ? engage2 : t >= engage1 ? engage1 : 0;
        const bool inFight = (t >= engage1 && t < end1) || (t >= engage2 && t < end2);
        const uint64_t graceUntil = t >= end2 ? end2 + 15000 : t >= end1 ? end1 + 15000 : 0;
        const bool hidden = inFight || t < graceUntil;
        // The opponent's hellos every 500 ms all the time (they keep coming in the room), until t = crashAt + 30 s.
        if (t < crashAt + 30000 && t % 500 < 16) lastHeard = t;
        const uint32_t sa = book.Show(A, t, a.Real(t), hidden, engagedAt, lastHeard);
        const bool aStarved = book.lastWasStarved;
        const uint32_t sb = book.Show(B, t, b.Real(t), hidden, engagedAt, lastHeard);
        if (hidden) Check(sa == 0 && sb == 0, "nothing is shown during a fight and its grace");
        if (sb > maxB) maxB = sb;
        if (!hidden && t > silentA + kSilentMs && t < crashAt + 30000) {
            if (sa > maxShownAfter) maxShownAfter = sa;
            sawStarved |= aStarved;
        }
        if (!hidden && t < silentA) sawLive |= !aStarved && sa == a.Real(t);
        // After the hellos stop the opponent's game is gone for all the mod knows: MK9 sees the time since the last.
        if (t > crashAt + 31000 && t < crashAt + 60000) {
            const uint32_t want = (uint32_t)(t - lastHeard) < a.Real(t) ? (uint32_t)(t - lastHeard) : a.Real(t);
            Check(sa == want, "with the opponent silent MK9 sees the time since it was last heard");
        }
        if (t > crashAt + 61000) Check(sa < kSilentMs, "a connection that hears packets again shows MK9's own value");
    }
    Check(sawLive, "before the silence the connection shows MK9's own value");
    Check(sawStarved, "the silent connection is taken as one the fight starved");
    Check(maxShownAfter <= 520, "while the opponent's hellos come, MK9 is never shown more than one hello's gap");
    Check(maxB < 40, "the live connection shows its own small value");
    Check(book.starvedMarks == 1, "one connection was marked");

    // A connection first seen after this game engaged (a player who came later) is never hidden after the fight.
    {
        IdleBook bk;
        const void* C = reinterpret_cast<const void*>(0x3);
        const uint64_t eng = 10000;
        bk.Show(A, 5000, 10, false, 0, 5000);  // A seen before
        uint32_t shown = 0;
        uint64_t last = 0;
        for (uint64_t t = 20000; t < 40000; t += 16, last = t - 16) shown = bk.Show(C, t, (uint32_t)(t - 19000), t < 30000, eng, t);
        Check(shown == (uint32_t)(last - 19000), "a later player's silence is MK9's own");
        Check(bk.starvedMarks == 0, "and is not marked");
    }
    // A silence that began well before the engagement is MK9's own (not the fight's doing).
    {
        IdleBook bk;
        bk.Show(A, 1000, 10, false, 0, 1000);
        uint32_t shown = 0;
        uint64_t last = 0;
        for (uint64_t t = 20000; t < 60000; t += 16, last = t - 16) shown = bk.Show(A, t, (uint32_t)(t - 5000), t < 40000, 20000, t);
        Check(shown == (uint32_t)(last - 5000), "a silence from before the fight is shown as it is");
        Check(bk.starvedMarks == 0, "and is not marked");
    }
    // Margin: a silence that began half a second before the engagement is the fight's.
    {
        IdleBook bk;
        bk.Show(A, 1000, 10, false, 0, 1000);
        uint32_t shown = 0;
        for (uint64_t t = 20000; t < 60000; t += 16) shown = bk.Show(A, t, (uint32_t)(t - 19500), t < 40000, 20000, t - (t % 500));
        Check(bk.starvedMarks == 1 && shown < 500, "a silence from just before the engagement is the fight's");
    }
    // Nothing heard of the opponent in this room: MK9's own value.
    {
        IdleBook bk;
        bk.Show(A, 1000, 10, false, 0, 0);
        uint32_t shown = 0;
        uint64_t last = 0;
        for (uint64_t t = 20000; t < 60000; t += 16, last = t - 16) {
            const uint32_t real = t > 25000 ? (uint32_t)(t - 25000) : 0;
            shown = bk.Show(A, t, real, t < 40000, 20000, 0);
        }
        Check(bk.starvedMarks == 1 && shown == (uint32_t)(last - 25000), "no sign of the opponent: MK9's own value");
    }
    // More connections than the book holds: the most recently used ones stay.
    {
        IdleBook bk;
        for (uint32_t i = 1; i <= 40; ++i) bk.Show(reinterpret_cast<const void*>((uintptr_t)i), i, 10, false, 0, i);
        uint32_t found = 0;
        for (const auto& x : bk.e)
            if ((uintptr_t)x.self > 40 - kBookSize) ++found;
        Check(found == kBookSize, "the book keeps the connections used last");
    }

    // 4000 random rooms: connections, fights, hellos that stop; what MK9 is shown always obeys the rules.
    int violations = 0;
    for (int room = 0; room < 4000; ++room) {
        IdleBook bk;
        const int n = 1 + (int)(Rand() % 6);
        Sock s[6];
        uint64_t born[6], stop[6];
        for (int i = 0; i < n; ++i) {
            s[i] = Sock{reinterpret_cast<const void*>((uintptr_t)(0x100 + i)), 0};
            born[i] = Rand() % 60000;
            stop[i] = Rand() % 3 ? (uint64_t)-1 : born[i] + Rand() % 200000;
        }
        uint64_t engaged = 0, fightEnd = 0, grace = 0, heard = 0, heardStop = Rand() % 2 ? (uint64_t)-1 : 50000 + Rand() % 200000;
        bool inFight = false;
        for (uint64_t t = 1; t < 260000; t += 16) {
            if (!inFight && Rand() % 2000 == 0) {
                inFight = true;
                engaged = t;
                fightEnd = t + 20000 + Rand() % 120000;
            }
            if (inFight && t >= fightEnd) {
                inFight = false;
                grace = t + 15000;
            }
            if (t < heardStop && t % 500 < 16) heard = t;
            const bool hidden = inFight || t < grace;
            for (int i = 0; i < n; ++i) {
                if (t < born[i]) continue;
                if (t < stop[i]) s[i].lastPacket = t - (t % 33);
                if (!s[i].lastPacket) s[i].lastPacket = t;
                const uint32_t real = s[i].Real(t);
                const uint32_t shown = bk.Show(s[i].id, t, real, hidden, engaged, heard);
                bool ok = shown <= real;
                if (hidden) ok = ok && shown == 0;
                else if (!bk.lastWasStarved) ok = ok && shown == real;
                else ok = ok && (!heard || shown <= t - heard || shown == real);
                if (!ok) ++violations;
            }
        }
    }
    Check(violations == 0, "4000 random rooms: MK9 is shown 0 in a fight, at most its own value, and for a starved "
                           "connection at most the time since the opponent was heard");
}
}  // namespace

int main() {
    RoomTests();
    MemberTests();
    StickyTests();
    IdleTests();
    printf("%d checks, %d failed\n", gChecks, gFailed);
    if (!gFailed) printf("ALL CHECKS PASSED\n");
    return gFailed ? 1 : 0;
}
