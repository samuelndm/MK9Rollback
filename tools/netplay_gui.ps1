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
# Updates are not a choice either (2026-11-16, the player: "make the mod launcher auto update and don't let the user
# launch the game if there is an update"): a newer release is installed without a question and the window restarts
# on it; Play waits for the check and is refused until the update is in (see "updates" below).
# The texts are in the player's language (tools\launcher_text.ps1: English and Portuguese (Brazil); Windows' display
# language, or the "Language" link at the bottom). They are written here in English, as the argument of Tr.
#   -NoUpdateCheck         no update check at all (tests): Play is never held for one
#   -SnapshotPng <file>    saves a picture of the window, then closes it (layout check). The window is then shown
#                          off the screen and without taking the keyboard, and does not count as the open window.
#   -SnapshotState <name>  with -SnapshotPng: what the picture shows (idle, checking, starting, running, update,
#                          updatewait, updatefailed, failed)
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
# Any error that stops this script: written to %TEMP%\MK9Rollback-launcher-error.txt (in English, for the bug report)
# and shown in the player's language.
trap {
    $errFile = Join-Path $env:TEMP 'MK9Rollback-launcher-error.txt'
    $detail = "$($_.Exception.Message)`r`n$($_.InvocationInfo.PositionMessage)"
    try { Set-Content -LiteralPath $errFile -Value "The MK9 Rollback window stopped because of an error:`r`n`r`n$detail" -Encoding ascii } catch { }
    $head = 'The MK9 Rollback window stopped because of an error:'; $saved = "(Saved in $errFile; please send it with your bug report.)"
    try { $head = Tr 'The MK9 Rollback window stopped because of an error:'; $saved = Tr '(Saved in {0}; please send it with your bug report.)' $errFile } catch { }
    $shown = $false
    try {
        Add-Type -AssemblyName System.Windows.Forms
        [void][System.Windows.Forms.MessageBox]::Show("$head`r`n`r`n$detail`r`n`r`n$saved", 'MK9 Rollback', 'OK', 'Error')
        $shown = $true
    } catch { }
    if (-not $shown) { try { Start-Process notepad.exe -ArgumentList "`"$errFile`"" } catch { } }
    exit 1
}
$ErrorActionPreference = 'Continue'
# The texts in the player's language (Tr, the tables and the choice of language). Missing, the window would say
# nothing at all: English then, until the check below names the missing file.
$textPath = Join-Path $PSScriptRoot 'launcher_text.ps1'
if (Test-Path -LiteralPath $textPath) { . $textPath; Set-UiLanguage (Get-UiLanguageChoice) }
else { function Tr { $t = "$($args[0])"; if ($args.Count -gt 1) { $t -f @($args[1..($args.Count - 1)]) } else { $t } } }

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
                [void][System.Windows.Forms.MessageBox]::Show("$(Tr 'MK9 Rollback is already open.')`r`n`r`n$(Tr 'If you cannot see its window: open Task Manager (Ctrl+Shift+Esc), end "Windows PowerShell" there, then open MK9 Rollback again.')", 'MK9 Rollback', 'OK', 'Information')
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
foreach ($p in @($libPath, $updaterPath, $textPath)) { if (-not (Test-Path -LiteralPath $p)) { throw (Tr '{0} is missing. Extract the zip again into an empty folder.' $p) } }
# Update check and install from the GitHub releases (Get-InstalledVersion, Get-LatestRelease, Install-Update), and the
# window's pure helpers.
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
        FieldText = [System.Drawing.ColorTranslator]::FromHtml('#E2D8C6'); Accent = Hex 'Accent'; AccentHot = Hex 'AccentHot'; AccentDown = Hex 'AccentDown'
        OnAccent = [System.Drawing.ColorTranslator]::FromHtml('#FBEBC0'); Good = Hex 'Good'; Warn = Hex 'Warn'; Bad = Hex 'Bad'; Idle = Hex 'Idle'
        Link = [System.Drawing.ColorTranslator]::FromHtml('#CDBB97'); LinkOff = [System.Drawing.ColorTranslator]::FromHtml('#5C5045')
        Quiet = [System.Drawing.ColorTranslator]::FromHtml('#241A15'); QuietText = Hex 'Muted'
        Running = [System.Drawing.ColorTranslator]::FromHtml('#14281A'); Banner = [System.Drawing.ColorTranslator]::FromHtml('#2E1A0E'); BannerText = Hex 'Text' }
}
# The arena look's extra colours: gold for the title, the rules and the button edges; the header's dark red; the
# credit strip under the footer. With high contrast, the system's.
if ($kHighContrast) {
    $c.Gold = $c.Text; $c.GoldDim = $c.Border; $c.Blood = $c.Panel; $c.Strip = $c.Bg
} else {
    $c.Gold = Hex 'Gold'; $c.GoldDim = Hex 'GoldDim'; $c.Blood = Hex 'Blood'; $c.Strip = [System.Drawing.ColorTranslator]::FromHtml('#070504')
}
# The title and the big button's font: Impact (on every Windows since XP; another font if it is missing).
function DisplayFont([single]$size) { New-Object System.Drawing.Font('Impact', $size) }
function Mix($a, $b, [double]$t) {
    [System.Drawing.Color]::FromArgb([int]($a.R + ($b.R - $a.R) * $t), [int]($a.G + ($b.G - $a.G) * $t), [int]($a.B + ($b.B - $a.B) * $t))
}
function UiFont([single]$size, [switch]$Bold, [switch]$Semi) {
    if ($Semi) { return New-Object System.Drawing.Font('Segoe UI Semibold', $size) }
    if ($Bold) { return New-Object System.Drawing.Font('Segoe UI', $size, [System.Drawing.FontStyle]::Bold) }
    New-Object System.Drawing.Font('Segoe UI', $size)
}

# ---- window ----
# Laid out at 96 DPI (100 %); ResumeLayout below scales every position and size to the display's DPI.
$kVersionText = if ($installed.version) { Tr 'Version {0}' $installed.version } else { Tr 'Development copy' }
$form = if ('MK9R.QuietForm' -as [type]) { New-Object MK9R.QuietForm } else { New-Object System.Windows.Forms.Form }
$form.SuspendLayout()
$form.Text = 'MK9 Rollback'
$form.ClientSize = New-Object System.Drawing.Size(460, 644)
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
# A rule across the window: a gold line that fades out at both ends ($strong: brighter, the header's edge).
function Rule($y, [int]$h = 1, [switch]$Strong) {
    $r = New-Object System.Windows.Forms.Panel
    $r.BackColor = $c.Border
    if (-not $kHighContrast) {
        $r.BackColor = $c.Bg
        $r.Tag = if ($Strong) { $c.Gold } else { $c.GoldDim }
        $r.Add_Paint({ param($s, $e) PaintFade $e.Graphics $s.ClientRectangle $s.Tag $c.Bg })
    }
    Place $r $form 0 $y 460 $h
}
function PaintFade($g, $rect, $mid, $edge) {
    if ($rect.Width -lt 2 -or $rect.Height -lt 1) { return }
    $wide = New-Object System.Drawing.Rectangle(($rect.X - 1), $rect.Y, ($rect.Width + 2), $rect.Height)
    $br = New-Object System.Drawing.Drawing2D.LinearGradientBrush($wide, $edge, $edge, [single]0)
    $blend = New-Object System.Drawing.Drawing2D.ColorBlend(3)
    $blend.Colors = [System.Drawing.Color[]]@($edge, $mid, $edge)
    $blend.Positions = [single[]]@(0, 0.5, 1)
    $br.InterpolationColors = $blend
    $g.FillRectangle($br, $rect)
    $br.Dispose()
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
    # Lit from above: lighter at the top, darker at the bottom; a gold edge on a red (ready) button, a bronze one else.
    $path = RoundedPath 0.5 0.5 ($b.Width - 1) ($b.Height - 1) $r
    $all = New-Object System.Drawing.Rectangle(0, 0, $b.Width, ($b.Height + 1))
    $brush = New-Object System.Drawing.Drawing2D.LinearGradientBrush($all, (Mix $fill ([System.Drawing.Color]::FromArgb(255, 120, 60)) 0.10), (Mix $fill ([System.Drawing.Color]::Black) 0.45), [single]90)
    $g.FillPath($brush, $path)
    $lit = $fill.ToArgb() -eq $c.Accent.ToArgb() -or $fill.ToArgb() -eq $c.AccentHot.ToArgb() -or $fill.ToArgb() -eq $c.AccentDown.ToArgb()
    $edgeColor = if ($lit) { $c.Gold } else { $c.Border }
    $edgePen = New-Object System.Drawing.Pen($edgeColor, [single][Math]::Max(1, (Scaled 1)))
    $g.DrawPath($edgePen, $path)
    $shine = New-Object System.Drawing.Pen([System.Drawing.Color]::FromArgb(45, 255, 240, 210), [single]1)
    $g.DrawLine($shine, [single]($r), [single](Scaled 2), [single]($b.Width - $r), [single](Scaled 2))
    $brush.Dispose(); $path.Dispose(); $edgePen.Dispose(); $shine.Dispose()
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
    if ($lit) {
        $sh = $b.ClientRectangle; $sh.Offset((Scaled 1), (Scaled 2))
        [System.Windows.Forms.TextRenderer]::DrawText($g, $b.Text, $b.Font, $sh, (Mix $fill ([System.Drawing.Color]::Black) 0.65), $flags)
    }
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
$kTitle = 'MK9 ROLLBACK'
$kTitleFont = DisplayFont 21
if ($kHighContrast) {
    [void](Label $header $kTitle 80 10 240 36 $kTitleFont $c.Text)
} else {
    # The arena look: dark red to black from the left, the title in gold with a shadow (painted, so it can shade).
    $logo.BackColor = [System.Drawing.Color]::Transparent
    $header.Add_Paint({
        param($s, $e)
        $g = $e.Graphics
        $all = $s.ClientRectangle
        $back = New-Object System.Drawing.Drawing2D.LinearGradientBrush((New-Object System.Drawing.Rectangle(-1, 0, ($all.Width + 2), $all.Height)), $c.Blood, $c.Panel, [single]0)
        $blend = New-Object System.Drawing.Drawing2D.ColorBlend(3)
        $blend.Colors = [System.Drawing.Color[]]@($c.Blood, (Mix $c.Blood $c.Panel 0.55), $c.Panel)
        $blend.Positions = [single[]]@(0, 0.45, 1)
        $back.InterpolationColors = $blend
        $g.FillRectangle($back, $all)
        $back.Dispose()
        $g.TextRenderingHint = [System.Drawing.Text.TextRenderingHint]::AntiAliasGridFit
        $at = New-Object System.Drawing.PointF([single](Scaled 78), [single](Scaled 8))
        $shadow = New-Object System.Drawing.SolidBrush([System.Drawing.Color]::FromArgb(200, 0, 0, 0))
        $g.DrawString($kTitle, $kTitleFont, $shadow, [single]($at.X + (Scaled 2)), [single]($at.Y + (Scaled 2)))
        $box = New-Object System.Drawing.RectangleF($at.X, $at.Y, [single](Scaled 260), [single](Scaled 40))
        $gold = New-Object System.Drawing.Drawing2D.LinearGradientBrush($box, (Hex 'AccentA'), (Hex 'AccentB'), [single]90)
        $g.DrawString($kTitle, $kTitleFont, $gold, $at)
        $shadow.Dispose(); $gold.Dispose()
    })
}
$tagline = Label $header (Tr 'Rollback netcode for Mortal Kombat Komplete Edition') 81 50 366 20 (UiFont 9) $c.Muted
$verLabel = Label $header $kVersionText 300 14 148 18 (UiFont 8.25) $c.Muted
$verLabel.TextAlign = 'TopRight'
if (-not $kHighContrast) { $tagline.BackColor = [System.Drawing.Color]::Transparent; $verLabel.BackColor = [System.Drawing.Color]::Transparent }
[void](Rule 84 2 -Strong)

# What is going on now: a coloured dot, one line, and one sentence of what to do next.
$dot = Label $form ([string][char]0x25CF) 18 97 22 28 (UiFont 13) $c.Idle
$headline = Label $form '' 40 98 404 28 (UiFont 12.5 -Semi) $c.Text
$subline = Label $form '' 21 128 420 38 (UiFont 9) $c.Muted

$play = New-Object System.Windows.Forms.Button
$play.Font = DisplayFont 19
StyleButton $play 4
[void](Place $play $form 20 172 420 54)
$form.AcceptButton = $play

# Shown only while a newer version is due (ShowBanner): waiting for the game to close, installing, or failed (then with
# a button that tries again); the status box below then starts lower.
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
$updBtn.Text = Tr 'Try a&gain'
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
$matchLabel = Label $form (Tr 'Last match: none yet.') 21 486 420 20 (UiFont 9) $c.Muted
# The score of the online matches (the DLL's match_history.txt; launcher_lib Format-MatchRecord), and the list of them.
$recordLabel = Label $form (Tr 'Score: no online match counted yet.') 21 506 420 20 (UiFont 9) $c.Text
$hintLabel = Label $form '' 21 528 420 20 (UiFont 9) $c.Warn
[void](Rule 556)
# The footer: two centred rows of links with gold diamonds between them (CenterRow places them once the window is
# scaled: their widths depend on the language and the display), then the credit strip.
function FooterLink([string]$text, $y) {
    $l = Link $form $text 0 $y 100 'MiddleCenter'
    $l.AutoSize = $true
    $l
}
function Diamond($y) {
    $d = Label $form ([string][char]0x25C6) 0 $y 10 22 (UiFont 6.75) $c.GoldDim
    $d.AutoSize = $true
    $d
}
$trouble = FooterLink (Tr '&Having trouble?') 566
$footDot1 = Diamond 566
$saveLogs = FooterLink (Tr '&Save logs to the Desktop') 566
$footDot2 = Diamond 566
$updLink = FooterLink (Tr '&Check for updates') 566
# Where "Go back to the previous version" was until 2026-11-16: with updates installed by themselves, an older version
# would only be updated again at the next start.
$historyLink = FooterLink (Tr '&Matches and replays') 590
$footDot3 = Diamond 590
$langLink = FooterLink (Tr '&Language: {0}' (Get-UiLanguageName $script:UiLang)) 590
$kFooterRows = @(@($trouble, $footDot1, $saveLogs, $footDot2, $updLink), @($historyLink, $footDot3, $langLink))
function CenterRow($items) {
    $gap = Scaled 10
    $total = 0
    foreach ($i in $items) { $total += $i.Width }
    $total += $gap * ($items.Count - 1)
    $x = [int](($form.ClientSize.Width - $total) / 2)
    $mid = $items[0].Top + [int]($items[0].Height / 2)
    foreach ($i in $items) { $i.Left = $x; $i.Top = $mid - [int]($i.Height / 2); $x += $i.Width + $gap }
}
# The credit strip: the author's nickname and the project's page (the releases' repository, updater.ps1 $kUpdateRepo).
$kProjectUrl = 'https://github.com/samuelndm/MK9Rollback'
$strip = New-Object System.Windows.Forms.Panel
$strip.BackColor = $c.Strip
[void](Place $strip $form 0 616 460 28)
$kCreditBy = Tr 'Made by Gyros'
$kCreditUrl = 'github.com/samuelndm/MK9Rollback'
$creditLink = New-Object System.Windows.Forms.LinkLabel
$creditLink.Text = "$kCreditBy   $([char]0x00B7)   $kCreditUrl"
$creditLink.LinkArea = New-Object System.Windows.Forms.LinkArea(($creditLink.Text.Length - $kCreditUrl.Length), $kCreditUrl.Length)
$creditLink.TextAlign = 'MiddleCenter'
$creditLink.Font = UiFont 8.25
$creditLink.ForeColor = $c.GoldDim
$creditLink.LinkBehavior = 'HoverUnderline'
$creditLink.LinkColor = $c.Muted; $creditLink.VisitedLinkColor = $c.Muted; $creditLink.ActiveLinkColor = $c.Gold
$creditLink.UseMnemonic = $false
[void](Place $creditLink $strip 0 0 460 28)
$creditLink.Add_LinkClicked({ try { Start-Process $kProjectUrl } catch { Say $kProjectUrl } })
if (-not $kHighContrast) {
    $creditLink.ForeColor = $c.Gold
    $stripEdge = New-Object System.Windows.Forms.Panel
    $stripEdge.BackColor = $c.Strip
    $stripEdge.Add_Paint({ param($s, $e) PaintFade $e.Graphics $s.ClientRectangle $c.GoldDim $c.Strip })
    [void](Place $stripEdge $strip 0 0 460 1)
}

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
foreach ($row in $kFooterRows) { CenterRow $row }
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
$kSubIdle = Tr 'Press Play: the game starts through Steam with the mod loaded.'
$kSubInGame = Tr 'In the game: Online > Player Match, a 1 vs 1 room. Rollback takes over when your opponent runs MK9 Rollback too.'
$kSubFailed = Tr 'The lines below say why. Then press Play again.'
# The Play button is never greyed out by Windows (its grey text is unreadable on the dark look): a press is ignored
# while $script:playMode is 'busy', 'running', 'checking', 'updating', 'updatewait' or 'blocked'.
#   play      ready to start                     busy        a start is under way
#   running   the game runs with the mod         retry       the game runs, the mod did not answer: a press sends again
#   blocked   this folder cannot be written      checking    the update check before a start
#   updating  an update is being installed       updatewait  an update is due and waits for the game to close
#   update    an update is due and its install failed: a press tries again
$script:playMode = 'play'
function SetPlay([string]$mode) {
    $script:playMode = $mode
    $hot = $mode -eq 'play' -or $mode -eq 'retry' -or $mode -eq 'update'
    $play.Text = switch ($mode) {
        'busy' { Tr 'STARTING...' } 'running' { Tr 'GAME RUNNING' } 'retry' { Tr '&TRY AGAIN' } 'checking' { Tr 'CHECKING FOR UPDATES...' }
        'updating' { Tr 'UPDATING...' } 'updatewait' { Tr 'CLOSE THE GAME TO UPDATE' } 'update' { Tr '&INSTALL THE UPDATE' } default { Tr '&PLAY' }
    }
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
# The banner with $text; -Button: with "Try again" on its right (the label then leaves room for it).
function ShowBanner([string]$text, [switch]$Button) {
    $bannerLabel.Text = $text
    $updBtn.Visible = [bool]$Button
    $right = if ($Button) { $updBtn.Left - (Scaled 8) } else { $banner.ClientSize.Width - (Scaled 12) }
    $bannerLabel.Width = [Math]::Max((Scaled 40), $right - $bannerLabel.Left)
    $script:bannerOn = $true
    $banner.Visible = $true
    LayoutLog
    $title = "MK9 Rollback - $(Tr 'update available')"
    if ($form.Text -ne $title) { $form.Text = $title }
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
            Say (Tr 'Something went wrong in this window: {0} (Saved in {1}; please send it with your bug report if it keeps happening.)' $text $errFile)
        }
        # A start step that fails at every tick of the timer would leave the Play button busy for good.
        if ($script:errorWatch.Count -eq 3 -and $script:state -ne 'idle' -and $script:state -ne 'watch') {
            Say (Tr 'This start is given up. Press Play to try again.')
            GoIdle -Failed
        }
    } catch { }
}

# Same text in README.md and the README of tools\package.ps1.
$troubleLines = @(
    (Tr 'Allow MKKE.exe through the Windows firewall (Windows Security > Firewall > Allow an app).'),
    (Tr 'Add the MK9 Rollback folder to your antivirus exclusions.'),
    (Tr 'Run the game from an SSD, and turn off disk sleep.'),
    (Tr 'On a laptop: plug it in and set the Windows power mode to "Best performance".'),
    (Tr 'Close browsers and other heavy apps (downloads, streaming, launchers).'),
    (Tr 'Use a wired (Ethernet) connection instead of Wi-Fi.'),
    (Tr 'Both players need the same version: this window installs a new one by itself when it opens.'),
    (Tr 'In a fight, F9 switches between the HUD display and the detailed network line.')
)
function ShowTrouble {
    [void][System.Windows.Forms.MessageBox]::Show("$(Tr 'If the game stutters, freezes or will not connect:')`n`n- $($troubleLines -join "`n- ")", (Tr 'Having trouble?'), 'OK', 'Information')
}
$trouble.Add_LinkClicked({ ShowTrouble })

# ---- score and match history ----
# The DLL writes a line to match_history.txt for every online one-on-one match that reaches MK9's results screen
# (src\net\ScoreKeeper.h); the window reads the file when it opens, when the DLL says a line was written, and for the list.
$historyPath = Join-Path $dir 'match_history.txt'
function UpdateRecord {
    try { $recordLabel.Text = Format-MatchRecord @(Read-MatchHistory $historyPath) } catch { }
}
# Every match, with its replay where the DLL wrote one (src\net\ReplayKeeper.h). The in-game score is the room's only
# since 2026-11-21 ("all matches should be saved elsewhere, so the user may find them in a History menu option or a
# better name (probably replay, this way we can also see the replay of the match)"): this list keeps all of them.
$replaysPath = Join-Path $dir 'replays'
# Arms the playback of one replay in the running game ('replay <file>'): the next offline fight plays it.
function WatchReplay($r, $note) {
    if (-not $r -or -not (Test-Path -LiteralPath $r.Path)) { $note.Text = Tr 'This replay file is gone.'; return }
    if ($script:state -ne 'watch' -or -not (Get-Process MKKE -ErrorAction SilentlyContinue)) {
        $note.ForeColor = $c.Warn
        $note.Text = Tr 'Press PLAY first: a replay plays in the game with the mod. Then choose it here again.'
        return
    }
    if (-not (Cmd @("replay $($r.Name)"))) { $note.ForeColor = $c.Bad; $note.Text = Tr 'The game could not be told (the command file is busy). Try again.'; return }
    $note.ForeColor = $c.Good
    $note.Text = Tr 'Ready: in the game, start an offline fight (Ladder or Versus) and the replay plays in it. Not in an online room.'
    Say (Tr 'Replay {0} is ready: start an offline fight (Ladder or Versus) in the game and it plays there.' $r.Name)
}
function ShowHistory {
    $entries = @(Read-MatchHistory $historyPath)
    $entries = @(Add-MatchReplays $entries @(Get-ReplayFiles $replaysPath))
    $friends = Get-SteamFriendNames
    $dlg = New-Object System.Windows.Forms.Form
    $dlg.SuspendLayout()
    $dlg.Text = Tr 'Matches and replays'
    $dlg.ClientSize = New-Object System.Drawing.Size(920, 500)
    $dlg.MinimumSize = New-Object System.Drawing.Size(520, 300)
    $dlg.StartPosition = 'CenterParent'
    $dlg.ShowInTaskbar = $false
    $dlg.MinimizeBox = $false
    $dlg.Font = UiFont 9.75
    $dlg.BackColor = $c.Bg
    $dlg.ForeColor = $c.Text
    if ($script:icons.Count) { $dlg.Icon = $script:icons[0] }
    if (-not $kHighContrast) {
        $dlg.Add_HandleCreated({ param($s, $e) try { $on = 1; [void][MK9R.Native]::DwmSetWindowAttribute($s.Handle, 20, [ref]$on, 4) } catch { } })
    }
    # The list: newest first, the result in its colour.
    $lv = New-Object System.Windows.Forms.ListView
    $lv.View = 'Details'; $lv.FullRowSelect = $true; $lv.HeaderStyle = 'Nonclickable'; $lv.MultiSelect = $false
    $lv.BorderStyle = 'None'; $lv.BackColor = $c.Field; $lv.ForeColor = $c.FieldText
    $lv.Dock = 'Fill'
    foreach ($col in @(@((Tr 'When'), 128), @((Tr 'Result'), 72), @((Tr 'Rounds'), 60), @((Tr 'You played'), 150), @((Tr 'Opponent played'), 150), @((Tr 'Arena'), 160), @((Tr 'Opponent'), 120), @((Tr 'Replay'), 60))) {
        [void]$lv.Columns.Add($col[0], $col[1])
    }
    $whenFormat = Tr 'yyyy-MM-dd HH:mm'
    $lv.BeginUpdate()
    for ($i = $entries.Count - 1; $i -ge 0; $i--) {
        $e = $entries[$i]
        $it = New-Object System.Windows.Forms.ListViewItem($e.Time.ToString($whenFormat, [Globalization.CultureInfo]::InvariantCulture))
        $it.UseItemStyleForSubItems = $false
        $it.ForeColor = $c.FieldText; $it.BackColor = $c.Field
        $res = switch ($e.Result) { 'WIN' { Tr 'Won' } 'LOSS' { Tr 'Lost' } default { Tr 'Draw' } }
        foreach ($v in @($res, $(if ($e.Rounds -eq '?') { '' } else { $e.Rounds }), (Get-FighterName $e.Mine), (Get-FighterName $e.Theirs), (Get-ArenaName $e.Arena), (Get-OpponentLabel $e.Opponent $friends), $(if ($e.Replay) { Tr 'yes' } else { '' }))) {
            $sub = $it.SubItems.Add([string]$v)
            $sub.ForeColor = $c.FieldText; $sub.BackColor = $c.Field
        }
        $it.SubItems[1].ForeColor = switch ($e.Result) { 'WIN' { $c.Good } 'LOSS' { $c.Bad } default { $c.Muted } }
        $it.SubItems[7].ForeColor = $c.Accent
        $it.Tag = $e.Replay
        [void]$lv.Items.Add($it)
    }
    $lv.EndUpdate()
    if (-not $kHighContrast) { $lv.Add_HandleCreated({ param($s, $e) try { [void][MK9R.Native]::SetWindowTheme($s.Handle, 'DarkMode_Explorer', [IntPtr]::Zero) } catch { } }) }
    # Above it: today's and every day's score, and each opponent's (the three most played).
    $top = New-Object System.Windows.Forms.Panel
    $top.Dock = 'Top'
    $top.BackColor = $c.Panel
    $lines = @()
    if (-not $entries.Count) {
        $lines += Tr 'No online match counted yet.'
        $lines += Tr 'A match counts when both players are in a 1 vs 1 Player Match room and it reaches MK9''s results screen.'
    } else {
        $lines += (Format-MatchRecord $entries)
        $byOpp = @($entries | Group-Object { $_.Opponent } | Sort-Object Count -Descending | Select-Object -First 3)
        foreach ($g in $byOpp) {
            $t = Get-MatchTally $g.Group
            $lines += Tr 'Against {0}: {1}.' (Get-OpponentLabel ([uint64]$g.Name) $friends) (Format-Tally $t)
        }
    }
    $sum = New-Object System.Windows.Forms.Label
    $sum.UseMnemonic = $false
    $sum.Text = $lines -join "`r`n"
    $sum.ForeColor = $c.Text
    $sum.Location = New-Object System.Drawing.Point(16, 10)
    $sum.AutoSize = $true
    $top.Controls.Add($sum)
    $top.Height = 20 + 20 * $lines.Count
    # Below it: the selected match's replay, and where the files are.
    $foot = New-Object System.Windows.Forms.Panel
    $foot.Dock = 'Bottom'
    $foot.Height = 76
    $foot.BackColor = $c.Bg
    $watch = New-Object System.Windows.Forms.Button
    $watch.Text = Tr '&Watch the replay'
    $watch.Font = UiFont 9.75 -Semi
    $watch.Location = New-Object System.Drawing.Point(14, 8)
    $watch.Size = New-Object System.Drawing.Size(180, 30)
    StyleButton $watch
    $note = New-Object System.Windows.Forms.Label
    $note.UseMnemonic = $false
    $note.AutoSize = $false
    $note.Location = New-Object System.Drawing.Point(206, 8)
    $note.Size = New-Object System.Drawing.Size(700, 30)
    $note.TextAlign = 'MiddleLeft'
    $note.Anchor = 'Top, Left, Right'
    $note.ForeColor = $c.Muted
    $countWith = @($entries | Where-Object { $_.Replay }).Count
    $note.Text = if ($countWith) { Tr 'Pick a match marked "yes" in the Replay column, then Watch (or double-click it).' } else { Tr 'No replay yet: matches played with version 20261117-1517 or later have one.' }
    # The button is lit only for a match that has a replay.
    $syncWatch = {
        $r = if ($lv.SelectedItems.Count) { $lv.SelectedItems[0].Tag } else { $null }
        $watch.Enabled = [bool]$r
        if (-not $kHighContrast) { $watch.Tag.Back = $(if ($r) { $c.Accent } else { $c.Border }); $watch.Tag.Fore = $(if ($r) { $c.OnAccent } else { $c.Muted }); $watch.Invalidate() }
    }
    $lv.Add_SelectedIndexChanged($syncWatch)
    $watch.Add_Click({ if ($lv.SelectedItems.Count) { WatchReplay $lv.SelectedItems[0].Tag $note } })
    $lv.Add_DoubleClick({ if ($lv.SelectedItems.Count -and $lv.SelectedItems[0].Tag) { WatchReplay $lv.SelectedItems[0].Tag $note } })
    & $syncWatch
    $fileLink = New-Object System.Windows.Forms.LinkLabel
    $fileLink.Text = Tr 'Show the file (match_history.txt)'
    $fileLink.LinkBehavior = 'HoverUnderline'
    $fileLink.LinkColor = $c.Link; $fileLink.VisitedLinkColor = $c.Link; $fileLink.ActiveLinkColor = $c.AccentHot
    $fileLink.Location = New-Object System.Drawing.Point(14, 48)
    $fileLink.AutoSize = $true
    $fileLink.Enabled = Test-Path -LiteralPath $historyPath
    $fileLink.Add_LinkClicked({ if (Test-Path -LiteralPath $historyPath) { Start-Process explorer.exe -ArgumentList "/select,`"$historyPath`"" } })
    $folderLink = New-Object System.Windows.Forms.LinkLabel
    $folderLink.Text = Tr 'Open the replays folder'
    $folderLink.LinkBehavior = 'HoverUnderline'
    $folderLink.LinkColor = $c.Link; $folderLink.VisitedLinkColor = $c.Link; $folderLink.ActiveLinkColor = $c.AccentHot
    $folderLink.Location = New-Object System.Drawing.Point(330, 48)
    $folderLink.AutoSize = $true
    $folderLink.Enabled = Test-Path -LiteralPath $replaysPath
    $folderLink.Add_LinkClicked({ if (Test-Path -LiteralPath $replaysPath) { Start-Process explorer.exe -ArgumentList "`"$replaysPath`"" } })
    $foot.Controls.Add($watch)
    $foot.Controls.Add($note)
    $foot.Controls.Add($fileLink)
    $foot.Controls.Add($folderLink)
    $dlg.Controls.Add($lv)
    $dlg.Controls.Add($foot)
    $dlg.Controls.Add($top)
    $dlg.AutoScaleDimensions = New-Object System.Drawing.SizeF(96, 96)
    $dlg.AutoScaleMode = [System.Windows.Forms.AutoScaleMode]::Dpi
    $dlg.ResumeLayout($true)
    try { [void]$dlg.ShowDialog($form) } finally { $dlg.Dispose() }
    UpdateRecord
}
$historyLink.Add_LinkClicked({ try { ShowHistory } catch { Say (Tr 'The match history could not be shown: {0}' $_.Exception.Message) } })

# ---- language ----
# The "Language" link opens a list of the languages (launcher_text.ps1 $script:UiLanguages). A choice is kept for
# this Windows account (Save-UiLanguageChoice) and the window restarts in it; while a start, the game or an update is
# under way it is only kept, for the next time the window opens.
$langMenu = New-Object System.Windows.Forms.ContextMenuStrip
$langMenu.ShowImageMargin = $true
foreach ($code in @($script:UiLanguages.Keys)) {
    $item = New-Object System.Windows.Forms.ToolStripMenuItem($script:UiLanguages[$code])
    $item.Tag = $code
    $item.Checked = ($code -eq $script:UiLang)
    $item.Add_Click({ param($s, $e) ChangeLanguage "$($s.Tag)" })
    [void]$langMenu.Items.Add($item)
}
function ChangeLanguage([string]$code) {
    if ($code -eq $script:UiLang) { return }
    if (-not (Save-UiLanguageChoice $code)) { Say (Tr 'The language could not be saved.'); return }
    if ($script:state -eq 'idle' -and -not $script:updateBusy -and -not $script:installing) { RestartWindow; return }
    Say (Tr 'This window shows {0} the next time it opens.' (Get-UiLanguageName $code))
}
$langLink.Add_LinkClicked({ $langMenu.Show($langLink, (New-Object System.Drawing.Point(0, 0)), [System.Windows.Forms.ToolStripDropDownDirection]::AboveRight) })

# ---- background work (update check, download and install, saving the logs, the Wi-Fi check) ----
# Each job is a runspace of its own; $bgTimer polls them and passes status lines from $bgQueue to the window. Every job
# first loads the texts in this window's language (Start-BgJob adds $text and $lang to its parameters).
$script:jobs = New-Object System.Collections.ArrayList
$script:bgQueue = [System.Collections.ArrayList]::Synchronized((New-Object System.Collections.ArrayList))
function Start-BgJob([scriptblock]$code, [hashtable]$params, [scriptblock]$done) {
    $ps = [powershell]::Create()
    [void]$ps.AddScript($code.ToString())
    $params['text'] = $textPath
    $params['lang'] = $script:UiLang
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
$kCheckJob = { param($text, $lang, $updater, $timeout) . $text; Set-UiLanguage $lang; . $updater; $r = Get-LatestRelease -TimeoutSec $timeout; [pscustomobject]@{ Release = $r; Info = (Get-UpdateError) } }
$kInstallJob = { param($text, $lang, $updater, $release, $root, $queue) . $text; Set-UiLanguage $lang; . $updater; Install-Update $release $root { param($s) [void]$queue.Add($s) } }
$kFinishJob = { param($text, $lang, $updater, $release, $root, $queue) . $text; Set-UiLanguage $lang; . $updater; Complete-Install $release $root { param($s) [void]$queue.Add($s) } }
$kLogsJob = { param($text, $lang, $lib, $dir, $dest, $self) . $text; Set-UiLanguage $lang; . $lib; Save-LogsZip $dir $dest $self }
$kHintJob = { param($text, $lang, $lib) . $text; Set-UiLanguage $lang; . $lib; Get-ConnectionHint }

# ---- updates ----
# Only the newest version plays (2026-11-16, the player: "make the mod launcher auto update and don't let the user
# launch the game if there is an update"). The window asks GitHub when it opens, and again before a start when it last
# asked more than $kRecheckMinutes ago; Play waits for the answer (CHECKING FOR UPDATES...). A newer release is then
# installed at once, without a question, and the window restarts on it. While the game runs, the install waits for the
# game to close (the timer's idle step starts it). Play is refused for as long as an update is due, also after an
# install that failed: the button, the banner's "Try again" and "Check for updates" then try again (never by
# themselves, so a broken download does not loop), and a status line names the release page for a download by hand.
# A check that gets no answer (no internet, GitHub's limit of 60 checks an hour) lets Play go ahead: an update that
# cannot be seen cannot be installed either, and the next start asks again. A development copy (no version.txt) is
# never updated by itself. Going back to the previous version is no longer offered in the window (it would be updated
# again at the next start); tools\updater.ps1 -Restore still does it.
$script:release = $null
$script:updateBusy = $false      # a check, an install or the program's fetch is under way
$script:checkQuiet = $false
$script:updateDue = $null        # the release to install before the next start; $null: none
$script:updateFailed = $false    # its install failed: tried again on a press only
$script:waitTold = $false        # the player was told to close the game for it
$script:lastCheck = [datetime]::MinValue
$script:playAfterCheck = $false  # a press of Play waits for the check under way
$kRecheckMinutes = 10
$kDevCopy = -not $installed.version
function CheckUpdate([switch]$Quiet) {
    if ($script:updateBusy) { return }
    $script:updateBusy = $true
    $script:checkQuiet = [bool]$Quiet
    $script:lastCheck = Get-Date
    $updLink.Enabled = $false
    if (-not $Quiet) { Say (Tr 'Checking for updates...') }
    if ($script:state -eq 'idle' -and -not $script:updateDue -and -not $script:readOnly) { SetPlay 'checking' }
    Start-BgJob $kCheckJob @{ updater = $updaterPath; timeout = $(if ($Quiet) { 6 } else { 10 }) } $onChecked
}
# Play again, once nothing holds it (no start under way, no update due).
function ReleasePlay {
    if ($script:state -ne 'idle' -or $script:updateDue) { return }
    SetPlay $(if ($script:readOnly) { 'blocked' } else { 'play' })
}
$onChecked = {
    param($out, $err)
    $script:updateBusy = $false
    $updLink.Enabled = $true
    $quiet = $script:checkQuiet
    $thenPlay = $script:playAfterCheck
    $script:playAfterCheck = $false
    if ($out -and $out.Release) { $script:release = $out.Release }
    if (-not ($out -and $out.Release)) {
        $why = if ($out -and $out.Info.Error) { $out.Info.Error } elseif ($err) { $err } else { Tr 'no answer' }
        Say (Tr 'The update check failed ({0}). You can play; the window asks again before the next start.' "$why".TrimEnd('.'))
        ReleasePlay
        if ($thenPlay) { StartPlay }
        return
    }
    if (Test-UpdateNewer $script:release $installed.version) {
        if ($kDevCopy) {
            Say (Tr 'Version {0} is out. A development copy is not updated by itself.' $script:release.Version)
            ReleasePlay
            if ($thenPlay) { StartPlay }
            return
        }
        if (-not ($script:updateDue -and $script:updateDue.Version -eq $script:release.Version)) {
            $script:updateDue = $script:release
            $script:updateFailed = $false
            $script:waitTold = $false
            Say (Tr 'UPDATE: version {0} is out. It is installed now, by itself; your opponent needs it too.' $script:release.Version)
        } elseif (-not $quiet) {
            $script:updateFailed = $false  # "Check for updates" after a failed install: try again
        }
        InstallDueUpdate
        return
    }
    if (-not $quiet) { Say (Tr 'You have the latest version ({0}).' $installed.version) }
    ReleasePlay
    FinishInstall
    if ($thenPlay) { StartPlay }
}
# The release that is due: installed now when nothing is in the way, else as soon as it is gone (the timer's idle step
# calls this every second while an update is due and has not failed). -Retry: after a failed install, on a press.
function InstallDueUpdate([switch]$Retry) {
    $rel = $script:updateDue
    if (-not $rel -or $script:installing) { return }
    if ($Retry) { $script:updateFailed = $false }
    if ($script:updateFailed) {
        if ($script:state -eq 'idle' -and $script:playMode -ne 'update') { SetPlay 'update' }
        return
    }
    if ($script:readOnly) {
        if (-not $script:waitTold) {
            $script:waitTold = $true
            ShowBanner (Tr 'Version {0} is available' $rel.Version)
            Say (Tr 'Version {0} cannot be installed into a read-only folder. Move the MK9 Rollback folder to e.g. C:\Games, then open it again.' $rel.Version)
        }
        return
    }
    if ($script:updateBusy) { return }   # a check or the program's fetch: the next tick comes back
    if (GameBlocksUpdate) {
        if ($script:state -eq 'idle' -and $script:playMode -ne 'updatewait') { SetPlay 'updatewait' }
        if (-not $script:waitTold) {
            $script:waitTold = $true
            ShowBanner (Tr 'Version {0} is available: it installs once the game is closed' $rel.Version)
            Say (Tr 'Close the game: version {0} then installs by itself. Your opponent needs it too.' $rel.Version)
            if ($script:state -eq 'idle') { SetStatus (Tr 'Close the game to update') 'warn' (Tr 'The new version installs by itself once the game is closed. Then press Play.') }
        }
        return
    }
    $script:updateBusy = $true
    $script:installing = $true
    $updLink.Enabled = $false
    SetPlay 'updating'
    ShowBanner (Tr 'Installing version {0}...' $rel.Version)
    SetStatus (Tr 'Installing version {0}...' $rel.Version) 'busy' (Tr 'This window restarts by itself when the update is in. Then press Play.')
    Start-BgJob $kInstallJob @{ updater = $updaterPath; release = $rel; root = $root; queue = $script:bgQueue } $onInstalled
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
    Say (Tr 'Finishing the update: fetching "MK9 Rollback.exe", the program that opens this window from now on...')
    Start-BgJob $kFinishJob @{ updater = $updaterPath; release = $script:release; root = $root; queue = $script:bgQueue } {
        param($out, $err)
        $script:updateBusy = $false
        $updLink.Enabled = $true
        if ($out -eq $true) { Say (Tr 'Done. From now on, open MK9 Rollback with "MK9 Rollback.exe" in its folder ("Play Online.bat" is gone).') }
        else { Say (Tr 'It could not be added now{0}; it is tried again the next time this window opens. Until then "Play Online.bat" still opens it.' $(if ($err) { " ($err)" } else { '' })) }
    }
}
# Restarts this window on the files now on disk (after an update or a change of language): through "MK9 Rollback.exe"
# when it is there, else PowerShell directly, with no console and no request to start hidden (see ShowSelf).
function RestartWindow([switch]$AfterUpdate) {
    Say (Tr 'Restarting this window...')
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
        Say (Tr 'This window could not restart itself. Close it and open MK9 Rollback again.')
        if ($AfterUpdate) {
            # The new files are in; this window still runs the old ones.
            SetPlay 'blocked'
            SetStatus (Tr 'Open MK9 Rollback again') 'warn' (Tr 'The update is in. Close this window and open MK9 Rollback again to play.')
        }
        return
    }
    $form.Close()
}
function GameBlocksUpdate { $script:state -ne 'idle' -or [bool](Get-Process MKKE -ErrorAction SilentlyContinue) }
$updLink.Add_LinkClicked({ if (-not $script:updateBusy) { CheckUpdate } })
$updBtn.Add_Click({
    if ($script:updateBusy -or -not $script:updateDue) { return }
    if (GameBlocksUpdate) { Say (Tr 'Close the game first: version {0} then installs by itself.' $script:updateDue.Version); return }
    InstallDueUpdate -Retry
})
$onInstalled = {
    param($out, $err)
    $script:updateBusy = $false
    $script:installing = $false
    if ($script:closeWanted) { $form.Close(); return }  # closed during the install: no restart
    if ($out -eq $true) {
        # The files on disk are the new version's: nothing of this window may start another install meanwhile.
        $script:updateDue = $null
        $timer.Stop()
        RestartWindow -AfterUpdate
        return
    }
    if ($err) { Say (Tr 'The update failed: {0}' $err) }
    $script:updateFailed = $true
    $rel = $script:updateDue
    $updLink.Enabled = $true
    Say (Tr 'Press Try again. Or download version {0} yourself from {1} and extract it over this folder.' $rel.Version $rel.Page)
    ShowBanner (Tr 'Version {0} was not installed' $rel.Version) -Button
    GoIdle
    SetStatus (Tr 'The update was not installed') 'bad' (Tr 'The lines below say why. Play stays off until the new version is in: press the button to try again.')
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
    if (-not $script:closeWanted) { Say (Tr 'The update is still being installed: this window closes by itself when it is done. Please wait.') }
    $script:closeWanted = $true
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
    Say (Tr 'Saving the logs...')
    $selfIds = @()
    $acct = MyAccountId
    if ($acct) { $selfIds += "$($kSteamBase + $acct)" }
    Start-BgJob $kLogsJob @{ lib = $libPath; dir = $dir; dest = [Environment]::GetFolderPath('Desktop'); self = [string[]]$selfIds } {
        param($out, $err)
        $saveLogs.Enabled = $true
        if (-not $out -or -not $out.Zip) { Say $(if ($out) { $out.Error } else { Tr 'The logs zip could not be made ({0}).' $err }); return }
        Say (Tr 'Logs saved: {0}. Steam IDs, IP addresses and your user and PC names in them were replaced (PRIVACY.txt in the zip lists what). Send it together with your opponent''s zip.' $out.Zip)
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
    SetStatus (Tr 'Ready in the game') 'good' $kSubInGame
    Say (Tr 'Ready. In the game: Online > Player Match, a 1 vs 1 room or a King of the Hill room with just the two of you. Play anyone there.')
    Say (Tr 'If your opponent also runs MK9 Rollback, rollback takes over at the start of the fight. Against anyone else, MK9''s own netcode is used. Ranked is never touched.')
}

# Back to no start under way. An update that came due meanwhile is installed now, or waits for the game to close.
function GoIdle([switch]$Failed) {
    $script:state = 'idle'
    $script:ackDue = $null
    SetPlay $(if ($script:readOnly) { 'blocked' } else { 'play' })
    if ($Failed) { SetStatus (Tr 'The game did not start with the mod') 'bad' $kSubFailed }
    if ($script:updateDue) { $script:waitTold = $false; InstallDueUpdate }
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
        'idle' {
            # An update that is due waits for the game to close (InstallDueUpdate says so once, then starts it).
            if ($script:updateDue -and -not $script:installing -and -not $script:updateFailed) { InstallDueUpdate }
        }
        'launch' {
            if (Get-Process MKKE -ErrorAction SilentlyContinue) {
                Say (Tr 'The game is running. Waiting for its menu...')
                SetStatus (Tr 'Waiting for the game''s menu...') 'busy' (Tr 'The mod is loaded once the game has reached its menu.')
                $script:state = 'settle'; $script:wait = 0
                return
            }
            if (++$script:wait -gt 150) { Say (Tr 'The game did not start. Start it from Steam, then press Play again.'); GoIdle -Failed }
        }
        'settle' {
            if (++$script:wait -ge 15) { $script:state = 'inject'; $script:wait = 0; SetStatus (Tr 'Loading the mod...') 'busy' }
        }
        'injectretry' {
            if (++$script:wait -ge 2) { $script:state = 'inject'; $script:wait = 0 }
        }
        'inject' {
            $games = @(Get-Process MKKE -ErrorAction SilentlyContinue)
            if ($games.Count -gt 1) { Say (Tr 'Two copies of the game are running. Close one.'); GoIdle -Failed; return }
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
                Say (Tr 'The mod was already loaded in this game.')
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
            if (Test-Path -LiteralPath $log) { Say (Tr 'Mod loaded.'); $script:tail = New-LogTail $log; Arm; return }
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
                Say (Tr 'The game has closed.')
                $script:hang = New-HangWatch
                UpdateRecord
                SetStatus (Tr 'Ready to play') 'idle' $kSubIdle
                GoIdle   # last: an update that came due during the game starts here and sets its own status
                return
            }
            # A game that stays frozen (launcher_lib Update-HangWatch; the DLL's "HANG?" lines alone are not shown).
            $hang = Update-HangWatch $script:hang (GameHung $game)
            if ($hang -eq 'hung') {
                Say (Tr 'The game is not responding (frozen for more than 15 seconds). If it stays frozen, close it, then save the logs.')
                SetStatus (Tr 'The game is not responding') 'bad'
            } elseif ($hang -eq 'back') {
                Say (Tr 'The game is responding again.')
                SetStatus (Tr 'Ready in the game') 'good'
            }
            foreach ($line in (Read-LogTail $script:tail)) {
                if ($script:ackDue -and (Test-CommandAck $line)) {
                    $script:ackDue = $null
                    if ($script:playMode -eq 'retry') { SetPlay 'running'; SetStatus (Tr 'Ready in the game') 'good' $kSubInGame }
                }
                if (Update-MatchFacts $script:facts $line) { $matchLabel.Text = Format-MatchFacts $script:facts }
                # A counted match: what it gave, and the score line again once the DLL has written it to its file.
                $scoreSay = Get-ScoreSay $line
                if ($scoreSay) { Say $scoreSay }
                if ($line -like '*SCORE: written to*') { UpdateRecord }
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
                    Say (Tr 'The mod did not answer. Press TRY AGAIN; if it repeats, close the game, press Play, and save the logs.')
                    SetStatus (Tr 'The mod is not answering') 'bad' (Tr 'Press TRY AGAIN. If it repeats, close the game and press Play.')
                    SetPlay 'retry'
                }
            }
        }
    }
})

$kReadOnlyText = Tr 'This folder is read-only (or protected by Windows ransomware protection). Move the MK9 Rollback folder to e.g. C:\Games.'
# Starts the game (or loads the mod into a running one), once the press of Play has passed its checks and, when the
# last one was too old, the update check (onChecked calls this then).
function StartPlay {
    if ($script:state -ne 'idle' -or $script:updateDue) { return }
    SetPlay 'busy'
    $script:tries = 0
    $game = Get-Process MKKE -ErrorAction SilentlyContinue | Select-Object -First 1
    if (-not $game) {
        Say (Tr 'Starting Mortal Kombat through Steam...')
        SetStatus (Tr 'Starting the game...') 'busy' (Tr 'Steam is starting Mortal Kombat. This can take a minute.')
        Start-Process $kPowerShell -WindowStyle Hidden -ArgumentList '-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', "`"$PSScriptRoot\launch_game.ps1`""
        $script:state = 'launch'; $script:wait = 0
    } elseif ((Test-Path -LiteralPath $log) -and ((Get-Item -LiteralPath $log).LastWriteTime -gt $game.StartTime)) {
        # This game already has the mod: only what it logs from now on is news.
        $script:tail = New-LogTail $log
        [void](Read-LogTail $script:tail -Discard)
        Arm
    } else {
        Say (Tr 'The game is already running. Loading the mod...')
        SetStatus (Tr 'Loading the mod...') 'busy' (Tr 'The game is already running.')
        $script:state = 'inject'
    }
}
$play.Add_Click({
    if (@('busy', 'running', 'checking', 'updating', 'updatewait') -contains $script:playMode) { return }
    # An update is due and its install failed (the button says INSTALL THE UPDATE): a press tries it again.
    if ($script:updateDue -and $script:state -eq 'idle') {
        if (GameBlocksUpdate) { Say (Tr 'Close the game first: version {0} then installs by itself.' $script:updateDue.Version); return }
        InstallDueUpdate -Retry
        return
    }
    if ($script:readOnly) { Say $kReadOnlyText; return }
    if (-not (MyAccountId)) {
        Say (Tr 'Start Steam and sign in first.')
        SetStatus (Tr 'Steam is not running') 'warn' (Tr 'Start Steam and sign in, then press Play.')
        return
    }
    if (-not (Test-Path -LiteralPath (Join-Path $dir 'MK9RollbackInjector.exe'))) { Say (Tr 'The mod files are missing next to this launcher ({0}).' $dir); return }
    if ($script:state -eq 'watch') { Arm; return }
    if ($script:state -ne 'idle') { return }
    $files = Test-InstalledFiles $root
    if (-not $files.Ok) { Say $files.Message; SetStatus (Tr 'The mod''s files are damaged') 'bad' $kSubFailed; return }
    # The last answer from GitHub is older than $kRecheckMinutes: ask again first; the answer starts the game.
    if (-not $NoUpdateCheck -and -not $kDevCopy -and ((Get-Date) - $script:lastCheck).TotalMinutes -ge $kRecheckMinutes) {
        if ($script:updateBusy) { return }
        $script:playAfterCheck = $true
        CheckUpdate -Quiet
        return
    }
    StartPlay
})

# Until the first update check has answered (Shown starts it), Play waits.
SetPlay $(if ($NoUpdateCheck -or $SnapshotPng) { 'play' } else { 'checking' })
SetStatus (Tr 'Ready to play') 'idle' $kSubIdle
Say (Tr 'Press Play. The game starts with the mod.')
if (-not $SnapshotPng) { UpdateRecord }
if ($script:readOnly) {
    Say $kReadOnlyText
    SetPlay 'blocked'
    SetStatus (Tr 'This folder cannot be written') 'bad' (Tr 'Move the MK9 Rollback folder to e.g. C:\Games, then open it again.')
}

if ($SnapshotPng) {
    # What the picture shows: made-up lines, nothing is started.
    $kSnapVersion = '20261231-1200'
    switch ($SnapshotState) {
        'checking' { SetPlay 'checking' }
        'starting' { SetPlay 'busy'; SetStatus (Tr 'Starting the game...') 'busy' (Tr 'Steam is starting Mortal Kombat. This can take a minute.'); Say (Tr 'Starting Mortal Kombat through Steam...') }
        'running' {
            SetPlay 'running'; SetStatus (Tr 'Rollback is on') 'good' $kSubInGame
            Say (Tr 'Starting Mortal Kombat through Steam...'); Say (Tr 'The game is running. Waiting for its menu...'); Say (Tr 'Mod loaded.')
            Say (Tr 'Ready. In the game: Online > Player Match, a 1 vs 1 room or a King of the Hill room with just the two of you. Play anyone there.')
            foreach ($l in @('ONLINE auto: the game''s online opponent is x', 'ONLINE: both games run MK9Rollback', 'ONLINE: input delay 1 agreed', 'ONLINE: this game controls player 1')) { Say (Get-LauncherMessage $l).Text }
            $sf = New-MatchFacts; $sf.Delay = 1; $sf.Rtt = 34; $sf.Frames = 3600; $sf.Loads = 3; $sf.Fights = 1; $sf.Seen = $true
            $matchLabel.Text = Format-MatchFacts $sf
            $recordLabel.Text = Tr 'Score today: {0}. All time: {1}.' (Format-Tally ([pscustomobject]@{ Wins = 3; Losses = 2; Draws = 0 })) '41-38'
            $hintLabel.Text = Tr 'You are on Wi-Fi: a cable gives steadier fights.'
        }
        'update' {
            Say (Tr 'UPDATE: version {0} is out. It is installed now, by itself; your opponent needs it too.' $kSnapVersion); Say (Tr 'Downloading version {0}...' $kSnapVersion)
            SetPlay 'updating'; ShowBanner (Tr 'Installing version {0}...' $kSnapVersion)
            SetStatus (Tr 'Installing version {0}...' $kSnapVersion) 'busy' (Tr 'This window restarts by itself when the update is in. Then press Play.')
        }
        'updatewait' {
            Say (Tr 'UPDATE: version {0} is out. It is installed now, by itself; your opponent needs it too.' $kSnapVersion)
            Say (Tr 'Close the game: version {0} then installs by itself. Your opponent needs it too.' $kSnapVersion)
            SetPlay 'updatewait'; ShowBanner (Tr 'Version {0} is available: it installs once the game is closed' $kSnapVersion)
            SetStatus (Tr 'Close the game to update') 'warn' (Tr 'The new version installs by itself once the game is closed. Then press Play.')
        }
        'updatefailed' {
            Say (Tr 'UPDATE: version {0} is out. It is installed now, by itself; your opponent needs it too.' $kSnapVersion); Say (Tr 'Downloading version {0}...' $kSnapVersion)
            Say (Tr 'The download failed: {0}. Nothing was changed.' 'The operation has timed out.')
            Say (Tr 'Press Try again. Or download version {0} yourself from {1} and extract it over this folder.' $kSnapVersion "https://github.com/samuelndm/MK9Rollback/releases/tag/v$kSnapVersion")
            SetPlay 'update'; ShowBanner (Tr 'Version {0} was not installed' $kSnapVersion) -Button
            SetStatus (Tr 'The update was not installed') 'bad' (Tr 'The lines below say why. Play stays off until the new version is in: press the button to try again.')
        }
        'failed' { GoIdle -Failed; Say (Get-InjectFailureText $null $true) }
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
