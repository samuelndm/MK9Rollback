# The MK9 Rollback window ("MK9 Rollback.exe" starts it; before 2026-11 a "Play Online.bat" did). One button: Play.
# The window starts the game through Steam, injects MK9Rollback, tells it how to play and translates its log into
# plain status lines. The same steps as tools\netplay.ps1, run from a timer so the window stays responsive.
# Nothing to choose (2026-11, the player: "remove everything selectable, just make all default"): the game is always
# told rollback, the network display in the fight's HUD (F9 in a fight switches to the detailed line and back) and
# MK9's own Online mode with the input delay taken from the ping ('online auto ping': the two games measure the round
# trip before the fight, both take the larger proposal, and adjust it between rounds). The boxes this window had
# until then (Safe mode, a fixed input delay, the display, the older Local Versus mode with friend codes) are gone;
# the DLL still knows those commands (tools\netplay.ps1 and the test harness use them).
# The log-line table, the injector's exit codes, the log reader, the log privacy filter, the mark and the other pure
# helpers are in tools\launcher_lib.ps1 (checked by tools\launcher_selftest.ps1). The update check, the download and
# saving the logs run in background runspaces, polled by a timer, so the window never freezes on the network.
#   -NoUpdateCheck         no update check when the window opens
#   -SnapshotPng <file>    saves a picture of the window, then closes it (layout check). The window is then shown
#                          off the screen and without taking the keyboard, and does not count as the open window.
#   -SnapshotState <name>  with -SnapshotPng: what the picture shows (idle, starting, running, update, failed)
#   -StartCheck            exits at once with code 0 (the "Play Online.bat" of the versions before 2026-11 runs this
#                          first, in its console, to learn whether this PC runs the script at all)
param([switch]$NoUpdateCheck, [string]$SnapshotPng = '', [switch]$StartCheck, [string]$SnapshotState = 'idle')
if ($StartCheck) { exit 0 }

# Started without a console, so nothing may fail silently. A PC that forces PowerShell's Constrained Language Mode
# (company or school policy, some security software) cannot run this window at all: say so in Notepad.
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
    $msg = "The MK9 Rollback window stopped because of an error:`r`n`r`n$($_.Exception.Message)`r`n$($_.InvocationInfo.PositionMessage)"
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

Add-Type -AssemblyName System.Windows.Forms, System.Drawing
# Windows calls and a form that can be shown without taking the keyboard (the layout picture). Every use below is
# guarded: where this cannot be compiled (a blocked compiler), the window still opens, plainer.
try {
    if (-not ('MK9R.Native' -as [type])) {
        Add-Type -ReferencedAssemblies System.Windows.Forms, System.Drawing -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
namespace MK9R {
    public static class Native {
        [DllImport("user32.dll")] public static extern bool SetProcessDPIAware();
        [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr hWnd);
        [DllImport("user32.dll")] public static extern bool AllowSetForegroundWindow(int processId);
        [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr hWnd, int nCmdShow);
        [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr hWnd);
        [DllImport("user32.dll")] public static extern bool IsIconic(IntPtr hWnd);
        [DllImport("user32.dll")] public static extern bool IsHungAppWindow(IntPtr hWnd);
        [DllImport("user32.dll", CharSet = CharSet.Unicode)] public static extern IntPtr FindWindowEx(IntPtr parent, IntPtr after, string className, IntPtr title);
        [DllImport("user32.dll", EntryPoint = "FindWindowExW")] public static extern IntPtr NextWindow(IntPtr parent, IntPtr after, IntPtr className, IntPtr title);
        [DllImport("user32.dll", CharSet = CharSet.Unicode)] public static extern int GetWindowText(IntPtr hWnd, System.Text.StringBuilder text, int max);
        [DllImport("user32.dll", CharSet = CharSet.Unicode)] public static extern int GetClassName(IntPtr hWnd, System.Text.StringBuilder text, int max);
        [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr hWnd, out uint processId);
        [DllImport("user32.dll")] public static extern IntPtr SendMessage(IntPtr hWnd, int msg, IntPtr wParam, IntPtr lParam);
        [DllImport("dwmapi.dll")] public static extern int DwmSetWindowAttribute(IntPtr hWnd, int attribute, ref int value, int size);
        [DllImport("uxtheme.dll", CharSet = CharSet.Unicode)] public static extern int SetWindowTheme(IntPtr hWnd, string app, IntPtr idList);
        [DllImport("shell32.dll", CharSet = CharSet.Unicode)] public static extern int SetCurrentProcessExplicitAppUserModelID(string id);
    }
    public class QuietForm : System.Windows.Forms.Form {
        public bool NoActivate;
        protected override bool ShowWithoutActivation { get { return NoActivate; } }
    }
}
'@
    }
    # Declared before any window exists: Windows then draws the window sharp at 125-200 % display scaling instead of
    # stretching a blurry bitmap (the controls are scaled by AutoScaleMode Dpi below).
    [void][MK9R.Native]::SetProcessDPIAware()
    # Its own button on the taskbar, with its own icon (without this it is grouped under Windows PowerShell's).
    [void][MK9R.Native]::SetCurrentProcessExplicitAppUserModelID('MK9Rollback.Window')
} catch { }
[System.Windows.Forms.Application]::EnableVisualStyles()
# An error inside a button or timer handler does not reach the trap above: Windows Forms catches it and shows its own
# ".NET Framework" error box, again at every tick when a timer step fails. It goes to the status box instead
# (HandlerError). The mode must be set before the first control exists.
try { [System.Windows.Forms.Application]::SetUnhandledExceptionMode([System.Windows.Forms.UnhandledExceptionMode]::CatchException) } catch { }
[System.Windows.Forms.Application]::add_ThreadException({ param($source, $e) try { HandlerError $e.Exception } catch { } })

# One window at a time: a second one would fight the first over the game and the command file. The window that is
# already open is asked to come to the front (it watches $kShowEvent; "MK9 Rollback.exe" asks the same way).
# (A self-test sets MK9R_LAUNCHER_TEST and gets names of its own, as in launcher\Launcher.cpp: it neither meets nor
# disturbs the player's open window.)
$kTestNames = if ($env:MK9R_LAUNCHER_TEST) { 'Test' } else { '' }
$kWindowMutex = "Local\MK9RollbackLauncher$kTestNames"
$kShowEvent = "Local\MK9RollbackLauncherShow$kTestNames"
$script:mutex = $null
$script:mutexOwned = $false
$script:showEvent = $null
if (-not $SnapshotPng) {
    $script:mutex = New-Object System.Threading.Mutex($false, $kWindowMutex)
    try { $script:mutexOwned = $script:mutex.WaitOne(500) } catch [System.Threading.AbandonedMutexException] { $script:mutexOwned = $true }
    if (-not $script:mutexOwned) {
        $asked = $false
        try {
            $ev = [System.Threading.EventWaitHandle]::OpenExisting($kShowEvent)
            try { [void][MK9R.Native]::AllowSetForegroundWindow(-1) } catch { }
            [void]$ev.Set()
            $ev.Dispose()
            $asked = $true
        } catch { }
        if (-not $asked) {
            # An older version's window (it has no such event): found by its title among PowerShell's windows and
            # brought to the front, also when it is invisible. (The versions before 2026-11 restarted hidden after
            # "Update now" and then only answered "already open" to every new start.)
            $shown = $false
            try {
                $others = @{}
                foreach ($p in Get-Process powershell, pwsh -ErrorAction SilentlyContinue) { if ($p.Id -ne $PID) { $others[[uint32]$p.Id] = $true } }
                $text = New-Object System.Text.StringBuilder 260
                $h = [IntPtr]::Zero
                for ($i = 0; $i -lt 20000 -and -not $shown; $i++) {
                    $h = [MK9R.Native]::NextWindow([IntPtr]::Zero, $h, [IntPtr]::Zero, [IntPtr]::Zero)
                    if ($h -eq [IntPtr]::Zero) { break }
                    $owner = [uint32]0
                    [void][MK9R.Native]::GetWindowThreadProcessId($h, [ref]$owner)
                    if (-not $others.ContainsKey($owner)) { continue }
                    [void]$text.Clear(); [void][MK9R.Native]::GetWindowText($h, $text, 260)
                    if ("$text" -notlike 'MK9 Rollback*') { continue }
                    [void]$text.Clear(); [void][MK9R.Native]::GetClassName($h, $text, 260)
                    if ("$text" -notlike 'WindowsForms*') { continue }   # the window itself, not a message box of it
                    if ([MK9R.Native]::IsIconic($h)) { [void][MK9R.Native]::ShowWindow($h, 9) }                      # SW_RESTORE
                    elseif (-not [MK9R.Native]::IsWindowVisible($h)) { [void][MK9R.Native]::ShowWindow($h, 5) }     # SW_SHOW
                    [void][MK9R.Native]::SetForegroundWindow($h)
                    $shown = [MK9R.Native]::IsWindowVisible($h)
                }
            } catch { }
            if (-not $shown) {
                [void][System.Windows.Forms.MessageBox]::Show("MK9 Rollback is already open.`r`n`r`nIf you cannot see its window: open Task Manager (Ctrl+Shift+Esc), end `"Windows PowerShell`" there, then open MK9 Rollback again.", 'MK9 Rollback', 'OK', 'Information')
            }
        }
        exit 0
    }
    try { $script:showEvent = New-Object System.Threading.EventWaitHandle($false, [System.Threading.EventResetMode]::AutoReset, $kShowEvent) } catch { }
}

$root = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot '..')).Path
$dir = Join-Path $root 'build\clang'
$log = Join-Path $dir 'MK9Rollback.log'
$kSteamBase = [uint64]76561197960265728
$libPath = Join-Path $PSScriptRoot 'launcher_lib.ps1'
$updaterPath = Join-Path $PSScriptRoot 'updater.ps1'
foreach ($p in @($libPath, $updaterPath)) { if (-not (Test-Path -LiteralPath $p)) { throw "$p is missing. Extract the zip again into an empty folder." } }
# Update check and install from the GitHub releases (Get-InstalledVersion, Get-LatestRelease, Install-Update,
# Restore-PreviousVersion), and the window's pure helpers.
. $updaterPath
. $libPath
$installed = Get-InstalledVersion $root
$kPowerShell = Join-Path $PSHOME 'powershell.exe'
# Files extracted from a downloaded zip carry the internet zone mark; the injector and the DLL would then be blocked.
# Only the release's own files, by name (updater.ps1 $kPackageFiles): the zip has no top folder, so this folder may be
# the player's Downloads or Desktop, and the other files there keep their mark.
Unblock-PackageFiles $root
# The DLL writes its log, and this window its commands, next to the DLL. A read-only folder (or one guarded by
# Windows' ransomware protection, "Controlled folder access", e.g. under Documents) cannot work.
$script:readOnly = $false
if (Test-Path -LiteralPath $dir) {
    $probe = Join-Path $dir "write_test_$PID.tmp"
    try { [System.IO.File]::WriteAllText($probe, 'x'); [System.IO.File]::Delete($probe) } catch { $script:readOnly = $true }
}

function MyAccountId {
    $u = (Get-ItemProperty 'HKCU:\Software\Valve\Steam\ActiveProcess' -ErrorAction SilentlyContinue).ActiveUser
    if ($u) { [uint64]$u } else { [uint64]0 }
}
# Commands to the DLL (launcher_lib Send-LauncherCommand: a file still waiting is merged, not overwritten). $false
# when the file could not be placed; the caller sends again (the watch step, when the DLL's echo does not come).
function Cmd([string[]]$lines) { [bool](Send-LauncherCommand $dir $lines) }

# ---- colours ----
# The dark look (launcher_lib $script:UiColors). With Windows' high-contrast setting on, the system's own colours.
$kHighContrast = [System.Windows.Forms.SystemInformation]::HighContrast
function Hex([string]$name) { [System.Drawing.ColorTranslator]::FromHtml($script:UiColors[$name]) }
if ($kHighContrast) {
    $sc = [System.Drawing.SystemColors]
    $c = @{ Bg = $sc::Control; Panel = $sc::Control; Field = $sc::Window; Border = $sc::WindowFrame; Text = $sc::ControlText; Muted = $sc::ControlText
        FieldText = $sc::WindowText; Accent = $sc::Highlight; AccentHot = $sc::Highlight; AccentDown = $sc::Highlight; OnAccent = $sc::HighlightText
        Good = $sc::ControlText; Warn = $sc::ControlText; Bad = $sc::ControlText; Idle = $sc::ControlText; Link = $sc::HotTrack; LinkOff = $sc::GrayText
        Quiet = $sc::Control; QuietText = $sc::GrayText; Running = $sc::Control; Banner = $sc::Info; BannerText = $sc::InfoText }
} else {
    $c = @{ Bg = Hex 'Bg'; Panel = Hex 'Panel'; Field = Hex 'Field'; Border = Hex 'Border'; Text = Hex 'Text'; Muted = Hex 'Muted'
        FieldText = [System.Drawing.ColorTranslator]::FromHtml('#D5D9DF'); Accent = Hex 'Accent'; AccentHot = Hex 'AccentHot'; AccentDown = Hex 'AccentDown'
        OnAccent = [System.Drawing.Color]::White; Good = Hex 'Good'; Warn = Hex 'Warn'; Bad = Hex 'Bad'; Idle = Hex 'Idle'
        Link = [System.Drawing.ColorTranslator]::FromHtml('#B9C2CE'); LinkOff = [System.Drawing.ColorTranslator]::FromHtml('#5A626D')
        Quiet = [System.Drawing.ColorTranslator]::FromHtml('#252A32'); QuietText = Hex 'Muted'
        Running = [System.Drawing.ColorTranslator]::FromHtml('#17301F'); Banner = [System.Drawing.ColorTranslator]::FromHtml('#2B2113'); BannerText = Hex 'Text' }
}
function UiFont([single]$size, [switch]$Bold, [switch]$Semi) {
    if ($Semi) { return New-Object System.Drawing.Font('Segoe UI Semibold', $size) }
    if ($Bold) { return New-Object System.Drawing.Font('Segoe UI', $size, [System.Drawing.FontStyle]::Bold) }
    New-Object System.Drawing.Font('Segoe UI', $size)
}

# ---- window ----
# Laid out at 96 DPI (100 %); ResumeLayout below scales every position and size to the display's DPI.
$kVersionText = if ($installed.version) { "Version $($installed.version)" } else { 'Development copy' }
$form = if ('MK9R.QuietForm' -as [type]) { New-Object MK9R.QuietForm } else { New-Object System.Windows.Forms.Form }
$form.SuspendLayout()
$form.Text = 'MK9 Rollback'
$form.ClientSize = New-Object System.Drawing.Size(460, 598)
$form.FormBorderStyle = 'FixedSingle'
$form.MaximizeBox = $false
$form.StartPosition = 'CenterScreen'
$form.Font = UiFont 9.75
$form.BackColor = $c.Bg
$form.ForeColor = $c.Text
function Place($control, $parent, $x, $y, $w, $h) {
    $control.Location = New-Object System.Drawing.Point($x, $y)
    $control.Size = New-Object System.Drawing.Size($w, $h)
    $parent.Controls.Add($control)
    $control
}
function Label($parent, [string]$text, $x, $y, $w, $h, $font = $null, $color = $null) {
    $l = New-Object System.Windows.Forms.Label
    $l.Text = $text
    $l.UseMnemonic = $false
    if ($font) { $l.Font = $font }
    if ($color) { $l.ForeColor = $color }
    Place $l $parent $x $y $w $h
}
function Link($parent, [string]$text, $x, $y, $w, [string]$align = 'MiddleLeft') {
    $l = New-Object System.Windows.Forms.LinkLabel
    $l.Text = $text
    $l.TextAlign = $align
    $l.LinkBehavior = 'HoverUnderline'
    $l.LinkColor = $c.Link; $l.VisitedLinkColor = $c.Link; $l.ActiveLinkColor = $c.AccentHot; $l.DisabledLinkColor = $c.LinkOff
    $l.TabStop = $true
    Place $l $parent $x $y $w 22
}
function Rule($y) {
    $r = New-Object System.Windows.Forms.Panel
    $r.BackColor = $c.Border
    Place $r $form 0 $y 460 1
}
$script:dpi = 96.0
try { $g0 = [System.Drawing.Graphics]::FromHwnd([IntPtr]::Zero); $script:dpi = [double]$g0.DpiX; $g0.Dispose() } catch { }
function Scaled([int]$at96) { [int][Math]::Round($at96 * $script:dpi / 96.0) }
# The two buttons are painted here, as a rounded block in the colours of their Tag (SetPlay changes them): Windows
# Forms' own flat button frames the default button in its text colour and greys a disabled one's text, unreadable on
# the dark look. With high contrast on, Windows draws them as usual.
function StyleButton($b, [int]$radius = 6) {
    $b.UseVisualStyleBackColor = $kHighContrast
    $b.Tag = @{ Back = $c.Accent; Hot = $c.AccentHot; Down = $c.AccentDown; Fore = $c.OnAccent; Over = $false; Pressed = $false; Radius = $radius }
    if ($kHighContrast) { return }
    $b.FlatStyle = 'Flat'
    $b.FlatAppearance.BorderSize = 0
    $b.Add_MouseEnter({ param($s, $e) $s.Tag.Over = $true; $s.Invalidate() })
    $b.Add_MouseLeave({ param($s, $e) $s.Tag.Over = $false; $s.Tag.Pressed = $false; $s.Invalidate() })
    $b.Add_MouseDown({ param($s, $e) $s.Tag.Pressed = $true; $s.Invalidate() })
    $b.Add_MouseUp({ param($s, $e) $s.Tag.Pressed = $false; $s.Invalidate() })
    $b.Add_Paint({ param($s, $e) PaintButton $s $e.Graphics })
}
function RoundedPath([single]$x, [single]$y, [single]$w, [single]$h, [single]$r) {
    $d = [single](2 * $r)
    $p = New-Object System.Drawing.Drawing2D.GraphicsPath
    $p.AddArc($x, $y, $d, $d, 180, 90); $p.AddArc($x + $w - $d, $y, $d, $d, 270, 90)
    $p.AddArc($x + $w - $d, $y + $h - $d, $d, $d, 0, 90); $p.AddArc($x, $y + $h - $d, $d, $d, 90, 90)
    $p.CloseFigure()
    $p
}
function PaintButton($b, $g) {
    $t = $b.Tag
    $g.SmoothingMode = [System.Drawing.Drawing2D.SmoothingMode]::AntiAlias
    $g.PixelOffsetMode = [System.Drawing.Drawing2D.PixelOffsetMode]::HighQuality
    $g.Clear($b.Parent.BackColor)
    $fill = if ($t.Pressed) { $t.Down } elseif ($t.Over) { $t.Hot } else { $t.Back }
    $r = [single](Scaled $t.Radius)
    $path = RoundedPath 0 0 $b.Width $b.Height $r
    $brush = New-Object System.Drawing.SolidBrush($fill)
    $g.FillPath($brush, $path)
    $brush.Dispose(); $path.Dispose()
    # The keyboard's focus mark, once Windows shows such marks (after Tab or Alt): WM_QUERYUISTATE, UISF_HIDEFOCUS.
    $marks = $false
    try { $marks = $b.Focused -and (([int64][MK9R.Native]::SendMessage($b.Handle, 0x0129, [IntPtr]::Zero, [IntPtr]::Zero) -band 1) -eq 0) } catch { }
    if ($marks) {
        $in = [single](Scaled 3)
        $ring = RoundedPath $in $in ($b.Width - 2 * $in) ($b.Height - 2 * $in) ([single][Math]::Max(1, $r - $in))
        $pen = New-Object System.Drawing.Pen([System.Drawing.Color]::FromArgb(190, $t.Fore), [single]1)
        $g.DrawPath($pen, $ring)
        $pen.Dispose(); $ring.Dispose()
    }
    $flags = [System.Windows.Forms.TextFormatFlags]'HorizontalCenter, VerticalCenter, SingleLine, HidePrefix'
    [System.Windows.Forms.TextRenderer]::DrawText($g, $b.Text, $b.Font, $b.ClientRectangle, $t.Fore, $flags)
}

# The header: the mark, the name, what it is, and the installed version.
$header = New-Object System.Windows.Forms.Panel
$header.BackColor = $c.Panel
[void](Place $header $form 0 0 460 84)
$logo = New-Object System.Windows.Forms.PictureBox
$logo.SizeMode = 'Zoom'
$logo.BackColor = $c.Panel
try { $logo.Image = New-LogoBitmap (Scaled 48) } catch { }
[void](Place $logo $header 20 18 48 48)
[void](Label $header 'MK9 Rollback' 80 13 240 32 (UiFont 16 -Semi) $c.Text)
[void](Label $header 'Rollback netcode for Mortal Kombat Komplete Edition' 82 47 366 20 (UiFont 9) $c.Muted)
$verLabel = Label $header $kVersionText 300 20 148 18 (UiFont 8.25) $c.Muted
$verLabel.TextAlign = 'TopRight'
[void](Rule 84)

# What is going on now: a coloured dot, one line, and one sentence of what to do next.
$dot = Label $form ([string][char]0x25CF) 18 97 22 28 (UiFont 13) $c.Idle
$headline = Label $form '' 40 98 404 28 (UiFont 12.5 -Semi) $c.Text
$subline = Label $form '' 21 128 420 38 (UiFont 9) $c.Muted

$play = New-Object System.Windows.Forms.Button
$play.Font = UiFont 14 -Bold
StyleButton $play 7
[void](Place $play $form 20 172 420 54)
$form.AcceptButton = $play

# Shown only when a newer version is out (ShowUpdateBanner); the status box below then starts lower.
$banner = New-Object System.Windows.Forms.Panel
$banner.BackColor = $c.Banner
$banner.Visible = $false
[void](Place $banner $form 20 238 420 44)
$bannerEdge = New-Object System.Windows.Forms.Panel
$bannerEdge.BackColor = $c.Warn
[void](Place $bannerEdge $banner 0 0 4 44)
$bannerLabel = Label $banner '' 16 0 270 44 (UiFont 9.75 -Semi) $c.BannerText
$bannerLabel.TextAlign = 'MiddleLeft'
$updBtn = New-Object System.Windows.Forms.Button
$updBtn.Text = '&Update now'
$updBtn.Font = UiFont 9.75 -Semi
$updBtn.Cursor = [System.Windows.Forms.Cursors]::Hand
StyleButton $updBtn 5
[void](Place $updBtn $banner 296 7 116 30)

# The status lines, newest at the bottom: a border, then the box with a little air on the left.
$logEdge = New-Object System.Windows.Forms.Panel
$logEdge.BackColor = $c.Border
[void](Place $logEdge $form 20 238 420 240)
$logBack = New-Object System.Windows.Forms.Panel
$logBack.BackColor = $c.Field
[void](Place $logBack $logEdge 1 1 418 238)
$status = New-Object System.Windows.Forms.TextBox
$status.Multiline = $true; $status.ReadOnly = $true; $status.ScrollBars = 'Vertical'; $status.TabStop = $false
$status.BorderStyle = 'None'
$status.BackColor = $c.Field
$status.ForeColor = $c.FieldText
$status.Font = UiFont 9
[void](Place $status $logBack 10 8 408 224)
if (-not $kHighContrast) {
    # The scroll bar in the dark look too (Windows 10 1809 and later; ignored before).
    $status.Add_HandleCreated({ try { [void][MK9R.Native]::SetWindowTheme($status.Handle, 'DarkMode_Explorer', [IntPtr]::Zero) } catch { } })
}

# Facts of the last match from the log (launcher_lib Update-MatchFacts) and the Wi-Fi hint (Get-ConnectionHint).
$matchLabel = Label $form 'Last match: none yet.' 21 486 420 20 (UiFont 9) $c.Muted
$hintLabel = Label $form '' 21 506 420 20 (UiFont 9) $c.Warn
[void](Rule 534)
$trouble = Link $form '&Having trouble?' 19 546 110
$saveLogs = Link $form '&Save logs to the Desktop' 136 546 166
$updLink = Link $form '&Check for updates' 300 546 142 'MiddleRight'
$backLink = Link $form 'Go &back to the previous version' 19 570 240
$backLink.Visible = Test-Path -LiteralPath (Join-Path $root 'previous_version')

# The window's icon: the mark at the title bar's and the taskbar's sizes. The title bar and the window's edge take the
# header's colours where Windows can (dark title bar: Windows 10 1809 and later; the colours: Windows 11). Registered
# before the first thing that makes Windows create the window (the screen question below).
$script:icons = @()
try {
    foreach ($px in @((Scaled 32), (Scaled 16))) {
        $bmp = New-LogoBitmap $px
        $script:icons += [System.Drawing.Icon]::FromHandle($bmp.GetHicon())
        $bmp.Dispose()
    }
    $form.Icon = $script:icons[0]
} catch { }
$form.Add_HandleCreated({
    try {
        $h = $form.Handle
        if ($script:icons.Count -eq 2) { [void][MK9R.Native]::SendMessage($h, 0x80, [IntPtr]::Zero, $script:icons[1].Handle) }  # WM_SETICON, the small one
        if ($kHighContrast) { return }
        $on = 1
        if ([MK9R.Native]::DwmSetWindowAttribute($h, 20, [ref]$on, 4) -ne 0) { [void][MK9R.Native]::DwmSetWindowAttribute($h, 19, [ref]$on, 4) }
        $caption = [int]($c.Panel.R -bor ($c.Panel.G -shl 8) -bor ($c.Panel.B -shl 16))
        [void][MK9R.Native]::DwmSetWindowAttribute($h, 35, [ref]$caption, 4)
        $edge = [int]($c.Border.R -bor ($c.Border.G -shl 8) -bor ($c.Border.B -shl 16))
        [void][MK9R.Native]::DwmSetWindowAttribute($h, 34, [ref]$edge, 4)
    } catch { }
})
# Set after the controls are added: set first, the labels were scaled twice (tested at 125 %).
$form.AutoScaleDimensions = New-Object System.Drawing.SizeF(96, 96)
$form.AutoScaleMode = [System.Windows.Forms.AutoScaleMode]::Dpi
$form.ResumeLayout($true)
# A small screen at a high scaling: scroll instead of cutting the bottom links off.
$work = [System.Windows.Forms.Screen]::FromControl($form).WorkingArea
if ($form.Height -gt $work.Height) { $form.AutoScroll = $true; $form.Height = $work.Height }

function Say([string]$s) {
    $status.AppendText("$(Get-Date -Format 'HH:mm:ss')  $s`r`n")
}
# The line under the header. $tone: idle, busy, good, warn or bad (the dot's colour). $sub: the sentence below it
# ($null keeps the one that is there).
function SetStatus([string]$text, [string]$tone = 'idle', $sub = $null) {
    $headline.Text = $text
    $dot.ForeColor = switch ($tone) { 'busy' { $c.Warn } 'good' { $c.Good } 'warn' { $c.Warn } 'bad' { $c.Bad } default { $c.Idle } }
    if ($null -ne $sub) { $subline.Text = "$sub" }
}
$kSubIdle = 'Press Play: the game starts through Steam with the mod loaded.'
$kSubInGame = 'In the game: Online > Player Match, a 1 vs 1 room. Rollback takes over when your opponent runs MK9 Rollback too.'
$kSubFailed = 'The lines below say why. Then press Play again.'
# The Play button is never greyed out by Windows (its grey text is unreadable on the dark look): a press is ignored
# while $script:playMode is 'busy', 'running' or 'blocked'.
#   play     ready to start                     busy     a start is under way
#   running  the game runs with the mod         retry    the game runs, the mod did not answer: a press sends again
#   blocked  this folder cannot be written
$script:playMode = 'play'
function SetPlay([string]$mode) {
    $script:playMode = $mode
    $hot = $mode -eq 'play' -or $mode -eq 'retry'
    $play.Text = switch ($mode) { 'busy' { 'STARTING...' } 'running' { 'GAME RUNNING' } 'retry' { '&TRY AGAIN' } default { '&PLAY' } }
    $back = if ($hot) { $c.Accent } elseif ($mode -eq 'running') { $c.Running } else { $c.Quiet }
    $play.Tag.Back = $back
    $play.Tag.Hot = if ($hot) { $c.AccentHot } else { $back }
    $play.Tag.Down = if ($hot) { $c.AccentDown } else { $back }
    $play.Tag.Fore = if ($hot) { $c.OnAccent } elseif ($mode -eq 'running') { $c.Good } else { $c.QuietText }
    $play.Cursor = if ($hot) { [System.Windows.Forms.Cursors]::Hand } else { [System.Windows.Forms.Cursors]::Default }
    $play.Invalidate()
}
# The status box starts under the banner while the banner shows, and keeps its bottom edge.
$script:logTop = $logEdge.Top
$script:logBottom = $logEdge.Bottom
$script:logBackPad = $logEdge.Height - $logBack.Height
$script:statusPad = $logBack.Height - $status.Height
# ($script:bannerOn, not $banner.Visible: that reads False for as long as the window itself is not shown yet.)
$script:bannerOn = $false
function LayoutLog {
    $top = if ($script:bannerOn) { $banner.Bottom + ($script:logTop - $play.Bottom) } else { $script:logTop }
    $logEdge.SetBounds($logEdge.Left, $top, $logEdge.Width, $script:logBottom - $top)
    $logBack.Height = $logEdge.Height - $script:logBackPad
    $status.Height = $logBack.Height - $script:statusPad
}
function ShowUpdateBanner([string]$version) {
    $bannerLabel.Text = "Version $version is available"
    $script:bannerOn = $true
    $banner.Visible = $true
    LayoutLog
}
# Brings this window to the front: asked by a second start ($kShowEvent), and at the start when Windows was told to
# start this process hidden. Windows then hides the first window the process shows, which is this one: the versions
# before 2026-11 restarted that way after an update (Start-Process -WindowStyle Hidden) and stayed invisible, with
# "already open" for every new start. Their restart still starts this script, so it shows itself.
function ShowSelf([switch]$OnlyIfHidden) {
    try {
        $h = $form.Handle
        $hidden = -not [MK9R.Native]::IsWindowVisible($h)
        if ($OnlyIfHidden -and -not $hidden) { return }
        if ([MK9R.Native]::IsIconic($h)) { [void][MK9R.Native]::ShowWindow($h, 9) }   # SW_RESTORE
        elseif ($hidden) { [void][MK9R.Native]::ShowWindow($h, 5) }                    # SW_SHOW
        [void][MK9R.Native]::SetForegroundWindow($h)
        $form.Activate()
    } catch { }
}

# An error no handler caught (the ThreadException handler at the top): one status line and the error file, not a .NET
# error box. The same error again within a minute is not repeated (launcher_lib Update-ErrorWatch).
$script:errorWatch = New-ErrorWatch
function HandlerError($ex) {
    try {
        $text = ("$($ex.Message)".Trim() -split "`r?`n")[0]
        if (Update-ErrorWatch $script:errorWatch $text) {
            $errFile = Join-Path $env:TEMP 'MK9Rollback-launcher-error.txt'
            $at = ''
            if ($ex -is [System.Management.Automation.IContainsErrorRecord] -and $ex.ErrorRecord.InvocationInfo) { $at = $ex.ErrorRecord.InvocationInfo.PositionMessage }
            try { Set-Content -LiteralPath $errFile -Value "An error inside the MK9 Rollback window:`r`n`r`n$($ex.Message)`r`n$at" -Encoding ascii } catch { }
            Say "Something went wrong in this window: $text (Saved in $errFile; please send it with your bug report if it keeps happening.)"
        }
        # A start step that fails at every tick of the timer would leave the Play button busy for good.
        if ($script:errorWatch.Count -eq 3 -and $script:state -ne 'idle' -and $script:state -ne 'watch') {
            Say 'This start is given up. Press Play to try again.'
            GoIdle -Failed
        }
    } catch { }
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
    'In a fight, F9 switches between the HUD display and the detailed network line.'
)
function ShowTrouble {
    [void][System.Windows.Forms.MessageBox]::Show("If the game stutters, freezes or will not connect:`n`n- $($troubleLines -join "`n- ")", 'Having trouble?', 'OK', 'Information')
}
$trouble.Add_LinkClicked({ ShowTrouble })

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
$script:startTicks = 0
$bgTimer = New-Object System.Windows.Forms.Timer
$bgTimer.Interval = 200
$bgTimer.Add_Tick({
    if ($script:startTicks -lt 25) { $script:startTicks++; if (-not $SnapshotPng) { ShowSelf -OnlyIfHidden } }
    if ($script:showEvent) { try { if ($script:showEvent.WaitOne(0)) { ShowSelf } } catch { } }
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
$kFinishJob = { param($updater, $release, $root, $queue) . $updater; Complete-Install $release $root { param($s) [void]$queue.Add($s) } }
$kLogsJob = { param($lib, $dir, $dest, $self) . $lib; Save-LogsZip $dir $dest $self }
$kHintJob = { param($lib) . $lib; Get-ConnectionHint }

# Updates: checked once when the window opens (quietly: only a newer version is mentioned) and on the link.
$script:release = $null
$script:updateBusy = $false
$script:checkQuiet = $false
function CheckUpdate([switch]$Quiet) {
    if ($script:updateBusy) { return }
    $script:updateBusy = $true
    $script:checkQuiet = [bool]$Quiet
    $updLink.Enabled = $false
    if (-not $Quiet) { Say 'Checking for updates...' }
    Start-BgJob $kCheckJob @{ updater = $updaterPath; timeout = $(if ($Quiet) { 4 } else { 10 }) } $onChecked
}
$onChecked = {
    param($out, $err)
    $script:updateBusy = $false
    $updLink.Enabled = $true
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
        Say "UPDATE AVAILABLE: version $($script:release.Version). Close the game, then press 'Update now'. Your opponent needs the same version."
        ShowUpdateBanner $script:release.Version
        if ($form.Text -notlike '*update available') { $form.Text = "$($form.Text) - update available" }
    } else {
        if (-not $Quiet) { Say "You have the latest version ($($installed.version))." }
        FinishInstall
    }
}
# An update installed by a version before 2026-11 could not bring "MK9 Rollback.exe" (that version's updater refuses
# a zip with a file it does not know, so it is given a zip without it; updater.ps1). The program is fetched here, once,
# from the same release's full zip, and that version's "Play Online.bat" goes to previous_version.
$script:finishTried = $false
function FinishInstall {
    if ($script:finishTried -or $script:updateBusy -or $script:readOnly) { return }
    if (-not (Test-InstallIncomplete $script:release $root)) { return }
    $script:finishTried = $true
    $script:updateBusy = $true
    $updLink.Enabled = $false
    Say 'Finishing the update: fetching "MK9 Rollback.exe", the program that opens this window from now on...'
    Start-BgJob $kFinishJob @{ updater = $updaterPath; release = $script:release; root = $root; queue = $script:bgQueue } {
        param($out, $err)
        $script:updateBusy = $false
        $updLink.Enabled = $true
        if ($out -eq $true) { Say 'Done. From now on, open MK9 Rollback with "MK9 Rollback.exe" in its folder ("Play Online.bat" is gone).' }
        else { Say "It could not be added now$(if ($err) { " ($err)" }); it is tried again the next time this window opens. Until then `"Play Online.bat`" still opens it." }
        $backLink.Visible = Test-Path -LiteralPath (Join-Path $root 'previous_version')
    }
}
# Restarts this window on the files now on disk (after an update or going back): through "MK9 Rollback.exe" when it
# is there, else PowerShell directly, with no console and no request to start hidden (see ShowSelf).
function RestartWindow {
    Say 'Restarting this window...'
    try { $script:mutex.ReleaseMutex(); $script:mutexOwned = $false } catch { }
    $started = $false
    foreach ($exe in @((Join-Path $root 'MK9 Rollback.exe'), "$env:MK9R_LAUNCHER")) {
        if ($started -or -not $exe -or -not (Test-Path -LiteralPath $exe)) { continue }
        try { Start-Process -FilePath $exe -ErrorAction Stop; $started = $true } catch { }
    }
    if (-not $started) {
        try {
            $psi = New-Object System.Diagnostics.ProcessStartInfo $kPowerShell, "-NoProfile -ExecutionPolicy Bypass -File `"$PSScriptRoot\netplay_gui.ps1`""
            $psi.UseShellExecute = $false
            $psi.CreateNoWindow = $true
            [void][System.Diagnostics.Process]::Start($psi)
            $started = $true
        } catch { }
    }
    if (-not $started) {
        try { $script:mutexOwned = $script:mutex.WaitOne(0) } catch { }
        Say 'This window could not restart itself. Close it and open MK9 Rollback again.'
        return
    }
    $form.Close()
}
function GameBlocksUpdate { $script:state -ne 'idle' -or [bool](Get-Process MKKE -ErrorAction SilentlyContinue) }
$updLink.Add_LinkClicked({ if (-not $script:updateBusy) { CheckUpdate } })
$updBtn.Add_Click({
    if ($script:updateBusy) { return }
    if (-not ($script:release -and (Test-UpdateNewer $script:release $installed.version))) { CheckUpdate; return }
    if (GameBlocksUpdate) { Say 'Close the game first, then press Update now.'; return }
    $answer = [System.Windows.Forms.MessageBox]::Show("Install version $($script:release.Version)?`n`nYour opponent needs the same version to play with you.", 'MK9 Rollback update', 'YesNo', 'Question')
    if ($answer -ne 'Yes') { return }
    $script:updateBusy = $true
    $script:installing = $true
    $updLink.Enabled = $false; $backLink.Enabled = $false
    SetPlay 'busy'
    $play.Text = 'UPDATING...'
    SetStatus "Installing version $($script:release.Version)..." 'busy' 'This window restarts by itself when the update is in.'
    Start-BgJob $kInstallJob @{ updater = $updaterPath; release = $script:release; root = $root; queue = $script:bgQueue } $onInstalled
})
$onInstalled = {
    param($out, $err)
    $script:updateBusy = $false
    $script:installing = $false
    if ($script:closeWanted) { $form.Close(); return }  # closed during the install: no restart
    if ($out -eq $true) { RestartWindow; return }
    if ($err) { Say "The update failed: $err" }
    $updLink.Enabled = $true; $backLink.Enabled = $true
    SetStatus 'The update was not installed' 'bad' 'The lines below say why. Your files are as they were.'
    GoIdle
}
# Closing the window stops the background jobs where they are (the end of this file). An install stopped while it
# copies would leave some files new and some old, with nothing to put the old ones back (updater.ps1 Invoke-FileSwap
# only does that for an error). So the window stays until the install job has ended, then closes by itself.
$script:installing = $false
$script:closeWanted = $false
$form.Add_FormClosing({
    param($source, $e)
    if (-not $script:installing) { return }
    $e.Cancel = $true
    if (-not $script:closeWanted) { Say 'The update is still being installed: this window closes by itself when it is done. Please wait.' }
    $script:closeWanted = $true
})
# Undoes the last update (updater.ps1 Restore-PreviousVersion); refused while the game runs, like updating.
$backLink.Add_LinkClicked({
    if ($script:updateBusy) { return }
    if (GameBlocksUpdate) { Say 'Close the game first, then press the link again.'; return }
    $prevVer = (Get-InstalledVersion (Join-Path $root 'previous_version')).version
    $answer = [System.Windows.Forms.MessageBox]::Show("Go back to the previous version$(if ($prevVer) { " ($prevVer)" })?`n`nYour opponent needs the same version to play with you.", 'MK9 Rollback', 'YesNo', 'Question')
    if ($answer -ne 'Yes') { return }
    if (Restore-PreviousVersion $root { param($s) Say $s }) { RestartWindow }
})
$form.Add_Shown({
    if ($SnapshotPng) { return }
    ShowSelf -OnlyIfHidden
    if (-not $NoUpdateCheck) { CheckUpdate -Quiet }
    Start-BgJob $kHintJob @{ lib = $libPath } { param($out, $err) if ($out) { $hintLabel.Text = "$out" } }
})

# For a public bug report: Steam IDs, IP addresses and the user and PC names are replaced in the zip (launcher_lib
# Save-LogsZip, PRIVACY.txt in the zip says what was replaced).
$saveLogs.Add_LinkClicked({
    if (-not $saveLogs.Enabled) { return }
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
$script:armTries = 0
$timer = New-Object System.Windows.Forms.Timer
$timer.Interval = 1000

# What the game is told once the mod is loaded, the same on every PC: rollback, the network display in the HUD, and
# MK9's Online mode with the input delay from the ping (1 frame up to a 50 ms round trip, then 2 and more; OnlineMode
# DelayFromPing). The DLL echoes each command it takes ("CMD netcode rollback"); with no echo within 5 s the watch
# step sends them again, twice, before it asks the player (the button becomes TRY AGAIN). Sent again, the commands
# change nothing in a running fight.
$kArmLines = @('netcode rollback', 'overlay 1', 'online auto ping')
$kArmSends = 3
function Arm([switch]$Again) {
    if (-not $Again) { $script:armTries = 0 }
    $script:armTries++
    $script:ackDue = (Get-Date).AddSeconds(5)
    [void](Cmd $kArmLines)
    $script:state = 'watch'
    SetPlay 'running'
    if ($Again) { return }
    SetStatus 'Ready in the game' 'good' $kSubInGame
    Say 'Ready. In the game: Online > Player Match, a 1 vs 1 room (not King of the Hill). Play anyone there.'
    Say "If your opponent also runs MK9 Rollback, rollback takes over at the start of the fight. Against anyone else, MK9's own netcode is used. Ranked is never touched."
}

function GoIdle([switch]$Failed) {
    $script:state = 'idle'
    $script:ackDue = $null
    SetPlay $(if ($script:readOnly) { 'blocked' } else { 'play' })
    if ($Failed) { SetStatus 'The game did not start with the mod' 'bad' $kSubFailed }
}
$script:shownDesync = 0
# The game's own window has taken no message for 5 s (what Windows shows as "Not responding"). Asked of that window
# only, by its class: the game thread owns it (the "HANG?   window" lines of src\debug\CrashLog.cpp), and the game's
# second window (D3DProxyWindow) belongs to another thread. IsHungAppWindow answers at once; Process.Responding would
# wait up to 5 s for the game, and this window with it. $false when it cannot be told.
$script:hang = New-HangWatch
function GameHung($game) {
    try {
        $h = [IntPtr]::Zero
        for ($i = 0; $i -lt 16; $i++) {
            $h = [MK9R.Native]::FindWindowEx([IntPtr]::Zero, $h, 'LaunchUnrealUWindowsClient', [IntPtr]::Zero)
            if ($h -eq [IntPtr]::Zero) { break }
            $owner = [uint32]0
            [void][MK9R.Native]::GetWindowThreadProcessId($h, [ref]$owner)
            if ($owner -eq $game.Id) { return [bool][MK9R.Native]::IsHungAppWindow($h) }
        }
    } catch { }
    $false
}

$timer.Add_Tick({
    switch ($script:state) {
        'launch' {
            if (Get-Process MKKE -ErrorAction SilentlyContinue) {
                Say 'The game is running. Waiting for its menu...'
                SetStatus 'Waiting for the game''s menu...' 'busy' 'The mod is loaded once the game has reached its menu.'
                $script:state = 'settle'; $script:wait = 0
                return
            }
            if (++$script:wait -gt 150) { Say 'The game did not start. Start it from Steam, then press Play again.'; GoIdle -Failed }
        }
        'settle' {
            if (++$script:wait -ge 15) { $script:state = 'inject'; $script:wait = 0; SetStatus 'Loading the mod...' 'busy' }
        }
        'injectretry' {
            if (++$script:wait -ge 2) { $script:state = 'inject'; $script:wait = 0 }
        }
        'inject' {
            $games = @(Get-Process MKKE -ErrorAction SilentlyContinue)
            if ($games.Count -gt 1) { Say 'Two copies of the game are running. Close one.'; GoIdle -Failed; return }
            # Closed while this window waited for its menu or between two tries: the injector would end with code 1 and
            # the antivirus text below would be shown for it.
            if ($games.Count -eq 0) { Say (Get-InjectFailureText $null $false); GoIdle -Failed; return }
            $files = Test-InstalledFiles $root
            if (-not $files.Ok) { Say $files.Message; GoIdle -Failed; return }
            # The last session's logs are kept for "Save logs": a player who restarts after a crash would lose them.
            # Only the first try of a start moves them: another try would replace them with the log of the try before.
            Move-SessionLogs $dir ($script:tries -gt 0)
            # Never throws (launcher_lib): an injector the antivirus removed or blocks is a status line, not an error.
            $run = Invoke-Injector (Join-Path $dir 'MK9RollbackInjector.exe')
            if (-not $run.Started) { Say $run.Error; GoIdle -Failed; return }
            $r = Get-InjectorResult $run.Code
            if ($r.AlreadyLoaded) {
                Say 'The mod was already loaded in this game.'
                $script:tail = New-LogTail $log
                [void](Read-LogTail $script:tail -Discard)
                Arm
                return
            }
            if (-not $r.Loaded) {
                $running = [bool](Get-Process MKKE -ErrorAction SilentlyContinue)
                if ($running -and $r.Retry -and ++$script:tries -lt 6) { $script:state = 'injectretry'; $script:wait = 0; return }
                Say (Get-InjectFailureText $r $running)
                GoIdle -Failed
                return
            }
            $script:state = 'injectwait'; $script:wait = 0
        }
        'injectwait' {
            # A fresh log: read from its first line (a Steam or hook failure at start-up is shown too).
            if (Test-Path -LiteralPath $log) { Say 'Mod loaded.'; $script:tail = New-LogTail $log; Arm; return }
            if (++$script:wait -ge 10) {
                $running = [bool](Get-Process MKKE -ErrorAction SilentlyContinue)
                if ($running -and ++$script:tries -lt 6) { $script:state = 'inject'; return }
                Say (Get-InjectFailureText $null $running)
                GoIdle -Failed
            }
        }
        'watch' {
            $game = Get-Process MKKE -ErrorAction SilentlyContinue | Select-Object -First 1
            if (-not $game) {
                Say 'The game has closed.'
                $script:hang = New-HangWatch
                GoIdle
                SetStatus 'Ready to play' 'idle' $kSubIdle
                return
            }
            # A game that stays frozen (launcher_lib Update-HangWatch; the DLL's "HANG?" lines alone are not shown).
            $hang = Update-HangWatch $script:hang (GameHung $game)
            if ($hang -eq 'hung') {
                Say 'The game is not responding (frozen for more than 15 seconds). If it stays frozen, close it, then save the logs.'
                SetStatus 'The game is not responding' 'bad'
            } elseif ($hang -eq 'back') {
                Say 'The game is responding again.'
                SetStatus 'Ready in the game' 'good'
            }
            foreach ($line in (Read-LogTail $script:tail)) {
                if ($script:ackDue -and (Test-CommandAck $line)) {
                    $script:ackDue = $null
                    if ($script:playMode -eq 'retry') { SetPlay 'running'; SetStatus 'Ready in the game' 'good' $kSubInGame }
                }
                if (Update-MatchFacts $script:facts $line) { $matchLabel.Text = Format-MatchFacts $script:facts }
                $r = Get-LauncherMessage $line 'Rollback'
                if (-not $r) { continue }
                if ($r.Kind -eq 'desync' -and $script:shownDesync++) { continue }
                # a new fight or session starts counting again, so a later desync is shown too
                if ($r.Kind -eq 'reset') { $script:shownDesync = 0 }
                if ($r.Text) { Say $r.Text }
                if ($r.Head) { SetStatus $r.Head $r.Tone }
                if ($r.CheckUpdate) { CheckUpdate }
            }
            if ($script:ackDue -and (Get-Date) -gt $script:ackDue) {
                if ($script:armTries -lt $kArmSends) { Arm -Again }
                else {
                    $script:ackDue = $null
                    Say 'The mod did not answer. Press TRY AGAIN; if it repeats, close the game, press Play, and save the logs.'
                    SetStatus 'The mod is not answering' 'bad' 'Press TRY AGAIN. If it repeats, close the game and press Play.'
                    SetPlay 'retry'
                }
            }
        }
    }
})

$kReadOnlyText = 'This folder is read-only (or protected by Windows ransomware protection). Move the MK9 Rollback folder to e.g. C:\Games.'
$play.Add_Click({
    if ($script:playMode -eq 'busy' -or $script:playMode -eq 'running') { return }
    if ($script:readOnly) { Say $kReadOnlyText; return }
    if (-not (MyAccountId)) {
        Say 'Start Steam and sign in first.'
        SetStatus 'Steam is not running' 'warn' 'Start Steam and sign in, then press Play.'
        return
    }
    if (-not (Test-Path -LiteralPath (Join-Path $dir 'MK9RollbackInjector.exe'))) { Say "The mod files are missing next to this launcher ($dir)."; return }
    if ($script:state -eq 'watch') { Arm; return }
    if ($script:state -ne 'idle') { return }
    $files = Test-InstalledFiles $root
    if (-not $files.Ok) { Say $files.Message; SetStatus 'The mod''s files are damaged' 'bad' $kSubFailed; return }
    SetPlay 'busy'
    $script:tries = 0
    $game = Get-Process MKKE -ErrorAction SilentlyContinue | Select-Object -First 1
    if (-not $game) {
        Say 'Starting Mortal Kombat through Steam...'
        SetStatus 'Starting the game...' 'busy' 'Steam is starting Mortal Kombat. This can take a minute.'
        Start-Process $kPowerShell -WindowStyle Hidden -ArgumentList '-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', "`"$PSScriptRoot\launch_game.ps1`""
        $script:state = 'launch'; $script:wait = 0
    } elseif ((Test-Path -LiteralPath $log) -and ((Get-Item -LiteralPath $log).LastWriteTime -gt $game.StartTime)) {
        # This game already has the mod: only what it logs from now on is news.
        $script:tail = New-LogTail $log
        [void](Read-LogTail $script:tail -Discard)
        Arm
    } else {
        Say 'The game is already running. Loading the mod...'
        SetStatus 'Loading the mod...' 'busy' 'The game is already running.'
        $script:state = 'inject'
    }
})

SetPlay 'play'
SetStatus 'Ready to play' 'idle' $kSubIdle
Say 'Press Play. The game starts with the mod.'
if ($script:readOnly) {
    Say $kReadOnlyText
    SetPlay 'blocked'
    SetStatus 'This folder cannot be written' 'bad' 'Move the MK9 Rollback folder to e.g. C:\Games, then open it again.'
}

if ($SnapshotPng) {
    # What the picture shows: made-up lines, nothing is started.
    switch ($SnapshotState) {
        'starting' { SetPlay 'busy'; SetStatus 'Starting the game...' 'busy' 'Steam is starting Mortal Kombat. This can take a minute.'; Say 'Starting Mortal Kombat through Steam...' }
        'running' {
            SetPlay 'running'; SetStatus 'Rollback is on' 'good' $kSubInGame
            Say 'Starting Mortal Kombat through Steam...'; Say 'The game is running. Waiting for its menu...'; Say 'Mod loaded.'
            Say 'Ready. In the game: Online > Player Match, a 1 vs 1 room (not King of the Hill). Play anyone there.'
            Say 'Online fight found. Checking whether your opponent runs the mod...'; Say 'Both games run the mod.'
            Say 'Input delay this match: 1 frame(s).'; Say 'Rollback on. You are player 1.'
            $matchLabel.Text = 'Last match: input delay 1, ping 34 ms, 3 rollbacks a minute, always in sync.'
            $hintLabel.Text = 'You are on Wi-Fi: a cable gives steadier fights.'
        }
        'update' {
            Say "UPDATE AVAILABLE: version 20261231-1200. Close the game, then press 'Update now'. Your opponent needs the same version."
            ShowUpdateBanner '20261231-1200'
            $backLink.Visible = $true
        }
        'failed' { GoIdle -Failed; Say 'Could not load the mod into the game. Your antivirus may have blocked MK9RollbackInjector.exe. Press ''Having trouble?'' for what to try, then save the logs if it keeps failing.' }
    }
    try { $form.NoActivate = $true } catch { }
    $form.ShowInTaskbar = $false
    $form.StartPosition = 'Manual'
    $form.Location = New-Object System.Drawing.Point(-32000, -32000)
    $snapTimer = New-Object System.Windows.Forms.Timer
    $snapTimer.Interval = 1200
    $snapTimer.Add_Tick({
        $snapTimer.Stop()
        $bmp = New-Object System.Drawing.Bitmap($form.Width, $form.Height)
        $form.DrawToBitmap($bmp, (New-Object System.Drawing.Rectangle(0, 0, $form.Width, $form.Height)))
        $bmp.Save($SnapshotPng, [System.Drawing.Imaging.ImageFormat]::Png)
        $bmp.Dispose()
        $form.Close()
    })
    $form.Add_Shown({ $snapTimer.Start() })
    [System.Windows.Forms.Application]::Run($form)
} else {
    $timer.Start()
    $bgTimer.Start()
    [void]$form.ShowDialog()
}
$timer.Stop()
$bgTimer.Stop()
foreach ($j in @($script:jobs)) { try { $j.PS.Stop(); $j.PS.Dispose() } catch { } }
foreach ($i in $script:icons) { try { $i.Dispose() } catch { } }
if ($script:showEvent) { try { $script:showEvent.Dispose() } catch { } }
if ($script:mutexOwned) { try { $script:mutex.ReleaseMutex() } catch { } }
exit 0
