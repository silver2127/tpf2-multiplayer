# Capture the screen, or one window, to a PNG.
# BitBlt cannot capture OpenGL surfaces in exclusive fullscreen -- if the game
# area comes out black, run the game windowed. The reported black% is there so
# that failure is detectable without a human looking at the image.
param(
    [Parameter(Mandatory = $true)][string]$Path,
    [string]$Window = "",
    [int]$TargetPid = 0
)

Add-Type -AssemblyName System.Drawing
Add-Type -AssemblyName System.Windows.Forms
Add-Type @"
using System;
using System.Runtime.InteropServices;
public class Win32Rect {
    [StructLayout(LayoutKind.Sequential)]
    public struct RECT { public int Left, Top, Right, Bottom; }
    [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr hWnd, out RECT r);
    [DllImport("user32.dll")] public static extern bool SetProcessDpiAwarenessContext(IntPtr v);
}
"@ -ErrorAction SilentlyContinue

# Without this, GetWindowRect reports LOGICAL pixels while CopyFromScreen works
# in PHYSICAL ones. On a secondary monitor with different scaling the two
# disagree and the capture comes out shifted -- the game window appeared ~145px
# off inside its own screenshot, which made the menu look like it was somewhere
# it was not. -4 = DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2.
[void][Win32Rect]::SetProcessDpiAwarenessContext([IntPtr](-4))

$rect = $null
if ($Window) {
    $p = Get-Process -Name $Window -ErrorAction SilentlyContinue |
         Where-Object { $_.MainWindowHandle -ne 0 -and ($TargetPid -eq 0 -or $_.Id -eq $TargetPid) } |
         Select-Object -First 1
    if ($p) {
        $r = New-Object Win32Rect+RECT
        [void][Win32Rect]::GetWindowRect($p.MainWindowHandle, [ref]$r)
        $rect = New-Object System.Drawing.Rectangle $r.Left, $r.Top, ($r.Right - $r.Left), ($r.Bottom - $r.Top)
        Write-Host "window '$Window' pid=$($p.Id) rect=$($r.Left),$($r.Top) $($rect.Width)x$($rect.Height)"
    } else {
        Write-Host "no window for '$Window'$(if($TargetPid){" pid $TargetPid"}) - full screen"
    }
}
if (-not $rect) { $rect = [System.Windows.Forms.Screen]::PrimaryScreen.Bounds }

$bmp = New-Object System.Drawing.Bitmap $rect.Width, $rect.Height
$g = [System.Drawing.Graphics]::FromImage($bmp)
$g.CopyFromScreen($rect.Left, $rect.Top, 0, 0, $bmp.Size)
$g.Dispose()
$bmp.Save($Path, [System.Drawing.Imaging.ImageFormat]::Png)

$dark = 0; $total = 0
for ($y = 0; $y -lt $bmp.Height; $y += 20) {
    for ($x = 0; $x -lt $bmp.Width; $x += 20) {
        $c = $bmp.GetPixel($x, $y); $total++
        if ($c.R -lt 12 -and $c.G -lt 12 -and $c.B -lt 12) { $dark++ }
    }
}
$bmp.Dispose()
"saved $Path  ($($rect.Width)x$($rect.Height))  black={0:N0}%" -f (100 * $dark / [Math]::Max($total, 1))
