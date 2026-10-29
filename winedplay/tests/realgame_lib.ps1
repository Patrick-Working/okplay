# Shared by the real-game tests: put the drop-in into an AoE2 folder for a run, and put the folder back.
#   . $PSScriptRoot\realgame_lib.ps1
#   $st = Install-DropIn -GameDir $g -Release $r -Ini @{ intro = "0"; trace = "1" } -Windowed
#   ...
#   Restore-DropIn $st   (in a finally block)
# Install refuses to touch a folder that already holds the drop-in's files or a running game.

$script:DropInFiles = "dplayx.dll", "okindeo.dll", "okplay.ini"

function Set-IniValues([string]$path, [hashtable]$values, [string]$section) {
    # key=value lines in $section (all sections when empty); missing keys go at the end of $section
    $lines = [System.Collections.ArrayList]@(Get-Content $path)
    $cur = ""; $seen = @{}; $end = $lines.Count
    for ($i = 0; $i -lt $lines.Count; $i++) {
        if ($lines[$i] -match '^\s*\[(.+)\]') {
            if ($section -and $cur -eq $section -and $end -eq $lines.Count) { $end = $i }
            $cur = $Matches[1]; continue
        }
        if ($section -and $cur -ne $section) { continue }
        foreach ($k in $values.Keys) {
            if ($lines[$i] -match "^\s*$([regex]::Escape($k))\s*=") { $lines[$i] = "$k=$($values[$k])"; $seen[$k] = 1 }
        }
    }
    foreach ($k in $values.Keys) { if (-not $seen[$k]) { $lines.Insert($end, "$k=$($values[$k])"); $end++ } }
    Set-Content -Path $path -Value $lines -Encoding ASCII
}

function Install-DropIn([string]$GameDir, [string]$Release, [hashtable]$Ini = @{}, [switch]$Windowed) {
    foreach ($f in $script:DropInFiles + "netplay-logs") {
        if (Test-Path (Join-Path $GameDir $f)) { throw "refusing: $f already in $GameDir" }
    }
    if (Get-Process | Where-Object { $_.ProcessName -like "age2*" }) { throw "refusing: the game is running" }
    $st = @{ GameDir = $GameDir; Copied = @(); DdrawBackup = $null }
    foreach ($f in $script:DropInFiles) {
        $src = Join-Path $Release $f
        if (Test-Path $src) { Copy-Item $src $GameDir; $st.Copied += $f }
    }
    if ($Ini.Count) { Set-IniValues (Join-Path $GameDir "okplay.ini") $Ini "okplay" }
    $dd = Join-Path $GameDir "ddraw.ini"
    if ($Windowed -and (Test-Path $dd)) {
        # cnc-ddraw: a normal 800x600 window, so two instances fit side by side
        $st.DdrawBackup = Join-Path $GameDir "ddraw.ini.realgame-backup"
        if (Test-Path $st.DdrawBackup) { throw "refusing: $($st.DdrawBackup) exists (an earlier run did not restore)" }
        Copy-Item $dd $st.DdrawBackup
        Set-IniValues $dd @{ fullscreen = "false"; windowed = "true"; maintas = "false"; width = "800"; height = "600"; border = "true"; renderer = "gdi"; devmode = "true"; noactivateapp = "true" } "ddraw"
        # gdi: a screen copy sees it (OpenGL comes out black); devmode: no cursor lock (its first click
        # into the window would only lock the cursor); noactivateapp: the game never hears it lost focus
    }
    return $st
}

# --- windows: place them, capture them (for finding a button, once), click in them
Add-Type -AssemblyName System.Drawing
Add-Type @"
using System;
using System.Runtime.InteropServices;
public static class RgWin {
    [StructLayout(LayoutKind.Sequential)] public struct RECT { public int L, T, R, B; }
    [StructLayout(LayoutKind.Sequential)] public struct POINT { public int X, Y; }
    [DllImport("user32.dll")] public static extern bool SetWindowPos(IntPtr h, IntPtr after, int x, int y, int w, int hh, uint f);
    [DllImport("user32.dll")] public static extern bool GetClientRect(IntPtr h, out RECT r);
    [DllImport("user32.dll")] public static extern bool ClientToScreen(IntPtr h, ref POINT p);
    [DllImport("user32.dll")] public static extern bool PostMessage(IntPtr h, uint m, IntPtr w, IntPtr l);
    [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
    [DllImport("user32.dll")] public static extern bool SetCursorPos(int x, int y);
    [DllImport("user32.dll")] public static extern void mouse_event(uint f, int x, int y, uint d, IntPtr e);
    [DllImport("user32.dll")] public static extern void keybd_event(byte vk, byte scan, uint f, IntPtr e);
    [DllImport("user32.dll")] public static extern IntPtr GetForegroundWindow();
    [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
    [DllImport("user32.dll")] public static extern bool AttachThreadInput(uint a, uint b, bool attach);
    [DllImport("user32.dll")] public static extern bool BringWindowToTop(IntPtr h);
    [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr h, int cmd);
    [DllImport("kernel32.dll")] public static extern uint GetCurrentThreadId();
    [DllImport("user32.dll")] public static extern void SwitchToThisWindow(IntPtr h, bool altTab);
    [DllImport("user32.dll")] public static extern IntPtr WindowFromPoint(POINT p);
    [DllImport("user32.dll")] public static extern IntPtr GetAncestor(IntPtr h, uint flags);
    public static bool Front(IntPtr h) {
        IntPtr fg = GetForegroundWindow();
        uint pid, fgThread = GetWindowThreadProcessId(fg, out pid), me = GetCurrentThreadId();
        uint target = GetWindowThreadProcessId(h, out pid);
        keybd_event(0x12, 0, 0, IntPtr.Zero); keybd_event(0x12, 0, 2, IntPtr.Zero);   // Alt tap
        if (fgThread != me) AttachThreadInput(me, fgThread, true);
        if (target != me) AttachThreadInput(me, target, true);
        ShowWindow(h, 9);   // SW_RESTORE
        BringWindowToTop(h);
        SetForegroundWindow(h);
        if (target != me) AttachThreadInput(me, target, false);
        if (fgThread != me) AttachThreadInput(me, fgThread, false);
        if (GetForegroundWindow() != h) SwitchToThisWindow(h, true);   // what Alt-Tab uses
        return GetForegroundWindow() == h;
    }
}
"@

function Get-GameWindow($p) {
    $p.Refresh()
    if ($p.HasExited -or $p.MainWindowHandle -eq [IntPtr]::Zero) { return [IntPtr]::Zero }
    return $p.MainWindowHandle
}

function Move-GameWindow($p, [int]$x, [int]$y) {
    $h = Get-GameWindow $p
    # NOSIZE|NOZORDER|ASYNCWINDOWPOS: never wait on the game's message loop
    if ($h -ne [IntPtr]::Zero) { [void][RgWin]::SetWindowPos($h, [IntPtr]::Zero, $x, $y, 0, 0, 0x0001 -bor 0x0004 -bor 0x4000) }
}

function Save-GameWindow($p, [string]$png) {
    # the client area as it is on screen (the game draws with OpenGL: PrintWindow would be black)
    $h = Get-GameWindow $p
    if ($h -eq [IntPtr]::Zero) { return $false }
    [void][RgWin]::SetForegroundWindow($h)
    Start-Sleep -Milliseconds 400
    $r = New-Object RgWin+RECT; [void][RgWin]::GetClientRect($h, [ref]$r)
    $pt = New-Object RgWin+POINT; [void][RgWin]::ClientToScreen($h, [ref]$pt)
    $bmp = New-Object System.Drawing.Bitmap ($r.R - $r.L), ($r.B - $r.T)
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    try {
        $g.CopyFromScreen($pt.X, $pt.Y, 0, 0, $bmp.Size)
        $bmp.Save($png, [System.Drawing.Imaging.ImageFormat]::Png)
        $ok = $true
    } catch {
        # a locked or disconnected desktop cannot be copied; the test itself does not need the picture
        Write-Host "  (no capture: $($_.Exception.InnerException.Message) - desktop locked or disconnected?)"
        $ok = $false
    }
    $g.Dispose(); $bmp.Dispose()
    return $ok
}

function Post-Click($p, [int]$x, [int]$y, [int]$HoldMs = 120) {
    # the click as window messages straight to the game's window: no real mouse, no foreground
    # needed (with OKP_TEST_MULTI the game believes its window is the active one)
    $h = Get-GameWindow $p
    if ($h -eq [IntPtr]::Zero) { return $false }
    $lp = [IntPtr](($y -shl 16) -bor ($x -band 0xffff))
    [void][RgWin]::PostMessage($h, 0x001C, [IntPtr]1, [IntPtr]::Zero)   # WM_ACTIVATEAPP(TRUE)
    [void][RgWin]::PostMessage($h, 0x0006, [IntPtr]1, [IntPtr]::Zero)   # WM_ACTIVATE(WA_ACTIVE)
    Start-Sleep -Milliseconds 150
    [void][RgWin]::PostMessage($h, 0x0200, [IntPtr]::Zero, $lp)   # WM_MOUSEMOVE
    Start-Sleep -Milliseconds 150
    [void][RgWin]::PostMessage($h, 0x0201, [IntPtr]1, $lp)        # WM_LBUTTONDOWN, MK_LBUTTON
    # buttons that act on release want to be drawn pressed for a frame or two first
    for ($t = 0; $t -lt $HoldMs; $t += 50) {
        Start-Sleep -Milliseconds 50
        [void][RgWin]::PostMessage($h, 0x0200, [IntPtr]1, $lp)    # WM_MOUSEMOVE with the button held
    }
    [void][RgWin]::PostMessage($h, 0x0202, [IntPtr]::Zero, $lp)   # WM_LBUTTONUP
    Start-Sleep -Milliseconds 150
    [void][RgWin]::PostMessage($h, 0x0200, [IntPtr]::Zero, $lp)
    return $true
}

function Post-Text($p, [string]$text, [switch]$Enter) {
    # typed characters as WM_CHAR to the game's window (e.g. into the room's chat line)
    $h = Get-GameWindow $p
    if ($h -eq [IntPtr]::Zero) { return $false }
    foreach ($c in $text.ToCharArray()) { [void][RgWin]::PostMessage($h, 0x0102, [IntPtr][int]$c, [IntPtr]1); Start-Sleep -Milliseconds 40 }
    if ($Enter) {
        [void][RgWin]::PostMessage($h, 0x0100, [IntPtr]0x0D, [IntPtr]0x001C0001)   # WM_KEYDOWN VK_RETURN
        [void][RgWin]::PostMessage($h, 0x0102, [IntPtr]0x0D, [IntPtr]0x001C0001)   # WM_CHAR '\r'
        [void][RgWin]::PostMessage($h, 0x0101, [IntPtr]0x0D, [IntPtr]0xC01C0001)   # WM_KEYUP
    }
    return $true
}

function Click-GameWindow($p, [int]$x, [int]$y) {
    # a real click at client (x, y): the window comes to the front, the cursor moves there, button down/up
    $h = Get-GameWindow $p
    if ($h -eq [IntPtr]::Zero) { return $false }
    # Windows only lets the process that had the last input bring a window to the front: a tap of
    # Alt makes that us. The game ignores clicks while its window is not the active one.
    [void][RgWin]::Front($h)
    # if Windows keeps another program in front (a desktop gadget, say), tell the game it is the
    # active one anyway - the messages Windows sends on activation - so it takes the click
    [void][RgWin]::PostMessage($h, 0x001C, [IntPtr]1, [IntPtr]::Zero)   # WM_ACTIVATEAPP(TRUE)
    [void][RgWin]::PostMessage($h, 0x0006, [IntPtr]1, [IntPtr]::Zero)   # WM_ACTIVATE(WA_ACTIVE)
    [void][RgWin]::PostMessage($h, 0x0007, [IntPtr]::Zero, [IntPtr]::Zero)   # WM_SETFOCUS
    Start-Sleep -Milliseconds 500
    $fg = [RgWin]::GetForegroundWindow()
    $front = $fg -eq $h
    if (-not $front) {
        $fpid = 0; [void][RgWin]::GetWindowThreadProcessId($fg, [ref]$fpid)
        $front = "no - foreground is pid $fpid '$((Get-Process -Id $fpid -ErrorAction SilentlyContinue).MainWindowTitle)'"
    }
    $pt = New-Object RgWin+POINT; $pt.X = $x; $pt.Y = $y
    [void][RgWin]::ClientToScreen($h, [ref]$pt)
    [void][RgWin]::SetCursorPos($pt.X - 3, $pt.Y - 3)
    Start-Sleep -Milliseconds 100
    [void][RgWin]::SetCursorPos($pt.X, $pt.Y)                # (a move the game sees before the press)
    Start-Sleep -Milliseconds 200
    $under = [RgWin]::GetAncestor([RgWin]::WindowFromPoint($pt), 2)   # GA_ROOT
    if ($under -ne $h) {
        $upid = 0; [void][RgWin]::GetWindowThreadProcessId($under, [ref]$upid)
        $front = "$front; under the cursor: pid $upid ($((Get-Process -Id $upid -ErrorAction SilentlyContinue).ProcessName))"
    }
    [RgWin]::mouse_event(0x0002, 0, 0, 0, [IntPtr]::Zero)   # LEFTDOWN
    Start-Sleep -Milliseconds 150
    [RgWin]::mouse_event(0x0004, 0, 0, 0, [IntPtr]::Zero)   # LEFTUP
    Start-Sleep -Milliseconds 200
    return $front
}

function Restore-DropIn($st, [string]$KeepLogs) {
    Get-Process | Where-Object { $_.ProcessName -like "age2*" } | Stop-Process -Force -ErrorAction SilentlyContinue
    Start-Sleep 1
    $g = $st.GameDir
    $logs = Join-Path $g "netplay-logs"
    if (Test-Path $logs) {
        New-Item -ItemType Directory -Force $KeepLogs | Out-Null
        Move-Item -Force (Join-Path $logs "*") $KeepLogs
        Remove-Item -Force -Recurse $logs
    }
    foreach ($f in $script:DropInFiles + "dplayx-windows9x.dll") { Remove-Item -Force -ErrorAction SilentlyContinue (Join-Path $g $f) }
    if ($st.DdrawBackup -and (Test-Path $st.DdrawBackup)) { Move-Item -Force $st.DdrawBackup (Join-Path $g "ddraw.ini") }
    $left = $script:DropInFiles + "netplay-logs" + "ddraw.ini.realgame-backup" | Where-Object { Test-Path (Join-Path $g $_) }
    if ($left) { return "could not restore the game folder: $left" }
    return $null
}
