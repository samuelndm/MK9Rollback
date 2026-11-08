# Non-AI rollback test on attract demo fights: destroy the AIDrone objects ('killai'), then drive
# both fighters with random scripted pad input ('hold', recorded per frame and replayed during
# resimulation) while requesting identity rollbacks of depth 1-10. Demo fights are short, so the
# test continues across consecutive demos (AI killed again in each) until $Count rollbacks are sent.
param([string]$Dir = "$PSScriptRoot\..\build\clang", [int]$Count = 40, [int]$GapMs = 1200, [int]$Seed = 1234,
      [string[]]$Setup = @(), [int]$MaxMinutes = 20, [switch]$Correct, [int]$Synctest = 0, [switch]$P2P, [switch]$Lockstep, [string]$SteamPeer = '', [int]$Delay = 0, [string[]]$PerFight = @(),
      [switch]$Combo,  # -Combo: scripted combos (tools\combo_inputs.ps1), one attacker per batch; SyncTest: full meters at the start
      [switch]$Damage)  # -Damage: 'demodamage 1' in every fight (attract fights otherwise never lose life: no KO, no round end)
$ErrorActionPreference = 'Continue'
if ($Combo) { . "$PSScriptRoot\combo_inputs.ps1" }
function Cmd([string[]]$lines) { Set-Content -Encoding ascii "$Dir\cmd.tmp" $lines; Move-Item "$Dir\cmd.tmp" "$Dir\MK9Rollback_cmd.txt" -Force; Start-Sleep -Milliseconds 150 }
function InFight {
    $f = Get-Item "$Dir\MK9Rollback_frames.log" -ErrorAction SilentlyContinue
    if (-not $f -or ((Get-Date) - $f.LastWriteTime).TotalSeconds -gt 3) { return $false }
    [bool]((Get-Content $f.FullName -Tail 80) -match '^FRAME \d+ ')
}
$rng = [Random]::new($Seed)
# directions: 0 none, 1 up, 2 down, 4 left, 8 right; attacks 0x10/0x20/0x40/0x80; block 0x8000
$dirs = 0, 0, 4, 8, 4, 8, 2, 1, 6, 10
$atk = 0, 0, 0, 0x10, 0x20, 0x40, 0x80, 0x8000
function Chunk { $m = $dirs[$rng.Next($dirs.Count)] -bor $atk[$rng.Next($atk.Count)]; $f = 4 + $rng.Next(20); '{0:X} {1}' -f $m, $f }
$start = (Get-Content "$Dir\MK9Rollback.log").Count
# Setup lines split on ',': 'powershell -File' hands an array argument ('animtrace 1','traceall 1') over as one string.
Cmd (@('lab 1', 'objhooks 1', 'rbmode 1') + @($Setup | ForEach-Object { $_ -split ',' } | Where-Object { $_.Trim() }))  # lab: the soaks read the audits, traces and hashes
$sent = 0; $fights = 0
$deadline = (Get-Date).AddMinutes($MaxMinutes)
while ($sent -lt $Count -and (Get-Date) -lt $deadline) {
    if (-not (Get-Process MKKE -ErrorAction SilentlyContinue)) { 'GAME EXITED'; break }
    if (Select-String -Path "$Dir\MK9Rollback.log" -Pattern 'CRASH\? code' -Quiet) { 'CRASHED'; break }
    # Next demo fight: wait for it, let the RNG trace see the AI script executors, kill the AI.
    for ($i = 0; $i -lt 150 -and -not (InFight); $i++) { Start-Sleep 2 }
    if (-not (InFight)) { 'NO FIGHT'; break }
    Start-Sleep 8
    # The fight seen may have been ending (soak 13116 2026-11-05: FRAME lines of the fight's last seconds, then the
    # session armed at gf 321 of the next screen, "object heap not discovered", 3 artifact sync errors): look again.
    if (-not (InFight)) { continue }
    Cmd 'killai'
    # Combos: both meters full before the session starts (every saved state has them), so X-rays and enhanced specials
    # happen; a poke inside a session would differ from the saved frames.
    if ($Combo -and $Synctest -gt 0) { Cmd @('poke E8A990 3F800000', 'poke E8B61C 3F800000') }
    if ($Damage) { Cmd 'demodamage 1' }  # attract fights take no damage otherwise (game mode table 0x00887B20)
    if ($Synctest -gt 0) { Cmd "ggpo synctest $Synctest" }
    if ($PerFight.Count) { Cmd $PerFight }
    if ($P2P) {
        # Stand-in remote player on loopback (tools/ggpo_peer): GGPO exchanges inputs over real UDP.
        if ($Lockstep) { Cmd "ggpo lockstep 1" }
        if ($SteamPeer) {
            # Over Steam P2P (SteamBridge); with this account's own SteamID64, Steam delivers to self (mirror peer).
            Cmd "ggpo steam $SteamPeer 1 $Delay"
        } else {
            Get-Process ggpo_peer -ErrorAction SilentlyContinue | Stop-Process -Force
            Start-Process -FilePath "$Dir\ggpo_peer.exe" -ArgumentList '7001 127.0.0.1 7000 2 600' -WindowStyle Hidden -RedirectStandardOutput "$Dir\ggpo_peer.log"
            Cmd "ggpo p2p 7000 127.0.0.1 7001 1 $Delay"
        }
    }
    $fights++
    $misses = 0
    $batch = 0
    while ($sent -lt $Count -and $misses -lt 3) {
        $lines = @()
        if ($Combo) {
            # One attacker per batch (P1 3 of 5); the first two batches of a fight are X-ray attempts (P1, then P2).
            # ~80 frames each, about what the batch's wait lets the game consume (an empty queue keeps the last mask).
            $fr = [int]($GapMs * 60 / 1000) + 8
            $att = if ($batch -lt 2) { $batch + 1 } elseif ($rng.Next(5) -lt 3) { 1 } else { 2 }
            foreach ($p in 1, 2) { $lines += ComboLines $rng $p $fr -Attack:($p -eq $att) -Xray:($batch -lt 2) }
            $batch++
        } else {
            foreach ($p in 1, 2) { for ($j = 0; $j -lt 6; $j++) { $lines += "hold $p $(Chunk)" } }
        }
        Cmd $lines
        if ((InFight) -and ($Synctest -gt 0 -or $P2P)) {
            $sent++; $misses = 0  # GGPO rolls back by itself; only inputs are fed
        } elseif (InFight) {
            if ($Correct -and ($sent % 2 -eq 0)) {
                # Prediction correction: P1 input of the first frames of the window is replaced, then an
                # identity rollback from the same S two frames later must reproduce the corrected timeline.
                $k = 4 + $rng.Next(5); $cnt = 1 + $rng.Next(3); $m = $dirs[$rng.Next($dirs.Count)] -bor $atk[$rng.Next($atk.Count)]
                Cmd @(("fakerollback {0} {1:X} {2}" -f $k, $m, $cnt), "at 2 rollback $($k + 2)")
            } else { Cmd @("rollback $(1 + ($sent % 10))") }
            $sent++; $misses = 0
        } else { $misses++ }
        Start-Sleep -Milliseconds $GapMs
    }
}
Start-Sleep 2
$new = Get-Content "$Dir\MK9Rollback.log" | Select-Object -Skip $start
"fights=$fights sent=$sent performed=$(@($new | Select-String 'ROLLBACK \d+ frames:').Count) refused=$(@($new | Select-String 'ROLLBACK \d+ refused').Count) gameplayExact=$(@($new | Select-String 'gameplay exact').Count) gameplayMismatch=$(@($new | Select-String 'gameplay MISMATCH').Count) alive=$([bool](Get-Process MKKE -ErrorAction SilentlyContinue))"
$new | Select-String 'gameplay MISMATCH' | ForEach-Object { '  ' + $_.Line.Substring(15) }
if ($P2P) {
    Get-Process ggpo_peer -ErrorAction SilentlyContinue | Stop-Process -Force
    "p2p: " + (($new | Select-String 'GGPO (p2p|running|connected|synchronized|disconnected)|LOCKSTEP|STEAM' | ForEach-Object { $_.Line.Substring(15) }) -join ' || ')
}
if ($Synctest -gt 0) {
    "synctest: sync errors=$(@($new | Select-String 'GGPO SYNC ERROR').Count) " + (($new | Select-String 'GGPO synctest (started|stopped)' | ForEach-Object { $_.Line.Substring(15) }) -join ' || ')
}
if ($Correct) {
    # After each "CMD fakerollback": the 1st gameplay line is the correction itself (differs by design),
    # the 2nd is the identity rollback over the corrected window (must be exact).
    $state = 0; $ok = 0; $bad = 0
    foreach ($line in $new) {
        if ($line -match 'CMD fakerollback') { $state = 1; continue }
        if ($line -notmatch 'gameplay (exact|MISMATCH)') { continue }
        if ($state -eq 1) { $state = 2; continue }
        if ($state -eq 2) { if ($line -match 'gameplay exact') { $ok++ } else { $bad++ }; $state = 0 }
    }
    "corrections: replay-after-correction exact=$ok mismatch=$bad"
}
