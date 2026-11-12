#pragma once
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>

#include "../common/StrAppend.h"

// The text of the "G2" trace lines (docs\feature_parity_plan.md, sections 4.2, 4.3 and 5): how MK9 passes the stage and
// the rematch between the two games of an online match. This header only turns values into text and decides which
// calls get a line; it reads no game memory and has a host-side test (tests\host\flow_test.cpp). FlowTrace.cpp collects
// the values in the game.
//
// Everything named here was read in the game's code (..\analysis\mkke_text.asm) on 2026-11-12:
//  - Message 9 "GameSettings": 16 bytes, four 32-bit values in the order of the game's own debug line
//    "DumpDebugGameSettingsMessage RoundTime:%d RoundsToWin:%d PlayerOrientation:%d SelectedBackgroundIndex:%d"
//    (0x00D681A8): the sender 0x0087F850 writes RoundTime and RoundsToWin as 0, SelectedBackgroundIndex from the
//    session's +0x4C30 into the third value and "this game's [0x00E89CF0]+0x3C0 is 0" into the fourth; the receiver
//    0x0087F9E0 stores the fourth into [0x00E89CF0]+0x3C0 and the third into the session's +0x4C30.
//  - Message ids are the ones the session's HandleIncomingMessage 0x00886EF0 switches on (jump table 0x00887374): its
//    own log texts name 2 MT_Topology, 7 MT_Spectator, 9 MT_GameSettings, 10 MT_ReadyState, 12 MT_MatchDecision and
//    16 StartOfBacklog.
//  - Message 10 "ReadyState" is a zero-terminated name and one byte (0x00887046: read string, read byte, find the
//    connection of that name, set its state). The name is a player's name: it is never put into the log.
//  - Sync wait types: the name function 0x009301F0 (jump table 0x009302D8).
namespace FlowDecode {

inline const char* MessageName(uint32_t id) {
    switch (id) {
        case 2: return "Topology";
        case 7: return "Spectator";
        case 9: return "GameSettings";
        case 10: return "ReadyState";
        case 12: return "MatchDecision";
        case 16: return "StartOfBacklog";
        default: return "";
    }
}

inline const char* SyncWaitName(uint32_t type) {
    switch (type) {
        case 0: return "ST_Invalid";
        case 1: return "ST_EndPlayerSelectScreenLoading";
        case 2: return "ST_BeginGameLoading";
        case 3: return "ST_VersusKodeLockdown";
        case 4: return "ST_EndGameLoading";
        case 5: return "ST_RematchDecisionAccept";
        case 6: return "ST_RematchDisconnect";
        case 8: return "ST_EndMatch";
        default: return "";  // 7 and everything above 8: the game has no name for them either
    }
}

// Bytes as they travel, in groups of four: "00000000 07000000". At most 'max' bytes, then "...".
inline int AppendHex(char* out, size_t cap, int n, const uint8_t* p, size_t len, size_t max) {
    for (size_t i = 0; i < len && i < max; ++i) n = AppendF(out, cap, n, "%s%02X", i && !(i & 3) ? " " : "", p[i]);
    if (len > max) n = AppendF(out, cap, n, "...");
    return n;
}

// Text from the game's memory for a log line: printable ASCII only, anything else '?', at most cap - 1 characters.
inline void CleanText(char* out, size_t cap, const char* in, size_t inLen) {
    if (!cap) return;
    size_t k = 0;
    for (; k + 1 < cap && k < inLen && in[k]; ++k) out[k] = (unsigned char)in[k] >= 0x20 && (unsigned char)in[k] <= 0x7E ? in[k] : '?';
    out[k] = 0;
}

constexpr size_t kGameSettingsBytes = 16;
struct GameSettings {
    uint32_t roundTime = 0, roundsToWin = 0, backgroundIndex = 0, playerOrientation = 0;
};
inline uint32_t Le32(const uint8_t* p) { return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24); }
inline bool DecodeGameSettings(const uint8_t* p, size_t len, GameSettings* out) {
    if (!p || len < kGameSettingsBytes) return false;
    out->roundTime = Le32(p);
    out->roundsToWin = Le32(p + 4);
    out->backgroundIndex = Le32(p + 8);
    out->playerOrientation = Le32(p + 12);
    return true;
}
// "16 bytes 00000000 00000000 07000000 01000000: round time 0, rounds to win 0, selected background index 7, player
// orientation 1". A message of another length is shown as bytes, with what does not fit said.
inline int AppendGameSettings(char* out, size_t cap, int n, const uint8_t* p, size_t len) {
    n = AppendF(out, cap, n, "%u bytes ", (unsigned)len);
    n = AppendHex(out, cap, n, p, len, 32);
    GameSettings g;
    if (!DecodeGameSettings(p, len, &g)) return AppendF(out, cap, n, ": fewer than the 16 of GameSettings, not decoded");
    n = AppendF(out, cap, n, ": round time %u, rounds to win %u, selected background index %u, player orientation %u", (unsigned)g.roundTime,
                (unsigned)g.roundsToWin, (unsigned)g.backgroundIndex, (unsigned)g.playerOrientation);
    if (len > kGameSettingsBytes) n = AppendF(out, cap, n, " (the first 16 bytes; the game reads no more)");
    return n;
}

// "9 bytes: a name of 7 characters (not logged), then 01". The name's bytes never reach the text.
inline int AppendReadyState(char* out, size_t cap, int n, const uint8_t* p, size_t len) {
    n = AppendF(out, cap, n, "%u bytes: ", (unsigned)len);
    size_t name = 0;
    while (name < len && p[name]) ++name;
    if (name == len) return AppendF(out, cap, n, "no end of the name in them, not decoded (a player's name is not logged)");
    n = AppendF(out, cap, n, "a name of %u characters (not logged), then ", (unsigned)name);
    const size_t rest = len - name - 1;
    if (!rest) return AppendF(out, cap, n, "nothing");
    return AppendHex(out, cap, n, p + name + 1, rest, 16);
}

// The payload of a message this trace shows: 9 and 10 decoded; any other id as its length alone (its bytes may hold
// names, and nothing here knows their layout).
inline bool Shown(uint32_t id) { return id == 9 || id == 10; }
inline int AppendPayload(char* out, size_t cap, int n, uint32_t id, const uint8_t* p, size_t len) {
    if (id == 9) return AppendGameSettings(out, cap, n, p, len);
    if (id == 10) return AppendReadyState(out, cap, n, p, len);
    return AppendF(out, cap, n, "%u bytes", (unsigned)len);
}

// ---- script natives ----
// What the trace holds about one call of a traced native. The operands are the words the native itself reads
// ([executor +0x3C] + 4, + 8, ...); 'result' is the executor's result slot (+0x1C) after the call.
struct NativeCall {
    uint32_t id = 0;
    uint32_t arg[4] = {0, 0, 0, 0};
    bool argsRead = false;        // the operand words could be read
    uint32_t result = 0;
    bool resultRead = false;
    const char* text[3] = {nullptr, nullptr, nullptr};  // operands 1-3 as text (670, 671, 672), cleaned; null: not readable
    // Read after the call:
    bool sessionRead = false;     // the online session's +0x4C30 could be read
    uint32_t sessionIndex = 0;
    const char* sessionStage = nullptr;  // the name of the stage table's entry number sessionIndex; null: no such entry
    const char* stage = nullptr;  // 671, 672, 1179: the fight's stage name as the game holds it now; null: not readable
    int stageIndex = -1;          // its place in the stage table; -1: not in it
    uint32_t stageCount = 0;      // the stage table's entries; 0: the table could not be read
    bool settingsKnown = false;   // 1375: the tap where the game fills message 9 is installed
    bool settingsSent = false;    // 1375: message 9 was built during this call
    const uint8_t* payload = nullptr;  // 1224-1227 with a shown message id: the data operand's bytes (null: not readable)
    size_t payloadLen = 0;
};

inline bool SendNative(uint32_t id) { return id >= 1224 && id <= 1227; }
// The message id operand of a send native: 1226 takes the receiver's name first.
inline uint32_t SendMessageId(const NativeCall& c) { return c.id == 1226 ? c.arg[1] : c.arg[0]; }
inline uint32_t SendLength(const NativeCall& c) { return c.id == 1226 ? c.arg[3] : c.arg[2]; }

inline bool Traced(uint32_t id) {
    return id == 670 || id == 671 || id == 672 || id == 1178 || id == 1179 || id == 1259 || id == 1375 || SendNative(id);
}

// "BGND_Pit, stage table entry 12 of 37": the fight's stage (the name 0x00811DD0 keeps at [0x00E89B8C]+0x3C) and where
// the stage table [0x00E9234C] holds that name. name null: not readable; index -1: not in the table; count 0: no table.
inline int AppendStage(char* out, size_t cap, int n, const char* name, int index, uint32_t count) {
    if (!name) return AppendF(out, cap, n, "<not readable>");
    n = AppendF(out, cap, n, "%s, ", name);
    if (!count) return AppendF(out, cap, n, "stage table not readable");
    return index >= 0 ? AppendF(out, cap, n, "stage table entry %d of %u", index, (unsigned)count)
                      : AppendF(out, cap, n, "not in the stage table (%u entries)", (unsigned)count);
}
// "session +0x4C30 is 7 (stage table entry 7 of 37 is BGND_Pit)": the selected background index the two games pass to
// each other, and the stage table's entry of that number. Whether the game uses the index as that entry's number is one
// of the things these lines are for; the entry is shown so that a log answers it.
inline int AppendSession(char* out, size_t cap, int n, bool read, uint32_t index, const char* entryName, uint32_t count) {
    if (!read) return AppendF(out, cap, n, "no online session to read +0x4C30 from");
    n = AppendF(out, cap, n, "session +0x4C30 is %u", (unsigned)index);
    if (!count) return n;
    return entryName ? AppendF(out, cap, n, " (stage table entry %u of %u is %s)", (unsigned)index, (unsigned)count, entryName)
                     : AppendF(out, cap, n, " (the stage table has %u entries, none of that number)", (unsigned)count);
}

// The part of an ONLINEFLOW line after "): ". Returns the new length; nothing is appended for an id that is not traced.
inline int AppendNative(char* out, size_t cap, int n, const NativeCall& c) {
    const auto quoted = [&](const char* t) { return t ? AppendF(out, cap, n, "\"%s\"", t) : AppendF(out, cap, n, "<not readable>"); };
    const auto session = [&] { return AppendSession(out, cap, n, c.sessionRead, c.sessionIndex, c.sessionStage, c.stageCount); };
    const auto stageNow = [&] {
        n = AppendF(out, cap, n, "; the fight's stage is now ");
        return AppendStage(out, cap, n, c.stage, c.stageIndex, c.stageCount);
    };
    switch (c.id) {
        case 670:  // 0x008BB510 -> 0x008142E0, the game's "SetSelectedPlayers(pchr1:%s, pchr2:%s)"
            n = AppendF(out, cap, n, "SetSelectedPlayers(");
            n = quoted(c.text[0]);
            n = AppendF(out, cap, n, ", ");
            n = quoted(c.text[1]);
            return AppendF(out, cap, n, ")");
        case 671:  // 0x008BB530 -> 0x00811DD0, the game's "SetSelectedBackground(bgnd:%s)"
            n = AppendF(out, cap, n, "SetSelectedBackground(");
            n = quoted(c.text[0]);
            n = AppendF(out, cap, n, ")");
            return stageNow();
        case 672:  // 0x008BB550 -> 0x008143D0: 0x00811DD0 with operand 3, then 0x008142E0 with operands 1 and 2
            n = AppendF(out, cap, n, "SetSelectedBackground(");
            n = quoted(c.text[2]);
            n = AppendF(out, cap, n, ") and SetSelectedPlayers(");
            n = quoted(c.text[0]);
            n = AppendF(out, cap, n, ", ");
            n = quoted(c.text[1]);
            n = AppendF(out, cap, n, ") in one call");
            return stageNow();
        case 1178:  // 0x008BFC90 -> 0x0081C150: operand 1 into the session's +0x4C30
            if (c.argsRead) n = AppendF(out, cap, n, "sets the selected background index to %u; ", (unsigned)c.arg[0]);
            else n = AppendF(out, cap, n, "sets the selected background index (operand not readable); ");
            return session();
        case 1179:  // 0x008BFCB0 -> 0x0081C180: the session's +0x4C30 as the script's result
            if (c.resultRead) n = AppendF(out, cap, n, "reads the selected background index: %u; ", (unsigned)c.result);
            else n = AppendF(out, cap, n, "reads the selected background index (result not readable); ");
            n = session();
            return stageNow();
        case 1259: {  // 0x008C0500 -> 0x0081C700 -> MKOnlineSession::BeginSyncWait 0x0091C6F0
            if (!c.argsRead) return AppendF(out, cap, n, "BeginSyncWait(operand not readable)");
            const char* name = SyncWaitName(c.arg[0]);
            n = AppendF(out, cap, n, "BeginSyncWait(%u%s%s)", (unsigned)c.arg[0], *name ? " " : "", name);
            return c.resultRead ? AppendF(out, cap, n, " -> %u", (unsigned)c.result) : n;
        }
        case 1375:  // 0x008C1170 -> 0x0087F850 "MKOnline_SendGameSettings!"
            n = AppendF(out, cap, n, "SendGameSettings: %s; ",
                        !c.settingsKnown ? "whether message 9 was built is not known (the tap on it is not installed)"
                        : c.settingsSent ? "message 9 built for the other game (see ONLINEMSG send)"
                                         : "no message built (the game's two checks before it, 0x0081C240 and 0x0081C2C0, did not both pass)");
            return session();
        default: break;
    }
    if (!SendNative(c.id)) return n;
    // 1224 0x008C0040 -> 0x0081BCD0 (every connection), 1225 0x008C0070 -> 0x0081BDA0 (one connection the session picks),
    // 1226 0x008C00A0 -> 0x0081BE80 (the connection of the named player), 1227 0x008C00D0 -> 0x00820C10 (a list).
    static const char* const kTo[4] = {"every connection", "one connection", "the named player's connection", "a list of connections"};
    if (!c.argsRead) return AppendF(out, cap, n, "sends a message to %s (operands not readable)", kTo[c.id - 1224]);
    const uint32_t msg = SendMessageId(c);
    const char* name = MessageName(msg);
    n = AppendF(out, cap, n, "sends message %u%s%s to %s: ", (unsigned)msg, *name ? " " : "", name, kTo[c.id - 1224]);
    if (Shown(msg) && !c.payload) n = AppendF(out, cap, n, "%u bytes, not readable", (unsigned)SendLength(c));
    else n = AppendPayload(out, cap, n, msg, c.payload, c.payload ? c.payloadLen : SendLength(c));
    return c.resultRead ? AppendF(out, cap, n, " -> %u", (unsigned)c.result) : n;
}

// ---- which calls get a line ----
// ONLINEFLOW has always logged calls 1-3 of a timeline and then the 10th, 100th, ...: a native polled every tick shows
// as such and costs a handful of lines.
inline bool CountedCall(uint32_t n) { return n <= 3 || n == 10 || n == 100 || n == 1000 || n == 10000 || n == 100000; }
inline uint32_t TextHash(const char* s) {  // FNV-1a
    uint32_t h = 2166136261u;
    for (; *s; ++s) h = (h ^ (uint8_t)*s) * 16777619u;
    return h;
}
// The same hash over bytes, continued from 'h': the values of a call (operands, result, session index, texts) are
// hashed as they are read, so that a call whose line would say the same as the last one costs no text at all.
inline uint32_t BytesHash(const void* p, size_t len, uint32_t h = 2166136261u) {
    const uint8_t* b = static_cast<const uint8_t*>(p);
    for (size_t i = 0; i < len; ++i) h = (h ^ b[i]) * 16777619u;
    return h;
}
// A traced native also gets a line whenever its values differ from those of its last line (another stage index,
// another sync wait), at most kChangeLines such lines per timeline: the values are the point of the trace, and a native
// that changes every tick must still not fill the log.
constexpr uint32_t kChangeLines = 24;
struct Repeat {
    uint32_t lastHash = 0, changeLines = 0;
    bool any = false;
    // 1: log; 2: log, and this was the last change line of the timeline; 0: no line.
    int Decide(uint32_t callNo, uint32_t hash) {
        const bool counted = CountedCall(callNo), changed = !any || hash != lastHash;
        if (!counted && (!changed || changeLines >= kChangeLines)) return 0;
        any = true;
        lastHash = hash;
        if (counted) return 1;
        return ++changeLines == kChangeLines ? 2 : 1;
    }
    void Reset() { *this = Repeat{}; }
};

// ---- messages that are only counted ----
// Every message the session's handler takes that is not shown above: how many of each id since the last line that
// listed them. " | other messages received since the last such list: 3 x1200, 13 x2" (ids above 31 under "32+").
constexpr int kCountedIds = 33;
inline int AppendOthers(char* out, size_t cap, int n, const uint32_t* counts) {
    bool any = false;
    for (int i = 0; i < kCountedIds; ++i) {
        if (!counts[i]) continue;
        n = AppendF(out, cap, n, "%s", any ? ", " : " | other messages received since the last such list: ");
        any = true;
        if (i == kCountedIds - 1) n = AppendF(out, cap, n, "32+ x%u", (unsigned)counts[i]);
        else n = AppendF(out, cap, n, "%d x%u", i, (unsigned)counts[i]);
    }
    return n;
}

}  // namespace FlowDecode
