<#
.SYNOPSIS
Automated soak / regression run for the LOCKSTEP multiplayer mod. One command,
PASS or FAIL, with the evidence named.

.DESCRIPTION
Every verification of this mod so far has been a human clicking and me reading
logs. This does the watching and the grading; where it can, it also does the
clicking. It exists because three regressions shipped in one session that a
scripted run would have caught in minutes (a depot placement that destroyed the
depot, a Lua load-order mistake that crashed the client at startup, and a batch
line-assign that silently skipped half the vehicles).

    tools\soak.ps1                 attach to the running rig, soak, assert
    tools\soak.ps1 -Quick          short version (shorter soak, loan only)
    tools\soak.ps1 -Launch         start the 3-instance rig first
    tools\soak.ps1 -AssertOnly     grade the rig as it stands right now
    tools\soak.ps1 -Manual         prompt a human for each action, then grade
    tools\soak.ps1 -Measure        screenshot each window + print its rect
    tools\soak.ps1 -WhatIf         print the plan and exit (also a parse check)

WHAT IT ASSERTS  (each one names itself in the report)
  A1  every expected instance is still alive, and none died during the run
  A2  N distinct lockstep letters, each identity file fresh
  A3  every instance's dashboard is being written NOW (its Lua is alive)
  A4  verdict=SYNC everywhere
  A5  desyncs=0 everywhere
  A6  every instance can hear every other one, and grades it SYNC
  A7  balance and loan identical across instances     (co-op mode only)
  A8  egeo_*.txt edge geometry byte-identical across instances
  A9  no new .dmp minidump under any crash_dump dir
  A10 no DIVERGENCE / Lua error lines in any instance's live logs
  A11 the simulation actually advanced (not a wedged sim reporting a stale SYNC)
  A12 no command left stuck in a queue
  A13 every action performed was executed by every OTHER instance

WHY THE ACTIONS ARE DRIVEN THE WAY THEY ARE  --  read this before adding one.
The mod replicates the PLAYER's NATIVE UI commands, captured by the slice DLL on
the CALLER'S RETURN ADDRESS (bridge/src/slice_hook.cpp: CALLER_BUILDPROPOSAL
0x459e97, CALLER_UPGRADE 0x4790fc, and the Lua block 0xcec000..0xcf2000 which is
explicitly treated as "a replay, not shipped"). A command issued from a script --
this mod's own EVAL channel, game.interface.*, api.cmd.* -- arrives with the
wrong caller and is DROPPED, with a log line saying so. Driving a build or a
purchase from a script would therefore change ONE world and manufacture a desync
the product does not have. Verified in the source, not assumed.

So actions come in exactly two legitimate flavours here:

  POLL-DRIVEN CHANNELS -- safe to drive from a script.
    lockstep.lua watches its own world for some changes rather than hooking a
    command: the LOAN is polled every 15 ticks ("any later change is the player
    at the finances window and ships as LOAN"). A loan moved by a script is
    indistinguishable from one moved at the finances window, because the mod
    only ever sees the resulting value. This runner uses that, and only that.

  HOOK-DRIVEN CHANNELS -- must be real clicks.
    roads, upgrades, stations, depots, buys, line assignment, sells. Every one
    of them is VERIFIED against lockstep_inject_<letter>.txt: if the slice did
    not capture a native command, the action is reported NOT PERFORMED. It is
    never reported as a pass.

    AS OF 2026-09-03 THESE ARE NOT AUTOMATED. Synthetic clicks drive this
    game's 2D menus perfectly but do not reach its in-world build tools -- see
    the long note at $UiRecipes for exactly what was tried and what the evidence
    was. So they run under -Manual: a human performs the action, this harness
    waits for the slice to prove it was native, and then grades the result. That
    still removes the whole "click around and then read four log files across a
    sandbox boundary" half of the job.

The inject-file check is the reason this is trustworthy: a click that misses,
or a channel nobody exercised, cannot turn into a green run.
#>
[CmdletBinding(SupportsShouldProcess = $true)]
param(
    # Short run: shorter soak window, loan action only.
    [switch]$Quick,
    # Launch the rig first. Refused if instances are already running unless
    # -Force is also given -- somebody else may be using them.
    [switch]$Launch,
    [switch]$Force,
    # Grade what is running right now: no actions, no waiting, no launching.
    [switch]$AssertOnly,
    # Ask a human to perform each action; wait for the slice to prove it
    # happened, then carry on. Turns this into a supervised regression run.
    [switch]$Manual,
    # Screenshot each instance and print its window rect, for filling in the
    # UI coordinate table. Does nothing else.
    [switch]$Measure,
    # Deploy the mod from the repo before launching. OFF by default: someone may
    # be mid-edit in mod\ and a half-written lockstep.lua is not what you want
    # a regression run to be testing.
    [switch]$Deploy,
    # How long to let the sim run after the actions before grading.
    [int]$SoakSeconds = 0,
    [int]$Players = 3,
    # Leave the games running at the end (default: leave them running too --
    # this never kills anything it did not start).
    [switch]$CloseWhenDone,
    # Comma-separated subset, e.g. -Only loan,road
    [string]$Only = "",
    # Grade a snapshot folder written by snapshot_logs.ps1 instead of a live
    # rig. No game needs to be running. Use it to re-grade last night's
    # evidence -- and to check this harness itself without touching the rig.
    # "latest" picks the newest folder under %LOCALAPPDATA%\tpf2mp\runs.
    [string]$FromSnapshot = ""
)

$ErrorActionPreference = "Stop"
$T = $PSScriptRoot
. (Join-Path $T "lib\tpf2rig.ps1")

if ($SoakSeconds -le 0) { if ($Quick) { $SoakSeconds = 45 } else { $SoakSeconds = 150 } }

# ------------------------------------------------------------------ results
$script:Assertions = @()
$script:Actions    = @()
$script:RunStart   = Get-Date
$script:EvidenceDir = $null

function Add-Assertion([string]$Id, [string]$Name, [string]$Status, [string]$Detail, $Evidence = @()) {
    $script:Assertions += [pscustomobject]@{
        Id = $Id; Name = $Name; Status = $Status; Detail = $Detail; Evidence = @($Evidence)
    }
}
function Add-Action([string]$Id, [string]$Name, [string]$Status, [string]$Detail) {
    $script:Actions += [pscustomobject]@{ Id = $Id; Name = $Name; Status = $Status; Detail = $Detail }
    $c = "DarkGray"
    if ($Status -eq "DONE")         { $c = "Green" }
    if ($Status -eq "NOT PERFORMED"){ $c = "Yellow" }
    if ($Status -eq "FAILED")       { $c = "Red" }
    Write-Host ("  action {0,-9} {1,-14} {2}" -f $Id, $Status, $Detail) -ForegroundColor $c
}

# ------------------------------------------------------------------- plan
# ExecOp is what the PEERS log in their dashboard event ring when they apply the
# command ("EXEC ROADP seq=3 origin=a ... success=true"). It is how the runner
# tells "the actor did it" from "everyone did it" -- the difference the whole
# product is about, and the one a capture-only check would miss.
$allActions = @(
    @{ Id = "loan";    Name = "take then repay a loan";        Kind = "eval"; Quick = $true;  ExecOp = "LOAN"        }
    @{ Id = "road";    Name = "build a road";                  Kind = "ui";   Quick = $true;  ExecOp = "ROADP"       }
    @{ Id = "upgrade"; Name = "upgrade a road (bus/tram lane)";Kind = "ui";   Quick = $false; ExecOp = "ROADP"       }
    @{ Id = "station"; Name = "place a station";               Kind = "ui";   Quick = $false; ExecOp = "(CONX|CONP)" }
    @{ Id = "depot";   Name = "place a depot (splits a road)"; Kind = "ui";   Quick = $false; ExecOp = "(CONX|CONP)" }
    @{ Id = "buy";     Name = "buy a batch of vehicles";       Kind = "ui";   Quick = $false; ExecOp = "VBUY"        }
    @{ Id = "assign";  Name = "assign the batch to a line";    Kind = "ui";   Quick = $false; ExecOp = "VLINE"       }
    @{ Id = "sell";    Name = "sell the batch";                Kind = "ui";   Quick = $false; ExecOp = "VSELL"       }
)
$planned = @($allActions | Where-Object { -not $Quick -or $_.Quick })
if ($Only) {
    $want = @($Only -split '[,\s]+' | Where-Object { $_ })
    $planned = @($allActions | Where-Object { $want -contains $_.Id })
}
$script:Offline = ($FromSnapshot -ne "")
if ($AssertOnly -or $script:Offline) { $planned = @() }

if ($WhatIfPreference) {
    Write-Host "soak.ps1 plan" -ForegroundColor Cyan
    Write-Host ("  mode        : {0}" -f $(if ($AssertOnly) { "assert-only" } elseif ($Manual) { "manual actions" } else { "auto actions" }))
    Write-Host ("  players     : {0}" -f $Players)
    Write-Host ("  launch      : {0}" -f $Launch)
    Write-Host ("  soak window : {0} s" -f $SoakSeconds)
    Write-Host  "  actions     :"
    foreach ($a in $planned) { Write-Host ("    {0,-9} [{1}] {2}" -f $a.Id, $a.Kind, $a.Name) }
    Write-Host  "  assertions  : A1..A13 (see the header of this script)"
    Write-Host  "  note        : [ui] actions are NOT automated -- synthetic clicks reach this game's"
    Write-Host  "                menus but not its in-world build tools. Use -Manual for those."
    exit 0
}

# ================================================================= preflight
Write-Host ""
Write-Host "================ TPF2 LOCKSTEP SOAK ================" -ForegroundColor Cyan
Say ("started {0}" -f $script:RunStart.ToString("HH:mm:ss"))

$inst = @(); $live = @(); $mode = "coop"; $egeoOn = $true

if ($script:Offline) {
    # ---------------- grade a saved snapshot ----------------
    $dir = $FromSnapshot
    if ($dir -eq "latest") {
        $runs = Join-Path $env:LOCALAPPDATA "tpf2mp\runs"
        $l = Get-ChildItem $runs -Directory -ErrorAction SilentlyContinue | Sort-Object LastWriteTime | Select-Object -Last 1
        if (-not $l) { Bad "no snapshot folders under $runs"; exit 2 }
        $dir = $l.FullName
    }
    if (-not (Test-Path -LiteralPath $dir)) { Bad "no such snapshot folder: $dir"; exit 2 }
    Say ("grading SNAPSHOT {0}" -f $dir)
    Note "offline: A1 (crash), A3 (script alive), A9 (minidumps) and A11 (sim advanced) cannot be judged from files alone and are skipped."
    Note "offline: mp_company_<x>.log APPENDS across sessions and is never truncated, so A10 here"
    Note "         reports every error in that file's history, not only this run's. A live run reads"
    Note "         it from a byte offset taken at the start and does not have that problem."
    $inst = @(Get-SnapshotInstances $dir)
    if ($inst.Count -lt 2) { Bad ("only {0} instance folder(s) found in that snapshot" -f $inst.Count); exit 2 }
    $live = $inst
    foreach ($i in $inst) { Write-Host ("  {0,-11} letter={1}" -f $i.Box, $i.Letter) }
    $cfg = Join-Path $dir "native\mp_company_cfg.txt"
    if (Test-Path -LiteralPath $cfg) {
        $mode = (@(Get-Content -LiteralPath $cfg))[0].Trim()
    } else {
        $mode = Get-CompanyMode
        Note ("the snapshot has no mp_company_cfg.txt; using the CURRENT mode '{0}' for A7 -- re-check if the session used the other one" -f $mode)
    }
    Say ("company mode = {0}" -f $mode)
} else {

$running = @(Get-Process TransportFever2 -ErrorAction SilentlyContinue)
if ($Measure) {
    if ($running.Count -eq 0) { Bad "nothing is running to measure"; exit 2 }
    $shotDir = Join-Path $T "shots"
    if (-not (Test-Path $shotDir)) { New-Item -ItemType Directory -Path $shotDir | Out-Null }
    foreach ($i in (Get-RigInstances 0)) {
        if (-not $i.Proc) { continue }
        $r = Get-WinRect $i.Proc.MainWindowHandle
        Write-Host ("  {0,-11} letter={1} pid={2,-6} rect={3},{4} {5}x{6}" -f $i.Box, $i.Letter, $i.Pid, $r.X, $r.Y, $r.W, $r.H)
        $p = Join-Path $shotDir ("measure_{0}_{1}.png" -f $i.Box, $i.Letter)
        & (Join-Path $T "screenshot.ps1") -Path $p -Window TransportFever2 -TargetPid $i.Pid | Out-Null
        Write-Host ("               shot -> {0}" -f $p)
    }
    Write-Host "Fill the measured pixel fractions into `$UiRecipes in tools\soak.ps1." -ForegroundColor DarkGray
    exit 0
}

# NEVER launch on top of somebody else's session. The rig is shared with a human
# and with the other half of this project; relaunching costs them their state and
# truncates the very logs a report about "the last run" needs.
if ($Launch -and $running.Count -gt 0 -and -not $Force) {
    Warn ("{0} instance(s) are already running -- NOT launching (add -Force to override)." -f $running.Count)
    Warn  "Attaching to them instead. That is usually what you want."
    $Launch = $false
}

if ($Launch) {
    if ($Deploy) {
        Say "deploying the mod from the repo (must happen before launch: the Lua is read once at startup)"
        & (Join-Path $T "deploy_mod.ps1") -Quiet
    } else {
        Note "not deploying (-Deploy to force). Testing whatever is currently installed in the game dir."
    }
    Say ("launching the {0}-instance rig to the title menu" -f $Players)
    & (Join-Path $T "mp_menu_launch.ps1") -Players $Players

    # WHY THIS BRANCHES ON PLAYER COUNT -- measured 2026-09-03, do not simplify.
    #
    # The bridge's auto-election is BINARY. bridge_main.cpp: "Whoever claims the
    # host port first is 'a'; the other is 'b'." A THIRD instance runs the same
    # check, finds the host port taken, and also elects 'b'. Seen live: three
    # boxes wrote instance=a / instance=b / instance=b, all logging
    # "[m5] auto identity: port 7771 taken -> instance b".
    #
    # Letters c..h exist only because the LOBBY writes `instance=c` into
    # tpf2_bridge_ctl.txt on join ("[ctl] instance b -> c: re-identifying").
    # So for three or more players the in-game HOST/JOIN flow is not optional
    # decoration -- it is what assigns the identities, AND it is what puts the
    # same save on every instance. Clicking CONTINUE instead gives duplicate
    # letters and three different worlds, which grades as garbage.
    if ($Players -le 2) {
        Say "clicking CONTINUE in each instance to load the save"
        Note "  two-instance mode: the a/b election is automatic. Both instances must have the"
        Note "  SAME save as their profile.lua lastGame, or they load different worlds."
        & (Join-Path $T "click_continue.ps1")
    } else {
        Write-Host ""
        Write-Host "  >>> HOST/JOIN IS REQUIRED FOR $Players PLAYERS, AND IT IS MANUAL." -ForegroundColor White -BackgroundColor DarkBlue
        Write-Host "      In instance A : MULTIPLAYER -> HOST GAME, then START GAME once everyone is in." -ForegroundColor White
        Write-Host "      In every other: MULTIPLAYER -> JOIN GAME (the code is on the clipboard)." -ForegroundColor White
        Write-Host  "      Waiting for $Players distinct lockstep letters with live dashboards..." -ForegroundColor DarkGray
        $ok = $false
        $deadline = (Get-Date).AddMinutes(10)
        while ((Get-Date) -lt $deadline) {
            Start-Sleep -Seconds 5
            $seen = @(Get-RigInstances | Where-Object { $_.Alive })
            $ls = @($seen | ForEach-Object { $_.Letter } | Sort-Object -Unique)
            if ($seen.Count -ge $Players -and $ls.Count -ge $Players) { $ok = $true; break }
        }
        if ($ok) { Say "all instances identified" }
        else { Warn "timed out waiting for the lobby to assign distinct letters -- grading anyway; A2 will say what happened" }
    }
} elseif ($running.Count -eq 0) {
    Bad "no game instances are running, and -Launch was not given."
    Bad "  start the rig yourself (tools\mp_menu_launch.ps1 -Players 3 + host/join), or re-run with -Launch."
    exit 2
} else {
    Say ("attaching to {0} running instance(s)" -f $running.Count)
}

# ------------------------------------------------------- identify the rig
$inst = @(Get-RigInstances)
$live = @()
if ($inst.Count -eq 0) {
    Bad "found game processes but no usable tpf2_instance.txt in any box's data dir."
    Bad "  the bridge DLL writes that file at startup; without it nothing can be graded."
    exit 2
}
Say "instances:"
foreach ($i in $inst) {
    $liveTag = "ALIVE"
    if (-not $i.Alive) { $liveTag = "DEAD" }
    $staleTag = ""
    if ($i.IdStale) { $staleTag = "  (identity file is {0}s old -- stale?)" -f $i.IdAgeSec }
    Write-Host ("  {0,-11} letter={1} pid={2,-6} port={3,-5} {4}{5}" -f $i.Box, $i.Letter, $i.Pid, $i.Port, $liveTag, $staleTag)
}
$live = @($inst | Where-Object { $_.Alive })
if ($live.Count -lt 2) {
    Bad ("only {0} live instance(s) -- a replication test needs at least 2." -f $live.Count)
    exit 2
}
$mode = Get-CompanyMode
$egeoOn = (Get-SliceFlag "dump_egeo" "0") -eq "1"
Say ("company mode = {0}   dump_egeo = {1}" -f $mode, $(if ($egeoOn) { "on" } else { "OFF (A8 will be skipped)" }))

# -------------------------------------------------- wait until it is ready
# A dashboard that is being written NOW is the only proof the game script is
# alive. The native sim hook keeps counting ticks even when the Lua is dead --
# that mistake had the old harness watching a healthy-looking sim while
# replication was silently off.
function Wait-RigReady([int]$Seconds = 120) {
    $deadline = (Get-Date).AddSeconds($Seconds)
    while ((Get-Date) -lt $deadline) {
        $ok = $true
        foreach ($i in $live) {
            $d = Read-Dash $i.DashFile
            if (-not $d) { $ok = $false; break }
            $age = Get-DashAgeSeconds $d
            if ($null -eq $age -or $age -gt 30) { $ok = $false; break }
            if ($d.Peers.Count -lt ($live.Count - 1)) { $ok = $false; break }
        }
        if ($ok) { return $true }
        Start-Sleep -Seconds 3
    }
    return $false
}
if (-not $AssertOnly) {
    Say "waiting for every instance's dashboard to go live and see its peers"
    if (Wait-RigReady 150) { Say "rig is ready" }
    else { Warn "not every instance reported a fresh dashboard with all peers -- continuing; the assertions will say which" }
}

}  # end of the live-rig branch

# ------------------------------------------------------------- baselines
$baseDumps = @()
if (-not $script:Offline) { $baseDumps = @(Get-NewDumps ([datetime]::Now.AddDays(-30))) }
$baseOffsets = @{}
foreach ($i in $inst) {
    # Offline, the whole saved file IS this run's output, so scan it from 0.
    # Live, only what is appended from here on belongs to this run.
    if ($script:Offline) {
        $baseOffsets[$i.Letter] = @{ Inject = 0; Company = 0; Stdout = 0 }
    } else {
        $baseOffsets[$i.Letter] = @{
            Inject  = Get-FileLen $i.InjectFile
            Company = Get-FileLen $i.CompanyLog
            Stdout  = Get-FileLen $i.StdoutFile
        }
    }
}
$basePids   = @($live | ForEach-Object { $_.Pid })
$baseTime   = Get-Date

# ================================================================== actions
# The instance that performs the actions. The native one, because it is the one
# whose window is easiest to address and whose logs are not behind a sandbox.
$actor = $live | Where-Object { -not $_.IsBox } | Select-Object -First 1
if (-not $actor) { $actor = $live[0] }
if ($planned.Count -gt 0) { Say ("actor = instance '{0}' ({1}, pid {2})" -f $actor.Letter, $actor.Box, $actor.Pid) }

# ---------------------------------------------------------------- UI recipes
<#
Window-relative click recipes for the hook-captured channels.

EMPTY, AND NOT BECAUSE NOBODY GOT ROUND TO IT. Measured 2026-09-03 against a
live instance; what follows is what the experiment found, so that the next
person does not spend the evening rediscovering it.

WHAT WORKS
  * Focus + click on the 2D HUD and the construction menus. Clicking the road
    category at (0.4445, 0.947) of the window opens the STREETS panel every
    time, tabs and all. This is the same technique click_continue.ps1 uses on
    the title menu, and it is reliable.
  * Wheel zoom (Invoke-GameWheel). The build tools refuse to act zoomed out and
    say "Zoom in to build" on the HUD; twelve notches at a point brings the
    camera to ground level.
  * Synthetic cursor MOVES reach the 3D view. The terrain cursor ring follows
    SetCursorPos and mouse_event(MOVE|ABSOLUTE) to the exact fraction asked for,
    confirmed across five screenshots.

WHAT DOES NOT WORK
  * Synthetic mouse BUTTONS do not reach the in-world build tool. With the
    STREETS panel open and a street armed, and the camera zoomed in, and the
    ring sitting exactly on the target: click-then-click, press-drag-release,
    and mouse_event(MOVE|ABSOLUTE) before each button all produce NO road, NO
    preview line, and -- decisively -- lockstep_inject_a.txt gains nothing and
    tpf2_slice.log stays at "captured=0 cancelled=0 addHits=0". The same button
    events do drive the 2D UI in the same session, so it is not focus, not DPI,
    and not the coordinates. The 3D tool takes its buttons from somewhere that
    injected input does not reach.

    That is consistent with the whole history of this repo: every piece of GUI
    automation here (autotest.ps1, click_continue.ps1, the menu DLL's own
    Continue click) only ever clicks MENUS. Nothing has ever driven the world.

WHAT TO TRY NEXT, if this is worth another run at
  * SendInput rather than mouse_event (same injection path in theory, cheap to
    rule out), or an SDL2-level injection -- SDL2.dll is in the game dir.
  * A tiny hook in tpf2_slice.dll that calls the tool entry directly. The slice
    already knows the exact caller RVAs the capture requires
    (CALLER_BUILDPROPOSAL 0x459e97, CALLER_UPGRADE 0x4790fc), so a call made
    FROM there would be captured like a player's, which no script can be.
  * Until then, -Manual is the honest answer: a human does the clicking and this
    harness does all the watching and grading.

If you do measure a recipe, add it here. Steps run in order on the actor:
    { Click = @(fx, fy) }   left click at that fraction of the window
    { Wheel = @(fx, fy, n) }zoom n notches at that point
    { Key   = <VK code> }   key press
    { Wait  = <ms> }
An action is only ever reported DONE when the slice actually captured a native
command for it, so a wrong coordinate shows up as NOT PERFORMED, never as a
false pass. Note that Escape disarms whatever tool is selected -- do not put one
between arming the tool and clicking the world.
#>
$UiRecipes = @{
}

function Invoke-UiRecipe($Instance, $Recipe) {
    foreach ($s in $Recipe.Steps) {
        if ($s.ContainsKey('Click')) {
            $ok = Invoke-GameClick -Instance $Instance -FracX $s.Click[0] -FracY $s.Click[1]
            if (-not $ok) { return "a click was skipped -- another window owned that pixel" }
        } elseif ($s.ContainsKey('Wheel')) {
            $ok = Invoke-GameWheel -Instance $Instance -FracX $s.Wheel[0] -FracY $s.Wheel[1] -Notches $s.Wheel[2]
            if (-not $ok) { return "a wheel step was skipped -- another window owned that pixel" }
        } elseif ($s.ContainsKey('Key')) {
            $ok = Invoke-GameKey -Instance $Instance -Vk ([byte]$s.Key)
            if (-not $ok) { return "could not focus the game window for a key press" }
        } elseif ($s.ContainsKey('Wait')) {
            Start-Sleep -Milliseconds $s.Wait
        }
    }
    return $null
}

# ---------------------------------------------------------- run the actions
# The loan amount: stay under the 10,000,000 journal chunk in -Quick (one entry,
# the simple path); cross it in a full run, because that chunking is exactly what
# broke the loan channel before -- cmBookJournal splits at 10,000,000, so a large
# loan lands as several async entries and the poll caught a HALF-APPLIED value
# and shipped it back, ping-ponging the host up to the cap.
$loanAmount = 12000000
if ($Quick) { $loanAmount = 5000000 }

# Every action that actually happened, and whether the OTHER instances applied
# it. Feeds A13. Kept separate from the action list because "the actor did it"
# and "everybody did it" are different facts and only the second one is the
# product working.
$script:ExecChecks = @()
function Confirm-PeersExecuted($Action) {
    $x = Test-ExecEverywhere -Instances $live -Op $Action.ExecOp -Origin $actor.Letter -MinCount 1 -TimeoutSeconds 60
    $counts = ($x.Counts.GetEnumerator() | ForEach-Object { "{0}={1}" -f $_.Key, $_.Value }) -join " "
    $script:ExecChecks += [pscustomobject]@{ Id = $Action.Id; Op = $Action.ExecOp; Ok = $x.Ok; Counts = $counts }
    if ($x.Ok) { Note ("  every peer executed {0}: {1}" -f $Action.ExecOp, $counts) }
    else { Warn ("  NOT every peer executed {0}: {1}" -f $Action.ExecOp, $counts) }
}

foreach ($a in $planned) {
    $off = Get-FileLen $actor.InjectFile

    if ($a.Kind -eq "eval") {
        $r = Invoke-RigLoanAction -Instance $actor -Amount $loanAmount
        Add-Action $a.Id $a.Name $r.Status $r.Detail
        if ($r.Status -eq "DONE") { Confirm-PeersExecuted $a }
        continue
    }

    # ---- hook-captured channel: must be a real native command ----
    $pattern = $script:RigInjectOps['any']
    if ($UiRecipes.ContainsKey($a.Id) -and $UiRecipes[$a.Id].Expect) {
        $pattern = $script:RigInjectOps[$UiRecipes[$a.Id].Expect]
    }

    if ($Manual) {
        Write-Host ""
        Write-Host ("  >>> PERFORM NOW in instance '{0}' ({1}): {2}" -f $actor.Letter, $actor.Box, $a.Name) -ForegroundColor White -BackgroundColor DarkBlue
        Write-Host  "      waiting up to 180 s for the slice to capture a native command..." -ForegroundColor DarkGray
        $w = Wait-RigCapture -Instance $actor -Pattern $pattern -FromOffset $off -TimeoutSeconds 180
        if ($w.Ok) {
            Add-Action $a.Id $a.Name "DONE" ("captured: " + ($w.Lines[0]))
            Confirm-PeersExecuted $a
        } else {
            Add-Action $a.Id $a.Name "NOT PERFORMED" "nothing was captured within 180 s"
        }
        continue
    }

    if (-not $UiRecipes.ContainsKey($a.Id)) {
        Add-Action $a.Id $a.Name "NOT PERFORMED" "in-world clicks do not reach the build tools -- run with -Manual (see `$UiRecipes)"
        continue
    }
    $err = Invoke-UiRecipe $actor $UiRecipes[$a.Id]
    if ($err) { Add-Action $a.Id $a.Name "NOT PERFORMED" $err; continue }
    $w = Wait-RigCapture -Instance $actor -Pattern $pattern -FromOffset $off -TimeoutSeconds 30
    if (-not $w.Ok) {
        Add-Action $a.Id $a.Name "NOT PERFORMED" "the clicks ran but the slice captured no native command -- coordinates are wrong"
        continue
    }
    Add-Action $a.Id $a.Name "DONE" ("captured {0} line(s): {1}" -f $w.Count, $w.Lines[0])
    Confirm-PeersExecuted $a
}

# =================================================================== soak
if (-not $AssertOnly -and -not $script:Offline) {
    Say ("soaking for {0} s so the sim runs on past the actions" -f $SoakSeconds)
    Start-Sleep -Seconds $SoakSeconds
}

# ================================================================ assertions
Write-Host ""
Say "grading"

# ---- A11 needs two samples over time, so take the first one now ----
$dashA = @{}
foreach ($i in $live) { $dashA[$i.Letter] = Read-Dash $i.DashFile }
if (-not $script:Offline) { Start-Sleep -Seconds 20 }
$dashB = @{}
foreach ($i in $live) { $dashB[$i.Letter] = Read-Dash $i.DashFile }

# ---------------- A1 processes ----------------
$nowPids = @(Get-Process TransportFever2 -ErrorAction SilentlyContinue | ForEach-Object Id)
$died = @($basePids | Where-Object { $nowPids -notcontains $_ })
if ($script:Offline) {
    Add-Assertion "A1" "no instance crashed" "SKIP" "offline: a snapshot cannot say whether a process died"
} elseif ($died.Count -gt 0) {
    $who = @()
    foreach ($d in $died) { $who += @($inst | Where-Object { $_.Pid -eq $d } | ForEach-Object { "{0}/{1}" -f $_.Letter, $_.Box }) }
    Add-Assertion "A1" "no instance crashed" "FAIL" ("{0} instance(s) died during the run: {1}" -f $died.Count, ($who -join ", "))
} elseif ($live.Count -lt $Players -and -not $AssertOnly) {
    Add-Assertion "A1" "no instance crashed" "FAIL" ("only {0} of the expected {1} instances were alive to begin with" -f $live.Count, $Players)
} else {
    Add-Assertion "A1" "no instance crashed" "PASS" ("{0} instance(s) alive throughout" -f $live.Count)
}

# ---------------- A2 identities ----------------
$letters = @($live | ForEach-Object { $_.Letter })
$uniq = @($letters | Sort-Object -Unique)
if ($uniq.Count -ne $letters.Count) {
    Add-Assertion "A2" "distinct lockstep letters" "FAIL" ("two instances claim the same letter: " + ($letters -join ", ")) @(
        "the bridge's auto-election is BINARY -- first to claim the host port is 'a', everyone",
        "else is 'b' (bridge_main.cpp). Letters c..h are assigned by the LOBBY writing",
        "instance=<x> into tpf2_bridge_ctl.txt when a player JOINs.",
        "So this almost always means the instances were started without going through the",
        "in-game MULTIPLAYER -> HOST/JOIN flow. Check each box's tpf2_bridge.log for",
        "'[m5] auto identity: port 7771 taken -> instance b'."
    )
} else {
    $stale = @($live | Where-Object { $_.IdStale })
    if ($stale.Count -gt 0) {
        Add-Assertion "A2" "distinct lockstep letters" "FAIL" ("identity file stale for: " + (($stale | ForEach-Object { "{0}/{1} ({2}s)" -f $_.Letter, $_.Box, $_.IdAgeSec }) -join ", "))
    } else {
        Add-Assertion "A2" "distinct lockstep letters" "PASS" ($letters -join ", ")
    }
}

# ---------------- A3 dashboards live ----------------
if ($script:Offline) {
    Add-Assertion "A3" "every game script is alive" "SKIP" "offline: freshness is meaningless in a snapshot"
} else {
    $deadDash = @()
    foreach ($i in $live) {
        $d = $dashB[$i.Letter]
        if (-not $d) { $deadDash += ("{0}/{1}: no dashboard file at {2}" -f $i.Letter, $i.Box, $i.DashFile); continue }
        $age = Get-DashAgeSeconds $d
        if ($null -eq $age) { $deadDash += ("{0}/{1}: dashboard has no wall= line" -f $i.Letter, $i.Box); continue }
        if ($age -gt 60) { $deadDash += ("{0}/{1}: dashboard is {2}s stale" -f $i.Letter, $i.Box, $age) }
    }
    if ($deadDash.Count -gt 0) {
        Add-Assertion "A3" "every game script is alive" "FAIL" "a stale dashboard means that instance's Lua stopped writing" $deadDash
    } else {
        Add-Assertion "A3" "every game script is alive" "PASS" "all dashboards written within the last 60 s"
    }
}

# ---------------- A4 verdict / A5 desyncs / A12 queue ----------------
$notSync = @(); $withDesyncs = @(); $queued = @()
foreach ($i in $live) {
    $d = $dashB[$i.Letter]
    if (-not $d) { continue }
    if ($d['verdict'] -ne 'SYNC') { $notSync += ("{0}: verdict={1}  detail={2}" -f $i.Letter, $d['verdict'], $d['detail']) }
    if ($d['desyncs'] -gt 0)      { $withDesyncs += ("{0}: desyncs={1}" -f $i.Letter, $d['desyncs']) }
    if ($d['queued'] -gt 0)       { $queued += ("{0}: queued={1}" -f $i.Letter, $d['queued']) }
}
if ($notSync.Count -gt 0) { Add-Assertion "A4" "verdict=SYNC everywhere" "FAIL" "at least one instance does not agree with its peers" $notSync }
else { Add-Assertion "A4" "verdict=SYNC everywhere" "PASS" "all SYNC" }

if ($withDesyncs.Count -gt 0) { Add-Assertion "A5" "desyncs=0 everywhere" "FAIL" "the session recorded a divergence, even if it is SYNC now" $withDesyncs }
else { Add-Assertion "A5" "desyncs=0 everywhere" "PASS" "no instance recorded a desync this session" }

if ($queued.Count -gt 0) { Add-Assertion "A12" "no command left queued" "FAIL" "a command never reached its stamp" $queued }
else { Add-Assertion "A12" "no command left queued" "PASS" "every queue drained" }

# ---------------- A6 mutual visibility ----------------
# 'a says SYNC' is not a three-way result: an instance nobody can hear also
# reports SYNC, because it is agreeing with an empty set of peers.
$blind = @()
foreach ($i in $live) {
    $d = $dashB[$i.Letter]
    if (-not $d) { continue }
    foreach ($j in $live) {
        if ($j.Letter -eq $i.Letter) { continue }
        if (-not $d.Peers.ContainsKey($j.Letter)) {
            $blind += ("{0} cannot hear {1} (peers={2})" -f $i.Letter, $j.Letter, $d['PeersRaw'])
        } elseif ($d.Peers[$j.Letter].Verdict -ne 'SYNC') {
            $blind += ("{0} grades {1} as {2}" -f $i.Letter, $j.Letter, $d.Peers[$j.Letter].Verdict)
        }
    }
}
if ($blind.Count -gt 0) { Add-Assertion "A6" "every instance hears every other" "FAIL" "a peer is missing or graded not-SYNC" $blind }
else { Add-Assertion "A6" "every instance hears every other" "PASS" ("full mesh across {0} instances" -f $live.Count) }

# ---------------- A7 money ----------------
if ($mode -ne "coop") {
    Add-Assertion "A7" "balance and loan identical" "SKIP" ("company mode is '{0}': separate wallets are CORRECT there, so this cannot be asserted" -f $mode)
} else {
    $vals = @()
    foreach ($i in $live) {
        $d = $dashB[$i.Letter]
        if ($null -eq $d -or $null -eq $d.Money) { $vals += ("{0}: money unavailable" -f $i.Letter); continue }
        $vals += ("{0}: balance={1} loan={2}" -f $i.Letter, $d.Money, $d.Loan)
    }
    $moneySet = @($live | ForEach-Object { if ($dashB[$_.Letter]) { $dashB[$_.Letter].Money } } | Sort-Object -Unique)
    $loanSet  = @($live | ForEach-Object { if ($dashB[$_.Letter]) { $dashB[$_.Letter].Loan  } } | Sort-Object -Unique)
    if ($moneySet.Count -gt 1 -or $loanSet.Count -gt 1) {
        Add-Assertion "A7" "balance and loan identical" "FAIL" "co-op drives ONE shared company, so a split here is real money divergence" $vals
    } elseif ($moneySet.Count -eq 0) {
        Add-Assertion "A7" "balance and loan identical" "SKIP" "no instance reported a balance"
    } else {
        Add-Assertion "A7" "balance and loan identical" "PASS" ($vals -join "  |  ")
    }
}

# ---------------- A8 egeo geometry ----------------
if (-not $egeoOn) {
    Add-Assertion "A8" "edge geometry identical" "SKIP" "dump_egeo=0 in tpf2_slice.cfg -- set it to 1 for the precise geometry check"
} else {
    $geo = @()
    $missing = @()
    foreach ($i in $live) {
        $g = Read-Egeo $i.EgeoFile
        if (-not $g) { $missing += ("{0}/{1}: no {2}" -f $i.Letter, $i.Box, $i.EgeoFile); continue }
        # A file present in a box's view can be the NATIVE instance's read
        # through, or a leftover from a session in which this box held another
        # letter. Only a file written during this run belongs to this run.
        # (Offline, snapshot_logs already picked the files, and their mtimes are
        # copy times -- there is nothing to check.)
        if (-not $script:Offline -and $g.Mtime -lt $baseTime.AddMinutes(-5)) {
            $missing += ("{0}/{1}: {2} is stale (written {3})" -f $i.Letter, $i.Box, (Split-Path $i.EgeoFile -Leaf), $g.Mtime.ToString("HH:mm:ss"))
            continue
        }
        $geo += [pscustomobject]@{ Letter = $i.Letter; Box = $i.Box; G = $g }
    }
    if ($geo.Count -lt 2) {
        Add-Assertion "A8" "edge geometry identical" "SKIP" "fewer than two fresh egeo dumps to compare" $missing
    } else {
        $hashes = @($geo | ForEach-Object { $_.G.BodyHash } | Sort-Object -Unique)
        $summary = @($geo | ForEach-Object { "{0}: stamp={1} edges={2} body={3}" -f $_.Letter, $_.G.Stamp, $_.G.Lines, $_.G.BodyHash })
        if ($hashes.Count -eq 1) {
            $extra = ""
            if ($missing.Count -gt 0) { $extra = "  (note: " + ($missing -join "; ") + ")" }
            Add-Assertion "A8" "edge geometry identical" "PASS" (("{0} instances agree on {1} edges" -f $geo.Count, $geo[0].G.Lines) + $extra) $summary
        } else {
            $ev = $summary
            $d = Compare-EgeoBodies $geo[0].G $geo[1].G
            foreach ($l in $d.OnlyA) { $ev += ("  only in {0}: {1}" -f $geo[0].Letter, $l) }
            foreach ($l in $d.OnlyB) { $ev += ("  only in {0}: {1}" -f $geo[1].Letter, $l) }
            Add-Assertion "A8" "edge geometry identical" "FAIL" "the worlds' road/rail geometry differs" $ev
        }
    }
}

# ---------------- A9 minidumps ----------------
$newDumps = @()
if (-not $script:Offline) {
    $newDumps = @(Get-NewDumps $baseTime)
    $baseNames = @($baseDumps | ForEach-Object { $_.Path })
    $newDumps = @($newDumps | Where-Object { $baseNames -notcontains $_.Path })
}
if ($script:Offline) {
    Add-Assertion "A9" "no new minidump" "SKIP" "offline: snapshot_logs does not copy crash_dump/*.dmp"
} elseif ($newDumps.Count -gt 0) {
    $ev = @($newDumps | ForEach-Object { "{0}: {1} ({2} KB) at {3}" -f $_.Box, $_.Name, $_.KB, $_.When.ToString("HH:mm:ss") })
    Add-Assertion "A9" "no new minidump" "FAIL" "an engine assert fired (the game may still be running -- a dump is not necessarily a crash)" $ev
} else {
    Add-Assertion "A9" "no new minidump" "PASS" "no .dmp written since the run started"
}

# ---------------- A10 error lines ----------------
# stdout is buffered by the game and is unreliable mid-run, so the live channels
# come first: the dashboard's own event ring and mp_company_<x>.log, both of
# which are flushed as they are written.
$errs = @()
foreach ($i in $live) {
    $o = $baseOffsets[$i.Letter]
    foreach ($src in @(
        @{ Name = "mp_company_$($i.Letter).log"; Text = (Read-SharedText $i.CompanyLog $o.Company) },
        @{ Name = "stdout.txt";                  Text = (Read-SharedText $i.StdoutFile  $o.Stdout ) }
    )) {
        foreach ($h in (Get-BadLines $src.Text 5)) { $errs += ("{0} [{1}] {2}" -f $i.Letter, $src.Name, $h) }
    }
    $d = $dashB[$i.Letter]
    if ($d) {
        foreach ($e in $d.Ev) {
            if ($e -match 'DIVERGENCE|success=false|ERROR|!! ') { $errs += ("{0} [dash ev] {1}" -f $i.Letter, $e) }
        }
    }
}
if ($errs.Count -gt 0) {
    Add-Assertion "A10" "no divergence or Lua errors" "FAIL" ("{0} suspicious log line(s)" -f $errs.Count) (@($errs | Select-Object -First 12))
} else {
    Add-Assertion "A10" "no divergence or Lua errors" "PASS" "clean logs across every instance"
}

# ---------------- A11 the sim actually moved ----------------
# Process.Responding is the WRONG probe and this was measured: a bad vehicle
# config wedges the SIM thread while the render thread keeps pumping messages,
# so the game answers the UI and looks healthy while its world has stopped.
$frozen = @(); $paused = @()
foreach ($i in $live) {
    $a = $dashA[$i.Letter]; $b = $dashB[$i.Letter]
    if (-not $a -or -not $b) { continue }
    if ($b['paused'] -eq 'yes') { $paused += $i.Letter; continue }
    if ($null -ne $a['t'] -and $a['t'] -eq $b['t']) {
        $frozen += ("{0}: game time stuck at {1} for 20 s (speed={2}, paused={3})" -f $i.Letter, $b['t'], $b['speed'], $b['paused'])
    }
}
if ($script:Offline) {
    Add-Assertion "A11" "the simulation advanced" "SKIP" "offline: a snapshot is one moment, so nothing can be seen to advance"
} elseif ($frozen.Count -gt 0) {
    Add-Assertion "A11" "the simulation advanced" "FAIL" "a wedged sim still reports SYNC, because nothing is changing" $frozen
} elseif ($paused.Count -eq $live.Count) {
    Add-Assertion "A11" "the simulation advanced" "SKIP" ("every instance is PAUSED ({0}) -- unpause to make this meaningful" -f ($paused -join ","))
} else {
    Add-Assertion "A11" "the simulation advanced" "PASS" "game time advanced on every running instance"
}

# ---------------- A13 every action reached every peer ----------------
# The point of the product. The world-state assertions (A4/A5/A8) would
# eventually catch a lost command as a divergence, but this names the ACTION
# that went missing, which is the difference between "something is wrong" and
# "the batch line-assign dropped half the vehicles again".
if ($script:ExecChecks.Count -eq 0) {
    if ($planned.Count -eq 0) {
        Add-Assertion "A13" "every action reached every peer" "SKIP" "no actions were run"
    } else {
        Add-Assertion "A13" "every action reached every peer" "SKIP" "no action got far enough to check"
    }
} else {
    $missed = @($script:ExecChecks | Where-Object { -not $_.Ok })
    if ($missed.Count -gt 0) {
        $ev = @($missed | ForEach-Object { "{0} ({1}): peers that logged it -- {2}" -f $_.Id, $_.Op, $_.Counts })
        $ev += "counts come from each peer's dashboard ev= ring, which holds only the last 8 events;"
        $ev += "a very busy run can evict one, so cross-check the peer's mp_company_<x>.log before"
        $ev += "concluding the command was lost."
        Add-Assertion "A13" "every action reached every peer" "FAIL" ("{0} action(s) were not seen to execute on every peer" -f $missed.Count) $ev
    } else {
        Add-Assertion "A13" "every action reached every peer" "PASS" (($script:ExecChecks | ForEach-Object { "{0}:{1}" -f $_.Id, $_.Op }) -join ", ")
    }
}

# ==================================================================== report
$fails = @($script:Assertions | Where-Object { $_.Status -eq "FAIL" })
$skips = @($script:Assertions | Where-Object { $_.Status -eq "SKIP" })
$notDone = @($script:Actions | Where-Object { $_.Status -ne "DONE" })

Write-Host ""
Write-Host "======================= RESULT =======================" -ForegroundColor Cyan
foreach ($a in $script:Assertions) {
    $c = "Green"
    if ($a.Status -eq "FAIL") { $c = "Red" }
    if ($a.Status -eq "SKIP") { $c = "DarkYellow" }
    Write-Host ("  {0,-4} {1,-5} {2,-34} {3}" -f $a.Id, $a.Status, $a.Name, $a.Detail) -ForegroundColor $c
    if ($a.Status -ne "PASS") {
        foreach ($e in $a.Evidence) { Write-Host ("         | " + $e) -ForegroundColor DarkGray }
    }
}

if ($script:Actions.Count -gt 0) {
    Write-Host ""
    Write-Host "  --- coverage: what this run actually exercised ---" -ForegroundColor Cyan
    foreach ($a in $script:Actions) {
        $c = "Green"
        if ($a.Status -ne "DONE") { $c = "Yellow" }
        Write-Host ("  {0,-10} {1,-14} {2}" -f $a.Id, $a.Status, $a.Name) -ForegroundColor $c
    }
    if ($notDone.Count -gt 0) {
        Write-Host ("  {0} of {1} planned action(s) did NOT run. Those channels are UNTESTED by this run." -f $notDone.Count, $script:Actions.Count) -ForegroundColor Yellow
        Write-Host  "  The assertions above still graded the world -- they just had less to grade." -ForegroundColor DarkGray
    }
} elseif ($AssertOnly) {
    Write-Host ""
    Write-Host "  --- coverage: NONE. -AssertOnly grades the world as it stands; no action was performed. ---" -ForegroundColor Yellow
}

$verdict = "PASS"
if ($fails.Count -gt 0) { $verdict = "FAIL" }
Write-Host ""
Write-Host ("  OVERALL: {0}   ({1} passed, {2} failed, {3} skipped)" -f $verdict, `
    @($script:Assertions | Where-Object { $_.Status -eq "PASS" }).Count, $fails.Count, $skips.Count) `
    -ForegroundColor $(if ($verdict -eq "FAIL") { "Red" } else { "Green" })
if ($fails.Count -gt 0) {
    Write-Host  "  failed:" -ForegroundColor Red
    foreach ($f in $fails) { Write-Host ("    {0} {1} -- {2}" -f $f.Id, $f.Name, $f.Detail) -ForegroundColor Red }
}

# ------------------------------------------------------------ keep evidence
# The game truncates stdout.txt and the mod truncates its data files on the next
# launch, so a failure that is not snapshotted now cannot be investigated later
# (a B-vs-C divergence was lost exactly this way on 2026-09-02).
if ($fails.Count -gt 0 -and -not $script:Offline) {
    Write-Host ""
    Say "FAIL -- snapshotting every instance's logs before anything restarts"
    $tag = "soak-fail"
    & (Join-Path $T "snapshot_logs.ps1") -Tag $tag
    $runs = Join-Path $env:LOCALAPPDATA "tpf2mp\runs"
    $latest = Get-ChildItem $runs -Directory -EA SilentlyContinue | Sort-Object LastWriteTime | Select-Object -Last 1
    if ($latest) {
        $script:EvidenceDir = $latest.FullName
        # Write the verdict INTO the snapshot, so the folder explains itself
        # months later without this console output.
        $rep = @()
        $rep += "soak.ps1 run at $($script:RunStart.ToString('yyyy-MM-dd HH:mm:ss'))"
        $rep += "verdict: $verdict"
        $rep += ""
        $rep += "instances:"
        foreach ($i in $inst) { $rep += ("  {0,-11} letter={1} pid={2} alive={3}" -f $i.Box, $i.Letter, $i.Pid, $i.Alive) }
        $rep += ""
        $rep += "assertions:"
        foreach ($a in $script:Assertions) {
            $rep += ("  {0,-4} {1,-5} {2} -- {3}" -f $a.Id, $a.Status, $a.Name, $a.Detail)
            foreach ($e in $a.Evidence) { $rep += ("        | " + $e) }
        }
        $rep += ""
        $rep += "actions:"
        foreach ($a in $script:Actions) { $rep += ("  {0,-10} {1,-14} {2}" -f $a.Id, $a.Status, $a.Detail) }
        Set-Content -Path (Join-Path $latest.FullName "soak_report.txt") -Value $rep -Encoding ASCII
        Write-Host ("  evidence: {0}" -f $latest.FullName) -ForegroundColor Yellow
        Write-Host  "            (logs, dashboards, egeo dumps, and soak_report.txt)" -ForegroundColor DarkGray
    }
}

if ($CloseWhenDone) {
    Say "closing the games (sandboxed Steam left running)"
    Get-Process TransportFever2 -EA SilentlyContinue | Stop-Process -Force -EA SilentlyContinue
}

exit $(if ($verdict -eq "FAIL") { 1 } else { 0 })
