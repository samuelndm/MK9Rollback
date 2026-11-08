# Classify each rollback's first NATIVETRACE / RNGTRACE divergence by its offset from the
# snapshot frame S (0 = first resimulated frame: the load did not restore something the frame
# reads; >0 = state drifted during resimulation), and group by native.
param([string[]]$Logs)
$rows = @()
foreach ($f in $Logs) {
    $pending = @()
    foreach ($line in Get-Content $f) {
        if ($line -match 'NATIVETRACE k=(\d+) gf=(\d+): call #\d+ of \d+/\d+ differs: original native (\w+) exec \w+ -> (\w+), resim native (\w+) exec \w+ -> (\w+)') {
            $pending += [pscustomobject]@{ k = [int]$Matches[1]; gf = [int]$Matches[2]; orig = $Matches[3]; res = $Matches[5]; ov = $Matches[4]; rv = $Matches[6] }
        } elseif ($line -match 'ROLLBACK gameplay (exact|MISMATCH)') {
            $mm = $Matches[1] -eq 'MISMATCH'
            if ($pending.Count) {
                $first = $pending[0]
                $rows += [pscustomobject]@{ log = (Split-Path $f -Leaf); k = $first.k; offset = $first.gf - ($first.gf - 0); mismatch = $mm; first = $first; n = $pending.Count }
            } else {
                $rows += [pscustomobject]@{ log = (Split-Path $f -Leaf); k = 0; offset = -1; mismatch = $mm; first = $null; n = 0 }
            }
            $pending = @()
        } elseif ($line -match 'ROLLBACK \d+ frames: gf (\d+) -> (\d+) -> \d+') {
            $S = [int]$Matches[2]
            # fix offsets of the rollback just classified
            if ($rows.Count -and $rows[-1].first -and $rows[-1].offset -ne -1) { $rows[-1].offset = $rows[-1].first.gf - $S }
        }
    }
}
"rollbacks=$($rows.Count) mismatches=$(@($rows | ? mismatch).Count)"
"--- mismatching rollbacks: first divergence offset from S (-1 = no trace divergence)"
$rows | ? mismatch | Group-Object offset | Sort-Object { [int]$_.Name } | % { '{0,4} at offset {1}' -f $_.Count, $_.Name }
"--- mismatching rollbacks: first divergence native (orig->resim) and offset"
$rows | ? { $_.mismatch -and $_.first } | % { '{0}->{1} @S+{2}' -f $_.first.orig, $_.first.res, $_.offset } | Group-Object | Sort-Object Count -Descending | Select -First 15 | % { '{0,4} {1}' -f $_.Count, $_.Name }
"--- exact rollbacks that still had a trace divergence: $(@($rows | ? { -not $_.mismatch -and $_.first }).Count)"
