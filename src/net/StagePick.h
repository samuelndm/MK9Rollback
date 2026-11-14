#pragma once
#include <cstddef>
#include <cstdint>
#include <cstring>

// The mods' second datagram on the hello's Steam channel (OnlineMode.cpp, channel 79): a choice each player makes on a
// panel of the mod, shown to the other one, and what the two choices give. First use: the stage picks of an online
// match (docs\feature_parity_plan.md 4.2: each player picks an arena after the fighters are locked, one of the two
// picks is taken at random). Shaped for a second use, the choice after a match (plan 4.3, kind 2: reserved, not built).
// Nothing here touches the game, Steam or Windows: tests\host\stagepick_test.cpp runs it without them.
//
// A release that does not know this packet drops it: HelloProto::Parse answers kNone for another magic, and
// OnlineMode's read loop goes on to the next datagram (checked by the host test with the real HelloProto).
//
// Wire layout, little endian, 32 bytes:
//    0 u32 magic "KR9M"       4 u8 version (1)     5 u8 kind         6 u8 size (32)      7 u8 flags (kOpen)
//    8 u32 context            what both sides must share for the choices to mean the same thing (stage picks: a hash
//                             of the stage table's names and "selectable" marks)
//   12 u32 nonce              the sender's random number for this exchange (never 0 while its panel is open)
//   16 u8 choice             17 u8 cursor         18 u8 locked      19 u8 0
//   20 u32 heardNonce        24 u8 heardChoice    25 u8 heardLocked              what the sender has from the receiver
//   26 u16 moves              the sender's count of its cursor moves in this exchange (its lock counts as one)
//   28 u32 serial             the sender's count of its exchanges: a later exchange of one sender has a larger one
// A later build may append: a packet of 32 bytes or more with version 1 is read by its first 32 bytes.
//
// How two games come to the same answer without a message that says it:
//  - every exchange (one panel shown once) gets a fresh nonce on each side; a side's packets repeat its whole state, so
//    a lost datagram costs nothing;
//  - a packet left over from an earlier exchange carries another nonce and a smaller serial: it never replaces what a
//    later exchange of the same sender said (the host test found why the serial is needed: with the nonce alone, two
//    games that each held a leftover lock of the other waited for each other for ever);
//  - "locked" is final: after it, the choice of that nonce never changes (a reordered older packet is ignored);
//  - the cursor is shown on the other screen while the player still chooses. Every packet carries where it stands and
//    how often it has moved: a packet that arrives late (with a smaller count than one already taken) never moves the
//    shown cursor back, and a packet that never arrives is made good by the next one;
//  - Ready: both sides locked, as this side knows them. Confirmed: Ready, and the other side's latest packet repeats
//    this side's lock under this side's nonce, so the other side is Ready too. Anything written into the game waits
//    for Confirmed; the panel may show the result at Ready;
//  - the result is a function of the two (nonce, choice) pairs that does not depend on which side computes it.
// The coin needs no value of the game's: each side draws its nonce itself. (The plan's first idea, the match's random
// seed at the game session's +0x4C2C, is real: the game reads it from a message at 0x008866D6 and seeds the fight's
// random numbers with it at 0x00884829. But it is one value per connection, the same for every rematch, and nothing
// of it has been seen in a real match. Two exchanged numbers do the same job without it.)
namespace StagePick {

constexpr uint32_t kMagic = 0x4D39524Bu;  // "KR9M"
constexpr uint8_t kVersion = 1;
constexpr size_t kSize = 32;
enum Kind : uint8_t { kKindStage = 1, kKindAfterMatch = 2 /* plan 4.3: reserved */ };
// kOpen: the sender's panel is open (clear: it only says "this mod has the panel"). kApply: the sender will write an
// agreed stage into the game when it is the one that sends the settings message (clear: a trial run, the draw is shown
// and the game's own choice stays), so the receiver's banner never names an arena the game was not given.
// kSure: the sender holds the game (kApply) and knows that the receiver has both picks (Exchange::Confirmed); read by
// StageHold. A build that does not know the bit never sets it and never writes an arena after the picks.
enum Flags : uint8_t { kOpen = 1, kApply = 2, kSure = 4 };

// A choice: an entry number (stage picks: the stage table's entry), kRandom ("the game's own choice"), or kNoChoice.
constexpr uint8_t kNoChoice = 0xFF, kRandom = 0xFE, kMaxEntries = 64;
inline bool IsEntry(uint8_t c) { return c < kMaxEntries; }
inline bool IsChoice(uint8_t c) { return IsEntry(c) || c == kRandom; }

struct Side {  // one player's part of an exchange
    uint32_t nonce = 0;          // 0: no such side yet
    uint8_t choice = kNoChoice;  // meaningful once locked
    uint8_t cursor = kNoChoice;  // where the player's cursor stands (for the other screen; no meaning for the result)
    bool locked = false;
};

struct Packet {
    uint8_t kind = kKindStage;
    bool open = false;
    bool apply = false;
    bool sure = false;
    uint8_t seq = 0;     // the sender's count of packets (wraps): the two flags are taken from the latest one sent, not the latest to arrive
    uint32_t context = 0;
    uint32_t serial = 0;
    uint16_t moves = 0;  // how often the sender's cursor has moved in this exchange
    Side me;     // the sender
    Side heard;  // what the sender last had from the receiver (cursor unused)
};

namespace detail {
inline void Put32(uint8_t* p, uint32_t v) { memcpy(p, &v, 4); }
inline uint32_t Get32(const uint8_t* p) {
    uint32_t v;
    memcpy(&v, p, 4);
    return v;
}
// Peer-controlled bytes are brought into what this mod sends: a choice that is none of the three kinds is no choice,
// and a lock without a choice is no lock.
inline Side Clean(uint32_t nonce, uint8_t choice, uint8_t cursor, uint8_t locked) {
    Side s;
    s.nonce = nonce;
    s.choice = IsChoice(choice) ? choice : kNoChoice;
    s.cursor = IsChoice(cursor) ? cursor : kNoChoice;
    s.locked = locked != 0 && s.choice != kNoChoice && nonce != 0;
    if (!s.locked) s.choice = kNoChoice;
    return s;
}
inline uint32_t Mix(uint32_t h) {  // a 32-bit finalizer (every input bit reaches every output bit)
    h ^= h >> 16;
    h *= 0x7FEB352Du;
    h ^= h >> 15;
    h *= 0x846CA68Bu;
    h ^= h >> 16;
    return h;
}
}  // namespace detail

// True for a datagram that starts with this packet's magic (whatever follows): not a hello.
inline bool IsPacket(const void* data, size_t size) {
    return data && size >= 4 && detail::Get32(static_cast<const uint8_t*>(data)) == kMagic;
}

// Writes the 32 bytes; returns the size, 0 when cap is too small.
inline size_t Build(const Packet& p, void* out, size_t cap) {
    if (cap < kSize) return 0;
    uint8_t* b = static_cast<uint8_t*>(out);
    memset(b, 0, kSize);
    detail::Put32(b, kMagic);
    b[4] = kVersion;
    b[5] = p.kind;
    b[6] = (uint8_t)kSize;
    b[7] = (uint8_t)((p.open ? kOpen : 0) | (p.apply ? kApply : 0) | (p.open && p.apply && p.sure ? kSure : 0));
    b[19] = p.seq;
    detail::Put32(b + 8, p.context);
    detail::Put32(b + 12, p.me.nonce);
    b[16] = p.me.locked ? p.me.choice : kNoChoice;
    b[17] = p.me.cursor;
    b[18] = p.me.locked ? 1 : 0;
    detail::Put32(b + 20, p.heard.nonce);
    b[24] = p.heard.locked ? p.heard.choice : kNoChoice;
    b[25] = p.heard.locked ? 1 : 0;
    memcpy(b + 26, &p.moves, 2);
    detail::Put32(b + 28, p.serial);
    return kSize;
}

enum class Parsed : uint8_t {
    kNone,          // not this packet (another magic, under 8 bytes)
    kOtherVersion,  // this magic, a version this build does not read: the other mod is another build
    kMalformed,     // this magic and version, a length or a kind no build sends
    kOk,
};
// Any byte string of any length.
inline Parsed Parse(const void* data, size_t size, Packet* out) {
    *out = Packet{};
    const uint8_t* b = static_cast<const uint8_t*>(data);
    if (!b || size < 8 || detail::Get32(b) != kMagic) return Parsed::kNone;
    if (b[4] != kVersion) return Parsed::kOtherVersion;
    if (size < kSize || b[6] < kSize || (b[5] != kKindStage && b[5] != kKindAfterMatch)) return Parsed::kMalformed;
    out->kind = b[5];
    out->open = (b[7] & kOpen) != 0;
    out->apply = (b[7] & kApply) != 0;
    out->sure = out->apply && (b[7] & kSure) != 0;
    out->seq = b[19];
    out->context = detail::Get32(b + 8);
    out->serial = detail::Get32(b + 28);
    memcpy(&out->moves, b + 26, 2);
    out->me = detail::Clean(detail::Get32(b + 12), b[16], b[17], b[18]);
    out->heard = detail::Clean(detail::Get32(b + 20), b[24], kNoChoice, b[25]);
    if (!out->open) {  // a closed panel has no side
        out->me = out->heard = Side{};
        out->moves = 0;
    }
    return Parsed::kOk;
}

// One side's view of an exchange.
struct Exchange {
    uint8_t kind = kKindStage;
    uint32_t context = 0;
    uint32_t serial = 0, theirSerial = 0;
    uint16_t moves = 0, theirMoves = 0;  // cursor moves: this side's count, and the count of the state held in 'theirs'
    bool open = false;
    Side mine, theirs;
    Side echoed;                  // what they say they have from this side's current exchange
    bool theirsOpen = false;      // their latest packet came from an open panel
    bool apply = false;           // this side will write an agreed stage into the game (set after Open)
    bool theirsApply = false;     // their latest packet says they will
    bool theirsSure = false;      // their latest packet says they will, and that they know this side has both picks
    uint8_t seq = 0, theirSeq = 0;  // packets sent (Next), and the count of the packet the two flags above were taken from
    bool contextDiffers = false;  // their latest packet names another context: nothing of theirs is used

    // A new exchange. nonce: this side's random number, not 0 (0 is replaced by 1). ser: larger than the one of every
    // earlier exchange this game opened (a count kept by the caller, started from the clock).
    void Open(uint8_t k, uint32_t ctx, uint32_t nonce, uint32_t ser) {
        *this = Exchange{};
        kind = k;
        context = ctx;
        serial = ser;
        open = true;
        mine.nonce = nonce ? nonce : 1;
    }
    void Close() { open = false; }

    bool MoveCursor(uint8_t c) {
        if (!open || mine.locked || !IsChoice(c)) return false;
        if (c != mine.cursor) ++moves;
        mine.cursor = c;
        return true;
    }
    bool Lock(uint8_t choice) {
        if (!open || mine.locked || !IsChoice(choice)) return false;
        mine.choice = mine.cursor = choice;
        mine.locked = true;
        ++moves;
        return true;
    }

    enum class Heard : uint8_t { kIgnored, kContextDiffers, kTaken };
    Heard OnPacket(const Packet& p) {
        if (!open || p.kind != kind) return Heard::kIgnored;
        if (p.context != context) {
            contextDiffers = true;
            theirs = echoed = Side{};
            theirsOpen = false;
            return Heard::kContextDiffers;
        }
        contextDiffers = false;
        theirsOpen = p.open;
        if (!p.open || !p.me.nonce) {  // "this mod has the panel", nothing more
            theirsApply = theirsSure = false;
            return Heard::kIgnored;
        }
        if (p.me.nonce != theirs.nonce) {
            // Another exchange of theirs than the one known here: taken when it is a later one, dropped when it is a
            // leftover of an earlier one.
            if (theirs.nonce && (int32_t)(p.serial - theirSerial) <= 0) return Heard::kIgnored;
            theirs = p.me;
            theirSerial = p.serial;
            theirMoves = p.moves;
            echoed = Side{};
            theirSeq = p.seq;
            theirsApply = p.apply;
            theirsSure = p.sure;
        } else {
            if (!theirs.locked && (p.me.locked || (int16_t)(p.moves - theirMoves) >= 0)) {
                // A lock is final under its nonce. Before it, the cursor follows the packets in the order they were
                // sent: one that arrives after a later one is dropped, so the shown cursor never jumps back.
                theirs = p.me;
                theirMoves = p.moves;
            }
            // "I hold" and "I am sure" can be taken back, so they come from the latest packet sent, not from one that
            // arrives late. (A build without the count sends 0 every time: every packet of it is taken.)
            if ((int8_t)(p.seq - theirSeq) >= 0) {
                theirSeq = p.seq;
                theirsApply = p.apply;
                theirsSure = p.sure;
            }
        }
        // What they have from this side's exchange only grows (this side's lock is final too).
        if (p.heard.nonce == mine.nonce && (p.heard.locked || !echoed.locked)) echoed = p.heard;
        return Heard::kTaken;
    }

    Packet Out() const {
        Packet p;
        p.kind = kind;
        p.open = open;
        p.context = context;
        p.serial = serial;
        p.apply = open && apply;
        p.sure = p.apply && Confirmed();
        p.seq = seq;
        if (open) {
            p.moves = moves;
            p.me = mine;
            p.heard = theirs;
        }
        return p;
    }
    Packet Next() {  // the next packet to send: Out with the next count
        ++seq;
        return Out();
    }

    bool Ready() const { return open && !contextDiffers && mine.locked && theirs.locked && theirs.nonce != 0; }
    bool Confirmed() const {
        return Ready() && echoed.locked && echoed.nonce == mine.nonce && echoed.choice == mine.choice;
    }
};

// The coin of two nonces: 0 or 1, the same whichever order they are given in.
inline uint32_t Coin(uint32_t a, uint32_t b) {
    const uint32_t lo = a < b ? a : b, hi = a < b ? b : a;
    return detail::Mix(detail::Mix(lo ^ 0x9E3779B9u) + hi) & 1;
}

enum class Winner : uint8_t { kNone, kMine, kTheirs, kBoth };
struct Result {
    Winner winner = Winner::kNone;
    uint8_t choice = kNoChoice;  // the winning ticket: an entry, or kRandom (the game's own choice stands)
};
// Stage picks: each locked pick is one ticket. The same pick twice is that pick. Otherwise the two sides are put in
// the order of their nonces and the coin names the first or the second; equal nonces (one in 4 billion) give no
// result, and the caller falls back. Computed on either side, the answer names the same ticket.
inline Result Decide(const Side& mine, const Side& theirs) {
    Result r;
    if (!mine.locked || !theirs.locked || !mine.nonce || !theirs.nonce) return r;
    if (!IsChoice(mine.choice) || !IsChoice(theirs.choice)) return r;
    if (mine.choice == theirs.choice) {
        r.winner = Winner::kBoth;
        r.choice = mine.choice;
        return r;
    }
    if (mine.nonce == theirs.nonce) return r;
    const bool mineFirst = mine.nonce < theirs.nonce;
    const bool firstWins = Coin(mine.nonce, theirs.nonce) == 0;
    r.winner = mineFirst == firstWins ? Winner::kMine : Winner::kTheirs;
    r.choice = r.winner == Winner::kMine ? mine.choice : theirs.choice;
    return r;
}
// A number both sides share once Ready, for anything that should look the same on both screens (the roulette).
inline uint32_t SharedSeed(const Side& mine, const Side& theirs) {
    const uint32_t lo = mine.nonce < theirs.nonce ? mine.nonce : theirs.nonce;
    const uint32_t hi = mine.nonce < theirs.nonce ? theirs.nonce : mine.nonce;
    return detail::Mix(detail::Mix(hi ^ 0x85EBCA6Bu) + lo);
}

// The stage table as the picks see it: which entries a player may pick, and the context both mods must share.
struct Table {
    uint8_t count = 0;                // entries, at most kMaxEntries
    bool selectable[kMaxEntries] = {};
    uint32_t nameHash[kMaxEntries] = {};  // Hash of each entry's name (any letter case)
    uint32_t Context() const {
        uint32_t h = detail::Mix(0x57A6E000u + count);
        for (uint8_t i = 0; i < count && i < kMaxEntries; ++i) h = detail::Mix(h ^ nameHash[i]) + (selectable[i] ? 0x9E3779B9u : 0x1u);
        return h ? h : 1;
    }
    bool Pickable(uint8_t choice) const { return choice == kRandom || (choice < count && choice < kMaxEntries && selectable[choice]); }
};
inline uint32_t NameHash(const char* name) {  // FNV-1a over the name without its letter case
    uint32_t h = 2166136261u;
    for (; name && *name; ++name) {
        const uint8_t c = (uint8_t)(*name >= 'A' && *name <= 'Z' ? *name + 32 : *name);
        h = (h ^ c) * 16777619u;
    }
    return h;
}

// What the mod does with the game once an exchange ended, and why.
enum class Outcome : uint8_t {
    kStage,       // both games use this stage table entry
    kGameChoice,  // the winning ticket is "random": the game's own choice stands
    kFallback,    // nothing agreed: the game's own choice stands (today's behaviour)
};
enum class Why : uint8_t {
    kAgreed,
    kNotReady,         // a pick is missing (not locked, or the other mod never answered)
    kNotConfirmed,     // both locked here, but the other side has not shown that it knows this side's lock
    kContextDiffers,   // the two mods see different stage tables
    kNoCoin,           // equal nonces
    kNotPickable,      // the winning ticket is not an entry a player may pick in this game's table
};
struct Verdict {
    Outcome outcome = Outcome::kFallback;
    Why why = Why::kNotReady;
    Winner winner = Winner::kNone;
    uint8_t entry = kNoChoice;  // kStage: the stage table entry
};
// needConfirmed: for anything written into the game (the settings message). The panel's own display asks without it.
inline Verdict Judge(const Exchange& x, const Table& t, bool needConfirmed) {
    Verdict v;
    if (x.contextDiffers || x.context != t.Context()) {
        v.why = Why::kContextDiffers;
        return v;
    }
    if (!x.Ready()) return v;
    if (needConfirmed && !x.Confirmed()) {
        v.why = Why::kNotConfirmed;
        return v;
    }
    const Result r = Decide(x.mine, x.theirs);
    if (r.winner == Winner::kNone) {
        v.why = Why::kNoCoin;
        return v;
    }
    v.winner = r.winner;
    // Both tickets must be ones this game's table offers: a pick this game could not have made is not taken, whoever
    // won, so that both games judge the pair alike.
    if (!t.Pickable(x.mine.choice) || !t.Pickable(x.theirs.choice)) {
        v.why = Why::kNotPickable;
        v.winner = Winner::kNone;
        return v;
    }
    v.why = Why::kAgreed;
    if (r.choice == kRandom) {
        v.outcome = Outcome::kGameChoice;
        return v;
    }
    v.outcome = Outcome::kStage;
    v.entry = r.choice;
    return v;
}

inline const char* WhyText(Why w) {
    switch (w) {
    case Why::kAgreed: return "agreed";
    case Why::kNotReady: return "a pick is missing";
    case Why::kNotConfirmed: return "the other game has not confirmed this game's pick";
    case Why::kContextDiffers: return "the two games list different stages";
    case Why::kNoCoin: return "no coin (equal random numbers)";
    case Why::kNotPickable: return "a pick is not a stage this game offers";
    }
    return "?";
}

// The settings message's side of it (game message 9, 16 bytes: round time, rounds to win, selected background index,
// player orientation; the index is the third 32-bit value).
constexpr size_t kSettingsBytes = 16, kSettingsIndexAt = 8;
inline uint32_t SettingsIndex(const uint8_t* bytes16) { return detail::Get32(bytes16 + kSettingsIndexAt); }
// The game that sends the message: writes the agreed entry into it when there is one. False: the bytes are untouched.
inline bool ApplyToSettings(const Verdict& v, uint8_t* bytes16) {
    if (v.outcome != Outcome::kStage || !IsEntry(v.entry)) return false;
    detail::Put32(bytes16 + kSettingsIndexAt, v.entry);
    return true;
}
// The game that receives it: what the received index says about this game's own verdict.
enum class Check : uint8_t {
    kNothingExpected,  // this game has no agreed stage (a fallback, or "random" won): whatever the message says goes
    kSame,
    kDiffers,          // this game computed another stage: logged; the message's index is what both games load
};
inline Check CheckReceived(const Verdict& v, uint32_t receivedIndex) {
    if (v.outcome != Outcome::kStage) return Check::kNothingExpected;
    return receivedIndex == v.entry ? Check::kSame : Check::kDiffers;
}

}  // namespace StagePick
