# helpers for driving the 86Box window with real keystrokes (needs an unlocked desktop)
Add-Type -AssemblyName System.Drawing
Add-Type @"
using System; using System.Runtime.InteropServices;
public static class VK86 {
 [StructLayout(LayoutKind.Sequential)] public struct RECT { public int L,T,R,B; }
 [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
 [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
 [DllImport("user32.dll")] public static extern IntPtr GetForegroundWindow();
 [DllImport("user32.dll")] public static extern bool BringWindowToTop(IntPtr h);
 [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr h, int c);
 [DllImport("user32.dll")] public static extern void keybd_event(byte vk, byte scan, uint f, IntPtr e);
 [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
 [DllImport("user32.dll")] public static extern bool AttachThreadInput(uint a, uint b, bool f);
 [DllImport("kernel32.dll")] public static extern uint GetCurrentThreadId();
 [DllImport("user32.dll")] public static extern void SwitchToThisWindow(IntPtr h, bool a);
 public static bool Front(IntPtr h) {
  uint pid; uint fg = GetWindowThreadProcessId(GetForegroundWindow(), out pid), me = GetCurrentThreadId();
  keybd_event(0x12,0,0,IntPtr.Zero); keybd_event(0x12,0,2,IntPtr.Zero);
  if (fg != me) AttachThreadInput(me, fg, true);
  ShowWindow(h, 9); BringWindowToTop(h); SetForegroundWindow(h);
  if (fg != me) AttachThreadInput(me, fg, false);
  if (GetForegroundWindow() != h) SwitchToThisWindow(h, true);
  return GetForegroundWindow() == h;
 }
}
"@
function Get-86Box { Get-Process 86Box -ErrorAction SilentlyContinue | Where-Object { $_.MainWindowTitle -like "*86Box*" } | Select-Object -First 1 }
function Shot($path) {
    $p = Get-86Box; $r = New-Object VK86+RECT; [void][VK86]::GetWindowRect($p.MainWindowHandle, [ref]$r)
    $b = New-Object System.Drawing.Bitmap ($r.R - $r.L), ($r.B - $r.T); $g = [System.Drawing.Graphics]::FromImage($b)
    $g.CopyFromScreen($r.L, $r.T, 0, 0, $b.Size); $b.Save($path); $g.Dispose(); $b.Dispose()
}
function Key([byte]$vk, [byte]$scan, [switch]$ext) {
    $f = if ($ext) { 1 } else { 0 }
    [VK86]::keybd_event($vk, $scan, $f, [IntPtr]::Zero); Start-Sleep -Milliseconds 120
    [VK86]::keybd_event($vk, $scan, $f -bor 2, [IntPtr]::Zero); Start-Sleep -Milliseconds 200
}
Add-Type @"
using System; using System.Runtime.InteropServices;
public static class VKS { [DllImport("user32.dll")] public static extern short VkKeyScan(char c); [DllImport("user32.dll")] public static extern uint MapVirtualKey(uint c, uint t); }
"@
function TypeText([string]$s) {
    foreach ($ch in $s.ToCharArray()) {
        $k = [VKS]::VkKeyScan($ch); if ($k -eq -1) { continue }
        $vk = [byte]($k -band 0xff); $shift = ($k -shr 8) -band 1
        $sc = [byte][VKS]::MapVirtualKey($vk, 0)
        if ($shift) { [VK86]::keybd_event(0x10, 0x2A, 0, [IntPtr]::Zero) }
        [VK86]::keybd_event($vk, $sc, 0, [IntPtr]::Zero); Start-Sleep -Milliseconds 40
        [VK86]::keybd_event($vk, $sc, 2, [IntPtr]::Zero)
        if ($shift) { [VK86]::keybd_event(0x10, 0x2A, 2, [IntPtr]::Zero) }
        Start-Sleep -Milliseconds 60
    }
}
