# Gate E measurement: non-AI rollbacks over as many fresh game sessions as needed (the attract loop
# stops offering demo fights after a few), aggregated. Optional -Correct runs the prediction-correction
# test (see noai_rollback.ps1).
param([int]$Target = 150, [int]$PerSession = 60, [int]$MaxSessions = 6, [switch]$Correct, [string[]]$Setup = @(),
      [string]$Out = "$PSScriptRoot\..\results\gatee", [string]$Dir = "$PSScriptRoot\..\build\clang")
$ErrorActionPreference = 'Continue'
New-Item -ItemType Directory -Force $Out | Out-Null
$done = 0; $seed = 1000
for ($s = 1; $s -le $MaxSessions -and $done -lt $Target; $s++) {
    $start = & "$PSScriptRoot\start_attract.ps1" | Select-Object -Last 1
    $args2 = @{ Count = [Math]::Min($PerSession, $Target - $done); Seed = $seed + $s; Dir = $Dir }
    if ($Correct) { $args2.Correct = $true }
    if ($Setup.Count) { $args2.Setup = $Setup }
    $res = & "$PSScriptRoot\noai_rollback.ps1" @args2 2>&1
    Copy-Item "$Dir\MK9Rollback.log" "$Out\session-$s.log" -Force
    $performed = @(Get-Content "$Out\session-$s.log" | Select-String 'ROLLBACK \d+ frames:').Count
    $done += $performed
    "session $s ($start): " + (($res | Where-Object { $_ -match '^fights=|^corrections|CRASHED|GAME EXITED' }) -join ' | ')
}
# Aggregate: plain rollbacks, corrections and replays after corrections; crashes.
$c = @{ plainOk = 0; plainBad = 0; fakeChanged = 0; fakeSame = 0; replayOk = 0; replayBad = 0; crashes = 0; refused = 0 }
foreach ($f in Get-ChildItem "$Out\session-*.log") {
    $state = 0
    foreach ($line in Get-Content $f.FullName) {
        if ($line -match 'CRASH\? code') { $c.crashes++; continue }
        if ($line -match 'ROLLBACK \d+ refused') { $c.refused++; continue }
        if ($line -match 'CMD fakerollback') { $state = 1; continue }
        if ($line -notmatch 'gameplay (exact|MISMATCH)') { continue }
        $ex = $line -match 'gameplay exact'
        if ($state -eq 1) { if ($ex) { $c.fakeSame++ } else { $c.fakeChanged++ }; $state = 2; continue }
        if ($state -eq 2) { if ($ex) { $c.replayOk++ } else { $c.replayBad++ }; $state = 0; continue }
        if ($ex) { $c.plainOk++ } else { $c.plainBad++ }
    }
}
$plain = $c.plainOk + $c.plainBad; $replay = $c.replayOk + $c.replayBad
"GATE E: plain identity rollbacks exact $($c.plainOk)/$plain" + $(if ($plain) { ' ({0:P1})' -f ($c.plainOk / $plain) } else { '' }) +
    "; replays after correction exact $($c.replayOk)/$replay; corrections that changed the outcome $($c.fakeChanged); refused $($c.refused); crashes $($c.crashes)"
