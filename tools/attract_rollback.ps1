# Autonomous identity-rollback stress test on the attract-mode AI-vs-AI demo fights.
# No user input needed. Each 'rollback k' is checked by the built-in exactness probe
# (live Tier A+B+C state at F before vs after resimulating back to F).
param([string]$Dir = "$PSScriptRoot\..\build\clang", [int]$Count = 40, [int]$GapMs = 1500)
$ErrorActionPreference = 'Stop'
function Cmd([string[]]$lines) { Set-Content -Encoding ascii "$Dir\cmd.tmp" $lines; Move-Item "$Dir\cmd.tmp" "$Dir\MK9Rollback_cmd.txt" -Force }
function InFight { (Get-Content "$Dir\MK9Rollback_frames.log" -Tail 80 -ErrorAction SilentlyContinue) -match '^FRAME \d+ ' }
$start = (Get-Content "$Dir\MK9Rollback.log").Count
Cmd @('lab 1', 'objhooks 1', 'rbmode 1')
Start-Sleep 1
$sent = 0
for ($i = 0; $i -lt $Count * 4 -and $sent -lt $Count; $i++) {
    if (-not (Get-Process MKKE -ErrorAction SilentlyContinue)) { 'GAME EXITED'; break }
    if (InFight) { Cmd @("rollback $(1 + ($sent % 10))"); $sent++ }
    Start-Sleep -Milliseconds $GapMs
}
Start-Sleep 2
$new = Get-Content "$Dir\MK9Rollback.log" | Select-Object -Skip $start
$ok = ($new | Select-String 'exactness: state at F identical').Count
$bad = ($new | Select-String 'exactness: span').Count
$done = $new | Select-String 'ROLLBACK \d+ frames:'
$refused = $new | Select-String 'ROLLBACK \d+ refused'
$gpOk = ($new | Select-String 'gameplay exact').Count
$gpBad = ($new | Select-String 'gameplay MISMATCH').Count
"sent=$sent performed=$($done.Count) refused=$($refused.Count) gameplayExact=$gpOk gameplayMismatch=$gpBad rawIdentical=$ok alive=$([bool](Get-Process MKKE -ErrorAction SilentlyContinue))"
$ms = $done | ForEach-Object { if ($_.Line -match ', ([\d.,]+) ms') { [double]($Matches[1] -replace ',', '.') } }
if ($ms) { "resim time ms: min $(($ms | Measure-Object -Minimum).Minimum) avg $([math]::Round(($ms | Measure-Object -Average).Average,2)) max $(($ms | Measure-Object -Maximum).Maximum)" }
$new | Select-String 'exactness: span' | Group-Object { ($_.Line -split 'span ')[1].Split(' ')[0] } | ForEach-Object { "  differs: $($_.Name) x$($_.Count)" }
$refused | Select-Object -First 5 | ForEach-Object { '  ' + $_.Line }
$done | Select-Object -First 3 | ForEach-Object { '  ' + $_.Line }

