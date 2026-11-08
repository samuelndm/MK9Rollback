# Two MKKE instances on this PC, each with its own copy of the DLL (build\clang and build\clang2: separate logs and
# command files). 'scriptseed' makes both pick the same attract demo fights (FIGHT lines).
# -Netplay: a real two-game session. Both arm netplay for the next demo fight (A = P1 on UDP 7000, B = P2 on 7001,
# checksum channels 7100/7101). At its first frame both pause, seed their RNGs and wait for each other; the AI is
# removed while paused, then each instance drives only its own player with random input. Per-frame checksums are
# compared between the two games (NETPLAY checksums / NETPLAY DESYNC).
# -FromLoad: the session starts at the map load (timeline restart) and the AI is removed whenever it appears.
# -KillBAfter N: N seconds into the first netplay fight instance B is killed (peer loss); A is then checked.
#   -KillBAfter -1: B is killed right after arming (the peer never joins).
# -GamePauseAfter N: N seconds into the first netplay fight, A's pause setter is called as the game calls it (pause guard).
# -NetSim MS [-NetSimOop P]: GGPO's network simulator in both games ('netsim'): sends delayed 2/3..1 x MS, P% out of order.
# -DelayChange: mid-fight input delay changes in every netplay fight ('sessiondelay', the online mode's between-rounds
#   change): up and down on both games, a jump of 3 included. The checksums must still agree (patched GGPO sends the
#   frames an increase pads).
# Usage: tools\two_instance.ps1 [-Netplay] [-Lockstep] [-FromLoad] [-Delay 2] [-Fights 2] [-Minutes 8] [-Seed 1234ABCD]
param([string]$Seed = '1234ABCD', [int]$Minutes = 8, [string[]]$Setup = @(), [switch]$Netplay, [switch]$Lockstep,
      [string[]]$PreSetup = @(),  # -PreSetup 'lab 1': commands sent first, before 'netcode' (patch sets are chosen at install)
      [int]$Delay = 2, [int]$Fights = 2, [int]$InputSeed = 4242, [switch]$FromLoad, [switch]$NoAi, [switch]$SlowB, [switch]$KeepRunning, [int]$KillBAfter = 0, [int]$GamePauseAfter = 0,
      [switch]$AutoRearm,  # -AutoRearm: arm fight 1 only, with 'netplayrearm 1' (the online mode's re-arm path starts the later fights)
      [string]$Shot = '',  # -Shot out.png: a screenshot 20 s into fight 1 (the status line), desktop only
      [int]$NetSim = 0, [int]$NetSimOop = 0,  # -NetSim 60: each game's GGPO packets wait 40..60 ms (simulated internet; -NetSimOop %: out of order)
      [int]$NetLoss = 0, [int]$NetStall = 0, [int]$NetStallEvery = 0,  # -NetLoss %: packets dropped; -NetStall ms every -NetStallEvery ms: nothing sent
      [int[]]$Stages = @(),  # -Stages 3,7,12: netplay fight i on stage table entry Stages[(i-1) % n] ('forcestage'; 'stages' logs the table)
      [switch]$VaryFighters,  # other attract fighters each netplay fight ('pickvariety i')
      [int]$TickSpinA = 0, [int]$TickSpinB = 0,  # ms of extra CPU per session frame in game A / B ('tickspin': a slower PC)
      [switch]$DelayChange,
      [switch]$Combo,  # -Combo: scripted combos instead of random input (tools\combo_inputs.ps1), one attacker per batch
      [switch]$Damage,  # -Damage: attract fights take damage ('demodamage 1' when arming): KOs, round ends
      [switch]$FullMeter,  # -FullMeter: both meters full at session frame 8 ('netplaymeter 1' when arming) and, with
                           # -Combo, X-ray attempts first (P1, then P2; the defender stands): X-ray starts under online rollback
      [double]$LowLife = -1,  # -LowLife 0.25: both fighters' life at session frame 8 ('netplaylife'): KOs, round ends
      [string[]]$SetupB = @(),  # -SetupB: commands for game B only, after -Setup (e.g. 'freezeevery 830 10000', or a
                                # 'netsim ...' of its own: a stall of B's sends only, as in the 2026-11-07 field session)
      [string[]]$Dirs = @())  # -Dirs build\clang_x,build\clang_x2: game A and B folders (default build\clang, build\clang2);
                              # A's DLL and injector are copied to B, as with the defaults
$ErrorActionPreference = 'Continue'
if ($Combo) { . "$PSScriptRoot\combo_inputs.ps1" }
$root = Resolve-Path "$PSScriptRoot\.."
$dirs = if ($Dirs.Count -eq 2) { @($Dirs | ForEach-Object { if ([IO.Path]::IsPathRooted($_)) { $_ } else { "$root\$_" } }) } else { @("$root\build\clang", "$root\build\clang2") }
$game = 'D:\Jogos\Steam\SteamLibrary\steamapps\common\MortalKombat_KompleteEdition\DiscContentPC'
function Cmd([string]$dir, [string[]]$lines) {
    for ($w = 0; $w -lt 20 -and (Test-Path "$dir\MK9Rollback_cmd.txt"); $w++) { Start-Sleep -Milliseconds 100 }  # previous batch not taken yet
    Set-Content -Encoding ascii "$dir\cmd.tmp" $lines
    Move-Item "$dir\cmd.tmp" "$dir\MK9Rollback_cmd.txt" -Force
    Start-Sleep -Milliseconds 200
}
function InFight([string]$dir) {
    $f = Get-Item "$dir\MK9Rollback_frames.log" -ErrorAction SilentlyContinue
    if (-not $f -or ((Get-Date) - $f.LastWriteTime).TotalSeconds -gt 3) { return $false }
    [bool]((Get-Content $f.FullName -Tail 80) -match '^FRAME \d+ ')
}
function LogHas([string]$dir, [string]$pattern) { Select-String "$dir\MK9Rollback.log" -Pattern $pattern -Quiet }

Get-Process MKKE -ErrorAction SilentlyContinue | Stop-Process -Force
Start-Sleep 3
New-Item -ItemType Directory -Force $dirs[1] | Out-Null
Copy-Item "$($dirs[0])\MK9Rollback.dll" "$($dirs[1])\MK9Rollback.dll" -Force
foreach ($d in $dirs) { Remove-Item "$d\MK9Rollback*.log", "$d\MK9Rollback_cmd.txt" -ErrorAction SilentlyContinue }

# Instance A through Steam (initializes the Steam client), instance B started directly from the game folder.
& "$PSScriptRoot\launch_game.ps1" | Where-Object { $_ -is [string] } | Select-Object -Last 1
Start-Sleep 15
Start-Process "$game\MKKE.exe" -WorkingDirectory $game
for ($w = 0; $w -lt 60 -and @(Get-Process MKKE -ErrorAction SilentlyContinue).Count -lt 2; $w++) { Start-Sleep 2 }
$procs = @(Get-Process MKKE -ErrorAction SilentlyContinue | Sort-Object StartTime)
if ($procs.Count -lt 2) { "only $($procs.Count) MKKE instance(s) running"; return }
"instances: A pid $($procs[0].Id), B pid $($procs[1].Id)"
if ($SlowB) {  # very different timing: B on one core at below-normal priority
    $procs[1].ProcessorAffinity = [IntPtr]1
    $procs[1].PriorityClass = 'BelowNormal'
    'instance B: one core, below-normal priority'
}
Start-Sleep 5
for ($k = 0; $k -lt 2; $k++) {
    for ($try = 1; $try -le 6 -and -not (Test-Path "$($dirs[$k])\MK9Rollback.log"); $try++) {
        & "$($dirs[0])\MK9RollbackInjector.exe" inject "$($dirs[$k])\MK9Rollback.dll" $procs[$k].Id | Select-Object -Last 1
        for ($w = 0; $w -lt 10 -and -not (Test-Path "$($dirs[$k])\MK9Rollback.log"); $w++) { Start-Sleep 1 }
    }
    if (-not (Test-Path "$($dirs[$k])\MK9Rollback.log")) { "instance $k INJECTION FAILED"; return }
}
Start-Sleep 3
# 'netplaydemo 1': the netplay fights here are the attract demos (CPU vs CPU), which players' games never use.
# Netplay: the launcher's mode command (the player patch set), 'netcode safe' with -Lockstep.
foreach ($d in $dirs) { Cmd $d ($PreSetup + @("scriptseed $Seed") + $(if ($Netplay) { @($(if ($Lockstep) { 'netcode safe' } else { 'netcode rollback' }), 'netplaydemo 1') } else { @() }) + $(if ($NetSim -gt 0 -or $NetSimOop -gt 0 -or $NetLoss -gt 0 -or $NetStall -gt 0) { @("netsim $NetSim $NetSimOop $NetLoss $NetStall $NetStallEvery") } else { @() }) + $(if ($FromLoad) { @('netplayload 1', 'killai 2') } elseif ($NoAi) { @('killai 2') } else { @() }) + $(if ($Stages.Count) { @('stages', "forcestage $($Stages[0])") } else { @() }) + $Setup + $(if ($d -eq $dirs[1]) { $SetupB } else { @() })) }
if ($TickSpinA -gt 0) { Cmd $dirs[0] "tickspin $TickSpinA" }
if ($TickSpinB -gt 0) { Cmd $dirs[1] "tickspin $TickSpinB" }

$end = (Get-Date).AddMinutes($Minutes)
if ($Netplay) {
    $rng = [Random]::new($InputSeed)
    $dirMoves = 0, 0, 4, 8, 4, 8, 2, 1, 6, 10
    $atk = 0, 0, 0, 0x10, 0x20, 0x40, 0x80, 0x8000
    function Chunk { $m = $dirMoves[$rng.Next($dirMoves.Count)] -bor $atk[$rng.Next($atk.Count)]; '{0:X} {1}' -f $m, (4 + $rng.Next(20)) }
    $retried = 0  # the fight that had an arming-skew retry
    for ($fight = 1; $fight -le $Fights -and (Get-Date) -lt $end; $fight++) {
        # Arm both during the current fight; the session starts at the next fight's first frame.
        for ($i = 0; $i -lt 90 -and -not ((InFight $dirs[0]) -and (InFight $dirs[1])); $i++) { Start-Sleep 2 }
        $armedAt = @((Get-Content "$($dirs[0])\MK9Rollback.log").Count, (Get-Content "$($dirs[1])\MK9Rollback.log").Count)
        if ($fight -gt 1 -and $endedAt) { $armedAt = $endedAt }
        if ($AutoRearm -and $fight -gt 1) {
            "fight ${fight}: not armed by the script (netplayrearm)"
        } else {
            foreach ($k in 0, 1) {
                $lines = @()
                # Fight 1's stage and fighters (the attract script picks them when this demo ends). Arming persists, so
                # this pass of a later fight runs during that fight: those are sent at the previous fight's start (below).
                if ($fight -eq 1 -and $Stages.Count) { $lines += "forcestage $($Stages[0])" }
                if ($fight -eq 1 -and $VaryFighters) { $lines += 'pickvariety 1' }
                if ($Lockstep) { $lines += 'ggpo lockstep 1' }
                if ($AutoRearm) { $lines += 'netplayrearm 1' }
                # Sent here, before the session (2026-11-07): sent at the paused first frame, they reached fight 2's
                # games 2 s after the session had started (xray1). Both persist across fights.
                if ($Damage) { $lines += 'demodamage 1' }
                if ($FullMeter) { $lines += 'netplaymeter 1' }  # both meters full at session frame 8, inside the frame loop
                if ($LowLife -ge 0) { $lines += "netplaylife $([string]::Format([Globalization.CultureInfo]::InvariantCulture, '{0}', $LowLife))" }
                $lines += if ($k -eq 0) { "ggpo netplay 7000 127.0.0.1 7001 1 $Delay" } else { "ggpo netplay 7001 127.0.0.1 7000 2 $Delay" }
                Cmd $dirs[$k] $lines
            }
            "fight ${fight}: armed"
        }
        if ($KillBAfter -lt 0) {
            # The peer never joins: B is killed before the fight start; A must give up waiting and unpause.
            Stop-Process -Id $procs[1].Id -Force
            "fight ${fight}: instance B killed before the netplay start"
            for ($i = 0; $i -lt 240 -and -not (Get-Content "$($dirs[0])\MK9Rollback.log" | Select-Object -Skip $armedAt[0] | Select-String 'did not join' -Quiet); $i++) { Start-Sleep 1 }
            Start-Sleep 5
            $f = Get-Item "$($dirs[0])\MK9Rollback_frames.log"
            "instance A alive: $([bool](Get-Process -Id $procs[0].Id -ErrorAction SilentlyContinue)), frames log written $([int]((Get-Date) - $f.LastWriteTime).TotalSeconds) s ago"
            $tail = Get-Content "$($dirs[0])\MK9Rollback_frames.log" -Tail 400 | Select-String '^FRAME (\d+) ' | Select-Object -Last 1
            Start-Sleep 5
            $tail2 = Get-Content "$($dirs[0])\MK9Rollback_frames.log" -Tail 400 | Select-String '^FRAME (\d+) ' | Select-Object -Last 1
            "instance A last frame: $($tail.Matches[0].Groups[1].Value) -> 5 s later $($tail2.Matches[0].Groups[1].Value)"
            break
        }
        # Both reach the first frame of the next fight and pause: remove the AI on both while paused.
        $paused = @($false, $false)
        for ($i = 0; $i -lt 200 -and -not ($paused[0] -and $paused[1]); $i++) {
            Start-Sleep 1
            foreach ($k in 0, 1) {
                if (-not $paused[$k] -and (Get-Content "$($dirs[$k])\MK9Rollback.log" | Select-Object -Skip $armedAt[$k] | Select-String 'NETPLAY: first fight frame' -Quiet)) {
                    Cmd $dirs[$k] @('killai')
                    $paused[$k] = $true
                }
            }
        }
        if (-not ($paused[0] -and $paused[1])) { "fight ${fight}: did not reach the netplay start on both (A $($paused[0]), B $($paused[1]))"; break }
        # Arming skew (2026-10-29): with -SlowB, B's attract fight can last over a minute longer than A's, so B reaches
        # the session start after A's 60 s join timeout. Once per fight: let both give up, then arm again for the
        # next fight. queued_checks.ps1 excuses at most two 'peer did not join' stops per retry line.
        $skew = { param($k) [bool](Get-Content "$($dirs[$k])\MK9Rollback.log" | Select-Object -Skip $armedAt[$k] | Select-String 'did not join' -Quiet) }
        if (-not $AutoRearm -and ((& $skew 0) -or (& $skew 1))) {  # -AutoRearm: fights 2-3 must not be armed by the script
            if ($retried -eq $fight) { "fight ${fight}: the peer did not join again after a retry"; break }
            for ($i = 0; $i -lt 90 -and -not ((& $skew 0) -and (& $skew 1)); $i++) { Start-Sleep 1 }
            "fight ${fight}: arming skew retry (the peer did not join within 60 s; both armed again for the next fight)"
            $retried = $fight
            $endedAt = $null
            $fight--
            continue
        }
        "fight ${fight}: both at the first frame, AI removed"
        # The next fight's stage and fighters, before the attract script picks them at this one's end.
        if ($Stages.Count -or $VaryFighters) {
            $nl = @()
            if ($Stages.Count) { $nl += "forcestage $($Stages[$fight % $Stages.Count])" }
            if ($VaryFighters) { $nl += "pickvariety $($fight + 1)" }
            foreach ($k in 0, 1) { Cmd $dirs[$k] $nl }
        }
        # Inputs until the fight ends: A drives P1, B drives P2.
        $idle = 0
        $batch = 0  # input batches sent this fight (-FullMeter: the first two are X-ray attempts)
        $fightStart = Get-Date
        $gpSent = $false
        # (seconds into the fight, instance, delay): an attract fight lasts about 33 s after the netplay start
        # Delays 5 and 6 since the online mode's ping rule reaches 6 (2026-11-06): B jumps 2 -> 6 and A 1 -> 5.
        $delays = if ($DelayChange) { @(@(5, 0, 3), @(9, 1, 2), @(13, 0, 1), @(17, 1, 6), @(21, 0, 5), @(25, 1, 1), @(28, 0, 2)) } else { @() }
        $nextDelay = 0
        $shotDone = -not $Shot -or $fight -gt 1
        while ((Get-Date) -lt $end -and $idle -lt 4) {
            if (-not $shotDone -and ((Get-Date) - $fightStart).TotalSeconds -gt 20) {
                . "$PSScriptRoot\desk.ps1"
                # Two game windows: GameWindow returns both, and desk.ps1's Shot then fails (2026-10-29). This
                # capture keeps the status line but not the 3D frame (black).
                [Desk]::Shot(@(GameWindow)[0], $Shot, 0.5)
                "fight ${fight}: screenshot $Shot"
                $shotDone = $true
            }
            if ($GamePauseAfter -gt 0 -and -not $gpSent -and ((Get-Date) - $fightStart).TotalSeconds -gt $GamePauseAfter) {
                # A local pause request the way the game makes one (pause guard test): it must be refused mid-session.
                Cmd $dirs[0] 'gamepause 1'
                "fight ${fight}: game pause requested on A after $GamePauseAfter s"
                $gpSent = $true
            }
            if ($KillBAfter -gt 0 -and ((Get-Date) - $fightStart).TotalSeconds -gt $KillBAfter) {
                # Peer loss mid-fight: B disappears without a word; A must end the session and keep running.
                Stop-Process -Id $procs[1].Id -Force
                "fight ${fight}: instance B killed after $KillBAfter s"
                # The fight must go on in A once its session stops. Sampled right after the stop: an attract fight ends
                # by itself 1966 frames after the netplay start, then about 50 s pass without a fight frame, and a
                # check 30 s after the kill landed there (lockstep_peerloss 2026-10-31).
                for ($i = 0; $i -lt 30 -and -not (Get-Content "$($dirs[0])\MK9Rollback.log" | Select-Object -Skip $armedAt[0] | Select-String 'GGPO p2p stopped' -Quiet); $i++) { Start-Sleep 1 }
                $last = { (Get-Content "$($dirs[0])\MK9Rollback_frames.log" -Tail 400 | Select-String '^FRAME (\d+) ' | Select-Object -Last 1).Matches[0].Groups[1].Value }
                $f1 = & $last
                Start-Sleep 2
                $f2 = & $last
                $alive = [bool](Get-Process -Id $procs[0].Id -ErrorAction SilentlyContinue)
                "instance A alive: $alive, frames after the session stop: $f1 -> $f2 (2 s later)"
                $Fights = 0
                break
            }
            $since = ((Get-Date) - $fightStart).TotalSeconds
            while ($nextDelay -lt $delays.Count -and $since -ge $delays[$nextDelay][0]) {
                $dc = $delays[$nextDelay++]
                Cmd $dirs[$dc[1]] "sessiondelay $($dc[2])"
                "fight ${fight}: instance $('AB'[$dc[1]]) delay -> $($dc[2]) at $([int]$since) s"
            }
            $any = $false
            # Combos: one attacker per batch (P1 3 of 5), ~80 frames per player (the loop takes ~1.6 s; an empty queue
            # keeps the last mask). No meter poke here: the two games would get it on different frames.
            $att = if (-not $Combo) { -1 } elseif ($FullMeter -and $batch -lt 2) { $batch } elseif ($rng.Next(5) -lt 3) { 0 } else { 1 }  # random input keeps its stream
            $xr = $FullMeter -and $batch -lt 2
            $batch++
            foreach ($k in 0, 1) {
                if (InFight $dirs[$k]) { $any = $true }
                # X-ray batches: the defender stands still (a blocked X-ray plays no cinematic).
                $lines = if ($xr -and $k -ne $att) { @("hold $($k + 1) 0 80") }
                         elseif ($Combo) { ComboLines $rng ($k + 1) 80 -Attack:($k -eq $att) -Xray:$xr }
                         else { $l = @(); for ($j = 0; $j -lt 6; $j++) { $l += "hold $($k + 1) $(Chunk)" }; $l }
                Cmd $dirs[$k] $lines
            }
            if ($any) { $idle = 0 } else { $idle++ }
            Start-Sleep -Milliseconds 1200
        }
        "fight ${fight}: ended"
        # Arming persists, so a game can reach the next fight's start before the next pass records $armedAt (B did at
        # 120 ms simulated lag, lag_ns120_d3): search the next start from here instead.
        $endedAt = @((Get-Content "$($dirs[0])\MK9Rollback.log").Count, (Get-Content "$($dirs[1])\MK9Rollback.log").Count)
    }
    # Each session reports its checksums when it stops (the next map load): lag_ns40_d1 2026-10-28 killed the games
    # before fight 3's report and still passed. Wait up to 60 s for one report per fight in each game.
    $waitEnd = (Get-Date).AddSeconds(60)
    while ($fight -gt 1 -and (Get-Date) -lt $waitEnd) {
        $n = @(foreach ($d in $dirs) { @(Select-String "$d\MK9Rollback.log" -Pattern 'GGPO p2p stopped' -SimpleMatch -ErrorAction SilentlyContinue).Count })
        if ($n[0] -ge $fight - 1 -and $n[1] -ge $fight - 1) { break }
        Start-Sleep 2
    }
} else {
    while ((Get-Date) -lt $end -and @(Get-Process MKKE -ErrorAction SilentlyContinue).Count -eq 2) { Start-Sleep 10 }
}
for ($k = 0; $k -lt 2; $k++) {
    "--- instance $('AB'[$k]) ($($dirs[$k]))"
    Select-String "$($dirs[$k])\MK9Rollback.log" -Pattern 'FIGHT at gf|NETPLAY|GGPO (running|p2p stopped|p2p frame delay|connected|synchronized|disconnected)|SESSIONDELAY|CRASH|FORCESTAGE|STAGES|PACE session' |
        ForEach-Object { $_.Line.Substring(15, [Math]::Min(220, $_.Line.Length - 15)) }
}
if (-not $KeepRunning) { Get-Process MKKE -ErrorAction SilentlyContinue | Stop-Process -Force }
