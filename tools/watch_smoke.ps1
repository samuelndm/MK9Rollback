# Smoke test of the change watch (CrashLog::ChangeWatchArm, armed by the online mode on MK9's connection flags). Starts
# its own MKKE from the game folder (a game already running, e.g. a player's, is left alone; Steam must be running),
# injects build\clang's DLL into that process only, arms 'watchchange' on the host frame (GameInfo 0x00E89B70 + 0x6C,
# +1 every main-loop iteration, menus included) and checks that every thread was armed, that changes are logged with
# their writer (a counter: only the first 3) and that the game keeps running after 'watchchange 0'. Closes only the
# MKKE it started.
$ErrorActionPreference = 'Continue'
$dir = Resolve-Path "$PSScriptRoot\..\build\clang"
$log = "$dir\MK9Rollback.log"
$game = 'D:\Jogos\Steam\SteamLibrary\steamapps\common\MortalKombat_KompleteEdition\DiscContentPC'
function Cmd([string[]]$lines) {
    for ($w = 0; $w -lt 20 -and (Test-Path "$dir\MK9Rollback_cmd.txt"); $w++) { Start-Sleep -Milliseconds 100 }
    Set-Content -Encoding ascii "$dir\cmd.tmp" $lines
    Move-Item "$dir\cmd.tmp" "$dir\MK9Rollback_cmd.txt" -Force
}
Remove-Item "$dir\MK9Rollback*.log", "$dir\MK9Rollback_cmd.txt" -ErrorAction SilentlyContinue
$p = Start-Process "$game\MKKE.exe" -WorkingDirectory $game -PassThru
"own game: pid $($p.Id)"
Start-Sleep 20
for ($t = 0; $t -lt 6 -and -not (Test-Path $log) -and -not $p.HasExited; $t++) {
    & "$dir\MK9RollbackInjector.exe" inject "$dir\MK9Rollback.dll" $p.Id | Select-Object -Last 1
    for ($w = 0; $w -lt 10 -and -not (Test-Path $log); $w++) { Start-Sleep 1 }
}
if (-not (Test-Path $log)) { 'FAIL: injection'; if (-not $p.HasExited) { Stop-Process -Id $p.Id -Force }; return }
# Commands are read once the engine ticks (FPUGUARD lines at the first tick): 35 s after the injection in a player's
# game, 2 min in this background window (12:05). A batch written before that was overwritten by the next one.
for ($w = 0; $w -lt 240 -and -not (Select-String $log -Pattern 'FPUGUARD' -SimpleMatch -Quiet); $w++) { Start-Sleep 1 }
Cmd @('netcode safe', 'online auto 3')  # as the launcher's Safe mode
for ($w = 0; $w -lt 60 -and -not (Select-String $log -Pattern 'CMD online auto 3' -SimpleMatch -Quiet); $w++) { Start-Sleep 1 }
Cmd @('watchchange E89BDC')
Start-Sleep 5
Cmd @('watchchange 0')
Start-Sleep 5
$p.Refresh()
$alive = -not $p.HasExited -and $p.Responding
$ticking = (Get-Item $log).LastWriteTime
Start-Sleep 12  # MEMORY lines every 600 frames: the log keeps growing while the game ticks
$p.Refresh()
$stillTicking = (Get-Item $log).LastWriteTime -gt $ticking
$lines = @(Select-String $log -Pattern 'WATCH|CRASH|HANG|CMD (online|watch)' -CaseSensitive | ForEach-Object { $_.Line })
$lines
$armed = [bool]($lines -cmatch 'WATCH armed on \d+ threads: test address 00E89BDC')
$changes = @($lines -cmatch 'WATCH test address: [0-9A-F]{8} -> [0-9A-F]{8} eip=').Count
$disarmed = [bool]($lines -cmatch 'WATCH disarmed on \d+ threads')
"armed: $armed, changes logged: $changes, disarmed: $disarmed, game responding: $alive, still ticking after: $stillTicking"
$setup = [bool]($lines -cmatch 'CMD online auto 3')
"launcher setup applied first: $setup"
$ok = $setup -and $armed -and $changes -eq 3 -and $disarmed -and $alive -and $stillTicking -and -not ($lines -cmatch 'CRASH|HANG')
if (-not $p.HasExited) { Stop-Process -Id $p.Id -Force }
if ($ok) { 'PASS: change watch armed, logged its writers and disarmed' } else { 'FAIL: see the lines above' }
