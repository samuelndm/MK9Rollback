// Host-side test of src\net\FlowDecode.h: the text of the "G2" trace lines (game messages 9 and 10, the traced script
// natives, which calls get a line, the list of other messages). No game: the values are made here.
// Build and run: see run.ps1 beside this file.
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "FlowDecode.h"

using namespace FlowDecode;

namespace {
int gFailed = 0, gChecks = 0;
void Check(bool ok, const char* what) {
    ++gChecks;
    if (!ok) {
        ++gFailed;
        printf("FAIL  %s\n", what);
    }
}
void Same(const char* got, const char* want, const char* what) {
    ++gChecks;
    if (!strcmp(got, want)) return;
    ++gFailed;
    printf("FAIL  %s\n  got    \"%s\"\n  wanted \"%s\"\n", what, got, want);
}
void Le(uint8_t* p, uint32_t v) {
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

void Names() {
    Same(MessageName(9), "GameSettings", "message 9 is GameSettings");
    Same(MessageName(10), "ReadyState", "message 10 is ReadyState");
    Same(MessageName(12), "MatchDecision", "message 12 is MatchDecision");
    Same(MessageName(2), "Topology", "message 2 is Topology");
    Same(MessageName(7), "Spectator", "message 7 is Spectator");
    Same(MessageName(16), "StartOfBacklog", "message 16 is StartOfBacklog");
    Same(MessageName(3), "", "message 3 has no name here");
    Same(MessageName(0xFFFFFFFF), "", "a huge message id has no name");
    Same(SyncWaitName(1), "ST_EndPlayerSelectScreenLoading", "sync wait 1");
    Same(SyncWaitName(2), "ST_BeginGameLoading", "sync wait 2");
    Same(SyncWaitName(3), "ST_VersusKodeLockdown", "sync wait 3");
    Same(SyncWaitName(4), "ST_EndGameLoading", "sync wait 4");
    Same(SyncWaitName(5), "ST_RematchDecisionAccept", "sync wait 5");
    Same(SyncWaitName(6), "ST_RematchDisconnect", "sync wait 6");
    Same(SyncWaitName(8), "ST_EndMatch", "sync wait 8");
    Same(SyncWaitName(7), "", "sync wait 7 has no name in the game");
    Same(SyncWaitName(9), "", "sync wait 9 has no name");
}

void Hex() {
    const uint8_t b[10] = {0x00, 0x01, 0xAB, 0xFF, 0x10, 0x20, 0x30, 0x40, 0x7F, 0x80};
    char t[64];
    int n = AppendHex(t, sizeof t, 0, b, 10, 32);
    Same(t, "0001ABFF 10203040 7F80", "ten bytes in groups of four");
    Check(n == (int)strlen(t), "AppendHex returns the length");
    AppendHex(t, sizeof t, 0, b, 10, 4);
    Same(t, "0001ABFF...", "cut at the limit with dots");
    t[0] = 0;
    n = AppendHex(t, sizeof t, 0, b, 0, 4);
    Check(n == 0 && !t[0], "no bytes, no text");
}

void Settings() {
    uint8_t m[20] = {};
    Le(m + 0, 0);
    Le(m + 4, 0);
    Le(m + 8, 7);
    Le(m + 12, 1);
    GameSettings g;
    Check(DecodeGameSettings(m, 16, &g) && g.roundTime == 0 && g.roundsToWin == 0 && g.backgroundIndex == 7 && g.playerOrientation == 1,
          "16 bytes decode to the four values");
    Check(!DecodeGameSettings(m, 15, &g), "15 bytes do not decode");
    Check(!DecodeGameSettings(nullptr, 16, &g), "no bytes do not decode");
    char t[256];
    AppendGameSettings(t, sizeof t, 0, m, 16);
    Same(t, "16 bytes 00000000 00000000 07000000 01000000: round time 0, rounds to win 0, selected background index 7, player orientation 1",
         "the GameSettings line as the host sends it");
    Le(m + 0, 90);
    Le(m + 4, 2);
    Le(m + 8, 0x12345678);
    Le(m + 12, 0);
    AppendGameSettings(t, sizeof t, 0, m, 16);
    Same(t, "16 bytes 5A000000 02000000 78563412 00000000: round time 90, rounds to win 2, selected background index 305419896, "
            "player orientation 0",
         "every value comes from its own four bytes, low byte first");
    AppendGameSettings(t, sizeof t, 0, m, 12);
    Same(t, "12 bytes 5A000000 02000000 78563412: fewer than the 16 of GameSettings, not decoded", "a short message is shown, not decoded");
    AppendGameSettings(t, sizeof t, 0, m, 20);
    Check(strstr(t, "20 bytes ") == t && strstr(t, "selected background index 305419896") && strstr(t, "(the first 16 bytes; the game reads no more)"),
          "a longer message: the first 16 decoded, and said so");
    AppendPayload(t, sizeof t, 0, 9, m, 16);
    Check(strstr(t, "round time 90") != nullptr, "AppendPayload decodes message 9");
}

void Ready() {
    const uint8_t m[] = {'P', 'l', 'a', 'y', 'e', 'r', 'O', 'n', 'e', 0, 1};
    char t[256];
    AppendReadyState(t, sizeof t, 0, m, sizeof m);
    Same(t, "11 bytes: a name of 9 characters (not logged), then 01", "ReadyState: the name's length and the byte after it");
    Check(!strstr(t, "Player") && !strstr(t, "506C") && !strstr(t, "50 6C"), "the name is in the text neither as letters nor as hex");
    AppendReadyState(t, sizeof t, 0, m, 9);  // the terminator cut off
    Same(t, "9 bytes: no end of the name in them, not decoded (a player's name is not logged)", "no terminator: nothing of the bytes shown");
    Check(!strstr(t, "Player") && !strstr(t, "506C"), "also then the name is not in the text");
    AppendReadyState(t, sizeof t, 0, m, 10);
    Same(t, "10 bytes: a name of 9 characters (not logged), then nothing", "a name with nothing after it");
    const uint8_t m2[] = {'A', 'b', 0, 0, 0xFE, 3};
    AppendReadyState(t, sizeof t, 0, m2, sizeof m2);
    Same(t, "6 bytes: a name of 2 characters (not logged), then 00FE03", "three bytes after the name");
    const uint8_t m3[] = {0, 1};
    AppendReadyState(t, sizeof t, 0, m3, sizeof m3);
    Same(t, "2 bytes: a name of 0 characters (not logged), then 01", "an empty name");
    AppendReadyState(t, sizeof t, 0, m3, 0);
    Same(t, "0 bytes: no end of the name in them, not decoded (a player's name is not logged)", "an empty message");
    AppendPayload(t, sizeof t, 0, 10, m, sizeof m);
    Check(strstr(t, "a name of 9 characters") && !strstr(t, "Player"), "AppendPayload decodes message 10 without the name");
    AppendPayload(t, sizeof t, 0, 13, m, sizeof m);
    Same(t, "11 bytes", "another message: its length only");
    Check(Shown(9) && Shown(10) && !Shown(12) && !Shown(0), "9 and 10 are the shown messages");
}

void Natives() {
    char t[400];
    auto line = [&](const NativeCall& c) {
        t[0] = 0;
        AppendNative(t, sizeof t, 0, c);
        return t;
    };
    NativeCall c;
    c.id = 670;
    c.argsRead = true;
    c.text[0] = "char_scorpion";
    c.text[1] = "char_subzero";
    Same(line(c), "SetSelectedPlayers(\"char_scorpion\", \"char_subzero\")", "native 670");
    c.text[1] = nullptr;
    Same(line(c), "SetSelectedPlayers(\"char_scorpion\", <not readable>)", "native 670 with an unreadable operand");

    c = NativeCall{};
    c.id = 671;
    c.text[0] = "random";
    c.stage = "BGND_Pit";
    c.stageIndex = 12;
    c.stageCount = 37;
    Same(line(c), "SetSelectedBackground(\"random\"); the fight's stage is now BGND_Pit, stage table entry 12 of 37", "native 671");
    c.stageIndex = -1;
    Same(line(c), "SetSelectedBackground(\"random\"); the fight's stage is now BGND_Pit, not in the stage table (37 entries)",
         "native 671, a name the table does not hold");
    c.stage = nullptr;
    Same(line(c), "SetSelectedBackground(\"random\"); the fight's stage is now <not readable>", "native 671, the stage not readable");
    c.stage = "BGND_Pit";
    c.stageCount = 0;
    Same(line(c), "SetSelectedBackground(\"random\"); the fight's stage is now BGND_Pit, stage table not readable", "native 671, no stage table");

    c = NativeCall{};
    c.id = 672;
    c.text[0] = "char_scorpion";
    c.text[1] = "char_subzero";
    c.text[2] = "BGND_Subway";
    c.stage = "BGND_Subway";
    c.stageIndex = 4;
    c.stageCount = 37;
    Same(line(c), "SetSelectedBackground(\"BGND_Subway\") and SetSelectedPlayers(\"char_scorpion\", \"char_subzero\") in one call; "
                  "the fight's stage is now BGND_Subway, stage table entry 4 of 37",
         "native 672: the stage is its third operand");

    c = NativeCall{};
    c.id = 1178;
    c.argsRead = true;
    c.arg[0] = 7;
    c.sessionRead = true;
    c.sessionIndex = 7;
    Same(line(c), "sets the selected background index to 7; session +0x4C30 is 7", "native 1178, no stage table read");
    c.stageCount = 37;
    c.sessionStage = "BGND_Pit";
    Same(line(c), "sets the selected background index to 7; session +0x4C30 is 7 (stage table entry 7 of 37 is BGND_Pit)", "native 1178");
    c.sessionIndex = 99;
    c.sessionStage = nullptr;
    Same(line(c), "sets the selected background index to 7; session +0x4C30 is 99 (the stage table has 37 entries, none of that number)",
         "native 1178, an index past the table");
    c.sessionRead = false;
    Same(line(c), "sets the selected background index to 7; no online session to read +0x4C30 from", "native 1178 without a session");
    c.argsRead = false;
    Check(strstr(line(c), "(operand not readable)") != nullptr, "native 1178 with unreadable operands");

    c = NativeCall{};
    c.id = 1179;
    c.resultRead = true;
    c.result = 3;
    c.sessionRead = true;
    c.sessionIndex = 3;
    c.sessionStage = "BGND_Armory";
    c.stage = "BGND_Pit";
    c.stageIndex = 12;
    c.stageCount = 37;
    Same(line(c), "reads the selected background index: 3; session +0x4C30 is 3 (stage table entry 3 of 37 is BGND_Armory); "
                  "the fight's stage is now BGND_Pit, stage table entry 12 of 37",
         "native 1179: the index, the table's entry of that number and the fight's stage side by side");
    c.resultRead = false;
    Check(strstr(line(c), "reads the selected background index (result not readable); session +0x4C30 is 3") == t, "native 1179, no result");

    c = NativeCall{};
    c.id = 1259;
    c.argsRead = true;
    c.arg[0] = 5;
    c.resultRead = true;
    c.result = 1;
    Same(line(c), "BeginSyncWait(5 ST_RematchDecisionAccept) -> 1", "native 1259, the rematch vote's sync wait");
    c.arg[0] = 7;
    c.resultRead = false;
    Same(line(c), "BeginSyncWait(7)", "native 1259, a type without a name, no result");

    c = NativeCall{};
    c.id = 1375;
    c.settingsKnown = true;
    c.settingsSent = true;
    c.sessionRead = true;
    c.sessionIndex = 9;
    Check(strstr(line(c), "SendGameSettings: message 9 built for the other game") == t && strstr(t, "; session +0x4C30 is 9"), "native 1375 on the sender");
    c.settingsSent = false;
    Check(strstr(line(c), "SendGameSettings: no message built") == t && strstr(t, "; session +0x4C30 is 9"), "native 1375 on the other game");
    c.settingsKnown = false;
    Check(strstr(line(c), "SendGameSettings: whether message 9 was built is not known") == t && !strstr(t, "no message built"),
          "native 1375 without the tap on message 9: no claim either way");

    // The send natives: (id, data, length), and 1226 (name, id, data, length).
    const uint8_t ready[] = {'S', 'o', 'm', 'e', 'o', 'n', 'e', 0, 1};
    c = NativeCall{};
    c.id = 1224;
    c.argsRead = true;
    c.arg[0] = 10;
    c.arg[2] = sizeof ready;
    c.payload = ready;
    c.payloadLen = sizeof ready;
    c.resultRead = true;
    c.result = 1;
    Same(line(c), "sends message 10 ReadyState to every connection: 9 bytes: a name of 7 characters (not logged), then 01 -> 1", "native 1224 with message 10");
    Check(!strstr(t, "Someone"), "and the name stays out");
    c.id = 1226;
    c.arg[0] = 0x12345678;  // the receiver's name pointer: never shown
    c.arg[1] = 10;
    c.arg[2] = 0x0BADF00D;
    c.arg[3] = sizeof ready;
    Check(SendMessageId(c) == 10 && SendLength(c) == sizeof ready, "native 1226 takes the name first");
    Check(strstr(line(c), "sends message 10 ReadyState to the named player's connection: 9 bytes: a name of 7") == t, "native 1226 with message 10");
    Check(!strstr(t, "12345678") && !strstr(t, "0BADF00D"), "pointers are not in the line");
    c.id = 1225;
    c.arg[0] = 14;
    c.arg[2] = 4;
    c.payload = nullptr;
    Same(line(c), "sends message 14 to one connection: 4 bytes -> 1", "native 1225 with a message that is only counted");
    c.id = 1227;
    c.arg[0] = 9;
    c.arg[2] = 16;
    Same(line(c), "sends message 9 GameSettings to a list of connections: 16 bytes, not readable -> 1", "a shown message whose bytes could not be read");
    c.argsRead = false;
    Same(line(c), "sends a message to a list of connections (operands not readable)", "a send native with unreadable operands");

    c = NativeCall{};
    c.id = 1182;
    Same(line(c), "", "a native that is not traced adds nothing");
    Check(Traced(670) && Traced(671) && Traced(672) && Traced(1178) && Traced(1179) && Traced(1259) && Traced(1375) && Traced(1224) &&
              Traced(1227) && !Traced(669) && !Traced(673) && !Traced(1182) && !Traced(1223) && !Traced(1228) && !Traced(0),
          "the traced natives");

    // The two pieces the message and fight-start lines use on their own.
    t[0] = 0;
    AppendStage(t, sizeof t, 0, "BGND_Pit", 12, 37);
    Same(t, "BGND_Pit, stage table entry 12 of 37", "a stage and its table entry");
    t[0] = 0;
    AppendSession(t, sizeof t, 0, true, 0, "BGND_First", 37);
    Same(t, "session +0x4C30 is 0 (stage table entry 0 of 37 is BGND_First)", "index 0 is an entry like any other");
}

void Repeats() {
    Check(CountedCall(1) && CountedCall(2) && CountedCall(3) && !CountedCall(4) && !CountedCall(9) && CountedCall(10) && !CountedCall(11) &&
              CountedCall(100) && CountedCall(1000) && CountedCall(10000) && CountedCall(100000) && !CountedCall(1000000),
          "calls 1-3 and the powers of ten are counted calls");
    Check(TextHash("a") != TextHash("b") && TextHash("index 7") == TextHash("index 7") && TextHash("") == 2166136261u, "the text hash");
    const uint32_t v1[2] = {7, 1}, v2[2] = {7, 2};
    Check(BytesHash("index 7", 7) == TextHash("index 7") && BytesHash(v1, sizeof v1) != BytesHash(v2, sizeof v2) &&
              BytesHash("7", 1, BytesHash("index ", 6)) == TextHash("index 7") && BytesHash(v1, 0, 5) == 5,
          "the byte hash: the same function, continued over pieces");
    Repeat r;
    Check(r.Decide(1, 11) == 1 && r.Decide(2, 11) == 1 && r.Decide(3, 11) == 1, "the first three calls get a line");
    int lines = 0;
    for (uint32_t n = 4; n <= 9; ++n) lines += r.Decide(n, 11) != 0;
    Check(lines == 0, "calls 4-9 with the same text get none");
    Check(r.Decide(10, 11) == 1, "the 10th call gets one");
    Check(r.Decide(11, 22) == 1, "a call with another text gets one");
    Check(r.Decide(12, 22) == 0, "the same text again does not");
    Check(r.Decide(13, 11) == 1, "back to the first text: a line again");
    // A native whose text changes on every call: kChangeLines lines, the last one marked, then quiet until a counted call.
    Repeat p;
    lines = 0;
    int marked = 0, markedAt = 0;
    for (uint32_t n = 1; n <= 99; ++n) {
        const int d = p.Decide(n, 1000 + n);
        lines += d != 0;
        if (d == 2) {
            ++marked;
            markedAt = (int)n;
        }
    }
    Check(lines == 4 + (int)kChangeLines && marked == 1, "a text that changes every call: the counted calls and kChangeLines more");
    Check(markedAt == 4 + (int)kChangeLines, "the last change line is the marked one (calls 1-3 and 10 are counted calls)");
    Check(p.Decide(100, 7) == 1 && p.Decide(101, 8) == 0, "after that only counted calls");
    p.Reset();
    Check(p.Decide(500, 8) == 1 && p.changeLines == 1, "a new timeline starts over");
    // A first call that is not a counted call (the native became traced in mid-timeline) is a change.
    Repeat q;
    Check(q.Decide(57, 5) == 1 && q.Decide(58, 5) == 0, "the first text seen is a change");
}

void Others() {
    uint32_t counts[kCountedIds] = {};
    char t[256] = "x";
    int n = AppendOthers(t, sizeof t, 1, counts);
    Check(n == 1 && !strcmp(t, "x"), "no other message: nothing appended");
    counts[3] = 1200;
    counts[13] = 2;
    counts[kCountedIds - 1] = 5;
    n = AppendOthers(t, sizeof t, 1, counts);
    Same(t, "x | other messages received since the last such list: 3 x1200, 13 x2, 32+ x5", "the list of counted messages");
    Check(n == (int)strlen(t), "AppendOthers returns the length");
}

void Clean() {
    char t[8];
    CleanText(t, sizeof t, "BGND_Pit_Day", 64);
    Same(t, "BGND_Pi", "text is cut to the buffer");
    const char raw[] = {'a', 1, 'b', (char)0xE9, 'c', 0, 'z'};
    CleanText(t, sizeof t, raw, sizeof raw);
    Same(t, "a?b?c", "bytes that are not printable ASCII become '?', and the text ends at its terminator");
    CleanText(t, sizeof t, "abcdef", 3);
    Same(t, "abc", "no more than the given length is read");
    char one[1] = {'!'};
    CleanText(one, sizeof one, "abc", 3);
    Check(one[0] == 0, "a one-byte buffer holds the terminator");
}

// Every text function into buffers of every small size: terminated, and nothing written past the end.
void Bounded() {
    uint8_t settings[16] = {};
    Le(settings + 8, 4000000000u);
    const uint8_t ready[] = {'N', 'a', 'm', 'e', 0, 1, 2, 3};
    uint32_t counts[kCountedIds];
    for (uint32_t& c : counts) c = 4000000000u;
    constexpr int kCalls = 14;
    NativeCall calls[kCalls];
    const uint32_t ids[kCalls] = {670, 671, 671, 672, 1178, 1178, 1179, 1259, 1375, 1375, 1224, 1225, 1226, 1227};
    for (int i = 0; i < kCalls; ++i) {
        NativeCall& c = calls[i];
        c.id = ids[i];
        c.argsRead = c.resultRead = c.sessionRead = true;
        c.arg[0] = c.arg[1] = 10;
        c.arg[2] = c.arg[3] = sizeof ready;
        c.result = c.sessionIndex = 4000000000u;
        c.text[0] = "a_rather_long_operand_text_a_rather_long_operand_text";
        c.text[1] = i ? nullptr : "second";
        c.text[2] = "third";
        c.stage = "BGND_SomeStageName";
        c.stageIndex = i == 2 ? -1 : 33;
        c.stageCount = 4000000000u;
        c.sessionStage = i == 5 ? nullptr : "BGND_AnotherStageName";
        c.settingsKnown = true;
        c.settingsSent = i == 9;
        c.payload = ready;
        c.payloadLen = sizeof ready;
    }
    bool ok = true;
    for (size_t cap = 1; cap <= 96 && ok; ++cap) {
        for (int f = 0; f < 6 + kCalls && ok; ++f) {
            char buf[128];
            memset(buf, 0x5A, sizeof buf);
            int n = 0;
            if (f == 0) n = AppendGameSettings(buf, cap, 0, settings, 16);
            else if (f == 1) n = AppendReadyState(buf, cap, 0, ready, sizeof ready);
            else if (f == 2) n = AppendOthers(buf, cap, 0, counts);
            else if (f == 3) n = AppendHex(buf, cap, 0, settings, 16, 16);
            else if (f == 4) n = AppendStage(buf, cap, 0, "BGND_SomeStageName", 33, 4000000000u);
            else if (f == 5) n = AppendSession(buf, cap, 0, true, 4000000000u, "BGND_AnotherStageName", 4000000000u);
            else n = AppendNative(buf, cap, 0, calls[f - 6]);
            bool tail = true;
            for (size_t i = cap; i < sizeof buf; ++i) tail = tail && buf[i] == 0x5A;
            const bool ended = memchr(buf, 0, cap) != nullptr;
            if (!tail || !ended || n < 0 || (size_t)n >= cap) {
                ok = false;
                printf("FAIL  buffer of %u bytes, text function %d: past the end %d, terminated %d, length %d\n", (unsigned)cap, f, !tail, ended, n);
            }
        }
    }
    Check(ok, "every text function stays inside buffers of 1 to 96 bytes and ends its text");
}
}  // namespace

int main() {
    Names();
    Hex();
    Settings();
    Ready();
    Natives();
    Repeats();
    Others();
    Clean();
    Bounded();
    printf("%d checks, %d failed\n", gChecks, gFailed);
    if (!gFailed) printf("ALL CHECKS PASSED\n");
    return gFailed ? 1 : 0;
}
