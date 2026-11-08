#pragma once
#include <cstdint>

// Live-vs-replay comparison of the fighters' SkeletalMeshComponent at skeleton-update entry ('animtrace 1|0').
namespace AnimInputTrace {
bool SetEnabled(bool on);
// Per replayed frame S+1..gf, which traced stage differed from live (for SyncTest desync reports).
void ImagePointAt(int pt);  // full MK image compare at WorldTick entry (4) / EngineTick entry (5)
void HitReplayFrameDone(uint32_t gf);  // end of a replayed frame: live hit tests the replay did not repeat
void OnLoadDone();  // after a load: arm the Location write trace on a fighter-attached MK actor (LOCMOVED)
void ImagePoint(int at);  // MK image page hashes at FightUpdate entry (0) / exit (1), live vs replay
int Describe(uint32_t S, uint32_t gf, char* out, int len);  // installs the 0x007741E0 hook on first use (restored by CodePatch::RestoreAll)
}
