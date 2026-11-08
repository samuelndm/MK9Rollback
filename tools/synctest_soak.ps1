# Repeated fresh-launch GGPO SyncTest sessions (noai_rollback.ps1 -Synctest 4), one input seed per session.
# Tallies loads, replayed frames, sync errors and crash/hang lines per session (results\synctest_soak\).
# Usage: tools\synctest_soak.ps1 [-Sessions 4] [-Minutes 13] [-FirstSeed 9201]
# -Combo: scripted combos instead of random input (tools\combo_inputs.ps1; full meters poked before each session).
param([int]$Sessions = 4, [int]$Minutes = 13, [int]$FirstSeed = 9201, [string[]]$Setup = @(),
      [string]$Dir = "$PSScriptRoot\..\build\clang", [string]$Out = "$PSScriptRoot\..\results\synctest_soak", [switch]$Combo,
      [switch]$Damage)  # -Damage: attract fights take damage ('demodamage 1'): KOs and round ends under rollback
$ErrorActionPreference = 'Continue'
New-Item -ItemType Directory -Force $Out | Out-Null
$rows = @()
for ($s = 0; $s -lt $Sessions; $s++) {
    $seed = $FirstSeed + $s
    Get-Process MKKE -ErrorAction SilentlyContinue | Stop-Process -Force
    Start-Sleep 3
    Remove-Item "$Dir\MK9Rollback*.log", "$Dir\MK9Rollback_cmd.txt" -ErrorAction SilentlyContinue
    & "$PSScriptRoot\launch_game.ps1" | Out-Null
    Start-Sleep 15
    for ($try = 1; $try -le 6 -and -not (Test-Path "$Dir\MK9Rollback.log"); $try++) {
        & "$Dir\MK9RollbackInjector.exe" inject | Out-Null
        for ($w = 0; $w -lt 10 -and -not (Test-Path "$Dir\MK9Rollback.log"); $w++) { Start-Sleep 1 }
    }
    if (-not (Test-Path "$Dir\MK9Rollback.log")) { "seed ${seed}: injection failed"; continue }
    # Commands sent before the DLL finished initializing are lost (first try: rollback never enabled).
    for ($w = 0; $w -lt 60 -and -not (Select-String "$Dir\MK9Rollback.log" -Pattern 'Hook initialized successfully' -Quiet); $w++) { Start-Sleep 1 }
    Start-Sleep 5
    & "$PSScriptRoot\noai_rollback.ps1" -Dir $Dir -Count 400 -Synctest 4 -MaxMinutes $Minutes -Seed $seed -Setup $Setup -Combo:$Combo -Damage:$Damage | Out-Null
    Start-Sleep 3
    $alive = [bool](Get-Process MKKE -ErrorAction SilentlyContinue)
    Get-Process MKKE -ErrorAction SilentlyContinue | Stop-Process -Force
    Start-Sleep 2
    Copy-Item "$Dir\MK9Rollback.log" "$Out\seed$seed.log" -Force
    if ($Setup.Count -and (Test-Path "$Dir\MK9Rollback_frames.log")) { Copy-Item "$Dir\MK9Rollback_frames.log" "$Out\seed${seed}_frames.log" -Force }
    $log = Get-Content "$Out\seed$seed.log"
    $loads = 0; $replayed = 0; $errors = 0; $fights = 0
    foreach ($l in $log) {
        if ($l -match 'GGPO synctest stopped .*: (\d+) loads, (\d+) replayed frames, (\d+) sync errors') {
            $loads += [int]$Matches[1]; $replayed += [int]$Matches[2]; $errors += [int]$Matches[3]; $fights++
        }
    }
    $crash = @($log | Select-String 'CRASH\?|HANG\?').Count
    $row = [pscustomobject]@{ Seed = $seed; Fights = $fights; Loads = $loads; Replayed = $replayed; SyncErrors = $errors; CrashLines = $crash; AliveAtEnd = $alive }
    $rows += $row
    $row | Format-Table -HideTableHeaders | Out-String | ForEach-Object { $_.Trim() }
}
$rows | Format-Table -AutoSize | Out-String
$rows | Export-Csv -NoTypeInformation "$Out\summary.csv"
