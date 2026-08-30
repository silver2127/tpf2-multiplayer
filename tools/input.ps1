# Synthetic mouse/keyboard input, for driving the game with no human present.
#   input.ps1 -Focus TransportFever2 -TargetPid 1234
#   input.ps1 -Click 341,1102
#   input.ps1 -Key "{ESC}"
param(
    [string]$Click = "",
    [string]$DoubleClick = "",
    [string]$Move = "",
    [string]$Key = "",
    [string]$Text = "",
    [string]$Focus = "",
    [int]$TargetPid = 0
)

Add-Type @"
using System;
using System.Runtime.InteropServices;
public class Inp {
    [DllImport("user32.dll")] public static extern bool SetCursorPos(int x, int y);
    [DllImport("user32.dll")] public static extern void mouse_event(uint f, uint x, uint y, uint d, IntPtr e);
    [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
    [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr h, int c);
    [DllImport("user32.dll")] public static extern void SwitchToThisWindow(IntPtr h, bool altTab);
    [DllImport("user32.dll")] public static extern IntPtr GetForegroundWindow();
    [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, IntPtr pid);
    [DllImport("kernel32.dll")] public static extern uint GetCurrentThreadId();
    [DllImport("user32.dll")] public static extern bool AttachThreadInput(uint a, uint b, bool attach);
    [DllImport("user32.dll")] public static extern bool BringWindowToTop(IntPtr h);
    [DllImport("user32.dll")] public static extern bool SetProcessDpiAwarenessContext(IntPtr v);
    // Windows refuses SetForegroundWindow from a background process; attaching
    // to the current foreground thread's input queue makes the call legal.
    public static void ForceForeground(IntPtr h) {
        IntPtr fg = GetForegroundWindow();
        uint fgThread = GetWindowThreadProcessId(fg, IntPtr.Zero);
        uint me = GetCurrentThreadId();
        AttachThreadInput(me, fgThread, true);
        ShowWindow(h, 9);
        BringWindowToTop(h);
        SetForegroundWindow(h);
        AttachThreadInput(me, fgThread, false);
        SwitchToThisWindow(h, true);
    }
    public const uint LEFTDOWN = 0x0002, LEFTUP = 0x0004;
    public static void ClickAt(int x, int y) {
        SetCursorPos(x, y);
        System.Threading.Thread.Sleep(60);
        mouse_event(LEFTDOWN, 0, 0, 0, IntPtr.Zero);
        System.Threading.Thread.Sleep(40);
        mouse_event(LEFTUP, 0, 0, 0, IntPtr.Zero);
    }
}
"@ -ErrorAction SilentlyContinue

# Coordinates are PHYSICAL pixels. Without this, SetCursorPos on a scaled
# monitor lands at the wrong place by exactly the scale factor.
# -4 = DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2.
[void][Inp]::SetProcessDpiAwarenessContext([IntPtr](-4))

if ($Focus) {
    $p = Get-Process -Name $Focus -ErrorAction SilentlyContinue |
         Where-Object { $_.MainWindowHandle -ne 0 -and ($TargetPid -eq 0 -or $_.Id -eq $TargetPid) } |
         Select-Object -First 1
    if ($p) {
        [Inp]::ForceForeground($p.MainWindowHandle)
        Start-Sleep -Milliseconds 700
        $ok = ([Inp]::GetForegroundWindow() -eq $p.MainWindowHandle)
        "focused $Focus pid=$($p.Id) foreground=$ok"
    } else { "no window for $Focus$(if($TargetPid){" pid $TargetPid"})" }
}
if ($Move)  { $c = $Move -split ','  ; [void][Inp]::SetCursorPos([int]$c[0], [int]$c[1]); "moved $Move" }
if ($Click) { $c = $Click -split ','; [Inp]::ClickAt([int]$c[0], [int]$c[1]); "clicked $Click" }
if ($DoubleClick) {
    $c = $DoubleClick -split ','
    [Inp]::ClickAt([int]$c[0], [int]$c[1]); Start-Sleep -Milliseconds 80
    [Inp]::ClickAt([int]$c[0], [int]$c[1]); "double-clicked $DoubleClick"
}
if ($Key)  { Add-Type -AssemblyName System.Windows.Forms; [System.Windows.Forms.SendKeys]::SendWait($Key); "key $Key" }
if ($Text) { Add-Type -AssemblyName System.Windows.Forms; [System.Windows.Forms.SendKeys]::SendWait($Text); "typed" }
