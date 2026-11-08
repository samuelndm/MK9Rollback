#pragma once
#include <cstdint>

// What two netplay peers must have in common besides the executable (whose hash MK9Game already enforces):
// the same game content, the same DLL build and the same determinism-relevant switches. Exchanged on the
// checksum channel at session start; a mismatch stops the session (fail closed) instead of desyncing later.
namespace EnvFingerprint {

enum Field { kContent, kDll, kSwitches, kFields };

struct Print {
    uint32_t v[kFields];
};

// Content: FNV-1a over the relative path (lower case) and size of every file under <exe dir>\Asset, sorted.
// DLL: FNV-1a over this DLL's file bytes. Both are computed once and cached.
// Switches: snapshot coverage flags and FP environment constants, computed on every call.
const Print& Get();
const char* FieldName(int f);

}  // namespace EnvFingerprint
