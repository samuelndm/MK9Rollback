# MK9Rollback architecture (current state)

```
MKKE.exe (untouched on disk)
   ▲ LoadLibraryW via CreateRemoteThread
MK9RollbackInjector.exe (x86) ── verifies target SHA-256, inject / unload / status
   │
MK9Rollback.dll
 ├─ dllmain.cpp           init thread → verify build → install hooks; MK9R_Unload export
 ├─ game/MK9Game          hash pin + signature resolution (fail closed)
 ├─ hook/HookManager      aligned 4-byte atomic patches with recorded originals
 ├─ hook/GameLoopHook     EngineTick (vtable slot 0xD60DF0) + FightUpdatePlayers (call 0x80F70B)
 ├─ game/Input            ReadPad / ReadPlayerInput / OverridePlayerInput / Release
 ├─ game/Fighter          GetObj-equivalent read of life + position
 ├─ debug/CommandChannel  MK9Rollback_cmd.txt → hold / release / pause / step / mark
 └─ debug/Logger          MK9Rollback.log (events) + MK9Rollback_frames.log (per frame)
```

## Per-frame order with hooks installed

1. Main loop: HostFrame++, GameFrame++ (if unpaused), then the hardware sampler.
2. **HkEngineTick entry:** `CommandChannel::RunFrame()` applies the scripted input overrides.
3. Original EngineTick:
   1. Recorder, RNG and pre-tick work.
   2. **HkFightUpdate:** mirror the gates, log FRAME/STATE, then call the original.
   3. Script VM, world tick.
4. **HkEngineTick exit:** `CommandChannel::EndFrame()` applies pause/step at the frame boundary.

## Safety properties
- **No game modification on disk:** the game executable and game directory are never written.
- **Fail closed:** the DLL stays inert if any of these fails: exe hash, machine/base, signature uniqueness or expected VA, call-target or vtable expected value.
- **Patching:** both hooks are single aligned 4-byte stores. Unload restores both originals, drains in-flight calls, releases overridden pads, then `FreeLibraryAndExitThread`. Verified 4 times live, with byte-exact restore logged.

## Planned modules (not yet written)
- `rollback/` (Snapshot, StateHash, Replay, RollbackManager)
- `network/` (GGPO backend behind an interface)
- `debug/Overlay` (D3D9 EndScene/Present)

These come after Gates C and D, following the milestone order.

## Build
- `build.ps1`: clang through workspace-local llvm-mingw 20260922.
- `build.ps1 -Toolchain msvc`: VS 2022 Build Tools.
- Both produce `build/<tc>/MK9Rollback.dll` and `MK9RollbackInjector.exe`.
