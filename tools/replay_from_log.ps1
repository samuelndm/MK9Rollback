# A replay file (.mk9r, src\net\ReplayFile.h) rebuilt from the logs of a game that played online matches before replays
# were recorded: the frames log's FRAME lines carry both fighters' input masks of every frame whose fight body ran
# (cur=, the first value), the main log names the fighters, the arena, the Kombat Kodes, the side this game played and
# the session's length, and its "NETPLAY STATE frame N" lines give the state at frames 0-3 and every 300th, rounded.
#   .\tools\replay_from_log.ps1 -FramesLog <MK9Rollback_frames.log>[,<older part first>] -MainLog <MK9Rollback.log>
#                              [-Session N] [-Out <file.mk9r>] [-List]
#   -FramesLog  one file, or the rotated pair in order (MK9Rollback_frames.old.log, MK9Rollback_frames.log)
#   -Session    the Nth online session of that game (1 = the first; default: the last)
#   -List       one line per session found, nothing written
# What the logs do not have, the file marks as not recorded (ReplayFile kHaveInputs clear) and fills with a guess:
#   - frames the fight body did not run (the ~500 frames between two rounds): the input held at the first frame after
#     them (that line's prev=), which is what the players held as the next round began;
#   - frames after the last FRAME line (after the fight body stopped at the match's end): neutral.
# No checksums: a playback of such a file is checked against the rounded state samples only. Colours are written only
# when the REFIGHT line names them (releases from 2026-11-16 23:36 on).
param([Parameter(Mandatory = $true)][string[]]$FramesLog, [Parameter(Mandatory = $true)][string]$MainLog, [int]$Session = 0,
      [string]$Out = '', [switch]$List)
$ErrorActionPreference = 'Stop'
$inv = [Globalization.CultureInfo]::InvariantCulture
$FramesLog = @($FramesLog | ForEach-Object { $_ -split ',' } | ForEach-Object { $_.Trim() } | Where-Object { $_ })

# ---- the main log: one record per session (what was said before it, its rounded states, its end) ----
$reArena = [regex]"REFIGHT this online fight's arena is ([A-Za-z0-9_]+) \(stage table entry (-?\d+)\) and its fighters are ([A-Za-z0-9_]+) and ([A-Za-z0-9_]+)(?:, colours (\d) and (\d))?"
$reKodes = [regex]"REFIGHT this online fight's Kombat Kodes, as the game locked them when its load ended: this player (\d)-(\d)-(\d), the other player (\d)-(\d)-(\d)"
$reDelay = [regex]'ONLINE: input delay (\d+) agreed'
$reSide = [regex]'ONLINE: this game controls player ([12])'
$reStart = [regex]'GGPO p2p started at gf=(\d+) \(rollback\):.* player ([12]),.* frame delay (\d+)'
$reState = [regex]'NETPLAY STATE frame (\d+): life (\S+) (\S+) pos (\S+) (\S+) (\S+) / (\S+) (\S+) (\S+) meter (\S+) (\S+) rng ([0-9A-Fa-f]{8})'
$reStop = [regex]'GGPO p2p stopped \(([^)]*)\): (\d+) frames'
$reTime = [regex]'^\[(\d\d:\d\d:\d\d)'
$sessions = New-Object System.Collections.ArrayList
$seen = @{ arena = $null; kodes = $null; delay = 1; side = 0 }
$cur = $null
foreach ($line in [System.IO.File]::ReadLines($MainLog)) {
    if ($line.Contains('REFIGHT this online fight')) {
        $m = $reArena.Match($line); if ($m.Success) { $seen.arena = $m; continue }
        $m = $reKodes.Match($line); if ($m.Success) { $seen.kodes = $m; continue }
    }
    $m = $reDelay.Match($line); if ($m.Success) { $seen.delay = [int]$m.Groups[1].Value; continue }
    $m = $reSide.Match($line); if ($m.Success) { $seen.side = [int]$m.Groups[1].Value; continue }
    $m = $reStart.Match($line)
    if ($m.Success) {
        $t = $reTime.Match($line)
        $cur = @{ arena = $seen.arena; kodes = $seen.kodes; delay = [int]$m.Groups[3].Value; side = $seen.side; startGf = [int]$m.Groups[1].Value
                  player = [int]$m.Groups[2].Value; states = New-Object System.Collections.ArrayList; frames = -1; why = ''
                  time = $(if ($t.Success) { $t.Groups[1].Value } else { '' }) }
        [void]$sessions.Add($cur)
        $seen.arena = $null; $seen.kodes = $null; $seen.side = 0
        continue
    }
    if (-not $cur) { continue }
    $m = $reState.Match($line)
    if ($m.Success) { [void]$cur.states.Add($m); continue }
    $m = $reStop.Match($line)
    if ($m.Success -and $cur.frames -lt 0) { $cur.why = $m.Groups[1].Value; $cur.frames = [int]$m.Groups[2].Value; $cur = $null }
}
# Only sessions that began with the netplay start (frame numbers renumbered: the start's first fight frame is 100000).
$online = @($sessions | Where-Object { $_.startGf -eq 100000 })

# ---- the frames log: per session, every frame's masks (a replayed line written later wins) ----
$reFrame = [regex]'^FRAME(R?) \d+ gf=(\d+) .*? mode=(\d+) sub=(\d+) .*?\| P1 v=\d pad=\w+ i=-?\d+ f=\w+ cur=([0-9A-F]{8}),\w+,\w+ prev=([0-9A-F]{8}) .*?\| P2 v=\d pad=\w+ i=-?\d+ f=\w+ cur=([0-9A-F]{8}),\w+,\w+ prev=([0-9A-F]{8})'
$fsessions = New-Object System.Collections.ArrayList
$fs = $null
foreach ($f in $FramesLog) {
    foreach ($line in [System.IO.File]::ReadLines($f)) {
        if (-not $line.StartsWith('FRAME')) { continue }
        $m = $reFrame.Match($line); if (-not $m.Success) { continue }
        $gf = [uint32]$m.Groups[2].Value
        if ($gf -eq 100000 -and $m.Groups[1].Value -ne 'R') {
            $fs = @{ frames = @{}; mode = [int]$m.Groups[3].Value; sub = [int]$m.Groups[4].Value }
            [void]$fsessions.Add($fs)
            continue
        }
        if (-not $fs) { continue }
        if ($gf -lt 100000) { if ($m.Groups[1].Value -ne 'R') { $fs = $null }; continue }
        $fs.frames[[int]($gf - 100001)] = @([Convert]::ToUInt32($m.Groups[5].Value, 16), [Convert]::ToUInt32($m.Groups[7].Value, 16),
                                            [Convert]::ToUInt32($m.Groups[6].Value, 16), [Convert]::ToUInt32($m.Groups[8].Value, 16))
    }
}

if ($List) {
    for ($i = 0; $i -lt [Math]::Max($online.Count, $fsessions.Count); ++$i) {
        $s = if ($i -lt $online.Count) { $online[$i] } else { $null }
        $q = if ($i -lt $fsessions.Count) { $fsessions[$i] } else { $null }
        $what = if ($s -and $s.arena) { "$($s.arena.Groups[3].Value) vs $($s.arena.Groups[4].Value) on $($s.arena.Groups[1].Value)" } else { 'fighters not named' }
        $len = if ($s) { "$($s.frames) frames ($($s.why))" } else { 'no session in the main log' }
        $logged = if ($q) { "$($q.frames.Count) frames in the frames log" } else { 'not in the frames log' }
        "{0,3}  {1}  {2}; {3}; {4}" -f ($i + 1), $(if ($s) { $s.time } else { '        ' }), $what, $len, $logged
    }
    exit 0
}
if (-not $online.Count) { throw "no online session (GGPO p2p started at gf=100000) in $MainLog" }
if ($online.Count -ne $fsessions.Count) {
    Write-Warning "the main log has $($online.Count) sessions and the frames log $($fsessions.Count): the frames log may have lost its oldest part (pass both rotated files, older first)"
}
$n = if ($Session -gt 0) { $Session } else { [Math]::Min($online.Count, $fsessions.Count) }
if ($n -gt $online.Count -or $n -gt $fsessions.Count) { throw "session $n not found ($($online.Count) in the main log, $($fsessions.Count) in the frames log)" }
$s = $online[$n - 1]
$q = $fsessions[$n - 1]
if ($s.frames -lt 1) { throw "session $n has no end line in the main log (the game closed in it?)" }
$frames = [Math]::Min($s.frames, 65536)

# ---- the masks: logged, gap (the held input after it), tail (neutral) ----
$mask1 = New-Object 'uint32[]' $frames
$mask2 = New-Object 'uint32[]' $frames
$have = New-Object 'byte[]' ($frames + 1)
$next = $null
for ($i = $frames - 1; $i -ge 0; --$i) {
    $v = $q.frames[$i]
    if ($v) { $mask1[$i] = $v[0]; $mask2[$i] = $v[1]; $have[$i] = 1; $next = $v; continue }
    if ($next) { $mask1[$i] = $next[2]; $mask2[$i] = $next[3] }
}
$missing = @($have[0..($frames - 1)] | Where-Object { $_ -eq 0 }).Count

# ---- the file ----
$ms = New-Object System.IO.MemoryStream
$w = New-Object System.IO.BinaryWriter($ms)
function Text([string]$t, [int]$cap) {
    $b = New-Object 'byte[]' $cap
    if ($t) { $a = [Text.Encoding]::ASCII.GetBytes($t); [Array]::Copy($a, $b, [Math]::Min($a.Length, $cap - 1)) }
    $w.Write($b)
}
function Kode([System.Text.RegularExpressions.Match]$m, [int]$g) {
    return [uint16]((([int]$m.Groups[$g].Value) -shl 8) -bor (([int]$m.Groups[$g + 1].Value) -shl 4) -bor [int]$m.Groups[$g + 2].Value)
}
$flags = 1 -bor 2 -bor 8  # from the start, online, rebuilt
if ($s.frames -gt 65536) { $flags = $flags -bor 4 }
$a = $s.arena
$colours = @(0, 0)
if ($a -and $a.Groups[5].Success) { $colours = @([int]$a.Groups[5].Value, [int]$a.Groups[6].Value); $flags = $flags -bor 16 }
$kodes = @([uint16]0, [uint16]0)
if ($s.kodes) {
    $mine = Kode $s.kodes 1; $theirs = Kode $s.kodes 4
    $side = if ($s.side) { $s.side } else { $s.player }
    $kodes = if ($side -eq 2) { @($theirs, $mine) } else { @($mine, $theirs) }
    $flags = $flags -bor 32
}
$states = @($s.states | Where-Object { [int]$_.Groups[1].Value -le $frames } | Sort-Object { [int]$_.Groups[1].Value } -Unique)
$w.Write([uint32]0x52394B4D); $w.Write([uint32]1); $w.Write([uint32]$flags); $w.Write([uint32]$frames); $w.Write([uint32]$states.Count)
Text $(if ($a) { $a.Groups[3].Value } else { '' }) 64
Text $(if ($a) { $a.Groups[4].Value } else { '' }) 64
Text $(if ($a) { $a.Groups[1].Value } else { '' }) 64
$w.Write([int32]$(if ($a) { [int]$a.Groups[2].Value } else { -1 }))
$w.Write([uint32]$colours[0]); $w.Write([uint32]$colours[1])
$w.Write([uint16]$kodes[0]); $w.Write([uint16]$kodes[1])
$w.Write([uint32]$(if ($s.side) { $s.side } else { $s.player })); $w.Write([uint32]$s.delay)
$w.Write([uint32]$q.mode); $w.Write([uint32]$q.sub); $w.Write([uint32]100000); $w.Write([uint32]100001)
$w.Write([uint32]0); $w.Write([uint32]0); $w.Write([uint32]0)  # environment prints: not in the logs
$w.Write([uint32]0); $w.Write([uint32]0)                        # release stamp: not known
$w.Write([single]-1); $w.Write([single]-1)                      # the test settings netplaymeter / netplaylife: off
$date = (Get-Item $MainLog).LastWriteTime.ToString('yyyy-MM-dd', $inv)
Text "$date $($s.time)" 24
Text $s.why 64
for ($i = 0; $i -lt $frames; ++$i) { $w.Write([uint32]$mask1[$i]); $w.Write([uint32]$mask2[$i]) }
for ($i = 0; $i -le $frames; ++$i) { $w.Write([uint32]0) }
$w.Write($have)
foreach ($st in $states) {
    $w.Write([uint32][int]$st.Groups[1].Value); $w.Write([byte]2)
    foreach ($g in 2, 3, 4, 5, 6, 7, 8, 9, 10, 11) { $w.Write([single][double]::Parse($st.Groups[$g].Value, $inv)) }
    $w.Write([uint32][Convert]::ToUInt32($st.Groups[12].Value, 16))
}
$w.Flush()
$bytes = $ms.ToArray()
[uint64]$h = 2166136261
foreach ($b in $bytes) { $h = (($h -bxor $b) * 16777619) -band [uint64]4294967295 }
$w.Write([uint32]$h)
$w.Flush()
$bytes = $ms.ToArray()
if (-not $Out) {
    $dir = Join-Path (Split-Path (Resolve-Path $MainLog)) 'replays'
    New-Item -ItemType Directory -Force $dir | Out-Null
    $Out = Join-Path $dir ("{0}_{1}_from_log.mk9r" -f $date, ($s.time -replace ':', ''))
}
[System.IO.File]::WriteAllBytes($Out, $bytes)
$names = if ($a) { "$($a.Groups[3].Value) vs $($a.Groups[4].Value) on $($a.Groups[1].Value)" } else { 'fighters not named in the log' }
"session $n of $($online.Count) ($($s.time)): $names; $frames frames ($($s.why)), $($frames - $missing) of them logged, $missing guessed; $($states.Count) rounded state samples; $($bytes.Length) bytes -> $Out"
