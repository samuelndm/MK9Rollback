# Runs the remaining checks only after the user's friend test (2026-10-29): an MKKE must appear and exit, then no
# MKKE for 10 minutes (queued_checks.ps1 then also waits for 5 min without keyboard/mouse input and an unlocked PC).
Set-Location "$PSScriptRoot\.."
$t0 = Get-Date
$seen = $false
while (-not $seen) {
    if (Get-Process MKKE -ErrorAction SilentlyContinue) { $seen = $true; "friend test game seen $(Get-Date)" }
    elseif (((Get-Date) - $t0).TotalHours -ge 12) { 'no game seen in 12 h: not running the checks'; return }
    else { Start-Sleep 20 }
}
$quietSince = $null
while ($true) {
    if (Get-Process MKKE -ErrorAction SilentlyContinue) { $quietSince = $null }
    elseif (-not $quietSince) { $quietSince = Get-Date }
    elseif (((Get-Date) - $quietSince).TotalMinutes -ge 10) { break }
    Start-Sleep 20
}
"no game for 10 min since $quietSince; checks queued $(Get-Date)"
# The DLL in build\clang is the 1837 zip's (online auto + collctor probe); checked before each step.
$want = '0589CA20582A98D637BEFDF029B0E3BF968A71B1C8B2ADBD238F4E7FED961B46'
if ((Get-FileHash build\clang\MK9Rollback.dll).Hash -ne $want) { 'build\clang is not the 1837 DLL: not running the checks'; return }
# 1. smoke, plain regression (with the arming-skew retry), auto re-arm (no zip wanted, but package.ps1 makes one)
& .\tools\queued_checks.ps1 -Soak 0
# 2. the 80 ms lag run
& .\tools\queued_checks.ps1 -LagMatrix -LagOnly lag_ns80_d2
# 3. traced soak with the collctor probe
& .\tools\queued_checks.ps1 -SoakOnly -Soak 12 -FirstSeed 12901 -SoakSetup 'animtrace 1'
"all done $(Get-Date)"
