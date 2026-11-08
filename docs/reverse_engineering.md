# MK9Rollback: reverse-engineering ledger

Target build: `MKKE.exe`, SHA-256 `D1C71E086DEAC26716E48F060E486679A36DDD4002AB5080D66DF9D11437D0E6`, Steam build 344915, PE32 x86, image base `0x00400000`. There is **no ASLR** (no DYNAMIC_BASE), so VA = RVA + 0x400000. See [build_identity.md](build_identity.md).

**Confidence key**
- **VERIFIED-RT:** confirmed by a runtime experiment with our DLL in the live game; the evidence path is given.
- **HIGH:** strong static evidence (REA/Ghidra decompilation plus byte checks) but no runtime proof.
- **MED / LOW:** partial or circumstantial.

Static sources:
- [rea_pass1.md](rea_pass1.md): REA 6.4.0 with the Ghidra 12.1.4 provider.
- [mk9hook_findings.md](mk9hook_findings.md): all 110 MK9Hook addresses verified against this build.
- The prior Codex workspace: `../analysis/*.md`.

Runtime evidence lives in `../results/runN-*/`. Each run folder holds our DLL's `MK9Rollback.log` and its per-frame `MK9Rollback_frames.log`.

## Summary table

| VA (RVA) | Proposed name | Conv. | Confidence | Evidence |
|---|---|---|---|---|
| `0x00404D30` (0x4D30) | MainLoopIteration | thiscall | HIGH | Calls the sampler at `0x404E26`, then `0x4025A0` at `0x404E35`. Increments GameInfo+0x6C every iteration and +0x70 only while unpaused (`0x404DFC..0x404E12`). |
| `0x004025A0` (0x25A0) | TimedEngineFrame | thiscall | HIGH | QPC pacing, then engine vtable +0xBC/+0xC0 with the same float dt. dt bits observed as `0x3C888889` (1/60) on every frame of every run. |
| `0x0080F670` (0x40F670) | **EngineTick** (engine vt+0xC0, slot VA `0xD60DF0`) | thiscall(engine, float dt), RET 4 | **VERIFIED-RT** (hooked through the vtable) | Called once per main-loop iteration in menus, attract and fights. It contains the only call to FightUpdatePlayers. run1..run4. |
| `0x008ACD90` (0x4ACD90) | **FightUpdatePlayers** (combat input→move tick) | cdecl void(void) | **VERIFIED-RT** | Only caller is `0x0080F70B`. It is called every unpaused tick, including menus, but the body early-outs unless all three gates below hold. Observed: menus `mode=1 f10=0` → gated off; Practice `mode=2 sub=8 f10=0x8001` → runs. The body never runs while paused (run2: 240 ticks, zero calls). |
| `0x00887AF0` | IsFightMode | cdecl | HIGH (decoded) | `GameInfo+0x8` mode ∈ {0-3,5-7,10-14} → 1 (jump table at `0x887B18/0x887B20`). |
| `0x00406850` / `0x004066D0` | GameInfo::GetMode / GetSubMode | thiscall | HIGH (decoded) | Return `[GameInfo]+0x8` / `+0x4`. |
| `0x00406790` | IsPaused | cdecl | **VERIFIED-RT** | `mov eax,[0xE43054]; ret`. |
| `0x00407AB0` | SetPaused(int) | cdecl | **VERIFIED-RT** | Writes `0xE43054` (and calls `0x426A70` when setting). Calling it with 1/0 froze/resumed the combat tick and the game frame (run2), and drove single-frame stepping (run3). This is the same setter MK9's delay netcode uses to stall (`0x00885F80`). |
| `0x00E89B70` | FG_GAME_INFO singleton (pointer at `0x00DEF4B4`) | data | **VERIFIED-RT** | `*0xDEF4B4 == 0xE89B70` was read live. |
| `0x00E89BDC` (GameInfo+0x6C) | HostFrame | data | **VERIFIED-RT** | +1 per iteration, including while paused (run2: +241 during a 240-tick pause). |
| `0x00E89BE0` (GameInfo+0x70) | **GameFrame** | data | **VERIFIED-RT** | +1 per unpaused iteration, exactly +1 per FightUpdatePlayers body (no `GF_JUMP` flags in runs 2–4). It freezes while paused and resets on mode/round transitions (observed `gf=0` at "Combat tick STOPPED"). |
| `0x00E43054` | PauseFlag | data (int) | **VERIFIED-RT** | See SetPaused. |
| `0x0080E540` | GetInfo(idx) → PlayerInfo | thiscall, RET 4 | HIGH (MK9Hook + REA) | Returns `0xE89D7C + idx*0xC8C`; idx ≥ 4 returns record 0. |
| `0x00895B70` | PlayerInfo::GetObj | thiscall, RET 4 | **VERIFIED-RT** (logic re-implemented) | `obj=[info+0x1C]`, `id=[info+0x20]`. Rejects null, 0xDEADBEEF and id −1, and requires `[obj+4]==id`. Our reader produced the expected fighters. |
| `0x00424A50` (0x24A50) | HardwareInputSampler | cdecl | HIGH (+ prior Frida RT) | Six hardware pads at `0xE221A8`, stride 0x60C. Flag 0x8 → `prev=cur` only. Flag 0x1 → `prev=cur; cur=0;` read the device, OR in the mapped bits, and push press/release ring events (`0x425770`/`0x4256A0`). |
| `0x0087F440` (0x47F440) | PadEdges(pad) | cdecl | **VERIFIED-RT** | Compares cur with prev and pushes ring events. We call it after writing cur; the ring write indices advanced exactly as expected (run2: press ring +1 per button, direction ring +1 per direction, release ring +1 after each release). |
| `0x00881360` | OnlineApplyRemoteInput | thiscall | HIGH (decoded) | Per remote pad: `prev(12B)=cur(12B)`, `cur[0]=ring.buttons(u16) & ~0x10000`; `0x008863A1..0x88644D` then calls PadEdges for every pad. This is the model for our override. |
| `0x005F5080` / `0x005F50A0` / `0x005ED570` | RNG int / float / seed set | — | HIGH (static) | State `0x00E7C054`, LCG `x = x*0x0BB38435 + 0x3619636B` (31-bit). EngineTick records it via `0x80F6C0`. Not yet runtime-tested. |
| `0x00B93540` | OnlineDesyncChecksum | — | HIGH (static) | Cumulative CRC-32 over a 10-entry fencepost ring at `0xFE2168`. Only 7 registrations exist: arena/character name CRCs and **P1/P2 team life once per online frame**. Mismatch → disconnect (`0x881C80`). **Not a state map**; too thin for rollback desync detection. |
| `0x00903700` | Practice recorder BeginFrame (playback) | virtual | HIGH (static) | Native input macro: 48 B/frame of P1/P2 pad cur + flags, 8 slots of about 2,999 frames. Does not store RNG or frame counters. Not a deterministic replay. |

## Combat Tick

**Main loop iteration:**
- **`0x404D30`:**
  - `HostFrame++` (`0xE89BDC`).
  - If `!IsPaused()`: `GameFrame++` (`0xE89BE0`).
  - `HardwareInputSampler()` (`0x424A50`).
  - `TimedEngineFrame()` (`0x4025A0`).
- **Inside `0x4025A0`:**
  - Engine vt+0xBC (`0x80DD40`, dt).
  - Engine vt+0xC0 = **EngineTick** (`0x80F670`, dt). Its order:
    1. Recorder BeginFrame; record HostFrame, GameFrame and RNG.
    2. Pre-tick subsystems.
    3. If `!paused`: `call 0x8ACD90` (**FightUpdatePlayers**), where the body runs only under the gates below.
    4. `0x45CAF0` (MKScript VM, 21 process lists).
    5. `0xB927E0`.
    6. Base world tick `0x7F7F50(dt)` unless `[0xE3B85C]`.
- **Gates at the top of FightUpdatePlayers** (`0x8ACD96..0x8ACDD6`, decoded):
  1. `IsFightMode()`.
  2. `(flags10 & 1) || (subMode == 0xB && !(flags10 & 0x400))`, where `flags10 = GameInfo+0x10` (`0xE89B80`).
  3. `!(GameInfo+0x14 & 0x10)` (`0xE89B84`).

**Runtime-verified (Gate A):**
- **Counter:** `gSimulationFrame` counts FightUpdatePlayers bodies.
- **Menus and title:** these produce only `SKIP` records; examples are `mode=1 sub=0/1/2/8` and `mode=2 sub=0/1/7`.
- **Practice:** `mode=2 sub=8 f10=0x8001` runs every frame.
- **Attract mode:** attract-mode AI fights (title screen) also run the fight body, with AI pads at indices 14/15 and flag 0xC. This must be excluded by mode when recording.
- **dt:** constant `0x3C888889`. The game is frame-locked: there is no fixed-step accumulator, and one iteration is one simulation frame.
- **Pause:** `SetPaused(1)` stops the fight body and freezes GameFrame while HostFrame continues. `SetPaused(0)` resumes at exactly GameFrame+1 (run2).
- **Frame advance:** applying `SetPaused(0)` at the **end** of EngineTick and `SetPaused(1)` at the end of the tick that ran N fight bodies advances exactly N frames. GameFrame goes +1 per step and P1 moves about 2.9 units per stepped frame while holding Right (run3).
- **Boundary caveat:** pausing from the **start** of EngineTick wasted one GameFrame value, because the main loop had already incremented it (run3: 40672 → 40674). The pause and step controls therefore apply at the end-of-tick boundary. Verified in run5: GameFrame stays strictly +1 per fight frame across pause, 3× step 1 and resume (59692 → 59697).

## Input

**Representation:**
- **Pad record:** `pad = [PlayerInfo + 0xB40]`. The current 12-byte record is at `pad+0x0C`, and `cur[0]` is the processed button/direction mask. The previous record is at `pad+0x18`.
- **Rings:** three 30×16-byte rings at `+0x58/+0x238/+0x418` (press / direction / release), with u8 write indices at `+0x54/55/56`. They serve as the engine's command buffer.
- **Pad flags** (`pad+0x08`):
  - `0x1` hardware-sampled.
  - `0x4` unconfirmed (AI pads carry `0xC`).
  - `0x8` externally driven.
  - `0x100` seen on P1's pad.
- **Hardware pads:** `0xE221A8 + k*0x60C`, k = 0..5.
- **Practice:** P1 used pad `0xE221A8` (i=0, flags `0x105`); P2 used AI pad `0xE27C5C` (i=15, flags `0xC`).

**Injection (Gate B), VERIFIED-RT** (`Input::OverridePlayerInput`; run2, run3, run4):
- **Method:**
  1. At EngineTick entry (after the sampler, before the fight update), set `flags |= 8`.
  2. Set `prev = cur`.
  3. Set `cur[0] = mask & ~0x10000`.
  4. Call `PadEdges(pad)`.
- **Release:** restore the 0x8 bit and zero `cur[0]`.
- This mirrors the engine's own online path (`0x881360` + `0x87F440`).

**Mask bits observed by injection** (P1 = Scorpion, Practice, run2):

| Bit | Observed effect | Proposed meaning | Confidence |
|---|---|---|---|
| `0x1` | jump, z peaks at 217.7 | Up | VERIFIED-RT |
| `0x2` | no translation | Down (crouch) | MED (needs visual confirmation) |
| `0x4` | y −125 → −377 over 60 frames | Left (screen-absolute) | VERIFIED-RT |
| `0x8` | moved right, pushed P2 | Right | VERIFIED-RT |
| `0x40`, `0x10`, `0x20`, `0x80` | each hit P2: life 1 → 0.97 → 0.92 → 0.87 → 0.82 | the four attack buttons (FP/BP/FK/BK order unconfirmed) | VERIFIED-RT that they attack |
| `0x8000` | no movement, no damage | Block (REA, MED) | MED |
| `0x2000` | small forward lurch, then back | throw attempt? | LOW |
| `0x4000` | small z bump | stance flip? | LOW |
| `0x1000`, `0x100`, `0x400`, `0x800` | nothing visible | unknown | — |
| `0x200` | Practice reset: positions back to ±125, life back to 1.0 | Practice "reset" (Select/Back) | VERIFIED-RT |
| `0x10000` | — | modifier; the engine clears it on remote input | static |

**Device/keyboard:** `SendInput` keys (WASD, Esc and others) reached neither the P1 mask nor the pause menu in this session (keyprobe JSONs in run1). The prior Codex run moved P1 only through the in-process keyboard producer `0x421130`. OS-level synthetic input is therefore not a usable test path; in-process override is.

## Fighter Structure
**Object size:** `MKFGCharacterObj` is **0xC580 bytes**. It is allocated at `0x8AEAED`/`0x8D6F73`/`0x8D7003` via `0x42D5F0(0xC580)`; constructors are `0x8AD7D0`/`0x8ADA30`.

**Layout (MED):**
- inline array 0x3C × 0x124 at +0x30B4
- move list at +0x7524
- PlayerInfo pointer at +0x7638
- inline array 0x32 × 0x88 at +0xAAC0
- 20+ heap TArray containers (deep copy needed)

(desync_rng_static.md)


| Field | Offset | Confidence |
|---|---|---|
| object | `[PlayerInfo+0x1C]`, generation `[PlayerInfo+0x20] == [obj+4]` | VERIFIED-RT |
| life (float, 1.0 = full) | `obj+0x78C0` | VERIFIED-RT (drops on hits, resets with 0x200) |
| actor | `[obj+0x08]` | VERIFIED-RT |
| location (float3; Y = horizontal, Z = height) | `actor+0xDC` | VERIFIED-RT (start ±125, jump z 217) |
| time scale | `obj+0xF4` (× global `0xE91E24`) | MK9Hook, HIGH |
| script context | `obj+0x2FA4` | MK9Hook |
| input lock | `PlayerInfo+0x944` | REA HIGH (always 0 in the Practice runs) |
| meter | `PlayerInfo+0xC14` | MK9Hook + prior static |

## Match Structure

FG_GAME_INFO (`0xE89B70`):

| Offset | Field |
|---|---|
| +0x4 | sub-mode |
| +0x8 | mode |
| +0x10 | fight flags (`&1` fight active, `&0x8000` set in Practice) |
| +0x14 | flags (`&0x10` suppresses the fight update) |
| +0x6C | HostFrame |
| +0x70 | GameFrame |
| +0x20C | PlayerInfo[4], stride 0xC8C |

Observed `(mode, sub)` pairs:
- Title/attract: (1,0), (1,1), (1,2), (1,8). Attract fights run the body.
- Menus toward Practice: (2,0), (2,1), (2,7).
- Practice fight: (2,8).
- Mode enumeration names are still unknown.

## RNG
Full static census: [desync_rng_static.md](desync_rng_static.md).

**Stream A `0xE7C054` (synced gameplay RNG; UE3 `appSRand` constants):**
- Update `x = x*0x0BB38435 + 0x3619636B`, full 32-bit state.
- `0x5F5080` returns `x & 0x7FFFFFFF`; `0x5F50A0` returns a float in [0,1).
- EngineTick draws 1 per unpaused frame (`0x80F6C0`).
- Gameplay users go through RandomInt `0x492050` (93 sites: 59 AI, 20 script/fatality/gore, menus, the percentage roll `0x448920`) and RandomChance `0x491A40`.
- Seeded from QPC at boot, and from the session seed at online match start. There is no offline per-match reseed.
- **Runtime-verified:** with an equal seed, per-frame state and draw counts are identical across trials (determinism.md, Result 2).

**Stream B `0xE5352C` (UE3 `GSRandSeed`, unsynced):**
- 85 inlined sites plus helper `0x4E3B50`; mostly FX, UI and online code.
- Script native `0x493C90` refuses B for process types 2/3/4, but natives `0x887F50`, `0x8EDF20` and `0x873BC0` draw from it unguarded.
- **Must be snapshotted and seeded identically on both peers.** Now included in StateHash and Snapshot.

**Not gameplay:** FMOD `rand()`; third-party per-object LCGs; network/crypto randomness.

**Earlier notes, static only:**
- **Generators:** `0x5F5080` (int) and `0x5F50A0` (float).
- **State and seed:** state `0xE7C054`, seed setter `0x5ED570`.
- **Recording:** EngineTick records the state each frame.
- **Not yet done:** runtime call-site census (gameplay vs cosmetic) and save/restore.

## Projectiles
Not yet investigated.

## State Snapshot
**Per-frame write set (run8, 2026-10-14):**
- **Method:** pause; hash every writable committed 4 KiB page externally (`tools/memscan`: 181,291 pages, 708 MiB, about 1.3 s per scan); `step 1`; scan again. Noise is the union of page changes across all paused scan pairs.
- **Result:** with P1 holding Right, 4 trials found **614 pages (2.4 MiB)** that change on every one-frame step and never in the noise (3,525 noise pages, mostly render/audio):
  - 3 image pages: `.data` around `0xE39000` (script VM records), `0xE7C000` (RNG) and `0xAE8E000` (a DLL).
  - 611 private heap pages.
  - Large contiguous blocks: `0x2C44B000` (51 pages), `0x42C82000` (80), `0x48F24000` (92), `0x4A152000` (46). Likely pose/skinning or PhysX buffers; attribution pending.
- **Attribution by pointer distance** (`memscan ptrwalk`: BFS over pointer-like dwords from char0, char1, PlayerInfo[0..3] and GameInfo, reading 0x400 bytes per target):

| Max hops | Write-set pages reached | Objects visited | Notes |
|---|---|---|---|
| 1 | 15 | 235 | 3 pages are the char objects themselves |
| 2 | 59 | 3,259 | char0 31, char1 25, gameinfo 2, pinfo 1 |
| 3 | 223 | 36,339 | |
| 4 | 353 | 178,964 | the walk now covers most of the heap; no longer discriminative |

  About 60 written pages are tightly owned by the fighter graph (≤2 hops). The large contiguous blocks are not, which fits engine/render/physics buffers.
- **Interpretation:** this is an upper bound on the bytes one combat frame writes, including render-side derived data. A raw page copy would cost about 0.25 ms per frame, so performance is not the constraint for snapshotting; ownership and safety are (heap metadata, pointers to objects allocated or freed between save and load).

**MK object system globals (all restored in Tier A since 2026-10-14):**

| Address | What | Evidence |
|---|---|---|
| `0xDE820C` | object id counter (wraps to `0x10000042`) | base ctor `0x42E000` |
| `0xDE8210`, `0xDE8224` | global node lists `{?, head +4, tail +8, ? +0xC, pending +0x10}`; nodes `{vt 0xC66B14, obj +4, id +8, next +0xC, prev +0x10, list +0x14, dead +0x18}` | ctor `0x42E000`, sweep `0x42D950`, per-frame `0x42DE00` |
| `0xE393D8[64]`, `0xE394DC` | lists pending a sweep, and their count | `0x42DBE0`, `0x42DB1B` |
| `0xE394E0`, `0xE394E4` | node free list, node pool base (pool allocated from the object heap at level load `0x42DE20`) | `0x42DD13`, `0x42DE77` |
| `0xE394EC`, `0xE394F0`, `0xE394F4` | flags; another node list (0x14) | `0x436CEB`, `0x438A42` |
| `0xE39E00 + 0x14*i`, i < 21 | **tick-group node lists**; object group index at MK object `+0x44` (move: remove `0x42DDC0`, add `0x42DFE0` in `0x4583B0`) | `0x459D80` iterates them |
| `0xE39FA4`, `0xE39FB0` | object refs `{vt 0xC5B274 / 0xC5AEB4, obj, id}` | change during fights |
| `0xE39DF4` | heavily used global pointer (4,897 refs), constant during a fight | |
| `0xE0AC58` | GMalloc small-block allocator (static): bins `+0x498` (lock-free heads `{ptr, tag}`, stride 0x10), bin count `+0x7CC`; pages are separate VirtualAlloc regions, not in the MK heap | `0x42C9D9` → pop `0x714560` |
| `[0xE7C058]`, `[0xE7C05C]` | GNames data/count; entry `+0x10` = ANSI name | `0x5F51D0` |
| UObject | `+0x4` index, `+0x24` Outer, `+0x28` Name (FName), `+0x30` Class | `0x411A90`, runtime `uobj` |
**Earlier notes:**
- **Not implemented.** The only runtime state captured is 8 floats: life and xyz for both fighters.
- **Prior Codex mapping:** PhysX scenes, MKScript scheduler state (21 lists, 0x8000 scratch), and clock object fields (`+0x280..+0x28C`, `+0x394/+0x398`). See `../analysis/physics-state-ownership.md` and `../analysis/script-state-ownership.md`.
- **Lead:** the engine's own OnlineDesync checksum (`0xB93540`, fencepost array `0xFE2168`) is the best lead for which regions NetherRealm considered gameplay state.

## Audio/Effects
Not yet investigated. Note: `SetPaused(1)` calls `0x426A70` (possibly audio pause), which matters if pause is ever used for rollback stalls.

## Networking
Static catalogue only (build_identity.md):
- **Transport:** custom UDP `mwNet` over ws2_32 (by ordinal).
- **Steam:** `SteamNetworking` / `SteamMatchmaking`.
- **Native delay netcode pieces:**
  - `0x885F80` orchestrator: stalls through SetPaused.
  - `0x881360` remote-input apply.
  - `0x884770` start; resets GameFrame.
  - `0x881190` spectator frame data.
  - Replay recorder `0x903700`: 12-byte pad current + 4 flags per frame.

## Unknowns
- Whether all gameplay state is restorable (Gate D). The 8-float comparison is far from a complete hash.
- Bit names for 0x10/0x20/0x40/0x80 (FP/BP/FK/BK), 0x1000, 0x2000, 0x4000, 0x100, 0x400, 0x800 and pad flags 0x4/0x100. These need on-screen confirmation (the input-history display in Practice).
- Where hit detection, animation and physics run: under `0x7F7F50`/`0x7F4CE0` or inside the script VM.
- Mode enumeration names; how a Versus/Ladder match differs from Practice (P2 human pad, round flow).
- Screen capture of the game failed with a GDI+ error once the fight was running (fullscreen D3D9Ex). Visual confirmation needs a D3D9 Present/EndScene capture hook or an overlay.

## MK9 online fight loop (static analysis, 2026-10-25; not runtime-verified)

- **Per engine tick:** the connection tick `0x00886880` (this = connection) calls the gamepad exchange `0x00885F80` on its MKOnlineGamepadData at connection `+0x4C3C` (call `0x008868EC`). Prologue `55 8B EC 51 56 8B F1`; thiscall with 1 argument (the connection), returns int.
- **Gamepad data fields:**
  - `+0x18` active (the exchange returns at once when 0);
  - `+0x20` / `+0x24` frame indices, checked against `[0x00DF0568]`;
  - `+0x224` / `+0x228`: local pad list and count;
  - `+0x230` / `+0x234`: remote pad list and count (0x60C pad records, as for hardware pads);
  - `+0x13C..+0x148`: outgoing ring of 11-byte records;
  - `+0x18C/+0x190/+0x194` per remote pad: incoming ring of 11-byte records, 0x14 apart.
- **Lockstep with a pause:**
  - `0x00881540(n)` asks whether n remote records are queued.
  - If they aren't, the exchange pauses the whole game through `0x00407AB0` ("JTLQUEUE PAUSING LF:%u") and remembers `[0xDEF4B4]+0x6C` in `0x00DF0664`. It unpauses ("JTLQUEUE UNPAUSING paused frames") when they arrive.
  - When the remote records are there, it:
    - sends the local pads with the desync hash (`0x00885710`, hash `0x00B93540`);
    - writes the remote pads (`0x00881360`: prev = cur, cur = buttons & ~0x10000);
    - runs PadEdges `0x0087F440` on every local and remote pad;
    - then runs `0x00881A70`, the desync check `0x00881C80` (nonzero = disconnect path) and `0x00881E10`.
- **Adaptive input delay:**
  - The send path `0x00885710` measures the remote queue depth: "JTLQUEUE Queue Depth Local/Remote/Average/NumZeroes".
  - It marks outgoing records with flag `0x4` ("Decreasing Frame delay", around `0x00885B5A`) or `0x2` ("Doubling input frame", around `0x00885BAB`).
  - `+0x20` is the current delay, bounded by `[0x00DF0568]`.
  - `0x00881E10` consumes these flags at both rings and steps `+0x20`.
  - These flags are delay changes, not round-end sync points.
- **Per-frame desync fenceposts:** `0x00881A70` records both fighters' life (`0x00894290`) and position (`0x00433460`) only while the exchange runs.
- **Connection timeouts:**
  - `0x0090FCB0` (CheckTimeout) and an inlined copy at `0x0091C29B` compare the socket's "ms since the last packet" `0x004F5E10` (prologue `53 56 8B F1 33 DB`) with the connection's `+0x1844` m_Timeout.
  - DisableTimeouts is `0x0090FBA0`.
- **Transport:** Steam P2P (`SteamNetworking()` IAT `0x00C558C8`). The socket class around `0x00948560` keeps the remote CSteamID at `+0x20/+0x24`, and SendP2PPacket goes through vtable slot 0 with send type 2, channel 0.
- **Match description (GameContext):** at online engine `+0x73C`, dumped by `0x0081B8A0`.
  - `+0x08` GameType (0 RANKED, 1 PLAYER; names `0x0081A5C0`);
  - MatchType (0 DM, 1 KOTH; `0x0081A630`);
  - `+0x14` MatchMode (1V1, TAG, TAG_2P; `0x0081A660`).
  - The current match type setter is `0x0093C820` (field `+0x308`).
- **Start, lifetime and frame numbering (static, 2026-10-26):**
  - **StartSendingGamepadData** `0x00884770` (thiscall gamepad data, arg the connection) logs `StartSendingGamepadData (RandSeed:0x%08X)`. It:
    - resets the rings and counters (`0x00882AE0`);
    - seeds the RNG from the connection's shared seed `+0x4C2C` (`0x005ED570`);
    - resets both fighters' pads (`0x0087FA70`);
    - sets **GameFrame (`GameInfo+0x70`) = 0**;
    - re-initialises the desync fenceposts (`0x00B935E0`);
    - sets `+0x18 = 1`.
  - It is reached from the fight scripts (native **1172**, `0x008BFC30` → `0x0081C080` → online engine `+0x4C3C`) and from a network message handler (`0x00885400`, message types 1 and 4).
  - `+0x18` is cleared only by the constructor (`0x008824A0`, from the connection's constructor at `0x00887232`) and the destructor (`0x008845D0` / deleting `0x00884660`). The gamepad data therefore stays active between fights until the connection is destroyed.
  - **Records carry no GameFrame.** The send path reads `GameInfo+0x70` only as a log argument, and pacing uses HostFrame (`+0x6C`, at `+0x12C` and `+0x1E4` against `[0xDF0570]` / `[0xDF056C]`). Our netplay renumbering of GameFrame (100000) cannot confuse the exchange, and a stopped native exchange resumes cleanly after the next StartSendingGamepadData.
  - `+0x1C` is the pre-fill counter (it sends `+0x20` delay frames first, `"%u"` logs at `0x00D69318`), and `+0x28` the started flag (`0x00881540` until enough remote records arrive).
- **Online rollback (src/net/OnlineMode.cpp)** uses these:
  - it skips `0x00885F80` on both games while a GGPO session runs;
  - it zeroes `0x004F5E10` during the session and for 15 s after it;
  - it takes the local fighter from the local pad list.
  - **Fixed 2026-10-26 (22:52 build):** a session ends at every timeline restart, and the next StartSendingGamepadData zeroes GameFrame, so that is at least every fight. `Online::Tick` took that stop for a failure and left every later fight of the lobby (rematches, King of the Hill) to MK9's netcode.
    - `Online::OnSessionStop(why)` now re-arms the Steam netplay session on `"timeline restart"`, synchronously inside `GgpoStop`. That is required: the restart handler sets `gNetplayAwaitFight` only when netplay is armed at that moment.
    - The same path is testable offline: `netplayrearm 1` and `tools\two_instance.ps1 -AutoRearm` arm fight 1 only, and fights 2–3 must start by themselves.
  - **Online natives (22:55 build):** 135 natives reach the online engine (`0x0093C7D0` or `0x0081C080`) within two calls. The native trace logs the first call of each (`ONLINENATIVE first call: id … at gf … (live|replay, …)`), so runs show which ones fight scripts reach. One that sends a message would be sent again by a replay.
  - **First fight of a match (fixed, 22:57 build).** StartSendingGamepadData zeroes GameFrame (a timeline restart) before the two games' hellos meet, so an armed session waited for the following restart, and the first fight ran on MK9's netcode.
    - `Rollback::NetplayAwaitThisTimeline()`, called right after the engage arms, waits for this timeline's first fight frame when none has run yet (`gFoughtThisTimeline`).
    - If one game engaged before that frame and the other after it, the first one's join times out and the match falls back to MK9's netcode. That is safe, and unlikely: both engage within a round trip, and the fight intro lasts seconds.
  - **Sharing the Steam P2P session.** MK9 reads one channel per call: `0x0094B529` asks for channel 0 explicitly, and the read loop at `0x004F4F74` asks for the channel its socket returns (`0x0049FB70(this+0xD4)`). It never drains other channels, so our bridge (77/78) and hellos (79) are not consumed by the game, and SteamBridge reads only its own channels.
  - **Connection tick `0x008868C0`.** It calls `0x0091FD00`, `0x0090FB60`, `0x0091F830`, `0x0091F4C0` and the manager's `0x0093CAB0`, then the exchange. When the exchange returns 0 and the connection is valid (`0x0090FB00`), it calls `0x009196D0`, then tail-jumps to `0x0090FB40`.
    - Every exit of the exchange returns 0 (each `ret 4` follows `xor eax,eax`), and the disconnect decision is inside its own `0x00881F10`. So our hook returning 0 while skipping is the normal path, and the rest of the tick keeps running.
  - **MK9's lockstep pause** (`0x00407AB0` from the exchange when remote records are missing) can be active at the moment a session starts. The netplay start's unpause (`RequestPause(0)`, the same setter through the pause guard's trampoline) clears it.
  - **One-on-one gate (23:00 build).** GameContext is at online manager `[0x00E924F0] + 0x73C` (the connection is manager `+0x1F4`, via `0x0093C7D0`): `+0x00` Mode (None/Looking/Found/Cancel, table `0xDEF708`), `+0x04` Type (None/Ranked/Player/Private, `0xDEF6F8`), `+0x10` MatchType (DM/KOTH).
    - The online mode engages only for Type 2/3 (Player/Private) with MatchType 0 (DM).
    - In King of the Hill, spectators receive the fighters' pads through the exchange we skip, and the idle hook would also keep their timeouts from firing. Anything unreadable is refused (`ONLINE: not engaging in this match`).
  - **Online natives that fights call (ONLINENATIVE, soak 12101–12109, offline).** 3275 (gf 48, `0x0080DF50` GameInfo reset, no network), 1165 (`0x0081C240` "connection valid" → bool), 1404 (by-name lookup) and **1182 at gf 290–460**.
    - 1182 → `0x008808C0` sends **message 14** (connection `+0x4C34/+0x4C36` as 4 bytes, sets `+0x4C38 = 1`) through `0x0081BCD0`.
    - 1172 (StartSendingGamepadData) is never reached offline, as expected.
  - **Replay send guard (02:21 build).** `0x0081BCD0` (cdecl `send(type, data, size)`, 6 callers, prologue `55 8B EC 6A FF`) is hooked. During a rollback replay with an online manager present, the send is skipped and returns 1 (logged `ONLINE: a rollback replay tried to send online message …`): the original pass already sent it, and connection memory is not snapshotted.
    - Offline the original runs unchanged (it returns 0 without a manager).
    - Only `0x008C0054` (natives 1222/1223) uses the result, as a script result, so the replay must return what the original did.
    - `tools\online_smoke.ps1` now also requires this third hook.
  - Not yet known: whether native 1172 runs once per fight or once per round. If per round, each round starts a new session (one start handshake per round).

## MK9's own online desync check (OnlineDesync, 2026-10-17)

Found from the strings `OnlineDesync::Initialize(NbFenceposts=%u)` and `[MKOnlineGamepadData] ... DesyncHash: %08X`.

- **Fencepost ring:** records are 0x1C bytes `{name, a, b, c, GameFrame, script id, kind}`, at `[0xFE2168]` with count `[0xFE216C]`. The write index is `0xFE2170`, the read index `0xFE2174`, the running hash `0xFE2178`.
  - `0xB934A0` allocates the ring; it is called from `0x822505` and `0x93E121` (online setup only).
  - The ring is active only while fenceposts are allocated, i.e. in online matches.
- **Recording:** `0xB93380` (int values) and `0xB933C0` (float values) append through `0xB932E0`.
- **Hashing:** `0xB93540` folds the new records into `0xFE2178` (string and dword hash `0x50C270`). The result is sent as `DesyncHash` with every 14-byte gamepad record: frame number, buttons, hash (serialised at `0x8812xx`).
- **What is checked:** only 7 call sites.
  - `0x880280`: at match start, the hashed names at `GameInfo` stage `+0x3C` and both players' `+0x964` (character names).
  - `0x881C58/0x881C6F`: each frame, two floats from a fighter getter (`0x433460`/`0x433490`; `0x433410` is the position getter).
- **Implication:** MK9's original online mode is lockstep (delay-based) and relies on the simulation being deterministic, with a cheap two-float check. Our lockstep mode uses the same model. Its checksum is not a usable full gameplay hash for rollback SyncTest.
- **Related:** the actor property `bIgnoreForDesyncs` (`+0xA8`, see [mkpawn_layout.md](mkpawn_layout.md)) and the online-session strings `ORMF_DESYNC_AI_FIGHT`, `ORMF_DEBUG_DESYNC_BYPASS`, `DT_MatchConnectionDesync`, `ONLINEDUMPDESYNC`.
