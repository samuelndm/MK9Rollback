#pragma once
#include <cstdint>

// The choice after an online match (docs\feature_parity_plan.md 4.3), game-thread side: a panel of the mod over MK9's
// own rematch box with three rows, REMATCH / CHANGE FIGHTER OR ARENA / LEAVE. Each player sees the other's choice.
// (The first row read RE-FIGHT until 2026-11-16; the players asked for "another name used by the other fighting games".
// The code still calls it the re-fight.)
//
// What a choice does (RefightVote.h has the rule and the reading of MK9's script it rests on):
//   - LEAVE answers MK9's box "decline" for this player: MK9 ends the match for both, as it does today.
//   - RE-FIGHT and CHANGE answer it "accept": when both accepted, MK9 goes to its character select, as it does today.
//   - Both chose RE-FIGHT: the next fight has the same fighters, the same costumes and the same arena, and asks the
//     players nothing (their wish after the first evening on it, 2026-11-14: a re-fight must not stop at the character
//     select). MK9's script still goes through its character select, the only way it has to the next fight. So the mod
//     covers that screen with two lines, holds the controls, presses "confirm" for its player until both fighters are
//     locked, and gives the game the last fight's two fighters at the point where the script sets them
//     (ScriptRng::SetMatchPlayers; the select screen's own picks never reach the fight), with the colours each fighter
//     had (the select screen's START: ScriptRng::FightColours). The stage screen then stays
//     shut and both games take the last arena (StageFlow::SetRefight). Whether the fighters stand is settled there,
//     with the arena: both games must hold the same pair and have said so to each other, else each game is given back
//     what its select screen had set. 'refightauto 0': the select screen is the player's and only the arena is kept
//     (the release of 2026-11-14).
//     Under the cover the select screen is not heard either: while the game is on it, no sound is started (its
//     announcer names the select screen's fighters; rollback\SoundDedupe.h, Quiet). The cover stays until the fight
//     begins: by itself MK9's versus / loading screen shows copies of the select screen's two body pictures, not the
//     fight's fighters (seen in one offline game, 2026-11-16). 'refightpics 1' has those copies made of the re-fight's
//     fighters (rollback\ScriptRng.h, MatchPictures), and 'refightload 1' then ends the cover a moment into the load
//     and shows that screen with its Kombat Kodes (the players' wish of 2026-11-15); where a picture could not be
//     made, the cover stays. Both are on at the start since one offline game showed them (2026-11-16).
//     The Kombat Kodes of that versus screen are the players' (2026-11-16: "make the MK codes not locked, make it
//     editable and reseted"): it starts at 0-0-0 and each player enters a kode, as before any fight. 'refightkodes 1':
//     the game's session is given the last fight's kodes, locked, while that screen runs (as MK9 locks them itself when
//     a load ends; RefightVote.h has the reading of the game's code), and typing changes nothing.
//   - Anything else that is not a LEAVE: the character select and the stage screen as after any match.
// The panel opens when MK9's rematch screen begins (script native 1350 with operand 1, the first call of its script
// MKOnlineEngine_RematchScreen; in the first tick after it in which no rollback session runs), in an online one-on-one
// match whose two mods have answered each other as the same build, and closes when MK9's box is over (native 1351), the
// game leaves that screen, or the match ends. Where it does not open, the other mod does not answer within three
// seconds, or both picks are in and MK9's box does not end, MK9's own box is the players', as before.
// In the two-PC logs of 2026-11-11 that box was over 1.8 to 6 s after it began (both players pressed "confirm" at
// once): with the panel, a player who presses "confirm" at once picks REMATCH, the row the cursor starts on.
//
// Commands:
//   refight 0|1|test   0 off; 1 (the start) as above; test: a simulated opponent answers through the real packet code
//                      and nothing is pressed on the game's screen, so one PC can show the panel in any menu
//   refight open|close open now (mode 1 or test) / close and give the controls back
//   refightauto 0|1    1 (the start): a re-fight keeps its fighters and asks nothing on the character select;
//                      0: each player confirms the fighter there, only the arena is kept
//   refight direct [A B|0]  a look at one PC, in any game: the next fight a script sets up gets fighters A and B (the
//                      game's names, "CHAR_Sektor_C"; none: those of the fight the game last set up), and on the next
//                      character select the mod covers the screen and presses "confirm"; 0: over. Without names the
//                      fighters' colours are kept too; 'refight direct A B c1 c2' gives them colours c1 / c2 (0 or 1)
//   refightkodes 0|1   0 (the start): a rematch's versus screen starts at 0-0-0 and the players enter their kodes;
//                      1: the last fight's kodes, locked
//   refightquiet 0|1   1 (the start): the hidden character select of a re-fight is not heard; 0: it is
//   refightquiet <ms>  a look at one PC (100 to 120000): no sound is started for that long, whatever the game shows,
//                      and the same "REFIGHT quiet" lines name what was held back. Seen so in one game, 2026-11-14:
//                      a fighter's lock on the character select starts two sounds through the method at +0x1C, the
//                      "confirm" sound and one of a second sound object whose number differs from fighter to fighter
//                      (Scorpion 0x3495, Sub-Zero 0x349E): the announcer's name, held back like the rest
//   refightload 0|1    0: the cover stays until the fight begins; 1 (the start): a re-fight shows MK9's versus /
//                      loading screen where its two pictures are the re-fight's fighters ('refightpics 1'), only the
//                      character select is hidden. This PC's picture alone
//   refightload <ms>   1, and the cover stays that long into the load (100 to 10000; 1500 at the start)
//   refightpics 0|1    0: the versus / loading screen's two pictures are MK9's (the select screen's fighters);
//                      1 (the start): in a re-fight that keeps its fighters they are made of those fighters' pictures.
//                      This PC's picture alone
//   refightjump M F    a look at one PC: asks the game for the script mode F of module M through MK9's own routine
//                      (0x00495A10), as its scripts do between screens; 'refightjump versus.mko versus_mode' is what
//                      MK9's offline rematch asks for (a fight with the fighters and the arena the game holds, and no
//                      character select)
//   refightjump arm M F  the next time a script asks for the character select, the game is asked for M / F instead;
//                      'refightjump arm 0': not any more
//   refightstatus      two log lines
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
// A replay's fighters (net\ReplayKeeper.cpp), game thread: what 'refight direct A B c1 c2' does, from code (colours may be
// null: left as the select screen sets them). False: not the game's names of two fighters, or the game's code for setting
// fighters is not the one this mod knows. DirectEnd: such a pending fight is forgotten ('refight direct 0').
bool Direct(const char* p1, const char* p2, const uint32_t* colours, const char* who);
void DirectEnd(const char* why);
// The Kombat Kodes of the last online fight as the game locked them when its load ended (this player's, the other's).
// False: not seen, or not three digits each.
bool LastFightKodes(uint16_t* mine, uint16_t* theirs);

}  // namespace Refight
