#pragma once
#include <cstdint>

#include "ReplayFile.h"

// Replays of online matches, game-thread side (net\ReplayFile.h has what a replay is, the file's bytes and the
// comparison; rollback\RollbackManager.cpp records the session and drives the playback).
//
// Recording: every rollback session that began at a fight's first frame with the netplay start (each online match, and a
// test session armed with 'ggpo netplay') is recorded into memory set aside once (about 1 MB; nothing is allocated or
// written while it runs): both fighters' masks of every frame and every frame's gameplay checksum, as the session
// settled them (a rollback's replay of a frame overwrites its live values), the state of every 60th frame, and the setup
// read at its start (fighters and colours, arena, Kombat Kodes, the side this game played, the input delay). At the
// first engine tick after the session in which no rollback session runs, it is written to
//   <the DLL's folder>\replays\<date>_<time>_<P1>_vs_<P2>.mk9r
// (about 110 KB for a 140 s match) and a "REPLAY written" line says so. Sessions shorter than a second are not kept.
//
// Playback, in an offline game ('netcode rollback' on, no online match): 'replay last' or 'replay <file>' reads the file,
// arms 'refight direct' with its two fighters and colours (the next character select is covered and confirmed by the
// mod, and the fight that loads has the recording's fighters) and gives every stage a script sets the recording's arena.
// The next fight that begins is played back (RollbackManager.cpp): it starts as an online session starts, both fighters
// follow the recording frame by frame, and each frame's checksum is compared with the recording's. "REPLAY" lines say
// where the fight went out of step, if it did, and the VERDICT at its end. The Kombat Kodes are not given to an offline
// fight (they live in MK9's online session): a recording made with kodes is said to be so.
//
// Commands:
//   replay last            arm the playback of the newest file in the replays folder
//   replay <file>          ... of that file (a name in the replays folder, ".mk9r" may be left out, or a full path)
//   replay stop            disarm / end the playback; the fighters and the arena go back to the scripts
//   replay status          the playback's and the recorder's state
//   replay list            the newest files, with what they hold
//   replay record 0|1      record sessions (1 at the start)
//   replay like 0|1        1 (the start): a playback's frames run as a session's do (effect actors' deaths deferred,
//                          freed blocks quarantined); 0: as an offline fight's
namespace ReplayKeeper {

void SetFolder(const wchar_t* dir);  // the DLL's folder; the files go to its replays\ subfolder
void Tick();                         // game thread, every live engine tick: writes a finished recording when no session runs
bool Command(const char* line);      // true: the line was one of this file's commands

// The recorder (RollbackManager.cpp, game thread). RecBegin: a P2P session started (this game is paused at its first
// fight frame, waiting for the other game); recorded only when fromStart. testMeter / testLife: the test settings
// 'netplaymeter' / 'netplaylife' (< 0 off), which a playback applies again. RecInputs: frame n's two masks (live, or a
// rollback's replay of it); startGf: GameFrame of frame 0. RecChecksum: the gameplay checksum at the start of frame n,
// with the state on every 60th frame. RecEnd: the session stopped.
void RecBegin(uint32_t localPlayer, uint32_t delay, uint32_t netplayGf, bool fromStart, bool online, float testMeter, float testLife);
void RecInputs(uint32_t frame, uint32_t startGf, uint32_t m1, uint32_t m2);
void RecChecksum(uint32_t frame, uint32_t cs, const ReplayFile::State* s);
void RecEnd(const char* why);

// The playback (RollbackManager.cpp): its frame 0 has begun (the fight is loaded: the scripts get their arena back), it
// is over.
void OnPlaybackStart();
void OnPlaybackEnd(const char* why);
void Shutdown();  // the DLL unloads: a replay being written is finished first

}  // namespace ReplayKeeper
