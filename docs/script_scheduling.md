# MKScript scheduling, waits and wake conditions

Static analysis of `MKKE.exe` (base `0x400000`), 2026-10-15. Nothing here was run. **[H]** marks a hypothesis that needs a runtime check; everything else is read directly from the disassembly.
Motivating bug: after a k=10 rollback, executor A runs its first native (`0x8B6DA0`) at S+0 in the resimulation, but at S+3 in the original timeline.

## 1. Per-frame tick

- EngineTick `0x80F670` runs FightUpdatePlayers `0x8ACD90`, which is gated (see R4), and then always calls **`0x45CAF0`**. It calls it while paused too; the pause gate is checked per VM.
- `0x45CAF0` first runs `0x42D950` on root `0xE39FB0`, which purges dead nodes. Then, for groups g = 0..20 in that order, it runs `0x45A310(&desc[0xE39E00 + g*0x14], 0x45C700)`.
- **`0x45A310`** iterates the group list:
  - Node layout: `{vt, +4 obj, +8 id, +0xC next, +0x10 prev, +0x18 dead}`. The list head is at `desc+4`.
  - A stale handle is unlinked through `0x42D6E0`. A node with `+0x18 != 0` is skipped.
  - The object must type-check as MKScriptVM (`0xDE861C`). The callback is the executor `0x45C700`.
  - `next` is read after the callback. A VM inserted into a later group, or later in the same list, during the sweep therefore runs in the same frame. **[H]**: the insert position of `0x42DD10(0, node)` is not verified.
- Other callers of the executor `0x45C700`:
  - `0x45CC00` and `0x45CC40` "bind and run now". They force `+0x6C = 1` for the duration.
  - `0x89A75F` runs the character context VM (`char+0x2FA4`) synchronously from the character state switch at `0x89A700`.
  - The absolute "refs" to `0x45C700` at `0x998A20` and similar addresses are false positives.

### Executor `0x45C700` decision order (per visit)

1. **Pause gate.** Skip if `(IsPaused=[0xE43054] || vm+0x70) && !vm+0x6C`.
2. **Expiry.** If `vm+0x84 != 0 && vm+0x84 < GameFrame([0xDEF4B4]+0x70)`, call `vt+4(1)` (destroy).
3. **Nesting limit.** If depth `[0xDE8628] >= 3`, set state 0 (the VM dies on its next visit).
4. **Dead states.** State 0 or 3 → `vt+4(1)`.
5. **Waiting.** State 2: if `+0x40 > 0`, then `+0x40 -= +0x68`, clamped at 0. If `+0x40 <= 0`, set state 1. The wait is a **frame count**; no clock is read.
6. **Running.** State 1:
   - Context = handle `vm+0x20` (`0x407800`).
   - Binding refresh `0x45BD30` (see R2).
   - Dispatch loop `0x45C9D3..0x45CA08` (`call edi`). It stops on state 2/3, end of code, or `[0xE3B85C] != 0`, the level-travel abort set only by `0x495A10` (native 171).
7. **After each native.**
   - State 4 with `+0x40 == 0.0` (`[0xC5A52C]`) → state 1, and execution continues in the same visit with the new function.
   - State 4 with `+0x40 != 0` → state 2.

### Spawn / restart

- **Constructor.** `0x45D8C0` allocates; ctor `0x45CC80` sets:
  - `+0x44 = 0x15`;
  - `+0x68 = 1.0` (`[0xC5A698]`);
  - `+0x6C/+0x70/+0x74..+0x84 = 0`;
  - and calls `GameInfo vt+4(vm)`.
- **Bind.** `0x45CB20(vm, group, ctx, fn, ...)`:
  - reset `0x45C4C0`;
  - leave the old group (`0x45A380` with `0x458430`);
  - bind `0x45A570`, which sets `+0x40 = 0`, IP = fn+8 and state 1, and resolves a negative fn id through `ctx+0x94` imports;
  - then force **state 2**, set `+0x44 = group`, and append the VM to the group list (`0x42DFE0`).
- `0x45CB90` is the same operation on an existing VM (restart).
- **Consequence:** a new or restarted VM is in "state 2, timer 0". Its **first native runs at the first sweep visit after the spawn**. So "A first runs at S+0 instead of S+3" means one of these is true:
  - (a) A was spawned or restarted 3 frames earlier;
  - (b) A's `+0x40` differed;
  - (c) a gate (`+0x70`, pause, or node `+0x18`) differed.

## 2. Wait and wake natives (id = (slot − 0xDF0C40)/4)

| id | native | effect |
|---|---|---|
| 0 | `0x8B5D80` → `0x458320` | **wait(n)**. With policy `+0x48 == 0`, the VM is killed (state 3). Otherwise `+0x40 = (float)(u32)n`, and state 1 → 2. |
| 1 | `0x8B5DA0` → `0x45C590` | goto fn in the current context: reset, bind, state 4 |
| 2 | `0x8B5DC0` → `0x45C560` | goto (ctx, fn), state 4 |
| 3 | `0x8B5E00` → `0x458360` | **exit**, not a wait: state 1 → 3; otherwise `vt+4(1)`. *Corrects the earlier "wait/yield" label.* |
| 4 | `0x8B5E10` → `0x45C5C0` | goto fn **after n frames**: bind, `+0x40 = n`, state 4 → 2 |
| 6 | `0x8B5DE0` → `0x45C610` | run fn on a VM. If the target is the current VM → state 4. If the target is in state 1 or 2 it is **restarted** (state 1). External code uses the same function (25 callers: `0x8A0140` family, `0x89B3xx`, `0x44F141`, …). |
| 1544 | `0x8C2800` → `0x4583F0` | add n frames to **another** VM's `+0x40` |
| 4939 | `0x8E9A20` → `0x4583A0` | set another VM's `+0x70` pause gate. Native code also sets it at `0x44F69D`, `0x45558F` and `0x82F8D3`. |

- **Fields involved:** all timers and gates live in the VM object (`+0x2C`, `+0x40`, `+0x44`, `+0x68`, `+0x6C`, `+0x70`, `+0x84`), in the MK heap, which is restored.
- **Writers of `+0x68`:** only the ctor writes it inside `0x458000..0x45FFFF`; writers elsewhere are unknown. **[H]** The rate is always 1.0.
- **Expiry clock:** the expiry compares against GameFrame, which is restored.

## 3. Wake inputs that are NOT restored

- **R1. Real time reachable from scripts.** These values only ever increase, so a script deadline set before S is already met when the fast resimulation reaches it. This fits the "runs earlier" signature exactly. **[H]**: no trace yet shows A, or A's spawner, calling them.
  - **id 191 `0x8B7F50`** (slot `0xDF0F3C`): `QPC × GSecondsPerCycle[0xE42FF0]`, stored as a float in result `+0x1C`. This is appSeconds.
  - **Stopwatch natives, ids 782..789** (slots `0xDF1878..0xDF1894`):

    | id | native | operation |
    |---|---|---|
    | 782 | `0x8BBF90` | start, ms |
    | 783 | `0x8BBFB0` | start, µs |
    | 784 | `0x8BBFD0` | stop, ms |
    | 785 | `0x8BBFF0` | stop, µs |
    | 786 | `0x8BC010` | clear, µs |
    | 788 | `0x8BC030` | read, ms |
    | 789 | `0x8BC050` | read, µs |

  - **Stopwatch state is in .data, not in the snapshot:**
    - ms table A at `0xE3B7B0`, filled by `0x4D5DA0`: QPC / `[0xE3D750]`;
    - µs table B at `0xE3B6D8`, filled by `0x4D5DF0`: QPC / `[0xE3D758]`;
    - stride 0x18: `+0` running, `+4` start u64, `+0xC` elapsed u64.
    - **[H] bounds:** B has 9 slots, `0xE3B6D8..0xE3B7AF`. A runs to at most `0xE3B8A7`, because `0xE3B8A8` belongs to `0x495A10`.
- **R2. The HostFrame binding cache is on the program context, not on the VM.** `0x45BD30` is called with ECX = context `[0xE39DF8]`. It skips the refresh when `ctx+0x74/+0x78` (the last VM handle) equals this VM and `ctx+0x80 == HostFrame (GameInfo+0x6C)`. HostFrame is kept live, so the resimulation refreshes where the original skipped, or the reverse. This changes binding pointers (`ctx+0xA0` from `vm+0x9C`), not timers (low risk).
- **R3. Pause flag `0xE43054`** is a global and is not restored. It must be 0 for the whole resimulation, otherwise VMs without `+0x6C` freeze while the sweep still runs.
- **R4. FightUpdatePlayers gate** (UI/flow manager `[0xE924F0]`, outside the snapshot):
  - The gate is `0x406760` (`0x81BAE0() == 1`), then `!0x81C430()` (`mgr→0x93C7D0()+0x78`, `0x93CA10`, `mgr+0x20`), then `0x81BB00(0x13)`.
  - `0x45CAF0` runs even when the fight update is skipped. Any VM spawned by fighter code shifts if this gate fires on different frames in the resimulation. **[H]**
- **R5. WorldInfo natives.** Natives 115 and 116 (`0x80F130`/`0x80F160`) write WorldInfo `+0x24C` and `+0x3FC/+0x400`; **[H]** these are time dilation / slomo. Ids 655, 1038, 1435, 1648, 2462, 2464, 2641, 3291, 3756, 3759 and 3766 reach the WorldInfo getter `0x6D5430` within two calls, but were not decoded.
- **R6. Async loading.** A shallow scan (depth 2) of all 5,202 natives found no reference to the async package loader `0x62CA40` or the async task loop `0x661A90`. There is no evidence yet of a script "wait for load".
- **Other timer natives not yet decoded.** The `timeGetTime` / QPC hits for ids 478, 491, 492, 1218, 1277, 1440, 5029, 5030 and 5042 come from the scanner's 0x180-byte window running past tiny natives into their neighbours. Decoded examples (`0x8BFFB0`, `0x8C07E0`, `0x8EAC30`) do not read time themselves. Treat them as unresolved, not as clean.

## 4. `0x8B6DA0` → `0x493CF0` → `0x493590` (native id 22, slot `0xDF0C98`)

- **Call:** `0x8B6DA0` reads arg0 from `[exec+0x3C]+4` and stores the result in `exec+0x1C`.
- **`0x493CF0(flag)` returns 0 when:**
  - there is no current executor; or
  - `[0xE39DFC] == 0` and `flag != 0`.
- **`[0xE39DFC]`** is set at each executor visit by `0x42DA60(&vm+0x54)`. It is the first live object in the VM's **owner list** `vm+0x54` (`+0x58` head, `+0x5C` tail); dead owner nodes are pruned as a side effect.
- **Otherwise** it takes the **tail node `vm+0x5C`** and calls `0x493590`. That validates the `{obj +4, id +8}` handle (not null, not `0xDEADBEEF`, id == `obj+4`) and type-checks it against **MKBaseObj** (`0xDE8238`).
- **Result:** the native is **"get self/owner object"**. It returns the MK object that owns the process (for a fighter script, the character or its spawned object), or null.
- **Why it is the first native:** it is a typical prologue of a freshly bound function. That makes it a marker of (re)start time, not the cause.

## 4b. Still the dominant class (2026-10-26)

- In soaks 11101–11312, most script-class failing loads have native `0x8B6DA0` at the first differing call. Its results are MK objects 0x200 apart, the MKSkeletalObj owners that ATTACHFUTURE names. One pass spawns the attached prop, the other doesn't.
  - Variant A: the replay's first executor has its locals outside the MK image (11202, 11205 twice with the same block `0E664110`, 11307).
  - Variant B: an executor with null locals makes an extra call at the end of the frame (11101, 11204 twice, 11308).
- **VM event trace (05:29 build, `VMTRACE`, proposal 1 implemented).** The entries of ctor `0x45CC80` (2 args), bind `0x45CB20` (cdecl, first 3 args), restart `0x45CB90` (5 args, first 3), RunScript `0x45C610` (2), wait `0x458320` (1), add-wait `0x4583F0` (1) and gate `0x4583A0` (1) are hooked.
  - Each records `{site, ecx, args, return address, current executor [0xE39DF4]}` per frame. The replay is compared with the original pass, and the first differing event is logged, at most 200 per process.
  - Only the real arguments are recorded: reading three stack dwords for every site made every `wait` differ through the caller's stack.
  - The first 5 minutes logged 2 differences and no desync. One was a RunScript called from engine code (return `0x00450A40`, no executor) in the original pass only.

- **Traced soak 11402–11413 (05:29 build): 10,951 loads, 7 independent real failing loads, 0 crashes.** For 5 of them (11403 S=1880, 11404 S=925 and 1049, 11405 S=1411, 11411 S=1554) the first VMTRACE difference is a **VM constructor present in only one pass**, called from native code (executor 0).
  - Most are `ctor args (1, 0xCE)` from the allocator `0x45D8C0` (ctor return `0x45D9C8`); one is `args (0, 0x48)` from `0x0089F5C3`.
  - On the next frame the new VM is bound by `runscript` (return `0x0089ED78`, executed by the new VM), and its first native is `0x8B6DA0`. That is the known call-#0 / spawned-prop signature, so the native difference is a consequence of the spawn.
  - **The spawn decision is the cause.** `0x45D8C0` has five callers: the generic wrappers `0x4925BE`/`0x49265E` (they forward their own parameters; these are the `(1, 0xCE)` ones), `0x893C52` (2, 0x98), `0x89A4A2` (2, 0x16) and the native `0x8B67DE`.
  - The 08:47 build records the three return addresses above each ctor (`up …`), to name the spawner. Soak 11501–11512 runs on it.
  - 11410 S=1880 is different: a condition native `0x8CE360` returned 0 in one pass and 1 in the other, and the script then ran `runscript` on another VM.

- **Partial traced soak 11501–11504 (08:47 build, stopped when the session ended): 3,525 loads, 0 failing loads, 0 crashes.** Two spawn-only-in-one-pass ctors, both without a desync, named spawners:
  - `ctor (0, 0x48)` from `0x0089F5C3`, up `0x0089F825` ← `0x004079BD` ← `0x0080F794`.
    - EngineTick, when not paused, sweeps the tick list `0xE0B504` (restored, Tier A `mkobj_ticklist`) with callback `0x406C30` = `vt+0x18`.
    - For this MK object class that is `0x008A7300`, which calls `0x0089F7F0` at `0x008A7491`. `0x0089F7F0` spawns the object's pending script (`+0x1D0`, owner handle `+0x1C8/+0x1CC`) through `0x0089F570`.
    - Gate before the spawn: `+0x1BC`, actor `+0x8` with Base (`actor+0x60`) null, `+0x14 == 0`, then `[+0x1D8]+0xC`, then `Cast<[0xE58C24]>([[+0x1D8]+0x30]+0x38)` → `Cast<[0xE59098]>` → `+0x288` → **`+0x14C != 0`**.
    - Those are UE3 objects; this flag is a candidate for state the load keeps live.
  - `ctor (2, 8)` with up `0x00887F17` (not decoded yet).
  - **Contact probe (10:02 build):** `0x008A7420` (`mov eax,[eax+0x288]`, 6 bytes) records `{object, cast result, +0x288, [+0x14C]}` as VMTRACE site `contact`, so the first differing event shows whether the flag differs between the passes. The native trace also resolves the object operand of `0x8CE360` (the 11410 condition native, `vt+0x1B0`). Soak 11601–11612 is queued for a free PC.
- **Soak 11601–11612 (10:02 build): 13,452 loads, 0 crashes.**
  - **11606 S=902 and S=1330 confirm the mechanism.** On the first replayed frame the replay logged `contact` for MK object `11292600` / `11292800` (`+0x288` = `4709D040`, flag `+0x14C` = 1); the original pass had no contact event that frame.
  - The new VM's `0x8B6DA0` then returned that object, and the RNG diverged.
  - The flag was set in the replay; what differed is that the original never reached the check. **An earlier gate of `0x008A7300` differed right after the load.**
  - 11611 is a separate burst of small P1/P2 position drifts (0.001–0.17).
  - **Tick-gate probe (13:10 build, site `tickgate` at `0x008A73B7`):** per object per tick it records `+0x1BC`, actor `+0x8`, actor Base `+0x60`, `+0x14`, `[+0x1D8]+0xC` and actor `+0x80`. Soak 11701–11712 runs on it.
  - The first version compared the actor pointer, and every re-spawned actor differed (new address in the replay). The 13:15 build compares only actor/Base presence and logs the raw pointers.
- **Soak 11701–11712 (13:15 build): 12,707 loads, 1 real failing load (11706 S=1181, P1/P2 0.001–0.07 position drift), 0 crashes.** No spawn-class failure occurred, so the gate probe had nothing to name. Soak 11801–11812 follows.
- **Cause found (soak 11801–11804, 13:15 build).** At 11804 S=2115 the first differing VMTRACE event is the tick gate of MK object `11290600`: `[+0x1D8]+0xC` was 0 in the original and 1 in the replay; actor, Base, `+0x1BC` and `+0x14` were equal. The replay then spawned the VM, whose `0x8B6DA0` returned that object, and the RNG diverged.
  - `+0x1D8` is a 0x4C-byte GMalloc helper with vtable `0x00C6A5B4`, allocated at `0x0089F2CE` when the object is set up (also copied at `0x008A0CDA`). It is outside the MK image and in no span, so its flags kept the abandoned future's values.
  - **Fix (17:22 build, `mkhelper`, on by default, `abtest mkhelper`).** Every save adds a span for each listed MK object's helper (vtable checked, live block of at least 0x4C bytes), using the pointer-safe merge; identity is the owner.
  - At load it is restored only while the owner still points at the same live helper. `FXACTORS` lines now count saved/restored helpers. Soak 11901–11912 runs on it.
  - The first 4 sessions of that soak (11801–11804) had 3 failing loads, all this case.
- **Soak 11901–11912 on the 17:22 build (mkhelper): 13,343 loads, 3 real failing loads, 0 crashes.** About 13,000 helper spans were saved and about 1,700 restored, with no crash. The `rec+C` case did not recur; previous soaks had 7 per ~12–13k.
  - 11901 S=1909 and 11912 S=2095 are the hit-test group. The original spawned a `(1, 0xCE)` VM from inside the MK object hit test `0x008A46E0` (ctor up `0x008A4C16` ← `0x008A73AE` ← `0x004332D4`); the replay did not.
  - **12808 S=1199 (2026-10-29, 14:32 build) names the object of that group.** The MKCollisionObj made by script
    native `0x008D94F0` (owner from the current executor through `0x00458930`) → `0x008A4C70` → ctor `0x008A45C0`
    `(owner, [owner+0x7638], arg8, argC)` exists in both passes, but on the first replayed frame it stands next to P1
    (y -321.7) instead of next to P2 (y -263.8). The replay's hit test then hits and spawns the `(1, 0xCE)` VM. The
    VMTRACE site `collctor` (18:02 build) records the ctor's owner and script arguments per pass.
  - **Soak 12901-12912 (collctor on): 12904 S=2289 refutes the owner idea for that case.** Both passes built the
    object on the first replayed frame with identical owner and arguments, but at different addresses (original
    11A50400, replay 11A52200): the allocator (0x0042D5F0 -> pool [0xED5414] for 0x1DC bytes -> 0x1000 pool ->
    master heap) returned another block after the load. The 01:07 build logs both pools' free-list heads and the
    master heap's used bytes at the ctor (soak 13001-13012). The other four real failures of that soak (12901,
    12909 x3) are the hit-test group again, without a differing collctor event before the first difference.
  - 11909 S=840: at the first replayed frame's FightUpdate entry, P2's component `+0xA8` was `0x40600000` (3.5) in the original and `0x424EDD74` (51.7) in the replay, and `+0x5E8` was 3.5 vs 0; then P2 z went 0 → 46.8. Suspected: floats that pass the pointer test (0x40600000 is a region-aligned address) and are kept live by the merge. The 20:23 build logs `COMPKEEP` (fighter-component dwords kept live with saved ≠ live) to check.
- **COMPKEEP confirmed it (soak 12101–12109, 8,842 loads, 0 crashes).** The merge keeps fighter-component floats live at `+0xA0`, `+0xA4`, `+0xE0`, `+0xE4`, `+0x58`, `+0x5E4`, `+0x610` and `+0x3B4..+0x3BC`.
  - Example: `+0xE4` saved `446737A8`, live `446737A5`; an unaligned "pointer" is a float.
  - These are values from 0x40000000 upward that the pointer test accepts, because a real object happens to start at that address.
  - Next: restore such dwords by value when the saved or live value is unaligned or not an object start, or list the component's float offsets.
- **745 COMPKEEP lines (soaks 12101 and 12201, plus netplay runs) split the offsets into two groups:**
  - Float vectors: `+0xA0..+0xA8`, `+0xE0..+0xE8`, `+0x5E0..+0x5E8` and `+0x60C..+0x614`. Their values are unaligned or negative, e.g. `428DB6E7`, `B2A44588`, `4279F84A`.
  - Real pointers or arrays, which stay live:
    - `+0x58`: 0x40-aligned heap blocks.
    - `+0x3A8..+0x3C4`: TArrays such as `{data, 1, 0x21}` saved against `{0, 0, 0}` live. Restoring a freed data pointer would be unsafe.
    - The bone-matrix pointers.
  - 2026-10-27 13:56 build: `gCompFloats` (`abtest compfloats`, on by default, part of the netplay switch fingerprint) removes the float-vector offsets from the keep list, so they are restored by value. The log says `COMPFLOATS` every 500 loads that used it.
  - Soak 12301 compared with 12201:

    | Soak | Loads | Crashes | Real failing stops | End-of-fight artifact errors |
    |---|---|---|---|---|
    | 12201, before | 13,307 | 0 | 4 stops, 15 errors | 40 |
    | 12301, compfloats | 11,542 | 0 | 0 | 41 |

    - A stop is the "synctest stopped" line that reports the errors of one fight.
    - Artifact errors are the ones in stops with as many failed saves as sync errors: the last frames of a fight that cannot be saved.
    - Netplay checks passed on this build (zip 20261027-1436).
    - One soak is not proof; the remaining known classes (hit-test VM spawn, Touching list, engine-tick drift) may simply not have occurred.
- **Failing loads per soak since the attachfuture fix (01:52 build onward):**

  | Soak | Failing loads / loads |
  |---|---|
  | 11301 | 7 / 12,196 |
  | 11402 | 7 / 10,951 |
  | 11501 (partial) | 0 / 3,525 |
  | 11601 | 7 / 13,452, with a 5-load drift burst in one session |
  | 11701 | 1 / 12,707 |

- **The non-script failing loads of these soaks:**
  - **Touching / actor `+0x1B0` kept live.** Example: 11706 S=1181, P2 actor `+0x1B4` 4 at the save vs 5 live, different at FightUpdate entry of the first replayed frame, then both fighters' x/y drift by up to 0.07. The count differed at 149 loads across the four soaks; only this one failed. Not reconciled: Touching is kept on both actors and changes fire Touch/UnTouch events, so a one-sided restore is riskier than this 1-in-150 effect.
  - **Engine-tick float drift.** Example: 11611's 5-load burst; S=1927 is identical through FightUpdate exit and differs only at EngineTick exit by one ulp of P2's component position (`+F8`) and a different queued anim-object count (comp `+0x3AC`). This is the known drift class, produced in the engine's animation/physics pass after the fighter update; no new lead.

## 5. Proposals (in order)

1. **Name the spawner of A.**
   - Log `{GameFrame, vm+4 id, vm+0x50 key, vm+0x44, fn, return address}` at:
     - `0x45CB20`, `0x45CB90` and `0x45A570` (bind/restart);
     - `0x45C610` (RunScript);
     - `0x458320` and `0x4583F0` (wait n / add n);
     - `0x4583A0` (gate).
   - Diff the original against the resimulation. The first row that differs for A names the input.
   - Also log `+0x2C/+0x40/+0x70` of A at save and at load.
2. **Test R1 cheaply.** Count calls to slots `0xDF0F3C` and `0xDF1878..0xDF1894` during a fight.
   - If there are any, make them deterministic: redirect the slots (or `0x4D5DA0`/`0x4D5DF0` plus the native `0x8B7F50`) to a **virtual clock** = `GameFrame × (1/60)`, scaled to ms or µs.
   - Also add the stopwatch tables `0xE3B6D8..0xE3B8A7` to Tier A.
   - Rolling the real clock back is not an option.
3. **R2.** After load, zero `ctx+0x80` of every live program, so the first visit always refreshes. Better still, restore HostFrame together with the snapshot and advance it per resimulated frame.
4. **R3/R4.** At save and load, assert that `[0xE43054] == 0`, `[0xE3B85C] == 0` and `[0xDE8628] == -1`.
   - Record the FightUpdatePlayers gate result per frame (the branch at `0x80F6E2..0x80F70B`).
   - During the resimulation, replay the recorded result instead of asking the UI/flow manager.
5. **Verify invariants.** At save, check that every live VM has `+0x68 == 1.0`. Confirm that list insertion through `0x42DD10(0, …)` appends at the tail (this matters for same-frame runs of new VMs).
