#pragma once
#include <cstdint>

// Per-region hashing of candidate gameplay state (Milestone 6). Regions are
// read in place on the game thread at FightUpdatePlayers entry. Absolute frame
// counters in GameInfo are excluded so two trials at different GameFrame values
// remain comparable; other frame-stamped fields are found by dump diffs.
namespace StateHash {

constexpr int kMaxRegions = 16;

struct Result {
    int count;
    const char* names[kMaxRegions];
    uint32_t hashes[kMaxRegions];
    uint32_t combined;
    uint32_t rngState;
    int rngDraws;  // LCG steps since the previous hashed frame (-1 = unknown / > limit)
    uint32_t objectsCreated;  // MK object id counter delta since the previous hashed frame
    int32_t scriptDepth;      // 0xDE8628, -1 = VM idle
};

void Compute(Result* out);

// Writes every region's raw bytes (plus a small text index) to
// <dir>\dumps\<label>.bin / .txt. Returns false on I/O failure.
bool Dump(const wchar_t* dir, const char* label, uint32_t gameFrame);

uint32_t XXH32(const void* data, size_t len, uint32_t seed);

}  // namespace StateHash
