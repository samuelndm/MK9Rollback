# Pure functions of the Play Online window (tools\netplay_gui.ps1 dot-sources this file). Nothing here opens a window,
# so tools\launcher_selftest.ps1 can check them with fixture data:
#   Get-LauncherMessage    log line -> plain status line (the table of the DLL's log lines the window translates)
#   Get-InjectorResult     MK9RollbackInjector.exe exit code -> message, retry or not
#   Invoke-Injector / Get-InjectFailureText   runs the injector without ever throwing; why the mod was not loaded
#   Move-SessionLogs       the last session's logs -> previous_logs, once per start
#   ConvertTo-AccountId    a pasted friend code, SteamID64 or profile link -> Steam account ID
#   New-HangWatch / Update-HangWatch     a game that stays frozen (not a stall it recovers from)
#   New-ErrorWatch / Update-ErrorWatch   an error of a window handler is shown once, not at every timer tick
#   Test-InstalledFiles    MK9Rollback.dll against version.txt's dll= hash
#   Send-LauncherCommand   commands to the DLL through MK9Rollback_cmd.txt, merged with a file still waiting
#   Test-CommandAck        the DLL's echo of a launcher command ("CMD netcode rollback")
#   New-LogTail / Read-LogTail     the lines appended to MK9Rollback.log since the last read
#   New-LogScrubState / Protect-LogLine / Protect-LogFile / Save-LogsZip   logs for a public bug report
#   New-MatchFacts / Update-MatchFacts / Format-MatchFacts   the window's "Last match" line
#   Get-ConnectionHint     Wi-Fi or cable (slow: run it in the background)
# Windows PowerShell 5.1, ASCII only.

# ---- log line -> status ----
# First match wins; m = $null: matched but not shown. {1} {2}: the pattern's groups, {mode}: Rollback or Safe mode.
# kind: 'desync' (shown once per fight), 'reset' (a new fight or session: a later desync is shown again).
# check: run the update check (this mod is the older one).
$script:LauncherMessages = @(
    @{ p = 'NETPLAY armed'; m = $null },
    # Logged 5 s into a match whose opponent's mod never answered (rc25, OnlineMode Tick); the connection-end line is the
    # older DLLs' only trace of it.
    @{ p = 'ONLINE: the opponent does not run the mod'; m = 'Your opponent is not running the mod: MK9''s own netcode plays this match. Send them the download link.' },
    @{ p = 'ONLINE connection end: mod not engaged; the opponent''s mod last answered never'; m = $null },
    @{ p = 'ONLINE: hooks not installed'; m = 'The online mode could not hook this game version. Use Local Versus instead.' },
    @{ p = 'ONLINE auto: the game''s online opponent is'; m = 'Online fight found. Checking whether your opponent runs the mod...' },
    @{ p = 'ONLINE auto: no opponent found yet'; m = $null },
    # The three version lines (one per disagreement) of the DLL's hello check.
    @{ p = 'ONLINE: the other player''s mod is older'; m = 'Your opponent''s mod is older: they must press ''Check for updates'', then ''Update now''. MK9''s netcode plays this match.' },
    @{ p = 'ONLINE: this mod is older than the other player''s'; m = 'Your mod is older than your opponent''s: close the game and press ''Update now''. MK9''s netcode plays this match.'; check = $true },
    @{ p = 'ONLINE: the other player''s mod is a different build'; m = 'You and your opponent run different builds of the mod: both extract the same zip. MK9''s netcode plays this match.' },
    @{ p = 'ONLINE: the other player''s game files differ'; m = 'Your game files differ from your opponent''s: both need the unmodified Steam Komplete Edition (Steam: Properties, Installed Files, Verify). MK9''s netcode plays this match.' },
    @{ p = 'ONLINE: the other player''s mod runs with other settings'; m = 'Your mod''s settings differ from your opponent''s: both close the game and start it from this window again. MK9''s netcode plays this match.' },
    @{ p = 'ONLINE: hello from the other player'; m = 'Your opponent''s mod answered: {mode} can take over.' },
    @{ p = 'ONLINE: both games run MK9Rollback'; m = 'Both games run the mod.' },
    @{ p = 'ONLINE: not engaging in this match'; m = 'This room is not a 1 vs 1 Player/Private match (King of the Hill or Ranked): MK9''s own netcode plays this match.' },
    @{ p = 'ONLINE: the other player runs (Safe mode|Rollback), this game (Safe mode|Rollback)'; m = 'Your opponent uses {1}, you use {2}: both must pick the same setting. MK9''s netcode plays until you do.' },
    @{ p = 'ONLINE: the session starts at the first fight frame'; m = '{mode} takes over when this fight starts.'; kind = 'reset' },
    @{ p = 'ONLINE: the session starts at the next fight start'; m = 'This fight already started: {mode} takes over from the next fight.'; kind = 'reset' },
    @{ p = 'ONLINE: the fight ended; rollback re-armed'; m = 'Fight over. {mode} stays on for the next fight.' },
    @{ p = 'ONLINE: this game controls player (\d)'; m = '{mode} on. You are player {1}.' },
    @{ p = 'ONLINE: input delay (\d) agreed'; m = 'Input delay this match: {1} frame(s).' },
    @{ p = 'ONLINE: input delay \d -> (\d) between rounds'; m = 'Input delay: {1} frames from this round (the ping changed).' },
    @{ p = 'ONLINE: the match is over'; m = 'Match over. The results and rematch screens run on MK9''s netcode; {mode} takes over again at the next fight.' },
    @{ p = 'ONLINE: rollback disengaged \(match over\)'; m = $null },
    @{ p = 'ONLINE: rollback disengaged \((.*?)\);'; m = 'Back to MK9''s own netcode ({1}).' },
    @{ p = 'STEAM bridge: steam_api.dll interfaces unavailable'; m = 'Steam is not available to the game. Is Steam running and signed in?' },
    @{ p = 'NETPLAY: first fight frame'; m = 'Fight loaded. Waiting for your opponent to start the same fight...'; kind = 'reset' },
    @{ p = 'GGPO synchronized with peer'; m = 'Connected to your opponent.' },
    @{ p = 'NETPLAY: both peers running'; m = 'FIGHT! Both games are running.' },
    @{ p = 'NETPLAY environment matches'; m = 'Game files and mod version match your opponent''s.' },
    @{ p = 'NETPLAY ENVIRONMENT MISMATCH: (.*) differs'; m = 'Your setup differs from your opponent''s: {1}. Use the same mod version (''Check for updates'' at the bottom) and an unmodified game.' },
    @{ p = 'NETPLAY: frame 0 differs'; m = 'The two fights don''t match. Pick the same characters on the same sides, the same stage and settings, then fight again.' },
    @{ p = 'NETPLAY DESYNC: frame (\d+)'; m = 'OUT OF SYNC at frame {1}: the two screens may now differ. Please save the logs after this fight.'; kind = 'desync' },
    # 'desyncstop' (round 3): an online match's session stops once the two games stay different.
    @{ p = 'NETPLAY: the two games went out of sync'; m = 'The two games went out of sync: the rest of this fight runs on MK9''s own netcode. Please save the logs after it.' },
    @{ p = 'NETPLAY checksums: (\d+) frames matched the peer, (\d+) did not'; m = 'Fight over: {1} frames in sync, {2} out of sync.'; kind = 'reset' },
    @{ p = 'did not join within'; m = 'Your opponent''s game did not join in time. This fight continues on MK9''s own netcode.'; rearm = $true },
    @{ p = 'NETPLAY: the session could not start'; m = 'The connection to your opponent could not be set up (a network port may be in use). This fight continues on MK9''s own netcode.'; rearm = $true },
    @{ p = 'the other player disconnected'; m = 'Your opponent disconnected. The rest of this fight continues on MK9''s own netcode; the next fight tries again.' },
    # A crash: the first line of the DLL's report (src\debug\CrashLog.cpp), so the message comes once per crash. Its
    # other "CRASH?" lines and the "HANG?" lines are matched and not shown: the watchdog writes "HANG? game thread N
    # stalled >3s" and one line per thread and window (48 lines in the field log of 2026-11-11) for a stall the game
    # recovers from too, such as the DLL's own start-up. Two kinds of real stop are still told: an error box among those
    # windows (a dialog, class #32770: a C runtime error, a GGPO assertion; the game stands still behind it), and a game
    # that stays frozen, which the window itself sees by asking Windows whether the game's window still answers
    # (Update-HangWatch below).
    @{ p = 'CRASH\? code='; m = 'The mod reported a crash. Please save the logs.' },
    @{ p = 'HANG\?   window [0-9A-Fa-f]+ class ''#32770'''; m = 'The game stopped at an error box (it may be hidden behind the game). Please save the logs.' },
    @{ p = 'CRASH\?|HANG\?'; m = $null },
    @{ p = 'GGPO p2p stopped'; m = $null; rearm = $true }  # each fight is one session; arm the next one
)

# $null when no entry matches; else Text ($null: not shown), Rearm, Kind, CheckUpdate, Pattern.
function Get-LauncherMessage([string]$line, [string]$mode = 'Rollback') {
    foreach ($e in $script:LauncherMessages) {
        $m = [regex]::Match($line, $e.p)
        if (-not $m.Success) { continue }
        $text = $null
        if ($e.m) { $text = $e.m.Replace('{1}', $m.Groups[1].Value).Replace('{2}', $m.Groups[2].Value).Replace('{mode}', $mode) }
        return [pscustomobject]@{ Text = $text; Rearm = [bool]$e.rearm; Kind = "$($e.kind)"; CheckUpdate = [bool]$e.check; Pattern = $e.p }
    }
    $null
}

# The DLL echoes every command it takes from MK9Rollback_cmd.txt as "CMD <command>" (src\debug\CommandChannel.cpp,
# Logger::Info("CMD %s")); an unknown one is "CMD ignored: '<command>'", which is an answer too (an older DLL).
function Test-CommandAck([string]$line) {
    $line -match '\bCMD (ignored: ''(netcode|online|ggpo|overlay)|(netcode|online|ggpo|overlay)\b)'
}

# ---- injector ----
$script:InjectorTexts = @{
    2 = 'This game version is not supported (it must be the Steam Komplete Edition, unmodified).'
    3 = 'MK9Rollback.dll is missing (often removed by antivirus). Restore it from quarantine or extract the zip again, then add the folder to the exclusions.'
    4 = 'The game runs as administrator. Close it and start Steam normally.'
    5 = 'Windows or your antivirus refused to load MK9Rollback.dll.'
    7 = 'The game did not answer while the mod was loading.'
}
# Exit codes of injector\Injector.cpp: 0 done, 2 unknown game build, 3 DLL missing, 4 access denied, 5 LoadLibraryW
# NULL, 6 already loaded, 7 timeout, anything else a failure worth another try.
function Get-InjectorResult([int]$code) {
    $text = $script:InjectorTexts[$code]
    [pscustomobject]@{
        Code = $code
        Loaded = ($code -eq 0 -or $code -eq 6)
        AlreadyLoaded = ($code -eq 6)
        Retry = -not ($code -eq 0 -or $code -eq 2 -or $code -eq 3 -or $code -eq 4 -or $code -eq 6)
        Text = $text
    }
}

# Runs "<exe> inject" and never throws: Started = $false with Error (a sentence for the player) when the file is gone
# (antivirus quarantine) or Windows refuses to start it (blocked). Else Code is the exit code and Output what it printed.
function Invoke-Injector([string]$exe) {
    $ErrorActionPreference = 'Continue'  # here only: with 'Stop' the injector's stderr lines would be thrown
    $r = [pscustomobject]@{ Started = $false; Code = -1; Output = ''; Error = '' }
    if (-not [System.IO.File]::Exists($exe)) {
        $r.Error = 'MK9RollbackInjector.exe is missing (often removed by antivirus). Restore it from quarantine or extract the zip again, then add the folder to the exclusions.'
        return $r
    }
    try {
        $r.Output = (& $exe inject 2>&1 | Out-String)
        $r.Code = $LASTEXITCODE
        $r.Started = $true
    } catch {
        $e = $_.Exception
        if ($e.InnerException) { $e = $e.InnerException }
        $why = ("$($e.Message)".Trim() -split "`r?`n")[0]
        $r.Error = "Windows could not start MK9RollbackInjector.exe ($why). Your antivirus may be blocking it: add the MK9 Rollback folder to its exclusions, then press Play again."
    }
    $r
}

# Why the mod was not loaded, for the player. A game closed before or during the injection ends the injector with
# exit code 1 ("MKKE.exe is not running"), the same code as an injector stopped half-way: only that one is an
# antivirus matter. $result: Get-InjectorResult's, or $null (the mod's log never appeared).
$script:InjectClosedText = 'The game was closed before the mod was loaded. Press Play to start it again.'
$script:InjectBlockedText = 'Could not load the mod into the game. Your antivirus may have blocked MK9RollbackInjector.exe. Press ''Having trouble?'' for what to try, then save the logs if it keeps failing.'
function Get-InjectFailureText($result, [bool]$gameRunning) {
    if ($result -and $result.Text -and -not $result.Retry) { return $result.Text }  # 2, 3, 4: the injector knew why
    if (-not $gameRunning) { return $script:InjectClosedText }
    if ($result -and $result.Text) { return $result.Text }
    $script:InjectBlockedText
}

# Before an injection: the last session's logs go to previous_logs ("Save logs" zips them too: a player who restarts
# after a crash would lose them) and $dir starts without a log. On another try of the same start ($retry) the few
# lines of the try that failed are only deleted, so previous_logs keeps the session before it.
function Move-SessionLogs([string]$dir, [bool]$retry = $false) {
    $old = @(Get-ChildItem -LiteralPath $dir -Filter 'MK9Rollback*.log' -File -ErrorAction SilentlyContinue)
    if ($old -and -not $retry) {
        $prevLogs = Join-Path $dir 'previous_logs'
        Remove-Item -LiteralPath $prevLogs -Recurse -Force -ErrorAction SilentlyContinue
        New-Item -ItemType Directory -Force -Path $prevLogs | Out-Null
        $old | Move-Item -Destination $prevLogs -Force -ErrorAction SilentlyContinue
    }
    Get-ChildItem -LiteralPath $dir -Filter 'MK9Rollback*.log' -File -ErrorAction SilentlyContinue | Remove-Item -Force -ErrorAction SilentlyContinue
}

# ---- friend codes ----
# What the player pasted -> the Steam account ID (32 bits), or 0. Taken: the friend code (the account ID in digits), the
# 17-digit SteamID64 of a player's account, or a profile link that ends in one. Anything else is refused: until
# 2026-11-12 every digit of the text was strung together, so "at 9, my code is 123" was taken as the code 9123.
$script:SteamIdBase = [uint64]76561197960265728
function ConvertTo-AccountId([string]$text) {
    $s = "$text".Trim()
    if ($s -match '^(?:https?://)?(?:www\.)?steamcommunity\.com/profiles/(\d{17})(?:[/?#].*)?$') { $s = $Matches[1] }
    if ($s -notmatch '^\d{1,17}$') { return [uint64]0 }
    $v = [uint64]0
    if (-not [uint64]::TryParse($s, [ref]$v)) { return [uint64]0 }
    if ($v -gt $script:SteamIdBase) { $v = $v - $script:SteamIdBase }
    if ($v -ge 1 -and $v -lt 4294967296) { return $v }
    [uint64]0
}

# ---- a frozen game ----
# Called once a second with what Windows says about the game's window ($hung: it has taken no message for 5 s, what
# the title bar shows as "Not responding"). 'hung' at the $after-th second in a row, 'back' when a game reported that
# way answers again, else ''. A stall the game recovers from (a slow load, the DLL's patches at start-up) stays below.
function New-HangWatch { @{ Ticks = 0; Told = $false } }
function Update-HangWatch($w, [bool]$hung, [int]$after = 10) {
    if ($hung) {
        $w.Ticks++
        if ($w.Ticks -eq $after) { $w.Told = $true; return 'hung' }
        return ''
    }
    $w.Ticks = 0
    if ($w.Told) { $w.Told = $false; return 'back' }
    ''
}

# ---- errors inside the window's handlers ----
# $true when $text is to be shown: not the same error again within a minute (a step of the timer that fails every
# second). Count: how many times in a row it came.
function New-ErrorWatch { @{ Text = ''; At = [datetime]::MinValue; Count = 0 } }
function Update-ErrorWatch($w, [string]$text, [datetime]$now = (Get-Date)) {
    $again = ($text -eq $w.Text) -and (($now - $w.At).TotalSeconds -lt 60)
    if ($again) { $w.Count++ } else { $w.Count = 1 }
    $w.Text = $text
    $w.At = $now
    -not $again
}

# ---- installed files ----
# A developer tree has no version.txt (or no dll= line): no check.
function Test-InstalledFiles([string]$root) {
    $ok = [pscustomobject]@{ Ok = $true; Message = '' }
    $v = Join-Path $root 'version.txt'
    if (-not (Test-Path -LiteralPath $v)) { return $ok }
    $want = ''
    foreach ($line in Get-Content -LiteralPath $v) { if ($line -match '^dll=([0-9A-Fa-f]{64})\s*$') { $want = $Matches[1].ToUpperInvariant() } }
    if (-not $want) { return $ok }
    $dll = Join-Path $root 'build\clang\MK9Rollback.dll'
    if (-not (Test-Path -LiteralPath $dll)) { return [pscustomobject]@{ Ok = $false; Message = $script:InjectorTexts[3] } }
    $got = ''
    try { $got = (Get-FileHash -LiteralPath $dll -Algorithm SHA256 -ErrorAction Stop).Hash.ToUpperInvariant() } catch { }
    if ($got -ne $want) {
        return [pscustomobject]@{ Ok = $false; Message = "MK9 Rollback's files are damaged or mixed (MK9Rollback.dll). Extract the zip again into an empty folder." }
    }
    $ok
}

# ---- commands to the DLL ----
# The DLL takes MK9Rollback_cmd.txt by renaming it (every 50 ms). A file still waiting is taken the same way here and
# its lines go in front of the new ones, so two quick commands both arrive. $false: the file could not be placed.
function Send-LauncherCommand([string]$dir, [string[]]$lines) {
    $cmd = Join-Path $dir 'MK9Rollback_cmd.txt'
    $tmp = Join-Path $dir 'cmd.tmp'
    $mine = Join-Path $dir 'cmd.merge'
    $pending = New-Object System.Collections.Generic.List[string]
    for ($i = 0; $i -lt 40; $i++) {
        foreach ($f in @($mine, $cmd)) {
            if (-not [System.IO.File]::Exists($f)) { continue }
            try {
                if ($f -eq $cmd) { [System.IO.File]::Move($cmd, $mine) }
                foreach ($l in [System.IO.File]::ReadAllLines($mine)) { if ($l.Trim()) { $pending.Add($l) } }
                [System.IO.File]::Delete($mine)
            } catch { }
        }
        try {
            $all = @($pending) + @($lines)
            [System.IO.File]::WriteAllLines($tmp, [string[]]$all, [System.Text.Encoding]::ASCII)
            [System.IO.File]::Move($tmp, $cmd)
            return $true
        } catch {
            Start-Sleep -Milliseconds 25  # a sharing violation (antivirus scan) or a file that appeared meanwhile
        }
    }
    $false
}

# ---- reading the log as it grows ----
# Only the bytes appended since the last read, from a stream that lets the game keep writing. A shorter file, or one
# whose first bytes changed (a new game's log in the same place), starts again from the top.
function New-LogTail([string]$path) {
    @{ Path = $path; Pos = [int64]0; Partial = ''; Head = '' }
}
function Read-LogTail($tail, [switch]$Discard) {
    $out = New-Object System.Collections.Generic.List[string]
    if (-not [System.IO.File]::Exists($tail.Path)) {
        $tail.Pos = [int64]0; $tail.Partial = ''; $tail.Head = ''
        return $out.ToArray()
    }
    try {
        $fs = New-Object System.IO.FileStream($tail.Path, [System.IO.FileMode]::Open, [System.IO.FileAccess]::Read,
            ([System.IO.FileShare]::ReadWrite -bor [System.IO.FileShare]::Delete))
    } catch { return $out.ToArray() }
    try {
        $len = $fs.Length
        $headBytes = New-Object byte[] ([int][Math]::Min(64, $len))
        $n = 0
        while ($n -lt $headBytes.Length) { $r = $fs.Read($headBytes, $n, $headBytes.Length - $n); if ($r -le 0) { break }; $n += $r }
        $head = [System.Text.Encoding]::Default.GetString($headBytes, 0, $n)
        $known = $tail.Head
        if ($len -lt $tail.Pos -or ($known -and -not $head.StartsWith($known))) {
            $tail.Pos = [int64]0; $tail.Partial = ''
        }
        $tail.Head = $head
        if ($len -le $tail.Pos) { return $out.ToArray() }
        [void]$fs.Seek($tail.Pos, [System.IO.SeekOrigin]::Begin)
        $count = [int]($len - $tail.Pos)
        $buf = New-Object byte[] $count
        $got = 0
        while ($got -lt $count) { $r = $fs.Read($buf, $got, $count - $got); if ($r -le 0) { break }; $got += $r }
        $tail.Pos += $got
        $text = $tail.Partial + [System.Text.Encoding]::Default.GetString($buf, 0, $got)
        $parts = $text.Split("`n")
        $tail.Partial = $parts[$parts.Length - 1]
        if (-not $Discard) { for ($i = 0; $i -lt $parts.Length - 1; $i++) { $out.Add($parts[$i].TrimEnd("`r")) } }
    } finally { $fs.Dispose() }
    $out.ToArray()
}

# ---- log privacy (Save logs to the Desktop) ----
# How the DLL names this account: "STEAM bridge: self <id> <-> peer <id>" (src\net\SteamBridge.cpp) and
# "STEAM: interfaces ..., this account's SteamID64 = <id>" (src\debug\CommandChannel.cpp).
# A player's SteamID64 is 76561197960265728 + the 32-bit account number: 76561197960265728 to 76561202255233023, so
# 17 digits that start with 7656119 or, for accounts made from late 2026 on, 7656120 (to ...2999999999 here).
$script:SteamId64Pattern = '7656(?:119\d|120[0-2])\d{9}'
$script:SelfIdPattern = "(?:STEAM bridge: self |this account's SteamID64 = )($script:SteamId64Pattern)\b"

function Find-LogSelfIds([string[]]$paths) {
    $ids = New-Object System.Collections.Generic.List[string]
    $re = New-Object System.Text.RegularExpressions.Regex($script:SelfIdPattern)
    foreach ($p in $paths) {
        try {
            $fs = New-Object System.IO.FileStream($p, [System.IO.FileMode]::Open, [System.IO.FileAccess]::Read, [System.IO.FileShare]::ReadWrite)
            $sr = New-Object System.IO.StreamReader($fs, [System.Text.Encoding]::Default)
            try {
                while ($null -ne ($line = $sr.ReadLine())) {
                    if ($line.IndexOf('STEAM', [StringComparison]::Ordinal) -lt 0) { continue }
                    $m = $re.Match($line)
                    if ($m.Success -and -not $ids.Contains($m.Groups[1].Value)) { $ids.Add($m.Groups[1].Value) }
                }
            } finally { $sr.Dispose() }
        } catch { }
    }
    $ids.ToArray()
}

# One state for all files of one zip, so a player keeps the same pseudonym in each. $names: literal user/PC names.
function New-LogScrubState([string[]]$SelfIds = @(), [string]$UserName = $env:USERNAME, [string]$ComputerName = $env:COMPUTERNAME,
                           [string]$UserProfile = $env:USERPROFILE) {
    $self = New-Object 'System.Collections.Generic.HashSet[string]'
    foreach ($s in $SelfIds) { if ($s) { [void]$self.Add("$s") } }
    $userNames = New-Object System.Collections.Generic.List[string]
    foreach ($u in @($UserName, $(if ($UserProfile) { Split-Path -Leaf $UserProfile } else { '' }))) {
        if ($u -and $u.Length -ge 3 -and -not $userNames.Contains($u)) { $userNames.Add($u) }
    }
    $literal = New-Object System.Collections.Generic.List[object]
    foreach ($u in $userNames) { $literal.Add(@{ Re = New-Object System.Text.RegularExpressions.Regex("(?<![A-Za-z0-9])$([regex]::Escape($u))(?![A-Za-z0-9])", 'IgnoreCase'); To = '<user>'; Kind = 'User' }) }
    if ($ComputerName -and $ComputerName.Length -ge 3) {
        $literal.Add(@{ Re = New-Object System.Text.RegularExpressions.Regex("(?<![A-Za-z0-9])$([regex]::Escape($ComputerName))(?![A-Za-z0-9])", 'IgnoreCase'); To = '<pc>'; Kind = 'Pc' })
    }
    $quick = $script:SteamId64Pattern + '|\d\.\d{1,3}\.\d{1,3}\.\d|:\\Users\\'
    foreach ($u in $userNames) { $quick += '|' + [regex]::Escape($u) }
    if ($ComputerName -and $ComputerName.Length -ge 3) { $quick += '|' + [regex]::Escape($ComputerName) }
    @{
        Self = $self; Peers = @{}; Ips = @{}; LanN = 0; IpN = 0; PeerN = 0
        Literal = $literal
        Quick = New-Object System.Text.RegularExpressions.Regex($quick, 'IgnoreCase')
        Steam = New-Object System.Text.RegularExpressions.Regex("\b$script:SteamId64Pattern\b")
        # Another account's profile folder (this one's name is in Literal). The name runs to the next backslash, spaces
        # included ("C:\Users\Jane Doe\..."); with no backslash after it, it can only be taken up to the first space.
        UsersPath = New-Object System.Text.RegularExpressions.Regex('\b[A-Za-z]:\\Users\\(?!<user>)(?:[^\\/:*?"<>|\[\]]{1,64}?(?=\\)|[^\\/:*?"<>|\s\[\]]+)', 'IgnoreCase')
        # Four dot-separated numbers not inside a longer dotted run or a word ("v1.2.3.4", "1.2.3.4.5").
        Ip = New-Object System.Text.RegularExpressions.Regex('(?<![\w.])(\d{1,3})\.(\d{1,3})\.(\d{1,3})\.(\d{1,3})(?!\.?\d)(?!\w)')
        Counts = @{ SelfId = 0; PeerId = 0; User = 0; Pc = 0; Ip = 0 }
    }
}

function ConvertTo-IpPseudonym($state, [System.Text.RegularExpressions.Match]$m, [string]$line) {
    $o = @()
    foreach ($g in 1..4) {
        $s = $m.Groups[$g].Value
        if ($s.Length -gt 1 -and $s.StartsWith('0')) { return $m.Value }  # "01.02...": not an address
        $o += [int]$s
    }
    foreach ($x in $o) { if ($x -gt 255) { return $m.Value } }
    # A version written as one ("version 1.0.0.2", "build 6.1.7601.1"): kept.
    $before = $line.Substring(0, $m.Index)
    if ($before -match '(?i)\b(version|ver\.?|build|driver)\s*[:=]?\s*$') { return $m.Value }
    if ($o[0] -eq 127 -or $m.Value -eq '0.0.0.0') { return $m.Value }  # loopback / any: says nothing about anyone
    $key = $m.Value
    if (-not $state.Ips.ContainsKey($key)) {
        $lan = $o[0] -eq 10 -or ($o[0] -eq 172 -and $o[1] -ge 16 -and $o[1] -le 31) -or ($o[0] -eq 192 -and $o[1] -eq 168) -or ($o[0] -eq 169 -and $o[1] -eq 254)
        if ($lan) { $state.LanN++; $state.Ips[$key] = "lan-ip-$($state.LanN)" } else { $state.IpN++; $state.Ips[$key] = "ip-$($state.IpN)" }
    }
    $state.Counts.Ip++
    $state.Ips[$key]
}

function Protect-LogLine([string]$line, $state) {
    if (-not $line -or -not $state.Quick.IsMatch($line)) { return $line }
    $line = $state.Steam.Replace($line, [System.Text.RegularExpressions.MatchEvaluator]{
        param($m)
        if ($state.Self.Contains($m.Value)) { $state.Counts.SelfId++; return 'steam-self' }
        if (-not $state.Peers.ContainsKey($m.Value)) { $state.PeerN++; $state.Peers[$m.Value] = "steam-peer$($state.PeerN)" }
        $state.Counts.PeerId++
        $state.Peers[$m.Value]
    })
    foreach ($l in $state.Literal) {
        $n = $l.Re.Matches($line).Count
        if ($n) { $state.Counts[$l.Kind] += $n; $line = $l.Re.Replace($line, $l.To) }
    }
    $n = $state.UsersPath.Matches($line).Count
    if ($n) { $state.Counts.User += $n; $line = $state.UsersPath.Replace($line, 'C:\Users\<user>') }
    $line = $line -replace '(?i)\b[A-Z]:\\Users\\<user>', 'C:\Users\<user>'
    $current = $line
    $current = $state.Ip.Replace($current, [System.Text.RegularExpressions.MatchEvaluator]{ param($m) ConvertTo-IpPseudonym $state $m $current })
    $current
}

function Protect-LogFile([string]$from, [string]$to, $state) {
    $fs = New-Object System.IO.FileStream($from, [System.IO.FileMode]::Open, [System.IO.FileAccess]::Read, [System.IO.FileShare]::ReadWrite)
    try {
        $sr = New-Object System.IO.StreamReader($fs, [System.Text.Encoding]::Default)
        $sw = New-Object System.IO.StreamWriter($to, $false, [System.Text.Encoding]::Default)
        try {
            $quick = $state.Quick
            while ($null -ne ($line = $sr.ReadLine())) {
                if ($quick.IsMatch($line)) { $line = Protect-LogLine $line $state }
                $sw.WriteLine($line)
            }
        } finally { $sw.Dispose(); $sr.Dispose() }
    } finally { $fs.Dispose() }
}

function Get-PrivacyNote($state) {
    $c = $state.Counts
    @(
        'These logs were cleaned by MK9 Rollback before they were saved:',
        '- Steam IDs: yours became steam-self; other players became steam-peer1, steam-peer2, ... in the order they first',
        '  appear. The same player has the same name in every file of this zip.',
        '- Your Windows user name became <user>, and C:\Users\<name> became C:\Users\<user>.',
        '- This PC''s name became <pc>.',
        '- IP addresses became lan-ip-1, lan-ip-2, ... (home network) or ip-1, ip-2, ... (internet).',
        '  127.0.0.1 (this PC) and 0.0.0.0 were kept, and so were version numbers.',
        '',
        "Replaced: $($c.SelfId + $c.PeerId) Steam ID(s) ($($state.PeerN) other player(s)), $($c.Ip) IP address(es) ($($state.LanN) home, $($state.IpN) internet), $($c.User) user name(s), $($c.Pc) PC name(s).",
        'Nothing else was changed.'
    )
}

# Copies every log of $dir (and previous_logs) through Protect-LogFile into one zip in $destDir. Returns Zip (the path,
# or '' with Error set) and Note (the counts line of PRIVACY.txt).
function Save-LogsZip([string]$dir, [string]$destDir, [string[]]$selfIds = @()) {
    $stamp = Get-Date -Format 'yyyyMMdd-HHmmss'
    $stage = Join-Path $env:TEMP "mk9r-logs-$stamp-$(Get-Random)"
    New-Item -ItemType Directory -Force -Path $stage | Out-Null
    try {
        $src = @()
        $src += @(Get-ChildItem -LiteralPath $dir -Filter 'MK9Rollback*.log' -File -ErrorAction SilentlyContinue | ForEach-Object { @{ From = $_.FullName; Name = $_.Name } })
        $prev = Join-Path $dir 'previous_logs'
        if (Test-Path -LiteralPath $prev) {
            $src += @(Get-ChildItem -LiteralPath $prev -Filter 'MK9Rollback*.log' -File -ErrorAction SilentlyContinue | ForEach-Object { @{ From = $_.FullName; Name = "previous_$($_.Name)" } })
        }
        $ids = @($selfIds) + @(Find-LogSelfIds @($src | ForEach-Object { $_.From }))
        $state = New-LogScrubState -SelfIds $ids
        foreach ($s in $src) {
            $to = Join-Path $stage $s.Name
            try { Protect-LogFile $s.From $to $state } catch { Remove-Item -LiteralPath $to -Force -ErrorAction SilentlyContinue }
        }
        Get-ChildItem -LiteralPath $stage -File | Where-Object { $_.Length -eq 0 } | Remove-Item -Force -ErrorAction SilentlyContinue
        if (-not @(Get-ChildItem -LiteralPath $stage -File)) { return [pscustomobject]@{ Zip = ''; Error = 'No logs yet (or they could not be read).'; Note = '' } }
        $note = Get-PrivacyNote $state
        [System.IO.File]::WriteAllLines((Join-Path $stage 'PRIVACY.txt'), [string[]]$note, [System.Text.Encoding]::ASCII)
        $zip = Join-Path $destDir "mk9r-logs-$stamp.zip"
        if (Test-Path -LiteralPath $zip) { Remove-Item -LiteralPath $zip -Force }
        Add-Type -AssemblyName System.IO.Compression.FileSystem
        [System.IO.Compression.ZipFile]::CreateFromDirectory($stage, $zip)
        if (-not (Test-Path -LiteralPath $zip) -or (Get-Item -LiteralPath $zip).Length -le 0) { return [pscustomobject]@{ Zip = ''; Error = 'The logs zip could not be made.'; Note = '' } }
        [pscustomobject]@{ Zip = $zip; Error = ''; Note = $note[8] }
    } catch {
        [pscustomobject]@{ Zip = ''; Error = "The logs zip could not be made ($($_.Exception.Message))."; Note = '' }
    } finally {
        Remove-Item -LiteralPath $stage -Recurse -Force -ErrorAction SilentlyContinue
    }
}

# ---- last match ----
# From the DLL's lines: "ONLINE: input delay N agreed" / "input delay A -> B between rounds" (src\net\OnlineMode.cpp),
# "ONLINE: round trip to the other game N ms", "the match is over ... hello round trip this match: median N ms",
# "GGPO p2p stopped (why): F frames, L loads" (src\rollback\RollbackManager.cpp; a load is one rollback),
# "NETPLAY checksums: M frames matched the peer, D did not", "GGPO p2p started ... frame delay N" (Local Versus).
function New-MatchFacts { @{ Delay = $null; Rtt = $null; Frames = 0; Loads = 0; Fights = 0; DesyncFights = 0; Over = $false; Seen = $false } }
function Update-MatchFacts($f, [string]$line) {
    if ($line -match 'ONLINE: input delay (\d+) agreed') {
        if ($f.Over) { $n = New-MatchFacts; foreach ($k in @($n.Keys)) { $f[$k] = $n[$k] } }
        $f.Delay = [int]$Matches[1]; $f.Seen = $true; return $true
    }
    if ($line -match 'ONLINE: input delay \d+ -> (\d+) between rounds') { $f.Delay = [int]$Matches[1]; return $true }
    if ($line -match 'ONLINE: round trip to the other game (\d+) ms') { $f.Rtt = [int]$Matches[1]; return $false }
    if ($line -match 'GGPO p2p started .*frame delay (\d+)') {
        if ($f.Over) { $n = New-MatchFacts; foreach ($k in @($n.Keys)) { $f[$k] = $n[$k] } }
        if ($null -eq $f.Delay) { $f.Delay = [int]$Matches[1] }
        $f.Seen = $true; return $false
    }
    if ($line -match 'GGPO p2p stopped \([^)]*\): (\d+) frames, (\d+) loads') {
        $f.Frames += [int]$Matches[1]; $f.Loads += [int]$Matches[2]; $f.Fights++; $f.Seen = $true; return $true
    }
    if ($line -match 'NETPLAY checksums: \d+ frames matched the peer, (\d+) did not') {
        if ([int]$Matches[1] -gt 0) { $f.DesyncFights++ }
        return $true
    }
    if ($line -match 'ONLINE: the match is over') {
        if ($line -match 'median (\d+) ms') { $f.Rtt = [int]$Matches[1] }
        $f.Over = $true; return $true
    }
    $false
}
function Format-MatchFacts($f) {
    if (-not $f.Seen) { return 'Last match: none yet.' }
    $parts = @()
    if ($null -ne $f.Delay) { $parts += "input delay $($f.Delay)" }
    if ($null -ne $f.Rtt) { $parts += "ping $($f.Rtt) ms" }
    if ($f.Frames -ge 600) { $parts += "$([Math]::Round($f.Loads * 3600.0 / $f.Frames)) rollbacks a minute" }
    if ($f.Fights) { $parts += $(if ($f.DesyncFights) { "out of sync in $($f.DesyncFights) fight(s)" } else { 'always in sync' }) }
    "Last match: $($parts -join ', ')."
}

# ---- connection ----
# Wi-Fi when the adapter of the lowest-metric default route is a wireless one. '' when unknown (no NetTCPIP/NetAdapter
# modules, no route, any error).
function Get-ConnectionHint {
    try {
        if (-not (Get-Command Get-NetRoute -ErrorAction SilentlyContinue) -or -not (Get-Command Get-NetAdapter -ErrorAction SilentlyContinue)) { return '' }
        $routes = @(Get-NetRoute -DestinationPrefix '0.0.0.0/0' -ErrorAction Stop)
        if (-not $routes) { return '' }
        $best = $routes | Sort-Object { [int]$_.RouteMetric + [int]$_.InterfaceMetric } | Select-Object -First 1
        $a = Get-NetAdapter -InterfaceIndex $best.ifIndex -ErrorAction Stop
        if ("$($a.PhysicalMediaType)" -eq 'Native 802.11' -or "$($a.PhysicalMediaType)" -eq 'Wireless LAN') { return 'You are on Wi-Fi: a cable gives steadier fights.' }
    } catch { }
    ''
}
