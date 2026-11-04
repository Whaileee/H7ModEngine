# Clicks the game window at a position given in window pixels (as in shot.ps1 screenshots):
#   powershell -File click.ps1 <x> <y>
# Brings the game to the front first. Used by in-game UI tests that need real mouse input.
param([int]$X, [int]$Y)
Add-Type @"
using System;
using System.Runtime.InteropServices;
public static class Mouse {
    [StructLayout(LayoutKind.Sequential)] public struct RECT { public int Left, Top, Right, Bottom; }
    [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr hWnd, out RECT rect);
    [DllImport("user32.dll")] public static extern bool SetProcessDPIAware();
    [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr hWnd);
    [DllImport("user32.dll")] public static extern bool SetCursorPos(int x, int y);
    [DllImport("user32.dll")] public static extern void mouse_event(uint flags, uint dx, uint dy, uint data, UIntPtr extra);
}
"@
[Mouse]::SetProcessDPIAware() | Out-Null
$game = Get-Process MMH7Game-Win64-Shipping -ErrorAction Stop | Where-Object { $_.MainWindowHandle -ne 0 } | Select-Object -First 1
$r = New-Object Mouse+RECT
[Mouse]::GetWindowRect($game.MainWindowHandle, [ref]$r) | Out-Null
[Mouse]::SetForegroundWindow($game.MainWindowHandle) | Out-Null
Start-Sleep -Milliseconds 300
[Mouse]::SetCursorPos($r.Left + $X, $r.Top + $Y) | Out-Null
Start-Sleep -Milliseconds 150
[Mouse]::mouse_event(0x2, 0, 0, 0, [UIntPtr]::Zero) # left down
Start-Sleep -Milliseconds 60
[Mouse]::mouse_event(0x4, 0, 0, 0, [UIntPtr]::Zero) # left up
