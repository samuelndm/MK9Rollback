# Load refusals: what they are and how to remove them (static analysis, 2026-10-14)

GGPO assumes that `load_game_state` always succeeds. This note covers every refusal that occurred in soaks 20–31, what triggers it, and how each could be removed.

Sources:
- the `ROLLBACK N refused:` lines in `results/soak2*`, `soak3*`;
- the code paths `RollbackManager::DoRollback`, `ObjectHooks::CanLoadTo`, `Ue3Refs::TornDownSince`, `PhysWatch::EventsSince`;
- disassembly of the MKKE.exe analysis copy.

This is static analysis plus log mining. Nothing here was run in the game.

## Counts (soaks 20–31, ~1,100 performed rollbacks)

| Refusal | Count | Notes |
|---|---|---|
| `no snapshot for gf=N` | 15 | Every one follows `timeline restart at gf=1` (fight ended) |
| PhysWatch: `UE3 actor X destroyed ... by 004BB857` | 9 | Cinematic spawned actor |
| PhysWatch: `... by 007AEC5C` | 2 | LifeSpan expiry |
| `CanLoadTo`: actor-owning MK object died | 0 | ~20 per soak before ActorDefer (soaks ≤ 19), none since |
| `Ue3Refs::TornDownSince` | 0 | |
| `object history does not cover frame` | 0 | |

## 1. "no snapshot for gf=N": fight boundary, a harness artifact

- **What happens.** The attract demo ends its fight, and `GameFrame` restarts at 1 (`ROLLBACK timeline restart at gf=1 (previous 3630)`). Saves are then refused with `object heap not discovered` until the next fight's MK object heap exists. The harness kept sending rollback requests across that boundary.
- **Not a restore defect.** There is no snapshot to load because there is no fight.
- **What GGPO needs:**
  - A session must only span in-fight frames.
  - Find out whether `GameFrame` also restarts between **rounds** of one match. Attract demos are single-round, so the logs don't answer this.
    - If it does restart, GGPO's frame numbers must come from our own monotonic counter, not `GameFrame`.
    - The rollback ring must also not straddle the restart.
  - **Static answer on round boundaries** (to be confirmed with a 2-round fight in-game). GameFrame (`[0xDEF4B4]+0x70`) has exactly two writers in the binary:
    - `0x404E12`, the main loop's `inc` once per frame;
    - `0x884826`, which zeroes it inside `MKOnlineGame` "StartSendingGamepadData (RandSeed:0x%08X)" (`0x884770`).
      - Callers: the online object `[0xE924F0]` (`0x81C09A`) and the `MKOnlineGame` connection-sync handler `0x885340` (vtable `0xD68A78` slot 20, log string "MKOnlineGame::ConnectionSyncCompleteHandler"), for sync states 1 and 4.
    - No offline code writes it. The attract-mode "timeline restart at gf=1" therefore most likely comes from the GameInfo object being recreated at map load.
    - So **GameFrame should keep counting across rounds of one match** (same map), and the snapshot ring can span a round transition. The bigger risk across rounds is the round-reset logic itself (positions, life and meter reinitialised). It is just more simulated frames and should resimulate like any other frame.
  - **MK9's own online lockstep.** `MKOnlineGame` exchanges gamepad data and agrees an RNG seed (`RandSeed`) when the connection syncs. A GGPO session must:
    - not use that path (and never touch ranked);
    - make both peers seed RNG A (`[0xE7C054]`) identically at session start, for example the host's seed sent in the GGPO session setup.
  - Proposed rule for non-fight frames (round intro, KO, outro): **both peers send a zero input mask**. GGPO's prediction (repeat the last input) is then always right and never triggers a rollback into a frame that has no snapshot. Combine this with refusing to save there, which is what happens today.

## 2. PhysWatch `0x7AEC5C`: LifeSpan expiry of non-MK actors

**Code path:**
- AActor tick (around `0x7A8B75`): `LifeSpan` (`actor+0x94`) -= dt.
- When it expires, and `(actor+0x80 & 0x11) == 0` (not flagged no-delete), the actor is appended to the pending-destroy array (data `[0xE88F24]`, count `[0xE88F28]`), and bDeleteMe (`0x20`) is set.
- World tick `0x7AEC37..0x7AEC63`, right after the async-task wait at `0x7AEC27`:
  - For each queued actor: clear `0x20`, call `DestroyActor(actor, 0, 1)` (call at `0x7AEC57`, return `0x7AEC5C`).
  - Then set the count to 0.

**Key fact: these actors are never MK-owned.** MK flags the actors it owns with `0x11` (no-delete) at `actor+0x80`, and the expiry check skips flagged actors. So everything destroyed here is a plain UE3 actor with a LifeSpan, typically an emitter or decal. Restored MK objects don't own it. Its LifeSpan isn't snapshotted either: the actor isn't in any span.

**Options, cheapest first:**

1. **Refuse only if the snapshot references the actor.**
   - Change: replace the blanket PhysWatch refusal for this caller with a scan of the snapshot about to be loaded.
   - Its spans (actor0/1, xblocks, ue3cells, Tier E, script modules, the master image) are checked for the destroyed actor's address as an aligned dword. Only a hit refuses.
   - Cost: one `memmem`-style pass over the saved bytes per destroyed actor in the window; usually zero or one actor.
   - Expected outcome: no refusal. These actors aren't referenced by restored gameplay state, and attachment lists detach on destroy (`SetBase(NULL)` in DestroyActor).
   - Effect on gameplay: none expected, since the actor is cosmetic. It is simply gone in the resimulated timeline instead of expiring at frame g.
2. **Defer, like ActorDefer.** Patch the call at `0x7AEC57` to a stub that records `{actor, vtable, name, gf}`, sets `LifeSpan` (`+0x94`) to 0 so the tick won't re-queue it every frame, and does not destroy.
   - Release (no snapshot predates the deferral): call `DestroyActor` for real.
   - Load to S ≤ deferral frame: give it back with `LifeSpan = (gfDeferred - S) * dt` (approximate), so it expires again in resimulation.
   - Cost: expired effects linger up to 11 frames (~183 ms) unless they are also hidden. A StillOurs check is needed, as in ActorDefer.
   - Only worth it if option 1 still refuses in practice.

## 3. PhysWatch `0x4BB857`: MwyCinema spawned-actor teardown

**Code (`0x4BB820`, reached only through vtables):**
```
if (this+0x88 & 1) && this+0x3C:
    actor = this+0x3C; actor+0x80 &= ~0x11   (clear no-delete)
    DestroyActor(actor, 0, 1)                (call 0x4BB852, return 0x4BB857)
    this+0x88 &= ~1;  0x79AFD0(this, 0)
```
- **Owner classes:** slot 53 of five vtables, `0xC620D8 / 0xC621B8 / 0xC62298 / 0xC62378 / 0xC62458`, set by InternalConstructors `0x416020..0x4160A0`. The class registration strings name them `CinemaGroupInstSpawnedActor`, `…SpawnedCamera`, `…SpawnedEmitter`, `…SpawnedSkel` and `…SpawnedStatic` (MwyCinema).
  - So this is a **cinematic group instance destroying the actor it spawned when the group ends**: X-rays, throws, intros and similar in-fight cinematics.
- **The bigger problem is that cinematic state isn't restored at all.**
  - The load-only memory diff (`tools/unrestored.ps1`, 2026-10-14) listed `CinemaTrackInstAttachment` (10 changed 64-byte chunks), `SeqAct_Cinema` (6) and `CinemaGroupInst*` among the owners of changed, unrestored memory.
  - A rollback across a cinematic start, step or end resimulates against cinema objects in their live (abandoned-timeline) state. This is probably also a mismatch source whenever a window overlaps a cinematic.
- **Options:**
  1. **Short term:** the same "refuse only if referenced" scan as in section 2 (spawned cinema actors may be referenced by MK script data; the scan will show it).
  2. **Proper fix:** snapshot the MwyCinema instance graph (`SeqAct_Cinema`, `CinemaGroupInst*`, `CinemaTrackInst*`) as Tier E-style UObject property blocks, with the pointer-safe merge.
     - Defer the spawned-actor destroy by patching `0x4BB852`, as in ActorDefer.
     - Give-back then needs no special handling: restoring the group instance restores `+0x3C` (actor) and `+0x88` bit 0 (owns it), and the deferred actor is still alive.
     - Release after the window as usual.
  3. **Measure first.** Tag rollbacks whose window overlaps a cinema event and compare their mismatch rate with the rest. If cinematics account for many of the S+1..S+8 mismatches, option 2 is also a Gate E item.

## 4. `CanLoadTo` (MK object with an actor died, not deferred)

- Zero since ActorDefer.
- It can still fire when ActorDefer is bypassed:
  - a death in a tick without a fight update (`!FoughtThisTick`, by design for teardown);
  - a deferred actor that destroyed itself meanwhile (StillOurs: bDeleteMe).
- **For GGPO:** keep it as a session-fatal error event (log plus desync report), and watch its count in long synctest runs. If it fires, extend ActorDefer to self-destroying deferred actors: track their own destroy calls, as PhysWatch does, and give them back too.

## 5. `Ue3Refs::TornDownSince` (class-A object holding MK objects torn down)

- Zero in soaks 20–31.
- Class A (AnimSequence lists) is torn down at level and fight teardown, which section 1 already excludes from sessions.
- **For GGPO:** session-fatal if it ever fires mid-fight.

## Summary for Milestone 12

| Refusal | Proposal | Effort |
|---|---|---|
| No snapshot / fight boundary | Sessions only in fights; zero inputs on non-fight frames; GGPO frame counter independent of `GameFrame` | Small |
| `0x7AEC5C` LifeSpan expiry | Refuse only if a snapshot span references the actor | Small |
| `0x4BB857` cinema spawned actor | Same scan now; snapshot MwyCinema instances and defer `0x4BB852` later | Small now, medium later |
| `CanLoadTo` / `TornDownSince` | Session-fatal events, monitored in synctest | None |
