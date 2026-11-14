#pragma once
#include <cstdint>

// In-game status line (todo item 4, docs\status_overlay_plan.md): drawn by MKKE's own Direct3D 9 code path on the
// rendering thread, just before the EndScene of RHIEndDrawingViewport 0x00666F20 (5-byte CodePatch at 0x00667106).
// It reads only the stats the game thread publishes here: no game memory, no game calls, no GMalloc, no D3D
// resources kept across frames, and no call into the window manager on the render thread: the F9 key and the monitor
// behind the DISPLAY line are read by a watcher thread of this file's own (Overlay.cpp, "the watcher thread").
namespace Overlay {

enum Mode : uint8_t { kOff = 0, kWaiting, kRollback, kLockstep, kSyncTest, kStopped, kInfo };  // kInfo: stopWhy as is
// Where the game is (GameInfo+0x4), for the HUD display: it draws its fight group in a fight and its badge before one.
enum Phase : uint8_t { kPhaseOther = 0, kPhaseSelect, kPhaseLoad, kPhaseFight, kPhaseResults, kPhaseFinisher };
// Before an online fight (character select, loading): what the next fight will run on (Online::PreFight).
enum Pre : uint8_t { kPreNone = 0, kPreReady, kPreNoMod, kPreBlocked };  // kPreBlocked: the reason is in stopWhy

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
    // The HUD display (Hud.h; feature plan 4.1).
    uint8_t phase = kPhaseOther;   // Phase
    uint8_t hudHidden = 0;         // the game's own HUD is off the screen (a finisher): ours hides with it
    uint8_t localSide = 0;         // the fighter this PC plays, 1 or 2 (the Wi-Fi marks' sides); 0 unknown
    uint8_t pre = kPreNone;        // Pre
    int16_t preRttMs = -1;         // kPreReady: the hellos' round trip in ms; -1 not measured yet
};

bool Install();          // verifies the 16 bytes at 0x00667106 first (fail closed); idempotent
void EnsureInstalled();  // game thread, now and then: re-applies the patch if a CodePatch::RestoreAll removed it
void Disable();          // unload: the stub becomes a pass-through (call before CodePatch::RestoreAll)
void Shutdown();         // unload, after Disable: waits for the watcher thread to end (up to 5 s)
bool SheetHeld();        // the stage screen's picture texture is on the device (after Disable the render thread's next
                         // frame releases it: the unload waits for that before it removes the stub)
int SheetsMade();        // how often that texture was made since the DLL loaded ('stagestatus')
void RequestDisplayLog();  // the display mode is logged from the next frame's swap chain (DISPLAY line; each session's start)
long InFlight();         // render-thread calls inside the draw
// 0 off; 1 the display that belongs to the game's HUD (Hud.h: a group under the round timer in a fight, a badge before
// one, nothing otherwise); 2 the boxed line with every number, top left, always (F9 in the game switches 1 <-> 2).
void SetLevel(int level);
int Level();
// 'huddemo N' (test harness, screenshots): the HUD display shows canned state N of Hud::Demo instead of the published
// stats, wherever the game is; 0 = off. Drawing only: nothing in the game or a session changes.
// 'hudshot <file>' (the test harness): the next finished frame, with the display on it, is written to this file as a
// BMP by the rendering thread. The log says "HUDSHOT written" or why not.
void RequestShot(const char* path);
void SetDemo(int n);
int Demo();
void Publish(const Stats& s);  // game thread, live frames only
void LogCounters();            // game thread: draws, draws while resimulating, max draw time, the F9 poll's longest wait
// 'syswatch 0|1' (default 1): the watcher thread writes down what Windows announces, which program comes to the front
// and which windows appear (SYSEVENT lines, SysWatch.h), and after a KEYWAIT the programs that used the processor.
void SetSysWatch(bool on);
// Render latency: 'framelatency N' asks the IDirect3DDevice9Ex for at most N frames queued ahead of the GPU
// (SetMaximumFrameLatency, 1..16; D3D9Ex's default is 3); 0 leaves the game's value. Applied on the render thread at the
// next draw. The probe (event queries, one per presented frame) counts the frames the GPU had not finished at each
// EndScene: GPUQUEUE lines from LogCounters.
void SetFrameLatency(int n);
int FrameLatencyWanted();

}  // namespace Overlay
