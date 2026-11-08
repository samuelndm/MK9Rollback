#pragma once
#include <cstdint>

// Background crowd randomness off the gameplay RNG (2026-11-04).
//
// Stages with background characters (Courtyard's monks, ...) run a crowd manager from the stage script every frame
// (script native 0x008DB5C0 -> 0x008F9E70, per character 0x008F9C70: a tick counter that picks a new idle state after
// rand(100)+200 ticks). Its rand(n) calls (0x00492050) drew RNG A, the stream the fighters' scripts draw. The stage
// script's phase against the fight start differed by a tick between the two games (two-instance lab trace, Courtyard:
// frame 0 drew 2 RNG A values on one peer and 6 on the other, the four extra from 0x008F9D65/0x008F9D77 in the stage
// script's executor), so every later fighter draw came from another position of the stream: a desync that no rollback
// repairs. The crowd's 15 call sites now draw from a stateless stream: a hash of (GameFrame, draw index in the frame),
// so a replayed frame repeats the original frame's crowd draws without anything in the snapshot.
namespace CrowdRng {

bool Install();           // retargets the call sites (expected bytes checked); idempotent
void SetActive(bool on);  // off: the call sites forward to the game's rand(n) (RNG A) as before
bool Active();
void BeginFrame();        // a live or replayed frame starts: the draw index restarts

}  // namespace CrowdRng
