# Types keys into the game window: powershell -File keys.ps1 <key> [<key> ...]
# Keys: 0-9, BACK, RETURN, ESCAPE, PERIOD, MINUS (virtual-key names). Brings the game to the front first.
param([Parameter(ValueFromRemainingArguments = $true)][string[]]$Keys)
Add-Type @"
using System;
using System.Runtime.InteropServices;
public static class Kbd {
    [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr hWnd);
    [DllImport("user32.dll")] public static extern uint MapVirtualKey(uint code, uint type);
    [DllImport("user32.dll")] public static extern void keybd_event(byte vk, byte scan, uint flags, UIntPtr extra);
}
"@
$vk = @{ BACK = 0x08; RETURN = 0x0D; ESCAPE = 0x1B; PERIOD = 0xBE; MINUS = 0xBD }
$game = Get-Process MMH7Game-Win64-Shipping -ErrorAction Stop | Where-Object { $_.MainWindowHandle -ne 0 } | Select-Object -First 1
[Kbd]::SetForegroundWindow($game.MainWindowHandle) | Out-Null
Start-Sleep -Milliseconds 200
foreach ($k in $Keys) {
    $code = if ($vk.ContainsKey($k)) { $vk[$k] } elseif ($k.Length -eq 1) { [int][char]$k.ToUpper() } else { throw "unknown key $k" }
    $scan = [byte][Kbd]::MapVirtualKey([uint32]$code, 0)
    [Kbd]::keybd_event([byte]$code, $scan, 0, [UIntPtr]::Zero)
    Start-Sleep -Milliseconds 60
    [Kbd]::keybd_event([byte]$code, $scan, 2, [UIntPtr]::Zero)
    Start-Sleep -Milliseconds 80
}
