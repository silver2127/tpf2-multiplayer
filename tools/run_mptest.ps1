<#
.SYNOPSIS
Drives an automated replication test across the two running game instances.

.DESCRIPTION
Writes a scenario for the host to execute, waits, then collects evidence from
both sides and reports what actually crossed the wire:

  host   capture file   - did the host observe and ship its own actions?
  joiner events file    - did those lines arrive?
  joiner results/stdout - did the joiner replay them?

Instance A writes to the real workshop out dir; instance B is sandboxed, so its
files live in the Sandboxie overlay. Both are read here.

.PARAMETER Scenario
Scenario file to run. Default: tools\scenarios\basic_replication.txt

.PARAMETER Target
Which instance executes the scenario (default a, the host).

.PARAMETER WaitSeconds
How long to let it run before collecting results.
#>
[CmdletBinding()]
param(
    [string]$Scenario = "",
    [ValidateSet("a", "b")][string]$Target = "a",
    [int]$WaitSeconds = 60,
    [string]$Box = "GameAgent"
)

$ErrorActionPreference = "Stop"

$Out = "C:\Program Files (x86)\Steam\steamapps\workshop\content\1066780\3710243057\recon\m4\out"
$Overlay = "C:\Sandbox\$env:USERNAME\$Box\drive\C" + $Out.Substring(2)
if (-not $Scenario) { $Scenario = Join-Path $PSScriptRoot "scenarios\basic_replication.txt" }
if (-not (Test-Path $Scenario)) { Write-Host "no such scenario: $Scenario" -ForegroundColor Red; exit 1 }

# ACTOR is the side that executes the scenario; PEER is the side that must
# receive it. Everything below is expressed in those terms.
#
# It used to be written as host=a / joiner=b throughout, with -Target only
# choosing where the scenario file was dropped. So `-Target b` ran the actions
# in B and then measured A's capture file against B's events file -- i.e. it
# reported on the A->B direction while B was the one acting. It could not fail
# for a B->A fault, which is the direction that has never been tested and where
# every real-play bug has surfaced.
$actor = $Target
$peer  = if ($Target -eq "a") { "b" } else { "a" }
# instance a runs unsandboxed (real dir); instance b inside the overlay
function DirFor($inst) { if ($inst -eq "a") { $Out } else { $Overlay } }
$actorDir = DirFor $actor
$peerDir  = DirFor $peer
$targetDir = $actorDir     # kept: the scenario is dropped in the actor's dir

$StdoutFor = @{
    a = "C:\Program Files (x86)\Steam\userdata\*\1066780\local\crash_dump\stdout.txt"
    b = "C:\Sandbox\$env:USERNAME\$Box\drive\C\Program Files (x86)\Steam\userdata\*\1066780\local\crash_dump\stdout.txt"
}
function ResolveStdout($inst) {
    $f = Get-ChildItem $StdoutFor[$inst] -EA SilentlyContinue | Select-Object -First 1
    if ($f) { return $f.FullName } else { return $null }
}
$peerStdout = ResolveStdout $peer

foreach ($d in @( ,@($actor, $actorDir) ; ,@($peer, $peerDir) )) {
    if (-not (Test-Path $d[1])) {
        Write-Host "instance $($d[0]) dir not found: $($d[1])" -ForegroundColor Red
        Write-Host "(is it running? for 'b' the sandbox overlay only exists once it has written something)"
        exit 1
    }
}
Write-Host "direction: $actor -> $peer" -ForegroundColor Cyan

# Both must tolerate a $null path: the peer's stdout does not exist until the
# game has written to it, and with $ErrorActionPreference='Stop' a bare
# `Test-Path $null` is a terminating error that would abort the whole run.
function FileLen($p) { if ($p -and (Test-Path $p)) { (Get-Item $p).Length } else { 0 } }
function TailFrom($p, $off) {
    if (-not $p -or -not (Test-Path $p)) { return "" }
    $fs = [System.IO.File]::Open($p, 'Open', 'Read', 'ReadWrite')
    $fs.Seek([Math]::Min($off, $fs.Length), 'Begin') | Out-Null
    $sr = New-Object System.IO.StreamReader($fs)
    $t = $sr.ReadToEnd(); $sr.Close(); $fs.Close(); return $t
}

# mark every file we are going to read, so we only see this run's output
$actorCaptureFile = Join-Path $actorDir "tpf2_capture_$actor.txt"
$actorResultsFile = Join-Path $actorDir "mp_test_results_$actor.txt"
$peerEventsFile   = Join-Path $peerDir  "tpf2_events_$peer.txt"
$marks = @{
    actorCapture = FileLen $actorCaptureFile
    actorResults = FileLen $actorResultsFile
    peerEvents   = FileLen $peerEventsFile
    peerStdout   = FileLen $peerStdout
    bridgeLog    = FileLen (Join-Path $actorDir "tpf2_bridge.log")
}

$scenarioText = Get-Content $Scenario -Raw
$dest = Join-Path $targetDir "mp_test_scenario_$Target.txt"
[System.IO.File]::WriteAllText($dest, $scenarioText)
Write-Host "scenario -> $dest"
Write-Host "running for $WaitSeconds s ..." -ForegroundColor Cyan

$deadline = (Get-Date).AddSeconds($WaitSeconds)
while ((Get-Date) -lt $deadline) {
    Start-Sleep -Seconds 2
    # $actorDir, NOT $Out. This polled the real dir unconditionally, so with
    # -Target b it watched a file instance B never writes: the early break could
    # never fire and every B-side run burned the full -WaitSeconds.
    $r = TailFrom $actorResultsFile $marks.actorResults
    if ($r -match "scenario complete") { break }
}

# Clear the scenario as soon as the run is over. Leaving it in place means any
# instance that later boots with this identity replays the whole thing into its
# world -- which has now happened three times, including into the sandboxed
# game after an a/b role swap.
[System.IO.File]::WriteAllText($dest, "# cleared by run_mptest`r`n")

Write-Host ""
Write-Host "==== actor ${actor}: scenario results ====" -ForegroundColor Yellow
$res = TailFrom $actorResultsFile $marks.actorResults
if ($res.Trim()) { $res.TrimEnd() } else { Write-Host "(nothing - is the mod loaded and the instance identified?)" -ForegroundColor Red }

Write-Host ""
Write-Host "==== actor ${actor}: lines shipped (capture file) ====" -ForegroundColor Yellow
$cap = TailFrom $actorCaptureFile $marks.actorCapture
$capLines = @($cap -split "`n" | Where-Object { $_.Trim() })
"$($capLines.Count) new line(s)"
$capLines | ForEach-Object { "  " + $_.Substring(0, [Math]::Min(110, $_.Length)) }

Write-Host ""
Write-Host "==== peer ${peer}: lines received (events file) ====" -ForegroundColor Yellow
$ev = TailFrom $peerEventsFile $marks.peerEvents
$evLines = @($ev -split "`n" | Where-Object { $_.Trim() })
"$($evLines.Count) new line(s)"
$evLines | ForEach-Object { "  " + $_.Substring(0, [Math]::Min(110, $_.Length)) }

# ---- what the peer actually DID with them ----
# Arrival was the old stopping point, and it is not the same as replication:
# a line can land in the events file and then be refused, disabled by a flag,
# or thrown out by a Lua error. Every one of those printed "OK: everything the
# host shipped arrived" and left a human to spot the difference in stdout.
Write-Host ""
Write-Host "==== peer ${peer}: replay outcome (stdout) ====" -ForegroundColor Yellow
$peerLog = TailFrom $peerStdout $marks.peerStdout
$peerLines = @($peerLog -split "`n" | Where-Object { $_ -match "^\[mpb-" })
$ok    = @($peerLines | Where-Object { $_ -match 'success=true|already present|ok=true' })
$bad   = @($peerLines | Where-Object { $_ -match 'success=false|ok=false|replay error|replay skipped|disabled --' })
$alive = @($peerLines | Where-Object { $_ -match 'alive ticks=' })
"replayed ok  : $($ok.Count)"
"failed       : $($bad.Count)"
$bad | Select-Object -First 12 | ForEach-Object { Write-Host ("  " + $_.Trim()) -ForegroundColor Red }

Write-Host ""
Write-Host "==== verdict ====" -ForegroundColor Yellow
$rx = '^(BUILD|EDGE|EDGEDEL|EDGEMOD|DEMOLISH|CONMOD|LINE|VEH) '
$sent = ($capLines | Where-Object { $_ -match $rx }).Count
$recv = ($evLines  | Where-Object { $_ -match $rx }).Count
"direction    : $actor -> $peer"
"actor shipped: $sent replication line(s)"
"peer got     : $recv"
"peer replayed: $($ok.Count) ok, $($bad.Count) failed"

$fail = $false
# A peer whose Lua died looks identical to a peer with nothing to do -- both are
# silent. Checked BEFORE the counts, because a dead peer explains every other
# number and should not be reported as a replication bug.
#
# Liveness is ANY [mpb-] output, with the beacon only as the fallback for a peer
# that genuinely had nothing to say. Requiring the beacon alone was wrong twice
# over: update() ticks at 10 Hz only while the sim is RUNNING, and these runs are
# done paused, so a 120 s scenario reached tick ~120 on the joiner. It failed a
# run in which that same joiner had just replayed a station edit ok=true.
if ($peerLines.Count -eq 0) {
    Write-Host "FAIL: peer $peer produced NO [mpb-] output at all - its game script is dead or never loaded" -ForegroundColor Red
    Write-Host "      (the native sim hook can still be ticking; that proves nothing about the mod)" -ForegroundColor Red
    $fail = $true
} elseif ($alive.Count -eq 0) {
    Write-Host "note: peer $peer is alive ($($peerLines.Count) log line(s)) but never beaconed - sim likely paused" -ForegroundColor DarkGray
}
if ($sent -eq 0) {
    Write-Host "FAIL: actor $actor captured nothing - actions did not run, or capture is broken" -ForegroundColor Red
    $fail = $true
} elseif ($recv -eq 0) {
    Write-Host "FAIL: nothing crossed the wire - check tpf2_bridge.log for the tail/net lines" -ForegroundColor Red
    $fail = $true
} elseif ($recv -lt $sent) {
    Write-Host "PARTIAL: $recv of $sent arrived at $peer" -ForegroundColor DarkYellow
    $fail = $true
}
if ($bad.Count -gt 0) {
    Write-Host "FAIL: peer $peer refused $($bad.Count) replay(s) - see the lines above" -ForegroundColor Red
    $fail = $true
} elseif ($recv -gt 0 -and $ok.Count -eq 0) {
    Write-Host "FAIL: $recv line(s) arrived at $peer but it replayed NOTHING" -ForegroundColor Red
    $fail = $true
}
if (-not $fail) {
    Write-Host "OK: $sent line(s) $actor -> $peer, all arrived, $($ok.Count) replayed, none refused." -ForegroundColor Green
}

Write-Host ""
Write-Host "peer stdout: $peerStdout"
exit $(if ($fail) { 1 } else { 0 })
