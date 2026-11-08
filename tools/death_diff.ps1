# For each failing load (GGPO DESYNC gf=G (loaded S)) in a session log, compares the MK object deaths of frames S..G
# between the live pass and the first replay pass after the load (OBJ- / OBJ-R lines in the frames log): which objects
# died in only one pass, or at a different frame.
# Usage: tools\death_diff.ps1 -Log results\x.log -Frames results\x_frames.log
param([string]$Log, [string]$Frames)
$loads = Select-String $Log -Pattern 'GGPO DESYNC gf=(\d+) \(loaded (\d+)\)' | ForEach-Object {
    [pscustomobject]@{ G = [int]$_.Matches[0].Groups[1].Value; S = [int]$_.Matches[0].Groups[2].Value }
} | Sort-Object S -Unique
$c = [IO.File]::ReadAllLines($Frames)
foreach ($ld in $loads) {
    "=== load S=$($ld.S) G=$($ld.G)"
    # The replay pass: from the FRAMER line 'gf=S ... GF_JUMP' until the next FRAME (live) line.
    $rs = -1
    for ($k = 0; $k -lt $c.Count; $k++) { if ($c[$k] -match "^FRAMER \d+ gf=$($ld.S) .*GF_JUMP") { $rs = $k } }
    if ($rs -lt 0) { '  no replay pass found'; continue }
    $re = $rs + 1
    while ($re -lt $c.Count -and $c[$re] -notmatch '^FRAME \d+ ') { $re++ }
    # The live pass of the same frames: the FRAME lines gf=S..G before the replay.
    $ls = $rs
    while ($ls -gt 0 -and $c[$ls] -notmatch "^FRAME \d+ gf=$($ld.S) ") { $ls-- }
    $live = @{}; $rep = @{}
    for ($k = $ls; $k -lt $rs; $k++) { if ($c[$k] -match '^OBJ- gf=(\d+) obj=(\S+) id=(\S+) type=(\S+)(.*stk=(.*))?') { $live[$Matches[3]] = "$($Matches[1]) $($Matches[4]) $($Matches[2]) killer: $($Matches[6])" } }
    for ($k = $rs; $k -lt $re; $k++) { if ($c[$k] -match '^OBJ-R gf=(\d+) obj=(\S+) id=(\S+) type=(\S+)(.*stk=(.*))?') { $rep[$Matches[3]] = "$($Matches[1]) $($Matches[4]) $($Matches[2]) killer: $($Matches[6])" } }
    foreach ($id in $live.Keys) { if (-not $rep.ContainsKey($id)) { "  live only:   id $id gf $($live[$id])" } elseif ($rep[$id].Split(' ')[0] -ne $live[$id].Split(' ')[0]) { "  diff frame:  id $id live gf $($live[$id]) / replay gf $($rep[$id])" } }
    foreach ($id in $rep.Keys) { if (-not $live.ContainsKey($id)) { "  replay only: id $id gf $($rep[$id])" } }
}

