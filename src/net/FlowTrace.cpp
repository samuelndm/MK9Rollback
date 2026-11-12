#include "FlowTrace.h"

#include <windows.h>

#include <cstring>

#include "../common/StrAppend.h"
#include "../debug/CrashLog.h"
#include "../debug/Logger.h"
#include "../game/MK9Game.h"
#include "../hook/CodePatch.h"
#include "../hook/ObjectHooks.h"
#include "../rollback/RollbackManager.h"
#include "../rollback/ScriptRng.h"
#include "FlowDecode.h"
#include "OnlineMode.h"

// Addresses, all read in the game's code (..\analysis\mkke_text.asm, 2026-11-12):
//  - online manager [0x00E924F0] (0x0081A4D0 returns it; 0 offline); its session = [manager + 0x1F4] (0x0093C7D0 is
//    "mov eax,[ecx+0x1F4]; ret"); the session's +0x4C30 is the "SelectedBackgroundIndex" of the game's own debug line
//    (script native 1178 writes it through 0x0081C150, 1179 reads it through 0x0081C180, message 9 carries it).
//  - 0x0087F850 "MKOnline_SendGameSettings" (reached only from script native 1375, a jmp at 0x008C1170): after its two
//    checks (0x0081C240, 0x0081C2C0) it builds message 9 on its stack and fills it at 0x0087F94C,
//        push 0x10 / push 1 / lea edx,[ebp-0x103C] / push edx / lea ecx,[ebp-0x102C] / call 0x004EDA10
//    (thiscall on the message, the callee pops the three arguments), then hands it to every connection of the session.
//  - MKOnlineSession's HandleIncomingMessage 0x00886EF0 (thiscall: session, connection, message): the id byte is
//    looked at and kept in [ebp-0x1134]; id 7 goes its own way; for every other id 0x00887028 takes the byte out,
//        push 1 / push 1 / lea ecx,[ebp-0x112D] / push ecx / mov ecx,esi / call 0x004EDAA0
//    and the jump table 0x00887374 follows (9 -> 0x0087F9E0 with the 16 bytes; 10 = a name and a byte). A message
//    object: +0x08 data, +0x0C size, +0x14 read position (0x00886FC9: the rest of a message is data + position,
//    size - position).
//  - the fight's stage name: 64 characters at [0x00E89B8C] + 0x3C (0x00811DD0 copies it there).
// Both patches replace one "call rel32" by a call of a stub here, the way RngTrace's line-check taps and ScriptRng's
// pick hooks do; CodePatch refuses a site whose five bytes are not the expected call.
//
// What only a real online match can show (docs\feature_parity_plan.md 4.2): that both taps fire and with which
// values, which thread the session's handler runs on (the received lines say it), whether a 1 vs 1 room sends
// message 10 at all, and whether the selected background index is the stage table's entry number.

extern "C" {
uint32_t gMK9R_MsgWriteFn = 0x004EDA10;  // the stubs' way back into the game; no register is used for the jump
uint32_t gMK9R_MsgReadFn = 0x004EDAA0;
void __cdecl MK9R_OnSettingsSend(const uint8_t* bytes);
void __cdecl MK9R_OnMsgRecv(uint32_t id, uint32_t msg);
void MK9R_HkSettingsSend();
void MK9R_HkMsgRecv();
}
// MK9R_HkSettingsSend: the message's 16 bytes are the first stack argument (36 bytes of saved flags and registers
// above it); then the game's own call, as a jump with the stack as the game left it.
// MK9R_HkMsgRecv: the game's call first, with copies of its three arguments (the callee pops them; ecx is the game's);
// then the id the function keeps at [ebp-0x1134] and the message (esi); the callee's result in eax is kept.
asm(R"(
    .text
    .globl _MK9R_HkSettingsSend
_MK9R_HkSettingsSend:
    pushfl
    pushal
    pushl 40(%esp)
    call _MK9R_OnSettingsSend
    addl $4, %esp
    popal
    popfl
    jmp *_gMK9R_MsgWriteFn
    .globl _MK9R_HkMsgRecv
_MK9R_HkMsgRecv:
    pushl 12(%esp)
    pushl 12(%esp)
    pushl 12(%esp)
    call *_gMK9R_MsgReadFn
    pushfl
    pushal
    pushl %esi
    pushl -0x1134(%ebp)
    call _MK9R_OnMsgRecv
    addl $8, %esp
    popal
    popfl
    ret $12
)");

namespace {
constexpr uintptr_t kOnlineManager = 0x00E924F0;
constexpr uint32_t kManagerSession = 0x1F4;
constexpr uint32_t kSessionBackground = 0x4C30;
constexpr uintptr_t kFightInfo = 0x00E89B8C;
constexpr uint32_t kFightStageName = 0x3C;
constexpr uintptr_t kSettingsSendSite = 0x0087F94C, kMsgRecvSite = 0x00887028;

// Every read of the game's memory here is guarded (CrashLog::SafeRead32: a fault fails the read), also on a thread
// that is not the game's. A byte comes from the aligned word that holds it, so no page is touched that the range
// itself does not lie in.
bool Rd(uintptr_t a, uint32_t* v) { return a >= 0x10000 && CrashLog::SafeRead32(a, v); }
struct ByteReader {
    uint32_t word = 0;
    uintptr_t at = 1;  // the address of the word held; 1: none
    bool Get(uintptr_t a, uint8_t* b) {
        const uintptr_t base = a & ~(uintptr_t)3;
        if (base != at) {
            if (!Rd(base, &word)) return false;
            at = base;
        }
        *b = (uint8_t)(word >> (8 * (a & 3)));
        return true;
    }
};
bool ReadBytes(uintptr_t p, uint8_t* out, size_t len) {  // all of them or false
    if (p < 0x10000 || p + len < p) return false;
    ByteReader r;
    for (size_t i = 0; i < len; ++i)
        if (!r.Get(p + i, out + i)) return false;
    return true;
}
// A zero-terminated text of the game's, cleaned for a log line (FlowDecode::CleanText), at most 64 characters of it.
// False when not even its first byte can be read.
bool ReadText(uintptr_t p, char* out, size_t cap) {
    out[0] = 0;
    if (p < 0x10000) return false;
    char raw[65];
    size_t n = 0;
    ByteReader r;
    for (; n < sizeof raw - 1; ++n) {
        uint8_t b = 0;
        if (!r.Get(p + n, &b)) {
            if (!n) return false;
            break;
        }
        if (!b) break;
        raw[n] = (char)b;
    }
    FlowDecode::CleanText(out, cap, raw, n);
    return true;
}

// In a one-on-one online match: the game's online session is connected, and the match is one the online mode would
// take (Online::OneOnOne). "Connected" is MKOnlineSession's own flag at +0x4BB0: its setter 0x0090F6E0 logs
// "MKOnlineSession::CONNECTED" or "MKOnlineSession::DISCONNECTED" and stores its argument there (0x0090F753); the
// constructor (0x00918342) and the reset (0x00919691, "DISCONNECTED") store zero; and every sender of a session
// message first asks 0x0090FB00, which is "[+0x4BB0] == 1" (the four generic senders at 0x0081BD45, 0x0081BE15,
// 0x0081BF06, 0x00820C95, and 0x0081C240 on native 1375's way at 0x0081C25B). The match description alone cannot
// tell: it reads all zero offline and in a room joined by invite alike, so the first build of this trace (rc32)
// wrote its lines in every offline and attract fight (its gate's logs, 2026-11-12). Not seen in a real online match
// yet: that the flag is 1 from the lobby to the results screen is read from the senders' check, not observed.
constexpr uint32_t kSessionConnected = 0x4BB0;
bool InMatch() {
    uint32_t mgr = 0, session = 0, connected = 0;
    return Rd(kOnlineManager, &mgr) && mgr && Rd(mgr + kManagerSession, &session) && session &&
           Rd(session + kSessionConnected, &connected) && connected == 1 && Online::OneOnOne();
}
bool SessionIndex(uint32_t* index) {
    uint32_t mgr = 0, session = 0;
    return Rd(kOnlineManager, &mgr) && mgr && Rd(mgr + kManagerSession, &session) && session && Rd(session + kSessionBackground, index);
}
bool FightStage(char* out, size_t cap) {
    uint32_t gi = 0;
    out[0] = 0;
    return Rd(kFightInfo, &gi) && gi && ReadText(gi + kFightStageName, out, cap);
}
void GameNow(uint32_t* gf, uint32_t* state) {
    *gf = *state = 0xFFFFFFFF;
    uint32_t gi = 0;
    if (!Rd(MK9Game::Get().gameInfoPtr, &gi) || !gi) return;
    Rd(gi + MK9Game::kGameInfoGameFrame, gf);
    Rd(gi + 4, state);
}

// The stage table's side of a line, looked up only when a line is written (ScriptRng's looks probe each entry).
struct StageLook {
    uint32_t count = 0;
    int fightIndex = -1;         // the fight's stage in the table
    char entry[65] = "";         // the entry of the session's index
    const char* entryName = nullptr;
    void Fill(const char* fightStage, bool sessionRead, uint32_t sessionIndex) {
        count = ScriptRng::StageCount();
        if (fightStage) fightIndex = ScriptRng::StageIndex(fightStage);
        if (!sessionRead) return;
        const char* e = ScriptRng::StageAt(sessionIndex);
        if (e && ReadText(reinterpret_cast<uintptr_t>(e), entry, sizeof entry)) entryName = entry;
    }
};

// ---- messages ----
volatile LONG gOther[FlowDecode::kCountedIds];  // received ids that are only counted, since the last line that listed them
volatile LONG gInReplay = 0;                    // messages that arrived while a rollback replayed frames (not looked at)
volatile LONG gSettingsBuilt = 0;               // message 9 was filled since native 1375's last line
// Which messages of an id get a line: as for the natives (calls 1-3 of a timeline, the 10th, 100th, ..., and a change
// of the bytes, FlowDecode::Repeat). The session's handler may run on another thread than the game's: under a lock.
SRWLOCK gMsgLock = SRWLOCK_INIT;
struct MsgSeen {
    uint32_t count = 0;
    FlowDecode::Repeat repeat;
} gMsgSeen[3];  // sent 9, received 9, received 10
int MsgDecide(int which, uint32_t hash, uint32_t* number) {
    AcquireSRWLockExclusive(&gMsgLock);
    *number = ++gMsgSeen[which].count;
    const int d = gMsgSeen[which].repeat.Decide(*number, hash);
    ReleaseSRWLockExclusive(&gMsgLock);
    return d;
}
int AppendTail(char* out, size_t cap, int n, int decision) {
    if (decision == 2) n = AppendF(out, cap, n, " (the last line of this timeline for a change of these values; they are still counted)");
    uint32_t counts[FlowDecode::kCountedIds];
    for (int i = 0; i < FlowDecode::kCountedIds; ++i) counts[i] = (uint32_t)InterlockedExchange(&gOther[i], 0);
    n = FlowDecode::AppendOthers(out, cap, n, counts);
    const LONG replayed = InterlockedExchange(&gInReplay, 0);
    if (replayed) n = AppendF(out, cap, n, " | %ld message(s) arrived while a rollback replayed frames and were not looked at", replayed);
    return n;
}

// ---- natives ----
struct TracedNative {
    uint16_t id;
    uint8_t operands;  // the operand words the native itself reads ([executor + 0x3C] + 4, + 8, ...)
};
constexpr TracedNative kTraced[] = {{670, 2}, {671, 1}, {672, 3}, {1178, 1}, {1179, 0}, {1224, 3},
                                    {1225, 3}, {1226, 4}, {1227, 3}, {1259, 1}, {1375, 0}};
constexpr int kTracedCount = sizeof(kTraced) / sizeof(kTraced[0]);
uint32_t gCalls[kTracedCount];  // game thread; per timeline
FlowDecode::Repeat gRepeat[kTracedCount];
// A script's sends of message 9 or 10 are decided apart from its other sends through the same native (1224-1227): a
// native that sends something else every tick would use up the lines for changed values before a message 10 came.
uint32_t gShownSends[4];
FlowDecode::Repeat gShownRepeat[4];
int Slot(uint32_t id) {
    for (int i = 0; i < kTracedCount; ++i)
        if (kTraced[i].id == id) return i;
    return -1;
}

bool gSendTapped = false, gRecvTapped = false, gInstallTried = false;
bool PatchCall(uintptr_t site, uintptr_t oldTarget, void (*stub)(), const char* name) {
    uint8_t expected[5] = {0xE8}, replacement[5] = {0xE8};
    const int32_t relOld = (int32_t)(oldTarget - (site + 5));
    const int32_t relNew = (int32_t)(reinterpret_cast<uintptr_t>(stub) - (site + 5));
    memcpy(expected + 1, &relOld, 4);
    memcpy(replacement + 1, &relNew, 4);
    return CodePatch::Write(site, expected, replacement, 5, name);
}
}  // namespace

extern "C" void __cdecl MK9R_OnSettingsSend(const uint8_t* bytes) {
    if (Rollback::gIsResimulating) return;  // a replayed call of native 1375: nothing read, nothing noted
    InterlockedExchange(&gSettingsBuilt, 1);
    if (!InMatch()) return;
    uint8_t b[FlowDecode::kGameSettingsBytes];
    const bool read = ReadBytes(reinterpret_cast<uintptr_t>(bytes), b, sizeof b);
    uint32_t index = 0;
    const bool sessionRead = SessionIndex(&index);
    uint32_t h = FlowDecode::BytesHash(&index, sizeof index, sessionRead ? 1u : 2u);
    if (read) h = FlowDecode::BytesHash(b, sizeof b, h);
    uint32_t number = 0;
    const int decision = MsgDecide(0, h, &number);
    if (!decision) return;
    uint32_t gf = 0, state = 0;
    GameNow(&gf, &state);
    StageLook look;
    look.Fill(nullptr, sessionRead, index);
    char line[900];
    int n = AppendF(line, sizeof line, 0, "ONLINEMSG send 9 GameSettings #%u of this timeline at gf %u (game state %u): ", number, gf, state);
    if (read) n = FlowDecode::AppendGameSettings(line, sizeof line, n, b, sizeof b);
    else n = AppendF(line, sizeof line, n, "16 bytes, not readable");
    n = AppendF(line, sizeof line, n, "; ");
    n = FlowDecode::AppendSession(line, sizeof line, n, sessionRead, index, look.entryName, look.count);
    AppendTail(line, sizeof line, n, decision);
    Logger::Info("%s", line);
}

extern "C" void __cdecl MK9R_OnMsgRecv(uint32_t id, uint32_t msg) {
    if (Rollback::gIsResimulating) {  // nothing is read while a rollback replays frames, whichever thread this is
        InterlockedIncrement(&gInReplay);
        return;
    }
    if (!InMatch()) return;
    if (!FlowDecode::Shown(id)) {
        InterlockedIncrement(&gOther[id < (uint32_t)FlowDecode::kCountedIds - 1 ? id : FlowDecode::kCountedIds - 1]);
        return;
    }
    // What is left of the message after its id byte.
    uint8_t b[320];
    uint32_t data = 0, size = 0, pos = 0;
    const bool shape = Rd(msg + 0x08, &data) && Rd(msg + 0x0C, &size) && Rd(msg + 0x14, &pos) && pos <= size && size <= 0x10000;
    const uint32_t len = shape ? size - pos : 0;
    const uint32_t have = len < sizeof b ? len : (uint32_t)sizeof b;
    const bool read = shape && ReadBytes(data + pos, b, have);
    uint32_t index = 0;
    const bool sessionRead = id == 9 && SessionIndex(&index);
    uint32_t h = FlowDecode::BytesHash(&len, sizeof len, id);
    if (read) h = FlowDecode::BytesHash(b, have, h);
    if (id == 9) h = FlowDecode::BytesHash(&index, sizeof index, h);
    uint32_t number = 0;
    const int decision = MsgDecide(id == 9 ? 1 : 2, h, &number);
    if (!decision) return;
    uint32_t gf = 0, state = 0;
    GameNow(&gf, &state);
    char line[900];
    int n = AppendF(line, sizeof line, 0, "ONLINEMSG received %u %s #%u of this timeline at gf %u (game state %u, %s): ", id,
                    FlowDecode::MessageName(id), number, gf, state,
                    GetCurrentThreadId() == ObjectHooks::gGameThread ? "on the game thread" : "on another thread than the game's");
    if (!shape) n = AppendF(line, sizeof line, n, "the message object is not readable");
    else if (!read) n = AppendF(line, sizeof line, n, "%u bytes, not readable", len);
    else if (have < len) n = AppendF(line, sizeof line, n, "%u bytes, more than this trace looks at, not decoded", len);
    else n = FlowDecode::AppendPayload(line, sizeof line, n, id, b, have);
    if (id == 9) {
        // The game's receiver 0x0087F9E0 runs after this: it stores the message's index into the session.
        StageLook look;
        look.Fill(nullptr, sessionRead, index);
        n = AppendF(line, sizeof line, n, "; before the game takes it: ");
        n = FlowDecode::AppendSession(line, sizeof line, n, sessionRead, index, look.entryName, look.count);
    }
    AppendTail(line, sizeof line, n, decision);
    Logger::Info("%s", line);
}

namespace FlowTrace {

void Install() {
    if (!gSendTapped)
        gSendTapped = PatchCall(kSettingsSendSite, gMK9R_MsgWriteFn, &MK9R_HkSettingsSend, "GameSettings message fill 0x0087F94C (stage trace)");
    if (!gRecvTapped)
        gRecvTapped = PatchCall(kMsgRecvSite, gMK9R_MsgReadFn, &MK9R_HkMsgRecv, "session message id read 0x00887028 (stage trace)");
    if (gInstallTried) return;
    gInstallTried = true;
    Logger::Info("FLOWTRACE: stage and rematch trace lines in online one-on-one matches (ONLINEFLOW with values, ONLINEMSG, ONLINESTAGE); "
                 "message 9 as it is sent: %s; messages as they are received: %s",
                 gSendTapped ? "tapped" : "NOT tapped (code not as expected: no 'ONLINEMSG send' lines)",
                 gRecvTapped ? "tapped" : "NOT tapped (code not as expected: no 'ONLINEMSG received' lines)");
}

bool OnNative(uint32_t id, uint32_t fn, uint32_t exec, uint32_t gf, uint32_t callNo) {
    const int slot = Slot(id);
    if (slot < 0 || Rollback::gIsResimulating || !InMatch()) return false;
    if (!callNo) callNo = ++gCalls[slot];
    FlowDecode::NativeCall c;
    c.id = id;
    uint32_t h = FlowDecode::BytesHash(&id, sizeof id);
    // The operand words, read where the native read them: the call opcode 0x004596D0 moved the executor's operand
    // pointer past the native's own address before it jumped, so operand k is at [executor + 0x3C] + 4k either way.
    uint32_t ops = 0;
    c.argsRead = Rd(exec + 0x3C, &ops) && ops;
    for (int k = 0; c.argsRead && k < kTraced[slot].operands; ++k) c.argsRead = Rd(ops + 4 + 4 * k, &c.arg[k]);
    c.resultRead = Rd(exec + 0x1C, &c.result);
    char text[3][65];
    char stage[65];
    uint8_t payload[320];
    switch (id) {
        case 670:
        case 671:
        case 672:
            for (int k = 0; c.argsRead && k < kTraced[slot].operands; ++k) {
                if (ReadText(c.arg[k], text[k], sizeof text[k])) c.text[k] = text[k];
                h = FlowDecode::BytesHash(text[k], strlen(text[k]) + 1, h);
            }
            break;
        case 1178:
        case 1259:
            h = FlowDecode::BytesHash(&c.arg[0], 4, h);
            break;
        case 1375:
            c.settingsKnown = gSendTapped;
            c.settingsSent = InterlockedExchange(&gSettingsBuilt, 0) != 0;
            h = FlowDecode::BytesHash(&c.settingsSent, 1, h);
            break;
        default: break;
    }
    FlowDecode::Repeat* repeat = &gRepeat[slot];
    uint32_t decideNo = callNo;
    if (FlowDecode::SendNative(id) && c.argsRead) {
        const uint32_t msgId = FlowDecode::SendMessageId(c), len = FlowDecode::SendLength(c);
        h = FlowDecode::BytesHash(&msgId, 4, h);
        h = FlowDecode::BytesHash(&len, 4, h);
        if (FlowDecode::Shown(msgId)) {
            repeat = &gShownRepeat[id - 1224];
            decideNo = ++gShownSends[id - 1224];
            // The data operand's bytes for the two messages that are shown; any other message's are not read.
            if (len <= sizeof payload && ReadBytes(c.arg[id == 1226 ? 2 : 1], payload, len)) {
                c.payload = payload;
                c.payloadLen = len;
                h = FlowDecode::BytesHash(payload, len, h);
            }
        }
    }
    if (id == 1179 || id == 1259 || FlowDecode::SendNative(id)) h = FlowDecode::BytesHash(&c.result, 4, h);
    if (id == 1178 || id == 1179 || id == 1375) {
        c.sessionRead = SessionIndex(&c.sessionIndex);
        h = FlowDecode::BytesHash(&c.sessionIndex, 4, h);
    }
    if (id == 671 || id == 672 || id == 1179) {
        if (FightStage(stage, sizeof stage)) c.stage = stage;
        h = FlowDecode::BytesHash(stage, strlen(stage) + 1, h);
    }
    const int decision = repeat->Decide(decideNo, h);
    if (!decision) return true;
    StageLook look;
    look.Fill(c.stage, c.sessionRead, c.sessionIndex);
    c.stageCount = look.count;
    c.stageIndex = look.fightIndex;
    c.sessionStage = look.entryName;
    char line[1100];
    int n = AppendF(line, sizeof line, 0, "ONLINEFLOW id %u native %08X call #%u of this timeline at gf %u (%s): ", id, fn, callNo, gf,
                    Rollback::GgpoActive() ? "in a GGPO session" : "no session");
    n = FlowDecode::AppendNative(line, sizeof line, n, c);
    if (decision == 2) AppendF(line, sizeof line, n, " (the last line of this timeline for a change of this native's values; calls are still counted)");
    Logger::Info("%s", line);
    return true;
}

void OnFightStart(uint32_t gf) {
    if (Rollback::gIsResimulating || !InMatch()) return;
    char stage[65];
    const bool stageRead = FightStage(stage, sizeof stage);
    uint32_t index = 0;
    const bool sessionRead = SessionIndex(&index);
    StageLook look;
    look.Fill(stageRead ? stage : nullptr, sessionRead, index);
    char line[900];
    int n = AppendF(line, sizeof line, 0, "ONLINESTAGE at gf %u: the fight starts on ", gf);
    n = FlowDecode::AppendStage(line, sizeof line, n, stageRead ? stage : nullptr, look.fightIndex, look.count);
    n = AppendF(line, sizeof line, n, "; ");
    n = FlowDecode::AppendSession(line, sizeof line, n, sessionRead, index, look.entryName, look.count);
    AppendTail(line, sizeof line, n, 0);
    Logger::Info("%s", line);
}

void NewTimeline() {
    memset(gCalls, 0, sizeof gCalls);
    memset(gShownSends, 0, sizeof gShownSends);
    for (FlowDecode::Repeat& r : gRepeat) r.Reset();
    for (FlowDecode::Repeat& r : gShownRepeat) r.Reset();
    AcquireSRWLockExclusive(&gMsgLock);
    for (MsgSeen& m : gMsgSeen) m = MsgSeen{};
    ReleaseSRWLockExclusive(&gMsgLock);
}

}  // namespace FlowTrace
