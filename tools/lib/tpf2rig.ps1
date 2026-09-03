<#
.SYNOPSIS
Shared rig library: find the running instances, drive their windows, read their
evidence. Dot-sourced by tools\soak.ps1.

.DESCRIPTION
Everything in here is either lifted from a script that already worked
(autotest.ps1, click_continue.ps1, snapshot_logs.ps1) or measured from live
files this session. The comments say which, because the difference matters when
one of these stops working.

WHAT AN "INSTANCE" IS
Three things have to be joined up before anything can be asserted:

  a Sandboxie BOX  (native / GameAgent / GameAgent2)  -- decides every PATH
  a lockstep LETTER (a / b / c)                       -- decides every FILE NAME
  an OS process                                       -- decides liveness

The letter is NOT a property of the box: it comes from a port election at
startup and swaps between launches (measured 2026-09-03: GameAgent was 'c' and
GameAgent2 was 'b'). So the mapping is read at run time from each box's own
tpf2_instance.txt, never assumed. Getting this wrong reads one instance's
dashboard three times and calls it a three-way SYNC.

THE SANDBOX READ-THROUGH TRAP
Sandboxie shows a box a merged view: its own writes first, the real filesystem
underneath. So GameAgent's game dir lists egeo_b.txt AND egeo_c.txt -- one of
them is the NATIVE instance's file read through, or a leftover from a previous
session in which this box held the other letter. Every reader below therefore
checks freshness (mtime, and the in-band wall clock where there is one) instead
of trusting that a file present in a box's view belongs to that box's run.
#>

Set-StrictMode -Off

# ---------------------------------------------------------------- constants
$script:RigGameDir  = "C:\Program Files (x86)\Steam\steamapps\common\Transport Fever 2"
$script:RigExe      = Join-Path $script:RigGameDir "TransportFever2.exe"
$script:RigSbie     = "C:\Program Files\Sandboxie-Plus\Start.exe"
# The data dir AS THE GAME PROCESS SEES IT. A boxed process resolves this same
# string to its own overlay, which is exactly what we want when handing a path
# to Lua running inside that process. Forward slashes: it goes into Lua source.
$script:RigLuaDataDir = ($env:LOCALAPPDATA -replace '\\', '/') + "/tpf2mp/data/"

# --------------------------------------------------------------- win32 glue
# Lifted verbatim from autotest.ps1 / click_continue.ps1. Each line of this cost
# a debugging round; see the header of autotest.ps1 for the full story.
#   * SetForegroundWindow is refused from a background process unless we attach
#     to the foreground thread's input queue first.
#   * The first click on an unfocused window only activates it.
#   * A synthetic click goes to whatever window is on top AT THAT PIXEL, not to
#     a process -- so never click without asking WindowFromPoint first.
Add-Type @"
using System;
using System.Runtime.InteropServices;
public class RIG {
    [StructLayout(LayoutKind.Sequential)] public struct RECT { public int Left, Top, Right, Bottom; }
    [StructLayout(LayoutKind.Sequential)] public struct POINT { public int X, Y; }
    [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
    [DllImport("user32.dll")] public static extern bool SetProcessDpiAwarenessContext(IntPtr v);
    [DllImport("user32.dll")] public static extern bool SetCursorPos(int x, int y);
    [DllImport("user32.dll")] public static extern bool GetCursorPos(out POINT p);
    [DllImport("user32.dll")] public static extern void mouse_event(uint f, uint x, uint y, uint d, IntPtr e);
    [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
    [DllImport("user32.dll")] public static extern IntPtr GetForegroundWindow();
    [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr h, int c);
    [DllImport("user32.dll")] public static extern void SwitchToThisWindow(IntPtr h, bool alt);
    [DllImport("user32.dll")] public static extern bool BringWindowToTop(IntPtr h);
    [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, IntPtr p);
    [DllImport("kernel32.dll")] public static extern uint GetCurrentThreadId();
    [DllImport("user32.dll")] public static extern bool AttachThreadInput(uint a, uint b, bool at);
    [DllImport("user32.dll")] public static extern IntPtr WindowFromPoint(POINT p);
    [DllImport("user32.dll")] public static extern IntPtr GetAncestor(IntPtr h, uint flags);
    [DllImport("user32.dll")] public static extern void keybd_event(byte vk, byte scan, uint flags, IntPtr extra);

    public static void Raise(IntPtr h) {
        IntPtr fg = GetForegroundWindow();
        uint ft = GetWindowThreadProcessId(fg, IntPtr.Zero), me = GetCurrentThreadId();
        AttachThreadInput(me, ft, true);
        ShowWindow(h, 9); BringWindowToTop(h); SetForegroundWindow(h);
        AttachThreadInput(me, ft, false);
        SwitchToThisWindow(h, true);
    }
    public static void RawClick(int x, int y) {
        SetCursorPos(x, y); System.Threading.Thread.Sleep(80);
        mouse_event(0x0002,0,0,0,IntPtr.Zero); System.Threading.Thread.Sleep(60);
        mouse_event(0x0004,0,0,0,IntPtr.Zero);
    }
    // Click ONLY if the expected window owns that pixel. Compared against
    // WindowFromPoint, not GetForegroundWindow: the latter is a proxy that is
    // wrong in both directions (it blocked forever on a titleless GameInputSvc
    // window nowhere near the click point, and would happily click a focused
    // window that was occluded at that pixel).
    public static bool ClickIfOnTarget(IntPtr expect, int x, int y) {
        POINT p; p.X = x; p.Y = y;
        IntPtr hit = WindowFromPoint(p);
        if (hit == IntPtr.Zero) return false;
        IntPtr root = GetAncestor(hit, 2 /* GA_ROOT */);
        if (root != expect && hit != expect) return false;
        RawClick(x, y);
        return true;
    }
    public static bool CursorOnTarget(IntPtr expect, int x, int y) {
        POINT p; p.X = x; p.Y = y;
        IntPtr hit = WindowFromPoint(p);
        if (hit == IntPtr.Zero) return false;
        IntPtr root = GetAncestor(hit, 2);
        return (root == expect || hit == expect);
    }
    public static void Key(byte vk) {
        keybd_event(vk, 0, 0, IntPtr.Zero); System.Threading.Thread.Sleep(45);
        keybd_event(vk, 0, 2 /* KEYEVENTF_KEYUP */, IntPtr.Zero);
    }
    // The build tools refuse to work zoomed out ("Zoom in to build"), so a
    // world click has to be preceded by wheel notches at the same point.
    // MOUSEEVENTF_WHEEL = 0x0800; one notch = WHEEL_DELTA 120, positive = in.
    public static bool WheelIfOnTarget(IntPtr expect, int x, int y, int notches) {
        if (!CursorOnTarget(expect, x, y)) return false;
        SetCursorPos(x, y); System.Threading.Thread.Sleep(60);
        int step = notches > 0 ? 120 : -120;
        int n = notches > 0 ? notches : -notches;
        for (int k = 0; k < n; k++) {
            mouse_event(0x0800, 0, 0, unchecked((uint)step), IntPtr.Zero);
            System.Threading.Thread.Sleep(70);
        }
        return true;
    }
}
"@ -ErrorAction SilentlyContinue

# MUST run before any window query or cursor move. Without it GetWindowRect and
# SetCursorPos speak LOGICAL pixels while the screen is PHYSICAL, so on a scaled
# monitor every coordinate is wrong by the scale factor (the sandboxed instance
# sits on a 125% display and reports 1936x1119 for a 2420x1399 window).
# -4 = DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2.
[void][RIG]::SetProcessDpiAwarenessContext([IntPtr](-4))

# ------------------------------------------------------------------ logging
$script:RigQuiet = $false
function Say  ($m) { if (-not $script:RigQuiet) { Write-Host ("[soak] " + $m) -ForegroundColor Cyan } }
function Note ($m) { if (-not $script:RigQuiet) { Write-Host ("[soak] " + $m) -ForegroundColor DarkGray } }
function Warn ($m) { Write-Host ("[soak] " + $m) -ForegroundColor Yellow }
function Bad  ($m) { Write-Host ("[soak] " + $m) -ForegroundColor Red }

# ------------------------------------------------------- reading busy files
# The game holds stdout.txt and its data files open for writing. A plain
# Get-Content dies with "used by another process", so every read here opens with
# FileShare.ReadWrite. Copied from autotest.ps1's NewStdout, which learned it the
# same way.
function Read-SharedText([string]$Path, [long]$Offset = 0) {
    if (-not $Path -or -not (Test-Path -LiteralPath $Path)) { return "" }
    try {
        $fs = [System.IO.File]::Open($Path, 'Open', 'Read', 'ReadWrite')
        try {
            if ($Offset -gt 0) { [void]$fs.Seek([Math]::Min($Offset, $fs.Length), 'Begin') }
            $sr = New-Object System.IO.StreamReader($fs)
            $t = $sr.ReadToEnd()
            $sr.Close()
            return $t
        } finally { $fs.Close() }
    } catch { return "" }
}

function Get-FileLen([string]$Path) {
    if ($Path -and (Test-Path -LiteralPath $Path)) { return (Get-Item -LiteralPath $Path).Length }
    return 0
}

# ------------------------------------------------------------ rig discovery
# One row per Sandboxie box. Names match snapshot_logs.ps1's folders on purpose,
# so a failure report can point straight at the snapshot it just took.
function Get-RigBoxes {
    $u = $env:USERNAME
    $stdoutRel = "Program Files (x86)\Steam\userdata\*\1066780\local\crash_dump"
    $gameRel   = "Program Files (x86)\Steam\steamapps\common\Transport Fever 2"
    @(
        [pscustomobject]@{
            Box = "native"; IsBox = $false; SbieBox = ""
            DataDir  = (Join-Path $env:LOCALAPPDATA "tpf2mp\data")
            GameDir  = "C:\$gameRel"
            CrashDir = "C:\$stdoutRel"
        }
        [pscustomobject]@{
            Box = "GameAgent"; IsBox = $true; SbieBox = "GameAgent"
            DataDir  = "C:\Sandbox\$u\GameAgent\user\current\AppData\Local\tpf2mp\data"
            GameDir  = "C:\Sandbox\$u\GameAgent\drive\C\$gameRel"
            CrashDir = "C:\Sandbox\$u\GameAgent\drive\C\$stdoutRel"
        }
        [pscustomobject]@{
            Box = "GameAgent2"; IsBox = $true; SbieBox = "GameAgent2"
            DataDir  = "C:\Sandbox\$u\GameAgent2\user\current\AppData\Local\tpf2mp\data"
            GameDir  = "C:\Sandbox\$u\GameAgent2\drive\C\$gameRel"
            CrashDir = "C:\Sandbox\$u\GameAgent2\drive\C\$stdoutRel"
        }
    )
}

<#
Build the same instance records from a SNAPSHOT folder written by
snapshot_logs.ps1, so the grader can be run against evidence after the fact --
and so it can be tested without touching a shared rig.

Attributing a letter to a box folder is the awkward part. snapshot_logs did not
used to copy tpf2_instance.txt, so old folders have no record of which letter a
box held, and each folder contains several dashboards: Sandboxie shows a box the
real filesystem underneath its own writes, so a box's view carries the native
instance's files too. When the identity file is absent, the box's OWN dashboard
is the one with the newest wall clock in that folder -- the others are
read-through or leftovers, and both are older by construction.
#>
function Get-SnapshotInstances([string]$Dir) {
    $out = @()
    foreach ($sub in (Get-ChildItem -LiteralPath $Dir -Directory -ErrorAction SilentlyContinue)) {
        $letter = $null
        $idf = Join-Path $sub.FullName "tpf2_instance.txt"
        if (Test-Path -LiteralPath $idf) {
            $l = @(Get-Content -LiteralPath $idf -ErrorAction SilentlyContinue)
            if ($l.Count -ge 1 -and $l[0].Trim() -match '^[a-h]$') { $letter = $l[0].Trim() }
        }
        if (-not $letter) {
            $best = $null; $bestWall = -1
            foreach ($f in (Get-ChildItem (Join-Path $sub.FullName "lockstep_dash_*.txt") -ErrorAction SilentlyContinue)) {
                $d = Read-Dash $f.FullName
                if ($null -eq $d -or -not $d.ContainsKey('wall')) { continue }
                if ([double]$d['wall'] -gt $bestWall) { $bestWall = [double]$d['wall']; $best = $f.Name }
            }
            if ($best -and $best -match 'lockstep_dash_([a-h])\.txt') { $letter = $Matches[1] }
        }
        if (-not $letter) { continue }
        $out += [pscustomobject]@{
            Box = $sub.Name; IsBox = ($sub.Name -ne "native"); SbieBox = ""
            Letter = $letter; Pid = 0; Port = 0; Alive = $true; Proc = $null
            IdAgeSec = 0; IdStale = $false
            DataDir = $sub.FullName; GameDir = $sub.FullName; CrashDir = $null
            DashFile   = (Join-Path $sub.FullName ("lockstep_dash_{0}.txt"   -f $letter))
            StatusFile = (Join-Path $sub.FullName ("lockstep_status_{0}.txt" -f $letter))
            InjectFile = (Join-Path $sub.FullName ("lockstep_inject_{0}.txt" -f $letter))
            CompanyLog = (Join-Path $sub.FullName ("mp_company_{0}.log"      -f $letter))
            EgeoFile   = (Join-Path $sub.FullName ("egeo_{0}.txt"            -f $letter))
            StdoutFile = (Join-Path $sub.FullName "stdout.txt")
            LuaOutDir  = $null
        }
    }
    return @($out | Sort-Object Letter)
}

function Resolve-CrashDir([string]$Pattern) {
    $d = Get-Item -Path $Pattern -ErrorAction SilentlyContinue | Select-Object -First 1
    if ($d) { return $d.FullName }
    return $null
}

<#
Join box + letter + process into one instance record.

-MaxAgeSeconds guards against a box whose identity file is left over from a
previous session. A stale identity is worse than a missing one: it names a
letter whose dashboard is also stale, and every assertion then grades a run that
ended hours ago. 0 disables the check (used by -AssertOnly against files the
caller knows are historical).
#>
function Get-RigInstances([int]$MaxAgeSeconds = 900) {
    $now = Get-Date
    $procs = @(Get-Process TransportFever2 -ErrorAction SilentlyContinue)
    $out = @()
    foreach ($b in (Get-RigBoxes)) {
        $idf = Join-Path $b.DataDir "tpf2_instance.txt"
        if (-not (Test-Path -LiteralPath $idf)) { continue }
        $age = ($now - (Get-Item -LiteralPath $idf).LastWriteTime).TotalSeconds
        $lines = @(Get-Content -LiteralPath $idf -ErrorAction SilentlyContinue)
        if ($lines.Count -lt 1) { continue }
        $letter = $lines[0].Trim()
        if ($letter -notmatch '^[a-h]$') { continue }
        $thePid = 0; $port = 0
        foreach ($l in $lines) {
            if ($l -match 'pid=(\d+)')  { $thePid = [int]$Matches[1] }
            if ($l -match 'port=(\d+)') { $port   = [int]$Matches[1] }
        }
        $proc = $procs | Where-Object { $_.Id -eq $thePid } | Select-Object -First 1
        $crash = Resolve-CrashDir (Join-Path $b.CrashDir "")
        $out += [pscustomobject]@{
            Box        = $b.Box
            IsBox      = $b.IsBox
            SbieBox    = $b.SbieBox
            Letter     = $letter
            Pid        = $thePid
            Port       = $port
            Alive      = ($null -ne $proc)
            Proc       = $proc
            IdAgeSec   = [int]$age
            IdStale    = ($MaxAgeSeconds -gt 0 -and $age -gt $MaxAgeSeconds)
            DataDir    = $b.DataDir
            GameDir    = $b.GameDir
            CrashDir   = $crash
            DashFile   = (Join-Path $b.DataDir ("lockstep_dash_{0}.txt"   -f $letter))
            StatusFile = (Join-Path $b.DataDir ("lockstep_status_{0}.txt" -f $letter))
            InjectFile = (Join-Path $b.DataDir ("lockstep_inject_{0}.txt" -f $letter))
            CompanyLog = (Join-Path $b.DataDir ("mp_company_{0}.log"      -f $letter))
            EgeoFile   = (Join-Path $b.GameDir ("egeo_{0}.txt"            -f $letter))
            StdoutFile = $(if ($crash) { Join-Path $crash "stdout.txt" } else { $null })
            # Path the LUA inside this process must use to write a file we can
            # then read from DataDir. Identical string for every box: Sandboxie
            # redirects it per box, which is the point.
            LuaOutDir  = $script:RigLuaDataDir
        }
    }
    return @($out | Sort-Object Letter)
}

# ---------------------------------------------------------- dashboard files
<#
lockstep_dash_<letter>.txt, written by lockstep.lua every 15 ticks. Format
(measured 2026-09-03, and the writer is one string.format so it does not drift):

  t= peer= skew= desyncs= late= applylag= applylate= applied= queued=
  paused= speed= verdict= detail=
  vdrift=<per-peer mean/max>
  money=<balance> / loan <loan>
  wall=<unix seconds>                     <- liveness, see below
  nack=<sent>/<answered> recovered=<n>
  peers=<letter>:<t>:<skew>:<verdict>,... <- who this instance can actually hear
  ev=<recent event lines>

`wall` is the only trustworthy freshness signal. mtime is not: a boxed
instance's dashboard can be read through from the native dir, in which case its
mtime is the OTHER instance's write.
#>
function Read-Dash([string]$Path) {
    $txt = Read-SharedText $Path
    if (-not $txt.Trim()) { return $null }
    $d = @{ Path = $Path; Ev = @(); Peers = @{} }
    foreach ($line in ($txt -split "`r?`n")) {
        if ($line -match '^ev=(.*)$')            { $d.Ev += $Matches[1]; continue }
        if ($line -match '^money=(-?\d+)\s*/\s*loan\s*(-?\d+)') {
            $d.Money = [long]$Matches[1]; $d.Loan = [long]$Matches[2]; continue
        }
        if ($line -match '^money=') { $d.Money = $null; $d.Loan = $null; continue }
        if ($line -match '^peers=(.*)$') {
            $d.PeersRaw = $Matches[1]
            foreach ($p in ($Matches[1] -split ',')) {
                if ($p -match '^([a-h]):(-?[\d.]+):([+-][\d.]+):(\S+)$') {
                    $d.Peers[$Matches[1]] = [pscustomobject]@{
                        Letter = $Matches[1]; T = [double]$Matches[2]
                        Skew = [double]$Matches[3]; Verdict = $Matches[4]
                    }
                }
            }
            continue
        }
        if ($line -match '^([a-z]+)=(.*)$') { $d[$Matches[1]] = $Matches[2] }
    }
    # typed convenience fields; a missing key must read as $null, never 0
    foreach ($k in @('t','desyncs','late','applylate','applied','queued','wall')) {
        if ($d.ContainsKey($k) -and $d[$k] -match '^-?\d+(\.\d+)?$') { $d[$k] = [double]$d[$k] }
    }
    return $d
}

function Get-DashAgeSeconds($dash) {
    if ($null -eq $dash -or -not $dash.ContainsKey('wall')) { return $null }
    $w = $dash['wall']
    if ($null -eq $w) { return $null }
    # os.time() is a real time_t: MEASURED against a known dashboard write --
    # wall=1788411107 is 00:51:47 local / 04:51:47 UTC, i.e. a UTC epoch.
    #
    # NOT `Get-Date -UFormat %s`. On Windows PowerShell 5.1 that returns a
    # LOCAL-time-based epoch: measured 1788398684 against the true 1788413084,
    # four hours out. Using it made every dashboard look 4 hours stale, which
    # would have failed A3 on a perfectly healthy rig.
    $nowUnix = [DateTimeOffset]::UtcNow.ToUnixTimeSeconds()
    return [int]($nowUnix - [double]$w)
}

# --------------------------------------------------------------- egeo files
<#
egeo_<letter>.txt is written into the GAME dir (the script's CWD), not the data
dir, when tpf2_slice.cfg has dump_egeo=1. Line 1 is "stamp=<gametime>
nedges=<n>"; every later line is one edge "x,y,z>x,y,z".

The stamp is per-instance -- each dumps on its own tick -- so the FILES never
hash equal even in a perfectly synced session (measured: three different MD5s
for three byte-identical bodies). Compare the BODY; report the stamps.
#>
function Read-Egeo([string]$Path) {
    if (-not $Path -or -not (Test-Path -LiteralPath $Path)) { return $null }
    $txt = Read-SharedText $Path
    if (-not $txt.Trim()) { return $null }
    $lines = @($txt -split "`r?`n")
    $stamp = $null; $n = $null
    if ($lines[0] -match '^stamp=([\d.]+)\s+nedges=(\d+)') {
        $stamp = [double]$Matches[1]; $n = [int]$Matches[2]
    }
    $body = @($lines | Select-Object -Skip 1 | Where-Object { $_.Trim() -ne '' })
    $sha = [System.Security.Cryptography.SHA256]::Create()
    $bytes = [System.Text.Encoding]::ASCII.GetBytes(($body -join "`n"))
    $hash = [BitConverter]::ToString($sha.ComputeHash($bytes)).Replace('-','').Substring(0,16)
    $sha.Dispose()
    return [pscustomobject]@{
        Path = $Path; Stamp = $stamp; NEdges = $n; Lines = $body.Count
        BodyHash = $hash; Body = $body
        Mtime = (Get-Item -LiteralPath $Path).LastWriteTime
    }
}

# First few differing edges, for a failure report that says WHAT diverged rather
# than only that something did.
function Compare-EgeoBodies($a, $b, [int]$Max = 6) {
    $setA = New-Object 'System.Collections.Generic.HashSet[string]'
    foreach ($l in $a.Body) { [void]$setA.Add($l) }
    $setB = New-Object 'System.Collections.Generic.HashSet[string]'
    foreach ($l in $b.Body) { [void]$setB.Add($l) }
    $onlyA = @($a.Body | Where-Object { -not $setB.Contains($_) } | Select-Object -First $Max)
    $onlyB = @($b.Body | Where-Object { -not $setA.Contains($_) } | Select-Object -First $Max)
    return [pscustomobject]@{ OnlyA = $onlyA; OnlyB = $onlyB }
}

# ------------------------------------------------------------- crash dumps
<#
An engine assert writes <guid>.dmp into the crash_dump dir and the game KEEPS
RUNNING (measured: 18 dumps in one co-op session from nil-entity API calls). So
a new dump is not the same as a crash -- it is an assert that fired, which is
worth failing on either way, and the report must not claim the process died.
#>
function Get-NewDumps([datetime]$Since) {
    $found = @()
    foreach ($b in (Get-RigBoxes)) {
        $dir = Resolve-CrashDir (Join-Path $b.CrashDir "")
        if (-not $dir) { continue }
        Get-ChildItem (Join-Path $dir "*.dmp") -ErrorAction SilentlyContinue |
            Where-Object { $_.LastWriteTime -gt $Since } |
            ForEach-Object { $found += [pscustomobject]@{ Box = $b.Box; Name = $_.Name; Path = $_.FullName; When = $_.LastWriteTime; KB = [int]($_.Length / 1KB) } }
    }
    return @($found)
}

# ----------------------------------------------------------------- log scan
<#
WHERE THE MOD'S LOG LINES ACTUALLY ARE, WHILE THE GAME IS RUNNING.

stdout.txt is buffered by the game: the [ls-<letter>] lines appear in bulk, and
a chunk of them only at shutdown. So a live soak run CANNOT rely on it. Three
live channels are read instead, in this order of usefulness:

  1. lockstep_dash_<x>.txt `ev=` -- the mod's own recent-event ring. Live,
     rewritten every 15 ticks, and it is where EXEC/DIVERGENCE/BARRIER land.
  2. mp_company_<x>.log        -- appended and closed per call, so always
     flushed. Carries the detailed replay diagnostics.
  3. stdout.txt                -- scanned anyway, because startup errors DO
     reach it early and that is the load-order crash case.

Patterns are deliberately narrow. 'error' as a substring matches the game's own
harmless "ERROR: Could not verify user authentication" on every launch, and a
check that cries wolf on every run is worse than no check.
#>
$script:RigBadPatterns = @(
    'DIVERGENCE'
    '__CRASHDB_DUMP__'
    'attempt to index'
    'attempt to call'
    'attempt to perform arithmetic'
    'attempt to compare'
    'stack traceback'
    'replay error'
    'Lua error'
    'game script.*error'
    '!! '                       # the mod's own loud-failure prefix
    'REFUSING'
    'Assertion'
)

function Get-BadLines([string]$Text, [int]$Max = 8) {
    if (-not $Text) { return @() }
    $hits = @()
    foreach ($line in ($Text -split "`r?`n")) {
        foreach ($p in $script:RigBadPatterns) {
            if ($line -match $p) { $hits += $line.Trim(); break }
        }
        if ($hits.Count -ge $Max) { break }
    }
    return @($hits)
}

# ------------------------------------------------------- the EVAL side door
<#
lockstep.lua's inject file already carries a diagnostic opcode:

    EVAL <one line of lua>

pollInject compiles and runs it in the global environment, and it runs
unconditionally -- peer or no peer, before the "captures are dropped in single
player" gate. That gives the harness a scripting channel into a LIVE instance
with no mod edit and no conflict with anything.

WHAT IT MAY AND MAY NOT BE USED FOR
It may OBSERVE (read balances, count entities) and it may drive the channels
lockstep captures BY POLLING THE WORLD -- the loan is the one this harness uses.
It must NEVER be used to build, buy, or place: those channels are captured by
the slice DLL on the caller's return address, a script call has the wrong
caller, and slice_hook.cpp logs it and drops it ("BuildProposal from
caller_rva=... -- ignored", "VBUY from the Lua path -- a replay, not shipped").
Driving them from here would change one world only and manufacture a desync the
product does not have.

The result comes back through a FILE, not the log: print() is buffered by the
game for minutes, so "EVAL -> ..." in stdout is useless to a live runner.
#>
function Invoke-RigEval {
    param(
        [Parameter(Mandatory)]$Instance,
        [Parameter(Mandatory)][string]$Chunk
    )
    if ($Chunk -match "[`r`n]") { throw "EVAL chunk must be a single line (the inject file is line-based)" }
    $line = "EVAL " + $Chunk + "`n"
    # Append with FileShare.ReadWrite: the slice DLL appends to this same file
    # and the Lua reads it at an offset. One whole line in one write.
    $fs = [System.IO.File]::Open($Instance.InjectFile, 'Append', 'Write', 'ReadWrite')
    try {
        $bytes = [System.Text.Encoding]::ASCII.GetBytes($line)
        $fs.Write($bytes, 0, $bytes.Length)
        $fs.Flush()
    } finally { $fs.Close() }
}

<#
Run a chunk and wait for its answer. The chunk is wrapped so that whatever it
RETURNS is written to a probe file in the instance's own data dir; a nonce makes
a stale file from a previous call impossible to mistake for this one's answer.
#>
function Invoke-RigProbe {
    param(
        [Parameter(Mandatory)]$Instance,
        [Parameter(Mandatory)][string]$Expr,      # a lua expression or 'do ... end' block returning a string
        [int]$TimeoutSeconds = 25
    )
    $nonce = [guid]::NewGuid().ToString('N').Substring(0, 8)
    $luaPath = $Instance.LuaOutDir + "soak_probe_" + $Instance.Letter + ".txt"
    $winPath = Join-Path $Instance.DataDir ("soak_probe_{0}.txt" -f $Instance.Letter)
    # single line; the writer is wrapped in its own pcall so a bad expression
    # still produces a readable answer instead of silence
    $chunk = ("local ok,v = pcall(function() return {0} end); " +
              "local f = io.open('{1}','w'); if f then f:write('{2}|'..tostring(ok)..'|'..tostring(v)); f:close() end") `
             -f $Expr, $luaPath, $nonce
    Invoke-RigEval -Instance $Instance -Chunk $chunk
    $deadline = (Get-Date).AddSeconds($TimeoutSeconds)
    while ((Get-Date) -lt $deadline) {
        Start-Sleep -Milliseconds 400
        $t = Read-SharedText $winPath
        if ($t -match ("^" + $nonce + "\|(true|false)\|(.*)$")) {
            return [pscustomobject]@{ Ok = ($Matches[1] -eq 'true'); Value = $Matches[2] }
        }
    }
    return [pscustomobject]@{ Ok = $false; Value = "<no answer in ${TimeoutSeconds}s>"; TimedOut = $true }
}

# ------------------------------------------------------------ loan action
<#
The one gameplay action the harness can perform entirely on its own, and it is
genuine rather than a simulation.

lockstep.lua polls its own company's loan every 15 ticks and ships any change as
a LOAN command -- its comment says "any later change is the player at the
finances window and ships as LOAN". It never sees HOW the loan moved, only the
resulting value, so a journal entry booked from here and one booked by the
player at the finances window are the same event to it. That is what makes this
legitimate, and it is why the same trick MUST NOT be used for builds or buys:
those are captured by return address, and a scripted one is dropped.

REPAY FIRST, THEN RE-TAKE. Taking first can hit the engine's loan cap
(30,000,000). A clamped loan makes the mod log "expected N but settled at M
after 90 ticks -- re-baselining, not shipping (DIVERGENCE)" -- seen for real in
the 2026-09-03 00:48 snapshot, where the host sat pinned at the cap while the
peers were at 18,500,000. Repaying always has headroom, and the pair ends where
it started, so repeated runs do not walk the loan anywhere.
#>
function Invoke-RigLoanAction {
    param(
        [Parameter(Mandatory)]$Instance,
        [long]$Amount = 5000000,
        [int]$SettleSeconds = 12
    )
    $read = "(function() local p = api.engine.util.getPlayer(); local e = game.interface.getEntity(p); return tostring(p)..','..tostring(e.balance)..','..tostring(e.loan) end)()"
    $q = Invoke-RigProbe -Instance $Instance -Expr $read
    if (-not $q.Ok) { return @{ Status = "NOT PERFORMED"; Detail = ("could not read balance/loan via EVAL: " + $q.Value) } }
    if ($q.Value -notmatch '^(\d+),(-?\d+),(-?\d+)$') {
        return @{ Status = "NOT PERFORMED"; Detail = ("unparseable probe answer: " + $q.Value) }
    }
    $pid0 = [long]$Matches[1]; $bal0 = [long]$Matches[2]; $loan0 = [long]$Matches[3]
    Note ("  loan probe: player={0} balance={1} loan={2}" -f $pid0, $bal0, $loan0)

    if ($loan0 -lt $Amount -or $bal0 -lt $Amount) {
        $Amount = [long][Math]::Min($loan0, $bal0)
        $Amount = [long]([Math]::Floor($Amount / 1000000) * 1000000)
    }
    if ($Amount -lt 1000000) {
        return @{ Status = "NOT PERFORMED"; Detail = ("no headroom to repay: loan={0} balance={1}" -f $loan0, $bal0) }
    }

    foreach ($step in @(@{ d = -$Amount; what = "repay" }, @{ d = $Amount; what = "take" })) {
        $expr = ("(function() local p = api.engine.util.getPlayer(); local c = api.type.JournalEntryCategory.new(); c.type = 0; " +
                 "local j = api.type.JournalEntry.new(); j.amount = {0}; j.category = c; j.time = -1; " +
                 "api.cmd.sendCommand(api.cmd.make.bookJournalEntry(p, j, api.type.Vec3f.new(0,0,0))); return 'sent' end)()") -f $step.d
        $r = Invoke-RigProbe -Instance $Instance -Expr $expr
        if (-not $r.Ok) { return @{ Status = "FAILED"; Detail = ("{0} {1} rejected: {2}" -f $step.what, $Amount, $r.Value) } }
        Note ("  loan {0} {1} booked" -f $step.what, $Amount)
        # Let the 15-tick poll notice and the peers execute before moving it
        # again -- two changes inside one poll window would ship as one.
        Start-Sleep -Seconds $SettleSeconds
    }

    $q2 = Invoke-RigProbe -Instance $Instance -Expr "(function() local p = api.engine.util.getPlayer(); return tostring(game.interface.getEntity(p).loan) end)()"
    $loan1 = $null
    if ($q2.Ok -and $q2.Value -match '^-?\d+$') { $loan1 = [long]$q2.Value }
    $shown = "?"
    if ($null -ne $loan1) { $shown = $loan1 }
    $detail = ("repaid then re-took {0}; loan {1} -> {2}" -f $Amount, $loan0, $shown)
    if ($null -ne $loan1 -and $loan1 -ne $loan0) {
        return @{ Status = "FAILED"; Detail = ($detail + "  <- the originator's own loan did not return to its start value") }
    }
    return @{ Status = "DONE"; Detail = $detail; ExecOp = "LOAN"; MinExec = 1 }
}

# --------------------------------------------------- native-capture evidence
<#
Did a NATIVE command actually happen?

lockstep_inject_<letter>.txt is the slice DLL's channel into the Lua: one line
per captured native command (ROADE, ROADC, VBUY, VSELL, VLINE, VREV, VDEPOT,
LUPDATE, LDELETE, plus the ARMED/STREETP side-channel lines). Nothing a script
does reaches it -- that is the whole caller-RVA filter. So watching this file is
the honest test of "was that click a real player action?", and it is what lets
this harness report NOT PERFORMED instead of quietly passing when a click misses.
#>
$script:RigInjectOps = @{
    road    = '^ROADE '
    con     = '^ROADC '
    buy     = '^VBUY '
    sell    = '^VSELL '
    line    = '^VLINE '
    depot   = '^VDEPOT '
    reverse = '^VREV '
    any     = '^(ROADE|ROADC|VBUY|VSELL|VLINE|VDEPOT|VREV|VREPL|VNAME|VCOLOR|LUPDATE|LDELETE) '
}

function Wait-RigCapture {
    param(
        [Parameter(Mandatory)]$Instance,
        [Parameter(Mandatory)][string]$Pattern,
        [long]$FromOffset,
        [int]$TimeoutSeconds = 30,
        [int]$MinCount = 1
    )
    $deadline = (Get-Date).AddSeconds($TimeoutSeconds)
    $seen = @()
    while ((Get-Date) -lt $deadline) {
        $txt = Read-SharedText $Instance.InjectFile $FromOffset
        $seen = @($txt -split "`r?`n" | Where-Object { $_ -match $Pattern })
        if ($seen.Count -ge $MinCount) { break }
        Start-Sleep -Milliseconds 500
    }
    return [pscustomobject]@{
        Count = $seen.Count
        Ok    = ($seen.Count -ge $MinCount)
        Lines = @($seen | ForEach-Object { $_.Substring(0, [Math]::Min(120, $_.Length)) })
    }
}

# Did every instance EXECUTE the command? The dashboard's ev= ring carries
# "EXEC <OP> seq=<n> origin=<letter> ..." on every peer that ran it, which is
# the arrival-AND-replay evidence, not just arrival.
function Test-ExecEverywhere {
    param($Instances, [string]$Op, [string]$Origin, [int]$TimeoutSeconds = 60, [int]$MinCount = 1)
    $deadline = (Get-Date).AddSeconds($TimeoutSeconds)
    $result = @{}
    while ((Get-Date) -lt $deadline) {
        $allOk = $true
        foreach ($i in $Instances) {
            if ($i.Letter -eq $Origin) { continue }   # the originator logs SCHED, peers log EXEC
            $d = Read-Dash $i.DashFile
            $n = 0
            if ($d) { $n = @($d.Ev | Where-Object { $_ -match ("EXEC\s+" + $Op + "\b.*origin=" + $Origin) }).Count }
            $result[$i.Letter] = $n
            if ($n -lt $MinCount) { $allOk = $false }
        }
        if ($allOk) { return [pscustomobject]@{ Ok = $true; Counts = $result } }
        Start-Sleep -Seconds 2
    }
    return [pscustomobject]@{ Ok = $false; Counts = $result }
}

# ------------------------------------------------------------------- config
# tpf2_slice.cfg lives next to the DLL (the game dir) first, then the data dir.
function Get-SliceFlag([string]$Key, $Default = $null) {
    foreach ($p in @((Join-Path $script:RigGameDir "tpf2_slice.cfg"),
                     (Join-Path (Join-Path $env:LOCALAPPDATA "tpf2mp\data") "tpf2_slice.cfg"))) {
        if (-not (Test-Path -LiteralPath $p)) { continue }
        foreach ($line in (Get-Content -LiteralPath $p -ErrorAction SilentlyContinue)) {
            if ($line -match '^\s*[#;]') { continue }
            if ($line -match ("^\s*" + [regex]::Escape($Key) + "\s*=\s*(\S+)")) { return $Matches[1] }
        }
    }
    return $Default
}

# mp_company_cfg.txt: line 1 = mode ("coop" | "companies"). In coop every
# instance drives ONE shared company, so balances must match exactly. In
# companies mode each player has their own wallet and a difference is correct --
# the money assertion has to be skipped there, loudly, not silently passed.
function Get-CompanyMode {
    $p = Join-Path (Join-Path $env:LOCALAPPDATA "tpf2mp\data") "mp_company_cfg.txt"
    if (-not (Test-Path -LiteralPath $p)) { return "coop" }
    $l = @(Get-Content -LiteralPath $p -ErrorAction SilentlyContinue)
    if ($l.Count -lt 1) { return "coop" }
    $m = $l[0].Trim()
    if ($m -eq "") { return "coop" }
    return $m
}

# --------------------------------------------------------- window placement
function Get-WinRect([IntPtr]$h) {
    $r = New-Object RIG+RECT
    [void][RIG]::GetWindowRect($h, [ref]$r)
    return [pscustomobject]@{ X = $r.Left; Y = $r.Top; W = $r.Right - $r.Left; H = $r.Bottom - $r.Top }
}

<#
One click into a game window, addressed in WINDOW-RELATIVE coordinates.

Never resize the window to normalise coordinates. Verified the hard way: a game
sitting on a fully drawn main menu, resized with SetWindowPos, drops back to a
logo-only screen with no menu -- indistinguishable from a hang. Take the window
at whatever size it opens and compute offsets from its rect.

Focus is borrowed for the click and handed straight back, cursor included, so an
unattended run does not sit on top of whatever the user left in front.
#>
function Invoke-GameClick {
    param(
        [Parameter(Mandatory)]$Instance,
        [Parameter(Mandatory)][double]$FracX,     # 0..1 across the window
        [Parameter(Mandatory)][double]$FracY,
        [int]$SettleMs = 600
    )
    if (-not $Instance.Proc) { return $false }
    $h = $Instance.Proc.MainWindowHandle
    if ($h -eq 0) { return $false }
    $r = Get-WinRect $h
    $x = $r.X + [int]($r.W * $FracX)
    $y = $r.Y + [int]($r.H * $FracY)
    $prevFg = [RIG]::GetForegroundWindow()
    $pt = New-Object "RIG+POINT"
    [void][RIG]::GetCursorPos([ref]$pt)
    [RIG]::Raise($h)
    Start-Sleep -Milliseconds 400
    $ok = [RIG]::ClickIfOnTarget($h, $x, $y)
    Start-Sleep -Milliseconds $SettleMs
    if ($prevFg -ne [IntPtr]::Zero -and $prevFg -ne $h) {
        [RIG]::Raise($prevFg)
        [void][RIG]::SetCursorPos($pt.X, $pt.Y)
    }
    return $ok
}

# Zoom the camera at a point in the window. Needed before any build click: the
# game refuses to build when zoomed out and says "Zoom in to build" on the HUD,
# which produces no command, no capture, and no log line -- exactly the silent
# nothing that would otherwise look like a broken hook.
function Invoke-GameWheel {
    param(
        [Parameter(Mandatory)]$Instance,
        [Parameter(Mandatory)][double]$FracX,
        [Parameter(Mandatory)][double]$FracY,
        [int]$Notches = 8,
        [int]$SettleMs = 800
    )
    if (-not $Instance.Proc) { return $false }
    $h = $Instance.Proc.MainWindowHandle
    if ($h -eq 0) { return $false }
    $r = Get-WinRect $h
    $x = $r.X + [int]($r.W * $FracX)
    $y = $r.Y + [int]($r.H * $FracY)
    $prevFg = [RIG]::GetForegroundWindow()
    $pt = New-Object "RIG+POINT"
    [void][RIG]::GetCursorPos([ref]$pt)
    [RIG]::Raise($h)
    Start-Sleep -Milliseconds 400
    $ok = [RIG]::WheelIfOnTarget($h, $x, $y, $Notches)
    Start-Sleep -Milliseconds $SettleMs
    if ($prevFg -ne [IntPtr]::Zero -and $prevFg -ne $h) {
        [RIG]::Raise($prevFg)
        [void][RIG]::SetCursorPos($pt.X, $pt.Y)
    }
    return $ok
}

function Invoke-GameKey {
    param([Parameter(Mandatory)]$Instance, [Parameter(Mandatory)][byte]$Vk, [int]$SettleMs = 400)
    if (-not $Instance.Proc) { return $false }
    $h = $Instance.Proc.MainWindowHandle
    if ($h -eq 0) { return $false }
    $prevFg = [RIG]::GetForegroundWindow()
    [RIG]::Raise($h)
    Start-Sleep -Milliseconds 300
    if ([RIG]::GetForegroundWindow() -ne $h) {
        if ($prevFg -ne [IntPtr]::Zero) { [RIG]::Raise($prevFg) }
        return $false
    }
    [RIG]::Key($Vk)
    Start-Sleep -Milliseconds $SettleMs
    if ($prevFg -ne [IntPtr]::Zero -and $prevFg -ne $h) { [RIG]::Raise($prevFg) }
    return $true
}
