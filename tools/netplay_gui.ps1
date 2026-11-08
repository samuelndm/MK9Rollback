# One-window netplay launcher for players ("Play Online.bat" starts it). Over Steam only: each player shares their
# Steam friend code (the number on Steam's "Add a Friend" page), pastes the other's, and presses Play. The window then
# starts the game, injects MK9Rollback, arms the session and translates the log into plain status lines.
# Sides are chosen without asking: the lower friend code is player 1 (left side), on both PCs alike.
# The same steps as tools\netplay.ps1, run from a timer so the window stays responsive.
# Netcode: rollback unless the player ticks "Safe mode" (lockstep: no snapshots or loads). The default was Safe mode
# until 2026-11-01, when the players chose rollback as the default ("focus on the lag, ping, rollback").
# In the MK9 Online mode the input delay follows the ping ('online auto ping'): the two games measure the round trip
# before the fight and both take the larger proposal (rollback 2-6 frames, Safe mode 2-3), then keep measuring and
# adjust it between rounds.
$ErrorActionPreference = 'Continue'
Add-Type -AssemblyName System.Windows.Forms, System.Drawing
[System.Windows.Forms.Application]::EnableVisualStyles()

$root = Resolve-Path "$PSScriptRoot\.."
$dir = "$root\build\clang"
$log = "$dir\MK9Rollback.log"
$settings = "$dir\netplay_gui.txt"
$kSteamBase = [uint64]76561197960265728
# Update check and install from the GitHub releases (Get-InstalledVersion, Get-LatestRelease, Install-Update).
. "$PSScriptRoot\updater.ps1"
$installed = Get-InstalledVersion "$root"
# Files extracted from a downloaded zip carry the internet zone mark; the injector and the DLL would then be blocked.
Get-ChildItem $root -Recurse -File -ErrorAction SilentlyContinue | Unblock-File -ErrorAction SilentlyContinue

function MyAccountId {
    $u = (Get-ItemProperty 'HKCU:\Software\Valve\Steam\ActiveProcess' -ErrorAction SilentlyContinue).ActiveUser
    if ($u) { [uint64]$u } else { [uint64]0 }
}
# A friend code (account ID) or a 17-digit SteamID64 -> account ID; 0 if neither.
function ParseCode([string]$s) {
    $s = ($s -replace '[^\d]', '')
    if (-not $s) { return [uint64]0 }
    $v = [uint64]0
    if (-not [uint64]::TryParse($s, [ref]$v)) { return [uint64]0 }
    if ($v -gt $kSteamBase) { return $v - $kSteamBase }
    if ($v -lt 4294967296) { return $v }
    [uint64]0
}
function Cmd([string[]]$lines) {
    Set-Content -Encoding ascii "$dir\cmd.tmp" $lines
    Move-Item "$dir\cmd.tmp" "$dir\MK9Rollback_cmd.txt" -Force
}

# ---- window ----
$form = New-Object System.Windows.Forms.Form
$form.Text = 'MK9 Rollback - Play Online'
$form.ClientSize = New-Object System.Drawing.Size(460, 530)
$form.FormBorderStyle = 'FixedDialog'
$form.MaximizeBox = $false
$form.StartPosition = 'CenterScreen'
$form.Font = New-Object System.Drawing.Font('Segoe UI', 10)
function Label($text, $x, $y, $w = 420, $h = 22) {
    $l = New-Object System.Windows.Forms.Label
    $l.Text = $text; $l.Location = New-Object System.Drawing.Point($x, $y); $l.Size = New-Object System.Drawing.Size($w, $h)
    $form.Controls.Add($l); $l
}
function Button($text, $x, $y, $w, $h = 30) {
    $b = New-Object System.Windows.Forms.Button
    $b.Text = $text; $b.Location = New-Object System.Drawing.Point($x, $y); $b.Size = New-Object System.Drawing.Size($w, $h)
    $form.Controls.Add($b); $b
}

Label '1. Send your code to your friend:' 20 15 | Out-Null
$myBox = New-Object System.Windows.Forms.TextBox
$myBox.Location = New-Object System.Drawing.Point(20, 40); $myBox.Size = New-Object System.Drawing.Size(200, 26)
$myBox.ReadOnly = $true; $myBox.Font = New-Object System.Drawing.Font('Consolas', 12)
$form.Controls.Add($myBox)
$copy = Button 'Copy' 230 38 80

Label "2. Paste your friend's code:" 20 80 | Out-Null
$peerBox = New-Object System.Windows.Forms.TextBox
$peerBox.Location = New-Object System.Drawing.Point(20, 105); $peerBox.Size = New-Object System.Drawing.Size(200, 26)
$peerBox.Font = New-Object System.Drawing.Font('Consolas', 12)
$form.Controls.Add($peerBox)
$side = Label '' 230 108 220

Label 'Where you fight:' 20 143 120 | Out-Null
$where = New-Object System.Windows.Forms.ComboBox
$where.DropDownStyle = 'DropDownList'
$where.Location = New-Object System.Drawing.Point(140, 140); $where.Size = New-Object System.Drawing.Size(300, 26)
[void]$where.Items.Add('MK9 Online: Player Match (experimental)')
[void]$where.Items.Add('Local Versus on both PCs (tested)')
$where.SelectedIndex = 0
$form.Controls.Add($where)
function IsOnline { $where.SelectedIndex -eq 0 }

$safe = New-Object System.Windows.Forms.CheckBox
$safe.Text = 'Safe mode (no rollback: try it if fights go out of sync)'
$safe.Location = New-Object System.Drawing.Point(20, 172); $safe.Size = New-Object System.Drawing.Size(420, 24)
$form.Controls.Add($safe)
Label "   Unticked = Rollback. The input delay follows the ping (2 frames at a`n   normal ping). Both players must use the same setting." 20 194 420 40 |
    ForEach-Object { $_.ForeColor = [System.Drawing.Color]::Gray }
function ModeName { if ($safe.Checked) { 'Safe mode' } else { 'Rollback' } }
$script:modeName = 'Rollback'

$play = Button '3. Play' 20 242 420 40
$play.Font = New-Object System.Drawing.Font('Segoe UI', 12, [System.Drawing.FontStyle]::Bold)

$status = New-Object System.Windows.Forms.TextBox
$status.Location = New-Object System.Drawing.Point(20, 294); $status.Size = New-Object System.Drawing.Size(420, 150)
$status.Multiline = $true; $status.ReadOnly = $true; $status.ScrollBars = 'Vertical'
$form.Controls.Add($status)
$saveLogs = Button 'Save logs to the Desktop (for bug reports)' 20 452 420 30
$verLabel = Label "Version $(if ($installed.version) { $installed.version } else { '(unknown)' })" 20 496 200
$verLabel.ForeColor = [System.Drawing.Color]::Gray
$updBtn = Button 'Check for updates' 240 490 200 30

function Say([string]$s) {
    $status.AppendText("$(Get-Date -Format 'HH:mm:ss')  $s`r`n")
}

# Updates: checked once when the window opens (quietly: only a newer version is mentioned) and on the button.
$script:release = $null
function CheckUpdate([switch]$Quiet) {
    $script:release = Get-LatestRelease -TimeoutSec $(if ($Quiet) { 4 } else { 10 })
    if (-not $script:release) { if (-not $Quiet) { Say "Update check failed: $script:UpdateError" }; return }
    if (Test-UpdateNewer $script:release $installed.version) {
        Say "Update available: version $($script:release.Version). Close the game, then press 'Update now' at the bottom. Your friend needs the same version."
        $updBtn.Text = "Update now ($($script:release.Version))"
    } elseif (-not $Quiet) {
        Say "You have the latest version ($($installed.version))."
    }
}
$updBtn.Add_Click({
    if (-not ($script:release -and (Test-UpdateNewer $script:release $installed.version))) { CheckUpdate; return }
    if ($script:state -ne 'idle' -or (Get-Process MKKE -ErrorAction SilentlyContinue)) { Say 'Close the game first, then press Update now.'; return }
    $answer = [System.Windows.Forms.MessageBox]::Show("Install version $($script:release.Version)?`n`nYour friend needs the same version to play with you.", 'MK9 Rollback update', 'YesNo', 'Question')
    if ($answer -ne 'Yes') { return }
    $updBtn.Enabled = $false; $play.Enabled = $false
    if (Install-Update $script:release "$root" { param($s) Say $s; [System.Windows.Forms.Application]::DoEvents() }) {
        Say 'Restarting this window with the new version...'
        Start-Process -FilePath "$root\Play Online.bat"
        $form.Close()
        return
    }
    $updBtn.Enabled = $true; $play.Enabled = $true
})
$form.Add_Shown({ [System.Windows.Forms.Application]::DoEvents(); CheckUpdate -Quiet })

$me = MyAccountId
if ($me) { $myBox.Text = "$me" } else { $myBox.Text = '(start Steam)' }
if (Test-Path $settings) {
    $peerBox.Text = (Get-Content $settings -TotalCount 1)
    # Only an explicit 'safe' line opts in to Safe mode (saved since 2026-11-01); older files ('rollback' or nothing)
    # open in rollback, the default.
    $safe.Checked = [bool](Get-Content $settings | Select-String '^safe$' -Quiet)
    if (Get-Content $settings | Select-String '^local$' -Quiet) { $where.SelectedIndex = 1 }
}

function UpdateSide {
    $p = ParseCode $peerBox.Text
    if (-not $me -or -not $p) { $side.Text = ''; return }
    if (IsOnline) { $side.Text = ''; return }  # MK9's online mode decides the sides
    if ($p -eq $me) { $side.Text = "That's your own code"; return }
    $side.Text = if ($me -lt $p) { 'You: Player 1 (left)' } else { 'You: Player 2 (right)' }
}
# MK9 Online needs no codes (the mod finds the opponent in the game's own connection, 'online auto'); only the Local
# Versus mode has to be told who the other player is.
$onlineNote = Label "No codes needed: press Play, then play anyone in Online > Player Match who also started the game with Play Online.bat." 20 20 420 100
function UpdateMode {
    $codes = -not (IsOnline)
    foreach ($c in $form.Controls) { if ($c.Top -lt 135 -and $c -ne $onlineNote) { $c.Visible = $codes } }
    $onlineNote.Visible = -not $codes
}
# While the game runs with the mod, a change of the box is sent at once (2026-10-31: players ticked it between two
# online matches and kept Safe mode, it was only read at Play). A fight already under way keeps its netcode.
function SaveSettings {
    Set-Content -Encoding ascii $settings (@("$($script:peer)") + $(if ($safe.Checked) { 'safe' } else { @() }) + $(if (IsOnline) { @() } else { 'local' }))
}
$safe.Add_CheckedChanged({
    if ($script:state -ne 'watch') { return }
    Say "Netcode changed to $(ModeName). A fight already under way keeps its netcode; the change applies from the next one."
    SaveSettings
    Arm
})
$peerBox.Add_TextChanged({ UpdateSide })
$where.Add_SelectedIndexChanged({ UpdateSide; UpdateMode })
UpdateSide
UpdateMode
$copy.Add_Click({ if ($me) { [System.Windows.Forms.Clipboard]::SetText("$me"); Say 'Your code is copied. Send it to your friend.' } })

$saveLogs.Add_Click({
    $stage = Join-Path $env:TEMP "mk9r-logs-$(Get-Date -Format 'yyyyMMdd-HHmmss')"
    New-Item -ItemType Directory -Force $stage | Out-Null
    Get-ChildItem "$dir\MK9Rollback*.log" -ErrorAction SilentlyContinue | ForEach-Object { Copy-Item $_.FullName $stage -ErrorAction SilentlyContinue }
    Get-ChildItem "$dir\previous_logs\MK9Rollback*.log" -ErrorAction SilentlyContinue |
        ForEach-Object { Copy-Item $_.FullName (Join-Path $stage "previous_$($_.Name)") -ErrorAction SilentlyContinue }
    if (-not (Get-ChildItem $stage)) { Say 'No logs yet.'; return }
    $zip = Join-Path ([Environment]::GetFolderPath('Desktop')) "$(Split-Path $stage -Leaf).zip"
    Compress-Archive -Path "$stage\*" -DestinationPath $zip -Force
    Remove-Item $stage -Recurse -Force
    Say "Logs saved: $zip"
})

# ---- steps, driven by a 1 s timer ----
$script:state = 'idle'
$script:wait = 0
$script:logPos = 0
$script:launcher = $null
$timer = New-Object System.Windows.Forms.Timer
$timer.Interval = 1000

function Arm([switch]$Next) {
    $script:logPos = @(Get-Content $log -ErrorAction SilentlyContinue).Count
    # One command per mode: Safe mode installs only the lockstep patch set (docs/restructure_plan.md, phase 1). Sent
    # again on a re-arm or a checkbox change, it only switches the mode (a running fight keeps its netcode).
    $lines = @($(if ($safe.Checked) { 'netcode safe' } else { 'netcode rollback' }))
    # Rollback at 2 frames: in the simulated-lag runs (2026-10-28) delay 1 replayed 4-8 times as many frames as delay 2
    # at 40 and 80 ms one way (and one 40 ms / delay 1 run crashed, results\lag_matrix*.csv). Online, the delay follows
    # the ping from there ('online auto ping'); Local Versus keeps the fixed delays.
    $delay = if ($safe.Checked) { 3 } else { 2 }
    $script:modeName = ModeName
    if ($safe.Checked) { Say 'Netcode: Safe mode (no rollback). Your opponent must tick Safe mode too.' }
    else { Say 'Netcode: Rollback. Your opponent must leave Safe mode unticked too.' }
    if (IsOnline) {
        Cmd ($lines + 'online auto ping')
        Say 'Ready. In the game: Online > Player Match, a 1 vs 1 room (not King of the Hill). Play anyone there.'
        Say "If your opponent also started the game with Play Online.bat, $($script:modeName) takes over at the start of the fight. Against anyone else, MK9's own netcode is used. Ranked is never touched."
        $script:state = 'watch'
        $play.Text = 'Ready (press to re-arm)'
        $play.Enabled = $true
        return
    }
    $peer64 = $kSteamBase + $script:peer
    $player = if ($me -lt $script:peer) { 1 } else { 2 }
    $lines += 'online off'  # a game that was in the MK9 Online mode before stops watching online fights
    $lines += "ggpo steamnetplay $peer64 $player $delay"
    Cmd $lines
    $sideText = if ($player -eq 1) { 'Player 1 (LEFT side)' } else { 'Player 2 (RIGHT side)' }
    if ($Next) { Say "Ready for the next fight (you are $sideText): pick it the same way on both PCs." }
    else {
        Say "Ready. You are $sideText."
        Say 'In the game: Versus > Local Versus. Agree with your friend on both characters and the stage, pick them the same on both PCs (P1 left, P2 right), and start the fight within a minute of each other.'
    }
    $script:state = 'watch'
    $play.Text = 'Waiting for the fight (press to re-arm)'
    $play.Enabled = $true
}

# Log line -> plain status (first match wins); $null = not shown.
$messages = @(
    @{ p = 'NETPLAY armed'; m = $null },
    @{ p = 'ONLINE: hooks not installed'; m = 'The online mode could not hook this game version. Use Local Versus instead.' },
    @{ p = 'ONLINE auto: the game''s online opponent is'; m = 'Online fight found. Checking whether your opponent runs the mod...' },
    @{ p = 'ONLINE auto: no opponent found yet'; m = $null },
    @{ p = 'ONLINE: hello from the other player'; m = 'Your opponent''s mod answered: {mode} can take over.' },
    @{ p = 'ONLINE: both games run MK9Rollback'; m = 'Both games run the mod.' },
    @{ p = 'ONLINE: not engaging in this match'; m = 'This room is not a 1 vs 1 Player/Private match (King of the Hill or Ranked): MK9''s own netcode plays it.' },
    @{ p = 'ONLINE: the other player runs (Safe mode|Rollback), this game (Safe mode|Rollback)'; m = 'Your opponent uses {1}, you use {2}: both must pick the same setting. MK9''s netcode plays until you do.' },
    @{ p = 'speaks online version'; m = 'Your opponent runs another version of the mod: both players need the same version (press ''Check for updates'' at the bottom, both of you). MK9''s netcode plays.' },
    @{ p = 'ONLINE: the session starts at the first fight frame'; m = '{mode} takes over when this fight starts.' },
    @{ p = 'ONLINE: the session starts at the next fight start'; m = 'This fight already started: {mode} takes over from the next fight.' },
    @{ p = 'ONLINE: the fight ended; rollback re-armed'; m = 'Fight over. {mode} stays on for the next fight.' },
    @{ p = 'ONLINE: this game controls player (\d)'; m = '{mode} on. You are player {1}.' },
    @{ p = 'ONLINE: input delay (\d) agreed'; m = 'Input delay: {1} frames (picked from the ping).' },
    @{ p = 'ONLINE: input delay \d -> (\d) between rounds'; m = 'Input delay: {1} frames from this round (the ping changed).' },
    @{ p = 'ONLINE: the match is over'; m = 'Match over. The results and rematch screens run on MK9''s netcode; {mode} takes over again at the next fight.' },
    @{ p = 'ONLINE: rollback disengaged \(match over\)'; m = $null },
    @{ p = 'ONLINE: rollback disengaged \((.*?)\);'; m = 'Back to MK9''s own netcode ({1}).' },
    @{ p = 'STEAM bridge: steam_api.dll interfaces unavailable'; m = 'Steam is not available to the game. Is Steam running and signed in?' },
    @{ p = 'NETPLAY: first fight frame'; m = 'Fight loaded. Waiting for your friend to start the same fight...' },
    @{ p = 'GGPO synchronized with peer'; m = 'Connected to your friend.' },
    @{ p = 'NETPLAY: both peers running'; m = 'FIGHT! Both games are running.' },
    @{ p = 'NETPLAY environment matches'; m = 'Game files and mod version match your friend''s.' },
    @{ p = 'NETPLAY ENVIRONMENT MISMATCH: (.*) differs'; m = 'Your setup differs from your friend''s: {1}. Use the same mod version (''Check for updates'' at the bottom) and an unmodified game.' },
    @{ p = 'NETPLAY: frame 0 differs'; m = 'The two fights don''t match. Pick the same characters on the same sides, the same stage and settings, then fight again.' },
    @{ p = 'NETPLAY DESYNC: frame (\d+)'; m = 'OUT OF SYNC at frame {1}: the two screens may now differ. Please save the logs after this fight.' },
    @{ p = 'NETPLAY checksums: (\d+) frames matched the peer, (\d+) did not'; m = 'Fight over: {1} frames in sync, {2} out of sync.' },
    @{ p = 'did not join within'; m = 'Your friend''s game did not join in time. This fight continues without the mod''s netcode.'; rearm = $true },
    @{ p = 'NETPLAY: the session could not start'; m = 'The connection to your friend could not be set up (a network port may be in use). This fight continues without the mod''s netcode.'; rearm = $true },
    @{ p = 'the other player disconnected'; m = 'Your friend disconnected. The fight continues offline; the next fight tries again.' },
    @{ p = 'CRASH\?|HANG\?'; m = 'The mod reported a crash or hang. Please save the logs.' },
    @{ p = 'GGPO p2p stopped'; m = $null; rearm = $true }  # each fight is one session; arm the next one
)
$script:shownDesync = 0

$timer.Add_Tick({
    switch ($script:state) {
        'launch' {
            if (Get-Process MKKE -ErrorAction SilentlyContinue) { Say 'The game is running. Waiting for its menu...'; $script:state = 'settle'; $script:wait = 0; return }
            if (++$script:wait -gt 150) { Say 'The game did not start. Start it from Steam, then press Play again.'; $script:state = 'idle'; $play.Enabled = $true }
        }
        'settle' {
            if (++$script:wait -ge 15) { $script:state = 'inject'; $script:wait = 0 }
        }
        'inject' {
            if (@(Get-Process MKKE -ErrorAction SilentlyContinue).Count -gt 1) { Say 'Two copies of the game are running. Close one.'; $script:state = 'idle'; $play.Enabled = $true; return }
            # The last session's logs are kept for "Save logs": a player who restarts after a crash would lose them.
            if (Get-ChildItem "$dir\MK9Rollback*.log" -ErrorAction SilentlyContinue) {
                Remove-Item "$dir\previous_logs" -Recurse -Force -ErrorAction SilentlyContinue
                New-Item -ItemType Directory -Force "$dir\previous_logs" | Out-Null
                Move-Item "$dir\MK9Rollback*.log" "$dir\previous_logs" -Force -ErrorAction SilentlyContinue
            }
            Remove-Item "$dir\MK9Rollback*.log" -ErrorAction SilentlyContinue
            $out =(& "$dir\MK9RollbackInjector.exe" inject 2>&1 | Out-String)
            if ($out -match 'Unknown MKKE.exe build') { Say 'This game version is not supported (it must be the Steam Komplete Edition, unmodified).'; $script:state = 'idle'; $play.Enabled = $true; return }
            $script:state = 'injectwait'; $script:wait = 0
        }
        'injectwait' {
            if (Test-Path $log) { Say 'Mod loaded.'; Arm; return }
            if (++$script:wait -ge 10) {
                if (++$script:tries -lt 6) { $script:state = 'inject'; return }
                Say 'Could not load the mod into the game. Your antivirus may have blocked MK9RollbackInjector.exe. Please save the logs.'
                $script:state = 'idle'; $play.Enabled = $true
            }
        }
        'watch' {
            if (-not (Get-Process MKKE -ErrorAction SilentlyContinue)) {
                Say 'The game has closed.'; $script:state = 'idle'; $play.Text = '3. Play'; $play.Enabled = $true; return
            }
            $all = @(Get-Content $log -ErrorAction SilentlyContinue)
            if ($all.Count -le $script:logPos) { return }
            $rearm = $false
            foreach ($line in $all[$script:logPos..($all.Count - 1)]) {
                foreach ($e in $messages) {
                    $m = [regex]::Match($line, $e.p)
                    if (-not $m.Success) { continue }
                    if ($e.p -like 'NETPLAY DESYNC*') { if ($script:shownDesync++) { break } }
                    if ($e.p -like 'NETPLAY checksums*') { $script:shownDesync = 0 }
                    if ($e.m) { Say ($e.m -replace '\{1\}', $m.Groups[1].Value -replace '\{2\}', $m.Groups[2].Value -replace '\{mode\}', $script:modeName) }
                    if ($e.rearm) { $rearm = $true }
                    break
                }
            }
            $script:logPos = $all.Count
            if ($rearm -and -not (IsOnline)) { Arm -Next }  # the online mode re-engages by itself for every match
        }
    }
})

$play.Add_Click({
    if (-not $me) { $script:me = MyAccountId; $me = $script:me; if ($me) { $myBox.Text = "$me" } }
    if (-not $me) { Say 'Start Steam and sign in first.'; return }
    $script:peer = ParseCode $peerBox.Text
    if (-not (IsOnline) -and (-not $script:peer -or $script:peer -eq $me)) { Say "Paste your friend's code first (box 2)."; return }
    SaveSettings
    if (-not (Test-Path "$dir\MK9RollbackInjector.exe")) { Say "The mod files are missing next to this launcher ($dir)."; return }
    if ($script:state -eq 'watch') { Arm; return }
    $play.Enabled = $false
    $script:tries = 0
    $game = Get-Process MKKE -ErrorAction SilentlyContinue | Select-Object -First 1
    if (-not $game) {
        Say 'Starting Mortal Kombat through Steam...'
        Start-Process powershell -WindowStyle Hidden -ArgumentList '-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', "`"$PSScriptRoot\launch_game.ps1`""
        $script:state = 'launch'; $script:wait = 0
    } elseif ((Test-Path $log) -and ((Get-Item $log).LastWriteTime -gt $game.StartTime)) {
        Arm  # this game already has the mod
    } else {
        Say 'The game is already running. Loading the mod...'
        $script:state = 'inject'
    }
})

$timer.Start()
if (IsOnline) { Say 'Press Play. The game starts with the mod.' } else { Say 'Both players: start this launcher, swap codes, then press Play.' }
[void]$form.ShowDialog()
$timer.Stop()
