# Builds the release's two zips in dist\ (each with a .sha256), in the repository's layout (the window finds the DLL
# and the injector in build\clang and writes its commands there):
#   MK9-Rollback-<stamp>.zip          the package: what players download. "MK9 Rollback.exe" at the top opens the
#                                     window (tools\netplay_gui.ps1); README.txt says how to play.
#   MK9Rollback-netplay-<stamp>.zip   the same without "MK9 Rollback.exe", only for the updaters of the versions
#                                     before 2026-11: they look for a zip of this name and refuse one with a file they
#                                     do not know (tools\updater.ps1, $kLegacyReleaseFiles). The window they install
#                                     fetches the program from the package afterwards (Complete-Install).
#   tools\package.ps1                         -> both zips, from build\clang
#   tools\package.ps1 -BinariesFrom <folder>  -> the same with the DLL, the injector and the program of another build
#   tools\package.ps1 -NoLegacy               -> only the package (once no player is on a version before 2026-11)
# The DLL and the injector need only Windows' own libraries (UCRT, WinSock, bcrypt, winmm). Both PCs must run the
# same DLL: the session start compares the DLL hash and stops on a mismatch (NETPLAY ENVIRONMENT MISMATCH).
param([string]$BinariesFrom = '', [switch]$NoLegacy)
$ErrorActionPreference = 'Stop'
$root = Resolve-Path "$PSScriptRoot\.."
$bin = if ($BinariesFrom) { Resolve-Path $BinariesFrom } else { "$root\build\clang" }
# tools\updater.ps1's $kReleaseFiles must allow every file listed here (checked below).
$files = @('tools\netplay.ps1', 'tools\netplay_gui.ps1', 'tools\launcher_lib.ps1', 'tools\updater.ps1', 'tools\launch_game.ps1',
    'docs\netplay_lan_test.md')
. "$PSScriptRoot\updater.ps1"   # the lists the zips are checked against, and the program's file name
foreach ($f in $files) { if (-not (Test-Path -LiteralPath "$root\$f")) { throw "missing $f" } }
foreach ($f in 'MK9Rollback.dll', 'MK9RollbackInjector.exe', $kLauncherExe) { if (-not (Test-Path -LiteralPath "$bin\$f")) { throw "missing $bin\$f (build first: .\build.ps1)" } }
$stamp = Get-Date -Format 'yyyyMMdd-HHmm'
$zip = "$root\dist\MK9-Rollback-$stamp.zip"
$legacyZip = "$root\dist\MK9Rollback-netplay-$stamp.zip"
# A published zip of the same stamp must never change under its .sha256: wait a minute (a new stamp) instead.
foreach ($z in $zip, $legacyZip) { if (Test-Path -LiteralPath $z) { throw "$z already exists; run again in a minute (a new stamp) or remove it yourself" } }
$stage = Join-Path $env:TEMP "mk9r-package-$stamp"
if (Test-Path -LiteralPath $stage) { Remove-Item -LiteralPath $stage -Recurse -Force }
foreach ($f in $files) {
    New-Item -ItemType Directory -Force (Split-Path "$stage\$f") | Out-Null
    Copy-Item "$root\$f" "$stage\$f"
}
New-Item -ItemType Directory -Force "$stage\build\clang" | Out-Null
Copy-Item "$bin\MK9Rollback.dll", "$bin\MK9RollbackInjector.exe" "$stage\build\clang"
# The here-string below takes this file's line endings (LF); Notepad showed such a README as one line until
# Windows 10 1809.
function CrLf([string]$text) { $text -replace "`r?`n", "`r`n" }
$hash = (Get-FileHash "$bin\MK9Rollback.dll" -Algorithm SHA256).Hash
# The updater (tools\updater.ps1) compares this version with the latest GitHub release (tag v<stamp>).
"version=$stamp`r`ndll=$hash" | Set-Content -Encoding ascii "$stage\version.txt"
CrLf @"
MK9 Rollback ($stamp)
Rollback netcode for Mortal Kombat Komplete Edition (Steam). Not affiliated with Warner Bros. or NetherRealm Studios.
MK9Rollback.dll SHA-256: $hash

HOW TO PLAY
1. Extract this zip anywhere and start Steam. (Nothing is installed; the game files are not changed.)
2. Open "MK9 Rollback.exe". If Windows says it protected your PC, click "More info", then "Run anyway"
   (the program is not signed by a company; its source is public).
3. Press Play. The window starts Mortal Kombat with the mod. There is nothing to set up.
4. In the game: Online > Player Match, a 1 vs 1 room (not King of the Hill). Play anyone there.
   If your opponent also runs MK9 Rollback (the same version), its netcode takes over by itself at the
   fight start; under the round timer the game then shows three connection bars (green good, yellow
   so-so, red bad), the ping in ms, R (frames rolled back) and D (input delay). Before the fight the
   screen says ROLLBACK READY. F9 switches to a line with every number and back. After the last round,
   the results and rematch screens run on MK9's own netcode; the mod takes over again at the next fight.
   Against anyone without the mod, MK9's own netcode plays. Ranked is never touched by the mod.
5. When you are done, or if something goes wrong, press "Save logs to the Desktop" and send the zip it
   makes, together with your opponent's zip. Both players' logs are needed. (Steam IDs, IP addresses and
   your Windows user and PC names are replaced in it; PRIVACY.txt inside lists what.)
6. Updates: the window checks for a new version when it opens. If it says one is available, close the
   game and press "Update now"; the window restarts on the new version. Both players need the same
   version. (The files it replaced are kept in the previous_version folder; "Go back to the previous
   version" at the bottom of the window puts them back.)

HAVING TROUBLE? (the window has a "Having trouble?" link with the same list)
- Allow MKKE.exe through the Windows firewall (Windows Security > Firewall > Allow an app).
- Add the MK9 Rollback folder to your antivirus exclusions.
- Run the game from an SSD, and turn off disk sleep.
- On a laptop: plug it in and set the Windows power mode to "Best performance".
- Close browsers and other heavy apps (downloads, streaming, launchers).
- Use a wired (Ethernet) connection instead of Wi-Fi.
- Both players need the same version ("Check for updates" at the bottom of the window).
- In a fight, F9 switches between the HUD display and the detailed network line.

NETCODE
Rollback: a late input from the other player is guessed and the frames are replayed when it arrives, so
the game never waits. The input delay follows the ping: the two games measure it before the fight and
agree on it (1 frame up to a 50 ms ping, 2 up to 100 ms, 4 on a long-distance one, up to 6 above a
200 ms ping), keep measuring during the fight, and adjust it between rounds if the ping changed. A PC
too slow for the extra replays of 1 frame moves to 2 by itself after a round.
Rollback can still go out of sync on rare replays (the window then says OUT OF SYNC): the rest of that
fight runs on MK9's own netcode. Please save the logs when it happens.

Advanced (LAN by IP address, other delays): tools\netplay.ps1, see docs\netplay_lan_test.md.
"@ | Set-Content -Encoding ascii "$stage\README.txt"

# The zip for the older updaters first: the staged files without the program. Every file must pass the list those
# updaters have, or they would refuse the update.
function Test-Legacy([string]$rel) { foreach ($p in $kLegacyReleaseFiles) { if ($rel -match $p) { return $true } }; $false }
New-Item -ItemType Directory -Force "$root\dist" | Out-Null
$made = @()
if (-not $NoLegacy) {
    foreach ($rel in Get-TreeFiles $stage) { if (-not (Test-Legacy $rel)) { throw "$rel is not a file the versions before 2026-11 take in an update (tools\updater.ps1 `$kLegacyReleaseFiles)" } }
    Compress-Archive -Path "$stage\*" -DestinationPath $legacyZip
    $made += $legacyZip
}
# The package: with the program. Every file must pass the updater's allow-list, or the players' updater would refuse
# this zip, and be on its $kPackageFiles list, or the window would leave it blocked (the internet zone mark of a
# downloaded zip) on the players' PCs.
Copy-Item -LiteralPath "$bin\$kLauncherExe" -Destination "$stage\$kLauncherExe"
foreach ($rel in Get-TreeFiles $stage) {
    if (-not (Get-ReleaseEntryPath $rel)) { throw "$rel is not on tools\updater.ps1's `$kReleaseFiles list: add it there first" }
    if ($kPackageFiles -notcontains $rel) { throw "$rel is not on tools\updater.ps1's `$kPackageFiles list: add it there first" }
}
Compress-Archive -Path "$stage\*" -DestinationPath $zip
$made = @($zip) + $made
Remove-Item $stage -Recurse -Force
# Published with the zips (tools\publish_release.ps1): an updater installs a download only if it matches.
foreach ($z in $made) { "$((Get-FileHash $z -Algorithm SHA256).Hash)  $(Split-Path $z -Leaf)" | Set-Content -Encoding ascii "$z.sha256" }
"$zip"
if (-not $NoLegacy) { "$legacyZip  (for the updaters of the versions before 2026-11 only)" }
"MK9Rollback.dll SHA-256: $hash"
