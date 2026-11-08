# Gate E: deliberate local rollback (Milestone 10).
# Setup: Practice reset + seed, save slot 1 at frame 150 (common start state).
# R: load slot 1; P1 neutral 50, FP(0x40) x6, neutral 150             (reference)
# I: same inputs as R, plus 'rollback 8' at frame 58 (identity)        (must equal R)
# T: P1 neutral at 50..55 (misprediction), 'fakerollback 8 40 6' at 58 (must equal R from 58 on)
param([string]$Dir = "$PSScriptRoot\..\build\clang", [string]$Seed = '1234ABCD', [int]$Depth = 8)
$ErrorActionPreference = 'Stop'
function Cmd([string[]]$lines, [int]$waitMs) {
    Set-Content -Encoding ascii "$Dir\cmd.tmp" $lines
    Move-Item "$Dir\cmd.tmp" "$Dir\MK9Rollback_cmd.txt" -Force
    Start-Sleep -Milliseconds $waitMs
}
$stamp = Get-Date -Format 'yyyyMMdd-HHmmss'
$at = 50 + $Depth - 1   # request at RunFrame of frame (50+Depth-1) -> executes at entry of 50+Depth with S = 50
Cmd @('clear', 'pause 0', 'lab 1', 'objhooks 1', 'rbmode 1', "mark gateE-setup-$stamp", "rngset $Seed", 'hold 1 200 2', 'hold 1 0 148',
      'at 150 saveab 1') 4500
Cmd @('clear', 'loadab 1', "mark gateE-R-$stamp", 'hold 1 0 50', 'hold 1 40 6', 'hold 1 0 150') 4500
Cmd @('clear', 'loadab 1', "mark gateE-I-$stamp", 'hold 1 0 50', 'hold 1 40 6', 'hold 1 0 150', "at $at rollback $Depth") 4500
Cmd @('clear', 'loadab 1', "mark gateE-T-$stamp", 'hold 1 0 50', 'hold 1 0 6', 'hold 1 0 150', "at $at fakerollback $Depth 40 6") 4500
Cmd @('clear', 'rbmode 0', 'release 1') 1000

$log = Get-Content "$Dir\MK9Rollback_frames.log"
function Rows([string]$label) {
    $i = ($log | Select-String -SimpleMatch "MARK  $label-$stamp").LineNumber
    $rows = New-Object System.Collections.Generic.List[object]; $cur = $null
    foreach ($l in $log[$i..($log.Count - 1)]) {
        if ($l -like 'MARK *') { break }
        if ($l -match '^FRAME \d+ gf=(\d+) .*\| P1 v=\d pad=\w+ i=-?\d+ f=\w+ cur=(\w+),') { $cur = [ordered]@{ gf = [int]$Matches[1]; in = $Matches[2] } }
        elseif ($cur -and $l -match '^  STATE .*bits=(\S+)') { $cur.bits = $Matches[1] }
        elseif ($cur -and $l -match '^  HASH \d+ all=(\w+) rng=(\w+) draws=(-?\d+) newobj=\d+ depth=-?\d+ (.*)$') {
            $cur.rng = $Matches[2]; $reg = [ordered]@{}
            foreach ($kv in $Matches[4].Trim() -split ' ') { $p = $kv -split '='; $reg[$p[0]] = $p[1] }
            $cur.reg = $reg; $rows.Add([pscustomobject]$cur); $cur = $null
        }
    }
    return , $rows
}
$R = Rows 'gateE-R'; $I = Rows 'gateE-I'; $T = Rows 'gateE-T'
"stamp $stamp ; presented frames R=$($R.Count) I=$($I.Count) T=$($T.Count)"
function Compare-Runs($A, $B, [string]$name, [int]$from) {
    $n = [Math]::Min($A.Count, $B.Count); $first = -1; $diffs = 0; $regFirst = [ordered]@{}
    for ($k = $from; $k -lt $n; $k++) {
        $a = $A[$k]; $b = $B[$k]
        $same = ($a.bits -eq $b.bits) -and ($a.rng -eq $b.rng)
        foreach ($r in $a.reg.Keys) { if ($a.reg[$r] -ne $b.reg[$r]) { $same = $false; if (-not $regFirst.Contains($r)) { $regFirst[$r] = $k } } }
        if (-not $same) { $diffs++; if ($first -lt 0) { $first = $k } }
    }
    "$name (frames $from..$($n-1)): differing frames $diffs, first $first" + $(if ($regFirst.Count) { ' ; regions: ' + (($regFirst.GetEnumerator() | ForEach-Object { "$($_.Key)@$($_.Value)" }) -join ' ') } else { '' })
}
Compare-Runs $R $I 'R vs I (identity rollback)' 0
Compare-Runs $R $T 'R vs T (corrected input)  ' (50 + $Depth)
"P1 life/pos R[$(50+$Depth+20)]: $($R[50+$Depth+20].bits)"
"P1 life/pos T[$(50+$Depth+20)]: $($T[50+$Depth+20].bits)"
Get-Content "$Dir\MK9Rollback.log" | Select-String 'ROLLBACK|SNAPSHOT-AB load' | Select-Object -Last 8 | ForEach-Object { '   ' + $_.Line }
