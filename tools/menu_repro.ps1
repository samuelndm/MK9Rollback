# Title screen and menus with the mod (2026-10-29: both players' games froze after Start / in the menus; the 18:41
# freeze was MK heap #5 [0x00ED5400] failing an allocation from 0x0081A4B0 -> fatal handler 0x00406160).
# Starts MKKE through Steam, injects build\clang, sends the launcher's commands (unless -Passive: the DLL is loaded
# but rollback is not armed, the control run), presses Space / Enter through the title and the controller choice, then
# walks the menus with Enter / arrows / Backspace for -Minutes (keys posted to the game's window). Stops at the first
# "HANG? game thread" or crash line, or when the game exits. Needs the desktop (the game in the foreground).
#   tools\menu_repro.ps1 [-Minutes 10] [-Passive] [-Safe] [-Name menu_repro]   (-Safe: the launcher's Safe mode)
param([int]$Minutes = 10, [switch]$Passive, [switch]$Safe, [string]$Name = 'menu_repro',
      [string[]]$Extra = @())  # more game commands after the launcher's, e.g. 'abtest qmenus' (the pre-fix quarantine)
$ErrorActionPreference = 'Continue'
$root = Resolve-Path "$PSScriptRoot\.."
$dir = "$root\build\clang"
$log = "$dir\MK9Rollback.log"
. "$PSScriptRoot\desk.ps1"
function Cmd([string[]]$lines) {
    Set-Content -Encoding ascii "$dir\cmd.tmp" $lines
    Move-Item "$dir\cmd.tmp" "$dir\MK9Rollback_cmd.txt" -Force
}
function Bad { Select-String $log -Pattern 'HANG\? game thread|CRASH\? code' -ErrorAction SilentlyContinue | Select-Object -First 1 }

Get-Process MKKE -ErrorAction SilentlyContinue | Stop-Process -Force
Start-Sleep 3
Remove-Item "$dir\MK9Rollback*.log", "$dir\MK9Rollback_cmd.txt" -ErrorAction SilentlyContinue
& "$PSScriptRoot\launch_game.ps1" | Out-Null
Start-Sleep 12
for ($t = 0; $t -lt 6 -and -not (Test-Path $log); $t++) {
    & "$dir\MK9RollbackInjector.exe" inject | Out-Null
    for ($w = 0; $w -lt 10 -and -not (Test-Path $log); $w++) { Start-Sleep 1 }
}
if (-not (Test-Path $log)) { 'FAIL: injection'; return }
for ($w = 0; $w -lt 60 -and -not (Select-String $log -Pattern 'Hook initialized successfully' -Quiet); $w++) { Start-Sleep 1 }
if (-not $Passive) {  # what the launcher sends
    Cmd (@($(if ($Safe) { 'netcode safe' } else { 'netcode rollback' }), "online auto $(if ($Safe) { 3 } else { 2 })") + $Extra)
}
"mode: $(if ($Passive) { 'passive (mod loaded, rollback not armed)' } else { 'armed like the launcher' })"
$game = Get-Process MKKE | Select-Object -First 1
for ($w = 0; $w -lt 240 -and -not (Select-String $log -Pattern 'FPUGUARD' -SimpleMatch -Quiet); $w++) { Start-Sleep 1 }  # first tick
Start-Sleep 8
Focus | Out-Null
# Intro logos, title ("press the space bar"), then the controller choice: posted keys (SendInput never reached the
# menus, so before 2026-10-31 every run of this script stayed on the title screen and its attract demos).
$restarts = @(Select-String $log -Pattern 'timeline restart').Count
for ($i = 0; $i -lt 20; $i++) { PostKey @(0x20, 0x0D)[$i % 2]; Start-Sleep 2 }
"title: Start pressed $(Get-Date -Format HH:mm:ss)"
$rng = [Random]::new(7)
$end = (Get-Date).AddMinutes($Minutes)
$moves = 0
while ((Get-Date) -lt $end -and -not (Bad) -and -not $game.HasExited) {
    if (-not (IsFg)) { try { Focus | Out-Null } catch {} }
    # Mostly down/right + Enter into submenus (fights, the Krypt and other modes load levels), Backspace back out now and then.
    $k = @(0x28, 0x28, 0x27, 0x0D, 0x0D, 0x08, 0x26)[$rng.Next(7)]
    PostKey $k
    $moves++
    Start-Sleep -Milliseconds (600 + $rng.Next(1200))
    $game.Refresh()
}
$b = Bad
$levels = @(Select-String $log -Pattern 'timeline restart').Count - $restarts
"menus: $moves key presses, $levels timeline restart(s) (level loads); $(if ($b) { "FROZE: $($b.Line.Substring(0, [Math]::Min(200, $b.Line.Length)))" } elseif ($game.HasExited) { "GAME EXITED at $(Get-Date $game.ExitTime -Format HH:mm:ss) (exit code 0x{0:X8})" -f $game.ExitCode } else { 'no hang or crash' })"
"game responding: $((Get-Process MKKE -ErrorAction SilentlyContinue | Select-Object -First 1).Responding)"
Select-String $log -Pattern 'MEMORY gf|MKHEAPS' | Select-Object -Last 2 | ForEach-Object Line
# The quarantine must stay off in the menus (no session): an 'on' line or a pressure release here is a failure.
$qOn = @(Select-String $log -Pattern 'QUARANTINE on' -SimpleMatch).Count
$pr = @(Select-String $log -Pattern 'QUARANTINE pressure release' -SimpleMatch).Count
"quarantine switched on $qOn time(s), pressure releases $pr$(if ($qOn -or $pr) { ' FAIL' })"
Copy-Item $log "$root\results\${Name}.log" -ErrorAction SilentlyContinue
Copy-Item "$dir\MK9Rollback_frames.log" "$root\results\${Name}_frames.log" -ErrorAction SilentlyContinue
Get-Process MKKE -ErrorAction SilentlyContinue | Stop-Process -Force
