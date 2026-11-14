#pragma once
#include <cstdint>

// The stage screen's game-thread side (docs\feature_parity_plan.md 4.2): opens the panel, gives it the controls, runs
// the exchange of picks with the other mod (StagePick.h), publishes what the panel shows (StageScreen.h) and, where
// the game's settings message is built or received, writes or checks the agreed stage.
//
// Since 2026-11-13 the screen is on from the start and opens by itself in an online one-on-one match between two mods of
// the same build, in the frame the last fighter is locked (script native 1375). The commands:
//   stagescreen 0|1|test   0 off; 1 (the start) the panel may open in an online one-on-one match in which both mods
//                          are the same build; test: a simulated opponent answers through the real packet code, so one
//                          PC can show the whole sequence in any menu or select screen ("TEST" stands on the panel)
//   stagescreen open|close open now (mode 1 or test) / close and fall back
//   stagetrigger N         mode 1: open when script native N runs (0 none; 1375 at the start, the native the select
//                          screen's script runs in the frame the last fighter is locked, read in the two-PC logs).
//   stageoverride 0|1      1: write an agreed stage into the settings message this game sends (default 0: a trial run,
//                          the draw is shown and the game's own choice stays)
//   stagehold 0|1          1 (the start): a panel that opened by itself keeps the game on the character select until
//                          the draw, and both games then load the drawn arena (StageHold.h); 0: the trial run, the game
//                          goes on under the panel and loads its own arena
//   stagetime N            the countdown of a panel that holds the game, 3 to 15 seconds (8 at the start)
//   stagewrite N           a test of the arena write alone, in any game: stage table entry N is written as the panel
//                          writes an agreed arena (-1 takes it back); the next fight's log line names its stage
//   stagedemo N            the panel's canned picture N (StageScreen::Demo), 0 off; drawing only
//   stagestatus            one log line with the switches and the current step
// Every step writes a STAGEPICK line.
//
// Fallbacks (the game's own choice stands, a line says why): no mod or another build on the other side (the panel does
// not open), no answer or no pick in time, two different stage tables, equal random numbers, a result the other game
// has not confirmed when the settings message is built, a display that cannot draw the panel. A rollback session and
// the panel never exist together: the panel does not open in one and closes when one starts. Nothing here reads or
// writes the fight's state.
namespace StageFlow {

void Tick();                                // game thread, every engine tick, before CommandChannel::RunFrame
bool Command(const char* line);             // true: the line was one of this file's commands
void OnPacket(const void* data, int size);  // game thread: a pick datagram from the opponent (OnlineMode's read loop)
void OnNative(uint32_t id);                 // game thread: an online script native ran live (RngTrace)
// Game thread: the game's script asked native 1260 whether sync wait 'type' still runs and the game answered
// 'gameAnswer' (0 over, 1 still waiting: StageHold.h says where that was read). True: the answer must read
// StageHold::kAnswerWaiting, because a panel that opened by itself still has the players picking, or because a re-fight
// both players chose waits for its arena (Refight.h). The caller writes it where the native left its answer.
bool OnSyncPoll(uint32_t type, uint32_t gameAnswer);
// FlowTrace's tap where the game builds message 9, in a connected one-on-one match, on the game thread. True: the
// message's selected background index was replaced by the agreed stage table entry.
bool OnSettingsSend(uint8_t* bytes16);
// FlowTrace's tap where message 9 arrives, on whichever thread that is: noted, and looked at by the next Tick.
void NoteSettingsReceived(uint32_t index);
void Shutdown();  // the DLL unloads: the game gets the controls back
// The choice after a match (Refight.h), game thread. Idle: no panel of the stage screen is open and no banner of it is
// on the screen. SetRefight: both players chose the same fight again; the next panel that opens by itself (the next
// match's) is not shown, locks stage table entry 'entry' for this player at once, and shows two lines at the foot of the
// screen instead of the grid (-1: forget it). The exchange, the hold and the write are the usual ones.
bool Idle();
void SetRefight(int entry);

}  // namespace StageFlow
