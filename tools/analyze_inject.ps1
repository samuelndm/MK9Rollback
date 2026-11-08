# Summarises an input-injection run from MK9Rollback_frames.log: for each
# segment of identical P1 cur mask, report frames, P1/P2 position change, P2
# life change and P1 ring-index movement.
param([string]$Trace = "$PSScriptRoot\..\build\clang\MK9Rollback_frames.log", [string]$From = 'inject-test-1')
$lines = Get-Content $Trace
$start = ($lines | Select-String -SimpleMatch "MARK  $From", "MARK $From" | Select-Object -First 1).LineNumber
if (-not $start) { throw "marker $From not found" }
$rows = @(); $cur = $null
foreach ($l in $lines[$start..($lines.Count - 1)]) {
    if ($l -match '^FRAME (\d+) gf=(\d+) .*?pause=(\d).*\| P1 v=\d pad=\w+ i=-?\d+ f=(\w+) cur=(\w+),\w+,\w+ prev=(\w+) rings=(\d+)/(\d+)/(\d+)') {
        $cur = [ordered]@{ sim = [int]$Matches[1]; gf = [int]$Matches[2]; pause = [int]$Matches[3]; flags = $Matches[4];
                           mask = $Matches[5]; prev = $Matches[6]; r = "$($Matches[7])/$($Matches[8])/$($Matches[9])" }
    } elseif ($cur -and $l -match 'STATE \d+ ovr=(\d)\d \| F1 v=\d life=([\d.-]+) pos=([\d.-]+),([\d.-]+),([\d.-]+) \| F2 v=\d life=([\d.-]+) pos=([\d.-]+),([\d.-]+),([\d.-]+)') {
        $cur.ovr = [int]$Matches[1]; $cur.l1 = [double]$Matches[2]; $cur.y1 = [double]$Matches[4]; $cur.z1 = [double]$Matches[5]
        $cur.l2 = [double]$Matches[6]; $cur.y2 = [double]$Matches[8]
        $rows += [pscustomobject]$cur; $cur = $null
    } elseif ($l -match '^MARK (.*)') { $rows += [pscustomobject]@{ mark = $Matches[1] } }
}
$segs = @(); $seg = $null
foreach ($r in $rows) {
    if ($r.mark) { $segs += [pscustomobject]@{ mask = "== $($r.mark)" }; $seg = $null; continue }
    if (-not $seg -or $seg.mask -ne $r.mask) {
        $seg = [pscustomobject]@{ mask = $r.mask; frames = 0; sim0 = $r.sim; ovr = $r.ovr; y1a = $r.y1; y1b = $r.y1; z1max = $r.z1;
                                  y2a = $r.y2; y2b = $r.y2; l2a = $r.l2; l2b = $r.l2; l1b = $r.l1; rA = $r.r; rB = $r.r }
        $segs += $seg
    }
    $seg.frames++; $seg.y1b = $r.y1; $seg.y2b = $r.y2; $seg.l2b = $r.l2; $seg.l1b = $r.l1; $seg.rB = $r.r
    if ($r.z1 -gt $seg.z1max) { $seg.z1max = $r.z1 }
}
$segs | Format-Table mask, frames, sim0, ovr, y1a, y1b, z1max, y2a, y2b, l2a, l2b, l1b, rA, rB -AutoSize | Out-String -Width 250
