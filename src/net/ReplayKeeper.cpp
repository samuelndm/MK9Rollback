#include "ReplayKeeper.h"

#include <windows.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwchar>

#include "../debug/CrashLog.h"
#include "../debug/Logger.h"
#include "../game/MK9Game.h"
#include "../rollback/RollbackManager.h"
#include "../rollback/ScriptRng.h"
#include "EnvFingerprint.h"
#include "FlowTrace.h"
#include "OnlineMode.h"
#include "Refight.h"

// (ReplayFile.h's recorder, file and comparison are the host-tested part; this file reads the game, writes and reads the
// files and logs. ReplayKeeper.h says what replays are.)
namespace ReplayKeeper {
namespace {
using ReplayFile::Replay;

constexpr uint32_t kMinFrames = 60;  // a session shorter than a second is not kept
wchar_t gDir[MAX_PATH] = L"";         // <the DLL's folder>\replays
bool gRecordOn = true;                // 'replay record 0|1'
bool gLike = true;                    // 'replay like 0|1'

// The session being recorded, or waiting to be written (static: set aside once, never allocated in a fight).
Replay gRec;
bool gRecOn = false, gRecPending = false;
uint32_t gWritten = 0, gWriteFailures = 0, gNotKept = 0;

// The playback's recording (it must stay while the playback runs).
Replay gPlay;
char gPlayFile[MAX_PATH] = "";
bool gStageForced = false;

// The game is in an online match: MK9's online session says "connected" (FlowTrace.cpp's test, the flag every sender of
// a session message asks for; its trace lines filled the logs of the online evening of 2026-11-16). Online::OneOnOne
// cannot tell: the match description reads all zero offline and in a room joined by invite alike, so it answers yes in
// an offline game (the first look at replays, 2026-11-17 09:34, was refused by it at the title screen).
constexpr uintptr_t kOnlineManager = 0x00E924F0;
constexpr uint32_t kManagerSession = 0x1F4, kSessionConnected = 0x4BB0;
bool OnlineConnected() {
    uint32_t mgr = 0, s = 0, c = 0;
    return CrashLog::SafeRead32(kOnlineManager, &mgr) && mgr >= 0x10000 && CrashLog::SafeRead32(mgr + kManagerSession, &s) && s >= 0x10000 &&
           CrashLog::SafeRead32(s + kSessionConnected, &c) && c == 1;
}

uint32_t GameInfoField(uint32_t off) {
    const uint32_t gi = *reinterpret_cast<const volatile uint32_t*>(MK9Game::Get().gameInfoPtr);
    return gi ? *reinterpret_cast<const volatile uint32_t*>(gi + off) : 0;
}

void LocalTime(char* out, size_t n) {
    SYSTEMTIME t;
    GetLocalTime(&t);
    snprintf(out, n, "%04u-%02u-%02u %02u:%02u:%02u", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
}

// A fighter's part of a file name: "CHAR_JOHNNYCAGE_B" -> "JOHNNYCAGE_B" (letters, digits and '_' only).
void NamePart(const char* fighter, char* out, size_t n) {
    size_t k = 0;
    const char* s = fighter && !_strnicmp(fighter, "CHAR_", 5) ? fighter + 5 : fighter;
    for (; s && *s && k + 1 < n && k < 24; ++s)
        if ((*s >= 'A' && *s <= 'Z') || (*s >= 'a' && *s <= 'z') || (*s >= '0' && *s <= '9') || *s == '_') out[k++] = *s;
    if (!k && n > 1) out[k++] = 'X';
    out[k] = 0;
}

const char* ColoursText(const Replay& r, char* out, size_t n) {
    if (!(r.flags & ReplayFile::kColoursKnown)) snprintf(out, n, "not known");
    else snprintf(out, n, "%u and %u", r.setup.colours[0], r.setup.colours[1]);
    return out;
}
const char* KodesText(const Replay& r, char* out, size_t n) {
    if (!(r.flags & ReplayFile::kKodesKnown)) {
        snprintf(out, n, "not known");
        return out;
    }
    char a[6], b[6];
    ReplayFile::KodeText(r.setup.kodes[0], a);
    ReplayFile::KodeText(r.setup.kodes[1], b);
    snprintf(out, n, "player 1 %s, player 2 %s", a, b);
    return out;
}

bool FileSink(void* ctx, const void* p, size_t n) { return fwrite(p, 1, n, static_cast<FILE*>(ctx)) == n; }

// The file, on the writer thread (StartWrite): gRec is not touched by anyone else until gWriting is 0 again.
void WriteNow(const char* how) {
    const Replay& r = gRec;
    if (!gDir[0]) return;
    CreateDirectoryW(gDir, nullptr);  // (one that exists stays as it is)
    // <date>_<time>_<P1>_vs_<P2>.mk9r, the time the session began ("2026-11-16 21:34:44" -> "2026-11-16_213444").
    char stamp[24] = "", a[32], b[32], base[120];
    const char* t = r.setup.time;
    if (strlen(t) == 19) snprintf(stamp, sizeof stamp, "%.10s_%.2s%.2s%.2s", t, t + 11, t + 14, t + 17);
    else snprintf(stamp, sizeof stamp, "replay");
    NamePart(r.setup.p1, a, sizeof a);
    NamePart(r.setup.p2, b, sizeof b);
    snprintf(base, sizeof base, "%s_%s_vs_%s", stamp, a, b);
    wchar_t path[MAX_PATH], part[MAX_PATH];
    swprintf(path, MAX_PATH, L"%ls\\%hs.mk9r", gDir, base);
    for (int k = 2; GetFileAttributesW(path) != INVALID_FILE_ATTRIBUTES && k < 100; ++k) swprintf(path, MAX_PATH, L"%ls\\%hs_%d.mk9r", gDir, base, k);
    swprintf(part, MAX_PATH, L"%ls.part", path);
    FILE* f = _wfopen(part, L"wb");
    bool ok = f != nullptr;
    size_t bytes = 0;
    if (f) {
        ReplayFile::Sink s;
        s.ctx = f;
        s.write = &FileSink;
        ok = ReplayFile::Write(r, s);
        bytes = s.bytes;
        ok = (fclose(f) == 0) && ok;
        ok = ok && MoveFileExW(part, path, 0);
        if (!ok) _wremove(part);
    }
    if (!ok) {
        const DWORD e = GetLastError();
        if (gWriteFailures++ < 4) Logger::Info("REPLAY: %ls could not be written (error %lu): this session's replay is lost", path, e);
        return;
    }
    ++gWritten;
    char colours[24], kodes[48];
    Logger::Info("REPLAY written%s: %ls (%u frames, %.1f s, %u bytes; %u frame(s) without recorded inputs, %u checksums, %u state samples; "
                 "%s vs %s on %s, colours %s, Kombat Kodes %s; this game played side %u; the session stopped: %s%s)",
                 how, path, r.frames, r.frames / 60.0, (unsigned)bytes, ReplayFile::MissingInputs(r), ReplayFile::Checksums(r), r.states,
                 r.setup.p1, r.setup.p2, r.setup.arena, ColoursText(r, colours, sizeof colours), KodesText(r, kodes, sizeof kodes),
                 r.setup.localPlayer, r.stopWhy, (r.flags & ReplayFile::kTruncated) ? "; CUT at the size limit" : "");
}

volatile LONG gWriting = 0;
const char* gWriteHow = "";
DWORD WINAPI WriterThread(LPVOID) {
    WriteNow(gWriteHow);
    InterlockedExchange(&gWriting, 0);
    return 0;
}
// A finished recording to its file, off the game thread: it is written right after a match, while MK9's own netcode
// runs the finisher, and a new file can be held up by a virus scanner. gRec stays untouched meanwhile (RecBegin waits).
void StartWrite(const char* how) {
    gRecPending = false;
    gWriteHow = how;
    InterlockedExchange(&gWriting, 1);
    HANDLE h = CreateThread(nullptr, 0, &WriterThread, nullptr, 0, nullptr);
    if (h) return (void)CloseHandle(h);
    WriteNow(how);  // no thread: here, now
    InterlockedExchange(&gWriting, 0);
}
bool WaitWriter(int ms) {
    for (int i = 0; i < ms / 5 && InterlockedCompareExchange(&gWriting, 0, 0); ++i) Sleep(5);
    return !InterlockedCompareExchange(&gWriting, 0, 0);
}

bool SessionRuns() {
    return Rollback::GgpoActive() || Rollback::NetplayRunning() || Rollback::NetplayStarting() || Rollback::gIsResimulating;
}

// The file into `out`. False: why says why.
bool LoadFile(const wchar_t* path, Replay* out, char* why, size_t n) {
    FILE* f = _wfopen(path, L"rb");
    if (!f) {
        snprintf(why, n, "it could not be opened (error %lu)", GetLastError());
        return false;
    }
    long size = -1;
    if (fseek(f, 0, SEEK_END) == 0) size = ftell(f);
    const size_t most = ReplayFile::FileBytes(ReplayFile::kMaxFrames, ReplayFile::kMaxStates);
    if (size <= 0 || (size_t)size > most || fseek(f, 0, SEEK_SET) != 0) {
        fclose(f);
        snprintf(why, n, "its size (%ld bytes) is not a replay's", size);
        return false;
    }
    uint8_t* data = static_cast<uint8_t*>(malloc((size_t)size));
    const bool read = data && fread(data, 1, (size_t)size, f) == (size_t)size;
    fclose(f);
    const ReplayFile::Parsed p = read ? ReplayFile::Read(data, (size_t)size, out) : ReplayFile::Parsed::kShort;
    free(data);
    if (!read) {
        snprintf(why, n, "it could not be read");
        return false;
    }
    if (p != ReplayFile::Parsed::kOk) {
        snprintf(why, n, "%s", ReplayFile::ParsedText(p));
        return false;
    }
    return true;
}

// The newest .mk9r file of the replays folder (its full path). False: none.
bool Newest(wchar_t* out, size_t n) {
    wchar_t pattern[MAX_PATH];
    swprintf(pattern, MAX_PATH, L"%ls\\*.mk9r", gDir);
    WIN32_FIND_DATAW d;
    HANDLE h = FindFirstFileW(pattern, &d);
    if (h == INVALID_HANDLE_VALUE) return false;
    FILETIME best{};
    bool found = false;
    do {
        if (d.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        if (!found || CompareFileTime(&d.ftLastWriteTime, &best) > 0) {
            best = d.ftLastWriteTime;
            swprintf(out, n, L"%ls\\%ls", gDir, d.cFileName);
            found = true;
        }
    } while (FindNextFileW(h, &d));
    FindClose(h);
    return found;
}

void ReleaseSetup(const char* why) {
    if (gStageForced) {
        ScriptRng::SetMatchStage(-1);
        gStageForced = false;
    }
    Refight::DirectEnd(why);
}

void Arm(const char* arg) {
    // A recording that waits to be written goes to its file first ('replay last' right after a session).
    if (gRecPending && !SessionRuns() && !InterlockedCompareExchange(&gWriting, 0, 0)) StartWrite("");
    WaitWriter(3000);
    if (const char* no = Rollback::PlaybackRefusal()) return Logger::Info("REPLAY not armed: %s", no);
    if (Online::Engaged() || OnlineConnected()) return Logger::Info("REPLAY not armed: an online match is on (a replay plays in an offline game)");
    wchar_t path[MAX_PATH] = L"";
    if (!strcmp(arg, "last")) {
        if (!Newest(path, MAX_PATH)) return Logger::Info("REPLAY not armed: no replay in %ls", gDir);
    } else if (strchr(arg, '\\') || strchr(arg, '/') || strchr(arg, ':')) {
        swprintf(path, MAX_PATH, L"%hs", arg);
    } else {
        const size_t len = strlen(arg);
        const bool ext = len > 5 && !_stricmp(arg + len - 5, ".mk9r");
        swprintf(path, MAX_PATH, L"%ls\\%hs%ls", gDir, arg, ext ? L"" : L".mk9r");
    }
    // Whatever runs now ends first: the recording in memory is about to be replaced.
    Rollback::PlaybackStop("another replay was asked for");
    ReleaseSetup("another replay was asked for");
    char why[160];
    if (!LoadFile(path, &gPlay, why, sizeof why)) return Logger::Info("REPLAY not armed: %ls: %s", path, why);
    snprintf(gPlayFile, sizeof gPlayFile, "%ls", path);
    const Replay& r = gPlay;
    if (!(r.flags & ReplayFile::kFromStart))
        return Logger::Info("REPLAY not armed: %s did not begin at a fight's first frame with the netplay start: it cannot be played from a start", gPlayFile);
    const uint32_t* colours = (r.flags & ReplayFile::kColoursKnown) ? r.setup.colours : nullptr;
    if (!Refight::Direct(r.setup.p1, r.setup.p2, colours, "a replay"))
        return Logger::Info("REPLAY not armed: the fight could not be given its fighters (%s and %s: not the game's names of two fighters, or "
                            "the game's code for setting them is not the one this mod knows)",
                            r.setup.p1, r.setup.p2);
    int stage = ScriptRng::StageIndex(r.setup.arena);
    if (stage < 0 && r.setup.arenaIndex >= 0 && ScriptRng::StageAt((uint32_t)r.setup.arenaIndex)) stage = r.setup.arenaIndex;
    if (stage >= 0 && ScriptRng::SetMatchStage(stage)) {
        ScriptRng::WriteStage((uint32_t)stage);
        gStageForced = true;
    }
    if (!Rollback::PlaybackArm(&gPlay, gLike)) {
        ReleaseSetup("the playback was not armed");
        return;
    }
    char col[24], kodes[48];
    const bool kodesSet = (r.flags & ReplayFile::kKodesKnown) && (r.setup.kodes[0] || r.setup.kodes[1]);
    Logger::Info("REPLAY armed: %s (%s; %u frames = %.1f s, %u of them without recorded inputs, %u checksums, %u state samples%s): %s vs %s on %s "
                 "(stage table entry %d%s), colours %s, Kombat Kodes %s; recorded by side %u, input delay %d, game mode %u; the session stopped: "
                 "%s. The next fight that begins plays it: the next character select is covered and confirmed by the mod (Ladder: pick "
                 "any fighter)%s",
                 gPlayFile, (r.flags & ReplayFile::kRebuilt) ? "rebuilt from the logs" : r.setup.time, r.frames, r.frames / 60.0,
                 ReplayFile::MissingInputs(r), ReplayFile::Checksums(r), r.states, (r.flags & ReplayFile::kTruncated) ? ", CUT" : "", r.setup.p1,
                 r.setup.p2, r.setup.arena[0] ? r.setup.arena : "?", stage, stage < 0 ? ": not in this game's table, the fight keeps its own arena" : "",
                 ColoursText(r, col, sizeof col), KodesText(r, kodes, sizeof kodes), r.setup.localPlayer, (int)r.setup.delay, r.setup.mode, r.stopWhy,
                 kodesSet ? ". NOTE: the match was played with Kombat Kodes, which an offline fight is not given: the playback may go out of step "
                            "where a kode changed the fight"
                          : "");
}

void List() {
    wchar_t pattern[MAX_PATH];
    swprintf(pattern, MAX_PATH, L"%ls\\*.mk9r", gDir);
    WIN32_FIND_DATAW d;
    HANDLE h = FindFirstFileW(pattern, &d);
    if (h == INVALID_HANDLE_VALUE) return Logger::Info("REPLAY list: no replay in %ls", gDir);
    int n = 0;
    do {
        if (d.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        ++n;
        Logger::Info("REPLAY list: %ls (%lu bytes)", d.cFileName, d.nFileSizeLow);
    } while (FindNextFileW(h, &d) && n < 200);
    FindClose(h);
    Logger::Info("REPLAY list: %d file(s) in %ls; 'replay <name>' plays one, 'replay last' the newest", n, gDir);
}
}  // namespace

void SetFolder(const wchar_t* dir) {
    if (!dir || !*dir) return;
    swprintf(gDir, MAX_PATH, L"%ls\\replays", dir);
}

void Tick() {
    if (gRecPending && !SessionRuns() && !InterlockedCompareExchange(&gWriting, 0, 0)) StartWrite("");
    // An armed playback never takes an online fight (it would drive both fighters there): it is dropped once the game
    // is in an online match.
    if (Rollback::PlaybackPending() && !Rollback::PlaybackRunning() && (Online::Engaged() || OnlineConnected()))
        Rollback::PlaybackStop("an online match began: a replay plays in an offline game");
}

void RecBegin(uint32_t localPlayer, uint32_t delay, uint32_t netplayGf, bool fromStart, bool online, float testMeter, float testLife) {
    // (This game is paused at the start: waiting here costs the players nothing.)
    if (gRecPending && WaitWriter(2000)) StartWrite(" (before the next session's recording began)");
    gRecOn = false;
    if (!gRecordOn) return;
    if (!WaitWriter(2000)) {
        Logger::Info("REPLAY: the last replay is still being written after 2 s: this session is not recorded");
        return;
    }
    if (!fromStart) {
        Logger::Info("REPLAY: this session did not begin at a fight's first frame with the netplay start: not recorded");
        return;
    }
    Replay& r = gRec;
    ReplayFile::Clear(r);
    // The arrays the fight fills, touched now (the game is paused here) rather than page by page in the fight.
    memset(r.mask, 0, sizeof r.mask);
    memset(r.cs, 0, sizeof r.cs);
    ReplayFile::Setup& u = r.setup;
    r.flags = ReplayFile::kFromStart | (online ? ReplayFile::kOnline : 0);
    char a[ReplayFile::kText] = "", b[ReplayFile::kText] = "";
    if (ScriptRng::FightPlayers(a, b, sizeof a)) {
        ReplayFile::CopyText(u.p1, sizeof u.p1, a);
        ReplayFile::CopyText(u.p2, sizeof u.p2, b);
    }
    if (ScriptRng::FightColours(u.colours)) r.flags |= ReplayFile::kColoursKnown;
    char arena[80] = "";
    if (FlowTrace::FightStageName(arena, sizeof arena)) {
        ReplayFile::CopyText(u.arena, sizeof u.arena, arena);
        u.arenaIndex = ScriptRng::StageIndex(arena);
    }
    uint16_t mine = 0, theirs = 0;
    if (online && Refight::LastFightKodes(&mine, &theirs)) {
        u.kodes[0] = localPlayer == 2 ? theirs : mine;
        u.kodes[1] = localPlayer == 2 ? mine : theirs;
        r.flags |= ReplayFile::kKodesKnown;
    }
    u.localPlayer = localPlayer;
    u.delay = delay;
    u.mode = GameInfoField(0x8);
    u.sub = GameInfoField(0x4);
    u.netplayGf = netplayGf;
    u.startGf = netplayGf + 1;  // (the frame-0 call corrects it)
    u.testMeter = testMeter;
    u.testLife = testLife;
    if (EnvFingerprint::Ready()) {
        const EnvFingerprint::Print& p = EnvFingerprint::Get();
        for (int i = 0; i < 3 && i < EnvFingerprint::kFields; ++i) u.env[i] = p.v[i];
        const EnvFingerprint::Stamp s = EnvFingerprint::ReleaseStamp();
        u.release[0] = s.date;
        u.release[1] = s.time;
    }
    LocalTime(u.time, sizeof u.time);
    gRecOn = true;
    char col[24], kodes[48];
    Logger::Info("REPLAY recording this session (%s): %s vs %s on %s, colours %s, Kombat Kodes %s; this game plays side %u, input delay %u",
                 online ? "an online match" : "a test session", u.p1[0] ? u.p1 : "?", u.p2[0] ? u.p2 : "?", u.arena[0] ? u.arena : "?",
                 ColoursText(r, col, sizeof col), KodesText(r, kodes, sizeof kodes), localPlayer, delay);
}

void RecInputs(uint32_t frame, uint32_t startGf, uint32_t m1, uint32_t m2) {
    if (!gRecOn) return;
    if (frame == 0) gRec.setup.startGf = startGf;
    ReplayFile::PutInputs(gRec, frame, m1, m2);
}

void RecChecksum(uint32_t frame, uint32_t cs, const ReplayFile::State* s) {
    if (gRecOn) ReplayFile::PutChecksum(gRec, frame, cs, s);
}

void RecEnd(const char* why) {
    if (!gRecOn) return;
    gRecOn = false;
    ReplayFile::Finish(gRec, gRec.frames, why);
    if (gRec.frames < kMinFrames) {
        ++gNotKept;
        Logger::Info("REPLAY: the session stopped after %u frame(s) (%s): too short to keep", gRec.frames, why ? why : "?");
        return;
    }
    gRecPending = true;
    Logger::Info("REPLAY recorded: %u frames (%.1f s), %u of them without inputs, %u checksums (the session stopped: %s); written once no "
                 "session runs",
                 gRec.frames, gRec.frames / 60.0, ReplayFile::MissingInputs(gRec), ReplayFile::Checksums(gRec), why ? why : "?");
}

void OnPlaybackStart() {
    if (gStageForced) {  // the fight is loaded: every later stage is the scripts' own again
        ScriptRng::SetMatchStage(-1);
        gStageForced = false;
    }
    const Replay& r = gPlay;
    char a[ReplayFile::kText] = "", b[ReplayFile::kText] = "", arena[80] = "";
    const bool names = ScriptRng::FightPlayers(a, b, sizeof a);
    const bool stage = FlowTrace::FightStageName(arena, sizeof arena);
    uint32_t colours[2] = {9, 9};
    const bool col = ScriptRng::FightColours(colours);
    const bool same = names && !_stricmp(a, r.setup.p1) && !_stricmp(b, r.setup.p2) && stage && !_stricmp(arena, r.setup.arena) &&
                      (!(r.flags & ReplayFile::kColoursKnown) || (col && colours[0] == r.setup.colours[0] && colours[1] == r.setup.colours[1]));
    Logger::Info("REPLAY the fight being played has %s and %s on %s, colours %u and %u, game mode %u (the recording: %s and %s on %s, mode %u): %s",
                 names ? a : "?", names ? b : "?", stage ? arena : "?", colours[0], colours[1], GameInfoField(0x8), r.setup.p1, r.setup.p2,
                 r.setup.arena, r.setup.mode, same ? "the same setup" : "NOT the same setup (the playback will likely go out of step at once)");
}

void OnPlaybackEnd(const char* why) { ReleaseSetup(why); }

void Shutdown() {
    if (!WaitWriter(3000)) Logger::Info("REPLAY: a replay was still being written after 3 s at the unload");
}

bool Command(const char* line) {
    char verb[16] = "", arg[MAX_PATH] = "";
    if (sscanf(line, "%15s", verb) != 1 || strcmp(verb, "replay")) return false;
    const char* rest = line + strlen("replay");
    while (*rest == ' ' || *rest == '\t') ++rest;
    snprintf(arg, sizeof arg, "%s", rest);
    for (size_t n = strlen(arg); n && (arg[n - 1] == ' ' || arg[n - 1] == '\r' || arg[n - 1] == '\n'); --n) arg[n - 1] = 0;
    char v[8] = "";
    if (!arg[0] || !strcmp(arg, "status")) {
        Rollback::PlaybackStatus();
        Logger::Info("REPLAY recorder: %s; %s; %u file(s) written in this game, %u session(s) too short, %u write failure(s); folder %ls; "
                     "'replay like %d'",
                     gRecordOn ? "on" : "off ('replay record 1')", gRecOn ? "recording a session" : gRecPending ? "a recording waits to be written" : "idle",
                     gWritten, gNotKept, gWriteFailures, gDir, gLike ? 1 : 0);
        return true;
    }
    if (!strcmp(arg, "stop")) {
        Rollback::PlaybackStop("'replay stop'");
        ReleaseSetup("'replay stop'");
        Logger::Info("REPLAY stop: no playback armed or running");
        return true;
    }
    if (!strcmp(arg, "list")) {
        List();
        return true;
    }
    if (sscanf(arg, "record %7s", v) == 1) {
        gRecordOn = !strcmp(v, "1");
        Logger::Info("REPLAY record %d (%s)", gRecordOn ? 1 : 0, gRecordOn ? "every session that begins at a fight's start is recorded" : "no session is recorded");
        return true;
    }
    if (sscanf(arg, "like %7s", v) == 1) {
        gLike = !strcmp(v, "1");
        Logger::Info("REPLAY like %d (%s; from the next playback armed)", gLike ? 1 : 0,
                     gLike ? "a playback's frames run as a session's: effect actors' deaths deferred, freed blocks quarantined" : "as an offline fight's");
        return true;
    }
    Arm(arg);
    return true;
}

}  // namespace ReplayKeeper
