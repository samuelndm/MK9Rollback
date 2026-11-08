# For each failing load (GGPO DESYNC gf=G (loaded S)) in a session log, compares the MKObj-derived
# births (type + spawn stack) of frames S..G between the live pass and the first replay pass of that
# load, using the frames log written with the 09:43 build (OBJ+ / OBJ+R lines with stk=).
# Usage: tools\spawn_diff.ps1 -Log results\spawn_1.log -Frames results\spawn_1_frames.log
param([string]$Log, [string]$Frames, [int]$Max = 12)

$loads = Select-String $Log -Pattern 'GGPO DESYNC gf=(\d+) \(loaded (\d+)\)' | ForEach-Object {
    [pscustomobject]@{ G = [int]$_.Matches[0].Groups[1].Value; S = [int]$_.Matches[0].Groups[2].Value }
} | Sort-Object S -Unique

# Index the frames log once: births of MKObj-derived types per (gf, pass kind).
$live = @{}; $replay = @{}
foreach ($line in [IO.File]::ReadLines((Resolve-Path $Frames))) {
    if ($line -notmatch '^(?:\[[^\]]*\] )?OBJ\+(R?) gf=(\d+) obj=\w+ id=\w+ type=(\S+) actor=\w+ ph=\d+ from=(\w+)(?: stk=(.*))?$') { continue }
    $type = $matches[3]
    if ($type -notmatch '<MKObj<') { continue }  # objects with a UE3 actor: effects, props, skeletal objects
    $gf = [int]$matches[2]
    $key = "$type from=$($matches[4]) stk=$($matches[5])"
    $tab = if ($matches[1] -eq 'R') { $replay } else { $live }
    if (-not $tab.ContainsKey($gf)) { $tab[$gf] = New-Object System.Collections.Generic.List[string] }
    $tab[$gf].Add($key)
}

$n = 0
foreach ($l in $loads) {
    if ($n++ -ge $Max) { break }
    "=== load S=$($l.S) G=$($l.G)"
    for ($g = $l.S; $g -le $l.G; $g++) {
        $a = @(); if ($live.ContainsKey($g)) { $a = @($live[$g]) }
        $b = @(); if ($replay.ContainsKey($g)) { $b = @($replay[$g]) }
        # A frame is replayed by several passes; count each key, compare multisets per pass size.
        $onlyLive = $a | Where-Object { $b -notcontains $_ } | Sort-Object -Unique
        $onlyReplay = $b | Where-Object { $a -notcontains $_ } | Sort-Object -Unique
        if ($onlyLive -or $onlyReplay) {
            "  gf=${g}: live $($a.Count) births, replay passes $($b.Count) births"
            $onlyLive | ForEach-Object { "    live only:   $_" }
            $onlyReplay | ForEach-Object { "    replay only: $_" }
        }
    }
}
