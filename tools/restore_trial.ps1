# Gate D experiment 1: save at trial frame $SaveAt, run sequence S (window W1),
# load the snapshot, run S again (window W2), compare W1[k] vs W2[k].
#   -Ab     use the Tier A+B snapshot (saveab/loadab) instead of the span snapshot
#   -Idle   S = 60 neutral frames (no object churn expected)
#   -Force  loadabforce (ignore the object-churn guard) -- may crash the game
param([string]$Dir = "$PSScriptRoot\..\build\clang", [int]$SaveAt = 120, [string]$Seed = '1234ABCD',
      [string]$CompareStamp = '', [switch]$Ab, [switch]$Idle, [switch]$Force)
$ErrorActionPreference = 'Stop'
$Seq = @(@('40', 6), @('0', 40), @('10', 6), @('0', 40), @('1', 6), @('0', 60), @('80', 6), @('0', 60), @('8', 30), @('0', 30))
if ($Idle) { $Seq = @(, @('0', 60)) }
$saveCmd = if ($Ab) { 'saveab 0' } else { 'save 0' }
$loadCmd = if ($Ab) { if ($Force) { 'loadabforce 0' } else { 'loadab 0' } } else { 'load 0' }
$len = ($Seq | ForEach-Object { $_[1] } | Measure-Object -Sum).Sum
$loadAt = $SaveAt + $len
$stamp = if ($CompareStamp) { $CompareStamp } else { Get-Date -Format 'yyyyMMdd-HHmmss' }
if (-not $CompareStamp) {
    $lines = @('clear', 'pause 0', 'lab 1', "mark restore-$stamp", "rngset $Seed", 'hold 1 200 2', "hold 1 0 $($SaveAt - 2)")
    foreach ($step in $Seq) { $lines += "hold 1 $($step[0]) $($step[1])" }
    foreach ($step in $Seq) { $lines += "hold 1 $($step[0]) $($step[1])" }
    $lines += 'hold 1 0 60'
    $lines += "at $SaveAt heapinfo"
    $lines += "at $SaveAt $saveCmd"
    $lines += "at $loadAt $loadCmd"
    foreach ($k in (@(1, 30, 100, ($len - 1)) | Where-Object { $_ -lt $len } | Select-Object -Unique)) {
        $lines += "dumpafter $($SaveAt + $k) r$stamp-w1-k$k"
        $lines += "dumpafter $($loadAt + $k) r$stamp-w2-k$k"
    }
    Set-Content -Encoding ascii "$Dir\cmd.tmp" $lines
    Move-Item "$Dir\cmd.tmp" "$Dir\MK9Rollback_cmd.txt" -Force
    Start-Sleep -Milliseconds ([int](($loadAt + $len + 60) / 60.0 * 1000) + 3000)
}
$log = Get-Content "$Dir\MK9Rollback_frames.log"
$i = ($log | Select-String -SimpleMatch "MARK  restore-$stamp").LineNumber
$rows = New-Object System.Collections.Generic.List[object]; $cur = $null
foreach ($l in $log[$i..($log.Count - 1)]) {
    if ($l -like 'MARK *') { break }
    if ($l -match '^FRAME \d+ gf=(\d+) .*\| P1 v=\d pad=\w+ i=-?\d+ f=\w+ cur=(\w+),') { $cur = [ordered]@{ gf = [int]$Matches[1]; in = $Matches[2] } }
    elseif ($cur -and $l -match 'STATE .*F1 v=\d life=(\S+) pos=(\S+) \| F2 v=\d life=(\S+) pos=(\S+) \| bits=(\S+)') { $cur.f = "$($Matches[1]) $($Matches[2]) | $($Matches[3]) $($Matches[4])"; $cur.bits = $Matches[5] }
    elseif ($cur -and $l -match '^  HASH \d+ all=(\w+) rng=(\w+) draws=(-?\d+) (.*)$') {
        $cur.rng = $Matches[2]; $cur.draws = $Matches[3]; $reg = [ordered]@{}
        foreach ($kv in $Matches[4].Trim() -split ' ') { $p = $kv -split '='; $reg[$p[0]] = $p[1] }
        $cur.reg = $reg; $rows.Add([pscustomobject]$cur); $cur = $null
    }
}
"stamp $stamp ; logged frames $($rows.Count) ; window length $len ; save@$SaveAt load@$loadAt"
"W1[0] gf=$($rows[$SaveAt].gf) $($rows[$SaveAt].f)"
"W2[0] gf=$($rows[$loadAt].gf) $($rows[$loadAt].f)"
$firstBits = -1; $firstRng = -1; $regFirst = [ordered]@{}; $regCount = [ordered]@{}; $bitDiffs = 0
for ($k = 0; $k -lt $len; $k++) {
    $a = $rows[$SaveAt + $k]; $b = $rows[$loadAt + $k]
    if ($a.bits -ne $b.bits) { $bitDiffs++; if ($firstBits -lt 0) { $firstBits = $k } }
    if ($firstRng -lt 0 -and $a.rng -ne $b.rng) { $firstRng = $k }
    foreach ($n in $a.reg.Keys) { if ($a.reg[$n] -ne $b.reg[$n]) { if (-not $regFirst.Contains($n)) { $regFirst[$n] = $k; $regCount[$n] = 0 }; $regCount[$n]++ } }
}
"fighter floats: first diff k=$firstBits, differing frames $bitDiffs / $len ; rng first diff k=$firstRng"
Get-Content "$Dir\MK9Rollback.log" | Select-String 'SNAPSHOT|HEAP object region' | Select-Object -Last 3 | ForEach-Object { "   " + $_.Line }
foreach ($n in $regFirst.Keys) { "   region $n : first diff k=$($regFirst[$n]), differing frames $($regCount[$n])" }
if ($firstBits -ge 0) {
    foreach ($k in $firstBits, ($firstBits + 1), ($firstBits + 10), ($len - 1)) {
        if ($k -lt $len) { "   k=$k W1: $($rows[$SaveAt + $k].f)"; "   k=$k W2: $($rows[$loadAt + $k].f)" }
    }
}


