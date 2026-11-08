# The freeze both players hit (2026-10-29) came right after pressing Start at the title screen and was intermittent
# (the user's 18:41 game froze, the 18:44 one with the same build did not). This repeats exactly that: start the game,
# arm it like the launcher (plus -Extra), press Start, walk the menus for a minute, check for a hang, close it.
#   tools\start_loop.ps1 -Runs 8 [-Extra 'qmenus 1'] [-Name start_loop_qmenus]
# Prints one line per run and a total; the game log of every frozen run is kept as results\<Name>_<i>.log.
param([int]$Runs = 8, [string[]]$Extra = @(), [string]$Name = 'start_loop')
$ErrorActionPreference = 'Continue'
$root = Resolve-Path "$PSScriptRoot\.."
$frozen = 0
for ($i = 1; $i -le $Runs; $i++) {
    $out = @(& "$PSScriptRoot\menu_repro.ps1" -Minutes 1 -Extra $Extra -Name "${Name}_$i")
    $bad = [bool]($out -match 'FROZE|FAIL: injection') -or -not [bool]($out -match 'game responding: True')
    if ($bad) { $frozen++ } else { Remove-Item "$root\results\${Name}_$i.log" -ErrorAction SilentlyContinue }
    "run ${i}: $(if ($bad) { 'FROZE / FAILED' } else { 'ok' })  $(($out | Select-String 'FROZE|menus:') -join ' ')" |
        Tee-Object -FilePath "$root\results\$Name.txt" -Append
}
"${Name}: $frozen of $Runs runs froze or failed" | Tee-Object -FilePath "$root\results\$Name.txt" -Append
