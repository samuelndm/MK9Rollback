# Fresh game launch via Steam, inject MK9Rollback, wait until an attract-mode demo fight runs.
# -Reuse: inject into an already running (not yet injected) game instead of relaunching.
param([string]$Dir = "$PSScriptRoot\..\build\clang", [int]$WaitFightSeconds = 150, [switch]$Reuse)
$ErrorActionPreference = 'Continue'
if (-not ($Reuse -and (Get-Process MKKE -ErrorAction SilentlyContinue))) {
    Get-Process MKKE -ErrorAction SilentlyContinue | Stop-Process -Force
    Start-Sleep 3
    Remove-Item "$Dir\MK9Rollback*.log" -ErrorAction SilentlyContinue
    & "$PSScriptRoot\launch_game.ps1" | Where-Object { $_ -is [string] }
    Start-Sleep 12
} else {
    Remove-Item "$Dir\MK9Rollback*.log" -ErrorAction SilentlyContinue
}
# The game can take longer than the fixed wait to start: wait for the process, then retry the injection until the
# DLL's log appears (a single early attempt left whole soaks running on a game without the DLL, 2026-10-15/17).
for ($w = 0; $w -lt 60 -and -not (Get-Process MKKE -ErrorAction SilentlyContinue); $w++) { Start-Sleep 2 }
for ($try = 1; $try -le 6; $try++) {
    & "$Dir\MK9RollbackInjector.exe" inject | Select-Object -Last 1
    for ($w = 0; $w -lt 10 -and -not (Test-Path "$Dir\MK9Rollback.log"); $w++) { Start-Sleep 1 }
    if (Test-Path "$Dir\MK9Rollback.log") { break }
    "injection attempt ${try}: no log yet, retrying"
    Start-Sleep 5
}
if (-not (Test-Path "$Dir\MK9Rollback.log")) { 'INJECTION FAILED'; return }
for ($i = 0; $i -lt $WaitFightSeconds / 2; $i++) {
    if ((Get-Content "$Dir\MK9Rollback_frames.log" -Tail 80 -ErrorAction SilentlyContinue) -match '^FRAME \d+ ') { "fight after $($i * 2)s"; return }
    Start-Sleep 2
}
'no fight seen'
