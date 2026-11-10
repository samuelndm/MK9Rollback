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
    // Connection quality inputs (rc25, published by RollbackManager::PublishOverlay; the overlay decides the wording).
    int16_t rttMs = -1;            // best recent round trip in ms (network time, not rounded to frames); -1 unknown
    uint16_t netStallMs = 0;       // longest wait for the other game's input in the last 10 s (ms); 0 none
    uint16_t netStallAgoS = 0;     // seconds since that wait ended
    uint16_t localHitchMs = 0;     // longest frame of this PC in the last 10 s that was not a network wait (ms); 0 none
    uint16_t localHitchAgoS = 0;   // seconds since that frame
    uint8_t peerInterrupted = 0;   // GGPO says the other game stopped responding (cleared when it responds again)
    uint8_t stopAgeS = 0;          // kStopped: seconds since the session stopped
    // The connection's kind (round 3, an online match's hellos): shown because the players can act on it.
    uint8_t wifi = 0;              // bit 0: this PC's route is Wi-Fi, bit 1: the opponent's
    uint8_t relayed = 0;           // Steam carries the match through its relay, not directly
};

bool Install();          // verifies the 16 bytes at 0x00667106 first (fail closed); idempotent
void EnsureInstalled();  // game thread, now and then: re-applies the patch if a CodePatch::RestoreAll removed it
void Disable();          // unload: the stub becomes a pass-through (call before CodePatch::RestoreAll)
void RequestDisplayLog();  // the render thread logs the display mode at its next frame (DISPLAY line; each session's start)
long InFlight();         // render-thread calls inside the draw
void SetLevel(int level);  // 0 off, 1 on (only while netplay is armed, starting, running or just stopped), 2 verbose
int Level();
void Publish(const Stats& s);  // game thread, live frames only
void LogCounters();            // game thread: draws, draws while resimulating, max draw time
// Render latency: 'framelatency N' asks the IDirect3DDevice9Ex for at most N frames queued ahead of the GPU
// (SetMaximumFrameLatency, 1..16; D3D9Ex's default is 3); 0 leaves the game's value. Applied on the render thread at the
// next draw. The probe (event queries, one per presented frame) counts the frames the GPU had not finished at each
// EndScene: GPUQUEUE lines from LogCounters.
void SetFrameLatency(int n);
int FrameLatencyWanted();

}  // namespace Overlay
