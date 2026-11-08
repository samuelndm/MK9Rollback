#pragma once

// Test-control channel. A background thread polls MK9Rollback_cmd.txt next to
// the DLL; the game thread executes the queued commands at EngineTick entry.
//   hold <player 1-4> <hexmask> <frames>   queue scripted input for a player
//   release <player 1-4>                   drop the queue, give the pad back
//   pause <0|1>                            call the engine pause setter 0x00407AB0
//   mark <text>                            write a MARK line into the frame log
namespace CommandChannel {
bool Start(const wchar_t* directory);
void Stop();
//   step <n>                               while paused: run exactly n fight frames, then re-pause
//   rngset <hex>                           overwrite the gameplay LCG state 0x00E7C054 (this tick)
//   dumpafter <n> <label>                  dump state regions at the n-th fight frame from now (0 = this tick)
// Game thread only.
void RunFrame();
void EndFrame(bool fightRan);
void OnFightFrame(unsigned gameFrame);  // called at FightUpdatePlayers entry when the body will run
bool ObjectHooksActive();
void Shutdown();  // game-thread-safe cleanup when unloading (releases pads)
// Pause (1) / unpause (0) through the engine setter at the end of this tick (frame boundary).
void RequestPause(int on);
// Netplay pause guard on the pause setter 0x00407AB0: the game's own pause-on requests are refused while a P2P session runs.
bool InstallPauseGuard();
// Destroys every AIDrone (netplay start: both fighters are driven by the session). Returns how many there were.
int KillAiDrones();
int CountAiDrones();  // AIDrones alive (one per CPU-controlled fighter)
}
