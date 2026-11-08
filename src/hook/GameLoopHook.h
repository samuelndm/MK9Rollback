#pragma once
#include <cstdint>

// Milestone 2/3 instrumentation: observe (never alter) the engine tick and
// the per-player fight update, count simulation frames and log P1/P2 pads.
namespace GameLoopHook {

extern uint64_t gSimulationFrame;  // fight updates observed since install
extern volatile bool gInFightUpdate;  // true while the original fight update (0x008ACD90) executes

bool Install();
// True once the fight body (0x008ACD90 past its gates) has run in the current engine tick.
// Effect objects die after it in fight ticks; the end-of-fight teardown runs in a tick without it.
bool FoughtThisTick();
// Resimulated EngineTick entry: false until its fight update runs, as HkEngineTick sets it for a live tick.
void ClearFoughtThisTick();
// The fight update's gate (FightUpdatePlayers' early-out) evaluated now; at EngineTick entry it predicts this tick.
bool FightBodyWillRun();
void Uninstall();
long InFlight();

}  // namespace GameLoopHook
