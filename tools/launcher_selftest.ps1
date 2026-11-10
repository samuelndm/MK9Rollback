# Checks the Play Online window's helpers (tools\launcher_lib.ps1) and the updater (tools\updater.ps1) with temporary
# folders and fixture data only: no game, no network, no window.
#   powershell -NoProfile -ExecutionPolicy Bypass -File tools\launcher_selftest.ps1
# Prints PASS/FAIL per check; the exit code is the number of failures.
$ErrorActionPreference = 'Continue'
. "$PSScriptRoot\updater.ps1"
. "$PSScriptRoot\launcher_lib.ps1"
Add-Type -AssemblyName System.IO.Compression, System.IO.Compression.FileSystem

$script:failures = 0
function Check([string]$name, [bool]$ok, [string]$detail = '') {
    if ($ok) { "PASS  $name" } else { $script:failures++; "FAIL  $name$(if ($detail) { ": $detail" })" }
}
$tmp = Join-Path $env:TEMP "mk9r-selftest-$(Get-Random)"
New-Item -ItemType Directory -Force -Path $tmp | Out-Null

function Write-Files([string]$root, [hashtable]$files) {
    foreach ($k in $files.Keys) {
        $p = Join-Path $root $k
        New-Item -ItemType Directory -Force -Path (Split-Path $p) | Out-Null
        if ($files[$k] -is [byte[]]) { [System.IO.File]::WriteAllBytes($p, $files[$k]) } else { [System.IO.File]::WriteAllText($p, $files[$k]) }
    }
}
function Get-TreeHashes([string]$root) {
    $h = @{}
    if (-not (Test-Path -LiteralPath $root)) { return $h }
    foreach ($rel in Get-TreeFiles $root) { $h[$rel] = (Get-FileHash -LiteralPath (Join-Path $root $rel)).Hash }
    $h
}
function Same-Hashes($a, $b) {
    if ($a.Count -ne $b.Count) { return $false }
    foreach ($k in $a.Keys) { if ($a[$k] -ne $b[$k]) { return $false } }
    $true
}
function Sha([byte[]]$b) { $s = [System.Security.Cryptography.SHA256]::Create(); ([BitConverter]::ToString($s.ComputeHash($b))).Replace('-', '') }
# A zip with entries named exactly as given (backslashes or slashes kept as written).
function New-FixtureZip([string]$path, [hashtable]$entries) {
    $z = [System.IO.Compression.ZipFile]::Open($path, [System.IO.Compression.ZipArchiveMode]::Create)
    try {
        foreach ($k in $entries.Keys) {
            $e = $z.CreateEntry($k)
            $s = $e.Open()
            try { $b = if ($entries[$k] -is [byte[]]) { $entries[$k] } else { [Text.Encoding]::ASCII.GetBytes("$($entries[$k])") }; $s.Write($b, 0, $b.Length) } finally { $s.Dispose() }
        }
    } finally { $z.Dispose() }
}
function Sha-Text([string]$zip) { "$((Get-FileHash -LiteralPath $zip -Algorithm SHA256).Hash)  $(Split-Path -Leaf $zip)" }

try {
    # ---- fixture release v1 (installed) and v2 (the update) ----
    $dll1 = [Text.Encoding]::ASCII.GetBytes('dll version one'); $dll2 = [Text.Encoding]::ASCII.GetBytes('dll version two, longer')
    $v1 = @{
        'version.txt' = "version=20261101-1200`r`ndll=$(Sha $dll1)"; 'README.txt' = 'readme 1'; 'Play Online.bat' = '@echo one'
        'build\clang\MK9Rollback.dll' = $dll1; 'build\clang\MK9RollbackInjector.exe' = 'inj 1'
        'tools\netplay_gui.ps1' = '# gui 1'; 'tools\updater.ps1' = '# upd 1'; 'tools\netplay.ps1' = '# np 1'; 'tools\launch_game.ps1' = '# lg 1'
        'docs\netplay_lan_test.md' = 'doc 1'
    }
    $v2 = @{
        'version.txt' = "version=20261110-1200`r`ndll=$(Sha $dll2)"; 'README.txt' = 'readme 2'; 'Play Online.bat' = '@echo two'
        'build\clang\MK9Rollback.dll' = $dll2; 'build\clang\MK9RollbackInjector.exe' = 'inj 2'
        'tools\netplay_gui.ps1' = '# gui 2'; 'tools\updater.ps1' = '# upd 2'; 'tools\netplay.ps1' = '# np 2'; 'tools\launch_game.ps1' = '# lg 2'
        'tools\launcher_lib.ps1' = '# lib 2 (new file)'; 'docs\netplay_lan_test.md' = 'doc 2'
    }
    $root = Join-Path $tmp 'install [1]'  # brackets: every path must be taken literally
    Write-Files $root $v1
    Write-Files (Join-Path $root 'build\clang') @{ 'netplay_gui.txt' = 'settings stay' }
    Write-Files (Join-Path $root 'previous_version') @{ 'marker.txt' = 'an older backup' }
    $orig = Get-TreeHashes $root

    # The update zip as tools\package.ps1 makes it (Compress-Archive of a staged folder).
    $stage = Join-Path $tmp 'stage'
    Write-Files $stage $v2
    $goodZip = Join-Path $tmp 'MK9Rollback-netplay-20261110-1200.zip'
    Compress-Archive -Path "$stage\*" -DestinationPath $goodZip
    $goodSha = Sha-Text $goodZip

    # ---- 1. allow-list ----
    $evil = Join-Path $tmp 'evil.zip'
    New-FixtureZip $evil @{ 'version.txt' = 'version=20261110-1200'; 'build\clang\MK9Rollback.dll' = 'x'; '..\evil.txt' = 'escape' }
    $lines = New-Object System.Collections.ArrayList
    $ok = Install-UpdateFromZip $evil (Sha-Text $evil) $root { param($s) [void]$lines.Add($s) } '20261110-1200'
    Check 'zip allow-list rejects a ..\evil.txt entry' ((-not $ok) -and -not (Test-Path -LiteralPath (Join-Path $tmp 'evil.txt')) -and ($lines -join ' ') -match 'evil') ($lines -join ' | ')
    $odd = Join-Path $tmp 'odd.zip'
    New-FixtureZip $odd @{ 'version.txt' = 'version=20261110-1200'; 'build\clang\MK9Rollback.dll' = 'x'; 'build\clang\payload.exe' = 'x' }
    Check 'zip allow-list rejects an unexpected file' (-not (Install-UpdateFromZip $odd (Sha-Text $odd) $root $null '20261110-1200'))
    Check 'zip allow-list: entry names' ((Get-ReleaseEntryPath '/abs.txt') -eq $null -and (Get-ReleaseEntryPath 'C:\x\version.txt') -eq $null -and
        (Get-ReleaseEntryPath 'tools/launcher_lib.ps1') -eq 'tools\launcher_lib.ps1' -and (Get-ReleaseEntryPath 'tools\sub\x.ps1') -eq $null -and
        (Get-ReleaseEntryPath 'tools/') -eq '' -and (Get-ReleaseEntryPath 'Play Online.bat') -eq 'Play Online.bat')
    Check 'checksum mismatch changes nothing' ((-not (Install-UpdateFromZip $goodZip ('0' * 64) $root $null)) -and (Same-Hashes $orig (Get-TreeHashes $root)))
    Check 'nothing changed by the refused zips' (Same-Hashes $orig (Get-TreeHashes $root))

    # ---- 2. a failed install puts everything back ----
    $ro = Join-Path $root 'tools\updater.ps1'
    Set-ItemProperty -LiteralPath $ro -Name IsReadOnly -Value $true
    $lines.Clear()
    $ok = Install-UpdateFromZip $goodZip $goodSha $root { param($s) [void]$lines.Add($s) } '20261110-1200'
    Set-ItemProperty -LiteralPath $ro -Name IsReadOnly -Value $false
    $after = Get-TreeHashes $root
    Check 'failed install (read-only target) reports failure' (-not $ok) ($lines -join ' | ')
    Check 'failed install restores every file byte-identically' (Same-Hashes $orig $after) "diff: $(($after.Keys + $orig.Keys | Sort-Object -Unique | Where-Object { $after[$_] -ne $orig[$_] }) -join ', ')"
    Check 'failed install removes the file it added and leaves no previous_version.new' ((-not (Test-Path -LiteralPath (Join-Path $root 'tools\launcher_lib.ps1'))) -and -not (Test-Path -LiteralPath (Join-Path $root 'previous_version.new')))
    Check 'failed install keeps the older previous_version' (Test-Path -LiteralPath (Join-Path $root 'previous_version\marker.txt'))
    Check 'failed install got as far as copying files before the failure' ($script:InstallCopyOrder.Count -gt 0) "$($script:InstallCopyOrder.Count) copied"

    # ---- 3. a good install ----
    $lines.Clear()
    $ok = Install-UpdateFromZip $goodZip $goodSha $root { param($s) [void]$lines.Add($s) } '20261110-1200'
    $want2 = Get-TreeHashes $stage
    $now = Get-TreeHashes $root
    $installedOk = $true
    foreach ($k in $want2.Keys) { if ($now[$k] -ne $want2[$k]) { $installedOk = $false } }
    Check 'good install succeeds with every new file' ($ok -and $installedOk) ($lines -join ' | ')
    Check 'good install writes version.txt last' ($script:InstallCopyOrder[$script:InstallCopyOrder.Count - 1] -eq 'version.txt') ($script:InstallCopyOrder -join ', ')
    $prevHashes = Get-TreeHashes (Join-Path $root 'previous_version')
    $prevOk = -not (Test-Path -LiteralPath (Join-Path $root 'previous_version\marker.txt')) -and -not (Test-Path -LiteralPath (Join-Path $root 'previous_version.new'))
    foreach ($k in $v1.Keys) { if ($prevHashes[$k] -ne $orig[$k]) { $prevOk = $false } }
    Check 'good install swaps previous_version (old files in, older backup gone)' $prevOk
    Check 'good install lists the added file' ((Get-Content -LiteralPath (Join-Path $root 'previous_version\added_files.txt')) -contains 'tools\launcher_lib.ps1')
    Check 'good install keeps the settings' ((Get-Content -LiteralPath (Join-Path $root 'build\clang\netplay_gui.txt')) -eq 'settings stay')
    Check 'installed version is the new one' ((Get-InstalledVersion $root).version -eq '20261110-1200')

    # ---- 4. Restore-PreviousVersion ----
    # It refuses while any MKKE runs (the game holds the DLL), on this PC too: with a game open (a harness run), the
    # refusal itself is what gets checked, and the restore's file checks are reported as skipped.
    $lines.Clear()
    $before = Get-TreeHashes $root
    $gameRuns = [bool](Get-Process MKKE -ErrorAction SilentlyContinue)
    $ok = Restore-PreviousVersion $root { param($s) [void]$lines.Add($s) }
    $back = Get-TreeHashes $root
    if ($gameRuns) {
        Check 'Restore-PreviousVersion refuses while the game runs and changes nothing' (-not $ok -and (Same-Hashes $before $back) -and (($lines -join ' ') -like '*Close the game first*')) ($lines -join ' | ')
        'SKIP  Restore-PreviousVersion file checks (2): a game is running; run this test again with the game closed'
    } else {
        $origNoPrev = @{}; foreach ($k in $orig.Keys) { if ($k -notlike 'previous_version\*') { $origNoPrev[$k] = $orig[$k] } }
        Check 'Restore-PreviousVersion puts the old files back and removes the added one' ($ok -and (Same-Hashes $origNoPrev $back)) ($lines -join ' | ')
        Check 'Restore-PreviousVersion removes previous_version' (-not (Test-Path -LiteralPath (Join-Path $root 'previous_version')) -and -not (Test-Path -LiteralPath (Join-Path $root 'previous_version.undo')))
        Check 'Restore-PreviousVersion without a backup says so' (-not (Restore-PreviousVersion $root $null))
    }

    # ---- 5. Test-InstalledFiles ----
    Check 'Test-InstalledFiles: matching DLL' ((Test-InstalledFiles $root).Ok)
    [System.IO.File]::AppendAllText((Join-Path $root 'build\clang\MK9Rollback.dll'), 'edited')
    $t = Test-InstalledFiles $root
    Check 'Test-InstalledFiles: edited DLL' ((-not $t.Ok) -and $t.Message -eq "MK9 Rollback's files are damaged or mixed (MK9Rollback.dll). Extract the zip again into an empty folder.") $t.Message
    $dev = Join-Path $tmp 'devtree'; Write-Files $dev @{ 'build\clang\MK9Rollback.dll' = 'anything' }
    Check 'Test-InstalledFiles: no version.txt (developer tree) is not checked' ((Test-InstalledFiles $dev).Ok)

    # ---- 6. Protect-LogLine (line shapes of results\online_20261103 and results\field_20261107_session2.log; the IDs
    # and names are made up) ----
    $selfId = '76561198000000011'; $peerA = '76561198000000022'; $peerB = '76561198000000033'
    $fileA = @(
        "[21:11:57.145] Executable: C:\Users\Some Body\Games\MortalKombat_KompleteEdition\DiscContentPC\MKKE.exe",
        "[21:13:20.183] ONLINE auto: the game's online opponent is $peerA (process memory, 15 candidate(s), Steam P2P session active)",
        "[21:13:29.793] NETPLAY armed: player 1, local port 7000, remote 127.0.0.1:7050, delay 2 (starts at the next fight start)",
        "[21:13:35.261] STEAM bridge: self $selfId <-> peer $peerA, 2 links",
        "[21:13:36.000] GGPO p2p p2p remote 192.168.1.20:7001, listening on 0.0.0.0:7000, public 203.0.113.7",
        "[21:13:37.000] MK9Rollback version 1.0.0.2 on SOMEPC-01, log D:\Users\bob\x.log, numbers 1.2.3.4.5 and 300.1.1.1",
        "[21:13:38.000] Executable SHA256: D1C71E086DEAC26716E48F060E486679A36DDD4002AB5080D66DF9D11437D0E6"
    )
    $fileB = @(
        "[21:23:22.039] ONLINE auto: the game's online opponent is $peerB (process memory, 14 candidate(s), Steam P2P session active)",
        "[21:23:48.333] STEAM bridge: self $selfId <-> peer $peerA, 2 links",
        "[21:23:49.000] again 192.168.1.20 and 10.0.0.5 and 8.8.8.8"
    )
    $la = Join-Path $tmp 'MK9Rollback.log'; $lb = Join-Path $tmp 'previous_MK9Rollback.log'
    [System.IO.File]::WriteAllLines($la, [string[]]$fileA); [System.IO.File]::WriteAllLines($lb, [string[]]$fileB)
    $ids = @(Find-LogSelfIds @($la, $lb))
    Check 'Find-LogSelfIds reads "STEAM bridge: self"' ($ids.Count -eq 1 -and $ids[0] -eq $selfId) ($ids -join ',')
    $st = New-LogScrubState -SelfIds $ids -UserName 'Some Body' -ComputerName 'SOMEPC-01' -UserProfile 'C:\Users\Some Body'
    $oa = Join-Path $tmp 'a.out'; $ob = Join-Path $tmp 'b.out'
    Protect-LogFile $la $oa $st; Protect-LogFile $lb $ob $st
    $ra = @(Get-Content -LiteralPath $oa); $rb = @(Get-Content -LiteralPath $ob)
    Check 'privacy: user path' ($ra[0] -eq '[21:11:57.145] Executable: C:\Users\<user>\Games\MortalKombat_KompleteEdition\DiscContentPC\MKKE.exe') $ra[0]
    Check 'privacy: opponent SteamID -> steam-peer1' ($ra[1] -like "*opponent is steam-peer1 (process*") $ra[1]
    Check 'privacy: loopback kept' ($ra[2] -eq $fileA[2]) $ra[2]
    Check 'privacy: self -> steam-self' ($ra[3] -eq '[21:13:35.261] STEAM bridge: self steam-self <-> peer steam-peer1, 2 links') $ra[3]
    Check 'privacy: LAN and internet addresses, 0.0.0.0 kept' ($ra[4] -eq '[21:13:36.000] GGPO p2p p2p remote lan-ip-1:7001, listening on 0.0.0.0:7000, public ip-1') $ra[4]
    Check 'privacy: version string, PC name, other user path, non-addresses' ($ra[5] -eq '[21:13:37.000] MK9Rollback version 1.0.0.2 on <pc>, log C:\Users\<user>\x.log, numbers 1.2.3.4.5 and 300.1.1.1') $ra[5]
    Check 'privacy: hashes and timestamps untouched' ($ra[6] -eq $fileA[6])
    Check 'privacy: pseudonyms stable across two files' ($rb[0] -like '*opponent is steam-peer2 (*' -and $rb[1] -eq '[21:23:48.333] STEAM bridge: self steam-self <-> peer steam-peer1, 2 links' -and
        $rb[2] -eq '[21:23:49.000] again lan-ip-1 and lan-ip-2 and ip-2') ($rb -join ' | ')
    $note = Get-PrivacyNote $st
    Check 'privacy: PRIVACY.txt counts' (($note -join "`n") -match 'Replaced: 6 Steam ID\(s\) \(2 other player\(s\)\), 5 IP address\(es\) \(2 home, 2 internet\)') $note[8]
    # A real log of results\, when this is a developer tree: no SteamID64 may survive, no line may be lost.
    $real = Join-Path $PSScriptRoot '..\results\online_20261103\desktop_212011\MK9Rollback.log'
    if (Test-Path -LiteralPath $real) {
        $rs = New-LogScrubState -SelfIds @(Find-LogSelfIds @($real))
        $ro2 = Join-Path $tmp 'real.out'
        Protect-LogFile $real $ro2 $rs
        $txt = [System.IO.File]::ReadAllText($ro2)
        Check 'privacy: real log keeps its lines and loses every SteamID64' (($txt -notmatch '7656119\d{10}') -and @(Get-Content -LiteralPath $real).Count -eq @(Get-Content -LiteralPath $ro2).Count -and $txt -match 'steam-self' -and $txt -match '127\.0\.0\.1:7050')
    }

    # ---- 7. Get-LauncherMessage ----
    $m1 = Get-LauncherMessage "[12:00:00.000] ONLINE: the other player's mod is older (theirs 20261101-1200, this 20261110-1200); not engaging"
    $m2 = Get-LauncherMessage "[12:00:00.000] ONLINE: this mod is older than the other player's (x)"
    $m3 = Get-LauncherMessage "[12:00:00.000] ONLINE: the other player's mod is a different build (x)"
    Check 'message: opponent older' ($m1.Text -eq "Your opponent's mod is older: they must press 'Check for updates', then 'Update now'. MK9's netcode plays this match." -and -not $m1.CheckUpdate)
    Check 'message: this mod older (runs the update check)' ($m2.Text -eq "Your mod is older than your opponent's: close the game and press 'Update now'. MK9's netcode plays this match." -and $m2.CheckUpdate)
    Check 'message: different build' ($m3.Text -eq "You and your opponent run different builds of the mod: both extract the same zip. MK9's netcode plays this match.")
    Check 'message: old "speaks online version" line no longer matched' ($null -eq (Get-LauncherMessage "ONLINE: the other player's MK9Rollback speaks online version 3 (this one 4); not engaging"))
    $m4 = Get-LauncherMessage '[21:13:29.793] ONLINE: input delay 2 agreed (this game proposed 2, the other 2)'
    $m5 = Get-LauncherMessage 'ONLINE: the other player runs Safe mode, this game Rollback: both must pick' 'Safe mode'
    $m6 = Get-LauncherMessage '[21:25:49.291] GGPO p2p stopped (match over): 7249 frames, 0 loads'
    $m7 = Get-LauncherMessage 'ONLINE: the session starts at the first fight frame of this fight' 'Safe mode'
    Check 'message: groups, {mode}, rearm, kind' ($m4.Text -eq 'Input delay this match: 2 frame(s).' -and $m5.Text -like 'Your opponent uses Safe mode, you use Rollback*' -and
        $m6.Rearm -and $null -eq $m6.Text -and $m7.Text -eq 'Safe mode takes over when this fight starts.' -and $m7.Kind -eq 'reset' -and
        (Get-LauncherMessage 'NETPLAY DESYNC: frame 12').Kind -eq 'desync')
    Check 'command acknowledgement lines' ((Test-CommandAck '[21:12:03.582] CMD netcode rollback') -and (Test-CommandAck '[21:12:03.718] CMD online auto ping') -and
        (Test-CommandAck "[1] CMD ignored: 'netcode rollback'") -and -not (Test-CommandAck '[1] CMD hold 1 16 21') -and -not (Test-CommandAck '[1] NETCODE mode: rollback'))

    # ---- 8. injector exit codes ----
    $c = @{}; foreach ($i in 0..8) { $c[$i] = Get-InjectorResult $i }
    Check 'injector 0: loaded' ($c[0].Loaded -and -not $c[0].Retry -and -not $c[0].Text)
    Check 'injector 2: unsupported, no retry' (-not $c[2].Loaded -and -not $c[2].Retry -and $c[2].Text -like 'This game version is not supported*')
    Check 'injector 3: DLL missing, no retry' (-not $c[3].Retry -and $c[3].Text -eq 'MK9Rollback.dll is missing (often removed by antivirus). Restore it from quarantine or extract the zip again, then add the folder to the exclusions.')
    Check 'injector 4: administrator, no retry' (-not $c[4].Retry -and $c[4].Text -eq 'The game runs as administrator. Close it and start Steam normally.')
    Check 'injector 5: refused, retried' ($c[5].Retry -and -not $c[5].Loaded -and $c[5].Text -eq 'Windows or your antivirus refused to load MK9Rollback.dll.')
    Check 'injector 6: already loaded = loaded, arm' ($c[6].Loaded -and $c[6].AlreadyLoaded -and -not $c[6].Retry)
    Check 'injector 7 and 1: retried' ($c[7].Retry -and $c[1].Retry -and -not $c[7].Loaded)

    # ---- 9. command merge ----
    $cd = Join-Path $tmp 'cmd'; New-Item -ItemType Directory -Force -Path $cd | Out-Null
    $s1 = Send-LauncherCommand $cd @('netcode rollback', 'overlay 1')
    $s2 = Send-LauncherCommand $cd @('online auto ping')
    $got = @(Get-Content -LiteralPath (Join-Path $cd 'MK9Rollback_cmd.txt'))
    Check 'Cmd merge: two quick calls keep both commands, in order' ($s1 -and $s2 -and ($got -join '|') -eq 'netcode rollback|overlay 1|online auto ping') ($got -join '|')
    Check 'Cmd merge leaves no temporary files' (@(Get-ChildItem -LiteralPath $cd).Count -eq 1)

    # ---- 10. incremental log reader ----
    $lf = Join-Path $tmp 'tail.log'
    [System.IO.File]::WriteAllText($lf, "[00:00:01.000] [MK9Rollback] ===== start (pid 1) =====`r`nline 1`r`nline 2`r`npart")
    $tl = New-LogTail $lf
    $r1 = Read-LogTail $tl
    Check 'log reader: first read, partial line held back' (($r1 -join '|') -eq '[00:00:01.000] [MK9Rollback] ===== start (pid 1) =====|line 1|line 2') ($r1 -join '|')
    $w = New-Object System.IO.FileStream($lf, [System.IO.FileMode]::Append, [System.IO.FileAccess]::Write, [System.IO.FileShare]::ReadWrite)
    $b = [Text.Encoding]::ASCII.GetBytes("ial 3`r`nline 4`r`n"); $w.Write($b, 0, $b.Length); $w.Flush()
    $r2 = Read-LogTail $tl   # read while the writer still holds the file open
    $w.Dispose()
    Check 'log reader: only the appended lines, while the writer holds the file' (($r2 -join '|') -eq 'partial 3|line 4') ($r2 -join '|')
    Check 'log reader: nothing new' (@(Read-LogTail $tl).Count -eq 0)
    [System.IO.File]::WriteAllText($lf, "[00:09:00.000] new`r`n")
    $r3 = Read-LogTail $tl
    Check 'log reader: a smaller (rotated) log starts from the top' (($r3 -join '|') -eq '[00:09:00.000] new') ($r3 -join '|')
    [System.IO.File]::WriteAllText($lf, "[00:10:00.000] [MK9Rollback] ===== start (pid 2) =====`r`nanother game, a longer first part of the log`r`n")
    $r4 = Read-LogTail $tl
    Check 'log reader: a recreated log as long as the old one starts from the top' ($r4.Count -eq 2 -and $r4[0] -like '*pid 2*') ($r4 -join '|')
    Remove-Item -LiteralPath $lf
    Check 'log reader: a missing log reads as nothing' (@(Read-LogTail $tl).Count -eq 0)

    # ---- 11. last match facts (line shapes of results\field_20261107_session2.log) ----
    $f = New-MatchFacts
    foreach ($l in @(
        '[21:23:26.073] ONLINE: round trip to the other game 33 ms (second-highest of 8 hellos, median 33 ms): input delay 2 proposed before the fight (rollback)',
        '[21:23:42.973] ONLINE: input delay 2 agreed (this game proposed 2, the other 2)',
        '[21:25:49.291] NETPLAY checksums: 7221 frames matched the peer, 0 did not',
        '[21:25:49.291] GGPO p2p stopped (match over): 7249 frames, 0 loads, 0 replayed frames, 21 barrier waits',
        '[21:28:07.910] NETPLAY checksums: 6638 frames matched the peer, 3 did not',
        '[21:28:07.910] GGPO p2p stopped (match over): 6667 frames, 12 loads, 46 replayed frames, 426 barrier waits')) { [void](Update-MatchFacts $f $l) }
    $txt = Format-MatchFacts $f
    Check 'last match line' ($txt -eq 'Last match: input delay 2, ping 33 ms, 3 rollbacks a minute, out of sync in 1 fight(s).') $txt
    [void](Update-MatchFacts $f '[21:16:12.342] ONLINE: the match is over (results screen): ... (hello round trip this match: median 49 ms, second-highest 49 ms; delay 2 at the end')
    [void](Update-MatchFacts $f '[21:30:00.000] ONLINE: input delay 3 agreed (this game proposed 3, the other 2)')
    Check 'last match line resets at the next match' ((Format-MatchFacts $f) -eq 'Last match: input delay 3.') (Format-MatchFacts $f)

    # ---- 12. connection hint ----
    $sw = [Diagnostics.Stopwatch]::StartNew()
    $hint = $null; $threw = $false
    try { $hint = Get-ConnectionHint } catch { $threw = $true }
    Check 'Get-ConnectionHint returns a string without failing' ((-not $threw) -and $hint -is [string] -and ($hint -eq '' -or $hint -eq 'You are on Wi-Fi: a cable gives steadier fights.')) "'$hint' in $($sw.ElapsedMilliseconds) ms"
} finally {
    Get-ChildItem -LiteralPath $tmp -Recurse -File -ErrorAction SilentlyContinue | ForEach-Object { $_.IsReadOnly = $false }
    Remove-Item -LiteralPath $tmp -Recurse -Force -ErrorAction SilentlyContinue
}
if ($script:failures) { "$script:failures check(s) FAILED" } else { 'All checks passed.' }
exit $script:failures
