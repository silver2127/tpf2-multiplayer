<#
.SYNOPSIS
Click CONTINUE on games that are ALREADY running, and wait for the save to load.

.DESCRIPTION
autotest.ps1 does this as part of its own launch sequence, wired to the paths of
the old dev layout. This is the same click, standalone, for the case that comes
up constantly now: two instances are already up on the main menu (launched by
hand, or by the MSI proxy) and both need to load the newest save.

    tools\click_continue.ps1                 every running instance
    tools\click_continue.ps1 -Pids 9744,13376

CONTINUE loads the NEWEST save, so make sure the save you want is newest in each
instance's save folder (the sandboxed instance has its own).

The click is real: the game reads raw input, so PostMessage is ignored. Focus is
borrowed for ~600 ms per attempt and handed straight back, and a click is only
sent when the game's own window is the one at those coordinates -- never onto
whatever else has come to the front.
#>
[CmdletBinding()]
param(
    [int[]]$Pids = @(),
    [int]$TimeoutSeconds = 600
)
$ErrorActionPreference = "Stop"
function Say($m) { Write-Host ("[click] " + $m) -ForegroundColor Cyan }

Add-Type @"
using System;
using System.Runtime.InteropServices;
public class CC {
    [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, ref RECT r);
    [DllImport("user32.dll")] public static extern bool SetCursorPos(int x, int y);
    [DllImport("user32.dll")] public static extern void mouse_event(uint f, uint x, uint y, uint d, IntPtr e);
    [DllImport("user32.dll")] public static extern IntPtr GetForegroundWindow();
    [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
    [DllImport("user32.dll")] public static extern bool BringWindowToTop(IntPtr h);
    [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr h, int c);
    [DllImport("user32.dll")] public static extern void SwitchToThisWindow(IntPtr h, bool alt);
    [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, IntPtr p);
    [DllImport("user32.dll")] public static extern bool AttachThreadInput(uint a, uint b, bool f);
    [DllImport("kernel32.dll")] public static extern uint GetCurrentThreadId();
    [DllImport("user32.dll")] public static extern IntPtr WindowFromPoint(POINT p);
    [DllImport("user32.dll")] public static extern IntPtr GetAncestor(IntPtr h, uint flags);
    [DllImport("user32.dll")] public static extern bool GetCursorPos(out POINT p);
    [DllImport("user32.dll")] public static extern bool SetProcessDpiAwarenessContext(IntPtr v);
    [StructLayout(LayoutKind.Sequential)] public struct RECT { public int Left, Top, Right, Bottom; }
    [StructLayout(LayoutKind.Sequential)] public struct POINT { public int X, Y; }
    public static void Raise(IntPtr h) {
        uint me = GetCurrentThreadId();
        uint ft = GetWindowThreadProcessId(GetForegroundWindow(), IntPtr.Zero);
        AttachThreadInput(me, ft, true);
        ShowWindow(h, 9); BringWindowToTop(h); SetForegroundWindow(h);
        AttachThreadInput(me, ft, false);
        SwitchToThisWindow(h, true);
    }
    // Only click when the game's own window is what sits at that pixel. A stray
    // click on the user's browser is not an acceptable failure mode.
    public static bool ClickIfOnTarget(IntPtr expect, int x, int y) {
        POINT p; p.X = x; p.Y = y;
        IntPtr hit = WindowFromPoint(p);
        if (hit == IntPtr.Zero) return false;
        IntPtr root = GetAncestor(hit, 2);
        if (root != expect && hit != expect) return false;
        SetCursorPos(x, y); System.Threading.Thread.Sleep(70);
        mouse_event(0x0002,0,0,0,IntPtr.Zero); System.Threading.Thread.Sleep(50);
        mouse_event(0x0004,0,0,0,IntPtr.Zero);
        return true;
    }
}
"@ -ErrorAction SilentlyContinue

# Before any window query or cursor move: without it GetWindowRect and
# SetCursorPos speak LOGICAL pixels while the screen is PHYSICAL, and every
# coordinate on a scaled monitor is wrong by the scale factor. -4 = PER_MONITOR_V2.
[void][CC]::SetProcessDpiAwarenessContext([IntPtr](-4))

# Measured offsets of CONTINUE from the window origin, per window size. The menu
# is NOT laid out proportionally (UI scale steps with resolution), so these are
# measured. Kept in step with $ContinueFor in autotest.ps1.
$ContinueFor = @{
    "3856x2128" = @(349, 1110)
    "3840x2161" = @(348, 1108)
    "2420x1399" = @(236, 680)
    "1600x900"  = @(237, 682)
}

function Offset-For($w, $h) {
    $key = "${w}x${h}"
    if ($ContinueFor.ContainsKey($key)) { return $ContinueFor[$key] }
    $best = $null; $bestErr = 1e9
    foreach ($k in $ContinueFor.Keys) {
        $kw, $kh = $k.Split('x') | ForEach-Object { [int]$_ }
        $err = [math]::Abs($kw / $w - 1) + [math]::Abs($kh / $h - 1)
        if ($err -lt $bestErr) { $bestErr = $err; $best = $k }
    }
    if ($best -and $bestErr -lt 0.05) {
        $bw, $bh = $best.Split('x') | ForEach-Object { [int]$_ }
        $o = $ContinueFor[$best]
        Write-Host "[click] window ${w}x${h} unmeasured; scaling from $best" -ForegroundColor Yellow
        return @([int]($o[0] * $w / $bw), [int]($o[1] * $h / $bh))
    }
    return $null
}

$procs = Get-Process TransportFever2 -ErrorAction SilentlyContinue |
    Where-Object { $_.MainWindowHandle -ne 0 -and ($Pids.Count -eq 0 -or $Pids -contains $_.Id) }
if (-not $procs) { Write-Host "[click] no running game with a window"; exit 1 }

# A loaded save is ~2 GB resident; the menu is a few hundred MB. That gap is the
# signal both that a click is still needed and that the load finished.
$LOADED_MB = 1500
$targets = @()
foreach ($p in $procs) {
    $r = New-Object "CC+RECT"
    [void][CC]::GetWindowRect($p.MainWindowHandle, [ref]$r)
    $w = $r.Right - $r.Left; $h = $r.Bottom - $r.Top
    $off = Offset-For $w $h
    if (-not $off) {
        Write-Host "[click] pid $($p.Id): no CONTINUE offset for ${w}x${h} -- screenshot the menu and add it" -ForegroundColor Red
        continue
    }
    $targets += [pscustomobject]@{
        Proc = $p; H = $p.MainWindowHandle
        X = $r.Left + $off[0]; Y = $r.Top + $off[1]
        Size = "${w}x${h}"; Done = $false
    }
    Say "pid $($p.Id): window $("${w}x${h}") at $($r.Left),$($r.Top) -- CONTINUE at $($r.Left + $off[0]),$($r.Top + $off[1])"
}
if (-not $targets) { exit 1 }

$deadline = (Get-Date).AddSeconds($TimeoutSeconds)
while ((Get-Date) -lt $deadline) {
    $pending = $targets | Where-Object { -not $_.Done }
    if (-not $pending) { break }
    foreach ($t in $pending) {
        $mb = 0
        try { $mb = [int]((Get-Process -Id $t.Proc.Id -EA Stop).WorkingSet64 / 1MB) } catch { }
        if ($mb -gt $LOADED_MB) {
            $t.Done = $true
            Say "pid $($t.Proc.Id): in-game (${mb} MB)"
            continue
        }
        # Borrow focus for one click and hand it straight back, cursor included.
        $prevFg = [CC]::GetForegroundWindow()
        $pt = New-Object "CC+POINT"
        [void][CC]::GetCursorPos([ref]$pt)
        [CC]::Raise($t.H)
        Start-Sleep -Milliseconds 500
        $ok = [CC]::ClickIfOnTarget($t.H, $t.X, $t.Y)
        if ($prevFg -ne [IntPtr]::Zero -and $prevFg -ne $t.H) {
            [CC]::Raise($prevFg)
            [void][CC]::SetCursorPos($pt.X, $pt.Y)
        }
        if (-not $ok) { Say "pid $($t.Proc.Id): skipped -- another window covers the click point" }
    }
    Start-Sleep -Seconds 5
}

$loaded = ($targets | Where-Object { $_.Done }).Count
Say "$loaded of $($targets.Count) instance(s) in-game"
if ($loaded -lt $targets.Count) { exit 1 }
