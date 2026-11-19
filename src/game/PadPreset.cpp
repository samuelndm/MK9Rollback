#include "PadPreset.h"

#include <windows.h>

#include <cwchar>

#include "../debug/Logger.h"
#include "../hook/GameLoopHook.h"
#include "../rollback/RollbackManager.h"

// MK9's controller preset per player, kept in <the DLL's folder>\controller_presets.txt (PadPreset.h says why and how).
namespace PadPreset {
namespace {
constexpr uintptr_t kRecords = 0x00E89D7C;   // MK9's four player records (0x00E89B70 + 0x20C), 0xC8C bytes each
constexpr uint32_t kRecordBytes = 0xC8C;
constexpr uint32_t kRecordVtable = 0x00D5DF68;  // written by the record constructor 0x008A1580
constexpr uint32_t kPadOff = 0xB40, kPad2Off = 0xB44, kPresetOff = 0xC48, kPreset2Off = 0xC4C;
constexpr uintptr_t kApplyPresets = 0x0087F040;  // native 64916:62: each player's preset into its pads (what the pause menu calls)
constexpr uint32_t kLookEvery = 30;              // engine ticks between two looks (twice a second in the menus)

wchar_t gPath[MAX_PATH] = L"";
bool gLoaded = false;
Saved gSaved;
Tracker gTracker;
uint32_t gTicks = 0;
bool gLoggedStart = false, gLoggedNoRecord = false;
int gWriteFailures = 0;

uintptr_t Record(int i) { return kRecords + (uintptr_t)i * kRecordBytes; }
int32_t Read32(uintptr_t a) { return *reinterpret_cast<const int32_t*>(a); }

const char* Label(int32_t v, char* buf, size_t n) {
    if (v >= 0 && v <= 3) snprintf(buf, n, "%d", (int)v + 1);
    else if (v == -1) snprintf(buf, n, "PADRAO");
    else if (v == -2) snprintf(buf, n, "REMOTO");
    else snprintf(buf, n, "?%d", (int)v);
    return buf;
}

// "2" when both pads of the player have the same preset, else "2 / 1" (signed-in pad / other pad).
const char* PairText(Pair p, char* buf, size_t n) {
    char x[16], y[16];
    if (p.a == p.b) snprintf(buf, n, "%s", Label(p.a, x, sizeof x));
    else snprintf(buf, n, "%s / %s", Label(p.a, x, sizeof x), Label(p.b, y, sizeof y));
    return buf;
}

void Load() {
    gLoaded = true;
    if (!gPath[0]) return;
    FILE* f = _wfopen(gPath, L"rb");
    if (!f) {
        Logger::Info("PRESET: no %ls yet: nothing to put back (the mod keeps a player's controller preset once one is chosen)", gPath);
        return;
    }
    char text[2048];
    const size_t n = fread(text, 1, sizeof text - 1, f);
    fclose(f);
    text[n] = 0;
    int skipped = 0;
    gSaved = Parse(text, &skipped);
    char line[160] = "";
    int len = 0;
    for (int i = 0; i < kPlayers; ++i) {
        if (!gSaved.has[i]) continue;
        char t[32];
        const int k = snprintf(line + len, sizeof line - len, " P%d %s", i + 1, PairText(gSaved.pair[i], t, sizeof t));
        if (k > 0 && k < (int)(sizeof line - len)) len += k;
    }
    Logger::Info("PRESET: read %ls:%s (%d line(s) skipped)", gPath, len ? line : " no player", skipped);
}

void Save() {
    if (!gPath[0]) return;
    char text[1024];
    const int n = Format(gSaved, text, sizeof text);
    wchar_t tmp[MAX_PATH + 8];
    swprintf(tmp, sizeof tmp / sizeof tmp[0], L"%ls.new", gPath);
    FILE* f = _wfopen(tmp, L"wb");
    bool ok = f && fwrite(text, 1, (size_t)n, f) == (size_t)n;
    if (f) ok = (fclose(f) == 0) && ok;
    ok = ok && MoveFileExW(tmp, gPath, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
    if (!ok) {
        const DWORD e = GetLastError();
        _wremove(tmp);
        if (gWriteFailures++ < 4) Logger::Info("PRESET: %ls could not be written (error %lu); the choice is kept until the game closes", gPath, e);
    }
}
}  // namespace

void SetFolder(const wchar_t* dir) {
    if (dir && *dir) swprintf(gPath, MAX_PATH, L"%ls\\controller_presets.txt", dir);
}

void Tick() {
    if (++gTicks % kLookEvery) return;
    // Only in the menus and a paused offline fight: no file work or game call inside a running fight or session.
    if (Rollback::gIsResimulating || Rollback::NetplayRunning() || Rollback::NetplayStarting() || Rollback::PlaybackRunning() ||
        GameLoopHook::FightBodyWillRun())
        return;
    if (!gLoaded) Load();
    for (int i = 0; i < kPlayers; ++i) {
        if ((uint32_t)Read32(Record(i)) != kRecordVtable) {
            if (!gLoggedNoRecord) {
                gLoggedNoRecord = true;
                Logger::Info("PRESET: player record %d is not MK9's (0x%08X at 0x%08X): controller presets are left alone", i + 1,
                             (uint32_t)Read32(Record(i)), (uint32_t)Record(i));
            }
            return;
        }
    }
    if (!gLoggedStart) {
        gLoggedStart = true;
        char line[240] = "";
        int len = 0;
        for (int i = 0; i < kPlayers; ++i) {
            const uintptr_t r = Record(i);
            char t[32];
            const int k = snprintf(line + len, sizeof line - len, " P%d %s (pads %08X %08X)", i + 1,
                                   PairText(Pair{Read32(r + kPresetOff), Read32(r + kPreset2Off)}, t, sizeof t),
                                   (uint32_t)Read32(r + kPadOff), (uint32_t)Read32(r + kPad2Off));
            if (k > 0 && k < (int)(sizeof line - len)) len += k;
        }
        Logger::Info("PRESET: the players' controller presets as the mod first sees them:%s", line);
    }
    bool restored = false, changed = false;
    for (int i = 0; i < kPlayers; ++i) {
        const uintptr_t r = Record(i);
        const Pair now{Read32(r + kPresetOff), Read32(r + kPreset2Off)};
        Pair p;
        char t[32];
        switch (gTracker.Look(i, now, gSaved, &p)) {
            case Act::kRestore:
                *reinterpret_cast<int32_t*>(r + kPresetOff) = p.a;
                *reinterpret_cast<int32_t*>(r + kPreset2Off) = p.b;
                restored = true;
                Logger::Info("PRESET: player %d's controller preset set to %s, as last chosen (MK9 starts every game on 1)", i + 1,
                             PairText(p, t, sizeof t));
                break;
            case Act::kSave:
                gSaved.has[i] = true;
                gSaved.pair[i] = p;
                changed = true;
                Logger::Info("PRESET: player %d chose controller preset %s (pads %08X %08X); kept for the next start", i + 1,
                             PairText(p, t, sizeof t), (uint32_t)Read32(r + kPadOff), (uint32_t)Read32(r + kPad2Off));
                break;
            case Act::kNone:
                break;
        }
    }
    if (restored) {
        // As the pause menu does after a choice: MK9 copies each player's preset into its pads now (a pad without its
        // profile loaded yet gets it at the next fight's start, where MK9 calls the same routine).
        reinterpret_cast<void(__cdecl*)()>(kApplyPresets)();
        Logger::Info("PRESET: MK9 applied the players' presets to their pads (0x%08X)", (uint32_t)kApplyPresets);
    }
    if (changed) Save();
}

}  // namespace PadPreset
