<#
.SYNOPSIS
Launch both instances, load the save in each, run a replication scenario and
report -- with no human at the keyboard.

.DESCRIPTION
Encodes the things that make GUI automation of this game work, each of which
cost a debugging round to find:

  * Windows refuses SetForegroundWindow from a background process. Attaching to
    the current foreground thread's input queue makes the call legal.
  * The first click on an unfocused window only activates it; the click does
    not reach the app. Always focus, then click.
  * The secondary monitors here run a different resolution/DPI to the primary,
    so absolute clicks land in the wrong place. Both windows are therefore
    MOVED to the primary monitor at a known size, and menu hits are computed as
    fixed offsets from the window origin.
  * The sandbox must be terminated before launching, or the boxed game exits
    immediately (a second Steam ends up running in the box).

.PARAMETER Scenario
Scenario file for the host to execute. Default: scenarios/basic_replication.txt

.PARAMETER KeepOpen
Leave both games running at the end (default closes them).

.PARAMETER SkipLaunch
Use the instances that are already running rather than restarting them.
#>
[CmdletBinding()]
param(
    [string]$Scenario = "",
    [switch]$KeepOpen,
    [switch]$SkipLaunch,
    # Run the scenario only from the unsandboxed side. The default runs it from
    # BOTH sides, because a->b passing tells you nothing about b->a and that is
    # the direction real-play bugs have kept appearing in.
    [switch]$OneWay,
    # Launch both instances, load the save in each, and STOP -- no replication
    # readiness wait, no scenario, no verdict. For experiments that are not
    # about replication at all, notably the M3 determinism run, where mp_bridge
    # is deliberately not installed so there is no Lua mod to wait for.
    [switch]$LaunchOnly,
    # Terminate the whole Sandboxie box before launching, taking sandboxed Steam
    # with it. Off by default because that Steam then cold-starts every run and
    # it is slow. Use it when the box is genuinely wedged.
    [switch]$HardReset,
    [int]$WaitSeconds = 90
)

$ErrorActionPreference = "Stop"
$T      = $PSScriptRoot
$Exe    = "C:\Program Files (x86)\Steam\steamapps\common\Transport Fever 2\TransportFever2.exe"
$Sbie   = "C:\Program Files\Sandboxie-Plus\Start.exe"
$Box    = "GameAgent"
$Out    = "C:\Program Files (x86)\Steam\steamapps\workshop\content\1066780\3710243057\recon\m4\out"
$Ovl    = "C:\Sandbox\$env:USERNAME\$Box\drive\C" + $Out.Substring(2)
$Shots  = Join-Path $T "shots"
if (-not (Test-Path $Shots)) { New-Item -ItemType Directory -Path $Shots | Out-Null }
if (-not $Scenario) { $Scenario = Join-Path $T "scenarios\basic_replication.txt" }

# Windows are used at whatever size they open -- see the warning in
# Place-And-Continue about why resizing must not happen. Per-size CONTINUE
# offsets live in $ContinueFor further down.
$StdoutA = "C:\Program Files (x86)\Steam\userdata\*\1066780\local\crash_dump\stdout.txt"
$StdoutB = "C:\Sandbox\$env:USERNAME\$Box\drive\C\Program Files (x86)\Steam\userdata\*\1066780\local\crash_dump\stdout.txt"
function ResolveStdout($pattern) {
    $f = Get-ChildItem $pattern -EA SilentlyContinue | Select-Object -First 1
    if ($f) { return $f.FullName } else { return $null }
}

Add-Type -AssemblyName System.Windows.Forms
Add-Type @"
using System;
using System.Runtime.InteropServices;
public class AT {
    [StructLayout(LayoutKind.Sequential)]
    public struct RECT { public int Left, Top, Right, Bottom; }
    [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
    [DllImport("user32.dll")] public static extern bool SetProcessDpiAwarenessContext(IntPtr v);
    [DllImport("user32.dll")] public static extern bool SetCursorPos(int x, int y);
    [DllImport("user32.dll")] public static extern void mouse_event(uint f, uint x, uint y, uint d, IntPtr e);
    [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
    [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr h, int c);
    [DllImport("user32.dll")] public static extern void SwitchToThisWindow(IntPtr h, bool alt);
    [DllImport("user32.dll")] public static extern IntPtr GetForegroundWindow();
    [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, IntPtr p);
    [DllImport("kernel32.dll")] public static extern uint GetCurrentThreadId();
    [DllImport("user32.dll")] public static extern bool AttachThreadInput(uint a, uint b, bool at);
    [DllImport("user32.dll")] public static extern bool BringWindowToTop(IntPtr h);
    [DllImport("user32.dll")] public static extern bool SetWindowPos(IntPtr h, IntPtr a, int x, int y, int cx, int cy, uint f);
    public static void Raise(IntPtr h) {
        IntPtr fg = GetForegroundWindow();
        uint ft = GetWindowThreadProcessId(fg, IntPtr.Zero), me = GetCurrentThreadId();
        AttachThreadInput(me, ft, true);
        ShowWindow(h, 9); BringWindowToTop(h); SetForegroundWindow(h);
        AttachThreadInput(me, ft, false);
        SwitchToThisWindow(h, true);
    }
    public static void Click(int x, int y) {
        SetCursorPos(x, y); System.Threading.Thread.Sleep(70);
        mouse_event(0x0002,0,0,0,IntPtr.Zero); System.Threading.Thread.Sleep(50);
        mouse_event(0x0004,0,0,0,IntPtr.Zero);
    }
    [DllImport("user32.dll")] public static extern IntPtr WindowFromPoint(POINT p);
    [DllImport("user32.dll")] public static extern IntPtr GetAncestor(IntPtr h, uint flags);
    [StructLayout(LayoutKind.Sequential)] public struct POINT { public int X, Y; }
    [DllImport("user32.dll")] public static extern bool GetCursorPos(out POINT p);

    // Synthetic clicks go to whatever window is on top AT THAT POINT, not to a
    // process. If anything has come to the front -- a browser, a chat popup, a
    // meeting window -- the click lands THERE. A stray click on someone's
    // desktop is not an acceptable failure mode for a test harness.
    //
    // Ask the precise question: what window is actually at these coordinates?
    // The earlier version compared against GetForegroundWindow(), which is only
    // a proxy and is wrong in both directions -- it blocked indefinitely on a
    // titleless GameInputSvc window that held focus while sitting nowhere near
    // the click point, and it would happily click a window that had focus but
    // was occluded at that pixel.
    public static bool ClickIfOnTarget(IntPtr expect, int x, int y) {
        POINT p; p.X = x; p.Y = y;
        IntPtr hit = WindowFromPoint(p);
        if (hit == IntPtr.Zero) return false;
        IntPtr root = GetAncestor(hit, 2 /* GA_ROOT */);
        if (root != expect && hit != expect) return false;
        Click(x, y);
        return true;
    }
}
"@ -ErrorAction SilentlyContinue

# Must run before any window query or cursor move. Without it GetWindowRect and
# SetCursorPos speak LOGICAL pixels while the screen is PHYSICAL, so on a
# monitor with non-100% scaling every coordinate is wrong by the scale factor --
# the sandboxed instance sits on a 125% display and reported 1936x1119 for a
# window that is really 2420x1399. -4 = PER_MONITOR_AWARE_V2.
[void][AT]::SetProcessDpiAwarenessContext([IntPtr](-4))

function Say($m) { Write-Host ("[autotest] " + $m) -ForegroundColor Cyan }
function Fail($m) { Write-Host ("[autotest] FAIL: " + $m) -ForegroundColor Red; exit 1 }

function Get-GameProc([int]$ThePid) {
    Get-Process TransportFever2 -ErrorAction SilentlyContinue |
        Where-Object { $_.MainWindowHandle -ne 0 -and ($ThePid -eq 0 -or $_.Id -eq $ThePid) } |
        Select-Object -First 1
}

function Wait-Window([int]$ThePid, [int]$Timeout = 180) {
    for ($i = 0; $i -lt $Timeout; $i += 3) {
        if (Get-GameProc $ThePid) { return $true }
        Start-Sleep -Seconds 3
    }
    return $false
}

# Where CONTINUE sits, measured from the window origin, per window size. The
# menu is NOT laid out proportionally -- the UI scale steps with resolution --
# so these are measured, not computed.
# Sizes are PHYSICAL pixels (the process is DPI-aware, see above).
$ContinueFor = @{
    "3856x2128" = @(349, 1110)   # instance A, primary display
    "3840x2161" = @(348, 1108)   # instance A, borderless (menu is top-anchored: same Y as 3856, not scaled)
    "2420x1399" = @(236, 680)    # instance B, sandboxed, 125%-scaled secondary
    "1600x900"  = @(237, 682)
}

function Get-Rect([IntPtr]$h) {
    $r = New-Object AT+RECT
    [void][AT]::GetWindowRect($h, [ref]$r)
    return @{ X = $r.Left; Y = $r.Top; W = $r.Right - $r.Left; H = $r.Bottom - $r.Top }
}

function Place-And-Continue([int]$ThePid, [string]$Tag, [string]$StdoutPath) {
    $p = Get-GameProc $ThePid
    if (-not $p) { Fail "no window for pid $ThePid" }
    $h = $p.MainWindowHandle

    # DO NOT RESIZE THE WINDOW. Verified the hard way: a game sitting on a fully
    # drawn main menu, resized with SetWindowPos, drops straight back to a
    # logo-only screen with no menu at all -- indistinguishable from a hang, and
    # it cost a long detour diagnosing a "wedged" game that was only ever
    # showing the damage from this call. Take the window at whatever size it
    # opens and look the offset up instead.
    $r = Get-Rect $h
    $key = "$($r.W)x$($r.H)"
    $off = $ContinueFor[$key]
    if (-not $off) {
        # No exact measurement. The menu is not proportional across UI-scale
        # STEPS, but a window that differs by a few pixels (frame/DPI rounding:
        # 3840x2161 vs the measured 3856x2128) is the same layout, so scale
        # the nearest measured size's offset when the size is within 5%.
        $best = $null; $bestErr = 1e9
        foreach ($k in $ContinueFor.Keys) {
            $kw, $kh = $k.Split('x') | ForEach-Object { [int]$_ }
            $err = [math]::Abs($kw / $r.W - 1) + [math]::Abs($kh / $r.H - 1)
            if ($err -lt $bestErr) { $bestErr = $err; $best = $k }
        }
        if ($best -and $bestErr -lt 0.05) {
            $bw, $bh = $best.Split('x') | ForEach-Object { [int]$_ }
            $o = $ContinueFor[$best]
            $off = @([int]($o[0] * $r.W / $bw), [int]($o[1] * $r.H / $bh))
            Say "$Tag : window $key has no measured offset; scaling $best -> ($($off[0]),$($off[1]))" Yellow
        } else {
            Fail ("$Tag : no measured CONTINUE offset for window size $key. Screenshot " +
                  "the menu, read off the position, and add it to `$ContinueFor.")
        }
    }
    $x = $r.X + $off[0]
    $y = $r.Y + $off[1]

    # "Loading from file <SAVE>" in the game's own stdout is the authoritative
    # signal that CONTINUE was accepted -- far better than watching the working
    # set, which also creeps up for unrelated reasons.
    $mark = if ($StdoutPath -and (Test-Path $StdoutPath)) { (Get-Item $StdoutPath).Length } else { 0 }
    function NewStdout {
        if (-not $StdoutPath -or -not (Test-Path $StdoutPath)) { return "" }
        $fs = [System.IO.File]::Open($StdoutPath, 'Open', 'Read', 'ReadWrite')
        [void]$fs.Seek([Math]::Min($mark, $fs.Length), 'Begin')
        $sr = New-Object System.IO.StreamReader($fs)
        # NOT $t: that differs from the script-scope $T (the tools dir) only in
        # case, and PowerShell treats those as one name. It is function-local so
        # it happens to be harmless here, but it shadows $T for the rest of this
        # body -- exactly the shape that already broke $Ovl. See tools/pscheck.ps1.
        $text = $sr.ReadToEnd(); $sr.Close(); $fs.Close(); return $text
    }

    # The window exists well before the menu is drawn, and the first launch
    # after a deploy prints "Mods changed, recreating data..." and rebuilds the
    # whole asset cache first -- minutes, not seconds. So retry: a click on the
    # still-animating title screen does nothing, and once the entries render one
    # of them takes.
    Say "$Tag : window $key at $($r.X),$($r.Y) -- clicking CONTINUE at $x,$y (will retry)"
    $blocked = 0
    for ($i = 0; $i -lt 120; $i++) {
      try {
        if ((NewStdout) -match 'Loading from file (\S+)') {
            Say "$Tag : loading '$($Matches[1])'"
            for ($j = 0; $j -lt 60; $j++) {
                Start-Sleep -Seconds 5
                $mb = [int]((Get-Process -Id $ThePid -EA SilentlyContinue).WorkingSet64 / 1MB)
                if ($mb -gt 1500) { Say "$Tag : save loaded (${mb} MB)"; return $true }
            }
            return $false
        }
        # Bail out if the game is already in-game (big working set): a missed
        # 'Loading from file' line (stdout rotation) once left this loop
        # clicking for minutes and un-fullscreening the user's windows.
        $mbNow = [int]((Get-Process -Id $ThePid -EA SilentlyContinue).WorkingSet64 / 1MB)
        if ($mbNow -gt 1500) { Say "$Tag : already in-game (${mbNow} MB) -- no clicks needed"; return $true }
        # Be polite about focus: remember what the user had in front and where
        # their cursor was, do the one click, and put BOTH back immediately.
        # (PostMessage clicks are ignored -- the game reads raw input only --
        # so a real click is unavoidable; stealing focus for 600 ms once every
        # few seconds is the least-bad version of it.)
        $prevFg = [AT]::GetForegroundWindow()
        $pt = New-Object "AT+POINT"
        [void][AT]::GetCursorPos([ref]$pt)
        [AT]::Raise($h)
        Start-Sleep -Milliseconds 500
        $clickOk = [AT]::ClickIfOnTarget($h, $x, $y)
        if ($prevFg -ne [IntPtr]::Zero -and $prevFg -ne $h) {
            [AT]::Raise($prevFg)
            [void][AT]::SetCursorPos($pt.X, $pt.Y)
        }
        Start-Sleep -Milliseconds 2500
        if (-not $clickOk) {
            $blocked++
            if ($blocked -eq 1 -or $blocked % 10 -eq 0) {
                Say "$Tag : SKIPPED click -- another window occupies $x,$y ($blocked so far)"
            }
        }
        Start-Sleep -Seconds 5
      } catch {
        # A transient Win32/file exception (a momentary null process during load,
        # a locked stdout file) must NOT abort the whole launch under
        # $ErrorActionPreference=Stop -- it did, exiting 255 after A had actually
        # loaded. Swallow and keep polling; the load check catches up next pass.
        Say "$Tag : transient continue-loop error ($($_.Exception.Message)) -- retrying"
      }
    }
    return $false
}

# ---------------------------------------------------------------- launch
if (-not $SkipLaunch) {
    # Must happen before launch: the game reads the Lua once at startup, so a
    # deploy after this point would test the previous build.
    Say "deploying mod from repo"
    & (Join-Path $T "deploy_mod.ps1") -Quiet

    # Kill the GAMES, not the box.
    #
    # This used to run `Start.exe /box:<name> /terminate`, which takes the
    # sandboxed STEAM down with it -- and that Steam then cold-starts on every
    # single run, costing minutes of waiting for nothing. Instance B is launched
    # directly through Start.exe with the exe path (see below), not through
    # Steam, so a warm Steam in the box is something to preserve, not clear.
    #
    # The old header claimed the box had to be terminated or the boxed game
    # exits immediately with a second Steam in the box. That is a symptom of a
    # STALE box, not of a healthy one: leaving a live Steam in place means the
    # relaunched game attaches to it instead of starting a duplicate. If a game
    # process does refuse to die, fall back to the big hammer rather than
    # launching on top of a half-dead instance.
    Say "stopping the games (leaving sandboxed Steam running)"
    if ($HardReset) {
        Say "  -HardReset: terminating the whole box, Steam included"
        & $Sbie "/box:$Box" /terminate 2>$null | Out-Null
        Get-Process TransportFever2 -ErrorAction SilentlyContinue | Stop-Process -Force
        Start-Sleep -Seconds 6
    } else {
        Get-Process TransportFever2 -ErrorAction SilentlyContinue | Stop-Process -Force
        Start-Sleep -Seconds 4
        $stubborn = @(Get-Process TransportFever2 -EA SilentlyContinue | ForEach-Object Id)
        if ($stubborn.Count -gt 0) {
            Say "  $($stubborn.Count) game process(es) survived; terminating the box as a fallback"
            & $Sbie "/box:$Box" /terminate 2>$null | Out-Null
            Start-Sleep -Seconds 6
        }
    }

    # Launch through Steam, not the exe directly. Steam sets up the environment
    # the game expects; this is also how a player starts it.
    Say "launching instance A (via Steam)"
    $before = @(Get-Process TransportFever2 -ErrorAction SilentlyContinue | ForEach-Object Id)
    Start-Process "steam://rungameid/1066780"
    if (-not (Wait-Window 0)) { Fail "instance A never opened a window" }
    $pidA = (Get-Process TransportFever2 | Where-Object { $before -notcontains $_.Id } | Select-Object -First 1).Id
    if (-not $pidA) { $pidA = (Get-GameProc 0).Id }
    Say "A pid=$pidA"
    if (-not (Place-And-Continue $pidA "A" (ResolveStdout $StdoutA))) { Fail "instance A did not load the save" }

    Say "launching instance B (sandboxed)"
    $before = @(Get-Process TransportFever2 -ErrorAction SilentlyContinue | ForEach-Object Id)
    Start-Process -FilePath $Sbie -ArgumentList "/box:$Box","`"$Exe`"" -WorkingDirectory (Split-Path $Exe)
    for ($i = 0; $i -lt 60; $i++) {
        Start-Sleep -Seconds 5
        $new = Get-Process TransportFever2 -ErrorAction SilentlyContinue |
               Where-Object { $before -notcontains $_.Id -and $_.MainWindowHandle -ne 0 }
        if ($new) { break }
    }
    if (-not $new) { Fail "instance B never opened a window" }
    $pidB = $new.Id
    Say "B pid=$pidB"
    if (-not (Place-And-Continue $pidB "B" (ResolveStdout $StdoutB))) { Fail "instance B did not load the save" }
} else {
    $ps = @(Get-Process TransportFever2 -ErrorAction SilentlyContinue)
    if ($ps.Count -lt 2) { Fail "-SkipLaunch needs two instances already running" }
    Say "using running instances: $($ps.Id -join ', ')"
}

# Each bridge writes "<identity>`n pid=<pid>" to tpf2_instance.txt in its own
# view of the out dir. That is a far better handle than process order: it tells
# us which side is 'a' and which is 'b' even after an identity swap, and it
# works whether or not this script did the launching.
function Read-Identity($dir) {
    $f = Join-Path $dir "tpf2_instance.txt"
    if (-not (Test-Path $f)) { return $null }
    $l = @(Get-Content $f -EA SilentlyContinue)
    if ($l.Count -lt 1) { return $null }
    $thePid = 0
    if ($l.Count -ge 2 -and $l[1] -match 'pid=(\d+)') { $thePid = [int]$Matches[1] }
    return [pscustomobject]@{ Id = $l[0].Trim(); Pid = $thePid; Dir = $dir }
}

# ------------------------------------------------------------- readiness
Say "waiting for both bridges to see each other"
$ready = $false
for ($i = 0; $i -lt 40; $i++) {
    Start-Sleep -Seconds 5
    $a = Get-Content "$Out\tpf2_bridge.log" -Tail 20 -EA SilentlyContinue | Select-String 'ticks=' | Select-Object -Last 1
    $b = Get-Content "$Ovl\tpf2_bridge.log" -Tail 20 -EA SilentlyContinue | Select-String 'ticks=' | Select-Object -Last 1
    if ($a -match 'peer=up' -and $b -match 'peer=up') { $ready = $true; break }
}
if (-not $ready) { Say "WARNING: peers never both reported up; continuing anyway" }

# NAMING: do NOT call these $real / $ovl. PowerShell variable names are
# case-INSENSITIVE, so `$ovl = Read-Identity $Ovl` silently overwrites $Ovl --
# the sandbox out DIRECTORY defined at the top -- with a pscustomobject. Every
# later use of $Ovl as a path then broke: Get-SimTicks handed the object to
# Join-Path and died with DriveNotFoundException, killing the run right after
# the verdict and taking the B-side wedge check with it.
$idReal = Read-Identity $Out
$idOvl  = Read-Identity $Ovl
if (-not $idReal -or -not $idOvl) { Fail "could not read both identity files (is the mod loaded in both?)" }
Say "identities -- real: '$($idReal.Id)' pid=$($idReal.Pid) | overlay: '$($idOvl.Id)' pid=$($idOvl.Pid)"
if ($idReal.Id -eq $idOvl.Id) { Fail "both instances claim identity '$($idReal.Id)' -- port probe failed" }

# run_mptest resolves -Target back to a directory itself, so hand it the
# identity of whichever side is unsandboxed and let it do the mapping.
$hostInst = $idReal.Id
Say "host instance is '$hostInst'"

# The bridge DLLs peer during LOADING -- they are injected long before the Lua
# game script starts -- so peer=up is not the same as "ready to replicate".
# A scenario run in that window is lost for good: the joiner's mod primes its
# read offset to the CURRENT end of the events file on startup, so lines that
# arrived before it woke up are skipped, not replayed. That produced a run where
# the joiner's events file held every line and reported consumed=0.
# Wait for the joiner's Lua to actually be ticking before running anything.
if ($LaunchOnly) {
    Say "-LaunchOnly: both instances are up with the save loaded; stopping here"
    Say "  A: $($idReal.Id) pid=$($idReal.Pid)   B: $($idOvl.Id) pid=$($idOvl.Pid)"
    Say "  (no replication wait, no scenario -- nothing will touch these worlds)"
    exit 0
}

Say "waiting for the joiner's mod to start ticking"
$joinLive = $false
$jstdout = ResolveStdout $StdoutB
for ($i = 0; $i -lt 60; $i++) {
    if ($jstdout -and (Test-Path $jstdout)) {
        $fs = [System.IO.File]::Open($jstdout, 'Open', 'Read', 'ReadWrite')
        $sr = New-Object System.IO.StreamReader($fs)
        $txt = $sr.ReadToEnd(); $sr.Close(); $fs.Close()
        # pollEvents is logged every sweep, so seeing it at all means update() runs
        if ($txt -match 'pollEvents') { $joinLive = $true; break }
    }
    Start-Sleep -Seconds 5
}
if ($joinLive) { Say "joiner mod is live" }
else { Say "WARNING: never saw the joiner's mod tick; results may be lost in transit" }

# ------------------------------------------------------------------ test
# Re-running a scenario on ground a previous run already built on fails every
# action: buildConstruction trips `!proposalData.errorState.critical' and the
# edges collide with the old ones. So each run is shifted onto virgin ground by
# an offset that advances and persists across runs.
$stateFile = Join-Path $T ".autotest_offset"
$offset = 0
if (Test-Path $stateFile) { $offset = [int](Get-Content $stateFile -Raw).Trim() }

function New-ShiftedScenario($srcPath, [int]$offset, $destPath) {
$lines = Get-Content $srcPath | ForEach-Object {
    # Shift the X of every coordinate pair. Ops differ in how many leading
    # tokens are coordinates and whether anything non-numeric follows:
    #   DEPOT x y                      -> 2
    #   ROAD/RAIL/DELROAD/DELRAIL      -> 4
    #   CONEDIT x y key value          -> 2, then arbitrary text
    # An earlier version required the whole argument list to be numeric, so
    # CONEDIT was silently left unshifted while the DEPOT it referred to moved
    # 2800 m away -- the edit then found nothing and the test looked like a
    # product failure rather than a harness bug.
    $coordCount = @{ DEPOT = 2; STATION = 2; CONEDIT = 2
                     ROAD = 4; RAIL = 4; DELROAD = 4; DELRAIL = 4 }
    if ($_ -match '^\s*([A-Z]+)\s+(.*)$' -and $coordCount.ContainsKey($Matches[1])) {
        $cmd  = $Matches[1]
        $rest = $Matches[2] -split '\s+'
        $want = $coordCount[$cmd]
        $okNums = $rest.Count -ge $want
        for ($i = 0; $i -lt $want -and $okNums; $i++) {
            if ($rest[$i] -notmatch '^-?\d+$') { $okNums = $false }
        }
        if ($okNums) {
            # X is every even index within the coordinate prefix
            for ($i = 0; $i -lt $want; $i += 2) { $rest[$i] = [string]([int]$rest[$i] + $offset) }
            "$cmd $($rest -join ' ')"
        } else { $_ }
    } else { $_ }
}
    Set-Content -Path $destPath -Value $lines -Encoding ASCII
    Say "scenario shifted +$offset in X onto virgin ground -> $destPath"
    return $destPath
}

# NOTE: reading the peer's stdout for replay outcomes now lives in
# run_mptest.ps1, which knows which side is the peer for the pass it is running.
# It used to be done here against the SANDBOX stdout unconditionally, which is
# only correct for a -> b.

# Watchdog. Some engine commands do not fail, they wedge the process -- a
# TransportVehicleConfig whose vehicleGroups do not sum to the vehicle count
# hangs the sim, and it is a C++ assert, invisible to pcall. Snapshot who is
# alive now so a hang can be attributed and cleaned up instead of leaving a
# stuck game (and a stuck sandbox) behind.
$watchPids = @(Get-Process TransportFever2 -EA SilentlyContinue | ForEach-Object Id)

# Run the scenario in BOTH directions unless told otherwise.
#
# For most of this project the harness only ever drove the unsandboxed side and
# only ever asserted that the sandboxed side received. That is half the product:
# the joiner replicating back to the host was never exercised here, and every
# fault found in it was found by a human playing the game. Worse, a one-way pass
# reads as "replication works".
#
# Each pass gets its own X offset so the second one builds on virgin ground
# rather than on top of what the first pass just replicated.
$passes = @()
$offset += 200
$passes += ,@($hostInst, (New-ShiftedScenario $Scenario $offset (Join-Path $Shots "scenario_run_$hostInst.txt")))
if (-not $OneWay) {
    $peerInst = if ($hostInst -eq "a") { "b" } else { "a" }
    $offset += 200
    $passes += ,@($peerInst, (New-ShiftedScenario $Scenario $offset (Join-Path $Shots "scenario_run_$peerInst.txt")))
}
Set-Content -Path $stateFile -Value $offset -Encoding ASCII

$passStats = @()
foreach ($pass in $passes) {
    $who, $file = $pass[0], $pass[1]
    Say "running scenario: $(Split-Path $Scenario -Leaf)  [direction $who -> other]"
    # Snapshot who is alive BEFORE the pass, so a crash during it is attributed
    # to this pass rather than noticed later by the watchdog -- or not at all.
    $aliveBefore = @(Get-Process TransportFever2 -EA SilentlyContinue | ForEach-Object Id)
    $runOut = & (Join-Path $T "run_mptest.ps1") -Scenario $file -Target $who -WaitSeconds $WaitSeconds *>&1
    $runOut | ForEach-Object { $_ }
    $rc = $LASTEXITCODE

    # An instance that DIED during the pass makes the counts meaningless, and
    # they can still look perfect: a scenario that ships its lines and then
    # crashes the actor leaves shipped==got==replayed, so this printed
    # "RESULT: PASS" for a run whose actor was face down. Measured exactly that
    # -- road_intersection killed A at the mid-span junction step and the a->b
    # pass was still reported as a pass.
    $aliveAfter = @(Get-Process TransportFever2 -EA SilentlyContinue | ForEach-Object Id)
    $died = @($aliveBefore | Where-Object { $aliveAfter -notcontains $_ })
    if ($died.Count -gt 0) {
        Write-Host ("[autotest] CRASHED DURING THIS PASS: pid(s) {0} -- the numbers below " +
            "describe a run that killed an instance" -f ($died -join ', ')) -ForegroundColor Red
        $rc = 1
    }

    # run_mptest already decided this pass and printed the evidence; parse its
    # numbers rather than re-deriving them here from a second read of the same
    # logs. The old code re-read the SANDBOX stdout unconditionally, so on a
    # b->a pass it graded the wrong instance.
    function Num($pattern) {
        $m = [regex]::Match("$runOut", $pattern)
        if ($m.Success) { [int]$m.Groups[1].Value } else { 0 }
    }
    $passStats += ,[pscustomobject]@{
        Dir      = "$who -> " + $(if ($who -eq "a") { "b" } else { "a" })
        Sent     = (Num 'actor shipped: (\d+)')
        Recv     = (Num 'peer got     : (\d+)')
        Ok       = (Num 'replayed ok  : (\d+)')
        Bad      = (Num 'failed       : (\d+)')
        Built    = ([regex]::Matches("$runOut", 'edge callback success=true')).Count
        Rejected = ([regex]::Matches("$runOut", 'edge callback success=false')).Count
        Died     = ($died -join ', ')
        Rc       = $rc
    }
}

# Process.Responding is the WRONG probe and this was measured, not guessed.
# A bad TransportVehicleConfig wedges the SIM thread while the render thread
# keeps pumping messages: the game answers the UI, reports Responding=True, and
# looks perfectly healthy while its world has stopped. Instance A sat like that
# with ticks frozen at 452 for minutes.
#
# The sim hook already logs "ticks=N" every 10 s from inside GameSim::Step, so
# that counter IS the sim's pulse. If it stops advancing, the sim is dead
# regardless of what the UI says.
function Get-SimTicks($dir) {
    $f = Join-Path $dir "tpf2_bridge.log"
    if (-not (Test-Path $f)) { return $null }
    $m = Select-String -Path $f -Pattern 'ticks=(\d+)' -EA SilentlyContinue | Select-Object -Last 1
    if ($m) { return [int]$m.Matches[0].Groups[1].Value }
    return $null
}

$hung = @()
$t1 = @{ A = (Get-SimTicks $Out); B = (Get-SimTicks $Ovl) }
Start-Sleep -Seconds 25          # tick lines are emitted every 10 s
$t2 = @{ A = (Get-SimTicks $Out); B = (Get-SimTicks $Ovl) }
# NOTE the unary commas: @( @(a,b), @(c,d) ) FLATTENS in PowerShell to
# a,b,c,d, so $side[0]/$side[1] silently became scalars and the watchdog
# handed an object to Join-Path (DriveNotFoundException), taking the whole
# run down before the scenario. ,@(...) preserves each pair.
foreach ($side in @( ,@("A", $idReal) ; ,@("B", $idOvl) )) {
    $tag, $inst = $side[0], $side[1]
    $pr = if ($inst) { Get-Process -Id $inst.Pid -EA SilentlyContinue } else { $null }
    if ($inst -and $inst.Pid -gt 0 -and -not $pr) {
        Say "WATCHDOG: instance '$($inst.Id)' (pid $($inst.Pid)) DIED during the scenario"
        continue
    }
    if ($null -ne $t1[$tag] -and $t1[$tag] -eq $t2[$tag]) {
        Write-Host ("[autotest] WATCHDOG: instance '{0}' SIM IS WEDGED -- ticks stuck at {1} " +
            "for 25 s (the UI may still respond; that means nothing)" -f
            $inst.Id, $t2[$tag]) -ForegroundColor Red
        if ($inst.Pid -gt 0) { $hung += $inst.Pid }
    }
}
# a process that stopped responding entirely is also hung
foreach ($wp in $watchPids) {
    $pr = Get-Process -Id $wp -EA SilentlyContinue
    if ($pr -and -not $pr.Responding -and $hung -notcontains $wp) { $hung += $wp }
}
if ($hung.Count -gt 0) {
    Write-Host ("[autotest] WATCHDOG: {0} instance(s) stopped responding: {1}" -f
        $hung.Count, ($hung -join ', ')) -ForegroundColor Red
    Write-Host "[autotest] killing them so the sandbox is not left wedged" -ForegroundColor Red
    foreach ($wp in $hung) { Stop-Process -Id $wp -Force -EA SilentlyContinue }
    & $Sbie "/box:$Box" /terminate 2>$null | Out-Null
}

# ------------------------------------------------------------ final verdict
Write-Host ""
Write-Host "================ SUMMARY ================" -ForegroundColor Cyan
$anyFail = $false
$anyReject = 0
foreach ($s in $passStats) {
    Write-Host ("  --- {0} ---" -f $s.Dir)
    # Edge counts are edge-specific and read as zero for a scenario that only
    # creates lines or edits stations -- say so rather than implying nothing ran.
    if ($s.Built -eq 0 -and $s.Rejected -eq 0) {
        Write-Host  "  actor edges     : none attempted (not an edge scenario)"
    } else {
        Write-Host ("  actor built     : {0} edge(s)" -f $s.Built)
        Write-Host ("  actor rejected  : {0} edge(s)  <- game refused; NOT a replication bug" -f $s.Rejected)
    }
    $anyReject += $s.Rejected
    Write-Host ("  shipped -> got  : {0} -> {1}" -f $s.Sent, $s.Recv)
    Write-Host ("  peer replayed   : ok={0} failed={1}" -f $s.Ok, $s.Bad)
    if ($s.Died) {
        Write-Host ("  RESULT: FAIL -- an instance CRASHED during this pass (pid {0})." -f $s.Died) -ForegroundColor Red
        Write-Host  "          Counts above are not trustworthy; a crash after shipping still totals clean." -ForegroundColor Red
        $anyFail = $true
    } elseif ($s.Rc -eq 0) {
        Write-Host "  RESULT: PASS -- everything the actor built replicated and replayed." -ForegroundColor Green
    } elseif ($s.Sent -eq 0 -and $s.Ok -eq 0 -and $s.Bad -eq 0) {
        Write-Host "  RESULT: INCONCLUSIVE -- actor shipped nothing. For an edge scenario that" -ForegroundColor DarkYellow
        Write-Host "          usually means bad terrain; re-run to shift ground." -ForegroundColor DarkYellow
        $anyFail = $true
    } else {
        Write-Host "  RESULT: FAIL -- replication lost or mis-replayed something." -ForegroundColor Red
        $anyFail = $true
    }
}
if ($anyReject -gt 0) {
    Write-Host "  note: rejected builds are usually terrain (rail max grade ~3-5%) or an" -ForegroundColor DarkGray
    Write-Host "        endpoint that already has a node (task 13, node reuse)." -ForegroundColor DarkGray
}
if ($OneWay) {
    Write-Host "  note: -OneWay, so b -> a was NOT tested. a -> b passing does not cover it." -ForegroundColor DarkGray
}
Write-Host ("  OVERALL: {0}" -f $(if ($anyFail) { "FAIL" } else { "PASS" })) `
    -ForegroundColor $(if ($anyFail) { "Red" } else { "Green" })

# --------------------------------------------------------------- capture
Say "capturing both windows for the record"
foreach ($x in @( ,@($idReal.Id, $idReal.Pid) ; ,@($idOvl.Id, $idOvl.Pid) )) {
    if ($x[1] -gt 0) {
        & (Join-Path $T "screenshot.ps1") -Path (Join-Path $Shots "final_$($x[0]).png") `
            -Window TransportFever2 -TargetPid $x[1] | Out-Null
    }
}

# --------------------------------------------------------------- health
Say "health check"
Get-Process TransportFever2 -EA SilentlyContinue |
    ForEach-Object { "  pid $($_.Id) responding=$($_.Responding) $([int]($_.WorkingSet64/1MB)) MB" }
# Unary commas required -- see the note at the watchdog. Without them $d came
# out as the scalars "A", <path>, "B", <path>, so $d[1] was $null and this check
# tailed "$null\tpf2_bridge.log": it never reported a crash because it was never
# reading a log at all.
foreach ($d in @( ,@("A", $Out) ; ,@("B", $Ovl) )) {
    $crash = Get-Content "$($d[1])\tpf2_bridge.log" -Tail 40 -EA SilentlyContinue |
             Select-String 'REFUSING|Assertion|SIGABRT'
    if ($crash) { Write-Host "  $($d[0]): $($crash[-1].Line)" -ForegroundColor Yellow }
}

if (-not $KeepOpen) {
    # Same reasoning as at launch: close the games, leave the box's Steam warm
    # so the NEXT run does not pay for a cold start. -HardReset still nukes it.
    Say "closing both instances (sandboxed Steam left running)"
    Get-Process TransportFever2 -EA SilentlyContinue | Stop-Process -Force
    if ($HardReset) { & $Sbie "/box:$Box" /terminate 2>$null | Out-Null }
}
Say "done. screenshots in $Shots"
# Exit code, so this can be chained or run unattended without scraping stdout.
exit $(if ($anyFail) { 1 } else { 0 })
