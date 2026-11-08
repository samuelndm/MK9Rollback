#pragma once
#include <cstdint>

// Frame pacing of a running P2P session (smoothness measurements, 'PACE' log lines): the wall time between live frame
// boundaries, what the boundary spent it on (live save, loads, replayed ticks and saves, waits for the peer), rollback
// depths, and the render thread's presented-frame intervals (Overlay). One line every 600 session frames and one per
// session at its stop.
namespace FramePace {
// kWorldTick / kEngineTick: the live frame's own world tick (vt+0xBC after the boundary) and engine tick (vt+0xC0), the
// game's simulation with our in-tick hooks; shown in HITCH lines only (the PACE line format is parsed by the tools).
enum Part { kLiveSave, kLoad, kReplayTick, kReplaySave, kWait, kBoundary, kExtra, kWorldTick, kEngineTick, kParts };

int64_t Now();                        // QPC ticks
void SessionStart();
void LiveFrame(uint32_t gf);          // a live, unpaused P2P frame boundary (its entry); gf = its GameFrame
// A periodic job ran in the current frame interval: named in that interval's HITCH line when it is >= 40 ms. `what`
// must be a string literal (kept by pointer).
void Note(const char* what);
void Gap();                           // a boundary that is not one: the next interval is not measured
void Add(Part p, int64_t ticks);
void Rollback(uint32_t depth);
void SkippedSave();
void Report(uint32_t gf);             // the window since the last report
void SessionReport(const char* why);  // the whole session

// Present intervals, filled by the render thread (Overlay's pre-EndScene hook): 1 ms buckets, cumulative.
constexpr int kBuckets = 256;
void OnPresent();

// 'stallsample N' (default 0 = off): once N ms have passed since the last live P2P frame boundary, a sampler thread
// captures the game thread (suspended for the copy) up to 5 times, 8 ms apart, and logs where it is: eip and the return
// addresses on its stack (MKKE addresses as is, others as module+offset), and the render thread's (the thread that
// calls OnPresent) in the same line. STALL lines; for long frames whose time is outside the simulation (HITCH "world tick
// 0.0 engine tick 1.4"), e.g. the per-fight Graveyard frame (the game thread waits in the render fence 0x006360E0).
void SetStallSample(int ms);
// The release default (P2P sessions, 2026-11-08): 250 ms unless a 'stallsample' was given first; also logs when the
// sampler itself could not run for >= 200 ms (the whole process stopped).
void DefaultStallSample(int ms);
}  // namespace FramePace
