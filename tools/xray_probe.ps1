# X-ray probe (2026-11-01): what an X-ray's cinematic creates and changes, for the rollback fix. One fresh game, an
# attract demo fight with the AI removed, P1 walked up to P2 with a full meter, then the candidate X-ray button masks
# until the X-ray shows: a cinematic object born or a fighter's motion-control slots changed ('cinewatch 1', CINE lines
# in the frames log), or P2 losing a big part of its life. MK9's X-ray is Block + Flip Stance: block is 0x8000, flip
# stance 0x1000 or 0x4000 (unconfirmed, docs/rea_pass1.md 3.4). With -Synctest, a second X-ray runs under GGPO SyncTest
# (a load and replay every frame) and the sync errors around it are reported.
# 2026-11-02 first run: no X-ray in 9 tries. P1 started on the right (y +163, P2 at -305) and 'hold right' walked it
# away; and 'hold' queues per player, so the press waited behind the walk. Now P1 walks towards P2 by the logged
# positions (the second coordinate is horizontal) and the queue is cleared before the press.
# Results: results\xray_probe\ (both logs and a summary).
param([string]$Dir = "$PSScriptRoot\..\build\clang", [switch]$Synctest, [int]$Attempts = 3, [int]$KismetTrace = 0)
$ErrorActionPreference = 'Continue'
$out = "$PSScriptRoot\..\results\xray_probe"
New-Item -ItemType Directory -Force $out | Out-Null
function Cmd([string[]]$lines) { Set-Content -Encoding ascii "$Dir\cmd.tmp" $lines; Move-Item "$Dir\cmd.tmp" "$Dir\MK9Rollback_cmd.txt" -Force; Start-Sleep -Milliseconds 300 }
function Frames { Get-Content "$Dir\MK9Rollback_frames.log" -ErrorAction SilentlyContinue }
function InFight { [bool]((Get-Content "$Dir\MK9Rollback_frames.log" -Tail 80 -ErrorAction SilentlyContinue) -match '^FRAME \d+ ') }
function LastGf {
    $s = Get-Content "$Dir\MK9Rollback_frames.log" -Tail 300 -ErrorAction SilentlyContinue | Select-String '^FRAME \d+ gf=(\d+)' | Select-Object -Last 1
    if ($s) { [int]$s.Matches[0].Groups[1].Value } else { -1 }
}
# The latest STATE line: P1 - P2 along the horizontal (second) coordinate, and P2's life.
function Fighters {
    $s = Get-Content "$Dir\MK9Rollback_frames.log" -Tail 3000 -ErrorAction SilentlyContinue |
        Select-String 'F1 v=1 life=\S+ pos=[-\d.]+,([-\d.]+),.*F2 v=1 life=([\d.]+) pos=[-\d.]+,([-\d.]+),' | Select-Object -Last 1
    if (-not $s) { return $null }
    $g = $s.Matches[0].Groups
    @{ Gap = [double]$g[1].Value - [double]$g[3].Value; Life2 = [double]$g[2].Value }
}
# Walk P1 towards P2 in short steps until they are close (or it stops getting closer).
function Approach {
    for ($k = 0; $k -lt 25; $k++) {
        $f = Fighters
        if (-not $f -or [Math]::Abs($f.Gap) -lt 110) { return $f }
        # Not $dir: PowerShell names ignore case, and Cmd would then write to "$Dir" = 4 (2026-11-02).
        $step = if ($f.Gap -gt 0) { 4 } else { 8 }  # 4 = left, 8 = right (absolute screen directions)
        Cmd @("hold 1 $step 15", 'hold 2 0 15')
        Start-Sleep -Milliseconds 700
    }
    Fighters
}
# One try: approach, full meters, the queue cleared, the mask held 8 frames. True when the X-ray showed.
function XrayTry([string]$m, [string]$label) {
    $a = Approach
    $before = @(Frames).Count
    $trace = if ($label -eq 'synctest' -and $KismetTrace -gt 0) { @("kismettrace $KismetTrace") } else { @() }  # KTRACE from the press on
    Cmd (@("mark xray $label $m (gap $(if ($a) { [int]$a.Gap } else { '?' }))", 'release 1', 'poke E8A990 3F800000', 'poke E8B61C 3F800000') + $trace + @("hold 1 $m 8"))
    Start-Sleep 6
    $new = @(Frames | Select-Object -Skip $before)
    $after = Fighters
    $hit = $a -and $after -and ($a.Life2 - $after.Life2) -ge 0.15
    # The cinematic controller (0x00C66E40) or a slot-2 class (0x00D58B08/18) in a fighter's motion slots; the slot-3
    # root motion (0x00D5A150) changes all the time, and cinematic track objects were also born without an X-ray.
    $cine = [bool]($new -match 'vt 00C66E40|vt 00D58B08|vt 00D58B18')
    # P1's meter after the press ('peek' logs nonzero dwords only): an X-ray empties it even when it misses.
    $logBefore = @(Get-Content "$Dir\MK9Rollback.log").Count
    Cmd 'peek E8A990 1'
    Start-Sleep 1
    $newLog = @(Get-Content "$Dir\MK9Rollback.log" | Select-Object -Skip $logBefore)
    $pk = $newLog -match 'PEEK 00E8A990 \+000 = ([0-9A-F]{8})'
    $meter = if ($pk) { [BitConverter]::ToSingle([BitConverter]::GetBytes([Convert]::ToUInt32(($pk[-1] -replace '.*= ([0-9A-F]{8}).*', '$1'), 16)), 0) }
             elseif ($newLog -match 'CMD peek E8A990') { 0.0 } else { -1.0 }  # -1: the peek was not processed (unknown)
    "  meter after: $(if ($meter -lt 0) { 'unknown' } else { [Math]::Round($meter, 3) })$(if ($meter -ge 0 -and $meter -lt 0.9) { ' (used)' })"
    "mask ${m} (gap $(if ($a) { [int]$a.Gap } else { '?' })): $(if ($cine) { 'cinematic controller in a motion slot' } else { 'no cinematic controller' }), births $(@($new -match 'CINE gf=\d+ BIRTH').Count), P2 life $(if ($a) { $a.Life2 } else { '?' }) -> $(if ($after) { $after.Life2 } else { '?' })"
    return ($cine -or $hit -or ($meter -ge 0 -and $meter -lt 0.1))
}
# Harness check: does a plain attack from this distance damage P2 at all?
function Sanity {
    foreach ($m in '40', '10') {
        $a = Approach
        Cmd @('release 1', "hold 1 $m 4")
        Start-Sleep 2
        $after = Fighters
        "sanity attack ${m} (gap $(if ($a) { [int]$a.Gap } else { '?' })): P2 life $(if ($a) { $a.Life2 } else { '?' }) -> $(if ($after) { $after.Life2 } else { '?' })"
    }
}

& "$PSScriptRoot\start_attract.ps1" -Dir $Dir
for ($w = 0; $w -lt 60 -and -not (Select-String "$Dir\MK9Rollback.log" -Pattern 'Hook initialized successfully' -Quiet); $w++) { Start-Sleep 1 }
Cmd @('lab 1', 'netcode rollback', 'cinewatch 1')
$masks = '9000', 'C000', 'A000', '8100', '8400', '8800', '5000'  # block 0x8000 + each candidate bit; 0x5000 without block
$found = $null
$tried = @()  # a fight that ends mid-list continues the list in the next one
for ($fight = 0; $fight -lt $Attempts -and -not $found; $fight++) {
    for ($i = 0; $i -lt 90 -and -not (InFight); $i++) { Start-Sleep 2 }
    if (-not (InFight)) { 'no fight'; break }
    Start-Sleep 6
    Cmd 'killai'
    Start-Sleep 1
    if ($fight -eq 0) { Sanity }
    foreach ($m in $masks) {
        if ($tried -contains $m) { continue }
        $tried += $m
        $r = XrayTry $m 'try'
        $r | Where-Object { $_ -is [string] }
        if ($r[-1] -eq $true) { $found = $m; "X-ray mask ${m}"; Start-Sleep 8; break }
        if (-not (InFight)) { break }
    }
}
if ($found -and $Synctest) {
    # The next fight: the same X-ray under SyncTest (every frame loaded and replayed, checksums compared). 2026-11-02 run 5:
    # the session started as the demo fight ended ("a load found no snapshot") and stopped at once: retried in the next fight.
    for ($try = 0; $try -lt 3; $try++) {
    for ($i = 0; $i -lt 90 -and (InFight); $i++) { Start-Sleep 2 }
    for ($i = 0; $i -lt 90 -and -not (InFight); $i++) { Start-Sleep 2 }
    if (InFight) {
        Start-Sleep 6
        Cmd 'killai'
        # cinewatch logs every cinematic object's changes each frame: off here, it slows SyncTest to a few frames a second.
        Cmd @('cinewatch 0', 'poke E8A990 3F800000', 'poke E8B61C 3F800000')  # before the session: SyncTest's loads would undo a later poke
        # The walk first, at full speed: under SyncTest (a few frames a second) run 6's walk outlasted the demo fight.
        $null = Approach
        Start-Sleep 1
        $logAt = @(Get-Content "$Dir\MK9Rollback.log").Count
        Cmd 'ggpo synctest 4'
        Start-Sleep 2
        if (@(Get-Content "$Dir\MK9Rollback.log" | Select-Object -Skip $logAt) -match 'GGPO synctest stopped') {
            "synctest stopped at its start (try $($try + 1)); next fight"
            continue
        }
        $r = XrayTry $found 'synctest'
        $r | Where-Object { $_ -is [string] }
        # Run 10: the X-ray whiffed (no cinematic). A second press in the same session would need a meter poke that the
        # session's loads undo, so the next fight gets the retry.
        Start-Sleep 3
        if (-not (@(Get-Content "$Dir\MK9Rollback.log" | Select-Object -Skip $logAt) -match 'MOTIONCTL gf=\d+: P\d slot 2 holds .*cinematic')) {
            Cmd 'ggpo off'
            "no X-ray cinematic under SyncTest (try $($try + 1)); next fight"
            continue
        }
        # 2026-11-02: the session ended ~60 frames into the X-ray. Now it runs on until 600 frames past the press (the
        # X-ray and its end, where the cinematic tears its actors down) or 4 minutes.
        $g0 = LastGf
        for ($i = 0; $i -lt 240 -and (LastGf) -lt $g0 + 600 -and (Get-Process MKKE -ErrorAction SilentlyContinue); $i++) { Start-Sleep 1 }
        "synctest frames after the press: $((LastGf) - $g0)"
        Cmd 'ggpo off'
        Start-Sleep 2
    }
    break
    }
}
Get-Process MKKE -ErrorAction SilentlyContinue | Stop-Process -Force
Start-Sleep 2
Copy-Item "$Dir\MK9Rollback.log" "$out\xray_probe.log" -Force
Copy-Item "$Dir\MK9Rollback_frames.log" "$out\xray_probe_frames.log" -Force
$f = Get-Content "$out\xray_probe_frames.log"
$m = Get-Content "$out\xray_probe.log"
"X-ray mask: $(if ($found) { $found } else { 'none found' })"
"CINE births: $(@($f -match 'CINE gf=\d+ BIRTH').Count), deaths: $(@($f -match 'CINE gf=\d+ DEATH').Count), changes: $(@($f -match 'CINE gf=\d+ CHANGE').Count), fighter lines: $(@($f -match 'CINE gf=\d+ P\d slots').Count)"
"MOTIONCTL lines: $(@($m -match 'MOTIONCTL').Count); sync errors: $(@($m -match 'GGPO SYNC ERROR').Count); crash lines: $(@($m -match 'CRASH\?|HANG\?').Count)"
# The X-ray restore (CinemaRestore, 2026-11-02): its per-load lines, deferred teardowns, and the first sync error's context.
"CINEMA lines: $(@($m -match 'CINEMA (load|class)').Count); deferred teardowns: $(@($f -match 'CINEDEFER defer').Count), released: $(@($f -match 'CINEDEFER release').Count)"
$m -match 'CINEMA load S=|CINEMA .*(not saved|collected)|CINEMA \d+ loads|MOTIONCTL .*cinematic' | Select-Object -First 16
"LEVELVIS lines: $(@($m -match 'LEVELVIS').Count), LEVELREADY lines: $(@($m -match 'LEVELREADY').Count), LEVELUNDO lines: $(@($m -match 'LEVELUNDO').Count)"
$m -match 'LEVELUNDO|LEVELVIS|LEVELREADY|CINEMA load S=\d+: .* held ' | Select-Object -First 12
"KISMET lines: $(@($m -match 'KISMET S=').Count); Kismet classes: $(@($m -match 'Kismet (sequence|object)').Count); refused loads: $(@($m -match 'nothing restored').Count)"
$m -match 'KISMET S=|no longer live|CINEMA \d+ loads' | Select-Object -First 12
$m -match 'GGPO SYNC ERROR|GGPO synctest stopped' | Select-Object -First 6
