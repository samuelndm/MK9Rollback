# Compares the per-frame region hashes (HASH lines of MK9Rollback_frames.log) of two instances over a range of session
# frames, starting after the first 'PAUSE 0 applied' (the netplay start). Prints the regions that differ per frame.
# Usage: tools\hashdiff.ps1 -A build\clang\MK9Rollback_frames.log -B build\clang2\MK9Rollback_frames.log -GfA 575 -GfB 628 -From 1035 -To 1060
param([string]$A, [string]$B, [int]$GfA, [int]$GfB, [int]$From, [int]$To)
function Get-HashMap([string]$path, [int]$gf0) {
    $map = @{}; $cur = $null; $on = $false
    foreach ($l in [IO.File]::ReadLines((Resolve-Path $path))) {
        if (-not $on) { $on = $l -match '^PAUSE 0 applied'; continue }
        if ($l -match '^FRAME \d+ gf=(\d+)') { $cur = [int]$matches[1] }
        elseif ($l -match '^\s+HASH' -and $null -ne $cur) {
            $rel = $cur - $gf0
            if ($rel -ge $From -and $rel -le $To -and -not $map.ContainsKey($rel)) { $map[$rel] = $l }
        }
    }
    $map
}
$ha = Get-HashMap $A $GfA
$hb = Get-HashMap $B $GfB
"A frames $($ha.Count), B frames $($hb.Count)"
foreach ($r in $From..$To) {
    $x = $ha[$r]; $y = $hb[$r]
    if (-not $x -or -not $y) { "$r : missing A=$([bool]$x) B=$([bool]$y)"; continue }
    $xa = $x.Trim() -split ' '; $ya = $y.Trim() -split ' '
    $d = @()
    for ($i = 2; $i -lt [Math]::Min($xa.Count, $ya.Count); $i++) { if ($xa[$i] -ne $ya[$i]) { $d += $xa[$i] -replace '=.*', '' } }
    "$r : $($d -join ',')"
}
