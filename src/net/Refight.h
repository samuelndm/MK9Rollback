#pragma once
#include <cstdint>

// The choice after an online match (docs\feature_parity_plan.md 4.3), game-thread side: a panel of the mod over MK9's
// own rematch box with three rows, RE-FIGHT / CHANGE FIGHTER OR ARENA / LEAVE. Each player sees the other's choice.
//
// What a choice does (RefightVote.h has the rule and the reading of MK9's script it rests on):
//   - LEAVE answers MK9's box "decline" for this player: MK9 ends the match for both, as it does today.
//   - RE-FIGHT and CHANGE answer it "accept": when both accepted, MK9 goes to its character select, as it does today.
//   - Both chose RE-FIGHT: the mod remembers the last fight's arena. On the character select each player confirms the
//     fighter (the game puts the cursor where it puts it; 'refightauto 1' lets the mod press "confirm" in the player's
//     place), and when both are locked the stage screen stays shut and both games take the same arena again
//     (StageFlow::SetRefight). So a re-fight is one button on the select screen instead of a pick and a stage screen.
//   - Anything else that is not a LEAVE: the character select and the stage screen as after any match.
// The panel opens when MK9's rematch screen begins (script native 1350 with operand 1, the first call of its script
// MKOnlineEngine_RematchScreen; in the first tick after it in which no rollback session runs), in an online one-on-one
// match whose two mods have answered each other as the same build, and closes when MK9's box is over (native 1351), the
// game leaves that screen, or the match ends. Where it does not open, the other mod does not answer within three
// seconds, or both picks are in and MK9's box does not end, MK9's own box is the players', as before.
// In the two-PC logs of 2026-11-11 that box was over 1.8 to 6 s after it began (both players pressed "confirm" at
// once): with the panel, a player who presses "confirm" at once picks RE-FIGHT, the row the cursor starts on.
//
// Commands:
//   refight 0|1|test   0 off; 1 (the start) as above; test: a simulated opponent answers through the real packet code
//                      and nothing is pressed on the game's screen, so one PC can show the panel in any menu
//   refight open|close open now (mode 1 or test) / close and give the controls back
//   refightauto 0|1    1: on the character select of a re-fight both chose, the mod presses "confirm" for this player
//                      (0 at the start: where the game puts the cursor there has not been seen in a real match)
//   refightstatus      one log line
// Every step writes a REFIGHT line. Nothing here reads or writes the fight's state, and no panel exists in a rollback
// session.
namespace Refight {

void Tick();                                       // game thread, every engine tick, before StageFlow::Tick
bool Command(const char* line);                    // true: the line was one of this file's commands
void OnPacket(const void* data, int size);         // game thread: a datagram with the picks' magic (OnlineMode's read loop)
void OnNative(uint32_t id, uint32_t firstOperand); // game thread: an online script native ran live (RngTrace), before StageFlow's tap
void Shutdown();                                   // the DLL unloads: the game gets the controls back
// Game thread: MK9's rematch screen is up with the panel open or about to open. The online mode keeps the match's
// opponent known meanwhile (it forgets it 20 s after a match's data went quiet, which a player who thinks can outlast).
bool Active();

}  // namespace Refight
