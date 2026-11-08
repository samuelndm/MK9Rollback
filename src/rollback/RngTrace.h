#pragma once
#include <cstdint>
#include <vector>

// Who draws RNG A ([0x00E7C054]), per frame. All five references to the state are in the two
// wrappers 0x005F5080 RandInt (cdecl, returns state & 0x7FFFFFFF) and 0x005F50A0 RandFloat, so
// hooking their entries sees every draw. The original timeline's per-frame caller sequence is
// kept for the rollback window; each resimulated frame is compared with it and the first
// diverging draw (index, original caller, resimulated caller) is logged. Gameplay mismatches
// with identical fighter state but a different RNG (soak8) point at the subsystem that owns
// unrestored state.
namespace RngTrace {

bool Install();
// Frame boundary (game thread): draws from now on belong to frame gf.
void BeginFrame(uint32_t gf, bool resimulating);
// After a resimulated frame gf ran: compare with the original sequence, then adopt it.
// Returns true if the sequences matched.
bool EndResimFrame(uint32_t gf, int rollbackK);
// Test harness: script executors that drew from AI natives (0x0088C700..0x00890E00) recently.
void AiExecutors(std::vector<uint32_t>& out);
// Script contexts (current-context global [0x00E39DF8]) seen at MKScript native dispatch.
const std::vector<uint32_t>& ScriptContexts();
void ClearScriptContexts();
// Diagnostics for the two-instance test: count RNG A draws outside fights per caller; DumpIdle logs and clears them.
void SetIdleLog(bool on);
void DumpIdle();
// The final timeline's RNG A callers (with the script executor of each draw) of frame gf, if still in the ring (64).
void LogFrameDraws(uint32_t gf, const char* tag);
void SetRngOnly(bool on);              // 'rngtrace 1' before netcode: LogFrameDraws data without the lab's other traces
void LogOtherThreads(const char* tag);  // RNG A draws made off the game thread (lab or rngtrace)
// GameFrame was renumbered (netplay start): shift the frame numbers kept per script context.
void RebaseFrames(uint32_t oldGf, uint32_t newGf);

}  // namespace RngTrace
