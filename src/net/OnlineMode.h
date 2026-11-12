#pragma once
#include <cstdint>

// Rollback inside MK9's own online fights (Player Match / private rooms) between two players who both run the DLL and
// named each other ('online <SteamID64> [delay]'). MK9's lobby, matchmaking and fight setup are untouched; only the
// fight's per-frame input exchange is replaced while a rollback session runs.
//
// MK9's own online fight (docs/reverse_engineering.md, "MK9 online fight loop"): every engine tick the connection
// (0x00886880) runs the gamepad exchange 0x00885F80 on its MKOnlineGamepadData (connection + 0x4C3C). That sends the
// local pads with a desync hash (0x00885710), and when the remote input for the frame is missing it pauses the whole
// game (0x00407AB0, "JTLQUEUE PAUSING"); otherwise it writes the remote pads (0x00881360), runs their edges and checks
// the desync hash (0x00881C80, mismatch = disconnect). That is lockstep with a pause.
//
// While our session runs (from the first fight frame, the same lockstep frame on both games):
// - 0x00885F80 is skipped on both games: GGPO over the Steam bridge supplies both fighters' pads instead;
// - the connections' idle timer (0x004F5E10, ms since the last packet) reads 0, so neither game drops the other
//   while the native exchange is quiet; it keeps reading 0 for a grace period after the session.
// When the session ends (round over, peer lost) the native exchange resumes on both games from where both left it.
//
// Engaging needs: an online fight in progress (gamepad data active), a P2P session the GAME opened with the named
// peer, and a hello from the peer's DLL on channel 79 (so a peer without the mod keeps MK9's own netcode) that says it
// is ready to engage too (hello v4), so one game never skips MK9's exchange alone. Ranked opponents are random
// strangers, never the named friend, so ranked fights keep MK9's netcode. A session that stops on a failure ends the
// engagement; MK9's netcode plays the rest of that match, and the rematch tries again.
//
// The hello also carries the build (HelloProto.h: the release stamp of version.txt and EnvFingerprint's content, DLL
// and switches prints), and a game says ready only to the same build, game files, mode and switches. Any other pair
// keeps MK9's netcode for the match and says why once, in the log (the launcher shows it) and in the status line: the
// other player's mod is older (also a release from before this check, whose hello is 28 bytes, or an older protocol
// version), this mod is older, a different build, other game files, other settings. Such a pair used to engage and
// stop at the session's environment check, in mid-fight.
//
// 'online auto [delay]' (kAutoPeer): no named friend. During an online fight the opponent is read from the game's own
// connection: a SteamID64-shaped value (high dword 0x01100001, not this account) in the connection object or the
// online manager with which Steam reports an active P2P session. The hello and the one-on-one Player/Private check
// still decide; ranked (type 1) is never engaged. The found opponent is forgotten when the online match ends.
//
// 'ping' in place of the delay (the launcher's default): the hellos measure the round trip and each game proposes the
// smallest delay it allows (Safe mode 2-3 frames, rollback 2-6); the session runs the larger proposal of the two. The
// hellos keep measuring during the fight, and between rounds (both fighters' input locked) the session's delay follows
// the larger of both games' current proposals.
// A session ends at MK9's results screen (game state 9): the results and rematch screens run on MK9's own netcode, and
// the rematch's fight engages again.
namespace Online {

constexpr uint64_t kAutoPeer = 1;  // not a valid SteamID64

bool Install();
// Arm for this friend; peer 0 = off; kAutoPeer = any opponent running the mod. pingDelay: the delay follows the round
// trip (delay unused).
void Command(uint64_t peer, int delay, bool pingDelay);
void Tick();                             // every engine tick (handshake, arming, diagnostics)
bool SkipNative();                       // the native exchange is replaced right now
int LocalPlayer();                       // 1 or 2: which fighter this game controls in the online fight (0 = unknown)
bool Engaged();                          // armed through the online mode for the current fight
void OnSessionStop(const char* why);     // a P2P session ended: idle-timer grace period; a timeline restart re-arms
// A session frame is about to start, live or replayed, with the game's state as that frame finds it (RollbackManager's
// NetplayFrameEvents): noted for the finisher phase's hand-back to MK9's netcode (FinisherRule.h).
void NoteFrameStart(uint32_t gameFrame);
// Script native 1182 ran live on the game thread: this game finished loading the fight and now waits for the other
// player's load. In an online match the wait's ticks are held to 60 per second from here (OnlineMode.cpp, kLoadState).
void OnLoadedNative();
// A background thread of the online mode is running (unload waits for it): the auto mode's memory pass, the Wi-Fi
// look, EnvFingerprint's build prints.
bool ScanRunning();
void TestScan();                         // 'onlinescan': run that pass now (logs size and time)
const char* StatusText();                // in an online fight on MK9's netcode: why (for the status line); else null
// Connection facts for the status line; game thread, plain reads of values kept up to date elsewhere (the Wi-Fi look
// runs on its own thread once per match, the opponent's side comes with its hellos, the relay state is read from Steam
// with every hello sent, twice a second). All false while unknown.
bool LocalOnWifi();                      // this PC's default route leaves through a wireless adapter
bool PeerOnWifi();                       // the opponent's does (its mod says so; false for a mod that does not say)
bool LinkRelayed();                      // Steam's P2P connection with the opponent goes through Steam's relay servers
// MK9's online connection object (its gamepad data - 0x4C3C) and the size through the end of the gamepad data; 0 when
// no online fight data was seen.
uintptr_t ConnectionAddress(uint32_t* size);

}  // namespace Online
