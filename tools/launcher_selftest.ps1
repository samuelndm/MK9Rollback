# Checks the MK9 Rollback window's helpers (tools\launcher_lib.ps1), the updater (tools\updater.ps1) and, where one is
# built, "MK9 Rollback.exe" (launcher\Launcher.cpp) with temporary folders and fixture data only: no game, no network,
# no window.
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
    # ---- fixture release v1 (installed: a version before 2026-11, opened by "Play Online.bat") and v2 (the update:
    # a package of today, opened by "MK9 Rollback.exe") ----
    $dll1 = [Text.Encoding]::ASCII.GetBytes('dll version one'); $dll2 = [Text.Encoding]::ASCII.GetBytes('dll version two, longer')
    $v1 = @{
        'version.txt' = "version=20261101-1200`r`ndll=$(Sha $dll1)"; 'README.txt' = 'readme 1'; 'Play Online.bat' = '@echo one'
        'build\clang\MK9Rollback.dll' = $dll1; 'build\clang\MK9RollbackInjector.exe' = 'inj 1'
        'tools\netplay_gui.ps1' = '# gui 1'; 'tools\updater.ps1' = '# upd 1'; 'tools\netplay.ps1' = '# np 1'; 'tools\launch_game.ps1' = '# lg 1'
        'docs\netplay_lan_test.md' = 'doc 1'
    }
    $v2 = @{
        'version.txt' = "version=20261110-1200`r`ndll=$(Sha $dll2)"; 'README.txt' = 'readme 2'; 'MK9 Rollback.exe' = 'program 2'
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
        (Get-ReleaseEntryPath 'tools/') -eq '' -and (Get-ReleaseEntryPath 'Play Online.bat') -eq 'Play Online.bat' -and
        (Get-ReleaseEntryPath 'MK9 Rollback.exe') -eq 'MK9 Rollback.exe' -and (Get-ReleaseEntryPath 'tools\MK9 Rollback.exe') -eq $null -and
        (Get-ReleaseEntryPath 'Other.exe') -eq $null)
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
    $addedList = @(Get-Content -LiteralPath (Join-Path $root 'previous_version\added_files.txt'))
    Check 'good install lists the added files' ($addedList -contains 'tools\launcher_lib.ps1' -and $addedList -contains 'MK9 Rollback.exe') ($addedList -join ', ')
    Check 'good install with the program takes "Play Online.bat" away (kept in previous_version)' (-not (Test-Path -LiteralPath (Join-Path $root 'Play Online.bat')) -and
        $prevHashes['Play Online.bat'] -eq $orig['Play Online.bat'])
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
        Check 'Restore-PreviousVersion puts the old files back (the .bat too) and removes the added ones (the program too)' ($ok -and (Same-Hashes $origNoPrev $back)) ($lines -join ' | ')
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
        Check 'privacy: real log keeps its lines and loses every SteamID64' (($txt -notmatch '7656(119|120)\d{10}') -and @(Get-Content -LiteralPath $real).Count -eq @(Get-Content -LiteralPath $ro2).Count -and $txt -match 'steam-self' -and $txt -match '127\.0\.0\.1:7050')
    }
    # Accounts made from late 2026 on: 7656120... (76561202255233023 is the highest SteamID64 of a player's account).
    $newSelf = '76561200000000044'; $newPeer = '76561202255233023'
    $fileC = @(
        "[22:00:00.000] STEAM: interfaces available, this account's SteamID64 = $newSelf",
        "[22:00:01.000] STEAM bridge: self $newSelf <-> peer $newPeer, 2 links",
        "[22:00:02.000] ONLINE auto: the game's online opponent is $peerA (process memory, 15 candidate(s), Steam P2P session active)",
        "[22:00:03.000] not Steam IDs: 76561200000000044123 and 7656120000000004",
        # Another account's profile folder with a space in its name (this one's is 'Some Body', replaced by name).
        '[22:01:00.000] Config: E:\Users\Jane Doe\Documents\My Games\x.ini',
        '[22:01:01.000] two: C:\Users\Jane Doe\a.log and D:\Users\bob\b.log, done',
        '[22:01:02.000] home C:\Users\bob (no file name)'
    )
    $lc = Join-Path $tmp 'MK9Rollback_c.log'
    [System.IO.File]::WriteAllLines($lc, [string[]]$fileC)
    $ids2 = @(Find-LogSelfIds @($lc))
    Check 'Find-LogSelfIds reads a 7656120... SteamID64' ($ids2.Count -eq 1 -and $ids2[0] -eq $newSelf) ($ids2 -join ',')
    $st2 = New-LogScrubState -SelfIds $ids2 -UserName 'Some Body' -ComputerName 'SOMEPC-01' -UserProfile 'C:\Users\Some Body'
    $oc = Join-Path $tmp 'c.out'
    Protect-LogFile $lc $oc $st2
    $rc = @(Get-Content -LiteralPath $oc)
    Check 'privacy: 7656120... SteamIDs -> steam-self / steam-peer' ($rc[0] -eq "[22:00:00.000] STEAM: interfaces available, this account's SteamID64 = steam-self" -and
        $rc[1] -eq '[22:00:01.000] STEAM bridge: self steam-self <-> peer steam-peer1, 2 links' -and $rc[2] -like '*opponent is steam-peer2 (process*' -and
        $rc[3] -eq $fileC[3]) ($rc[0..3] -join ' | ')
    Check 'privacy: a profile folder with a space in its name' ($rc[4] -eq '[22:01:00.000] Config: C:\Users\<user>\Documents\My Games\x.ini' -and
        $rc[5] -eq '[22:01:01.000] two: C:\Users\<user>\a.log and C:\Users\<user>\b.log, done' -and
        $rc[6] -eq '[22:01:02.000] home C:\Users\<user> (no file name)' -and (($rc -join ' ') -notmatch 'Jane|Doe|bob')) ($rc[4..6] -join ' | ')

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
    $m8 = Get-LauncherMessage '[21:13:30.001] ONLINE: this game controls player 2'
    $m9 = Get-LauncherMessage 'NETPLAY DESYNC: frame 12'
    Check 'message: the headline and its tone for the window''s status line' ($m8.Head -eq 'Rollback is on' -and $m8.Tone -eq 'good' -and $m8.Text -eq 'Rollback on. You are player 2.' -and
        $m9.Head -eq 'Out of sync' -and $m9.Tone -eq 'bad' -and $m5.Head -and $m5.Tone -eq 'warn' -and $m4.Head -eq '' -and $m4.Tone -eq '') "$($m8.Head) / $($m8.Tone) / $($m9.Head) / $($m9.Tone)"
    Check 'message: groups, {mode}, rearm, kind' ($m4.Text -eq 'Input delay this match: 2 frame(s).' -and $m5.Text -like 'Your opponent''s mod runs in Safe mode and yours in Rollback: both must update MK9 Rollback*' -and
        $m6.Rearm -and $null -eq $m6.Text -and $m7.Text -eq 'Safe mode takes over when this fight starts.' -and $m7.Kind -eq 'reset' -and
        (Get-LauncherMessage 'NETPLAY DESYNC: frame 12').Kind -eq 'desync')
    Check 'command acknowledgement lines' ((Test-CommandAck '[21:12:03.582] CMD netcode rollback') -and (Test-CommandAck '[21:12:03.718] CMD online auto ping') -and
        (Test-CommandAck "[1] CMD ignored: 'netcode rollback'") -and -not (Test-CommandAck '[1] CMD hold 1 16 21') -and -not (Test-CommandAck '[1] NETCODE mode: rollback'))
    # A stall the game recovered from (line shapes of results\field_20261111) says nothing; a crash says it once.
    $h1 = Get-LauncherMessage '[18:27:42.381] HANG? game thread 4544 stalled >3s: eip=758F5FDC esp=012AE5D4 ebp=012AE5F4 crumb=idle(0) stack: 0065006C'
    $h2 = Get-LauncherMessage "[18:27:42.391] HANG?   window 00050912 class 'LaunchUnrealUWindowsClient' title 'Mortal Kombat' (thread 4544)"
    $c1 = Get-LauncherMessage '[04:35:11.774] CRASH? code=C0000005 eip=0042B2BC read addr=BF2769AA thread=21740 crumb=idle(0)'
    $c2 = Get-LauncherMessage '[04:35:11.774] CRASH? stack code-like values: 00E2F4C0 00E0AC18 00E0AC48'
    $h3 = Get-LauncherMessage "[03:10:22.118] HANG?   window 001A0B2E class '#32770' title 'Microsoft Visual C++ Runtime Library' (thread 4544)"
    Check 'message: a stall dump (HANG? lines) is matched and not shown' ($h1 -and $null -eq $h1.Text -and $h2 -and $null -eq $h2.Text)
    Check 'message: an error box in the stall dump is told' ($h3.Text -eq 'The game stopped at an error box (it may be hidden behind the game). Please save the logs.') "$($h3.Text)"
    Check 'message: a crash is shown once, at its first line' ($c1.Text -eq 'The mod reported a crash. Please save the logs.' -and $c2 -and $null -eq $c2.Text) "$($c1.Text)"
    # The window's own check: 3 s frozen and back says nothing; frozen on says 'hung' once, at the 10th second, then 'back'.
    $hw = New-HangWatch
    $seq = @(foreach ($hung in @($true, $true, $true, $false) + @($true) * 12 + @($false, $false)) { Update-HangWatch $hw $hung })
    $want = @('', '', '', '') + @('') * 9 + @('hung', '', '') + @('back', '')
    Check 'hang watch: silent for a recovered stall, one message for a game that stays frozen' (($seq -join ',') -eq ($want -join ',')) ($seq -join ',')

    # ---- 8. injector exit codes ----
    $c = @{}; foreach ($i in 0..8) { $c[$i] = Get-InjectorResult $i }
    Check 'injector 0: loaded' ($c[0].Loaded -and -not $c[0].Retry -and -not $c[0].Text)
    Check 'injector 2: unsupported, no retry' (-not $c[2].Loaded -and -not $c[2].Retry -and $c[2].Text -like 'This game version is not supported*')
    Check 'injector 3: DLL missing, no retry' (-not $c[3].Retry -and $c[3].Text -eq 'MK9Rollback.dll is missing (often removed by antivirus). Restore it from quarantine or extract the zip again, then add the folder to the exclusions.')
    Check 'injector 4: administrator, no retry' (-not $c[4].Retry -and $c[4].Text -eq 'The game runs as administrator. Close it and start Steam normally.')
    Check 'injector 5: refused, retried' ($c[5].Retry -and -not $c[5].Loaded -and $c[5].Text -eq 'Windows or your antivirus refused to load MK9Rollback.dll.')
    Check 'injector 6: already loaded = loaded, arm' ($c[6].Loaded -and $c[6].AlreadyLoaded -and -not $c[6].Retry)
    Check 'injector 7 and 1: retried' ($c[7].Retry -and $c[1].Retry -and -not $c[7].Loaded)
    # Exit code 1 is also "MKKE.exe is not running": the text depends on whether the game is still there.
    Check 'injector 1 with the game closed: "the game was closed", not the antivirus text' ((Get-InjectFailureText $c[1] $false) -eq 'The game was closed before the mod was loaded. Press Play to start it again.' -and
        (Get-InjectFailureText $null $false) -eq (Get-InjectFailureText $c[1] $false))
    Check 'injector 1 with the game running: the antivirus text; 5 and 2 keep their own' ((Get-InjectFailureText $c[1] $true) -like 'Could not load the mod into the game. Your antivirus may have blocked MK9RollbackInjector.exe.*' -and
        (Get-InjectFailureText $null $true) -eq (Get-InjectFailureText $c[1] $true) -and (Get-InjectFailureText $c[5] $true) -eq $c[5].Text -and
        (Get-InjectFailureText $c[2] $false) -eq $c[2].Text)
    $threw = $false; $gone = $null; $bad = $null; $ran = $null
    try {
        $gone = Invoke-Injector (Join-Path $root 'build\clang\NoSuchInjector.exe')
        # The fixture's injector is a text file: Windows refuses to start it, as it does an exe an antivirus blocks.
        $bad = Invoke-Injector (Join-Path $root 'build\clang\MK9RollbackInjector.exe')
        # A program that starts and ends with an exit code (where.exe finds no file named "inject": 1).
        $ran = Invoke-Injector (Join-Path $env:SystemRoot 'System32\where.exe')
    } catch { $threw = $true }
    Check 'Invoke-Injector: a missing exe is a message, not an error' ((-not $threw) -and $gone -and (-not $gone.Started) -and $gone.Error -like 'MK9RollbackInjector.exe is missing*') "$($gone.Error)"
    Check 'Invoke-Injector: an exe Windows will not start is a message, not an error' ((-not $threw) -and $bad -and (-not $bad.Started) -and $bad.Error -like 'Windows could not start MK9RollbackInjector.exe (*). Your antivirus may be blocking it*') "$($bad.Error)"
    Check 'Invoke-Injector: a program that runs gives its exit code' ((-not $threw) -and $ran -and $ran.Started -and ($ran.Code -eq 0 -or $ran.Code -eq 1) -and -not $ran.Error) "started $($ran.Started), code $($ran.Code)"

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

    # ---- 13. the last session's logs survive another try of the same start ----
    $ld = Join-Path $root 'build\clang'
    $prevLog = Join-Path $ld 'previous_logs\MK9Rollback.log'
    function Logs-In([string]$d) { @(Get-ChildItem -LiteralPath $d -Filter 'MK9Rollback*.log' -File -ErrorAction SilentlyContinue).Count }
    [System.IO.File]::WriteAllText((Join-Path $ld 'MK9Rollback.log'), 'session one'); [System.IO.File]::WriteAllText((Join-Path $ld 'MK9Rollback_frames.log'), 'frames one')
    Move-SessionLogs $ld $false
    Check 'session logs: the first try moves them to previous_logs' ((Logs-In $ld) -eq 0 -and (Logs-In (Join-Path $ld 'previous_logs')) -eq 2 -and (Get-Content -LiteralPath $prevLog) -eq 'session one')
    [System.IO.File]::WriteAllText((Join-Path $ld 'MK9Rollback.log'), 'a try that failed')
    Move-SessionLogs $ld $true
    Check 'session logs: another try keeps previous_logs and starts without a log' ((Logs-In $ld) -eq 0 -and (Logs-In (Join-Path $ld 'previous_logs')) -eq 2 -and (Get-Content -LiteralPath $prevLog) -eq 'session one')
    Move-SessionLogs $ld $false
    Check 'session logs: a start without logs keeps previous_logs' ((Get-Content -LiteralPath $prevLog) -eq 'session one')
    [System.IO.File]::WriteAllText((Join-Path $ld 'MK9Rollback.log'), 'session two')
    Move-SessionLogs $ld $false
    Check 'session logs: the next start replaces previous_logs' ((Logs-In $ld) -eq 0 -and (Logs-In (Join-Path $ld 'previous_logs')) -eq 1 -and (Get-Content -LiteralPath $prevLog) -eq 'session two')
    Check 'session logs: the settings file stays' ((Get-Content -LiteralPath (Join-Path $ld 'netplay_gui.txt')) -eq 'settings stay')

    # ---- 14. an update through a version before 2026-11, and the program fetched afterwards ----
    # Those versions' updaters take a zip only when every entry passes the list they carry; the frozen copy of it:
    Check 'the older updaters'' list is unchanged' (($kLegacyReleaseFiles -join ' ') -eq ('^tools\\[^\\]+\.ps1$ ^build\\clang\\MK9Rollback\.dll$ ^build\\clang\\MK9RollbackInjector\.exe$ ' +
        '^docs\\[^\\]+\.md$ ^version\.txt$ ^README\.txt$ ^Play Online\.bat$'))
    function Passes-Legacy([string]$rel) { foreach ($p in $kLegacyReleaseFiles) { if ($rel -match $p) { return $true } }; $false }
    $legacyKeys = @($v2.Keys | Where-Object { $_ -ne 'MK9 Rollback.exe' })
    Check 'the zip for the older updaters: every file but the program passes their list' (-not @($legacyKeys | Where-Object { -not (Passes-Legacy $_) }) -and -not (Passes-Legacy 'MK9 Rollback.exe'))
    $lstage = Join-Path $tmp 'legacy stage'
    $v2legacy = @{}; foreach ($k in $legacyKeys) { $v2legacy[$k] = $v2[$k] }
    Write-Files $lstage $v2legacy
    $legacyZip = Join-Path $tmp 'MK9Rollback-netplay-20261110-1200.legacy.zip'
    Compress-Archive -Path "$lstage\*" -DestinationPath $legacyZip
    $root2 = Join-Path $tmp 'install [2]'
    Write-Files $root2 $v1
    $orig2 = Get-TreeHashes $root2
    # What an older updater does with that zip: every file replaced, "Play Online.bat" left as it is, no program.
    $ok = Install-UpdateFromZip $legacyZip (Sha-Text $legacyZip) $root2 $null '20261110-1200'
    Check 'the zip for the older updaters installs, keeps "Play Online.bat" and brings no program' ($ok -and (Test-Path -LiteralPath (Join-Path $root2 'Play Online.bat')) -and
        -not (Test-Path -LiteralPath (Join-Path $root2 'MK9 Rollback.exe')) -and (Get-InstalledVersion $root2).version -eq '20261110-1200')
    $as = @{ full = [pscustomobject]@{ name = 'MK9-Rollback-20261110-1200.zip'; browser_download_url = 'u1' }; fullSha = [pscustomobject]@{ name = 'MK9-Rollback-20261110-1200.zip.sha256'; browser_download_url = 'u2' }
        old = [pscustomobject]@{ name = 'MK9Rollback-netplay-20261110-1200.zip'; browser_download_url = 'u3' }; oldSha = [pscustomobject]@{ name = 'MK9Rollback-netplay-20261110-1200.zip.sha256'; browser_download_url = 'u4' } }
    $pick1 = Select-ReleaseAssets @($as.old, $as.oldSha, $as.full, $as.fullSha)
    $pick2 = Select-ReleaseAssets @($as.old, $as.oldSha, $as.full)
    $pick3 = Select-ReleaseAssets @($as.old, $as.fullSha)
    Check 'release assets: the package first, else the older updaters'' zip, never a zip without its checksum' ($pick1.Full -and $pick1.Zip.browser_download_url -eq 'u1' -and $pick1.Sha.browser_download_url -eq 'u2' -and
        (-not $pick2.Full) -and $pick2.Zip.browser_download_url -eq 'u3' -and $pick2.Sha.browser_download_url -eq 'u4' -and $null -eq $pick3)
    $relNow = [pscustomobject]@{ Version = '20261110-1200'; Full = $true }
    Check 'an install without the program is seen as unfinished, for its own version''s package only' ((Test-InstallIncomplete $relNow $root2) -and
        -not (Test-InstallIncomplete ([pscustomobject]@{ Version = '20261111-0000'; Full = $true }) $root2) -and
        -not (Test-InstallIncomplete ([pscustomobject]@{ Version = '20261110-1200'; Full = $false }) $root2) -and -not (Test-InstallIncomplete $relNow $root) -and
        -not (Test-InstallIncomplete $null $root2))
    $before2 = Get-TreeHashes $root2
    $otherStage = Join-Path $tmp 'other stage'
    $v3 = @{}; foreach ($k in $v2.Keys) { $v3[$k] = $v2[$k] }; $v3['version.txt'] = "version=20261111-0000`r`ndll=$(Sha $dll2)"
    Write-Files $otherStage $v3
    $otherZip = Join-Path $tmp 'MK9-Rollback-20261111-0000.zip'
    Compress-Archive -Path "$otherStage\*" -DestinationPath $otherZip
    $lines.Clear()
    $no1 = Complete-InstallFromZip $otherZip (Sha-Text $otherZip) $root2 { param($s) [void]$lines.Add($s) }
    $no2 = Complete-InstallFromZip $goodZip ('0' * 64) $root2 $null
    Check 'finishing an install: another version''s package or a damaged download changes nothing' ((-not $no1) -and (-not $no2) -and (Same-Hashes $before2 (Get-TreeHashes $root2))) ($lines -join ' | ')
    $ok = Complete-InstallFromZip $goodZip $goodSha $root2 $null
    $after2 = Get-TreeHashes $root2
    $kept = $true
    foreach ($k in $before2.Keys) { if ($k -ne 'Play Online.bat' -and $k -notlike 'previous_version\*' -and $after2[$k] -ne $before2[$k]) { $kept = $false } }
    Check 'finishing an install: the program is added, no other file is touched' ($ok -and $kept -and $after2['MK9 Rollback.exe'] -eq (Get-TreeHashes $stage)['MK9 Rollback.exe']) "kept $kept"
    Check 'finishing an install: "Play Online.bat" moves into previous_version and the program is noted there' ((-not $after2.ContainsKey('Play Online.bat')) -and
        $after2['previous_version\Play Online.bat'] -eq $orig2['Play Online.bat'] -and
        (@(Get-Content -LiteralPath (Join-Path $root2 'previous_version\added_files.txt')) -contains 'MK9 Rollback.exe') -and -not (Test-InstallIncomplete $relNow $root2))
    if ($gameRuns) {
        'SKIP  going back after a finished install (1): a game is running; run this test again with the game closed'
    } else {
        $ok = Restore-PreviousVersion $root2 $null
        Check 'going back after a finished install returns the older version as it was (the .bat, no program)' ($ok -and (Same-Hashes $orig2 (Get-TreeHashes $root2)))
    }
    # ---- 15. only the release's files are unblocked (the zip may sit in Downloads next to anything) ----
    $dl = Join-Path $tmp 'Downloads [x]'
    $strays = @('holiday.jpg', 'other-setup.exe', 'tools\someone-elses.ps1', 'docs\notes.md', 'build\clang\payload.exe', 'sub\tools\netplay.ps1')
    Write-Files $dl $v2
    foreach ($s in $strays) { Write-Files $dl @{ $s = 'not ours' } }
    function Set-Mark([string]$f) { Set-Content -LiteralPath $f -Stream 'Zone.Identifier' -Value "[ZoneTransfer]`r`nZoneId=3" -ErrorAction Stop }
    function Test-Mark([string]$f) { [bool](Get-Item -LiteralPath $f -Stream 'Zone.Identifier' -ErrorAction SilentlyContinue) }
    $marks = $true
    try { foreach ($rel in Get-TreeFiles $dl) { Set-Mark (Join-Path $dl $rel) } } catch { $marks = $false }
    Check 'package list: every file of a release is on $kPackageFiles, and every entry passes the allow-list' (-not @($v2.Keys | Where-Object { $kPackageFiles -notcontains $_ }) -and
        -not @($kPackageFiles | Where-Object { -not (Get-ReleaseEntryPath $_) })) (@($v2.Keys | Where-Object { $kPackageFiles -notcontains $_ }) -join ', ')
    if (-not $marks) {
        'SKIP  unblock checks (3): this temporary folder cannot hold the internet zone mark (not NTFS)'
    } else {
        Unblock-PackageFiles $dl
        Unblock-PackageFiles $dl @('holiday.jpg', 'build\clang\payload.exe', '..\other-setup.exe', '')
        $stillOurs = @($v2.Keys | Where-Object { Test-Mark (Join-Path $dl $_) })
        $lostStray = @($strays | Where-Object { -not (Test-Mark (Join-Path $dl $_)) })
        Check 'unblock: every file of the release loses the mark' (-not $stillOurs) ($stillOurs -join ', ')
        Check 'unblock: every other file in the folder keeps it' (-not $lostStray) ($lostStray -join ', ')
        foreach ($rel in Get-TreeFiles $dl) { Set-Mark (Join-Path $dl $rel) }
        $ok = Install-UpdateFromZip $goodZip $goodSha $dl $null '20261110-1200'
        $lostStray = @($strays | Where-Object { -not (Test-Mark (Join-Path $dl $_)) })
        Check 'unblock: an update leaves the mark of the other files too' ($ok -and -not $lostStray) ($lostStray -join ', ')
    }

    # ---- 16. an error inside a window handler is shown once, not at every tick ----
    $ew = New-ErrorWatch
    $t0 = New-Object datetime 2026, 1, 1, 12, 0, 0
    $e1 = Update-ErrorWatch $ew 'boom' $t0; $e2 = Update-ErrorWatch $ew 'boom' $t0.AddSeconds(1); $e3 = Update-ErrorWatch $ew 'boom' $t0.AddSeconds(2)
    $n3 = $ew.Count
    $e4 = Update-ErrorWatch $ew 'other' $t0.AddSeconds(3); $n4 = $ew.Count
    $e5 = Update-ErrorWatch $ew 'other' $t0.AddSeconds(200)
    Check 'error watch: the same error within a minute is counted, not shown again' ($e1 -and -not $e2 -and -not $e3 -and $n3 -eq 3 -and $e4 -and $n4 -eq 1 -and $e5)

    # ---- 17. the scripts themselves ----
    $parseErrors = @()
    foreach ($s in 'netplay_gui.ps1', 'launcher_lib.ps1', 'updater.ps1', 'netplay.ps1', 'launch_game.ps1', 'package.ps1', 'publish_release.ps1', 'make_icon.ps1') {
        $f = Join-Path $PSScriptRoot $s
        if (-not (Test-Path -LiteralPath $f)) { continue }
        $tokens = $null; $errs = $null
        [void][System.Management.Automation.Language.Parser]::ParseFile($f, [ref]$tokens, [ref]$errs)
        foreach ($e in @($errs)) { $parseErrors += "$s line $($e.Extent.StartLineNumber): $($e.Message)" }
    }
    Check 'the scripts parse without errors in this PowerShell' (-not $parseErrors) ($parseErrors -join ' | ')
    # The "Play Online.bat" of the versions before 2026-11 runs this first (it stays after an update until the program
    # is fetched): it must end at once with code 0, before anything of the window exists.
    function Run-Hidden([string]$exe, [string]$arguments, [int]$timeoutMs = 30000) {
        $psi = New-Object System.Diagnostics.ProcessStartInfo $exe, $arguments
        $psi.UseShellExecute = $false; $psi.CreateNoWindow = $true
        $psi.RedirectStandardInput = $true; $psi.RedirectStandardOutput = $true
        $psi.EnvironmentVariables['MK9R_LAUNCHER_TEST'] = '1'  # "MK9 Rollback.exe" then prints what it would show in a box
        $p = [System.Diagnostics.Process]::Start($psi)
        $p.StandardInput.Close()  # a "pause" reads the end of the input and goes on
        $read = $p.StandardOutput.ReadToEndAsync()
        $done = $p.WaitForExit($timeoutMs)
        if (-not $done) { try { $p.Kill() } catch { } }
        [pscustomobject]@{ Done = $done; Code = $(if ($done) { $p.ExitCode } else { -1 }); Output = $(if ($done) { $read.Result } else { '' }) }
    }
    $sc = Run-Hidden 'powershell.exe' "-NoProfile -ExecutionPolicy Bypass -File `"$PSScriptRoot\netplay_gui.ps1`" -StartCheck"
    Check 'netplay_gui.ps1 -StartCheck ends at once with code 0' ($sc.Done -and $sc.Code -eq 0) "done $($sc.Done), code $($sc.Code)"

    # ---- 18. "MK9 Rollback.exe" (the newest one built in this tree, or the one of a release folder) ----
    $program = @(Get-ChildItem -Path (Join-Path $PSScriptRoot '..\build\*\MK9 Rollback.exe'), (Join-Path $PSScriptRoot '..\MK9 Rollback.exe') -ErrorAction SilentlyContinue |
        Sort-Object LastWriteTime -Descending | Select-Object -First 1)
    if (-not $program) {
        'SKIP  "MK9 Rollback.exe" checks (6): none is built (.\build.ps1 -LauncherOnly)'
    } else {
        $pd = Join-Path $tmp 'program test'
        $pexe = Join-Path $pd 'MK9 Rollback.exe'
        New-Item -ItemType Directory -Force -Path $pd | Out-Null
        Copy-Item -LiteralPath $program[0].FullName -Destination $pexe
        $vi = (Get-Item -LiteralPath $pexe).VersionInfo
        Check 'MK9 Rollback.exe: named as a program of its own in its file details' ($vi.FileDescription -eq 'MK9 Rollback' -and $vi.ProductName -eq 'MK9 Rollback' -and $vi.OriginalFilename -eq 'MK9 Rollback.exe') "$($vi.FileDescription) / $($vi.ProductName)"
        $r0 = Run-Hidden $pexe ''
        Check 'MK9 Rollback.exe: alone in a folder (opened from inside the zip) it says to extract the zip' ($r0.Done -and $r0.Code -eq 1 -and $r0.Output -like '*Extract the whole zip first*') "code $($r0.Code): $($r0.Output)"
        # A PC that refuses the script: PowerShell ends with an error before any window (here a stand-in that does).
        Write-Files $pd @{ 'tools\netplay_gui.ps1' = '"stand-in: scripts are disabled on this system"; exit 1' }
        $r1 = Run-Hidden $pexe ''
        Check 'MK9 Rollback.exe: says what PowerShell printed when the window cannot open' ($r1.Done -and $r1.Code -eq 1 -and
            $r1.Output -like '*could not open its window*' -and $r1.Output -like '*stand-in: scripts are disabled on this system*') "code $($r1.Code): $($r1.Output)"
        # The script reported its error itself (its own box) and printed nothing: no second message.
        Write-Files $pd @{ 'tools\netplay_gui.ps1' = 'exit 1' }
        $r2 = Run-Hidden $pexe ''
        Check 'MK9 Rollback.exe: silent when the script ended without printing' ($r2.Done -and $r2.Code -eq 1 -and -not "$($r2.Output)".Trim()) "code $($r2.Code): $($r2.Output)"
        # Arguments reach the script as typed, and the script runs in the 64-bit PowerShell on 64-bit Windows.
        Write-Files $pd @{ 'tools\netplay_gui.ps1' = 'param([string]$A, [switch]$B) if ($A -eq "x y" -and $B -and [Environment]::Is64BitProcess -eq [Environment]::Is64BitOperatingSystem -and $env:MK9R_LAUNCHER) { exit 0 }; "got [$A] [$B] 64-bit: $([Environment]::Is64BitProcess)"; exit 1' }
        $r3 = Run-Hidden $pexe '-A "x y" -B'
        Check 'MK9 Rollback.exe: passes its arguments on, starts the 64-bit PowerShell, names itself to the script' ($r3.Done -and $r3.Code -eq 0) "code $($r3.Code): $($r3.Output)"
        # The source tree's layout: the program in build\<name>\, the scripts two folders up.
        $pd2 = Join-Path $tmp 'program tree'
        Write-Files $pd2 @{ 'tools\netplay_gui.ps1' = 'exit 0' }
        New-Item -ItemType Directory -Force -Path (Join-Path $pd2 'build\clang') | Out-Null
        Copy-Item -LiteralPath $program[0].FullName -Destination (Join-Path $pd2 'build\clang\MK9 Rollback.exe')
        $r4 = Run-Hidden (Join-Path $pd2 'build\clang\MK9 Rollback.exe') ''
        Check 'MK9 Rollback.exe: finds the scripts two folders up (build\<name>\ in the source tree)' ($r4.Done -and $r4.Code -eq 0) "code $($r4.Code): $($r4.Output)"
    }} finally {
    Get-ChildItem -LiteralPath $tmp -Recurse -File -ErrorAction SilentlyContinue | ForEach-Object { $_.IsReadOnly = $false }
    Remove-Item -LiteralPath $tmp -Recurse -Force -ErrorAction SilentlyContinue
}
if ($script:failures) { "$script:failures check(s) FAILED" } else { 'All checks passed.' }
exit $script:failures
