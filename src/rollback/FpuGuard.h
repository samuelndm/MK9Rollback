#pragma once
#include <cstdint>

// Floating-point environment pinning for cross-machine determinism.
// Two peers run the same binary on the same inputs; their results can still differ if the x87
// control word (precision/rounding) or MXCSR (rounding, flush-to-zero, denormals-are-zero) differ,
// and drivers, overlays and Direct3D are known to change them per thread. The game runs with
// x87 0x027F (53-bit, nearest, all masked) and MXCSR control 0x1F80 (nearest, no FTZ/DAZ, all
// masked) on every thread (command 'fpu', 2026-10-17); those values are enforced here.
namespace FpuGuard {

constexpr uint16_t kX87 = 0x027F;
constexpr uint32_t kMxcsrControl = 0x1F80;  // bits 6..15; the sticky exception flags (0..5) are ignored
constexpr uint32_t kMxcsrMask = 0xFFC0;

// Game thread, at every simulated frame (live and resimulated): restore the canonical values if
// something changed them. Cheap (two stores, two compares).
void PinGameThread();
// All threads of the process (the task pool runs anim jobs): fix any thread whose control bits
// differ. Suspends each thread briefly; call rarely (session start, every few hundred frames).
int PinAllThreads();
// 'fpupinsession 1': the every-600-ticks walk also runs during netplay sessions (default 0: once when a session starts
// waiting for its peer, none while it runs - net1 2026-11-04: one of the two long frames per 600, no correction ever).
extern bool gPinInSessions;
uint32_t Corrections();  // game-thread corrections so far
}  // namespace FpuGuard
