// Host-side test of src\net\StagePick.h: the stage pick packet, the choice between two picks, and what two games that
// exchange picks over a link that loses, delays, repeats and reorders datagrams end up with.
// Build and run: see run.ps1 beside this file.
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "HelloProto.h"
#include "StagePick.h"

using namespace StagePick;

namespace {
int gFailed = 0, gChecks = 0;
void Expect(bool ok, const char* what) {
    ++gChecks;
    if (!ok) ++gFailed;
    printf("%s  %s\n", ok ? "PASS" : "FAIL", what);
}

uint32_t gRng = 0x2F6E2B1u;
uint32_t Rnd() {
    gRng ^= gRng << 13;
    gRng ^= gRng >> 17;
    gRng ^= gRng << 5;
    return gRng;
}
uint32_t Rnd(uint32_t n) { return Rnd() % n; }
uint32_t Nonce() {
    uint32_t n;
    do n = Rnd() * 2654435761u ^ Rnd(); while (!n);
    return n;
}

// The stage table as the game listed it ('stages' command, 34 entries; 1-28 may be picked).
const char* const kNames[] = {
    "BGND_ChallengeTower", "BGND_Courtyard", "BGND_CourtyardN", "BGND_DeadPool", "BGND_Desert", "BGND_FleshPits", "BGND_GorosLair",
    "BGND_Graveyard", "BGND_Hell", "BGND_KahnsColiseum", "BGND_LivingForest", "BGND_Pit", "BGND_PitBottom", "BGND_PitDay",
    "BGND_RooftopDawn", "BGND_RooftopDay", "BGND_RooftopDusk", "BGND_SKThrone", "BGND_SoulChamber", "BGND_STGardens",
    "BGND_STGardensN", "BGND_Street", "BGND_STThrone", "BGND_Subway", "BGND_Temple", "BGND_Tower", "BGND_Wastelands", "BGND_Armory",
    "BGND_BellTower", "BGND_BioViewer", "BGND_KOTH", "BGND_KOTH_S", "BGND_Krypt", "BGND_Nekropolis"};
constexpr int kNameCount = sizeof kNames / sizeof kNames[0];
Table GameTable() {
    Table t;
    t.count = (uint8_t)kNameCount;
    for (int i = 0; i < kNameCount; ++i) {
        t.nameHash[i] = NameHash(kNames[i]);
        t.selectable[i] = i >= 1 && i <= 28;
    }
    return t;
}
uint8_t RandomPick(const Table& t) {
    if (Rnd(100) < 15) return kRandom;
    for (;;) {
        const uint8_t c = (uint8_t)Rnd(t.count);
        if (t.selectable[c]) return c;
    }
}

bool Same(const Side& a, const Side& b) { return a.nonce == b.nonce && a.choice == b.choice && a.cursor == b.cursor && a.locked == b.locked; }
bool Same(const Packet& a, const Packet& b) {
    return a.kind == b.kind && a.open == b.open && a.apply == b.apply && a.context == b.context && a.serial == b.serial && a.moves == b.moves &&
           Same(a.me, b.me) && Same(a.heard, b.heard);
}
// One game's result and the other's name the same ticket.
bool Mirrored(const Result& a, const Result& b) {
    if (a.choice != b.choice) return false;
    switch (a.winner) {
    case Winner::kBoth: return b.winner == Winner::kBoth;
    case Winner::kMine: return b.winner == Winner::kTheirs;
    case Winner::kTheirs: return b.winner == Winner::kMine;
    case Winner::kNone: return b.winner == Winner::kNone;
    }
    return false;
}

// ---- two games and a link ------------------------------------------------------------------------------------------
struct Wire {
    uint8_t b[kSize];
    int due;
    bool toA;
};
struct Trial {
    int lossPct = 0, maxDelay = 0, dupPct = 0;
    bool stale = false;     // datagrams of an earlier exchange of the same two games arrive during this one
    int lockA = 5, lockB = 9;  // the tick each player locks at (-1: never)
    int ticks = 200;
    bool otherTable = false;  // B's game lists the stages differently
};
struct Seen {
    bool confirmedWithoutPeerReady = false;  // a game was Confirmed while the other one could not compute the same result
    bool readyDisagreed = false;             // both Ready, different tickets
    bool everReadyA = false, everReadyB = false;
    bool confirmedA = false, confirmedB = false;
    bool unlocked = false;  // a lock was seen and later gone
    bool cursorWentBack = false;  // a game showed the other player's cursor at an older place than one it had already shown
    Exchange a, b;
};

void Send(std::vector<Wire>& link, const Exchange& from, bool toA, int tick, const Trial& t) {
    if ((int)Rnd(100) < t.lossPct) return;
    Wire w{};
    Build(from.Out(), w.b, sizeof w.b);
    w.toA = toA;
    w.due = tick + 1 + (int)Rnd((uint32_t)t.maxDelay + 1);
    link.push_back(w);
    if ((int)Rnd(100) < t.dupPct) {
        w.due = tick + 1 + (int)Rnd((uint32_t)t.maxDelay + 1);
        link.push_back(w);
    }
}

Seen Run(const Trial& t) {
    Seen s;
    const Table table = GameTable();
    Table tableB = table;
    if (t.otherTable) tableB.selectable[30] = true;
    std::vector<Wire> link;
    if (t.stale) {
        // The exchange before this one, run to its end; some of its datagrams are still on their way.
        Exchange a0, b0;
        a0.Open(kKindStage, table.Context(), Nonce(), 40);
        b0.Open(kKindStage, table.Context(), Nonce(), 7);
        a0.Lock(RandomPick(table));
        b0.Lock(RandomPick(table));
        for (int i = 0; i < 4; ++i) {
            b0.OnPacket(a0.Out());
            a0.OnPacket(b0.Out());
        }
        Trial clean;
        clean.maxDelay = t.ticks / 2;
        for (int i = 0; i < 6; ++i) {
            Send(link, a0, false, -1, clean);
            Send(link, b0, true, -1, clean);
        }
    }
    s.a.Open(kKindStage, table.Context(), Nonce(), 41);
    s.b.Open(kKindStage, tableB.Context(), Nonce(), 8);
    bool wasLockedA = false, wasLockedB = false;
    // The newest state of the sender's cursor that has reached each game (by the sender's move count; -1: none yet).
    int newestToA = -1, newestToB = -1;
    uint8_t cursorToA = kNoChoice, cursorToB = kNoChoice;
    for (int tick = 0; tick < t.ticks; ++tick) {
        if (tick < t.lockA || t.lockA < 0) {
            if (Rnd(3) == 0) s.a.MoveCursor(RandomPick(table));
        } else if (tick == t.lockA) {
            s.a.Lock(RandomPick(table));
        }
        if (tick < t.lockB || t.lockB < 0) {
            if (Rnd(3) == 0) s.b.MoveCursor(RandomPick(tableB));
        } else if (tick == t.lockB) {
            s.b.Lock(RandomPick(tableB));
        }
        Send(link, s.a, false, tick, t);
        Send(link, s.b, true, tick, t);
        // What is due arrives, in any order.
        std::vector<Wire> due;
        for (size_t i = 0; i < link.size();) {
            if (link[i].due <= tick) {
                due.push_back(link[i]);
                link[i] = link.back();
                link.pop_back();
            } else {
                ++i;
            }
        }
        for (size_t i = due.size(); i > 1; --i) std::swap(due[i - 1], due[Rnd((uint32_t)i)]);
        for (const Wire& w : due) {
            Packet p;
            if (Parse(w.b, sizeof w.b, &p) != Parsed::kOk) continue;
            Exchange& to = w.toA ? s.a : s.b;
            const Exchange& from = w.toA ? s.b : s.a;
            to.OnPacket(p);
            // The other player's cursor as this game shows it: the newest state that has arrived, never an older one.
            if (p.open && p.me.nonce == from.mine.nonce) {
                int& newest = w.toA ? newestToA : newestToB;
                uint8_t& cursor = w.toA ? cursorToA : cursorToB;
                if ((int)p.moves >= newest) newest = p.moves, cursor = p.me.cursor;
                if (to.theirs.nonce == from.mine.nonce && to.theirs.cursor != cursor) s.cursorWentBack = true;
            }
        }
        // What must hold after every tick.
        const Result ra = Decide(s.a.mine, s.a.theirs), rb = Decide(s.b.mine, s.b.theirs);
        if (s.a.Confirmed() && !(s.b.Ready() && Mirrored(ra, rb))) s.confirmedWithoutPeerReady = true;
        if (s.b.Confirmed() && !(s.a.Ready() && Mirrored(rb, ra))) s.confirmedWithoutPeerReady = true;
        if (!t.stale && s.a.Ready() && s.b.Ready() && !Mirrored(ra, rb)) s.readyDisagreed = true;
        s.everReadyA |= s.a.Ready();
        s.everReadyB |= s.b.Ready();
        if ((wasLockedA && !s.a.mine.locked) || (wasLockedB && !s.b.mine.locked)) s.unlocked = true;
        if ((s.confirmedA && !s.a.Confirmed()) || (s.confirmedB && !s.b.Confirmed())) s.unlocked = true;
        wasLockedA = s.a.mine.locked;
        wasLockedB = s.b.mine.locked;
        s.confirmedA = s.a.Confirmed();
        s.confirmedB = s.b.Confirmed();
    }
    return s;
}

}  // namespace

int main() {
    const Table table = GameTable();

    printf("-- 1. the packet\n");
    {
        Packet p;
        p.kind = kKindStage;
        p.open = true;
        p.context = 0xC0FFEE01u;
        p.serial = 0x01020304u;
        p.moves = 0xBEEF;
        p.apply = true;
        p.me.nonce = 0x11223344u;
        p.me.choice = 7;
        p.me.cursor = 7;
        p.me.locked = true;
        p.heard.nonce = 0x55667788u;
        p.heard.choice = kRandom;
        p.heard.locked = true;
        uint8_t b[64];
        memset(b, 0xEE, sizeof b);
        const size_t n = Build(p, b, sizeof b);
        Expect(n == 32 && kSize == 32 && b[32] == 0xEE, "a packet is 32 bytes and nothing is written behind them");
        Expect(Build(p, b, 31) == 0, "a buffer of 31 bytes: nothing built");
        Packet q;
        Expect(Parse(b, n, &q) == Parsed::kOk && Same(q.me, p.me) && q.heard.nonce == p.heard.nonce && q.heard.choice == kRandom &&
                  q.heard.locked && q.context == p.context && q.serial == p.serial && q.moves == p.moves && q.open && q.apply && q.kind == kKindStage,
              "what was built is what is read");
        Expect(IsPacket(b, n) && !IsPacket(b, 3) && !IsPacket(nullptr, 32), "IsPacket: the magic alone decides");

        // A release that does not know the packet: OnlineMode's read loop asks HelloProto first and drops a kNone.
        Expect(HelloProto::Parse(b, n).kind == HelloProto::Kind::kNone, "the hello parser of every release takes it for 'not a hello' (kNone: dropped)");
        HelloProto::Fields f{};
        uint8_t hello[64];
        const size_t hn = HelloProto::Build(f, hello, sizeof hello);
        Expect(hn == HelloProto::kSize && Parse(hello, hn, &q) == Parsed::kNone && !IsPacket(hello, hn), "a hello is not a pick packet");

        bool shortOk = true;
        for (size_t len = 0; len < n; ++len) {
            const Parsed r = Parse(b, len, &q);
            shortOk &= len < 8 ? r == Parsed::kNone : r == Parsed::kMalformed;
            shortOk &= !q.open && !q.me.locked && !q.heard.locked;
        }
        Expect(shortOk, "every shorter length: nothing below 8 bytes, 'malformed' from 8 to 31, no field taken");

        uint8_t later[48];
        memset(later, 0x5A, sizeof later);
        memcpy(later, b, n);
        later[6] = 48;
        Expect(Parse(later, sizeof later, &q) == Parsed::kOk && Same(q.me, p.me), "a later build's longer packet is read by its first 32 bytes");
        uint8_t other[32];
        memcpy(other, b, n);
        other[4] = 2;
        Expect(Parse(other, n, &q) == Parsed::kOtherVersion && !q.open, "another version: reported as such, nothing taken");
        memcpy(other, b, n);
        other[5] = 3;
        Expect(Parse(other, n, &q) == Parsed::kMalformed, "a kind no build sends: malformed");
        memcpy(other, b, n);
        other[5] = kKindAfterMatch;
        Expect(Parse(other, n, &q) == Parsed::kOk && q.kind == kKindAfterMatch, "kind 2 (the choice after a match, reserved) is carried by the same packet");
        Exchange x;
        x.Open(kKindStage, p.context, 99, 1);
        Expect(x.OnPacket(q) == Exchange::Heard::kIgnored && !x.theirs.nonce, "a stage exchange does not take a packet of kind 2");

        memcpy(other, b, n);
        other[7] = 0;  // the sender's panel is closed
        Expect(Parse(other, n, &q) == Parsed::kOk && !q.open && !q.me.nonce && !q.me.locked && !q.heard.locked, "a closed panel has no pick");
        memcpy(other, b, n);
        other[16] = 0x80;  // a choice that is none
        Expect(Parse(other, n, &q) == Parsed::kOk && !q.me.locked && q.me.choice == kNoChoice, "a lock on a choice that is none is no lock");
        memcpy(other, b, n);
        memset(other + 12, 0, 4);  // nonce 0
        Expect(Parse(other, n, &q) == Parsed::kOk && !q.me.locked, "a lock without a nonce is no lock");
    }

    printf("-- 2. any bytes\n");
    {
        bool sane = true, canonical = true;
        int ok = 0;
        for (int i = 0; i < 300000; ++i) {
            uint8_t b[48];
            for (uint8_t& c : b) c = (uint8_t)Rnd();
            const size_t len = Rnd(sizeof b + 1);
            if (Rnd(4)) memcpy(b, "KR9M", 4);
            if (Rnd(4)) b[4] = 1;
            if (Rnd(2)) b[5] = (uint8_t)(1 + Rnd(2));
            if (Rnd(2)) b[6] = (uint8_t)(32 + Rnd(16));
            if (Rnd(2)) b[16] = (uint8_t)Rnd(40);
            if (Rnd(2)) b[18] = (uint8_t)Rnd(2);
            Packet p;
            const Parsed r = Parse(b, len, &p);
            for (const Side* s : {&p.me, &p.heard}) {
                sane &= s->choice == kNoChoice || IsChoice(s->choice);
                sane &= !s->locked || (IsChoice(s->choice) && s->nonce != 0);
                sane &= s->locked || s->choice == kNoChoice;
            }
            if (r != Parsed::kOk) {
                sane &= !p.open && !p.me.nonce && !p.heard.nonce;
                continue;
            }
            ++ok;
            sane &= p.open || (!p.me.nonce && !p.heard.nonce);
            uint8_t again[kSize];
            Packet q;
            canonical &= Build(p, again, sizeof again) == kSize && Parse(again, sizeof again, &q) == Parsed::kOk;
            q.heard.cursor = p.heard.cursor;  // not on the wire
            canonical &= Same(p, q);
        }
        Expect(sane, "300000 random byte strings: every pick read is none, 'random' or an entry below 64; a lock always has both");
        Expect(canonical && ok > 20000, "every packet read can be built again and reads the same");
    }

    printf("-- 3. the choice between two picks\n");
    {
        Side a, b;
        a.nonce = 1000;
        a.choice = 5;
        a.locked = true;
        b.nonce = 2000;
        b.choice = 5;
        b.locked = true;
        Result r = Decide(a, b);
        Expect(r.winner == Winner::kBoth && r.choice == 5, "the same pick twice is that pick");
        b.choice = 9;
        b.locked = false;
        Expect(Decide(a, b).winner == Winner::kNone && Decide(b, a).winner == Winner::kNone, "one pick not locked: no result");
        b.locked = true;
        b.nonce = a.nonce;
        Expect(Decide(a, b).winner == Winner::kNone, "equal random numbers and different picks: no result (the caller falls back)");
        b.nonce = 0;
        Expect(Decide(a, b).winner == Winner::kNone, "a side without a random number: no result");

        bool mirrored = true;
        int mine = 0, first = 0, total = 0, randomWon = 0, randomIn = 0;
        for (int i = 0; i < 400000; ++i) {
            a.nonce = Nonce();
            b.nonce = Nonce();
            a.choice = RandomPick(table);
            b.choice = RandomPick(table);
            const Result ab = Decide(a, b), ba = Decide(b, a);
            mirrored &= Mirrored(ab, ba) && Mirrored(ba, ab) && ab.winner != Winner::kNone;
            mirrored &= Coin(a.nonce, b.nonce) == Coin(b.nonce, a.nonce) && SharedSeed(a, b) == SharedSeed(b, a);
            if (ab.winner == Winner::kBoth) continue;
            ++total;
            mine += ab.winner == Winner::kMine;
            first += (ab.winner == Winner::kMine) == (a.nonce < b.nonce);
            if (a.choice == kRandom || b.choice == kRandom) {
                ++randomIn;
                randomWon += ab.choice == kRandom;
            }
        }
        printf("  %d pairs of different picks: this side wins %.2f%%, the smaller number wins %.2f%%, 'random' wins %.2f%% of the pairs it is in\n",
               total, 100.0 * mine / total, 100.0 * first / total, 100.0 * randomWon / randomIn);
        Expect(mirrored, "400000 pairs: both games name the same ticket, whichever computes it");
        Expect(mine > total * 0.49 && mine < total * 0.51 && first > total * 0.49 && first < total * 0.51, "the coin is fair (49-51%), also between the smaller and the larger number");
        Expect(randomWon > randomIn * 0.48 && randomWon < randomIn * 0.52, "'random' is one ticket like any other");

        // Numbers that are close together (a poor random source) still give a fair coin.
        int firstWins = 0;
        const int n = 100000;
        for (int i = 0; i < n; ++i) {
            a.nonce = 1 + (uint32_t)i;
            b.nonce = 2 + (uint32_t)i + Rnd(3);
            a.choice = 3;
            b.choice = 4;
            firstWins += Decide(a, b).winner == Winner::kMine;
        }
        Expect(firstWins > n * 0.48 && firstWins < n * 0.52, "neighbouring numbers: still fair (48-52%)");
    }

    printf("-- 4. two games over a link\n");
    {
        // The plain case, step by step.
        Exchange a, b;
        a.Open(kKindStage, table.Context(), 0xA0000001u, 3);
        b.Open(kKindStage, table.Context(), 0xB0000002u, 900);
        Expect(!a.Ready() && Judge(a, table, false).why == Why::kNotReady, "a panel just opened: no result");
        Expect(!a.MoveCursor(0x90) && a.MoveCursor(4) && a.Out().me.cursor == 4, "the cursor travels; a cursor that is no choice is refused");
        Expect(a.Lock(11) && !a.Lock(12) && !a.MoveCursor(3) && a.mine.choice == 11, "a lock is final");
        b.OnPacket(a.Out());
        Expect(b.theirs.locked && b.theirs.choice == 11 && !b.Ready(), "the other game sees the lock");
        b.Lock(20);
        Expect(b.Ready() && !b.Confirmed(), "it locks too: it has the result, but not yet the knowledge that the first game has it");
        a.OnPacket(b.Out());
        Expect(a.Ready() && a.Confirmed(), "the first game hears the other lock and its own lock repeated: confirmed");
        b.OnPacket(a.Out());
        Expect(b.Confirmed(), "one more packet back: both confirmed");
        a.apply = true;
        b.OnPacket(a.Out());
        a.OnPacket(b.Out());
        Expect(b.theirsApply && !a.theirsApply && a.Confirmed() && b.Confirmed(),
               "each game learns whether the other one will write the stage into the game (the banner's honesty)");
        const Verdict va = Judge(a, table, true), vb = Judge(b, table, true);
        Expect(va.outcome == Outcome::kStage && vb.outcome == Outcome::kStage && va.entry == vb.entry && (va.entry == 11 || va.entry == 20) &&
                  ((va.winner == Winner::kMine) == (vb.winner == Winner::kTheirs)),
              "both games judge the same stage, one as its own pick and one as the opponent's");
        uint8_t msg[16];
        for (int i = 0; i < 16; ++i) msg[i] = (uint8_t)(0x10 + i);
        uint8_t before[16];
        memcpy(before, msg, 16);
        Expect(ApplyToSettings(va, msg) && SettingsIndex(msg) == va.entry && !memcmp(before, msg, 8) && !memcmp(before + 12, msg + 12, 4),
              "the settings message gets the entry as its third value; the other twelve bytes stay");
        Expect(CheckReceived(vb, SettingsIndex(msg)) == Check::kSame && CheckReceived(vb, va.entry + 1u) == Check::kDiffers,
              "the receiving game finds the index it computed; another index is reported");
        Verdict none;
        memcpy(msg, before, 16);
        Expect(!ApplyToSettings(none, msg) && !memcmp(before, msg, 16) && CheckReceived(none, 5) == Check::kNothingExpected,
              "no agreed stage: the message is not touched and nothing is expected of it");

        // A reordered older packet (the cursor before the lock) changes nothing.
        Exchange c;
        c.Open(kKindStage, table.Context(), 0xC0000003u, 1);
        Exchange d;
        d.Open(kKindStage, table.Context(), 0xD0000004u, 1);
        d.MoveCursor(6);
        const Packet early = d.Out();
        d.Lock(8);
        c.OnPacket(d.Out());
        c.OnPacket(early);
        Expect(c.theirs.locked && c.theirs.choice == 8, "an older packet that arrives late does not undo a lock");
        Packet forged = d.Out();
        forged.me.choice = 9;
        c.OnPacket(forged);
        Expect(c.theirs.choice == 8, "a second, different lock under the same number is not taken");

        // The cursor on the other screen while a player still chooses ("see in realtime the other selecting"): it
        // follows the packets in the order they were sent, whatever order they arrive in.
        Exchange mover, watcher;
        mover.Open(kKindStage, table.Context(), 0xE0000005u, 1);
        watcher.Open(kKindStage, table.Context(), 0xF0000006u, 1);
        mover.MoveCursor(3);
        const Packet first = mover.Out();
        mover.MoveCursor(5);
        const Packet second = mover.Out();
        mover.MoveCursor(5);
        Expect(mover.Out().moves == second.moves && (uint16_t)(first.moves + 1) == second.moves,
               "the move count rises with each move of the cursor, not with each packet");
        mover.MoveCursor(7);
        const Packet third = mover.Out();
        watcher.OnPacket(first);
        Expect(watcher.theirs.cursor == 3 && !watcher.theirs.locked && !watcher.Ready(), "the other screen has the cursor before any lock");
        watcher.OnPacket(third);
        watcher.OnPacket(second);
        Expect(watcher.theirs.cursor == 7, "a cursor packet that arrives after a later one does not move the shown cursor back");
        watcher.OnPacket(third);
        Expect(watcher.theirs.cursor == 7, "a repeated packet changes nothing");
        Exchange lateWatcher;
        lateWatcher.Open(kKindStage, table.Context(), 0xF0000007u, 1);
        lateWatcher.OnPacket(third);
        Expect(lateWatcher.theirs.cursor == 7, "packets that never arrive are made good by the next one: each carries the whole state");
        mover.Lock(9);
        watcher.OnPacket(mover.Out());
        watcher.OnPacket(third);
        Expect(watcher.theirs.locked && watcher.theirs.choice == 9 && watcher.theirs.cursor == 9,
               "the lock ends it: the cursor stands on the pick, and a late cursor packet is ignored");
        // The 16-bit count wraps after 65536 moves; the order still holds across the wrap.
        Exchange wrapMover, wrapWatcher;
        wrapMover.Open(kKindStage, table.Context(), 0xE0000008u, 1);
        wrapWatcher.Open(kKindStage, table.Context(), 0xF0000009u, 1);
        wrapMover.moves = 0xFFFE;
        wrapMover.MoveCursor(4);
        const Packet beforeWrap = wrapMover.Out();
        wrapMover.MoveCursor(3);
        const Packet afterWrap = wrapMover.Out();
        wrapWatcher.OnPacket(beforeWrap);
        wrapWatcher.OnPacket(afterWrap);
        const bool forward = wrapWatcher.theirs.cursor == 3;
        wrapWatcher.OnPacket(beforeWrap);
        Expect(beforeWrap.moves == 0xFFFF && afterWrap.moves == 0 && forward && wrapWatcher.theirs.cursor == 3, "the move count's wrap-around keeps the order");

        // Leftovers of the exchange before (a rematch): each game holds the other's old lock when the new panels open.
        // With the nonce alone the two games waited for each other for ever; the serial tells old from new.
        Exchange oldA, oldB, newA, newB;
        oldA.Open(kKindStage, table.Context(), 0x0A0A0A0Au, 100);
        oldB.Open(kKindStage, table.Context(), 0x0B0B0B0Bu, 50);
        oldA.Lock(3);
        oldB.Lock(4);
        oldA.OnPacket(oldB.Out());
        oldB.OnPacket(oldA.Out());
        newA.Open(kKindStage, table.Context(), 0x1A1A1A1Au, 101);
        newB.Open(kKindStage, table.Context(), 0x1B1B1B1Bu, 51);
        newA.OnPacket(oldB.Out());
        newB.OnPacket(oldA.Out());
        Expect(newA.theirs.locked && newB.theirs.locked && !newA.Ready(), "a leftover lock is taken while nothing newer is known (no result: this side has not locked)");
        newA.Lock(5);
        newB.Lock(6);
        Expect(newA.Ready() && !newA.Confirmed(), "with this side locked the leftover gives a result for the screen, never a confirmed one");
        for (int i = 0; i < 2; ++i) {
            newB.OnPacket(newA.Out());
            newA.OnPacket(newB.Out());
        }
        Expect(newA.Confirmed() && newB.Confirmed() && newA.theirs.choice == 6 && newB.theirs.choice == 5, "the new exchange replaces the leftovers on both sides");
        newA.OnPacket(oldB.Out());
        Expect(newA.theirs.choice == 6 && newA.Confirmed(), "a leftover that arrives after the new exchange is dropped");

        // "random" wins: the game's own choice stands.
        Exchange e, f;
        e.Open(kKindStage, table.Context(), 5, 1);
        f.Open(kKindStage, table.Context(), 6, 1);
        e.Lock(kRandom);
        f.Lock(kRandom);
        e.OnPacket(f.Out());
        f.OnPacket(e.Out());
        e.OnPacket(f.Out());
        const Verdict ve = Judge(e, table, true);
        Expect(ve.outcome == Outcome::kGameChoice && ve.why == Why::kAgreed && !ApplyToSettings(ve, msg) &&
                  CheckReceived(ve, 17) == Check::kNothingExpected,
              "'random' twice: agreed, and the game's own choice stands (nothing written, nothing expected)");

        // A pick this game's table does not offer (entry 0 is not selectable, entry 40 does not exist).
        for (const uint8_t bad : {(uint8_t)0, (uint8_t)40}) {
            Exchange g;
            g.Open(kKindStage, table.Context(), 77, 1);
            g.Lock(4);
            Packet p;
            p.open = true;
            p.context = table.Context();
            p.me.nonce = 88;
            p.serial = 5;
            p.me.choice = bad;
            p.me.locked = true;
            p.heard = g.mine;
            g.OnPacket(p);
            const Verdict vg = Judge(g, table, true);
            Expect(g.Confirmed() && vg.outcome == Outcome::kFallback && vg.why == Why::kNotPickable, "a pick the table does not offer: fallback, whoever won");
        }

        // The other mod lists the stages differently.
        Trial t;
        t.otherTable = true;
        const Seen s = Run(t);
        Expect(!s.everReadyA && !s.everReadyB && s.a.contextDiffers && s.b.contextDiffers &&
                  Judge(s.a, table, false).why == Why::kContextDiffers && Judge(s.a, table, false).outcome == Outcome::kFallback,
              "two different stage tables: no result on either side, the reason named");

        // A table that changed under an open exchange.
        Table changed = table;
        changed.selectable[1] = false;
        Expect(Judge(a, changed, true).why == Why::kContextDiffers, "a table that is no longer the one the exchange was opened with: fallback");
        Expect(table.Context() != changed.Context() && NameHash("BGND_Pit") == NameHash("bgnd_PIT") && NameHash("BGND_Pit") != NameHash("BGND_PitDay"),
              "the context follows the names (any letter case) and the selectable marks");
    }

    printf("-- 5. many exchanges over bad links\n");
    {
        int runs = 0, bothLocked = 0, confirmedBoth = 0, broken = 0, disagreed = 0, undone = 0, verdictsDiffer = 0, readyWithoutBoth = 0, staleRuns = 0;
        int cursorBack = 0;
        for (int i = 0; i < 6000; ++i) {
            Trial t;
            t.lossPct = (int)Rnd(51);
            t.maxDelay = (int)Rnd(6);
            t.dupPct = (int)Rnd(30);
            t.stale = Rnd(3) == 0;
            t.lockA = Rnd(8) == 0 ? -1 : (int)Rnd(60);
            t.lockB = Rnd(8) == 0 ? -1 : (int)Rnd(60);
            t.ticks = 200;
            const Seen s = Run(t);
            ++runs;
            staleRuns += t.stale;
            broken += s.confirmedWithoutPeerReady;
            disagreed += s.readyDisagreed;
            undone += s.unlocked;
            cursorBack += s.cursorWentBack;
            const bool both = t.lockA >= 0 && t.lockB >= 0;
            if (!both) {
                // One player never locked: at the end neither game has a result (a leftover of an earlier exchange may
                // have shown one for a moment; it is never confirmed).
                readyWithoutBoth += s.a.Ready() || s.b.Ready() || s.confirmedA || s.confirmedB;
                continue;
            }
            ++bothLocked;
            confirmedBoth += s.confirmedA && s.confirmedB;
            const Verdict va = Judge(s.a, table, true), vb = Judge(s.b, table, false);
            verdictsDiffer += va.outcome != vb.outcome || va.entry != vb.entry || va.outcome == Outcome::kFallback;
        }
        printf("  %d exchanges (%d with both players locking, %d with leftovers of an earlier exchange), loss 0-50%%, up to 5 ticks late, repeats, any order\n",
               runs, bothLocked, staleRuns);
        Expect(broken == 0, "a game is never confirmed unless the other game has the same result at that moment");
        Expect(disagreed == 0, "two games that both have a result have the same one");
        Expect(undone == 0, "a lock or a confirmation never goes away");
        Expect(cursorBack == 0, "the other player's cursor is always the newest state that has arrived, never an older one");
        Expect(confirmedBoth == bothLocked && bothLocked > 4000, "every exchange in which both players locked ends confirmed on both sides");
        Expect(verdictsDiffer == 0, "and with the same verdict in both games");
        Expect(readyWithoutBoth == 0, "an exchange in which a player never locked ends without a result");
    }

    printf("\n%d checks, %d failed\n", gChecks, gFailed);
    if (!gFailed) printf("ALL CHECKS PASSED\n");
    return gFailed ? 1 : 0;
}
