# GGPO integration notes (Milestone 12 groundwork)

Status 2026-10-14: preparatory work only. GGPO is **not integrated**, as the spec requires (Gates A–E first; Gate E is still open at ~20% gameplay mismatch).

## Build: verified

The source is `third_party/ggpo-7ddadef…`. It compiles with the project's llvm-mingw i686 toolchain once three adjustments are made:

1. **No `-DUNICODE` for the GGPO sources.** `platform_windows.cpp` passes `char` buffers to `GetEnvironmentVariable`.
2. **A one-line patch in `lib/ggpo/network/udp_proto.h:56`.** `UdpProtocol::Event(Type t = Unknown)` is an extra qualification that only MSVC accepts. Change it to `Event(Type t = Unknown)`. `-fms-extensions` does not help, and `-fms-compatibility` breaks the MinGW headers.
3. **Libraries and the two compiler sets:**
   - Link `-lws2_32 -lwinmm`.
   - Flags used: `-std=c++17 -O2 -fno-exceptions -fno-rtti -D_WINDOWS -I src/include -I src/lib/ggpo`.
   - Exclude `platform_linux.cpp`.
   - The MSVC toolchain (`build.ps1 -Toolchain msvc`) should compile the source unpatched.

**Smoke test (toy game, no MK9).** `ggpo_start_synctest`, 2 local players, `check_distance` 7, 600 frames:

| Variant | Result |
|---|---|
| Deterministic | No sync error. 1196 saves, 85 loads, 595 resimulated frames |
| Hidden unsaved state | Mismatch reported at the first check (frame 7) |

This was built in a scratch directory: a patched copy of `src` and the archive `libggpo.a` (124 KB).

## Required patch before running inside MKKE

`SyncTestBackend::RaiseSyncError` ends in `DebugBreak()`. Without a debugger that kills the game. Replace it with a log line plus a counter, or a GGPO event, so that synctest runs report mismatches instead of crashing.

## Mapping onto the existing rollback engine

| GGPO callback | MK9Rollback implementation |
|---|---|
| `save_game_state(buf, len, checksum, frame)` | `Snapshot::SaveAB` into the ring slot for the frame. The GGPO buffer is a small token `{gf, epoch}`, not the state itself (the state is MBs). The checksum is the **gameplay** hash: life, position, meter, RNG and GameFrame, as in `CaptureGameplay`. A raw memory hash never matches, because worker-thread allocator noise differs. |
| `load_game_state(buf, len)` | The restore half of `DoRollback`: `ObjectHooks::PreRestore` → `LoadAB` → `FreeQuarantine::OnLoad` → `ActorDefer::OnLoad` → `PhysWatch::DropFrom` → `PostRestore` → `Ue3Refs::FixDangling`, plus the attach audit. |
| `advance_frame(flags)` | One resimulated frame, as in the resim loop: `ggpo_synchronize_input` → `gWorldTick` → apply the frame's inputs → `gTick` → `FlushFrame` → `ggpo_advance_frame`. |
| `free_buffer` | Free the token. Ring slots are reused by frame number. |
| `on_event` | Log events. Map timesync to frame delay later (Milestone 13). |

**Live frame**, at WorldTick entry (`OnTickEntry`):
1. `ggpo_idle`.
2. Read the local pad and call `ggpo_add_local_input`.
3. Call `ggpo_synchronize_input`. Apply both players' masks with `Input::OverridePlayerInput` **at EngineTick entry, not before WorldTick**. This is the input-timing rule found on 2026-10-14: the live frame applies and records input at EngineTick entry.
4. At the end of the tick, call `ggpo_advance_frame`.

## Blockers to clear before synctest in MKKE

1. **Determinism (Gate E).** Synctest compares checksums on every rollback, so any of today's ~20% gameplay mismatches is a sync error.
   - First run synctest on the non-AI harness (`noai_rollback.ps1`), at ~11% mismatch.
   - Lead: the channel record `+4` object (bone mask and position) is not snapshotted. See the main session.
2. **Loads that can refuse.** GGPO assumes a load always succeeds. Today's refusals must become impossible or end the session with a desync event:
   - `CanLoadTo`
   - `PhysWatch` (a non-MKObj actor destroyed in the window)
   - "no snapshot"
   - `Ue3Refs::TornDownSince`
3. **Cost.** GGPO saves every frame and rolls back up to `max_prediction` frames, 8 by default.

   | Item | Today |
   |---|---|
   | Save | ~3.8–5.8 ms/frame |
   | Resimulated tick | ~1.5 ms/frame |
   | Load | ~3–6 ms |

   An 8-frame rollback is about 50 ms, three frames at 60 Hz. Reduce this before netplay with incremental or dirty-page saves, fewer xblocks, and no per-resim-frame span rebuild. Otherwise use a frame delay of 2–3 to keep rollbacks short.
4. **`DebugBreak` patch.** See above.
