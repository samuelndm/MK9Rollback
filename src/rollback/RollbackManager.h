#pragma once
#include <cstdint>

// Milestone 10: local rollback without networking.
//
// Frame boundary = entry of engine vt+0xBC (0x0080DD40, the UE3 world tick), i.e.
// before frame F's world tick dispatches any async work and before its scripted input
// is applied (the hardware sampler has already run). Each frame in rollback mode:
//   OnTickEntry   [vt+0xBC entry] : (maybe) roll back + resimulate, then save snapshot[gf]
//   world tick runs
//   (CommandChannel::RunFrame at vt+0xC0 entry applies this frame's scripted input)
//   RecordInputs  : remember the masks applied to overridden pads for gf
//   EngineTick (vt+0xC0) runs
// A rollback to S = F - k loads snapshot[S] (churn-aware), then for g = S..F-1:
//   save snapshot[g], apply recorded/corrected inputs for g, call vt+0xBC(dt) and
//   vt+0xC0(dt) (the pair TimedEngineFrame 0x004025A0 calls), flush object events, then
//   advance HostFrame/GameFrame exactly as the main loop does.
namespace Rollback {

extern bool gIsResimulating;
void SetBetweenProbe(int frames);  // command "betweenprobe N": fighter bytes changed between ticks
void BetweenProbeExit();  // live EngineTick exit
void SetDeepDiff(bool on);  // 'deepdiff 0|1': MK master image live vs replay at FightUpdate entry / EngineTick exit
void SetDeepEvery(bool on);  // 'deepdiff 2': every load's first replayed frame, reports of 8+ dwords
void OnFightUpdateEntry();
void OnEngineTickExit();
void OnFightUpdateExit();  // deep diff point after the fight update (scripts)  // same capture after the engine tick (live: hook; resim: ResimFrame)  // bone-state hash per pass (FightUpdatePlayers hook)
void BoneHashDump(uint32_t gf);
void SetAnimTagFix(bool on);
void SetTwice(bool on);
void SetResimSleep(int ms);
void SetLazySave(bool on);       // 'lazysave 0|1': P2P live save after the boundary's first poll (default 1)
void SetSkipConfirmed(bool on);  // 'skipconfirmed 0|1': replays skip saves of frames no rollback can load (default 1)
void SetSparseSave(bool on);     // 'sparsesave 0|1': P2P replays skip saves of odd frames; GGPO loads the frame before (default 1)
void SetSparseLive(bool on);     // 'sparselive 0|1': P2P live saves of odd frames skipped too (needs sparsesave; default 0: lab)
void SetTickSpin(int ms);        // 'tickspin N' (test): N ms of busy CPU per live and replayed P2P frame
// 'xraylock 0|1', 'xraylockcine 0|1', 'xraylockwindow N', 'xrayfreeze 0|1' (< 0 / 0: unchanged)
void SetXrayLock(int on, int cine, int window, int freeze);
void SetStallHold(int minMs, int maxMs);  // 'stallhold 0' off; 'stallhold MIN MAX' (default 60 150): even the games after a stall
void SetFreeze(int ms, int everyMs);      // 'freezeevery MS EVERY' (test): this game stops MS ms every EVERY ms in a P2P session
void SetDesyncStop(int frames);  // 'desyncstop N': an online match's session stops after N differing checksum frames in a row (default 120, 0 never)
void SetHandshakePoll(bool on);  // 'handshakepoll 0|1': the paused wait for a session polls GGPO every millisecond (default 1)
void SetPhaseGrowAfter(int decisions);  // 'phasegrow N': the phase search's step grows from decision N+1 in a row with one leader (default 2; 1 = rc27)
void SetWaitShift(bool on);      // 'waitshift 0|1': a boundary that waited for the peer moves the frame deadline by the wait (default 1)
void SetStartBarrier(bool on);   // 'startbarrier 0|1': rollback frame 0 waits for the peer's first input (default 1)
void SetEarlyInput(bool on);     // 'earlyinput 0|1': P2P local input added and sent at the boundary's start (default 1)
void SetLateSample(bool on);     // 'latesample 0|1': P2P controllers sampled at the boundary, after MK9's frame wait (default 1)
void SetSkipLive(bool on);       // 'skiplive 0|1': no live save for a frame whose remote input is already in (default 1)
void SetCanonClock(bool on);     // 'canonclock 0|1': world clocks set to the same value at the netplay start (default 0)
void SetBalance(bool on);        // 'balance 0|1': frame-advantage balancing on top of GGPO's time sync (default 1)
void SetBalanceTuning(int settleFrames, int samples, int maxGive);  // 'balancecfg S N G' (default 24 4 3)
void SetQualityInterval(int ms); // 'qualityms N': GGPO quality report period (default 200 ms, stock 1000)
void SetRunningRetry(int ms);    // 'netretry N': unacked inputs resent after N ms without a new peer frame (default 34, stock 200)
void SetFirstInputRetry(int ms); // 'firstretry N': until the peer's first input arrives, inputs resent every N+1 ms (default 3, 0 = off)
void SetRenderLate(int pct);     // 'renderlate N': a frame starting > N% of a period late is not rendered (default 50, stock 5)
void SetPaySmooth(int ms);       // 'paysmooth N': time-sync debt paid by moving the frame limiter deadline N ms/frame (default 2; 0: waits)
void SetPaySmoothSmall(int ms);  // 'paysmooth N S': debts of up to 2 frames paid S ms/frame (default 1; 0: as larger ones)
void SetGgpoTimesync(bool on);   // 'ggposync 0|1': pay GGPO's own time-sync requests although BALANCE is on (default 0 since rc25)
void SetRepoll(bool on);         // 'repoll 0|1': one more poll after a live save made before the frame's remote input (default 1)
void SetPhaseSync(bool on);      // 'phasesync 0|1': sub-frame phase alignment with the other game (default 1)
void SetFastReadable(bool on);   // 'fastreadable 0|1': readability checks by guarded reads instead of VirtualQuery (default 1)
void SetQuarantineMargin(int frames);  // 'qmargin N': frees kept N frames beyond the snapshot ring (default 8)
void SetTickParity(bool on);
void SetDeferOnlyGgpo(bool on);
void SetIdleSaves(bool on);  // 'idlesaves 0|1': snapshots and audits every tick without a session (default 0)
void SetPhaseAudit(bool on);  // 'phaseaudit 0|1': heap/list audits at the fight update and engine tick boundaries
void FightTickDone(bool fought);  // every tick after the fight update call (releases deferred actors when the fight is over)  // 'deferggpo 0|1': actor-destruction deferral only during GGPO sessions
void SetCompSync(bool on);
void SetAttachAll(bool on);  // 'attachall 0|1': fighters' whole attachment lists reconciled at load  // 'compsync 0|1': fighters' components re-placed from the restored actor after loads  // 'tickparity 0|1'
void SetMkAttach(bool on);  // 'mkattach 0|1': re-attach given-back MK object actors to their saved base at load
bool GgpoActive();  // a GGPO session (SyncTest or P2P) is running
bool NetplayRunning();  // a P2P session is running (both peers past the start handshake): no local pause allowed
bool SetSessionDelay(int delay);  // the running P2P session's local input delay (frames); false without one
int SessionDelay();               // that delay now (0 without a running P2P session)
bool NetplayStarting();  // paused at the first fight frame, waiting for the peer's session
bool NetplayArmed();     // a netplay session starts at the next fight start
void NetplayDisarm();    // cancel that (online mode: the match is over)
void NetplayStop(const char* why);  // stop a running P2P session (the online mode disengaged)  // 'resimsleep N'
void SetForceLod(bool on);  // 'forcelod 0|1': fighters' skeletal LOD forced to 0 (render-independent bone set)  // 'rbtwice 0|1': replay each local rollback window twice and compare  // clamp unrestored anim-node tick tags at load (command "animtagfix 0|1")
extern bool gDiag;  // heavy per-rollback / per-frame diagnostics (command "diag 0|1")

using TickFn = void(__thiscall*)(void* engine, float dt);
void SetOriginalTicks(TickFn worldTick, TickFn engineTick);

void Enable(bool on);
bool Enabled();
void SetScriptRng(bool on);  // 'scriptrng 0|1': script RandomInt on a private snapshotted stream (ScriptRng.h)
void SetCrowdRng(bool on);   // 'crowdrng 0|1': background crowds' rand(n) off the gameplay RNG (CrowdRng.h, default 1)
void SetNetplayFromLoad(bool on);  // 'netplayload 0|1'
void SetNetplayDemoFights(bool on);  // 'netplaydemo 0|1': 1 lets a CPU-vs-CPU fight (attract demo) start a session (tests)
void SetQuarantineMenus(bool on);  // 'qmenus 0|1': free quarantine outside sessions too (pre-fix, for the menu freeze test)
void SetNetplayAutoRearm(bool on);  // 'netplayrearm 0|1': re-arm at a timeline restart (the online mode's path)
// Armed after this timeline's restart but before its first fight frame: start at that frame instead of the next timeline's.
// Returns whether a session now waits for a fight start.
bool NetplayAwaitThisTimeline();
void SetNetplayGf(uint32_t gf);    // 'netplaygf N' (0 = off)
uint32_t NetplayGf();
void SetNetplayMeter(float value);  // 'netplaymeter F' (test): both meters = F at frame NetplayGf()+8 of local sessions; < 0 off
void SetNetplayLife(float value);   // 'netplaylife F' (test): both fighters' life = F at the same frame; < 0 off
void LogEventListeners(const char* tag);  // 'listeners': the script event listener list 0xE0B4F0

// Game thread. Returns true if a rollback+resimulation happened this tick.
bool OnTickEntry(void* engine, float dt);
void RecordInputs();
// Game thread, after the fight update: really destroy actors whose deferral window has passed.
void ReleaseDeferredActors();
// Diagnostic: log the spans of the newest snapshot that contain addr.
void LogSpansContaining(uint32_t addr);

// Requests (executed at the next OnTickEntry).
void RequestRollback(int frames);
// Diagnostic: restore snapshot F-frames and stop there (no resimulation).
void RequestLoadOnly(int frames);
// Pretend P1's input for frames [F-frames, F-frames+count) was `mask`, then roll back.
void RequestFakeRollback(int frames, uint32_t mask, int count);
// GGPO SyncTest session (Milestone 12, no network): N > 0 starts one at the next frame boundary with
// check distance N (rolls back N frames every N frames and verifies gameplay checksums); 0 stops it.
void GgpoCommand(int checkDistance);
// Lockstep (delay-based) netplay instead of rollback for the next P2P session: no prediction, no snapshots.
void SetLockstep(bool on);
bool Lockstep();
void SetFixSeed(uint32_t seed);
void AbTest(const char* name);  // in-fight A/B of a load-side toggle across alternating GGPO loads ('off' ends it)  // test: both RNGs from a fixed seed at every timeline restart (0 = off)
// GGPO peer-to-peer session (Milestone 13), started at the next frame boundary; 'ggpo off' stops it.
void GgpoP2PCommand(unsigned short localPort, const char* ip, unsigned short remotePort, int player, int delay);
// Netplay: like p2p, but started automatically at the next fight start with a pause/seed/handshake.
void NetplayArm(unsigned short localPort, const char* ip, unsigned short remotePort, int player, int delay);
// Milestone 15: P2P / netplay over the Steam P2P bridge to the peer's SteamID64 ('ggpo steam|steamnetplay <id> <player> [delay]').
void GgpoSteamCommand(uint64_t peer, int player, int delay, bool netplay);

}  // namespace Rollback
