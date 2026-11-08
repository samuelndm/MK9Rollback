# Smoke test of the online mode on one PC (no opponent): starts MKKE if needed, injects, arms 'online' for a dummy
# SteamID and checks that both hooks (gamepad exchange 0x00885F80, connection idle time 0x004F5E10) were patched and
# that the module answers. Leaves the game running (kill it before building).
$ErrorActionPreference = 'Continue'
$dir = Resolve-Path "$PSScriptRoot\..\build\clang"
$log = "$dir\MK9Rollback.log"
function Cmd([string[]]$lines) {
    Set-Content -Encoding ascii "$dir\cmd.tmp" $lines
    Move-Item "$dir\cmd.tmp" "$dir\MK9Rollback_cmd.txt" -Force
}
# Launched again once if Steam did not start it (2026-10-31: right after the gate killed a running game, the first
# launch request produced no MKKE and the smoke failed at injection).
for ($try = 0; $try -lt 2 -and -not (Get-Process MKKE -ErrorAction SilentlyContinue); $try++) {
    Start-Sleep 5
    & "$PSScriptRoot\launch_game.ps1" | Out-Null
}
Start-Sleep 15
Remove-Item "$dir\MK9Rollback*.log" -ErrorAction SilentlyContinue
for ($t = 0; $t -lt 6 -and -not (Test-Path $log); $t++) {
    & "$dir\MK9RollbackInjector.exe" inject | Out-Null
    for ($w = 0; $w -lt 10 -and -not (Test-Path $log); $w++) { Start-Sleep 1 }
}
if (-not (Test-Path $log)) { 'FAIL: injection'; return }
for ($w = 0; $w -lt 60 -and -not (Select-String $log -Pattern 'Hook initialized successfully' -Quiet); $w++) { Start-Sleep 1 }
Cmd @('netcode safe', 'online 76561197960265729 1')  # the launcher's default mode
Start-Sleep 5
Cmd @('online off')
Start-Sleep 2
Cmd @('online auto 2')  # any opponent (no named friend), fixed delay
Start-Sleep 5
Cmd @('online off')
Start-Sleep 2
Cmd @('online auto ping')  # the launcher's command since 2026-11-01: the delay follows the round trip
Start-Sleep 5
Cmd @('online off')
Start-Sleep 2
$lines = Select-String $log -Pattern 'CODEPATCH (online|connection)|ONLINE' -CaseSensitive | ForEach-Object { $_.Line }
$lines
$ok = ($lines -cmatch '0x00885F80 \(7 bytes\) patched').Count -eq 1 -and ($lines -cmatch '0x004F5E10 \(6 bytes\) patched').Count -eq 1 -and
      ($lines -cmatch '0x0081BCD0 \(5 bytes\) patched').Count -eq 1 -and
      ($lines -cmatch 'ONLINE armed for \d').Count -eq 1 -and ($lines -cmatch 'ONLINE armed for any opponent, delay 2:').Count -eq 1 -and
      ($lines -cmatch 'ONLINE armed for any opponent, delay from the ping \(2-3\)').Count -eq 1 -and
      ($lines -cmatch 'ONLINE off').Count -eq 3 -and ($lines -cmatch 'CRASH').Count -eq 0
if ($ok) { 'PASS: online hooks installed, armed and disarmed' } else { 'FAIL: see the lines above' }
