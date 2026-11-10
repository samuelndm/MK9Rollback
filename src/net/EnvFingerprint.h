#pragma once
#include <cstdint>

// What two netplay peers must have in common besides the executable (whose hash MK9Game already enforces):
// the same game content, the same DLL build and the same determinism-relevant switches. Exchanged on the
// checksum channel at session start; a mismatch stops the session (fail closed) instead of desyncing later.
// The online mode sends the same prints in its hello before the fight (HelloProto.h), so two games that differ
// do not start a session at all; the session's own check stays as the last word.
namespace EnvFingerprint {

enum Field { kContent, kDll, kSwitches, kFields };

struct Print {
    uint32_t v[kFields];
};

// Content: FNV-1a over the relative path (lower case) and size of every file under <exe dir>\Asset, sorted.
// DLL: FNV-1a over this DLL's file bytes. Both are computed once and cached: 11 ms with a warm file cache (2026-11-10,
// the same code on the host: 1229 files listed in 0.7 ms, the 11.8 MB DLL read and hashed in 10.5 ms), unknown from a
// cold disk; most of a frame either way, which the first session used to pay on the game thread at its frame 24.
// Switches: snapshot coverage flags and FP environment constants, computed on every call (game thread).
// Get() computes the cached part itself when Prepare() was not called, and waits for a Prepare() still running.
const Print& Get();
const char* FieldName(int f);

// The cached part on a background thread, once per process (any thread; later calls do nothing): the online mode calls it
// when it is armed, so neither the hello nor the first session's frame 24 pays for it on the game thread.
void Prepare();
bool Ready();  // the cached part is computed: Get() and ReleaseStamp() return at once
bool Busy();   // Prepare()'s thread is running (the unload waits for it)
// The switches print alone, now (game thread; a few dozen bytes hashed).
uint32_t Switches();
// This install's release: "version=YYYYMMDD-HHMM" in version.txt two folders above the DLL (tools\package.ps1), as two
// decimal numbers; 0/0 without the file (a developer tree). Part of the cached data: waits like Get().
struct Stamp {
    uint32_t date, time;
};
Stamp ReleaseStamp();

}  // namespace EnvFingerprint
