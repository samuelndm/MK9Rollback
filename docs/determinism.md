# Determinism notes

## Result 1: Practice-reset replay, 8-float state (run4, 2026-10-13)

**Setup:**
- Scorpion vs Sub-Zero, Practice mode.
- Each trial:
  1. P1 sends mask `0x200` (Practice reset) for 2 frames.
  2. 90 frames neutral.
  3. A fixed scripted P1 sequence: walk, four attacks, jump, `0x28`, `0x2000`, back, `0x9` and so on.
  4. In variant B, P2 also gets a script (walk, `0x20`, `0x8000`, `0x80`).
- All input is injected per tick by `Input::OverridePlayerInput`.
- Script: `tools/determinism_trial.ps1`.

**Compared:** raw IEEE-754 bits of P1/P2 life and P1/P2 xyz, every fight frame.

| Variant | Trials | Frames compared | Mismatching frames | Stamp |
|---|---|---|---|---|
| A (P2 = Practice dummy, idle) | 3 | 820 (frames 1..820) | **0** | `20261013-215828` |
| B (P2 scripted too) | 3 | 822 (frames 1..822) | **0** | `20261013-215946` |

**Content checks:**
- Trials included three landed hits on P2 (life 1.00 → 0.97 → 0.92 → 0.87).
- P2 movement driven by its own injected input (variant B).
- Frame 0 (the reset frame itself) differs, as expected: trial 1 began from a different pre-reset state than trials 2 and 3, yet all trials matched from frame 1 on.

**What this shows:**
- From the Practice-reset state with identical per-frame inputs, the observed fighter state evolves bit-identically.
- So far no wall-clock or delta-time dependence is visible: dt was the constant `0x3C888889` throughout.

**What this does NOT show:**
- **Coverage:** only 8 floats are compared. Animation, script VM, move and hitstun state, RNG, meter, projectiles and PhysX are not hashed.
- **Restart:** a full match (round intro, round 2, timer) started from the menus is not covered.
- **Randomness:** randomness-heavy moves, projectiles and stage interactions are not covered.
- **Rollback:** this is not a restore/resimulate test. Nothing was saved or restored. That is Gate D.

## Result 2: region hashes and RNG (run6/run7, 2026-10-14)

**Hashing:**
- `StateHash` computes xxHash32 per region at FightUpdatePlayers entry. Regions:
  - GameInfo `0xE89B70`, 0x20C bytes, with HostFrame/GameFrame masked out.
  - PlayerInfo[0..3], 0xC8C each.
  - P1/P2 char object `[0..0x78C8)`.
  - P1/P2 pad `[0..0x5F8)`.
  - RNG `0xE7C054`.
- The RNG draw count is derived by stepping the LCG from the previous frame's state.
- Raw region dumps were taken at trial frames 1/2/100/400/800 (`dumpafter`), then analysed with `tools/dumpdiff.mjs` and `tools/fieldclass.mjs`.

**Without a common seed** (stamp `20261014-013506`):
- **What matches:** inputs and the 8 fighter floats.
- **What differs:** every frame of pinfo0/1, char0/1, pad0/1 and rng differs.
- **Why the RNG differs:** Practice reset (`0x200`) does **not** reseed the RNG.
- **Draw rate:** about 1 draw per fight frame (max 2–3).
- **RNG reaches character state:** `char1+0x2D4` (float) matched at frames 100/400 but diverged at 800 (67.5 vs 136.6).

**With `rngset 1234ABCD` at trial start** (stamp `20261014-013707`, both players scripted):
- **Matches:** RNG state and per-frame draw count are **identical in all 3 trials for all 820 frames**. Fighter floats are identical too.
- **Gone:** the `char1+0x2D4` divergence.
- **Conclusion:** RNG consumption is input-deterministic, and the RNG feeds character state.
- **Remaining byte differences** (trials 2 vs 3, frames 1/100/400/800). None is a trajectory divergence:

| Kind | Where | Interpretation |
|---|---|---|
| Rotated ring buffers | pad +0x58/+0x238/+0x418 rings; PlayerInfo +0x14C… (0x2C-byte records holding −0.03/−0.05 = damage dealt, frame stamps, object handles) | Same events in different slots, because ring write indices differed at trial start. |
| Absolute event timestamps | char +0x2D0 (float), +0x2F78/+0x3000/+0x3008/+0x300C (int) | They run on a clock that drifted 2 ticks against GameFrame between trials (identity of that clock: TODO). |
| Cumulative counters/accumulators | char +0x2F70/+0x2F7C (counts), +0x3014/+0x3018 (floats with constant offsets −0.87 / +11.1) | Not reset by Practice reset. |
| Dynamic sub-object pointers | char +0x34/+0x38/+0x184/+0x2B4/+0x2F1C/+0x2F20 | Spawned per action. Allocation identity differs, so these are **not restorable by value**. |

**Implication for Gate C:** in-process trials cannot reach byte-identical hashes, because the start state is not byte-identical (rings, counters, clocks, allocations). A strict pass needs either:
- a cross-process replay (restart the game, reproduce the menu path, record and replay a full match), or
- a real snapshot/restore (Gate D) so trials start from the same state.

The gameplay-relevant observables tested so far all match exactly: positions, life, RNG stream and RNG-dependent character fields.

## Gate D experiment 1: partial in-process save/load (run11, stamp `20261014-014853`)

**Setup:**
- **Snapshot:** `Snapshot::Save/Load` at EngineTick entry. 10 spans, 73 KB:
  - RNG.
  - GameInfo head, including GameFrame; HostFrame is kept live.
  - PlayerInfo[0..1].
  - pad0/pad1 `[0..0x5F8)`.
  - char0/char1 `[0..0x78C8)`.
  - actor0/actor1 `[0..0x300)`.
- **Never written back:** pointer fields (533 dwords at save time). Pointer test: value < 0x30000000 and committed private, or target's first dword is a vtable in the MKKE image.
- **Sequence:** seed the RNG, Practice reset, save at frame 120, run sequence S (284 frames: FP, BP, jump, BK, walk), load at frame 404, run S again. Compare W1[k] with W2[k].

**Results:**
- **No crash:** the game kept running; loaded 10/10 spans, verified 3 times across runs 9–11.
- **k = 0 and 1:** fighter floats, PlayerInfo, pads, GameInfo and RNG are identical. Input rings and command state restore correctly; PlayerInfo and pads stay identical for the whole window.
- **k = 1, char0:** only new sub-object allocations differ (`+0x38`, `+0x184`, `+0x2B4/+0x2B8`, `+0x2F44/+0x2F48`), plus a global handle generation (`+0x188`) and allocation serial (`+0x18C`). Expected: the allocator and the global id counter are outside the snapshot.
- **k = 1, char1 `+0x2D4`:** 107.1 vs 162.7, written during frame 0 from state outside the spans, though the RNG was identical. Candidate: script VM storage.
- **k ≥ 2:** P1's x is a constant ~0.28 behind (−124.438 vs −124.157) from the first frame of the attack. Then it drifts: at k = 283 P1 is at 18.19 vs 19.74 and P2 at 126.55 vs 125.28. Candidate: animation/root-motion state in sub-objects (anim tree / SkeletalMeshComponent), not restored.
- **k = 42:** the RNG stream diverges (a different code path draws).

**Verdict:**
- Restoring flat fighter/match/input memory is safe for these spans and gets the first frames exact.
- Gate D needs the fighter's animation and script-VM state, plus correct pointer/type information (heap addresses overlap float bit patterns here, so heuristics are unsafe).
- The next step depends on the static state-ownership map (`docs/state_ownership.md`).

## Gate D experiment 2: crash (run12, 2026-10-14 01:51)

**What happened:**
- The same partial snapshot was used. Differences from run 11:
  - The game sat paused about 3 s right after `save` (external full-memory read).
  - The `load` was followed by `pause 1` at the end of the same tick.
- MKKE exited without a Windows error report or UE3 log, right after `load` (last logged fight frame is the one before the load).

**Most likely cause:** restoring a raw heap pointer the strict rule did not classify as a pointer. For example, a TArray data pointer above 0x30000000 whose target has no vtable, restored after the array was reallocated.

**Mitigation (built, not yet run-tested):**
- Load now keeps a dword if it is pointer-like in **either** the saved bytes or live memory.
- UE3 `TArray {data*, num, max}` triples are kept as a unit, so array contents are not rolled back yet, but a restored count can never exceed a reallocated buffer.

**Lesson:** blind span restore is unsafe in a way that shows up only intermittently. Typed field maps (`docs/state_ownership.md`) must replace heuristics before Gate D can pass.

## Gate D experiment 3: results (runs 13–15, 2026-10-14 03:26–03:28)

**Heap discovery:**
- The object heap is a sub-range of the 12 MB master reservation `0x10970000`.
- Its GMalloc object `[0xED53F4]` stores base at +0x34 and size at +0x38. Observed: base `0x109EFFC0`, size `0x180000`.
- Every active pool's free-list head lies inside it, and so do both fighters.
- Snapshot size: 30 spans, **1,620,592 bytes**.

**Idle restore** (`restore_trial.ps1 -Ab -Idle`: save at 120, 60 neutral frames, load, 60 neutral frames):

| Variant | Result |
|---|---|
| HostFrame kept live (run14) | Fighter floats and both RNGs identical for all 60 frames. The mkheap image differs on 59/60 frames: 24 dwords in MKScriptVM records hold **HostFrame** stamps off by exactly 60, the window length. |
| HostFrame restored too (run15, stamp `20261014-032720`) | **Every hashed region bit-identical on frames 2..59**: full 0xC580 fighters, 1.5 MB mkheap, full GameInfo, pads, RNG A/B. Frame k = 1 differs in exactly one field: `char+0x2D4` on both fighters (107.1 vs 161.1, also visible through the heap image). It self-corrects at k = 2. Writer and source are TODO. |

**Attack sequence** (`restore_trial.ps1 -Ab`):
- The load was **refused by the churn guard** with no crash.
- The id counter went `0x10056A3B → 0x10056AB7`: **124 MK objects created in 284 frames** of FP/BP/jump/BK/walk, about 30 per attack.

**Conclusions:**
- Tier A+B is the right state model: with no object churn, restore is exact.
- Rollback across any attack needs churn handling:
  - defer UE3 actor destruction during the rollback window;
  - destroy actors whose MK objects were created after the save;
  - re-link restored objects to their still-alive actors.

  This is the next engineering step.

## Milestone 10: local rollback engine (runs 16–48, 2026-10-14)

**Driver:** attract-mode AI-vs-AI fights (`tools/attract_rollback.ps1`). They need no input and keep running while the PC is locked. Each request is an identity rollback of depth 1–10, checked by a built-in exactness probe: the live Tier A+B+C state at F is compared before the rollback and after resimulating back to F.

**Architecture** (`src/rollback/RollbackManager.cpp`):
- **Boundary:** entry of engine vt+0xBC (`0x0080DD40`, the UE3 world tick). That is the settled point: scheduler and physics work are joined and the frame's world tick has not dispatched yet. The C0 entry used before was *after* async work was dispatched, and its snapshots were inconsistent.
- **Saving:** a ring of 12 snapshots, one per frame. Inputs are recorded per frame.
- **Resimulation:** each resimulated frame runs vt+0xBC + vt+0xC0, then advances HostFrame/GameFrame exactly as the main loop does.
- **Cost:** about 2 ms per resimulated frame; a 10-frame rollback takes 20–25 ms.

| Problem found | Evidence | Fix |
|---|---|---|
| World clock outside the snapshot | timer-like floats off by one frame | WorldInfo +0x280..0x28F and +0x394/+0x398 found by tick-to-tick diff (`wiprobe`). Getter `0x6D5430`. A blanket WorldInfo restore crashed (run23). |
| Render-side actor fields and the FX RNG are not simulation state | actor +0xAC (render time), +0x80 bit 0x40; RNG B | kept live, never compared |
| GameFrame reset/jump at fight start (attract demos reuse gf values) | stale snapshots / history | timeline epoch; ring, object history and quarantine restart |
| Object events between frames stamped as the next frame | anim-node deaths outside a phase | flush object events at the boundary, stamped F−1 |
| Animation work on OpenMP-style worker threads inside C0 | 73 anim-node deaths on non-game threads, all while the game thread was in C0 | boundary still valid; pool free-list order is nondeterministic (also without rollback), so raw heap bytes cannot be compared, only gameplay state |
| **Freed external memory reused under restored objects** | EIP in heap after resim (run31) | **free quarantine:** GMalloc outer vt+0x10 / inner vt+0x18 (`0x405AF0`); while rollback mode is on every free is deferred until no loadable snapshot predates it. Keyed by pointer (a replayed free must not double-free, run38). Frees stamped ≥ S are un-freed on load. |
| MK anim nodes own GMalloc arrays mutated in place (blend children at +0x3C, base array at +0x08) | worker null deref at `0x4D1ACB` (run34) | typed Tier C: walk the global MK object list `0xDE8214`, type via vtable→descriptor, snapshot those blocks (size from the guarded block lookup `0x405A10`). A generic {ptr,num,max} scan restored UE3 objects by mistake (run37). |
| Block lookup not safe on arbitrary pointers | AV in `0x405A10` | vectored-exception-guarded probe (`CrashLog::SafeBlockInfo`) |
| UE3 objects holding raw MK pointers (class at InternalConstructor `0x5DA820`, vtable `0xD3EAC0`, `[+0xB4]`→holder) | delete through a freed MK slot (`0x7623BD`, runs 41/43/45) | `Ue3Refs`: track instances from DLL init, snapshot holder cells, and on load clear holders of instances created at/after S or pointing to invalid objects |
| Master MK heap header restored without its 12 MB region | OOM error path `0x42B4F5`→`0x406160`, game frozen (watchdog, run46) | superseded (run49+): the master heap's committed ranges (minus the asset sub-heaps `[0xED53FC]`/`[0xED5400]`, about 10 MB) are restored as one image together with both heap headers and the 13 pools (all pool storage lies inside it). Master frees are no longer deferred. |
| Post-restore "retire" of resurrected effects via their deleting destructor | crash in a resimulated MKObj update (run47) | removed: a rollback whose window contains the death of any actor-owning object is refused |
| Init race: hooks live before the command channel existed | AV in EnterCriticalSection at injection (run44) | everything the hooks use is initialized first |

**Diagnostics added:**
- `CrashLog`: vectored handler with breadcrumbs, plus a 3 s hang watchdog that logs the game thread's EIP and stack.
- `memscan`: page write-set, full snapshots, pointer walks, and `xrefs` (external cells pointing into the MK heap).
- Probes: `whatis`, `wiprobe`, `animprobe`, `skelprobe`, `listtypes`, `gmalloc`.

**Status:**
- Identity rollbacks work and survive dozens of requests in a row on live demo fights. Depth reached 10.
- They are not yet stable indefinitely; every remaining crash so far was a new state-ownership leak with a clear signature.
- The exactness probe still reports differing pools, lists and anim arrays. These are expected (thread-order nondeterminism and pointer identities), so a gameplay-level hash is needed as the Gate E criterion.

## Milestone 10 stabilization: soak runs (2026-10-14 06:00–07:45)

**Method:** `tools/soak.ps1`: fresh Steam launch per session, injection before the first demo fight, then 60–80 identity rollbacks of depth 1–10 at ~1.2 s intervals, 30 s observation, liveness check. Evidence in `results/soak1-classA-list-crash`, `results/soak2-list-gaps`, `results/soak3-clean`.

| Crash signature | Root cause (verified by disassembly + runtime) | Fix |
|---|---|---|
| `0x761411` write to `[0+4]` in class-A list unlink `0x7613C0` | the class-A list (`+0xB0` object: head `+0xB4`, tail `+0xB8`, count `+0xBC`) is **doubly linked**, nodes `{value, next +4, prev +8}`; only 8 bytes per node were saved, so `prev` went stale | save 12 bytes per node; `FixDangling` relinks survivors keeping next/prev/tail/count consistent |
| `0x42D9B6` null vtable call in the MK node-list sweep `0x42D950` | the pending-sweep array `0xE393D8[64]` (count `0xE394DC`) names lists anywhere; the global list at `0xE394F4` and flags `0xE394EC/F0` were outside the `mkobj_unlink` span; list `0xDE8224` had only its head covered (`mkobj_globals` stopped at `0xDE822C`) | spans extended: `0xDE820C+0x2C`, `0xE393D8+0x130` |
| `0x42AF1D` MK heap free-tree write through an id-like value | consequence of the same list inconsistency (an object destroyed twice) | same |
| `0x7145BD` GMalloc small-block pop through a garbage link (allocator `0xE0AC58`, bins `+0x498` stride 0x10, count `+0x7CC`) | not yet pinned; a pre-load free-list check (`Snapshot::CheckFreeLists`) found **no** free block inside any restored span in 120+ rollbacks, so restores do not write into freed GMalloc memory. Not seen after the list fixes. | diagnostic kept |
| stack overflow at a prologue `push` (`0x78907F`, WER only; the VEH had no stack) | unknown recursion, once | `SetThreadStackGuarantee(64 KB)` on the game thread so the handler can log it |

**soak3-clean (list fixes):** 3 sessions, **110 rollbacks performed, 0 crashes, all alive**; 78 gameplay-exact, 32 gameplay mismatches. Refusals (≈40%) are almost all "actor-owning MKParticle/MKFXLight died in the window".

**Gameplay mismatches, investigated:**
- dt is a fixed 1/60 (`0x3C888889`) on every frame (1967/1967 frames): not a variable-timestep problem.
- Restoring the fighters' SkeletalMeshComponent bone arrays (fighter `+0x10C`, vtable `0xD5A388`, 189 bones: `+0x2C8` ×40 B, `+0x2F4/+0x300/+0x30C` ×64 B) did not change the mismatch rate (16/40 vs ~30%). Kept (pure data, cheap).
- Page diff of the image pages written per frame (`tools/pagediff.ps1`) found **the MK tick-group lists `0xE39E00..0xE39FA4` (21 lists × 0x14) and the object refs `0xE39FA4`/`0xE39FB0` `{vt, obj, id}` outside the snapshot**. They are now in Tier A (`mkobj_groups`). Other per-frame image writes: `0xDF0C28/2C` (wall-clock floats, advance while paused: render time) and `0xE7C0FC` (UE3 core counter).

### Effect-object deaths: ActorDefer (2026-10-14 08:20–09:20)

Refusals (~40% of requests) came from rollbacks whose window contained the death of an MK object owning a UE3 actor (MKParticle 234, MKFXLight 30, MKMaterialImpactLocator 5 in one session). The MKObj destructor `0x4399F0` does, for `obj+8 = actor`:
- `0x433E40` (if `actor+0x60`): detach from the parent MK object, `actor->vt[0x190](NULL, …)` (SetBase-like)
- `0x433CF0(1)`: children (destroy or detach)
- `0x5EDAB0`: clear RF_RootSet `0x4000` in `actor+8` when `obj+0xC`
- **`GWorld->DestroyActor(actor, 0, 1)`** at call site `0x439A70` (callee `0x79D960`) when `obj+0x2C` (owns the actor)
- `0x42D650`: reset the actor's back reference `actor+0x9C {vt, obj +4, id +8}`

`src/rollback/ActorDefer.cpp` patches the call site (5-byte `E8` through `CodePatch`, the rel32 is not 4-aligned). An asm stub passes the dying object (caller ESI) in EDX. While frames are being snapshotted **and the fight body already ran in the current engine tick**, the destroy is recorded instead of performed: actor, owner, id, the 12 back-reference bytes, root flag, GWorld, gf. Releases happen once gf < F−11. A load to S gives back deaths at gf ≥ S (back reference and root flag rewritten). `CanLoadTo` only refuses deaths of actor-owning objects whose actor was not deferred.

Pitfalls found on the way:
- PreRestore destroys post-snapshot objects. That **cascades** into pre-existing children, which the restore then resurrects. Only owners born at/after S may really lose their actors (`SetBypassOwners`); otherwise a resurrected child pointed at a destroyed actor (null scene at `0x43BB66`).
- **End-of-fight teardown must not be deferred.** It runs in an engine tick without a fight update. Deferring it, then either dropping or destroying those actors at the next timeline start, crashed the next level start in the registry walk `0x71C900`→`0x71C510` (`0x71C517/0x71C530/0x71C53C`). This reproduced with rollback mode on and no rollback at all. Every normal effect death happens in phase 2 after the fight update, so "fight body ran this tick" separates the two cases. After that change, two consecutive demo transitions ran clean.

Result (soak6/7/8): refusals went from ~40% to ~0%.

Still open:
- the residual allocator signature `0x7145BD` (small-block bins; seen once on a non-game thread after a 9-frame rollback that destroyed 9 objects)
- gameplay mismatches (~35%), now logged per fighter field. First cases point at per-object counters such as fighter `+0x2D4` being two frames ahead after resimulation.

### Allocator crashes, root causes (soak 9–13, 2026-10-14 09:40–11:45)

| Signature | Root cause (evidence) | Fix |
|---|---|---|
| `0x42AF1D` MK heap red-black free tree (color `+0x14`, children `+0xC/+0x10`, parent `+8`) with a link holding an MK object id; the heap is the **object heap** | (1) Heap headers are `0xC0` bytes (per-size bin state from `+0x3C`, all -1/0); only `0x3C` were restored. (2) A rollback gave back deferred actors whose MK owner was **born after S**: the live actor's back-ref `{vt, obj, id}` then pointed into object-heap memory the image had made free, and later writes through it landed in the free tree. (3) Actor merge kept live pointer-like dwords even when they pointed into the master image. | restore headers `0xC0` and pools `0x50`; `ActorDefer::OnLoad` destroys actors of owners born ≥ S (birth frame from `ObjectHooks::BirthGf`); actor merge restores any kept dword whose saved or live value points into the master image |
| `0x7145BD` small-block pop through a garbage link, always **bin 29 (625–704 bytes)**, garbage = position-like floats | Bin watcher at four points per frame plus a release ring caught it: block freed by the game at gf 877, really released by the quarantine at gf 889, and **written as a live object** (vtable `0xD89FAC`, ctor `0xA38A40`, behind a middleware block header) at gf 890, with **no rollback in between**. Third-party code (statically linked after the CRT, `0x98C000+`) relies on the allocator's immediate LIFO reuse of a freed block; deferral breaks that. Top deferred-free callers in a fight also include `PhysXCore.dll` (`0x09600000–0x09981000`). | frees called from outside MKKE.exe are not deferred (PhysX state is never restored and rollbacks across rigid-body changes are refused by `PhysWatch`); a thread-local bypass keeps the owner-level hook from deferring what passed through. The in-image middleware callers are next: they should be classified and passed through the same way. |
| crash in PhysXCore during a resimulated frame | `0x6D2900` terminates a component's rigid bodies (`+0x12C` handle, `+0x124` index); `0x74FB20` processes the scene's pending-release list | `PhysWatch` hooks `0x6D2900` (7-byte trampoline) and refuses rollbacks across such events (none seen yet in fights) |
| `0x8A753A` reads a zeroed vtable through `[obj+0x1D8]` (obj class `0xD6A9B0`), MSVC R6025 "pure virtual call" dialog (lag_ns40_d1, 2026-10-28) | The class's destructor `0x895BA0` queues `+0x1D8` on the engine's pending-release list (`0x74FA10`, kind 1 = GMalloc free after a 6-update countdown; performed by `0x74FB20`). The list is live across loads: a record queued in the abandoned timeline fired during the replay while the restored object, still alive in the replay, used the block. The quarantine released it 12 frames later. | `RecordPendRel`/`RestorePendRel`: the list is recorded at every save; at a load, kind-1 records the save does not have are removed (`pendrel`). A release of the save that was performed after it is not re-queued (the block leaks). Verified running 2026-10-28/29: soaks 12501-12504 and 12601-12612 (14,120 loads, 0 crashes; ~20 records removed and 8-10 blocks leaked per session), lag_ns40_d1 rerun passes. |
| hang: game thread in a render fence (`0x661BBA` → `0x511A90` 2 ms event waits) | render thread blocked | the watchdog now reports every thread (captured while suspended, logged after resume) |

**Coverage additions:** `extlist` (MK node lists embedded in non-MK objects, found every save through the node pool `[0xE394E4]` (live vt `0xC66B14`, list at `+0x14`); lists created after S are emptied on load); `mkobj_lists2` `0xE39CD8/0xE39CEC`; `mkanim_list` `0xE3D7E0` (+ counters `0xE3D7F4/F8`, flags `0xE3D804`); `mkobj_ticklist` `0xE0B504` to `0x14`; `script_scratch` (the `0x8000`-byte MKScript executor scratch at `[0xE39FC4]`).

**RNG A trace** (`RngTrace`: entry hooks on RandInt `0x5F5080` and RandFloat `0x5F50A0`, the only code touching `0xE7C054`; draws through RandomInt `0x492050` are attributed to its caller). In attract fights the first diverging draw after a load comes from the MKScript native random (`0x8B87B0`, return `0x8B87C1`) and AI natives (`0x88D03B`, `0x88D4F9`, `0x88E15B`). AI is an **AIDrone** MK object (`0x114` bytes, vtable `0xD69818` with only type/dtor slots) run as script natives. It writes the AI pads (`0xE27650`/`0xE27C5C`, index 14/15) at `0x8895FF` (found with a hardware write breakpoint, `hwbp`) and times itself off GameFrame. So the AI's divergence comes from inputs it reads that are not restored.

**Performance** (diag off): load ~2.5 ms, save ~2.1 ms/frame, resimulated tick ~1.1 ms/frame; 1-frame rollback ~3.6 ms, 10-frame ~32 ms. A per-node `VirtualQuery` in the external-list walk had made every save ~10–13 ms (soak 8–12).

**Test harness note:** hardware pad records `0xE221A8 + i*0x60C` (Practice P1 = index 0, flags `0x105`; AI pads 14/15 with flags `0xC`). Writing pad 0 with the external flag (`hwhold`) is seen by the front end: one attack press ended an attract demo. The PC is locked, so screen capture shows only the lock screen and menu navigation would be blind.

### Gameplay mismatches: tracing them to unrestored state (2026-10-14 12:00–14:40)

**Test without AI** (`tools/noai_rollback.ps1`): `killai` destroys the two AIDrone objects in an attract fight, then both fighters are driven by random scripted pad input (`hold`, recorded per frame and replayed in resimulation). AI script processes keep running natives after their drone is gone, so this is "mostly human". Destroying their executors too did not stop it and crashed a resimulation once.

**Load-only diff** (`tools/unrestored.ps1`): pause at S, full writable snapshot, step, `loadonly 1` (restore S, no resimulation), full snapshot, `memscan rbdiff`, owners labelled by `describefile`. Result: ~4,200 pages changed by one tick and not put back. Most are unattributable render/allocator memory; the identified owners include ParticleSystemComponent, FmodAudioComponent, fighter SkeletalMeshComponents, SkelControls, WorldInfo, CinemaTrackInstMove.

**MKScript native trace** (`RngTrace`): dispatch at `0x45C9FF..0x45CA09` (`call edi`, esi = executor, result at executor `+0x1C`) is hooked; each resimulated frame's (native, result) sequence is compared with the original. Opcodes: `0x4596D0` call native (operand 1 = native, operand pointer `+0x3C` advanced by 4), `0x4596B0` jump, `0x4597E0` decrement variable. Variables (`0x458930`): bit 31 → context variable table `[ctx+0xA0]`, bit 30 → `[ctx+0x84]+off`, else executor locals `[exec+0x10]`. Context = `[0xE39DF8]` = executor `+0x24`.

| Diverging native (first in frame) | What it reads | Fix |
|---|---|---|
| `0x4597E0` decrement, results off by k | script **context data** `[ctx+0x84, +0x88)` and variable table, inside the **script module allocation** (base `ctx+0xAC`, size `ctx+0xB0`, 6 KB–336 KB) outside the MK heap | `script_module` spans: every live MKScriptVM's context (+0x24), plus contexts seen at dispatch. Data-only first, then whole module (data-only left object-reference variables stale → a script deleted an object twice, `0x8D10A6`). |
| `0x8C80F0` anim position (→ `0x442370` → channel lookup `0x43B070` → `0x440FF0` float `+0x74`) | fighter (MKSkeletalObj) animation channels: TArray at `+0x1B0` of 0x18-byte records, playback blocks at record `+0x10/+0x14` (GMalloc, reallocated per animation) | xblocks for the channel array and both playback blocks (no stale/free blocks seen by the pre-load check) |
| same native, remaining | UE3 objects instanced into the fighter's SkeletalMeshComponent (Outer = component; found through **GObjObjects `[0xE7C0EC]`/count `[0xE7C0F0]`**): AnimTree, sequence/blend nodes, SkelControls (~50) | **Tier E** (whole objects past the UObject header, pointer-safe merge): anim-position divergences 10–17 → 3 per run. A hang inside middleware during a resimulation followed once; being measured by soak 14. |
| `0x8E16E0`/`0x8E1740` | a static object at `0xE91FF0` (vtable `0xD5E420`, methods `0x91FEC0..0x922ED0`) past the GameInfo span, used by the fight update; its own fields do not change per frame, so the state is deeper | open |

Mismatch rate (attract, AI on): ~30% → 12–17% in single runs after these changes (noisy, ~30 rollbacks per run).

### Soak 14–19 (2026-10-14 14:40–16:00): allocator reuse, UE3 merges, current status

- **Middleware Realloc(p,0) users** (Scaleform-like objects, vtable `0xD89FAC`, refcounted, ~20 holders) and an engine view-like struct (freed by `0x5A0FBC`, a realloc of the global buffer `[0xE590E8]`) keep writing blocks after freeing them, relying on immediate LIFO reuse. Measures:
  - Realloc callers are now tracked (outer `vt+0xC`, inner `vt+0x1C` hooks).
  - Frees from middleware code (`0x98C000–0xC00000`) and from other modules pass straight through.
  - Bin-29 blocks (625–704 bytes) stay parked for the whole fight.
- **Written-after-free detector**: a content hash at deferral, compared at release. It found nothing, so the stale writes happen *after* release, i.e. code relying on "free then reallocate at the same address".
- **Rejected**: parking bin 0 (16-byte blocks) grew the quarantine to about 600,000 entries and made rollbacks ~80 ms.
- **The bin watcher had false positives**: other threads pop the lock-free lists concurrently, so it now re-walks the list to confirm a hit.
- **ActorDefer**: a deferred actor that destroyed itself meanwhile (`bDeleteMe` = actor `+0x80` bit `0x20`, `DestroyActor` `0x79D97E`) is no longer given back; such rollbacks refuse.
- **PhysWatch**: records `DestroyActor` calls from game code in any tick, not only fight ticks.
- **Merge**: a TArray triple `{data, num, max}` is decided as a unit; a restored data pointer with a live count crashed at `0x7892AB`.
- **Tier E** is restricted to SkelControls. The component's ~38 material instances were the TArray victims, and they are render-only.

**soak19** (current build, 3 sessions): 100 rollbacks, **17% gameplay mismatch**. Each session still ended in a crash or hang:
- worker-thread garbage read `0x7795AF`;
- a resimulation hang inside a UI display-tree walk (`0xAA1B10` → `0xA80830`, recursion, middleware).

**Assessment:** two quarantine principles produce the remaining instability:
1. **Defer every engine free.** This changes allocator reuse for subsystems that are never restored (UI, audio, render data), and some of them rely on immediate reuse.
2. **Restore UE3 objects wholesale through a pointer heuristic.**

The next step is to narrow the quarantine to frees that restored gameplay state can reference (MK objects, anim channels and arrays, script modules), selected by free caller or by references from the snapshot, instead of widening the pass-through list case by case.

### Soak 20–27 (2026-10-14 16:00–17:50): quarantine modes; the crashes are UE3 attachment lists

- **Quarantine modes:**
  - Window-only (mode 1) hung the script VM.
  - Caller-based (mode 2) crashed in a resimulation at `0x8A753A`: an MKObj `+0x1D8` actor freed by UObject GC and reused.
  - Kept: mode 0, plus pass-through for the two LIFO-dependent engine sites `0x52EC30` and `0x5A0FBC`.
- **soak26:** 88 rollbacks, ~20% mismatch, one crash per session.
- **The three remaining crash classes have one cause.** The crash handler now dumps the structure at these sites; `contacts`/`hwexec` checked it live. All three crashes read the same 0x60-byte record lists:

  | Site | What it does |
  |---|---|
  | `0x7795AF` | SkeletalMeshComponent method (vt `0xD5A388`, inherited slot 70 = `0x7794A0`); walks the records of its owner (`comp+0x48`) whose `+0x30` is the component |
  | `0x7892AB` | `0x764240` walks `actor+0x68` and reads each attached actor's Components TArray |
  | `0x43E117` | resimulation; `0x7896F0` searches `[actor+0x60]+0x68` for the actor itself and returns null+4 when it is missing |

- **Layout** (runtime check: fighter 1 grabbed by fighter 2 has `pawn1+0x60 = pawn2`, and `pawn2+0x68` lists pawn1):
  - `actor+0x60` = Base. The MK detach `0x433E40` calls `SetBase(NULL)` through vt `+0x190`.
  - `actor+0x68` = TArray of attachment records:
    - `+0x00` attached actor
    - `+0x04` relative location
    - `+0x30` base skeletal component
- **soak27 crash dump (`0x7795AF`):**
  - Fighter `MKPawn_4` (restored span `actor0`) listed two MKParticleActors attached to its mesh, `SkeletalMeshComponent_265`.
  - One of them, `8670DB30`, was freed at gf 3255 through `appFree` `0x81A470` (UObject destruction) while still listed.
  - The quarantine released it at gf 3267; the memory was reused, and the crash followed at gf 3268.
  - The previous rollback was at gf 3247 → 3242.
- **Hypothesis:** an effect actor dies without leaving its base's attachment list. The fighter's list is kept live by the actor merge. Candidates:
  - an ActorDefer give-back or real destroy whose Base field disagrees with the list;
  - a self-destroying particle actor.
- **Test:** an `ATTACH` audit of both fighters, run every frame and after each load, logs the first frame a record names a dead actor, or a Base that does not list its actor. Soak 28.
- **Soak 28 result (root cause):**
  - The audit fired right after a 1-frame load (gf 9221 → 9220): `attached[2] 6BFC6B80 has another Base`. The next tick it reported `dead`. The crash `0x7795AF` followed 35 frames later, on the same record.
  - Sequence:
    1. PreRestore destroyed 4 MK objects born after S. Their destructors detached their particle actors (`SetBase(NULL)` removes the record and shifts the list).
    2. LoadAB then restored the fighter actor span.
    3. The list's TArray `{data, num, max}` had data `0x80EB69F0`. `CommittedHeap` rejected every value ≥ `0x7FFF0000`, so the triple was not recognised as a pointer and was restored by value.
    4. The saved count re-exposed records the detach had shifted away. A destroyed actor was listed again; GC freed it; the quarantine released it 12 frames later.
  - **MKKE is large-address-aware.** The same misclassification hits any Base or actor pointer above 2 GB. That also explains `0x43E117`: a restored Base whose list has no record of the actor.
  - **Fix:** the pointer limit is now `0xFFFE0000`, also in the holder scan and the `uobj` command. Soak 29 verifies it.

### Soak 29 and mismatch tracing (2026-10-14 18:00–18:45)

- **soak29 (fix for pointers above 2 GB):**
  - Sessions 1 and 2 ran clean: 158 rollbacks, no crash, no `ATTACH` inconsistency, game alive at the end. Before the fix, every session crashed within 6–48 rollbacks.
  - Session 3 hung once at gf 2006 in normal play. The game thread waited in the async-task loop `0x661A90` (InterlockedExchange plus a 2 ms WaitForSingleObject).
  - The watchdog's thread dump did not run. Cause: `GetWindowTextA` on a window of the hung thread blocks the caller. Fixed by sending WM_GETTEXT with a 200 ms timeout and dumping threads before windows.
- **The native trace was dominated by sound noise.**
  - Sound natives return a play handle that is left in the executor result slot (`+0x1C`).
  - The store op `0x458AC0` and natives that set no result report that stale handle; for example `0x8B5E00` is a wait/yield.
  - Fix: such results are now normalised per executor. Each divergence also logs the producer, i.e. the executor's previous record.
- **Input replay was one tick early** (the main cause of the non-AI mismatches).
  - The main loop runs WorldTick (`vt+0xBC`, the snapshot boundary) and then EngineTick (`vt+0xC0`). Scripted inputs are applied and recorded at EngineTick entry, so in a live frame g, WorldTick still sees frame g−1's mask.
  - The resim loop applied frame g's recorded input before WorldTick. It now applies it between the two ticks.
  - Non-AI harness (drones killed, random holds):

    | Build | Rollbacks | Mismatches |
    |---|---|---|
    | Before | 40 | 6 (15%), including k=1 position-only |
    | After | 27 | 3 (11%) |

- **Anim position (`0x8C80F0`).** Path: `0x442370` → `0x43B070`, then `0x4355A0`, which walks the MK anim tree `obj+0x194` to an MKAnimNodeSequence; the position is that node's `+0x74`.
  - Both the node (master image) and its UE3 counterpart `+8` (xblock) are in the snapshot.
  - The remaining divergences are tree-shape and timing differences (in resim the native sometimes finds no node), not missing memory.
  - Note: `0x43B070` writes the channel record, so diagnostics must not call it.
- **Blend-node bone masks were not snapshotted** (static analysis, then fixed; awaiting a soak).
  - `0x4355A0` chooses the child playing on a bone through a second TArray of each blend node, at `+0x4C` (stride 0x2C, mask at `+8`; read by `0x4CC470` / `0x4CC4C0`).
  - FindExternalBlocks took only `+0x08` and `+0x3C`. It now takes `+0x4C` too, and the XBLOCK walk reports blocks rejected by the 4 MB cap (`capped`).
- **soak30 (input-timing fix): the first fully clean soak.**
  - 3/3 sessions, 235 rollbacks (5 refused), no crash or hang, game alive at every session end.
  - Gameplay mismatch 19.6% (13/79, 14/79, 19/77) on AI attract fights.
  - One post-load `ATTACH ... has another Base` warning; it never became a dead record.
- **Channel record `+4` (bone-mask/playback object) is now captured** (static lead from the side session, 2026-10-14 19:50).
  - `0x43B070` tests `[[record+4]+0x18+4*(bone>>5)]`. On the `[obj+0x19C]==0` path, `[[record+4]]+0x74` is the position.
  - FindExternalBlocks took only record `+0x10`/`+0x14`. It now takes `+0x4` too.
  - Result: first-frame (S+0) divergences fell from 22/46 of the soak30 mismatches to 0 in the next runs.
- **Blend-node `+0x4C` is not a mask array.** It holds floats (weights): verified live with `animarrays`. Capturing it "restored" arbitrary addresses and caused the soak31 `0x42AF1D` free-tree crashes. Removed.
  - xblocks are now restored only if the block is still allocated (`SafeBlockInfo`, size ≥ span).
- **x87 precision ruled out:** the game thread and every pool thread run with control word `027F`. Two `037F` threads carry no game code.
- **Milestone 11, sound dedupe** (`SoundDedupe.cpp`).
  - Hook: vtable slot `0xD5E440` (sound object vt `0xD5E420 +0x20` = `0x921200`, thiscall, 7 args) swapped through HookManager.
  - Live frames record `{gf, cue-name hash / arg1, arg2, handle}`.
  - Resimulated frames reuse the original handle for a matching record and don't play again. Unmatched sounds play (late). Unmatched originals are counted as abandoned (not stopped yet).
  - Removes double sounds, and the stale-handle noise in script variables and the native trace.
- **Non-AI harness** (`noai_rollback.ps1`, now spanning consecutive demo fights with the AI killed in each):

  | Run | Rollbacks | Exact |
  |---|---|---|
  | noai-run3 | 58 (4 fights) | **57 (98.3%)** |

  - The one mismatch: P1 posy off by 0.36, with no script or RNG divergence.
  - 2 refusals: requests at fight start, before snapshots existed (harness timing).
  - Sounds were deduped in every rollback that had any (no new, none abandoned).
  - The AI attract fights stay at ~15–20% mismatch, from AI-only natives (`0x8BC9B0` → `0x889C30` distance and others). AI is not part of netplay, so Gate E is now measured on the non-AI harness.
- **PhysWatch:** an actor destroyed at gf ≥ S (LifeSpan expiry `0x7AEC5C`, cinematic spawns `0x4BB857`; see `docs/load_refusals.md`) refuses the rollback only if snapshot S still holds a pointer to it (dword scan of the snapshot, ~3 ms). Rigid-body terminations still refuse.
- **Stale script contexts corrupted the GMalloc medium heap (2026-10-15 00:15–01:00).**
  - Crashes `0x42ACFD`, `0x42AF1D` and `0x42AFC2` in the free-tree code of GMalloc's medium-block heap (`[inner+0x30]` ≈ `0xE0AC48`, in .data, never restored). Corrupt links were an MK id (`0x10000044`) and an MKScript opcode handler (`0x458AC0`, i.e. script bytecode).
  - A free-list node lay inside a restored `script_data` span.
  - Cause: `RngTrace::ScriptContexts()` kept every context seen at dispatch for the whole fight. A context whose script had ended still named its freed data/module block, which was saved and restored by content.
  - Proof: with `scriptdata 0`, 59 rollbacks over 3 fights ran without a crash.
  - Fixes:
    - Contexts come only from live executors or from dispatch within the last 60 frames. The list is pruned when it fills.
    - A module span must be a live GMalloc block of the size seen at save; this is re-checked at load before anything is written.
  - The correction test with these fixes: 90 rollbacks over 4 fights, no crash.
- **Correction test** (`noai_rollback.ps1 -Correct`): `fakerollback k mask n` replaces P1's input on the first frames of the window, then `at 2 rollback k+2` replays the corrected window from the same S.
  - First result: replays 24/30 exact, plain rollbacks 26/30, with 16 corrections that changed the outcome.
  - So corrected timelines are not yet fully reproducible. Still under investigation.
- **Actor merge:** fields that are pointer-like now are also kept live (UE3 actor spans only), not only those that were pointers at the save. Reason: a fighter's attachment TArray that was null at S was restored to null while the other fighter's Base stayed live, giving the resim crash `0x7898E5` during a grab.
  - In MK object spans this broke things: MK ids look like pointers, an id was kept live, and it ended up in a free tree.
- **Gate E driver** (`tools/gate_e.ps1`): non-AI sessions with prediction corrections, relaunching the game when the attract loop stops offering fights. Results are aggregated.
  - gatee1, the baseline before ScriptClock: 3 sessions, 157 rollbacks, **no crash**.

    | Rollback kind | Exact |
    |---|---|
    | Plain identity | 42/49 (85.7%) |
    | Replay after correction | 47/54 (87%) |

- **Script scheduling** (static analysis by a background agent: `docs/script_scheduling.md`).
  - An executor that started 3 frames earlier in the resim pointed at MKScript's real-time natives:
    - native 191 (`0x8B7F50`, QPC seconds);
    - stopwatches 782–789 (helpers `0x4931A0..0x4933C0` → real clocks `0x4D5DA0` ms and `0x4D5DF0` µs), with state tables at `0xE3B6D8` (µs) and `0xE3B7B0` (ms).
  - Real time keeps running during a resimulation of a few ms, so deadlines taken before S looked expired.
  - Fix (`ScriptClock.cpp`): while rollback is enabled, the 6 helper call sites (CodePatch) and native slot `0xDF0F3C` (HookManager) use a virtual clock that advances 1/60 s per GameFrame from an anchor (the real clock at enable and at each timeline restart). The tables are snapshotted as Tier A `script_clocks` (`0xE3B6D8`, `0x1D0`).
- **GMalloc's medium heap: UObject frees were never deferred (root cause of the `0x42Axxx` free-tree crashes).**
  - GMalloc's medium-block heap (`[inner+0x30]` ≈ `0xE0AC48`) is an instance of the MK heap class (vt `0xC66A3C`). UObject frees reach it directly (`appFree 0x81A460 → 0x4059C0 → owner vt+4`), bypassing GMalloc's hooked Free.
  - `HkHeapFree` passed every heap free through, assuming only the master heap (restored as an image) used that path. Medium-sized blocks freed in the abandoned timeline were therefore really freed, and restoring their contents (xblocks, script modules, Tier E objects) corrupted the free tree.
  - The load-time check could not catch this: the heap's block-info `0x42A890` returns `([p-4] & ~7) - 8` without testing the allocated bit.
  - Fix: heap frees for instances outside `[0xED53F0..0xED5400]` are deferred like GMalloc frees. Tier E spans also get the load-time allocation check.
- **Tier A bounds must come from code references.** A guessed `script_clocks` range (`0x1D0`) covered unrelated globals at `0xE3B858+`: a gate read by EngineTick, CRT statics, heap pointers. Restoring them crashed the game. The exact bounds are `0xE3B6D8+0x180`, found with `tools/immrange.mjs`.
- **Save cost:** cached VirtualQuery (`common/MemQuery.h`, bracketed per save/load) brought it from 3.45 to 2.38 ms/frame. Profile: master image 1.1, script spans 0.4, actors 0.25, Tier E 0.19 ms.
- **Gate E run 4** (virtual script clock, 1-frame context rule): plain 49/51 (96.1%), replays after correction 51/54 (94.4%), 1 crash in 159 rollbacks.
- **Milestone 12 groundwork:** GGPO is built as a static library (`tools/build_ggpo.ps1`, three patches: udp_proto qualifier, synctest DebugBreak → `MK9R_GgpoSyncError`, DllMain renamed) and linked.
  - `ggpo synctest N` runs a SyncTest session (no network) on top of the rollback engine.
  - DoRollback was split into `LoadSnapshot` and `ResimFrame`, which the GGPO callbacks reuse.
- **Crash bisect (2026-10-15 02:50–03:30).** Runtime toggles `memcache 0|1` and `heapdefer 0|1`, about 100 Gate E rollbacks each:

  | Configuration | Crashes |
  |---|---|
  | Cache off, heap deferral off | 0 in 106 |
  | Cache on | 1 in 51 |
  | Heap deferral on | 1 in 47 |

  - Both stay off by default.
  - The cache now stores only committed regions. Hypothesis: a cached "reserved" region turned committed under another thread, so pointers into it looked like data.
- **The crash class that was left (`0x42AF1D` in the master heap, a node-pool vtable `0xD4D980` or an MK id as a free-tree link) came from ActorDefer's load step.**
  - Actors whose owner was born after S are destroyed after LoadAB. The owner's memory is free in the restored image, but the actor's back-reference `{vt, obj, id}` at `+0x9C` still names it, and the destruction path wrote through it into the free chunk.
  - Fix: clear obj/id first.
  - Result: GGPO SyncTest ran 4 fights with no crash (below).
- **Milestone 12, GGPO SyncTest inside MKKE.** The callbacks are mapped onto the rollback engine:
  - save = the ring snapshot taken at WorldTick entry, with a gameplay checksum (life, pos, meter, RNG, GameFrame);
  - load = `LoadSnapshot`;
  - advance = `ResimFrame` plus `ggpo_advance_frame`.

  The live frame closes the previous one with `ggpo_advance_frame` at WorldTick entry, and adds and synchronizes both players' inputs at EngineTick entry. Harness: `noai_rollback.ps1 -Synctest 4`.
  - **synctest2** (check distance 4, AI killed, random inputs): 4 fights, about 1,360 loads, 5,436 replayed frames, **no crash**. Sync errors 185 + 89 + 151 + more, about 10% of replayed frames.
  - Top first divergences:
    - `0x458AE0` (variable read; data/vartable outside the image);
    - native `0x8CDE00`;
    - anim position `0x8C80F0`.
  - The `script_data` rule had rejected 22% of the ranges (not block starts). The data usually shares an allocation with the variable table `[ctx+0xA0]`, so ranges are now captured from the table start when that is a live block.
- **2026-10-15 04:00–05:10, SyncTest-driven fixes:**
  - **Script data ranges** are captured from the variable-table block start: rejections fell from 451 to 27.
  - **Fighter SkeletalMeshComponent span** (`fightercomp`, pointer-safe merge). Native `0x8CDE00` reads comp`+0x148`, a per-frame ramp.
    - Its first name, `skelcomp`, matched LoadAB's `strncmp("skel")` bone-array check and crashed (null identity read).
  - **Render fields:** each live frame records the actors' LastRenderTime `+0xAC` and render bit, and each resimulated frame replays them. The renderer does not run between resimulated frames.
  - **Fighter-to-fighter attachments** (grabs) are recorded per save and re-established at load:
    - attached now but not at S: MK's own detach call (vt`+0x190`, as in `0x433E40`);
    - attached at S but not now: Base plus the saved 0x60-byte record appended to the other fighter's list.
  - **ActorDefer** detaches the back-reference (`+0x9C` obj/id) once, at deferral; the saved bytes restore it on a give-back.
    - Detaching again at drop/release wrote 0/-1 into actors the world teardown had already freed (`0x42AF1D` with `ebx=FFFFFFFF` or an MK id).
  - **Heap locks:** the five MK heaps' spinlocks (`[heap+0x30]`) are held while the headers and the master image are copied or restored (`heaplock 0|1`).
  - **The VirtualQuery cache stays off.**
    - Positive-only caching still produced the `0x42AF1D` crash.
    - It also read a decommitted page (another thread freed a large block during a save).
    - The save cost is therefore ~5 ms/frame again (actors, fightercomp and Tier E pointer scans). It needs a different optimization.
  - **SyncTest error rate:** 3.6–12% of replayed frames per fight (synctest 11–15). Most desyncs are position-only, with no script divergence (anim/attachment-driven).
- **Milestone 11:** rumble (`Rumble.cpp`). MKKE's stored `XInputSetState` pointer (found in its writable data) is redirected: calls during a resimulation are dropped, and the last requested state is flushed when the resimulation ends.
- **Milestone 13 groundwork:** GGPO peer-to-peer in the DLL (`ggpo p2p <localport> <ip> <remoteport> <player> [delay]`). Every GGPO call happens at the WorldTick-entry boundary, because P2P rolls back inside `ggpo_idle`. The synchronized masks are applied at EngineTick entry, and local input has one frame of delay.
  - `tools/ggpo_peer` is a stand-in remote player (real UDP, random inputs) for loopback tests without a second game; harness `noai_rollback.ps1 -P2P`.
  - Not yet run: the host refused process creation at 05:10.
- **Heap free-tree audit (05:10–05:35).** `HeapAudit` walks the red-black free trees (root `[heap+0x34]`; nodes: `[+4]` flags 0, `+8` parent, `+0xC`/`+0x10` children, `+0x14` color) of the five MK heaps and of GMalloc's medium heap (`0xE0AC18`), under each heap's spinlock.
  - It runs before and after each load, after each resimulation, and every frame, and logs the first broken node with the frame and the last load.
  - Decoding: `{0xD4D980, 0, -1}` is not a pool node. It is the MK reference reset `0x42D7B0` (`{vt, obj, id}` → empty ref) applied to memory that is a free chunk.
  - **synctest19** (heap locks, audit): 3 fights, about 1,140 loads, **no crash, no audit failure**. Sync errors 4.3% and 4.0% of replayed frames.
- **Milestone 13 on loopback: works.** `noai_rollback.ps1 -P2P` runs the game as player 1 and `tools/ggpo_peer.exe` as player 2 over UDP on 127.0.0.1.
  - p2p2: 4 fights, about 1,350–1,500 synchronized frames each, 3–42 rollbacks per fight on corrected remote inputs, about 33 prediction-barrier waits per fight, ping ~16 ms, **no crash**.
  - The per-field desync report is off in P2P (corrections change the outcome by design).
  - Open for a real two-game LAN test:
    - both games must enter the same fight on the same frame, with the same characters, stage and RNG seed (fight-start handshake);
    - cross-machine checksum comparison (GGPO's desync detection, or our own gameplay checksum exchange);
    - frame pacing beyond the simple time-sync sleep.
- **Netplay start (05:40–05:55).** `ggpo netplay <localport> <ip> <remoteport> <player> [delay]` arms a P2P session. Sequence:
  - At the next timeline restart, the DLL waits for the first frame that followed a fight update and has a snapshot.
    - GameFrame also resets at menu/attract map loads. The first version started there, every save was refused ("object heap not discovered"), and GGPO replayed without loads.
  - On that frame it pauses the game (engine pause setter), seeds RNG A with a fixed value, and starts the session.
  - It unpauses on GGPO's RUNNING event.
  - A GGPO load that finds no snapshot now stops the session instead of replaying on unrestored state.
  - Loopback result (game plus `ggpo_peer`): started at gf 1789 (first fight frame after the intro). A whole fight: 1,966 frames, 83 rollbacks, 0 failed saves, no crash.
- **Save cost:** per-scan region caches in the pointer scan (`MarkKeep`) and the UE3 cell walk. Profile went from about 5.7 to 4.1 ms/frame. Remaining: master image 1.1, script spans ~0.7, UE3 cells ~0.9 (before its cache), Tier E 0.6.
- **Residual issues seen once each:**
  - R6025 (pure virtual call) inside a worker thread's GMalloc Malloc;
  - an endless MK list walk in the script sweep (`0x42D5D0` from `0x45A360`, tick-group address on the stack).
- **TASKREG audit:** after each load and every frame, the per-object async-work registrations of `[0xE8436C]` are checked for dead or bDeleteMe objects. This targets the soak29 gf 2006 hang. The audit disables itself if the layout guess is wrong.
- **Fighter non-mesh components (`pawncomps`, 06:15): rejected.** Snapshotting the pawn's other components (`+0x3C` list minus the skeletal mesh) as `ue3anim` spans raised synctest23's desync from 4.0–4.3% to 6.1% (84/1380), and a `0x42AF1D` MK-ref crash followed in the next fight. Off by default (`pawncomps 1` re-enables it).
- **What the desyncs are (synctest19, 120 reports):** almost all are fighter actor Location (`+0xDC/+0xE0/+0xE4`) differing by 1 ulp, sometimes Z replaced by 0. They are spread evenly over check distances 1–4, and engine dt is a constant 1/60.
  - A few (11) are grabs: the original run attached P1 to P2 (P1 `+0x60` Base = P2, P2 list count 1) during the replayed frame, and the replay did not re-make the attachment.
  - Next: `loctrace 1` (CrashLog::TraceArm) keeps 4 hardware write watchpoints on both fighters' Location Y/Z. Each write (eip, value, callers) is recorded per frame, live and resimulated passes separately. A desync report logs where the two passes of the frame first wrote differently.
- **Location write trace results (synctest24/25/27, 06:30–06:55).**
  - Fighter Location is written by the setter `0x7225C0` twice per frame:
    1. root-motion application `0x762C10` (from EngineTick `0x80F896`): `Location += Owner-rotation × comp.RootMotionDelta (+0x5E0)`;
    2. MK's position update (`0x7A7433` path), which nearly overwrites it.
  - In every dump that named a writer, the first divergent write is the root-motion one, with wildly different intermediate Z (46.93 vs -1.72). The final value then differs by 1–4 ulp.
  - The root-motion delta is reset by `0x7741E0` at the start of the skeleton update, called from a task-system job (`0x71EAA0`). These jobs run on worker threads or the game thread, so a per-thread watchpoint sees them only sometimes. The "update ran on different frames" asymmetry was that artifact, not a divergence.
  - MXCSR is `0x1F80` control (only the sticky flags differ) on every thread, like the x87 CW: thread choice cannot change results.
- **Ruled out (07:00–07:15):**
  - stale anim-node tick tags (`AnimTagAudit`: none found);
  - writes between EngineTick exit and the next WorldTick entry (`betweenprobe 300`: no fighter comp/actor/char byte changes there; rendering happens inside the engine tick);
  - anim nodes missing from Tier E: every node in the components' tick arrays was already captured (`gTierETickNodes`, kept as a safety net).
- **Error rate is per replayed frame, not per load:** distance 1 (load every frame) 5.1%, distance 4 6.6%.
- **HostFrame replay:** a resimulated frame g now runs with the HostFrame value (GameInfo `+0x6C`) the live frame g had, instead of the live counter advanced from F. Distance 1: 3.6% (from 5.1%).
- **Bone matrices were not restored on most loads (fixed 08:03; synctest38: 2.1% of replayed frames, from ~6%).**
  - The fighter SkeletalMeshComponent triple-buffers its bone matrices: TArrays at `+0x2F4/+0x300/+0x30C` rotate the same three blocks every frame.
  - `BONEHASH` (bone/comp hashes at FightUpdatePlayers entry, live vs resim) showed the matrices differing at the first replayed frame while the local atoms were identical. The only component difference was these three data pointers, rotated.
  - Two faults:
    1. the pointer-safe merge kept the headers at the live rotation phase;
    2. the `skel_m*` spans' load check (`[identity] == base`) then refused the matrix contents whenever the phase differed (distance ≠ 0 mod 3).
  - Now:
    - the fightercomp merge restores the saved rotation when the live three pointers are a permutation of the saved ones (`bonerot`);
    - a `skel_m*` span loads if its block is any of the three.
  - Also on: `strictvt`. Above 0x30000000, a value counts as a pointer only if its target starts with an aligned .rdata vtable whose slot 0 is code. `VTKEEP` had shown floats (anim node `+0x5C` = 26.1, actor `+0x1CC` = 35.0) kept live as "pointers".
  - Remaining first-frame desyncs: mostly fighter Z (`char +0x1EC`) off by 1–2 ulp, with bones identical at FightUpdate entry. Also a component TArray at `+0x3A8` (object pointers appended per call by `0x76DCB0`, cleared at `0x77B6xx`) whose count, kept live with the triple, differs by 2.
- **Component queued anim-object list (`complist`, 08:30).** TArray at component `+0x3A8`: `0x76DF10` looks up an FName in the `+0x290` table and appends the object; it is cleared at `0x77B6xx`.
  - The merge kept `{data, num, max}` live, so a replay saw the abandoned future's count and processed stale entries. Possibly also behind the game-thread R6025 (pure virtual call inside a task job, synctest39).
  - The contents are now a span, and the saved count is restored when the block is the same.
- **Fighter name-lookup cache (`namemaps`).** A TMap at MK object `+0x138`, filled lazily by `0x456F30` → `0x443C10`.
  - The header lives in the MK image; its element (28-byte, next index at `+0x14`) and hash blocks are GMalloc blocks. Restoring the header over live blocks made a chain cycle: synctest40 hung at `0x00443BCC` inside a script native.
  - Now the blocks are spans, restored when the pre-load header still names them. If the map reallocated since the save, the live header is put back so header and blocks stay together.
- **Guarded pointer-heuristic read.** `MarkKeep` read `[v]` after VirtualQuery said committed; a worker decommitted the region in between (synctest41: DLL crash reading `0x3F800000` = 1.0f). `CrashLog::SafeRead32` is a one-instruction asm stub whose fault resumes at its failure exit.
- **Double replay (`rbtwice 1`, local rollbacks, 08:50).** Each rollback window is replayed twice from S, and the two replays are compared (gameplay plus fighter char/actor/component bytes) as well as against the live frames.
  - twice2: 19/20 replays identical to each other. 18/20 final states exact against live.
  - One case where the replays agreed with each other but not with live: something only live execution does.
  - One case where the two replays differed (P1 Z 49 vs 0): nondeterminism inside replays.
  - Bytes that differ between replays: actor `+0x80` bit 0x40 (render), char `+0x18C` (counter), component `+0x444`.
- **Fighter LOD forced (`forcelod`).** Component `+0x444` = MaxDistanceFactor, copied by `0x76DCB0` from the render-side mesh object (`[comp+0x2B4]+8`).
  - PredictedLODLevel `+0x43C` follows ForcedLodModel `+0x438` when set. Otherwise it follows the render thread's desired LOD `[[comp+0x2B4]+4]`, so render timing chose the bone set a frame evaluated.
  - ForcedLodModel is now set to 1 (LOD 0) for both fighters at every boundary and every resimulated frame.
  - The kinematic-update skip at the same place uses `+0x444` and the render-written LastRenderTime `+0x228` only when MinDistFactorForKinematicUpdate `+0x484` > 0.
- **twice3 (forced LOD): 46/46 replay pairs identical in gameplay; 3 rollbacks ended different from live.** Replays are deterministic among themselves. What remains is something live execution does differently.
  - Two-point bone capture (synctest44): at FightUpdate entry everything is identical. At EngineTick exit both fighters' atoms and m0 differ, so that frame's animation evaluation reads something different live vs replay.
  - The component's `+0x75C`/`+0x290` are SkelControls (EulerLookAt, IKRootAdjust, PinLimb), all already in Tier E. MK drives animation with its own MK anim nodes; their xblocks were never skipped.
  - Not it: the pacer's real-time doubles (`0xE0AB10/18`, `0xE43468`); outside the pacer they are only read as object timestamps.
  - Being tested: `resimsleep N` puts a live-like gap after each resimulated frame. Live frames always have the pacing gap and replays run back to back, so async work that crosses the boundary would resolve differently.
- **Other candidates checked:**
  - `resimsleep 16` (a live-like gap after each resimulated frame): 2.6% in its one complete fight, within the usual spread. No async-race signal.
  - OpenMP (VCOMP90): only two parallel regions (`0x601730`/`0x601A30`, a dynamic loop over objects with per-thread result lists; vtable `0xD47DF4` slots 4–5). An execute breakpoint showed they are not called on the game thread during fights. Not pursued.
- **Name-map restore made atomic (synctest45 crash `0x00443BC9`).** The hash block had been reallocated (span skipped, live header kept), but the elements block was restored. Now one decision per map: elements, hash and header are restored together only if both blocks are the saved ones; otherwise all stay live.
- **Remaining session killers (rare):**
  - R6025 pure virtual call in a task job on the game thread (`0x71EA31`), early in fights (synctest39, 46). `PURECALL` diagnostics: MSVCR90 `_set_purecall_handler` logs the caller's ebx/esi/edi (described), plus stack values, before the CRT aborts.
  - Global MK object list cycle: `0x0042D950` from `0x0042DE00` in EngineTick (synctest47). `LISTAUDIT` walks the list with Floyd plus a tail check before/after loads, after resims and every frame.
- **Cross-machine desync detection (Milestone 13).** P2P sessions open a side UDP socket (GGPO port + 100).
  - Every boundary sends the machine-independent gameplay checksum (gameplay state with GameFrame zeroed) of the frame 24 frames back, which can no longer be rolled back.
  - Frames are counted from the session start. Peer values are compared with the local record (a replay overwrites predicted values).
  - Logs: `NETPLAY DESYNC: frame ...`, a running total every 600 frames, and a summary at session end.
  - `tools/ggpo_peer` echoes these packets (it has no game state). Loopback p2p5: 1362/1362 matched, which tests the channel only.
- **Milestone 15: GGPO over Steam P2P (`src/net/SteamBridge`).**
  - GGPO keeps its UDP socket; its remote is a local bridge (127.0.0.1:7050).
  - The bridge forwards datagrams with the game's own ISteamNetworking (SteamNetworking005, vtable slots 0–2: Send/IsAvailable/ReadP2PPacket, unreliable-no-delay) to the peer's SteamID64 on channel 77. Checksums go on 78 (7100 ↔ 7150).
  - Commands: `steamid`, `ggpo steam <id> <player> [delay]`, `ggpo steamnetplay <id> <player> [delay]`.
  - Self-loopback (own SteamID as the peer, Steam delivers to self):
    - a 556-frame session, 1122/1121 packets;
    - a whole netplay fight: arming → pause → RNG seed → RUNNING → unpause, 1811 frames, 98 rollbacks, 0 failed saves, 1787/1787 checksums matched, no crash. It re-armed for the next fight.
  - Not touched: MK's own online mode and matchmaking.
- **Inputs exactly once per frame in GGPO sessions.** CommandChannel only records the wanted mask (`Input::SetWantedInput`); RecordInputs applies the synchronized mask once per fighter pad and never hands the pad back to the sampler.
  - Before, in P2P the local player's pad was overridden twice per live frame (scripted mask, then the synchronized one) but once per replayed frame, so prev/cur and the press/release rings differed.
- **Deep diff (`deepdiff 1`):** the MK master image is identical at FightUpdate entry of replayed frames. Live and replay diverge inside the frame:
  - particle simulation floats (MKParticle);
  - MK anim nodes (`+0x74` position ahead by 4.0 in one run; `+0x88` written by the anim-tree job `0x4CBC78` ← `0x4CBA8E` ← `0x773F95` ← task runner `0x71EAED` ← `0x661AEA`, from WorldTick `0x7F8034`).
  - The anim-evaluation jobs run on whichever thread takes them, so the game-thread-only watch cannot show the replay's writes. Root cause still open.
- **Where the residual starts (synctest55, three deep-diff points):** the MK master image is identical at FightUpdate entry **and exit** in every sampled replay, so scripts and MK gameplay logic are deterministic. Differences appear only after the fight update, in the rest of EngineTick:
  - MK tick list `0x407860`;
  - task-system actor ticks and anim evaluation `0x7F7F50` → `0x70E065` → `0x661AEA`;
  - root motion `0x80F896`;
  - particles.
  - Single-core test (`affinity 1`, twice5): 54/58 exact vs 55/58 on all cores, so it is not a thread race.
  - Untested candidate: state outside every snapshot that this part reads, e.g. the PhysX scene (cloth, rigid bodies, particle collision) or the UE3 collision octree.
- **bTicked parity (fixed, `tickparity`).** Actor `+0x80` bit 0x40 is not a render bit. It is bTicked, set by the actor tick prologue `0x7A8F90` from `GWorld+0x130` (world tick parity, not rolled back). ReplayRenderFields replayed the live frame's recorded bit, which after an odd rollback distance had the wrong parity for the replay. It is now set to the world's current parity. The effect was not measurable in synctest54 (3.8% / 3.1%) or twice4 (55/58 exact).
- **`compsync` (rejected, off):** after a load, each fighter actor was re-placed at its restored Location through its own SetLocation (vt+0xB8 → `0x790230` updates the attached components), so cached component transforms and collision entries would follow the restored actor. twice6: 56/59 exact against live, but 5/59 replay pairs no longer agreed with each other (previously 0), so SetLocation's side effects depend on unrestored state.
- **Actor arrays kept live (`ACTORARRAY` diagnostic):** of the fighter actors' TArrays, only the attachment list `+0x68` (0x60-byte records) has a different count at load than at the save (5→4, 0→1, 3→0…).
  - Effect actors attach to and detach from the fighters' bones. RestoreAttach only re-makes fighter-to-fighter attachments, so after a load the list holds the abandoned future's effects and lacks those attached at S.
  - This matches the MKParticle/effect differences in the deep diff. Mostly visual, unless a projectile starts out attached.
  - Next candidate: rebuild the attachment list from the actors whose Base is the fighter after ActorDefer gives deferred actors back.
- **`attachall` (rejected, off):** record each fighter's whole attachment list at save. At load, detach (SetBase NULL) live attachments not present at S and rebuild the list in saved order with Base reset. twice7: a crash calling a null function in a resimulated EngineTick, and one replay pair disagreed. Re-attaching effect actors whose own state was not restored is unsafe.
- **`tierecam` retested with the double-replay metric (twice8): still worse** (54/59 exact vs 55–56, and 2 replay pairs disagreed). Off.
- **Transition crash (synctest58).** After two clean fights, the next fight's intro crashed at `0x0042AF1D` with `esi=0x00E39FB0` as a heap tree node. 0xE39FB0 is the header of the script VM list walked by `0x0042D950` from `0x0045CAF0` right after the fight update, so a freed heap chunk still carried a link to it. That list's nodes and objects come from pools (no chunk headers), so a per-node "freed" check is not possible. LISTAUDIT now also walks 0xE39FB0 (cycle and tail).
- **The biggest remaining cause: fighter Location kept live by the pointer merge (fixed 12:48, `floatfields`).**
  - Three-point capture (synctest59): at the first replayed frame's FightUpdate *entry*, P2's actor Location Z already differed (109.9 live vs 178.2 replay) while the whole MK image matched.
  - The game thread writes Location only via root motion in EngineTick, so the restore itself had skipped Z. A coordinate like 109.9 = `0x42DBD61C` is an aligned address in the UE3 heap. When it happened to start an object with a real vtable, the strict pointer rule called it a pointer and the actor merge kept the abandoned future's value.
  - Location changes every frame, so this misfired at random a few percent of the time: the "random 1-ulp drift" seen all day (the final position is computed from it).
  - Fix: actor `+0xDC..+0xE4` is always restored by value.
  - synctest61: **0.7% and 1.3%** of replayed frames (10/1440, 22/1688; third fight 0 errors in its partial run), from 3–5%, no crash.
- **Generalised (`learnfields`, testing):** every pointer scan records, per address, how often the nonzero value looked like a pointer vs data. At load, a single kept field whose address was data-like at least 3 times and more often than pointer-like is restored by value. This covers the other float fields (velocity, component and anim-node floats).
- **TArray rule misfire (fixed 13:30).** The `{data, num, max}` rule needed only a committed "data" value. Fighter Z (a float in the heap's address range), pitch 0 and yaw 0x4000 looked like `{data, 0, 16384}`, so Location and Rotation were kept live far more often than the vtable coincidence alone. Above 0x30000000 the data pointer must now be 8-aligned (GMalloc blocks are), and actor `+0xDC..+0xF0` is always restored.
  - Soak results (sync errors / replayed frames, no crash): synctest64 1.5%; synctest65 1.2% and 2.6%.
  - `learnfields 1` (restore every field learned to hold data) was worse: 2.6% / 2.0%. It stays in report-only mode (2).
- **First replayed frame, all inputs identical (synctest68).** With sampling aligned to the load phase, the MK image *and* the xblocks are identical at FightUpdate entry and exit. Only after it do some anim-tree positions get +2.0/+4.0 extra frames, and a particle's time follows.
  - Trace: setter `0x4CBDE0` ← `0x4CBFA6` ← `0x4B55CF` ← `0x4B618C`. The position comes from a delta time passed down the MK anim tree from `0x773F95`: `vt[2]() × the component job's DeltaTime`, after the component stores WorldInfo time into `+0x534`.
  - These trees and particles appear to belong to effect or stage objects whose UE3 components are not in the snapshot. Their component times stay at the abandoned future's values, which is visual only.
  - The fighters' residual 1–2% still has to be tied to a specific input.
- **Between-tick writes into the MK image:** the main loop's `0x0081C7C0` (manager `[0xE924F0]`, online/input layer) writes wall-clock millisecond stamps and a counter (`0x4F8D38` ← `0x94E6A7` ← `0x94574D`) into objects in the restored image, outside the ticks, so replays never repeat them. Not shown to affect gameplay.
- **R6025 is PhysX's:** the message exists only in PhysXCore/PhysXCooking/PhysXLoader (own static CRTs) and cudart, so MSVCR90's purecall handler never sees it.
  - In synctest64 the game thread was waiting inside a physics task (`0x6CCA90` → `0x6C9A40`, PhysX vt+0x238 on `obj+0x154`, a body instance) while a PhysX thread showed the dialog.
  - The hang dump now names the dialog's owner thread and its stack as module+offset, skipping system DLLs.
- **Save cost: positive-only region cache (`poscache`, 14:40).** MarkKeep's VirtualQuery results for *committed* regions are kept across saves (reset every 120 saves); "not committed" answers are never cached. A stale "committed" only keeps a field live, and the one dereference is the guarded SafeRead32.
  - Profile (ms/save), before → after: actors 0.4–0.7 → 0.04; skel 0.4–0.6 → 0.2; tierE 0.5–0.6 → 0.15–0.34.
  - A save now totals about 3 ms: master image 1.1, UE3 cells 0.6, script spans 0.55.
- **Fight-start crash traced to ActorDefer (synctest74).**
  - LISTAUDIT caught the global MK object list broken at gf 924 (a null link after 51 nodes), one frame after four effect actors' destruction was deferred (gf 922–923). The next frame crashed in `0x0042AF1D` with an MK-reference reset `{0xD4D980, 0, -1}` in freed heap memory, and an MSVCR90 R6025 followed.
  - A deferred actor keeps ticking with its MK owner freed. The `+0x9C` back-reference is detached, but other paths still reach the owner.
  - This happened before the fight's GGPO session had started, where deferral buys nothing. Deferral is now active only during GGPO sessions (`deferggpo 0` restores the old behaviour for the local-rollback harness).
- **The fight-start crash is the deferred actor left for the level teardown (synctest75, 15:00).** Deferring only during GGPO sessions did not stop it: synctest75 crashed the same way at the 4th fight's start (gf 544, LISTAUDIT one frame earlier).
  - That session's frames log showed a single `ACTORDEFER drop 1 deferred actors at timeline restart`: an effect actor still deferred when its world was torn down. The level teardown destroyed it with its MK owner already freed, and MK memory was corrupted; the damage surfaced a few hundred frames into the next fight.
  - Now:
    - once the fight update has not run for 5 ticks (fight over), every deferred actor is destroyed while its world exists (`Rollback::FightTickDone`, called from the fight-update hook every tick, at the same point as normal releases);
    - deferred actors are released after 6 (SyncTest) or 10 (P2P) frames instead of the whole 12-frame ring.
- **Follow-up (15:00–16:15): the fight-start crash is not explained yet.**
  - The fight update runs until the very last frame before the world change: the first tick without it is already in the new world (`fight over at gf=0`). So "release when the fight is over" cannot run before the teardown.
  - The deferred actors hold no reference to their owner besides the detached `+0x9C`: an at-deferral scan of the actor (0x300 bytes) and its components found none.
  - synctest76 had 3 sessions ending with drops (1–2 actors each) and one crash; synctest78 had a world change dropping about 80 deferred actors and no crash in 4 fights. The drop correlation is weak.
  - LISTAUDIT now logs the broken node's dwords, its object and type, and the tail's object for the next occurrence.
  - Frequency: about one fight start in 4–8, always in the fight after a GGPO session and before the next one starts.
- **Memory growth during GGPO sessions (synctest80–88, 16:30–17:20).** Soak79 hung after ~50 minutes with `STATUS_NO_MEMORY` (private 3.3 GB). Private memory grows about 150 MB per 600 SyncTest frames, only while a session runs, and is not returned at the map change.
  - Ruled out:
    - **Abandoned-timeline GMalloc allocations:** `ALLOCLEAK` counts game-thread allocations made at gf ≥ S that are still unfreed when a load to S happens. Only 4.3 MB over 400 loads.
    - **Quarantine:** only parked bin-29 blocks, about 15 MB.
    - **GGPO buffers:** they are freed.
    - **Threads:** the count is flat at about 135.
    - **Our snapshot ring:** 5–9 CRT blocks, constant.
  - `REGIONS` (a committed-private diff per allocation base, every 600 saves) shows the growth is about 140 new regions of exactly 1 MB per 600 frames. Their first dwords are zero. A few NT-heap segments also grow (FFEEFFEE signature).
  - `ALLOCWATCH` hooks:
    - IAT hooks on VirtualAlloc in every module recorded none of these regions;
    - inline hooks on ntdll `NtAllocateVirtualMemory[Ex]`/`NtFreeVirtualMemory` (original bytes kept, refused unless the prologue is `mov eax, imm32`) only recorded our own snapshot buffers.
    - So the 1 MB regions are created below the user-mode allocation API, most likely kernel-mode graphics mappings.
  - **Correction (17:30):** the 1 MB regions are `VirtualAlloc(NULL, n, MEM_COMMIT)`: an implicit reservation, which my MEM_RESERVE filter skipped. With chains 6 deep, `ALLOCWATCH` attributed the growth:
    - **GMalloc's medium heap growing in 1 MB segments.** `0x0042B4DA`/`0x0042BA05` → `0x00429E90` → `0x00429B00` → `0x00427890` → `0x0042D490` (VirtualAlloc). This is the heap whose free tree crashes at 0x0042AF1D. Requests of 256 KB to 1 MB from `0x004053D0` go to `0x004297E0`, also through `0x0042D490`.
    - **Our own snapshot ring.** `SaveAB`'s `add()` reused `bytes[n]` by index while the span order changes between saves, so every buffer ratcheted to the largest span seen at its index: +180 MB per 1,200 frames.
  - **Root cause of the medium-heap growth: un-freeing at load.** `FreeQuarantine::OnLoad` made every block freed after S "live again". That is right only when restored state still references the block. Two cases leaked:
    - **(a) Blocks allocated and freed after S**, 45–90 MB per 100–200 loads. The restored state never saw them, so nothing frees them again. Fix: the free records the block's allocation frame (`UntrackAlloc` returns it), and on load such blocks are requeued (stamped S) instead of dropped.
    - **(b) Older blocks freed after S by non-MK code**, about 490 KB per load (147 MB per 300 loads). Their owners are live UE3 state that is not rewound, or a pointer-safe merge kept the owner's new pointer.
  - **Fix for (b):** non-MK, out-of-window candidates are requeued unless a dword in the restored spans (scanned live right after `LoadAB`, using a hash set of exact block starts) still points at them.
    - The scan finds 74–450 such blocks per 100 loads and keeps them live. Releasing those would be use-after-free.
    - Cost: about 2.5 ms per load.
    - `unfreeall 1` restores the old behaviour.
  - **Snapshot ring fix:** buffers are matched by best fit from the slot's previous buffers, through a multimap by capacity.
  - **Result:** private memory is flat during GGPO sessions. synctest96–99: 1009 → 1039 MB over 1,200 frames (the parked quarantine), against +170 MB before.
  - **AllocWatch caveat:** the first version's `thread_local` guard crashed in ntdll thread start/exit, because the TLS array is null there (`MK9Rollback.dll+151C3` reading `fs:[0x2C]`, synctest95/98/99). The guard is removed, since the hooks never allocate, and the tool is now opt-in (`allocwatch 1`).
  - **Measurement note:** "ROLLBACK save cost" includes the GGPO boundary (SyncTest's load plus 4 replayed frames). `SAVE PROFILE` is the real save time: about 2.1 ms.
- **R6025 pure virtual call on a PhysX worker (synctest86/87, about 40% of runs).**
  - The filtered stack is entirely inside PhysXCore.dll. The dialog dump now keeps only values that follow a call instruction; the old raw scan listed `MKKE+0x220872`/`+0x230031`, which are not return sites.
  - Cause found by inspection:
    - the strict pointer rule above 0x30000000 only accepts vtables in MKKE's own .rdata;
    - PhysX objects live in GMalloc memory too, with vtables in PhysXCore.dll's .rdata;
    - so a UE3 field pointing to an NxActor/NxShape was restored by value;
    - a stale pointer to a released PhysX object (its vtable reset to the abstract base) then gives exactly a pure virtual call.
  - Fix: `foreignvt` (default on) also accepts a vtable in any other module's read-only section whose slot 0 is code of the same module. `FOREIGNVT` counts these: 2–5 per 600 saves.
- **0x0042AF1D heap theory rejected (synctest103, 18:55).** Statically, `[heap+0x34]` of class 0x00C66A3C is the free-tree root. At runtime the master heap is a single 12 MB segment (`+0x24 = 0xC00000`) obtained from its parent `0x0F749830`, not from the segment registry at `[0x00E29308]`. Its root stays at the segment start, as does the object heap's (`0x10F6FFC0`). MasterRanges does cover the heap, so the crash cause is still open. The registry path is off; Discover now caches the last good region.
- **Lockstep netplay works on loopback (lockstep2, 18:55).**
  - Setup: `ggpo lockstep 1`, then `noai_rollback.ps1 -P2P -Lockstep -Delay 2`.
  - Result: two full fights (1,342 and 1,312 frames) with 0 loads, 0 replays, all checksums matching the echo peer, and no crash.
  - How it works: GGPO publishes `ggpo_last_confirmed_frame` (patched in tools/build_ggpo.ps1), and the P2P boundary waits until it reaches the frame before synchronizing it.
  - A pure prediction barrier deadlocked: the confirmed frame is a minimum that includes the local player's own last input.
- **Milestone 11, camera shake verified at runtime (synctest104).** `TYPECENSUS` walks the global MK object list at every MEMORY tick: 575–579 MK objects in 27–29 types, all inside the master ranges, including one `ScreenShakePdata` (the shake state found statically by the side session: descriptor 0xDE8400, size 0x50). So shakes roll back with Tier B. Particles (MKParticle) are also MK objects inside mkmaster (synctest52 DEEPDIFF/INSPAN).
- **0x0042AF1D clue:** in synctest96/100 the crash registers held MK object ids (`ebx` 0x10000044 / 0x10000054 / 0x10001811) as free-tree links, so a chunk in the object heap's free tree was still a live object. `LINKEDFREE` (in HkHeapFree, rollback mode, game thread) now logs any free of a block that is still linked as an object in the global MK list, with its caller chain.
- **Medium-heap crash explained (synctest107, 19:59).**
  - The crash: 0x0042ACFD in GMalloc's medium heap (`ecx = 0x00E0AC4C`, its tree root), on a free whose `esi` was inside a restored `script_data` span (+0x420).
  - The cause:
    - `script_data` spans (GMalloc blocks restored by content) were never registered in the quarantine's restore window. Only xblock/skel/ue3cell/ue3anim/script_module/actor spans were.
    - `appFree` frees reach the medium heap through the MK heap class's vt+4 (HkHeapFree). Those were only deferred with `heapdefer`, which is off.
    - So such a block could be freed and reused, and a load then wrote the old contents over a free chunk.
  - Fix:
    - `script_data` bases are now restore-window blocks.
    - HkHeapFree always defers restore-window blocks of non-image heaps.
  - The phase audits (FightUpdate entry/exit, EngineTick exit) caught nothing in 6 sessions: the corruption and the crash happen inside one engine tick.
- **0x0042AF1D root cause found (synctest111, 20:30).**
  - The membership audit (every global-list node must name 0xDE8210 at +0x14), run at the phase boundaries, caught the corruption.
    - The global list was intact after the load (S=773) and at FightUpdate exit 778.
    - By EngineTick exit 778 an `MKAnimTrackMatcher` node whose header is `0x00E3B694` was chained in.
  - `LISTHEADERS` scans every MK list node (vt 0x00C66B14) in the master image for its static header. 14 headers are in use, and exactly one is not fully restored by Tier A: 0xE3B694, of which only the head (`misc_E3B698`, 4 bytes) was saved.
  - Its tail and pending fields stayed live across loads, so the list went inconsistent. Engine-tick anim work then linked and unlinked through it, cross-linking the global list and corrupting the object heap's free tree.
  - Fix: Tier A restores the whole header (`mkanimtrack_list`, 0x00E3B694 + 0x14).
  - Also: LoadAB now skips any content span overlapping a free chunk of the medium heap (tree at 0x00E0AC18+0x34, locked by HeapLocks).
  - Verification (synctest112/113): 7 full SyncTest sessions in a row with no crash, no HEAPAUDIT/LISTAUDIT failure, no R6025, and flat memory. Before the fix there was about one 0x0042AF1D crash per 1–3 sessions. LISTHEADERS reports 0 uncovered headers at every census.
- **Queued anim-object list count (21:00–21:55).**
  - The problem: comp +0x3AC (the count of the TArray at +0x3A8) was the most frequent BONEHASH difference at the first replayed FightUpdate, off by 1–2. The merge restored the saved count only when the data pointer and capacity were unchanged. When the array had been allocated or grew since the save, the abandoned future's count survived.
  - The fix (`complistrebase`, default on): when the live capacity can hold the saved count, the saved count is restored and the saved entries are copied into the live block.
  - Result, synctest113 → 115: sync errors 2.2% → 1.6% of replayed frames, +3AC lines 185 → 117. The fight-to-fight spread is large (42 vs 3 per session).
  - synctest114 crashed 31 frames in at 0x0042B32F: a small-block page returned to the medium heap next to a restored script_data span. synctest115 ran 3 clean sessions with the same build. This is the medium-heap/script_data family, now rare; its cause is still open.
- **Stability after the list-header fix (synctest112–117):** 1 crash in 14 full SyncTest sessions (the rare medium-heap/script_data case, synctest114).
  - The new LoadAB guards (free medium chunk, and a script block released by the quarantine after the save) never had to skip a span in synctest117.
  - Desync per run with the same harness seed: 2.2% / 1.6% / 3.2% of replayed frames (one fight alone: 63 errors). Attract fights differ run to run, so this metric cannot separate single fixes. A fixed-content test fight is needed: same characters, same scripted inputs.
- **In-fight A/B testing (`abtest <toggle>`, 2026-10-17).**
  - Why: run-level error rates cannot judge a fix (fight-to-fight spread). `fixseed` does not make attract fights repeatable either, because the first demo starts before any timeline restart and the attract flow depends on timers.
  - How: `abtest` alternates a load-side toggle on even/odd GGPO loads and charges each SyncTest error to the arm of the load whose replay produced it. Both arms see the same fight.
  - First result: `complistrebase` ON 37 errors vs OFF 32 over 433 loads per arm, i.e. no effect. It is now off by default; the earlier 2.2% → 1.6% was spread.
  - Batch 1 (one fight session per toggle, ~210 loads per arm), errors ON vs OFF:
    - floatfields 10 vs 41: helps strongly.
    - animtagfix 16 vs 29: helps.
    - bonerot 25 vs 36: likely helps.
    - namemaps 15 vs 7: may hurt.
    - strictvt 30 vs 23: inconclusive.
    - forcelod: crashed (below).
  - The forcelod run caught the rare medium-heap crash with a detector hit just before it: "written after free" on an 8408-byte block freed by 0x004D2E96 at gf 4664 and released at 4676, then 0x0042ACFD.
    - Likely writer: our own LoadAB. The xblock walk can still reach a block the game already freed (stale MK pointers), so later snapshots captured it and loads wrote into it.
    - The medium free-chunk guard misses this, because small medium chunks live in bins, not the tree.
    - Fix: SaveAB never captures a block that is pending in the quarantine. The span is kept empty so callers' identity bookkeeping stays aligned; the count is logged with SAVE PROFILE.
  - Batch 2 (with the pending-skip fix), errors ON vs OFF:
    - forcelod 6 vs 19: helps (the batch 1 crash was the heap crash above, not forcelod).
    - namemaps 18 vs 20: no effect; the batch 1 "hurts" was spread. Kept on (it keeps map header and blocks consistent).
    - strictvt: the OFF arm is unsafe. R6025 inside PhysXCore 4 s into the session (physics tick, `MKKE+34D7B7`). Stays on, not A/B-testable.
    - No crash, no written-after-free hit, and "already-freed blocks not captured" stayed 0 in these runs, so the skip was not exercised.
  - All defaults (the ON arms) spread from 0.7% to 3.5% of replayed frames between fight sessions, about 1.9% on average.
- **Fighter actor span was 0x40 bytes into the next heap slot (fixed 2026-10-17 03:35).**
  - Desync reports often named only P1 actor `+0x2C8/+0x2D0/+0x2E8` plus a 1-ulp Z drift. Their values meant different things in every fight (counters, a float, `0x80FF0001`, `0x01010101`), and a write watch on all threads never hit them.
  - Cause: the actor is `MKPawn` (UClass PropertiesSize at UStruct `+0x40` is 0x28C; `Engine.Pawn` is 0x27C) in a 0x2C0-byte GMalloc slot. The span was 0x300 ("provisional until the class size is known"). Every save captured, and every load wrote back, the first 0x40 bytes of the neighbouring slot: another live object, or a free block whose first dword is its free-list link.
  - The neighbour's bytes were also in the SyncTest checksum, so its own changes counted as sync errors. Writing them back corrupted it, which matches the old bin-29 crashes (blocks of 625–704 bytes, a position-like float over a free block's next link).
  - Now: `kActorSpan` = 0x28C. Saving is skipped if the actor's class PropertiesSize does not cover the span (fail closed), and the diagnostics compare 0x28C bytes.
  - Soak span28c (2 sessions, seed 6006): 11/1700, 27/1632, 11/516 sync errors per replayed frame (1.27% overall), no crash. Actor `+0x2C8..` no longer appears in desync reports. What remains: actor Location drift (`+0xDC/+0xE0/+0xE4`), occasional meter differences (−0.0075) after a script branch difference, and `AttachedActors`.
- **Second overrun: MK sub-heap headers (`heapobj`, fixed 04:02).** The new `SPANAUDIT` (every 600 saves, a heap span must end inside its GMalloc block) flagged `heapobj [0F718440,+C0)` in a 0x40-byte block.
  - Class `0xC66A3C` sub-heaps are allocated as `GMalloc(0x3C, 8)` (`0x00887601`, `0x0088773E`, constructor `0x42AAD0` writes up to `+0x38`). So the 0xC0 span restored two neighbouring 0x40 slots. The earlier reading ("-1/0 bin array after +0x3C; headers 0xC0 apart") was neighbouring objects.
  - The 0x0042AF1D crash that motivated 0xC0 was later traced to the partial `mkanimtrack_list` header.
  - Now: 0x3C for that class and never past the GMalloc block.
  - Soak heapobj3c (2 sessions): 58/3156 and 41/1708 sync errors per replayed frame (2.0%), no crash, no further SPANAUDIT hit. Both overrun fixes are memory-safety fixes. Their effect on the desync rate is within the fight-to-fight spread (span28c 1.27%, heapobj3c 2.0%).
  - The 84 failing loads of these four sessions, by worst gameplay difference:
    - position < 0.05: 43;
    - meter: 22;
    - position 0.05–1: 10;
    - position ≥ 1: 5;
    - RNG: 4.

    The meter differences are quantised gains missing in the replay (−0.0075, −0.0125, −0.02; +0.056 a few times), i.e. a hit or block resolving differently. They are consequences, not a separate cause (SoundDedupe only intercepts sound plays).
- **Testing `comprender`:** replays the fighter SkeletalMeshComponent's render-written LastRenderTime (`+0x228`, Pawn.Mesh = actor `+0x244`) per resimulated frame, like the actor's `+0xAC`. The component span restores it to S's value, while live frames see the renderer's update. A/B: `abtest comprender`.
  - Result: ON 39 errors vs OFF 26 (222+208 loads per arm), no help. Off by default.
- **The rare medium-heap crash is a dead script context's data span (fixed 04:44).** In comprender run 1, "written after free" fired on a 0x11000-byte block freed by `0x004D2E96`, then the heap crashed at `0x0042B2BC` with `ebx` inside a restored `script_data` span (+0x840). The freed block lay inside the span, not at its base, so the pending-skip did not apply.
  - The data-only branch of the script-span capture accepts contexts without a module. Those are dead (`ctxprobe`: module 0, the module was released). Their stale `[+0x84,+0x88)` range can lie over newer allocations, and the block-start test then passes on whatever block starts there now.
  - Recent sessions counted hundreds of such "data" spans (span28c: data 536).
  - Now: a context with module base 0 is never captured.
  - Soak deadctx (3 sessions, seeds 7001–7003):
    - no crash, no "written after free", no SPANAUDIT, and SCRIPTSPANS "data 0" in all sessions;
    - sync errors per replayed frame: 23/1724, 29/1752, 74/1660. Run 3's matchup is mostly position drift (21 of its 24 failing loads);
    - the 4 failed saves in run 3 are refusals at the fight-over transition ("object heap not discovered"), i.e. fail-closed as intended.
- **Live-vs-replay candidates checked (05:15–05:45), all ruled out:**
  - Component MaxDistanceFactor `+0x444` (render-written): its only consumer is the kinematic-update skip at `0x76DE82`, which needs `+0x484` > 0. The fighters have `+0x484` = 0.
  - Per-frame counters (`counterscan`: dwords of MKKE's writable sections that step by 1–4 per live frame). 20 found, none restored:
    - the engine-loop counter `0xE0AB30+0x14`;
    - the OnlineDesync index `0xFE2164`;
    - the 64-bit `0xE84720` with its "already run" copy `0xE858C0`. Both are inside `0x69D560`, which increments and checks in the same call, so it is not a cache that spans frames;
    - small stat counters at `0xE32B34..0xE353D4`.

    No frame-keyed cache that a replay could hit was found.
  - Current load-only diff (`unrestored.ps1`, results\unrestored_1017): 5150 pages changed and not restored. Almost all of it is unattributed (15k chunks "?").
    - The "SkeletalMeshComponent_37/_41 +0xF8/+0x100/+0x228" and "MKPawn_3 +0x80/+0xAC" labels repeat at addresses 0x600–0xE00 apart, so they are render-side records that point into those objects, not the objects themselves.
    - The fighter-owned unrestored fields are the intentional ones (bTicked `+0x80`, LastRenderTime `+0xAC`).
- **Animation input trace (`animtrace 1`, src/debug/AnimInputTrace.cpp, 06:00–06:40): first concrete live-vs-replay input divergence.**
  - **Method:** hook at the fighters' skeleton update `0x007741E0` (6-byte prologue, CodePatch). The first call per frame copies the component block plus the AnimTree and the named-node table nodes (`comp+0x280`, `+0x290`/`+0x294`). Replayed calls are compared with the live copy of the same GameFrame, separately for the first replayed frame after a load.
  - **Component level:** on first replayed frames the component matches live (8 of 550 differ). The only recurring difference is `+0x664/+0x66C`, which is task-graph bookkeeping written by workers (`0x71C359`, `0x7141E9`).
  - **Later replayed frames** differ in the bound-anim count `+0x3AC` (the replay is usually one higher) and the component Z (`+0x100`/`+0x3E0`).
  - **Node level, first replayed frames:** 99–132 of 500 differ, all in `SkelControlPinLimb` (leg IK) `+0x40` (ControlStrength) and sometimes `+0xC0`. The replay's value equals the live value of frame F, e.g. gf 6940 replay 0.8 = live gf 6944.
  - **Chain:** strength `+0x40` = `[bound+0x20]` (`0x007601B0`; bound = node `+0xC0`, set by `0x0076DF10`).
    - The bound objects are 0x60-byte elements of the component's TArray `+0x384` (vtable `0xD5A5E0`, ctor `0x00783E00`).
    - Each element's strength is set every frame by `0x0077BA40` from the component's TArray `+0x39C` ({float, float} per element, clamped). The same function binds an element (`0x0076DF10`) when its strength rises above a threshold.
    - The bound elements are also listed in the component's `+0x3A8` list.
    - Neither array's data was in the snapshot.
  - **`compweights` (capture `+0x384` and `+0x39C` data, restore when the blocks are unchanged):**
    - first-replayed-frame node differences went from 132/500 to 0/500;
    - but A/B gave ON 19 errors vs OFF 9 (218 loads per arm), and run 2 crashed in the bind function (`0x0076E094`, write through a null list with count −1, `esi` inside `compbound`).
    - Restoring the weights and elements while the links (element `+0xC/+0x10`, node `+0xC0`, the `+0x3A8` list, the element's helper `+0x14` → `comp+0x658`) stay live leaves the binding state inconsistent.
  - `skelbind` (node `+0xC0` only) and `skelbound` (bound elements only) are partial restores of the same state.
  - **`bindstate` (on, 06:48):** the whole binding subsystem is restored atomically per fighter at the end of LoadAB, or not at all when any block, count, node or helper changed since the save (`SaveBindState`/`RestoreBindState`).
    - The bind/unbind side effects are local: element vt+4 is a no-op, vt+8 resets the element's own `+0x34..+0x54`, and `0x00767060` removes from the `+0x3A8` list.
    - Trace results: first-replayed-frame node differences fell from 26% to 4.4%; replayed skeleton updates differing from live fell from 13.7% to 2.7%. Applied 705 times, skipped 156.
    - First A/B: ON 39 vs OFF 32 errors (433/432 loads). But in one session the restore never applied (0 restored, 730 skipped).
    - Skip reasons (logged per check, 07:45):
      - mostly check 8: a table node whose `+0xC0` is not a binding slot (a float, `0x3E5CFBBD`) vetoed the whole restore. Now `+0xC0` is written only where both the saved and live values are null or an element;
      - rarely check 6: the `+0x3A8` list was freed after the save (capacity 0), which is correctly skipped.
    - After the fix: 1109 restored, 5 skipped (99.5%). First-replayed-frame node differences fell to 3.6%.
    - **A/B with the fix: ON 29 errors / 670 loads (0.043/load) vs OFF 56 / 660 (0.085/load)**, i.e. per session 18 vs 19 and 11 vs 37. That halves SyncTest errors overall, with no crash and no SPANAUDIT hit. On by default.
  - **`listrescue` (on, 08:32):** a traced gameplay divergence went bound list → PinLimb unbound → root motion → a script running a different sequence (69 RNG draws, meter +0.056).
    - Its cause: the fighter's `+0x3A8` list was freed between S and F. The live header was null, so complist and bindstate both skipped.
    - Now, when the live header is null and the saved block is pending in the free quarantine, the saved header and contents are restored. OnLoad then keeps the block allocated, because it scans every restored span for references, the component span included.
    - A/B (2 sessions): ON 9 vs OFF 8 errors / 442 loads per arm. The rescue did not trigger in these sessions, so this is noise. No crash, no "written after free".
  - **Overall level in these A/B sessions:** 17 errors / 884 loads ≈ 0.5% of replayed frames, down from ~1.9% at the start of the day.
  - **Clean baseline, all fixes, seeds 7001–7003 (same seeds as deadctx: 1.3% / 1.7% / 4.5%):**
    - 39, 11 and 10 sync errors in sessions of about 400 loads (~1600 replayed frames), i.e. ≈ 2.3% / 0.7% / 0.6%;
    - no crash, no "written after free";
    - bindstate applied 2566 times, skipped 101.

    The sessions ran to the harness deadline without a "synctest stopped" summary, so the errors were counted from the `GGPO SYNC ERROR` lines.
  - **Cost:** SAVE PROFILE is unchanged at ≈ 3.6 ms/save. "ROLLBACK save cost" runs 55–110 ms/frame during SyncTest (a load plus 4 replayed frames every frame) and 2–4 ms outside it, the same pattern as before today's changes.
  - **Per-desync attribution (09:20):** each SyncTest desync now prints the traced stages per replayed frame since the load (`ANIMTRACE frames S+1..gf`: c component, n nodes, r root motion, m/M MoveActor in/out). Every replay pass is compared, not only the first. Two sessions:
    - about a third start with `r` alone: same component and node inputs, different root motion, so some untraced animation input differs (MK anim nodes, bone data);
    - the rest start with `c` or `cnr` on the first replayed frame. The node part is `SkelControlPinLimb` `+0x90..+0x98` (EffectorLocation) and `+0xA8..+0xB0` (JointTargetLocation), confirmed as FVector by `classprops`.
  - **`pinfloats` (on):** those two vectors are always restored by value.
  - **`reflectdata` (on, 09:43):** generalises that fix. For UE3 spans (fighter actors, the component, anim nodes) the merge never keeps live a dword that the class's reflection declares as data:
    - Float/Int/Bool/Byte/Name properties, and structs of type Vector/Rotator/Quat/Vector2D/Color/LinearColor/Plane/Matrix/Box/Sphere/IntPoint/TwoVectors/Vector4;
    - the struct pointer is UStructProperty `+0x78`, and the chain is walked via Next `+0x3C`;
    - cached per class: MKPawn 133 data dwords, SkeletalMeshComponent 274, PinLimb 11, EulerLookAt 8.

    First run: 1500 dwords restored by value in 5 minutes that the heuristic had kept live, 0 sync errors, no crash.
    - **Clean baseline with all fixes (10:00–10:35, seeds 7001–7003, comparable with deadctx):**
      - sync errors 17 / 22 / 19 over ≈600 / 600 / 900 loads, ≈0.7% / 0.9% / 0.5% of replayed frames (deadctx: 1.3% / 1.7% / 4.5%);
      - only 6 / 7 / 7 failing loads;
      - no crash, no "written after free";
      - bindstate applied 4187 times, skipped 13.

      Load counts are estimated from the bindstate tally (two fighters per load); these sessions logged no "synctest stopped" summary.
    - **Attribution with all fixes (11:00–11:35, 19 failing loads):**
      - most first diverge with `r` alone (root motion differs while component and node inputs match), often after one or more clean replayed frames;
      - a few with `c` alone: component `+0x58` (native, next to GraphInstance `+0x54`) and `+0xF8..+0x113` Bounds (BoxSphereBounds, now classified as data);
      - since the MK image and the xblocks matched at FightUpdate exit in the earlier deep diffs, the `r` cases point at an animation input outside them, or at cross-fighter reads in the jobs. Open.
    - **Struct types now classified as data:** BoxSphereBounds and the bitfield containers (LightingChannelContainer, RBCollisionChannelContainer, BeastPrimPropertiesContainer). Unclassified types are logged once (`REFLECT   struct ...`): SkeletalPose, MKClothSim, Map_Mirror, RenderOrderPrioirtyInfo, CinemaTransitionBlendInfo, Pointer.
    - **Baseline after BoxSphereBounds and the containers (11:45–12:30, seeds 7001–7003):**
      - sync errors 11 / 13 / 47 over ≈600 / 600 / 900 loads (≈0.46% / 0.54% / 1.3%), failing loads 4 / 5 / 15;
      - no crash, no "written after free";
      - overall 71 vs 58 errors in the 10:30 baseline: same level, within matchup spread.

      Run 3's failing loads are gameplay branches (7 position, 5 meter, 3 RNG). Its log is full of `ACTORARRAY actor +0x68` (AttachedActors) count mismatches at load, i.e. the known effect-attachment gap (`attachall` crashed earlier). That is the next candidate, besides the `r`-only root-motion cases.
    - **Fighter job order (12:25):** the trace records which fighter's skeleton update starts first per frame (Describe `o`).
      - The order differs between live and replay on 5–7% of replayed frames (118 and 184 of 2500).
      - Only 2 of 14 failing loads start with `o`, so job order is not the main cause of the `r` cases.
    - **`bindstate` with a reallocated list (12:40):** the most frequent first-frame component difference was `+0x3A8` alone, a reallocated bound-list block with the same count.
      - Check 6 required the saved block. It now writes the saved entries and count into the live block when that block has the capacity (as complistrebase does).
      - The list-freed case stays with listrescue.
      - **Baseline (12:42–13:15, seeds 7001–7003):**
        - sync errors 42 / 21 / 18, failing loads 14 / 9 / 7 (11:45: 4 / 5 / 15);
        - bindstate applied 1200 / 1200 / 1800 times, skipped 0;
        - no crash, no "written after free".

        In total that is 30 failing loads against 24, within the matchup spread, so the change is A/B tested on its own (`abtest listrebase`, the flag `gListRebase` restores the old same-block requirement when off).
      - **A/B (13:20–13:45, 3 sessions): ON 3 errors / 656 loads vs OFF 18 / 654** (3 vs 16, 0 vs 2, 0 vs 0). No crash, no "written after free". Kept on; the baseline's higher count was matchup spread.
      - **What the failing loads are (1145 + 1300, 54 loads):**
        - About two thirds first show as a meter or RNG difference rather than a position.
        - The most frequent first differing script call is native `0x808C80F0` with the same function and executor but a different float result (2.0 vs 26.5, 44.0 vs 0). Its operand is a fighter's MK character object with its mesh, i.e. a distance or position query. So the gameplay branches follow from the 1-ulp position drift, not from separate script state.
        - About 15 of the 54 have an `ACTORARRAY +0x68` (AttachedActors) mismatch shortly before. That is ≈7× the rate of loads without one, but a minority of failures. 183 of 197 mismatches are "more attached at the save than live" (effects detached or destroyed since).
      - **Trace gap fixed:** AnimInputTrace compared only the named-node table heads (`comp+0x290`), not the ticked chain (`+0x78`) that the snapshot restores. Unnamed sequence nodes were never compared, so the `r`-only cases ("inputs matched") may have had differing chain nodes. It now walks the chain (192 nodes, 0x400 bytes each).
      - **With the chain compared (16:30–16:46, 13 failing loads):** 8 still first show `r` with every component and node input matching. Live/replay root-motion ratios are round (1.25, 0.875, 1.125, 0.667), not ulp drift.
      - **Animation time step ruled out:** the component tick `0x773ED0` advances the MK anim tree (`0x4CBA60` → `0x4CB930`) and TickAnimNodes by `dt × rate`. The rate is `vt[2]()` of an MK object reached from the owner's MK object `+0xA0` (class `0xDE8260`). Hooked at the call `0x773F90`: 0 of 12,000 replayed frames had a different `dt × rate`.
      - **MK anim tree:** the fighters' trees (from comp `+0x284`, class `0xDE8D94`) are in the master image. Their first 0x100 bytes matched on first replayed frames; later differences (`+0xCC/+0xD4` blend ramps) follow the divergence. MK anim nodes tick once per HostFrame stamp (`+0x2C` vs `[0xDEF4B4]+0x6C`, count `+0x14`, divisor `+0xC`); position `+0x74`, length `+0x78`, rate `+0x7C`. The script's anim-position native reads the same `+0x74`.
      - **Whole master image (12 MB, one range) at P1's anim step, live vs replay:** most replayed frames differ only in counters (`0x114D4A84`, a packed pair ahead by a constant; `0x115EFC38`/`0x115EFD54`, frozen during replays, i.e. the between-tick writers). A noise-filtered run (addresses differing on >20% of frames excluded) is running to see what else differs on the `r` frames.
      - **Noise-filtered image compare (17:45–18:13):** the mid-frame image is order-dependent (the parallel component jobs: stamp counters off by one, UE3 pointers in effect objects), so many clean frames differ by 1–100 dwords. Failing loads stand out:
        - 735 dwords on the first replayed frame of load 6547: a new MK object exists only in the replay (a timer counting 1, 2, 3, floats ramping 0.05/0.10/0.15).
        - 169 dwords on load 6079: an array with an extra 12-byte element inserted.

        So the first replayed frame's fight update does something different: the divergence starts in gameplay, before the animation.
      - **Load 6547 in detail:**
        - At the load: `ACTORARRAY actor1 +0x68: count 3 at the save, 2 live (kept live)`.
        - Replayed frame 1: the first script call differs. The replay runs an executor (locals outside the image) that live did not run.
        - Result: P1 ends up with 4 attached actors (an extra MKParticleActor) against 3 in the original.

        The AttachedActors gap does cause failures. The earlier correlation (15 of 54) undercounted it because ACTORARRAY was capped at 40 lines per session; it is uncapped now and tagged with S.
      - **`attachall` A/B (18:15–18:41; the third session was cut off by a session interruption): ON 38 errors / 656 loads vs OFF 14 / 645.** No crash or "written after free". Restoring the attachment lists alone (re-linking effect actors whose own state is from the abandoned future) makes it worse; it stays off. Three loads found the live list freed (`list has no room (need 1, max 0)`).
      - **`fxactors` (23:28):** the actors attached to the fighters at the save are snapshotted like Tier E objects: span from `+0x3C`, class PropertiesSize capped by the block; load checks identity (GObjects index), block, and bDeleteMe; reflection data restored, pointers kept live.
        - **A/B (23:30–23:50, 3 sessions): ON 15 errors / 647 loads vs OFF 18 / 646.** No crash, no "written after free". Neutral within noise.
        - A count run confirmed the spans are real: 4,739 saved and 479 restored in six minutes, about one per load.
        - **Deep diff on every load (`deepdiff 2`, 00:08–00:35, 21 failing loads):** at FightUpdate entry and exit of the first replayed frame, the MK master image and the compared xblocks matched live on every failing load. The only reports were noise:
          - one xblock of UE3 pointers;
          - two MKScriptVM `+0xF48` cache records on loads that did not fail.

          So MK gameplay state is restored exactly and the first fight update replays exactly. The divergence starts after it, in the UE3 part of the frame: animation jobs on worker threads, actor ticks, movement. EngineTick exit is noisy (MK anim nodes `+0x8` hold per-frame heap pointers) and is not reported in this mode.
        - **One-core test (00:40):** the game pinned to one CPU (ProcessorAffinity 1) on seeds 7001–7003, against the same build on all cores. If cross-thread reads in the parallel animation jobs cause the remaining `r` cases, the rate should drop.
        - **One-core result (00:40–01:51): 17 failing loads (4 / 10 / 3) vs 17 on all cores (fxactors sessions 6 / 8 / 3), same seeds.** True parallelism in the anim jobs is not the cause.
        - **Root-motion producer:** the skeleton update `0x7741E0` zeroes the root-motion struct at comp `+0x5D0..+0x5F4`. The MK anim tree (comp `+0x288`, class `0xDE8D94`) evaluates into it at `0x7743FF`, taking:
          - the bone arrays `+0x2C8` (atoms, 40 B), `+0x318`, `+0x378` (28 B per bone) and `+0x39C` (8 B per bone);
          - the flag `+0x758 & 1`.

          Only `0x774232` and the apply function `0x762C10` write `+0x5E0` directly.
        - **Array contents at skeleton-update entry (5,500 replayed frames):**
          - `+0x318`, `+0x378`: 0 differ; `+0x39C`: 4 differ.
          - `+0x2C8`: 4,109 differ, on every replayed frame after the first.

          After the evaluation (at the root-motion hook) the atoms differ on only 181 of 5,500. There is exactly one skeleton update per frame, live and replay (6,784 of 6,786 live frames). So the stale entry atoms are overwritten by the evaluation and are not the cause; later per-frame bone writes (IK) differ and are render-side.
        - **Write watch on a bone atom (`atomwatch`, distinct writers):** live frames have one writer, `0x4D2A8A` via `0x774456` (the evaluation). The other writer seen was LoadAB itself (the harness's forced local rollbacks).
        - **Residual:** root motion with every compared input matching is 5 of 5,241 replayed frames. With the deep diff clean at FightUpdate, the remaining failing loads come from the UE3/animation half of the frame, at about 0.1% of replayed frames.
        - **SyncTest distance 2 (02:10, closer to real netplay):** run 1 crashed in a resim at `0x00793E0E`, reading `[0+0x20]`.
          - The detach path `0x00793DC0` looks up an actor's record in its Base's attachment list and dereferences it unchecked.
          - An effect actor was based on a fighter whose kept-live list no longer had it. Seen once before (noai-correct1, 2026-10-15).
          - Runs 2–3: 5 and 8 failing loads. In run 3, bindstate skipped 434 loads with reason 11: the bound list held an object that is not a `+0x384` element.
        - **Fixes (02:35):**
          - `attachrepair`: after every attachment restore and the actor give-back, every live Actor subclass whose Base is a fighter but is missing from its list is detached (Base = 0). The class chain is checked against the Actor class, found by name (`Actor_0`), failing closed. Fighters are left to RestoreAttach.
          - bindstate check 11 accepts a non-element list entry only while the same object is still in the live list.
        - **Distance 2 after the fixes (02:40–03:01, seeds 7001–7003):**
          - failing loads 9 / 6 / 6 over ≈1,200 loads each (≈0.6% of loads);
          - no crash; bindstate applied 2,400 times per session with 0 skips;
          - no repair needed in these runs.
        - **Localizing the `r` frames (03:20–04:30):**
          - HostFrame at the anim step: identical on all 5,500 replayed frames.
          - Fighter pads (0x60 records) at FightUpdate entry: identical on all 2,000 replayed frames, so inputs are ruled out.
          - The MK image at P1's anim step on each `r` frame (ring-buffered, printed only on `r` frames with matching UE3 inputs) has 200–780 non-noise dwords differing; the earlier frames of the same pass had 0–60.
          - Page hashes at FightUpdate entry and exit of those frames match the clean-frame noise (the same 3–6 counter pages).
          - So the MK state diverges after FightUpdate exit and before P1's anim step, inside the animation-job phase. The jump is not created by the fight logic.
          - Candidate: the order of the two fighters' anim steps (each advances its MK anim tree), live vs replay. Measured: the same order on all 3 `r` frames (it differs on 5–10% of frames overall), so not the cause.
          - **Correction (05:00):** the "after FightUpdate exit" conclusion was unsound. The entry/exit page hashes are not noise-filtered, and on `r` frames they showed the same 3–6 changed pages as on clean frames. The 200–780 diverging dwords can sit inside those pages unseen. The FightUpdate-exit point now keeps a full image ring with the same per-address noise filter as the anim-step compare (+72 MB). The `r`-frame report prints both diffs.
          - **Writer watch on the first differing address (writer_1/2, 04:52):** the first differing address was not gameplay state:
            - session 1, `0x111EFFC4`: an MK heap chunk header, written by the allocator `0x42B2D4/0x42B31E/0x42B517/0x42B546` (size and in-use fields);
            - session 2, `0x112C6AC0`: the script VM's scratch buffer (memset at `0x45C554`, memcpy at `0x45C8C9`, slot store `0x458869`).

            The lowest differing address therefore marks a different allocation pattern or script scratch, not the cause.
          - **Noise-filtered FightUpdate-exit compare (exitcmp_1/2, 05:10, 2 × 12 min, 17 sync errors each, no crash, 0 attach repairs):**
            - On 3 of the 4 `r` frames with matching inputs, the MK image at FightUpdate exit had 0 non-noise differing dwords, both on that frame and on the one before. At P1's anim step on the same frame, 266–353 differed. The localization holds: the divergence arises after FightUpdate exit and before P1's anim step.
            - The 4th (gf 1585) had diverged one frame earlier, in the same window: frame 1584 had exit 0 and step 301, then frame 1585 had exit 304.
            - In session 2 the differing records sat in one region on all three frames (`0x110C6A64…`, `0x110C6EA4…`). They look like an array of records {heap ptr, 3/6/12, floats 2.0–3.1, an MK id `0x10000559`, ptr `0x110E66E0`, floats ≈103}, with live and replay shifted by about one record: an inserted or reordered entry.
          - **Gated writer watch.** The watch on the first differing address now counts only writes inside that window: `CrashLog::gWatchPhase` is 1 live and 2 in a replay, set at FightUpdate exit and cleared at P1's anim step and FightUpdate entry.
            - **Result (gated_1/2, 05:30):** every in-window writer, live and replay, ran inside the MK object/script tick. It is `0x0045CAF0` (EngineTick calls it at `0x0080F710`, right after FightUpdatePlayers), which walks the 21 MK object lists and runs `0x0045A310` on each.
            - The writers were script VM frame setup (memset `0x45C554`, argument memcpy `0x45C8C9`) and opcode stores (`0x458A87`, `0x458AB4`, `0x458869`): the scripts took a different path, with the same MK image going in.
          - **Root cause found: a pad ring index not restored.**
            - NATIVETRACE on the same `r` frames: on 6 of 10 the first divergence is a boolean native, `0x008CB8E0` (3×), `0x008CB0D0` (2×) or `0x008CBD10` (1×), returning 1 live and 0 in the replay.
            - Each resolves a fighter object and calls a method that queries the pad's input history at `[obj+0xB40]`: `0x00425D80`/`0x00425E40`/`0x00426EC0` (30-entry ring at +0x58). These compare the index at +0x54 with a second index byte at **+0x5F8**.
            - The P2 pad in this harness is `0x00E27C5C`, outside the hardware pad array, so Tier A restored it as `aipad` with 0x5F8 bytes. The index byte stayed live; the hardware pads (`hwpads`, raw 6×0x60C) were always complete.
            - StateHash also stopped at 0x5F8, so checksums never saw it.
            - **Fix:** the AI/remote pad is restored and hashed as the whole 0x60C object (`kPadObjSize`; the hardware array's stride, the same class). Load-side A/B switch: `abtest padfull`.
          - Also added: an image compare after the MK object tick (point 2, call hook at `0x0080F710`), so `r`-frame reports split the window into object tick vs world/actor tick.
          - **padfull A/B (padfull_1/2, padtail_1/2; 4 × 15 min):** ON 25 errors / 890 loads, OFF 26 / 890, so neutral on the rate.
            - PADTAIL: the +0x5F8 index differed between save and live on about 6.5% of loads (92 of 1,400), e.g. saved 21 / live 27.
            - Kept on as a correctness fix, but it is not the main source.
            - The `r` frames with an input-history native first were a small subset; the larger remainder diverges elsewhere.
          - **Script RandomInt on the cosmetic stream (padfull_1 NATIVETRACE):** `0x008B8790` returned different small integers live vs replay (4/7, 6/1, 1/2, 5/7).
            - It is the script RandomInt: `0x00493C90` → the LCG on GSRandSeed `0x00E5352C` (rngB). About 40 functions draw from that stream (particles `0x59C120`/`0x710590`, UE3 and MK code), and it is the only caller.
            - rngB is restored at load, but any cosmetic draw the replay makes differently shifts the script's numbers. Sound dedupe suppresses replayed sounds, which is one known difference. Two PCs in lockstep would disagree the same way.
            - **Fix (`src/rollback/ScriptRng.cpp`):** the call at `0x008B879C` goes to a copy with a private seed (same LCG and scaling, same executor-state check).
              - The seed and the fight key are in Tier A (`scriptrng`).
              - It is seeded from RNG A when the fighters' MK id pair changes, so a rollback never reseeds.
              - `scriptrng 0|1` switches it per session.
            - The script random float, native `0x008B7FD0` → `0x00887F50` (the only caller, call at `0x008B7FF6`), also used rngB: 26 first divergences. It now draws from the same private stream, each step rounded to float as in the SSE original. Both call sites are checked before either is patched.
            - **Result:** only some matchups call these natives (a session seeds the stream only then). Sessions with such matchups had 6–20 RandomInt first divergences each before the fix, and 0 with it (scriptrng_on_1/2: 341 loads / 14 errors, 431 loads / 3 errors). In other matchups it changes nothing.
          - **Failing loads classified (171 `GGPO DESYNC` across today's logs; first NATIVETRACE in (S, G]):**

            | First divergence | Loads |
            |---|---|
            | path `0x458A00` vs `0x8B6DA0` at call #0 (the replay starts the frame with another executor) | 33 |
            | no native difference | 32 |
            | path `0x458B00` vs `0x45D520` | 19 |
            | input history `0x8CB8E0` / `0x8CB0D0` / `0x8CBD10` (pad ring index, fixed) | 23 |
            | anim position `0x8C80F0` | 13 + 9 |
            | other, under 10 each | rest |

            The call-#0 group means the order or membership of the MK object/script lists differs at the start of the object tick. The image compare's noise filter (addresses differing on >20% of replayed frames) could be hiding exactly such links. A dump of the noise set and per-point image counts in each failing-load report (`[img exit/objtick/step]`) are in the 07:37 build.
          - **Noise set and per-point counts (points_1/2, 07:37 build, 2 × 14 min):**
            - The noise filter hides only 5–6 dwords, the same at every point: per-frame counters, differing on 70–100% of replayed frames. It masks no list links, so that theory is ruled out.
            - Failing loads (points_2: 8 errors, 4 desync reports, no crash), as `[img exit/objtick/step]` on the first diverging frame:
              - `5096: 67/626/976` (frames before 1/1/3);
              - `1365: 153/717/928`, the first replayed frame after the load at 1364.
            - So on failing loads the image already differs at FightUpdate exit, the object tick multiplies it, and the anim step adds more. That is unlike the root-motion `r` frames above, where exit was clean.
            - Either the load left something unrestored that FightUpdate reads, or FightUpdate itself diverges. To separate the two, the 08:11 build adds a full-image point at FightUpdate **entry** (`[img entry/exit/objtick/step]`). It also logs `ANIMTRACE FIRSTDIFF` with the addresses at the earliest diverging point of each failing load.
          - **FightUpdate entry (entry_1/2, 08:11 build, 2 × 13 min, 14 sync errors, 10 desync reports, no crash):** on every failing load the MK image already differs at FightUpdate **entry** of the first diverging frame.
            - S=4381 → 4382 and S=1443 → 1444 are the very first replayed frame: 73 and 95 dwords.
            - S=6312: frame 6313 is clean at every point, then 64 dwords at 6314 entry. So it arises between frame 6313's anim step and 6314's FightUpdate.
            - The differing records:
              - MK reference objects, live `{vt 0xC66B14, obj, MK id}` vs replay reset `{0xD4D980, 0, -1}`, or the reverse. E.g. a reference to 0x12702800 (id 0x10001EFB) is reset in the replay, and a whole linked group of them (the next and previous links differ too) changes.
              - An MK heap chunk header flips between in-use and free (0x211/0x210), and the master heap's top size changes (0x92308 vs 0x920F8: one 0x210 block).
            - So before FightUpdate, one timeline releases an MK object (its references reset, its 0x210-byte block freed) that the other keeps.
            - A smaller case (S=1637, 11 dwords) holds UE3 pointers and floats (≈49 vs ≈53) in an MK record and stayed small.
            - The window between a load and FightUpdate holds the UE3 WorldTick (vt+0xBC, `0x0080DD40`; its entry is the snapshot boundary) and EngineTick's calls before FightUpdatePlayers (`0x887E80`, `0x4D36D0`, `0x4D3660`, `0x406790`, `0x5F5080`, `0x4D6160`, `0x49A0B0`, `0x42DE00`, `0x406760`, `0x81C430`, `0x81BB00`).
            - The 08:34 build adds image points at WorldTick entry (4) and EngineTick entry (5), live and replay. A report reads `[img wt/et/fu-entry/fu-exit/objtick/step]`.
          - **Netplay regression on the 08:34 build (loopback, stand-in peer, with the 0x60C pad hash):**
            - lockstep delay 2: 3 full fights, 1,410, 1,375 and 1,287 checksums matched, 0 mismatched;
            - P2P rollback delay 1: 1,705 matched, 3 loads;
            - no crash.
          - **Frame-boundary points (boundary_1/2, etexit_1/2; 08:34 and 09:17 builds):** with WorldTick entry, EngineTick entry and EngineTick exit added, and reports starting at frame S (the first frame a replay re-runs):
            - In 5 failing loads, frame S is clean at every point through P1's anim step (0 or ≤1 dwords). It differs only at **EngineTick exit**, by 53–63 dwords: the same reset/live MK references and one 0x210 MK block.
            - So the divergence arises after P1's anim step, inside the rest of the world/actor tick `0x0080F570` or EngineTick's tail. That is before any between-frame rollback code, so ActorDefer's release and the quarantine are ruled out.
            - Other failing loads start at the object tick (S=5177, 386 dwords), at FightUpdate exit or at the anim step: several sources.
          - **Effect spawns differ (etexit_2 frames log, OBJ+/OBJ- history):**
            - At gf 5177 one pass creates MKParticle ×2 and an MKFXLight (ids `0x1000096B/D/F`, MKObj ctor return `0x00439712`); the other pass does not, and those ids go to anim nodes.
            - At gf 5546 likewise: an MKSkeletalObj, an MKParticle and an MKListCheck in one pass only.
            - These are effects with UE3 actors; spawning one allocates and links MK objects, which is the 0x210-block / reference pattern above.
            - The 09:43 build records a 6-deep spawn stack for each MK object birth and tags replayed passes (`OBJ+R`). `tools/spawn_diff.ps1` lists the births that only one pass made for each failing load.
          - **One failing load traced end to end (spawn_1, load S=3188):**
            1. Frame 3188, the first replayed frame, matches live at every image point through P1's anim step. The animtrace flags are `-/-` for both players: component, nodes, root motion, MoveActor in/out, dt and MK tree all match.
            2. By EngineTick exit the replay has a new **MKScriptVM** (id `0x100009BE`, the 0x210 block and its linked references).
               - Spawn stack: `0x45D9C8` ← `0x8A4C16` ← `0x8A73AE` ← `0x4079BD` ← `0x80F794`.
               - That is EngineTick's second pass over the MK tick list (`0x407960` at `0x80F78F`) → an MK object's tick `0x8A7300` → `0x8A46E0`.
            3. `0x8A46E0` is a hit check: sweep/overlap tests `0x8B2F90`/`0x8B2C80`/`0x8B0B10`/`0x8B0910` with the radius at +0xF8. On a hit it sets +0x104 and starts script 0xCE (`0x45D960`). The replay registered a hit that live did not.
            4. Frame 3189: that executor runs first (the call-#0 NATIVETRACE group) and spawns 4 MKParticles through native `0x8CF5B0`. P2's attached actors are 2 vs 6, meter 0.77 vs 0.82, posy 166 vs 177.
            - So the call-#0 / object-release / effect-spawn groups are one mechanism: **a hit test that answers differently right at the collision boundary**.
            - The 10:06 build hooks the four test call sites and records arguments, pointed vectors, target bytes and result, live vs replay (`ANIMTRACE HIT`).
          - **The differing hit-test input (hit_1/2, 10:06 build):**
            - hit_1 (9 errors): at gf 1100, the first frame of load S=1100, site 3 (`0x8B0910`) returned 0 live and 1 in the replay. Only one input differed: vec0[1], `4300B6D8` vs `42896DB0` (128.7 vs 68.7).
            - On the replays of frames 1096–1099, vec0[1] already differed with equal results: 248.7/188.7, 233.7/173.7, 218.7/158.7, 203.7/143.7. The value falls 15 per frame and the replay is always 60 lower, i.e. 4 frames (the SyncTest distance) ahead.
            - arg1 differs on every call, but only by the resim stack depth (`012AFB70` vs `012AEFD0`). hit_2 (0 errors) shows the same vec0 offset once (gf 6950), with equal results.
            - Source (disassembly): `0x8A4BDC` passes `ebp-0x3C`, filled at `0x8A4AB8` by `0x00433460(obj)`. That reads the MK object's UE3 actor (`obj+0x08`) Location `+0xDC/+0xE0/+0xE4`.
            - That actor (a projectile or effect not attached to a fighter) was in no snapshot span: fighter actors are `actor0/1`, and fighter-attached ones are `fxactor`. So after a load it kept the abandoned future's Location and kept moving from there. Its MK object's state was restored, so it stayed in flight, but 4 frames further along.
            - Fix (10:29 build): `mkactor` spans hold the actor of every listed MK-object-derived object (global list `0xDE8214`, `obj+0x08`), minus the fighters' actors and those already saved as fxactor. They are saved from +0x3C and restored like fxactor spans: GObjects identity, live block, bDeleteMe, pointer-safe merge, reflection data restored by value. Location/Rotation are never kept live; this now also applies to fxactor spans, at span offsets 0xA0..0xB4. Toggle: `abtest mkactors`.
            - **A/B (mkact_1/2, seeds 8301/8302, alternate loads): ON 5 errors / 426 loads vs OFF 11 / 424** (5 vs 8, 0 vs 3). No crash. About 27 MK-object actor spans are saved per frame. An ON load restores about 18 of them; the rest are skipped by the identity/live checks (the actor was destroyed or replaced since the save).
            - mkact_1 logged no differing hit-test vector. mkact_2 still showed one projectile's Z 73 units (4 frames × 18.3) ahead in the replay (gf 8067–8086, results equal). Its load may have been an OFF one; a run with the spans always on follows.
            - **Always on (mkon_1/2, same seeds):** 10 and 6 errors, no crash. Only one hit test had a differing vector or result (hit_1/2 had 6). So the Location source is fixed. The remaining errors come from other groups:
              - an rng drift that first differs at the MK object tick (S=1095);
              - a recurring 9-dword pattern at FightUpdate exit (S=1695 here, S=3476 in hit_1, S=5545 in etexit_2). It sits at the same MK-heap offsets every session (`0x112016E0` vs `0x113016E0`: a fixed, early-allocated object). Live has 1656.0 / `0x40200` / 0.2 / `0xD8` / `0xFFFFFFFF`; the replay has 0 / `0x40400` / 0 / `0x84` / `0x80000002`.
            - The 11:23 build names the owner of each FIRSTDIFF address: the listed MK object with the highest start at or below it, with its type and offset.
          - **A queued move taken differently (owner_1/2, 11:23 build: 7 and 0 errors, no crash):**
            - Both failing loads of owner_1 (S=1379, S=1752) first differ at the MK object tick exit, in a block at `0x11586EE4`. It is zero live and holds script-like data in the replay (type tags 0xC, floats). P1's meter is 0.02 lower in the replay.
            - The owner lookup printed "?": the global list `0xDE8214` lacks script VMs and anim nodes. The 11:49 build also walks the constructor-tracked live set.
            - In the frames log at gf 1380, both passes destroy and re-create the same `Move` object (same id, same address), but from different call paths:
              - live: `0x8A76BC ← 0x8A95FA ← 0x8AB99A`;
              - replay: `0x8A7A08 ← 0x8AABDE ← 0x8AD0A5`.
            - `0x8AD087`: unless `[fighter+0x2A0]+0x44` is set, it calls `0x8AB6F0`. If that returns 0 it falls back to `0x8AAA70` (the replay's path). Live, `0x8AB6F0` started the move itself.
            - `0x8AB6F0` walks the fighter's queued move requests (`fighter+0x7524`, count `+0x75EC`). It skips inactive entries (`+0x6C`), checks the MK reference `+0x28/+0x2C`, and dispatches on the type `+0x70` (0..10). Type handlers include `0x8A99B0`, `0x8A9AB0`, `0x8A9BB0`, `0x8A9DD0`, `0x8A9EE0` and `0x8A9510` (live's path, call at `0x8AB995`); `0x90A520` with stat ids 0x35/0x36/0x42/0x43 runs on success.
            - The MK image matched at FightUpdate exit, so the input that differs is outside the MK heap.
            - The 11:52 build traces both the dispatch result (site 4, call `0x8AD095`) and `0x8A9510` (site 5: 7 arguments copied before the call, entry bytes +0x00..+0x1F and +0x60..+0x9F), live vs replay. HIT records are now matched per site and per-site call ordinal.
          - **Move-request entries carry per-fight state outside every span (movereq_1/2, 11:52 build: 16 and 24 errors, no crash):**
            - Site 5 (`0x8A9510`) gets the same entries with equal results, but each entry's +0x04 differs live vs replay (1/0) on whole frames. All of a fighter's entries flip together on 2-frame windows, e.g. gf 4571–4572 and 4591–4592.
            - At gf 1130 the replay made 4 more site-5 calls than live; that load desynced (P2 meter 0.5117 vs 0.4992), with a script VM block active in the replay only.
            - entry_2 logged the entries: `0x42920694`, `+0x248`, … for P1 with +0x04 = 1, and `0x473D355C`, … for P2 with +0x04 = 0. They have no GMalloc block: they live in the character's loaded data, which the snapshot treated as immutable during a fight.
            - So +0x04 is a per-side flag, flipped when the fighters switch sides. After a load across a side switch the replay used the abandoned future's side, and could read a special-move input differently from live.
            - Fix (12:33 build): `movereq` spans restore each fighter's listed entries raw (0x248 bytes) when the fighter still lists the entry. A histogram of the differing offsets is logged every 2000 restores (`MOVEREQ:`). Toggle: `abtest movereq`.
            - **A/B (mrab_1/2, seeds 8401/8402): ON 4 errors / 508 loads vs OFF 23 / 506**, about 6× fewer (0.008 vs 0.045 per load), no crash. This is the largest single gain since fxactors.
            - About 8% of entry restores changed something. Fields that differ between save and load: +0x04 (side, ~940 of 16000), +0x128 (~800), +0x1AC (35), +0x1B8 (26), +0x168/+0x16C, +0x190, +0x44/+0x48.
            - Lesson: "loaded data is immutable during a fight" is false for these records. Other per-fighter loaded data may hold state too.
          - **Clean sessions with mkactors + movereq (clean_1/2, seeds 8601/8602):**
            - clean_1: 8 sync errors from 2 failing loads; clean_2: 0 errors, 0 failing loads. No crash. Each session compared over 5,000 first replayed frames.
            - Both failing loads were RNG-only (no meter or position difference). RNGTRACE: "original 2 draws (caller `0x0044729E`), resim 1".
            - The cause is progressive costume damage:
              - `0x00447240(region, amount)` adds to the fighter's per-region damage `+0x4D8[region]` (12 call sites `0x448C8C..`).
              - When the integer level rises it calls `0x00493F80(fighter+0x10C comp, region)`.
              - That collects up to 4 unused pieces from the component's groups (`comp+0x570`, count `+0x574`; 16-byte groups with data +4 and count +8; 0x14-byte pieces with "used" at +0x04), draws `0x5F5080`, marks the pick used and updates the mesh (`0x78F1C0`).
            - The pieces were in no span, so a replay saw the abandoned future's picks as used, found no candidate and skipped the draw.
            - Fix (16:25 build): `costdmg` spans restore the pieces raw while the group still owns the block. Toggle: `abtest costdmg`.
            - Note: SyncTest errors only on the fight's last frame at the timeline restart, with failed saves and no `GGPO DESYNC` line, are an end-of-fight artifact (cdab_1: 4 such errors at frame 1764).
          - **Cross-machine hardening, FpuGuard (src/rollback/FpuGuard.cpp):**
            - The game runs with x87 control word `0x027F` and MXCSR control `0x1F80` on every thread (`fpu` command).
            - FpuGuard enforces those values on the game thread at every WorldTick entry and every resimulated frame, and on all threads every 600 ticks (suspend, Get/SetThreadContext, resume; no logging while a thread is suspended).
            - A driver or overlay that changes the FP environment on another PC can no longer make peers compute differently.
            - First runs (fpu_1/2): two non-game threads repeatedly showed x87 `0x037F` (the Windows default, 64-bit precision) and reset it themselves.
              - The 17:08 build pins only threads whose start address is inside MKKE.exe (the game thread and its task pool); `NtQueryInformationThread` class 9 gives the start address.
              - Other threads are reported once (`FPUGUARD thread … not the game's … left as is`) and are not suspended again.
            - fpu_1/2 (seeds 8701/8702): 6 and 0 errors, no crash. fpu_1's two failing loads are small residual cases:
              - S=3246: sub-display P2 posx difference. Fighter `+0x3B4` is 1 live vs 0 in the replay, and some MKAnimNodeSequence `+0x74` times are 4.0 ahead.
              - S=3854: MKParticle objects bound to different UE3 actor addresses (`+0x08`), and `+0xF8` differs.
          - **Environment fingerprint (src/net/EnvFingerprint.cpp, 17:08 build):**
            - The checksum channel also exchanges a fingerprint every 30 frames for the first 600 (`'MK9E'` packets) with three fields:
              - game content: FNV-1a of the lower-case path and size of every file under `DiscContentPC\Asset`;
              - this DLL's file hash;
              - the determinism-relevant snapshot switches plus the FP constants.
            - A mismatch logs `NETPLAY ENVIRONMENT MISMATCH: <field>` and stops the session (fail closed).
            - Steam sessions carry it too: SteamBridge's second link bridges the checksum socket.
          - **Two game instances on one PC (tools/two_instance.ps1, 2026-10-18 17:30–18:40):**
            - MKKE runs twice: A is launched through Steam, B directly from the game folder. Each instance gets its own DLL copy (build\clang, build\clang2: separate log and command files); the injector takes a PID (`inject <dll> <pid>`).
            - Same fight in both games:
              - `scriptseed <hex>` seeds RNG A right before each attract pick, from the seed and the pick's index in the coming fight;
              - the picks are random stage `0x8D69F0` (call `0x8D6A1E` → `0x810530`, table `0xE9234C`) and random character `0x8D6AB0` (call `0x8D6ADE` → `0x8105C0`, roster `0xE92358`), found with `rnglog 1` (RNG A callers between fights);
              - between fights, `0x80F6C5` makes thousands of timing-dependent draws, so seeding alone was not enough;
              - `FIGHT at gf=…: P1 <mesh>, P2 <mesh>` names the characters (SkeletalMesh asset of fighter+0x10C).
            - Netplay fixes found by it, all relevant to two real PCs:
              1. At the netplay start only RNG A was seeded. The script stream (ScriptRng) had been seeded from RNG A when the fighters were created, while the two RNG A values still differed. RNG B and the script stream are now seeded too.
              2. The seed was written after frame F's snapshot (a rollback to F restored the unseeded RNG). It is now written before.
              3. The game's pause keeps EngineTick running, and the idle draw (`0x80F6C5`) advanced RNG A once per paused tick. The peer that waited longer for the other left the pause with a different RNG, and every checksum differed from frame 0. The seeds are now re-applied every tick until the first unpaused one (`NETPLAY: seeds held through the pause`).
            - Starting at the map load (`netplayload 1`) is worse: the loading/intro phase consumes RNG A by timing, and checksums split at frame 10 before fighters exist. The first fight frame stays the start point.
            - **Lockstep, delay 2, AI removed as soon as it appears (`killai 2`): the first fight matched for all 1,943 checksummed frames in both games, 0 mismatches**, with random inputs from each side over UDP. The second fight started from different meters (P1 0.333 vs 0): the attract script hands out a random starting meter while the games still run independently. That is a harness limitation; Versus fights start at 0.
            - **Rollback, delay 1 (two_instance.ps1 -Netplay -NoAi -Delay 1):**
              - Fight 1: 35 and 33 loads (40 and 41 replayed frames); 1,941 and 1,943 checksums matched, 0 mismatched.
              - Fight 2: 1,938 and 1,943 matched, 0 mismatched, no loads.
              - No crash, 0 failed saves. These are real rollbacks driven by the other game's late inputs.
            - What this covers and what it doesn't: separate processes, separate timing, separate heaps and object ids, the same CPU. A different CPU and other machine settings remain for the two-PC test.
          - **Different timing between the two games (2026-10-18 19:00–19:45):** delay 0, and `-SlowB` (instance B on one core at below-normal priority).
            - Delay 0 at normal speed: fight 1 matched 1,057 frames, then split after a rollback on B (replay of frames 1051–1053; frame 1057 drew RNG A 3 times on B, 2 on A). That is the known replay residual; between two games it becomes a permanent desync.
            - With B slowed, every session split at frame 2. `tools/hashdiff.ps1` (per-frame region hashes of two frames logs) and the new first-desync report found four causes, all of which also apply to two real PCs:
              1. **Unsynchronized frames.** When GGPO refused the local input for 250 tries (the peer far behind), the frame ran anyway with the pads instead of GGPO's inputs. Now the boundary waits up to 5,000 tries, and the session stops instead (`P2P: no synchronized input …`).
              2. **Seed pin one frame too long.** The seeds were re-applied once more at the boundary after GGPO's frame 0 snapshot, so live frame 1 started from a state no replay of frame 0 produces: a rollback to frame 0 gave RNG 3 draws from the seed instead of 2 (D5A3E472 instead of 83BD5FCB, checked by stepping the LCG). The pin now ends with GGPO frame 0 (`NETPLAY: seeds released with GGPO frame 0`).
              3. **The first fight update runs before the session.** Inside one engine tick the fight update comes before the WorldTick boundary, so frame 0's fight update used each game's own pads: the AI's jump (0x09) on one peer, and a previous fight's held 0x8018 in `prev` on the other. While netplay is armed, both pads now get a neutral override (twice: prev = cur = 0) on every fight update until the session takes over (`GameLoopHook::FightBodyWillRun`). If frame 0 still differs, the session stops: `NETPLAY: frame 0 differs from the peer` (fail closed; usually different characters, sides or settings).
              4. **Absolute GameFrame.** Each game reaches the first fight frame at its own GameFrame: the intro waits on loading and audio (588 vs 808 with B slowed). Gameplay keeps absolute GameFrame stamps: input rings, fighter timers, script gates. The draw that split frame 2 was `0x00495AF0`, a script "roll at most every n frames" gate (`[obj+0x14] == 0 || n <= GameFrame - [obj+0x10]`). The update order also rotates by `GameFrame & 3`. MK9's own online start (`0x00884770`, from the script native `0x008BFC30` via `0x0081C080`, and from `ConnectionSyncCompleteHandler 0x00885340`) does three things: it rebases both fighters' stamps (`0x0087FA70`: `x > GF ? x - GF : 0` for char `+0x75F8`, 60 records ×0x124 at `+0x30EC/+0x30F0/+0x30F4`, and move-request entries `+0x38/+0x3C/+0x40`), sets GameFrame to 0, and seeds RNG A.
                - Now (`netplaygf`, default 100000, a multiple of 4): at the netplay start the fighters get the same rebase onto 100000, and GameFrame becomes 100000. Every other stamp from before the start reads as long ago on both peers.
                - The script clock (stopwatches, native 191) gets fixed bases (`ScriptClock::AnchorCanonical`: 2^42 ms, seconds at 2^17) instead of each machine's real clock at the map load.
                - All gf-keyed rollback structures start over, as at a timeline restart.
                - The value is part of the environment fingerprint.
              5. **GGPO frame 0 was a paused tick.** GGPO's 'running' event arrives at a boundary of the pause, and the unpause only takes effect at the end of that tick. So the live frame 0 ran no fight update (0 RNG draws), while a replay of frame 0 ran a full frame (3 draws on B after a rollback to frame 0). Paused ticks no longer become GGPO frames: frame 0 is the first unpaused one, and the seeds are re-applied right before its snapshot. Checksum frame 0 starts there too. A peer that pauses mid-session (pause menu) stalls the other, and the session stops after the wait instead of desyncing.
            - With 1–4 fixed, frame 2's live RNG agreed between the slowed and the normal game (D5A3E472 on both, including the `0x00495AF0` gate draw). The second attract fight still differed at frame 1 because of the AI's script executors (AI natives `0x0088E15B`): `killai` removes only the drones (pads), and the AI scripts' state from before the start differs between the games. This is a harness limitation (Versus has no AI).
            - With 1–5 fixed: the slowed run matched 1,413 frames (≈90 loads per side). A normal-speed delay-1 regression also split at frame 1413, while its second fight matched 1,940/1,942 (no loads). At frame 1412 one peer spawned one extra handler VM through the script event broadcast: native `0x008B6EC0` → `0x00408140` → `0x00407CF0` → `0x00407540`. `0x407540` walks the event-listener node list at `0xE0B4F0` and spawns, runs and destroys a VM per listener of the event.
              - That list header (0x14 bytes, same constructor `0x42D660` as the tick list `0xE0B504` right after it) was not in Tier A. Its nodes are in the MK heap, so a load across a listener add/remove left the replaying game with another listener set.
              - Added to Tier A as `mkevent_listeners`. That is correct on its own, but it did not remove the split: the next delay-1 run split at frame 1414 the same way.
              - `LISTENERS` (command `listeners`, logged at the netplay start and at the first desync) shows the list is empty at the start on both peers, so the listeners are registered during the fight.
              - The following run (one fight, 40/41 loads) matched all 1,942/1,941 frames; the split is intermittent.
            - **The 1413/1414 split explained (21:10):**
              - The `LISTENERS` dumps of a failing run (B slowed, delay 1) showed B starting the session with 2 listener nodes (`GameEventListener`, events 0x4C fn 2 and 0x10 fn 4) and A with none.
              - Both instances had created such listeners in the previous level, and both destroyed them at its teardown (`OBJ- gf=0`).
              - The list holds weak references `{obj, id}`, valid while `obj+4 == id`, and the game removes dead nodes only lazily, when it walks the list (`0x407460` add, `0x407540` broadcast). With the free quarantine a destroyed listener keeps its id. So whichever game had not walked the list since still held "live" stale listeners, and at frame 1412 (event 0x10 with the costume-damage hit) it ran their handler.
              - First fix attempt: unlink nodes whose listener is not a live MK object. It did not act in the next failing run, because the listeners were alive.
              - The real origin: B had created them at gf 517, just before its first fight frame, through `Toasty.mko`'s `toasty_init`:
                - `0x897830` loads the module (strings `Toasty.mko` / `toasty_init` / `FightingArt`), called by the MKScript native `0x8D6150`;
                - the listeners are created by `0x407501` ← native `0x8B6EA0`;
                - A never ran it in that level.
                - The "Toasty!" easter egg's listeners (events 0x4C and 0x10) run a handler on the first matching hit, and that handler draws RNG A.
              - Fix: at the netplay start the listener list is emptied on both peers before frame 0's snapshot, with the game's own unlink `0x42D6E0` (`NETPLAY: N script event listeners registered before the start were unlinked`). Every run without Toasty started empty, so empty is the canonical state. Cost: a Toasty armed before the start does not pop up in that fight.
            - **Results on the 21:31 build (first fight of each two-instance session):**
              - delay 0, B slowed: 1,942/1,940 matched, 0 mismatched (92/88 loads);
              - delay 1, normal speed: A started with the 2 Toasty listeners, they were unlinked, 1,942 matched, 0 mismatched (51 loads). This is the case that split at 1413 before.
              - delay 1, B slowed: 1,942/1,940 matched, 0 mismatched (59/12 loads).
              - lockstep delay 2, B slowed: 1,940/1,942 matched, 0 mismatched (322/131 barrier waits).
            - **Local player input (found while checking the pause behaviour):**
              - During a P2P session the local input sent to GGPO came only from scripted `hold` commands (`Input::TakeWantedInput`). Driving a fighter pad sets its external flag, so the hardware sampler `0x424A50` skips it, and a real player's controller or keyboard never reached the session.
              - Now the sampler's only call (`0x404E26`, main loop) goes through `MK9R_Sampler`. Hardware pads we drive are handed back to the sampler for that call with their 0x60C bytes saved; their sampled masks are read and the bytes put back, so prev/cur and the press rings change only through the synchronized input.
              - The local input is the OR of every local hardware pad's mask, so each player can use any local device on either side, without the menu/aux bits 0x0F00. Scripted input still wins (harness).
              - Logged as `INPUT capture: …`.
              - Not testable without a person at the PC: synthetic OS input does not reach the game.
            - **Delay-1 runs on the 20:30 build (normal speed):**
              - first fights: 1,942/1,941 matched (40/41 loads), then 1,942/1,940 matched (31/38 loads), 0 mismatches each;
              - earlier first fights split at 1413/1414 (the event handler above);
              - SyncTest regression on the same build (`noai_rollback.ps1 -Synctest 4`, seed 9101): 420 + 440 loads, 0 real sync errors. The 4 errors in fight 1 are all on its last frame (1680) with 4 failed saves and no `GGPO DESYNC` line: the end-of-fight artifact. No crash.
              - later fights in the same session split at frame 1 from the attract script's starting meter (0 on a different player in each game, set during frame 0) and the AI scripts' natives. That is harness noise; Versus has neither.
              - The neighbours `0xE0B434` (a reader/writer lock) and `0xE0B450` (an allocator) are not gameplay state.
            - **SyncTest soaks (2026-10-18/19, `tools/synctest_soak.ps1`: 4 fresh launches × 13 min, `noai_rollback.ps1 -Synctest 4`, one input seed each):**
              - 22:15 build (seeds 9201–9204): 4,007 loads, 0 crashes, 0 hangs.
                - 5 of the 8 failing frames are the end-of-fight artifact: the last frame, logged in the same instant as `Combat tick STOPPED`, with 4 failed saves.
                - 3 real failing frames. Two of them (9202 frame 752, 9203 frame 68) logged `ATTACH restore: P2's list has no room to re-attach P1 (num 0 max 0)`: P1 was attached to P2 (a throw) at the save. Since then, P2's attachment TArray had released its storage, so the replay left P1 unattached (posz 0 → 8.4).
                - Fix: `AttachListRoom` grows the list the way the engine grows a TArray, `GMalloc->Realloc(data, bytes, 8)` through the vtable (as at `0x00401BED`). It is used by RestoreAttach and the ATTACHALL rebuild, and fails closed if GMalloc's vtable is not `0x00C5ABB4`.
              - 00:46 build with the fix (seeds 9301–9304): 5,305 loads, 0 crashes, 0 hangs.
                - The list was grown twice (`ATTACH restore: list of … grown to 4 records`), with no error after either.
                - 1 end-of-fight artifact and 4 real failing frames (9301 frame 744; 9303 frames 108, 368 and 1480).
                - All 4, and the third real one of the first soak (9204 frame 1640), are the known call-#0 group: the replay starts the frame with another script executor (`0x8B6DA0` vs `0x458A00` as the first native). One timeline spawns an attached prop (SkeletalMeshActor/particles) and changes the meter by 0.05–0.07; the other doesn't.
              - Traced soak, same build with `animtrace 1` (seeds 9401–9403): 3,119 loads, 0 real failing frames, 0 crashes. The 8 errors are 2 end-of-fight artifacts. The tracer caught nothing to trace.
              - Real rate since the attach fix: 4 in 8,424 loads (about 1 in 2,100), all in the call-#0 group. Lockstep mode has no replays and so none of these.
              - **Long traced soak (seeds 9501–9512, 12 × 13 min, `animtrace 1`): 10,487 loads.**
                - 2 real failing frames, both in seed 9509 (load S=658: the classic pattern of one MK object released in one timeline and kept in the other).
                - The other sync errors are artifacts:
                  - fight ends: the replay ended at the timeline restart;
                  - "frame 4" errors where the harness started SyncTest while saves were refused (`object heap not discovered`, outside a fight). The session stops at once.
                - **1 crash (seed 9509), the known `eip=0` from `0x008A753D`, now seen twice:**
                  - An MKCollisionObj (`0x111C9E00`, id `0x10002ED9`) was born at gf 648 and died live at gf 660. The load to 658 revived it, and the divergent replay kept it alive.
                  - At gf 673 its tick called through `[obj+0x1D8]`, a 0x4C-byte GMalloc helper (vtable `0xC6A5B4`, allocated at `0x0089F2C4`, deleted by `0x004A5B70` from MK code). Its vtable slot held heap data: the block had been freed and reused.
                  - By the quarantine's rules this free should have been undone at the load: MK caller, freed at gf ≥ S. So the gap is not understood yet.
                  - The crash report now describes every heap-like register with its quarantine history (`CRASH?   ecx …: quarantine-released (freed gf … released gf … caller …)`). The next occurrence will show which path freed the block.
              - Totals for the 00:46 build: 18,911 loads, 6 real failing frames (about 1 in 3,000), 1 crash (about 1 per 2.5 h of continuous rollbacks every 4 frames).
              - **Second long soak (06:00 build, seeds 9601–9612): 10,905 loads, 0 crashes, 7 failing loads**, all in the call-#0 group.
                - `ANIMTRACE HIT` compared more than 6,000 replayed hit tests per session with 0 differing, so hit tests are no longer the source.
                - New tool `tools/death_diff.ps1`: for each failing load, it lists MK object deaths found in only one of the two passes.
                - It shows the pattern in 9607: **a particle dies in the replay's first frame and not live.**
                  - S=611: MKParticle id `0x100028F7` (age 122) died at 611 in the replay only.
                  - S=607: MKParticle `0x1000295C` died at 607 in the replay, but at 609 live. Its freed slot then went to a different object, so the addresses returned by spawn natives differ: call #114, `0x11113C00` vs `0x11112400`.
                - The MK-side death checks are deterministic, so something else kills it:
                  - `0x43E010`: `+0xAC` against GameFrame (`gameInfo+0x70`);
                  - MKParticle's tick `0x43F530`: the `+0xFC` countdown.
                - The 09:10 build records a 6-deep stack for every death (`OBJ- … stk=`), and death_diff prints it, so the next failing load names the killer.
              - **Third long soak (09:10 build, seeds 9701–9712): 10,251 loads, 0 crashes, 1 real failing load** (9707, S=1027). The other sync errors are fight-end artifacts. That load shows no one-pass death, so the early particle death was not caught this time.
              - Totals across the three long soaks: 31,643 loads, 10 real failing loads (about 1 in 3,000), 1 crash.
              - **Fourth long soak (seeds 9801–9812): 10,633 loads, 3 real failing loads, 0 crashes. The killer was caught** (9807, load S=1170).
                - death_diff: CachedParticle id `0x10000B8F` died at 1170, the replay's first frame, in the replay only. Killer stack `0x008AE986` ← `0x008AF38B` ← `0x004366BF` ← `0x004079BD` (MK tick list).
                - `0x00436690` reads the particle's UE3 ParticleSystemComponent (`[obj+0xF8]`) flags at `+0x2C0`, and deletes the MK object when bit 2 (bWasCompleted) is set.
                - The component is engine state outside every snapshot. After a load it still says "completed" from the abandoned future, so the replay deletes the particle on its first frame. The live timeline deleted it frames later, or not inside the window.
                - The freed slot then goes to another object: different addresses, different list order, and a script executor run first (the call-#0 group).
              - **Fix (01:09 build, `src/rollback/ParticleDone.cpp`):**
                - The flags read at `0x004366A6` goes through a stub.
                - The first frame a component is seen completed in the live timeline is remembered per component and MK id.
                - A replay behind that frame reads the bit as clear (`PARTDONE: … still runs`), so the particle dies on the same frame as live.
                - It never sets the bit, and fails closed if the site bytes differ.
              - **Soak with the fix (seeds 9901–9912): 9,580 loads, 1 real failing load (call-#0 group, 9905 S=1827), 0 crashes.**
                - The fix fires many times per session.
                - Before it, the four long soaks had 2, 7, 1 and 3 real failing loads per roughly 10,500 loads. That's an improvement, but not yet statistically conclusive.
              - Netplay regression on the 01:09 build (two instances, delay 0, B slowed): 1,942 / 1,940 matched, 0 mismatched, 92 / 90 loads.
              - **Confirmation soak (seeds 10001–10012): 10,598 loads, 6 real failing loads, 0 crashes**, so the rate did not drop clearly; other mechanisms remain. death_diff on them:
                - 10006 S=1727: one particle still died in the replay only through `0x004366BF`. The live particle had died another way first (its parent's teardown, `0x00433D70` ← `0x00439A36`), so the live timeline never ran the completion check on the completed component, and nothing was recorded.
                - 10006 S=1715 and 10010 S=1705: one-pass deaths of an MKCollisionObj and a script VM, killed from script natives (`0x008D10A6`, `0x0045CC30`) and the collision object's tick (`0x0089F825`). These are not particle completions.
                - 10006 (three loads): the replay runs one extra script executor at the end of frame S, with 3 extra script RandomInt draws.
              - **07:20 build, ParticleDone extended:**
                - During a replay, a completion with no live record is treated as the abandoned future's: the bit reads clear and nothing is recorded.
                - Records are keyed by MK id instead of by component: a replay re-creates an object born in the window with the same id and a new component.
                - Netplay regression: 1,942 / 1,941 matched, 0 mismatched, 90 / 90 loads.
              - **Soak on the 07:20 build (seeds 10101–10112): 10,260 loads, 7 real failing loads, 0 crashes.**
                - ParticleDone shows no measurable effect on the rate: 2, 7, 1 and 3 before it; 1, 6 and 7 with it, all per about 10,000 loads. It stays in: it removes a demonstrated mechanism, and regressions are clean.
                - **The dominant group now (5 of 7 failing loads, also seen before ParticleDone): a collision hit fires live but not in the replay.**
                  - death_diff shows "live only" deaths of an MKCollisionObj killed through its on-hit handler `0x0089F6E0` (runs the hit script via `0x0089F570`, then deletes itself), together with a Move, a CollisionContext, a BackgroundReactionNotifyPdata and a script VM: the hit reaction.
                  - Callers of `0x0089F6E0`:
                    - `0x008AE22B`, in `0x008ADF90`, a method reached through a vtable that reads the fighter's `+0x7A18..+0x7A3C` and the object's references `+0x114/+0x188`;
                    - the script native at `0x008D360D`.
                  - The traced hit tests (`0x008A46E0` sites) do not differ, so the decision is made in `0x008ADF90` or in its inputs.
                  - **Next step:** a trace site at `0x008ADF90` that records, live vs replay, its inputs and whether it reaches `0x008AE22B`.
                  - **Refined (10:51 build, soak 10201–10212, `animtrace 1`): 10,669 loads, 3 real failing loads (10203 S=1921, 10204 S=1507, 10209 S=1716), 0 crashes.**
                    - All three are this group: P2's or P1's AttachedActors count 3–4 live and 0 in the replay, meter 0.06–0.07 apart.
                    - The hit is decided by the hit check, not by `0x008ADF90`. In 10204 the live-only script VM is born at gf 1508 from `0x0045D9C8` ← `0x008A4C16` ← `0x008A73AE` (the start of hit script 0xCE in `0x008A46E0`). The collision object it then kills is reached through script native `0x008D35F0`: the frame after `0x0089F710` is a NATIVETRACE thunk, because `0x0089F6E0` keeps no frame of its own.
                    - NATIVETRACE at the failing frame: call #0 live is native `0x008B6DA0` by the live-only VM, returning the collision object. In the replay that VM does not exist.
                    - The new "live made N calls, the replay M" report was inconclusive. Its 60-line cap was used up early by sites 4/5 (move-request dispatch, routinely live-only with result 0), and the live record held only 32 calls a frame, which sites 4/5 can fill.
                  - **14:03 build:** live record 128 calls a frame; sites 4/5 left out of the missing and extra reports (caps 300). Two new sites: 6, the hit script start `0x0045D960` at `0x008A4C11` (result: the executor); 7, the hit check `0x008A46E0` itself at `0x008A73A9` (arguments: the object, its id, `+0x8`, `+0x104`, `+0x188`, `+0x18C` before the call; result: `+0x104` after it). Soak 10301–10312 runs on it.
                  - **Soak 10301–10308 (stopped after 8 sessions): the hit check itself decides differently.**
                    - In 4 failing loads (10301 S=2251, 10306 S=2204, 10308 S=1408 and S=2277) the same object, with the same id and the same gates before the call (`+0x104` 0, `+0x188/+0x18C` the same), gets a hit in one pass and not in the other. In 10301 and 10306 the replay hits 2 frames *earlier* than live (gf 2251 vs 2253); in 10308 live hits and the replay does not within the window.
                    - No call at sites 0–3 differed or was missing in those frames, so the deciding test is one of the five behind the `+0x194` switch at `0x008A493B` that were not traced: `0x008A4965` (`0x008B0B10`), `0x008A4A22` (`0x008B2F90`), `0x008A4A38` (`0x008B2C80`), `0x008A4A8F` (`0x008B3430`), `0x008A4AA5` (`0x008B3180`). Before the switch, `0x0089E220` (call at `0x008A491F`, nonzero = skip the test) can also stop the check.
                  - **2026-10-24 build:** sites 8–12 trace those five tests (target, both vectors, arguments, result) and site 13 the `0x0089E220` gate (its `ecx`, three arguments, result). Site 7 now records `+0x194` instead of the actor pointer `+0x8`, plus `+0x110`. Soak 10401–10412 runs on it.
                  - **Soak 10401–10407 (7 sessions, then interrupted): 1 real failing load (10401 S=1132), the hit group.**
                    - The deciding test is site 3 (`0x008B0B10`... reached through the switch path `0x008A4973` → `0x008A4BE0`), and the difference is its input: the object's Location (`0x00433460` = actor `+0xDC..+0xE4`). At the hit check of frame S: live (-115.7, 725.2, 147.6), replay (-109.0, 665.9, 206.9).
                    - The object moves about 15 units a frame (y up, z down), so the two positions are almost exactly 4 frames of travel apart, the same as the SyncTest check distance.
                    - History of the object (MKCollisionObj id 10000B78): created live at gf 1131; the load to 1128 destroyed it (born after S) and the replay re-created it at 1131 with a new actor `704A95C0`; live 1132 hit; the hit killed it at 1133 and ActorDefer kept the actor; the load to 1132 gave it back. MK object actors are saved and restored as `mkactor` spans (Location restored by value), so the span either was not applied or the movement within frame S differs.
                  - **2026-10-25 build:** every site-7 call logs a `HITPOS` line to the frames log (pass, gf, object, id, switch value, actor, Location, Velocity, Base, CreationTime, Physics, flags, result) to compare the trajectories of both passes. Soak 10501–10512 runs on it.
                  - **Soak 10501–10512: 6 real failing loads, 0 crashes; 4 are the hit group (10506 S=2382, 10507 S=1034, 10508 S=969 and S=1177). Cause found: the given-back actor lost its base.**
                    - The hit always diverges at frame S, the first replayed frame. Sites 3 and 10 differ in the object's position only.
                    - HITPOS, 10506: the collision object (id 10001386, created at 2381) is attached, Base `69D25F00`. Live at 2382 it follows its base (y 317.6 → 376.8, z 206.9 → 147.6) and hits. In the replay after the load to 2382 the same actor has **Base 0** and stays at its spawn point (-141.0, 317.6, 206.9) on every replayed frame.
                    - Mechanism: the hit killed the object at 2383. The MKObj destructor detaches the actor (Base 0, its record removed from the base's AttachedActors list) before ActorDefer postpones the actor's destruction. At the load ActorDefer gives the actor back, but the `mkactor` span keeps pointer fields live, so Base stays 0 and the base's list lacks the record. The ~4 frames of travel were simply the distance the base moved while the replay's object stood still.
                  - **Fix (2026-10-25 05:19 build, `RestoreMkAttach`, abtest `mkattach`, on by default):** every save records, for each MK object actor (not the fighters) whose Base is a live actor that lists it, `{actor, UObject index, base, its 0x60-byte record}`. At load, after ActorDefer's give-back and before `RepairAttachBase`: an actor that is still the same live object, not being destroyed, and now detached (or based on its saved base but unlisted) gets its record appended to the saved base's list (grown with `AttachListRoom` if needed) and Base set back. Actors based on something else now are left alone. Logs `MKATTACH`. Soak 10601–10612 (traced) runs on it.
                  - **Soak 10601–10612 with the fix (traced): 10,688 loads, 3 real failing loads (was 6 on 10501–10512), 0 crashes; 240 re-attachments, no "no room".** None of the three is the detached-actor case:
                    - 10602 S=2546: a collision object created at S+1 by a script (native `0x008D9523`) spawned in the replay with Location (0,0,0) and radius `+0xF8` 10 instead of (20,-468,162) and 28. The same executor's next operand read (`0x00458AE0`, call #123 at gf 2547) resolved an object reference to `12791800` live and to null in the replay. This is the script object-reference class, not attachment.
                    - 10606 (frame 864, gf ~1564) and 10611 S=2290: 1-ulp drifts of P2's Location (`0x44AEB7F9` vs `…F8`), with P2 actor `+0x13C` differing (`59C8AAE0` vs 0) in 10606. `+0x13C` is Actor.MotionControl (native, 0x10 bytes): a pointer kept live by the merge, so a controller present at that frame live is missing in the replay. Lead for the drift class.
                  - **Netplay regression on the 05:19 build (two instances, delay 0, B slowed): fight 1 clean (1,942 / 1,941 matched, 0 mismatched, 89 / 85 loads); fight 2 split at frame 1** (gf 100002), before any load of that fight: A made 9 RNG A draws at gf 100001 (callers `0x008B87C1` from executors `12649CE0` and five from `12649C00`, `0x00495B2D`), B made 3. Not yet known whether this is the re-attach leaving state behind across rounds or an older start-of-round issue. A/B queued: the same regression again with the fix on, then with `mkattach 0` (new command).
                    - **A/B result: not the re-attach.** With `mkattach 0` fight 2 split at frame 1 the same way (fight 1 clean, 1,942 matched, 93 loads). The deviating instance varies (B in run 1, A in run 2); the other always had checksum `FDDDF7D0`.
                    - **Cause: the fighters' input history is not synchronized between rounds.** At gf 100001 of fight 2 the P1 pad's previous record was `0x24` on A and `0x1000` on B, ring write indices 13/19/13 vs 14/21/14, pad hashes different; script executors were the same on both. The edge pass `0x0087F440` turns cur vs prev into press/release ring entries (`0x00425770` / `0x004256A0`, 16-byte records with the GameFrame stamp, 30 per ring at pad `+0x58/+0x238/+0x418`), and a per-player last-6-buttons list sits at `0x00E22170 + player*0x1C`. Round 1 is entered from a neutral menu, so it matched; round 2 inherits each game's own between-rounds history.
                  - **Fix (09:08 build, `Input::CanonicalizePadsForNetplay`):** at the netplay start, and at every tick the seeds are pinned (the start pause), both fighter pads are cleared past the header (`+0x0C..+0x60B`: cur, prev, rings, indices, tail) and the players' last-buttons lists zeroed. This happens before frame F's snapshot, so a rollback to F restores the neutral state. Regression with 3 fights (two round transitions) running.
                  - **3-fight regression (09:36, unlocked desktop): fight 1 clean (1,942 matched, 95 loads); fights 2 and 3 still split at frame 1.** The pads now matched on both sides at the start, so they were not the only cause.
                    - At gf 100001 of fight 2 both games ran extra executors fight 1 did not have (A 13 RNG draws, B 9; caller `0x0088E15B` is AI code).
                    - **Cause: AI drone removal timing (test harness).** The harness sets `killai 2` (remove drones at every tick exit) at setup, but its per-fight `killai` (mode 1) also switched that mode off. From round 2 on, the drones the game re-created were removed only by that command: on A while paused at gf 100000, on B after the session had started (gf 100009 in fight 2, 100003 in fight 3). A real match has no drones; the netplay start should guarantee it rather than depend on an outside command.
                  - **Fix (09:56 build):** a plain `killai` no longer turns the automatic mode off, and the netplay start (with the seeds and pads, every pinned tick, before frame F's snapshot) removes every AIDrone itself (`CommandChannel::KillAiDrones`). Note: the earlier two lock-screen failures ("did not reach the netplay start", B frozen at gf 598) were with the workstation locked.
                  - **Netplay regression on the 09:56 build (two instances, delay 0, B slowed, 3 fights / 4 sessions): 0 mismatches in every session.** A 1,942 / 1,935 / 1,942 / 1,935 matched, B 1,940 / 1,942 / 1,940 / 1,942; rollbacks A 92 / 0 / 94 / 0, B 84 / 0 / 91 / 0; 0 failed saves. Round transitions now stay in sync. (Sessions 2 and 4 had no loads: no misprediction with that input pattern.) Untraced SyncTest soak 10702–10712 runs on this build (10701, on the 09:08 build: 885 loads, 0 errors).
                  - **Untraced soak 10702–10712 on the 09:56 build: 13,203 loads, 4 real failing loads (≈0.03 %), 0 crashes.** With 10701: 4 / 14,088. Earlier untraced soaks: 7 / 10,905, 1 / 10,251, 3 / 10,633, 1 / 9,580, 6 / 10,598, 7 / 10,260. The collision-hit group, the dominant class before, did not appear.
                    - 10704 S=2414 and 10707 S=1305: 1-ulp drifts of P2's Location (the float-drift class; lead: Actor.MotionControl `+0x13C` kept live).
                    - 10709 S=1116: P1's AttachedActors had 0 records at the save and 1 live; the merge keeps the list live, so the replay started with an actor attached after S (P1 z 4.16 vs 0, meter 0.06 apart). Fighter lists carried extra future records into 51 loads of this soak and only this one failed; the full reconciliation (`attachall`) stays off because it crashed before (twice7).
                    - 10709 S=2013: P2 meter 0.035 and position apart, not identified (untraced).
                  - **The drift class is Actor.MotionControl.** In all three drift failures (10606, 10704, 10707) the fighter actor's `+0x13C` was nonzero live (`59C8AAE0`, `434769C0`, `4574FEE0`) and 0 in the replay already at the first replayed frame's FightUpdate entry; Location drifted by 1 ulp in that frame.
                    - A distinct-writer watch (`hwbpactorall 0 13c 1`, new third argument) found the setter `0x00786200` (slot k, controller: deletes the old one, stores the new; when all four slots are empty it calls `0x00799530`), reached from a script native through the attachment code `0x00793C50` → `0x00793D25`; the clear path is `0x00785380` (deletes all four, zeroes them).
                    - The controller is class `0x00D5A85C`: 12 bytes `{vt, ref, owned block +8}` built by `0x00793BA0`; its destructor `0x00789530` frees the `+8` block; method `0x0078D1B0` looks the actor up in its Base's AttachedActors (a fighter following what it is attached to: grabs, throws).
                    - The actor span keeps the four pointers live, so a load inside such a motion kept the future's empty slots.
                  - **Fix (13:23 build, `RecordMotionCtl` / `RestoreMotionCtl`, abtest `motionctl`):** every save records both fighters' four slots, each controller's 12 bytes and its owned block's content (≤ 0x200 bytes), and registers both blocks with the free quarantine (frees after the save are deferred and handed back at a load to it). At load, after the quarantine's give-back, each slot gets its saved controller and content again; a controller the abandoned future created is deleted with its own deleting destructor. Slots holding another class are left live and logged.
                    - One-session check (10811): 893 loads, 0 errors, no crash; saved controllers put back and one future controller deleted. Slot 3 holds a second, long-lived class (`0x00D5A150`) for the whole fight; left alone.
                  - **Untraced soak 10901–10912 on the 13:23 build: 13,564 loads, 4 real failing loads, 0 crashes.** The motion-control restore fired only a few times per session, so this soak cannot show its effect. The mix changed:
                    - 3 of 4 are fighter attachment lists carrying the abandoned future's records: 10904 S=1184 (P2 list empty live, 4 records in the replay), 10904 S=1896 (P1 list block differs), 10911 S=805 (P1 count 7 live vs 12 in the replay).
                    - 10909 S=1125: a 1-ulp drift of P1's Location with no `+0x13C` difference.
                  - **`attachfuture` (16:39 build, on by default, abtest `attachfuture`):** only the first half of `attachall`. At load, actors attached to a fighter now but absent from its list at S (recorded every save) are detached with the engine's SetBase(NULL) (vt+0x190, as RestoreAttach does); no list rebuild or re-basing. Each detach logs the actor's class and its MK owner (`ATTACHFUTURE`). `attachall` crashed in twice7 long before effect and MK actors had their own state restored.
                    - One-session check (11001): one detach (an MKParticleActor owned by an MKCollisionObj, attached to P1 after the save), then 2 minutes later a crash in a resimulated EngineTick at `0x00B9000C`, the HUD handle lookup, seen once before attachfuture existed.
                  - **Crash `0x00B9000C` explained and guarded (16:46 build, `HandleGuard`).** `0x00B8FFD0(handle)` (33 HUD/UI callers) reads table `[0x00FE2004]` (`+0x44` entries, `+0x48` count) and returns `entry->+0xA4`, or 0 for an invalid handle. The caller `0x008433E0` passes a handle stored at `+0x1C` of a HUD object (MKHUDManager in esi). The handle came back with the MK heap image, but the library's table is outside the snapshot and the abandoned future had emptied that slot, so the entry was null. The function is now replaced by the same lookup with a null check (patched at its entry, original bytes checked): an emptied slot answers 0, as every caller already handles; logged as `HANDLEGUARD`. HUD only, no gameplay effect. Soak 11101–11112 runs on this build.
                  - **Soak 11101–11112 on the 18:01 build (HandleGuard plus the online-mode code, 20:28–22:51): 12,178 loads, 1 real failing load, 1 crash.** The 11 other failing-load groups are timeline-restart artifacts (the last check before `replay ended at gf=4` / "4 failed saves", or 11107's "ring lost it" right after a restart). The real one: 11101 S=1667, the original pass had no script executor at the draw and the replay ran one more script draw (executor `112AA140`), the script-executor class. Fight counts of 27/30 (11107, 11110) are the known short-restart artifact.
                    - **Crash `0x00B914C1` (11105, resim EngineTick, HUD manager in edi), right after two `HANDLEGUARD` lookup hits:** the same emptied-slot case in the library's *release* of the `+0x44` table, `0x00B91480(handle)`, which reads `entry->+0xA4` unchecked. The `+0x5C` table's release `0x00B90070` has the same shape; the `+0x38`/`+0x50` releases (`0x00B90C80`, `0x00B91690`) and the raw lookups `0x00B8FF80`/`0x00B90020` already test null. **Fix (22:56 build):** both releases are entered through a copy of their checks plus a null test; a filled slot resumes the original after its prologue (`0x00B914B3` / `0x00B900A3`, frame rebuilt), an emptied slot (already released) returns and logs `HANDLEGUARD: release of HUD handle …`. Soak 11201–11212 runs on it.
                  - **Soak 11201–11212 on the 22:56 build: 12,631 loads, 9 real failing loads, 1 crash, 0 HANDLEGUARD hits.** Failing loads: 11202 S=1295 (meter/position), 1851 and 2351 (P1 z), 11204 S=1525 and 1533 (rng), 11205 S=1829 and 2537 (P2 z 0 → 4.156, the attachment class), 11212 S=1489 (meter, rng). Five restart artifacts.
                    - **Crash `0x0045B168` → MSVCR90 string compare (11203, resim EngineTick, right after "ACTORARRAY S=886 … count 6 at the save, 1 live"):** the same signature as `baseline_0917_3.log` (2026-10-17, also right after a 6 → 1 attached-count load). `0x0045B0F0(name)` walks the script object registry list `[0x00E39FB4]` (node +4 object, +8 object id, +0xC next, +0x18 removed flag; `0x0042D6E0` marks a node removed). A node is used when object+4 equals node+8; then the object's name FString (+0x14 data, +0x18 count) is compared. Both node and object were in the restored MK heap (mkmaster +0x123774 / +0x136060; 0917: +0x122060 / +0x1362D0), and the name pointer was garbage (`9F915B97`). Callers: `0x0045CF20`, `0x0045D58F`, `0x0096348D` (by-name lookup). Not yet explained: mkmaster spans are restored whole, so the garbage was either in the S image or written by the replay. **Diagnostic (01:50 build):** for every crash register inside a restored span, CrashLog now logs 12 dwords saved vs live (`CRASH?     reg +0..+0x2C saved[>live]`).
                    - **attachfuture skipped empty lists (fixed, 01:52 build):** `RecordAttachAll` treated a fighter list with released storage (data 0, num 0) as unusual and marked the fighter "leave alone", so the loads whose list was empty at S, the commonest attachment-class case, were never reconciled. 11205 S=1829 and S=2537: "ACTORARRAY … count 0 at the save, 1 live", no ATTACHFUTURE line, then the replay kept a SkeletalMeshActor on P2 (P2 z 0 → 4.156). Such loads were frequent (up to 20 per session in these soaks). An empty list is now a valid record. Soak 11301–11312 runs on the 01:52 build (this fix plus the crash dump).
                  - **Soak 11301–11312 on the 01:52 build: 12,196 loads, 7 real failing loads, 0 crashes; ATTACHFUTURE detached in 10 of 12 sessions, no z 0 → 4.156 failure left.** Failing loads: 11301 S=1662 (script executor: the original ran native `808B6DA0` at gf 1664, the replay did not; P1 then had 2 attached actors instead of 5, no ATTACHFUTURE at that load), 11306 S=1822 and 2282 (P2 z drift, 0.4 / 0.007), 11307 S=918 (P2 y/z, meter), 11308 S=2365 (rng), 11309 S=2404 and 2455 (sub-0.001 drifts). Restart artifacts as before.
                  - **Netplay regression on the 01:52 build (two instances, delay 0, B slowed, 3 fights, 05:00–05:15): 0 mismatches in every session** (1,935–1,942 frames matched), 90–101 loads per fight on the rolling side, 0 failed saves. Packaged as `dist\MK9Rollback-netplay-20261026-0455.zip`.
            - New diagnostics: on the first `NETPLAY DESYNC` each peer logs its gameplay state of that frame and the RNG A callers (with the script executor) of the three frames before it (RngTrace ring now 64 frames). `P2P boundary:` lines show GGPO frames 0–2 with gf, pause flag and inputs.
          - **Netplay regression on the 12:33 build (loopback, stand-in peer):**
            - lockstep delay 2: 1,352 checksums matched, 0 mismatched;
            - P2P rollback delay 1: 3 fights, 1,684 / 1,746 / 1,717 matched, 0 mismatched, 1–3 loads per fight, 0 failed saves;
            - no crash.
          - **New crashes (one each, not yet recurring):**
            - `0x00B9000C` in a resim: a static library's array lookup (`0xB8FFD0` from `0x8433EB`) got a null element.
            - `eip=0` live, from `0x008A753D`: an MK object's `+0x1D8` referenced a GMalloc object whose vtable pointer was heap data, i.e. freed and reused.
          - **Resim crash `0x0043E117` → `0x00793E0E` (writer_2):** an MK effect object's actor had Base = another effect actor (not a fighter) that did not list it. `0x007896F0` (find own record in Base+0x68) returned null+4.

            Cause: `fxactor` spans kept only the offsets that were pointer-like at the *save*. A base effect with no children at S had its AttachedActors TArray restored to {null, 0, 0}, while its live child's Base stayed live.

            Fix: fxactor spans also keep offsets that are pointer-like now, as actor spans have since 2026-10-15 (the triple rule keeps data/num/max together). `RepairAttachBase` now checks every actor's Base, not only fighters.
        - **Netplay regression on the 02:38 build (loopback, stand-in peer, 03:03–03:15):**
          - lockstep delay 2: a full fight of 1,332 frames, 0 loads, 4 barrier waits, 1,308 checksums matched, 0 mismatched;
          - P2P rollback delay 1: a full fight of 1,612 frames, 2 loads, 9 replayed frames, 0 failed saves, 1,588 checksums matched;
          - no crash in either.
        - Kept on as the more complete snapshot, but the effect actors' own data is not what diverges. Their attachment links (Base and the fighter list, pointers kept live) and their components (particle systems) are still the abandoned future's.
    - **A/B (2 sessions): ON 4 errors / 423 loads vs OFF 14 / 423** (2 vs 4, 2 vs 10), about 3.5× fewer. That is ≈0.01 errors/load, ≈0.25% of replayed frames. No crash, no "written after free"; 2500–3000 dwords restored by value per session.
  - **Watch item:** the quarantine block count grows during a fight (45k → 88k blocks in baseline run 1, vs 8–15k peaks before) and drops to a few hundred at the timeline restart. Private memory stays flat (1.0–1.1 GB) and the blocks are small, so it is not a leak.
  - **Root motion (hook at `0x00762C10`, RootMotionDelta `+0x5E0`):** after bindstate, 0–4 of 2200–2400 replayed frames differ from live.
  - **MK's position update → `UWorld::MoveActor 0x007A6F00`** (hooked with a return-address stub): 898 replayed fighter calls with identical inputs, 0 with a different result.
  - So the remaining 1-ulp Location drifts are not explained by animation output or the collision sweep in those runs; longer traced sessions are running.
  - **Earlier status of the partial restores:** `compweights`, `skelbind` and `skelbound` are off. A correct fix restores the binding subsystem atomically: elements including links, node `+0xC0`, the `+0x3A8` list and its count, the weights, and the helper. It must also account for the elements' vt+4/vt+8 bind/unbind side effects. This is the strongest lead for the remaining position drift.
- **Netplay regression on the 05:10 build (loopback, stand-in peer):**
  - Lockstep, delay 2 (`regress_lockstep`): full fights of 1322 and 1433 frames, 0 loads, no crash.
  - P2P rollback, delay 1 (`regress_p2p`): fights of 1760 and 1718 frames with 1–2 loads each, 0 failed saves, no crash.
  - The checksum side channel reported 0 mismatches. The stand-in peer only echoes it, so this tests the channel, not cross-machine determinism. That still needs the two-PC test (docs/netplay_lan_test.md).
- **UE3 reflection (`classprops <class>`):** fighter actor field names, offsets and types are in [mkpawn_layout.md](mkpawn_layout.md). The fields seen in desync reports:
  - `+0x68` AttachedActors;
  - `+0x6C/+0x70` its count/max;
  - `+0xC8` Touching;
  - `+0x1B0` DeleteListeners;
  - `+0x1E8` AvgPhysicsTime;
  - `+0x20C` Floor (`+0x214` = Floor.Z).

  MKPawn also has `bIgnoreForDesyncs` at `+0xA8`, a hint that the game has its own desync detection. At load, `Touching` never differed (ACTORARRAY); `AttachedActors` did in about 45 of ~960 loads.
- **Script context data:** `SCRIPTSPANS` showed "data 0, rejected (not a block start) N" in every session. `ctxprobe` showed that the rejected contexts are dead: their module pointer is already 0 and the data range is decommitted. Live contexts are captured through their module (`script_module`). Rejecting them is correct.
- **Top NATIVETRACE divergences** `0x0045C280` / `0x00458A00`: the script call opcode (`0x45C090`) and an operand read. They write no result of their own, so the recorded "result" is whatever the executor's result slot already held. It grows over the whole process lifetime, across fights (0x109 → 0x4DD → …), as the sound-play handles do. There are 217 NATIVETRACE lines but about 60 sync errors, so most of these divergences are cosmetic.
- **Lockstep over Steam P2P works in self-loopback (lockstep_steam2, 19:30).**
  - Setup: `noai_rollback.ps1 -P2P -Lockstep -Delay 3 -SteamPeer <own id>`.
  - Result: two full fights (1,305 and 1,271 frames), 0 loads, at most 2 barrier waits, checksums all matched (1,281 and 1,247), 2,644/2,643 Steam packets, no crash.
  - Fixed on the way: GGPO's `DiscardConfirmedFrames` discarded inputs before any frame had been requested (the lockstep wait idles before the first synchronize), so `GetInput(0)` asserted at input_queue.cpp:134. It is patched in tools/build_ggpo.ps1 to discard nothing until a frame has been requested.
- **Final regression (synctest56, current defaults):** 3 fights, 4.7% / 4.2% sync errors, no crash, hang or audit failure.
- **Measurement note:** GGPO "sync errors" count repeated checks of the same frame. Unique diverging frames per fight session (about 12–35 of ~1,400) are the better measure. Fight-to-fight spread (characters, moves) is larger than the effect of single fixes since synctest38.
- **script_data heap corruption fixed (synctest31):** a medium free-tree crash `0x0042AD08` had `esi` inside a restored `script_data` span at `+0x8C4`. The span started at `0x42619D04`, a variable table 4 bytes into its allocation. The GMalloc block lookup also answers for interior pointers, so the span ran over the next chunk's header. Spans must now start 8-aligned (MK chunk header size); the same applies to script modules.

## Gate D experiment 3: Tier A+B snapshot (design notes)

Built from the static map in [state_ownership.md](state_ownership.md) §5:

- **Tier A (raw):**
  - GameInfo 0x82E0, including PlayerInfo[4]; HostFrame is kept live.
  - Hardware pads (6×0x60C) plus the AI pads.
  - RNG A `0xE7C054` and RNG B `0xE5352C`.
  - MK object globals: id counter `0xDE820C..`, unlink/free-node area `0xE393D8..`, tick list `0xE0B504`.
  - Script globals and the 21 list descriptors `0xE39DF0..`.
  - The 13 pool objects and the two GMalloc heap objects.
- **Tier B (raw):** the MK object heap image. Its allocation is discovered at runtime from a fighter object via VirtualQuery; about 1.5 MB is expected.
- **Tier C (partial):** the fighters' UE3 actors, with a pointer-safe merge.
- **Guards:**
  - refuse save/load unless script depth `0xDE8628 == -1`;
  - refuse a load if the id counter changed since the save (object churn), unless `loadabforce`.
- **Per-frame hash:** now also covers `gamefull` (0x82E0), `mkheap` (whole image) and `rngB`, and logs `newobj` (MK objects created that frame) and the script `depth`.

**Run plan:**
1. `tools/restore_trial.ps1 -Ab -Idle`: expect bit-identical windows, including the mkheap hash.
2. `tools/restore_trial.ps1 -Ab`: expect the load to be refused because of churn. The log then shows how many objects one attack exchange creates.
3. Only after both: churn handling (deferred actor destruction, cleanup of actors created after the save).

## Next steps (Milestone 6)
1. **Broader state hash:** extend the hash to the whole character object span `obj[0..0x78C8]`, masked against known pointers, plus PlayerInfo records, the RNG state `0xE7C054`, GameInfo+0x10/+0x14/+0x70 and the clock fields. Use xxHash per region, with per-region hashes logged so a divergence names its region.
2. **RNG census:** log RNG call counts and caller return addresses per frame (hook `0x5F5080`/`0x5F50A0`). This separates gameplay from cosmetic consumers.
3. **Full-match record:** record P1/P2 masks per GameFrame for a full Versus match started from a fixed menu path. Replay it, then compare hashes and report the first divergent frame and region.
