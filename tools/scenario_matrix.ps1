# Smoothness scenarios (2026-11-04): two-instance rollback netplay fights across stages, simulated networks (delay,
# jitter, loss, stalls) and slower CPUs, with both games' PACE lines (frame pacing per session, src\rollback\FramePace.cpp)
# collected per fight into results\scenarios\<Tag>.csv; each scenario's logs are kept as results\scenarios\<Tag>_<name>_A/B.log.
# Usage: tools\scenario_matrix.ps1 -Set stages|network|cpu|all [-Tag base] [-Only name,...] [-Setup 'lazysave 0',...]
#        tools\scenario_matrix.ps1 -ParseOnly -Tag base   (rebuild the CSV and the summary from the kept logs)
param([string]$Set = 'all', [string]$Tag = 'run', [string[]]$Only = @(), [string[]]$Setup = @(), [switch]$ParseOnly,
      [int[]]$StageList = @(),   # every stage for the 'stages' set (default: the STAGES table of the first log, minus off/excluded)
      [int[]]$NetStages = @(7, 13, 3),  # the stages the network and CPU scenarios cycle through (0 is ChallengeTower: off)
[string[]]$PreSetup = @(),  # commands sent before 'netcode' (two_instance -PreSetup)
[string[]]$Dirs = @(),  # -Dirs build\clang_x,build\clang_x2: run another build without touching build\clang / clang2
[switch]$FromLoad)  # -FromLoad: sessions start at the map load (intro in lockstep, like MK9's own online start)
$ErrorActionPreference = 'Continue'
$root = Resolve-Path "$PSScriptRoot\.."
Set-Location $root
$out = "$root\results\scenarios"
New-Item -ItemType Directory -Force $out | Out-Null

function StagesFromLogs {
    foreach ($f in Get-ChildItem "$out\*_A.log" -ErrorAction SilentlyContinue | Sort-Object LastWriteTime -Descending) {
        $l = Select-String $f.FullName -Pattern 'STAGES (\d+):(.*)$' | Select-Object -First 1
        if (-not $l) { continue }
        # '(x)': in the random-pick list (0x00DEF5E8), '(off)': not selectable. The selectable stages random picks never
        # use (boss and special stages) go last, so a stage that misbehaves in attract cannot cut the sweep short.
        $ids = @(); $late = @()
        foreach ($m in [regex]::Matches($l.Matches[0].Groups[2].Value, ' (\d+)=([^ ]+)')) {
            $v = $m.Groups[2].Value
            if ($v -match '\(off\)|^\?$') { continue }
            if ($v -match '\(x\)') { $ids += [int]$m.Groups[1].Value } else { $late += [int]$m.Groups[1].Value }
        }
        return @($ids + $late)
    }
    return @()
}

# name, then two_instance.ps1 parameters (Fights defaults to the stage count)
function Scenarios {
    $all = @()
    if ($Set -in 'stages', 'all') {
        $st = if ($StageList.Count) { $StageList } else { StagesFromLogs }
        if (-not $st.Count) { Write-Host 'no stage list: run a scenario first (its log has the STAGES line) or pass -StageList'; }
        else { $all += @{ Name = 'stages_ns30_d2'; NetSim = 30; Delay = 2; Stages = $st } }
    }
    if ($Set -in 'network', 'all') {
        $all += @{ Name = 'lan_d1'; NetSim = 0; Delay = 1; Stages = $NetStages }
        $all += @{ Name = 'ns30_d2'; NetSim = 30; Delay = 2; Stages = $NetStages }
        $all += @{ Name = 'ns60_d2'; NetSim = 60; Delay = 2; Stages = $NetStages }
        $all += @{ Name = 'ns60_d3'; NetSim = 60; Delay = 3; Stages = $NetStages }
        $all += @{ Name = 'ns100_d3'; NetSim = 100; Delay = 3; Stages = $NetStages }
        $all += @{ Name = 'ns30_d2_loss5'; NetSim = 30; Delay = 2; NetLoss = 5; Stages = $NetStages }
        $all += @{ Name = 'ns30_d2_stall'; NetSim = 30; Delay = 2; NetStall = 150; NetStallEvery = 4000; Stages = $NetStages }
        $all += @{ Name = 'ns60_d2_jittery'; NetSim = 60; Delay = 2; NetSimOop = 5; NetLoss = 3; Stages = $NetStages }
        # a round trip of 300-440 ms (GGPO's simulated latency is 2/3..1 x the delay each way): the handshake's retries
        # outpace it (2026-11-06)
        $all += @{ Name = 'ns220_d4'; NetSim = 220; Delay = 4; Stages = $NetStages }
        # the delays the online mode's ping rule picks (ns100: round trip 133-200 ms -> 4), and larger ones past its cap of 4
        # for the 300-440 ms link (2026-11-06)
        $all += @{ Name = 'ns100_d4'; NetSim = 100; Delay = 4; Stages = $NetStages }
        $all += @{ Name = 'ns220_d5'; NetSim = 220; Delay = 5; Stages = $NetStages }
        $all += @{ Name = 'ns220_d6'; NetSim = 220; Delay = 6; Stages = $NetStages }
    }
    if ($Set -in 'combo', 'all') {
        # Scripted combos (two_instance -Combo, 2026-11-06): strings, launchers and juggles, specials (some enhanced),
        # throws, X-ray attempts; at the delays the online mode's ping rule picks for each link.
        $all += @{ Name = 'lan_d1_combo'; NetSim = 0; Delay = 1; Combo = $true; Stages = $NetStages }
        $all += @{ Name = 'ns30_d2_combo'; NetSim = 30; Delay = 2; Combo = $true; Stages = $NetStages }
        $all += @{ Name = 'ns60_d3_combo'; NetSim = 60; Delay = 3; Combo = $true; Stages = $NetStages }
        $all += @{ Name = 'ns100_d4_combo'; NetSim = 100; Delay = 4; Combo = $true; Stages = $NetStages }
        $all += @{ Name = 'ns30_d2_loss5_combo'; NetSim = 30; Delay = 2; NetLoss = 5; Combo = $true; Stages = $NetStages }
        $all += @{ Name = 'ns220_d6_combo'; NetSim = 220; Delay = 6; Combo = $true; Stages = $NetStages }
        # The same with damage (attract fights otherwise never lose life: KO, low health and round ends, 2026-11-07).
        $all += @{ Name = 'ns30_d2_combodmg'; NetSim = 30; Delay = 2; Combo = $true; Damage = $true; Stages = $NetStages }
        $all += @{ Name = 'ns60_d3_combodmg'; NetSim = 60; Delay = 3; Combo = $true; Damage = $true; Stages = $NetStages }
        $all += @{ Name = 'ns100_d4_combodmg'; NetSim = 100; Delay = 4; Combo = $true; Damage = $true; Stages = $NetStages }
        # X-ray starts under online rollback: full meters at the session start, X-ray attempts first (P1, then P2).
        $all += @{ Name = 'ns30_d2_xray'; NetSim = 30; Delay = 2; Combo = $true; Damage = $true; FullMeter = $true; Stages = $NetStages }
        $all += @{ Name = 'ns60_d3_xray'; NetSim = 60; Delay = 3; Combo = $true; Damage = $true; FullMeter = $true; Stages = $NetStages }
        $all += @{ Name = 'ns100_d4_xray'; NetSim = 100; Delay = 4; Combo = $true; Damage = $true; FullMeter = $true; Stages = $NetStages }
        # KOs and round ends under online rollback: both fighters at 25% life from session frame 8 (combo2's fights ended
        # at 42-65% life, 2026-11-07).
        $all += @{ Name = 'ns60_d3_ko'; NetSim = 60; Delay = 3; Combo = $true; Damage = $true; LowLife = 0.25; Stages = $NetStages }
        $all += @{ Name = 'ns100_d4_ko'; NetSim = 100; Delay = 4; Combo = $true; Damage = $true; LowLife = 0.25; Stages = $NetStages }
    }
    if ($Set -in 'stall', 'all') {
        # The 2026-11-07 field session's stalls, on game B only, every 10 s, with scripted combos at the field's link
        # (round trip ~50 ms, delay 2): B's whole game stopping 830 ms, and B's packets stopping 600 ms. Each with the
        # stall hold (default) and without it ('stallhold 0'): the frames after each stall are what differs.
        $all += @{ Name = 'ns30_d2_freezeB'; NetSim = 30; Delay = 2; Combo = $true; SetupB = @('freezeevery 830 10000'); Stages = $NetStages }
        $all += @{ Name = 'ns30_d2_freezeB_nohold'; NetSim = 30; Delay = 2; Combo = $true; SetupB = @('freezeevery 830 10000'); Setup = @('stallhold 0'); Stages = $NetStages }
        $all += @{ Name = 'ns30_d2_gapB'; NetSim = 30; Delay = 2; Combo = $true; SetupB = @('netsim 30 0 0 600 10000'); Stages = $NetStages }
        $all += @{ Name = 'ns30_d2_gapB_nohold'; NetSim = 30; Delay = 2; Combo = $true; SetupB = @('netsim 30 0 0 600 10000'); Setup = @('stallhold 0'); Stages = $NetStages }
        # B's packets lost (not held) for 300 ms every 10 s, a Wi-Fi uplink dropping out: the games recover only through
        # GGPO's input resend ('netretry', default 34 ms; 200 = stock, as in the field session's 5CD892F7).
        $all += @{ Name = 'ns30_d2_dropB'; NetSim = 30; Delay = 2; Combo = $true; SetupB = @('netsim 30 0 0 300 10000 1'); Stages = $NetStages }
        $all += @{ Name = 'ns30_d2_dropB_retry200'; NetSim = 30; Delay = 2; Combo = $true; SetupB = @('netsim 30 0 0 300 10000 1'); Setup = @('netretry 200'); Stages = $NetStages }
        # 400 ms: ends between two of the stock 200 ms resends (300 ends just before one, the stock's best case).
        $all += @{ Name = 'ns30_d2_dropB400'; NetSim = 30; Delay = 2; Combo = $true; SetupB = @('netsim 30 0 0 400 10000 1'); Stages = $NetStages }
        $all += @{ Name = 'ns30_d2_dropB400_retry200'; NetSim = 30; Delay = 2; Combo = $true; SetupB = @('netsim 30 0 0 400 10000 1'); Setup = @('netretry 200'); Stages = $NetStages }
        # The stall hold from 60 ms waits (default 100): with the 34 ms resend B's waits after a drop were 82-117 ms, under
        # the hold's threshold in about half the drops, and B then rolled back up to 8 frames after them.
        $all += @{ Name = 'ns30_d2_dropB_hold60'; NetSim = 30; Delay = 2; Combo = $true; SetupB = @('netsim 30 0 0 300 10000 1'); Setup = @('stallhold 60 150'); Stages = $NetStages }
        $all += @{ Name = 'ns30_d2_dropB400_hold60'; NetSim = 30; Delay = 2; Combo = $true; SetupB = @('netsim 30 0 0 400 10000 1'); Setup = @('stallhold 60 150'); Stages = $NetStages }
    }
    if ($Set -in 'cpu', 'all') {
        $all += @{ Name = 'ns30_d2_spinB6'; NetSim = 30; Delay = 2; TickSpinB = 6; Stages = $NetStages }
        $all += @{ Name = 'ns30_d2_spinB10'; NetSim = 30; Delay = 2; TickSpinB = 10; Stages = $NetStages }
        $all += @{ Name = 'ns30_d2_slowB'; NetSim = 30; Delay = 2; SlowB = $true; Stages = $NetStages }
    }
    if ($Only.Count) { $all = @($all | Where-Object { $Only -contains $_.Name }) }
    return $all
}

$paceRx = [regex]'PACE session gf=\d+: (\d+) frames in ([\d.]+) s = ([\d.]+) fps; frame ms avg ([\d.]+) p50 (\d+) p95 (\d+) p99 (\d+) max ([\d.]+), >=20ms (\d+) >=34ms (\d+) >=50ms (\d+); presented (\d+) \(([\d.]+)/s\) ms p50 (\d+) p95 (\d+) p99 (\d+) max ([\d.]+), >=20ms (\d+) >=34ms (\d+) >=50ms (\d+); boundary ms/frame livesave ([\d.]+) load ([\d.]+) replay ([\d.]+) replaysave ([\d.]+) wait ([\d.]+) extra ([\d.]+) other ([-\d.]+) total ([\d.]+) \(max ([\d.]+), max replay ([\d.]+), max wait ([\d.]+)\); rollbacks (\d+) depth avg ([\d.]+) max (\d+) \[([^\]]*)\]; replay saves skipped (\d+)'
$paceCols = 'Frames', 'Secs', 'Fps', 'FrameAvg', 'FrameP50', 'FrameP95', 'FrameP99', 'FrameMax', 'Frame20', 'Frame34', 'Frame50',
            'Presents', 'PresentRate', 'PresP50', 'PresP95', 'PresP99', 'PresMax', 'Pres20', 'Pres34', 'Pres50',
            'LiveSave', 'Load', 'Replay', 'ReplaySave', 'Wait', 'Extra', 'Other', 'Boundary', 'BoundaryMax', 'ReplayMax', 'WaitMax',
            'Rollbacks', 'DepthAvg', 'DepthMax', 'DepthHist', 'SavesSkipped'

# One record per session in a game's log: the stage picked before it, the fighters, the PACE session line and checksums.
function ParseLog([string]$path, [string]$scenario, [string]$game) {
    $recs = @()
    if (-not (Test-Path $path)) { return $recs }
    $stage = ''; $chars = ''; $cs = ''; $n = 0
    foreach ($line in Get-Content $path) {
        if ($line -match 'FIGHT at gf=100000: P1 (\S+), P2 ([^,\s]+)(?:, stage (\S+))?') {
            $chars = "$($Matches[1]) v $($Matches[2])"
            if ($Matches[3]) { $stage = $Matches[3] }
        }
        elseif ($line -match 'NETPLAY checksums: (\d+) frames matched the peer, (\d+) did not') { $cs = "$($Matches[1])/$($Matches[2])" }
        elseif ($line -match 'GGPO p2p stopped \(([^)]*)\)') { if ($recs.Count) { $recs[-1].Stop = $Matches[1] } }
        else {
            $m = $paceRx.Match($line)
            if (-not $m.Success) { continue }
            $n++
            $r = [ordered]@{ Scenario = $scenario; Game = $game; Session = $n; Stage = $stage; Fighters = $chars; Checksums = $cs; Stop = '' }
            for ($i = 0; $i -lt $paceCols.Count; $i++) { $r[$paceCols[$i]] = $m.Groups[$i + 1].Value }
            $recs += [pscustomobject]$r
            $cs = ''
        }
    }
    return $recs
}

function Summary($rows) {
    $rows | Group-Object Scenario, Game | ForEach-Object {
        $g = $_.Group
        $mins = ($g | Measure-Object Secs -Sum).Sum / 60
        $num = { param($c) ($g | ForEach-Object { [double]$_.$c } | Measure-Object -Sum).Sum }
        $frames = & $num Frames
        $wavg = { param($c) if ($frames) { [math]::Round((($g | ForEach-Object { [double]$_.$c * [double]$_.Frames }) | Measure-Object -Sum).Sum / $frames, 2) } else { 0 } }
        # A session stopped at frame 0 (the two demo fights did not start from the same state: harness, not netcode) is
        # counted apart from a desync during the fight.
        $start = @($g | Where-Object { $_.Stop -match 'frame 0 differs' }).Count
        $bad = @($g | Where-Object { $_.Stop -notmatch 'frame 0 differs' -and $_.Checksums -match '/(\d+)$' -and $Matches[1] -ne '0' }).Count
        [pscustomobject]@{
            Scenario = $g[0].Scenario; Game = $g[0].Game; Fights = $g.Count; Minutes = [math]::Round($mins, 1)
            Fps = [math]::Round($frames / [math]::Max(1, ($g | Measure-Object Secs -Sum).Sum), 1)
            'Hitch34/min' = [math]::Round((& $num Frame34) / [math]::Max(0.01, $mins), 1)
            'Hitch50/min' = [math]::Round((& $num Frame50) / [math]::Max(0.01, $mins), 1)
            'Pres34/min' = [math]::Round((& $num Pres34) / [math]::Max(0.01, $mins), 1)
            FrameP99 = ($g | Measure-Object FrameP99 -Maximum).Maximum; FrameMax = ($g | ForEach-Object { [double]$_.FrameMax } | Measure-Object -Maximum).Maximum
            'Rollbacks/min' = [math]::Round((& $num Rollbacks) / [math]::Max(0.01, $mins), 0); DepthAvg = & $wavg DepthAvg
            LiveSave = & $wavg LiveSave; Load = & $wavg Load; Replay = & $wavg Replay; ReplaySave = & $wavg ReplaySave; Wait = & $wavg Wait
            Boundary = & $wavg Boundary; Desynced = $bad; StartMismatch = $start
        }
    }
}
# One line per fight (both games): stage, fighters, the worse game's hitches, checksums.
function PerFight($rows) {
    $rows | Group-Object Scenario, Session | ForEach-Object {
        $g = $_.Group
        $a = $g | Where-Object Game -eq 'A' | Select-Object -First 1
        $b = $g | Where-Object Game -eq 'B' | Select-Object -First 1
        $w = { param($c) ($g | ForEach-Object { [double]$_.$c } | Measure-Object -Maximum).Maximum }
        [pscustomobject]@{
            Scenario = $g[0].Scenario; Fight = $g[0].Session; Stage = $g[0].Stage; Fighters = $g[0].Fighters
            Secs = [math]::Round((& $w Secs), 0); 'Hitch34 A/B' = "$($a.Frame34)/$($b.Frame34)"; 'Pres34 A/B' = "$($a.Pres34)/$($b.Pres34)"
            P99 = & $w FrameP99; Max = & $w FrameMax; Rollbacks = "$($a.Rollbacks)/$($b.Rollbacks)"; Depth = & $w DepthAvg
            Load = & $w Load; LiveSave = & $w LiveSave; Checksums = "$($a.Checksums) $($b.Checksums)"; Stop = $g[0].Stop
        }
    }
}

if (-not $ParseOnly) {
    foreach ($s in Scenarios) {
        if (Get-Process Overwatch* -ErrorAction SilentlyContinue) { 'another game is running: stopping the matrix'; break }
        $p = @{ Netplay = $true; NoAi = $true; Minutes = 0; VaryFighters = $true }
        if ($FromLoad) { $p.Remove('NoAi'); $p.FromLoad = $true }  # two_instance: FromLoad sends 'killai 2' itself
        foreach ($k in $s.Keys) { if ($k -ne 'Name') { $p[$k] = $s[$k] } }
        if (-not $p.ContainsKey('Fights')) { $p.Fights = $s.Stages.Count }
        $p.Minutes = 4 + 2 * $p.Fights
        if ($Setup.Count) { $p.Setup = @(@($s.Setup) | Where-Object { $_ }) + $Setup }  # a scenario's own Setup first
        if ($PreSetup.Count) { $p.PreSetup = $PreSetup }  # before 'netcode': e.g. 'rngtrace 1' (its hooks go in at install)
        $name = "${Tag}_$($s.Name)"
        "$(Get-Date -Format HH:mm:ss) $name : $($p.Fights) fights, stages $($s.Stages -join ',')"
        $gd = if ($Dirs.Count -eq 2) { $Dirs } else { @('build\clang', 'build\clang2') }
        if ($Dirs.Count -eq 2) { $p.Dirs = $Dirs }
        & .\tools\two_instance.ps1 @p *> "$out\$name.txt"
        Copy-Item "$($gd[0])\MK9Rollback.log" "$out\${name}_A.log" -ErrorAction SilentlyContinue
        Copy-Item "$($gd[1])\MK9Rollback.log" "$out\${name}_B.log" -ErrorAction SilentlyContinue
        $crash = @(Select-String "$out\${name}_A.log", "$out\${name}_B.log" -Pattern 'CRASH\? code|GGPO ASSERTION' -ErrorAction SilentlyContinue).Count
        "$(Get-Date -Format HH:mm:ss) $name : done$(if ($crash) { ", $crash CRASH/ASSERT line(s)" })"
    }
    Get-Process MKKE -ErrorAction SilentlyContinue | Stop-Process -Force
}

# Every kept log of this tag
$rows = @()
foreach ($f in Get-ChildItem "$out\${Tag}_*_A.log" -ErrorAction SilentlyContinue) {
    $sc = $f.BaseName.Substring($Tag.Length + 1) -replace '_A$', ''
    $rows += ParseLog $f.FullName $sc 'A'
    $rows += ParseLog ($f.FullName -replace '_A\.log$', '_B.log') $sc 'B'
}
$rows | Export-Csv -NoTypeInformation "$out\$Tag.csv"
$sum = Summary $rows
$sum | Export-Csv -NoTypeInformation "$out\${Tag}_summary.csv"
$sum | Format-Table -AutoSize | Out-String -Width 300
$pf = PerFight $rows
$pf | Export-Csv -NoTypeInformation "$out\${Tag}_fights.csv"
$pf | Format-Table -AutoSize | Out-String -Width 300
