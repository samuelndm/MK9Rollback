# Desktop helper for driving MKKE during runtime experiments (no in-process input injection).
#   . .\desk.ps1 ; Shot 'out.png' ; Key 0x0D 120 ; Keys @(0x28,0x28,0x0D)
Add-Type -ReferencedAssemblies System.Drawing, System.Windows.Forms @'
using System;
using System.Drawing;
using System.Drawing.Imaging;
using System.Runtime.InteropServices;
public static class Desk {
  [StructLayout(LayoutKind.Sequential)] struct KEYBDINPUT { public ushort wVk, wScan; public uint dwFlags, time; public IntPtr extra; }
  [StructLayout(LayoutKind.Explicit)] struct INPUT { [FieldOffset(0)] public uint type; [FieldOffset(8)] public KEYBDINPUT ki; [FieldOffset(8)] long pad0; [FieldOffset(16)] long pad1; [FieldOffset(24)] long pad2; }
  [DllImport("user32.dll")] static extern uint SendInput(uint n, INPUT[] i, int size);
  [DllImport("user32.dll")] static extern uint MapVirtualKey(uint code, uint type);
  [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
  [DllImport("user32.dll")] public static extern IntPtr GetForegroundWindow();
  [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr h, int cmd);
  [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
  [StructLayout(LayoutKind.Sequential)] public struct RECT { public int L, T, R, B; }
  static void Send(ushort vk, bool up) {
    ushort sc = (ushort)MapVirtualKey(vk, 0);
    uint flags = 0x0008u | (up ? 0x0002u : 0u);            // KEYEVENTF_SCANCODE | KEYUP
    if (vk >= 0x21 && vk <= 0x2E) flags |= 0x0001u;        // extended: arrows, ins/del, home/end, pgup/dn
    var i = new INPUT[1]; i[0].type = 1; i[0].ki.wVk = vk; i[0].ki.wScan = sc; i[0].ki.dwFlags = flags;
    SendInput(1, i, Marshal.SizeOf(typeof(INPUT)));
  }
  public static void Down(ushort vk) { Send(vk, false); }
  public static void Up(ushort vk) { Send(vk, true); }
  public static void Shot(IntPtr hwnd, string path, double scale) {
    RECT r; GetWindowRect(hwnd, out r);
    int w = r.R - r.L, h = r.B - r.T;
    using (var bmp = new Bitmap(w, h)) {
      using (var g = Graphics.FromImage(bmp)) g.CopyFromScreen(r.L, r.T, 0, 0, new Size(w, h));
      using (var s = new Bitmap(bmp, (int)(w * scale), (int)(h * scale))) s.Save(path, ImageFormat.Png);
    }
  }
}
'@
Add-Type @'
using System;
using System.Runtime.InteropServices;
public static class DeskPost {
  [DllImport("user32.dll")] static extern bool PostMessage(IntPtr h, uint m, IntPtr w, IntPtr l);
  [DllImport("user32.dll")] static extern uint MapVirtualKey(uint code, uint type);
  public static void Press(IntPtr h, uint vk, int holdMs) {
    uint sc = MapVirtualKey(vk, 0);
    PostMessage(h, 0x100, (IntPtr)vk, (IntPtr)(int)(1 | (sc << 16)));                        // WM_KEYDOWN
    System.Threading.Thread.Sleep(holdMs);
    PostMessage(h, 0x101, (IntPtr)vk, (IntPtr)unchecked((int)(1 | (sc << 16) | (3u << 30))));  // WM_KEYUP
  }
}
'@
function GameWindow { (Get-Process MKKE).MainWindowHandle }
function Focus {
    $h = GameWindow; $r = New-Object Desk+RECT; [Desk]::GetWindowRect($h, [ref]$r) | Out-Null
    if ($r.L -le -30000 -or $r.L -eq -25600) { [Desk]::ShowWindow($h, 9) | Out-Null }  # restore only if minimized
    for ($i = 0; $i -lt 5 -and [Desk]::GetForegroundWindow() -ne $h; $i++) {
        [Desk]::Down(0x12); [Desk]::Up(0x12)   # Alt tap lifts the foreground lock
        [Desk]::SetForegroundWindow($h) | Out-Null; Start-Sleep -Milliseconds 400
    }
    Start-Sleep -Milliseconds 300
    if ([Desk]::GetForegroundWindow() -ne $h) { throw 'MKKE window is not in the foreground' }
}
function IsFg { [Desk]::GetForegroundWindow() -eq (GameWindow) }
function Shot([string]$path, [double]$scale = 0.5) { [Desk]::Shot((GameWindow), $path, $scale) }
function Key([int]$vk, [int]$holdMs = 120) { [Desk]::Down($vk); Start-Sleep -Milliseconds $holdMs; [Desk]::Up($vk); Start-Sleep -Milliseconds 250 }
function Keys([int[]]$vks, [int]$holdMs = 120, [int]$gapMs = 400) { foreach ($k in $vks) { Key $k $holdMs; Start-Sleep -Milliseconds $gapMs } }
# MK9's menus read the keyboard from its window messages: SendInput (Key) never reaches them (2026-10-31: menu_repro's
# Enter presses left every run at the title, whose prompt is the space bar). Posted WM_KEYDOWN/WM_KEYUP do.
function PostKey([int]$vk, [int]$holdMs = 120) { [DeskPost]::Press((GameWindow), $vk, $holdMs); Start-Sleep -Milliseconds 250 }
