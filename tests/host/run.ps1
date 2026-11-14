# Host-side tests: no game, no injection. Built with the project's compiler into build\hosttests, at BelowNormal priority.
#   .\tests\host\run.ps1 [-GgpoLib build\clang\libggpo.a] [-Only hello,finisher,startsync,flow,stagepick,stagehold,refightvote,presslatch,codewrite,strappend,guardpage,hud,keyedge,syswatch,arenapics,stagescreen,ggpo,logger]
#                        [-Seconds 25] [-GameDir <the folder that holds DiscContentPC>]
#
#   hello   src\net\HelloProto: the online mode's hello parsing, build comparison and ready decision.
#   finisher  src\net\FinisherRule.h: the frame where a session is handed back to MK9's netcode in the finisher phase;
#           two simulated games with different rollback histories must pick the same one.
#   startsync  src\net\StartSync.h: the start alignment of a session's two games; two simulated games that reach frame 0
#           at different moments exchange their timing over simulated links (trips of 0.1-80 ms, jitter, loss, the
#           harness's loopback channel) and must end with their frame grids together, only the earlier game moving.
#   flow    src\net\FlowDecode.h: the text of the stage / rematch trace lines (ONLINEFLOW, ONLINEMSG): message 9
#           "GameSettings" and message 10 "ReadyState" decoded from their bytes (a player's name never reaches the
#           text), the traced script natives, which calls of a polled native get a line, every text bounded.
#   stagepick  src\net\StagePick.h: the stage pick packet next to the hello (a release without it drops it; any bytes are
#           read safely), the choice between two picks (the same ticket in both games, a fair coin), and 6000 exchanges
#           between two simulated games over links that lose, delay, repeat and reorder datagrams: a game is confirmed
#           only when the other one has the same result, and what the settings message carries is what both expect.
#   stagehold  src\net\StageHold.h: the stage screen that opens by itself keeps MK9 on the character select (which of the
#           script's questions it answers, for how long at the most) and decides when the drawn arena is written. 5200
#           match starts between two simulated games: no link makes them write two different arenas, a game that does
#           not hold makes both write nothing, and over a link that loses up to four packets in a row both write or
#           neither does. The "note:" lines count the starts in which one game alone wrote (links that lose most
#           packets, links that die).
#   refightvote  src\net\RefightVote.h: the choice after an online match (RE-FIGHT / CHANGE FIGHTER OR ARENA / LEAVE). The
#           table of the two picks, the presses that answer MK9's own rematch box (against a model of that box written
#           from its script: an accept, a leave, a leave after an accept, a box that reads late), and 6000 exchanges of
#           the two picks between two simulated games over links that lose, delay and reorder datagrams: a game acts on a
#           re-fight only when both chose it.
#   presslatch  src\game\PressLatch.h: a press that falls between two samples of the controls, for a panel of the mod. A
#           key's release held back until MK9 has sampled the key (every order of down, up, sample and posted release;
#           3000 taps through a simulated main loop with pauses of up to 400 ms: all lost ones arrive, none twice, no
#           key left down), and the pads' reading thread (sources, the rule that a source counts only once MK9's own
#           sampler has shown it, two threads on the latch, 3000 presses through the same loop).
#   codewrite  src\hook\CodePatch + src\common\ThreadWalk.h: code patched while 48 threads run (all suspended for a write
#           and running after it, a write under a running thread refused), and 56 writes (what arming makes in one game
#           frame) inside half a second. The program is not called "codepatch": Windows takes a 32-bit program with
#           "patch", "setup", "install" or "update" in its name for an installer and asks for administrator rights.
#   strappend  src\common\StrAppend.h: AppendF never writes past its buffer and always leaves it terminated.
#   guardpage  src\common\GuardPage.h: a guarded read of another thread's stack guard page puts the guard back, so
#           that thread can still grow its stack (a child process shows the crash without it).
#   hud    src\debug\Hud.cpp: the network display that belongs to the game's HUD, without the game. For eight picture
#           sizes and twelve canned states: whole rectangles inside the picture, none dropped, gone half a second after
#           the state ends, hidden with the game's HUD, a full buffer reported and never overrun; the build time of a
#           frame and of the faces. Each case is also drawn into build\hosttests\hud\hud_<size>_<state>.bmp.
#   keyedge  src\debug\KeyEdge.h: one press of F9 is one switch of the display, whatever the overlay's 25 ms polls of
#           the key see of it (held with auto-repeat, shorter than a poll interval, made while a poll waited, old).
#   syswatch  src\debug\SysWatch: what Windows announces (power, display, device, setting changes; posted to its hidden
#           window only) becomes SYSEVENT lines, another program's new window is named by program and class, the
#           processor-time list names a program that worked, a flood of events is cut, Stop leaves nothing behind
#           (about 12 s).
#   arenapics  src\debug\ArenaPics.cpp: the reader of the stage screen's arena pictures (the game's own small pictures,
#           read from the player's installed game, none of them in the mod). LZO1X against streams the test writes
#           (every kind of instruction, cut-off and damaged ones, buffers that end at a page without access); a
#           package in MK9's layout that the test writes itself (what is read, what is left out, every pixel), 4000
#           damaged copies of it; the names; the reading thread where the game's file is missing. The same for what
#           MK9's own arena select is drawn with (two fonts as sheets of letters, six frame pieces, the movie that
#           says where each letter stands): a package with all of it, every letter's box, pen, advance and kerning,
#           every pixel of the pieces, nineteen forms that are refused with their reason while the pictures are read
#           all the same, 1200 damaged copies. With the installed game (found through Steam's library list, or
#           -GameDir): its 29 pictures, one for each of the 28 arenas a player may pick, its lettering and pieces.
#           Without it the test says so and checks its own packages only.
#   stagescreen  src\debug\StageScreen.cpp: the stage screen of an online match (feature plan 4.2), without the game. For
#           eight picture sizes and thirteen canned states, in its three forms (the list of names, plain picture
#           tiles, MK9's look with the game's lettering and frame pieces): whole rectangles on the panel and inside the
#           picture, none dropped, every stage name whole in its cell or under its tile, every picture in its tile,
#           every letter drawn pixel for pixel from inside the atlas; the cursor's steps, the roulette's timing (the
#           same on both screens), the names made from the stage table, a damaged state. Pictures in
#           build\hosttests\stagescreen (stage_*: names, tiles_*: plain tiles, look_*: MK9's look; with the installed
#           game's art when it was found, with the test's own stand-in otherwise; those stay in build\).
#   ggpo   the patched GGPO library (tools\build_ggpo.ps1) against a hand-made remote on 127.0.0.1 (ggpo_raw_peer.cpp):
#           a valid 60 Hz session with malformed datagrams of every kind in between; after each kind the peer must still
#           take the stream. Needs a library with the round 3 exports (ggpo_bye_on_close, ggpo_bind_failed). Then the
#           rc30 checks, which need the rc30 library (they fail on an older one, as they should): quality replies,
#           handles, statistics, packet bytes and random numbers in process (ggpo_session_test.cpp), a packet that
#           cannot hold every unacknowledged frame, and the handshake's round trip through a relay (ggpo_relay.cpp).
#   logger  src\debug\Logger: bursts from four threads, a 60 Hz loop beside a flat-out logger, Flush / TryInfo with the
#           other threads suspended, children killed and exiting mid-burst (about a minute, and a few hundred MB of logs
#           under build\hosttests\logger, deleted afterwards); then the exits again in a build that drops frame-log
#           lines early, for the report of dropped lines, and in one that rotates the frame log every 32 KB, for the
#           exit that comes in the middle of a rotation and for the rotation that cannot open its new file.
#   winlock  NOT in the default list (-Only winlock): winlock_probe.cpp keeps the window manager busy three times (about
#           60-100 ms each on the PC it was written on) and shows that GetAsyncKeyState in another thread waits as long
#           while PeekMessage on an empty queue and GetForegroundWindow do not: why the overlay reads the F9 key on a
#           thread of its own. Every program on the PC waits with it, so it refuses to start while MKKE runs.
# Exit code: the number of tests that failed.
param([string]$GgpoLib = 'build\clang\libggpo.a',
      [string[]]$Only = @('hello', 'finisher', 'startsync', 'flow', 'stagepick', 'stagehold', 'refightvote', 'presslatch', 'codewrite', 'strappend', 'guardpage', 'hud', 'keyedge', 'syswatch', 'arenapics', 'stagescreen', 'ggpo', 'logger'),
      [int]$Seconds = 25, [string]$GameDir = '')
$ErrorActionPreference = 'Continue'
$Only = @($Only | ForEach-Object { $_ -split ',' } | ForEach-Object { $_.Trim() } | Where-Object { $_ })  # -File passes "a,b" as one string
$known = 'hello', 'finisher', 'startsync', 'flow', 'stagepick', 'stagehold', 'refightvote', 'presslatch', 'codewrite', 'strappend', 'guardpage', 'hud', 'keyedge', 'syswatch', 'arenapics', 'stagescreen', 'ggpo', 'logger', 'winlock'
$unknown = @($Only | Where-Object { $_ -notin $known })
if ($unknown.Count -or -not $Only.Count) { Write-Host "unknown or no test: '$($unknown -join ', ')' ($($known -join ', '))"; exit 99 }
$root = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
$cxx = Join-Path $root '..\tools\llvm-mingw-20260922-ucrt-x86_64\bin\i686-w64-mingw32-clang++.exe'
$out = Join-Path $root 'build\hosttests'
New-Item -ItemType Directory -Force $out | Out-Null
(Get-Process -Id $PID).PriorityClass = 'BelowNormal'
$failed = @()

function Build([string]$exe, [string[]]$flags) {
    & $cxx @flags -o (Join-Path $out $exe) | Out-Host
    if ($LASTEXITCODE) { Write-Host "  ${exe}: compile failed ($LASTEXITCODE)"; return $false }
    return $true
}

if ($Only -contains 'hello') {
    Write-Host '== hello protocol'
    $net = Join-Path $root 'src\net'
    $ok = Build 'hello_test.exe' @('-std=c++20', '-O2', '-fno-exceptions', '-fno-rtti', '-static', '-Wall', '-I', $net,
                                   (Join-Path $PSScriptRoot 'hello_test.cpp'), (Join-Path $net 'HelloProto.cpp'))
    if ($ok) {
        & (Join-Path $out 'hello_test.exe') | Select-Object -Last 3 | ForEach-Object { Write-Host "  $_" }
        $ok = $LASTEXITCODE -eq 0
    }
    if (-not $ok) { $failed += 'hello' }
}

if ($Only -contains 'finisher') {
    Write-Host '== finisher hand-back rule'
    $net = Join-Path $root 'src\net'
    $ok = Build 'finisher_test.exe' @('-std=c++20', '-O2', '-fno-exceptions', '-fno-rtti', '-static', '-Wall', '-I', $net,
                                      (Join-Path $PSScriptRoot 'finisher_test.cpp'))
    if ($ok) {
        & (Join-Path $out 'finisher_test.exe') | Where-Object { $_ -match '^FAIL|^ +wanted|^ +fight|checks,|fights,' } | ForEach-Object { Write-Host "  $_" }
        $ok = $LASTEXITCODE -eq 0
    }
    if (-not $ok) { $failed += 'finisher' }
}

if ($Only -contains 'startsync') {
    Write-Host '== start alignment'
    $net = Join-Path $root 'src\net'
    $ok = Build 'startsync_test.exe' @('-std=c++20', '-O2', '-fno-exceptions', '-fno-rtti', '-static', '-Wall', '-I', $net,
                                       (Join-Path $PSScriptRoot 'startsync_test.cpp'))
    if ($ok) {
        & (Join-Path $out 'startsync_test.exe') | Where-Object { $_ -match '^FAIL|^ +\S.*: (A moved|B''s frame)|checks,' } | ForEach-Object { Write-Host "  $_" }
        $ok = $LASTEXITCODE -eq 0
    }
    if (-not $ok) { $failed += 'startsync' }
}

# One-file tests of the DLL's own building blocks: built with the DLL's flags, run with a time limit (a test that hangs
# is a failure, not a stuck run), their summary lines shown.
function RunSmall([string]$name, [string]$title, [string[]]$sources, [string[]]$extraFlags, [int]$limitSeconds, [string[]]$libs = @(),
                  [string[]]$exeArgs = @()) {
    Write-Host "== $title"
    $flags = @('-std=c++20', '-O2', '-fno-exceptions', '-fno-rtti', '-static', '-DUNICODE', '-D_UNICODE', '-DWIN32_LEAN_AND_MEAN', '-Wall') +
             $extraFlags + $sources + $libs
    if (-not (Build "$name.exe" $flags)) { return $false }
    $log = Join-Path $out "$name.txt"
    $start = @{ FilePath = (Join-Path $out "$name.exe"); NoNewWindow = $true; PassThru = $true; RedirectStandardOutput = $log }
    if ($exeArgs.Count) { $start.ArgumentList = $exeArgs }
    $p = Start-Process @start
    $null = $p.Handle  # keeps the exit code readable after the process has gone
    if (-not $p.WaitForExit($limitSeconds * 1000)) {
        $p.Kill()
        Write-Host "  ${name}: still running after $limitSeconds s, stopped"
        return $false
    }
    $lines = @(Get-Content $log)
    $lines | Where-Object { $_ -match '^FAIL|checks,|^56 writes|never pause|guard page\(s\)|without the re-arm|^largest frame|^hold \d|^no hold|^note: |^the game.s file|was not given|^the tiles. pictures|was not read' } |
        ForEach-Object { Write-Host "  $_" }
    return $p.ExitCode -eq 0 -and ($lines -contains 'ALL CHECKS PASSED')
}

# The installed game's arena package (the stage screen's pictures are read from it at run time): under -GameDir, or
# in one of Steam's libraries. Empty when the game is not installed here: the tests then use their own pictures.
function ArenaPackage {
    $rel = 'DiscContentPC\Asset\ui_ps_arenaSelect.xxx'
    $dirs = @()
    if ($GameDir) { $dirs += $GameDir }
    $steam = (Get-ItemProperty 'HKCU:\Software\Valve\Steam' -ErrorAction SilentlyContinue).SteamPath
    if ($steam) {
        $libs = @($steam)
        $vdf = Join-Path $steam 'steamapps\libraryfolders.vdf'
        if (Test-Path $vdf) { $libs += [regex]::Matches((Get-Content $vdf -Raw), '"path"\s+"([^"]+)"') | ForEach-Object { $_.Groups[1].Value -replace '\\\\', '\' } }
        $dirs += $libs | ForEach-Object { Join-Path $_ 'steamapps\common\MortalKombat_KompleteEdition' }
    }
    foreach ($d in $dirs) {
        $f = Join-Path $d $rel
        if (Test-Path $f) { return (Resolve-Path $f).Path }
    }
    return ''
}

if ($Only -contains 'flow') {
    $ok = RunSmall 'flow_test' 'stage / rematch trace text' @((Join-Path $PSScriptRoot 'flow_test.cpp')) @('-I', (Join-Path $root 'src\net')) 30
    if (-not $ok) { $failed += 'flow' }
}

if ($Only -contains 'stagepick') {
    $net = Join-Path $root 'src\net'
    $ok = RunSmall 'stagepick_test' 'stage picks: packet, coin, two games over bad links' @((Join-Path $PSScriptRoot 'stagepick_test.cpp'),
                   (Join-Path $net 'HelloProto.cpp')) @('-I', $net) 120
    if (-not $ok) { $failed += 'stagepick' }
}

if ($Only -contains 'stagehold') {
    $net = Join-Path $root 'src\net'
    $ok = RunSmall 'stagehold_test' 'stage hold: the wait on the character select, and when both games write the drawn arena' @((Join-Path $PSScriptRoot 'stagehold_test.cpp')) @('-I', $net) 120
    if (-not $ok) { $failed += 'stagehold' }
}

if ($Only -contains 'refightvote') {
    $net = Join-Path $root 'src\net'
    $ok = RunSmall 'refightvote_test' 'choice after a match: the two picks, the presses on MK9''s own box, two games over bad links' @((Join-Path $PSScriptRoot 'refightvote_test.cpp')) @('-I', $net) 120
    if (-not $ok) { $failed += 'refightvote' }
}

if ($Only -contains 'presslatch') {
    $ok = RunSmall 'presslatch_test' 'press latch: a press between two samples of the controls' @((Join-Path $PSScriptRoot 'presslatch_test.cpp')) @('-I', (Join-Path $root 'src\game')) 120
    if (-not $ok) { $failed += 'presslatch' }
}

if ($Only -contains 'codewrite') {
    $ok = RunSmall 'codewrite_test' 'code writes under running threads' @((Join-Path $PSScriptRoot 'codewrite_test.cpp'),
                   (Join-Path $root 'src\hook\CodePatch.cpp')) @() 120
    if (-not $ok) { $failed += 'codewrite' }
}

if ($Only -contains 'strappend') {
    $ok = RunSmall 'strappend_test' 'bounded text append' @((Join-Path $PSScriptRoot 'strappend_test.cpp')) @() 30
    if (-not $ok) { $failed += 'strappend' }
}

if ($Only -contains 'guardpage') {
    $ok = RunSmall 'guardpage_test' 'stack guard page after a guarded read' @((Join-Path $PSScriptRoot 'guardpage_test.cpp')) @('-municode') 60
    if (-not $ok) { $failed += 'guardpage' }
}

if ($Only -contains 'hud') {
    # The pictures (one per picture size and state) stay in build\hosttests\hud for a look.
    $ok = RunSmall 'hud_test' 'HUD display: rectangles, fades, sizes' @((Join-Path $PSScriptRoot 'hud_test.cpp'),
                   (Join-Path $root 'src\debug\Hud.cpp')) @() 120 @('-lgdi32') @((Join-Path $out 'hud'))
    if (-not $ok) { $failed += 'hud' }
}

if ($Only -contains 'keyedge') {
    $ok = RunSmall 'keyedge_test' 'F9 presses from polls of the key' @((Join-Path $PSScriptRoot 'keyedge_test.cpp')) @('-I', (Join-Path $root 'src\debug')) 30
    if (-not $ok) { $failed += 'keyedge' }
}

if ($Only -contains 'syswatch') {
    $ok = RunSmall 'syswatch_test' 'system events log: announcements, new windows, processor time' @((Join-Path $PSScriptRoot 'syswatch_test.cpp'),
                   (Join-Path $root 'src\debug\SysWatch.cpp')) @('-I', (Join-Path $root 'src\debug')) 60
    if (-not $ok) { $failed += 'syswatch' }
}

if ($Only -contains 'winlock') {
    # Only when asked for (-Only winlock): during its holds every program on the PC waits for the window manager, a
    # running game too. It shows which calls wait (winlock_probe.cpp; why the overlay reads F9 on a thread of its own).
    if (Get-Process MKKE -ErrorAction SilentlyContinue) {
        Write-Host '== window manager lock: NOT RUN, MKKE is running and would stall with it'
        $failed += 'winlock'
    } else {
        $ok = RunSmall 'winlock_probe' 'window manager lock: which calls wait for it' @((Join-Path $PSScriptRoot 'winlock_probe.cpp')) @() 60 @('-lwinmm', '-lgdi32')
        if (-not $ok) { $failed += 'winlock' }
    }
}

if ($Only -contains 'arenapics') {
    $pkg = ArenaPackage
    $exeArgs = @((Join-Path $out 'arenapics'))
    if ($pkg) { $exeArgs += "`"$pkg`"" }
    $ok = RunSmall 'arenapics_test' 'arena pictures: LZO, package reader, damaged files' @((Join-Path $PSScriptRoot 'arenapics_test.cpp'),
                   (Join-Path $root 'src\debug\ArenaPics.cpp')) @() 240 @() $exeArgs
    if (-not $ok) { $failed += 'arenapics' }
}

if ($Only -contains 'stagescreen') {
    # The pictures stay in build\hosttests\stagescreen for a look (every state at 1920x1080, 1280x720 and 960x600).
    $pkg = ArenaPackage
    $exeArgs = @((Join-Path $out 'stagescreen'), '-')
    if ($pkg) { $exeArgs += "`"$pkg`"" }
    $ok = RunSmall 'stagescreen_test' 'stage screen: grid, tiles, picks, roulette, banner' @((Join-Path $PSScriptRoot 'stagescreen_test.cpp'),
                   (Join-Path $root 'src\debug\StageScreen.cpp'), (Join-Path $root 'src\debug\StageSkin.cpp'), (Join-Path $root 'src\debug\Hud.cpp'),
                   (Join-Path $root 'src\debug\ArenaPics.cpp')) @() 240 @('-lgdi32') $exeArgs
    if (-not $ok) { $failed += 'stagescreen' }
}

if ($Only -contains 'ggpo') {
    Write-Host "== GGPO packet validation ($GgpoLib)"
    $lib = Join-Path $root $GgpoLib
    $inc = Join-Path $root 'third_party\ggpo-7ddadef8546a7d99ff0b3530c6056bc8ee4b9c0a\src\include'
    $ok = (Test-Path $lib) -and
          (Build 'ggpo_peer.exe' @('-std=c++17', '-O2', '-static', '-w', '-I', $inc, (Join-Path $PSScriptRoot 'ggpo_peer.cpp'), $lib,
                                   '-lws2_32', '-lwinmm')) -and
          (Build 'ggpo_raw_peer.exe' @('-std=c++17', '-O2', '-static', '-w', (Join-Path $PSScriptRoot 'ggpo_raw_peer.cpp'),
                                       '-lws2_32', '-lwinmm'))
    if (-not (Test-Path $lib)) { Write-Host "  no library at $lib" }
    if ($ok) {
        # The library under test is player 1 on port 7411; the hand-made remote is player 2 on 7412 and stops first.
        $peerLog = Join-Path $out 'ggpo_peer.txt'
        $peer = Start-Process (Join-Path $out 'ggpo_peer.exe') -ArgumentList '7411', '7412', '1', ($Seconds + 6) -NoNewWindow -PassThru `
                              -RedirectStandardOutput $peerLog
        $raw = & (Join-Path $out 'ggpo_raw_peer.exe') 7412 7411 2 $Seconds all
        $rawExit = $LASTEXITCODE
        if (-not $peer.WaitForExit(($Seconds + 30) * 1000)) { $peer.Kill() }
        $raw | Where-Object { $_ -match 'FAILED|raw_peer done' } | ForEach-Object { Write-Host "  $_" }
        $peerOut = Get-Content $peerLog
        $peerOut | Where-Object { $_ -match 'ASSERT|sync error|^done:' } | ForEach-Object { Write-Host "  peer: $_" }
        $ok = $rawExit -eq 0 -and -not ($peerOut -match 'ASSERT') -and ($peerOut -match '^done:') -and ($raw -match 'failed 0')
    }
    # rc30 (tools\build_ggpo.ps1): in-process checks against a hand-made remote (ggpo_session_test.cpp); an input that
    # changes every frame against a remote that stops acknowledging (more unacknowledged frames than one packet holds);
    # the handshake's round trip through a relay whose far end starts a second late (ggpo_relay.cpp, ports 7413 / 7414 too).
    if ($ok) {
        $ok = (Build 'ggpo_session_test.exe' @('-std=c++17', '-O2', '-static', '-w', '-I', $inc, (Join-Path $PSScriptRoot 'ggpo_session_test.cpp'),
                                               $lib, '-lws2_32', '-lwinmm')) -and
              (Build 'ggpo_relay.exe' @('-std=c++17', '-O2', '-static', '-w', (Join-Path $PSScriptRoot 'ggpo_relay.cpp'), '-lws2_32', '-lwinmm'))
    }
    if ($ok) {
        $st = & (Join-Path $out 'ggpo_session_test.exe') 7411
        $ok = $LASTEXITCODE -eq 0
        $st | Where-Object { $_ -match '^FAIL|ggpo_session_test done' } | ForEach-Object { Write-Host "  $_" }
    }
    foreach ($rawCases in 'slowack,slowack,slowack,churncheck', 'noack') {
        if (-not $ok) { break }
        $peer = Start-Process (Join-Path $out 'ggpo_peer.exe') -ArgumentList '7411', '7412', '1', '9', 'churn=1' -NoNewWindow -PassThru `
                              -RedirectStandardOutput $peerLog
        $raw = & (Join-Path $out 'ggpo_raw_peer.exe') 7412 7411 2 6 $rawCases
        $rawExit = $LASTEXITCODE
        if (-not $peer.WaitForExit(40000)) { $peer.Kill() }
        $raw | Where-Object { $_ -match 'FAILED|churncheck|raw_peer done' } | ForEach-Object { Write-Host "  $_" }
        $peerOut = Get-Content $peerLog
        $peerOut | Where-Object { $_ -match 'ASSERT|sync error|^done:' } | ForEach-Object { Write-Host "  peer: $_" }
        $ok = $rawExit -eq 0 -and -not ($peerOut -match 'ASSERT') -and ($peerOut -match '^done:') -and
              ($rawCases -eq 'noack' -or $raw -match 'cases passed 3, failed 0')
    }
    if ($ok) {
        $logA = Join-Path $out 'ggpo_relay_a.txt'
        $logB = Join-Path $out 'ggpo_relay_b.txt'
        $relay = Start-Process (Join-Path $out 'ggpo_relay.exe') -ArgumentList '7411', '7413', '7412', '7414', '20', '9' -NoNewWindow -PassThru `
                               -RedirectStandardOutput (Join-Path $out 'ggpo_relay.txt')
        Start-Sleep -Milliseconds 300
        $peerA = Start-Process (Join-Path $out 'ggpo_peer.exe') -ArgumentList '7411', '7413', '1', '6' -NoNewWindow -PassThru -RedirectStandardOutput $logA
        Start-Sleep -Milliseconds 1000
        $peerB = Start-Process (Join-Path $out 'ggpo_peer.exe') -ArgumentList '7412', '7414', '2', '5' -NoNewWindow -PassThru -RedirectStandardOutput $logB
        foreach ($p in $peerA, $peerB, $relay) { if (-not $p.WaitForExit(30000)) { $p.Kill() } }
        $rtt = @($logA, $logB | ForEach-Object {
            $m = [regex]::Match((Get-Content $_ -Raw), '(?m)^done:.*sync_rtt=(-?\d+)ms')
            if ($m.Success) { [int]$m.Groups[1].Value } else { -1 }
        })
        Write-Host "  relay, 20 ms each way, the second peer 1 s late: handshake round trip $($rtt[0]) ms and $($rtt[1]) ms (wanted 35-120)"
        $ok = -not ((Get-Content $logA, $logB) -match 'ASSERT') -and $rtt[0] -ge 35 -and $rtt[0] -le 120 -and $rtt[1] -ge 35 -and $rtt[1] -le 120
    }
    if (-not $ok) { $failed += 'ggpo' }
}

if ($Only -contains 'logger') {
    Write-Host '== log writer'
    $dbg = Join-Path $root 'src\debug'
    $ok = Build 'logger_test.exe' @('-std=c++20', '-O2', '-fno-exceptions', '-fno-rtti', '-static', '-DUNICODE', '-D_UNICODE',
                                    '-DWIN32_LEAN_AND_MEAN', '-Wall', '-municode', '-DMK9R_LOG_FRAME_CAP=3145728LL', '-I', $dbg,
                                    (Join-Path $PSScriptRoot 'logger_test.cpp'), (Join-Path $dbg 'Logger.cpp'))
    if ($ok) {
        $work = Join-Path $out 'logger'
        Remove-Item -Recurse -Force $work -ErrorAction SilentlyContinue  # what a failed run left
        & (Join-Path $out 'logger_test.exe') $work | Where-Object { $_ -match 'FAIL|ALL CHECKS|not in the files|dropped over|files kept' } | ForEach-Object { Write-Host "  $_" }
        $ok = $LASTEXITCODE -eq 0
        # A failed run keeps its files (the test names the child's folder): the exit failures come about once in several
        # hundred children, and this line used to delete the only evidence (2026-11-12: one child's frame lines absent
        # and not reported, 1 of 660 exits that day and once more that evening; the second one's files showed an exit
        # inside a rotation of the frame log, see the last run below).
        if ($ok) { Remove-Item -Recurse -Force $work -ErrorAction SilentlyContinue }
        else { Write-Host "  the run's files are kept in $work (deleted by the next run)" }
    }
    # The same exits with a 64 KB cap on frame-log text waiting for the disk, so that lines are dropped in most children:
    # every line missing from the frame log must be reported in the main log (a busy PC reaches the real cap now and then).
    if ($ok) {
        $ok = Build 'logger_test_drops.exe' @('-std=c++20', '-O2', '-fno-exceptions', '-fno-rtti', '-static', '-DUNICODE', '-D_UNICODE',
                                              '-DWIN32_LEAN_AND_MEAN', '-Wall', '-municode', '-DMK9R_LOG_FRAME_CAP=3145728LL',
                                              '-DMK9R_LOG_PENDING_FRAME_CAP=65536u', '-I', $dbg,
                                              (Join-Path $PSScriptRoot 'logger_test.cpp'), (Join-Path $dbg 'Logger.cpp'))
    }
    if ($ok) {
        $work = Join-Path $out 'logger_drops'
        Remove-Item -Recurse -Force $work -ErrorAction SilentlyContinue
        & (Join-Path $out 'logger_test_drops.exe') $work exitonly 1 drops | Where-Object { $_ -match 'FAIL|ALL CHECKS|not in the files|dropped over|files kept' } |
            ForEach-Object { Write-Host "  $_" }
        $ok = $LASTEXITCODE -eq 0
        if ($ok) { Remove-Item -Recurse -Force $work -ErrorAction SilentlyContinue }
        else { Write-Host "  the run's files are kept in $work (deleted by the next run)" }
    }
    # The same exits with a 32 KB frame log, which rotates a few hundred times in a child's life: about a third of the
    # children exit while the writer is inside a rotation, with the file closed (at 3 MB: about 1 in 700). The lines
    # still in memory must reach the files then too; the run fails if no child exited there. Before the fix of
    # 2026-11-12 those lines went to a closed handle: 24 of 60 children failed here.
    # Then, in the same build, a rotation whose open fails (the test takes the file's name in between): the main log
    # must say so and count every line dropped, and the file must open again once it can. Before 2026-11-13 the frame
    # log stayed closed for the rest of the session without a word.
    if ($ok) {
        $ok = Build 'logger_test_rotate.exe' @('-std=c++20', '-O2', '-fno-exceptions', '-fno-rtti', '-static', '-DUNICODE', '-D_UNICODE',
                                               '-DWIN32_LEAN_AND_MEAN', '-Wall', '-municode', '-DMK9R_LOG_FRAME_CAP=32768LL', '-I', $dbg,
                                               (Join-Path $PSScriptRoot 'logger_test.cpp'), (Join-Path $dbg 'Logger.cpp'))
    }
    if ($ok) {
        $work = Join-Path $out 'logger_rotate'
        Remove-Item -Recurse -Force $work -ErrorAction SilentlyContinue
        & (Join-Path $out 'logger_test_rotate.exe') $work exitonly 1 rotations |
            Where-Object { $_ -match 'FAIL|ALL CHECKS|not in the files|inside a rotation|files kept' } | ForEach-Object { Write-Host "  $_" }
        $ok = $LASTEXITCODE -eq 0
        if ($ok) { Remove-Item -Recurse -Force $work -ErrorAction SilentlyContinue }
        else { Write-Host "  the run's files are kept in $work (deleted by the next run)" }
    }
    if (-not $ok) { $failed += 'logger' }
}

Write-Host $(if ($failed.Count) { "FAILED: $($failed -join ', ')" } else { 'host tests passed' })
exit $failed.Count
