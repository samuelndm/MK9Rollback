# Gate D "missing state finder":
#   A = full memory after original frame $SaveAt (paused right after it)
#   B = full memory after re-executing frame $SaveAt from the restored snapshot
# Pages reachable from the fighters (<= $Depth hops) that differ between A and B
# and do not change while paused are simulation state the snapshot did not restore.
param([string]$Dir = "$PSScriptRoot\..\build\clang", [int]$SaveAt = 120, [int]$Depth = 3, [string]$Seed = '1234ABCD',
      [string]$Out = "$PSScriptRoot\..\results\missing-state")
$ErrorActionPreference = 'Stop'
$ms = "$PSScriptRoot\memscan\memscan.exe"
New-Item -ItemType Directory -Force $Out | Out-Null
function Cmd([string[]]$lines) { Set-Content -Encoding ascii "$Dir\cmd.tmp" $lines; Move-Item "$Dir\cmd.tmp" "$Dir\MK9Rollback_cmd.txt" -Force; Start-Sleep -Milliseconds 300 }
function WaitPaused { for ($i = 0; $i -lt 100; $i++) { $t = Get-Content "$Dir\MK9Rollback_frames.log" -Tail 3; if ($t -match 'PAUSE 1 applied') { return }; Start-Sleep -Milliseconds 200 }; throw 'did not pause' }
$Seq = @(@('40', 6), @('0', 40), @('10', 6), @('0', 40), @('1', 6), @('0', 60), @('80', 6), @('0', 60), @('8', 30), @('0', 30))
$len = ($Seq | ForEach-Object { $_[1] } | Measure-Object -Sum).Sum
$loadAt = $SaveAt + $len
$lines = @('clear', 'pause 0', 'lab 1', 'mark missing-state', "rngset $Seed", 'hold 1 200 2', "hold 1 0 $($SaveAt - 2)")
foreach ($step in $Seq) { $lines += "hold 1 $($step[0]) $($step[1])" }
foreach ($step in $Seq) { $lines += "hold 1 $($step[0]) $($step[1])" }
$lines += 'hold 1 0 60'
$lines += "at $SaveAt save 0"
$lines += "at $SaveAt pause 1"
Cmd $lines
WaitPaused
$mpid = (Get-Process MKKE).Id
& $ms snapfull $mpid "$Out\A.full"
Cmd @("at $len load 0", "at $len pause 1", 'pause 0')
Start-Sleep -Milliseconds ([int]($len / 60.0 * 1000) + 1500)
WaitPaused
& $ms snapfull $mpid "$Out\B.full"
& $ms scan $mpid "$Out\n1.bin"; Start-Sleep 1; & $ms scan $mpid "$Out\n2.bin"
$t = Get-Content (Get-ChildItem "$Dir\dumps" | Sort-Object LastWriteTime | Select-Object -Last 1 | Where-Object Name -like '*.txt').FullName -ErrorAction SilentlyContinue
$c0 = '{0:X8}' -f 0; $c1 = $c0
$roots = @()
foreach ($line in (Get-Content "$Dir\MK9Rollback.log" -Tail 400)) { }
# root objects: read char object addresses from the latest STATE-bearing FRAME via the snapshot log line is not available;
# use PlayerInfo + GameInfo (char objects are reached at depth 1 through PlayerInfo+0x1C).
& $ms ptrpages $mpid "$Out\pages.txt" ($Depth + 1) 400 'pinfo=E89D7C:3230' 'gameinfo=E89B70:20C'
& $ms fulldiff "$Out\A.full" "$Out\B.full" "$Out\n1.bin" "$Out\n2.bin" "$Out\pages.txt" 400 > "$Out\fulldiff.txt"
Get-Content "$Out\fulldiff.txt" | Select-Object -Last 1
Cmd @('pause 0', 'release 1', 'clear')
