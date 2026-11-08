# In-game status display: design (to-do item 4)

Written 2026-10-29. This is a design only: nothing is built or run yet, and no project file was changed.

Goal: one small line of text on screen, for example
`ROLLBACK  ping 84 ms  delay 2  rolled back: last 3, max 5 (1 s), 12/s  behind 1`, plus a `LOCKSTEP` / `OFF` /
`WAITING FOR PEER` / `STOPPED: <reason>` state.

Labels used below:

- **[V]**: I verified it statically in this pass. The method is given: the disassembly
  `D:\Projects\MK9\analysis\mkke_text.asm`, or bytes read from `analysis\artifacts\DiscContentPC\MKKE.exe`, whose
  SHA-256 is `D1C71E08…37D0E6`, the pinned build.
- **[D]**: documented in another project doc as a runtime observation. I did not re-verify it.
- **[A]**: assumed. It must be checked at runtime (see Testing).

Note on tools: `node` is not installed on this PC, so the `tools\*.mjs` helpers could not run. I did the same work
with read-only PowerShell scripts in the session scratchpad: a PE import/delay-import parser and a string finder
with imm32 xrefs. Their output matches `docs\build_identity.md` (delay-loaded d3d9.dll / d3dx9_43.dll).

---

## 1. Recommendation

**Draw from MKKE's own Direct3D 9 code path, on the rendering thread. Do it through one 5-byte CodePatch in
`RHIEndDrawingViewport` (0x00666F20), just before its `EndScene`.**

- Use a built-in 8x8 bitmap font, drawn as untextured quads with one `DrawPrimitiveUP`.
- Create no D3D resources: no ID3DXFont, no textures, no vertex buffers.
- The text comes from a double-buffered stats struct that the game thread publishes. It publishes only at the
  live P2P frame boundary, after any resimulation has finished.

Why this option is the least risky:

1. **It runs off the simulation.** Present runs on UE3's rendering thread (verified below). The draw touches only
   device state, which it saves and restores, and the back buffer's pixels. It reads only DLL globals. It never
   reads or writes game memory, never calls game code, and never uses GMalloc. Formatting uses stack buffers and
   the vertices go in a static DLL array.
2. **There is nothing to lose on a device reset.** It creates no D3DPOOL_DEFAULT resources and keeps no state
   block between frames. `Reset` and `ResetEx` need no handling, and nothing D3D-related can end up near the
   snapshot.
3. **It uses the existing, proven hook mechanism.** `CodePatch::Write` checks the original bytes, suspends every
   other thread (including the rendering thread), checks their EIPs and keeps the original bytes.
   `CodePatch::RestoreAll()` undoes the patch at unload. It needs no d3d9.dll code or vtable patching and no
   MinHook, so it cannot clash with the Steam overlay or MK9Hook's kiero hooks, which patch d3d9.dll.
4. **The device state at that point is already clean**, which keeps the draw simple. MKKE has just bound the back
   buffer as render target 0 and unbound depth, shaders, streams, index buffer and all 16 textures (see 2.3).
5. **It runs only for frames that are presented.** The other branch of the function ends the scene without
   presenting (2.3).

Fallbacks:

- **(c) Window title:** in windowed mode only, `SetWindowTextW` from the game thread at the boundary, every
  30 frames and only when the text changes.
- **(a') Shadow vtable:** if the MKKE code site ever proves unsuitable, replace the device object's vtable pointer
  with a copy whose EndScene slot (+0xA8) is ours. Section 3 has the details.

---

## 2. Findings

### 2.1 Does the DLL already touch the render path? (question 2)

**No** [V]. A grep of `src\` for `d3d|Present|EndScene|Device|IDirect3D|D3DX|DrawText|Canvas|PostRender|SetWindowText`
finds only:

- `HandleGuard`: the HUD *handle lookup* guard (0x00B8FFD0 / 0x00B91480 / 0x00B90070). This is game-side HUD
  bookkeeping, not drawing.
- Comments.

`docs\architecture.md` lists `debug/Overlay (D3D9 EndScene/Present)` as planned and not written. The DLL has no
device pointer today. No hook code exists in the render path.

### 2.2 How MKKE creates and holds the device [V]

- **MKKE.exe has no static d3d9 import.** The import table has only `DINPUT8!DirectInput8Create` and
  `XINPUT1_3 #2/#3` of the input/graphics DLLs. Direct3D comes through the **delay-import directory** (data
  directory 13, RVA 0x9DCFFC):
  - `d3d9.dll`: `Direct3DCreate9Ex`. The IAT slot is `0x00E0A3F4`, the delay thunk is `0x00996AF3` and the call
    stub is `jmp [0xE0A3F4]` at `0x00996B0E`.
  - `d3dx9_43.dll`: `D3DXCompileShader`, `D3DXCreateTextureFromFileInMemoryEx`, `D3DXCheckVersion`. The slots are
    `0xE0A3FC/0xE0A400/0xE0A404`.
  - So hooking an import (IAT) is not possible for Present/EndScene. Those are COM vtable calls anyway.
- **IDirect3D9Ex\*** is at the global `0x00E84530`. At `0x0066A96E` the code calls
  `call 0x996B0E`, which is `Direct3DCreate9Ex(0x20 /*D3D_SDK_VERSION*/, GetInitReference(&[0xE84530]))`.
- **IDirect3DDevice9Ex\*** is at the global **`0x00E84534`**.
  - The two `CreateDeviceEx` calls are at `0x0066B436` and `0x0066B4D2`: `call [vt+0x50]` on `[0xE84530]`. The
    second one is the retry after `D3DERR_DEVICELOST` 0x88760868.
  - Their arguments: `ppDevice` is `GetInitReference(&[0xE84534])` (`0x667230` with `ecx = 0xE84534`), the type is
    `D3DDEVTYPE_HAL` (1), and the behaviour flags are **0x46**.
  - 0x46 = `HARDWARE_VERTEXPROCESSING | MULTITHREADED | FPU_PRESERVE`. There is **no `PUREDEVICE`**, so the
    `Get*` state calls work. FPU_PRESERVE means D3D does not touch the x87 control word.
  - All 226 references to `0xE84534` in .text are `mov reg,[0xE84534]` followed by a call through that object's
    vtable.
- **Reset:** `0x0066AC5E` calls `[[0xE84534]]+0x40` (`Reset`) after `TestCooperativeLevel` returns
  `D3DERR_DEVICENOTRESET` (0x88760869).
- **Rendering thread:** `0x00404C4E..0x00404C73` reads `ParseParam("ONETHREAD")` (string `0x00C5A944`) and
  `GNumHardwareThreads` `[0xDE9868]`. It then sets `GUseThreadedRendering` `[0xE82080]` and calls
  **`StartRenderingThread` 0x0063CD20**. That function sets `GIsThreadedRendering` `[0xE8207C] = 1` and creates
  the thread named `"RenderingThread"` (string `0x00D4A7CC`, pushed at `0x0063CD63`).
  - So on any multi-core PC, all RHI work, including Present, runs on the rendering thread, not the game thread.
  - With `-ONETHREAD` it runs on the game thread inside `FEngineLoop::Tick`. See the resimulation note in 2.4.

### 2.3 Present path and the chosen hook site [V]

`RHIEndDrawingViewport(Viewport, bPresent, bLockToVsync)` = **0x00666F20**. It has 5 callers, all E8 calls:
`0x00636017`, `0x0074B75D`, `0x0074B77F`, `0x00756D51`, `0x00757C44`.

The matching `RHIBeginDrawingViewport` is `0x00666DE0`, with BeginScene `[vt+0xA4]` at `0x00666E53`/`0x00666EA9`.
Its callers are `0x00635F14` and `0x0075364F`.

Inside 0x00666F20, the byte `[0xE84705]` selects one of two branches.

- **Branch 1** (flag set, `0x00666F50..0x00666FFA`): rebinds RT0, unbinds and ends the scene (`EndScene` at
  `0x00666FFA`), and returns **without** Present.
- **Branch 2** (`0x00667002..`), the presenting path, in order:
  1. `SetRenderTarget(0, [0xE84538])` (vt+0x94), `SetRenderTarget(1, NULL)`, `SetDepthStencilSurface(NULL)`
     (vt+0x9C).
  2. `0x666A80` and the frame index `0xE845B4`.
  3. `SetTexture(i, NULL)` for i in 0..15 (vt+0x104), `SetVertexShader(NULL)` (vt+0x170), `SetStreamSource(i, NULL)`
     for i in 0..15 (vt+0x190), `SetIndices(NULL)` (vt+0x1A0), `SetPixelShader(NULL)` (vt+0x1AC).
  4. Calls `0x569EA0` and `0x569ED0`.
  5. **`0x00667106: A1 34 45 E8 00  mov eax,[0xE84534]`**
  6. `0x0066710B: 8B 08 8B 91 A8 00 00 00 50 FF D2`: **EndScene** (vt+0xA8).
  7. Frame-pacing counter `0xE845F4`, then `if (!bPresent) return`.
  8. **Present** (vt+0x44): `0x00667163` (`Present(NULL,NULL,NULL,NULL)`) or `0x0066718E` (with a rect). A
     `D3DERR_DEVICELOST` result sets `[0xE84480] = 1`.

The vtable offsets match the IDirect3DDevice9 layout throughout: 0x40 Reset, 0x44 Present, 0x94 SetRenderTarget,
0x9C SetDepthStencilSurface, 0xA4 BeginScene, 0xA8 EndScene, 0x104 SetTexture, 0x114 SetSamplerState,
0x170 SetVertexShader, 0x190 SetStreamSource, 0x1A0 SetIndices, 0x1AC SetPixelShader. This identifies the function
with good confidence.

**Hook site: 0x00667106, the 5 bytes `A1 34 45 E8 00`.** They are one whole instruction, so an exact 5-byte
`E8 rel32` replacement fits.

- No branch in .text targets `0x667106..0x667116` (grep of `j* 0x66710[6-a]` / `0x66711[0-6]`).
- MK9Hook's documented sites do not include this function (`docs\mk9hook_findings.md`).

The stub (naked, in the DLL):

```
MK9R_OverlayStub:            ; reached by E8 from 0x667106; [esp] = 0x0066710B
    pushad
    pushfd
    mov   eax, [0x00E84534]  ; the device; the stub re-reads it, no AddRef needed (this thread owns the use)
    push  eax
    call  Overlay_DrawRT     ; __stdcall void(IDirect3DDevice9*): null/sanity checks inside
    popfd
    popad
    mov   eax, [0x00E84534]  ; the replaced instruction
    ret                      ; back to 0x0066710B (EndScene)
```

Optionally, before writing, also `memcmp` the following 11 bytes (`8B 08 8B 91 A8 00 00 00 50 FF D2`) so the patch
refuses to go in unless the EndScene call follows.

At this point:

- The scene is open and RT0 is `[0xE84538]`, which is UE3's back buffer [A: UE3 source convention; checked at
  runtime in step 3 of the plan].
- Because `SetRenderTarget(0, …)` was just called, D3D9 has **reset the viewport to the full render-target size**.
- Depth, shaders, streams, indices and textures are unbound. The overlay only has to set fixed-function render
  states and an FVF, and put them back.

### 2.4 Does the overlay draw during resimulated frames?

- Resimulation (`Rollback::ResimFrame`, RollbackManager.cpp:1281) calls only engine vt+0xBC (`0x0080DD40`) and
  vt+0xC0 (`0x0080F670`). The live main loop calls the same pair from `FEngineLoop::Tick` 0x004025A0
  (`0x004028B8`, `0x004028CF`). After that, the frame-rate limiter and the render fence (`0x631F00`, `0x63CD80`,
  `0x6360E0`) run outside the pair [V].
- `docs\determinism.md:442` says "The renderer does not run between resimulated frames" [D].
- `docs\determinism.md:494` says that rendering happens inside the engine tick [D]. That wording is ambiguous, so
  whether a resimulated vt+0xC0 enqueues a viewport draw is **not verified** [A]. The plan logs a counter for it.
- **The design does not depend on it.** The text shows a snapshot that only the live path publishes, after
  resimulation has finished (2.5). Even if a resimulated frame did enqueue a draw, the overlay would show the last
  live frame's values and touch no state. The overlay also never calls back into the game.

### 2.5 Data sources (question 3) [V, RollbackManager.cpp]

| Display | Source | Notes |
|---|---|---|
| Rollback on/off/mode | `gGgpo`, `gGgpoIsP2P`, `gGgpoRunning` (`NetplayRunning()` at :2204), `gLockstep` (`Lockstep()` :2753), `gNetplayArmed` / `gNetplayWaitRunning` (`NetplayArmed()`, `NetplayStarting()`), `Online::Engaged()` (OnlineMode.h) | Modes: ROLLBACK (P2P, not lockstep), LOCKSTEP (safe mode), WAITING FOR PEER (armed/starting), SYNCTEST (`gGgpo && !gGgpoIsP2P`), OFF. |
| Ping | `ggpo_get_network_stats(gGgpo, gGgpoPlayer[2 - gP2P.player], &ns)` → `ns.network.ping` (round trip, ms) | Already called for NETSTATS in `CsPump` (:1846, every 600 frames). In the vendored GGPO it is a **pure read**: `Peer2PeerBackend::GetNetworkStats` → `UdpProtocol::GetNetworkStats` only copies `_round_trip_time`, `_pending_output.size()`, `_kbps_sent`, `_remote/_local_frame_advantage` (`build\ggpo-src\lib\ggpo\network\udp_proto.cpp:666`) [V]. Calling it every 30 live frames is safe. It must stay on the game thread: GGPO is not thread-safe. |
| Frames behind | `ns.timesync.local_frames_behind` / `remote_frames_behind` | Same call. |
| Frames rolled back | `gGgpoLoads` (loads this session), `gGgpoReplayed` (replayed frames this session) (:1581) | Per-rollback depth is **new**. In `GgpoBoundaryP2P`, where `gGgpoLoadedS != 0xFFFFFFFF` after `ggpo_idle` (:2103), the depth is `gGgpoLiveF - gGgpoLoadedS`. Keep the last value, the maximum over the last 60 live frames and loads per second. |
| Input delay | `gP2P.delay` (and `gLockstep && delay < 1 ? 2 : delay`, :2015) | Store the value actually used at session start. |
| Desync | `gCsMatched` / `gCsMismatched` (:1804) | Show `DESYNC` in red if mismatched > 0. |
| Barrier waits | `gBarrierWaits` | Optional, verbose mode. |
| Stop reason | the `why` argument of `GgpoStop` (:1945) | Copy into a fixed `char[64]` and show `STOPPED: <why>` for about 5 s. |

**Publishing point:** at the end of `GgpoBoundaryP2P`, just after `CsPump(gGgpoLiveF)` (:2111). This runs on the
game thread, once per live frame, after any rollback and resimulation inside `ggpo_idle` has finished and
`gIsResimulating` has been cleared (:2104). GgpoAdvanceFrame, the resimulation callback, never publishes. When no
session runs, publish from `Rollback::OnTickEntry`'s idle path (or `HkEngineTick` exit), and only when
`!Rollback::gIsResimulating`.

---

## 3. Options compared (question 1)

| | (a) D3D9 draw at 0x667106 (recommended) | (a') D3D9 EndScene through the device vtable | (b) UE3 Canvas / HUD | (c) Window title / own window |
|---|---|---|---|---|
| How it gets in | 5-byte CodePatch in MKKE .text (verified bytes) | Copy the device's whole vtable (134 slots: 119 for IDirect3DDevice9 plus 15 for Ex), swap the **object's** vtable pointer `[[0xE84534]]` | Hook a game-thread HUD render function and call `UCanvas::DrawText` | `SetWindowTextW(hwnd)`; or a WS_EX_TOPMOST/LAYERED/TRANSPARENT/NOACTIVATE window on its own thread |
| Thread | rendering thread | rendering thread | game thread | game thread / own thread |
| Touches game state | no | no | **yes**: Canvas batches and FStrings | no |
| GMalloc | none | none | **yes** (verified below) | none |
| Resim risk | none (draws a published snapshot) | none | would need its own resim guard; HUD code runs inside the ticks | none |
| Device reset | nothing to release | the vtable pointer is unchanged by Reset; a **new** device after CreateDeviceEx needs re-hooking | n/a | n/a |
| Unhook | `CodePatch::RestoreAll` | write the original vtable pointer back | restore the patch | none |
| Conflicts | none known | other overlays that compare vtables (rare) | MK HUD code paths | none |
| Visible in fullscreen | yes | yes | yes | **title: no**; own window: not over exclusive fullscreen and may steal focus |
| Static work left | none | none | large: no `PostRender`/`DrawHUD` strings in the exe; Canvas natives only | none |

**(b) details [V]:**

- The UCanvas natives are in the native registration table: `intUCanvasexecDrawText` (string `0x00D1A4D8`, entry
  `0x00DEA928`) is at **`0x00759A30`**, and `execDrawTextClipped` (entry `0x00DEA920`) is at **`0x00759BC0`**.
  `execDrawTile` is `0x007597F0`.
- `execDrawText` needs `Canvas->Font` at canvas `+0x38`; otherwise it logs error `0x237`. It calls
  `UCanvas::DrawText` at `0x00759950` and frees its temporary FString through **GMalloc** (`[0xE3EFC8]` vt+0x10 at
  `0x00759B1A`).
- So a Canvas line allocates and frees on GMalloc every frame, and the text length differs between peers (different
  pings). That could change allocator reuse, which the project has had to control before (FreeQuarantine,
  MKHeap/AllocWatch). It also needs a live UCanvas, which exists only inside the HUD's PostRender on the game
  thread, plus more reverse engineering.
- **Rejected.**

**(a') details:** UE3 also calls EndScene from branch 1, but only once per frame, so a hook at EndScene (+0xA8) is
equivalent. The cost of (a'):

- It touches a d3d9-owned object (the vtable pointer).
- It needs re-hooking if MKKE recreates the device (`CreateDeviceEx` again).
- It must check that the device is still MKKE's (`[0xE84534]`) on every call.

It remains the backup if the CodePatch site misbehaves.

**Text renderer choice:**

- **ID3DXFont** is possible. `d3dx9_43.dll` is loaded at RHI init: `D3DXCheckVersion` is called through the delay
  stub at `0x0066A8F5` [V]. `GetProcAddress(GetModuleHandle("d3dx9_43.dll"), "D3DXCreateFontW")` would work. But:
  - it creates GDI fonts and textures;
  - it needs OnLostDevice/OnResetDevice around Reset;
  - on a **D3D9Ex** device `D3DPOOL_MANAGED` is not allowed, and I could not verify whether D3DX's glyph cache
    copes [A].
- **The built-in 8x8 font** has none of these problems: about 760 bytes of public-domain glyph data
  (font8x8_basic), with horizontal pixel runs merged into quads.
  - The vertices are `D3DFVF_XYZRHW | D3DFVF_DIFFUSE`.
  - It draws about 2,000 triangles at most, in one `DrawPrimitiveUP`, from a static vertex array.
  - The text is scaled ×2 at 1080p or more, starts at (8, 8), and sits on a semi-transparent black background quad.

---

## 4. Implementation plan

New files:

- `src\debug\Overlay.h`, `src\debug\Overlay.cpp`: install/uninstall, the stub, the draw, and the published stats.
- `src\debug\OverlayFont.h`: the 8x8 glyph table.

Changed files:

- `src\rollback\RollbackManager.cpp/.h`: publish the stats, rollback depth, stop reason.
- `src\debug\CommandChannel.cpp`: the `overlay` command.
- `src\dllmain.cpp`: install and unload order.
- `build.ps1`: add `src\debug\Overlay.cpp`. It needs no new libraries: d3d9.h comes from the llvm-mingw / Windows
  SDK headers, and every D3D call goes through the device vtable.
- Optional: `tools\netplay_gui.ps1` (a checkbox that writes `overlay 1`).

1. **Stats struct (game thread → rendering thread).**
   - In `Overlay.h`: `struct OverlayStats { uint32_t seq, liveGf; uint8_t mode; int16_t ping, behindL, behindR,
     delay; uint16_t lastDepth, maxDepth1s, loadsPerSec; int32_t loads, replayed, csMismatch; char stopWhy[64]; }`.
   - It is a POD with no pointers into game memory.
   - `Overlay::Publish(const OverlayStats&)` writes the inactive one of two static buffers, then
     `InterlockedExchange(&gFront, idx)`. The render thread copies the front buffer into a local before formatting.
     That makes it one writer and one reader, and lock-free.
2. **RollbackManager.**
   - In `GgpoBoundaryP2P`, inside the `gGgpoLoadedS != 0xFFFFFFFF` block (:2103): compute the depth
     `gGgpoLiveF - gGgpoLoadedS` *before* it is reset at the next boundary. Track the last value, a 60-frame rolling
     maximum and loads in the last 60 frames, using small static ring arrays (no heap).
   - After `CsPump` (:2111):
     - every 30 live frames, call `ggpo_get_network_stats` (P2P only, as at :1854) and cache the ping and the
       frames-behind values;
     - fill `OverlayStats` and call `Overlay::Publish` (every frame is fine; it costs about 100 bytes).
   - In `GgpoStop`: record `why` (strncpy into a static buffer) plus a timestamp in ms; publish mode STOPPED.
   - Idle (no session): publish OFF / WAITING from the live `OnTickEntry` path, guarded by
     `!Rollback::gIsResimulating`.
   - **Do not** add the overlay switch to `EnvFingerprint`'s "snapshot/FP switches": it is display-only, and peers
     may differ.
3. **Overlay::Install()**, on the injector thread, after `GameLoopHook::Install()` succeeds.
   - Verify `[0x00667106..0x00667116]` = `A1 34 45 E8 00 8B 08 8B 91 A8 00 00 00 50 FF D2`. On any mismatch, log
     and stay off (fail closed). The DLL as a whole already fails closed on an unknown exe hash (`MK9Game`).
   - `CodePatch::Write(0x00667106, {A1 34 45 E8 00}, {E8 rel32→MK9R_OverlayStub}, 5,
     "RHIEndDrawingViewport pre-EndScene 0x00667106 (overlay)")`.
   - The device may not exist yet at inject time. That is fine: the stub reads `[0xE84534]` every call and returns
     if it is null.
   - The first call on the render thread logs, once:
     - the device pointer and whether its vtable lies inside `d3d9.dll`'s image
       (`GetModuleHandleW(L"d3d9.dll")` plus SizeOfImage);
     - `GetRenderTarget(0)` == `GetBackBuffer(0,0)` (each Released);
     - the viewport size;
     - the thread id compared with `ObjectHooks::gGameThread`.

     If the vtable check fails, the overlay disables itself (`gEnabled = false`).
   - Install order matters. A failed `ObjectHooks::Install` calls `CodePatch::RestoreAll()` (ObjectHooks.cpp:229),
     which also removes the overlay patch. That is harmless (the stub is no longer reached), but log it. Better, make
     `Overlay::Install` idempotent and re-run it after `objhooks 1`.
4. **The draw (`Overlay_DrawRT`, rendering thread).**
   - Increment an in-flight counter on entry (`InterlockedIncrement`) and decrement it on exit. Return at once if
     `!gEnabled`, if the device is null, or if the `gUnloading` flag is set.
   - Save state: `CreateStateBlock(D3DSBT_ALL, &sb)` (vt+0xEC). Fallback if it costs too much: explicitly
     `Get`/`Set` this list: FVF and VertexDeclaration (AddRef'd: Release after restore), render states ZENABLE,
     ZWRITEENABLE, ALPHATESTENABLE, ALPHABLENDENABLE, SRCBLEND, DESTBLEND, BLENDOP, SEPARATEALPHABLENDENABLE,
     CULLMODE, FILLMODE, STENCILENABLE, SCISSORTESTENABLE, COLORWRITEENABLE, SRGBWRITEENABLE, FOGENABLE, LIGHTING,
     and TSS0/1 COLOROP/COLORARG1/ALPHAOP/ALPHAARG1.
     - Per frame it is create → draw → `Apply` → `Release`. It is never kept, so there is nothing to release before
       Reset.
   - Set those states for alpha-blended, untextured, fixed-function drawing. `SetFVF(XYZRHW|DIFFUSE)`. Textures and
     shaders are already NULL.
   - Format the text with `snprintf` into a `char[128]` on the stack. Build the vertices in a static array: 8x8
     glyphs, horizontal runs merged, ×2 scale if the viewport height is 1000 or more, offset −0.5 px.
   - Run one `DrawPrimitiveUP(D3DPT_TRIANGLELIST, n, verts, 20)`, then `sb->Apply(); sb->Release()`.
   - Do not call `Logger::Frame` from this thread. `Logger` is locked by a critical section and could be used, but
     it would add file I/O to the render thread. Keep counters only: draws, draws while `Rollback::gIsResimulating`
     was set (a racy read, diagnostic only) and the maximum draw time in µs (QueryPerformanceCounter). The game
     thread prints them on the NETSTATS line every 600 frames as `OVERLAY draws=… resimDraws=… maxUs=…`.
   - Never call GGPO, game code or GMalloc, and never write game memory.
5. **CommandChannel.** Add `overlay 0|1|2`: off, on, or verbose (adds kbps, send queue and barrier waits).
   - Default: **on** while a P2P session or an armed netplay exists, off otherwise.
   - The launcher can force it either way. A keyboard toggle is deliberately left out: `GetAsyncKeyState` would
     consume the "pressed since last call" bit the game might rely on.
6. **Unload (dllmain `MK9R_Unload`).**
   - Before `GameLoopHook::Uninstall()`, call `Overlay::Disable()`: set `gEnabled = false` so the stub becomes a
     pass-through.
   - `CodePatch::RestoreAll()`, which runs via `ObjectHooks::Uninstall()`, puts `A1 34 45 E8 00` back.
   - Then wait for the overlay's in-flight count to reach 0 (up to 2 s). Add `Sleep(100)` for a thread between the
     E8 and the counter increment, the same pattern as ObjectHooks.
7. **Fallback (c), optional, about 1 h.** `overlaytitle 1`: on the game thread at the live boundary, every
   30 frames and only when the text changed, `SetWindowTextW(hFocusWindow, L"MK9 - ROLLBACK ping 84 ...")`.
   - `hFocusWindow` comes from the device's `GetCreationParameters` (vt+0x24), read once by the render thread and
     cached, or from `FindWindow` on this process's top-level window.
   - Restore the original title (saved with `GetWindowTextW`) when the session ends and at unload.
   - It sends WM_SETTEXT to the game's own window on its own thread, so the call is direct and synchronous.

Estimate: 5–8 h, matching the to-do item's 5–10 h.

---

## 5. Risks and mitigations

| Risk | Mitigation |
|---|---|
| The device state is not restored exactly, so UE3 renders wrongly next frame | Use a D3DSBT_ALL state block per frame. RT and depth are not changed by the overlay. Check visually (screenshots) and in the logs (no D3D errors). |
| The site is not the back buffer, or not every frame (e.g. movies, loading screens through `0x00635EF0`) | `0x00635EF0` also calls 0x00666F20, so the same hook covers it. Step 3 logs RT0 == back buffer on the first call. If loading screens go through branch 1 (no present), the overlay is simply absent there, which is acceptable. |
| Extra time on the render thread changes render-written fields (LastRenderTime, LOD) | Already handled for rollback (ReplayRenderFields, `forcelod`); they are not simulation state (`docs\determinism.md` table at :158) [D]. The draw costs well under 0.1 ms; check `maxUs`. |
| FP environment on the render thread | The device is FPU_PRESERVE (0x46) [V]. The overlay uses plain float math and does not change the CW or MXCSR. `FpuGuard::PinAllThreads` re-pins every 600 ticks anyway. |
| A resimulated frame draws (if vt+0xC0 enqueues a viewport draw) | It draws only the published live snapshot. `resimDraws` measures it. Not a correctness issue. |
| Snapshot capturing overlay or D3D data | Overlay state lives only in DLL .data/.bss and on the render thread's stack, never in MK heaps or the game image. The snapshot never sees D3D objects or handles [A: Snapshot spans cover game memory only; confirm by checking that no span lies in the DLL's image range]. |
| Determinism or desync from the overlay itself | It reads only DLL globals, makes no game calls and uses no GMalloc. Prove it with A/B two-instance runs (Testing). |
| Ranked matchmaking | The overlay sends nothing, opens no session and reads no matchmaking state. It shows the online mode only through `Online::Engaged()`. Ranked fights stay on MK9's netcode and show OFF only if the user had armed a friend. |
| Unload race on the render thread | Disable flag, then RestoreAll, then wait for the in-flight count, then Sleep(100), as in ObjectHooks. |
| Another CodePatch user's failed install calls RestoreAll | Idempotent `Overlay::Install` plus a log line. Re-install after `objhooks 1`. |
| An unknown exe build | Already handled: the DLL fails closed on the SHA-256. The overlay also checks its 16 bytes. |
| D3D9Ex device recreated (fullscreen toggle) | The stub re-reads `[0xE84534]` every call. Nothing is cached except the HWND for the title fallback. |

---

## 6. Testing with the existing tools

All tests run after `.\build.ps1`. Logs are `build\clang\MK9Rollback.log` (A) and `build\clang2\…` (B).

1. **Install/unload smoke** (one instance, `tools\launch_game.ps1`, then `MK9RollbackInjector.exe inject`).
   - Expect `CODEPATCH RHIEndDrawingViewport pre-EndScene 0x00667106 (overlay): … patched`, then the first-call line
     (device, vtable in d3d9.dll, RT0 == back buffer, viewport WxH, render thread id ≠ game thread id).
   - `MK9RollbackInjector.exe unload` → `… restored`. The game keeps rendering.
   - Repeat three times, as the hooks were verified before.
2. **Visual check.** Use `tools\desk.ps1` (`Shot 'overlay.png'`) during a netplay fight in windowed and fullscreen
   mode. Alt-tab and change the resolution with the overlay on, which exercises the Reset path: no crash, the
   overlay comes back.
3. **Netplay regression, A/B.**
   - Overlay on: `tools\two_instance.ps1 -Netplay -Fights 3 -Setup 'overlay 1'`.
   - Overlay off: the same with `-Setup 'overlay 0'`.
   - Pass rule, as for the current regressions: 6 of 6 checksum reports, 0 `NETPLAY DESYNC`, no cut session. The
     `OVERLAY draws=` count should be about the live frames presented, and `resimDraws` should be 0 (or explained,
     see 2.4).
4. **Under lag.** `tools\two_instance.ps1 -Netplay -Fights 3 -NetSim 80 -Delay 2 -Setup 'overlay 1'`, and the
   `queued_checks.ps1 -LagMatrix` set with `overlay 1` added.
   - The on-screen ping should track NETSTATS (about 2 × 80 ms plus frame time). At 40/0 the "rolled back" figures
     should be clearly non-zero, as the lag matrix shows (360 loads per run).
5. **SyncTest soak with the overlay on.** `tools\queued_checks.ps1 -SoakOnly -SoakSetup 'overlay 1'`, or
   `tools\synctest_soak.ps1`. The error classes and rates should match the latest soaks: real failing stops of the
   known classes only, no new crash sites.
6. **Lockstep and the stop path.**
   - `-Lockstep` shows LOCKSTEP and no rollbacks.
   - `-KillBAfter 20` shows `STOPPED: the peer disconnected` on A for about 5 s, then OFF.
7. **Online mode** (needs the user, with the real Steam test): the overlay shows ROLLBACK once `Online::Engaged()`,
   and OFF for a ranked fight.
