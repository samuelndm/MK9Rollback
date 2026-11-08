# MK9Rollback

Rollback netcode for **Mortal Kombat Komplete Edition** on PC (Steam, `MKKE.exe` build 344915). A DLL injected into the
running game replaces MK9's own online input exchange with [GGPO](https://github.com/pond3r/ggpo) rollback when both
players run the mod, and leaves every other match (and Ranked) to the game's own netcode. The game files are not changed.

## Play

1. Download the latest zip from [Releases](../../releases/latest) and extract it anywhere. Start Steam.
2. Double-click **Play Online.bat**. (If Windows says it protected your PC: "More info", then "Run anyway".)
3. Press **Play**. The window starts the game with the mod.
4. In the game: Online > Player Match, a 1 vs 1 room. Against another player who started the game the same way,
   rollback takes over at the fight start (the top left of the screen shows ROLLBACK, the ping and the input delay).

Both players need the same version. The Play Online window checks for a new release when it opens; press
**Update now** (with the game closed) to install it. If something goes wrong, press "Save logs to the Desktop" and
attach the zip to an issue, from both players.

## What it does

- Rollback with GGPO, input delay picked from the ping (2 frames at a normal ping, up to 6) and adjusted between rounds.
- Whole-game snapshots each frame (fighters, effects, stage cinematics, script state) restored on a misprediction.
- Checks against desyncs: per-frame gameplay checksums compared with the other player, a build/content fingerprint at
  the session start.
- Smoothing for real connections: frame-advantage balancing, a short hold after a network stall so both games come out
  level, fast input resend after packet loss, X-ray starts run on confirmed inputs.
- "Safe mode": delay-based lockstep instead of rollback.

## Build

```powershell
.\build.ps1                       # DLL + injector with clang (llvm-mingw in ..\tools)
.\tools\build_ggpo.ps1            # the patched GGPO static library (from third_party\ggpo-*)
.\tools\package.ps1               # dist\MK9Rollback-netplay-<stamp>.zip (+ .sha256)
.\tools\publish_release.ps1 <zip> # GitHub release v<stamp> that the players' updater finds
```

Releases are published only after the release checks (`tools\queued_checks.ps1`: two-game netplay regressions, delay
changes, re-arming, an 8-minute menu walk). Scenario simulation across stages and network conditions:
`tools\scenario_matrix.ps1`.

## Docs

- [docs/netplay_lan_test.md](docs/netplay_lan_test.md): playing and testing
- [docs/architecture.md](docs/architecture.md), [docs/ggpo_integration.md](docs/ggpo_integration.md)
- [docs/determinism.md](docs/determinism.md), [docs/reverse_engineering.md](docs/reverse_engineering.md)
- [docs/todo.md](docs/todo.md): the running engineering log

## License and credits

GGPO is MIT-licensed (`third_party/ggpo-*/LICENSE`). Mortal Kombat is a trademark of Warner Bros. Entertainment; this
project is not affiliated with or endorsed by Warner Bros. or NetherRealm Studios, and contains none of the game's
files.
