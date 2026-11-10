# MK9Rollback to-do

Last updated 2026-10-31. **The latest zip for players is `dist\MK9Rollback-netplay-20261031-0159.zip`** (DLL
5CC62578…, Safe mode default, the Safe mode gate passed: see 2026-10-31 below). Fallback: 2145 (DLL 63FE4D67…, the
rollback gate passed, see 21:10-21:45 below; it has the Safe mode disconnect crash). The rest of this paragraph is history: 1757 (DLL SHA-256
48FF0F5D…EC131) was 2348 plus the in-game status line (item 4) and mkattrec (item 8). Checks, made quickly for the
2026-10-29 18:00 friend test: online smoke passes (the build before the overlay's check change), the 3-fight
delay-2 two-instance regression passes (6 of 6 reports, 0 mismatches, 0 failed saves, no cut session, 0 crashes,
5,907 overlay draws, none in a replay, max 0.8 ms). The auto re-arm regression passed on the 14:32 build
(mkattrec, no overlay), not rerun on 1757. Fallback: `dist\MK9Rollback-netplay-20261028-2348.zip` (full checks;
pending-release fix, 0140's lag fixes, frame-log rotation, NETSTATS, delay 2). 1259 is 2348's DLL with delay 1.
Do not use `dist\MK9Rollback-netplay-20261028-0107.zip`: its regression had a cut session (see item 7).

**Latest zip for players (2026-11-03): `dist\MK9Rollback-netplay-20261103-2313.zip`** (DLL EE8A69D4…, rollback gate
and lag_ns80_d2 passed; fixes below).

**2026-11-04 scenario simulation (all stages, simulated lag, frame pacing).** Two-instance runs at netsim 30 (ping
65-145 ms) and delay 2, every stage forced in turn (`forcestage`, `stages`), fighters varied (`pickvariety`), both
games' frame pacing logged per session (`PACE session`: frame and present times, >=34/50 ms counts, boundary cost per
part, rollback depths; `src\rollback\FramePace.cpp`). Tools: `tools\scenario_matrix.ps1 -Set stages|network|cpu`
(CSV per run in `results\scenarios`), `two_instance.ps1 -Stages -VaryFighters -NetLoss -NetStall -TickSpinB -PreSetup`.
- **Sweep 1 (old build, `results\scenarios\sweep1_*`, 17 stages before it was stopped):** one game rolled back almost
  alone in each fight (GorosLair 111/1, Graveyard 116/0, FleshPits 0/113) and every rollback was a >= 34 ms frame
  (74-119 per 34 s fight on the leading side, ~51 presents/s on Courtyard); every start froze one game ~0.7 s; 9 of 16
  stages desynced.
- **Fixes (01:57 build; 02:20 adds the four gate items below):**
  - Load ~46 ms -> ~8 ms: RepairAttachBase reads through CrashLog::SafeRead32 with a per-class cache (the attach
    phase made a VirtualQuery per GObjObjects entry, 31.8 ms); LoadAB caches regions while it holds the heap locks
    (`MemQuery::LockedScope`); SSE2 quarantine requeue scan; `RecentlyReleased` stops at the loaded frame.
  - Live save: deferred to the boundary and skipped when a replay saved the frame (`lazysave`); replays skip saves of
    frames no rollback can load (`skipconfirmed`, patched GGPO `ggpo_rollback_safe_frame`); the skel phase checks reads
    with guarded loads instead of VirtualQuery (`fastprobe`; 760 calls per save -> 0, Courtyard 7.1 -> 5.5 ms per
    frame, light stages ~3.5 ms). `SAVE SKEL` / `LOADAB PROFILE` / `LOAD PROFILE` lines show the parts.
  - Start: GGPO's first sync request went to a port not yet bound and was retried after 500 ms, so one game reached
    'running' ~0.8 s before the other. Patched udp_proto.cpp: the peer's first sync request re-sends ours at once
    (both now within ~40 ms). Rollback frame 0 also waits for the peer's first input (`startbarrier`).
  - Frame-advantage balance (`balance`): GGPO's time sync only acts at >= 3 frames with idle input; now a game whose
    sampled advantage says >= 1 frame ahead gives 1-3 frames (`BALANCE` lines). Rollbacks are shared between the games.
  - Background crowds off the gameplay RNG (`crowdrng`, `src\rollback\CrowdRng.cpp`): the stage crowd manager (script
    natives 0x008DB5C0, 0x008BB660, 0x008BB680; 18 rand(n) sites in 0x008F9260-0x008FB62C) drew RNG A, and its phase
    against the fight start differed between the games (Courtyard: 4 extra draws at frame 0 on one peer, lab trace).
    Its draws now come from a hash of (GameFrame, draw index); part of the environment fingerprint.
- **After the fixes (`results\scenarios\fix1_*`, `rng1_*`):** Courtyard 15/10 frames >= 34 ms (was 74/71), 57.5
  presents/s; GorosLair 10/7 (was 109/8), rollbacks 65/53; PitDay p99 16 ms, 62 presents/s; Graveyard and PitDay
  0 mismatches (both desynced before).
- **Left (harness only, as far as known):** attract-demo sessions start each game at its own first fight frame, so
  stage scripts run out of phase (Courtyard: a stage script's `random` 0x008B87C1 a frame apart, fighters identical,
  only RNG A differs) and Skarlet's intro height differs at frame 0. In real online matches MK9 keeps both games on the
  same frames before the fight (field test: both first fight frames at gf 359), and the field test had 0 mismatches.
  `rngtrace 1` (before netcode) logs the RNG A callers of the 3 frames before the first mismatch at full speed.
- **Gate on the 01:57 build (`results\gate_20261104_rollback.txt`): no zip.** Online smoke, delay changes and the
  lockstep checks passed. Plain regression: A crashed replaying gf 101771 (PitDay, Reptile v Kitana): a call through an
  FXLightComponent (0x3AAFD600) whose block, freed at 101759, the quarantine released at the end of 101771's boundary;
  the next boundary loaded S=101771, whose state still pointed at it. Auto re-arm: one "failed save", the fight's last
  frame refused with "object heap not discovered" (map teardown). Menu walk: "MKKE window is not in the foreground"
  (run with -Now right after input; Windows' foreground lock).
  - 02:20 build: freed blocks stay quarantined 8 frames beyond the ring (`qmargin`); teardown refusals are not counted
    as failed saves; the attach repair's per-class answers are dropped at each timeline start (a reused class address
    could make it zero +0x60 of a non-actor); the balancer samples every 10 frames, decides after 6 samples, settles
    45 frames and pays every 10th frame while 2+ frames are owed (rng1 Courtyard: B drifted ~0.4 frames/s ahead and
    stayed 2-3 frames ahead with depth 4-5 rollbacks). Gate rerun queued without -Now (waits for an idle PC).
- **02:20 build, plain regression x3 (`results\regress3_20261104.txt`): 2 of 3 passed.** r1 crashed the same way at
  the same scripted inputs (replay of gf 101894 right after a load to it, 0 mismatches before). It was not the
  quarantine: eax = 0x00D46AA0 is UObject's own vtable, which ~UObject (0x005F8850) writes before clearing the
  GObjObjects slot (the crash dump's `gobj=OTHER`), so the component had been garbage-collected and destructed; the
  released block "below it" was an unrelated temp array. The path: the light effect's destructor 0x0043D130 (MK class
  vtable 0x00C671D0, built by 0x0043D010 on the MKObj constructor 0x004396E0, which spawns its actor into obj+8) reads
  actor+0x1CC, sets the brightness and calls 0x0078F1C0 -> FComponentReattachContext 0x0078BF50 -> vt+0xE8, past the
  end of the 49-slot UObject vtable (eip = the float 0x3C40C0C1 stored after it). The load had brought the effect back
  from the MK heap image holding an actor whose light component no longer existed (the fighter's attachment list had
  3 entries at the save and 0 live, `ACTORARRAY ... actor0 +0x68` before both crashes).
  - Guard (`src\rollback\ActorGuard.cpp`): at 0x0043D176 a collected actor or component reads as none (no brightness
    change or reattach, render state only); at 0x00439A36 (MKObj base destructor) a collected obj+8 actor reads as none,
    so the un-root, DestroyActor and +0x9C back-reference reset are skipped. Both are no-ops in normal play. `ACTORGUARD`
    lines name the object, actor and component; `ACTORDEFER ... vanished before the load` is now logged too. Still to
    learn from the next hit: which path let the actor or component be collected inside the rollback window.
- **`netplayload 1` intro crash, explained and fixed (`results\scenarios\load1_B.log`):** the script module link
  0x0045CE90 runs once per module (flag +0x5C) and turns each import entry's name offset into a pointer (entry+4 +=
  [module+0xA8] - 1 at 0x0045CEF9). The module object is in the restored mkmaster span; its import table is in MK heap #2
  (0x117AFFC0, below that span). A load to a frame before the link restored the flag to 0 but not the entries, the link
  ran again and added the base twice: `_stricmp` read 0x22F6621B (about twice the table address). The add is now
  idempotent (`src\rollback\ScriptLinkGuard.cpp`, `SCRIPTLINK` lines): an entry still holding the pointer the guard
  produced is left alone. Any session in which a module links inside the rollback window could hit this, not only the
  test-only `netplayload 1`. The GGPO stop line now counts the guards' hits.
- **7A13579D (both guards), plain regression x3 (`results\regress3b_20261104.txt`): 0 crashes, 0 guard hits, 2 of 3
  passed.** r2 fight 2 (PitDay, Reptile v Kitana) desynced at frame 683 and stayed desynced (683 matched, 1254 not):
  same RNG, life and meter, P2 y -109.406 on A vs -106.781 on B. Two frames before, A's load to S=100682 logged
  `COMPKEEP fightercomp +0x74 / +0xC0 kept live: saved 3F800000 live BF800000` and `+0x608 saved 3F800000 live
  3F7FFFFF`: a UObject sat at 0x3F800000, so the saved 1.0f passed the pointer test (committed, target starts with a
  vtable) and the fighter component (SkeletalMeshComponent) kept the abandoned timeline's -1.0 (the fighter had turned
  around in the window). Those offsets are not declared properties, so the reflection mask could not restore them. Only
  occurrence of these offsets in all logs. **Fix (`complivedata`, default 1, in the environment fingerprint):** a
  fighter-component dword kept for its saved value is restored by value when its live value is nonzero and fails the
  pointer test (a real pointer field holds a pointer or null); `COMPLIVEDATA` lines. Plain regression x3 on it
  (`results\regress3c_20261104.txt`): 3 of 3 passed, 0 mismatches, 0 crashes; the rule did not fire (the other
  COMPKEEP lines are the +0x58 pointer and the +0x3B4 array, whose live values are pointers or null).
  - Reflection mask check (`REFLECT   <class>: field list stopped at ...` lines, 10:09 build): every class level's
    field list ends with its structs/enums (`ScriptStruct`, `Enum`, 1-2 fields) and 0 declared data dwords follow the
    stop, so no declared property is left to the pointer heuristic. +0x74/+0xC0/+0x608 are native-only fields.
- **Time sync payments were mostly undone by MK9's frame limiter.** TimedEngineFrame 0x004025A0 waits (Sleep(0) until
  4 ms before, then spins) for the deadline double 0xE0AB10, then sets it to now + fmod(deadline + period - now,
  K periods), K = 2.0 (0xC5A6A8, flag 0xDECAFC = 1 in the exe; 32.0 otherwise). The grid is fixed: a frame that ends
  late makes the next ones start without a wait until the grid is caught up (up to one period). The 16 ms payment wait
  inside the tick therefore made one 20-24 ms frame and a short one and gave the peer little or no time: r2 fight 1
  gave >= 21 frames (balance + time sync) but lost 14 grid slots in 33.0 s (1966 frames), and A stayed ~4 frames ahead
  with 5-deep rollbacks. **`paysmooth N`** (10:36 build, default 0 until the A/B): the debt is paid by adding N ms to
  the deadline per frame (2 ms: frames of 18.7 ms, 1/60 s paid every ~8 frames); the GGPO stop line reports how far
  the deadline was moved.
- **net1 (network matrix, 10:09 build, defaults; `results\scenarios\net1*`):**
  - Every fight, even LAN with 0 rollbacks, had ~8 frames >= 50 ms (75-100 ms max): exactly 2 per 600-frame PACE
    window, inside the boundary (boundary max ~56 ms). Two jobs ran every 600 frames inside it: the tier E rebuild
    (BuildTierE: every UObject's class name formatted up to three times) and FpuGuard::PinAllThreads (a system-wide
    thread snapshot, every game thread suspended for its context; it never corrected a game thread in any log).
    **Fix (11:35 build):** the rebuild looks each class up once (per-class kind cache, same result); the all-thread
    pin runs once when a session waits for its peer and not while it runs (`fpupinsession 1`: as before). `HITCH` lines
    now log every interval >= 40 ms with the boundary's parts and the periodic jobs that ran in it.
  - Single 1-3 s frames (ns30_d2_loss5, ns60_d2_jittery): all in the boundary's wait, in sessions 2-3 s longer than
    the others: GGPO's start handshake retried a lost sync packet after SYNC_RETRY_INTERVAL = 2000 ms, so one game waited
    at frame 0. **Fix (libggpo 11:38):** 400 ms, and a reply to the request a retry replaced still counts (round trips up
    to ~800 ms complete).
  - DeadPool (Noob v Kano) desynced in 4 of 8 scenarios, each at frames 164-232 (the end of the round intro): two with
    only RNG A a frame apart (A's checksum at 193 = B's at 192), two with P2's position one walk step apart. The same
    start-phase artifact as the Courtyard case above (the attract demos start each game at its own first fight frame);
    to be checked with map-load starts (`-FromLoad`, MK9's own online start). PitDay (Skarlet) stopped at frame 0 in 3 of
    8 (known harness artifact). No COMPLIVEDATA or guard hit in any net1 log.
- **net2 (same build, `paysmooth 2`; `results\scenarios\net2*`), per game-minute over all 8 scenarios:** frames
  >= 34 ms 33.4 -> 21.4, presents >= 34 ms 39.3 -> 26.7, frames >= 20 ms 98.6 -> 75.8, rollback depth 2.32 -> 1.96
  (ns60_d2 2.82 -> 2.00, ns60_d2_jittery 3.01 -> 2.11); LAN loads 199 -> 102 and shared by both games. The deadline
  moved 34-152 ms per LAN fight, matching the frames owed. **`paysmooth 2` becomes the default.**
  - **Game closed: GGPO assertion sync.cpp:238** (ns30_d2_loss5 fight 3, game B). A's start handshake lost its sync
    packets (2 s retry) and A never sent an input; B ran, logged "the peer stopped responding" and at GGPO's 3 s
    disconnect DisconnectPlayerQueue rewound to A's last frame, -1: FindSavedFrameIndex found nothing and asserted.
    **Fix (libggpo 12:31):** the rewind only runs to a frame still saved (Sync::HasSavedFrame); the session then stops
    on the disconnect event as before.
  - **Open: a mid-fight desync** (ns60_d2 fight 1, Graveyard, Jade v Reptile, frame 1665 of 1942): RNG equal, P2 y
    74.266 vs 75.912 and P2 meter 0.7697 vs 0.7907 (Reptile acted differently). A loaded S=101664 two frames before it
    (MKATTACH re-attached actor 39D0B2C0 to its saved base 0FF00000, PENDREL released/removed two blocks); B had loaded
    S=101653 (MKATTACH too). Not reproduced yet; next: a SyncTest soak on Graveyard.
- **net3 (11:39 build: tier E class cache, no all-thread FPU walk in sessions, 400 ms sync retry, HITCH lines;
  `paysmooth 2`; `results\scenarios\net3*`), per game-minute (sessions that ran):** frames >= 34 ms 23.2 -> 10.2,
  >= 50 ms 15.4 -> 4.8, presents >= 34 ms 27.6 -> 12.1, depth 1.97 -> 1.72. Against net1 (this morning's defaults):
  -69%, -70%, -69%. HITCH lines (137): 35 at the demo's timeline restart (session end, harness), periodic jobs 2 (one
  tier E, one memory report, 41 ms each), barrier waits 34 (mostly the start barrier at frame 1: 0.1-0.2 s at 60-100 ms,
  0.65-1.0 s with loss/jitter), rollbacks 27 (median 43 ms), and ~25 outside the boundary in pairs on both games at the
  same fight frame (52-68 ms, e.g. Graveyard frame 961-996 every scenario: the game's own content loads at a scripted
  move, as far as known). ns100_d3 fight 1: four ~1 s stalls on both games at the same wall-clock instants, outside the
  boundary (the PC, not the netcode); game B's attract loop then left the demo (online natives, no fight for 40 min: a
  harness failure; its other sessions show "peer did not join" and are left out). Desyncs: DeadPool intro only (frames
  161-192, the same checksums B6434367/022B9CE4 as in net1, which game is a frame early varies), PitDay frame 0 in 4 of
  8 (harness); no mid-fight desync.
- **net4 (12:32 build, `paysmooth 2`, `qualityms 200`, `balancecfg 24 4 3`; `results\scenarios\net4*`):** against net3
  without ns100_d3 (net3's had the PC-wide stalls): frames >= 20 ms 54.8 -> 45.2, >= 34 ms 6.5 -> 5.1, >= 50 ms 4.4 ->
  3.4, presents >= 34 ms 9.6 -> 7.8 per game-minute, rollbacks 144 -> 129/min, depth 1.60 -> 1.49 (with ns100_d3:
  F34 10.2 -> 6.1, P34 12.1 -> 8.4). Better in 6 of 8 scenarios; ns60_d2_jittery worse (F20 118 -> 166, P34 8.1 ->
  13.4). 0 crashes, asserts or disconnects; desyncs: DeadPool intro (frames 147-192) and PitDay frame 0 only.
  **Defaults since the 14:59 build: `paysmooth 2`, `qualityms 200` (libggpo), `balancecfg 24 4 3`.** Overall, this
  morning's defaults (net1) to now: frames >= 34 ms 33.4 -> 6.1 per game-minute, >= 50 ms 15.9 -> 3.5, presents
  >= 34 ms 39.3 -> 8.4, rollback depth 2.32 -> 1.70.
- **DeadPool intro desync with map-load starts (`results\scenarios\load2*`, 14:59 build, `-FromLoad`, 3 x DeadPool):
  not conclusive.** Both sessions that ran desynced at frame 321/363, but at frame 321 game A's fighters were placed
  (-125/+125) while game B's were still at 0,0,0: the harness's map-load start lets each game finish its (wall-clock)
  loading on its own frame, which MK9's own online start does not (both peers wait for each other's load). So neither
  harness start reproduces the real one. Evidence that the real start keeps the intro in phase: the 2026-11-03 two-PC
  field test (both first fight frames at gf 359, 0 mismatches in 19k frames). Still to do: a two-PC DeadPool match.
  The map-load harness also lost sessions ("peer did not join" on A, "peer disconnected" on B).
- **cpu1 (14:59 build, defaults; `results\scenarios\cpu1*`, ns30 delay 2, game B slowed):** spinB6 (6 ms of extra CPU
  per B frame): B 59.7 frames/s, 55-58 presents/s, frames >= 34 ms 1-5 per fight; spinB10: B still simulated 59.3-59.6
  frames/s but presented only 6.5-46/s; slowB (background window): B 26-36 presents/s. Desyncs: PitDay Skarlet intro
  height (frame 9, 0.252 vs 0.250) and DeadPool intro (frame 240), harness as above; no crash.
  - Why so few presents: the limiter marks a frame late when it starts > 0.05 of a period (0.83 ms) after its deadline
    (0x004026B9: mulsd [0x00C5A6B8 = 0.05]; flag 0xE844A5), and 0x00666DEE then skips that frame's rendering (with
    0xDECAFC = 1). A game thread near its budget is slightly late on most frames, so most renders were skipped while the
    simulation kept 60. `renderlate N` (16:58 build): the threshold in percent of a period, the instruction now reads
    a DLL variable (stock 5).
  - `renderlate 50` on spinB10 (`results\scenarios\rl50*`): B still simulated 59.7-59.8 frames/s; DeadPool presents
    6.5 -> 58.9/s (presents >= 50 ms 5 -> 3), Graveyard 16.2 -> 29.9/s but presents >= 50 ms 23 -> 68; PitDay stopped at
    frame 0 (harness). One fight per stage and a large spread between fights: rl50b repeats it with ns30_d2 and ns60_d2
    (normal speed) to see the effect on games that are not overloaded.
  - rl50b (`results\scenarios\rl50b*`): ns60_d2 presents >= 20 ms 81/92 -> 32/34 per minute and frames >= 20 ms
    107/133 -> 49/47 (games A/B; earlier ns60_d2 runs had 98-133), presents >= 34 ms 7.3/9.1 -> 6.1/6.1; ns30_d2
    similar or better; simulation 59.7-59.8 frames/s; no desync but a PitDay frame-0 stop. Overloaded (spinB10, 5
    sessions): B presents 23 -> 36-44/s, but presents >= 50 ms 31 -> 65-117 per minute: more frames shown, less evenly.
    **Default `renderlate 50` since the 17:32 build.**
- **Release gate on the 17:32 build (`results\gate_20261104c_rollback.txt`, -Now -Soak 0): pass, zip
  `dist\MK9Rollback-netplay-20261104-1804.zip`** (DLL SHA-256 490EE11D...2DAF2). Online smoke; plain, auto re-arm (each
  game re-armed itself 3 times) and delay-change (21 changes) regressions 0/6 mismatching each, 0 crashes, 0 failed
  saves; menu walk 305 presses, no hang. What changed since the 2313 zip, for players: deep-copy desync fix
  (complivedata), the replay crash and module-link guards, smooth time-sync payments (paysmooth 2), faster balancing
  (qualityms 200, balancecfg 24 4 3), no 600-frame hitches (tier E cache, FPU walk outside sessions), 400 ms GGPO sync
  retry, no GGPO assert on a peer lost during the start, renderlate 50.
- **Safe mode on the 17:32 build:** lockstep regression 1 of 1 passed (0/6 mismatching,
  `results\lockstep_regress_20261104.txt`); lockstep peer loss PASS (A ran 120 frames in the 2 s after its session
  stopped, `results\lockstep_peerloss_20261104.txt`).
- **Every stage on the 17:32 build (`results\scenarios\stages2*`, 28 fights, ns30 delay 2, fighters varied):** frames
  >= 34 ms 1-2 per 33 s fight on every stage (this morning's sweep1: 74-119 per fight on the leading game), >= 50 ms
  0-2, presents >= 34 ms 1-4, max 63-91 ms; 0 crashes. Checksums: 14 fights 0 mismatches, Street stopped at frame 0
  (harness), 13 with mismatches whose first one is RNG A only with both fighters identical, at frames 4-406 (mostly the
  round intro; Desert's frame-192 values equal DeadPool's, so one draw a frame apart). Fighter states (logged every 300
  frames, live and possibly predicted) stayed identical through the fight in 10 of those 13; they diverged for good in 3
  (BellTower Scorpion v Stryker from frame 143, Graveyard Sektor v KungLao from 59, RooftopDay SubZero v Cyrax from 61),
  each after an RNG-only difference in the intro: the attract-demo start-phase artifact as far as known, unproven until
  a two-PC test (MK9's online start keeps the intros in phase; the 11-03 field test had 0 mismatches).
- **Intro desyncs explained (`results\scenarios\rng3*`, `rngtrace 1`, 6 x DeadPool):** fight 4 split at frame 192: at gf
  100192 game A drew RNG A twice (the engine's per-frame 0x0080F6C5 and the script native `random` 0x008B87C1 from
  executor 1355AAE0), game B only once; B made the script draw a frame later (A's checksum at 192 = B's at 193), as in
  the Courtyard case. Script waits are frame counts (`goto fn after n frames`, VM +0x40), so a script that was one frame
  further into its wait when the session began draws a frame earlier. Each harness game reaches its demo's first fight
  frame after its own history (renumbered from gf 1349 on A and 1476 on B in that fight; 8-702 frames apart in the clean
  ones), so the sessions start from different script states that the frame-0 checksum (fighters, RNG) does not cover.
  In the real online start both PCs reached the first fight frame at gf 359 (11-03 field test), and the match's round 2
  and 3 intros, run inside the session (gf 102934, 106168), stayed in sync: 9,289 frames, 0 mismatches. **Harness-only.**
- **Diagnostics (19:19 build, `results\scenarios\diag1*`):** HITCH lines now give the live world tick and engine tick
  times. The per-fight ~50 ms frame (gf 100949, both games at the same instant): boundary 4-5 ms, world tick 0.0, engine
  tick 1.3-1.5 ms, so ~44 ms outside the simulation: rendering / the render-thread fence, on both games at once (one GPU
  in the harness; a first-use shader or texture as far as known, the game's own hitch). Script native 191 (appSeconds)
  was never called, in menus, demos or fights (hook installed, both its paths log): the >36 h uptime concern about its
  virtual base (131072 s) does not arise in these flows.
- **SyncTest soak 13101-13106 on the 19:19 build (`results\synctest_soak_20261104\`, 6 x 13 min, check distance 4):
  7,202 loads, 28,808 replayed frames, 0 crashes or hangs.** 7 groups of 4 sync errors (10-30 baseline on the same
  seeds: 5,328 loads, 6 groups): 6 are on a fight's last frame at the timeline restart ("replay ended at gf=4", no
  `GGPO DESYNC` line), the end-of-fight artifact. **1 real failing load:** 13104 S=3405 (Sonya v Skarlet, Goro's Lair,
  frame 292): the first VMTRACE difference is the tick gate of MK object 13601E00 (actor 3A43E9B0), `[+0x1D8]+0xC` 0 in
  the original and 1 in the replay, then the replay spawned the object's script VM (native 0x8B6DA0, 3 extra script RNG
  draws) and P2 actor +0xC8 differed: the 11804 class that the `mkhelper` restore (10-29) is meant to cover, so that
  restore did not apply here. The 20:59 build logs, with a differing tick gate, what the last load did with that helper
  (restored, skipped and why: vtable, owner's +0x1D8 moved, block; or not in the snapshot, with the owner's birth frame
  and type). Soak 13104-13109 runs on it.
- **Soak 13104-13109 on the 20:59 build (`results\synctest_soak_20261104b\`): 8,312 loads, 0 crashes; 2 real failing
  loads, both the hit-test class** (13105 S=4019, 13109 S=6429: the original spawned the `(1, 0xCE)` VM from the MK
  object hit test, ctor up 0x008A4C16 <- 0x008A73AE, the replay did not); 13107 frame 4 is the "object heap not
  discovered" start artifact, the rest end-of-fight. The `rec+C` case did not recur. One tick-gate difference without
  a desync (13109 22:21, helper 227AED00): the helper was restored (+0xC saved 0, live 0) and still read 1 at the
  replayed gate, so something wrote it during the replayed frame before the gate.
  - **Use-after-free found in the helper restore (fix, `mkhelperrevive`, on, in the fingerprint):** helper blocks were
    not in the quarantine's restore-block list, so an MK object destroyed in the abandoned future freed its helper for
    real (its deleting destructor 0x004A5B70 writes vtable 0x00C6A5A8, then GMalloc frees it); the object restored from
    the image then pointed at a freed or reused block, and the restore skipped it (vtable check). Now the save lists
    mkhelper blocks as restore blocks (frees deferred), and the load also restores a helper whose vtable is 0x00C6A5A8
    while the quarantine still holds the block (`MKHELPER revived` lines).
  - **Batched line checks ruled out.** The helper is the MK object's line-check receiver (0x008A7543 -> 0x006BDD50 ->
    0x006BDC00: +4 issued, +8 done, +0xC hit, +0x10.. hit data, +0x30 hit actor; the report object 0x00DED22C, vtable
    0x00D549E8, has the async callbacks 0x006BDA80/0x006BDB30). RayQueries (22:43 build) tracked them: 0 left pending in
    600 loads (answered synchronously), so no future results cross a load that way; not installed. Remaining candidate
    for the gate case: another MK object sharing the helper (the MK object copy 0x008A0C?? copies +0x1D8) whose line
    check hit in the replay only, i.e. the hit-test class again.
- **Soak 13110-13115 on the 22:43 build (mkhelperrevive on, RayQueries installed but inert; `results\synctest_soak\`
  seed13110-13115): 7,650 loads, 30,600 replayed frames, 0 crashes, all alive.** 6 groups of 4 errors are the
  end-of-fight artifact; **3 real failing loads, all the hit-test class, all "the original hit, the replay did not"**
  (VMTRACE: original `(1, 0xCE)` ctor up 0x008A4C16 <- 0x008A73AE, replay `tickgate` with rec+C 0; then the hit
  target's attached list 6 or 9 vs 2 and position drift): 13110 S=2593 and S=2969 (hit box actor based on
  40E2EAA0, a SkeletalMeshActor_0 of MKSkeletalObj 13604600 that P1 attaches and detaches repeatedly: ATTACHFUTURE
  at S=1809/2425/2601/2977), 13114 S=6216 (base 0DFB1740, whose attachment list the load grew to re-attach an
  actor). No helper-class (rec+C) case. ACTORARRAY "kept live" lines are at 2 of the 6 hit-class failures since
  13104 (13110 S=2593, 13105 S=4019) but at ~10% of all loads, so no clear link.
  - Final build 00:11 (RayQueries not installed; `build\MK9Rollback_final0011.dll`, SHA 7550E9C3...): **release
    gate passes** (`results\release_gate_20261105.txt`: online hooks; regress, autorearm and delaychange 3 fights
    each, 6/6 reports, 0 mismatches, 0 failed saves, 0 crashes; menu walk 305 presses, no hang) -> **zip
    `dist\MK9Rollback-netplay-20261105-0043.zip`**.
  - Traced soak 13116-13121 (`animtrace 1`) on a diagnostic build (`build\MK9Rollback_hitposdiag.dll`, F2D191E6...):
    HITPOS also logs the base's Location and base (`bloc=`, `bbase=`), to see whether a prop-based hit box's prop is
    where the original had it at the first replayed check. build\clang gets the release DLL back afterwards.
    **Result (`results\synctest_soak_20261105_traced1\`): 5,150 loads; 2 real failing loads (13116 S=1654, 13119
    S=1966), both the hit class "original hit, replay missed"; 2 crashes:**
    - **13120 01:55, null read at 0x00424B1E, game closed: a rollback bug in the pad state.** The pad poll 0x00424A60
      polls a hardware pad slot (0x00E221A8 + i*0x60C) when its +8 has bit 0x1, through the device table 0x00E0E1F0 +
      i*0x17C (+0xCC the DirectInput device, `GetDeviceState(0x50)` = vt+0x24). Bits 0x1/0x20 are set only by the
      attach 0x00422FC0 (with the device record) and bit 0x1 is cleared only by the release 0x004219D8, which also
      runs after a >300 ms gap between polls (0x00424A89 -> 0x00422E10). The `hwpads` Tier A span restored them
      with the input state, so a load after a device release (a hitch, a wireless pad sleeping or unplugged) put back
      "attached" over a null device. In an online match the same crash would follow any controller drop or long
      hitch inside a rollback window. **Fix (`padhw`, on): loads keep +8 bits 0x21 of the 6 hardware slots live**
      (`PADHW` lines). Verified with `poke E23FEC 20` mid-SyncTest (`results\padhw_test.log`): the load from before
      the poke kept the bit, 346 loads, 0 sync errors, no crash.
    - **13118 01:24, eip 0x006084A9 read [0+0x34], between fights:** a package load (LinkerLoad_77) whose loader
      (+0x61C) had a null vtable, ~2,000 frames after the fight's quarantine was released (0 blocks held at the
      time). First time in any log; nothing ties it to a load or restore. Watch for a repeat.
    - **13116 S=1654, the mechanism of this hit class:** the hit box (MK object 13600000's actor 61769790) was
      based on MKParticleActor_97 with a zero offset (`bloc` = `loc`). The load restored the hit box (`atload`
      right) but not the particle actor (neither a fighter's attached actor nor an MK object's +0x08), and on the
      first replayed frame the hit box's attachment controller (MotionControl +0x13C: 0x007853F0 -> 0x0078D1B0 ->
      MoveActor 0x007A7433) pulled it onto the particle actor at the abandoned future's position, 4 frames of travel
      ahead (LOCMOVED on every load of that flight, 29 lines). The original hit P2 at gf 1655, the replay never did.
      **Fix (`mkbases`, on, in the fingerprint): the save also stores, as an `mkactor` span, the Base of each MK actor
      that no other span covers** (`MKBASE` lines name the first 10).
    - 13117's 40 LOCMOVED lines are a prop-based hit box (base SkeletalMeshActor_9) one frame ahead: the older
      prop class (propskel restores the prop's pose; whether its actor was covered shows in the MKBASE lines).
    - **Traced soak 2 on the padhw + mkbases build (BB550311, same seeds; `results\synctest_soak\seed1311x*`, archived
      as `results\synctest_soak_20261105_traced2\`): 5,151 loads, 0 crashes, 0 replayed hit tests with a different
      result (soak 1: 3), 1 real failing load.** MKBASE saved 9-24 bases per session; PADHW never triggered (no
      device release). LOCMOVED is down to prop bases (SkeletalMeshActor_2/9/11/20/65, 3-25 per session) and one
      particle (MKParticleActor_585, 8 lines, 13119), none with a different hit result.
      - 13116 02:36: 3 "sync errors" at frame 4 with 0 loads: the harness armed SyncTest 8 s after seeing the fight's
        last FRAME lines, at gf 321 of the next screen ("object heap not discovered", the load found no snapshot).
        Harness only; `noai_rollback.ps1` now checks for a fight again after the 8 s wait.
      - 13121 03:59: end-of-fight artifact (4 errors at frame 1344, "replay ended at gf=4").
      - **13119 S=3309 (real): the leg-IK class.** FIRSTDIFF frame 3309: the MK image matched through the MK object
        tick, 6 dwords differed at P1's anim step and 54 at EngineTick exit (a 0x28-stride float array, last-bits
        differences); P1 flagged 'n' (anim nodes differ on the first replayed frame). The node differences in this
        session are all SkelControlPinLimb: +0x40 (strength) holds the abandoned future's value (live 1.0 replay
        0.9, live 0.9 replay 0.3, live 0.3 replay 0 with +0xC0 unbound) on ~0.4% of first replayed frames (34/8400),
        although bindstate restored every time (0 skipped). `IKDIFF` lines (build 94D19AE9, animtrace only) now log
        the bound elements' strengths and the +0x39C weights live / replayed / right after the load, to find which
        input still holds the future.
    - **Release gate on build 94D19AE9 (padhw + mkbases + IKDIFF; `results\release_gate_20261105b.txt`, 04:06):**
      online smoke, plain / auto re-arm / delay-change regressions 3 fights each, 6/6 reports, 0 mismatches, 0 failed
      saves, 0 crashes; **menu walk FAIL: the PC was locked** (LockApp in the foreground since before the -Now start), so
      no zip. `queued_checks.ps1 -MenuOnly results\release_gate_20261105b.txt` waits for an unlocked idle PC, reruns only
      the menu walk and packages when it passes on the same DLLs.
    - The IK-weight writers: element strength = clamp(weights[i].x) (0x0077BA40, comp +0x39C {x, y}); the weights are
      written by MK pose sampling at a given time (0x0043EC30 / 0x0043EE30, also 0x00457Bxx), called from script natives
      0x008C93B0 / 0x008C9400 (executor 0xE39DF4, result to +0x1C): an anim-curve output, a side effect of the sampling.
    - **Traced soak 3 (94D19AE9, seeds 13122-13125, `results\synctest_soak_20261105_traced3\`): 3,715 loads, 0 crashes,
      0 real failures** (16 errors, all four groups the end-of-fight artifact). IKDIFF (13122, 11 lines): on every first
      replayed frame with a PinLimb difference **the bound elements and the +0x39C weights equal the original's**; only
      the node's +0x40 differs, and it holds the abandoned future's value (gf 1511: live 1.0, replay 0.6 = live at
      1515; 1515: 0.6 vs 0.2; 1519: 0.2 vs 0 unbound). Live node +0x40 = element +0x24 = weight.y. So the load left node
      +0x40 at the future's value: kept live by the span's pointer test (1.0f = 0x3F800000 is a UObject address here).
      Node differences per session (first replayed frames): 13119 34/8400, 13121 16/5800, 13122 24/10500, the other
      seven 0 (matchups without leg-IK fades).
    - Build 80AE7C55 tried PinLimb +0x40 by value (`PINSTRENGTH` when the pointer test had kept it). **Traced soak 4
      (seeds 13119-13122, `results\synctest_soak_20261105_traced4\`): PINSTRENGTH 0, node differences unchanged**
      (13120 24/8600, 13121 15/8300): +0x40 is never kept live, so not the cause; reverted. 13121: two end-of-fight
      artifacts. **13119: the game thread hung at gf 2475 (05:35:25, `HANG?` eip 0x00443BCC), 900 frames into a
      SyncTest session (smoke v Scorpion, Living Forest).**
    - **The hang: the fighters' name-keyed lookup map (MK object +0x138), a sparse-array TSet with two blocks the
      snapshot did not have.** 0x00443B50 (find) walks a hash chain (element 0x1C bytes, next at +0x14, buckets +0x34,
      size +0x38) until -1; element 2 was linked to itself. The add (0x00443C10 -> sparse add 0x004B74D0) takes an index
      from the **free-index stack** (a TArray of ints at map +0xC/+0x10/+0x14: pops the last entry) or appends, and sets
      allocation bits (map +0x18 overflow block, else inline at +0x24). namemaps (synctest40/45) saved the element and
      hash blocks and restored them with the header only when both were the saved blocks, but not the stack: after a
      load, the restored free count indexed the abandoned future's stack, the next add popped an index still in use and
      linked it into its own bucket's chain (`next = hash[b] = itself`). **Fix (build 246F8CAF): the stack and the
      overflow bits block are saved too (`namemap_free`, `namemap_bits`) and all four blocks restore together or the
      whole map stays live** (`NAMEMAP ... save: free-index stack` lines). IKDIFF now also prints each table node's
      +0x40: saved (span), after the load, at the first replayed skeleton update, live. Traced soak 5 runs both.
    - **Traced soak 5 (246F8CAF, `results\synctest_soak_20261105_traced5\`): 3,591 loads, 0 crashes or hangs, 0 real
      failures** (3 end-of-fight artifacts). But no session ever had a free-index stack (no NAMEMAP save line), so the
      fix was untested, and the hung lookup's caller (0x00444B20: `this` +0x10C component, +0x138 map) is a method of
      every **MKSkeletalObj**, stage props included, whose maps namemaps never covered: their headers came back with the
      image over the abandoned future's blocks, the synctest40 cycle exactly (an add appends at the restored count into a
      chain the future already linked that index into). **Build 1FBB32A7: every listed MKSkeletalObj's map (props too;
      header checked to look like a map, else left out) is saved with (object, MK id) pairs; the save lists the blocks as
      restore blocks (frees deferred); the load decides per map before writing: restore (header from the image + saved
      blocks, each still the map's or held by the quarantine), keep live (same object alive, a block neither), or empty
      (object died, a block gone).** First session: maps saved for both fighters and three Living Forest props
      (MKSkeletalObj; one with a hash block and 0 elements). Traced soak 6 (13119-13122) runs it.
    - **IK class, soak 5's IKDIFF node lines (13121): PinLimb +0x40 is restored right (saved = after the load = the
      original's) and rewritten before the first replayed skeleton update** to the abandoned future's value (gf 2633:
      saved 1.0, after load 1.0, replay 0.8, original 1.0). Its writer 0x007601B0 copies `[node+0xC0]+0x20`; the bound
      objects are 0x48 apart (not bindstate's 0x60 elements), so that source is not restored. Build 03F787D6
      (`build\MK9Rollback_iktrace.dll`, not deployed) traces writes to the source in write-trace slot 3 (IKTRACE).
    - **Traced soak 6 (1FBB32A7, `results\synctest_soak_20261105_traced6\`): 3,499 loads, 0 hangs or crashes, 0 real
      failures** (4 end-of-fight artifacts). NAMEMAP decisions: 40,074 maps unchanged, 25 restored from blocks the
      quarantine held (a rehash or a prop's death in the window: before, a fighter's map stayed the future's and a prop's
      header came back over the future's blocks), 0 kept live, 0 emptied; no header rejected. No PinLimb differences in
      these matchups (the attract demo's characters vary run to run).
    - Release candidate **03F787D6** (1FBB32A7 + IKTRACE, animtrace only) deployed: gate `release_gate_20261105c.txt`
      (-Now; the PC is locked, so the menu walk will need `-MenuOnly`), then traced soak 7 (13126-13131, IKTRACE).
    - **Gate on 03F787D6 (`results\release_gate_20261105c.txt`, 08:27): online smoke, plain / auto re-arm / delay-change
      regressions 3 fights each, 6/6 reports, 0 mismatches, 0 failed saves, 0 crashes, lockstep checks pass; menu walk
      FAIL (PC locked).** `queued_checks.ps1 -MenuOnly results\release_gate_20261105c.txt` is waiting for a free PC
      (`results\menu_only_20261105c.txt`); build\clang and build\clang2 must keep 03F787D6 until it has run.
    - **Traced soak 7 (03F787D6, `results\synctest_soak_20261105_traced7\`): 5,780 loads, 0 crashes or hangs, 3 real
      failing loads** (the other stops: 4 end-of-fight artifacts). NAMEMAP: 5 maps restored from held blocks, 0 kept live
      or emptied.
      - **13129 S=2712 (KungLao v Kenshi, Street) and 13130 S=2156 (Kenshi v Sektor, Kahn's Coliseum): the hit class,
        LOCSTILL variant** (as 13119 S=1966, Kabal v Cyrax): a hit box attached to a prop mesh (base SkeletalMeshActor_3
        / _5, itself attached to the fighter) kept its restored Location at the first replayed check while the original
        had already followed the prop; the replay then trailed by exactly one frame (HITPOS: R2713 = L2712) and missed
        (live hit 1, replay 0). The prop's own actor Location matched the original at that check (`bloc`). The hit box
        is placed each actor tick by its motion controller (actor +0x13C, 0x007853F0 -> 0x0078D1B0: finds its 0x60
        record in Base->Attached, 0x0078D0B0 -> 0x00789060 type 1 = the base component's bone matrix (SpaceBases) x
        LocalToWorld x the record's relative transform, then MoveActor). So at the replayed check the base component's
        pose was either the restored one (its update came later or was skipped) or the update order differed.
      - **13128 S=1877 (Sheeva v Mileena, Shao Kahn's Throne): a cinematic.** `CINEMA load: ... nothing restored`
        (the saved Matinee objects were collected), P1 comp +0x3AC 2 vs 6 at the next FightUpdate, both fighters' actor
        +0x80 bit 0x40 (the per-frame tick parity) differ: in one pass the fighters did not tick that frame.
      - IK class: IKTRACE watched the first PinLimb's source, which never differed; the differing nodes (13131 gf 2614:
        +0x40 saved 1.0, load 1.0, replay 0.7 = the future's) had other sources. One original-only write seen:
        0x0077A159 (from 0x0077BBAB in 0x0077BA40) wrote 1.0 into the watched source.
    - **Diagnostic build 42801006 (`build\MK9Rollback_locplace.dll`, animtrace only):** LOCPLACE prints the hit box's
      placement inputs (record, relative location, base component, SpaceBases pointer and hash, triple-buffer headers,
      LocalToWorld) at the original's check and at the replayed one; the write trace's slots 1-2 now watch the base
      component's SpaceBases header and LocalToWorld X (the order of the prop's pose update and the hit box's placement
      in each pass), and live hit checks re-arm onto a newly seen based actor so the original pass is traced too.
      IKSRC lines (at each load) name every PinLimb whose source [node+0xC0]+0x20 disagrees with its restored +0x40,
      with the source's saved value and span; IKTRACE watches the worst one. Traced soak 8 (13132-13137) runs it from
      build\clang_next (`synctest_soak.ps1 -Dir`, which now passes -Dir on to noai_rollback.ps1).
    - **Soak 8 (stopped after session 1, `results\synctest_soak_20261105_traced8\seed13132_partial.log`, `traceall 1`):
      the LOCSTILL mechanism.** Jax v Jade (4 LOCSTILL, no desync) and Kitana v Kenshi (S=3160 -> DESYNC gf 3164, the
      same class as 13129/13130). At the first replayed check **the base component's pose and LocalToWorld were the
      original's** (LOCPLACE: same SpaceBases block, same hash, same LocalToWorld), yet the hit box kept its restored
      Location. Per frame both passes write, in order: the base component's SpaceBases header (0x00769517, the pose
      rotation, often a worker thread), its LocalToWorld X (0x00773251 in the skeleton job, then 0x0063B233/0x0063B55A
      via 0x0078592A / 0x00634D85, UpdateTransform), then the hit box's Location through its motion controller
      (0x0078541C -> 0x0078D2F0 -> MoveActor 0x007A7433, game or worker thread). **In the first replayed frame the last
      write is missing**: the hit box's controller did not run after the base's update (ticked before it, or not at
      all). AActor::Tick is 0x007A8F90 (vtable +0x154): it sets bTicked (+0x80 bit 0x40) from GWorld +0x130 bit 0 at
      0x007A8FB0, then vt+0x194 (0x007A8B50: timers, LifeSpan, then the motion controllers at +0x13C).
    - **Diagnostic build 75494FE6 (`build\MK9Rollback_tickorder.dll`, build\clang_tick):** slot 1 = the hit box's
      +0x80, slot 2 = its Base's +0x80, slot 3 = the base component's LocalToWorld X (IK watch off; IKSRC stays), so
      each pass's tick order of the base and the hit box (and the dispatcher, from the bTicked write's callers) shows.
      LOCPLACE also prints both actors' flags. Traced soak 9 (13138-13143, `traceall 1`) runs it.
    - Harness: `noai_rollback.ps1` splits Setup lines on ',' (`powershell -File` passed `'animtrace 1','traceall 1'`
      as one string, so `traceall` was lost in soak 8's first minutes; sent by hand).
    - **Soak 9, session 1 (75494FE6, `results\synctest_soak_20261105_traced9\seed13138*`; Kenshi v Kabal, Courtyard): the
      order, and the cause.** Every frame: base component LocalToWorld (skeleton job), the base actor's tick (bTicked
      write 0x007A8FB6, from the tick job 0x0071EAED -> 0x007A0592 -> 0x0079F108 -> AActor::Tick), the base component's
      UpdateTransform, then the hit box's tick and its controller's Location write. **On the first replayed frame the hit
      box's tick came first**, before the base's, and wrote no Location. 4 LOCSTILL in the session (S=2581 desynced at
      2583, the hit class); **all 9 LOCSTILL cases of soaks 8-9 are loads where MKATTACH re-attached a detached hit box**
      (118 and 54 MKATTACH loads). Attaching (SetBase vt+0x190 = 0x007942C0 -> 0x00794190 -> 0x00793DC0) also gives
      the actor its attachment controller in a motion-control slot (actor +0x13C, class 0x00D5A85C via 0x00793C50 ->
      0x00786200); detaching deletes it. RestoreMkAttach put the record and Base back but not the controller (only the
      fighters' controllers were restored, RestoreMotionCtl), so the re-attached hit box had nothing that follows its
      base on that frame.
    - **Fix (build 205D2679, `build\MK9Rollback_mkmotion.dll`, build\clang_mkmotion; 'abtest mkmotion', on):**
      RecordMkMotion saves, for every actor RecordMkAttach lists, its motion-control slots holding the attachment
      controller (the 12 bytes and the owned block, as the fighters'), both blocks quarantine restore blocks;
      RestoreMkMotion (after MKATTACH) puts a saved controller back into a slot that is empty again through the engine's
      slot store 0x00786290 (store + refresh, deletes nothing), restores the bytes of one still in place, and leaves a
      different controller alone (`MKMOTION` lines). Keeps soak 9's diagnostics. Traced soak 10 (13144-13149) runs it:
      expect no LOCSTILL at MKATTACH loads and no hit-class desync.
    - **Soak 10 session 1 (13144: Nightwolf v Jade, Kabal v Sonya, Stryker v Kano): 133 MKATTACH loads, the controller
      put back at all 133, LOCSTILL 0** (before: 4-5 per session at 54-118 MKATTACH loads), no real desync (one
      end-of-fight artifact). Memory flat (private 1469-1527 MB vs 1554-1565 before); the un-freed-at-load rate is
      unchanged. The unexplained net2 Graveyard desync (Jade v Reptile, frame 1665) followed a load that MKATTACH
      re-attached actor 39D0B2C0 on, two frames before: the same class as far as the log shows.
    - **Soak 10 sessions 1-2 (`results\synctest_soak_20261105_traced10\`, 205D2679): 228 MKATTACH loads, LOCSTILL 0,
      no real desync** (one end-of-fight artifact; 6 matchups incl. Jade, Kabal, Kenshi, Mileena). Stopped after 2
      sessions for the next build.
    - **Leg-IK fix (build 778B84CE, `build\MK9Rollback_bindpins.dll`, build\clang_bindpins; 205D2679 + 'abtest bindpins',
      on, in the fingerprint).** The PinLimb nodes' +0xC0 names a limb pin of the component's TArray +0x390 (0x48 each;
      soak 7's 13128 NODE lines: bound to 0x48-apart objects live, unbound in the replay), which 0x00779E10 sets from the
      request TArray +0x378 (0x1C each, strength +0x18 consumed: above the threshold the pin's strength and targets are
      set at 0x0077A156, below it the pin unbinds and node +0x40 = 0). Neither array was saved, so after a load inside a
      leg-IK fade the node copied the future's pin strength (0x007601B0) on the first replayed frame. The bind state now
      saves both arrays (and each pin's helper +0x20/+0x24) and restores them with the rest, all or nothing (new skip
      reasons 12-15: block/count/class/helper changed); node +0xC0 may name a pin, and a pin in the +0x3A8 list no longer
      fails reason 11 (which had refused the whole bind state on every load of some matchups). `BINDPINS` lines count it.
      Traced soak 11 (13150-13155) runs both fixes.
    - **Traced soak 11 (778B84CE, `results\synctest_soak_20261105_traced11\`): 0 crashes or hangs, LOCSTILL 0, first
      replayed frames with differing anim nodes 0 in 36,300 outside one Shang Tsung intro** (the IK class: 34/8400,
      16/5800, 24/10500 in the leg-IK sessions before), BINDPINS on every fighter (4 requests, 4 pins), BINDSTATE skipped
      0. End-of-fight artifacts only, plus **one real failing load, a new and rare class: 13153 S=1514, Shang Tsung's
      intro morph** (FIGHT lines flip ShangTsung/SubZero at gf 1470-1518, TIERE rebuilt; the load landed inside the
      mesh swap: P1 actor +0xC8 pointer and +0xDC 1 ulp at gf 1518, anim-node tick tags +0x70 4 apart). The harness
      armed SyncTest during that intro; in an online match the session spans the intros too. Open.
    - IKSRC note: right after a load a node's +0x40 may still differ from its pin's strength (the ue3anim span keeps it
      live); harmless, 0x007601B0 copies the (now restored) pin strength before the node is used.
    - Soak 11 totals: 5,403 loads, 500 MKATTACH loads, every sync-error group the end-of-fight artifact, all games alive.
    - **Release candidate 778B84CE (mkmotion + bindpins on top of 03F787D6) deployed to build\clang and build\clang2
      (13:02).** 03F787D6's `-MenuOnly` job was stopped as it started (13:01, superseded; it had launched a game, closed).
      Gate `results\release_gate_20261105d.txt` (-Now, PC locked), then `-MenuOnly` for the menu walk and the zip.
    - **Gate on 778B84CE (13:02-13:32): online smoke, plain / auto re-arm / delay-change regressions 3 fights each, 6/6
      reports, 0 mismatches, 0 failed saves, 0 crashes, lockstep lag and peer loss pass; menu walk FAIL (PC locked).**
      `queued_checks.ps1 -MenuOnly results\release_gate_20261105d.txt` (pid 23744, `results\menu_only_20261105d.txt`)
      waits for an unlocked idle PC, reruns the menu walk and packages the zip; keep build\clang and build\clang2 on
      778B84CE until it has run.
    - **Every stage on 778B84CE (`results\scenarios\stages3*`, 13:33-14:14, 28 fights, ns30 delay 2, fighters varied):
      0 crashes or hangs**; 59.7 frames/s, frames >= 34 ms 1-3 per 33 s fight (2.8 / 2.9 per game-minute), presents
      >= 34 ms 1-6, max 70-112 ms. Checksums: 14 fights 0 mismatches (Desert, PitBottom, PitDay clean now), Street
      stopped at frame 0 (harness), 13 with mismatches. **Every first mismatch is RNG A only with both fighters
      identical** (frames 2-561, and Pit 1817), the start-phase class. The STATE samples (every 300 frames, live, so
      possibly predicted) stayed identical in 11 of 13 (Graveyard and RooftopDay, which diverged in stages2, did not);
      the fighters diverged for good in CourtyardN (Reptile v Skarlet, from frame 2) and BellTower (Scorpion v Stryker,
      from frame 143: the same fighters and frame as stages2, so the harness start repeats it). Single differing samples
      (Pit 900, Wastelands 900) are predictions: Pit's checksum at 900 matched. Pit's only mismatch is frame 1817, RNG
      only, matched again at 1818: one draw a frame apart, late in the fight. **Traced (`results\scenarios\pitrng2*`, 3 x
      Pit, `-PreSetup 'rngtrace 1'`; scenario_matrix now passes -PreSetup): it recurs at frame 1817 with other fighters
      (Jade v Reptile; Kano v Baraka in pitrng) and is a stage script's `random` (0x008B87C1, executor 12699CE0) that game
      B called at frame 1816 and game A at 1817.** The games entered that session 13 s apart (renumbered from gf 5373 and
      4475), so the stage script's wait ran out of phase: the Courtyard class, harness-only as far as known. (Without
      -PreSetup, 'rngtrace 1' arrives after the patch set installs and no draws are recorded.)
    - **Script spans skipped at every load after a timeline restart (fix: build ADF8CB24..., `build\MK9Rollback_relfloor.dll`,
      build\clang_relfloor; not yet in the release candidate).** Soak 11 13153: `LOADAB skip script_module 6C097380+2C8E0:
      released by the quarantine at gf 3371 (freed at gf 3367 by 0045B3D0) after the save gf 1507`. The timeline
      restart's ReleaseAll stamps the old fight's frees with the old frame numbers (3367-3371); the next SyncTest session
      started at gf 1507 with the quarantine off in between, so no newer entry stopped the ring scan, and the new fight's
      script modules (reallocated at the same addresses) counted as "released after the save": skipped at every load of
      the session, their script data kept live. NATIVETRACE showed it as native 004597E0 (exec 125E81C0, data in that
      module) returning a countdown exactly 4 (the check distance) lower in every replay; the 13153 Shang Tsung intro-morph
      failure was in such a session. The skip log shared a 20-line cap, so how many sessions it hit is unknown.
      `FreeQuarantine::NewTimeline()` after ReleaseAll at the timeline restart and at the netplay renumbering: the
      LoadAB scan no longer looks at entries from before the current timeline; script skips are counted on their own.
      Online, releases made after the renumbering normally stop the scan before the old entries, so this mainly fixed
      the soak; a rematch whose start released nothing would have hit it too.
    - **Soak 12 (relfloor ADF8CB24, seeds 13150-13155, `results\synctest_soak_20261105_relfloor\`): 6,332 loads, 0
      crashes or hangs, LOCSTILL 0. Script spans skipped: 0 (soak 11: 1-20 lines per process); NATIVETRACE 004597E0: 0
      (13153: 48); 13153 had no failing load this time.** Sync errors: 13152 and 13155 one end-of-fight artifact each,
      plus **one real failing load, 13155 S=3254: the helper (rec+C) tick-gate class.** VMTRACE: the tick gate of MK
      object 12792200 (actor 7344F0F0) read helper 54D70280 +0xC = 0 in the original and 1 in the replay, although the
      last load restored the helper (+0xC saved 0, live before the load 0); the replay then ran the object's script
      (native 0x8B6DA0, executor 126E9A40) and drew an extra script `random`, and P1's anim sequence nodes were a tick
      behind (tags 2E16 vs 2E17). The same tick-gate difference occurred 6 times in the 6 sessions (5 times original 0 /
      replay 1, once the reverse), each with the helper restored to 0; only S=3254 desynced. So +0xC is written during
      the replayed frame before the gate. RayQueries' stale-result idea is out: soak 13115 had the same case with 0 MK
      line checks pending in 600 loads. **Diagnostic (`rayres`, lab only):** the scene-query report's callbacks
      (0x006BDA80 / 0x006BDB30) are tapped; a result for an MK helper becomes a VMTRACE event on the game thread (helper,
      kind, result, +0xC before/after, caller), so a result present or different in one pass is the first differing
      event, and every such result on any thread goes into a ring that the tick-gate difference prints for its helper
      (`RAYRES` logs the first result off the game thread). The tick-gate record also logs (not compared) the helper's
      +4 issued, +8 done, +0x30 hit actor and the last query's start/end x (+0x34/+0x40), so a flipped +0xC shows
      whether a whole query result arrived or only that word changed. Build CBBBC241... in `build\clang_raytap` (with
      relfloor and the sync retry timing below); soak 13 (seeds 13156-13161) runs on it.
    - **Network set on 778B84CE (`results\scenarios\net5*`, 16:10-17:07, 24 fights): 0 crashes, every fight 59-60 fps,
      frame P99 16 (LAN) / 18 (ns30) / 26-29 (ns60) / 32 (ns100) ms**, rollbacks 0-3 on LAN and 110-130 per fight at
      ns60/ns100; ns60_d3 PitDay stopped at frame 0 (harness, as in net4). Hitches >= 34 ms per game-minute: LAN 2.4-3,
      ns30 3.6-4.3, loss5 3.6, ns60 4.9-6.7, stall 9.7-13.3 (the injected 150 ms stalls), ns100 13-16. **The only long
      frames are session starts (gf 100001)**: normally one round trip in the start barrier (40-180 ms), but 452-826 ms
      after a lost sync packet (loss5 both games once, jittery 503 and 826 ms; net3/net4 had 637-1890 ms).
    - **Sync retry timing (build_ggpo.ps1, in `build\clang_raytap` BA07CDAF..., not yet tested or in a candidate):** the
      handshake's retry ran from `_last_send_time`, which the replies to the peer's own sync requests reset, so a lost
      request was not retried while the peer's handshake went on; it now runs from our last sync request, 2 x RTT + 50
      ms (150..400/500 ms) once a round trip was measured, the RTT estimate doubling at each retry. Expected: a loss at
      the start costs ~150-250 ms at ns30 instead of ~450-830. Test: `scenario_matrix.ps1 -Set network -Only
      ns30_d2_loss5,ns60_d2_jittery,ns100_d3 -Dirs build\clang_raytap,build\clang_raytap2` (new `-Dirs` in
      scenario_matrix / two_instance: another build without touching build\clang / clang2). The release candidate's GGPO
      library is kept as `build\libggpo_778B84CE.a` (build\clang\libggpo.a is now the patched one).
    - **Release candidate 778B84CE: the menu walk passes** (`results\menu_only_20261105d.txt`, 305 presses, no hang)
      -> **zip `dist\MK9Rollback-netplay-20261105-1723.zip`** (the gate's other checks in `release_gate_20261105d.txt`).
    - **net6 (CBBBC241 = relfloor + rayres tap + sync retry timing; `results\scenarios\net6{a,b}_*`, 17:24-18:15, 15
      fights): the sync retry timing works.** Handshake (GGPO connected -> synchronized) per session: a lost sync packet
      now costs 160-240 ms at ns30_d2_loss5 (net3-5: 370-800), the worst ns60_d2_jittery handshake 770-854 ms (net3-5:
      1,266-1,633). Start-barrier waits at gf 100001 over 300 ms: 2 of 19 (net3-5: 9 of 47), the worst 679 ms (B's
      handshake lost a packet twice: 899 ms). 0 crashes; PitDay frame-0 stops in each set (the known harness artifact).
      Script: `startwaits.ps1` / `syncdur.ps1` (scratchpad, summarised here).
    - **What a session start still costs is the handshake itself: 5 round trips** (stock NUM_SYNC_PACKETS), during
      which the fight's first frame is held: 300-340 ms at ns30, 490-570 ms at ns60, 720-830 ms at ns100, plus the
      start barrier's wait for the other game's first input. **libggpo now uses 2 round trips** (build_ggpo.ps1,
      'udp_proto sync round trips'; the peers already know each other from OnlineMode's hello and run the same build,
      so the handshake only proves packets get through both ways with this session's magic numbers). Expected ~130 ms
      at ns30, ~250 at ns60, ~420 at ns100. The 5-round-trip patched library is kept as `build\libggpo_syncretry_1711.a`.
      In `build\clang_raytap_c` (3D3CA978...); test net7 (`results\scenarios\net7_*`) queued.
    - **Soak 13 on CBBBC241 (seeds 13156-13161, `results\synctest_soak\seed1315[6-9]*`, `seed1316[01]*`):** 13156 0
      errors (709 loads), 13157 one end-of-fight group (4 errors at frame 296, "replay ended at gf=4"); 13158-13161 were
      cut by the PC's sleep at 19:19 and re-run from 23:14. No rec+C case in the first two. **New: MK-helper line-check
      results arrive off the game thread** (`RAYRES` 13156 helper 57C17B84 gf 2661, 13157 helper 4F91EA80 gf 1433,
      caller 0B?41FB5 / 0B551FB5 in a relocated DLL), against RayQueries' "0 pending in 600 loads".
      - Why both can be true: 0x006BDD50 has two callers, both passing type 1 (-> 0x006BDC00): **0x008A7564** (the MK
        object tick, the scene's default query object [scene+0x14]) and **0x006F3E12** (a loop over an array of
        *embedded* helpers, [obj+0x30] + i * stride + 0x24, with its own query object [[obj+0xD8]+0x1C8]). 0x006BDC00
        sets +4 = 1, +8 = 0, raycasts through the query object's vt[0x24], and writes +0xC / +8 itself unless the query
        object's vt[1] returns nonzero (asynchronous: the result comes later through the report callbacks). 57C17B84 is
        not 16-aligned, so an embedded helper: the off-thread results are (at least partly) 0x006F3E12's asynchronous
        queries, not the MK object's. RayQueries only watched 0x008A7564.
      - **Trace extended (3D3CA978, `build\clang_raytap_c`):** both call sites are tapped (E8 retarget to `RqIssue`),
        each issue goes into the helper ring with +0xC before/after, +4/+8 before/after and "sync / async / busy"; every
        tick-gate read goes in too (with the MK object); the ring holds 4,096 entries and a rec+C difference prints the
        helper's last 14 in order, with per-site issue / async / busy counts. `RAYRES` names the caller's module+offset
        and the helper's last issuer; `RAYISSUE` logs the first asynchronous issue per site. So the next rec+C case
        shows whether +0xC changed through an issue (which site, sync or async), a callback (which thread), or neither
        (another writer). Soak 14 (seeds 13162-13167) queued on it.
      - **13158 (re-run 23:14, 742 loads, 0 errors):** the VMTRACE differences now start with `rayres` events: results
        for MK helpers (vtable 0x00C6A5B4; 6EF56A30, 6E985C70, 52A48410, all 16-aligned) delivered **on the game thread**
        through the report callbacks (caller PhysXCore+?1FB5, the same offset as the off-thread ones), in one pass and not
        the other, or for another helper, or before the tick gate in one pass and after it in the other (gf 2538-2559,
        1918-1937). All "ray, 0 hits, +0xC 0 -> 0", so no state changed. So **asynchronous line-check results reach MK
        helpers at a point and frame that differ between the original and the replay**: a hit (+0xC 0 -> 1) delivered
        that way is the rec+C failure. Which call site issued them (0x008A7564 with the scene's default query object, or
        0x006F3E12's own query object; callers of its function 0x006F3D50: 0x006FA5ED, 0x006FE5CD) is what ABC36E6E's
        issue entries answer. A fix would then be to make that site's MK-helper queries synchronous during sessions
        (e.g. pass the scene's default, synchronous query object), so +0xC is written inside the issuing tick.
        The ring for 6E985C70: one result per frame, delivered on the game thread (20952) in some frames and on PhysX
        worker threads (17020, 6760) in others, in the original and the replay alike: the delivery point is thread
        timing, not the simulation. 162 such events in 13158's VMTRACE lines, every one "0 hits".
      - **Soak 13 total (13156-13161): 4,075 loads, 16,300 replayed frames, 0 crashes, all alive; 0 real failing loads**
        (13157 and 13159: one end-of-fight group each). No rec+C case this time. **13159 logged an asynchronous hit:**
        `RAYRES` helper 6A006980, thread 9924, gf 2396, **+0xC 0 -> 1**, caller PhysXCore+?1FA1 (the boolean-query path).
        So results that set the hit flag do arrive off the game thread; whether for an MK object's own helper is what
        soak 14's issue entries show.
    - **The rec+C writer found (soak 14's first minutes, ABC36E6E's ring):** per frame, for each moving MK object's
      helper, in order: the line-check *result* (report callback, on the game thread in some frames and on a PhysX worker
      in others, PhysXCore+191FB5), the *tick gate* read, then the *issue* at 0x008A7564, which 0x006BDC00 answers
      "synchronously" with a provisional +8 = 1, +0xC = 0 (`issue@8A7564 sync +C 0>0 +4/+8 11>11`); results ~= issues
      (207 / 208), 0x006F3E12 issued nothing in these fights. So **every MK object line check is answered one frame later
      through the report**, and RayQueries' 2026-11-04 "0 left in flight" only saw the provisional +8 = 1. At a load the
      queries issued in the abandoned future's last frame are still queued: their results land in the restored helpers
      during the first replayed frame, where the original got the answers to its own S-1 queries. Same ray most of the
      time ("no hit" both); a projectile about to hit gives +0xC 1 in the replay only = the rec+C desync (13155 S=3254).
      Both report handlers only write the helper (0x006BDA80: +0xC = result, +8 = 1; 0x006BDB30: hit data through the
      helper's vt[0], then +0xC / +8), so a stale result can be dropped.
      - **Fix (RayQueries, installed again, `rayqfix` default on; build 84C59BBF in `build\clang_rayq`):** every issue
        at 0x008A7564 for an MK helper counts as pending until its result arrives; each snapshot save records the helpers
        with a pending query; a load marks every pending query stale (its result is dropped) and re-issues the saved
        frame's pending queries from the restored helpers (+0x34 -> +0x40), putting the helper's 0x4C bytes back after
        the call (only the queued query remains). Chained after RngTrace's lab tap. `RAYQ` lines count issues, answers,
        stale drops and re-issues every 600 loads. Tests queued: batch 3 (net8, P2P, this DLL in `build\clang_net8`) and
        batch 4 = soak 15 (seeds 13168-13173, lab trace) against soak 14 (same trace, no fix). Expected: no tick-gate
        rec+C differences (soak 12 had 6 in 6 sessions) and no rec+C failing loads.
      - **Soak 14 (ABC36E6E, no fix; seeds 13162-13167, 00:58-02:22): 0 real failing loads**, 6 end-of-fight groups
        (each 4 errors then "replay ended at gf=4"), 0 crashes. **One rec+C case caught with the full ring** (13162,
        Reptile vs Mileena, Goro's Lair, gf 2541, k=4): helper 5A0F12C0, newest first: `replay gate read +C 1` <- `replay
        OTHER tid 30440 ray 1 +C 0>1` (PhysXCore+?1FA1) <- `live issue@8A7564 gf 2544` ... `live gf 2541: ray 0, gate 0`.
        The result read in the first replayed frame answered the query issued in the abandoned future's last frame (2544,
        the projectile 3 frames further on, a hit), where the original read the answer to its own 2540 query (no hit).
        Confirms the mechanism. 407 VMTRACE differences in all, mostly `tickgate/rayres` swaps (the result delivered on
        the game thread in one pass and on a PhysX worker in the other: thread timing, same "0 hits"), plus `none/rayres`
        x8 (a stale result for a helper the original had no result for: 13163 gf 2549, helper 7005F430 whose last issue
        was live gf 2552) and `rayres/none` x2 (the original's pending query lost at the load).
        **0x006F3E12 issues in 4 of 6 sessions, every one asynchronous** (13163 469, 13164 224, 13165 1,580, 13166 306;
        Bell Tower the most): the embedded helper stays in flight (+8 = 0) until the result, so these are pending at saves
        too; the fix does not cover them (dropping their stale results would leave the restored helper waiting forever;
        they would need their issue arguments recorded and re-issued). No failure traced to them.
      - **Fix made order-independent (BD3FC2F7, `build\clang_rayq`, for soak 15):** the saved frame's queries are
        re-issued from proxies (64 static 16-aligned copies of the restored helpers); a proxy's result is forwarded to its
        helper, and every result whose userData is the helper itself is dropped while that helper has stale queries
        (during the load frame; `stale never came` counts expiries). Before, with a stale and a re-issued query for the
        same helper, the first result to arrive was dropped, which relied on PhysX reporting in issue order. The restored
        helper is no longer touched by the re-issue. `RAYQ` adds: loads with both for one helper, forwarded / dropped /
        helper gone, re-issues abandoned by a load, no slot. net8 (batch 3) runs the earlier 84C59BBF.
      - **Soak 15 (BD3FC2F7, fix on, lab trace; seeds 13168-13173, 03:07-04:32): 0 real failing loads, 0 rec+C**, 3
        end-of-fight groups, 0 crashes, all alive (5,573 loads). `RAYQ` in the first 600 loads of each session: 1,582
        issues, 1,450 answered (all one frame later), 131 pending at loads, 131 stale results dropped, 130 re-issued
        (114 with the future's query for the same helper), **130 forwarded, 0 dropped, 0 helper gone, 0 without a slot,
        0 stale never came**. VMTRACE differences 407 -> 361 (soak 14 -> 15): tick-gate rec+C 1 -> 0, `none/rayres` 8 ->
        3; `tickgate/rayres` + `rayres/tickgate` 359 -> 283 (thread timing, as before). `rayres/rayres` 11 -> 45: two
        helpers' results swapped in the load frame, the same pair at every load (13171 gf 2318-2402), because OnSave
        listed the pending helpers in hash-map order, so the proxies were issued out of the original's order; plus a
        helper allocated anew inside the replay window (13171 gf 2446-2449: same object, another address). `rayres/none`
        x7: the replay's result came on a PhysX worker after the frame's event list closed (5, the ring shows it in the
        same frame), or for that replay-allocated helper (2). Each result writes only its own helper, so none of these
        changes state. **Re-issue in issue order:** Pend keeps an issue sequence number and OnSave sorts by it; RAYQ
        counters are also printed at each session end (RayQueries::Clear). Built as `build\clang_rayq2` (B46FBB33);
        soak 16 (seeds 13174-13179, lab trace) on it from 04:47.
      - **Soak 16 (B46FBB33; 04:47-06:13): 0 real failing loads, 0 rec+C**, 2 end-of-fight groups, 0 crashes, all
        alive (5,502 loads). RAYQ at each session's end: 2,313 issues, 189 re-issued, **189 forwarded, 0 dropped, 0
        helper gone, 0 without a slot, 0 stale never came** (176 loads with the future's query for the same helper).
        `rayres/rayres` 45 -> 13: the per-load swap is gone; the rest are helpers allocated anew inside the replay window
        (pairs of consecutive frames, another address). VMTRACE total 381 (`tickgate/rayres` swaps 338: thread timing).
        net9 (net8's six P2P units on B46FBB33, `results\scenarios\net9*`) queued as batch 7.
      - **net9 (B46FBB33, P2P; 06:14-06:59, 18 fights, `stallsample 30`): as net8, no regression.** 0 crashes;
        desyncs only the known harness ones (DeadPool intro frames 192-201 in 4 of 6, frame-0 start in 4 of 6); no
        in-fight hitch of 100 ms or more. RAYQ in every P2P session: 0-2 re-issues per fight, all forwarded, 0 dropped,
        0 helper gone. Handshake medians as net8 (ns30 82-86, ns60 118-133, ns100 184 ms). Start-barrier waits: 2 of 25
        over 500 ms, both net9a ns30_d2_loss5 (666 / 697 ms): fight 1, game A had waited 14 s (attract demos), got its
        first reply 87 ms after B started, then lost its second-round request or reply twice: retries after 2 x RTT +
        50 ms and then the doubled timer (230 + 410 ms), while B, synchronized after 83 ms, held frame 0. A random
        double loss at 5% loss each way; possible trims: no doubling for the first retries, or a 1-round-trip handshake
        (round 1 already proves both directions). ns100_d3 rollbacks cost 46-68 ms at worst (load 7-13, replay 4-11,
        replay saves 12-41 ms): replay saves remain the largest in-fight cost on high-latency links.
    - **Sparse replay saves + 1-round-trip handshake (`build\clang_sparse` 24E3C4ED = B46FBB33's sources + these;
      GGPO from `tools\build_ggpo.ps1 -Out build\ggpo_sparse`, linked with `build.ps1 -GgpoLib`; `build\clang\libggpo.a`
      untouched):** a P2P replay skips the saves of odd frames after `ggpo_rollback_safe_frame` (`sparsesave`, default
      on; the slot is marked not loadable). Patched `Sync::AdjustSimulation`: the newest frame at or before the target
      that MK9Rollback calls loadable (`ggpo_frame_loadable` -> SlotFor), never before `_last_confirmed_frame` (inputs
      are discarded below it), is loaded and the frames between are replayed with their confirmed inputs. A target is
      always after the last received frame, so the frame before an odd target is even and at or after every earlier
      replay's safe frame: at most one frame back (~1.5 ms of replay against ~6 ms per skipped save). Depth stays inside
      the quarantine / ActorDefer windows (GGPO's prediction limit). The stop line counts skipped saves, loads that went
      back and misses (nothing loadable: the target is loaded as before). `NUM_SYNC_PACKETS` 2 -> 1 (the first reply
      proves both directions; round 2 cost a round trip on every start and, lost twice, the 666 / 697 ms net9 starts).
      net10 (batch 8, `results\scenarios\net10*`): net9's units plus ns100_d3 b, to compare with net9 fight by fight.
    - **net10 (24E3C4ED; 07:08-08:00, 21 fights): 0 crashes; desyncs only the known harness ones** (DeadPool intro 192+
      in 4 of 7, frame-0 start in 2 of 7). **Sparse saves:** 60-117 replay saves skipped per fight at ns100, 16-67 on
      jittery, ~0 at ns30 (1-2 frame rollbacks); loads that went back a frame 0-1 per fight (a misprediction rarely lands
      inside the last replay: the replay predicts the input that just changed, which then holds), 0 found nothing.
      **ns100_d3 frames >= 34 ms per fight 9-18 (net9) -> 3-5, P99 33-134 -> 27-29 ms**; replay-save ms per frame
      0.75-0.86 -> 0.38-0.62; presents >= 34 ms unchanged (mostly the start barrier, the gf 0 restart and the fight-1
      GPU hitch). **Start (later game's session start -> both synchronized), medians: ns30 203 -> 115 ms, ns30_loss5
      205-822 -> 113-140, jittery 464-538 -> 208-215, ns100 418 -> 266-290**; start-barrier waits max 697 -> 260 ms. One
      548 ms start (net10b loss5 fight 2): both games started 50 ms apart, the second one's first request was lost, and
      neither had seen the other, so both waited SYNC_FIRST_RETRY_INTERVAL (500 ms).
    - **Handshake retries (`build\clang_sparse2` C7BEC36A, GGPO `build\ggpo_sparse2`):** SYNC_FIRST_RETRY_INTERVAL 500
      -> 150 ms. **Found while doing it, a hang in the net8 change:** a sync reply counted only if it answered the current
      or the previous request, so with retries every 150 ms (net8's peer-seen rule, in 84C59BBF / B46FBB33) a round trip of
      300 ms or more could never complete the handshake (each reply two requests late). Now the last 8 requests (random,
      send time) are kept and a reply to any of them counts (round trip measured from it). 778B84CE (the release zip)
      predates the peer-seen rule. New scenario `ns220_d4` (round trip 300-440 ms). net11 (batch 9): ns220_d4 and the
      lossy / jittery links x2 on C7BEC36A, then ns220_d4 on B46FBB33 as the control.
    - **net11 (C7BEC36A; 08:05-08:49): 0 crashes, 0 load misses; desyncs only the known harness ones** (frame-0 start on
      PitDay fight 2 in 4 of 5 runs, DeadPool intro in 1). **ns220_d4 start (later game's session start -> both
      synchronized) 355-357 ms (one round trip) on C7BEC36A, 1,464-1,685 ms on B46FBB33** (plus start-barrier waits of
      886-1,146 ms there): the old two-request window delayed it until a reply came back fast enough (the simulated
      latency varies 2/3..1 x; a steady 300 ms+ link could wait longer). Lossy / jittery start medians 110-150 ms, max
      230 (net10 548, net9 861). ns220_d4 itself is past what delay 4 hides (one-way ~11 frames): ~170 frames >= 34 ms per
      fight on both builds (B46FBB33 ~200), 273-332 replay saves skipped per fight, 5-9 loads went back a frame. **C7BEC36A
      replaces B46FBB33 as the next release candidate** (B46FBB33's handshake is slow on 300 ms+ round trips).
    - **Sparse live saves (`build\clang_sparse3` F91C3717 = C7BEC36A + `sparselive`, default on):** the live save
      (~4 ms every frame) is skipped on odd GGPO frames too (the boundary marks the slot not loadable; a replay that
      reaches an odd live frame skips it as well). Same rule as the replay saves: even frames are always saved, so a
      rollback to an odd frame loads the frame before (about half the rollbacks, +1 replayed frame). Checksums for the
      peer are recorded in the save callback whether or not a snapshot is taken. net12 (batch 10): ns100_d3 x2, jittery,
      loss5, LAN, and the slow-PC scenario (B +6 ms per frame) with sparselive on and off.
    - **net12 (F91C3717; 08:53-09:45): correct but not smoother -> `sparselive` default off (lab switch).** 0 crashes, 0
      load misses, desyncs only the harness ones (frame-0 start in 6 of 7, DeadPool intro in 3). Half the live saves
      skipped (982 of 1,965 per fight) and about half the rollbacks went back a frame (57-62 of ~126 at ns100). Frames
      >= 34 ms per fight: ns100_d3 7/7, 2/7 and 4/3, 3/2, 5/3 (net10: 4/5, 4/3, 4/4, 1/2, 2/3); **slow PC (spinB6) game B
      fight 1: 42 with sparselive, 6 without** (same build, `sparselive 0`). The skipped save comes off an ordinary frame
      that had slack; the replayed frame it costs lands in a rollback frame, the longest kind, and on a slow PC that is
      the expensive part. **Release candidate: `build\clang_rc` AE6FE750** (sources as now: sparse replay saves on,
      sparselive off; GGPO `build\ggpo_sparse2`: 1-round handshake, 150 ms first retry, 8-request reply window).
      net13 (batch 11): LAN, ns30, ns100, loss5, jittery, ns220 on it.
    - **net13 (AE6FE750; 09:47-10:40): one crash, pre-existing.** ns100_d3 fight 1 (Graveyard, Jade v Reptile) game B,
      resim EngineTick(100553) after a load of S=100552 (GGPO frame 551, odd: a direct load, not a sparse fallback):
      eip 0 via the Kismet activation loop (0x0073ABB0/0x0073ABE1 -> 0x0073A791 -> 0x004146AA), ecx/edi 3D9343C0 in a
      quarantine-released block (freed gf 100528, released 100548), esi SeqAct_Cinema_0. From S=100456 on every load said
      `SeqAct_Cinema_0 +0xE0 held 3D9343C0 at the save, no longer a live object ... nothing restored`: the field held an
      object destroyed before every one of those saves, the check counted it as a reference the restore would bring back,
      and the whole cinema / Kismet restore was skipped at each load, leaving the abandoned timeline's activation queue;
      the replay activated the cinema and called through the dead object. The other five net13 runs: no crash, desyncs
      only the harness ones; ns220 starts 1 round trip as in net11. Blocked restores ("nothing restored") are common in
      every run since net1 (176 logs: `SeqAct_Cinema +0x104 now collected` 881 = the looping stage cinema; "no longer a
      live object": CinemaGroupInstSpawnedEmitter +0x3C 282, SeqVar_Object +0x8C 76, SeqAct_Cinema +0xE0 65); this is the
      first crash from one in ~260 game-fights of net runs.
    - **Fix `cinemadeadok` (default on; `abtest cinemadeadok`; `build\clang_rc2` 5CD892F7):** each cinema save also lists
      the declared object-field values that named no live object at the save; such a value no longer blocks the restore
      (putting it back is S's own state, which the first pass ran from). A reference live at the save and collected
      since still blocks. The 300-load summary counts the dangling-at-save references put back. Batch 12: net14 (ns100 x2,
      jittery, loss5, ns30, LAN) and soak 17 (SyncTest, `abtest cinemadeadok`, seeds 13180-13185) on 5CD892F7.
    - **Batch 12 (5CD892F7; 10:46-13:03): no regression; the fix's path did not come up.** net14 (18 fights): 0
      crashes, 0 load misses ("found none"), desyncs only the harness ones (DeadPool intro at 208 / 268, PitDay frame 0
      in 1 of 6). Frames >= 34 ms per game-minute as net13 or lower (LAN 3.6-4.3, ns30 2.4-3.7, loss5 4.3-4.9, jittery
      4.3, ns100 5.5-10.3); live save 3.9-4.6 ms (the dead-at-save list costs nothing measurable). Starts (later game's
      session start -> both synchronized): medians LAN 26, ns30 82, loss5 118, jittery 123, ns100 186-189 ms, max 224.
      **Over net11-net14 (64 starts on the 1-round handshake), every start outside ns220 is <= 242 ms** (net9: 2 of 25
      lossy starts ~0.7 s): the lossy-start stall is closed. Soak 17: 6,874 loads, 0 crashes, all alive, 2 end-of-fight
      groups and **1 real failing load, 13180 S=3270: the known hit-test class** (VMTRACE original `(1, 0xCE)` ctor up
      0x008A4C16 <- 0x008A73AE vs replay `tickgate`, actor +0x80 0x2040 vs 0x2000; P1 y -702.96 vs -692.96), on an OFF
      load, same as 13105 / 13109 / 13110 / 13114. **`dangling-at-save references put back`: 0 in every log of both
      runs** (~9,500 loads), so ON and OFF ran the same code path and the batch shows the change is inert where it does
      not apply; the net13 case (a SeqAct_Cinema field dangling across every save) did not recur. The remaining "nothing
      restored" loads (37 in 2,617 net14 loads; net13 46 in 2,150) are all references live at the save and gone since
      (mostly CinemaGroupInstSpawnedEmitter +0x3C, 0 collected): the protection that stays. **Release candidate:
      `build\clang_rc2` 5CD892F7 (GGPO `build\ggpo_sparse2`)**, pending the user's OK for the gate and zip.
    - **How rare the cinemadeadok case is:** in 758 logs of the last 8 days (905 "no longer a live object" lines), one
      (object, field, value) was reported across loads more than 12 frames apart (dangling across saves): net13's crash
      (`SeqAct_Cinema_0 +0xE0 = 3D9343C0`, 7 loads over 96 frames). No ordinary run can be expected to exercise the fix.
    - **Where an ns100 rollback's time goes (net10-net14 ns100_d3, 3,266 rollbacks, PACE windows):** per rollback load
      6.2-7.3 ms (LOADAB 3.3-3.8, of which the master image 1.3; quarantine 2.1-2.3, nearly all the requeue reference
      scan, ALLOCLEAK 1.85-2.06 ms/load; attach 0.8), replay 3.9-4.2 ms (depth ~3), replay saves 7.5-9.4 ms (~1.7 saves,
      ~5 ms each against a live save's 3.0-3.7). Fights 1-3 average the same; of the 16 rollback HITCHes >= 40 ms in those
      14 logs, 13 were in fight 1 (tails, not the mean: the STALL samples sit in ordinary save work, master memcpy and
      the fighter actors' pointer classification). **The harness ran ns100 at delay 3; the online mode's ping rule
      (`DelayFromPing`, OnlineMode.cpp) picks 4 for a 133-200 ms round trip** and caps rollback at 4 (ns220's 300-440
      ms too). Batch 13 (net15, `results\scenarios\net15*`): ns100_d4 x2, ns220_d4/d5/d6, ns60_d3 (the rule's pick for
      80-120 ms) on 5CD892F7.
    - **Release of 5CD892F7 approved by the user (2026-11-06 ~13:20).** build\clang and build\clang2 now hold it (DLL,
      PDB, injector 0F9C0502); **build\clang\libggpo.a is now the ggpo_sparse2 library (BB382E34)**, so a default
      `.\build.ps1` links the released handshake; the 778B84CE files and the net8 libggpo.a (88F4AD06) are kept in
      `build\clang_778B84CE`. Batch 13 was stopped after its first unit (net15a ns100_d4); the guarded chain
      (`results\release_20261106.txt`) runs the gate (`results\release_gate_20261106.txt`, -Now once away), the -MenuOnly
      zip if only the menu walk failed (`results\menu_only_20261106.txt`), then batch 13's other five units.
    - **Gate on 5CD892F7 (13:23-13:46, PC away): online smoke, plain / auto re-arm (3 re-arms per game) / delay-change
      (21 changes) regressions 0 mismatches, 0 failed saves, 0 crashes; menu walk FAIL (game not in the foreground,
      locked PC).** -MenuOnly at 20:42 (unlocked idle PC): menu walk passes, **zip
      `dist\MK9Rollback-netplay-20261106-2042.zip`** (DLL 5CD892F7, injector 0F9C0502, checked by extracting).
    - **net15 (5CD892F7), input delay on high-latency links, frames >= 34 ms per 33 s fight (A/B):** ns100 at delay 4
      (the ping rule's pick) 3/2, 2/2, 2/3 (P99 19-21 ms, depth 1.7) against delay 3's 5/9, 2/3, 2/5 (P99 25-30, depth
      2.65-3.12): at the delay real matches use, a 133-200 ms round trip is as smooth as ns30. **ns220 (300-440 ms): delay
      4 (the rule's cap) 118-146 per fight, delay 5 28-61, delay 6 5-46** (presents >= 34 ms: 127-171 / 20-52 / 2-15).
      Above the cap, each frame of delay removes most of the stutter at that ping; the cost is input lag (6 frames =
      100 ms). Raising the rollback cap (e.g. to 6 for round trips over ~200 ms) is a game-feel choice for the user.
    - **Ping rule up to delay 6 (user: "Yes, up to 6"; `build\clang_rc3` CD2F60DE):** `DelayFromPing` rollback: 2 up
      to a 100 ms round trip, 3 up to 133, 4 up to 200 (unchanged below 200 ms), 5 up to 250, 6 beyond
      (`kMaxRollbackDelay`); launcher / README / header texts say 2-6. An older peer agrees too (the session takes the
      larger proposal; `sessiondelay` accepts 1-8). The gate's delay-change regression now jumps to 6 and 5.
    - **Combos were never scripted (user question, 2026-11-06):** every harness fight so far was random input (4-23
      frame holds of a random direction + button, both players), CPU-free attract fights, plus the X-ray probe; that
      lands hits but rarely strings, launchers, juggles or specials. New `tools\combo_inputs.ps1` (`-Combo` on
      two_instance, noai_rollback / synctest_soak, scenario set `combo`): one attacker per batch walks or dashes in and
      does dial strings, string into special, down+2 launcher and juggle, specials (d-f, b-f, d-b, f-f, b-b, d-d; a third
      enhanced), special into string, throws, low into string, X-ray attempts; the defender stands, blocks or
      crouch-blocks. `hold` masks take 0x10000 forward / 0x20000 back, resolved towards the opponent in the game on the
      live frame (CommandChannel; replays get the resolved mask). SyncTest combo soaks poke both meters full before the
      session. Batch 14 (`results\batch_20261106b.txt`, after batch 13): delay-change 5/6 regression, soak 18 (combo,
      13190-13193), combo1 (six links) on CD2F60DE.
    - **net15 so far (5 of 6 units): 0 crashes, 0 load misses, desyncs only the harness ones** (DeadPool intro at 153 /
      236 / 285, PitDay frame 0 once). ns60 at delay 3 (the rule's pick for 80-120 ms): frames >= 34 ms 2/3, 3/1, 2/1
      per fight, P99 18-21 ms, depth 1.1-1.3. **Both background chains ended with the Claude Code session at ~21:30**
      (net15b ns100_d4 never ran, batch 14 never started; no game was running). Restarted 2026-11-07 03:13 as one guarded
      chain (`guarded_batch14.ps1`: net15b ns100_d4 first, then batch 14).
    - **Batch 14 (CD2F60DE, 03:18-04:40).** net15b ns100_d4: frames >= 34 ms 4/4, 2/2, 2/3 per fight (net15a 3/2, 2/2,
      2/3). **Delay changes 5/6 (`results\two_instance_delaychange56*`): pass**, 6 reports 0 mismatches, 20 changes
      applied (B 2 -> 6 and A 1 -> 5 each fight), 0 crashes.
      **Soak 18 (SyncTest + combos, 13190-13193): 4 of 4 sessions crashed, a new crash:** eip 0x005F975D, the UE3
      garbage collector's reference-token loop (token 3 = TArray<UObject*>: object [ebp+0x10], header [ebp-0x20]),
      on the game thread or an OpenMP worker (VCOMP90), reading element pointers out of an array block the quarantine
      had released ~20 frames after the game freed it (edx: `quarantine-released block ... freed gf=6929 released
      gf=6949 caller 007903BC`; the block held text by then, e.g. read addr 7A62757B "{ubz"). Never in any earlier log.
      Random input never pressed block with flip stance or with an attack, so no harness run had X-rays or enhanced
      specials under SyncTest beyond the X-ray probe. Cause: CinemaRestore::OnLoad writes the cinematic / Kismet
      objects (raw bytes, TArray headers included) and their saved arrays back *after* FreeQuarantine::OnLoad's requeue
      scan, which only sees the snapshot spans; a block the scan requeued that the restored cinema state points at is
      then freed for real while referenced. **Fix (`protectlate`, default 1; `build\clang_rc4` 7ABE2849):** OnLoad keeps
      the blocks it requeued; after the cinema restore, `FreeQuarantine::ProtectReferenced` scans what it wrote back
      (`CinemaRestore::LastRestoredRanges`) and takes referenced ones out of the quarantine (live again), logged
      `QUARANTINE load S=...: block ... referenced from ... restored by the cinema restore after the requeue scan`.
      The crash report now decodes this GC loop (`CRASH?   GC token: object ..., array header ...`). First rc4 session
      (13190): 19 such blocks kept in 11 min (callers 007903BC, 0064BBEF, 006FE66A, 007B0ACB), no crash so far.
      **combo1 (two games, combos, six links), per 33 s fight, frames >= 34 ms:** ns30 1-4 (P99 16-18 ms, as random
      input), ns60 1-4 (P99 22-23; random 18-21), ns100 2-6 (P99 26; random 19-24), ns220 81-94 (random 5-46): combo
      input changes every 3-5 frames, so the defender's game rolls back up to twice as often (B 190-224 rollbacks per
      fight at ns100 against ~120). 0 crashes; desyncs only the harness ones (DeadPool intro 192+, PitDay frame 0).
    - **No harness fight ever took damage.** All 8,665 NETPLAY STATE lines in the scenario logs show life 1.0 / 1.0
      (the 11-03 two-PC match: 0.94 ... 0.01 by frame 2100). Damage (0x008988F0; `life += damage` at 0x00898A4D) is
      skipped in every game mode ([[0xDEF4B4]+8], getter 0x00406850) except 4, 8 and 9 (byte table 0x00887B20, via
      0x00887AF0) unless bit 0 of the flags word [[0xDEF4B4]+0x10] is set (setter 0x00406A90; the same bit gates a few
      other "real fight" checks: 0x0045178D, 0x0088D188, 0x0088DCB9, 0x00895618). So KO, low health, round ends, Finish
      Him and fatalities never ran under rollback in the harness. **`demodamage 1`** (`build\clang_rc5` E7298DEC) sets
      the bit; `-Damage` on two_instance (sent with killai while both games are paused at the session's first frame),
      noai_rollback / synctest_soak (per fight, before SyncTest starts); scenarios `*_combodmg`. Batch 16
      (`results\batch_20261107b.txt`, after batch 15): soak 20 (combo + damage, 13195-13198), combo2 (ns30/60/100).
    - **Soak 19 (7ABE2849, combos, 04:47-06:00): the GC crash is gone.** The four seeds that crashed in soak 18
      (13190-13193) ran 13 min each, 0 crash lines, all alive; 34-40 late-protected blocks per session (callers
      007903BC, 00401838, 005AA097, 006FE692 ...). Control 13194 with `protectlate 0`: no crash either (another seed,
      15 short fights, 897 loads), so it shows nothing; the four soak 18 / soak 19 pairs carry the evidence.
      **8 real failing loads in ~5,400** (random input: ~1 in 7,000), **7 of them X-ray starts:** both fighters'
      actors differ in +0x8 (0x00104010 vs 0x00104000), +0x58 Physics (7 vs 0), +0x138, +0x144 (cinematic controller vs
      0), +0x1A4, +0x288: the first pass entered the X-ray, the replay did not; VMTRACE first difference `original gate
      ... ret 00455594 | resim none` and native 0x00459910 called only in the first pass. 0x00455594 is the end of
      0x004554C0 ("enter cinematic": char +0x1D0 |= 0x20000, +0x1D4 = 0x4203, then 0x004583A0(1) on the handle at char
      +0x2FA4/+0x2FA8): the X-ray-start class of probes 8-13 (2026-11-02), where the replay found the abandoned
      timeline's X-ray level node in the world (engine state outside every restore), skipped its Kismet setup and
      started the X-ray a frame late. Rare in those probes (1-2 errors per run); with combos it hits about every X-ray
      a SyncTest replay crosses. The 8th (13193 S=5306): native 0x808D0840 / 0x808B6DA0 returned another object
      (110EB420 vs 110EB3A0) after `COMPKEEP fightercomp +0x288 kept live`. **No online run had an X-ray yet** (combo1:
      meters never full, 0 cinematic-controller lines). Batch 17 (`results\batch_20261107c.txt`, after batch 16): xray1,
      two games with combos, damage and both meters full at each session start (`two_instance -FullMeter`, poked while
      both are paused at frame 0), X-ray attempts first, at ns30 / ns60 / ns100.
    - **`demodamage` on E7298DEC did nothing:** `DEMODAMAGE 1: game mode 1, flags 000C0001 -> 000C0001` (bit 0 of
      +0x10 is already set in attract demos), life stayed 1.0. The skip that applies there is bit 0 of
      [[0xDEF4B4]+0x68] (test 0x00898A47, `jne` 0x00898A4B over `life += damage`), set in demos (cleared by the demo
      flow at 0x009568F6; also read at 0x004071C0, 0x004072C8, 0x00810A8C for pause / fight-flow handling). The command
      now patches only that `jne` (75 28 -> 90 90, CodePatch, `demodamage 0` restores): `build\clang_rc6` 136572D4.
      Batches 16 / 17 (on E7298DEC) were stopped at 06:07 (their one running game closed; soak 20 13195 cut short) and
      replaced by batch 18 (`results\batch_20261107d.txt`, 06:10): xray1 (ns60 / ns100 / ns30), soak 21 (combo +
      damage, 13195-13197), combo2 (ns60 / ns100 with damage) on 136572D4.
    - **Batch 18 (136572D4, 06:10-07:32).** Damage now works (life 1.0 -> 0.42-0.65 by the demo's end, CODEPATCH
      logged). **0 crashes in all 8 units.**
      **combo2 (online combos + damage), per 33 s fight, frames >= 34 ms:** ns60 2-4 (P99 18-21 ms, 59.6 fps), ns100
      2-5 (P99 23-25 ms); checksums: Graveyard and PitDay fights 0 mismatches, DeadPool only the known intro artifact
      (frames 192-201, every one a one-frame shift). No fight reached a KO (the demo ends by time at 1,965 frames).
      **Soak 21 (SyncTest combos + damage, 13195-13197, 13 fights): 0 crash lines, all alive;** sync-error reports: 5
      sessions with the X-ray-start class at frames 36-116 (VMTRACE `original gate ... ret 00455594 | resim none`), 1
      harness artifact (13196 frame 456: the next fight's meter `poke` reached the game while the previous SyncTest
      was still running: `P1 meter 1.0 -> 0.0`), 3 at a session's last frame when the game's fight-end timeline restart
      ran inside a SyncTest replay (`GGPO: replay ended at gf=4`; also on 11-07 without damage, 13190 / 13191).
      **xray1 (online, `-FullMeter`): no X-ray happened.** `poke` is lab-only (`CMD ignored`), the commands sent at the
      paused first frame reached fight 2's games 2 s after the session had started, and the attract script sets the
      meter (0.3333) during frame 0. Damage worked there; 0 crashes; desyncs only DeadPool's intro.
    - **`netplaymeter F` / `netplaylife F` (2026-11-07 evening, `build\clang_rc7` BBFB27B0 = 136572D4 + these):** both
      meters / both fighters' life (char +0x78C0) set at session frame gNetplayGf + 8 of every local P2P session (not
      MK9 online fights), before that frame's save, in the live tick (`NetplayFrameEvents` before the save at the
      boundary) and in every GGPO replay that passes the frame (`GgpoAdvanceFrame` after ++GameFrame, before
      `ggpo_advance_frame`'s save): the same write on both games and in every replay. two_instance sends them and
      `demodamage 1` when arming (`-FullMeter`, new `-LowLife 0.25`); in X-ray batches the defender stands still.
      New scenarios `ns60_d3_ko` / `ns100_d4_ko` (combos, damage, 25% life: KOs and round ends under online rollback).
      Batch 19 (`results\batch_20261107e.txt`, guarded): xray2 (ns60 / ns100 / ns30) and ko1 (ns60 / ns100) on rc7.
    - **Two-PC field session 2026-11-07 21:06-21:46 (release 5CD892F7, the user vs a friend over Steam, watched live
      with a read-only log follower; user's log kept as `results\field_20261107_session2.log`, the first part lost to
      the restart's overwrite).** 13 matches, ping 44-69 ms (mostly 50), delay 2 all night. **0 desyncs, 0 crash
      lines**; 58.4-60.0 fps; most matches had 0-10 rollbacks (inputs arrived inside the 2-frame delay).
      **14 freezes of 0.58-0.84 s, every one this game waiting for the friend's input** (wait 582-775 ms, durations
      clustered at ~585 / ~647 / ~773 ms), then 1-2 s of catch-up rollbacks (up to 8 deep, 30-40 ms per frame) and
      balance corrections (this game 3.75-5.6 frames ahead): the other game or the link stalled; this PC is wired
      (1 Gbps Ethernet). Suspect the friend's Wi-Fi (background scans); their log would tell a network gap (they wait
      too) from their game freezing. **Local hitches 0.05-0.27 s after KOs** (boundary ~4 ms: the game's own work in
      the KO / victory sequence, not the netcode); compare with the mod passive. One exit at 21:20:15, ~4 s after a KO
      mid-session, through the normal WinMain return (no crash line); not known whether the user closed it.
      Follow-ups: a HITCH line could say which side stalled (GGPO's remote frame advantage at the wait). (The release's
      log collector already zips `previous_MK9Rollback.log`: `mk9r-logs-*.zip`.)
      **The friend's logs (`mk9r-logs-20261107-212029/212155/215000.zip`, same build, clock ~0.5 s behind) explain all
      14:** 4 were their whole game stopping ~830 ms (engine tick 838 / 829 ms in matches 1-2, the mod's live save
      825 / 822 ms in matches 7-8: the same length wherever it landed, so the process was frozen, not slow code); 10
      were network gaps in which both games waited, theirs 440-635 ms and ours 582-775 ms, ours ~135 ms (8 frames of
      prediction) longer: their packets to us stopped. All on the friend's side; their PC also runs the save ~1.7x
      slower (saveab 4.9 vs 2.8 ms). The 21:20 exit: ours left first (21:20:15.2); theirs reported "the peer stopped
      responding" and exited 9 s later.
    - **Stall hold (`build\clang_rc8` 2F565195 = rc7 + this; user: "make the online game better, as smooth as Street
      Fighter 6").** In the field logs every 40-55 ms rollback frame (17, both games) came in the 3 s after one of the
      26 stalls, none elsewhere: the game that came out ahead stayed 4-8 frames ahead for 1-2 s (BALANCE gives <= 3
      frames per 40, paid 2 ms per frame) and rolled back 4-8 frames on each input change. `StallHold`: when a
      boundary waited >= 100 ms at GGPO's prediction limit, it keeps waiting (<= 150 ms more) until the peer's
      confirmed frame is within target = max(0, ceil(ping/2 / 16.7 ms) - delay) + 1 frames, then restarts the balance
      sampler; `STALLHOLD gf=...` lines, `stall holds N (ms)` in the session stop line; `stallhold 0` off,
      `stallhold MIN MAX`. Test switch `freezeevery MS EVERY` (game thread sleeps MS every EVERY ms in a P2P session)
      and two_instance `-SetupB` (B-only commands, e.g. its own `netsim` stall). Scenario set `stall`: ns30_d2
      combos with B freezing 830 ms / B's sends stopping 600 ms every 10 s, each with and without the hold. Batch 20
      (`results\batch_20261107f.txt`, guarded): stall1 (4), ko1 (2), xray2 (3) on rc8; batch 19 never ran.
    - **Batch 20 (rc8 2F565195, 22:17-23:38, all 9 units ran; `results\batch_20261107f.txt`).** **Stall hold works:**
      freezeB (B's game stops 830 ms every 10 s) with the hold: A held 40-81 ms after 8 of 9 freezes and had **no**
      rollback hitch after any of them; without it A had 7 rollback hitches of 43-63 ms after 6 freezes (each 10-30
      frames after the freeze). Frames >= 34 ms per game-minute A 9.1 vs 20.3. **KOs under online rollback (ko1, 25%
      life + damage): KOs and round changes in every fight, ns100 0 checksum mismatches in 3 fights**, ns60 only the
      harness ones (PitDay frame 0, DeadPool intro 192+). **Online X-rays (xray2, rc7's in-loop full meters): the
      X-ray-start desync is real online**: ns60 fight 1 frame 483, ns30 fight 1 frame 328 (life 0.87 / 0.65, P2 meter
      1.0), each a lasting desync. **3 crashes, all Graveyard (fight 1), all the stage cinematic:** gapB A (resim
      EngineTick(100959), eip 65636E61) and xray ns100 B (game(100180), eip FFFFFFFF): 0x00414694, the SeqAct target
      loop ([+0xE0 data + 4i]->vt+0x38), on a purged EmitterSpawnable (GC purge 0x00647FA3) the quarantine had
      released; each after a run of loads "SeqAct_Cinema +0xE0 held X at the save, no longer a live object ... nothing
      restored" (10 / 4 such loads). gapB_nohold B (resim EngineTick(101919), C000001D): the same function's
      Targets.Empty() realloc (0x004146F9) on an array block freed at 101860 by TArray::Empty (0x00765138) and released
      at 101880, after a full restore at S=101917 (a header the save had copied from an earlier blocked load's
      leftovers). Pre-existing: the blocked +0xE0 loads appear in ~40 logs since net7 on every build (net13's crash was
      the same); the gaps (long runs of rollbacks) and X-rays make the runs long enough for the release. The friend's
      network gaps in the field are this case.
    - **Fix `cinemadeadrefs` (default on; `abtest cinemadeadrefs`; `build\clang_rc9` 05F7B2EE = rc8 + this):** dead
      references no longer block the cinema restore. Saved object-array elements naming no live UObject are set to 0
      after the load in every tracked object's object arrays (the collector's own treatment of references to what it
      destroys; 0x00414694 skips 0); a saved object field naming a dead object keeps the live value (0 if that is dead
      too); a restored TArray header whose block the game freed before S (`FreeQuarantine::FreedBefore`: still held
      with an older stamp, or released since S-64) keeps the live header. Only "now collected" (tracked objects
      collected since the save) still blocks. Logged: `CINEMA load S=...: dead references: N array element(s) set to
      0, ...` and totals in the 300-load line. Batch 21 (`results\batch_20261108a.txt`, guarded): gapB x2,
      gapB_nohold, xray ns100 x2 (the crash scenarios), freezeB, SyncTest soak 22 (combos + damage, 13195), xray ns60.
    - **Batch 21 (rc9 05F7B2EE, 23:56-01:10, all 8 units ran).** **The Graveyard crash is gone:** gapB x2, gapB_nohold
      and both xray ns100 runs played Graveyard fight 1 through (batch 20: 3 crashes there); Graveyard 0 checksum
      mismatches in the gap runs. Still blocking: "now collected" loads (SeqAct_Cinema +0x104 naming the previous
      loop's groups, ~1 per 200 frames), harmless. **freezeB as batch 20:** A held 40-72 ms after 7 of 9 freezes, no
      rollback hitch after any. **Soak 22 (13195, combos + damage): 4 fights, 1351 loads, 10 sync errors, 0 crash
      lines, alive** (soak 21 same seed: 12). **X-ray-start desyncs as before** (ns60 316, ns100 329 and 842).
      **Two new X-ray crashes at ns100:** (1) xray3a fight 2 (PitDay), both games at resim 100474 after loading
      100473: eip 0x0077D56A, the fight tick's walk of the fighter comp +0x3A8 list (element +8 == 0 -> node [+0xC]
      -> [node+0xA0], no null check) on CinemaTrackInstIK_2+0x48, an X-ray IK track instance unbound (node 0) and
      collected since S (gobj OTHER) that the complist span put back; pre-existing (the cinema restore leaves
      collected objects out, nothing checked the fighter list). (2) xray3b B at resim 100801 (0x0070451E, a TArray
      at [obj+0x100] whose data is a motion controller's vtable: use-after-free) right after the harness's fight-2
      start desync (frames 773-776, PitDay "different states" class); A then crashed at game(198) after the
      disconnect (0x007145BD, the old secondary). Also: rc9's header check fired on ~1/3 of X-ray loads
      (Jade_XRAY_0 / Main_Sequence_0 +0xD8, 2-3 arrays): **false positives** (below).
    - **rc10 (`build\clang_rc10` E76AA607 = rc9 + these):** `FreeQuarantine::FreedBefore` counts held blocks only. The
      released-block fallback matched a previous life of the block: a per-frame TArray realloc got its own released
      block back, the save held it live, the game freed it after S and the load's requeue scan made it live again
      (unlisted), so the old release record said "freed before S" and the abandoned timeline's header was kept.
      **Fighter IK list scrub** (`Snapshot::ScrubCompLists`, after the cinema restore and ProtectReferenced;
      `abtest compscrub`): +0x3A8 entries that are unreadable, inside a cinematic object collected since S
      (`CinemaRestore::LastGoneRanges`) or unbound with +8 == 0 are removed (as the game's unbind 0x00767060 does);
      `COMPLIST load S=...` lines, total in the session stop line. Batch 22 (`results\batch_20261108b.txt`,
      guarded): xray ns100 x3, gapB, xray ns60, soak 23 (13196), gapB_nohold.
    - **Batch 22 (rc10 E76AA607, 01:21-02:35, all 7 units ran).** Header keeps 0 (rc9's false positives gone). Soak 23
      (13196, combos + damage): 4 fights, 1349 loads, **8 sync errors** (rc9 10, older 12), 0 crash lines, alive.
      gapB: Graveyard 0 mismatches, no crash. X-ray-start desyncs as before (ns100 fight 1 at 304 / 849 / 847, ns60
      fight 3 at 575). **Three crashes:** (1) xray4c A, PitDay, game(100403) right after loading 100403:
      **0x0077D56A again** on CinemaTrackInstIK_4+0x48 (+8 = +0xC = 0); that load's scrub removed a different
      unbound entry (CinemaTrackInstIK +0x48 at 39213A68), so this one was still bound at the scrub and lost its node
      between the load and the tick. (2) xray4b B, PitDay, resim 100807 after loading 100797: 0x006F2D4D (and the render
      thread at 0x007145BD) on block 4019CE00, `quarantine-released (freed gf=100785 released gf=100805 caller
      007B0ACB)`. It is the per-tick render data of a component class (vtable 0x00D3B5B4, tick 0x007CE0D0 ->
      0x007C81F0): each tick replaces the 0x200-byte object at comp +0x268 (ctor 0x007041B0, comp at +0xFC) and queues
      a render command {vt 0x00D5AFA4, old, new} whose Execute 0x007B0A90 deletes old on the render thread. A command
      queued in the abandoned timeline ran during the replay from 100785, after the cinema restore had put old back
      (stamp = the replay's frame, so later loads took it for "freed before S"); the component did not tick again
      until 100807, by which time the quarantine had released the block. Batch 21's 0x0070451E (same object type,
      +0x100 array) is probably the same. (3) stall gapB_nohold B, Graveyard, game(101341): 0x007A0BAC, GetObjectVars
      (0x00730BF0) of SeqAct_Cinema_7 walked a SeqVar_Object_16 (badindex) whose object (freed gf 101295, released
      101315) was garbage; the loads before it were "now collected" blocked restores (SeqAct_Cinema_0 +0x104),
      nothing restored. Not addressed yet.
    - **rc11 (`build\clang_rc11` 5D05C598 = rc10 + these):** HandleGuard: the IK list walk at 0x0077D567 skips an
      element with +0xC == 0 (as the weights-below-threshold path, eax = index, ecx = comp at 0x0077D646;
      `HANDLEGUARD: fighter comp ... skipped`), and the render data delete command 0x007B0A90 is dropped when
      [[new+0xFC]+0x268] == old (a load put old back; `HANDLEGUARD: render data ... dropped`; old stays, the
      abandoned new leaks 512 bytes). FreeQuarantine: frees between OnLoad's scan and ProtectReferenced (render
      commands running during the load) are offered to ProtectReferenced too (`..., during the load` in the log).
      Session stop line: IK skips, render deletes dropped, late frees kept. Batch 23 (`results\batch_20261108c.txt`,
      guarded): xray ns100 x4, gapB, xray ns60, soak 24 (13196), gapB_nohold.
    - **rc12 (`build\clang_rc12` F19F3116 = rc11 + this):** HandleGuard at 0x007A0B7E: the cinematic action's first
      object variable must be a live UObject (GObjObjects[+4] == obj), else it is taken as none (0x007A0BC3, the
      function's own no-actor path; `HANDLEGUARD: a cinematic action's object variable names ...`). For batch 22's
      0x007A0BAC. Batch 24 (`results\batch_20261108d.txt`, queued after batch 23): gapB_nohold x2, gapB x2, xray ns100 x2.
    - **Batch 22's online desyncs are all one-frame shifts with A behind:** ns100 fight 1 (Graveyard) at 302 / 278 /
      275 and ns60 fight 3 (DeadPool) at 575: from there A's checksum of frame n equals B's of n-1 (identical states).
      A is P1, the side pressing in the xray scenarios: the attacker's game rolled back across its own X-ray start
      (a late defender input) and started it a frame late, as the class predicts. (The cinema/MOTIONCTL log lines are
      capped, so their absence near 575 says nothing.) In SyncTest soak 23 (13196) 4 of the 8 error lines were two
      X-ray starts (frames 144, 148: `LEVELVIS ... joined the tree after the save`, `LEVELREADY ... used`, VMTRACE
      `ret 00455594 | resim none`), the other 4 one end-of-fight frame (1368).
    - **`levelredo` (experiment, `build\clang_rc13` C896AA07 = rc12 + this; command `levelredo 0|1`, default off,
      also `abtest levelredo`):** a load marks root nodes of the visibility tree that are not in the save's children
      (added after S; sticky, since later saves list the leftover too) and the level find-or-add's lookup (call at
      0x00888484 -> 0x004ADD20) takes a marked node out (levelundo's keep-mode teardown 0x004B3860) and answers "not
      there", so the add path runs again at the same point of the same frame as the pass that first added it (levelundo
      did it at the load: X-ray two frames early, probe 13). `LEVELREDO` lines. Batch 25 (`results\batch_20261108e.txt`,
      queued after batch 24): SyncTest combo soaks 13196 / 13195 and online xray ns100 x2 / ns60, all with it on.
    - **Why the X-ray-start class matters online:** the side that learns of the press late replays the start for the
      first time (fine), but any later rollback that loads a frame before a start some pass already ran (a late
      defender input on the attacker's game, or a second correction on the other) finds the X-ray level's node in the
      world, skips the Kismet setup and starts the X-ray a frame late on that game only: a lasting desync. xray2 shows
      how often. A fix must make the replay redo the first pass's level add + Kismet setup at the same frame;
      `levelundo` (teardown at the load) started it two frames early instead (probe 13).
    - **Batch 23 (rc11 5D05C598, 02:50-04:00, all 8 units ran).** The IK guard works (xray5 ns60 A: one unbound entry
      skipped every tick, no crash); no render data delete was dropped and no late free kept in any unit. Desyncs as
      before (X-ray class: ns60 all 3 fights, xray5d all 3; gapB_nohold PitDay). PitDay frame-0 start mismatches in gapB
      and xray5c (the known harness one). Soak 24 (13196): **33 sync errors**, 30 of them in fight 1: the X-ray start at
      144, then fighter-separation drifts (posy +-0.01..0.03, identical at FightUpdate exit, different at EngineTick exit)
      at 256..1076 until the fight ended; fights 2-3 clean, fight 4 one frame (36). No rc11 code ran in the soak (0 guard
      lines), so this is the fight, not rc11 (soak 23 logged only 144/148/1368). **Three failures:**
      (1) xray5a A, DeadPool, game(100938): eip 0 in the motion-control refresh 0x00786290 (lowest non-empty slot's vt[3])
      called from 0x00813EC0, the effect manager at 0x00E92230 (ctor 0x00819E70, 0x104 bytes, called each tick from the
      world tick 0x007AE980 via 0x00814000) that swaps both fighters' slot 3 with its own controllers (vt 0x00D5DD2C,
      +0xF0..; stash +0xC0, slot index +0xD0, pawns +0xE0, active +0x100; reverse 0x0080F8D0). The manager is not in any
      snapshot. The dead controller was a block freed at gf 100918 = the load's S and released at 100938.
      (2) xray5c B, the same fight and the same load (S=100918: ACTORARRAY actor0 +0x68 2 -> 1 kept live, a 528-byte block
      freed by 00401838 revived for the cinema restore, MKATTACH re-attach), game(100919): a GMalloc allocation returned
      0x00D56A38 (the vtable of a 0x98-byte object built by 0x006FA470 for a component's +0xC8): a free list held a live
      object's block (double free or a write into a released block), and three threads then crashed.
      (3) xray5b B, Graveyard -> PitDay: HANG at the fight change (gf 101966) in 0x00516E70, the purge loop waiting for an
      object's IsReadyForFinishDestroy (vt+0x2C) that never came. Not seen before.
    - **X-ray lock (rc14, `build\clang_rc14` FE7D93E9 = rc13 + this; `xraylock 0|1`, `xraylockcine 0|1`,
      `xraylockwindow N`, on by default):** instead of more guards for loads during X-rays, P2P frames run only on the
      peer's confirmed input (the lockstep barrier in GgpoBoundaryP2P) for 30 frames after a flip-stance press (0x4000)
      by a player with a full meter (PlayerInfo +0xC14 >= 0.995: the local press as it is added, `delay` frames before it
      applies; the peer's from the masks GGPO hands over live or in a replay) and while a fighter's slot 1 or 2 holds a
      cinematic controller (X-rays, throws). A frame that ran on confirmed inputs is never rolled back, so no load lands
      on or inside an X-ray: no X-ray-start desync, no cinema restore during the cinematic. Cost: a wait only when the
      peer's input is late (none while the frame delay covers the trip). `XRAYLOCK` on/off lines; the session stop line
      counts locks, frames, cinematic frames, waits and ms. Left: a peer's buffered X-ray start that a replay runs on a
      predicted input before the press is known; throws still start on predictions (only the cinematic part is locked).
      Batch 26 (`results\batch_20261108f.txt`, replaces batch 25, after batch 24): xray ns100 x4, ns60, gapB with the lock;
      then soak 25 (levelredo, SyncTest) and xray ns100 with levelredo and the lock off.
    - **Batch 24 (rc12 F19F3116, 04:05-04:55, all 6 units ran).** Graveyard in the gap runs (gapB_nohold x2, gapB x2):
      0 desyncs, 0 crashes; the 0x007A0B7E guard never fired (0x007A0BAC did not come back, so no evidence either way).
      Desyncs only the known harness ones there (DeadPool intro 192 / 197 / 215, PitDay frame-0 start in 3 of 6). xray6b:
      X-ray-start desyncs (Graveyard 290, DeadPool 578). **xray6a B crashed** (PitDay, game(100742), during the X-ray
      cinematic after six CINEMA loads S=100714..100737): a GMalloc allocation returned 0x00D56A38 again (the same
      signature as xray5c B), the render thread at 0x006F2EF1 (from a command at 0x006FA4E0) next to a block the render
      data delete 0x007B0ACB freed at gf 100717 (released 100737), and 0x007145BD. Heap corruption from a load inside the
      cinematic: what the X-ray lock removes.
    - **Batch 26 (rc14 FE7D93E9, 04:56-, X-ray lock units first).** xray8 ns100 a/b/c: **0 desyncs in all 8 fights that
      ran, 0 crashes** (rc11/rc12: X-ray-start desyncs in most fights, crashes in 3 of 6 runs); ns60: DeadPool desync at
      frame 1824 (after the lock had ended at ~916; not looked at yet) and the harness PitDay frame-0 start; gapB: 0 locks,
      DeadPool desync at 1914 (also late; same class?). **Cost: frames >= 34 ms during the locks.** xray ns100 Pres34/min
      90-150 (xray5: 6-10), Hitch34/min 32-60 (4-5), P99 40 ms. The lock is lockstep, which runs at 60 fps only while the
      round trip fits in 2 x delay frames; the ping rule sets delay = one-way frames - 1 for rollback, so at ns100 (round
      trip ~185 ms, delay 4) the locked frames run at ~40 fps (each waited ~11 ms; locks of 277-635 frames per fight, 75-90%
      of them the cinematic). ns60 (delay 3) much milder (A 6.3, B 3.6 hitches/min). At the field's 33-50 ms round trip and
      delay 2 it fits. Refinement to try (rc15): lock only from the press until a few frames into the X-ray (confirmed
      inputs through start + delay), and during the X-ray cinematic (slot 2 on both fighters) send the input held at its
      start instead of the live one (no misprediction, so no rollback, without waiting); throws not locked.
    - **The field session again (user, 2026-11-08: "I play with him SF6 on the Wi-Fi and it is fine").** The friend's
      four ~830 ms freezes were their whole game stopping, twice inside the mod's live save (825 / 822 ms) and twice in the
      engine tick; their private memory goes from ~656 MB (menus) to ~950 MB in a fight (the snapshot ring: the 12 MB master
      heap image alone x 12) and every frame copies ~15+ MB into a ring slot last touched 12 frames ago. Under memory
      pressure (their PC is slower: saveab 3.3-4.9 ms, tierE 1.7 vs 0.3 ms here) a trimmed slot comes back by hard page
      faults: a likely mod-made freeze SF6 would not have. Unknown: their RAM. To do: log hard page faults and available
      RAM with each HITCH (GetProcessMemoryInfo / GlobalMemoryStatusEx), and keep the ring resident or smaller. The ten
      ~0.5 s gaps (their packets to us stopped; GGPO send queue 17 just before one, normally 4) are the link: Wi-Fi or the
      Steam P2P path (unreliable no-delay sends, direct, no relay); SF6 rides through such gaps somehow — unknown how.
      The online-detection memory pass (WideScan) runs at the character select only: not near the freezes.
    - **Batch 26 tail (done 06:14).** xray8d ns100 lock: 0 desyncs, 0 crashes (rc14 total: 11 fights at ns100, all
      clean). Soak 25 (levelredo, SyncTest 13196): 13 fights, 912 loads, 16 errors (not comparable: 13 short fights
      against 4). xray7a ns100 with levelredo and the lock off: **A crashed** at game(100417), a write to 0x00D58B08 (the
      slot-2 cinematic controller's vtable) and to 0x0052EC10 (the controllers' deleting destructor): a free list held
      a controller's bytes, i.e. RestoreMotionCtl's memcpy of saved controller bytes into a block that had gone back to
      the allocator (the 0x00D56A38 crashes look like the same thing with another class). The lock / held input keeps
      loads out of the cinematic; RestoreMotionCtl still has no check that the saved block is held (SafeBlockInfo
      only gives the size class).
    - **rc15 (`build\clang_rc15` AE907DCE = rc14 + these):** (1) X-ray lock narrowed: the press window is 60 frames
      (batch 26 fight 3: the cinematic began ~39 frames after the press, when the X-ray connected, 4 unlocked frames after
      the 30-frame window) and ends when the X-ray cinematic begins (a slot 1/2 cinematic controller while the window is
      open, frame C); the lock runs on to C + delay + 4. (2) `xrayfreeze` (default on): from C until the controllers have
      been gone 2 frames each game adds the input it added before C, so GGPO's prediction is exact through the
      cinematic: no rollback and no wait there; the first changed input applies after the cinematic. Throws and other
      cinematics without an X-ray press: not locked, not held (`xraylockcine 1` = rc14's lock-every-cinematic rule).
      `XRAYFREEZE` lines; stop line adds X-ray cinematics and held frames. (3) HITCH >= 150 ms adds page faults in the
      interval, working set and free RAM. (4) P2P sessions turn the stall sampler on at 250 ms unless a `stallsample` was
      given (20 ms steps above 100 ms), and it logs `the sampler thread did not run for N ms` when the whole process was
      stopped. Batch 27 (`results\batch_20261108g.txt`, guarded): xray ns100 x3, ns60, ns30, combodmg ns60, gapB.
    - **Batch 27 (rc15 AE907DCE, 06:20-07:19, all 7 units ran): 0 crash / hang / process-stopped lines; the only
      desyncs are the harness ones** (PitDay frame-0 start in 3 runs, DeadPool intro at frame 192 in xray9a, before any
      press); every X-ray fight clean (ns100 x3, ns60, ns30). Frames >= 34 ms per minute (A/B), presents >= 34 ms:
      xray ns100 6.3/7.2, 9.1/7.3, 13.5/9.0 (presents 9-23) against rc14 32-60 (90-150) and rc11 ~4-9 (6-12, with
      X-ray desyncs in most fights and crashes in 2 of 4 runs); ns60 4.2/3.0 (6.7); ns30 3.6/3.6 (5-6); combodmg ns60
      4.3/4.9; gapB 8.7/18.1 (rc12 gapB 9.3/21.0). Locks now 26-48 frames each (the X-ray's start; 152-524 ms of waits at
      ns100). Held input per X-ray: Jade 261 frames, Reptile 496, Noob 601 (both fighters at the cinematic's anchor at
      frame 600, P2 life falling inside it: the cinematic itself, slow motion included). HITCH memory figures work
      (this PC: working set 435-625 MB, 4.5-4.9 of 16 GB free). To confirm by hand in a real match: moving right after an
      X-ray (the held input ends 2 frames after the controllers go).
    - **Batch 28 (rc15 AE907DCE, 07:22-08:52, all 7 units ran; `results\batch_20261108h.txt`).** Frames >= 34 ms per
      game-minute A/B (presents >= 34 ms): **every stage at ns30_d2 (28 fights) 3.0/2.6 (3.3/3.2)**, >= 50 ms 2.7/2.6,
      max 94/106 ms, 0 crashes; jittery ns60 4.8/5.4 (7.2); loss5 combos 5.4/7.2 (7-9); slow peer spinB6 5.4/6.3 (B
      presents 27/min); freezeB 11.8/10.1 (rc8 9.1; the 806-849 ms maxima are the injected freezes; A held after each,
      10 STALLHOLD lines; HITCH memory figures on every freeze: 137-592 page faults, working set ~620 MB, 4.3 GB free);
      ns220_d6 combos 125/158 (Hitch50 38/61, p99 58-60 ms; net11's ns220_d4 ~300/min): past what any delay hides. 0
      process-stopped lines. **Desyncs only the harness ones:** PitDay / Street frame 0, DeadPool intro 192; stages: 16
      of 28 fights with mismatches, every first one RNG A only with both fighters identical (frames 4-529, the
      attract-demo intro phase), fighter states apart later in 4 (DeadPool, GorosLair, PitDay, RooftopDawn; stages2 on
      the 11-04 build: 13 + Street, 3 apart). **One crash: slowB (B one core, below normal) B, Graveyard fight 1,
      game(100187):** the Kismet activation loop (0x0073ABE1 -> 0x0073A791 -> 0x004146AA) of SeqAct_Cinema_7 called
      vt+0x38 of 326F94A0, purged by the collector (caller 0x00647FA3) at 100155 and released at 100175; the only load
      before it (S=100125, 1 frame) said "1 lost". **All five crashes on a GC-purged object since net13** (net13a B,
      stall1 gapB A, xray2 B, stall3 gapB_nohold B, this) were SeqAct_Cinema_7 and each followed a load with "N lost" (a
      spawned group's actor destroyed between S and the load); 48 other logs had lost loads and no crash. The lost
      actor is still a UObject (bDeleteMe) at the load, so cinemadeadrefs' live-object test kept the restored references
      to it (object variables, Targets); nothing in the replay tears it down again, and after the purge the next
      activation called into it.
    - **rc16 (`build\clang_rc16` 3DC97A6B = rc15 + these):** (1) HandleGuard at 0x004146A1 (the latent action's
      target loop: `mov edx,[edi]; mov eax,[edx+0x38]; mov ecx,edi; call eax`): a target that is not a live UObject
      counts as vt+0x38 true (skipped, as the loop does for a destroyed one); `HANDLEGUARD: a latent action's target
      ... skipped at 0x004146A1`, counted in the guard total. (2) cinemadeadrefs: references to the lost actors in every
      tracked object's object fields and object arrays are set to 0 after the load (`N reference(s) to M actor(s)
      destroyed since S cleared`, 300-load total). Batch 29 (`results\batch_20261108i.txt`, guarded): slowB x2, gapB,
      gapB_nohold, xray ns100, jittery ns60.
    - **Batch 29 (rc16 3DC97A6B, 09:00-09:46, all 6 units ran): 18 fights, 0 crash lines; desyncs only PitDay frame 0
      (2 runs); every Graveyard, DeadPool and X-ray fight 0 mismatches.** The guard was patched in every game and
      transparent (stage cinematics ran through it with no mismatch); it never had to skip (no dead target came up).
      12 loads found a lost actor (gap, X-ray and jittery runs) and each cleared **0** references to it (each also set 1
      dead array element to 0, as rc9 does): in these loads nothing tracked still named the lost actor, so the crash's
      reference lives elsewhere (a struct array or an untracked object); the guard is the protection that covers it.
      Frames >= 34 ms per game-minute A/B: slowB 15/253 and 6.6/188 (B is the one-core game: 245-425 ms "whole process
      stopped" lines there, its own starvation; A, the normal peer, as other runs); gapB 9.9/14.0 (rc15 8.7/18.1);
      gapB_nohold 10.4/38.1; xray ns100 9.7/9.7 (rc15 6.3-13.5); jittery ns60 3.6/9.0 (rc15 4.8/5.4). **rc16 replaces
      rc15 as the candidate** (pending the user's OK for the gate and zip).
    - **What the ns30 "frames >= 34 ms" are:** in stages10 (rc15) the HITCH lines are the session start (gf 100001: the
      start barrier, wait 57-67 ms, on the paused first fight frame, nothing moving yet) and the harness's timeline restart
      (gf 0); in-fight ones 0-1 per fight. In-play stutter at the field's link is ~1 per game-minute; the start barrier
      counts in every per-fight figure.
    - **The field session's network freezes are on a 200 ms grid.** Our 12 waits >= 300 ms (field_20261107_session2.log)
      were 582-587 ms x7, 646-648 x2, 773-775 x2: not the spread a Wi-Fi outage gives. Steam P2P was direct (relay 0,
      queue 0) throughout. GGPO resends unacknowledged inputs only when no new input frame came for RUNNING_RETRY_INTERVAL
      (200 ms, counted from the last new frame, `OnInput` useInputs); a game blocked at the prediction limit adds no input,
      so after an outage of the friend's uplink (their waits = ours - 8 frames: they kept receiving ours) their first
      input that could get through left on that grid: our wait ~ 200 + 200k + 2 x one-way, whatever the outage was.
    - **rc17 (`build\clang_rc17` B83D6C6F = rc16 + GGPO `build\ggpo_retry`):** RUNNING_RETRY_INTERVAL is a variable,
      default 34 ms (`netretry N`, stock 200): in play a new frame comes every 16.7 ms, so it only fires when inputs stop.
      Netsim drop mode (`netsim d oop loss ms period 1`: the window's packets are lost, not held; the old stall holds them
      and the burst at its end recovers without any resend, so gapB never exercised this). Scenarios ns30_d2_dropB (B's
      packets lost 300 ms every 10 s) and ns30_d2_dropB_retry200. Batch 30 (`results\batch_20261108j.txt`, guarded): dropB
      new / 200 alternating x2, gapB, jittery ns60, loss5 combos.
    - **Batch 30 (rc17 B83D6C6F, 09:53-10:44, all 7 units ran): 0 crashes; desyncs only the harness ones** (PitDay frame
      0 in dropB a / a200, loss5 DeadPool intro at 192 with both fighters identical, 28 frames). **Per 300 ms drop of B's
      packets, A (whose peer's inputs were lost) waited 169-226 ms (avg ~205) with the 34 ms resend against 231-243 (avg
      ~236) with 200; B (incl. the stall hold) 82-186 (avg ~100) against 128-222 (avg ~160)** and held 7 times in 15 drops
      against 16 in 16. 300 ms is the stock timer's best case (B's first stock resend after the window, at ~333 ms, gets
      through); batch 31 runs 400 ms (`results\batch_20261108k.txt`). Frames >= 34 ms per game-minute dropB 10-12/21-23
      vs 8-9/16-19 (the 34 ms runs count more 34-60 ms frames on B after each drop, shorter waits split differently; the
      long ones are shorter); gapB 9.9/24.4 (rc16 9.9/14.0); jittery ns60 6.7/5.5; loss5 combos 3.6/3.0 (rc15 5.4/7.2).
      B's extra 34+ ms frames are rollbacks (5 and 12 rollback hitches against 2 and 0): its waits after a drop were now
      82-117 ms, under the stall hold's 100 ms threshold in about half the drops, so it came out ahead and rolled back up
      to 8 frames. Batch 32 (`results\batch_20261108l.txt`): stall hold from 60 ms waits (`stallhold 60 150`).
    - **Batch 31 (rc17, 400 ms drops, 10:45-11:14, all 4 units ran): the resend shortens every drop's freeze by ~130
      ms.** A waited 281-319 ms (avg ~303) with 34 ms against 426-445 (avg ~435) with 200; B 189-270 (avg ~222) against
      309-406 (avg ~361); longest frames A 337-349 vs 467-474 ms, B 280-283 vs 418-451. Stock's waits sit on its grid
      (A 431-438 / 426-445), the new ones follow the outage (400 - 8 frames of prediction + a round trip). 0 crashes;
      desyncs only PitDay frame 0 and the DeadPool intro at 192 (RNG only, fighters identical; for good in drop2b).
    - **Batch 32 (rc17 + `stallhold 60 150`, 11:15-11:44, all 4 units ran):** after 300 ms drops B held after every
      one (11, avg 57 ms) and had 3 rollback hitches (stallhold 100: 5 and 12; frames >= 34 ms/min B 14.9 against 21-23);
      400 ms drops B 0 rollback hitches (100: 4 and 3); freezeB 10.2/9.6 (A held 8 times, avg 58 ms); jittery ns60 0
      holds (no false trigger), 7.3/6.1. 0 crashes; desyncs only the harness ones (DeadPool intro 192, PitDay frame 0).
    - **rc18 (`build\clang_rc18` DB580C09 = rc17 + stall hold from 60 ms by default; `stallhold 1` = 60 150).** Batch
      33 (`results\batch_20261108m.txt`, guarded): dropB, freezeB, xray ns100, slowB on the defaults.
    - **Batch 33 (rc18 DB580C09, 11:45-12:20, all 4 units ran): 0 crashes; desyncs only the harness ones** (PitDay
      frame 0, DeadPool intro 192 with both fighters identical). dropB: B held after every drop (7, avg 53 ms), 5 rollback
      hitches; frames >= 34 ms/min 10.7/18.7, longest 227/176 ms. freezeB 15.3/9.1 (A held 7 times, avg 66 ms; the waits
      for B's 830 ms freezes themselves). xray ns100 9.7/10.9, every X-ray fight 0 mismatches. slowB: A (the normal peer)
      3.6/min, B (one core) 298/min as before, no crash. **rc18 is the candidate** (pending the user's OK for the gate
      and zip): rc16's Kismet target guard, GGPO's 34 ms resend, stall hold from 60 ms.
    - **Released (user: "Yes, package rc18"): `dist\MK9Rollback-netplay-20261108-1450.zip`, DLL DB580C09.** Gate
      (`results\gate_rc18.txt`, 14:18-14:50): online smoke PASS; regressions plain / auto re-arm / delay change 0/6
      mismatching each, 0 crashes, 0 failed saves, 0 cut sessions; menu walk 305 presses, no hang. build\clang and
      clang2 hold rc18; build\clang\libggpo.a is now build\ggpo_retry's (FC9817FC; sparse2 kept in build\ggpo_sparse2).
      Next: a field session on it (both players' logs: the friend's freezes with the memory figures, the network
      freezes' lengths after the resend change, moving right after an X-ray).
    - **GitHub and the updater (2026-11-08):** public repository samuelndm/MK9Rollback (source, tools, docs; results,
      analysis, build and dist ignored; two real SteamIDs and the Windows user name scrubbed first). The Play Online
      window shows its version (version.txt, written by tools\package.ps1) and checks the latest GitHub release when it
      opens (tools\updater.ps1: tag v<stamp>, assets zip + .sha256); "Update now" (game closed) downloads, checks the
      SHA-256 and the version inside, keeps the replaced files in previous_version and restarts the window.
      tools\publish_release.ps1 publishes a gated zip. **Release v20261108-1633** (rc18 DLL DB580C09 + the updater):
      end-to-end test from an older install: found, downloaded, verified, installed, then "up to date". v20261108-1631
      (published first) has an updater that read an empty version ($Matches replaced by the asset match) and never
      offers an update: deleted from GitHub (user, 2026-11-08), together with its tag.
    - **Field session 2 (2026-11-08 21:57-22:44, v20261108-1633 / rc18 on both PCs, Steam, 19 matches, ~39 min of
      fights): 0 desyncs; hello round trip 33-50 ms, GGPO ping 33-50; delay 2 all night; 6 freezes of 0.7-0.8 s, every
      one a wait for the other game.** The other player's log (`mk9r-logs-20261108-224400.zip`) explains all six:
      - 4 (22:02:38, 22:04:16, 22:11:48, 22:19:07): its live save stopped 817-832 ms in MarkKeep's SafeRead32 of a pointer
        target (every STALL sample eip MK9R_SafeReadInsn; SaveAB -> CandidateSpans -> MarkKeep); working set 663-752 MB
        of 888-977 MB private (ours ~ all resident): trimmed pages read back from that PC's disk.
      - 2 (21:59:03, 22:08:05, both at a round end): MKKE's own CreateFile (0x00511CEA, GENERIC_READ, sequential scan)
        took 836-840 ms (engine tick): that PC's disk / antivirus. Advice given: antivirus exclusion for the game folder,
        disk sleep off, or an SSD. Its game also threw C++ exceptions (E06D7363) from a non-MKKE thread every 9-17 s in
        the menus (third-party software in the process; none in ours).
      - Ours: one 264 ms pause, the game thread in USER32 message dispatch through gameoverlayrenderer (Steam overlay).
      - The player: "still feeling input lag... not like the new games, which feel offline"; a Cyrax bomb combo dropped.
        The bomb: both games matched every frame and in 4 of the 5 Cyrax matches the other game never rolled back, so it
        was the game's own result from the inputs as delivered (combo timed by eye, 2 frames late). Input path: the
        hardware sampler (0x00404E26) runs before the tick in the same main-loop pass, so online = offline + the delay.
    - **rc19 (`build\clang_rc19` D8D192BC): MarkKeep asks QueryWorkingSetEx once per scan; a target on a page outside the
      working set is kept live unread and the page handed to a warming thread** (`coldcheck 0|1`; `trimws N` test aid
      empties the working set every N ms). The first build without the thread kept ~40 targets a save unread on every PC
      (floats landing in untouched GMalloc pages, which the old read had committed); with it the unaffected game keeps
      ~38 per 600 saves.
    - **b34/b35 (two games on this PC, combo fights, stages 7/13/3, rc19): GGPO ping in the harness is ns15 ~50 ms
      (the field link) and ns30 ~70-85 ms.** Frames >= 34 ms per minute A/B: ns30 d2 3.6/3.6; **ns15 d1 3.0/4.3** (as
      smooth as d2, rollbacks 68/271 a minute, depth 1.0); ns30 d1 7.2/6.3 (depth 1.5-1.7); spinB3 d1 3.6/10.3; spinB6
      d1 5.5/119; spinB6 d2 3.6/12.7; trimB (B emptied every 2 s) d2 3.6/32.8 (the trims themselves), 0 crashes, 0 real
      desyncs. Desyncs only the harness ones (DeadPool intro frame 192, PitDay frame 0).
    - **rc20 (`build\clang_rc20` 533FBA39) = rc19 + delay 1 up to a 50 ms hello round trip (DelayFromPing) and a per-PC
      floor: a round at delay 1 with >= 3 live frames per 1000 of >= 25 ms with a replay makes this PC propose 2 from
      then on (FramePace::TakeReplayHitches, gCpuFloor).** Only a real match exercises the online delay choice (the
      harness uses fixed delays). Launcher, package README and README say "1 frame up to a 50 ms ping".
    - **"Professional level" review (2026-11-09, the player: "like the latest games"): three subagents (industry research,
      network-path latency audit, save/load cost audit).**
      - Industry: SF6 adds 0 frames below a 100 ms ping (1 at 100-199), delay only rises within a match (Infil's SF6
        review); GGPO's guide: more than 1 frame is noticed by intermediate players; KI/Injustice 2/MKX a fixed 3; NRS
        GDC 2018: up to 8 frames simulated in 16 ms, parallel restore, speculative saves; GGRS sparse saving when a save
        costs more than a tick. Our transport already matches (input resend in every packet, UnreliableNoDelay, Pump
        right before ggpo_idle and after the input is added).
      - Latency audit: the true round trip of the field link is ~22 ms (hello and GGPO pings are whole frames: 33 or 50).
        The local input was added (and sent) at the end of the boundary, after the poll, any rollback and the live save:
        3.5-4 ms late on a fast PC, 5-8 ms on the slower one, more on rollback frames. The controllers were sampled
        before MK9's frame-limiter wait (0x00404E26 before TimedEngineFrame): 3-11 ms old at the boundary.
        kDelay1UpToMs 50 sat on the 3-frame step (the field's second-highest round trip read 49-51).
      - Save audit: ~2.55 ms a save on the fast PC, 3.7-5.2 on the slower (VirtualQuery ~9 us there, ~2 ms a save:
        209-242 calls); loads 6.6 ms (LoadAB 3.7, quarantine requeue 1.8). Ranked: guarded reads for the whole save,
        fuse the requeue scan into LoadAB, adaptive sparse live saves, skip live saves of already-confirmed frames.
    - **GPU queue (b36, rc20 1FD4C9EF, GPUQUEUE probe): the game's D3D9Ex maximum frame latency is 3, but in fights the
      GPU had finished every earlier frame at each EndScene (avg 0.00-0.04); queues only build in loading screens.** A cap
      of 1 ('framelatency 1') changed nothing here; left off, the probe stays for field logs.
    - **rc21 (`build\clang_rc21` 2014E1A1) = rc20 + early send (the input added and sent at the boundary's start,
      'earlyinput'), late sampling (the main loop's sampler call deferred to the boundary's start, after the frame wait,
      'latesample'), kDelay1UpToMs 55, guarded-read probing through Tier E and script ('fastprobe'), the SyncTest-only
      VirtualQuery calls skipped in P2P saves.**
      - **b37 (rc21; ns15 d1, ns30 d1, ns15 d0): smooth; hitches/min ns15 d1 3/4.9 (rollbacks 115/230 a minute), ns30
        d1 3.6/4.9, ns15 d0 3.6/4.9 (P99 22-23 ms).** But two PitDay fights ran desynced from frame 1 (P2 z 0.250 vs 0.000
        before any frame): **rc21 regression in the start check, not gameplay.** GGPO saves frame 0 inside the first
        ggpo_add_local_input (Sync::AddLocalInput when its frame count is 0); the early send added it before the boundary
        moved checksum frame 0 (gCsStartGf) to the first unpaused frame, so frame 0 was recorded as "frame 1" ("NETPLAY
        STATE frame 1" twice, no "frame 0" in any rc21 log) and 'frame 0 differs' never fired. rc19/rc20 stopped the
        same harness case at frame 0 (b35). Fixed in rc22: gCsStartGf set before the early add too.
    - **rc22 (`build\clang_rc22`) = rc21 + the start-check fix + 'skiplive' (no live save for a frame whose remote input
      is already in: rc21's early send makes that most frames at a short round trip) + launcher input-delay choice (Auto
      / 1 / 2 / 3 frames, saved as delay=N, sent as `online auto N`).** Launcher later also got "0 frames" (applies only
      when both players pick it; Safe mode always takes its delay from the ping).
      - **b38 (rc22 FFBF16CB; 12 fights, 00:52-01:20): passes.** Start check works again (ns15 d0 PitDay stopped at
        frame 0, "different states"). Desyncs only the DeadPool intro artifact (frame 192, the same checksum pair
        B6434367/022B9CE4 as net8 on older builds; one recovered after 28 frames, one stayed). 0 crashes; 0 loads with no
        saved frame (24 went back a frame). Early send and late sampling on every frame (1966/1966). Per-fight P99 (ms):
        ns15 d0 16-21, ns15 d1 16-18, ns30 d1 18-22; hitches >= 34 ms 1-4 a fight on every game; spinB6 B at d1 70-92
        a fight (the CPU floor moves such a PC to 2 in a real match; the harness fixes the delay). The live-save skip
        fired 0-62 of 1966 frames: the harness's one-way latency (~25 ms at ns15) exceeds 1 frame of delay; the field
        link's (~11 ms) does not.
      - b38 cost picture (fast PC): in-fight HITCH lines are the known once-per-process GPU stall (~45 ms at the same
        frame on both games, boundary ~4 ms); one rollback hitch in 12 fights. Loads 6.6-7.3 ms: LoadAB 3.2-3.6 (master
        image 1.3), quarantine 2.0-2.2 (requeue scan 1.8-2.0), attach 0.8, cinema 0.5. Saves: master 1.24-1.28, ue3cells
        0.75-0.83, skel 0.5, tierE 0.27-0.34, script 0.32. The periodic Tier E / module-section rebuilds never appear in a
        HITCH line (b38 and both field logs of 2026-11-08: "jobs: none" or "stallhold"): not moved.
    - **rc23 (`build\clang_rc23` 79C5580A) = rc22 + ParallelMem: the master image's copy in every save and load and the
      load's requeue scan split in 128 KB chunks between the game thread and up to 3 helper threads (half the logical
      processors minus one; 'memthreads N', 0 = rc22's single-threaded passes). Same passes over the same memory at the
      same points; the game thread takes chunks too and waits for the last one.** Not done, with reasons: precise RTT
      (SteamBridge has no thread to stamp arrivals; reading Steam from a second thread is not promised safe; it would only
      move the 1/2-frame boundary, and the field link already gets 1); script module partial copy (0.3-0.6 ms a save,
      finding the changed part costs about as much as copying it).
    - **b39 (rc22 FFBF16CB; every stage: 28 fights x ns15 d1 and ns30 d1, scripted combos, fighters varied;
      `results\scenarios\b39*`, 01:25-02:45): 0 crashes; smooth on every stage.** Per-fight frame P99 ns15 16-19 ms
      (Courtyard 25 / CourtyardN 21, the process's first fights), ns30 18-26 (Courtyard / CourtyardN 29). HITCH lines
      by cause, per game over 28 fights: ns15 1-2 in-fight (outside the boundary: the GPU stall), 0 rollback; ns30 5
      rollback hitches (avg 42-44 ms; load 10-14 + replay + replaysave) and 0-1 outside; all others are the harness's
      session start (gf <= 9: start barrier, first replay) or end (timeline restart, gf 0). Rollbacks/min ns15 122/232,
      ns30 260/342, depth 1.0-1.6. Loads 6.5-7.5 ms (loadab 3.3-4.1, quarantine 2.0-2.2); saves' master 1.24-1.30 ms.
      - Checksums: the start check stopped 3 fights at frame 0 (STThrone both latencies, Street ns30: P2 z 0.250 vs
        0.219, the harness start). 14 (ns15) / 13 (ns30) others had mismatches; **at every first mismatch both fighters
        were identical (life, positions, meter) and only RNG A differed**, frames 3-1455. The same RNG pairs recur at the
        same frames across stages and latencies with the leading game swapped (DeadPool and Desert 192 F617ED01 /
        41F4F8A0; RooftopDay and RooftopDawn 61 6CEDA14C / 569AF827, RooftopDay 61 also in stages2 on 11-04; BellTower
        143; SKThrone / Armory 4): fixed demo-script draws a frame apart, the attract-demo start-phase artifact ("Intro
        desyncs explained" below). Fighter states (every 300 frames, live) diverged later in 5 / 9 of them. Real starts
        keep the scripts in phase: the 2026-11-08 field match matched every frame on both games.
    - **b40 (rc23 79C5580A, helpers on vs `memthreads 0`; stages 7/13/3, `results\scenarios\b40*`, 02:50-03:22):
      3 helper threads on this PC (32 logical).** Per save, the master image copy 1.25-1.33 -> 0.55-0.59 ms; per load,
      total 7.2-7.4 -> 5.4-5.7 ms (quarantine 2.2-2.4 -> 0.9-1.1, requeue scan 2.0-2.1 -> 0.67-0.88, loadab 3.6 ->
      2.9-3.3). 0 crashes. Checksums: every helper-on fight 0 mismatches (6 fights); PitDay stopped at frame 0 in all
      five scenarios (harness start check); the one mid-fight mismatch is DeadPool 192 F617ED01/41F4F8A0 in a
      helpers-off run (harness artifact). The "referenced from ... restored by the cinema restore after the requeue
      scan" notes are as frequent with helpers off and in rc22 (b38/b39): not new. Hitches per game-minute too few
      fights to rank; spinB6 B at d1 47-73 hitches a fight (rc22 b38: 70-92).
    - **b41 (rc23, `rngtrace 1`, ns15 d1, the 15 stages with b39 mismatches; 03:23-03:46): every mismatch is the
      demo-script phase artifact.** 0 crashes, P99 16-19 ms. CourtyardN stopped at frame 0 (start check, P2 z). At all 12
      first mismatches both fighters were identical and the engine's per-frame draw (0x0080F6C5) was made on both games;
      the only difference was the script native `random` (0x008B87C1, from a script executor) drawn on one game at that
      frame and not on the other. No other RNG caller ever differed. With SyncTest clean of it and the 11-08 field match
      0 mismatches: harness-only, as in "Intro desyncs explained".
    - **SyncTest soak on rc23 (9201-9202, combos + damage, 2 x 13 min, `results\synctest_soak\seed920*`): 0 crash
      lines, both alive, 2,762 loads; 16 sync errors, all the X-ray-start class** (KISMET *_XRAY_0, VMTRACE `original
      gate ... ret 00455594 | resim none`, actors +0x8/+0x58/+0x144): SyncTest forces rollbacks across X-ray starts,
      which P2P avoids with the X-ray lock (rc14). 0-4 per fight, as in soaks 19-21 on older builds. (The soak rewrote
      `summary.csv`; earlier seeds' numbers are in their logs.)
    - **b42 (rc23, harsh links, 27 fights, `results\scenarios\b42*`, 04:27-05:20): 0 crashes; no new desync.** All
      completed fights 0 mismatches except the DeadPool intro artifact (frames 200-295, 4 fights); PitDay frame-0 start
      stops in 5 of 9 scenarios (harness). Per-fight P99 (ms): ns60 d2 jittery (5% out of order, 3% loss) 18-19, ns60 d3
      17-18, ns100 d4 18-23, ns30 d2 loss 5% 16, ns30 d2 stall 18, ns60 d3 X-ray 18, ns100 d4 KO 19-20, ns30 d2 dropB
      18-24 (max 225-233 = the 300 ms drops), ns220 d6 46-48. **Against older builds, hitches >= 34 ms per game-minute:
      ns220 d6 66/95 (combo1 146/167, net10 125/158); ns30 d2 dropB 10.2/9.6 (drop1a/b, final1: 10-12 / 19-23);
      ns60 d2 jittery 4.9/3.6 (net6b-net9: 3.6-14).** Three B logs have one "HANG? game thread stalled >3s" each, at gf 0
      ~6 s after "Hook initialized" (the game's own startup load, crumb idle; 46 of 506 older logs have the same).
    - **rc24 (`build\clang_rc24` ECE5CB4E) = rc23 + hybrid-CPU diagnostics + no Windows power throttling.** b39's ns30
      rollback hitches ran every part ~2.5x slower than average at once (load 14-20 ms, replay saves 5-9 ms each), as on
      a slower core (this PC: 8 performance + 16 efficiency cores; the harness's windows are hidden behind the lock
      screen). FramePace samples the game thread's core class (GetSystemCpuSetInformation EfficiencyClass,
      GetCurrentProcessorNumberEx) at each live boundary and after each load / replay save: HITCH lines end in
      "; cores P|E|P+E", the session end logs "CORES session: N of M live frame intervals ... efficiency core". dllmain
      turns Windows power throttling off for the process (SetProcessInformation ProcessPowerThrottling, EXECUTION_SPEED
      + IGNORE_TIMER_RESOLUTION, state 0; Windows 10 before 11: execution speed only): a match keeps its pace when the
      window is not in front. `ecoqos 1` gives the choice back to Windows.
      - **b43 (rc24, 6 stages at ns30 d1, throttling off `b43qos0` vs Windows' choice `b43qos1`; 05:24-05:47): the
        efficiency-core idea is not borne out here.** The game thread was on an efficiency core in 1-16 of 1966 frame
        intervals per fight either way (PC locked, on AC power); loads 5.7-6.3 ms, master 0.56-0.58 ms either way. 0
        rollback hitches in 12 fights per game (b39 rc22 at ns30 d1: 5 per 28; rc23's faster loads, small sample); the
        in-fight HITCH lines are the GPU stall (boundary ~5 ms, cores P) and one 50 ms live save (cores P). Checksums:
        every first mismatch RNG-only with identical fighters (DD14CFE6/F66D0609 at 192 in both runs, ...), one frame-0
        stop. 0 crashes. The opt-out stays (no cost; Windows applies EcoQoS to hidden / minimized windows, which the
        harness's are not); the "cores" note in HITCH lines tells field logs whether a hybrid PC's hitches were on
        efficiency cores.
      - **Release gate on rc24 (user approved 2026-11-09 08:51; `results\gate_20261109_rc24.txt`, -Soak 0): pass, zip
        `dist\MK9Rollback-netplay-20261109-0924.zip`** (DLL SHA-256 ECE5CB4E...D430E, zip 4ED4B307...4789). Online smoke;
        plain, auto re-arm (3 re-arms each game) and delay-change (19 changes) regressions 0/6 mismatching each, 0
        crashes, 0 failed saves; menu walk 305 presses, no hang. build\clang / clang2 hold rc24; the released rc18 files
        are in build\clang_rc18_backup. Not published yet (needs the user's yes).
      - Cost left on this PC (rc24, b43): a save ~2.6 ms (SaveAB 2.0: master 0.58, skel 0.36-0.39, tierE 0.23, script
        0.23, ue3cells 0.14-0.27, extlists 0.11; extras 0.6: cinema+motion 0.31, records 0.19), a load 4.6-5.0 ms
        (loadab 2.3-2.8, quarantine 0.8, attach 0.8, cinema 0.4-0.6). The rest is per-object walks (writable checks,
        pointer-like offsets, identities), not bulk copies: splitting them across threads would need per-object work
        lists and is not done. Next evidence: both players' logs from a match on rc24 (the slower PC's SAVE / LOAD
        PROFILE and HITCH "cores").
    - **"Professional level" round 2 (2026-11-09 afternoon; the player: "like the latest games in industry"): five
      subagents (industry research; patched-GGPO audit; network/timing audit; save/load cost audit; player-experience
      audit), then rc25 (`build\clang_rc25` 549D1AAC, GGPO `build\ggpo_rc25`).** Findings that changed code:
      - Research: our design matches the industry where sources exist (delay 0-1 under ~100 ms in SF6, 3 fixed in NRS's
        Injustice 2 / 2XKO; between-round delay changes; frame stretching rather than skips, as GGRS recommends; all
        unacked inputs per packet). Missing: a continuous time-sync controller (GGRS / Unity: ~10% of the error per frame).
      - GGPO audit: GGPO's own TIMESYNC fired 3-4 frames right after stall holds (76 sessions with holds: 2.95 events
        each, 788 without: 0.36) and BALANCE paid on top: overshoot. Frame advantage used the whole latest round trip,
        truncated (stock comment says one way). timeGetTime clock (15.6 ms steps, wraps after 49.7 days: false
        disconnect). Uninitialized UdpProtocol members (first ping / advantage garbage, kbps divided by 0). Disconnect
        rewind to a frame that may be a sparse skip. GGPO's own sends (quality replies, resends) waited for the boundary's
        last Pump. Log formatting with logging off. Socket bound to every interface.
      - Timing audit: **'skiplive' almost never fired at delay 1** (the test was F < safe frame, which needs F+1's input,
        sampled by the other game at its own boundary F: b38 0-62 of 1966 frames); the replay's save of a confirmed live
        frame was always taken; **sub-frame phase uncontrolled**: at delay 1 on a ~22 ms link both games get the inputs
        in time only while their grids are within ~5.7 ms, and MK9's limiter keeps each grid's phase forever (lopsided
        rollbacks 68/271, 115/230, 122/232 a minute; hello 33 or 50 ms by match); hellos read after a stall inflated the
        round trip (delay 2 for the next round); gTimesyncFrames leaked into the next session.
      - Cost audit: VirtualQuery still on per-frame paths (3-20 us a call in multi-MB regions); the load's pointer-safe
        merge classified every dword of ~2,200 spans although most do not differ.
      - UX audit: one long green line whatever the link; no "opponent not responding"; launcher silent when the opponent
        has no mod, "friend"/"offline" wording wrong, delay box ignored while running, desync message swallowed, logs zip
        could be empty while the game holds the files.
      **rc25 changes** (each with a switch; defaults on):
      - GGPO (tools\build_ggpo.ps1, 7 rc25 blocks): QPC clock (base 1000); members initialized, no divide by 0;
        `ggpo_no_disconnect_rewind`; frame advantage from half the lowest of the last 5 round trips, rounded, 0 until known
        (`ggpo_rtt_min_ms` exported); `ggpo_after_send` hook (SteamBridge::PumpOut forwards each datagram to Steam at once);
        logging decided once; `ggpo_bind_loopback` for bridge sessions, SIO_UDP_CONNRESET off.
      - Time sync: GGPO's TIMESYNC counted, not paid, while BALANCE is on ('ggposync 1' = rc24); BALANCE samples only once
        a round trip is known; debts of up to 2 frames paid 1 ms a frame ('paysmooth N S'); StallHold uses the lowest round
        trip and rounds the one-way trip up correctly.
      - **Phase alignment ('phasesync', RollbackManager PhaseSample / PhaseDecide):** each game counts per 120-frame window
        the boundaries whose first poll already had the other player's input (quiet frames only), sends the share on the
        checksum link ('MK9P'); the game with the clearly smaller share (>150 per mille; its grid leads) moves its frame
        deadline later 0.5 ms a frame, 1-6 ms per window pair, at most 150 ms a session. PHASE lines; TIMING line per session.
      - Saves: live frame skipped from F <= safe frame (F < safe kept with 'sparselive'); the replay's save of a confirmed
        live frame skipped too (LiveFrameConfirmed). Re-poll after a live save made before the frame's input ('repoll').
      - Readability checks by guarded reads ('fastreadable'); cost agent (Snapshot / Ue3Refs / ParallelMem): guarded reads
        in CollectCells, name maps, ExternalLists; non-temporal stores into save slots (ParallelMem::CopyToSlot); load
        merge classifies only differing dwords ('diffmerge 1'; 2 = also run the full merge and log DIFFMERGE MISMATCH, the
        full result used); CommittedHeap cache 1,024 sorted ranges (same 120-save clear); LoadAB name flags cached.
      - Online: hellos read more than 25 ms after the previous tick take no round-trip sample and are not echoed; "ONLINE:
        the opponent does not run the mod" logged 5 s into such a match (launcher message).
      - Overlay / launcher (UX agent): compact line "ROLLBACK  22 ms  delay 1" + 3-bar quality marker (rtt, depth,
        rollbacks/s, recent stalls), F9 = detailed line, warnings "Opponent not responding", "Connection hiccup: waited
        0.6 s", "This PC hitched: N ms frame" (FramePace::Quality, not in a session's first second), readable stop
        reasons for 12 s; launcher: Network display Compact/Detailed/Off ('overlay N'), opponent wording, live delay
        change, "Having trouble?" help, shared-read log zip, version in the title.
      - Not done (follow-ups): Safe mode delay 1-2 since early send (lockstep runs needed); keyboard read through
        GetKeyState before the frame wait (late sampling may not help keyboard players; verify); input-to-present stamp;
        log writer thread; MK9 limiter catch-up after >= 67 ms frames (K 2 -> 6); delay 0 automatic once phase alignment
        is proven; RepairAttachBase over the actor set; quarantine candidates by frame index.
      - Checks: b44 (rc24 baseline vs rc25: stages 1,3,7,13,15,23 at ns15 d1 and ns30 d1 combos; diffmerge 2 check; dropB,
        freezeB, jittery ns60 on both builds; SyncTest soak 9301-9302 with diffmerge 2).
      - **b44 results (`results\batch_b44.txt`, `results\scenarios\b44*`).** rc24 baseline (ran twice as tag 'b': a batch
        loop flattened its argument array and used the default dirs, which hold the same rc24 DLL; the second run's files
        renamed b44base): ns15 d1 rollbacks per minute A/B 106/229, ns30 d1 243/365, lopsided per fight (Graveyard 33/194,
        PitDay 7/234, Subway 5/166). rc25: **live save per frame 3.0 -> 1.0-1.5 ms at ns15 (skipped 230/116 -> 5658/5949
        per 6 fights), boundary 3.8/3.7 -> 1.8/1.3 ms; ns30 2.9 -> 2.0 ms and 3.8/4.7 -> 2.7/3.2 ms**; ns15 rollbacks
        106/229 -> 109/81, ns30 unchanged (one way > a frame). Hitches >= 34 ms per minute 3-4 either way. Every first
        mismatch RNG-only with identical fighters (DeadPool 192 E5E35C8B/41F4F8A0 and DD14CFE6/F66D0609, PitDay 217, frame-0
        start stops): the demo-script artifact. diffmerge 2 check: 0 DIFFMERGE MISMATCH. 0 crashes (one startup HANG? 6 s
        after injection, as rc24). Network (3 fights each): dropB rollbacks A 45 -> 15; freezeB, jittery ns60 as rc24.
      - **Phase alignment oscillated in rc25** (PHASE lines): shares flipped ~50 <-> ~970 per mille each window, 7 corrections
        and ~40 ms moved per fight: a step sized from the share difference jumped past the balance (both games are in time
        only within ~2 ms of it at ns15). **rc26 (`build\clang_rc26` DFC1F678): step search** (3 ms, halved on every
        overshoot, x1.5 while the same game keeps leading, 0.5-6 ms). **b46 (rc26): ns15 d1 rollbacks per minute 7/27 (rc24
        106/229, rc25 109/81: -90%)**, 6-8 ms moved per fight, boundary 1.1/1.3 ms, hitches >= 34 ms/min 2.4/3.6, P99 16-17;
        ns30 d1 as rc25. First mismatches all the RNG-only artifact; 0 crashes, 0 hangs.
      - **SyncTest soak on rc25 (9301-9302, diffmerge 2): one game hang**, the first in 144 soak logs: seed 9302 gf 5587,
        UE3's purge loop 0x00516E70 (IncrementalPurgeGarbage from 0x005F225E, no time limit: it retries an object until its
        vt+0x2C IsReadyForFinishDestroy says yes; ecx ~2e9 iterations), right after the fight-end "MKHEAP large request:
        heap #2 395 KB". Sync errors 22/29 (script/spawn classes of script_scheduling.md 4b; rc23's 9201-9202 had 16, other
        seeds), SyncTest 2.4x faster than rc24 (loads 56 -> 7 ms: VirtualQuery dominated rc24's SyncTest loads).
        **b45 same seeds: rc24 0 hangs in 7 fights; rc25 defaults (diffmerge 1) hung again (seed 9301, fight 2, crumb
        'rollback:resim EngineTick(6401)', same purge loop, same heap #2 request just before).** diffmerge is not it (mode 2
        uses the full merge's bytes and hung too); phase / skiplive / re-poll are P2P only. Suspects: the larger committed-
        range cache (classification while fight-end memory is released) and the guarded Readable/Live. rc27 adds
        'heapcache N' (64 = rc24), 'slotnt 0|1' and a HANG? line naming the purge loop's stuck object (class, outer,
        nonzero dwords); b47 soaks it with defaults and with 'heapcache 64' + 'fastreadable 0'.
      - **b47/b48 (rc27 `build\clang_rc27` 482A2B69 = rc26 + those switches + the purge diagnostic): 0 hangs in 47 fights
        with defaults** (S1 seeds 9301-9302 18 fights, b48 seeds 9303-9305 29 fights; sync errors 0-12 a fight, the known
        classes), 0 in 7 with 'heapcache 64' + 'fastreadable 0' (that pair restores rc24's VirtualQuery-bound SyncTest
        speed: 3-4 fights a session). rc25-rc27 SyncTest hang rate so far 2 in 75 fight ends; rc24 and older 0 in ~500.
        Caller: 0x007F7B94 -> UObject::CollectGarbage 0x005FD6F0(0x4000000, full purge) -> 0x005F2220 -> purge 0x00516E70
        with no time limit: a map change. Real online sessions end at MK9's results screen (game state 9), before the map
        is unloaded, so no P2P session rolls back across it; the two-instance runs (sessions end at the timeline restart)
        had 0 hangs in 72 fights on rc25/rc26. Working hypothesis (not confirmed): a render-thread fence counter
        (FRenderCommandFence::NumPendingFences in a component or resource) restored as pending after the render thread
        cleared it; rc24's SyncTest loads took 56 ms (VirtualQuery: prerestore 18.9, motion 12.4, dangling 19.5), rc25's 7,
        so the render thread had always caught up before a load. The HANG? purge line names the object next time.
      - **Candidate: rc27** (482A2B69; GGPO build\ggpo_rc25, now also build\clang\libggpo.a).
      - **Gate 2026-11-09 21:51-22:23 PASSED on rc27** (user's yes; `results\gate_20261109_rc27.txt`): online smoke, the three
        regressions (3 fights each, 6 checksum reports each, 0 mismatching, 0 failed saves, 0 crashes, 0 cut sessions; both
        games re-armed themselves 3 times; 20 delay changes) and the 8-minute menu walk (305 key presses, no hang or crash).
        Zip `dist\MK9Rollback-netplay-20261109-2223.zip` (DLL 482A2B69, version 20261109-2223). **Published 2026-11-09
        23:37 as v20261109-2223 (Latest)** on the user's yes; the downloaded asset matches the gated zip (D7FB0141).
        Next evidence: both players' logs from a field match on it (TIMING line, 'OVERLAY draws ... max draw time',
        rollbacks a minute at delay 1). Seen in the gate's logs, neither a failure: (1) game A's overlay had one 53.9 ms draw in ~26,000 in the
        auto re-arm run (22:05:58-22:06:08, fight 3; no HITCH on A's game thread then; the other runs' maximum 2.4-3.9 ms,
        rc24's gate 0.9-2.6 ms): unexplained, look for it in the field logs ('OVERLAY draws ... max draw time');
        (2) game B's boot-time 'HANG? ... crumb=idle(0)' report at gf 0, the same one older B logs have (10-29, 11-04).
        Phase alignment in the gate (loopback, B slowed): 3-7 corrections and 4.6-26.4 ms moved a fight.
    - **"Professional level" round 3 (2026-11-10, 00:00-06:45; the player: "like the latest games in industry, find any
      algorithm, library, they use... launch subagents to check what we are doing wrong"): seven read-only subagents
      (industry research; rollback presentation; input and frame pacing; time-sync controllers; session lifecycle;
      launcher / updater / packaging; mining of the b44 / b46 / rc27-gate logs) and four implementers with disjoint files
      (GGPO patches; log writer; hello protocol; launcher), the rest by the orchestrator. Candidate `build\clang_r3`
      and `build\clang_r3b` (DLL 832DC235; GGPO `build\ggpo_r3\libggpo.a` 961C14E5) = rc28. **Gate passed 09:38 (the
      player: "Run the gate"; below): build\clang / build\clang2 hold rc28, rc27 is kept in build\clang_rc27 +
      build\ggpo_rc25. Published 11:18 as v20261110-1118 (the player: "Publish it").**
      Findings that changed code:
      - Log mining + presentation audit: **every replayed frame was drawn and presented** (presented = session frames +
        replayed frames: 64-74 presents a second at ns30 d1; with vertical sync a rollback shows a stale intermediate
        picture and can hold a refresh). Sounds: the exact-frame match doubled a sound whose frame moved by one in the
        replay, and the P2P counters were thrown away. The first live frame of a session took 34-65 ms in 11 of 22
        sessions, followed by a 1-frame BALANCE payment at +0.68 s in 7.
      - Timing audit (verified in all 10 complete b46 fights): the game that waited at the start barrier sprints back onto
        its old grid (MK9's limiter catches up every frame under 50 ms late) and plays 1-2 frames ahead: every ns15
        rollback fell in the first 10 s. Frame advantage rounded at 16.7 / 50 / 83.3 ms, the very values a round trip read
        at polls takes (+-1 between the games); BALANCE and the phase search paid for the same lead (20 ms for under 6);
        'MK9P' reports of an earlier session could hide a rematch's; the CPU floor (delay 2 for the rest of the process)
        counted deep rollbacks the other PC caused; the delay rule's limits (100/133/200/250 ms) sat on whole-frame hello
        readings. Its harness finding: the netsim releases packets at the sender's polls only, so ns15 is exactly one
        frame and the in-time window has no width: b46's -90% proved re-poll plus rough alignment, not convergence.
      - Input / pacing audit: nothing measured what the player feels (controller sample to picture); Logger::Info wrote
        and flushed the file on the game thread, plus a Flush every 60 ticks; MK9's limiter spins a core in Sleep(0);
        the display mode was not in the logs.
      - Lifecycle audit: a game that stops mid-fight just goes silent (the other stands 3 s at its prediction limit); a
        build or settings difference was found 24 frames into the fight, and another online version shown as "no mod";
        an out-of-sync match was only counted; the checksum socket listened on every interface; hello values from the
        peer unclamped; stock GGPO asserts (= the game closes) on a malformed packet and on any sendto error, and runs
        on without a socket after a failed bind; channel 78 was not drained at a session's start.
      - Launcher audit (12 findings): update not transactional (a failed copy left a mixed install), SteamIDs / user
        names / IPs in the logs zip, injector result misread, silent start failures, lost commands, no single instance.
        **Bug in the released updater:** it cuts relative paths with the length of %TEMP% as given (often the 8.3 short
        form) against Get-ChildItem's long names: with a user name whose two forms differ by more than one character the
        files land in a junk folder and nothing is updated (release notes: "if the update does not take, extract the
        zip again"; the new updater is fixed, the old one installs the new zip the old way).
      - Research (Skullgirls, NRS, Slippi, GGRS, SF6, Steam): at parity on delay policy, time sync, packets, disconnect
        warning and desync detection. Taken from it: replayed frames are never shown, sounds matched within a frame or
        two, Wi-Fi / relay shown to the player, build agreement before the fight, bye packet.
      **Changes** (each with a switch; defaults on unless said):
      - **RenderTag (new src\rollback\RenderTag.*; 'replayskip 0|1').** The begin-drawing render command (vtable
        0x00D59D08, queued at 0x00756C0E / 0x00757A47) gets one of two vtable copies by Rollback::gIsResimulating; the
        replayed copy's Execute sets a mark that RHIBeginDrawingViewport's late-flag read (0x00666DF9) ORs in, so the
        game's own late-frame path leaves out the draws and the Present (render thread only: the game thread's work in a
        replay is unchanged, so determinism is as rc27). At most 16 in a row. REPLAYSKIP line per session. The same tag
        carries a time stamp per frame: **LATENCY line** (FramePace): controller sample (Input::SampleCapture) to the
        Present call of the first frame that input changes, delay frames included, p50/p95/p99/max per window and session.
      - **WaitShift ('waitshift 0|1').** A boundary that waited for the peer (start barrier; prediction limit + stall
        hold) moves the limiter's deadline 0x00E0AB10 by the wait in whole frames instead of letting the limiter take it
        back. At the start barrier the one-way trip is taken off first: half of (the handshake's round trip - one frame;
        new export `ggpo_sync_rtt_ms`, measured against the last 8 requests' send times so a retry does not stretch it):
        the round trip is read at both games' frame polls, 0-2 frames over the wire's (r3smoke: 50-53 ms on the
        one-frame harness link; half of that moved 1 frame where 2 were needed). WAITSHIFT lines, count in TIMING.
      - **Handshake polls ('handshakepoll 0|1', off).** With 1 the paused wait for the session polls every millisecond
        for 12 ms of each frame: the handshake is two frames shorter and its round trip is the wire's (27-31 ms where
        the frame polls read 29-39), the estimate then being half of it as read. Not the default: on the timed netsim
        (below) the frame-0 wait no longer told which game was ahead (26-28 ms on every start: a round trip, because
        the game that ran second sends its first input one trip after the first one ran). Its one-frame shift was given
        back by a BALANCE frame 1.2 s later in 3 of 5 starts, against 0 of 6 with frame polls; on the stock netsim 3 of
        6 fights began with 23-58 rollbacks (batch r3c) against 0-3 (batch r3). To use the exact round trip the start
        has to be timed some other way (follow-up 1).
      - **First input ('firstretry N', default 3; GGPO `ggpo_first_input_retry_ms`).** Found with the handshake polls
        (batch r3b, 82F32EC8): GGPO's two handshakes are independent and a side drops every input packet until its own
        is done. The game that ran first (by 13-17 ms in 4 of 6 starts) sent its frame-0 input into that; the copy came
        34 ms later ('netretry'), so the later game waited 29-32 ms at frame 0 and its WaitShift took the wait for a
        lead: both games moved a frame, the real 14 ms lead stayed (2 of 6 fights: 32 and 17 rollbacks, a BALANCE give,
        22.0 ms of phase moves). Until the peer's first input is in, pending inputs are sent again every 4 ms (batch
        r3c: no start with both games waiting any more). The fast resend has no bound of its own: it ends with the
        peer's first input or the disconnect timeout (a peer that never answers gets ~250 small packets a second for
        those seconds; a bound was left out on purpose, the start barrier holds the game meanwhile anyway).
      - **Timed netsim ('netsimtimed 1', test only; GGPO `ggpo.network.timed`).** The stock simulator keeps a packet
        in the sender's queue and looks at it only when the sender polls: once a frame in a session, so 'netsim 15'
        was exactly one frame of trip on the sender's frame grid and nothing under a frame could be measured (the
        timing audit's harness finding). Timed, the packet leaves at once with its due time (8 bytes more; the
        system-wide performance counter, both test games being on one PC) and the receiver holds it until then: read
        at the receiver's first poll after the due time, as on a real link; delay drawn once per packet, 2/3..1 x,
        order kept. **First faithful measure of delay 1 at a ~28 ms round trip (batch r3d, 'netsim 15', 12 fights):
        0-3 rollbacks a fight; in 10 fights every 2 s window had each game's input in at the other's first poll in
        990-1000 of 1000 frames, in the first fight of each run from the third window on (6 ms of phase moves; the
        alignment then carries over to the next fights, which whole-frame shifts keep); LATENCY p50 19-21, p99 20-26
        ms.** The stock mode stays the default of the scenario sets (comparable with every batch before).
      - Phase search ('phasegrow N', default 2; 1 = rc27): the step grows only from the third decision in a row with the
        same leader (r3smoke: it grew right after each halving, 3.0 4.5 | 2.2 3.4 | 1.7 2.5 ..., swinging all fight).
        Reports for windows this session cannot have reached are dropped; a BALANCE give cancels the phase move still
        due and quiets the phase measure 60 frames; so does a delay change (and restarts the BALANCE samples).
      - GGPO (tools\build_ggpo.ps1, blocks marked round 3; subagent + two of mine): `ggpo_bye_on_close` (three input
        packets with stock's disconnect_requested at the close: the receiver, an older build too, raises DISCONNECTED in
        ~2 ms); every length / count / index the peer sends is validated and a bad packet dropped (16 of 16 malformed
        cases survive; stock asserted); `ggpo_bind_failed`; sendto errors logged, not asserted; frame advantage rounded
        with edges at 25 / 58 / 92 ms. DLL side: the bye is sent for stops that are not a normal end (not "match over",
        "restart", "timeline restart"); a session whose port cannot be bound is refused with the Windows error; a peer's
        bye that arrives while this game's own out-of-sync count is past half its limit is reported as out of sync.
      - **Out-of-sync stop ('desyncstop N', default 120; 0 = rc27):** in an engaged online match the session stops after
        120 compared frames in a row differ ("NETPLAY: the two games went out of sync"); harness sessions keep running.
      - Checksum / phase socket: bound to 127.0.0.1 and connect()ed to the peer for bridge and one-PC sessions.
      - **Hello (subagent; new src\net\HelloProto.*, OnlineMode, SteamBridge, EnvFingerprint):** 52 bytes, the first 28 as
        before (version 4): release stamp, content and DLL prints, switch print, Wi-Fi flag. `ready` stays 0 unless both
        prints match, so two different builds (this one against rc27 too) never start a rollback session: MK9's netcode
        plays, and the log says which side is older ("ONLINE: the other player's mod is older" / "this mod is older than
        the other player's" / "is a different build" / "game files differ" / "runs with other settings"). Delay values
        clamped to 8, player index to 1-2; channel 78 drained at Start (77 kept: GGPO's first sync request may wait
        there); `SteamBridge::UsingRelay`, default route on Wi-Fi; overlay detail line: "Wi-Fi: you / opponent / both",
        "(Steam relay)". Host test: 85 checks. Delay limits 108 / 142 / 208 / 258 ms (off the whole-frame readings).
      - **Log writer thread (subagent; Logger.cpp/.h):** Info / Frame format on the caller and append to memory under a
        short lock; a below-normal thread writes (Info wakes it, Frame within 100 ms); Flush is a bounded synchronous
        drain for the crash / exit paths; exit-time lock orphans handled (HEAD lost its last lines in 58 of 60 heavy
        exits, new 0 of 360); same bytes in the files. Info on the caller 12.1 -> 0.7 us median, worst 435-822 -> 0.9-2.1
        ms in a 4-thread burst. The Flush every 60 ticks is gone (GameLoopHook). LOGIO line per session.
      - FramePace: the CPU floor counts only frames >= 25 ms with a load of depth 1 after frame 300; DISPLAY line once per
        device (windowed / fullscreen, size, vertical sync, refresh rate, a note for vertical sync on a monitor that is
        not at 60 Hz); **'limiterwait 0|1' (default 0)**: MK9's limiter Sleep(0) loop (0x004026FF) waits on a
        high-resolution timer until 4.7 ms before the deadline, the final spin unchanged.
      - Sounds ('soundnear N', default 2): a replayed sound matches the original within N frames inside the replayed
        span (kept, not doubled); SOUND line per session. Commands are taken by renaming the command file (a command
        written while the DLL read the file was lost).
      - **Launcher (subagent; tools\netplay_gui.ps1, updater.ps1, launcher_lib.ps1, launcher_selftest.ps1, package.ps1,
        Play Online.bat, injector):** allow-listed extraction and a transactional file swap with rollback and a "Go back
        to the previous version" button; installed-files check before Play; logs zip without SteamIDs, user names, IPs
        (stable pseudonyms); injector exit codes 0-7 with retries; one message table (the new ONLINE / NETPLAY lines
        included); background update / zip / Wi-Fi check; commands acknowledged; single instance; DPI; "Last match"
        line. Selftest: 57 checks with no game open (the two restore checks run only then; with one open the refusal
        is checked instead).
      **Checks** (one PC, two instances, PC locked; `results\batch_r3.txt`, `results\scenarios\r3*`; baseline b46 = rc26):
      - Host tests, now in the repo (`tests\host\run.ps1 [-GgpoLib ...] [-Only hello,ggpo,logger]`, no game needed):
        hello 85 checks; GGPO against a hand-made remote that sends malformed datagrams of 14 kinds into a valid 60 Hz
        session (16 with the pre-handshake and never-acknowledging cases; stock asserted); log writer stand-alone
        (240,000 lines, 360 exits). Launcher selftest 57 (`tools\launcher_selftest.ps1`). None of the hello /
        engagement path or the launcher's game-attached paths runs in the one-PC harness: the gate and a field match
        cover them.
      - r3smoke (C7633F01, before the estimate fix and 'phasegrow'): LOGIO longest write 0-1 ms; DISPLAY line
        (fullscreen 1920x1200, vertical sync on, 165 Hz: the note fires on this PC); rollbacks worse than b46
        (32-45 a fight) = the two faults fixed above.
      - **Batch r3 (F7F5AC2F = all but the handshake polls; 02:16-03:56). Stages ns15 d1, 6 fights: rollbacks per fight
        0/0, 0/3, 0/0, 0/0, 0/0 (one fight = the known frame-0 start stop): 0 / 1 a minute (b46 7 / 27, rc24 106 /
        229)**; every waiting start moved 2 frames; 0 BALANCE gives; phase moved 6.0-6.8 ms a fight; presented 9828 of
        9830 frames; LATENCY p50 20-23, p95 22-25, p99 22-27 ms (max 75-101 = the frame at the fight's end).
        **Stages ns30 d1: presented 9822 of 9830 (b46: 64-74 a second)**, 652 / 1796 replayed frames not drawn, 0 forced;
        rollbacks 98-159 / 201-254 a fight as b46 (the trip is longer than the delay); LATENCY p50 21-24, p99 26-35.
        ns30_d2: 34 / 4 rollbacks a minute (fight 1: the 3-frame start above), LATENCY p50 37-39, p99 40-44.
        ns60_d2_jittery: all three fights in sync, 117-126 loads a fight, LATENCY p50 39-40, p99 45-48.
        Stalls (780 ms freezes, packet drops on B): 0 crashes, sessions recover, WAITSHIFT 46-47 / 11-14 frames at the
        prediction limit. Checksums: b46's pattern fight for fight (the RNG-only intro mismatches and start stops of
        the harness), nothing new; 0 crash lines anywhere.
      - A/B on the same build (3 fights each): **'replayskip 0' -> 64.8 presents a second, on 59.7**; **'waitshift 0'
        -> 57 / 12 rollbacks a minute at ns15 with BALANCE gives and 21.5 ms phase moves, on 0-2**; 'phasegrow 1' ->
        35 a minute and 20.0 ms moved in fight 1 against 25 and 12.1 (one bad start each: small sample); 'soundnear 0'
        -> no replayed sound was a frame or two off in either run (the case needs a longer or rougher link);
        **'limiterwait 1' -> 10,000+ timer waits a session, at most 0.6 ms late, frame p99 16-17 ms, rollbacks and
        latency as the base run: no regression and no gain measured here, so it stays off** (its gain is a core not
        spun, which matters on 2-4 core PCs: try it in the field).
      - Batches r3b (82F32EC8, handshake polls on) and r3c (68CFA19E, + first-input retry), stock netsim, stages ns15
        d1: rollbacks per fight A 0/0/0/32/0/17, B 0/0/11/0/0/0, then A 2/0/0/4/0/36, B 58/0/3/0/23/0. r3b's two bad
        fights had both games waiting at frame 0 (the dropped first input, fixed in r3c); r3c's three had one game
        1.3 frames ahead by the wait and moved one. Both stopped early once understood.
      - **Batch r3d (2E489D5E), timed netsim, stages ns15 d1, 6 fights each: 'handshakepoll 1': rollbacks 0/2, 0/0,
        0/0, 0/0, 0/0, 0/0 but a BALANCE frame at 1.2 s in 3 of 5 starts (the start shift undone, or a second frame
        needed; no input changes that early in the scripted fights, so no rollbacks came of it); 'handshakepoll 0':
        3/0, 0/0, 0/0, 0/0, 0/0, 0/0, no BALANCE frame, every waiting start moved 1 or 2 frames and stayed.**
      - **Batch r3f (832DC235 = the candidate: handshake polls off, first-input retry 3, GGPO 961C14E5; 04:58-05:59,
        `results\batch_r3f.txt`, `results\scenarios\r3f*`, 30 fights). Timed netsim, stages ns15 d1: 0 rollbacks in all
        6 fights on both games**, no BALANCE give, every 2 s window 1000/1000 from the first in 5 of 6 fights (3 ms of
        phase moves in fight 1), presented 11795 of 11796 frames, LATENCY p50 19-20, p99 20-26 ms. Timed ns30 d1 (a
        ~55 ms round trip held at delay 1): 103-222 rollbacks a fight, presented 59.7 a second, 843 / 1130 replayed
        frames not drawn, 0 forced, LATENCY p50 21-24, p99 26-32. Timed ns30 d2: rollbacks 0/4, 0/0, 0/0, LATENCY p50
        36-38, p99 37-40. Timed ns60 d2 jittery: 105-126 rollbacks a full fight, presented 59.4-59.6 a second, LATENCY
        p50 38-39, p99 39-48; one 273.5 ms start wait moved 13 frames. Stock netsim, stages ns15 d1: rollbacks A
        0/2/1/15/1/0, B 0/0/17/1/0/10 (b46: 2/0/2/10/0/10 and 41/23/0/0/5/20; the one-frame link has no in-time
        window, so this run only says "no regression"), LATENCY p50 20-22, p99 22-27. Stalls on B (stock ns30 d2):
        780 ms freezes -> 9 stall holds, 9 prediction-limit shifts, all three fights in sync; packet drops -> recovered,
        in sync. No stop, out-of-sync or disconnect line, 0 crash lines; the only hang line is the boot-time report on
        game B that every batch has. A timed run is the stock scenario plus one setup command: `tools\scenario_matrix.ps1
        -Set stages -Only stages_ns15_d1_combo -StageList 1,3,7,13,15,23 -Setup 'netsimtimed 1' -Dirs build\clang_r3,
        build\clang_r3b -Tag <tag>`.
      - **Checksums, classified (new `tools\desync_digest.ps1 [-Filter 'r3f*']`: per fight, which fields of the two
        games' first mismatch differ and how their 300-frame state samples compare; exit code = fights with life or
        meter apart).** r3f: 17 of 30 fights without a mismatch, 3 frame-0 start stops, 8 with RNG A only first
        (Courtyard and RooftopDay as in every batch; Graveyard / PitDay at 192 like b46's PitDay and the DeadPool 192 of
        batch r3), and 2 with positions first: Graveyard frame 157 (timed run, 0 loads on either game: P2 123.021
        against 122.031, to the digit the walk step of net4's DeadPool with the same fighters) and PitDay frame 180
        (both fighters 0.001 apart, then a constant 0.010 for the whole fight with life, meter and RNG A equal at every
        sample). **0 fights with life or meter apart.** Over every scenario log kept (1082 fights): 19 position-first
        mismatches after frame 9 since net1, 11 of them a constant offset under 0.5 for the rest of the fight; such an
        offset also follows an RNG-only start in a fight without a single load (r3f timed PitDay: 0.004). So both are
        the attract-demo start-phase class (the two games reach their first fight frame at different points of the
        intro), not the rollback: still unproven until a two-PC match (the 11-03 field test had 0 mismatches). Life or
        meter apart: 12 fights in all, in net2 ... xray6b and b44base, none in b46 or any r3 batch.
      - **SyncTest soak on the candidate (06:10-06:38, `results\synctest_soak_r3`; seed 9311 random input, 9312
        scripted combos): 18 fights, 6,366 loads, 25,464 replayed frames, 0 crash or hang lines, both games alive.**
        Sync errors 20 + 22, the classes of the rc27 soak (9301 / 9302: 22 + 29): 8 end-of-fight groups of 4 ("replay
        ended at gf=4"), and in the combo session 10 errors at frames 48-108 with the same actor signature as rc27's
        at 40-156 (+8 00104010>00104000, +58 7>0: the start class of the poked-meter combo sessions). No new class.
      - Final runs on the candidate (06:08): `tests\host\run.ps1 -GgpoLib build\ggpo_r3\libggpo.a`: hello 85 / 85, GGPO
        14 / 14 cases, log writer all checks; launcher selftest 57 / 57 with no game open (both restore checks ran).
        Released files untouched (build\clang DLL 482A2B69, libggpo 5C505AEF).
      - **Gate on rc28 (09:05-09:38, PC unlocked, `tools\queued_checks.ps1 -Now -Soak 0`, `results\
        gate_20261110_rc28.txt`): PASS.** Online smoke (hooks installed, armed, disarmed); plain, auto re-arm and
        delay-change regressions each 3 fights, 6 checksum reports, 0 mismatching, 0 failed saves, 0 crashes, 0 cut
        sessions (both games re-armed themselves 3 times; 21 delay changes applied); menu walk 305 key presses, no
        hang or crash, game responding. OVERLAY "while resimulating" 0 in all three runs (rc27: 61 / 47 / 21: the
        replayed frames are no longer drawn), max overlay draw 4.5-5.6 ms (rc27 had one of 54 ms). The only hang
        lines are one boot-time report on game B (delay-change run, 6 s after the process start, crumb idle).
        **Zip: `dist\MK9Rollback-netplay-20261110-0938.zip`** (SHA-256 ABD30923...; DLL 832DC235 and injector 6456D98B
        inside, version.txt 20261110-0938, 11 files, all on the updater's allow-list).
      - **Published 11:18 (the player: "Publish it"): v20261110-1118, Latest** (`dist\MK9Rollback-netplay-20261110-
        1118.zip`, SHA-256 1FD01499...; the downloaded asset matches). Not the 0938 zip: the scan before publishing
        found the short form of this PC's Windows profile folder in a comment of tools\updater.ps1 (the short-path
        example); replaced by a generic one, selftest 57 / 57, packaged again from the unchanged build\clang. Entry by
        entry the two zips differ in updater.ps1 (that comment), README.txt and version.txt (the stamp) only. Notes:
        rollbacks no longer shown, aligned starts, a leaving player noticed at once, go-back in the updater; both
        players need this version; "if the update does not take, extract the zip again" (rc27's updater). Source
        changes rc19-rc28 are not committed. Next evidence: both players' logs from a match on it (the hello /
        build-agreement path, LATENCY and TIMING lines, the DISPLAY note).
      - Not done (follow-ups, by expected value): (1) one PaceControl with a raw frame-gap exchange ('MK9T') replacing
        BALANCE's whole-frame samples and the start barrier's wait-based guess (each game's frame-0 instant sent to
        the other, or one agreed start instant: then 'handshakepoll 1' and its exact round trip can be the default),
        and a faster phase search (40-frame windows, 4 ms first step), all measured on the timed netsim; (2) the
        scenario sets on the timed netsim by default once a baseline batch exists (new numbers are not comparable with
        b46 and before), plus sub-frame jitter and loss cases;
        (3) a visible "waiting for the opponent" state (the wait blocks inside the boundary, so the overlay cannot say
        it) and a coordinated stop frame; (4) delay from measured margins between rounds, delay 0 when both shares stay
        >= 990; (5) stopping the sound of a mispredicted frame, late-start seek, announcer hold (game functions not
        found yet; lead: "STOPSOUND" 0x58B8B7); (6) key messages pumped at the boundary for keyboard players (probe
        first); (7) signed releases (needs the user's decision: an offline RSA key); (8) ISteamNetworkingMessages
        (the legacy P2P interface is deprecated); desync recovery; input recording for offline desync replay; VERSIONINFO;
        the phase target when the one-way trip exceeds the delay; clock-rate feed-forward and a rate cap instead of the
        150 ms session total; "connection too slow" from a 330 ms hello; MatchMode check; auto-mode opponent memory.
    - **net8 (84C59BBF: peer-seen 150 ms first-round retry, rayqfix in P2P, render-thread samples; `results\scenarios\
      net8*`, 02:22-03:07, 18 fights, `stallsample 30`): 0 crashes; desyncs only the known harness ones** (DeadPool
      intro frames 176-279 in 4 of 6, PitDay frame 0 in 3 of 6); all 6 Graveyard fights clean. Handshake medians as
      net7 (ns30 82-84, ns60 116-117, ns100 184 ms). **Start-barrier waits on lossy links: max 544 / 588 -> 262 / 306 ms,
      over 300 ms 3 of 22 -> 1 of 22.** RAYQ installed on both games in every run and dropped stale results in P2P
      rollbacks (`RAYQ stale result ... dropped`, 1-3 per run); its counters were not printed (no session reached 600
      loads: now also printed at each session end). The largest frames are the start barrier (gf 100001, 99-319 ms) and
      the harness's timeline restart at the demo's end (gf 0, 90-117 ms), not in-fight.
    - **The Graveyard hitch is a GPU/driver wait in Present.** All 12 Graveyard games (net8) hitched once, 53-65 ms,
      at the same fight frame on both games (100955-101025; net7 likewise). STALL samples, identical in every case: game
      thread in the render fence wait (0x0052B881 <- 0x00636153 <- 0x0040299A); render thread blocked in the NVIDIA
      user-mode driver (`nvd3dum+13BD7D` -> KERNELBASE wait), under d3d9's Present (`d3d9+E02EB` via the Steam overlay's
      hook `gameoverlayrenderer+7911A`). So the GPU is behind by ~45 ms that frame (driver present throttling: a heavy
      frame or a first-use upload / shader compile). Graveyard was fight 1 of every run, so stage content and "first
      fight of the process" are not told apart yet: batch 5 runs stage order PitDay, Graveyard, Graveyard (ns30_d2) and
      Graveyard x3 (lan_d1) on 84C59BBF.
      - **Batch 5 (`results\scenarios\gyorder*`, `gyrepeat*`, 04:32-04:47): it is not the stage, it is fight 1.** The
        mid-fight hitch came in fight 1 only, on PitDay too: PitDay fight 1 gf 100990 (52 / 55 ms, both games) and
        100472 (43 ms, one game); Graveyard fight 1 gf 101015 (54 / 59 ms) and 100112 (48 / 49 ms). Graveyard as fight 2
        or 3 (4 fights in all): no hitch inside the fight, only the start barrier and the harness's gf 0 restart. Same STALL
        stacks (render fence wait, render thread in the driver's Present). In these runs the attract demo always makes
        fight 1 Jade v Reptile (fights 2 and 3 Reptile v Skarlet, Noob v Kano), so "first netplay fight of the process"
        and "Jade v Reptile's moves at ~16 s" are not told apart; both say a first-use GPU cost (a shader or texture first
        drawn), once per process, on both games at the same frame: no rollback, no desync, a ~50 ms freeze. Not a
        netcode item. gyrepeat fight 2 (Reptile v Skarlet, Graveyard, LAN) hit the frame-0 start desync (P2 z 0.219 vs
        0.000), the known attract-demo harness case seen on PitDay before: not stage-specific either.
    - **net7 (ABC36E6E: 2-round-trip handshake; `results\scenarios\net7_*`, 00:12-00:58, 15 fights, `stallsample 30`):
      the handshake is 4-5x shorter.** Connected -> synchronized medians: LAN 65 -> 17 ms, ns30 315 -> 67, ns30_d2_loss5
      335 -> 68 (max 1,133 / 899 -> 299), ns60_d2_jittery 531 -> 117 (max 1,333 -> 147), ns100_d3 780 -> 183 ms. 0
      crashes. Start-barrier waits over 300 ms: 3 of 22 (435, 544, 588 ms). Two of them: one game had waited 14 s (the
      attract demos reach the fight 14 s apart), answered the peer's first request with the kick, that exchange was lost
      (loss5 / jittery), and with no round trip measured the next try came 500 ms later (SYNC_FIRST_RETRY_INTERVAL) while
      the peer, already synchronized, held frame 0; the third, both started together and the first game lost a second-
      round packet. **libggpo: once a sync request or reply from the peer arrived, the first-round retry is 150 ms**
      (build_ggpo.ps1 'udp_proto sync retry once the peer is seen'; the 2-round-trip library without it is
      `build\libggpo_sync2.a`). In `build\clang_net8` (9D480E22); net8 queued after soak 14.
    - **The Graveyard frame is the render thread's.** net7's STALL samples on all 9 Graveyard hitches (frames 939-1022,
      43-70 ms, both games): the game thread sits in 0x006360E0, the render-command fence wait (spins on a counter with
      appSleep(0) = 0x0052B850 -> Sleep(0)), called once per frame from the main loop at 0x00402995. So that frame's 40-50
      extra ms are the render thread's work (GPU / driver / a resource made for the stage event). The sampler now also
      captures the render thread (the thread that calls OnPresent) in the same STALL line (9D480E22); net8 shows what it
      does there.
    - **The "content hitch" is Graveyard only.** HITCH lines outside the boundary (boundary < 10 ms) in net5 + net6:
      30, all on BGND_Graveyard, one per fight on both games at the same fight frame, frames 894-1018 (40-83 ms; two
      earlier ones at 40-500 at 40-43 ms); none on any other stage. A Graveyard stage event (its timing varies with the
      fight), not a general cost. **New `stallsample N`** (FramePace, off by default, also in player builds): a sampler
      thread suspends the game thread once a live P2P frame has run N ms (up to 5 samples, 8 ms apart) and logs `STALL`
      lines with eip and the stack's return addresses (MKKE as is, others module+offset). net7 runs with `stallsample 30`
      (each network scenario has a Graveyard fight). Build ABC36E6E... in `build\clang_raytap_c` = rayres issue/gate
      ring + stall sampler + the 2-round-trip libggpo.
- **Still open:** the one Graveyard mid-fight desync (net2, not seen again in ~150 sessions since); the DeadPool intro
  and PitDay frame-0 harness desyncs (to confirm on two PCs); an overloaded game (>= 10 ms over budget) presents
  unevenly; the content hitch (~55 ms) once per fight on both games.
  - The 03:35 chain skipped both matrices (the last regression instance was still closing when it checked for a
    running game) and the gate then waited for an idle PC until 10:00. The step runner now waits up to 90 s for MKKE to
    exit before that check.

**2026-11-03 two-PC field test (zip 20261102-2043, DLL 56BD0371; logs `results\online_20261103`: `desktop_212011` =
the user, P1; `downloads_212009` = the friend, P2, 1360x768).** User report: lag and a lot of stutter; a disconnect
at a rematch's character select after a stage change; the game froze at a fatality. Two matches, both engaged
(rollback, delay 2, ping ~50 ms), 0 desyncs (9,289 and 9,828 frames matched).
- **Match 1 (21:13-21:16):** 59 draws/s on both, quarantine steady at 3-7k blocks, save cost ~4 ms (user) / ~6 ms
  (friend, peaks 11 ms); barrier (prediction-threshold) waits 436 user / 1,729 friend, 7 / 3 time-sync events.
- **Disconnect at the rematch's character select:** the friend's MK9 idle timer (0x004F5E10) read 156-166 s there
  (stale since ~6 s into the session; the exchange's results were all 1), and its connection closed at 21:16:26.388,
  34 ms after OnlineMode's 15 s post-session grace ran out. The stage change only kept them in the select past 15 s.
  **Fix (EE8A69D4):** once a session ran in the match, IdleHook returns the time since the exchange's last success
  (result 1) when MK9's own timer is older; a peer that really stops still times out from its last data.
- **Match 2 (21:17-21:20): the quarantine grew ~1,100 blocks/s on both (to 195-200k blocks, ~130 MB)** and the
  friend's draw rate fell 58 -> 44 -> 30 -> 12 -> 8 -> 0/s from 21:18:44 while the game ticked on at ~52 frames/s;
  the user's fell to ~0 at the fatality (fight body gated off at gf 108403). Barrier waits 9,071 in 9,861 frames on
  the user's side (waiting for the friend). The friend's game exited (ExitProcess on the game thread, no crash or
  out-of-memory line) at 21:20:05, the user's at 21:20:09 after GGPO lost the peer.
  - Cause: bin-29 (625-704 byte) frees are parked until the timeline restarts, and an online match does not restart
    between rounds. lag_ns80_d2's census: parked 10,085 -> 20,775 -> 31,489 per 600 frames, top caller 0x005B7076 =
    0x005B7020 emptying the two TArrays of a per-frame render structure (Realloc(p, 0), 20+ callers in
    0x005B..-0x0066..). The local regressions show the same growth per fight (up to 39k), hidden by their 33 s fights.
    `Release` also walked the whole list every frame under gLock (render-thread frees in a tick take that lock).
  - **Fix (EE8A69D4):** the four free sites of 0x005B7020 (0x005B7076, 0x005B709B, 0x005B70CF, 0x005B70F3) are not
    parked (still deferred for the window); Release takes per-stamp buckets (`gDue`) instead of walking `gList`; the
    census (with the top parked caller) is logged outside the lab when the quarantine passes 20,000 blocks.
- **Release gate on EE8A69D4 (`queued_checks.ps1 -Now -Soak 0`, 22:40-23:13, user: "run unattended"): pass, zip
  `dist\MK9Rollback-netplay-20261103-2313.zip`.** Online smoke; plain, auto re-arm and delay-change regressions 0/6
  mismatching each, 0 crashes; menu walk 306 presses, no hang. Quarantine during play 4-10k blocks in every fight
  (before: climbing to 30-40k per fight), parked 0. **lag_ns80_d2 (23:14): pass**, 0/6 mismatching, 633 loads; during
  play 0.8-7.7k (before: 15-78k, 31,489 parked from 0x005B7076). Old logs kept in
  `results\archive_gate_20261102_56BD0371` and `results\archive_lag_ns80_d2_before_EE8A69D4`.
  - Left: while a session waits paused at its first frame (gf 100000, one test game slow to join) every free is stamped
    100000 and none is due: 21-40k blocks in ~20 s (census top caller 0x00401753), released once the frames advance.
    Online the peers synchronized within 0.2 s.
- **Open:** a two-PC match of several rounds on 2313 (the freeze and the character-select disconnect are not
  reproduced locally); save cost 4-6 ms/frame (the friend's PC has little headroom); the time-sync payment (a 16 ms
  wait every 20th frame while owed). Safe mode gate (`-Lockstep`) not run on EE8A69D4.

**2026-10-30 Safe mode (lockstep, delay 3) is the launcher default** (`tools\netplay_gui.ps1`: a "Rollback
(experimental)" box, unticked; settings saved before today open in Safe mode; README section NETCODE in
`tools\package.ps1`). The 14:15 build is in `build\clang_1415`.
Gate `queued_checks.ps1 -Lockstep -Soak 0` on 2145's DLL (20:07-20:44, `results\lockstep_gate_2.txt`): online smoke, the lockstep
regression (6/6 reports, 0 mismatches, 0 crashes, 0 cut sessions), the lockstep auto re-arm regression (same, both
re-armed 3 times) and the lockstep menu walk (307 key presses, no hang) pass. **lockstep_ns60 fails, no zip:** at the end
of fight 2, B stopped at the timeline restart (frame 1965) with its last inputs still in GGPO's netsim send queue
(dropped on close); A waited at frame 1966 until GGPO's 3 s disconnect, whose DisconnectPlayerQueue rewinds to the
peer's last frame: AdjustSimulation(1965) -> GgpoLoadState's lockstep branch stops the session without stepping
GGPO's frame -> ASSERT `_framecount == framecount` (sync.cpp:166) -> "GGPO Assertion Failed" dialog, exit(0) after OK.
**Any disconnect during a lockstep session ends this way** (the peer quits mid-fight, the connection drops > 3 s).
**Fixed in the 20:53 build (DLL 5CC62578…, built from the current source: it also has the 14:11-14:15 changes
2145 lacks; 2145's DLL and the old libggpo.a are in `build\release_2145`):** lockstep runs only frames with
confirmed inputs (the barrier at the boundary), so a disconnect's 1-frame rewind would replay the same inputs:
GgpoLoadState keeps the state ("LOCKSTEP: GGPO rewinds ... nothing is replayed") and GgpoAdvanceFrame only calls
ggpo_advance_frame. `ggpo lockstep N` during a P2P session now waits for its stop (every per-frame check reads
gLockstep). GGPO patch (`tools\build_ggpo.ps1`): ~UdpProtocol sends the netsim queue instead of dropping it (no-op
without netsim). The Safe mode gate adds `lockstep_peerloss` (B killed mid-fight in lockstep: A alive, frames
still running, session stopped, no GGPO assertion); no lockstep peer loss had ever been run (killb*.txt: rollback).
The gate now starts after 4 idle minutes and holds the display on: at 5 (this PC's display timeout) Windows' 15-min
away lock locked the PC mid-run and the menu walk could not focus the game (first try, `results\lockstep_gate_1.txt`).

**2026-10-31 gate on the 20:53 build** (01:12-01:50, `results\lockstep_gate.txt`): online smoke, the lockstep
regression, the lockstep auto re-arm regression (both re-armed 3 times), the menu walk (306 key presses, no hang) and
**lockstep_ns60 pass** (3 fights, 6/6 reports, 0 mismatches, 0 cut sessions, 0 crashes: the 20:53 fixes hold).
lockstep_peerloss failed on its check, not in the game (`results\lockstep_peerloss_1.txt`, `_1_A.log`): A logged
"LOCKSTEP: GGPO rewinds to gf=101269 from gf=101270", stopped the session ("the peer disconnected", 1242 frames
matched, 0 mismatches, no assertion), played on alone to gf=101966 (where every attract fight ends, as in
lockstep_ns60) and started the next attract fight about 50 s later. The check read the frames log 30 s after the
kill, inside that gap. `two_instance.ps1 -KillBAfter` now waits for A's session stop and counts A's frames over the
next 2 s (the gate wants >= 60); the gate kills B 10 s in. `queued_checks.ps1 -PeerLossOnly` reruns only this check.
Rerun 01:54-01:59 (`results\lockstep_peerloss.txt`, `_A.log`, `results\lockstep_peerloss_gate.txt`): **PASS**, B
killed 10 s in, A rewound at gf=100717, stopped (689 frames matched, 0 mismatches, no assertion) and ran 120 frames
in the next 2 s. Same DLL as the gate run, so `tools\package.ps1` made **`dist\MK9Rollback-netplay-20261031-0159.zip`**
(DLL 5CC62578E0D709825316EE7EC66DE455F881B3B631654ACD53A280BCB839CD66). Next: the 2-PC test with a friend in Safe mode.

**2026-10-31 10:56-11:04 two-PC online test on 0159 (logs: mk9r-logs-20261031-110403/110405.zip, user and friend):**
both attempts ran in **Safe mode** (one game process each, `ggpo lockstep 1`; the user thought the second was rollback,
but the box only applies at Play / re-arm, and the games were restarted with it ticked at 11:03:30, 30 s before the logs
were saved: rollback was never played). In both, the online data went active at a timeline restart (the user: the
character select), the opponent was found (process-memory pass, 1.6-2.5 s), hellos met, the mod armed ("session
starts at the first fight frame") and **MK9's online data went inactive (connection destroyed) when the character
select ended**, 15.6 s (10:59) and 1.2 s (11:02) after the arm, before any fight frame (no FRAME/FIGHT lines, no GGPO,
0 exchanges skipped); 20 s later "rollback disengaged (the online match ended)". On 10-29 (1837, never engaged: room
type 0 refused) the same players' char selects worked for 11 min. Read-through of what armed-but-not-started does
before the fight: ExchangeHook/IdleHook/SendHook pass through (SkipNative false, no grace), no bridge, no pause guard
(only while running), no quarantine (lockstep), no input override (fight body never ran), HandleGuard's lookup and
releases match 0x00B8FFD0/0x00B91480/0x00B90070 instruction for instruction. **Cause not found.** The friend's game
crashed at 11:03:07 in the menus (render thread, 0x00631E88 `call [vtable+0x18]` = FFFFFFFF: an MK9 graphics object
used after free; no MK9Rollback code on the stack). Rollback-mode menus log "save refused: object heap not discovered"
once a second (attempted every frame).
**11:16 build (DLL 383D7A71…; 0159's DLL kept in `build\release_0159`), diagnostics only:** "ONLINE connection end"
lines when MK9's online data goes inactive (mod state, the opponent's last hello, Steam's P2P session state with
the opponent: active/connecting/error such as timeout/relay/queued; MK9's longest packet gap per second over the last
8 s from the idle-time hook; the last 16 exchange results and this match's totals; MK9's last 8 online messages sent),
the P2P state at the engage and 20 s after the end, and "MK9's connection went N ms without a packet" (>= 1 s, 20
per match). Launcher: ticking/unticking Rollback while the game runs re-arms at once (applies from the next fight)
and saves the setting. queued_checks' wait also requires no MKKE running (a controller does not reset the idle time).
Suggested control test for the players: an online Player Match started from Steam without the mod.
**`dist\MK9Rollback-netplay-20261031-1129.zip`** (DLL 383D7A71B10DDA993E3D4E216CC33B990587F024A38FBEF2CF687B93AF2C4995):
made at the user's request for a test with the friend **without the release gate** (stopped at 11:29 after online
smoke PASS; the regressions, menu walk, lockstep_ns60 and peer loss were not run on this DLL). `queued_checks.ps1 -Now`
skips the idle wait (user permission).

**2026-10-31 11:34-11:40 two-PC online test on 1129 (friend: mk9r-logs-20261031-114006.zip; user's game hung, its log
read from the extracted zip on the Desktop, `build\clang\MK9Rollback.log`):** Safe mode, invited room (type 0). The
user: "disconnected after the loading stage". Online data active 11:38:32.8/33.4 (friend/user clocks, ~0.7 s apart),
opponent found by the process-memory pass ~4 s later, hellos met, engaged at 36.777/37.471, and **MK9's online data
went inactive 0.217/0.233 s after the engage on both games** (the third engaged attempt of three to end this way;
engage -> close was 15.6 s, 1.2 s, 0.22 s). Connection end report, both sides: Steam P2P active, connecting 0, error 0,
direct (relay 0), nothing queued; the opponent's mod answered 0.2 s before; MK9's packet gaps <= 61 ms (friend) and
34-49 ms in the last 5 s (user; 445/799 ms gaps 5-6 s earlier, during the friend's process-memory pass); last
exchange results 1 x15 then 0; MK9 sent no script message. No fight frame ran on either side (0 FRAME lines), no
INPUT override, no GGPO. So MK9 closes a healthy connection right after the mod arms, and nothing the mod does while
armed reaches MK9 by code reading (see 10:56 above): **mechanism still unknown**. Then the user's game crashed in the
menus 6.4 s after the disengage (opening a private room): game thread read [0+8] at 0x0092DDB2, a pure virtual call
(R6025 dialog) on the render thread, hang; the PC had to be powered off. Friend's 11:03 crash was also ~6 s after a
disengage. Exchange 0x00885F80 returns 0 early when 0x0081C430() != 0, +0x20 < +0x24 or +0x20 > [0xDF0568]; it calls
the pause setter 0x00407AB0(1) itself when waiting. Next idea: a hardware write watchpoint on gamepads+0x18 (all
threads, vectored handler logging eip and stack) to see what MK9 code closes the connection and why.
Local Versus over Steam (`ggpo steamnetplay`, launcher codes) avoids MK9's online entirely; tested only as
self-loopback, never between two real PCs.
**Connection watch (built 11:5x, DLL 18BDCC8A…, not yet run):** `CrashLog::ChangeWatchArm` puts up to four 4-byte
hardware write watchpoints on every thread (armed from a helper thread so the game thread is included) and logs each
change of a watched value with its writer ("WATCH <name>: old -> new eip=... callers: <EBP chain> stack: ..."), 12
per slot, staying armed. The online mode arms it when the online data goes active, on gamepads+0x18 and the three
flags of the closing check 0x0081C430 (online manager [0xE924F0] +0x20, +0x1B0, [+0x1F4]+0x78: any nonzero makes
the exchange return 0), and disarms 3 s after the data goes inactive. `watchchange <hex>` arms it on any address
(`watchchange 0` disarms); `tools\watch_smoke.ps1` checks it on the host frame (0x00E89BDC) and must pass before
this goes into a player zip (the PC was in use).

**2026-10-31 11:54 Local Versus over Steam test (1129, Rollback ticked; user's log zip mk9r-logs-20261031-115526.zip and
the live log): "both our games crashed on the initial screen". Not a crash:** the launcher's Local Versus mode arms
`ggpo steamnetplay` at injection, at the title screen; left there, MK9 played attract demos, and the first CPU-vs-CPU
demo (11:58:29, Raiden vs Raiden) started the session: AI drones removed, "first fight frame: pausing", waiting for
the peer (60 s join timeout), so the screen froze; the launcher then re-arms and the next demo does it again. The
same happened on the friend's PC. Fix (12:01 build, DLL 671024F4…): at the netplay start, a fight with >= 2 AIDrones
(both fighters CPU) is skipped ("NETPLAY: this fight is CPU against CPU ..."), the pads are released and the session
waits for the next fight; `netplaydemo 1` restores the old behaviour, and `two_instance.ps1 -Netplay` sends it (its
fights are the attract demos). Assumes a human fighter has no AIDrone (the drones are the only writers of AI pads).
Not yet tested (the user's game was running).
**12:07 watch_smoke PASS** (12:06 build, DLL 3E111350…; the watch now keeps up to 10 zero <-> nonzero changes per slot
and only the first 3 others, so a counter cannot use up the budget before the flag that matters): its own MKKE
(the user's game left running), launcher Safe-mode setup applied first, armed on 142 threads, host frame changes
logged with writer 0x00404E04 (callers 00404F0E 00405266 0098CA0F), disarmed on 147 threads, game still ticking.
**`dist\MK9Rollback-netplay-20261031-1208.zip`** (DLL 3E111350B64D722114D6CBFFB59197333BBBB590AE2F74D4F0C77E60E7156B83)
for the next online test with the friend; not run through the release gate (only watch_smoke; the demo-fight skip is
untested and the two-instance regression was not rerun with 'netplaydemo 1').

**2026-10-31 12:13-12:19 two-PC test on 1208 (user: mk9r-logs-20261031-121843/121948.zip, friend: 121853/121949).**
**Safe mode: a whole online match played** (invited room, 3 rounds, 11,300 frames, 0 checksum mismatches, ping 33-50
ms, no disconnect; the user quit afterwards). **The disconnect's cause, from comparing it with the three failures
(10:59, 11:02, 11:38):** MK9's online data goes active twice per match: the character select's phase (+0x20 5), then,
after the fight load, the fight's own phase (+0x20 6, GameFrame zeroed). In all three failures both mods found each
other during the character select and engaged there (armed, "waiting for the fight") and stayed armed through the
load; the char select's phase then closed normally (WATCH 12:15: +0x18 cleared at 0x0087FBBE by a script native,
callers 0x0045A36D/0x0045CB0C), and during the load one game never reached native 1182 (message 14, "loaded"; the
friend's load at 11:38 stalled with half its fighter objects) and **both created MKOnline_Disconnect** from
0x0081E918 <- Disconnect 0x0081C640(0xB) <- 0x0091B980 in the "[OnlineSession] ... Leaving!" handler 0x0091B6E0 <-
0x00886AFE <- PLAYER_REMOVE_REQ in HandleIncomingMessage (0x0091D570, "[OnlineSession] [Connection] [%s]
PLAYER_REMOVE_REQ"): the session host removed the player that did not load. At 12:15 the friend's opponent search
finished only at the fight's phase, so neither engaged before the load: the fight's phase came, hellos met within
10 ms, engaged, the session started at the first fight frame. Which armed-state effect stalls the load is not
known (code reading: the overlay's "waiting" line is drawn through the load only when armed; the pad override needs
the fight body; the quarantine is off in lockstep). **Fix (12:35 build, DLL E234A217…):** when the online data goes
inactive while engaged and the session has not started, OnlineMode disengages at once (keeps the opponent, clears
the last hello) instead of 20 s later; it re-engages at the fight's phase as at 12:15. Not run yet (needs the game:
online smoke, then a two-PC match).
**Rollback mode crashes at a level change, every player run with it ticked:** 11:03 (friend: title -> menu; user:
the second level change) and 12:19 (both: title -> menu, ~30 s after start, no session, nothing armed). Each log ends
with "PHYS destroy actor ... from=004A3F40" + "PHYS term bodies ... from=006DD815" (the world teardown's first
rigid-body termination), no CRASH/HANG line, the frames log flushed (likely ExitProcess or a fast fail). Safe mode
passes the same teardown (12:13, gf 1665). The rollback menu walk last passed on 2145's DLL (2026-10-29 21:37,
`release_gate_2110.txt`); never run since (the 0159 gate was Safe mode only), so the cause is in the 10-30 changes
(HandleGuard, Snapshot propskel, AnimInputTrace, EnvFingerprint, RngTrace) or 10-31's; rollback-only per-tick work in
the menus: the four audits (Attach/TaskReg/Heap/List) and a Save attempt every frame ("object heap not discovered",
0.8-4 ms, frame rate down to ~44 fps on the friend's laptop). The 11:04 "rollback crashed his computer" report was
this bug (it was read as Safe mode at the time). Next: `tools\menu_repro.ps1` on this PC (rollback, Start at the
title) to reproduce, then bisect with `build\release_2145`, `build\clang_1415`, `build\release_0159`.
**13:00-13:45 tests on this PC (user's go-ahead):** online smoke on E234A217: hooks patched once each, armed/disarmed,
no crash (the script printed FAIL only because its commands were queued until the first tick and then ran twice with
mine). **menu_repro never left the title screen, in any run ever:** SendInput keys do not reach MK9's menus (as
reverse_engineering.md already said for gameplay) and the title's prompt is the space bar; every "menu walk passed"
was the title and its attract demos. `desk.ps1` now has `PostKey` (WM_KEYDOWN/WM_KEYUP posted to the game window,
which the menus read) and menu_repro uses it (Space/Enter through the title; J picks the keyboard at "press any button
on the controller you want to use"; Backspace is back) and stops when the game exits. Rollback mode on E234A217: 7
level changes (title <-> attract demo) and the main menu, Fight > 1 Player and Training submenus: no crash. **ONLINE and
mode entries (Training > Practice) cannot be reached with the keyboard:** they ask for the controller again and the
keyboard choice returns to the main menu (screenshots in the session scratchpad). **A normal Quit ends the logs exactly
like the players' "crashes":** "PHYS destroy ... 004A3F40" + "PHYS term bodies ... 006DD815" (UE3 tears the world down
on exit) and the frames log flushed by the process-exit detach. So the players' games most likely exited by MK9's own
exit path (requested exit or a fatal error), on both PCs at the same moment, as they entered Online.
**Diagnostics build (13:40, DLL 8271AE59…):** the process-exit detach logs "PROCESS EXIT on thread N (game thread M),
crumb; stack code values" (the thread that called ExitProcess, scanned to its stack base), and the vectored handler
logs the first 8 other first-chance exceptions ("EXCEPTION (first chance, passed on) code=..." with stack code values;
C++ throws, divide/FP faults; thread naming, debug output and RPC codes skipped). Repro needs a gamepad: rollback
ticked, main menu > Online, press a pad button at the controller prompt.
**Gamepad repro on 8271AE59, rollback mode (user, 16:42-16:44, this PC; the game had idled 3 h in title/attract
cycles first: ~60 level changes, 0 exceptions):** attract -> title -> Online (16:43:07, native 1156 at gf 21 as in the
Safe run's online entry) -> two more level changes (online char-select natives 459/66/67/455/453/1186, then its end
1375/1190/1386) -> a Liu Kang vs Sub-Zero fight (mode 2) still running 15 min later. **No exit, no exception**; every
teardown went on past "term bodies". So entering Online in rollback mode does not by itself end the game. Open: what
the players saw at 11:03 and 12:19 (both games ended within ~1 s of each other ~30 s after a simultaneous start; a
clean exit, as if closed) and whether 1208/0159's code differs there (8271AE59 adds only the disengage and logging).
**The user then reported the test game "froze in a black screen" (still running, after an alt-tab; 12:19 was a black
screen too).** Game thread ticking, render thread idle in its command loop (0x0063CB8C), window restored and in front:
black. Screenshot tests (scratchpad `alttab_test.ps1`, brightness of the game window): **fights are drawn almost black
in rollback mode, with no alt-tab at all** (attract demo, brightness 0 at +5/+10/+15 s, faint silhouettes), on
8271AE59 and on 2145's DLL alike; normal with the DLL loaded passively (no commands) and in Safe mode; minimize/restore
recovers in every other case (no mod at the menu/logos/title, Safe at the logos, rollback at the menu/logos). Toggling
lockstep during a dark fight does not bring it back; switching rollback mode on after the fight had started keeps the
picture. So the damage is done by rollback mode's per-tick work during the fight's load and intro (saves from the
first fighter, gf ~140, and the four restore audits; HeapAudit spins on every MK heap's lock and GMalloc's medium
heap's), not by the saves during combat. Which of the two is not isolated.
**Fix (18:27 build, DLL 299FEE5D…):** OnTickEntry snapshots and audits only when a load can follow: a GGPO session, a
pending one, the join wait, or this tick's netplay start (frame F's snapshot); `idlesaves 1` restores the every-tick
behaviour (needed by local `rollback N` tests outside a session). Rollback mode outside a session now does what Safe
mode does in menus and non-netplay fights. **Not verified yet:** its one run had no foreground (the user was at the
PC), so its screenshots are invalid; save cost there was 0.001 ms/frame (the gate works). Then: the rollback attract
fight screenshot, the two-instance regression (netplay at attract fights: the first save is now at the start frame),
online smoke, the menu walk with posted keys.
**Code/log analysis (3 read-only agents, 18:40-19:00) and the 19:02 build (DLL F926D13D…, SHA-256 prefix):**
- Logs, dark vs normal attract fights: the same fade/camera/HUD objects are created on the same ticks; what differs is
  the tick rate during the fight's load wait: dark runs got 200-290 engine ticks at 27-41 ms each, normal runs
  (Safe, passive) ~4200-4800 at ~2.5 ms, in the same wall time. The 18:27 build (no idle saves) still ran at 27 ms
  (116 ticks). So the darkness follows from too few ticks of whatever MK9 warms up per tick during the load (lighting
  or exposure, not identified), which also fits "rollback switched on mid-fight stays bright".
- Save path: no render writes; cost is indirect (lock holding, cache).
- HeapAudit's lock protocol matches the game's plain 0/1 spinlock (test 0, `lock cmpxchg` 0→1, release `mov 0`;
  waiters spin 100x with `pause`, then Sleep(0), forever): holding it can only stall other threads, never fail an
  allocation. The heap pointers never change after 0x887700. Ruled out as corruption.
- The work 18:27 left on every rollback-mode tick: **PhaseAudit** (HeapAudit + ListAudit at FightUpdate entry/exit and
  EngineTick exit, 3x per tick; `gPhaseAudit` defaulted on and no command existed). The "save cost" log line does not
  include it.
- 19:02 fix: PhaseAudit runs only on ticks OnTickEntry snapshots (`gTickAudits`, reset at every OnTickEntry) and in
  replays; new command `phaseaudit 0|1`. Rollback mode outside a session now does no per-tick audit or save at all.
  **Not verified yet.** First check (game in the foreground, nobody at the PC): `alttab_test.ps1 -Mode rollback -At
  fight` brightness, plus the log's ticks during the fight load (should match Safe's ~2.5 ms). If it is still dark,
  the next suspect is a deciding test: Safe mode with an artificial ~30 ms sleep per tick during the load (timing
  alone). Then the regression list above.
- **Verified 19:20 (F926D13D, rollback mode, attract fight, game in front, nobody at the PC): the fight is drawn
  normally** (lit stage and fighters; brightness 18/45/17/22 at +5/+10/+15 s and before the minimize, vs 0 in the dark
  runs; 91 after minimize/restore). Fight load wait (frames log, SKIP sub=1 → sub=8): 4434 engine ticks, vs Safe 5031,
  dark 2145/8271 run 513, 18:27 build 318. Still to do before a players' zip: the two-instance regression, online
  smoke, the menu walk.
- 19:13 `queued_checks.ps1 -Now -Soak 0` on F926D13D: online smoke PASS; both regressions and the menu walk never
  started a game: from ~19:15 Steam's launch stops at a Steam Cloud dialog ("Não foi possível sincronizar": MK9's
  saves could not sync; "Jogar mesmo assim" / "Cancelar"), which launch_game.ps1 does not handle (it timed out; the
  regressions report "only 1 MKKE instance", B being started directly). Local MK9PSET (16:43) is newer than the cloud
  copy (remotecache: localtime 1793475786 > remotetime 1793459881). Saves backed up to
  `results\save_backup_20261031_1930`. Left to the user: the dialog, or Steam Cloud off for MK9 during the checks.
- 19:49-20:29, Steam Cloud off for MK9 (user): `queued_checks.ps1 -Now -Soak 0` on F926D13D: **two_instance_regress
  PASS** (3 fights, 6 checksum reports, 0 mismatches, 0 failed saves, 0 crashes, 0 cut sessions), **two_instance_
  autorearm PASS** (same; each game re-armed itself 3 times), **menu walk PASS** (armed like the launcher, 302 key
  presses, no hang or crash, responding). Its online smoke failed at injection only (the game had not started yet;
  the gate's kill of a running MKKE right before it); rerun alone at 20:29 on a fresh game: **PASS**.
  **`dist\MK9Rollback-netplay-20261031-2029.zip`** (DLL F926D13DB1A340421C2086302F6BC8B62903F045CB40389BE55838B123ED76C4):
  black-fight fix + disconnect fix (char select → fight re-engage) + exit/exception diagnostics. The Safe mode gate
  (`-Lockstep`) was not rerun: lockstep's code is unchanged apart from the online disengage, which only a two-PC
  online match exercises. Next: the two-PC test with the friend, both modes; the user to turn Steam Cloud back on.

**2026-10-31 20:34 user, 2029 zip, rollback ticked, opening a private room: "out of memory", PC locked (Ctrl+Alt+Del
dead, logout needed)** (mk9r-logs-20261031-203636.zip). The mod never engaged (no online-data or hello line; only
ONLINENATIVE 1156/1404/1346 first calls). HANG at 20:34:25, game thread in MK9's out-of-memory handler 0x00406160's
spin (eip 0x00406232): the online manager's tick 0x0081C7C0 ([0xE924F0]) -> 0x0092DC60 ([+0x3C], calls vt+8 of each
object in its [+0x50]/[+0x54] array, 0x0092DDB5) -> ... -> 0x0092D7F0 (the library's alloc through a function table)
-> 0x0081A4B0 (MK heap #5 [0xED5400]) -> 0x00887520 -> HkHeapAlloc -> 0x0042B390: no free block fits, the grow through
[heap+0x18] vt+0x14 fails, [heap+0x1D] set -> 0x00406160(a1, growSize, heap, 3, 1) shows "out of memory" and loops
forever while [0xDE110C] != 0. MKHEAPS 7 s before: #5 70/1024 KB used, and no "near full" line, so the request was
large (hundreds of KB) or garbage. Same heap and path as the 10-29 18:41 freeze; the 11:40 crash today (Safe mode, also
opening a private room) died in the same loop 0x0092DD90 reading an object with no vtable (+ R6025 on the render
thread). So: MK9's online menu code, three times, in both modes, with the mod loaded but not engaged. Whether vanilla
MK9 does this is unknown (control test: private rooms with MK9 started from Steam without the mod). MKKE is LAA (4 GB);
address space 1.39 GB, so not address-space exhaustion.
**20:46 build (DLL E6D1862A…, not yet run):**
- 0x00406160 hooked at init (any mode, passive too): logs "OUT OF MEMORY" with the args, the MK heap (#, used/reserved,
  grow chunk, largest free block from the free tree's +0x10 side), this thread's last MK heap request (HkHeapAlloc ->
  CrashLog::NoteAlloc: bytes, heap, caller) and the stack; when MK9 would spin (spin arg != 0 and [0xDE110C]) it
  closes the game (TerminateProcess 0xC0000017) instead of freezing the PC; otherwise MK9's handler runs.
- PurecallHandler closes the game (0xC0000409) after its log instead of the CRT's R6025 dialog (the 11:40 freeze).
- "MKHEAP large request" + stack for sub-heap requests >= 256 KB (first 20).
- `oomtest`: 64 MB from 0x0081A4B0 on the game thread; must log OUT OF MEMORY and close the game. Needs the PC.

**2026-10-31 20:46 two-PC test on 2029 (rollback ticked both; user mk9r-logs-20261031-204654.zip, friend 204733):
"the match disconnected after loading the stage".** Invited room (type 0). Char select's online phase active
20:46:12.06/10.48 (user/friend clocks), process-memory passes found each other, hellos met, both **engaged at the
char select** (16.51/14.99), the phase closed normally 2.1 s later (WATCH: +0x18 cleared at 0x0087FBBE by a script
native), and the 12:35 change disengaged both before the load, as designed. The load (state 1) then: identical MK
objects created on both (frames-log type counts equal), both ticking normally (user 2.5 ms/tick, friend ~4.5), but
**the friend's load script never ran natives 1192/1193 (user: gf 106, exec 1355AA00) nor 1182 ("loaded", user gf
497)**; ~12 s later the user's game ran 1209 and the friend's 1217/1144 (removed), no fight phase on either. The
12:15 success, reread: both mods ran their process-memory passes during the char select too, the user found the
friend and sent hellos, but the friend had not identified the user, so **neither engaged before the fight phase**;
both loads ran 1192/1193 then 1182 at gf ~497, the fight's online phase went active right after 1182, both engaged
there in game state 8 (GameInfo+0x4: 7 = character select, 1 = stage load, 8 = fight), session at the first fight
frame 5 s later. Every failed match (10:59, 11:02, 11:38, 20:46) engaged in state 7. Mechanism unknown (engaged
state at the char select: NetplayArm flags, QUARANTINE on in rollback mode, hellos with engaged=1, SharedSession;
none touches MK9 by code reading).
**Fix (20:54 build, DLL FED477A8…):** OnlineMode engages only in game state 8 ("ONLINE: the other player's
MK9Rollback answered in game state N ...; engaging only in the fight's connection (state 8)", once per phase). The
search and hellos still run during the char select. Not yet tested (oomtest + online smoke, then a two-PC match).
Also noted: both games logged "NETPLAY armed: player 1" (the side is taken from the game at the fight start).
**`dist\MK9Rollback-netplay-20261031-2056.zip`** (DLL FED477A874E3B0C7B62752CD59B72DB1D47791B120ECA02E9BB3140A55AF58BA),
packaged at the user's request **without any run** (the user was playing another game): not even the online smoke
or oomtest ran on this DLL. First launch on the players' PCs is its first run.

**2026-10-31 21:xx: restructure plan written, `docs/restructure_plan.md`** (lean player build first, then decide
MK9 Online vs Local Versus over Steam by evidence, rollback experimental). Supersedes ad-hoc fixes on the online path.
**21:42 Phase 1 built (DLL D9AB5A78…, not run):** `lab 0|1` (default off) gates the diagnostics; `netcode safe|rollback`
installs per-mode patch sets; mode mismatch detected (hello v2, fingerprint); re-sent Enable no longer clears the ring
or re-anchors the script clock. Details and the gate commands: restructure_plan.md "Status". Needs the Safe-mode and
rollback release gates (PC, ~90 min) before a zip.
**2026-11-01 01:54: both gates PASS on D9AB5A78** (Safe: smoke, regression, auto re-arm, menu walk, lockstep_ns60, peer
loss; rollback: smoke, regression, auto re-arm, menu walk). Safe-mode player ≈21 patches, rollback ≈40.
**`dist\MK9Rollback-netplay-20261101-0154.zip`** (DLL D9AB5A7897F76CEBED0744EE092459764CDA23A1C44AD550E1839C122F87D6CC):
lean build + state-8 online gate + OOM/purecall close + mode mismatch checks. Next: two-PC Safe-mode test.
**2026-11-01 ~10:45: code review by five agents, fixes built and checked by a sixth (DLL 141A2396…, not run, no zip).** Details in
restructure_plan.md "Code review, 2026-11-01". Main ones: the virtual script clock no longer runs in menus/loads
(likely cause of the stage-load disconnects), precise lockstep waits, a crashed game closes itself after 6 s instead of
freezing, the launcher's arm command can no longer be deleted at injection, online re-arm/mode-change fixes. **Needs both
release gates before a zip**, then the two-PC Safe-mode test.
**2026-11-01 12:47: both gates PASS on 141A2396** (Safe: smoke, regression, auto re-arm, menu walk, lockstep_ns60, peer
loss; rollback: smoke, regression, auto re-arm, menu walk). No deadman/CRASH/could-not-start lines, and lockstep under
60 ms of simulated lag holds 59.9 fps. Details: restructure_plan.md "Gates run 2026-11-01 11:47-12:47".
**`dist\MK9Rollback-netplay-20261101-1247.zip`** (DLL 141A23960C62142CE12E0687028C450DAC1E6F1A19A013874D277AC342A3717A).
Next: the two-PC Safe-mode test with this zip (3 private-room matches, both players' log zips).
**2026-11-01 14:11 user, 1247 zip, Safe mode: crash in the online menus before any fight** (mk9r-logs-20261101-141131.zip,
user's side only). Injected 14:10:20, level load 14:10:45, crash 14:11:09.8 at gf 1479: game thread read [0+8] at
0x0092DDB2. That is the online library's tick loop (0x0092DC60: for each object in owner+0x50/+0x54, call vt+8). The
object at index 0 (0x128D2EB0, MK heap #5 like its owner 0x128CE580) had a null vtable: freed while still listed. The
render thread's PURECALL followed, MK9's crash handling as after every game-thread crash in the test logs. The mod closed
the game (no R6025 box, no hang). No ONLINE line: the mod never engaged, and Online::Tick does nothing before the fight
data exists. In Safe mode no heap/free/object hook is installed (only Ue3Refs' observing ctor/teardown hooks), so nothing
the mod runs there allocates, frees or delays a free in heap #5. Same address as the 10-31 crashes above (lines on
0x0092DDB2/0x0092DD90) and the same loop as the heap #5 out-of-memory hangs: **3rd crash, 5th event, all in private-room
menus with the mod loaded**. Still open: whether MK9 started from Steam without the mod does this (control test).
User, right after: it crashed **when MK9 opened Steam's friend list (invite) after creating the private room**. That is a
solo repro, so the control test needs no friend: create a room and open the invite list, several times, with MK9 started
from Steam. Outside a session the mod's pause guard and sampler wrapper only call the originals, so its menu-time
footprint is the observing hooks, the 10 s stats, the render-thread status overlay and one FPU read per new thread.
Web: a 2013 Steam thread (app 237110) reports unmodded KE "out of memory" in King of the Hill and private-game invites
failing, the same online code; no report of this exact crash.

**2026-11-01 19:03-19:12 two-PC test on 1247, invited room, no menu crash this time** (user mk9r-logs-20261101-191213.zip
with the Safe run as previous_*, friend 190825 = Safe and 191222 = Rollback with Safe as previous_*). The user's side is
player 2 (Johnny Cage), the friend's player 1 (Jax).
- **Safe match (19:06:07-19:08:09): the first engaged online match played to its end.** Engaged in state 8, lockstep
  delay 3, ping 32-46 ms, 7050 frames matched, 0 mismatches, 0 loads. **Rematch screen stuck ("we both could not press
  yes"):** the session ran on after the match (no timeline restart), so MK9's gamepad exchange stayed skipped
  (7089 skipped) and the pads stayed overridden; MK9's post-match flow never got its exchange. Game state (GameInfo+0x4):
  8 for the whole fight and between rounds, 11 from frame 4634 (gf 105135, both PCs the same frame, life still 0.30 /
  0.67), then 9 exactly 1118 ticks after Combat STOPPED on both PCs (the rematch screen, by timing). At the end screens
  the lockstep "save cost" line (in lockstep the s0-s1 span is the boundary wait, no Save runs) rose from 0.47 to
  9-13 ms/frame: the games no longer ran in step there. The user closed the game at 19:08:06; the friend's stopped with
  "peer disconnected".
  **Fix to make:** stop the session when the state leaves 8/11 during an engaged online session (both PCs reach 9 on the
  same frame in lockstep), give MK9 its exchange and pads back, re-arm for the rematch's fight. Online path only
  testable with two Steam accounts.
- **Rollback match (19:10:18-19:12:14): desync at the user's X-ray.** 5526 frames in sync, then frame 5526 (gf 105527)
  differed: same life/meter/RNG, positions off by ~28 / ~12 units. User's frames log: a 1-frame load at gf 105526
  (remote input mispredicted 0x64 -> 0x60), "ROLLBACK destroy post-snapshot MKSkeletalObj" x2 (created that frame:
  likely the X-ray skeleton view), replay, the desync from the next checked frame. Both logs at the fight start:
  "MOTIONCTL P1/P2 slot 3 holds ... (vt 00D5A150), not the known controller; left live at loads": prime suspect (motion
  not rewound by the 1-frame load). On the friend's side the X-ray killed Jax at once (STOPPED gf 105557); on the
  user's later. Both games exited normally (PROCESS EXIT on the game thread, no CRASH line) ~20 s later, the user's
  first (19:12:11). Also: from 19:11:21 (user) / 19:11:48 (friend) MK9's own connection got no packets from the
  opponent for 20+ s while the session ran (hidden from MK9 by the idle hook), each time after the other side's loads;
  in Safe mode no gap at all. Suspect: a load rewinding part of MK9's online send state.
- **Feel:** "ping good, could be better". At 32-46 ms RTT (one-way ~16-23 ms) Safe mode's 3-frame delay (50 ms) has
  about a frame to spare: delay 2 (33 ms) would fit most frames. Idea: pick the delay from the RTT measured during the
  char select (hellos), agreed as the larger of the two proposals.

**2026-11-01 ~19:30 user: "the default will be with rollback, focus on the lag, ping, rollback... as smooth as possible"
(SF6 as the reference). Built 19:33 (not run):**
- **Rollback is the launcher's default**; "Safe mode" is now the opt-in box (settings file: an explicit `safe` line).
  README rewritten.
- **Match end:** OnlineMode ends an engaged running session when GameInfo+0x4 is 9 (results screen; checked at the top
  of OnTickEntry, before the frame boundary, so both lockstep games stop on the same frame), as `Disengage("match
  over")` without marking the match failed, with the idle-time grace (now independent of gPeer). The rematch's fight
  engages again at its state 8. 'online auto' re-armed while MK9's fight data is active keeps the opponent (the memory
  pass runs once per match). GgpoStop treats "match over" as a normal end. Launcher: "Match over..." message.
- **Input delay from the ping** (`online auto ping`, the launcher's command in both modes; `online auto N` still fixed):
  hello v3 (28 bytes: lockstep, delay proposal, final flag, sender clock, echo of the other's clock and the time it was
  held) gives a round trip per hello read (QPC ms; includes both games' read latency, as the session does). A game
  proposes after 8 round trips, or at the fight's connection with what it has (fewer than 4: the old fixed delay, Safe
  3 / rollback 2); estimate = second-highest of the last 16. Safe mode: 2 while RTT+10 <= 66 ms, else 3. Rollback (keep
  a replay to about a frame): 2 up to 100 ms, 3 up to 133 ms, else 4. Engaging waits for both proposals to be final and
  takes the larger ("input delay N agreed" line and launcher message); the mode check compares only Safe/Rollback.
  Older (v2) hellos are refused with the version message.
- online_smoke.ps1 also arms `online auto ping`.
- Not testable on one PC (needs two Steam accounts): the results-screen stop, MK9's exchange resuming after the
  session, the rematch re-engage, and the delay agreement.
- **Found while doing it: rollback loads rewind MK9's own online connection.** The connection (0x11AA7710) and its
  gamepad data (0x11AAC34C) lie in MK heap #5 [0xED5400] ([0x119AFFD0, +1 MB) in the user's 19:08 log), and
  MKHeap::MasterRanges keeps the sub-heaps in the master image (kExcludeSubHeaps = false since soak4: MK objects free
  into #5 too). Every load therefore restores the connection's bytes to the snapshot frame. That fits the 19:11 packet
  gaps (rollback only, each starting after that side's loads; none in Safe mode) and means MK9's connection is likely
  unusable after a rollback session: the results-screen stop will probably still end in MK9 dropping the match in
  rollback mode. Options: keep the connection block (gamepads - 0x4C3C .. + 0x240) live across loads (safe only if
  the online library allocates/frees nothing it points to inside the rewound window: check), or keep MK9's online
  library from changing state during a session (its tick at 0x0081C7C0 -> 0x0092DC60), or exclude #5 with a plan for
  the MK objects that free into it. Needs a soak plus a two-PC match.
- **X-ray desync, investigated (agent + checked in MKKE.exe):** each fighter's Actor.MotionControl has 4 slots
  (+0x13C); the dispatcher 0x00786320 runs only the lowest non-empty slot (vt[2](dt)); the setter 0x00786200 deletes the
  old controller (vt[0](1)), stores, and refreshes [actor+0x1C8]+0x20..0x2C from the lowest slot's vt[3] (0x00786290:
  the same without the delete). RestoreMotionCtl handles only class 0x00D5A85C. The others are stateless (vt[0] =
  0x0052EC10 for all four, read from the exe): 0x00C66E40 in slot 1 is the **cinematic controller** (installed by the
  cinematic event track 0x004BB0F8..0x004BB1C2 with SetBase(NULL), Physics 9, bIgnoreForDesyncs; moves the fighter via
  0x006C5960), 0x00D58B08/0x00D58B18 in slot 2 (SetPhysics 0x007940C0), 0x00D5A150 in slot 3 (root motion of the mesh
  component, already covered by fightercomp: **not** the cause). The 10-line MOTIONCTL cap was spent at the fight start,
  so the logs could not show slot 1 at the X-ray. f10 went 000C0001 -> 000C4001 at gf 105526 (the X-ray start = the
  load frame). **Not fixed by restoring slot 1 alone:** the cinematic objects (SeqAct_Cinema, CinemaGroupInst*,
  CinemaTrackInst*) are not snapshotted at all (load_refusals.md section 3), so a replay across an X-ray start runs
  against a cinematic that already started; with slot 1 put back to empty, the cinematic would likely not install its
  controller again. Fix = restore both together (load_refusals.md option 2: the MwyCinema instance graph as Tier E-style
  UObject spans, the spawned-actor destroy at 0x4BB852 deferred like ActorDefer, plus slots 1/2 by pointer with their
  blocks in the quarantine). Needs a SyncTest soak with X-rays (Jax vs Johnny Cage) and a two-PC match.
- **19:42 build (DLL CFBBA4C7, not run):** MOTIONCTL logs each class change per slot (cap 60) and every load that leaves
  another class in place ("differs from the save ... left live", cap 40), so the next desync can be matched to a
  cinematic.
- **User: "Fix X-rays first" (before the release gates). 20:04 build (DLL 56F52672, not run), lab-only probe:**
  `cinewatch 1` (needs `lab 1`; Snapshot::CineWatch from FightTickDone, live ticks): every UE3 object whose class name
  contains "Cinema" from GObjObjects, frames log "CINE gf=N BIRTH/DEATH/CHANGE" (64-byte chunk hashes of the body from
  +0x3C) and each fighter's MotionControl slots with vtables, +0x1C8, Physics +0x58, Base +0x60, +0xA8 (bIgnoreForDesyncs
  word), CinemaControlCount +0x138 on change; `poke <hex addr> <hex dword>` (lab). `tools\xray_probe.ps1 [-Synctest]`:
  attract fight, killai, P1 walked right with full meters (poke E8A990 / E8B61C = 3F800000), tries masks C000, A000,
  E000, 6000 until a CINE BIRTH; -Synctest repeats the X-ray under `ggpo synctest 4`. Results in results\xray_probe\.
  **Next:** run it when the PC is free (the user said "not now" at 20:05), then design the restore from what it shows
  (which cinematic objects are born per X-ray, which change, and the fighter fields), then the SyncTest soak.
- **User (while playing another game, no MK9 launches): "Do 1 and 2". 21:26 build (DLL C69DB0BA, libggpo rebuilt 21:24;
  not run in game; adds `sessiondelay N`):**
  1. **OnlineKeep** (src\rollback\OnlineKeep.cpp): MK heap #5 (MK9's online library, the connection inside it) stays
     live across loads in online sessions, instead of being rewound by the master image. The capture is taken right
     before LoadAB (after PreRestore) and written back right after. Spans other than "mkmaster" inside it are still
     restored as saved. Only when the allocator agrees:
     - no heap #5 allocation or free since the save (counter in the MK heap vt+0x10/vt+4 hooks: FreeQuarantine::Heap5Ops;
       MKKE has no direct call to the free, and its one direct call to the alloc is GMalloc's medium heap), none during
       the save and none during the load;
     - heap #5's header (heapobj span) equal to the saved one apart from its spinlock (also catches a reset, vt+0xC);
     - its memory (registry segments, else root +0x34 / reservation +0x24) clipped to the master ranges and not
       reaching another heap's first chunk;
     - MK9's connection inside it.
     Logs: "ONLINEKEEP S=..: ... kept live: the restore had changed N dword(s), M in the connection ... outside it
     a..b: samples" (first 12 loads per session, then every 600th), the first refusal per reason, and a session summary
     at GgpoStop. `onlinekeep 0|1|2` (2 = every session, lab: SyncTest it offline). **To verify:** the session summary
     says how often it kept (if MK9's library allocates in #5 every frame, it never keeps and the log says so); dwords
     changed outside the connection would be other heap #5 state kept live (fight state there = desync risk): check the
     samples, and run `onlinekeep 2` under SyncTest offline.
  2. **Delay between rounds:** hello v3's spare byte carries each game's live proposal (DelayFromPing on the latest
     16 round trips; going down only with 8 ms to spare). While a session runs, once per stretch of 30+ ticks with both
     fighters' input locked (PlayerInfo+0x944: round intros, after a KO), the session takes the larger live proposal
     (Rollback::SetSessionDelay -> ggpo_set_frame_delay; "ONLINE: input delay a -> b between rounds", launcher message).
     GGPO patch (build_ggpo.ps1): Peer2PeerBackend::AddLocalInput sends the frames a delay increase pads (copies of the
     last input) before the new input; stock GGPO sent only the new frame, and the remote reads consecutive frames
     from start_frame, so every later input would have been read one frame off. A decrease drops one local input per
     frame (nothing sent, no gap). The results-screen stop also resets the proposal, so a rematch proposes from the
     latest round trips. The match-over line counts the changes and locked stretches. **To verify:** whether both
     inputs are really locked between rounds (stretch count > 0), then a two-PC match. Locally first: two_instance with
     `sessiondelay N` sent to one game mid-fight (up and down; "GGPO p2p frame delay a -> b"), checksums must agree.
- **Read-only review by three agents (user: "find any bug or performance improvement while I'm playing"), checked in
  the code, fixed in the 22:31 build (DLL 688133FF, libggpo unchanged since 21:24; not run in game):**
  - OnlineKeep: (1) the copy and write-back ran outside LoadAB's heap locks (another thread's heap #5 alloc could
    interleave): both now run inside LoadAB under HeapLocks. (2) The header fallback range [root, root+reservation)
    assumed the free-tree root is the region start; now the root chunk must hold half the reservation. (3) Keeping the
    whole heap #5 is not proven safe for its contents (MK fight objects were seen in #5, soak4: kept live they would
    desync replays, or point at rewound master blocks). **Default is now mode 1 = measure only** (first 12 loads per
    online session, then every 600th: dwords the restore changed inside/outside the connection, with samples);
    `onlinekeep 2` keeps live online, `3` every session (lab). The GGPO padding patch was checked correct (increase,
    decrease, decrease-then-increase, first input, lockstep, resends, capacity, receiver asserts).
  - SetSessionDelay uses the session's own local player and delay (gGgpoLocal/gGgpoDelay from GgpoStartP2P), not gP2P,
    which commands can rewrite; Rollback::SessionDelay() for AdaptDelay and the match-over line.
  - Online mode (hello v4, so both players need this build):
    - a 'ready' byte (own engage checks pass): a game engages only when the other is ready or engaged, and drops a
      wait at the fight start when the other says neither for 3 s ("the other player's game is not starting the
      session"); before, one game could engage alone (e.g. a latched mode mismatch on one side) and skip MK9's exchange
      until MK9 dropped the match;
    - any session stop other than a timeline restart now disengages with a failure (OnSessionStop): "no synchronized
      input", a busy script VM, environment / frame-0 mismatch and others left netplay armed, the game engaged, and the
      next fight started a session nobody joined;
    - the failure is cleared at the results screen (the rematch tries again); a mode mismatch is no longer latched
      (re-checked, logged once per match);
    - the match-over and failure checks run before the Steam session check (a stale Steam state no longer delays the
      results-screen stop);
    - a new connection object = a new match (MatchReset), and the 20 s match end clears the round trips (the next
      match measured with the last one's pings before);
    - `online off` after a session keeps the 15 s idle grace; gSessionDelay reset at disengage;
    - launcher messages for a Safe/Rollback mismatch and for a different mod version.
  - **Input latency: one frame less.** The local input was read at EngineTick entry (RecordInputs) and sent at the next
    boundary; the sampler (0x404E26) runs right before each boundary, so the boundary now reads LocalHardwareMask itself
    (scripted harness input unchanged). Both modes, no determinism impact (GGPO carries the input).
  - **Time sync:** GGPO's frames-ahead was slept in one piece (Sleep(n*16) = 62-78 ms freeze); now one 16 ms wait every
    20th frame until paid; events counted in the "GGPO p2p stopped" line.
  - **Not done, need measurements in the game (from the performance agent, verified where noted):**
    - every rollback frame saves F twice (the live save at OnTickEntry is replaced by the replay's save of F): ~3.6 ms
      on the frames that roll back; move the live save after the boundary's ggpo_idle when nothing loaded (verified);
    - FreeQuarantine::OnLoad's requeue scan reads every restored dword incl. the 11.8 MB master image: ~3 ms per load
      (ALLOCLEAK lines); SIMD filter or fold into LoadAB's copy;
    - replayed frames are probably drawn/presented (overlay draws ~ replayed frames; ~15-20 ms unexplained per replayed
      frame): time gWorldTick/gTick in ResimFrame and count presents; if so, skip the viewport draw in replays (SyncTest
      to validate);
    - per-save costs (ue3cells 0.6 ms as one span per node, skel three object-list walks with strstr, ~10 script modules
      copied whole), quarantine release walk under its lock every frame, synchronous log flushes on the game thread;
    - smaller saves: skip saves of replayed frames with confirmed remote input, or save every other frame.
- **Queued for when the user is back (they said yes; no MK9 launch before they say so).** `two_instance.ps1 -DelayChange`
  (7 `sessiondelay` changes per netplay fight on both games, 0..4 frames, a jump of 3 included) is now gate step 2a in
  queued_checks.ps1 (pass = the plain regression's criteria plus at least 12 "GGPO p2p frame delay" lines). Order:
  1. `tools\queued_checks.ps1 -Now -Soak 0` (rollback gate: online smoke, regression, auto re-arm, delay changes, menu
     walk; zip on a pass);
  2. `tools\queued_checks.ps1 -Now -Lockstep -Soak 0` (Safe mode gate, simulated lag and peer loss included);
  3. `tools\xray_probe.ps1 -Synctest` (what an X-ray's cinematic creates; then the X-ray restore design);
  4. optional: `tools\synctest_soak.ps1 -Sessions 2 -Minutes 13 -Setup 'lab 1','onlinekeep 3'` (heap #5 kept live under
     SyncTest offline: sync errors would mean fight state lives there);
  then a two-PC match with the new zip (both players: hello v4), and its logs: ONLINEKEEP measurements, the locked
  stretches / delay changes in the match-over line, time-sync events in the session stop lines.
- **2026-11-02 run (user back, "I'm back, start"):**
  - Rollback gate PASS (online smoke, regression, auto re-arm, delay changes: 21 applied, 0 mismatching frames; menu
    walk): `dist\MK9Rollback-netplay-20261102-0207.zip`. Safe mode gate PASS (delay changes 20, ns60, peer loss):
    `dist\MK9Rollback-netplay-20261102-0253.zip`. Same DLL 688133FF. Time-sync events: 2 per session, paid in full.
  - **X-ray probe (4 runs; the first three were script bugs: walking away, PowerShell's case-insensitive $dir = $Dir,
    a 300-line log window, a false 'slots changed' signal).** Results\xray_probe\:
    - **X-ray = mask 0xC000 (Block 0x8000 + 0x4000): 0x4000 is Flip Stance** (docs/rea_pass1.md 3.4 listed it as a
      candidate). 0x9000 used no meter. The meter at PlayerInfo+0xC14 is 0..1 (1.0 = full); the X-ray empties it.
      STATE life did not move even for plain attacks at gap ~99 in these runs: not usable as a hit signal here.
    - Live X-ray at gf 7863: **both fighters' slot 2** get class 0x00D58B08 (not slot 1's 0x00C66E40), Physics 7,
      CinemaControlCount 1; 50 cinematic objects born over ~190 frames: 14 CinemaTrackInstMove, 7
      CinemaGroupInstSpawnedEmitter, 6 CinemaGroupInstSpawnedActor, 4 BlendAnim, 3 MaterialOverride, 3 Attachment,
      2 each Physics / CharacterSound / HideActor / IK / ApplyCharDamage, FocalLength, CameraShakeControl, BgndPostFx.
    - Under SyncTest (check distance 4): MOTIONCTL "P1/P2 slot 2 holds ... (vt 00D58B08) ... left live at loads", the
      first GGPO SYNC ERROR at the X-ray's frame (144), then errors on every check (16); no cinematic objects born in
      the replayed X-ray. **Fix design input:** slot 2 (both fighters), Physics, CinemaControlCount and the
      cinematic instance graph must be restored together (load_refusals.md option 2), with the spawned actors /
      emitters' destruction deferred like ActorDefer.
  - **Heap #5 kept live offline (`onlinekeep 3`, SyncTest soak, seeds 13101/13102, results\synctest_onlinekeep\):
    0 sync errors in both sessions** (the 2026-10-30 baseline had 4 and 8 for the same seeds), 0 crash lines; 2,442
    loads, every one passed the heap checks (no heap #5 allocation or free during offline fights) and was kept live.
    The restore changed ~3 dwords per load there: a counter at 0x111AFC38 (+4 per 4 frames), a small value at
    0x111AFD54, and 0x111A1640 / 0x111AC2CC that look like millisecond tick values; none affect the gameplay checksum.
    So the soak4-era worry (fight state in #5) does not show offline. Still unknown online: whether MK9's online
    library allocates in #5 during a fight (the keep then refuses, logged), and what it changes there. The 0207/0253
    zips measure (mode 1); turning the keep on by default after the next two-PC match's ONLINEKEEP lines needs a
    rebuild and the gates again.
- **User: "Yes, start it" (the X-ray fix). 07:44 build (DLL CBCA4196, not run in game):**
  - **CinemaRestore** (src\rollback\CinemaRestore.cpp): every live UObject whose class name contains "Cinema" (plus
    InterpGroupInst/InterpTrackInst whose Outer chain is a cinematic; no actors, no Default__ objects) is saved per
    frame from +0x3C to PropertiesSize, raw (~570 objects, ~108 KB at a fight start). The set is kept by diffing
    GObjObjects against its last copy each save (only changed slots are classified). Runtime classes (names with
    "Inst", SeqAct_Cinema, MKCinemaSequence) also save their TArray blocks (ArrayProperty offsets from reflection, blocks
    up to 4 KB) and register object + blocks with the quarantine. At a load (after the quarantine's OnLoad):
    abandoned-timeline instances are left to the GC after the actors their spawned-actor groups own are destroyed
    (+0x3C actor / +0x88 bit 0, 0x004BB820); actors spawned after S by older groups too; every saved object gets its
    bytes and arrays back, all or nothing (one garbage-collected = no restore, logged). The teardown's DestroyActor
    (call 0x004BB852) is deferred in fights like ActorDefer (released with it; a load gives the actor back when its
    restored group owns it again, else destroys it). Logs "CINEMA class ...", "CINEMA load S=...", "CINEDEFER ...".
    `cinema 0|1`, `abtest cinema`.
  - **Fighter slots:** RecordMotionCtl/RestoreMotionCtl now also restore the cinematic controllers 0x00C66E40 /
    0x00D58B08 / 0x00D58B18 (stateless, <= 16 bytes, blocks in the quarantine): a future one is deleted through its
    vt[0], the saved one put back, then the setter's refresh 0x00786290 on the lowest slot. `abtest cineslots`,
    `abtest motionrefresh`.
  - **To verify:** `tools\xray_probe.ps1 -Synctest` (before: 16 sync errors from the X-ray frame on), then a SyncTest
    soak, the regression gates, and an X-ray in a two-PC match.
  - **Probe 1 (CBCA4196, user: "Yes, run it"): crash on the first load across the X-ray start** (S=6349: 301 objects
    restored, 105 born after S, 18 actors destroyed), in the replayed EngineTick: eip 0x0041166C reading [0+0x38].
    Path: motion tick 0x007A8BE1 -> slots dispatcher 0x007853F0 -> controller vt[2] 0x007854E0 -> actor physics
    0x0072D960 -> 0x00411600, which walks Actor.LatentActions (+0x1A0/+0x1A4), casts each to the sequence class
    ([0x00E58EB0], cast 0x0040EE90) and asks it for the group naming the actor (0x00795E10: GroupInst +0x104/+0x108,
    group +0x3C actor, skips bDeleteMe actors), then dereferences the result unchecked. The actor was a
    PointLightMovable (a stage light the X-ray drives, not spawned by it) still linked to SeqAct_Cinema, whose
    restored GroupInst did not name it. (DestroyActor 0x0079D960: returns 0 for bStatic/bNoDelete 0x11, 1 when
    already bDeleteMe; +0x194 GeneratedEvents get a destroyed notice.)
  - **09:06 build (C93E22E0): interpolation links.** CinemaRestore also tracks every actor's birth (the first save that
    saw it). After the byte restore, an actor whose LatentActions names a tracked sequence of that class whose
    restored GroupInst does not name it is destroyed if born after S, else the entry is removed. Diagnostic: restored
    groups naming a live actor whose list lacks the sequence ("missing").
  - **Probe 2 (C93E22E0): no crash, 2 sync errors (was 16).** S=3739 (X-ray started at 3741-3742): links removed from
    both MKPawns, a SkeletalMeshActor, a PointLightMovable and a DirectionalLight (all pre-existing). The 2 errors
    (frame 52, from that load) are the known hit-test / spawn class (VMTRACE: original `(1, 0xCE)` ctor vs replay
    `tickgate` on an MK object 1100EC00 of an actor based on that SkeletalMeshActor; `runscript` vs `wait`, native
    0x8B6DA0): the replay did not start the X-ray by 3743. S=3743 (mid X-ray): 385 objects restored, no error.
    S=3747: 86 of 471 saved objects garbage-collected (the S=3739 orphans, still saved) -> nothing restored. The
    session ended 57 frames into the X-ray (SyncTest ran ~7 frames/s with cinewatch on).
  - **09:14 build (31EB5AEF):** orphans (born after S, unrestored) leave the tracked set at the load; a load with
    collected objects restores the others unless their saved bytes name a collected one. Probe: `cinewatch 0` during
    SyncTest and the session runs 600 frames past the press (or 4 minutes).
  - **Probe 3 (31EB5AEF): 193 loads over the whole X-ray, 1 sync error, 0 crashes.** The error: S=3485, X-ray start;
    native 0x808C3870 answered 1 in the original and 0 in the replay (the same first difference as the 03:13 probe
    before CinemaRestore). 0x004B4660 = "level ready": the owner's +0x28/+0x2C object's outermost package; found in
    the visibility tree (GWorld +0x1B4, 0x004ADD20) with record +0x5C bit 0 clear -> 1; found with the bit set ->
    0x004B479D: 0 if GWorld->+0x48->+0x38->+0x328 (cast 0x0040EAA0) reports vt+0x298 and +0x1CC != 0, else 1; not
    in the tree -> a GObjObjects scan for that name with +0x5C bit 0 clear -> 1. Also: the periodic collection
    (every ~200 frames) took 18 saved objects each time and "the others name them" refused 4 loads.
  - **09:22 build (A4DED981), probe 4: crash** at resim EngineTick(9584), eip 0x3CF20000: SeqAct_Cinema GroupInst
    walk 0x007A0D30 (vt+0xC4 on each group) on InterpGroupInst_17, freed at 9560 (the X-ray end, ~300 frames after
    its start). That build counted a saved array dword equal to the live one as no reference; the emptied GroupInst
    keeps its old pointers in the slack, so a restore of the length made freed groups live again. LEVELVIS: the
    X-ray adds a record (+0x5C 0x21) after S; it stays in the replay (the bit put-back only touches records of S).
  - **09:32 build:** arrays are checked by their saved length (slack ignored, no live-equality exemption), and
    object arrays (Inner +0x78 ObjectProperty/ComponentProperty/ClassProperty, ElementSize 4) must name live
    UObjects only, else nothing is restored. Level-ready oracle: the call at 0x008C388D goes through
    MK9R_LevelReadyImpl; first-pass answers are kept per frame (8 per frame, ring 12) and replayed frames get them in
    call order ("LEVELREADY" lines; `abtest levelready`).
  - **Probe 7 (4F2FD0AF): 187 loads, 0 crashes, 3 errors, all from the X-ray-start load** (S=4142). LEVELREADY
    replaced the replay's 0 with the first pass's 1, but at S+1 the replay activated the X-ray's SeqAct_Cinema from
    the Kismet op loop (0x0073ABB0 -> 0x0073A6E9 -> 0x00417B96 -> 0x004B2A30, MK object type 0xA4) one event early.
    Other loads refused when a SeqAct_Cinema's saved GroupInst named groups collected since (correct; ~every 200 frames).
  - **Probe 8 (2F7D0713, Kismet containers restored): 174 loads, 2 errors at the X-ray start (S=4254).** KISMET:
    Skarlet_XRAY's op stack 0 at the save, 1 live (the abandoned timeline's activation), now restored; Main_Sequence
    stacks differ at ordinary loads too (3 vs 2, 1 vs 0). The replay now started the X-ray late: at S+2 the original
    ran 21 VM events and the replay 12 (no events after #11, no extra wait), all 860 native results equal.
  - **Probe 9 (51CB7D14, every Kismet object restored except stage Matinee; reflected object fields must name live
    objects): same 2 errors at the X-ray start (S=5823)**, plus 4 at a round change (gf 6359, 7 frames after "FIGHT
    at gf=6352": replay ctor (2, 8) from the resimulated engine tick, original wait; probably the known round-change
    artifact, not checked). The missing event at S+2 is the "enter cinematic" native 0x00893600 -> 0x004554C0 (char
    +0x1D0 |= 0x20000, +0x1D4 = 0x4203, then 0x0045558F pauses a VM through 0x004583A0): the attacker's X-ray VM
    yields one frame longer in the replay. Next: a trace of that VM's wait state at S+1/S+2 (VMTRACE records waits
    0x00458320 / add-wait 0x004583F0; none logged), and the X-ray level's visibility action (SeqAct_MKSetLevelVisibility,
    a Kismet latent op) whose completion depends on unrestored streaming state.
  - **User: "Yes, keep going" (11:00).** `kismettrace N` (command) logs to the frames log, for N frames, every traced
    Kismet object's dwords (and array contents) that changed since the same pass's previous save, the visibility
    tree records and the level-ready answers, tagged O (first pass) / R (replay). Probe: `-KismetTrace N` (from the
    press); the SyncTest fight is retried when the X-ray whiffs (run 10) or the session stops at its start (run 5).
  - **Probe 12 (trace): the cause of the late start.** S=5939: both passes ask level-ready at 5940 (1). At 5941 the
    first pass's world tree gains the X-ray level's node (+0x5C 0x21) and in the same frame the X-ray level's Kismet is
    set up (SeqEvent_Console_1 / SeqEvent_SequenceActivated_2 +0xAC arrays emptied, SeqAct_Cinema_1's link arrays
    rebuilt); at 5942 SequenceActivated fires (TriggerCount 1, ActivationTime), the cinema activates (+0x80 6>7,
    ActivateCount, GroupInst), SeqVar_Objects get the X-ray actors (MKSeqAct_SpawnXRayActors). The replay found the
    abandoned timeline's node (0x00888440 = find-or-add: 0x004ADD20 lookup at 0x00888484; add path makes the record
    and a 0x14-byte node), no setup ran, and only an input impulse on QuanChi_XRAY's link reached it at 5942: one
    frame late. The node and the level being in the world are engine state outside every restore.
  - **Probe 13 (770F1C5E, `levelundo`): worse.** At the load, nodes added after S were taken out with the game's own
    teardown 0x004B3860 (stand-in request, keep mode +0x38 = 1); no crash, level-ready then agreed by itself, but the
    replay put both fighters in PHYS_Interpolating (+0x58 = 7) at S+1, two frames early: 4 errors. **Off by default
    (182AE56A)**, `abtest levelundo` kept. Load requests: native 0x008C35D0 -> ctor 0x004B3E30 (MK object, vtable
    0x00C6C000, 0x94 bytes: +0x38 keep flag, +0x44, +0x50 node); its teardown 0x004B4AE0 calls 0x004B3860 per level.
  - **State at 182AE56A:** X-ray under SyncTest: no crashes in 8 X-ray runs, mid-X-ray loads clean, a load across the
    start: 1-2 errors (the level/Kismet setup timing above). Save cost now logged ("CINEMA save: ... ms").
  - **Soak 14001-14004 (182AE56A, 4 x 13 min SyncTest, `abtest kismet`): the Kismet restore does not hurt.** ON
    1,907 loads 4 errors, OFF 1,882 loads 11 errors (`results\soak_14001`). Every load blocked by a collection (43,
    about one per 200 frames in every fight) was a stage's looping SeqAct_Cinema whose saved GroupInst named the
    previous loop's groups. **Crash in 14002** at 0x004CCE5D: MK blend collapse 0x004B4C20 used the handle lookup's
    (0x004CBFB0) result unchecked at 0x004B4C6D -> 0x004CCE50 with ecx 0.
  - **969A436E:** (1) blend guard: 0x004B4C6D goes through MK9R_HkBlendCollapse (null -> skip to 0x004B4CE2, counted,
    "BLENDGUARD" in the save log); not yet seen to fire. (2) Prewarm: fight ticks without a snapshot run the object
    scan's classification (Track(0) every 30 calls) so the first save does not pay it. (3) Partial restore: a load
    across a collection left the blocked objects and their cinematic (Outer chain, GroupInst members) live and
    restored the rest.
  - **Probe 14 (969A436E): crash** 48 frames after the first partial restore (S=9126, 1 object left live), right
    after `ggpo off`: C0000096 on a VCOMP90 worker (GC traversal 0x005F95CA, vt+0x94) on 3F022580, a UObject freed at
    9126 (appFree 0x0081A470) and released by the quarantine at 9138. Restored state presumably kept a reference the
    checks do not see (struct arrays, untracked objects). **Partial restore off by default** (`abtest cinepartial`):
    a load across such a collection restores nothing, as before. Blend guard and prewarm kept.
  - **Probe 15 (56BD0371, partial restore off; after a PC restart, probe 14's game had stayed as a zombie holding
    the DLL): no crash.** 177 loads over 611 frames past the press (the X-ray and its end), 3 sync-error reports, all
    for one frame at the X-ray start (S=5585, LEVELREADY replaced 0 with 1 there); one load refused (S=5545, a
    stage SeqAct_Cinema's GroupInst naming collected groups: "nothing restored"). Save cost 0.13 ms average.
  - **Soak 14101-14104 (56BD0371, 4 x 13 min SyncTest, every restore on): no crash, all 4 alive at the end.** 4,746
    loads, 18,984 replayed frames, 17 sync-error reports: 16 are one frame per session with the replay ctor (2, 8)
    from 0x00887F17 (the known end-of-fight artifact), 1 the known attached-actor class (seed 14101 gf 4553: P1
    attached actors 7 original, 0 replay; actor +0x80 0x2000 > 0x2040). 75 loads refused by a collection (15-20 per
    session, the stages' looping cinematics), none followed by an error. BLENDGUARD never fired (the 14002 crash
    did not recur; the guard stays unverified). Results: `results\soak_14101`.
  - **Release gate on 56BD0371 (`queued_checks.ps1 -Now -Soak 0`, 18:20): no zip.** Online smoke, auto re-arm (0/6
    mismatching), delay changes (0/6, 20 changes) and the menu walk (306 presses, no hang) pass; **the plain
    regression failed: fight 1 desynced at frame 1142** (2/6 reports), fights 2-3 matched, 0 crashes. Same life and
    positions, P1 meter 0.4994 vs 0.5204 and RNG differ; 4 frames earlier A loaded S=101139 with "ACTORARRAY actor1
    +0x68: count 2 at the save, 1 live (kept live)" and MKATTACH re-attachments: the known attached-actor /
    hit-test class, nothing from the cinematic work near it (no CINEMA load, LEVELREADY or BLENDGUARD there).
    **Repeat (`-RegressRepeat 2`, 18:55): 2 of 2 pass, 0/6 mismatching each.** Old r1-r4 logs (10-28) moved to
    `results\archive_regress_r_20261028`. Safe mode gate (`-Lockstep`) not run.
  - **Zip (user approved after the repeat): `dist\MK9Rollback-netplay-20261102-2043.zip`** (DLL 56BD0371…).
  - **Open:** a two-PC X-ray. The X-ray-start load still gives 1-2 errors (the X-ray level's node and Kismet setup
    are outside every restore). The blend guard is unverified (never fired).

**2026-10-29 18:22: no-codes online (user request: "click on the bat and play with anyone online that will also
launch the game through the bat").** `online auto [delay]` (OnlineMode kAutoPeer): during an online fight the
opponent's SteamID64 is read from the game's connection object (gamepad data - 0x4C3C .. +0x240) or the online
manager's first 8 KB (dwords with high half 0x01100001, not this account) and confirmed with Steam's P2P session
state; the hello handshake and the one-on-one Player/Private check still decide (ranked never). Forgotten when the
match ends. The launcher hides the code boxes in the MK9 Online mode and sends `online auto 2`. Online smoke passes
(arms/disarms auto). **Not yet verified with two real Steam accounts: whether the SteamID is really in those
ranges.** Fallback (18:36): after 2 s without a match, one background pass over the process's committed RW
memory (ReadProcessMemory, 64 KB chunks), candidates confirmed on the game thread; `onlinescan` runs it on demand
(this PC: 659 MB in 1,250 ms, 10 candidates, game unaffected). If nothing is confirmed, MK9's netcode plays.
**Zip for players: `dist\MK9Rollback-netplay-20261029-1837.zip`** (DLL 0589CA20…, includes the `collctor` probe).
The 18:22 build's 3-fight delay-2 regression passed (6/6 reports, 0 mismatches, 0 crashes; the 18:36 changes are
online-auto-only); online smoke passes on 1837. 1822 deleted.

**2026-10-29 first real two-PC online test (user's 18:54 session, 1837 zip, ~11 min of online fights, no freeze):**
`online auto` found the opponent (connection scan: 0 candidates; process-memory pass: 834 MB in 1,875 ms, 12
candidates, <SteamID64> with an active P2P session) and the opponent's mod answered the hello, but every
fight was refused: GameContext type 0 (an invited 1 vs 1 room) failed the type 2/3 check. Fixed 19:17
(type 0/2/3 with match type 0; ranked type 1 still refused). The user had Safe mode on (delay 3 lockstep). Zip with
the fix: see below (not yet smoke-tested: the PC was not free).

**19:18-19:23 builds (not yet tested, queued: `queued_checks.ps1 -MenuRepro`):**
- Free quarantine only while a session is armed or running (per frame at the boundary, "QUARANTINE on/off" lines).
  Evidence: the user's games froze "all the time" with rollback on and played 11 min of online fights in Safe mode
  (lockstep), whose only menu-time difference is the quarantine (actor deferral already needs a GGPO session).
  Suspected mechanism: GMalloc frees on the game thread are deferred 12 frames unless the block lies in the master
  MK heap's region (InMasterImage); if heap #5 [0xED5400] (the allocator of the 18:41 failure, used by menu code
  0x0092D80F) has its own region, its menu churn is held back and fragmented until an allocation fails. The
  MKHEAPS line now gives each heap's used/reserved, base and OUTSIDE-MASTER.
- Status line in online fights on MK9's netcode: "MK9 NETCODE (reason)" (Online::StatusText, overlay kInfo).
- 19:27: pressure valve in HkHeapAlloc (game thread, quarantine on): an MK heap within 10% / 512 KB of its
  reservation releases the whole quarantine first ("QUARANTINE pressure release" lines). The failure itself cannot
  be caught (0x0042B390 calls the never-returning fatal handler internally). Relies on +0x28 = used, +0x24 =
  reserved: check against the MKHEAPS lines of the menu run before trusting it.
- **Runs 19:53-20:38 (user present, said "continue"):**
  - MKHEAPS in the menus: #1 master 11776/12288 KB (its +0x28 counts the sub-heaps' reservations, so the valve now
    skips the master), #2 ~900/1536, #3 not set up, #4 0/1024, **#5 70/1024 KB, inside the master region** (so its
    frees are never deferred by the GMalloc hooks: the "outside the master" theory was wrong).
  - menu_repro 10 min fixed (19:27 build): no hang; quarantine never on. menu_repro 10 min with `qmenus 1`
    (pre-fix quarantine): no hang either; quarantine up to 8,100 blocks during a fight load, then released.
    Private memory grew 665 -> ~1,050 MB in both (the game's menus/fight, not the quarantine).
  - 3-fight delay-2 regression on the 20:05 build: PASS (6/6 reports, 0 mismatches, 0 crashes, 0 pressure
    releases; QUARANTINE on at the arm). Final 8-min menu walk on it: no hang, quarantine never on.
  - The 18:41 freeze came right after Start while the main menu built dozens of UITextItem MK objects
    (frames log). Not reproduced in 20 min of automated menus: intermittent. `tools\start_loop.ps1` repeats
    start -> arm -> Start -> 1 min, 8x with `qmenus 1` vs 8x fixed (20:38 build with a QUARANTINE census line).
  - `qmenus 0|1` command (pre-fix quarantine for comparisons); PendingBytesApprox was a stub returning 0.
  - start_loop 20:38-21:05: 8 runs with `qmenus 1` and 8 fixed, all passed Start and the menus without a hang
    (no frozen-run log kept; the console summary was not readable, so start_loop now also writes
    results\<Name>.txt). **The freeze has not been reproduced in 36 automated minutes / 18 Starts; its cause is
    unproven.** In place: quarantine off outside sessions (what Safe mode did in the user's 11 freeze-free
    minutes), the pressure valve, MKHEAP near-full logging, the census line, and the menu walk in the release gate.
  - 21:10-21:45 full release gate on the 20:38 build (results\release_gate_2110.txt): online smoke PASS (both
    named and auto), plain regression and auto re-arm PASS (6/6 reports each, 0 mismatches, 0 crashes, no cut
    session; overlay 7,086 draws, max 0.21 ms), menu walk 8 min PASS (307 keys, no hang, quarantine never on).
    **Zip for players: `dist\MK9Rollback-netplay-20261029-2145.zip`** (DLL 63FE4D67…): room type 0 accepted, MK9
    NETCODE status line, quarantine only in sessions, pressure valve, heap diagnostics. 1917 deleted.
- Release gate: queued_checks' normal path runs menu_repro (8 min, armed) and makes no zip unless it reports no
  hang/crash, the game responding, and the quarantine never on / no pressure release in the menus.

**2026-10-29 18:40 BLOCKER: freezes after Start / in the menus (both players, 1837 zip).** The user's 18:41
session: the game thread died in MK heap #5's ([0x00ED5400]) allocation failure path: online code 0x0088753A ->
0x00887520 -> HkHeapAlloc (our vt+0x10 hook) -> 0x0042B390 returned null -> 0x0042B4F5 fatal handler 0x00406160
(spins). The user's 18:44 session then reported a frozen menu while the game thread kept ticking (no HANG line).
Not reproduced yet: no automated test passes the title screen or the menus. Prepared: `tools\menu_repro.ps1`
(launcher commands, Enter at the title, random menu walk, stops at a HANG/CRASH; `-Passive` = mod loaded but not
armed, the control) and a `MKHEAPS used/reserved` line every 10 s (18:54 build). Needs the PC. Workaround given:
inject after reaching the main menu.

## Needs the user

- [ ] **Real online test with two Steam accounts.**
  - Both players use the 1757 zip (the same zip on both PCs: the session start compares the DLL).
  - Play a 1 vs 1 room in Online > Player Match (not King of the Hill).
  - Afterwards both press "Save logs to the Desktop" and send the zips.
  - It also answers whether native 1172 (online start) runs once per round or once per fight.
- [ ] **Leave the PC free** (unlocked, idle for 5 minutes or more, Overwatch closed) so the queued tests can run.

## Claude can do alone (in this order)

1. [ ] **Finish soak 12401** (16 sessions) and classify it.
   - Trace any real desync or crash it catches.
   - Done: 15,481 loads, 0 crashes, 2 real failing stops (17 errors) and 24 end-of-fight artifact errors.
     - 12415 S=940, hit-test/attachment class: the replay attached 2 MKParticleActors to P1 that the original did not, and actor +0x80 differed (0x2000 vs 0x2040). VMTRACE shows the tickgate contact differing.
     - 12411 S=2236, engine-tick class: FightUpdate entry and exit were identical. At EngineTick exit P1 Z differed (79.560 vs 79.563), and so did comp +0x3A8/+0x3AC, the queued anim-object list header that is kept live (P2 count 0x2C vs 0x2D).
2. [ ] **More overnight soaks** to catch the rare classes:
   - the hit test 0x8A46E0 spawning a (1, 0xCE) VM;
   - the Touching list (+0x1B0);
   - engine-tick float drift;
   - the registry crash 0x45B168.

   Fix each class when one is caught.
3. [ ] **Simulated internet lag on one PC** (4–8 h).
   - Add delay, jitter and packet loss to the two-instance tool, e.g. 60–150 ms.
   - Use it to tune the input delay and check longer rollbacks.
   - Check the online-mode timing under lag.
   - In progress:
     - New `netsim <ms> [oop%]` command. It switches on GGPO's built-in network simulator (`ggpo.network.delay` / `ggpo.oop.percent`), from the next session start.
     - `two_instance.ps1 -NetSim/-NetSimOop` passes it to both games.
     - `queued_checks.ps1 -LagMatrix` runs 6 regressions: 40/0, 40/2, 80/2, 80/3, 120/3 and 80/2 with 5% out of order. Each is one-way ms / input delay frames. Results go to `results\lag_matrix.csv`.
     - First matrix (2026-10-27/28, results\lag_matrix.csv): 40/0, 40/2, 80/2 and 80/3 pass with every checksum matching.
       | run | loads | replayed | barrier waits (both games summed, 15,720 session frames per run) |
       |---|---|---|---|
       | 40 ms / 0 | 360 | 1,427 | 1,531 |
       | 40 ms / 2 | 140 | 196 | 889 |
       | 80 ms / 2 | 220 | 429 | 1,159 |
       | 80 ms / 3 | 109 | 171 | 1,437 |
       Input delay 2 cuts rollbacks by about 60% at 40 ms; at 80 ms, delay 3 halves them again.
     - 80/2 with 5% out of order: all checksums matched; failed only on 2 "failed saves" GGPO asked for after the
       game had reset GameFrame for the next map. Fixed: those are no longer counted (the session stops on the next tick).
     - 120/3: fight 2 started on B at GameFrame 0 (life 0 at frame 0). The game's timeline restarted between the start
       request and the session start, and the pending start was not cancelled. Fixed in the restart path. Also fixed
       a race in two_instance.ps1 (a game that reached the next fight start before the script re-armed it).
     - Rerun with the fixes (results\lag_matrix_rerun.csv): 120/3 and 80/2+oop5 both pass, 0 mismatches, 0 failed saves.
       The restart-cancel path did not trigger in the rerun, so it is not yet runtime-verified.
       NETSTATS at 120 ms: ping ~290-305 ms, local frames behind ~18.
     - Runs now keep both game logs (results\<run>_A.log / _B.log).
     - 80 ms / 1 (online mode's current default): passes; 498 loads, 1,452 replayed vs 220 / 429 at delay 2.
     - 40 ms / 1: crashed (item 8); rerun on the 12:27 build passes (313 loads, 820 replayed over 4 sessions).
       That rerun passed with fight 3's two checksum reports missing (the script stopped the games before the
       session's stop): two_instance.ps1 now waits up to 60 s for one report per fight, and a regression needs 6.
     - Per 1,965-frame session: 40/1 78 loads / 205 replayed, 40/2 18 / 25, 80/1 83 / 242, 80/2 28 / 54, 80/3 14 / 21.
     - **Decided: the launcher's rollback delay is now 2 (was 1)**; safe mode stays lockstep at 3. One more frame of
       input lag (17 ms) for 4-8 times fewer replayed frames. The 2348 zip has it. lag_ns40_d2 rerun (with the
       stricter pass rule) queued again: the 2026-10-28 queue was stopped before it ran.
4. [ ] **In-game status display** (5–10 h): rollback on or off, ping, frames rolled back.
   - Design: `docs\status_overlay_plan.md` (2026-10-29; hook site checked against the disassembly: 0x00667106
     `mov eax,[0xE84534]` right before RHIEndDrawingViewport's EndScene, vt+0xA8).
   - **Written 2026-10-29, built 17:37** (`src\debug\Overlay.*`; needs `-lgdi32`, added to build.ps1). Built
     first into build\overlay, then copied to build\clang for the 2026-10-29 17:40 smoke + 3-fight delay-2
     regression (results\overlay_regress*, screenshot results\overlay_regress.png).
     - **First runtime result (17:39):** on this PC the device's vtable is a heap copy next to the device (a
       hooking layer's), so the first check (vtable inside d3d9.dll) disabled the overlay. The check is now:
       vtable readable and all 119 entries point into committed executable memory; it logs the owning module
       ("in heap" here). Calls through it work: render target 0 is the back buffer, viewport 1920x1200.
     - **Draws (17:49):** results\overlay_live.png shows "ROLLBACK: waiting for the other player" in yellow at the
       top left (the window capture keeps the overlay; the 3D frame is black in it).
     - The 5-byte CodePatch at 0x00667106 calls a stub that draws and then performs the replaced instruction. It
       checks the 16 site bytes first (fail closed), re-applies the patch if a RestoreAll removed it, and is
       disabled at unload before RestoreAll.
     - Drawing: glyphs rendered once at install with GDI (Consolas 8x14), untextured quads with one
       DrawPrimitiveUP, a per-frame D3DSBT_ALL state block, nothing kept on the device. The first draw logs the
       device, whether its vtable is d3d9.dll's (else the overlay disables itself) and whether render target 0
       is the back buffer.
     - Data: `PublishOverlay()` at every live OnTickEntry (never in a replay): mode (ROLLBACK / LOCKSTEP /
       waiting / STOPPED: reason for 5 s / SYNCTEST / off), ping and frames behind (GGPO, every 30 ticks),
       delay, depth of the last load, max depth and loads in the last 60 frames, DESYNC count.
     - `overlay 0|1|2` (1 default: shown while netplay is armed, running or just stopped; 2 verbose, always).
       NETSTATS lines are followed by `OVERLAY draws … while resimulating … max draw time … us`.
     - Checks: the plain regression takes a screenshot 20 s into fight 1 (`two_instance.ps1 -Shot`,
       results\two_instance_regress_overlay.png) and every regression prints the overlay's log lines.
   - Next: build after the queued runs, then run the checks; verify the screenshot and a draw time well under 1 ms.
5. [ ] **Lighter logging for players** (1–2 h). The per-frame log reaches several MB per session, which makes the "Save logs" zips large.
   - Built 2026-10-28 00:05; verified by the lag reruns (NETSTATS lines in their logs).
     - Logger rotation: past 48 MB the frame log becomes `MK9Rollback_frames.old.log`, so at most about 96 MB on disk. "Save logs" includes both files.
     - New `NETSTATS` line every 600 frames of a P2P session: ping, send queue, kbps, frames behind (local/remote), loads and replayed frames so far. It is for reading the real online test.
6. [ ] **Online-mode edge cases** (2–4 h):
   - a player leaves mid-fight;
   - rematch;
   - an opponent without the mod.

   Simulate the ones that are possible with two instances.
7. [ ] **Fight-2 session cut on instance B** (found 2026-10-28).
   - two_instance_regress 00:52: B's fight-2 session ended at frame 415 with a map reload ("timeline restart"); A saw
     the peer disconnect, and fight 3 never joined.
   - lag_ns120_d3 (22:15 build): B's game reloaded the map just after the fight-2 start request.
   - Both times the script's arm command reached B after B had already started the session (arming persists).
     The cause of the reload is unknown: those logs were overwritten.
   - Regressions now keep both logs (results\<run>_A.log/_B.log) and fail on any session not ended by the fight's end.
   - The rerun (01:40) was clean, so the cut is intermittent. The fight-3 arm for A also arrived at gf 655, right at
     the fight start: the script's arm timing races the game.
   - `-RegressRepeat 4` (02:50-04:10): 4 of 4 clean, not reproduced.
8. [ ] **Crash: freed physics helper used after a load** (lag_ns40_d1, 2026-10-28 02:43, instance A, session frame ~880).
   - 0x008A753A read the zeroed vtable of the block at [obj+0x1D8]; obj has class 0x00D6A9B0 (a script VM object;
     its destructor 0x00895BA0 queues +0x1D8 for a delayed free). An MSVC "R6025 pure virtual call" dialog then
     blocked the game, and B saw the peer disconnect.
   - Cause: the engine's pending-release list (0x0074FA10 adds, 0x0074FB20 performs, 6-update countdown) is live
     across loads. A release queued in the abandoned timeline fired during the replay, while the restored object
     still used the block.
   - Fix (built 12:27): every save records the list, and a load removes kind-1 records the save doesn't have
     ('pendrel', PENDREL log lines). The reverse case (a release of the save performed after it) leaks the block,
     by choice.
   - The rollback before it showed the (1, 0xCE) VM divergence from item 2 (hit-test class): same area.
   - **Verified at runtime:** release checks pass (zip 1259); soak 12501-12504: 3,573 loads, 0 crashes, PENDREL
     ~29 per session (~20 removed, 8-10 leaked blocks); lag_ns40_d1 rerun passes with both PENDREL paths in its logs.
     The crash itself is rare, so the fix is proven to run safely, not yet proven to stop every such crash.
   - Soak 12501-12504 failing stops: 12501 S=2461 P1 posz 5.820 vs 5.828 (engine-tick float drift); 12502 S=1550
     the (1, 0xCE) hit class again (the original hit at gf 1551, the replay did not; then P2 attached 6 vs 2 and
     both fighters' actor +0x80 0x2040 vs 0x2000, a consequence); 12504 an end-of-fight artifact.
   - Traced soak 12601-12612 (`animtrace 1`): 10,547 loads, 0 crashes, 3 real failing loads; every other error
     is the end-of-fight artifact (4 errors with 4 failed saves), and 12606's short "(restart)" stops are clean.
     - 12611 S=2362 and 12606 S=1104: the hit class (12611 like 12502: the original hit, the replay did not;
       12606 the reverse, then P1 attached 0 vs 4).
     - 12603 S=2311: P1 actor +0x68 (attached-list data) and +0x1B4 differ, small position drift.
     - **HITPOS for 12611:** the collision object (attached to fighter 53400F80, Physics none) had at the
       first replayed hit check a Location from the abandoned future (-77.96, 217.75, 217.97 instead of
       -20.42, 210.89, 100.86), and after that the replay trailed the original by exactly one frame
       (replay 2363 = original 2362, 2364 = 2363), so the hit came a frame late. The traced hit test (site 10)
       shows the same one-frame lag in its tested position.
     - Not explained yet: mkactor spans restore Location by value. Either the span was skipped (identity,
       freed block, bDeleteMe) or something moved the actor after the load. **11:01 build:** each load notes
       what it did with every mkactor span, and the replay's HITPOS lines end with `atload=<restored
       Location>` or `atload=skip<n>` / `none`. Traced soak 12701-12712 queued on it.
     - **Answered by 12704 S=867 (same class):** the span *was* restored (atload -79.58,-175.32,100.86, the
       original's value), but at the first replayed hit check the actor was at -17.26,-186.90,233.79, beyond
       the original's frame 869: something moves it to the abandoned future's position between the load
       and the MK object tick, and the replay then trails by one frame (replay 868 = original 867, hit a
       frame early or late). Next: find that writer (write watch on the actor's Location during the first
       replayed frame, armed when such an object exists at a load), then restore or reset its source.
     - **Write trace (written 2026-10-29 14:20, builds after soak 12701-12712):** with `animtrace 1`, each load
       arms CrashLog's hardware write trace on the Location of a restored MK actor based on a fighter (Physics
       none), re-armed only when that actor changes. `LOCLOAD` = the load's own later steps (MKATTACH,
       RepairAttachBase, SyncFighterComponents) moved it after its span was written; `LOCMOVED` = the first
       replayed hit check found it moved, with every write of that frame in both passes (writer EIP, value,
       callers). A static search for the writer runs in parallel.
     - **Static search result (agent, 2026-10-29):** none of the candidates stores +0xDC directly; all go through
       MoveActor 0x007A6F00 (store in the actor's vt+0xB8). Attached children are re-placed from their
       AttachedActors record by 0x0078D0B0: target transform (bone: comp +0x2F4 buffer x comp +0x70) times the
       record's relative transform (+0x04..+0x18); callers are the skeletal UpdateTransform 0x0077B9E0 ->
       0x007794A0 (MoveActor at 0x007796CF) and MoveActor's attached loop (0x007A7603). MK's SetPosition 0x00433580
       writes the *relative* location (0x00727C40) and its GetPosition 0x00433410 reads record +0x04. The
       record lives in the fighter's AttachedActors block, kept live at loads, and RestoreMkAttach only appended
       records for unlisted actors: the future's relative transform survived the load.
     - **Fix candidate (14:28, 'mkattrec', on):** RestoreMkAttach now also restores a listed actor's saved record
       data (+0x04..+0x1B, +0x4C..+0x5B) while its targets (+0x1C, +0x30, +0x34, +0x48) are unchanged; logs
       MKATTREC. Not yet runtime-verified; the one-frame lag after the load is not explained by it alone.
     - Soak 12701-12712 (11:01 build, before the fix): 10,657 loads, 0 crashes, 1 real failing load (12704 S=867,
       above); all other errors end-of-fight artifacts.
     - 14:32 build (mkattrec + LOCLOAD/LOCMOVED trace): online smoke and auto re-arm pass (6 sessions, 0
       mismatches); the plain regression failed on an arming race (A armed in fight 1, B only in fight 2, A's
       "did not join within 60 s"; slow B, not a game desync), so no zip. Soak 12801-12809 (stopped at 9 of 12 for
       the 2026-10-29 friend test): 0 crashes, MKATTREC fired 210 times, **sync errors still in 4 of 9 sessions**
       (7/10/4/3, the same rate as 127xx), LOCLOAD/LOCMOVED 0 lines (the arm found no Physics-0 actor on a
       fighter base, or never armed: check OnLoadDone's search). mkattrec does not remove the class.
     - **Classified (2026-10-29 18:00):** of the 4 sessions with errors, the stops with as many errors as failed
       saves on the fight's last frame are the known end-of-fight artifact. The 3 real failing loads are all the
       script/spawn class of docs\script_scheduling.md §4b, none the attached-record class mkattrec targets:
       - 12808 S=1199 (frame 492): the hit-test group. An MKCollisionObj (11193800) is created on the first replayed
         frame in both passes (ctor 0x008A45C0 <- 0x008A4C70 <- native 0x008D94F0, owner from the executor via
         0x00458930), but the replay's stands next to P1 (y -321.7, P1 -322.2) and the original's next to P2
         (y -263.8, P2 -244.0). The replay's hit test then hits (res 1 vs 0) and spawns a (1, 0xCE) VM, ret
         0x0045D9C8.
       - 12801 S=2166 and 12803 S=1919: the first VM event differs as `wait` vs `runscript` (0x0089ED78, first
         native 0x8B6DA0); 12803's anim-node counters (+0x2C) are one frame apart.
       - Probe built (18:02, build\probe, not yet run): VMTRACE site `collctor` at the MKCollisionObj constructor
         entry records ecx and (owner, [owner+0x7638], script arg 8). Next: traced soak on it after the friend test.
       lag_ns80_d2 not run. 14:32 binaries kept in build\clang_1432.
     - **2026-10-29 21:46 lag_ns80_d2 on the 2145 build: PASS** (3 fights, 6/6 reports, 0 mismatches, 0 failed saves,
       0 crashes; 11,790 frames, 442 loads, 958 replayed frames, 122 barrier waits; overlay max 1.4 ms).
     - **Traced soak 12901-12912 on the 2145 build (collctor probe, `animtrace 1`): 0 crashes, all alive**
       (results\synctest_soak\summary_12901.csv). Real failing loads (sync errors > failed saves): 12901 f412 (S=1138),
       12904 f1456 (S=2289), 12906 f1472 (S=2259), 12909 f88/f44/f8 (S=1193, 1810, 1961); the rest (4 per stop in
       12902/03/04/06/08/11) are end-of-fight artifacts. Classes:
       - Hit-test / spawn class (12901, 12909 x3): a hit test (site 3/7/10) returns 0 live and 1 in the replay with
         different volume positions (vec0), then the `(1, 0xCE)` ctor vs `tickgate` order and `wait` vs `runscript`
         (first native 0x8B6DA0) differ; P1 gets 2 vs 5-6 attached particle actors.
       - **12904 S=2289: the `collctor` probe fired on the first replayed frame with the SAME owner and arguments
         (owner 119AAD90, [owner+0x7638] 00E89D7C, arg 119A66E0, ret 0x008A4CC8) but a DIFFERENT object: original
         11A50400, replay 11A52200.** The owner hypothesis of 12808 is refuted for this case; the allocator handed
         out another block after the load. 0x008A4C70 allocates 0x1DC bytes via 0x0042D5F0 -> size class 0x800 pool
         [0xED5414] (6 elements) -> fallback 0x1000 pool -> master heap. All 13 pool objects (+0..+0x50) are in
         Tier A and the master region in Tier B, so where the state differs is open (11A52200 - 11A50400 = 0x1E00 is
         not a multiple of 0x800: at least one pass fell back past the pool). FIRSTDIFF's image was identical up to
         the MK object tick and 251 dwords differed after it.
       - 12906 S=2259: P2 position drift of ~0.002 with 97 differing floats at 0x119029E8.. at EngineTick exit of the
         loaded frame (anim/pose floats), no VM/native difference: a separate float class.
     - **01:07 build (probe only):** collctor also logs the 0x800 and 0x1000 pools' free-list heads (+0x30) and the
       master heap's used bytes. Traced soak 13001-13012 queued (results\soak_13001_run.txt) to show which allocator
       state differs between the passes.
     - **Soak 13001-13012 (01:07 build): 0 crashes, all alive, 0 collctor differences** (summary_13001.csv): the
       12904 address case is rare (1 in 24 sessions), not the main class. Real failing loads: 13011 S=2063 (f1368)
       and 13012 S=1788 (f1088), both the hit-test class; 13001's 8 errors are "a load found no snapshot" at frame
       1-4 (harness start artifact), 13010's 4 the end-of-fight artifact.
       - HITPOS for 13011: the bone-attached collision actor (Base 636407C0, Physics none) was restored to
         1222.03,1463.64,131.85 (atload) but the first replayed hit check saw it at 1195.08,1409.90,129.44, about
         the original's NEXT frame (L 2063 1208.81,1437.86; L 2064 1195.67,1412.17): the replay is one frame
         ahead, hits at 2063 instead of 2064. Same mechanism as 12611/12704 (something re-places the attached actor
         from the future pose between the load and the MK object tick); mkattrec did not remove it.
       - LOCLOAD/LOCMOVED never fired: OnLoadDone only armed actors whose Base is a fighter actor, and these Bases
         are other actors (attached SkeletalMeshActors). **05:36 build:** the arm prefers the last live hit-checked
         actor with a Base (any Base, Physics none), then the armed one, then a fighter-based one. Traced soak
         13101-13112 queued (results\soak_13101_run.txt) to name the writer.
     - **Soak 13101-13112 (05:36 build): 0 real failing loads** (13103's are the start/no-snapshot artifact, the rest
       end-of-fight), **1 crash: 13110 gf 794 (just after a load to 789), eip 0x0044B956** reading [0+0x80]: in the
       fighter's anim-rate code, 0x0043B1A0 (the MK anim node playing on the bone the UE3 tree names, [[comp+0x280]+
       0xD0] -> 0x004355A0) returned 0; first time in any log. **Guard (14:12, HandleGuard):** a null node takes the
       not-equal branch 0x0044B990, whose 0x00442260 does the same lookup via 0x0043B070 and already handles null
       (logged HANDLEGUARD, counted in NullHits).
     - **The writer, from 102 LOCMOVED captures:** in the replay MKObj::Update (0x0043E010) writes the based actor's
       Location through 0x0043E14F -> SetRelativeLocation 0x00727C40 -> 0x00727D78 -> MoveActor 75 times; the original
       3 times (the engine's attached re-place 0x0078541C -> 0x0078D2F0 appears in both). 0x0043E108..0x0043E17C: an MK
       object with a Base takes its relative location (0x007896F0) + the frame's delta and sets it; the world position
       comes from the Base's bone matrix (comp +0x2F4) x LocalToWorld (comp +0x70). In the original that reproduces the
       current Location (no write); after a load the Base's pose is the abandoned future's, so the hit box jumps ahead.
       The Bases are not the fighters (their bone arrays are already restored) but other MK skeletal objects attached
       to them (SkeletalMeshActors: props, weapon meshes).
     - **Fix candidate (14:15 build, `propskel`, on):** for every listed MKSkeletalObj other than the fighters, the
       snapshot saves its component's bone arrays (skelp_*), the three matrix-array headers (proprot, restored only
       when they still name the same three blocks) and LocalToWorld (propl2w, only for a component whose proprot was
       restored). `propskel 0|1` and abtest `propskel` gate the load side; in the fingerprint. LOCMOVED now names the
       Base's class. Traced soak 13201-13212 queued (results\soak_13201_run.txt): expect the replay-only 0x0043E14F
       writes to drop from 75 per 12 sessions, 0 crashes. Then the release gate before any zip.
     - The plain regression's failure was harness timing: both were armed during attract fight 1, A's fight ended
       soon after and A started at its next fight, while B (-SlowB, one core) still had ~1,200 frames of its fight
       left, so B joined after A's 60 s join timeout. **Harness fix made (18:10, parse-checked, not run yet):**
       two_instance.ps1 retries a fight once ("arming skew retry") when either game logs "did not join" after the
       arm (not with -AutoRearm); a second time for the same fight fails the run. queued_checks.ps1 excuses up to
       two "peer did not join" stops per retry line.
     - LOCLOAD/LOCMOVED 0 lines is expected for these failures: the objects involved are new in both passes (no
       restored actor with a fighter Base), so OnLoadDone had nothing to arm.
   - lag_ns40_d2 rerun (2026-10-29): passes, all 6 sessions reported (11,790 frames, 200 loads, 274 replayed).

## Done (2026-10-27)

- [x] Replay send guard on the online message send 0x81BCD0.
- [x] ActorDefer diagnostic scan over-read crash.
  - The scan is clamped to readable memory and its snprintf overflow is guarded.
  - The crash report now names the quarantine block it matched.
- [x] fightercomp float vectors restored by value (`compfloats`). Soak 12301: 0 real failing stops in 11,542 loads.
- [x] Online hook check, both netplay regressions and the auto re-arm test pass. Zips 1436 and 1744 are built from the same DLL.
- [x] README for the online mode. The launcher keeps the previous session's logs for "Save logs".
- [x] `queued_checks.ps1 -SoakOnly`.
