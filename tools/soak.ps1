# Repeated fresh-launch rollback stress sessions on the attract-mode demo fights.
# Each session: launch MK9 via Steam, inject, wait for a demo fight, send rollback requests,
# observe, then record the outcome and crash/hang signatures, and kill the game.
param([int]$Sessions = 3, [int]$Requests = 80, [int]$ObserveSeconds = 45,
      [string]$Dir = "$PSScriptRoot\..\build\clang", [string]$Out = "$PSScriptRoot\..\results\soak")
$ErrorActionPreference = 'Continue'
New-Item -ItemType Directory -Force $Out | Out-Null
$summary = @()
for ($s = 1; $s -le $Sessions; $s++) {
    Get-Process MKKE -ErrorAction SilentlyContinue | Stop-Process -Force
    Start-Sleep 3
    Remove-Item "$Dir\MK9Rollback*.log" -ErrorAction SilentlyContinue
    & "$PSScriptRoot\launch_game.ps1" | Out-Null
    Start-Sleep 12
    & "$Dir\MK9RollbackInjector.exe" inject | Out-Null
    for ($i = 0; $i -lt 120; $i++) { if ((Get-Content "$Dir\MK9Rollback_frames.log" -Tail 80 -ErrorAction SilentlyContinue) -match '^FRAME \d+ ') { break }; Start-Sleep 2 }
    $res = & "$PSScriptRoot\attract_rollback.ps1" -Count $Requests -GapMs 1200 -Dir $Dir | Select-Object -First 1
    Start-Sleep $ObserveSeconds
    Set-Content -Encoding ascii "$Dir\cmd.tmp" 'mark alive-check'
    Move-Item "$Dir\cmd.tmp" "$Dir\MK9Rollback_cmd.txt" -Force
    Start-Sleep 3
    $log = Get-Content "$Dir\MK9Rollback.log"
    $alive = [bool]($log | Select-String 'CMD mark alive-check')
    $crash = ($log | Select-String 'CRASH\?|HANG\?' | Select-Object -First 2 | ForEach-Object { $_.Line }) -join ' || '
    $lastRb = ($log | Select-String 'ROLLBACK \d+ frames' | Select-Object -Last 1 | ForEach-Object { $_.Line })
    $line = "session $s : $res ; aliveAfter=$alive"
    $summary += $line
    $summary += "   last rollback: $lastRb"
    if ($crash) { $summary += "   failure: $crash" }
    $stamp = Get-Date -Format HHmmss
    Copy-Item "$Dir\MK9Rollback.log" "$Out\session-$s-$stamp.log" -ErrorAction SilentlyContinue
    # Frame log: only the tail (it grows by every frame), for ACTORDEFER/ATTACH timelines.
    Get-Content "$Dir\MK9Rollback_frames.log" -Tail 20000 -ErrorAction SilentlyContinue | Set-Content -Encoding ascii "$Out\frames-$s-$stamp.log"
}
Get-Process MKKE -ErrorAction SilentlyContinue | Stop-Process -Force
$summary
