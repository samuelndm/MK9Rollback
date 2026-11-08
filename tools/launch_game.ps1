# Start MKKE through Steam and wait for its process. After a Steam restart the game opens the
# Mortal Kombat Launcher first: its "Play Mortal Kombat" button is pressed (BM_CLICK) if it appears.
param([int]$TimeoutSeconds = 120)
$ErrorActionPreference = 'Continue'
Add-Type -AssemblyName UIAutomationClient, UIAutomationTypes
if (-not ('U.W' -as [type])) {
    Add-Type -Name W -Namespace U -MemberDefinition '[DllImport("user32.dll")] public static extern IntPtr SendMessage(IntPtr h, int m, IntPtr w, IntPtr l);'
}
Start-Process 'steam://rungameid/237110'
$t = 0
while (-not (Get-Process MKKE -ErrorAction SilentlyContinue) -and $t -lt $TimeoutSeconds) {
    Start-Sleep 3; $t += 3
    $l = Get-Process MKLauncher -ErrorAction SilentlyContinue | Where-Object { $_.MainWindowHandle -ne 0 } | Select-Object -First 1
    if (-not $l) { continue }
    try {
        $root = [System.Windows.Automation.AutomationElement]::FromHandle($l.MainWindowHandle)
        $cond = New-Object System.Windows.Automation.PropertyCondition([System.Windows.Automation.AutomationElement]::NameProperty, 'Play Mortal Kombat')
        $b = $root.FindFirst([System.Windows.Automation.TreeScope]::Descendants, $cond)
        if ($b) { [U.W]::SendMessage([IntPtr]$b.Current.NativeWindowHandle, 0x00F5, [IntPtr]::Zero, [IntPtr]::Zero) | Out-Null; 'launcher: pressed Play' }
    } catch {}
}
[bool](Get-Process MKKE -ErrorAction SilentlyContinue)
