# MK9Rollback: desync hash, RNG, native recorder, character object (static pass)

Target: `MKKE.exe` analysis copy, SHA-256 `D1C71E086DEAC26716E48F060E486679A36DDD4002AB5080D66DF9D11437D0E6`. The hash was re-verified after the pass. Image base 0x400000, no ASLR. All addresses are VAs.
**Static only.** The game was not run or attached.

**Tools**
- **REA 6.4.0** with the Ghidra 12.1.4 provider, one session with full auto-analysis. The driver is `analysis/desync-rng/rea/rea-inbox2.mjs`, a copy of `scripts/rea-inbox.mjs` with its own inbox.
- **llvm-objdump**.
- **Small byte-scan scripts.**

**Raw evidence** (`analysis/desync-rng/`)
- `rea/out/*.json`: about 560 REA responses. Batches:
  - `b01`, `b03`, `b06`, `b08`, `b11`: pseudo-code.
  - `b02`, `b05`, `b10`: containing procedures.
  - `b04`, `b07`, `b09`, `b12`: callers.
- `disasm_key.txt`: objdump of every function cited below.
- `lcg_sites.txt`: all 87 inlined LCG sites and their state global.
- `e5352c_refs.txt`: all references to the second RNG state.
- Classification summaries:
  - `contain_b02.tsv`, `contain_b10.tsv`: containing procedures per site.
  - `classify_b03.txt`, `classify_b08.txt`, `classify_b11.txt`: strings and callers per containing function.
- Scripts: `lcg_sites.ps1`, `imports.mjs`, `strat.mjs`, `vscan.mjs`, `classify.mjs`, `contain.mjs`, `mkbatch.mjs`, `show.mjs`.

Confidence labels: HIGH means decoded from disassembly or decompilation with cross-checks. MED means partially inferred. LOW means a guess.

---

## 1. OnlineDesync: what MK9 hashes

### 1.1 Data structures (HIGH)

| VA | Meaning |
|---|---|
| `0xFE2168` | `Fencepost* entries`: heap array of `NbFenceposts` × 0x1C bytes, used as a **ring buffer** |
| `0xFE216C` | `NbFenceposts` (capacity). If it is 0, the system is disabled and the hash function returns 0. |
| `0xFE2170` | write index (next slot; wraps to 0 and then sets `0xFE217C=1`) |
| `0xFE2174` | index of the last entry already folded into the hash |
| `0xFE2178` | **running hash** (u32 CRC state). It is never reset per frame. |
| `0xFE217C` | "wrapped" flag (byte) |

**Fencepost entry (0x1C bytes)**, filled by `AddFencepost 0xB932E0`:

| Off | Field | Hashed? |
|---|---|---|
| +0x00 | `const char* name`. Every call site passes `0xC5A3FC = ""`. | yes, the string bytes (0 bytes here) |
| +0x04 | u32 `a` (always 0 at the call sites) | yes, 4 B |
| +0x08 | `const char* str2` (always NULL) | yes, as a string, only if non-NULL |
| +0x0C | u32 **value** (float bits or a CRC) | yes, 4 B |
| +0x10 | **GameFrame**, `[[0xDEF4B4]+0x70]` = `0xE89BE0` at add time | yes, 4 B |
| +0x14 | `[[0xE39DF4]+0x50]`: the current MKScript process type, or 0 when no script context | yes, 4 B |
| +0x18 | type byte (1 = int, set by `0xB93380`; 3 = float, set by `0xB933C0`) | **no** |

### 1.2 Functions (HIGH)

| VA | Name | Logic |
|---|---|---|
| `0x00B934A0` | `OnlineDesync::Initialize(n)` | Frees the old array and allocates `n*0x1C` through GMalloc (`[0xE3EFC8]` vt+8). Then memset 0 and logs "OnlineDesync::Initialize(NbFenceposts=%u) MemUsage:%u bytes". Callers: `0x822430` (from `0x6128D0`) and `0x93E020`. Both use `n = 10` by default, overridable with the command-line switch **`ONLINE_FENCEPOSTS=`** (string `0xD62398`, `atoi`). |
| `0x00B935E0` | `OnlineDesync::Reset` | memset 0; write index = last index = **hash = 0**. Its only caller is `0x884770` StartSendingGamepadData, at online match start. |
| `0x00B932E0` | `AddFencepost(Fencepost*)` | Fills +0x10 and +0x14 as above, copies the 0x1C bytes into `entries[write]` and advances the ring. It does nothing if capacity is 0. |
| `0x00B93380` / `0x00B933C0` | `AddFencepostInt(name,a,str2,v)` / `AddFencepostFloat(…)` | Build the entry with type 1 or 3 and call `0xB932E0`. These are the **only** two callers of `0xB932E0`. |
| `0x00B93400` | `HashFencepost(e)` | `crc(name,strlen)`, `crc(&e+4,4)`, `crc(str2,strlen)` if non-NULL, then `crc(&e+0xC,4)`, `crc(&e+0x10,4)`, `crc(&e+0x14,4)`, all into `0xFE2178`. |
| `0x0050C270` | `Crc32Update(u32* state, data, len)` | MSB-first table CRC: `s = (s<<8) ^ T[(s>>24) ^ byte]`. Table `0xC719E8` = `{0, 0x04C11DB7, 0x09823B6E, …}`, i.e. **CRC-32 poly 0x04C11DB7, non-reflected, init 0 (at Reset), no final XOR**. This is UE3 `appMemCrc` style without the inversion. |
| `0x00B93540` | `OnlineDesync::ComputeHash()` | Returns 0 if capacity is 0. Otherwise it **incrementally** folds only the entries added since the last call: `[last, write)`, or on wrap `[last, cap)` then `[0, write)`. It then sets `last = write` and returns `[0xFE2178]`. |

The returned value is therefore a **cumulative CRC of every fencepost added since the online match started**. Entries that are overwritten before being hashed (more than 10 per frame) are lost.

### 1.3 Who registers fenceposts: the complete set (HIGH)

There are exactly 7 call sites of the two wrappers in the whole binary. There are no other writers of `0xFE2170`/`0xFE2168`; every reference is inside `0xB932E0..0xB93627`.

| Region / value | Address / expression | Size hashed | Registered by (VA) | Notes |
|---|---|---|---|---|
| Arena name CRC | `crc32(GameInfo.matchSetup = [0xE89B8C] + 0x3C, strlen)` | 4 B value (+frame, +ctx) | `0x008802D9` in `0x880280` | `[0xE89B8C]+0x3C` is the background name. `0x8154F0` writes "empty_map" there. The CRC accumulates in a local across the next entries. |
| P1 character name (chained CRC) | `crc(prev, PlayerInfo[0]+0x964)` (`0xE89D7C+0x964`) | 4 B | `0x00880311` | `PlayerInfo+0x964` is the character name string. The same field is copied into the practice-recording header. |
| P2 character name (chained) | `crc(prev, PlayerInfo[1]+0x964)` (`0xE8AA08+0x964`) | 4 B | `0x00880345` | |
| Tag mode: P1/P2 names again (chained) | same two strings | 4 B each | `0x00880381`, `0x008803B1` | Only if `[0xE8CFF0] != 0` (tag-team flag). |
| **P1 team health** | `PlayerInfo[0].GetObj()+0x78C0` (float), **plus the tag partner's `+0x78C0`** when `0x892B70()` says tag is active (`0x894290`) | 4 B (float bits) | `0x00881C58` in `0x881A70` | Added once per online-tick frame, and only if `MKOnlineGame+0x4BA0 || +0x4BA4` (`0x81C3A0`/`0x81C3D0`). The value is 1.0 if there is no object. |
| **P2 team health** | same for `PlayerInfo[1]` | 4 B | `0x00881C6F` | Same gate. |

Every entry also hashes the **GameFrame** (`0xE89BE0`) and the **script process type** `[[0xE39DF4]+0x50]` captured when it was added.

**Not hashed:** positions, velocities, meter, timers, RNG state, script state, physics, projectiles and the pad data.
- `0x881A70` *does* compute both fighters' positions and velocities (`0x433460`/`0x433490`, char vt+0xC4) and the tag partners' (`0x44CB60`) into locals.
- Those values are **never** passed to a fencepost. They are dead debug code left after the log calls were stripped.

The builtin checksum is therefore extremely thin: **match identity (arena + character names) + per-frame team health + frame number**. NRS's lockstep netcode relied on lockstep determinism and used this only as a coarse tripwire. It is **not** usable as a state map for rollback. The only hint it gives is that `obj+0x78C0` (life) is authoritative.

### 1.4 When it runs and what happens on mismatch (HIGH except where marked)

The sequence, per online frame, from `0x885F80` MKOnlineGamepadData::Tick (main loop → `0x81C7C0` → online object vt+0x3C `0x8868C0` → `0x885F80`):
1. `0x885710` UpdateLocalAndSend stores `ComputeHash()` into the local ring record. The **11-byte record** is `{u32 frame, u8 flags, u32 desyncHash @+5, u16 buttons @+9}`. Calls are at `0x885765` and `0x885896`, the latter once per local pad (a second call adds nothing new). That hash is sent to the peer: "SEND frame:%d GamepadIndex:%d hash:%08X …".
2. `0x881360` applies remote input. It only *logs* "READ REMOTE … local hash:%08X remote hash:%08X".
3. `0x87F440` runs the edge dispatch for every pad.
4. `0x881A70` registers the two health fenceposts for this frame, at `0x886460`. **This happens before EngineTick simulates the frame**, so the values are end-of-previous-frame health.
5. **`0x881C80` CheckDesync:**
   - It compares the local ring[read] hash with the remote ring[read] hash.
   - On a mismatch, unless debug flag `0x10000000` is set in `[0xE3D6F4]` (`0x4D4560`), it calls `0x81C640(2)`. That stops the recorder (`0x4D36A0`), calls StopSendingGamepadData (`0x87FB70`) and disconnects with reason 2 (`0x944C90`).
   - It returns 1, and Tick aborts (`0x886472`).

The name fenceposts come from `0x880280`, which is **MKOnlineGame vtable `0xD68A78` slot +0x20**. It has no direct callers. Its exact virtual caller was not resolved; it is most likely the fight-start/load-complete notification (**MED**).

The ring is reset at `0x884770` StartSendingGamepadData, which is called from ConnectionSyncCompleteHandler `0x885340` (param 1 or 4) and from `0x81C080`.

**Offline: nothing is ever hashed.** `0xB93540` has only the two callers above. The fencepost adds are gated on online state, apart from `0x880280`, which is an online-object virtual.

### 1.5 Second "fencepost" mechanism: recorder tags (HIGH)

The recorder interface (`[0xE3D6E4]`, §3) has vt+0x08 `RecordFencepost(tag, ptr, size)`. In the **online async-load recorder** (`0x87F660`: records every tag except 0xFF0009) and its playback (`0x882300`: reads the tag, memcmps, and discards the result), these sync markers are emitted:

| Tag | Data | Emitted at | Meaning |
|---|---|---|---|
| 0xFF0004 | u32 RNG seed | `0x5ED570` SetSeed, `0x88682C` (session seed `MKOnlineGame+0x4C2C`) | synced seed |
| 0xFF0006 | u32 linker-tick result | `0x62CBCB` (async package loader `0x62CA40`) | async load progress |
| 0xFF0007 | 12 B `char+0x1E4` (vec3) | `0x44868B` in MKFGCharacterObj vt+0x18 `0x448660`, only when the actor `[obj+8]` is set and `!0x785370()` | per-character vector, probably root position |
| 0xFF0008 | u32 GameFrame `0xE89BE0` | EngineTick `0x80F6AC` | |
| 0xFF0009 | u32 HostFrame `0xE89BDC` | EngineTick `0x80F68F` (ignored by the online recorder) | |
| 0xFF000A | u32 **one RNG draw** | EngineTick `0x80F6C0` | see §2.1 |
| 0xFF000E | async-load event | `0x87FF65`, `0x8816EC` | |

This set (frame numbers, an RNG draw, one vec3 per character) is a slightly better hint at what NRS considered sync-critical. It is still not a state map. Both replay-recorder classes treat vt+0x08 as a no-op (`ret 0xC` at `0x58DA60`).

---

## 2. RNG

### 2.1 Stream A: MK "synced" RNG, state `0xE7C054` (HIGH)

- **`0x005F5080` RandInt(file,line):**
  - `x = x*0x0BB38435 + 0x3619636B` (full 32-bit wrap, stored back unmasked).
  - Returns `x & 0x7FFFFFFF` in EAX.
  - cdecl. The two pushed arguments (`"..\..\MKCore\Src\UtilsCore.cpp"`, line) are ignored.
- **`0x005F50A0` RandFloat(file,line):**
  - Same update.
  - Builds float bits `(x & 0x007FFFFF) | 0x3F800000`, a value in [1,2).
  - Returns `f - (float)(int)f`, a value in [0,1), in **x87 ST0**.
- **`0x005ED570` SetSeed(u32):**
  - First calls recorder `0x4D3600(0xFF0004, &seed, 4, 0)`.
  - Then sets `[0xE7C054] = seed`.
- The constants are **UE3's `appSRand`** (196314165 / 907633515). MK copied the UE3 seeded generator into a private state.
- **Only 6 direct call sites** (no indirect references):

| Site | Containing fn | Role |
|---|---|---|
| `0x80F6C0` | EngineTick `0x80F670` | **Draws one value every unpaused host frame** (menus too). The result is only passed to the recorder (tag 0xFF000A). The state therefore advances by ≥1 per unpaused frame regardless of gameplay. |
| `0x49205D` | `0x492050` **RandomInt(n)** = `RandInt % n` | 93 call sites in 43 functions, below |
| `0x491A4D` | `0x491A40` **RandomChance(p)** = `p >= RandFloat` | 1 site: MKScript native `0x8B87D0` |
| `0x493FDD` | `0x493F80` pick 1 of ≤4 unhidden mesh pieces at `skel+0x570[idx]` and hide it | callers: `0x447240` (character per-region damage accumulator `+0x4D8`), `0x4472D0`, `0x4C5FD0` (anim event keys). This is **battle-damage / costume-piece cosmetics, but on the synced stream**. |
| `0x494BF2` | `0x494BD0` `rand%100 <= p` gate | MKScript native at `0x8D14A2` |
| `0x495B28` | `0x495AF0` frame-gated `rand%100 <= p` (`GameFrame - last >= n`) | MKScript native at `0x8D142E` |

**RandomInt (`0x492050`) callers, classified** (b05/b08/b09; strings and callers in `classify_b08.txt`):
- **AI (gameplay), 59 sites:** `0x8892F0`, `0x8893C0`, `0x88A270`, `0x88C700`–`0x890C00`. Strings include "ai_respond_to_*" and the AIDriver region next to `0x889610`.
- **Character gameplay, 1 site:** `0x448920`. A per-frame percentage roll on `char+0x2FBC..+0x2FC8`; on success it calls char vt+0x1C0.
- **MKScript native "random int", 1 site:** `0x8B87B0`. It is used by fighter and other scripts (gameplay **and** cosmetic).
- **Input / minigame, 1 site:** `0x426AF0`. It shuffles the attack buttons {0x10,0x20,0x40,0x80} into two random pairs and pushes them to a pad. This is likely a Test-Your-* style prompt (MED).
- **Pre-fight selection (menu/UI), 11 sites:**
  - `0x810530`, `0x8105C0`, `0x810670`, `0x812540`, `0x812870`, `0x8138D0`: random arena / character / costume.
  - `0x815B90`, `0x815D00`: random boss / alternate characters such as "CHAR_Goro" and "char_kratos".
  - `0x8F2BB0`: random background list.
  - `0x82E4F0`, `0x82EAF0`: random names, e.g. "Baraka…" and dev names "Greenberg…".
- **Other script / fatality / gore, 20 sites:** `0x8F6120`, `0x8F9260`, `0x8F9C70`, `0x8F9E70`, `0x8FB060`, `0x8FB390` ("HL_MeatArmL"). Mostly cosmetic but script-driven (MED).

**Seeding (HIGH):**
1. `0x8154F0` FG_GAME_INFO::GameInit (boot) calls `SetSeed(QueryPerformanceCounter().LowPart)` (`0x81552A`).
2. `0x884770` StartSendingGamepadData (**online match start**) calls `SetSeed([MKOnlineGame+0x4C2C])` (`0x884830`). This is the host-agreed seed, logged as "StartSendingGamepadData (RandSeed:0x%08X)". The same function resets GameFrame to 0 (`[0xE89BE0]=0`), clears the `[0xE89B8C]+0xA4` tables and calls `OnlineDesync::Reset`.

There is **no per-match reseed offline**. The seed carries over from boot and from all previous menu frames.

### 2.2 Stream B: UE3 `GSRandSeed`, state `0xE5352C` (HIGH)

- **Same LCG constants, always inlined** (`appSRand`): **85 of the 87 constant sites** use `0xE5352C`. Some of them keep the state in a register across consecutive draws. The other 2 are `0x5F5087`/`0x5F50AD` on `0xE7C054`. See `lcg_sites.txt` and `contain_b02.tsv`.
- **Seeded once at boot** in `0x404C33`/`0x404C48`. The seed is 0 if `-BENCHMARK` (`[0xE4303C]`) is given, otherwise `QueryPerformanceCounter().LowPart`.
- It is **never reseeded**, including at online match start. All other writes are LCG stores.

**Users** (41 REA-resolved containing functions, plus 2 code blocks REA did not place in a function, `0x5AE2B0` and `0x8EDF20`; the `0x4E3B50` helper adds 24 more callers):
- **Engine cosmetics, nearly all of them:**
  - particle emitter spawn: `0x59C120`, `0x5AB650`, `0x6F32B0`, `0x6FB9D0`, `0x70FA90`, `0x710590`;
  - distributions: `0x4E3B50` = `appSRand()` helper, with 24 callers;
  - FX: `0x7B9D10`–`0x7BBD30`;
  - plus `0x4A4DF0`/`0x4A5040` "Engine", `0x501xxx`, `0x534C50`, `0x61Bxxx`–`0x6204B0`, `0x636500`, `0x6B4A10`, `0x75C2B0`, `0x7FC480`.
- **Online / UI:**
  - `0x81F7E0` "vip_code" generator;
  - `0x9278F0` "load_screen_names";
  - `0x964B80`/`0x96C420` session jobs (nonces);
  - `0xB938A0`.
- **MK gameplay-adjacent (watch these):**
  - **`0x493C90` "unsynced script random(n)"** (MKScript native `0x8B8790`). It **returns 0 when the current script process type `[[0xE39DF4]+0x50]` is 2, 3 or 4**, and otherwise draws from `0xE5352C`. This is clear evidence that NRS split RNG into a synced stream (A) for gameplay process lists and an unsynced stream (B) for everything else.
  - **`0x887F50` RandRangeClamped(min,max,spread,center)** (MKScript native `0x8B7FD0`) draws from B **without** the process-type guard.
  - **`0x8EDF20`**, a raw `appSRand()&0x7FFFFFFF` MKScript native in native table `0xDF0F10`, also has no guard.
  - `0x873BC0` (native `0x8DD9F0`) is a randomised shake/impulse vector at obj+0x288..+0x290 (camera, MED).

**Implication:** for rollback, `0xE5352C` must be saved and restored with the snapshot. Resimulated frames re-run particle and FX code that draws from it, and a few script natives that may run in fighter scripts draw from it too. Within one machine, B stays deterministic only if cosmetic draws are identical on re-sim. Across peers it starts unsynchronised, so it must also be **seeded identically at match start**. NRS never did this, which is why it had to keep gameplay off B.

### 2.3 Other randomness (HIGH for existence, MED for classification)

| Source | Where | Class |
|---|---|---|
| MSVCR90 `rand()` (`0xC55424`, thunk `0x98CD16`) | 26 calls in FMOD (`0xA35xxx`–`0xA5Axxx`: `fmod_eventsystemi.cpp`, `fmod_sounddef.cpp`), plus `0x6A52D0` (`rand()%255` byte fill) | audio / engine cosmetic. `srand` is called once in FMOD init (`0xA3F5C2`). Per-thread CRT state. |
| MSVC-style LCG `*0x343FD+0x269EC3` inlined | `0x9B89C0` (state `[obj+0x64]`), `0x9E82A0` (state `[ecx]`) | **per-object** third-party RNG with no strings, near `0xDFA93C` (probably a UI or middleware library). Cosmetic. |
| ANSI LCG `*0x41C64E6D+0x3039` | `0xBBCDD0`, state `0xFE2E70` | network / crypto library region. Not gameplay. |
| `CryptGenRandom` | `0xBA9610` | network/crypto. Not gameplay. |

No other LCG/MT/xorshift constants were found (MT `0x6C078965`, `0x9908B0DF`, `0x19660D` and `0x5DEECE66D` all have zero hits). PhysX was not inspected for internal randomness.

---

## 3. Native recorder (`[0xE3D6E4]`)

### 3.1 Interface (HIGH)

These are global stubs that forward to the vtable of `[0xE3D6E4]` when it is non-NULL:

| Stub | Slot | Meaning |
|---|---|---|
| `0x4D3600(tag,ptr,size,padIdx)` | vt+0x04 | RecordEvent / Serialize |
| `0x4D3630(tag,ptr,size)` and `0x4D3660(tag,u32)` | vt+0x08 | RecordFencepost |
| `0x4D3680(x)` | vt+0x10 | |
| `0x4D36A0` | vt+0x14 then vt+0 delete, then the global is set to NULL | Stop |
| `0x4D36D0` | vt+0x18 | **BeginFrame** |
| `0x4D3710` | vt+0x1C | IsRecording |
| `0x4D3730` | vt+0x20 | IsPlayback |
| `0x4D36F0` | — | Install |

There are three implementations:
1. **Practice recorder / playback** (vtables `0xD6C7C8` / `0xD6C7EC`): the one in the question.
2. **Online async-load recorder / playback**: "MKOnlineInputRecorder" / "MKOnlineInputPlayback". `0x886670` installs it at GAME_START_GO when debug flag `0x4000000` is set; otherwise it installs object `0xD68524`. It records events and fenceposts while one peer is still loading ("Recorder was done, wait for playback to complete").
3. The `0xD68524` object.

### 3.2 Practice recorder: what a frame contains (HIGH)

- **BeginFrame:** record = `0x903560`, playback = `0x903700`. For P1 then P2 (`PlayerInfo[i]+0xB40` pad, or zeros if there is no pad):

  | Tag | Content |
  |---|---|
  | `0xFF0010` / `0xFF0012` | pad `+0x0C`, **12 B** (cur record) |
  | `0xFF0011` / `0xFF0013` | pad `+0x08`, **4 B** (flags) |

  Playback then calls `PadEdges 0x87F440(pad)`. In playback it **overwrites** both pads' cur and flags, keeping only the live `flags & 0x300`.
- **Serialize** (record `0x9042E0`, playback `0x9043D0`) accepts only tags `0xFF000B`, `C` and `D` always, and `0xFF0010`..`13` **only while not paused**.
  - Everything else is dropped: seed `FF0004`, frames `FF0008`/`9`, RNG `FF000A` and character `FF0007`. Fencepost vt+8 is `ret 0xC`.
  - The stream format is `{u32 tag, data}`, so a frame costs **48 bytes**.
  - Playback re-reads each tag and stops on a mismatch or on `0xFF000C`.
- **Session header:** written at start, by playback `0x903E40` and record `0x904D00`:

  | Tag | Content |
  |---|---|
  | `0xFF000B` | 4 B from the practice manager `0xE8DF64+0` |
  | `0xFF000D` ×2 | P1 and P2 **meter** (`PlayerInfo+0xC14`) |

  The slot header (outside the stream) also holds:
  - the P1/P2 name strings (`+0x964`, `+0xA50`);
  - the arena (`[0xE89B8C]+0x3C`);
  - a label "%s vs %s %s".
- **Storage:** `[0xED680C]`, 8 slots × 0x23448 bytes = 0x11A240 total (allocated in `0x903F80`). Each slot has a 0x1C0 header and a stream capped at **0x23280 bytes**, about 2,999 frames or **~50 s**. When the stream is full, `0x903CD0` is called. Slots are saved to and loaded from a save file ("Practice" / "SaveFileName").
- **End:** `0x903870` writes `0xFF000C` (record), stops, and restores P2's AI pad (index 0xF) on playback.

### 3.3 Enabling (HIGH for the mechanism, MED for exposure in the shipped UI)

- **Not a command-line switch.** It is driven by **MKScript natives**. The native table entries at `0xDF3C9C..0xDF3CB4` locate the practice manager through `0x458930` and call:
  - **record:** `0x8D3DE0` → `0x904D00(slot)`;
  - **stop:** `0x8D3E10` → `0x903870`;
  - **queries:** `0x8D3E40` / `0x8D3E70` → IsRecording / IsPlayback;
  - **slot validation:** `0x8D3EA0` → `0x9044E0(slot)`;
  - **playback:** `0x8D3EE0` → `0x903E40(slot)`.
- The UI is the "ui_i_recorder" movie with strings "Practice/RecorderStop" and "PlaybackStop" (`0x903130`).
- **Both start paths first call `0x903CD0`.** It stops any recorder and pauses audio (`0x426A70`). It then resets every PlayerInfo (`0x892670` ×4) and runs the script function **"ResetForPractice"**. Playback then sets P1's AI controller mode (`+0x1C = 2`, `0x889180`).
- Precondition: `[0xED680C] != 0` (the slot buffer exists) and `slot < 8`.

### 3.4 Verdict (HIGH)

- **Not a deterministic replay.** It is a Practice-mode **input macro**: P1 and P2 pad cur and flags per unpaused frame, plus the two meters.
- **Starting state:** the start state is whatever the "ResetForPractice" script produces. It is not a snapshot.
- **What it omits:** no RNG seed or state (neither A nor B), no frame numbers and no state checksum. The playback-side "fencepost" compare in the online variant discards its result.
- **Practical replay:** it can reproduce a match only when the practice reset is complete and RNG consumption is identical. AI, random script natives and battle-damage cosmetics consume stream A, which is never reset here.
- **Format:** `{tag,u32}`, 48 B per frame, ~50 s cap, 2 players only.
- **What to reuse:** the BeginFrame hook point and the "overwrite cur + flags, then `0x87F440`" pattern. This is the same pattern we already use.
- **For offline validation:** record our own pads and also snapshot and restore `0xE7C054`, `0xE5352C` and GameFrame at the start.

---

## 4. Character object: `MKFGCharacterObj` (vtable `0x00D6B3E0`)

### 4.1 Size and allocation (HIGH)

- **The allocation size is 0xC580 bytes (50,560 B).**
- All three construction sites call `0x42D5F0(0xC580)`, which is GMalloc 16-byte aligned (`0x887520(…,0x10)`). They then call the constructor:
  - `0x8AEAED` → ctor `0x8AD7D0` (in a factory function at `0x8AEAxx`);
  - `0x8D6F73` → ctor `0x8AD7D0` (MKScript native; the result goes to the script return register `[[0xE39DF4]+0x1C]`);
  - `0x8D7003` → ctor `0x8ADA30` (alternate base `0x457250`; MKScript native).
- `0x8ADC80` is the destructor.
- The class name is "MKFGCharacterObj" (`0xDF0B10`, returned by `0x8ADA20`; used by the `0x42D5C0` IsA checks in `0x903CD0`).
- The last constructed member is a TArray at `+0xC554` (12 B, ending at `+0xC560`). The remaining bytes up to `0xC580` are scalars and padding.

### 4.2 Layout landmarks (MED unless noted)

**Base part** (`0x458040`, which first calls `0x445B30`):

| Offset | Content |
|---|---|
| +0x00 | vtable |
| +0x04 | generation id (HIGH, ledger) |
| +0x08 | UE3 actor\* (HIGH) |
| +0x1D8, +0x228, +0x288, +0x2A4, +0x2B0, +0x2F8, +0x2F0C, +0x2F18, +0x2F2C, +0x2F40, +0x2FA0 | Container sub-objects whose vtable pointer is written at that offset (`0xC5B274` or `0xC5AEB4`; constructed with `0x42D7B0` / `0x42D660`). Offsets were derived from the dword indices in the decompiled `0x458040`. |
| +0x27C, +0x280, +0x44C..+0x46C | scalars zeroed by the constructor |
| +0x1E4 | vec3 recorded as sync tag 0xFF0007 (HIGH) |
| +0x2E8 | flags (`&0x10` triggers the input-lock recompute) |
| +0x2FA4 | script context (MK9Hook) |
| +0x2FBC..+0x2FC8 | random-chance gate {frame, count, threshold, percent} (`0x448920`) |
| +0x30B4 | inline array of **0x3C × 0x124 B** (ends at `+0x7524`) |
| +0x7524 | move list; count at +0x7AEC (pass 1) |

**Derived part** (`0x8AD7D0`):

| Offset | Content |
|---|---|
| +0x7638 | PlayerInfo\* (HIGH, pass 1) |
| +0x7724, +0x7788, +0x7798, +0x77A8, +0x7874, +0x78C8, +0x78DC, +0x7F48, +0x7F5C, +0xA950, +0xA968, +0xAA78, +0xAA9C, +0xAAB4, +0xC554 | TArray / list members (heap pointers) |
| +0x77B8 | sub-object (`0x45A230`) |
| +0x77D8 | 8 × 0x10 B array |
| +0x785C..+0x7870 | zeroed scalars |
| **+0x78C0** | **life (float)** (VERIFIED-RT, ledger) |
| +0x78F0, +0x7B0C, +0x7D28 | three 0x21C-byte sub-structs (`0x44F9F0`) |
| +0xAAC0 | inline array of **0x32 × 0x88 B** (ends at `+0xC550`) |

**Rollback caveat:** a raw `memcpy` snapshot of 0xC580 bytes is not sufficient.
- At least 20 members are UE3 `TArray`s or heap-backed containers. Their contents live outside the object and would need deep copies.
- `+0x08` points into the UE3 actor and its components, which hold animation and physics state.
- **Hashing raw bytes for cross-peer desync detection is invalid**, because the pointers differ between machines. A hash has to skip pointer fields or hash the pointed-to data.

---

## 5. Corrections / additions to the ledger (`reverse_engineering.md`)

1. **RNG update:** `0x5F5080`/`0x5F50A0` store the **full 32-bit** LCG state. Only the return value is masked: `&0x7FFFFFFF` for the int, and a 23-bit mantissa giving [0,1) for the float.
2. **EngineTick draws RNG:** EngineTick **draws** one value from stream A every unpaused frame (`0x80F6C0`) rather than "records the state". The value goes to recorder tag 0xFF000A.
3. **Two RNG streams:** there are **two** RNG streams that matter: `0xE7C054` (synced: seeded at boot and at online start) and `0xE5352C` (UE3 `GSRandSeed`: boot only, 85 inlined sites). Both belong in the rollback snapshot. Both need to be seeded identically at match start for P2P.
4. **OnlineDesync scope:** the OnlineDesync checksum covers only arena/character name CRCs, per-frame team life, GameFrame and the script process type. It is CRC-32 (MSB-first, poly 0x04C11DB7, init 0, no xorout), cumulative since online start, with a 10-entry ring (`ONLINE_FENCEPOSTS=`). A mismatch means disconnect reason 2. It **cannot serve as a state map**.
5. **Character size:** MKFGCharacterObj allocation size = **0xC580**.

## 6. Open items

- The virtual caller of MKOnlineGame vt+0x20 (`0x880280`), i.e. when the name fenceposts are added.
- Whether the Practice recorder UI is reachable in the shipped PC menus. Its strings and save-file plumbing exist.
- **Process types:** confirm at runtime which script process types are 2, 3 and 4 (the "gameplay" lists that block unsynced random). Also check whether fighter scripts call the unguarded natives `0x8B7FD0`, `0x8EDF20` and `0x8DD9F0`.
- **Character `+0x1E4`:** the semantics of the vec3 at `char+0x1E4` (tag 0xFF0007).
- PhysX internal randomness was not inspected.
