# Builds dist\MK9Rollback-netplay-<date>.zip: everything a second PC needs for a netplay test, in the repository's
# layout (the launchers find the DLL and the injector in ..\build\clang and write their commands there), plus
# "Play Online.bat" at the top for the one-window launcher (tools\netplay_gui.ps1).
#   tools\package.ps1                         -> dist\MK9Rollback-netplay-20261025-1200.zip
#   tools\package.ps1 -BinariesFrom <folder>  -> the same with the DLL and the injector of an earlier build
# The DLL and the injector need only Windows' own libraries (UCRT, WinSock, bcrypt, winmm). Both PCs must run the
# same DLL: the session start compares the DLL hash and stops on a mismatch (NETPLAY ENVIRONMENT MISMATCH).
param([string]$BinariesFrom = '')
$ErrorActionPreference = 'Stop'
$root = Resolve-Path "$PSScriptRoot\.."
$bin = if ($BinariesFrom) { Resolve-Path $BinariesFrom } else { "$root\build\clang" }
# tools\launcher_lib.ps1 (since 2026-11-10) reaches players who update from an older version too: their updater copies
# every file of the zip. tools\updater.ps1's $kReleaseFiles must allow every file listed here (checked below).
$files = @('tools\netplay.ps1', 'tools\netplay_gui.ps1', 'tools\launcher_lib.ps1', 'tools\updater.ps1', 'tools\launch_game.ps1',
    'docs\netplay_lan_test.md')
foreach ($f in $files) { if (-not (Test-Path -LiteralPath "$root\$f")) { throw "missing $f" } }
foreach ($f in 'MK9Rollback.dll', 'MK9RollbackInjector.exe') { if (-not (Test-Path -LiteralPath "$bin\$f")) { throw "missing $bin\$f (build first: .\build.ps1)" } }
$stamp = Get-Date -Format 'yyyyMMdd-HHmm'
$zip = "$root\dist\MK9Rollback-netplay-$stamp.zip"
# A published zip of the same stamp must never change under its .sha256: wait a minute (a new stamp) instead.
if (Test-Path -LiteralPath $zip) { throw "$zip already exists; run again in a minute (a new stamp) or remove it yourself" }
$stage = Join-Path $env:TEMP "mk9r-package-$stamp"
if (Test-Path -LiteralPath $stage) { Remove-Item -LiteralPath $stage -Recurse -Force }
foreach ($f in $files) {
    New-Item -ItemType Directory -Force (Split-Path "$stage\$f") | Out-Null
    Copy-Item "$root\$f" "$stage\$f"
}
New-Item -ItemType Directory -Force "$stage\build\clang" | Out-Null
Copy-Item "$bin\MK9Rollback.dll", "$bin\MK9RollbackInjector.exe" "$stage\build\clang"
# Opened from inside the zip (Explorer extracts only the .bat to a temporary folder), the script is missing: say so.
@'
@echo off
rem Opens the MK9 Rollback netplay window (tools\netplay_gui.ps1).
if not exist "%~dp0tools\netplay_gui.ps1" (
    echo Extract the whole zip first: right-click it, 'Extract All', then open Play Online.bat in the extracted folder.
    pause
    exit /b 1
)
start "" powershell -NoProfile -ExecutionPolicy Bypass -WindowStyle Hidden -File "%~dp0tools\netplay_gui.ps1" %*
'@ | Set-Content -Encoding ascii "$stage\Play Online.bat"
$hash = (Get-FileHash "$bin\MK9Rollback.dll" -Algorithm SHA256).Hash
# The updater (tools\updater.ps1) compares this version with the latest GitHub release (tag v<stamp>).
"version=$stamp`r`ndll=$hash" | Set-Content -Encoding ascii "$stage\version.txt"
@"
MK9Rollback netplay test build ($stamp)
MK9Rollback.dll SHA-256: $hash
Both players must use this same zip (the session start checks the DLL).

HOW TO PLAY
1. Extract this zip anywhere and start Steam. (Nothing is installed; the game files are not changed.)
2. Double-click "Play Online.bat". If Windows says it protected your PC, click "More info", then "Run anyway".
3. Press Play. The window starts Mortal Kombat with the mod. No codes to swap.
4. In the game: Online > Player Match, a 1 vs 1 room (not King of the Hill). Play anyone there.
   If your opponent also started the game with "Play Online.bat" (this same zip), the mod's netcode takes
   over by itself at the fight start; the top left of the screen then shows ROLLBACK (LOCKSTEP in Safe
   mode), the ping, the input delay and a three-bar connection marker (green good, yellow so-so, red
   bad). F9 shows or hides the network details; the window's "Network display" box picks Compact,
   Detailed or Off. After the last round, the results and rematch screens run on
   MK9's own netcode; the mod takes over again at the next fight.
   Against anyone without the mod, MK9's own netcode plays. Ranked is never touched by the mod.
5. When you are done, or if something goes wrong, press "Save logs to the Desktop" and send the zip it
   makes, together with your opponent's zip. Both players' logs are needed. (Steam IDs, IP addresses and
   your Windows user and PC names are replaced in it; PRIVACY.txt inside lists what.)

HAVING TROUBLE? (the window has a "Having trouble?" button with the same list)
- Allow MKKE.exe through the Windows firewall (Windows Security > Firewall > Allow an app).
- Add the MK9 Rollback folder to your antivirus exclusions.
- Run the game from an SSD, and turn off disk sleep.
- On a laptop: plug it in and set the Windows power mode to "Best performance".
- Close browsers and other heavy apps (downloads, streaming, launchers).
- Use a wired (Ethernet) connection instead of Wi-Fi.
- Both players need the same version ("Check for updates" at the bottom of the window).
- In a fight, F9 shows or hides the network details.
6. Updates: the window checks for a new version when it opens. If it says one is available, close the
   game and press "Update now" at the bottom; the window restarts on the new version. Both players need
   the same version. (The files it replaced are kept in the previous_version folder; the window's "Go back
   to the previous version" button puts them back.)

NETCODE
The default is rollback: a late input from the other player is guessed and the frames are replayed when it
arrives, so the game never waits. The input delay follows the ping: the two games measure it before the
fight and agree on it (1 frame up to a 50 ms ping, 2 up to 100 ms, 4 on a long-distance one, up to 6 above a 200 ms
ping), keep measuring during the fight, and adjust it between rounds if the ping changed. A PC too slow for the extra
replays of 1 frame moves to 2 by itself after a round. The "Input delay" box in the window fixes it instead
(0 to 3 frames): 0 feels like offline but replays more, so it suits fast PCs on a short ping. Each player picks
for themselves and the larger choice of the two is used, so 0 needs both players to pick it.
Rollback can still go out of sync on rare replays (the window then says OUT OF SYNC). If that keeps
happening, tick "Safe mode" in the window: no rollback, every frame waits for the other player's input
(2-3 frames of input delay, from the ping). Both players must use the same setting.

The other choice in the window (Local Versus over Steam) is the older test mode: both players swap the
codes shown in the window, go to Versus > Local Versus, pick the same characters and stage, and start
within a minute of each other.

Advanced (LAN by IP address, other delays): tools\netplay.ps1, see docs\netplay_lan_test.md.
"@ | Set-Content -Encoding ascii "$stage\README.txt"
# Every file must pass the updater's allow-list, or the players' updater would refuse this zip.
. "$PSScriptRoot\updater.ps1"
foreach ($rel in Get-TreeFiles $stage) {
    if (-not (Get-ReleaseEntryPath $rel)) { throw "$rel is not on tools\updater.ps1's `$kReleaseFiles list: add it there first" }
}
New-Item -ItemType Directory -Force "$root\dist" | Out-Null
Compress-Archive -Path "$stage\*" -DestinationPath $zip
Remove-Item $stage -Recurse -Force
# Published with the zip (tools\publish_release.ps1): the updater installs a download only if it matches.
"$((Get-FileHash $zip -Algorithm SHA256).Hash)  $(Split-Path $zip -Leaf)" | Set-Content -Encoding ascii "$zip.sha256"
"$zip"
"MK9Rollback.dll SHA-256: $hash"
