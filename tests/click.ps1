# Clicks the game window at a position given in window pixels (as in shot.ps1 screenshots), or drags from there:
#   powershell -File click.ps1 <x> <y> [<to x> <to y>]
# Brings the game to the front first. Used by in-game UI tests that need real mouse input.
param([int]$X, [int]$Y, [int]$ToX = -1, [int]$ToY = -1)
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
# The game follows the mouse through raw input, which SetCursorPos doesn't produce: nudge it with relative moves.
[Mouse]::SetCursorPos($r.Left + $X, $r.Top + $Y) | Out-Null
[Mouse]::mouse_event(0x1, 1, 0, 0, [UIntPtr]::Zero)
Start-Sleep -Milliseconds 50
[Mouse]::mouse_event(0x1, [uint32]4294967295, 0, 0, [UIntPtr]::Zero) # -1
Start-Sleep -Milliseconds 150
[Mouse]::mouse_event(0x2, 0, 0, 0, [UIntPtr]::Zero) # left down
Start-Sleep -Milliseconds 60
if ($ToX -ge 0) {
    # Absolute moves over the whole virtual desktop (MOVE | VIRTUALDESK | ABSOLUTE, normalized to 0..65535) reach
    # the game as mouse input; the game window may be on a monitor left of or above the primary one.
    Add-Type -AssemblyName System.Windows.Forms
    $vs = [System.Windows.Forms.SystemInformation]::VirtualScreen
    for ($i = 1; $i -le 20; $i++) {
        $px = $r.Left + $X + ($ToX - $X) * $i / 20; $py = $r.Top + $Y + ($ToY - $Y) * $i / 20
        $nx = [uint32](($px - $vs.Left) * 65535 / ($vs.Width - 1)); $ny = [uint32](($py - $vs.Top) * 65535 / ($vs.Height - 1))
        [Mouse]::mouse_event(0xC001, $nx, $ny, 0, [UIntPtr]::Zero)
        Start-Sleep -Milliseconds 40
    }
}
[Mouse]::mouse_event(0x4, 0, 0, 0, [UIntPtr]::Zero) # left up
