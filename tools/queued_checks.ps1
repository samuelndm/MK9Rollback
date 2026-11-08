# Waits until the PC is free (unlocked, 4 min without input, no Overwatch or MK9 running), then keeps the display on (no Windows
# lock) while it runs the release checks in order:
#   0. tools\online_smoke.ps1 (the online mode's hooks install, arm and disarm)
#   1. two-instance netplay regression (3 fights, every fight armed by the script)
#   2. the same with -AutoRearm (fight 1 armed once; fights 2-3 must start through the online mode's re-arm path)
#   2a. the same with -DelayChange (mid-fight input delay changes on both games, 'sessiondelay')
#   2b. tools\menu_repro.ps1: title screen (Start) and an 8-minute menu walk armed like the launcher
#   3. tools\package.ps1, only when all of them pass
#   4. a 12-session SyncTest soak (-Soak 0 skips it)
# -Lockstep: the same gate for Safe mode, the launcher's default since 2026-10-30: the regressions and the menu walk
#   run lockstep at delay 3, plus one lockstep regression under simulated internet (-NetSim 60) and a peer loss
#   (B killed mid-fight; A must play on) before the zip.
# Usage: tools\queued_checks.ps1 [-FirstSeed 12101] [-Soak 12] [-SoakOnly]   (-SoakOnly: only step 4)
#        tools\queued_checks.ps1 -Lockstep -Soak 0
#        tools\queued_checks.ps1 -PeerLossOnly   (only the Safe mode peer loss check, after the wait)
#        tools\queued_checks.ps1 -LagMatrix [-LagOnly lag_ns120_d3,...]   (only the simulated-internet regressions,
#        results\lag_matrix.csv; each run's two game logs are kept as results\<run>_A.log / _B.log)
param([int]$FirstSeed = 12101, [int]$Soak = 12, [int]$MaxWaitHours = 16, [switch]$SoakOnly, [switch]$LagMatrix, [string[]]$LagOnly,
      [int]$RegressRepeat = 0,  # -RegressRepeat N: only the plain regression, N times (results\two_instance_regress_r<i>*)
      [string[]]$SoakSetup = @(),  # extra game commands for the soak, e.g. 'animtrace 1' (traced: frames logs kept)
      [switch]$MenuRepro,  # -MenuRepro: tools\menu_repro.ps1 armed, then -Passive (title screen + menus), then the plain regression
      [switch]$Lockstep,  # -Lockstep: the release gate in Safe mode (lockstep, delay 3)
      [switch]$PeerLossOnly,  # -PeerLossOnly: only the Safe mode peer loss check (results\lockstep_peerloss.txt), no zip
      [string]$MenuOnly = '',  # -MenuOnly <gate output>: that gate failed only its menu walk (e.g. the PC was locked): after
                               # the wait, only the menu walk, and the zip when it passes on the same DLL
      [switch]$Now)  # -Now: the user allowed a run right away; no wait for an idle PC
$ErrorActionPreference = 'Continue'
$root = Resolve-Path "$PSScriptRoot\.."
Set-Location $root
Add-Type -Name Idle -Namespace QueuedChecks -MemberDefinition @'
[StructLayout(LayoutKind.Sequential)] public struct L { public uint cb; public uint t; }
[DllImport("user32.dll")] public static extern bool GetLastInputInfo(ref L l);
[DllImport("user32.dll")] public static extern IntPtr GetForegroundWindow();
[DllImport("user32.dll")] public static extern int GetWindowThreadProcessId(IntPtr h, out int p);
[DllImport("kernel32.dll")] public static extern uint SetThreadExecutionState(int f);  // int: PowerShell reads 0x80000003 as a negative Int32
'@
function Free {
    $l = New-Object QueuedChecks.Idle+L
    $l.cb = 8
    [QueuedChecks.Idle]::GetLastInputInfo([ref]$l) | Out-Null
    $idle = [int](([Environment]::TickCount - $l.t) / 1000)
    $p = 0
    [QueuedChecks.Idle]::GetWindowThreadProcessId([QueuedChecks.Idle]::GetForegroundWindow(), [ref]$p) | Out-Null
    $fg = (Get-Process -Id $p -ErrorAction SilentlyContinue).Name
    # 4 min, under the display's 5-min idle timeout: at 5 min the display was already off and Windows' 15-min
    # away timer running, so the PC locked mid-run and the menu walk could not focus the game (2026-10-30).
    # No game running either: gamepad input does not reset the idle time, so a player in an MK9 match with a
    # controller looks idle, and the checks start by killing MKKE.
    return $fg -and $fg -ne 'LockApp' -and -not (Get-Process Overwatch*, MKKE -ErrorAction SilentlyContinue) -and $idle -ge 240
}
function Regression([string]$name, [hashtable]$extra) {  # $extra: named switches, e.g. @{ AutoRearm = $true }
    Get-Process MKKE -ErrorAction SilentlyContinue | Stop-Process -Force
    Start-Sleep 3
    $out = "results\$name.txt"
    $p = @{ Netplay = $true; NoAi = $true; Delay = 0; SlowB = $true; Fights = 3; Minutes = 15 }
    if ($Lockstep) { $p.Lockstep = $true; $p.Delay = 3 }  # what the launcher's Safe mode sends
    foreach ($k in $extra.Keys) { $p[$k] = $extra[$k] }  # e.g. Delay = 2 overrides the default
    & .\tools\two_instance.ps1 @p *> $out
    $r = @(Select-String $out -Pattern 'checksums: (\d+) frames matched the peer, (\d+) did not' -CaseSensitive)
    $ended = @(Select-String $out -Pattern 'fight \d: ended' -CaseSensitive).Count
    $bad = @($r | Where-Object { $_.Matches[0].Groups[2].Value -ne '0' }).Count
    $fails = @(Select-String $out -Pattern 'failed saves' -CaseSensitive | Where-Object { $_.Line -notmatch ' 0 failed saves' }).Count
    $crash = @(Select-String "build\clang\MK9Rollback.log", "build\clang2\MK9Rollback.log" -Pattern 'CRASH\? code' -CaseSensitive -ErrorAction SilentlyContinue).Count
    # Any session that ended other than at the fight's end: two_instance_regress 2026-10-28 passed with B's fight 2
    # cut at frame 415 ('peer disconnected' on A) and fight 3 never joined.
    $cut = @(Select-String $out -Pattern 'GGPO p2p stopped \((?!timeline restart\))' -CaseSensitive).Count
    # two_instance.ps1's arming-skew retry (at most once per fight): its 'peer did not join' stops, up to 2 per retry
    $retries = @(Select-String $out -Pattern 'arming skew retry' -SimpleMatch).Count
    $joinStops = @(Select-String $out -Pattern 'GGPO p2p stopped (the peer did not join)' -SimpleMatch).Count
    $cut -= [Math]::Min($joinStops, 2 * $retries)
    if ($retries) { Write-Host "$name : $retries arming-skew retr$(if ($retries -gt 1) { 'ies' } else { 'y' })" }
    Copy-Item build\clang\MK9Rollback.log "results\${name}_A.log" -ErrorAction SilentlyContinue  # the next run overwrites them
    Copy-Item build\clang2\MK9Rollback.log "results\${name}_B.log" -ErrorAction SilentlyContinue
    Write-Host "$name : fights ended $ended, checksum reports $($r.Count), mismatching $bad, with failed saves $fails, crashes $crash, cut sessions $cut"
    # The in-game status line (display only): its first-draw check and draw counters, game A
    Select-String "results\${name}_A.log" -Pattern 'OVERLAY (first draw|draws|disabled|not installed)' -ErrorAction SilentlyContinue | Select-Object -First 1 -Last 1 |
        ForEach-Object { Write-Host "$name : $($_.Line.Substring(15))" }
    # One checksum report per session and game (3 fights x 2): lag_ns40_d1 passed with fight 3's two missing.
    return ($ended -eq 3 -and $r.Count -ge 6 -and $bad -eq 0 -and $fails -eq 0 -and $crash -eq 0 -and $cut -eq 0)
}
function PeerLoss {
    # The peer quits mid-fight (B killed 10 s in): A must end the session and play on. 2026-10-30: GGPO's
    # disconnect rewind ended a lockstep session in "GGPO Assertion Failed" and exit.
    Get-Process MKKE -ErrorAction SilentlyContinue | Stop-Process -Force
    Start-Sleep 3
    & .\tools\two_instance.ps1 -Netplay -Lockstep -Delay 3 -NoAi -Fights 1 -Minutes 8 -KillBAfter 10 *> results\lockstep_peerloss.txt
    $pl = Get-Content results\lockstep_peerloss.txt
    Copy-Item build\clang\MK9Rollback.log results\lockstep_peerloss_A.log -ErrorAction SilentlyContinue
    # The 2 s after A's session stop: at least 1 s of fight simulated
    $run = $pl | Select-String 'frames after the session stop: (\d+) -> (\d+)' | Select-Object -First 1
    $ran = if ($run) { [int]$run.Matches[0].Groups[2].Value - [int]$run.Matches[0].Groups[1].Value } else { 0 }
    $ok = [bool]($pl -match 'instance A alive: True') -and $ran -ge 60 -and
          [bool](Select-String results\lockstep_peerloss_A.log -Pattern 'GGPO p2p stopped \((the peer disconnected|no synchronized input)' -Quiet) -and
          -not (Select-String results\lockstep_peerloss_A.log -Pattern 'GGPO Assertion|CRASH\? code' -Quiet)
    Write-Host "lockstep_peerloss : $(if ($ok) { 'PASS' } else { 'FAIL' }) (A ran $ran frames in the 2 s after its session stopped)"
    return $ok
}

$t0 = Get-Date
while (-not $Now -and -not (Free)) {
    if (((Get-Date) - $t0).TotalHours -ge $MaxWaitHours) { 'gave up waiting for a free PC'; return }
    Start-Sleep 30
}
"checks start $(Get-Date)"
# ES_CONTINUOUS | ES_SYSTEM_REQUIRED | ES_DISPLAY_REQUIRED: no display timeout, standby or lock while the games run
# (a video player's request; it ends with this PowerShell or the reset at the end).
[QueuedChecks.Idle]::SetThreadExecutionState(0x80000003) | Out-Null
Get-Process MKKE -ErrorAction SilentlyContinue | Stop-Process -Force
Start-Sleep 3
if ($LagMatrix) {
    # Simulated internet (GGPO's network simulator in both games, 'netsim'): one 3-fight regression per
    # (one-way send delay ms, input delay frames). Each must pass like the plain regression; the rollback
    # cost (loads, replayed frames, barrier waits) per run goes to results\lag_matrix.csv.
    $rows = @()
    foreach ($c in @(@{ NetSim = 40; Delay = 0 }, @{ NetSim = 40; Delay = 1 }, @{ NetSim = 40; Delay = 2 }, @{ NetSim = 80; Delay = 1 },
                     @{ NetSim = 80; Delay = 2 },
                     @{ NetSim = 80; Delay = 3 }, @{ NetSim = 120; Delay = 3 }, @{ NetSim = 80; Delay = 2; NetSimOop = 5 })) {
        $name = "lag_ns$($c.NetSim)_d$($c.Delay)$(if ($c.NetSimOop) { "_oop$($c.NetSimOop)" })"
        if ($LagOnly -and $LagOnly -notcontains $name) { continue }
        $ok = Regression $name $c
        $stops = @(Select-String "results\$name.txt" -Pattern 'GGPO p2p stopped \(timeline restart\): (\d+) frames, (\d+) loads, (\d+) replayed frames, (\d+) barrier waits' -CaseSensitive)
        $sum = { param($g) ($stops | ForEach-Object { [int]$_.Matches[0].Groups[$g].Value } | Measure-Object -Sum).Sum }
        $rows += [pscustomobject]@{ Run = $name; Pass = $ok; Frames = & $sum 1; Loads = & $sum 2; Replayed = & $sum 3; BarrierWaits = & $sum 4
                                    NetsimLogged = [bool](Select-String 'build\clang\MK9Rollback.log' -Pattern 'NETSIM: sends delayed' -SimpleMatch -Quiet) }
    }
    $rows | Export-Csv -NoTypeInformation "results\lag_matrix$(if ($LagOnly) { '_rerun' }).csv"
    $rows | Format-Table -AutoSize | Out-String
    Get-Process MKKE -ErrorAction SilentlyContinue | Stop-Process -Force
    return
}
if ($MenuRepro) {  # 2026-10-29 menu freezes: armed like the launcher, then the control, then one plain regression
    & .\tools\menu_repro.ps1 -Minutes 10 -Name menu_repro_armed
    & .\tools\menu_repro.ps1 -Minutes 6 -Passive -Name menu_repro_passive
    $ok = Regression 'two_instance_regress' @{ Delay = 2 }
    "plain regression (delay 2): $(if ($ok) { 'PASS' } else { 'FAIL' })"
    Get-Process MKKE -ErrorAction SilentlyContinue | Stop-Process -Force
    return
}
if ($RegressRepeat -gt 0) {  # the plain regression N times, logs kept per run (hunting a rare failure), no zip
    $pass = 0
    for ($r = 1; $r -le $RegressRepeat; $r++) { if (Regression "two_instance_regress_r$r" @{}) { $pass++ } }
    "plain regression repeated: $pass of $RegressRepeat passed"
    Get-Process MKKE -ErrorAction SilentlyContinue | Stop-Process -Force
    return
}
if ($PeerLossOnly) {  # only the Safe mode peer loss check, no zip
    PeerLoss | Out-Null
    Get-Process MKKE -ErrorAction SilentlyContinue | Stop-Process -Force
    return
}
if ($MenuOnly) {
    # 2026-11-05 04:06 gate: every check passed but the menu walk, which could not focus the game behind the lock screen.
    $g = Get-Content $MenuOnly -ErrorAction SilentlyContinue
    $start = $g | Select-String '^checks start (.+)$' | Select-Object -First 1
    $onlyMenu = [bool]($g -match 'CHECKS FAILED \(online smoke True, regression True, auto re-arm True, delay changes True, menu walk False, lockstep lag and peer loss True\)')
    $dlls = Get-Item build\clang\MK9Rollback.dll, build\clang2\MK9Rollback.dll
    $same = $start -and -not ($dlls | Where-Object { $_.LastWriteTime -gt [datetime]::Parse($start.Matches[0].Groups[1].Value, [Globalization.CultureInfo]::InvariantCulture) })
    if (-not ($onlyMenu -and $same)) { "MENU ONLY: $MenuOnly is not a gate that failed only its menu walk on these DLLs: no zip"; return }
    Get-Process MKKE -ErrorAction SilentlyContinue | Stop-Process -Force
    Start-Sleep 3
    $m = try { @(& .\tools\menu_repro.ps1 -Minutes 8 -Name menu_repro -Safe:$Lockstep) } catch { @("FAIL: $($_.Exception.Message)") }
    $m | ForEach-Object { Write-Host "menu_repro : $_" }
    $mk = [bool]($m -match 'no hang or crash') -and -not ($m -match 'FROZE|FAIL') -and [bool]($m -match 'game responding: True')
    Get-Process MKKE -ErrorAction SilentlyContinue | Stop-Process -Force
    Start-Sleep 2
    if ($mk) { "THE MENU WALK PASSES (the other checks passed in $MenuOnly)"; & .\tools\package.ps1 } else { "MENU WALK FAILED: no zip" }
    [QueuedChecks.Idle]::SetThreadExecutionState(0x80000000) | Out-Null
    return
}
if ($SoakOnly) {
    "soak start $(Get-Date) (checks skipped: -SoakOnly)"
    & .\tools\synctest_soak.ps1 -Sessions $Soak -Minutes 13 -FirstSeed $FirstSeed -Setup $SoakSetup *> results\synctest_soak.txt
    "soak end $(Get-Date)"
    Get-Content results\synctest_soak\summary.csv
    return
}
$smoke = & .\tools\online_smoke.ps1
$smoke | Set-Content -Encoding ascii results\online_smoke.txt
$smoke
$s = [bool]($smoke -match '^PASS')
$a = Regression 'two_instance_regress' @{ Shot = "$root\results\two_instance_regress_overlay.png" }  # the status line, if the desktop is visible
# An array (@('-AutoRearm')) splats positionally: it became -Seed '-AutoRearm' and the run re-armed by script (2026-10-26).
$b = Regression 'two_instance_autorearm' @{ AutoRearm = $true }
$b = $b -and [bool](Select-String results\two_instance_autorearm.txt -Pattern 'not armed by the script' -SimpleMatch -Quiet)
foreach ($d in 'build\clang', 'build\clang2') {  # each game re-armed itself for fights 2 and 3
    $n = @(Select-String "$d\MK9Rollback.log" -Pattern 'NETPLAY re-armed automatically' -SimpleMatch -ErrorAction SilentlyContinue).Count
    Write-Host "two_instance_autorearm : $d re-armed itself $n time(s)"
    $b = $b -and $n -ge 2
}
# Mid-fight input delay changes on both games (the online mode's between-rounds change, 2026-11-01): the patched GGPO
# sends the frames an increase pads; checksums must agree like in the plain regression.
$dc = Regression 'two_instance_delaychange' @{ DelayChange = $true }
$changes = @(Select-String results\two_instance_delaychange_A.log, results\two_instance_delaychange_B.log -Pattern 'GGPO p2p frame delay' -SimpleMatch -ErrorAction SilentlyContinue).Count
Write-Host "two_instance_delaychange : $changes delay change(s) applied (7 scheduled per fight)"
$dc = $dc -and $changes -ge 12
# Title screen and menus, armed like the launcher (2026-10-29: both players' games froze there; no other check
# passes the title screen). Required for a zip.
Get-Process MKKE -ErrorAction SilentlyContinue | Stop-Process -Force
Start-Sleep 3
# desk.ps1's Focus throws when the game cannot get the foreground: a FAIL here, not the end of the gate.
$m = try { @(& .\tools\menu_repro.ps1 -Minutes 8 -Name menu_repro -Safe:$Lockstep) }
     catch { @("FAIL: $($_.Exception.Message)") }
$m | ForEach-Object { Write-Host "menu_repro : $_" }
$mk = [bool]($m -match 'no hang or crash') -and -not ($m -match 'FROZE|FAIL') -and [bool]($m -match 'game responding: True')
$lag = $true
if ($Lockstep) {  # Safe mode waits for every remote input: check it under internet-like delays too
    $lag = Regression 'lockstep_ns60' @{ NetSim = 60 }
    Write-Host "lockstep_ns60 : $(if ($lag) { 'PASS' } else { 'FAIL' })"
    $pl = PeerLoss
    $lag = $lag -and $pl
}
if ($s -and $a -and $b -and $dc -and $mk -and $lag) {
    "ONLINE SMOKE, THE REGRESSIONS (DELAY CHANGES INCLUDED) AND THE MENU WALK PASS$(if ($Lockstep) { ' (LOCKSTEP, UNDER SIMULATED LAG AND A PEER LOSS)' })"
    Get-Process MKKE -ErrorAction SilentlyContinue | Stop-Process -Force
    Start-Sleep 2
    & .\tools\package.ps1
} else {
    "CHECKS FAILED (online smoke $s, regression $a, auto re-arm $b, delay changes $dc, menu walk $mk, lockstep lag and peer loss $lag): no zip"
}
Get-Process MKKE -ErrorAction SilentlyContinue | Stop-Process -Force
[QueuedChecks.Idle]::SetThreadExecutionState(0x80000000) | Out-Null  # the display may turn off again
if ($Soak -gt 0) {
    Start-Sleep 3
    "soak start $(Get-Date)"
    & .\tools\synctest_soak.ps1 -Sessions $Soak -Minutes 13 -FirstSeed $FirstSeed -Setup $SoakSetup *> results\synctest_soak.txt
    "soak end $(Get-Date)"
    Get-Content results\synctest_soak\summary.csv
}
