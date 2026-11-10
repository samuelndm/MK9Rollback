# Classifies the checksum mismatches of two-instance scenario logs (results\scenarios\<tag>*_A.log / _B.log), fight by
# fight, so a real state desync is told apart from the harness's attract-demo start cases without reading the logs.
# Usage: tools\desync_digest.ps1 [-Filter 'r3f*'] [-All] [-Root results\scenarios]
#   first   which fields of the two games' first "NETPLAY DESYNC local frame" lines differ:
#             frame-0 stop   the games' first fight frames were not the same intro frame (known harness case)
#             rng            only RNG A: one intro draw a frame apart (known harness case)
#             pos / life / meter   the fighters themselves: read 'later'
#   later   the "NETPLAY STATE" samples (every 300 frames; live, so a single sample can be a prediction):
#             same           every sample equal: the games ran the same fight
#             offset N       the same fight, both games' fighters a constant N units apart (N < 0.5) in most samples
#             positions      positions apart by more, life and meter equal
#             LIFE/METER     the fights went different ways: a desync a player would see
# -All lists the clean fights as well. Exit code: the number of LIFE/METER fights.
param([string]$Filter = '*', [string]$Root = "$PSScriptRoot\..\results\scenarios", [switch]$All)

function Read-Fights([string]$path) {
    # One entry per session: stage, checksum totals, loads, the first mismatch's fields and the state samples.
    $new = { @{ stage = '?'; frame = $null; detail = $null; state = @{}; loads = -1; ok = 0; bad = 0 } }
    $fights = @(); $cur = & $new
    foreach ($l in [IO.File]::ReadLines($path)) {
        if ($l -notmatch 'NETPLAY|FIGHT at|GGPO p2p stopped') { continue }
        if ($l -match 'FIGHT at gf=1000\d\d: .* stage (\S+)') { $cur.stage = $Matches[1] -replace '^BGND_', '' }
        elseif ($l -match 'NETPLAY STATE frame (\d+): life (\S+) (\S+) pos (\S+) (\S+) (\S+) / (\S+) (\S+) (\S+) meter (\S+) (\S+)') {
            $cur.state[[int]$Matches[1]] = @($Matches[2..11] | ForEach-Object { [double]::Parse($_, [Globalization.CultureInfo]::InvariantCulture) })
        }
        elseif ($null -eq $cur.frame -and $l -match 'NETPLAY DESYNC: frame (\d+) ') { $cur.frame = [int]$Matches[1] }
        elseif (-not $cur.detail -and $l -match 'NETPLAY DESYNC\s+local frame (\d+): life (.+?) pos (.+?) meter (.+?) rng (\S+)') {
            $cur.detail = @{ frame = [int]$Matches[1]; life = $Matches[2]; pos = $Matches[3]; meter = $Matches[4]; rng = $Matches[5] }
        }
        elseif ($l -match 'NETPLAY checksums: (\d+) frames matched the peer, (\d+) did not') {
            $cur.ok = [int]$Matches[1]; $cur.bad = [int]$Matches[2]
            $fights += $cur; $cur = & $new
        }
        elseif ($fights.Count -and $l -match 'GGPO p2p stopped[^:]*: \d+ frames, (\d+) loads') { $fights[-1].loads = [int]$Matches[1] }
    }
    return $fights
}

function Get-First($a, $b) {
    if (-not $a.bad) { return '-' }
    if ($a.frame -le 1 -and $a.ok -le 1) { return 'frame-0 stop' }
    if (-not $a.detail -or -not $b -or -not $b.detail) { return 'no detail' }
    if ($a.detail.frame -ne $b.detail.frame) { return "details at $($a.detail.frame) / $($b.detail.frame)" }
    $diff = @('life', 'pos', 'meter', 'rng' | Where-Object { $a.detail[$_] -ne $b.detail[$_] })
    if ($diff.Count) { return $diff -join '+' }
    return 'no logged field'
}

function Get-Later($a, $b) {
    $frames = @($a.state.Keys | Where-Object { $_ -ge 300 -and $b.state.ContainsKey($_) } | Sort-Object)
    if (-not $frames.Count) { return 'no samples' }
    $small = 0; $large = 0; $other = 0; $max = 0.0
    foreach ($f in $frames) {
        $x = $a.state[$f]; $y = $b.state[$f]
        $pos = (2..7 | ForEach-Object { [Math]::Abs($x[$_] - $y[$_]) } | Measure-Object -Maximum).Maximum
        $rest = (0, 1, 8, 9 | ForEach-Object { [Math]::Abs($x[$_] - $y[$_]) } | Measure-Object -Maximum).Maximum
        if ($rest -gt 0) { $other++ } elseif ($pos -gt 0.5) { $large++ } elseif ($pos -gt 0) { $small++; $max = [Math]::Max($max, $pos) }
    }
    # In a fight whose checksums all agree a differing sample is a prediction caught live; so is a single one elsewhere.
    if (-not $a.bad) { return 'same' }
    if ($other) { return 'LIFE/METER' }
    if ($large -gt 1) { return 'positions' }
    if ($small -gt 1) { return 'offset ' + $max.ToString('0.000', [Globalization.CultureInfo]::InvariantCulture) }
    return 'same'
}

$seen = 0; $real = 0
Get-ChildItem (Join-Path $Root "${Filter}_A.log") -ErrorAction SilentlyContinue | Sort-Object LastWriteTime | ForEach-Object {
    $bPath = $_.FullName -replace '_A\.log$', '_B.log'
    if (-not (Test-Path $bPath)) { return }
    $a = @(Read-Fights $_.FullName); $b = @(Read-Fights $bPath)
    $name = $_.Name -replace '_A\.log$', ''
    for ($i = 0; $i -lt $a.Count; $i++) {
        $fb = if ($i -lt $b.Count) { $b[$i] } else { $null }
        $first = Get-First $a[$i] $fb
        $later = if ($fb) { Get-Later $a[$i] $fb } else { 'no game B' }
        $seen++
        if ($later -eq 'LIFE/METER') { $real++ }
        if (-not $All -and -not $a[$i].bad -and $later -eq 'same') { continue }
        '{0,-40} {1} {2,-12} {3,5}/{4,-5} loads {5,3}/{6,-3} first {7,-5} {8,-14} later {9}' -f $name, ($i + 1), $a[$i].stage, $a[$i].ok,
            $a[$i].bad, $a[$i].loads, $(if ($fb) { $fb.loads } else { -1 }), $a[$i].frame, $first, $later
    }
}
"$seen fights read, $real with life or meter apart"
exit $real
