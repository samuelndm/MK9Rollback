#pragma once
#include <cstdint>

// A mark that travels with each frame from the game thread to the render thread (round 3, 2026-11-10). Two uses:
// replayed frames are not drawn, and the picture of a live frame knows when its input was sampled.
//
// Replayed frames. A rollback replays frames S..F-1 inside one game-loop tick, and each replayed frame ran the engine's
// whole frame: its draw was queued, rendered and PRESENTED. A depth-2 correction showed two intermediate pictures and
// the live one within a few milliseconds (ns30 runs: 64-74 presents a second for 60 frames; "OVERLAY draws ... while
// resimulating"), and with vertical sync on a player's PC each of those presents can wait for a refresh. Commercial
// rollback games simulate replayed frames without rendering them.
//
// MK9 already has a path that does exactly this for a frame its limiter marks late (RollbackManager.cpp,
// InstallRenderLate): RHIBeginDrawingViewport (0x00666DE0, render thread) computes the skip flag 0x00E84705 from the
// limiter's late flag 0x00E844A5 (read at 0x00666DF9), every draw call tests it, and RHIEndDrawingViewport
// (0x00666F20) then ends the scene without presenting. The game thread's side of a skipped frame is identical to a
// drawn one, so the simulation, the snapshots and the render thread's object updates are unchanged: only draw calls
// and the present are left out, by the game's own code.
//
// The late flag is one global, read whenever the render thread gets there, so it cannot mark single frames. The mark
// travels with the frame instead: the begin-drawing render command {vtable 0x00D59D08, viewport} is queued on the game
// thread at 0x00756C0E and 0x00757A47 (mov dword [eax], 0x00D59D08). Both sites now store one of two copies of that
// vtable, chosen by Rollback::gIsResimulating; the copies' Execute slot (0x00753620) is a wrapper around the original
// that tells the patched read at 0x00666DF9 "skip this one" for a replayed frame. The game's other conditions for
// skipping (0x00DECAFC, 0x0052CD20, 0x0041C6A0) stay in force. The non-threaded fallback (a command built on the
// stack, Execute called directly) is not tagged: its frames are drawn as before. A replayed frame is drawn after all
// once kMaxRun (16) in a row were skipped, so a replay flag that stayed set by a bug shows a slow picture and a log
// line instead of a frozen one.
//
// Input stamp. Queued commands are counted on the game thread and executed in the same order on the render thread, so
// a value stored for "the next command" (SetStamp) is found by that command's wrapper and handed to the present hook
// (TakeStamp): FramePace's controller-to-present statistic (LATENCY lines).
namespace RenderTag {

bool Install();
void SetReplaySkip(bool on);  // 'replayskip 0': replayed frames are drawn and presented again (rc27)
void Disable();               // unload: no new command carries this module's vtables (call before CodePatch::RestoreAll)

// Game thread, after a boundary's replays: the frame queued next shows a local input sampled at `qpc` (0 = none).
void SetStamp(int64_t qpc);
// Render thread (present hook): the stamp of the frame being drawn, once; 0 when it has none.
int64_t TakeStamp();

// P2P session totals (field logs).
void SessionReset();
void LogSession();

}  // namespace RenderTag
