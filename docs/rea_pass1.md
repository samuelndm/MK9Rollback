# MK9Rollback — REA pass 1: simulation tick and processed-input candidates

Target: `MKKE.exe` (x86 PE32), SHA-256 `D1C71E086DEAC26716E48F060E486679A36DDD4002AB5080D66DF9D11437D0E6`, preferred image base `0x00400000`.
Analysis copy only: `D:\Projects\MK9\analysis\artifacts\DiscContentPC\MKKE.exe`. The hash was re-verified after the pass. The game was not run or attached.
All addresses are VAs at the preferred base. RVA = VA − 0x400000. At runtime, use module base + RVA.

**Evidence labels used below**
- **[REA]** comes from REA 6.4.0 MCP tools, with the Ghidra 12.1.4 provider running full default auto-analysis. Raw MCP responses with `evidence_id`s are in `analysis/rea-pass1/out/*.json`.
- **[DG]** comes from direct headless Ghidra run `-readOnly -noanalysis` against the old partial `focused-seeds-1` project. It is **not REA**. Dumps are in `analysis/ghidra-dumps/a1..a3.txt`.
- **[RAW]** comes from my own byte or string scans over the PE (`scripts/petool.mjs`).

No runtime verification was done in this pass. Every "HIGH" confidence rating rests on static evidence only.

---

## 1. REA / provider status

| Item | Result |
|---|---|
| Installed REA | `rea-agents` **6.4.0** (`tools/rea/node_modules/rea-agents`) |
| Latest published | **6.4.0**. Local npm 12.2.0 reported `npm view rea-agents` → `dist-tags.latest = 6.4.0`, modified 2026‑10‑11. |
| Repo `main` (github.com/morluto/rea) | `package.json` on main is still `6.4.0`. The newest commits (2026‑10‑13) cover JS/Hopper/managed/Mach‑O fixes and "admit 32‑bit big‑endian PowerPC ELF". No visible commit touches x86/PE32 Ghidra support. The commit body says "Windows Ghidra P0 is unchanged". **No relevant difference for x86 PE32.** |
| x86 PE32 support in 6.4.0 | Supported through the Ghidra provider. `open_binary` auto-detected `format=pe, architecture=x86`. `REA_GHIDRA_LANGUAGE_ID` / `REA_GHIDRA_COMPILER_SPEC_ID` are honoured (`config/ghidraLanguageOverride.js`). Some x64-only paths exist, such as Windows native loader and NativeAOT, but they don't apply here. |
| Claude Code client id | `claude_code` (from `dist/application/SupportedClients.js`). `claude_desktop` is a separate client. |
| `rea doctor --client claude_code --provider ghidra --format json` | `healthy=false`. **Ghidra provider: all checks OK** (12.1.4, headless, native decompiler, JDK 21.0.12.1 64‑bit, Windows native authority). The failures are config-only: `registration:claude_code` missing, `registration:claude_desktop` missing, `skill:identity` missing, plus Hopper and IDA not configured (irrelevant). Codex registration is `aligned`. Saved at `analysis/rea-doctor-claude_code.json`. |
| `rea setup --client claude_code --dry-run --format json` | `status=planned`, **nothing applied**. It would make two changes. (1) Update `%USERPROFILE%\.claude.json`, backing it up to `.claude.json.rea.backup`, to add MCP server `node.exe …\rea-agents\scripts\rea.mjs mcp` with env `GHIDRA_INSTALL_DIR`, `JAVA_HOME`. (2) Install the skill `reverse-engineer-anything` into `%USERPROFILE%\.claude\skills\`. Saved at `analysis/rea-setup-claude_code-dryrun.json`. No Codex or Claude config was modified. |
| **Provider actually loads this x86 PE** | **YES — REA worked end-to-end.** My stdio MCP driver (`scripts/rea-inbox.mjs`) started `rea mcp` with `GHIDRA_HEADLESS_MAXMEM=8G` (process env only), `REA_GHIDRA_LANGUAGE_ID=x86:LE:32:default`, `REA_GHIDRA_COMPILER_SPEC_ID=windows`, and `REA_GHIDRA_STARTUP_TIMEOUT_MS=5400000`. The JVM command line confirmed `-Xmx8G … -processor x86:LE:32:default -cspec windows -readOnly -deleteProject`. **Full default auto-analysis completed in 290 s.** That includes the **x86 Constant Reference Analyzer (50.9 s)**, which previously OOM'd at the default 2 G heap. Analyzer timings are in `analysis/rea-pass1/ghidra-analysis.log`. Afterwards ~120 REA calls (`procedure_pseudo_code`, `procedure_assembly`, `procedure_callers`, `xrefs`, `inspect_native_api`, `resolve_containing_procedure`) returned in 1–700 ms each. REA reports `analysis_profile.language_id = x86:LE:32:default`, `analyzer_preset = ghidra-default`, `import_mode = ephemeral-source-immutable`. The session was closed cleanly and REA deleted its temp project. |
| Why the prior run failed | The prior run used the default `-Xmx2G` from `GhidraJavaLaunch.js` (`GHIDRA_HEADLESS_MAXMEM || GHIDRA_MAXMEM || "2G"`) and the default 330 s startup deadline. 8 G heap plus a long startup timeout fixes both. Host: 16 GB RAM, about 4 GB free at start, and the pagefile absorbed the rest. |
| Caveat | Every REA session re-imports and re-analyzes (~5 min), because REA uses an ephemeral `-deleteProject`. Keep one long-lived session and queue requests (see `scripts/rea-inbox.mjs` + `scripts/rea-queue.mjs`). |

A few early [DG] dumps were taken with the old partial DB while REA was still analyzing. Every function cited in the tables was re-read or cross-checked with [REA] unless it is marked [DG].

---

## 2. Frame pipeline (what one 60 Hz host iteration does)

```
0x00404D30 host loop (one iteration = one sim frame; frame-locked, no accumulator)
 ├─ GameInfo+0x6C (VA 0xE89BDC) += 1                      host frame #     @0x00404DFC
 ├─ if !Paused (0xE43054==0): GameInfo+0x70 (0xE89BE0)+=1  GAME FRAME #     @0x00404E0D/0x00404E12
 ├─ 0x00424A50  hardware sampler: pads[0..5] prev=cur, cur=mapped buttons, push press/release rings
 ├─ 0x0081C7C0  online services → 0x008868C0 → 0x00885F80 MKOnlineGamepadData::Tick
 │               (when online: overwrites local+remote pad.cur from delay rings, edge-detects,
 │                or STALLS the game by setting pause when remote input for the frame is missing)
 └─ 0x004025A0  paced engine phases (QPC wait to fixed period from 0x005EAF40; delta = period)
     ├─ engine vt+0xBC 0x0080DD40 → 0x007F4B00 → 0x00799410 (clock: scales delta, +0x284/+0x288/+0x28C)
     └─ engine vt+0xC0 0x0080F670  "MK9GameEngine::Tick"
          ├─ recorder BeginFrame (0x004D36D0), record E89BDC/E89BE0/RNG (tags 0xFF0009/08/0A)
          ├─ 0x004D6160, 0x0049A0B0, 0x0042DE00 (pre-tick subsystems)
          ├─ if !Paused and fight mode: 0x008ACD90  ★ per-player fight update (input → moves)
          ├─ 0x0045CAF0  MKScript VM: runs all 21 proc lists (fighter scripts)
          ├─ if !Paused: 0x00407960 / 0x0080F570 …
          ├─ 0x00B927E0
          └─ if 0xE3B85C==0: 0x007F7F50 UGameEngine::Tick base → vt+0xD4 0x007F4CE0 world/actors
```

Pacing evidence [REA]: in `0x004025A0`, delta `local_24 = FUN_005EAF40()` is the fixed period. The loop spins with `Sleep(0)`/QPC until the next deadline. The engine slots `+0xBC`/`+0xC0` receive that constant. A late frame only sets `0xE844A5=1`. The earlier Frida trace saw a constant delta bit pattern `0x3C888889` (≈1/60). **There is no fixed-step accumulator: the game is frame-locked, one tick per host iteration.** Rollback re-simulation therefore means re-running the C0 work (or a subset), not draining an accumulator.

---

## 3. Key data structures

### 3.1 GameInfo (static object at VA `0x00E89B70`; `*(0x00DEF4B4) == 0x00E89B70`)
The vtable is `0x00D5E668` (constructor near `0x008156A8`).

| Offset | VA | Meaning | Evidence |
|---|---|---|---|
| +0x10 | 0xE89B80 | mode/fight flags (`&1` fight active, `&0x400`, `&0x20`) | 0x008ACD90, 0x00895150 [REA] |
| +0x14 | 0xE89B84 | more flags (`&0x10` suppresses fight update, `&4`) | 0x008ACD90 |
| +0x5C | 0xE89BCC | last gamepad that pressed a button (set by 0x00425510 any-pad query) | [REA] |
| +0x60 | 0xE89BD0 | "primary keyboard pad" pointer (ring entry flag) | 0x00425770 |
| **+0x6C** | **0xE89BDC** | host frame counter (every loop iteration) | 0x00404DFC, recorded tag 0xFF0009 |
| **+0x70** | **0xE89BE0** | **game frame counter**, advances only when not paused. Used as the timestamp in every input ring entry, by move timers (0x008A1EF0, 0x008959D0), toasty windows, etc. Reset to 0 at online match start (0x00884770 `GameInfo[0x1c]=0`). | 0x00404E12 `inc [eax+0x70]` guarded by `FUN_00406790()==0`; [REA] xrefs: ~230 sites |
| +0x20C | 0xE89D7C | PlayerInfo[4], stride 0xC8C | 0x0080FF30 loop, vector ctor @0x008156B2 |
| vt+0x10 | — | `GetPlayerInfo(idx)` | 0x004D5C90, 0x00881360 |

Pause flag: VA `0x00E43054` (getter `0x00406790`). When it is set, `+0x70` freezes and C0 skips `0x008ACD90`. MK9's delay netcode stalls through exactly this flag (0x00885F80 → `FUN_00407AB0(1)`).

### 3.2 PlayerInfo (`0xE89D7C + i*0xC8C`, i<4; vtable `0x00D5DF68`, ctor `0x008A1580`)
| Offset | Meaning |
|---|---|
| +0x1C/+0x20 | character handle + generation; `0x00895B70` = GetObj (thiscall, RET 4) |
| +0x944 | input-lock flag. When it is 1, every command query returns 0. It is recomputed each frame by vt+0xC `0x00895150`: unlocked only if the pad exists, the character health at char+0x78C0 > 0, and fight flags allow. |
| +0xB3C | player number (0/1/…) |
| **+0xB40** | **primary Gamepad\*** (`player_info->gamepad`, from the debug string at 0x004D5780) |
| +0xB44 | secondary Gamepad\* |
| +0xBA4 | pending basic-attack command id, reset to 0x80000000 each frame by 0x008AAA70 |
| +0xC14 | meter (float) |

The vtable entries that matter for input are:

| Slot | Address | Role |
|---|---|---|
| +0x0C | `0x00895150` | UpdateInputLock |
| +0x14 | `0x00452550` | IsCommandTriggered(cmd) |
| +0x2C | `0x00453A80` | IsCommandReleased-style(cmd) |
| +0x34 | `0x00893200` | context value stored in ring entries; the address is not a function start in REA and still needs resolving |

### 3.3 Gamepad (`m_Gamepads[18]` at VA `0x00E221A8`, stride **0x60C**)
The sampler fills pads 0..5 (hardware). Virtual/AI pads use index 0xE (P1 AI) and 0xF (P2 AI); this is why P2 showed up as pad 15 in Practice (AI dummy, set up by `0x00891AD0`).

| Offset | Meaning | Evidence |
|---|---|---|
| +0x00 | PlayerInfo\* owner | 0x004D5780 "player_info", 0x00425770 |
| +0x04 | pad index | logs in 0x0087F440 |
| +0x08 | flags. `&1` connected/sampled. **`&8` externally driven**: the sampler skips it and only does prev=cur. AI setup sets it at 0x00891BB0. `&0x80` use per-device mapping table. `&0xC9` "valid" test in queries. | 0x00424A50, 0x00891AD0, 0x00425510 |
| **+0x0C** | **current button mask** (u32; only the low 16 bits plus bit 16 are used). +0x10/+0x14 are aux words (12-byte "raw input" record). | sampler, online, replay, AI writers |
| +0x18 | previous 12-byte record (copy of +0x0C..+0x14) | same |
| +0x54/+0x55/+0x56 | ring write indices (u8, 0..29): buttons-press / directions-press / buttons-release | 0x00425770 / 0x004256A0 |
| +0x58 | button-press ring, 30 × 16 B `{mask, gameFrame(E89BE0), ctx=PlayerInfo.vt+0x34(), isPrimaryKbd}` | 0x00425770 |
| +0x238 | direction-press ring, 30 × 16 B `{dirMask, gameFrame, –, flag}` | 0x00425770 cases 1/2/4/8 |
| +0x418 | button-release ring, 30 × 16 B | 0x004256A0 |

These rings are the command buffer. Special-move matching (0x008AB6F0 → 0x008A9510 …) and the pressed/held/released queries all read pad +0x0C/+0x18 and the rings, timestamped with GameInfo+0x70.

### 3.4 Processed button mask (pad +0x0C) — bit meanings
Source: `0x00425F00 CommandToPadMask(cmd)` [REA, static switch], plus the sampler's mapping table at `0x00DE1268`. That table has 16 entries `{isDownFn(slot), mask}` with masks 0x1…0x8000. The DirectInput POV handlers for 1/2 compare against 0/4500/31500 (up) and 18000/13500/22500 (down).

| Bit | Command id(s) | Meaning (confidence) |
|---|---|---|
| 0x0001 | 0x80000003 | **Up** (HIGH: POV 0/45/315°) |
| 0x0002 | 0x80000004 | **Down** (HIGH: POV 180/135/225°) |
| 0x0004 | 0x8000000E | **Left** (absolute screen direction). HIGH: runtime 'A' key → mask 4 → left movement. |
| 0x0008 | 0x8000000D | **Right** (absolute). Forward/Back come from the character facing (char vt+0x19C). |
| 0x0010 | 0x80000010 | attack button #2 of 4 (MED) |
| 0x0020 | 0x80000011 | attack button #3 of 4 (MED) |
| 0x0040 | 0x8000000F | attack button #1 of 4 (MED). Commands 0x80000014..19 are exactly the six pairwise combos of 0xF/0x10/0x11/0x12 (checked with `0x00446500(a,b,0x5A)`). |
| 0x0080 | 0x80000012 | attack button #4 of 4 (MED) |
| 0x0100, 0x0400, 0x0800 | — | not mapped to fight commands; excluded from the practice input-history ring in 0x00425770 (menu/aux buttons, LOW) |
| 0x0200 | 0x8000003D | aux command (LOW; possibly start/pause) |
| 0x1000 | 0x8000001E / 0x80000032 | special command (LOW: flip-stance or throw candidate) |
| 0x2000 | 0x8000001D | special command (LOW: tag candidate) |
| 0x4000 | 0x8000001B / 0x80000033 | special command (LOW: throw or flip-stance candidate) |
| 0x8000 | 0x8000001C | **Block** (MED). Combined with Up/Down/Left/Right in 0x80000005–08 and with each attack in 0x80000034–37 (enhanced specials need block). |
| 0x10000 | 0x8000002B / 0x8000002C (=0x10002/0x10001) | modifier flag. Cleared `& 0xFFFEFFFF` by the online and AI writers. |

The mask is **post-button-config** (logical) space. The sampler ORs the per-device table masks at `0xE0E1F4 + slot*0x17C` (remappable). The default identity masks are passed separately as the 4th argument of the ring push. The FP/BP/FK/BK naming of the four attack bits is **unverified**. The next experiment should press each button once in Practice and read pad+0xC (see §7).

### 3.5 MKOnlineGamepadData (`this` of 0x00885F80; global address not yet resolved, so capture ECX at runtime)
- +0x18 sending-active; +0x1C startup counter; +0x20 **input delay (frames)** ("Decreasing input delay: %d"); +0x28; +0x12C/+0x134 timing.
- +0x138 + k*0x14: ring descriptors `{size, buf, read, write, full}`. Locals use k=0..3; **remote starts at k=4 (+0x188)**.
- Records are **11 bytes**: `{u32 frame, u8 flags(bit0, bits3..6 'unchanged', bit1/bit2 timing), u32 desyncHash, u16 buttons}`.
- +0x1D8 recv frame #; +0x224/+0x228 local Gamepad\*[]/count; +0x230/+0x234 remote Gamepad\*[]/count; +0x23C u16[] staged local buttons.
- Desync hash = `0x00B93540` OnlineDesync fenceposts (array at 0xFE2168, 0x1C-byte entries, initialized by 0x00B934A0). **This is a built-in game-state checksum. It is worth reusing for rollback desync detection** (open question: what the fenceposts cover).

---

## 4. Candidate dossiers

Format: **VA / RVA — proposed name** · callconv · params · fields · callers → callees · strings · confidence · evidence.

### Simulation-tick side

**T1. `0x008ACD90` / `0x4ACD90` — `FightUpdatePlayers()`**
- Call convention: cdecl `void(void)`, plain `RET`, ECX unused [REA asm].
- Called only from `0x0080F670` (call at `0x0080F70B`), and only when not paused and the game mode is a fight mode. The mode is checked through `FUN_00406760` / `FUN_0081C430` / `FUN_0081BB00(0x13)`, and `0x00887AF0` must accept mode ∈ {0,1,2,3,5,6,7,10,11,12,13,14}. It also requires GameInfo+0x10&1 (or mode 0xB) and !(+0x14&0x10).
- Iterates PlayerInfo[0..3] starting at `GameInfo+0x70 & 3` (rotating fairness order).
- Per player: if char+0x2E8&0x10, call PlayerInfo.vt+0xC (0x00895150, input lock); then if a pad exists (PlayerInfo+0xB40), `0x00448A00` (char+0x2FFC counter), `0x008AB6F0` (move-list match), else `0x008AAA70` (basic attacks). Toasty and other reward checks use `0xE89BE0`.
- Callees: 0x0080E540, 0x00895B70, 0x008959D0, 0x00451350, 0x004515B0, 0x00451B00, 0x00492590, 0x00448A00, 0x008AB6F0, 0x008AAA70, 0x0044DCE0.
- String: "perform_toasty_reward".
- **Confidence MED-HIGH.** This is the MK-specific per-frame "input → move" consumer. It doesn't cover animation, physics or script execution, which are T3/T4.

**T2. `0x0080F670` / `0x40F670` — `MK9GameEngine::Tick(float dt)` (engine vtable `0x00D60D30` slot +0xC0)**
- Call convention: thiscall, ECX = engine (`*(0xE5873C)`), 1 float on the stack, `RET 4` [REA asm].
- Order: replay/sync recorder BeginFrame → record host/game frame and RNG → pre-tick subsystems → `0x008ACD90` → `0x0045CAF0` → `0x00B927E0` → base `0x007F7F50(dt)` unless `0xE3B85C`.
- Callers: indirect from `0x004025A0`. Recorder tags: 0xFF0008/09/0A.
- **Confidence HIGH** that it is the per-frame gameplay tick wrapper. It was runtime-observed once per host frame earlier, but not yet specifically in combat.

**T3. `0x0045CAF0` / `0x5CAF0` — `MKScript_TickAllProcs()`**
- Call convention: cdecl void.
- Behaviour: calls `FUN_0042D950` on `0xE39FB0`, then for 21 descriptors at `0xE39E00..0xE39FA4` (stride 0x14) runs `FUN_0045A310(desc, 0x0045C700)`, the script VM executor.
- Fighter move/state scripts (MKScript VM) run here.
- **Confidence MED.** The signature has only 11 fixed bytes, so it is weak. Prefer hooking via the T2 call site.

**T4. `0x007F7F50` / `0x3F7F50` — `UGameEngine::Tick(float dt)` base**
- Call convention: thiscall, ECX = engine, `RET 4`.
- Behaviour: world/level travel countdown (WorldInfo +0x33C); `0xE89A24/0xE89A34` count-to-6 and `0xE89A30` alternation gate a call to engine vt+0xD4 = `0x007F4CE0` (world tick, `engine+0x328 → vt+0xC4`) with `0xE88580=1` around it. Viewport/UI (`0xE58738` vt+4) follows.
- Called from `0x0080F776` only.
- **Confidence MED** for "world/actor tick" (generic UE3). Fighter animation and physics probably live under 0x007F4CE0, but that wasn't traced in this pass.

**T5. `0x004025A0` / `0x25A0` — `AppTimedEngineFrame()`**
- Call convention: thiscall (ECX = app object; +0x0C double time, +0x14 frame counter, +0x18, +0x24), no stack args, `RET`.
- Behaviour: fixed-period pacing via 0x005EAF40; engine slots +0xBC/+0xC0 gated by `0xE53BFC`; `0x0066C650(dt)`; render and message pump.
- **Confidence HIGH** as the paced frame driver. It's too coarse for a rollback "advance one frame" because it includes presentation.

**T6. `0x00404D30` / `0x4D30` — host main loop**
- Not a hook target. Use the counter-increment site **`0x00404DFC`** (signature below) or the GameInfo+0x70 value.
- **Confidence HIGH.**

### Input side

**I1. Gamepad.cur (+0x0C) / PlayerInfo+0xB40** — the canonical processed per-frame input.
- Written by:
  - hardware sampler `0x00424A50`
  - online local `0x00885710`
  - online remote `0x00881360`
  - replay playback `0x00903700` (via the recorder)
  - AI `0x00889610`
- Read by every command query.
- **Confidence HIGH.** Five independent writer paths agree on the 12-byte cur/prev layout, and runtime showed P1 mask=4 while 'A' was held.

**I2. `0x0087F440` / `0x47F440` — `Gamepad_DispatchEdges(Gamepad*)`**
- Call convention: cdecl, 1 arg, `RET` (caller cleans).
- Behaviour: for each mapping bit (default table 0xDE1268, or per-device table when flags&0x80), compares cur (+0xC) and prev (+0x18), then calls `0x00425770` (press) or `0x004256A0` (release) with `GameInfo+0x70`.
- Callers: `0x00885F80` (online, every pad) and `0x00903700` (replay). The offline sampler inlines the same logic.
- Strings: two debug format strings at 0xD67FB0/0xD67FE4.
- **Confidence HIGH.** This is the engine's own "after overwriting cur, rebuild command-buffer events" routine, and the best primitive for injecting input.

**I3. `0x00881360` / `0x481360` — `MKOnlineGamepadData::ApplyRemoteInputs()`**
- Call convention: thiscall, 1 unused stack arg, `RET 4`.
- Behaviour: for each remote pad: `prev = cur` (12 bytes); `cur = (u16) ring[read].buttons & ~0x10000`; optionally merges the next record. Logs "READ REMOTE ReadIndex:%d local hash:%08X remote hash:%08X" and "P1 Buttons: %04X P2 Buttons: %04X".
- **Confidence HIGH. This is where remote input reaches the same consumer.**

**I4. `0x00885710` / `0x485710` — `MKOnlineGamepadData::UpdateLocalAndSend()`**
- Call convention: thiscall, 1 stack arg, `RET 4` (SEH frame plus a 0x1064 stack buffer).
- Behaviour:
  - stages local buttons `this+0x23C[i]` into ring[write] together with desync hash `0x00B93540()`
  - sets `prev = cur` and `cur = ring[read].buttons & ~0x10000` (**local input delayed by the delay ring**)
  - adapts input delay (flags bit1/bit2)
  - sends: "SEND frame:%d GamepadIndex:%d hash:%08X buttons: %04X"
- **Confidence HIGH.**

**I5. `0x00885F80` / `0x485F80` — `MKOnlineGamepadData::Tick()` → bool advance**
- Call convention: thiscall, 1 stack arg, `RET 4`.
- Called from `0x008868C0`, whose callers were not resolved; it is reached from the main loop via `0x0081C7C0`.
- Behaviour: startup delay ramp (+0x1C vs +0x20); `0x00881540` HaveInputForFrame. **If remote input is missing, it pauses the game** (`0x00407AB0(1)`, `0x004D3680(1)`) and returns 0. Otherwise it runs 0x00885710, 0x00881360, then `0x0087F440` on all pads (order depends on host/client), then 0x00881A70/0x00881C80/0x00881E10.
- **Confidence HIGH.** This is the place a rollback layer must replace or bypass, because it stalls the game.

**I6. `0x00903700` / `0x503700` — `ReplayPlayback::ApplyFrameInput()`** (vtable `0x00D6C7EC` slot 6)
- Call convention: virtual, ECX ignored, `RET`.
- Behaviour: `recorder->Serialize(0xFF0010, P1pad+0xC, 12)`, `(0xFF0011, P1pad+8, 4)`, same for P2 with 0xFF0012/13, then `0x0087F440(pad)`.
- The record twin `0x00903560` (vtable `0x00D6C7C8`) has identical serialization without edge dispatch. The recorder interface global is `0x00E3D6E4`, installed by `0x004D36F0` from `0x00886670` (online), `0x00903E40`, and `0x00904D00`.
- **Confidence HIGH** that MK9 already records and replays exactly {pad.cur 12 B + pad.flags 4 B} per player per frame. This is the native "input for frame N" format.

**I7. `0x00452550` / `0x52550` — `PlayerInfo::IsCommandTriggered(cmd)`** (vt+0x14)
- Call convention: thiscall, ECX = PlayerInfo, 1 arg, `RET 4`.
- Siblings, all thiscall `RET 4` and gated by +0x944:
  - `0x004468D0` WasPressed(cmd) → `0x00425510` (cur&m && !(prev&m))
  - `0x00446950` WasReleased → `0x004255A0`
  - `0x00446910` → `0x00425480` (held/timed; unverified)
  - `0x00446500(a,b,window)` combo
- **Confidence HIGH** for semantics. These are read-only consumer hooks, useful for logging which commands fire.

**I8. `0x00425F00` / `0x25F00` — `CommandToPadMask(cmd)`**
- Call convention: cdecl, 1 arg.
- Static switch giving the bit table in §3.4.
- **Confidence HIGH.**

**I9. `0x00424A50` / `0x24A50` — hardware sampler**
- Call convention: cdecl `int(void)` (returns 1).
- Six hardware slots, XInput / DirectInput / keyboard(type 7). Skips pads with flags&8.
- Already guarded and hooked in earlier work.
- **Confidence HIGH.**

**I10. `0x00889610` / `0x489610` — `AIDriver::SetPadButtons(a,b,c,frame)`**
- Call convention: thiscall, 4 args, `RET 0x10`.
- Behaviour: `pad.cur = (a|b|c) & ~0x10000`; pushes press events for b and c.
- This is the engine's own example of an external writer driving a flag-8 pad.
- **Confidence HIGH.**

**I11. `0x008AB6F0` / `0x8AAA70` — `Character::MatchMoveList()` / `Character::ProcessBasicAttack()`**
- Call convention: thiscall, ECX = character, `RET`.
- `0x008AB6F0`: move entries at char+0x7524, count at char+0x7AEC, type at +0x70 (0..10). Uses sequence matchers 0x008A9510 and siblings, then calls 0x0090A520 (rumble).
- char+0x7638 = PlayerInfo\*.
- **Confidence MED-HIGH** as the special-move consumer.

**I12. `0x0080E540` / `0x40E540` — `GameInfo::GetPlayerInfo(idx)`**
- Call convention: thiscall (ECX ignored), 1 arg, `RET 4`.
- Returns `0xE89D7C + idx*0xC8C`, or slot 0 if idx>3.
- Matches MK9Hook's `GetInfo`.
- **Confidence HIGH.**

Other mapped context:
- `0x00884770` StartSendingGamepadData: sets the seed through `0x005ED570`, resets GameInfo+0x70 to 0, and resets `0xE89B8C` tables.
- `0x00881190` GetSpectatorOutgoingDataSinceFrame.
- `0x0087FB70` StopSendingGamepadData.
- `0x00885480` RECV handler: "RECV GamepadIndex … WriteIndex … ReadIndex … DesyncHash", 4 args, `RET 0x10`.

---

## 5. Ranked tables

### (a) Simulation tick
| Rank | VA (RVA) | Callconv | Conf. | One-line evidence |
|---|---|---|---|---|
| 1 | `0x0080F670` (0x40F670) | thiscall(engine, float dt) RET 4 | HIGH | Engine slot +0xC0. Contains the recorder frame begin, fight update, script VM and world tick, in that order. |
| 2 | `0x008ACD90` (0x4ACD90) | cdecl void | MED-HIGH | Per-player input→move update. Runs only when unpaused and in a fight mode. Best "combat tick" probe. |
| 3 | `0x0045CAF0` (0x5CAF0) | cdecl void | MED | Runs all 21 MKScript proc lists through VM executor 0x0045C700. |
| 4 | `0x007F7F50` (0x3F7F50) | thiscall(engine, float) RET 4 | MED | UE3 base tick → world vt+0xD4 `0x007F4CE0`. Has a 6-frame/alternate gate (0xE89A24/30/34). |
| 5 | `0x004025A0` (0x25A0) | thiscall(app) RET | HIGH (driver) | Fixed-period QPC pacing; constant dt; includes render/pump. |
| — | counter `0xE89BE0` = GameInfo+0x70 | data | HIGH | Unpaused game-frame #. Inc at 0x00404E12. Timestamps every input ring entry. Reset at netplay start. |

### (b) Processed player input
| Rank | VA (RVA) | Callconv | Conf. | One-line evidence |
|---|---|---|---|---|
| 1 | Gamepad+0x0C via PlayerInfo[i]+0xB40 | data | HIGH | Sole per-frame input state. Sampler, online, replay and AI all write it; all queries read it. |
| 2 | `0x0087F440` (0x47F440) | cdecl(Gamepad*) | HIGH | Engine's own "cur/prev → press/release rings" rebuild. Used by both online and replay. |
| 3 | `0x00881360` (0x481360) | thiscall RET 4 | HIGH | Online remote-input apply: remote pad.cur = ring[read].buttons. |
| 4 | `0x00903700` (0x503700) | virtual void | HIGH | Replay: serializes P1/P2 pad.cur(12)+flags(4) per frame, then edges. |
| 5 | `0x00885F80` (0x485F80) | thiscall RET 4 | HIGH | Online orchestrator; stalls the game when input is missing. Must be bypassed for rollback. |
| 6 | `0x00452550` / `0x004468D0` | thiscall(PlayerInfo*, cmd) RET 4 | HIGH | Consumer queries (cmd → mask → cur/prev test). |
| 7 | `0x00424A50` (0x24A50) | cdecl int(void) | HIGH | Hardware → pad.cur; skips flag-8 pads. |
| 8 | `0x00889610` (0x489610) | thiscall(4 args) RET 0x10 | HIGH | AI writer pattern (flag-8 virtual pad). |

---

## 6. Signatures (verified unique in executable sections of this exact build)

Method (`scripts/sigmaker.mjs`):
- The pattern starts at the function entry.
- Every byte covered by a `.reloc` HIGHLOW entry (absolute VA) becomes `??`.
- The rel32 of direct `E8`/`E9` instructions becomes `??`. Instruction boundaries come from REA `procedure_assembly`.
- The pattern grows instruction by instruction until exactly one match exists across `.text` and that match is the entry.

Results are in `analysis/signatures.jsonl`. Short patterns are unique for this build but may need lengthening for other builds.

| VA | RVA | Len | Signature |
|---|---|---|---|
| `0x0080F670` | `0x40F670` | 15 | `55 8B EC 83 3D ?? ?? ?? ?? 00 56 8B F1 74 05` |
| `0x008ACD90` | `0x4ACD90` | 19 | `55 8B EC 83 EC 1C E8 ?? ?? ?? ?? 85 C0 0F 84 55 03 00 00` |
| `0x007F7F50` | `0x3F7F50` | 48 | `55 8B EC 6A FF 68 ?? ?? ?? ?? 64 A1 00 00 00 00 50 83 EC 30 53 56 57 A1 ?? ?? ?? ?? 33 C5 50 8D 45 F4 64 A3 00 00 00 00 8B F1 8B 8E 18 03 00 00` |
| `0x0045CAF0` | `0x5CAF0` | 31 | `56 B9 ?? ?? ?? ?? E8 ?? ?? ?? ?? BE ?? ?? ?? ?? 68 ?? ?? ?? ?? 8B CE E8 ?? ?? ?? ?? 83 C6 14` (only 11 fixed bytes) |
| `0x004025A0` | `0x25A0` | 35 | `53 8B DC 83 EC 08 83 E4 F0 83 C4 04 55 8B 6B 04 89 6C 24 04 8B EC 81 EC 88 00 00 00 83 3D ?? ?? ?? ?? 00` |
| `0x00424A50` | `0x24A50` | 14 | `55 8B EC A1 ?? ?? ?? ?? 83 EC 08 56 85 C0` |
| `0x0087F440` | `0x47F440` | 12 | `55 8B EC 53 56 8B 75 08 F6 46 08 80` |
| `0x00885710` | `0x485710` | 22 | `55 8B EC 6A FF 68 ?? ?? ?? ?? 64 A1 00 00 00 00 50 B8 64 10 00 00` |
| `0x00881360` | `0x481360` | 15 | `56 33 F6 39 B1 34 02 00 00 0F 8E C4 00 00 00` |
| `0x00885F80` | `0x485F80` | 13 | `55 8B EC 51 56 8B F1 83 7E 18 00 75 09` |
| `0x00903700` | `0x503700` | 56 | `55 8B EC 83 EC 10 56 6A 00 B9 ?? ?? ?? ?? E8 ?? ?? ?? ?? 8B B0 40 0B 00 00 6A 00 66 0F EF C0 C7 45 FC 00 00 00 00 66 0F D6 45 F0 C7 45 F8 00 00 00 00 6A 0C 85 F6 74 2B` |
| `0x00903560` | `0x503560` | 56 | same as above but ending `… 6A 0C 85 F6 74 18` |
| `0x00452550` | `0x52550` | 17 | `55 8B EC 53 56 57 8B F9 8B 4D 08 8D 81 FF FF FF 7F` |
| `0x00425F00` | `0x25F00` | 14 | `55 8B EC 8B 45 08 05 FD FF FF 7F 83 F8 3A` |
| `0x00889610` | `0x489610` | 13 | `55 8B EC 8B 41 3C 56 8B B0 40 0B 00 00` |
| `0x008AB6F0` | `0x4AB6F0` | 16 | `55 8B EC 83 EC 10 56 8B F1 57 8B BE 38 76 00 00` |
| `0x008AAA70` | `0x4AAA70` | 17 | `55 8B EC 83 EC 14 53 56 8B F1 57 8B BE 38 76 00 00` |
| `0x0080E540` | `0x40E540` | 16 | `55 8B EC 8B 45 08 83 F8 04 7C 09 B8 ?? ?? ?? ??` |
| `0x00425770` | `0x25770` | 17 | `55 8B EC 56 8B 75 08 8B 0E 85 C9 0F 84 B0 01 00 00` |
| `0x004256A0` | `0x256A0` | 17 | `55 8B EC 56 8B 75 08 8B 0E 85 C9 0F 84 A4 00 00 00` |
| `0x00425510` | `0x25510` | 12 | `55 8B EC 8B 45 08 56 57 85 C0 74 1C` |
| `0x00895150` | `0x495150` | 20 | `56 8B F1 83 BE 40 0B 00 00 00 C7 86 44 09 00 00 01 00 00 00` |
| `0x007F4CE0` | `0x3F4CE0` | 21 | `56 8B F1 8B 0D ?? ?? ?? ?? E8 ?? ?? ?? ?? F6 80 94 03 00 00 01` |
| site `0x00404DFC` (frame counters) | `0x4DFC` | 25 | `A1 ?? ?? ?? ?? FF 40 6C E8 ?? ?? ?? ?? 85 C0 75 08 A1 ?? ?? ?? ?? FF 40 70` (unique; `+0x70` inc at +0x16 = 0x00404E12) |

`0x004468D0` had no unique entry signature: it is a 21-instruction wrapper that is byte-identical in shape to 0x00446910/0x00446950. Hook `0x00425510` instead.

---

## 7. Recommended first runtime experiment (Practice, P1 human vs P2 dummy; Frida, guarded like the prior probes)

1. **Tick counter.** Put an `onEnter` hook on **`0x008ACD90`** (cdecl, no args) and log:
   - `u32[0xE89BE0]` (GameInfo+0x70)
   - `u32[0xE89BDC]` (GameInfo+0x6C)
   - `u32[0xE43054]` (pause flag)
   - the caller (expect return address `0x0080F710`)

   Expect exactly one call per host frame during the fight, with `+0x70` strictly +1 each call. Expect no calls while the pause menu is open (while +0x6C keeps increasing) and none in menus. Also hook `0x0080F670` entry and exit (thiscall; float at `[esp+4]`) to confirm 1:1 with 0x008ACD90 during combat.
2. **Per-player input read.** In the same `0x008ACD90` onEnter, for i∈{0,1}:
   - `pi = 0xE89D7C + i*0xC8C`
   - `pad = [pi+0xB40]`
   - log `[pad+4]` (index), `[pad+8]` (flags), `[pad+0xC]` (cur), `[pad+0x18]` (prev), `byte[pad+0x54..0x56]`, `[pi+0x944]` (lock)

   This is after sampler/online/replay/AI writes and before any consumption, so it is the canonical "input for frame N". Press, one at a time: Up, Down, Left, Right, each attack button, Block, and the remaining configured fight buttons. Verify the bit table in §3.4 and settle FP/BP/FK/BK and the throw/flip-stance/tag bits. Expect P2 pad index 0xF and flags&8 (AI).
3. **Override (second step, after 1–2 pass).** Reproduce the engine's own online pattern on P1:
   - set `[pad+8] |= 8`, so the sampler stops writing that pad and only does prev=cur
   - at `0x0080F670` onEnter (before 0x008ACD90): copy 12 bytes from +0xC to +0x18, write the scripted `cur`, then call `0x0087F440(pad)` (cdecl)
   - restore the flag on detach

   Success criterion: scripted Left reproduces the earlier mask-4 movement, and a scripted special-move sequence fires through `0x008AB6F0`.

---

## 8. Open questions
1. What do the OnlineDesync fenceposts (`0xFE2168`, 0x1C-byte entries, hashed by `0x00B93400`) cover? If they enumerate gameplay state regions, they are both a checksum and a partial state map for rollback. Find who fills the entries.
2. `0x007F7F50`: what is the `0xE89A24/0xE89A34` count-to-6 and `0xE89A30` alternation (with `0xE88580=1` during world tick)? Possibly a 50/60 Hz or half-rate render path. Check whether it is ever nonzero in versus.
3. Where do fighter animation, physics and hit detection run: under `0x007F4CE0` (world actor tick) or inside script procs? This determines which subsystems a resimulated frame must include.
4. Address of the MKOnlineGamepadData instance, and the caller chain `0x0081C7C0 → … → 0x008868C0`. Capture ECX at runtime.
5. Confirm the semantics of flags 0x8/0x40/0x80 and of PlayerInfo.vt+0x34 (`0x00893200` is not a function start in REA). Also confirm aux words pad+0x10/+0x14, which online does not transmit, so presumably they aren't gameplay-relevant.
6. Can the native recorder (`0x00E3D6E4` interface, tags 0xFF0004..0xFF0013) be driven to provide deterministic replay for offline validation? `0x00903E40` installs a playback recorder for slot <8 when `0xED680C != 0`.
7. `0x00425480` (used by 0x00446910): held versus timed semantics.

## 9. Files produced
- `docs/rea_pass1.md` (this document)
- `analysis/rea-doctor-claude_code.json`, `analysis/rea-setup-claude_code-dryrun.json`
- `analysis/rea-pass1/out/*.json` (123 raw REA MCP responses with evidence ids), `analysis/rea-pass1/status.jsonl`, `analysis/rea-pass1/ghidra-analysis.log`, `analysis/rea-pass1/tool-names.json`
- `analysis/ghidra-dumps/a1.txt a2.txt a3.txt` ([DG] direct-Ghidra dumps, not REA)
- `analysis/signatures.jsonl`, `analysis/cc-infer.json`
- Scripts:
  - `scripts/rea-inbox.mjs` (persistent REA MCP session)
  - `scripts/rea-queue.mjs`, `scripts/reashow.mjs`, `scripts/cc-infer.mjs`, `scripts/sigmaker.mjs`, `scripts/petool.mjs`
  - `scripts/run-ghidra-dump.ps1` + `scripts/ghidra/Mk9Pass1Dump.java`
  - `scripts/index-dossiers.mjs`, `scripts/showdump.mjs`
