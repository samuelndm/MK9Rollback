# Dword-level diff of whole pages across N single fight-frame steps (game paused in a fight).
# Uses the DLL 'peek' command (logs non-zero dwords). Prints dwords that changed in any step.
param([string[]]$Pages = @('DF0000', 'E39000', 'E7C000'), [int]$Steps = 3, [string]$Dir = "$PSScriptRoot\..\build\clang")
$ErrorActionPreference = 'Continue'
function Cmd([string[]]$lines) { Set-Content -Encoding ascii "$Dir\cmd.tmp" $lines; Move-Item "$Dir\cmd.tmp" "$Dir\MK9Rollback_cmd.txt" -Force; Start-Sleep -Milliseconds 700 }
function Snap {
    $start = (Get-Content "$Dir\MK9Rollback.log").Count
    foreach ($p in $Pages) { Cmd "peek $p 1024" }
    Start-Sleep -Milliseconds 500
    $m = @{}
    foreach ($l in (Get-Content "$Dir\MK9Rollback.log" | Select-Object -Skip $start)) {
        if ($l -match 'PEEK (\w+) \+\w+ = (\w+)') { $m[$Matches[1]] = $Matches[2] }
    }
    $m
}
Cmd 'pause 1'
Start-Sleep 1
$snaps = @(Snap)
for ($s = 0; $s -lt $Steps; $s++) { Cmd 'step 1'; Start-Sleep 1; $snaps += , (Snap) }
$changed = @{}
for ($s = 1; $s -lt $snaps.Count; $s++) {
    $a = $snaps[$s - 1]; $b = $snaps[$s]
    foreach ($k in ($a.Keys + $b.Keys | Sort-Object -Unique)) { if ($a[$k] -ne $b[$k]) { $changed[$k] = $true } }
}
foreach ($k in ($changed.Keys | Sort-Object)) { "$k : " + (($snaps | ForEach-Object { if ($_[$k]) { $_[$k] } else { '0' } }) -join ' ') }
Cmd 'pause 0'
