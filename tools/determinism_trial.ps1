# Determinism trials (Practice mode): reset the round with P1 mask 0x200, then
# inject an identical input script N times and compare, per fight frame:
#   - 8 raw fighter floats (STATE bits)
#   - per-region state hashes and RNG draw counts (HASH lines)
# and dump raw regions at selected trial frames for byte-level diffs.
param([int]$Trials = 3, [switch]$DriveP2, [string]$Dir = "$PSScriptRoot\..\build\clang",
      [string]$CompareStamp = '', [int]$From = 1, [int[]]$DumpFrames = @(1, 2, 100, 400, 800),
      [string]$RngSeed = '')
$ErrorActionPreference = 'Stop'
function Cmd([string[]]$lines) {
    Set-Content -Encoding ascii "$Dir\cmd.tmp" $lines
    Move-Item "$Dir\cmd.tmp" "$Dir\MK9Rollback_cmd.txt" -Force
    Start-Sleep -Milliseconds 300
}
# (mask, frames) for P1 after reset+settle; ~8 s of fighting
$p1 = @(@('8', 90), @('40', 6), @('0', 40), @('10', 6), @('0', 40), @('1', 6), @('0', 60), @('80', 6), @('0', 40),
        @('28', 6), @('0', 60), @('2000', 6), @('0', 60), @('4', 30), @('9', 6), @('0', 70))
$p2 = @(@('0', 60), @('4', 40), @('20', 6), @('0', 50), @('8000', 60), @('0', 40), @('80', 6), @('0', 300))
$total = ($p1 | ForEach-Object { $_[1] } | Measure-Object -Sum).Sum
$stamp = if ($CompareStamp) { $CompareStamp } else { Get-Date -Format 'yyyyMMdd-HHmmss' }
for ($t = 1; $t -le $Trials -and -not $CompareStamp; $t++) {
    $lines = @('lab 1', "mark trial-$stamp-$t", 'hold 1 200 2', 'hold 1 0 90')  # lab: the HASH lines it compares
    if ($RngSeed) { $lines += "rngset $RngSeed" }
    foreach ($f in $DumpFrames) { $lines += "dumpafter $f t$stamp-$t-f$f" }
    if ($DriveP2) { $lines += 'hold 2 0 92' }
    foreach ($s in $p1) { $lines += "hold 1 $($s[0]) $($s[1])" }
    if ($DriveP2) { foreach ($s in $p2) { $lines += "hold 2 $($s[0]) $($s[1])" } }
    $lines += 'hold 1 0 30'
    Cmd $lines
    Start-Sleep -Milliseconds ([int](($total + 92 + 30) / 60.0 * 1000) + 2500)
}
Start-Sleep 2

# ---- parse ----
$log = Get-Content "$Dir\MK9Rollback_frames.log"
$runs = @{}
for ($t = 1; $t -le $Trials; $t++) {
    $i = ($log | Select-String -SimpleMatch "MARK  trial-$stamp-$t").LineNumber
    $rows = New-Object System.Collections.Generic.List[object]
    $cur = $null
    foreach ($l in $log[$i..($log.Count - 1)]) {
        if ($l -like 'MARK *') { break }
        if ($l -match '^FRAME \d+ gf=(\d+) .*\| P1 v=\d pad=\w+ i=-?\d+ f=\w+ cur=(\w+),.*\| P2 v=\d pad=\w+ i=-?\d+ f=\w+ cur=(\w+),') {
            $cur = [ordered]@{ gf = [int]$Matches[1]; in = "$($Matches[2])/$($Matches[3])" }
        } elseif ($cur -and $l -match 'STATE .*bits=(\S+)') { $cur.bits = $Matches[1] }
        elseif ($cur -and $l -match '^  HASH \d+ all=(\w+) rng=(\w+) draws=(-?\d+) (.*)$') {
            $cur.all = $Matches[1]; $cur.rng = $Matches[2]; $cur.draws = [int]$Matches[3]
            $reg = [ordered]@{}
            foreach ($kv in $Matches[4].Trim() -split ' ') { $p = $kv -split '='; $reg[$p[0]] = $p[1] }
            $cur.reg = $reg
            $rows.Add([pscustomobject]$cur); $cur = $null
        }
    }
    $runs[$t] = $rows
}
$n = ($runs.Values | ForEach-Object { $_.Count } | Measure-Object -Minimum).Minimum
"stamp $stamp ; frames per trial: " + (($runs.Keys | Sort-Object | ForEach-Object { $runs[$_].Count }) -join ', ') + " ; comparing $From..$($n-1)"
$draws = ($runs[1][$From..($n - 1)] | ForEach-Object { $_.draws })
"trial1 RNG draws/frame: total=$(($draws | Measure-Object -Sum).Sum) max=$(($draws | Measure-Object -Maximum).Maximum) frames-with-draws=$(($draws | Where-Object { $_ -gt 0 }).Count) unknown=$(($draws | Where-Object { $_ -lt 0 }).Count)"
for ($t = 2; $t -le $Trials; $t++) {
    "--- trial 1 vs $t"
    $firstIn = -1; $firstBits = -1; $firstRng = -1; $firstAll = -1
    $regFirst = [ordered]@{}; $regCount = [ordered]@{}
    for ($k = $From; $k -lt $n; $k++) {
        $a = $runs[1][$k]; $b = $runs[$t][$k]
        if ($firstIn -lt 0 -and $a.in -ne $b.in) { $firstIn = $k }
        if ($firstBits -lt 0 -and $a.bits -ne $b.bits) { $firstBits = $k }
        if ($firstRng -lt 0 -and ($a.rng -ne $b.rng -or $a.draws -ne $b.draws)) { $firstRng = $k }
        if ($firstAll -lt 0 -and $a.all -ne $b.all) { $firstAll = $k }
        foreach ($name in $a.reg.Keys) {
            if ($a.reg[$name] -ne $b.reg[$name]) {
                if (-not $regFirst.Contains($name)) { $regFirst[$name] = $k; $regCount[$name] = 0 }
                $regCount[$name]++
            }
        }
    }
    "first mismatch: input=$firstIn fighterFloats=$firstBits rng=$firstRng combinedHash=$firstAll"
    if ($regFirst.Count) { foreach ($name in $regFirst.Keys) { "   region $name : first diff frame $($regFirst[$name]), differing frames $($regCount[$name])" } }
    else { "   all regions identical" }
}
