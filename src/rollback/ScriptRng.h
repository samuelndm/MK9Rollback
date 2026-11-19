#pragma once
#include <cstddef>
#include <cstdint>

// MKScript RandomInt (native 0x008B8790 -> 0x00493C90) draws from UE3's GSRandSeed 0x00E5352C (rngB), the
// stream that particles, FX, sound variation and other UE3 code also advance (about 40 functions). A replay
// that makes a different number of those cosmetic draws (sounds deduplicated, effects spawned differently)
// shifted the script's numbers, and NATIVETRACE showed 0x008B8790 results differing live vs replay
// (padfull_1, 2026-10-18). Two PCs would disagree the same way in lockstep.
//
// While active, the native's call at 0x008B879C goes to a copy of 0x00493C90 that draws from a private
// seed with the same LCG (0x0BB38435, 0x3619636B) and the same executor-state check; likewise the script
// random float (native 0x008B7FD0, call at 0x008B7FF6 -> 0x00887F50, its only caller), same stream. The seed is in the
// snapshot (Tier A "scriptrng"). It is seeded from the gameplay RNG A (0x00E7C054) whenever the pair of
// fighter MK ids changes, i.e. once per fight, identically on both peers; a rollback restores both the
// seed and the key, so it never reseeds.
namespace ScriptRng {

struct State {
    uint32_t seed;
    uint32_t key;  // fighter MK ids the seed belongs to
};

bool Install();  // CodePatch of the call site; idempotent
void SetActive(bool on);
bool Active();
State* StatePtr();  // for the snapshot
uint32_t CurrentKey();  // the fighter key the stream would be seeded for now
// Two-instance tests: seed RNG A, RNG B and the script stream from 'seed' whenever the attract script starts
// (StartAttractMode), so every instance picks the same demo fight. 0 = off.
void SetAttractSeed(uint32_t seed);
void OnFightStart();  // restarts the attract pick index (combat start)
// Two-instance tests on every stage: the attract script's random stage pick (0x00810530, table [0x00E9234C]) returns
// entry 'index' of the stage table instead ('forcestage -1': random again). Needs 'scriptseed' (the pick's hook).
void ForceStage(int index);
// The stage screen (StageFlow), game thread. SetMatchStage: every stage a script or a picker sets from now on becomes
// this stage table entry, until SetMatchStage(-1); it goes before 'forcestage'. False: no such entry (nothing changed).
// WriteStage: the game's own SetSelectedBackground with that entry's name, now. False: nothing was called.
void InstallStageSites();
bool SetMatchStage(int index);
int MatchStage();
bool WriteStage(uint32_t index);
// The fighters of a re-fight (net\Refight.cpp, net\StageFlow.cpp), game thread.
// FightPlayers: the names the game holds for the two fighters of the fight it last set up ("CHAR_JohnnyCage_B": fighter
// and costume), each into a buffer of 'cap' bytes (64 or more). False: not readable, or one of them is empty.
// SetMatchPlayers: armed with two names, every time a script sets the fight's fighters from now on (script natives 672
// and 670) the game's setter runs as the script asked and then once more with this pair; the two fighters the script
// had set are kept. False: the game's code is not the one this was read from (nothing is armed). Null names: disarmed,
// nothing else changes.
// MatchPlayersWritten: armed, and the game was given the pair since.
// KeepMatchPlayers: the pair stands (both games are known to have written the same one); it stays armed.
// UndoMatchPlayers: the pair does not stand: disarmed, and where the game was given it, the game's own setter is called
// with the names the script had passed. True: that call was made. Nothing happens after KeepMatchPlayers.
// KeepMatchPlayersForTest: the armed pair stands whether or not the game was given it yet ('refight direct' in an
// offline game, where no stage screen's exchange can settle it: a look at one PC).
// FightColours: the two players' "other colours" flags of the fight the game last set up (0 or 1 each; the character
// select's START on a fighter: "I choose the johny cage skin pressing start, it is a black suit, the re-match is
// getting the blue one", 2026-11-16). They are not part of the fighter's name: the select screen's script writes them
// into each player's record itself, just before it sets the fighters (ScriptRng.cpp has the reading). False: not
// readable. SetMatchPlayers' `colours` (may be null): the two flags the game is given back with the pair.
bool FightPlayers(char* p1, char* p2, size_t cap);
bool FightColours(uint32_t out[2]);
bool SetMatchPlayers(const char* p1, const char* p2, const uint32_t* colours = nullptr);
bool MatchPlayersWritten();
void KeepMatchPlayers();
void KeepMatchPlayersForTest();
bool UndoMatchPlayers(const char* why);
uint32_t PlayerCalls();  // how often a script has set the fight's fighters through the patched calls
// The pictures of a re-fight's versus / loading screen (ScriptRng.cpp has the reading of the game). MK9's versus /
// loading screen shows copies of the select screen's two body pictures, which the select screen's script makes just
// before it asks for the fight. While an armed pair stands, each copy is made of the body picture of the pair's fighter
// instead, where that picture is in memory.
// MatchPictures: bit 0 / bit 1, the left / right copy was made of the pair's fighter since the pair was armed (0 for a
// pair that does not stand). SetMatchPictures(false): the copies are MK9's own again ('refightpics 0').
uint32_t MatchPictures();
void SetMatchPictures(bool on);
bool MatchPicturesOn();
uint32_t PictureCalls();  // how often a script has asked for a named copy of a picture through the patched call
// A re-fight without the character select (a test, 2026-11-15). MK9's scripts change screens through script native 171,
// which asks the engine for a script's function by name. Armed with a module and a function, the next request of a
// script for the character select ("PlayerSelect.mko", "PlayerSelect") is turned into that one; every request is
// logged from the first arming on. MK9's own offline rematch asks for ("versus.mko", "versus_mode"): a fight with the
// fighters and the arena the game holds. (nullptr, nullptr) disarms. False: the call could not be patched or a name
// does not fit.
bool SetModeJump(const char* module, const char* function);
bool ModeJumpTaken();  // the armed request has been used since the last arming
// For log lines: the value at +0xB40 of player 0 or 1's record. The game's fighter setter copies it into the fighter's
// record (+0xA00) next to the name; what it means is not known (2026-11-14), so a re-fight leaves it as it is.
uint32_t PlayerRecordValue(int player);
void SetPickVariety(uint32_t v);  // 'pickvariety N': other attract fighters for the same scriptseed
void LogStages();  // 'stages': the table's entries (index, name, selectable flag, attract exclusion)
// Read-only looks at the stage table for trace lines (FlowTrace): its number of entries; the name of entry 'index'
// (the game's own text, null when there is no such entry or it cannot be read); the entry whose name is 'name', letter
// case ignored (-1: none).
uint32_t StageCount();
const char* StageAt(uint32_t index);
bool StageSelectable(uint32_t index);  // the entry carries the table's "selectable" mark (the 'stages' line shows the others as "(off)")
int StageIndex(const char* name);

}  // namespace ScriptRng
