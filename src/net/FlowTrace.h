#pragma once
#include <cstdint>

// The "G2" trace lines (docs\feature_parity_plan.md 4.2 step 1 and section 5, row G2): how MK9 passes the stage and the
// rematch between the two games of an online match, written to MK9Rollback.log so that a normal two-PC evening answers
// the plan's open questions. Logging only: nothing of the game's is written, nothing is read or logged while a
// rollback replays frames, and every line is an event (a call, a message, a fight start), never a frame. Lines are
// written only while the game is in a one-on-one online match: its online session says "connected" (the flag every
// sender of a session message asks for) and the match is one the online mode would take (Online::OneOnOne).
//
//   ONLINEFLOW id N native A call #k of this timeline at gf G (...): <values>
//       the script natives 670, 671, 672 (the players and the stage a script sets), 1178 / 1179 (the selected background
//       index set and read), 1375 (SendGameSettings), 1259 (BeginSyncWait: every sync wait that begins, the rematch
//       vote is type 5) and 1224-1227 (a script sends a message; 9 and 10 with their bytes). RngTrace's native hook
//       calls OnNative after the native returned; 1178, 1179 and 1375 had an ONLINEFLOW line without values before.
//   ONLINEMSG send 9 GameSettings ...      where the game fills the message (0x0087F94C), with its 16 bytes decoded
//   ONLINEMSG received 9 / 10 ...          where the session's handler takes a message's id (0x00887028)
//   ONLINESTAGE at gf G: ...               the stage the fight starts on, its stage table entry, the session's index
//
// The text is made by FlowDecode.h (host-tested, tests\host\flow_test.cpp). A player's name (inside message 10) is
// never written: its length only.
namespace FlowTrace {

// The two call-site patches. Idempotent; a site whose bytes are not the expected ones stays as it is, and the log says
// which lines will be missing.
void Install();
// A traced script native (FlowDecode::Traced) returned, live, on the game thread. callNo: the caller's count of this
// native in this timeline, or 0 to have the call counted here. True: handled here (a line with the call's values, or
// none because they are those of its last line); false: not in an online one-on-one match, nothing read or written.
bool OnNative(uint32_t id, uint32_t fn, uint32_t exec, uint32_t gf, uint32_t callNo);
void OnFightStart(uint32_t gf);  // combat begins (game thread, live)
void NewTimeline();              // GameFrame started over: the counts per timeline do too

}  // namespace FlowTrace
