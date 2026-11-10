# One-window netplay launcher for players ("Play Online.bat" starts it). Over Steam only: each player shares their
# Steam friend code (the number on Steam's "Add a Friend" page), pastes the other's, and presses Play. The window then
# starts the game, injects MK9Rollback, arms the session and translates the log into plain status lines.
# Sides are chosen without asking: the lower friend code is player 1 (left side), on both PCs alike.
# The same steps as tools\netplay.ps1, run from a timer so the window stays responsive.
# Netcode: rollback unless the player ticks "Safe mode" (lockstep: no snapshots or loads). The default was Safe mode
# until 2026-11-01, when the players chose rollback as the default ("focus on the lag, ping, rollback").
# In the MK9 Online mode the input delay follows the ping ('online auto ping'): the two games measure the round trip
# before the fight and both take the larger proposal (rollback 1-6 frames, Safe mode 2-3), then keep measuring and
# adjust it between rounds.
# The log-line table, the injector's exit codes, the log reader, the log privacy filter and the other pure helpers are
# in tools\launcher_lib.ps1 (checked by tools\launcher_selftest.ps1). The update check, the download and saving the logs
# run in background runspaces, polled by a timer, so the window never freezes on the network.
#   -NoUpdateCheck       no update check when the window opens
#   -SnapshotPng <file>  saves a picture of the window 2.5 s after it opens, then closes it (layout check)
param([switch]$NoUpdateCheck, [string]$SnapshotPng = '')

# Started hidden ("Play Online.bat"), so nothing may fail silently. A PC that forces PowerShell's Constrained Language
# Mode (company or school policy, some security software) cannot run this window at all: say so in Notepad.
if ($ExecutionContext.SessionState.LanguageMode -ne 'FullLanguage') {
    $errFile = Join-Path $env:TEMP 'MK9Rollback-launcher-error.txt'
    Set-Content -Path $errFile -Value @('MK9 Rollback cannot open its window on this PC.',
        'Windows runs PowerShell in a restricted mode here (Constrained Language Mode), set by whoever manages this PC',
        '(a company, a school, or security software). Ask them to allow it, or play from another PC.')
    Start-Process notepad.exe -ArgumentList "`"$errFile`""
    exit 1
}
# Any error that stops this script: written to %TEMP%\MK9Rollback-launcher-error.txt and shown.
trap {
    $errFile = Join-Path $env:TEMP 'MK9Rollback-launcher-error.txt'
    $msg = "The Play Online window stopped because of an error:`r`n`r`n$($_.Exception.Message)`r`n$($_.InvocationInfo.PositionMessage)"
    try { Set-Content -LiteralPath $errFile -Value $msg -Encoding ascii } catch { }
    $shown = $false
    try {
        Add-Type -AssemblyName System.Windows.Forms
        [void][System.Windows.Forms.MessageBox]::Show("$msg`r`n`r`n(Saved in $errFile; please send it with your bug report.)", 'MK9 Rollback', 'OK', 'Error')
        $shown = $true
    } catch { }
    if (-not $shown) { try { Start-Process notepad.exe -ArgumentList "`"$errFile`"" } catch { } }
    exit 1
}
$ErrorActionPreference = 'Continue'

# Declared before any window exists: Windows then draws the window sharp at 125-200 % display scaling instead of
# stretching a blurry bitmap (the controls are scaled by AutoScaleMode Dpi below).
try {
    if (-not ('MK9R.Native' -as [type])) {
        Add-Type -Namespace MK9R -Name Native -MemberDefinition @'
[DllImport("user32.dll")] public static extern bool SetProcessDPIAware();
[DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr hWnd);
[DllImport("user32.dll")] public static extern bool ShowWindowAsync(IntPtr hWnd, int nCmdShow);
'@
    }
    [void][MK9R.Native]::SetProcessDPIAware()
} catch { }
Add-Type -AssemblyName System.Windows.Forms, System.Drawing
[System.Windows.Forms.Application]::EnableVisualStyles()

# One window at a time: a second one would fight the first over the game and the command file.
$script:mutex = New-Object System.Threading.Mutex($false, 'Local\MK9RollbackLauncher')
$script:mutexOwned = $false
try { $script:mutexOwned = $script:mutex.WaitOne(500) } catch [System.Threading.AbandonedMutexException] { $script:mutexOwned = $true }
if (-not $script:mutexOwned) {
    [void][System.Windows.Forms.MessageBox]::Show('The Play Online window is already open.', 'MK9 Rollback', 'OK', 'Information')
    $first = Get-Process powershell, pwsh -ErrorAction SilentlyContinue |
        Where-Object { $_.Id -ne $PID -and $_.MainWindowTitle -like 'MK9 Rollback - Play Online*' } | Select-Object -First 1
    if ($first) { try { [void][MK9R.Native]::ShowWindowAsync($first.MainWindowHandle, 9); [void][MK9R.Native]::SetForegroundWindow($first.MainWindowHandle) } catch { } }
    exit 0
}

$root = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot '..')).Path
$dir = Join-Path $root 'build\clang'
$log = Join-Path $dir 'MK9Rollback.log'
$settings = Join-Path $dir 'netplay_gui.txt'
$kSteamBase = [uint64]76561197960265728
$libPath = Join-Path $PSScriptRoot 'launcher_lib.ps1'
$updaterPath = Join-Path $PSScriptRoot 'updater.ps1'
foreach ($p in @($libPath, $updaterPath)) { if (-not (Test-Path -LiteralPath $p)) { throw "$p is missing. Extract the zip again into an empty folder." } }
# Update check and install from the GitHub releases (Get-InstalledVersion, Get-LatestRelease, Install-Update,
# Restore-PreviousVersion), and the window's pure helpers.
. $updaterPath
. $libPath
$installed = Get-InstalledVersion $root
# Files extracted from a downloaded zip carry the internet zone mark; the injector and the DLL would then be blocked.
# Only the folders a release holds (a developer tree has big result folders).
@(Get-ChildItem -LiteralPath $root -File -ErrorAction SilentlyContinue) +
    @(foreach ($sub in 'tools', 'build\clang', 'docs') { Get-ChildItem -LiteralPath (Join-Path $root $sub) -File -ErrorAction SilentlyContinue }) |
    Unblock-File -ErrorAction SilentlyContinue
# The DLL writes its log, and this window its settings and commands, next to the DLL. A read-only folder (or one
# guarded by Windows' ransomware protection, "Controlled folder access", e.g. under Documents) cannot work.
$script:readOnly = $false
if (Test-Path -LiteralPath $dir) {
    $probe = Join-Path $dir "write_test_$PID.tmp"
    try { [System.IO.File]::WriteAllText($probe, 'x'); [System.IO.File]::Delete($probe) } catch { $script:readOnly = $true }
}

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
# Commands to the DLL (launcher_lib Send-LauncherCommand: a file still waiting is merged, not overwritten).
function Cmd([string[]]$lines) {
    if (-not (Send-LauncherCommand $dir $lines)) { Say 'Could not pass the command to the game (its folder is busy). Press the button again.' }
}

# ---- window ----
$form = New-Object System.Windows.Forms.Form
$form.SuspendLayout()
$form.Text = "MK9 Rollback - Play Online (version $(if ($installed.version) { $installed.version } else { 'unknown' }))"
$form.ClientSize = New-Object System.Drawing.Size(460, 650)
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

# Controls of the Local Versus mode only (hidden in the MK9 Online mode, see UpdateMode).
$codeControls = New-Object System.Collections.ArrayList
[void]$codeControls.Add((Label '1. Send your code to your friend:' 20 15))
$myBox = New-Object System.Windows.Forms.TextBox
$myBox.Location = New-Object System.Drawing.Point(20, 40); $myBox.Size = New-Object System.Drawing.Size(200, 26)
$myBox.ReadOnly = $true; $myBox.Font = New-Object System.Drawing.Font('Consolas', 12)
$form.Controls.Add($myBox)
$copy = Button '&Copy' 230 38 80
[void]$codeControls.Add($myBox); [void]$codeControls.Add($copy)

[void]$codeControls.Add((Label "2. Paste your friend's code:" 20 80))
$peerBox = New-Object System.Windows.Forms.TextBox
$peerBox.Location = New-Object System.Drawing.Point(20, 105); $peerBox.Size = New-Object System.Drawing.Size(200, 26)
$peerBox.Font = New-Object System.Drawing.Font('Consolas', 12)
$form.Controls.Add($peerBox)
$side = Label '' 230 108 220
[void]$codeControls.Add($peerBox); [void]$codeControls.Add($side)
# MK9 Online needs no codes (the mod finds the opponent in the game's own connection, 'online auto'); only the Local
# Versus mode has to be told who the other player is (UpdateMode shows one or the other).
$onlineNote = Label "No codes needed: press Play, then play anyone in Online > Player Match who also started the game with Play Online.bat." 20 20 420 100

Label 'Where you fight:' 20 143 120 | Out-Null
$where = New-Object System.Windows.Forms.ComboBox
$where.DropDownStyle = 'DropDownList'
$where.Location = New-Object System.Drawing.Point(140, 140); $where.Size = New-Object System.Drawing.Size(300, 26)
[void]$where.Items.Add('MK9 Online: Player Match (recommended)')
[void]$where.Items.Add('Local Versus over Steam (older test mode)')
$where.SelectedIndex = 0
$form.Controls.Add($where)
function IsOnline { $where.SelectedIndex -eq 0 }

$safe = New-Object System.Windows.Forms.CheckBox
$safe.Text = 'Safe mode (no rollback: try it if fights go out of sync)'
$safe.Location = New-Object System.Drawing.Point(20, 172); $safe.Size = New-Object System.Drawing.Size(420, 24)
$form.Controls.Add($safe)
Label 'Input delay:' 20 200 90 | Out-Null
# 2026-11-09 (the player: "like the latest games"): a fixed choice next to Auto, as Skullgirls / Fightcade offer and as
# Infil asked of SF6. Each game proposes its own and the session takes the larger, so a lower choice only applies when
# the other player's choice (or Auto) is not higher.
$delayBox = New-Object System.Windows.Forms.ComboBox
$delayBox.DropDownStyle = 'DropDownList'
$delayBox.Location = New-Object System.Drawing.Point(110, 197); $delayBox.Size = New-Object System.Drawing.Size(330, 26)
[void]$delayBox.Items.Add('Auto from the ping (recommended)')
[void]$delayBox.Items.Add('0 frames (like offline; more rollback, fast PCs only)')
[void]$delayBox.Items.Add('1 frame')
[void]$delayBox.Items.Add('2 frames')
[void]$delayBox.Items.Add('3 frames')
# The frames of each entry ($null: Auto). 0 (b37, rc21: as smooth as 1 on this PC at a 50 ms ping) applies only when
# both players pick it; Safe mode (lockstep) always takes its delay from the ping: at 0 it would wait every frame.
$delayFrames = @($null, 0, 1, 2, 3)
function FixedDelay { if ($safe.Checked) { $null } else { $delayFrames[$delayBox.SelectedIndex] } }
$delayBox.SelectedIndex = 0
$form.Controls.Add($delayBox)
$delayNote = Label '' 20 224 420 18
$delayNote.ForeColor = [System.Drawing.Color]::Gray
# Safe mode (its delay follows the ping, 2-3 frames) and Local Versus (a fixed 2) ignore the box: it is disabled there.
function UpdateDelayUi {
    $delayBox.Enabled = (IsOnline) -and -not $safe.Checked
    $delayNote.Text = if ($safe.Checked) { 'Safe mode picks its own delay from the ping (2-3 frames).' }
        elseif (-not (IsOnline)) { 'Local Versus uses a fixed delay of 2 frames.' }
        else { 'Auto: 1 frame up to a 50 ms ping, then 2+. The larger choice of the two players is used.' }
}
# The in-game network display (src\debug\Overlay.cpp): a compact line with a quality marker, all the numbers (F9 switches
# between the two in the game), or nothing. Sent to the game as 'overlay 1|2|0'.
Label 'Network display:' 20 250 120 | Out-Null
$overlayBox = New-Object System.Windows.Forms.ComboBox
$overlayBox.DropDownStyle = 'DropDownList'
$overlayBox.Location = New-Object System.Drawing.Point(140, 247); $overlayBox.Size = New-Object System.Drawing.Size(300, 26)
[void]$overlayBox.Items.Add('Compact (ping and connection quality)')
[void]$overlayBox.Items.Add('Detailed (all numbers)')
[void]$overlayBox.Items.Add('Off')
$overlayLevels = @(1, 2, 0)
$overlayBox.SelectedIndex = 0
$form.Controls.Add($overlayBox)
function ModeName { if ($safe.Checked) { 'Safe mode' } else { 'Rollback' } }
$script:modeName = 'Rollback'

$play = Button '3. &Play' 20 282 420 40
$play.Font = New-Object System.Drawing.Font('Segoe UI', 12, [System.Drawing.FontStyle]::Bold)
$form.AcceptButton = $play

$status = New-Object System.Windows.Forms.TextBox
$status.Location = New-Object System.Drawing.Point(20, 334); $status.Size = New-Object System.Drawing.Size(420, 150)
$status.Multiline = $true; $status.ReadOnly = $true; $status.ScrollBars = 'Vertical'; $status.TabStop = $false
$form.Controls.Add($status)
$trouble = Button '&Having trouble?' 20 492 130
$saveLogs = Button '&Save logs to the Desktop' 160 492 280
# Facts of the last match from the log (launcher_lib Update-MatchFacts) and the Wi-Fi hint (Get-ConnectionHint).
$matchLabel = Label 'Last match: none yet.' 20 530 420 20
$matchLabel.ForeColor = [System.Drawing.Color]::DimGray
$hintLabel = Label '' 20 552 420 20
$hintLabel.ForeColor = [System.Drawing.Color]::DarkOrange
$verLabel = Label "Version $(if ($installed.version) { $installed.version } else { '(unknown)' })" 20 584 215 22
$verLabel.ForeColor = [System.Drawing.Color]::Gray
$updBtn = Button 'Check for &updates' 240 578 200 30
$updBtn.FlatStyle = 'Flat'
$backBtn = Button 'Go &back to the previous version' 180 616 260 26
$backBtn.FlatStyle = 'Flat'
$backBtn.Visible = Test-Path -LiteralPath (Join-Path $root 'previous_version')
# Everything above is laid out at 96 DPI (100 %); ResumeLayout scales every position and size to the display's DPI.
# Set after the controls are added: set first, the labels were scaled twice (tested at 125 %).
$form.AutoScaleDimensions = New-Object System.Drawing.SizeF(96, 96)
$form.AutoScaleMode = [System.Windows.Forms.AutoScaleMode]::Dpi
$form.ResumeLayout($true)
# A small screen at a high scaling: scroll instead of cutting the bottom buttons off.
$work = [System.Windows.Forms.Screen]::FromControl($form).WorkingArea
if ($form.Height -gt $work.Height) { $form.AutoScroll = $true; $form.Height = $work.Height }

function Say([string]$s) {
    $status.AppendText("$(Get-Date -Format 'HH:mm:ss')  $s`r`n")
}

# Same text in README.md and the README of tools\package.ps1.
$troubleLines = @(
    'Allow MKKE.exe through the Windows firewall (Windows Security > Firewall > Allow an app).',
    'Add the MK9 Rollback folder to your antivirus exclusions.',
    'Run the game from an SSD, and turn off disk sleep.',
    'On a laptop: plug it in and set the Windows power mode to "Best performance".',
    'Close browsers and other heavy apps (downloads, streaming, launchers).',
    'Use a wired (Ethernet) connection instead of Wi-Fi.',
    'Both players need the same version ("Check for updates" at the bottom).',
    'In a fight, F9 shows or hides the network details.'
)
function ShowTrouble {
    [void][System.Windows.Forms.MessageBox]::Show("If the game stutters, freezes or will not connect:`n`n- $($troubleLines -join "`n- ")", 'Having trouble?', 'OK', 'Information')
}
$trouble.Add_Click({ ShowTrouble })

# ---- background work (update check, download and install, saving the logs, the Wi-Fi check) ----
# Each job is a runspace of its own; $bgTimer polls them and passes status lines from $bgQueue to the window.
$script:jobs = New-Object System.Collections.ArrayList
$script:bgQueue = [System.Collections.ArrayList]::Synchronized((New-Object System.Collections.ArrayList))
function Start-BgJob([scriptblock]$code, [hashtable]$params, [scriptblock]$done) {
    $ps = [powershell]::Create()
    [void]$ps.AddScript($code.ToString())
    foreach ($k in $params.Keys) { [void]$ps.AddParameter($k, $params[$k]) }
    [void]$script:jobs.Add(@{ PS = $ps; Handle = $ps.BeginInvoke(); Done = $done })
}
function DrainQueue {
    while ($script:bgQueue.Count -gt 0) { $s = $script:bgQueue[0]; $script:bgQueue.RemoveAt(0); Say "$s" }
}
$bgTimer = New-Object System.Windows.Forms.Timer
$bgTimer.Interval = 200
$bgTimer.Add_Tick({
    DrainQueue
    foreach ($j in @($script:jobs)) {
        if (-not $j.Handle.IsCompleted) { continue }
        [void]$script:jobs.Remove($j)
        $out = $null; $err = ''
        try { $out = @($j.PS.EndInvoke($j.Handle)) | Select-Object -Last 1 } catch { $err = $_.Exception.Message }
        if (-not $err -and $j.PS.Streams.Error.Count -gt 0 -and $null -eq $out) { $err = "$($j.PS.Streams.Error[0])" }
        $j.PS.Dispose()
        DrainQueue
        & $j.Done $out $err
    }
})
$kCheckJob = { param($updater, $timeout) . $updater; $r = Get-LatestRelease -TimeoutSec $timeout; [pscustomobject]@{ Release = $r; Info = (Get-UpdateError) } }
$kInstallJob = { param($updater, $release, $root, $queue) . $updater; Install-Update $release $root { param($s) [void]$queue.Add($s) } }
$kLogsJob = { param($lib, $dir, $dest, $self) . $lib; Save-LogsZip $dir $dest $self }
$kHintJob = { param($lib) . $lib; Get-ConnectionHint }

# Updates: checked once when the window opens (quietly: only a newer version is mentioned) and on the button.
$script:release = $null
$script:updateBusy = $false
$script:checkQuiet = $false
function CheckUpdate([switch]$Quiet) {
    if ($script:updateBusy) { return }
    $script:updateBusy = $true
    $script:checkQuiet = [bool]$Quiet
    $updBtn.Enabled = $false
    if (-not $Quiet) { Say 'Checking for updates...' }
    Start-BgJob $kCheckJob @{ updater = $updaterPath; timeout = $(if ($Quiet) { 4 } else { 10 }) } $onChecked
}
$onChecked = {
    param($out, $err)
    $script:updateBusy = $false
    $updBtn.Enabled = $true
    $Quiet = $script:checkQuiet
    if ($out -and $out.Release) { $script:release = $out.Release }
    if (-not ($out -and $out.Release)) {
        if (-not $Quiet) {
            if ($out -and $out.Info.RateLimited) { Say $out.Info.Error }
            elseif ($out) { Say "Update check failed: $($out.Info.Error)" }
            else { Say "Update check failed: $err" }
        }
        return
    }
    if (Test-UpdateNewer $script:release $installed.version) {
        Say "UPDATE AVAILABLE: version $($script:release.Version). Close the game, then press 'Update now' at the bottom. Your opponent needs the same version."
        $updBtn.Text = "&Update now ($($script:release.Version))"
        $updBtn.BackColor = [System.Drawing.Color]::Orange
        $updBtn.Font = New-Object System.Drawing.Font('Segoe UI', 10, [System.Drawing.FontStyle]::Bold)
        $verLabel.Text = "Update available: $($script:release.Version)"
        $verLabel.ForeColor = [System.Drawing.Color]::DarkOrange
        $verLabel.Font = New-Object System.Drawing.Font('Segoe UI', 10, [System.Drawing.FontStyle]::Bold)
        if ($form.Text -notlike '*UPDATE AVAILABLE') { $form.Text = "$($form.Text) - UPDATE AVAILABLE" }
    } elseif (-not $Quiet) {
        Say "You have the latest version ($($installed.version))."
    }
}
# Restarts this window on the files now on disk (after an update or going back).
function RestartWindow {
    Say 'Restarting this window...'
    try { $script:mutex.ReleaseMutex(); $script:mutexOwned = $false } catch { }
    Start-Process powershell -WindowStyle Hidden -ArgumentList '-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', "`"$PSScriptRoot\netplay_gui.ps1`""
    $form.Close()
}
$updBtn.Add_Click({
    if ($script:updateBusy) { return }
    if (-not ($script:release -and (Test-UpdateNewer $script:release $installed.version))) { CheckUpdate; return }
    if ($script:state -ne 'idle' -or (Get-Process MKKE -ErrorAction SilentlyContinue)) { Say 'Close the game first, then press Update now.'; return }
    $answer = [System.Windows.Forms.MessageBox]::Show("Install version $($script:release.Version)?`n`nYour opponent needs the same version to play with you.", 'MK9 Rollback update', 'YesNo', 'Question')
    if ($answer -ne 'Yes') { return }
    $script:updateBusy = $true
    $updBtn.Enabled = $false; $play.Enabled = $false; $backBtn.Enabled = $false
    Start-BgJob $kInstallJob @{ updater = $updaterPath; release = $script:release; root = $root; queue = $script:bgQueue } $onInstalled
})
$onInstalled = {
    param($out, $err)
    $script:updateBusy = $false
    if ($out -eq $true) { RestartWindow; return }
    if ($err) { Say "The update failed: $err" }
    $updBtn.Enabled = $true; $play.Enabled = -not $script:readOnly; $backBtn.Enabled = $true
}
# Undoes the last update (updater.ps1 Restore-PreviousVersion); refused while the game runs, like updating.
$backBtn.Add_Click({
    if ($script:updateBusy) { return }
    if ($script:state -ne 'idle' -or (Get-Process MKKE -ErrorAction SilentlyContinue)) { Say 'Close the game first, then press the button again.'; return }
    $prevVer = (Get-InstalledVersion (Join-Path $root 'previous_version')).version
    $answer = [System.Windows.Forms.MessageBox]::Show("Go back to the previous version$(if ($prevVer) { " ($prevVer)" })?`n`nYour opponent needs the same version to play with you.", 'MK9 Rollback', 'YesNo', 'Question')
    if ($answer -ne 'Yes') { return }
    if (Restore-PreviousVersion $root { param($s) Say $s }) { RestartWindow }
})
$form.Add_Shown({
    if (-not $NoUpdateCheck) { CheckUpdate -Quiet }
    Start-BgJob $kHintJob @{ lib = $libPath } { param($out, $err) if ($out) { $hintLabel.Text = "$out" } }
})

$me = MyAccountId
if ($me) { $myBox.Text = "$me" } else { $myBox.Text = '(start Steam)' }
if (Test-Path -LiteralPath $settings) {
    $saved = @(Get-Content -LiteralPath $settings)
    $peerBox.Text = "$(@($saved)[0])"
    # Only an explicit 'safe' line opts in to Safe mode (saved since 2026-11-01); older files ('rollback' or nothing)
    # open in rollback, the default.
    $safe.Checked = [bool]($saved | Select-String '^safe$' -Quiet)
    if ($saved | Select-String '^local$' -Quiet) { $where.SelectedIndex = 1 }
    $d = $saved | Select-String '^delay=([0-3])$' | Select-Object -First 1
    if ($d) { $delayBox.SelectedIndex = [array]::IndexOf($delayFrames, [int]$d.Matches[0].Groups[1].Value) }
    $o = $saved | Select-String '^overlay=([0-2])$' | Select-Object -First 1
    if ($o) { $overlayBox.SelectedIndex = [array]::IndexOf($overlayLevels, [int]$o.Matches[0].Groups[1].Value) }
}

function UpdateSide {
    $p = ParseCode $peerBox.Text
    if (-not $me -or -not $p) { $side.Text = ''; return }
    if (IsOnline) { $side.Text = ''; return }  # MK9's online mode decides the sides
    if ($p -eq $me) { $side.Text = "That's your own code"; return }
    $side.Text = if ($me -lt $p) { 'You: Player 1 (left)' } else { 'You: Player 2 (right)' }
}
function UpdateMode {
    $codes = -not (IsOnline)
    foreach ($c in $codeControls) { $c.Visible = $codes }
    $onlineNote.Visible = -not $codes
    UpdateDelayUi
}
# While the game runs with the mod, a change of the box is sent at once (2026-10-31: players ticked it between two
# online matches and kept Safe mode, it was only read at Play). A fight already under way keeps its netcode.
function SaveSettings {
    Set-Content -Encoding ascii -LiteralPath $settings (@("$($script:peer)") + $(if ($safe.Checked) { 'safe' } else { @() }) + $(if (IsOnline) { @() } else { 'local' }) +
        $(if ($delayBox.SelectedIndex -gt 0) { "delay=$($delayFrames[$delayBox.SelectedIndex])" } else { @() }) + "overlay=$($overlayLevels[$overlayBox.SelectedIndex])")
}
# The input delay of a running game changes the same way as Safe mode: the arm command is sent again.
$delayBox.Add_SelectedIndexChanged({
    if ($script:state -ne 'watch') { return }
    Say 'Input delay changed. It applies from the next fight.'
    SaveSettings
    Arm
})
$overlayBox.Add_SelectedIndexChanged({
    SaveSettings
    if ($script:state -ne 'watch') { return }
    Cmd @("overlay $($overlayLevels[$overlayBox.SelectedIndex])")
})
$safe.Add_CheckedChanged({ UpdateDelayUi })
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

# For a public bug report: Steam IDs, IP addresses and the user and PC names are replaced in the zip (launcher_lib
# Save-LogsZip, PRIVACY.txt in the zip says what was replaced).
$saveLogs.Add_Click({
    $saveLogs.Enabled = $false
    Say 'Saving the logs...'
    $selfIds = @()
    $acct = MyAccountId
    if ($acct) { $selfIds += "$($kSteamBase + $acct)" }
    Start-BgJob $kLogsJob @{ lib = $libPath; dir = $dir; dest = [Environment]::GetFolderPath('Desktop'); self = [string[]]$selfIds } {
        param($out, $err)
        $saveLogs.Enabled = $true
        if (-not $out -or -not $out.Zip) { Say $(if ($out) { $out.Error } else { "The logs zip could not be made ($err)." }); return }
        Say "Logs saved: $($out.Zip). Steam IDs, IP addresses and your user and PC names in them were replaced (PRIVACY.txt in the zip lists what). Send it together with your opponent's zip."
        Start-Process explorer.exe -ArgumentList "/select,`"$($out.Zip)`""
    }
})

# ---- steps, driven by a 1 s timer ----
$script:state = 'idle'
$script:wait = 0
$script:tries = 0
$script:tail = New-LogTail $log
$script:facts = New-MatchFacts
$script:ackDue = $null
$timer = New-Object System.Windows.Forms.Timer
$timer.Interval = 1000

function Arm([switch]$Next) {
    # One command per mode: Safe mode installs only the lockstep patch set (docs/restructure_plan.md, phase 1). Sent
    # again on a re-arm or a checkbox change, it only switches the mode (a running fight keeps its netcode).
    $lines = @($(if ($safe.Checked) { 'netcode safe' } else { 'netcode rollback' }), "overlay $($overlayLevels[$overlayBox.SelectedIndex])")
    # Local Versus: rollback at 2 frames (the simulated-lag runs of 2026-10-28: delay 1 replayed 4-8 times as many frames
    # as delay 2 at 40 and 80 ms one way). Online, the delay follows the ping ('online auto ping', src\net\OnlineMode.cpp):
    # 1 frame up to a 50 ms round trip since rc20 (b34/b35), unless this PC's replays then make slow frames.
    $delay = if ($safe.Checked) { 3 } else { 2 }
    $script:modeName = ModeName
    # The DLL echoes each command it takes ("CMD netcode rollback"); no echo within 5 s: the watch step says so.
    $script:ackDue = (Get-Date).AddSeconds(5)
    if ($safe.Checked) { Say 'Netcode: Safe mode (no rollback). Your opponent must tick Safe mode too.' }
    else { Say 'Netcode: Rollback. Your opponent must leave Safe mode unticked too.' }
    if (IsOnline) {
        # Auto: from the ping (OnlineMode DelayFromPing); a fixed choice is this game's proposal (the larger one wins).
        $fixed = FixedDelay
        $onlineDelay = if ($null -ne $fixed) { "$fixed" } else { 'ping' }
        Cmd ($lines + "online auto $onlineDelay")
        if ($null -ne $fixed) { Say "Input delay: $fixed frame(s) from this side (the larger choice of the two players is used)." }
        elseif ($safe.Checked -and $delayBox.SelectedIndex -gt 0) { Say 'Safe mode picks its input delay from the ping (2-3 frames).' }
        Say 'Ready. In the game: Online > Player Match, a 1 vs 1 room (not King of the Hill). Play anyone there.'
        Say "If your opponent also started the game with Play Online.bat, $($script:modeName) takes over at the start of the fight. Against anyone else, MK9's own netcode is used. Ranked is never touched."
        $script:state = 'watch'
        $play.Text = 'Ready (&press to re-arm)'
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
    $play.Text = 'Waiting for the fight (&press to re-arm)'
    $play.Enabled = $true
}

function GoIdle { $script:state = 'idle'; $play.Enabled = -not $script:readOnly }
$script:shownDesync = 0

$timer.Add_Tick({
    switch ($script:state) {
        'launch' {
            if (Get-Process MKKE -ErrorAction SilentlyContinue) { Say 'The game is running. Waiting for its menu...'; $script:state = 'settle'; $script:wait = 0; return }
            if (++$script:wait -gt 150) { Say 'The game did not start. Start it from Steam, then press Play again.'; GoIdle }
        }
        'settle' {
            if (++$script:wait -ge 15) { $script:state = 'inject'; $script:wait = 0 }
        }
        'injectretry' {
            if (++$script:wait -ge 2) { $script:state = 'inject'; $script:wait = 0 }
        }
        'inject' {
            if (@(Get-Process MKKE -ErrorAction SilentlyContinue).Count -gt 1) { Say 'Two copies of the game are running. Close one.'; GoIdle; return }
            $files = Test-InstalledFiles $root
            if (-not $files.Ok) { Say $files.Message; GoIdle; return }
            # The last session's logs are kept for "Save logs": a player who restarts after a crash would lose them.
            $old = @(Get-ChildItem -LiteralPath $dir -Filter 'MK9Rollback*.log' -File -ErrorAction SilentlyContinue)
            if ($old) {
                $prevLogs = Join-Path $dir 'previous_logs'
                Remove-Item -LiteralPath $prevLogs -Recurse -Force -ErrorAction SilentlyContinue
                New-Item -ItemType Directory -Force -Path $prevLogs | Out-Null
                $old | Move-Item -Destination $prevLogs -Force -ErrorAction SilentlyContinue
            }
            Get-ChildItem -LiteralPath $dir -Filter 'MK9Rollback*.log' -File -ErrorAction SilentlyContinue | Remove-Item -Force -ErrorAction SilentlyContinue
            $out = (& (Join-Path $dir 'MK9RollbackInjector.exe') inject 2>&1 | Out-String)
            $r = Get-InjectorResult $LASTEXITCODE
            if ($r.AlreadyLoaded) {
                Say 'The mod was already loaded in this game.'
                $script:tail = New-LogTail $log
                [void](Read-LogTail $script:tail -Discard)
                Arm
                return
            }
            if (-not $r.Loaded) {
                if ($r.Retry -and ++$script:tries -lt 6) { $script:state = 'injectretry'; $script:wait = 0; return }
                if ($r.Text) { Say $r.Text }
                else { Say 'Could not load the mod into the game. Your antivirus may have blocked MK9RollbackInjector.exe. Press ''Having trouble?'' for what to try, then save the logs if it keeps failing.' }
                GoIdle
                return
            }
            $script:state = 'injectwait'; $script:wait = 0
        }
        'injectwait' {
            # A fresh log: read from its first line (a Steam or hook failure at start-up is shown too).
            if (Test-Path -LiteralPath $log) { Say 'Mod loaded.'; $script:tail = New-LogTail $log; Arm; return }
            if (++$script:wait -ge 10) {
                if (++$script:tries -lt 6) { $script:state = 'inject'; return }
                Say 'Could not load the mod into the game. Your antivirus may have blocked MK9RollbackInjector.exe. Press ''Having trouble?'' for what to try, then save the logs if it keeps failing.'
                GoIdle
            }
        }
        'watch' {
            if (-not (Get-Process MKKE -ErrorAction SilentlyContinue)) {
                Say 'The game has closed.'; $script:ackDue = $null; $play.Text = '3. &Play'; GoIdle; return
            }
            $rearm = $false
            foreach ($line in (Read-LogTail $script:tail)) {
                if ($script:ackDue -and (Test-CommandAck $line)) { $script:ackDue = $null }
                if (Update-MatchFacts $script:facts $line) { $matchLabel.Text = Format-MatchFacts $script:facts }
                $r = Get-LauncherMessage $line $script:modeName
                if (-not $r) { continue }
                if ($r.Kind -eq 'desync' -and $script:shownDesync++) { continue }
                # a new fight or session starts counting again, so a later desync is shown too
                if ($r.Kind -eq 'reset') { $script:shownDesync = 0 }
                if ($r.Text) { Say $r.Text }
                if ($r.CheckUpdate) { CheckUpdate }
                if ($r.Rearm) { $rearm = $true }
            }
            if ($script:ackDue -and (Get-Date) -gt $script:ackDue) {
                $script:ackDue = $null
                Say 'The mod did not answer. Press the button again; if it repeats, save the logs.'
            }
            if ($rearm -and -not (IsOnline)) { Arm -Next }  # the online mode re-engages by itself for every match
        }
    }
})

$play.Add_Click({
    if ($script:readOnly) { Say 'This folder is read-only (or protected by Windows ransomware protection). Move the MK9 Rollback folder to e.g. C:\Games.'; return }
    if (-not $me) { $script:me = MyAccountId; $me = $script:me; if ($me) { $myBox.Text = "$me" } }
    if (-not $me) { Say 'Start Steam and sign in first.'; return }
    $script:peer = ParseCode $peerBox.Text
    if (-not (IsOnline) -and (-not $script:peer -or $script:peer -eq $me)) { Say "Paste your friend's code first (box 2)."; return }
    SaveSettings
    if (-not (Test-Path -LiteralPath (Join-Path $dir 'MK9RollbackInjector.exe'))) { Say "The mod files are missing next to this launcher ($dir)."; return }
    if ($script:state -eq 'watch') { Arm; return }
    if ($script:state -ne 'idle') { return }
    $files = Test-InstalledFiles $root
    if (-not $files.Ok) { Say $files.Message; return }
    $play.Enabled = $false
    $script:tries = 0
    $game = Get-Process MKKE -ErrorAction SilentlyContinue | Select-Object -First 1
    if (-not $game) {
        Say 'Starting Mortal Kombat through Steam...'
        Start-Process powershell -WindowStyle Hidden -ArgumentList '-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', "`"$PSScriptRoot\launch_game.ps1`""
        $script:state = 'launch'; $script:wait = 0
    } elseif ((Test-Path -LiteralPath $log) -and ((Get-Item -LiteralPath $log).LastWriteTime -gt $game.StartTime)) {
        # This game already has the mod: only what it logs from now on is news.
        $script:tail = New-LogTail $log
        [void](Read-LogTail $script:tail -Discard)
        Arm
    } else {
        Say 'The game is already running. Loading the mod...'
        $script:state = 'inject'
    }
})

if ($SnapshotPng) {
    $snapTimer = New-Object System.Windows.Forms.Timer
    $snapTimer.Interval = 2500
    $snapTimer.Add_Tick({
        $snapTimer.Stop()
        $bmp = New-Object System.Drawing.Bitmap($form.Width, $form.Height)
        $form.DrawToBitmap($bmp, (New-Object System.Drawing.Rectangle(0, 0, $form.Width, $form.Height)))
        $bmp.Save($SnapshotPng, [System.Drawing.Imaging.ImageFormat]::Png)
        $bmp.Dispose()
        $form.Close()
    })
    $form.Add_Shown({ $snapTimer.Start() })
}

$timer.Start()
$bgTimer.Start()
if (IsOnline) { Say 'Press Play. The game starts with the mod.' } else { Say 'Both players: start this launcher, swap codes, then press Play.' }
if ($script:readOnly) {
    Say 'This folder is read-only (or protected by Windows ransomware protection). Move the MK9 Rollback folder to e.g. C:\Games.'
    $play.Enabled = $false
}
[void]$form.ShowDialog()
$timer.Stop()
$bgTimer.Stop()
foreach ($j in @($script:jobs)) { try { $j.PS.Stop(); $j.PS.Dispose() } catch { } }
if ($script:mutexOwned) { try { $script:mutex.ReleaseMutex() } catch { } }
exit 0
