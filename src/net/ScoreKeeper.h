#pragma once
#include <cstddef>

#include "Score.h"

// The score of an evening's online matches and the history of matches, game-thread side (net\Score.h has how a match's
// winner is read and the history file's text).
//
// In every online one-on-one match (Player Match / private room, the matches the re-fight panel serves; ranked and King
// of the Hill are not counted), from the fight's first frame to MK9's results screen: the two fighters' lives are read
// once per engine tick, each round's winner is written to the log ("SCORE: round N over"), and at the results screen
// the match is counted as won, lost or drawn for this player. A counted match is a line of
// <the DLL's folder>\match_history.txt (written once no rollback session runs) and a "SCORE: the match is over" line.
//
// What the players see (the user's wish of 2026-11-16, "show it better ... on the fighting screen as well! Also show
// the win streak ... And put the history match in the game as well!"): the board of the room (Score::Board: the score,
// the wins in a row, the last matches, the record of each fighter this player chose). The choice after a match draws it
// around its panel (net\Refight.cpp: FillBoard -> debug\StageScreen.h); in a fight each player's wins stand under that
// player's health bar (debug\ScoreView.h, published from Tick).
// The room (2026-11-21: "Reset the count when closing the game or the match, also the match history, just keep the
// history there after the match while the match is still going, if we leaves we reset! Of course all matches should be
// saved elsewhere"): the counted matches against one opponent since the two met in this game. It is kept in memory
// only and ends when the game closes, when a player chooses LEAVE after a match (RoomOver, from the panel), when MK9's
// online match ends (OnlineMode's MatchEnds: the connection quiet for 20 s, a new connection), or when the next counted
// match is against someone else; the next match then starts a new room at 0 - 0. A REMATCH or a CHANGE FIGHTER OR
// ARENA stays in the room. Every match is still a line of match_history.txt (and a replay), for the window's list.
// Nothing here writes the game's memory or calls the game; a match that does not reach the results screen, or whose
// lives do not say who won, is not counted (a "not counted" line says why).
//
// Commands:
//   score [status]    a SCORE status line
//   score 0|1         off / on (on at the start)
//   score test 0|1    a look at one PC: EVERY fight counts, offline too (this PC's player is player 1), a match ends
//                     after 6 s without a fight update; written to match_history_test.txt, never to the real history
//   score demo N      the fight's numbers in a canned state N (1..4) wherever the game is; 0 off (screenshots)
namespace ScoreKeeper {

void SetFolder(const wchar_t* dir);  // the DLL's folder: match_history.txt beside the DLL
void Tick();                         // game thread, every live engine tick, before the fight update
bool Command(const char* line);      // true: the line was one of this file's commands
// The board of the room (the last counted match's, until the next one starts a new room). False (*out cleared):
// nothing to show yet. Game thread.
bool FillBoard(Score::Board* out);
// A player chose LEAVE on the choice after a match (net\Refight.cpp): the room ends; its board stays on that panel and
// the next counted match starts a new one. Game thread.
void RoomOver(const char* why);
// The board in one line ("SCORE  YOU 3 - 2 OPP ..."), for the log. Game thread.
bool PanelLine(char* out, size_t n);

}  // namespace ScoreKeeper
