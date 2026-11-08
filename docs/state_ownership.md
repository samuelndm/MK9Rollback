# MK9Rollback: combat-frame state ownership (Gate D input)

Target: `MKKE.exe` SHA-256 `D1C71E086DEAC26716E48F060E486679A36DDD4002AB5080D66DF9D11437D0E6`, base `0x400000`, no ASLR.
Boundary studied: **entry of EngineTick `0x0080F670`**. At that point the main loop has already run `GameFrame++` (`0xE89BE0`) and the hardware sampler. FightUpdatePlayers `0x8ACD90`, the MKScript VM `0x45CAF0` and the world tick `0x7F7F50` have not run yet.

**Method.** This pass is static only. The game install and the game process were not touched.
- **Disassembly:** full llvm-objdump of the analysis copy, plus a direct headless Ghidra 12.1.4 run (`-readOnly -noanalysis`) against a **private copy** of the partial `focused-seeds-1` project. This is **not REA**. REA was not started because a second 8 GB JVM did not fit next to the other agent's session.
- **Raw evidence** is in `analysis/state-ownership/`:
  - `dumps/b1..b7.txt`: decompiled C with callers.
  - `dumps/asm-avatar-alloc-clock.txt`: key assembly windows.
  - `mk-types.tsv`: the MK RTTI type table, 135 types with name, size and parent.
  - `alloc-sites.tsv`: every MK object allocation site with size, type and constructor.
  - `abs-gameinfo-window.tsv`: absolute writers in the GameInfo window.
  - Scripts: `so.mjs`, `mktypes.mjs`, `allocsites.mjs`, `abswrites.mjs`, `dis.ps1`, `run-sodump.ps1`, `ghidra/SoDump.java`.

**Confidence labels.** None of these findings is VERIFIED-RT.
- **HIGH:** decompiled or disassembled code reads or writes the field as described.
- **MED:** inferred from usage.
- **LOW:** hypothesis.

---

## 0. The big picture

1. **MK runtime type system.** Every MK gameplay object derives from `MKBaseObj`.
   - Type descriptors are 12-byte records `{char* name, u32 size, desc* parent}` in `.data`, for example `MKCharacterObj` at `0xDE8370`. The object's vt[0] returns its descriptor, and `0x42D5C0` is IsA.
   - The constructor `0x42E000` sets `obj+4 = id` from the **global id counter `0xDE820C`**. The counter wraps at 0x3FFFFFFF back to 0x10000042.
   - The constructor also links a 0x1C-byte list node `{vt, obj, id, next(+0xC), prev(+0x10), desc(+0x14), dead(+0x18)}` into the global list `0xDE8214`/`0xDE8218`.
   - The destructor `0x42D630` sets `obj+4 = -1`.
   - All references between MK objects are **handles `{ptr, id}`** that are validated as `ptr && ptr != 0xDEADBEEF && id != -1 && ptr->id == id`. A stale handle is therefore detected rather than crashing, provided the memory stays mapped.
2. **All MK objects live in a dedicated MK heap** (`0x42D5F0 alloc(size)` → `0x42D510` size class → `0x887520`). The heap is created per map load by `0x42DED0` → `0x8879B0`/`0x8873E0`/`0x8877E0`/`0x42DE20`.
   - **Master heap** `[0xED53F0]`: a 12 MB (`0xC00000`) reservation.
   - **Sub-heaps:**
     - `[0xED53F8]` 8 MB and `[0xED53FC]`, `[0xED5400]` 1 MB each. Their users are asset/file loaders (`0x81A440`/`0x81A490`/`0x81A4B0`). MED: these are immutable during a fight.
     - `[0xED53F4]` 1.5 MB (`0x180000`): **the gameplay object heap.**
   - **Fixed-size pools** are carved out of `[0xED53F4]`. Each pool is a 0x48-byte object with vtable `0xD9C66C`, a lock-free free-list head plus ABA tag at `+0x30/+0x34`, element size at `+0x3C` and total bytes at `+0x40`. When a pool is full, allocation falls back through `+0x2C` to the next larger pool and finally to the master heap `0xED53F0`.
   - **Pool counts for fight modes** (any mode ≠ 8, which includes Practice mode 2):

     | Pool | Element size | Count | Pool global |
     |---|---|---|---|
     | ≤0x40 | 0x40 | 280 | `0xED5404` |
     | ≤0x80 | 0x80 | 275 | `0xED5408` |
     | ≤0x100 | 0x100 | 230 | `0xED540C` |
     | ≤0x200 | 0x200 | 160 | `0xED5410` |
     | ≤0x800 | 0x800 | 6 | `0xED5414` |
     | ≤0x1000 | 0x1000 | 10 | `0xED5418` |
     | ≤0x2000 | 0x2000 | 1 | `0xED541C` |
     | ≤0x4000 | 0x4000 | 1 | `0xED5420` |
     | ≤0x8000 | 0x8000 | 2 | `0xED5424` |
     | ≤0x10000 | 0x10000 | 1 | `0xED5428` |
     | **MKFGCharacterObj** | **0xC580** (`[0xDEF4C0]`) | **8** | `0xED542C` |
     | **MKScriptVM** | 0xD8 | **50** | `0xED5430` |
     | MKScript | 0xC8 | 60 | `0xED5434` |
     | List nodes | 0x1C | 3000 | `[0xE394E4]`, free list `[0xE394E0]` |

     That is **≈ 0.9 MB of pools**. Other items in `0xED53F4` are MKScriptVM private stacks (0x200 or 0x1000 bytes) and the PLAYER_COL_INFO arrays.
   - **Consequence:** the entire MK-side simulation (fighters, script processes, collision objects, MK anim nodes, cameras, particles' MK halves, list nodes) sits in **two contiguous heap regions of about 2 MB total**. Their allocator metadata is a handful of small GMalloc objects.
   - This matches the other agent's run8 write-set: about 614 pages, or 2.4 MiB, written per frame.
3. **Every `MKObj` owns a UE3 actor.** The MKObj constructor `0x4396E0` calls `GWorld(0xE86AB0)->SpawnActor` (`0x7AE3A0`) and stores the result in `obj+8`. The destructor calls `GWorld->DestroyActor` (`0x79D960` at `0x439A70`). `MKSkeletalObj` (`0x4441B0`) also constructs a USkeletalMeshComponent (`0x613440`) into `+0x10C`. Creating or destroying an MK object between save and load is therefore **the central rollback hazard** (§5).
4. **The clock is GameFrame `GameInfo+0x70` (`0xE89BE0`).** Every gameplay timestamp found reads it (§4).

---

## 1. NetherRealm avatar save/restore (KOTH/spectator)

**Callers and timing** (HIGH, static):
- `SaveStates 0x82B2C0` (thiscall, this = SpectatorInfo) logs "[SPECTATOR_INFO] SaveStates()". It calls SaveAvatar for each non-null entry of `this+0x268[0..this+0x26C)`. If `this+0x254` is set it calls `0x81EB00` + `0x81BA10(0)` and then clears `+0x254`/`+0x214`.
  - Its only direct caller is **PreInitClean `0x82D580`**, which is called from **`UMK9GameEngine::LoadMap` `0x817D50`** (`0x817DF4`) and from **InitializeSpectatorMode `0x82E300`**.
- `RestoreAvatar 0x9703A0` is called from **ReloadSpectators `0x82D890`** (InitializeSpectatorMode) and from **AvatarLoadedCallback `0x971AC0`**. That callback is registered at `0x971E5B` and creates a fresh `MKSkeletalObj` (0x1D0, constructor `0x4441B0`).
- Exec thunks `0x8E9DE0` (SaveAvatar), `0x8E9E10` (RestoreAvatar) and `0x8EB200` (SaveStates) sit in a callback table at `0xDF59C0`/`0xDF59C4`/`0xDF5B4C`. They resolve their target object via `0x458930(0, …)`.
- **Nothing calls these per frame or in a fight.** They preserve lobby avatars across level loads.

**AvatarInfo record** (the `this` of SaveAvatar/RestoreAvatar):

| Offset | Size | Saved from (SaveAvatar `0x971780`) | Restored by (RestoreAvatar `0x9703A0`) | Confidence |
|---|---|---|---|---|
| +0xE4/+0xE8 | 8 | handle → `MKSkeletalObj` (type-checked against desc `0xDE829C`) | same | HIGH |
| +0x1C/+0x20 | 12 (FString data/num/max) | anim name: `0x4410E0(animSlot)` → seq object, `+0x24` FName → string (`0x411A90`) | `0x494CD0` looks the name up in the global anim list `0xE88B38`/`0xE88B3C` (only when num ≥ 2) | HIGH |
| +0x14/+0x18 | 8 | `[seq+0x38]`, `[seq+0x3C]` (name/ref pair of the current sequence) | used by the lookup | MED |
| +0x28 | f32 | **anim time** = `MKAnimNodeSequence+0x74` (`0x442370` → `0x440FF0`) | passed to `skel->vt+0x6C(anim, +0x28, +0x2C, +0x30)` = `0x446E20` → `vt+0x108(…,0,8)` (PlayAnim) | HIGH |
| +0x2C | f32 | **anim rate** = node`+0x84` (`0x441090`) | as above | HIGH |
| +0x30 | u32 | `2*bit5(node+0x38) + bit6(node+0x38)` (loop/play flags; `0x441300`/`0x441360`) | as above | HIGH (bits), MED (names) |
| +0x40 | 12 | actor Location `actor+0xDC` (or the physics pose via `0x7896F0` when `actor+0x60` is set) (`0x433410`) | `0x433580` → **`GWorld->MoveActor` (`0x7A6F00`) with delta** (collision-aware) | HIGH |
| +0x4C | 12 | actor Rotation `actor+0xE8` (`0x433490`) | `0x4338B0` → MoveActor with rotation | HIGH |
| +0x58 | 12 | `actor+0x11C..0x124` (DrawScale3D; `0x42F0F0`) | `0x42F120` → `0x790850` (setter plus `0x790230(1)` reattach) | HIGH |
| +0x38 | bool | mesh component `+0x648 != 0 && +0x650 == 1` (`0x42FA20`) | `0x436160` writes `+0x648`/`+0x650` | HIGH |
| +0x34 | bool | bit 1 of byte `actor+0x80` (`0x42F190`, bHidden-like) | `skel->vt+0x34(+0x34)` | MED |
| +0x3C, +0xC0 | 4 | cleared | — | HIGH |
| +0x80 | 4 | log channel | — | HIGH |
| +0x10C/+0x110 | FString | avatar name (log only) | — | HIGH |

**Coverage.** These routines cover one `MKSkeletalObj`'s **presentation**:
- anim name, time, rate and two flags
- transform and DrawScale3D
- two visibility flags

**What they do not cover:**
- script/VM state, moves, health, meter, input, hit state
- velocity (`MKObj+0x68..0x9C`)
- collision, child objects, any other MK object
- blend trees (only the top sequence)
- RNG and clocks

**Restore side effects.** Restore restarts the animation through PlayAnim, which fires anim notifies and resets blends. It moves the actor with collision-checked MoveActor, which can fail or encroach.

**Reuse verdict.** Do not use these routines as a serializer.
- **Worth reusing:**
  - **Field locators:** the getters `0x440FF0`/`0x441090`/`0x441300`/`0x441360` and `0x433410`/`0x433490`/`0x42F0F0`.
  - **Transform re-apply:** the engine paths `0x433580`/`0x4338B0`/`0x42F120` are the correct way to push a restored transform into UE3 so the octree and components update. They are a reasonable model for the UE3-side step in §5 tier 2.
- **Not worth reusing:** PlayAnim, for restoring animation. The MK anim node bytes are restored by the heap copy instead.

---

## 2. Per-fighter simulation state

### 2.1 Per-frame update order (HIGH, from EngineTick `0x80F670` asm)

1. **Pre-tick:** `0x4D6160` (= `0x42D950` on list `0xE3D7E0`), `0x49A0B0`, `0x42DE00` (deferred list-node cleanup).
2. **FightUpdatePlayers `0x8ACD90`:** the player order rotates by `GameFrame & 3`.
   - **Pass 1:** input-lock bookkeeping, `char+0x2FCC`; history ring `0x451350`; tapper counters `0x4515B0`; toasty (`0xE91BC4`…).
   - **Pass 2:** `PlayerInfo.vt+0xC` (input lock), `0x448A00` (`char+0x2FFC`), then MatchMoveList `0x8AB6F0` / basic attacks `0x8AAA70`, or else a script event (`0x44DCE0` with `char+0x2A0→+0x44`).
3. **`0x45CAF0`:** list-node cleanup on `0xE39FB0`, then for each of the 21 descriptors `0xE39E00..0xE39FA4` (stride 0x14) it runs the executor `0x45C700` on every live MKScriptVM.
4. **If not paused:**
   - **Update pass:** `for each MKObj in list 0xE0B504: vt+0x14` (Update).
   - `0x80F570`: MKAnimTrackMatcher list `0xE3B698`.
   - In submode 8 or 0xB: for each MKCharacterObj, `0x446EE0` (anim-rate sync).
5. **`0xB927E0`:** async storage/profile state machine at `0xFE2144..0xFE2158`. Not gameplay (MED).
6. **World tick `0x7F7F50` → `0x7AE980`** (UWorld::Tick: actor tick groups, PhysX, task scheduler). A scheduler task `0x811870` → **`0x80F850`**, registered at `0x818023` via `0x70E070(…, 3, 0, 0xE88F48)`, runs per-character callbacks `0x406C40`/`0x406C20` and **rebuilds hurtboxes `0x8AFEB0`** from bones. **This runs inside the task graph, possibly off-thread.**
7. **If not paused:** `for each MKObj in 0xE0B504: vt+0x18` (PostUpdate).

### 2.2 Objects a fighter owns

| Object | Address / offset | Size | Contents | Pointers? | Mutated by | Conf. | Evidence |
|---|---|---|---|---|---|---|---|
| MKFGCharacterObj | `[PlayerInfo+0x1C]`, id `+0x20`; pool `0xED542C` | **0xC580** (`0xDEF4C0`, RTTI `0xDF0B10`); MKCharacterObj part 0x766C, MKSkeletalObj 0x1D0, MKObj 0xF8 | entire fighter state | many handles, lists, actor and mesh | FightUpdatePlayers, VM natives, vt+0x14/+0x18, hurtbox task | HIGH | mk-types.tsv, alloc `0x8AEAF2`/`0x8D6F78`, ctor `0x8AD7D0`→`0x458040`→`0x445B30` |
| ↳ vtable | +0x0 = `0xD6B3E0` | — | vt+0x14 Update `0x448240` (→ MKSkeletalObj `0x4430E0` → MKObj `0x43E010`); vt+0x18 PostUpdate `0x448660` (→ `0x443320`; records location `+0x1E4` to the recorder, tag 0xFF0007); vt+8 timescale `0x8949E0` = `+0xF4 × [0xE91E24]` | code | — | HIGH | b3 |
| ↳ id | +0x4 | 4 | MK id (generation) | no | constructor/destructor | HIGH | `0x42E000` |
| ↳ actor | +0x8 | 4 | AActor* (UE3); Location +0xDC, Rotation +0xE8, DrawScale3D +0x11C, physics +0x60, flags +0x80 | **UE3 heap** | MoveActor `0x7A6F00` from MKObj update | HIGH | `0x4397AE`, `0x433580` |
| ↳ physics-driven flag | +0x14 | 4 | when non-zero, no kinematic integration | no | — | MED | `0x43E010` |
| ↳ child MKObj list | +0x30 (head +0x34, tail +0x38) | 12 | attached objects, PostUpdate'd via `0x433270` | MK heap | attach/detach | HIGH | `0x433270`; the determinism "dynamic ptr" at +0x34/+0x38 |
| ↳ ground offset | +0x48 | f32 | landing threshold | no | | MED | `0x43E010` |
| ↳ velocity/accel | +0x68..+0x80, +0x94..+0x9C | ~0x24 | integrated via vt+0x20(dt); zeroed on landing | no | `0x43E010` | MED | `0x43E010` |
| ↳ destroy-at / hide-at | +0xA8 flag, +0xAC GameFrame, +0xB0 GameFrame | 12 | deadlines against GameFrame → vt+4(1) delete / vt+0x34(1) | no | | HIGH | `0x43E010` |
| ↳ time scale | +0xF4 | f32 | × global `0xE91E24` (= GameInfo+0x82B4) | no | scripts | HIGH | `0x8949E0` |
| ↳ skeletal mesh component | +0x10C | 4 | USkeletalMeshComponent*: `+0x280` anim tree, `+0xD0` node index, `+0x648`/`+0x650` flags, `+0x124` bit 8, `+0x2A4` | **UE3** | `0x443320` | HIGH | `0x4441B0`, `0x443320` |
| ↳ anim event at frame | +0x110 (GameFrame), +0x114/+0x118 | 12 | fires `0x42FF70` when `+0x110 == GameFrame` | no | | HIGH | `0x4430E0` |
| ↳ bone-ground callbacks | list +0x11C (head +0x120) | — | MKSkelObjBoneGroundCallBackData (0x20) → spawn MKScriptVM (policy 0, key 0x49) | MK heap | | HIGH | `0x4430E0` |
| ↳ blend tree handle | +0x190 (ptr +0x194, id +0x198) | 12 | MKAnimNodeBlendTree (0x58) | MK heap | | HIGH | `0x443320` |
| ↳ **anim channel table** | +0x1B0 (TArray data), +0x1B4 count, stride 0x18 | n×0x18 | per UE3 anim node index: `{?, MK anim node*, …}`; `0x43B070(nodeIdx)` returns slot+8, where `+8` = **MKAnimNodeSequence** (time +0x74, rate +0x84, flags +0x38, seq +0x3C) | MK heap (nodes) + GMalloc (array) | anim natives, `0x442090` | HIGH (layout), MED (array home) | `0x43B070`, `0x440FF0`, `0x442090` |
| ↳ rotation-pending | +0x19C, +0x1A0 flag, +0x1A4 vec | | applied at the start of Update | no | | HIGH | `0x4430E0` |
| ↳ handles (type `0xC5B274` `{vt,ptr,id}`) | +0x1D8, +0x288, **+0x2A4 (→+0x2A8 MKScript program)**, **+0x2F8 (→+0x2FC/+0x300 main MKScriptVM)**, **+0x2F0C (→+0x2F10/+0x2F14 PLAYER_COL_INFO)**, **+0x2FA0 (→+0x2FA4/+0x2FA8 script context VM)** | 12 each | | MK heap | | HIGH | ctor `0x458040`, `0x44DCE0`, `0x8AFEB0`, `0x8A0140` |
| ↳ lists (type `0xC5AEB4` `{vt,head,tail,id,..}`) | +0x228, +0x2B0 (head +0x2B4), +0x2F18 (+0x2F1C/+0x2F20), +0x2F2C (+0x2F30 ButtonTapperContext), +0x2F40 | | | MK heap | | HIGH | ctor; `0x4515B0` |
| ↳ char data | +0x2A0 | 4 | pointer to static character data: `+0x38` move/collision table, `+0x44` override script id | asset (immutable) | — | MED | `0x8ACD90`, `0x8AFF90` |
| ↳ state flags | +0x1D0, +0x1D4 (`&0x800` → +0x2FFC), +0x2E8 (`&0x10` active, `&0x80` skip, `&0x40000000`), +0x2EC (`&0x1000` set by the move matcher, `&0x80000`) | 4 each | move/fight flags | no | many | HIGH (usage), LOW (names) | `0x8ACD90`, `0x89F180`, `0x448A00` |
| ↳ stamp/expiry | +0x2CC value, **+0x2D0 = (float)(GameFrame+10)** | 8 | timed state | no | `0x446FE0` | HIGH | §4 |
| ↳ counters | +0x2F54..+0x2F7C, **+0x2F5C flag / +0x2F64 GameFrame stamp** (`0x8959D0`), +0x2F74 state (3 = special), **+0x2F78 GameFrame stamp** (`GF−x < 10`), +0x2F70/+0x2F7C | | combo/move timing | no | `0x8AB300` region, scripts | MED | `0x89E64A`, `0x8959D0` |
| ↳ +0x2FCC | | 4 | input-lock latch | no | FightUpdatePlayers | HIGH | `0x8ACE5x` |
| ↳ +0x2FF0..+0x301C | | | +0x2FFC hold counter (`0x448A00`); +0x3000/+0x3004/+0x3008/+0x300C/+0x3014/+0x3018 have **no native writer except init `0x4567B0`** | no | **MKScript raw-offset stores** (see §4) | MED | grep: only `0x4567B0` writes them |
| ↳ command/move array | +0x30B4 .. +0x7524 | 60 × 0x124 | element ctor `0x44F910` | some | | HIGH (shape) | ctor `0x458040` |
| ↳ move-list | +0x7524 entries, +0x7AEC count, +0x7638 PlayerInfo* | | | | | HIGH (rea_pass1) | |
| ↳ life | **+0x78C0** f32 | 4 | | no | | VERIFIED-RT (ledger) | |
| ↳ FG part (beyond the 0x78C8 hash span) | +0x7724 list; +0x77D8 8×0x10 array; +0x785C..+0x7870; +0x78C8, +0x78DC lists; 3 × `0x44F9F0` objects; +0x7F48 list; **+0xA950 (→+0xA954) and +0xA968 (→+0xA96C) handles to MKSkeletalObj (props)**; +0xAA78 list; +0xAAB4 handle; **+0xAAC0: 50 × 0x88 records** (ctor `0x89B640`) up to +0xC550; +0xC554 handle; +0x7744 string; +0x7A24/+0x7A34 MKScript handles; +0x7B04 | 0x4CB8 | **not covered by the current 0x78C8 hash** | yes | | HIGH (layout) | ctor `0x8AD7D0`, `0x89B840`, `0x89F180` |
| Main fighter script process | `char+0x2FC/+0x300` → MKScriptVM; context VM `char+0x2FA4/+0x2FA8` | 0xD8 each, pool `0xED5430` | see §2.3 | MK heap + stack | VM `0x45C700` | HIGH | `0x44DCE0`, `0x8A0140` |
| PLAYER_COL_INFO (hurtboxes) | `char+0x2F10` | 0x194 (pool) + two arrays from heap `0xED53F4`: n×0x28 volumes at `+0x18`, 0xB48 at `+0x14` (36 × 0x28 records + count) | 9 inline 0x28 entries at +0x1C..; scale +0x18C=1.0 | MK heap | **rebuilt from bones each frame by the task `0x80F850`→`0x8AFEB0`→`0x8AFDD0`/`0x8AFF90`** | HIGH | `0x8B3D80`, `0x8AF750`, `0x8AFF90` |
| MK anim nodes | via `+0x1B0` table / `+0x194` | MKAnimNodeSequence 0x94, Blend/BlendTree 0x58, Rotation 0x5C, BlendBase 0x4C (pools) | anim time, rate, weights | MK heap | anim natives; time advance during the world-tick anim update (MED) | HIGH (types), MED (who advances) | alloc `0x4D31E6`/`0x4D32D6`/`0x4D33D6`, ctor `0x4CC870` (counter `0xE3CE34`) |
| UE3 pose | `skel+0x10C` component SpaceBases | — | **derived** from the MK nodes during the world tick, then consumed by the hurtbox rebuild | UE3 | world tick | MED | |
| Weapon/prop objects | FG +0xA954, +0xA96C | MKSkeletalObj | | MK heap + actor | | HIGH | `0x89B840` |
| PlayerInfo | `0xE89D7C + i*0xC8C` | 0xC8C | +0x48: 0x2C-byte event ring (count +0x570, write +0x574, read +0x93C); +0x578: 20 × 0x30 records (idx +0x938); +0x944 lock; +0xB40 pad; +0xBA4; +0xC14 meter | some handles | `0x451350` etc. | HIGH | b2 `0x451350` |
| Gamepads | `0xE221A8 + k*0x60C`, k < 18 | 0x60C | cur/prev, 3 rings with u8 indices | owner ptr | sampler / injection | VERIFIED-RT (ledger) | |

### 2.3 MKScriptVM (process) layout (type `0xDE861C`, 0xD8 bytes, vtable `0xC68798`, ctor `0x45CC80`, dtor `0x45CDB0`)

| Offset | Meaning | Pointers? | Conf. |
|---|---|---|---|
| +0x4 | MK id | | HIGH |
| +0x8 | instruction pointer (into the program, GMalloc/asset memory) | yes, program | HIGH |
| +0xC | stack/storage base (private buffer from heap `0xED53F4`, or 0) | MK heap | HIGH |
| +0x10/+0x14/+0x18 | frame/stack pointers (relative to +0xC; rebased into the scratch slice while running) | into +0xC | HIGH |
| +0x20 | handle (vt `0xC5B274`) → `+0x24/+0x28` MKScript program (`0xDE8610`) | MK heap | HIGH |
| +0x2C | state: 0 idle, 1 run, 2 waiting, 3 kill, 4 yield | | HIGH |
| +0x30 | current function/template (`+0x10` init data, `+0x14` size, `+0x24` relocation count, `+0x28` handle slots) | program | HIGH |
| +0x34..+0x3C | decoded instruction metadata | | HIGH |
| +0x40 | **wait timer (f32), decremented by +0x68 each tick** | | HIGH |
| +0x44 | process-list index (0x15 = default) | | MED |
| +0x48 | storage policy: 0 → no private stack (scratch only, stack discarded on exit); 1 → 0x200 private; 2 → 0x1000 private (`0xC6878C` table) | | HIGH |
| +0x4C | capacity | | HIGH |
| +0x50 | lookup key (`0x459DB0`) | | HIGH |
| +0x54, +0x88 | handle lists | MK heap | HIGH |
| +0x68 | tick rate (1.0) | | HIGH |
| +0x6C/+0x70 | run-even-if-paused / pause gates | | HIGH |
| +0x74 (→ +0x78/+0x7C) | handle → another VM (variable-binding owner) | MK heap | HIGH |
| +0x80 | **HostFrame (`GameInfo+0x6C`) stamp of the binding cache (`0x45BD30`)** | | HIGH |
| +0x84 | expire-at GameFrame | | HIGH |
| +0x9C | named-variable map (`0x45BBB0` insert; for example "current_ground_z" → `[0xE89B8C]+0x824`) | **likely GMalloc nodes** | MED |
| +0xA0 | binding array, 0x10 per entry | | MED |

**Globals.**
- Execution: `0xE39DF0` instr meta, `0xE39DF4` current VM, `0xE39DF8` current program, `0xE39DFC`, `0xE394E8` refresh flag.
- Nesting depth `0xDE8628` = −1 when idle.
- Scratch: `[0xE39FC4]` (0x8000 bytes, 4 slices of 0x2000). Only meaningful during execution, so the boundary must see depth = −1.
- Process lists: 21 descriptors `0xE39E00..0xE39FA4`, plus root `0xE39FB0`.

**Spawning.**
- `0x45D8C0` allocates a VM, then `0x45CB20` binds the function.
- `0x492590` is the general "spawn script proc for player".
- `0x45CC00` runs a function synchronously.
- Collision hits spawn "p_reschedule_col_reaction_to_top_of_frame" (`0x45D960`).
- 61 allocation sites create MKScriptVMs (alloc-sites.tsv). **Reactions, hitstun and blockstun durations are therefore largely VM wait timers (+0x40) plus char fields** (MED; no dedicated native hitstun field was found).

### 2.4 Projectiles / collision objects

- **Type.** `MKCollisionObj` (`0xDF0B1C`, 0x1DC bytes, vtable `0xD6A9B0`, ctors `0x8A45C0`/`0x89F050`, init `0x89F180`) is the attack/projectile volume. It is an MKObj, so it **spawns a UE3 actor** (class `0xE0B48C`). Visuals are separate `MKSkeletalObj`/`MKParticle` objects.
- **Fields** (HIGH, from `0x89F180`/`0x8A46E0`/`0x8A7300`):

  | Offset | Meaning |
  |---|---|
  | +0xF8/+0xFC/+0x100 | extents (default 10.0) |
  | +0x104 | done |
  | +0x108 | flags |
  | +0x10C | from char+0x460 |
  | +0x110 | — |
  | +0x118/+0x11C | handle |
  | +0x124 | from char+0x7B04 |
  | +0x12C | frame countdown, then done |
  | +0x170 | owner PlayerInfo* |
  | +0x174 | owner char* |
  | +0x188/+0x18C | target handle |
  | +0x190 | param |
  | +0x194 | shape (0 sphere, 1 capsule, 2 box, 3 bone) |
  | +0x1B8..+0x1C0 | — |
  | **+0x1D8** | **pointer to a 0x4C GMalloc object (vtable `0xC6A5B4`) whose position fields +0x34..+0x48 are updated every frame. This is outside the MK heap.** |

- **Tracking.** Projectiles are tracked by the global MKObj list `0xE0B504` and by script-owned handles. **There is no per-character projectile array.**
- **Hit test.** The hit test is `0x89E220` against the target's PLAYER_COL_INFO. On a hit it schedules a script reaction.

---

## 3. Round / match state

| Item | Address | Contents | Conf. | Evidence |
|---|---|---|---|---|
| GameInfo (static) | `0xE89B70` .. ≈`0xE91E50` (≈0x82E0) | The current hash covers only `[0, 0x20C)` + PlayerInfo. **The rest of the object is written by fight, stage and HUD code** (abs-gameinfo-window.tsv): e.g. GI+0x5434..+0x7FD8 written by `0x8F2xxx-0x8F6xxx` (stage/background), GI+0x8054 toasty, GI+0x8134..+0x8150 by `0x85xxxx-0x86xxxx` (HUD), GI+0x82B4 time scale (`0xE91E24`) | HIGH (writers), MED (extent) | ctor `0x815680` |
| fight flags | GI+0x10 `0xE89B80` | &1 fight, &0x400, &0x8000 practice; reset mask `& 0xB7FFFFCF` at round start | HIGH | `0x80FDB0` |
| fight-freeze flag | GI+0x14 `0xE89B84` &0x10 | toggled by `0x8FBCE0` (`0x492EA0()`), cleared at round start; while set, FightUpdatePlayers is skipped **but GameFrame still increments** | HIGH | `0x8FBCF8`, `0x80FE5D` |
| Stage/background object | `[GI+0x1C]` = `[0xE89B8C]` | 0x8E4 bytes, **GMalloc**, ctor `0x8FAEE0`; vt+4 = floor Z; +0x824 current_ground_z; +0x834..+0x850 toasty window/state; the random background event handler | MED | `0x815680`, `0x8ACD90`, `0x897B6E` |
| Round settings | GI+0x184.. (`0x80EB90` round time: default 90 or `[0xE90700]`; `0x80EC10` rounds to win) | static per match | HIGH | `0x87FA2C` |
| Round reset | `0x80FDB0` (called from `0x887A50`) | Resets players (`PlayerInfo vt+0x54`) and calls `0x80FCE0`. That routine writes `in_ECX[0x1C]=0`, i.e. **GameInfo+0x70 GameFrame = 0**, plus GI+0x208 = 0, GI+0x8110 = 0, GI+0x8114 = 1 and time scale GI+0x82B4 = 1.0. It also calls `0x44CBC0` ×4 (chars) and clears the fight flags. | HIGH | b7 |
| Live round timer / win counters / FIGHT!-KO flow | **not found in native code.** Strings ("rounds_played", "p_winner_winpose", "r_common_death_*", "initial_round_time") are stat keys or script names. The round state machine is **MKScript-driven**, so its state lives in VM stacks and processes in the MK heap and/or GameInfo | LOW | |
| Stage interactables | background handlers ("background_interaction_handler", "random_background_event_tick_handler" `0x8F9095`), `BackgroundInteractionPdata` (0x2C), stage object at GI+0x1C | MED | |

Note: `0x80FCE0` writes `in_ECX[0x1C] = 0` (GameInfo+0x70), so **GameFrame resets to 0 at each round start/reset**. This is consistent with the "gf=0 at Combat tick STOPPED" observation in the ledger. `0x80FCE0` is also called from the GameInfo constructor.

---

## 4. Clock

- **Every gameplay stamp found reads `GameInfo+0x70` (GameFrame)** (HIGH):
  - **Char:**
    - `char+0x2D0 = (float)(GF + 10)`, written by `0x446FE0` together with `+0x2CC`.
    - `0x8959D0`: `char+0x2F5C == 0 && char+0x2F64 <= GF`.
    - `0x89E64A`: `GF − char+0x2F78 < 10`.
    - `MKSkeletalObj+0x110 == GF`.
  - **Objects:** `MKObj+0xAC`/`+0xB0` deadlines.
  - **Script:** `MKScriptVM+0x84` deadline.
  - **Input:** pad ring entries.
- **Native writers.** No native writer of `+0x2F78/+0x3000/+0x3008/+0x300C/+0x3014/+0x3018` exists outside the zeroing init `0x4567B0`. There is no `lea`, offset table or indexed access to those offsets either. MED conclusion: they are written by **MKScript code through raw `char+offset` stores**. MKScript variables can hold char pointers (`0x4584D0` relocation type 6 = owner object), so the char layout is part of the script ABI, and **the whole 0xC580 object must be snapshotted.**
- **The "~2-tick drift"** (MED explanation): GameFrame is incremented by the main loop on **every unpaused host iteration**, including iterations in which FightUpdatePlayers early-outs. Two such cases are fight-freeze `GI+0x14 & 0x10` (`0x8FBCE0`) and the frames around a Practice reset (0x200).
  - The trial-relative frame index counts FightUpdatePlayers bodies, so GF-based stamps shift against it by the number of gated frames.
  - This is not a second clock. Verify at runtime by logging `GI+0x14` around the reset.
- **HostFrame `GameInfo+0x6C` is read by gameplay code** in one place: the VM binding cache (`0x45BD30`, VM+0x80).
  - **Why it matters for rollback:** resimulating K frames inside one host iteration leaves HostFrame unchanged, so the binding refresh is skipped.
  - **Mitigation:** either bump HostFrame per resimulated tick, or zero VM+0x80 after load.

---

## 5. Proposed snapshot design (minimal-first)

The boundary is EngineTick entry for frame N, with the host-loop GameFrame++ already applied. Save at that point, before any sim work. On load, write the saved bytes back, including GameFrame. Each resimulated frame must reproduce the host-loop side effects itself: `GF++`, HostFrame policy (§4), input injection, and then a call to the EngineTick body.

### Tier A: static memory, raw copy (~70 KB)

| Region | Range | Notes |
|---|---|---|
| GameInfo + PlayerInfo[4] | `0xE89B70 .. 0xE91E50` | Do **not** restore HostFrame +0x6C. Pointer fields (+0x1C, +0x180, +0x34D0, pads) are long-lived and unchanged, so restoring them is safe. The UI/online fields inside need a runtime diff. |
| Gamepads | `0xE221A8 .. +18*0x60C` | At minimum the two fighter pads and the AI pads 14/15. |
| RNG | `0xE7C054` | |
| MK object system globals | `0xDE820C` id counter; `0xDE8210..0xDE822B` (global node lists); `0xE393D8..0xE394E8` (pending-unlink array, count `0xE394DC`, node free list `0xE394E0`, node base `0xE394E4`, refresh `0xE394E8`); `0xE0B504` (MKObj tick list); `0xE3B698`; `0xE3D7E0`; `0xE39DF0..0xE39FC4` (script globals and the 21 list descriptors; **assert depth `0xDE8628 == -1` and `[0xE39DF4] == 0` at the boundary**); `0xE3CE34` | |
| Pool and heap objects (GMalloc) | 13 pools at `[0xED5404..0xED5434]` (0x48 each) and heaps `[0xED53F0]`, `[0xED53F4]` (0x3C each plus region descriptors, 0x28 each from `0x42A2F0`) | Free-list head and ABA tag at pool+0x30/+0x34. |

### Tier B: MK heap image (~1.5–2 MB, memcpy)

- **Regions:**
  - the backing region of heap `[0xED53F4]`: 1.5 MB, holding all pools, VM stacks, PLAYER_COL_INFO arrays and the 3000 list nodes
  - the used part of master `[0xED53F0]`: about 0.5 MB, for pool overflow
- **Discovery:** discover the region base and size at runtime from the heap objects' region descriptors. VirtualQuery can cross-check. The regions are recreated on every map load, so rediscover after each fight start.
- **What it restores, bit for bit:** both fighters (0xC580 each), all MKScriptVMs and programs' MK halves, collision objects, MK anim nodes, cameras, PLAYER_COL_INFO, and list membership, handles and ids.
- **Pointers into the MK heap stay valid** because the regions are restored in place, at the same addresses.

### Tier C: external state owned by live MK objects (walk list `0xE0B504` after the load)

| What | Where | How |
|---|---|---|
| Actor transform | `actor+0xDC` (loc), `+0xE8` (rot), `+0x11C` (DrawScale3D) | Save the values. Re-apply through `GWorld->MoveActor` (`0x7A6F00`, as in `0x433580`/`0x4338B0`) or the SetLocation path, so components and the octree update. |
| Actor flags | `+0x80` | |
| Actor physics | `+0x60` (non-null ⇒ physics-driven; initially forbid rollback while set on a gameplay object) | |
| Mesh-component flags | `skel+0x10C`: `+0x648`, `+0x650`, `+0x124` bit 8, `+0x2A4` | |
| MKCollisionObj trace shape | `obj+0x1D8` | 0x4C bytes in GMalloc. |
| MKScriptVM variable map | `+0x9C` | Verify whether the nodes live in GMalloc; if so, they can only change when a VM is created. |
| Stage object | `[0xE89B8C]` | 0x8E4 bytes, GMalloc. |

### Re-derived after a load (do not snapshot)

- **UE3 skeletal pose.** It is recomputed from the MK anim nodes during the world tick.
- **The hurtbox volumes.** They are already in the MK heap, so they are restored consistently, and they are rebuilt again at the end of the next world tick.
- **The VM scratch buffer.** It is empty at the boundary.
- **Particles and audio.** These are cosmetic and must instead be suppressed during resimulation.

### Forbidden in the first implementation (detect and refuse rollback)

1. **Any MK object create or destroy between save and load.**
   - **Detection:** `[0xDE820C]` changed, or any pool free-list head changed.
   - **Why created objects break:** an object created after the save keeps its spawned UE3 actor alive after the heap is restored. The actor leaks and stays visible.
   - **Why destroyed objects break:** an object destroyed after the save had its actor sent to **DestroyActor**. The restored MKObj then points at a dead actor, which can crash.
   - **Phase-2 fix:**
     - hook the MKObj destructor path (`0x439A70` DestroyActor call) to **defer actor destruction** for the rollback window, hiding the actor instead
     - after a load, destroy the actors of objects whose pool slot is free in the restored image
2. **A boundary with `0xDE8628 != -1`**, a non-empty pending-unlink array `0xE394DC`, or physics or scheduler work still outstanding (prior Codex physics notes).

---

## 6. Riskiest gaps (ranked)

1. **Object churn.** Hits spawn MKParticle, MKScriptVM and MKCollisionObj objects constantly, so the "forbid churn" rule will trip on almost every exchange. Deferred actor destruction plus cleanup of created actors (§5) is required before rollback is usable. An MKScriptVM has no actor; its only external links are the VM+0x9C map and program pointers, so its churn may be safe to restore by heap image alone. **Next step:** verify the 0x45BA50 map allocator.
2. **GMalloc-held mutable state referenced from MK objects** (MKCollisionObj+0x1D8, stage object, anim channel TArray `+0x1B0`, VM maps, MKScript TArrays). Every such allocation needs inventory. **Next step:** the runtime write-set diff (run8 style), restricted to pages outside the MK heap regions.
3. **The UE3 side of animation.** If any gameplay read uses UE3 SpaceBases between the boundary and the world tick (for example bone-attached MKCollisionObj, shape 3, or root motion), a stale pose after a load diverges. Hurtboxes are rebuilt from bones in the **world-tick task `0x80F850`**, which is scheduled through the task graph `0x70E070` (group `0xE88F48`). It may run off the main thread and must be joined before the boundary.
4. **Script-owned state outside the VM objects.** Possible mutable MKScript globals in program data (8 MB / GMalloc heaps), and the HostFrame-stamped binding cache.
5. **Round flow is script-driven and its storage is unidentified**, so the live timer and round counters are unknown. **Next step:** runtime diff of the MK heap and GameInfo across a round transition and a timer tick.
6. **GameInfo tail semantics:** about 33 KB containing HUD, stage, toasty and online fields. Restoring UI and online fields could break the session. Mask them via a runtime classification.
7. **The PhysX scenes** (prior Codex: 80 dynamic links, 119 shapes) are excluded. This is acceptable only if no gameplay object is physics-driven (`actor+0x60`) inside a rollback window.

## 7. Next static/runtime checks (bounded)

- **Runtime heap discovery:** read `[0xED53F4]` and its region descriptors to get the region base/size, and verify the 13 pool backing ranges fall inside. Then the experiment:
  1. Save the Tier A + B images.
  2. Run 8 frames with no churn (idle walking).
  3. Load the images.
  4. Compare the run6 hashes and the 8 floats.
- **Hook `0x42E000` (MK object constructed) and `0x42D630` (destructed)** with type names via vt[0], to measure churn per frame in real exchanges.
- **Static:** decompile `0x45BA50` (VM map allocator), `0x429EE0`/`0x42A2F0` (region descriptor layout), `0x4441B0`'s `0x613440` component flags, and the hurtbox builder `0x8AF910` (which bone source it reads).
