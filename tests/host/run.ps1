# Host-side tests: no game, no injection. Built with the project's compiler into build\hosttests, at BelowNormal priority.
#   .\tests\host\run.ps1 [-GgpoLib build\clang\libggpo.a] [-Only hello,finisher,ggpo,logger] [-Seconds 25]
#
#   hello   src\net\HelloProto: the online mode's hello parsing, build comparison and ready decision.
#   finisher  src\net\FinisherRule.h: the frame where a session is handed back to MK9's netcode in the finisher phase;
#           two simulated games with different rollback histories must pick the same one.
#   ggpo    the patched GGPO library (tools\build_ggpo.ps1) against a hand-made remote on 127.0.0.1 (ggpo_raw_peer.cpp):
#           a valid 60 Hz session with malformed datagrams of every kind in between; after each kind the peer must still
#           take the stream. Needs a library with the round 3 exports (ggpo_bye_on_close, ggpo_bind_failed).
#   logger  src\debug\Logger: bursts from four threads, a 60 Hz loop beside a flat-out logger, Flush / TryInfo with the
#           other threads suspended, children killed and exiting mid-burst (about a minute, and a few hundred MB of logs
#           under build\hosttests\logger, deleted afterwards); then the exits again in a build that drops frame-log
#           lines early, for the report of dropped lines.
# Exit code: the number of tests that failed.
param([string]$GgpoLib = 'build\clang\libggpo.a', [string[]]$Only = @('hello', 'finisher', 'ggpo', 'logger'), [int]$Seconds = 25)
$ErrorActionPreference = 'Continue'
$Only = @($Only | ForEach-Object { $_ -split ',' } | ForEach-Object { $_.Trim() } | Where-Object { $_ })  # -File passes "a,b" as one string
$unknown = @($Only | Where-Object { $_ -notin 'hello', 'finisher', 'ggpo', 'logger' })
if ($unknown.Count -or -not $Only.Count) { Write-Host "unknown or no test: '$($unknown -join ', ')' (hello, finisher, ggpo, logger)"; exit 99 }
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
        & (Join-Path $out 'logger_test.exe') $work | Where-Object { $_ -match 'FAIL|ALL CHECKS|not in the files|dropped over' } | ForEach-Object { Write-Host "  $_" }
        $ok = $LASTEXITCODE -eq 0
        Remove-Item -Recurse -Force $work -ErrorAction SilentlyContinue
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
        & (Join-Path $out 'logger_test_drops.exe') $work exitonly 1 drops | Where-Object { $_ -match 'FAIL|ALL CHECKS|not in the files|dropped over' } |
            ForEach-Object { Write-Host "  $_" }
        $ok = $LASTEXITCODE -eq 0
        Remove-Item -Recurse -Force $work -ErrorAction SilentlyContinue
    }
    if (-not $ok) { $failed += 'logger' }
}

Write-Host $(if ($failed.Count) { "FAILED: $($failed -join ', ')" } else { 'host tests passed' })
exit $failed.Count
