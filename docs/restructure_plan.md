# Restructure plan (2026-10-31)

Why: two-PC play is flaky. In the 2026-10-31 tests **no rollback session ever ran**. Every failure happened before the
first fight frame or outside the fight:

| Failure | Cause |
|---|---|
| Black fights in rollback mode | Per-tick debug audits running in the players' build |
| Disconnects after the stage load (4 of 5 matches) | The mod arming during MK9's character select, under MK9's own online session |
| "Out of memory" / pure virtual call freezes while opening private rooms | Inside MK9's online menu code; whether the mod provokes them is unknown |

Two design choices sit under all of these:

1. **Players run the research build.** A Safe-mode player gets 53 patches and hooks, of which about 35 exist only for
   rollback or diagnostics. Diagnostic work also runs every tick in both modes: state hashing, frame logs with stack
   captures, censuses, write-watches on every thread.
2. **The main connection path rides on MK9's online session.** MK9's session keeps running under ours: it can drop a
   player, time out or crash in its menu code. The mod can only guess its phases from memory addresses, and this
   path cannot be tested on one PC.

**Not a rewrite.** The 17,400 lines hold hard-won knowledge (addresses, state ownership, determinism fixes) that a
rewrite would have to rediscover. The plan below restructures the code around it, in this order.

Sources: the hook and diagnostics map and the Local Versus assessment, both 2026-10-31 (summarised here; file:line
references are in the tables below).

---

## Status (2026-10-31 21:42, DLL D9AB5A78…, built, not yet run)

**Done in code:**

| Item | What changed | Plan change |
|---|---|---|
| 1a | One DLL with a runtime `lab 0\|1` switch (`src/debug/Lab.h`), default off, sent before the mode commands. Lab test scripts send it: noai_rollback (soaks), attract_rollback, unrestored, rollback/restore/determinism trials, missing_state | Replaces the compile-time `MK9R_LAB` second DLL: one DLL, so the gate runs exactly what players get |
| 1b | `netcode safe\|rollback` command. `Rollback::Enable` installs the rollback modules only when rollback is wanted (`InstallRollbackModules`: object hooks, Ue3Refs, quarantine, RNG trace's native dispatch, sound dedupe, rumble, particle completion, HUD guards, actor deferral). `ggpo lockstep 0` / `netcode rollback` later add them. A rollback or SyncTest session refuses to start without them | Ue3Refs stays installed at DLL init (moving it risks rollback correctness: instances created before tracking) |
| 1c | Lab-only: the four restore audits (so PhaseAudit too), PadTrace, CaptureBones, STATE/HASH lines and StateHash, OBJ lines and their stack captures, WatchBin (`gDiag` default off), RegionDiff, TypeCensus, quarantine census, GGPO load refusal checks, LINKEDFREE scan, connection WATCH, the RNG/VM/tick trace patches (RngTrace keeps only the native dispatch), PhysWatch. P2P sessions skip the SyncTest-only per-frame copies | FRAME lines stay (the two-instance tests read them), and so do MEMORY/MKHEAPS every 600 ticks |
| 1e | `Enable` sent again while on only grows the patch set: no ring clear, no ScriptClock re-anchor. The launcher sends `netcode` and adds `online off` in Local Versus. The hello is version 2 with mode and delay, so the online mode refuses a Safe/Rollback mismatch with a log line. The environment fingerprint includes the lockstep flag | — |

**Expected patch count:** a Safe-mode player now gets about 22 install lines:
- init: OOM handler, Ue3Refs (2), game loop (3), overlay;
- `netcode safe`: sampler, pause guard, ScriptClock (6 + native 191), ScriptRng (2);
- online: 3.

Not verified yet.

**Side effect:** in Safe mode the MK heap alloc hook (part of the quarantine) is not installed. An out-of-memory log then has the heap state and stack, but not the request size and caller.

**Updated scripts:**
- The release-gate scripts use the player commands: `two_instance` (netcode safe/rollback), `menu_repro -Safe`, online_smoke, watch_smoke and the shipped `netplay.ps1`.
- online_smoke retries the launch once.

**Not done:** 1d (the RollbackManager split; incremental, later).

**Before any zip:** `queued_checks.ps1 -Now -Lockstep -Soak 0` (Safe mode gate), then `queued_checks.ps1 -Now -Soak 0` (rollback gate). Both need the PC for about 90 minutes. Then the two-PC Safe-mode test.

**Gates run 2026-11-01 00:53-01:54 on D9AB5A78, both PASS:**

| Gate | Online smoke | Regression | Auto re-arm | Menu walk | Extra |
|---|---|---|---|---|---|
| Safe (`-Lockstep`) | PASS | 3 fights, 6/6 reports, 0 mismatches | PASS, each game re-armed 3× | PASS, 305 keys | lockstep_ns60 PASS, peer loss PASS (A ran 120 frames after the stop) |
| Rollback | PASS | 6/6 reports, 0 mismatches, 0 failed saves | PASS | PASS, 306 keys | 86-95 loads and 259-331 replayed frames per fight |

- **Install lines counted:**
  - Safe mode: 22 in the regression log, of which 4 are the harness's scriptseed patches. That leaves 18, plus 3 online hooks for a player: about 21.
  - Rollback mode: 41, of which 4 are scriptseed. That leaves 37, plus 3 online hooks: about 40.
- **Overlay max draw time in the rollback regression:** 0.5 ms, against 4.6 ms on 2056.
- **Zip:** `dist\MK9Rollback-netplay-20261101-0154.zip` (DLL D9AB5A7897F76CEBED0744EE092459764CDA23A1C44AD550E1839C122F87D6CC), passed both gates. The 0129 zip holds the same DLL.
- **Next:** the two-PC Safe-mode test (3 private-room matches).

**Code review, 2026-11-01 (five review agents, findings checked against the code; a sixth agent checked the fixes and
its five findings were fixed too: neutral pads after a failed start, a waiting session stopped on a busy VM, the crash
mark cleared when the game keeps ticking and limited to the game thread / MKKE code, the shared clock for sessions
without the renumber, the launcher's disengage regex. Built as DLL 141A2396…; both gates PASS 11:47-12:47, see below):**

| Area | Fixed | Why it matters to players |
|---|---|---|
| Script clock | Virtual clock only from a netplay start to the next timeline restart (or `lab 1` / `idlesaves 1`); MK9's real clock in menus and loads | Likely cause of "disconnected after loading the stage": the 20:46 load wait ticked every ~2.5 ms, and a 40 s script timeout expired after 6.1 real seconds (native 1182 at gf 497, give-up natives at gf 2897 = 2400 frames) |
| Lockstep wait | 1 ms high-resolution timer waits (Sleep(1) waited 15.6 ms), 5 s deadline (the try count meant 78 s), no time-sync sleep in lockstep | Up to a timer tick of extra input delay on nearly every lagged frame |
| Crash = freeze | Watchdog closes the game 6 s after a crash if the game thread never ticks again; a fault inside the purecall/OOM handlers closes at once; a Visual C++ runtime error box closes the game; GGPO asserts log and close instead of a hidden message box | 40 of 40 crash logs ended spinning in MK9's heap lock (the frozen-PC reports) |
| Command race | The stale command file is removed before the log exists | The launcher arms as soon as the log appears; the old removal could delete `netcode`/`online` and the game never armed |
| Patching | Per-patch restore (a failed object-hook install no longer strips Safe mode's patches); one lock for every thread-suspending path (patching, FPU pinning, watchdog dumps) | Silent loss of determinism patches; two suspenders could freeze each other |
| Online mode | Re-arm at a round restart only while the fight connection is up and both modes still match; stale engagements dropped at a new connection phase; a mode change mid-match disengages instead of starting lockstep against rollback; Disengage stops a running session; 15 s join timeout when online (60 s locally); a failed session start unpauses at once; memory scan at below-normal priority | Armed through a rematch's character select; mismatched sessions; 60 s paused waits MK9 drops |
| Rollback | Load with no snapshot keeps GGPO's frame count (was an assert + exit); deferred actors released after the fight update; script-busy boundary no longer desyncs a lockstep session; replay clears FoughtThisTick; a mode switch during a session installs the rollback modules at its stop | Crash / desync paths, mid-fight hitch |
| Performance | RepairAttachBase reads Base with a guarded read (was ~23,000 VirtualQuery per load); RNG trace records and compares only with `lab 1`; LearnFields report mode lab-only; quarantine skips its lock when nothing is tracked and hashes only in lab; sound dedupe one VirtualQuery per name; FPU pinning suspends each foreign thread once | Rollback loads were the main stutter; a periodic hitch every 10 s in both modes |

Reported, not changed (design-level or not reproduced): a GGPO timeout mid-fight still hands MK9 two diverged fights
(needs a stop policy both games apply); no two-phase "armed for this fight" handshake; snapshot save cost (~3.4 ms);
quarantine list scan per frame; overlay state block per frame; main log has no size cap; side 0 at the fight start.

**Gates run 2026-11-01 11:47-12:47 on 141A2396, both PASS:**

| Gate | Online smoke | Regression | Auto re-arm | Menu walk | Extra |
|---|---|---|---|---|---|
| Safe (`-Lockstep`) | PASS | 3 fights, 6/6 reports, 0 mismatches, 0 cut sessions | PASS, each game re-armed 3× | PASS, 305 keys | lockstep_ns60 PASS (6/6, 0 mismatches), peer loss PASS (A ran 120 frames after the stop) |
| Rollback | PASS | 6/6 reports, 0 mismatches, 0 failed saves | PASS, each game re-armed 3× | PASS, 305 keys | game A: 100-121 loads and 231-291 replayed frames per fight (was 86-95 and 259-331) |

- **New code paths:** no deadman close, `CRASH`, "could not start", "script VM was busy", GGPO assertion or "staying in
  lockstep" line in any run. Every fight logs "script clock on fixed bases" at its start, and all 15 sessions ran their
  1965 frames.
- **Lockstep wait:** under 60 ms of simulated lag (delay 3 = 50 ms) the side that waits ran its sessions at 59.8-59.9 fps,
  with up to 7.7 waits per frame. That matches the ~10 ms gap between the lag and the delay, at 1 ms per wait. The other
  side measured 57.5-57.6 fps, the same as the no-lag rollback runs. The "barrier waits" count is now in 1 ms waits (it
  was in 15.6 ms Sleep(1) ticks), so it is not comparable with older logs.
- **Rollback:** more loads but fewer replayed frames per fight (about 2.4 frames per load, was about 3.3). Most likely
  the 1 ms wait at the prediction threshold lets the leading game resume sooner. The total replay work is lower.
- **Overlay max draw time:** Safe 2.8 / 4.7 / 4.6 ms (regression / re-arm / ns60). Rollback 10.9 ms on game A of the
  regression (game B 2.8 ms; re-arm 6.9 ms), against 0.5 ms on D9AB5A78. It is one worst sample out of about 20,000,
  already reached before the first session's first stats line (menus and loads), and D9AB5A78's ns60 had 13 ms. It reads
  as a load hitch, not a per-frame cost, but it is worth watching.
- **Zips:** `dist\MK9Rollback-netplay-20261101-1247.zip` (after both gates) and `-1223.zip` (after the Safe gate),
  same DLL 141A23960C62142CE12E0687028C450DAC1E6F1A19A013874D277AC342A3717A.
- **Next:** the two-PC Safe-mode test (3 private-room matches, both players' log zips). It is the first real check of
  the stage-load disconnect fix.

---

## Phase 1: lean player build (do first)

Goal: a player's game installs only what its mode needs, and no diagnostic runs unless asked for. This is also the
cheapest experiment on the MK9 online-menu crashes: if they stop once the extra patches are gone, the mod was
provoking them.

### 1a. Build profiles

- **Compile-time `MK9R_LAB`.** The player DLL is built without it, so diagnostic patches and per-tick diagnostic work
  are not even present. The lab DLL keeps everything as today.
- **Test commands stay in both DLLs** (`scriptseed`, `killai`, `netplaydemo`, `oomtest`, ...). They are cheap and
  idle, and the release gate must run on the player DLL itself.
- **`build.ps1` builds both**: `build\clang` (player) and `build\lab`. `package.ps1` ships only the player DLL.

### 1b. Safe mode stops depending on rollback machinery

Today `rbmode 1` is required for any netplay. It refuses without objhooks (`CommandChannel.cpp:307-308`) and without a
quarantine install (`RollbackManager.cpp:2776-2779`), and `OnTickEntry` / `RecordInputs` return early when rollback
mode is off.

- **New mode switch:** `netcode safe|rollback` replaces `objhooks 1` + `rbmode 1` + `ggpo lockstep N` in the launcher.
- **`netcode safe` installs only:**

| Group | Patches and hooks |
|---|---|
| Safety | VEH, purecall handler, out-of-memory handler 0x00406160, hang watchdog |
| Game loop | WorldTick / EngineTick / FightUpdatePlayers hooks |
| Input and pause | Hardware sampler 0x00404E26, pause guard 0x00407AB0 |
| Determinism | ScriptClock (6 patches + native 191), ScriptRng (2), ForceFighterLod, FpuGuard |
| Netcode | GGPO, SteamBridge, checksum channel, environment fingerprint |
| Online mode | Gamepad exchange 0x00885F80, idle time 0x004F5E10 |
| Status line | Overlay patch 0x00667106 |

  That is about 19 patches instead of 53.
- **Not installed in Safe mode:** objhooks (2), Ue3Refs (2), FreeQuarantine (9 hooks), RngTrace (13), PhysWatch (2),
  SoundDedupe, Rumble, ParticleDone, ActorDefer, the online message-send hook.
- **HandleGuard (4) is unsure:** leave it out of Safe mode unless a lockstep run hits a null HUD slot.
- **Code change:** split `Rollback::Enable` / `OnTickEntry` so the netplay session (arm, start handshake, pause, GGPO
  lifecycle) runs without the snapshot engine. That is the first cut of the `RollbackManager.cpp` split (1d).

### 1c. Diagnostics off by default (lab-only or behind a command)

| Item | Where | Today |
|---|---|---|
| StateHash + FRAME/STATE/HASH lines | `GameLoopHook.cpp:93-131` | ~1.7 MB hashed per fight frame, replays included |
| OBJ+/OBJ- frames log with stack captures | `ObjectHooks.cpp:81,97` | Every MK object ctor/dtor |
| 600-tick MEMORY/MKHEAPS/REGIONS/CENSUS/TYPECENSUS/LISTHEADERS | RM:3264-3306 | ~10 ms game-thread stall every 10 s, inside sessions too |
| Snapshot::WatchBin (`gDiag`) | `GameLoopHook.cpp:148,188`, RM:3163 | Every tick; can `Sleep(1)` on the game thread |
| CaptureBones, PadTrace | RM:2468, 2529-2555 | 3× per fight tick |
| GgpoSaveState diagnostic copies | RM:1635-1661 | ~100 KB per frame, read only by SyncTest |
| Online connection WATCH (debug registers on ~150 threads) | `OnlineMode.cpp:103-117` | Whenever MK9's online data is active |
| PhaseAudit and the tick / pre-load / post-load / post-resim audits | RM:2457, 3178-3183, 1206-1240 | In every session |
| LINKEDFREE scan | `FreeQuarantine.cpp:303-332` | Up to 20,000 nodes per MK-heap free while quarantine is on |
| GGPO load refusal checks | RM:1764-1766 | Log only, ~3 ms per load in some windows |
| RngTrace recording, the 12 trace patches | `RngTrace.cpp` | Every RNG draw / native / VM event |

**Keep in the player build:**
- the main log;
- crash, hang, OOM and purecall reports;
- the online connection-end report;
- one MEMORY line per minute.

The frames log becomes lab-only.

### 1d. Split `RollbackManager.cpp` (3,400 lines) along the same lines

| New file | What moves there |
|---|---|
| `NetSession` | Arm, start handshake, pause / unpause, GGPO lifecycle, checksums |
| `RollbackEngine` | Save / load / resimulate, ring, deferrals |
| `Diagnostics` (lab) | Audits, captures, censuses, traces |

Done incrementally: each slice moves code without changing behaviour, then the lab and player regressions run.

### 1e. Launcher hazards (small, from the hook map)

- **Mid-fight re-arm:** pressing Play or toggling Rollback while the game runs re-sends `rbmode 1`. `Enable` then
  clears the snapshot ring (RM:2800) and re-anchors ScriptClock to local time (RM:2788), undoing the netplay start's
  shared anchor. The launcher must not resend mode commands mid-session: queue them for the next match.
- **Leftover online mode:** `online off` is never sent, so online auto stays armed after switching to Local Versus.
- **Undetected Safe / Rollback mismatch:** the environment fingerprint (`EnvFingerprint.cpp:74-86`) and the hello
  (`OnlineMode.cpp:43-46`) carry neither the lockstep flag nor the delay. Add both, and refuse with a clear message.

### Phase 1 done when

- The player DLL in Safe mode installs ≤ 20 patches.
- The release gate passes on the player DLL: online smoke, two-instance lockstep regression, auto re-arm, 8-min menu
  walk.
- The lab DLL still passes the rollback regressions and soak.

**Then one two-PC Safe-mode test with the friend:** 3 private-room matches with the lean build. Both log zips.

**Size:** medium (1b and 1d are the bulk).

---

## Phase 2: own the connection

Two paths exist. Both stay, but one becomes the main path, decided by evidence rather than taste.

**Path A: MK9 Online (today's default).**
- MK9 does matchmaking, character select and stage sync.
- The mod finds the opponent, then replaces the input exchange.
- The 2056 zip only engages in game state 8 (the fight's connection), as in the one match that played.
- Weak points:
  - MK9's session underneath can still drop a player.
  - MK9's online menu code crashed twice today.
  - The opponent is found by a 2-3 s scan of the game's memory.
  - It cannot be tested on one PC.

**Path B: Local Versus over Steam (exists, never tested between two PCs).**
- Both players start a local versus match, and the mod connects the games over Steam P2P.
- MK9's online code never runs.
- It can be tested end to end on one PC with two game copies.

Gaps found by code reading:

| # | Gap | Work | Size |
|---|---|---|---|
| B1 | Steam connection never exercised: no `AcceptP2PSessionWithUser`, every send is "unreliable no-delay" (Steam drops those until a connection exists), the Steam session is closed and reopened every round, bridge packets carry no session id | Accept the peer, send unreliable (with delay) until connected, keep the session for the whole match, tag packets with a session id, log the P2P state while waiting | Small–medium |
| B2 | Nothing happens before the fight: no "friend connected" | A pre-fight hello from injection on: connect in the menus, exchange DLL fingerprint, mode, delay and side; the launcher shows "connected, ping N ms" | Medium |
| B3 | **Nothing makes both games pick the same characters, costumes, stage and round settings**; the frame-0 check (life, position, meter, RNG) likely misses a character mismatch | Before unpausing, exchange character names (PlayerInfo+0x964), stage ([0xE89B8C]+0x3C), mesh names, round time / rounds-to-win, mode; stop with a message naming what differs | Small–medium |
| B4 | Any non-demo fight starts a session (Practice, Ladder too) | Start only in Versus (learn GameInfo+0x8's value from one logged run) | Small |
| B5 | Each player must set up both sides alike; may need two input devices | Optional: host picks for both through `SetCharacter` 0x008141A0 / `SetStage` 0x00811DD0 at GameInit 0x0080DF82 (as MK9Hook does); costume override address unknown | Medium–large |
| B6 | Launcher: Local Versus is the "older test mode", misleading per-round message | Make it a first-class choice, show both picks | Small |
| B7 | One-PC test of the real flow | Two-instance harness drives the local versus menus with posted keys (menu_repro's PostKey) instead of attract demos | Medium |

**Decision gate:**
1. Play the 2056 zip (Path A, state-8 gate) for a few Safe-mode matches, then the Phase-1 lean build.
2. If Path A plays 3 matches in a row without a disconnect or freeze, it stays the main path, and Path B is finished
   as a fallback (B1–B4, B6, B7).
3. If Path A still fails, Path B becomes the main path (B1–B4, B6, B7 first, B5 if picking both sides is too
   awkward).
4. Either way, B1 and B3 are worth doing: they are what makes Path B safe to offer at all.

**Control test (independent, the players):** open a private room a few times with MK9 started from Steam *without*
the mod. If plain MK9 also freezes there, the menu crashes are MK9's own, and Path B is the only way around them.

---

## Phase 3: rollback stays experimental

- Rollback keeps its checkbox, labelled experimental. Safe mode is the default and the supported mode until Phases 1-2
  are done.
- **Lean rollback profile:**
  - Cut the 12 trace patches, PhysWatch, all audits, the LINKEDFREE scan, per-replay StateHash, CaptureBones, the
    GgpoSaveState copies and OBJ stack captures (keep the event ring).
  - Reduce the native-dispatch stub to context tracking only.
  - Fix SoundDedupe's per-character VirtualQuery.
- **Promotion criteria:**
  - a lab soak with 0 real failing stops;
  - then the two-instance rollback regression on the player DLL;
  - then 3 two-PC matches.
- **Open core issues:** rare replay desyncs (12411, 12415 classes in todo.md) and crashes at level change.

---

## Testing changes (all phases)

- **Gate the build players get:** the release gate runs on the player DLL, per mode.
- **Real-flow test:** the two-instance harness gets a real-flow variant: local versus picked through the menus, not
  attract demos (B7).
- **Two-PC checklist for the players** (`docs/two_pc_test.md`):
  - which zip and mode;
  - what to play (3 matches, a rematch, a quit mid-fight);
  - sending both log zips.
- **Untested builds are labelled so.** A zip is either gate-passed or labelled untested in its name and README,
  never silently.

## Order and rough size

| Step | Size | Needs the PC / players |
|---|---|---|
| 1a profiles + 1c diagnostics off | Medium | Gate run (PC, ~40 min) |
| 1b Safe mode without rollback machinery | Medium | Gate run |
| 1e launcher hazards | Small | Online smoke |
| Two-PC Safe test on the lean build | — | Both players |
| 1d RollbackManager split (incremental) | Medium–large | Gate run per slice |
| B1, B3, B4, B6 | Small–medium each | Two-instance runs |
| B7 real-flow harness | Medium | PC |
| B2 pre-fight hello | Medium | PC, then both players |
| B5 host picks for both (only if needed) | Medium–large | PC, then both players |
| Phase 3 lean rollback + promotion | Medium + soak time | PC, then both players |

**Not doing:** a rewrite from scratch, new features, or more fixes on the MK9-online path beyond the state-8 gate
until Phase 1 is in.
