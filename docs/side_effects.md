# Milestone 11: side effects during resimulation (design notes, 2026-10-14)

Static groundwork for the forked side session. Nothing here is implemented or verified in-game except where marked **verified**.

## Problem

A rollback resimulates frames S..F-1 by running the real WorldTick and EngineTick. Anything those frames do outside game state happens again:
- sounds
- rumble
- camera shake
- particle and decal spawns
- announcer / UI
- network messages

Each side effect needs one of three treatments:

| Treatment | When to use it |
|---|---|
| **Suppress** | Purely presentational and already happened in the original timeline |
| **Dedupe** | Happened in both timelines: keep the original instance and skip the replay |
| **Correct** | Happened only in the corrected timeline: play it now, possibly late. Or only in the abandoned timeline: stop it |

## 1. Sound (highest priority: audible double plays)

**Path.** Script natives `0x8E16E0` / `0x8E1740` → the operand object's `vt+0x20`. On the global sound object `[0xE91FF0]` (vt `0xD5E420`) that slot is `0x921200` (slot address `0xD5E440`).
- **Verified:** runtime native trace, 2026-10-14. The natives store the return value (a play handle, or -1) in the executor result.
- **Verified (static):** `0x921200` is thiscall with 7 stack arguments (`ret 0x1C`). If arg 2 is 0, it forwards to `0x461E10` (play by name; arg 1 is a `char*` cue name, checked non-empty). Otherwise it resolves through `0x920F60`.
- **Not verified:** what the other arguments mean (actor, volume, position).

**Proposed hook:** replace vtable slot `0xD5E440` (a data write, no code patch) with a wrapper:
- **Live frame:** call the original and record `{gf, nameHash(arg1), arg2, handle}` in a per-frame ring (12 frames, the rollback window).
- **Resimulated frame g:**
  - If an unconsumed original record for g has the same name and arg 2: **don't play**. Return the original handle and mark the record consumed.
  - Returning the original handle keeps script variables identical to the original timeline. That also removes the sound-handle noise from the native trace without special filtering.
  - Otherwise the sound is new in the corrected timeline: play it for real (late by F−g frames, acceptable for short SFX). Record the new handle under g.
- **After the resimulation:** original records for frames S..F-1 that were never consumed belong only to the abandoned timeline. Stop those sounds by handle (the stop path still needs to be found: string "STOPSOUND" referenced at `0x58B8B7`, or another slot of vt `0xD5E420`), or just let short ones finish.
- **Threads:** the sound path is called from the game thread through script natives. Confirm no other thread uses the slot before hooking.

Other sound sources not covered by this slot need their own wrapper:
- UE3 `PlaySound` and Kismet `ClientPlaySound` (strings at `0xD21150`, `0xD21378`; `AActor::execPlaySound`);
- `AudioComponent`s owned by effect actors;
- FMOD events (`FmodAudioComponent`, `SoundEngine.mko`).

Measure first: count calls into the FMOD event-start function while `gIsResimulating` is set.

## 2. Rumble (force feedback)

- **Strings:** `p_rumble_gamepad` (referenced at `0x42718B`), `%s F_SUPPORTS_RUMBLE`, `%s F_RUMBLE_MOTOR_ON` (`0x4D56C1`/`0x4D56E1`). The motor writes are presumably XInput `XInputSetState` (MKKE loads xinput1_3).
- **Proposal:** hook the XInput `SetState` import (IAT) and ignore it while `gIsResimulating` is set. After the resimulation, apply the motor state of the corrected frame F−1 once.
  - Rumble is level-based (motor speeds), not event-based, so "last state wins" is correct and no dedupe is needed.

## 3. Camera shake

- **Data:** `SpecialCameraShakeData`, `shakeTicks`, `shakeCounts`, `shakeIntensities` (around `0x977F66` / `0x97E09B`), plus `shake_camera_ignore_pause`.
- **The fight camera floats `0xDF0BA0..0xDF0C40` are already restored (Tier A).** If shake state lives in the camera object, the resimulation recomputes it and there's nothing to do.
- **Check:** whether the shake counters live outside restored memory (the load-only diff lists `MKCamera_0:DefaultCamera` with 3 changed chunks). If they do, include them in Tier A/C. They're presentational, but a camera that drifts after each rollback is visible.

## 4. Particles, lights, decals (MKParticle / MKFXLight / decal actors)

Current behaviour (from ActorDefer and PreRestore):
- Effects born after S are destroyed at load and spawned again during the resimulation, so a running effect restarts.
- Effects that died after S are given back.

Visible consequences:
- an effect may visibly restart (for example a hit spark playing again from frame 0 instead of continuing);
- LifeSpan-based UE3 effects (`0x7AEC5C` path, see `load_refusals.md`) aren't rolled back at all.

Proposal: leave as is for now. Judge it visually in LAN tests (Milestone 13); with GGPO rollbacks are 1–3 frames long, so restarts are short. If needed later, match respawned effects to the destroyed ones by (spawn frame, type, owner) and keep the original actor instead of respawning.

## 5. Announcer, UI, HUD, network

- **Announcer and voice lines:** sound path, see section 1.
- **HUD:** health bars, meter and timer read restored gameplay state, so they re-render correctly. Combo counters and "First Hit" banners are script or UI driven and need a check in LAN tests.
- **Network:**
  - MK9's own online layer (`MKOnlineGame`, `[0xE924F0]`) must not run in GGPO sessions (see `load_refusals.md`). Never touch ranked.
  - Steam calls during resimulation: none expected in fight ticks. Verify by counting Steam API calls while `gIsResimulating` is set.

## Implementation order (suggested)

1. A "side-effect census" in the main DLL. While `gIsResimulating` is set, count calls into:
   - sound vt slot `0xD5E440`;
   - FMOD event start;
   - XInput `SetState`;
   - DestroyActor and SpawnActor;
   - Steam API.

   One soak gives the actual list.
2. Sound wrapper with dedupe (section 1).
3. Rumble IAT hook (section 2).
4. Camera-shake check (section 3).
5. Effects: revisit after LAN tests (section 4).
