#pragma once
#include <cstdint>

// In-game status line (todo item 4, docs\status_overlay_plan.md): drawn by MKKE's own Direct3D 9 code path on the
// rendering thread, just before the EndScene of RHIEndDrawingViewport 0x00666F20 (5-byte CodePatch at 0x00667106).
// It reads only the stats the game thread publishes here: no game memory, no game calls, no GMalloc, no D3D
// resources kept across frames.
namespace Overlay {

enum Mode : uint8_t { kOff = 0, kWaiting, kRollback, kLockstep, kSyncTest, kStopped, kInfo };  // kInfo: stopWhy as is

struct Stats {  // plain data, no pointers
    uint8_t mode = kOff;
    int16_t ping = -1, behindLocal = 0, behindRemote = 0, delay = 0;
    uint16_t lastDepth = 0, maxDepth = 0, loadsPerSec = 0;
    int32_t loads = 0, replayed = 0, mismatched = 0, barrierWaits = 0, kbps = 0, sendQueue = 0;
    char stopWhy[64] = "";
};

bool Install();          // verifies the 16 bytes at 0x00667106 first (fail closed); idempotent
void EnsureInstalled();  // game thread, now and then: re-applies the patch if a CodePatch::RestoreAll removed it
void Disable();          // unload: the stub becomes a pass-through (call before CodePatch::RestoreAll)
long InFlight();         // render-thread calls inside the draw
void SetLevel(int level);  // 0 off, 1 on (only while netplay is armed, starting, running or just stopped), 2 verbose
int Level();
void Publish(const Stats& s);  // game thread, live frames only
void LogCounters();            // game thread: draws, draws while resimulating, max draw time

}  // namespace Overlay
