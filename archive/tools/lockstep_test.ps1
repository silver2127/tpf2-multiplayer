<#
.SYNOPSIS
Prove the lockstep loop: one command, executed by BOTH instances at the same
game-time stamp, with no desync.

.DESCRIPTION
Injects a road command into instance A. A does NOT execute it on the spot -- it
stamps it for game time now+EXEC_DELAY, ships it, and queues it like any peer
would. The pass condition is not "the road appeared on both"; it is that both
executed the SAME command at the SAME stamp and their world hashes still agree.

That distinction is the whole point. The previous state-diff design also made
the road appear on both sides, and the worlds still drifted, because each side
applied a different thing at a different moment.

Checks, in the order they can fail:
  1. A scheduled it            SCHED ... at=<stamp>
  2. B received it             RECV  ... at=<stamp>
  3. BOTH executed it          EXEC ROAD seq=<n> origin=a at=<stamp>
  4. Same stamp on both        (a mismatch means the clock is not shared)
  5. No DESYNC lines           hashes agreed at every checkpoint
#>
param(
    # One or more commands, injected in order. Defaults exercise every op the
    # prototype supports; ROAD and RAIL are laid apart so a failure is a genuine
    # rejection rather than the two colliding with each other.
    [string[]]$Commands = @(
        "ROAD 1200 -4200 1320 -4200",
        "RAIL 1200 -4400 1320 -4400 1 0",
        "CON depot/road_depot_era_a.con 1500 -4200"
    ),
    [int]$WaitSeconds = 300,
    [string]$Target = "a"
)

$ErrorActionPreference = "Stop"
$Base    = "C:\Program Files (x86)\Steam\steamapps\workshop\content\1066780\3710243057\recon\m4\out"
$Overlay = "C:\Sandbox\$env:USERNAME\GameAgent\drive\C" + $Base.Substring(2)
$StdoutA = "C:\Program Files (x86)\Steam\userdata\*\1066780\local\crash_dump\stdout.txt"
$StdoutB = "C:\Sandbox\$env:USERNAME\GameAgent\drive\C\Program Files (x86)\Steam\userdata\*\1066780\local\crash_dump\stdout.txt"

function Read-Shared($pattern) {
    $f = Get-ChildItem $pattern -EA SilentlyContinue | Select-Object -First 1
    if (-not $f) { return "" }
    $fs = [System.IO.File]::Open($f.FullName, 'Open', 'Read', 'ReadWrite')
    $sr = New-Object System.IO.StreamReader($fs)
    $t = $sr.ReadToEnd(); $sr.Close(); $fs.Close()
    return $t
}

# mark both logs so only this run's lines are graded
$markA = (Read-Shared $StdoutA).Length
$markB = (Read-Shared $StdoutB).Length

$dir = if ($Target -eq "a") { $Base } else { $Overlay }
$inject = Join-Path $dir "lockstep_inject_$Target.txt"
# APPEND, never overwrite. The mod primes its read offset to the file's current
# size at startup; truncating would make the offset exceed the size, which
# resyncs to the new end and eats the line we just wrote.
foreach ($cmd in $Commands) {
    Add-Content -Path $inject -Value $cmd -Encoding ASCII
    Write-Host "[lockstep] injected into $Target : $cmd" -ForegroundColor Cyan
}
Write-Host "[lockstep] waiting up to $WaitSeconds s for $($Commands.Count) command(s) ..." -ForegroundColor DarkGray

# EXEC lines carry the op, so one regex covers every command type.
$rx = 'EXEC (\w+) seq=(\d+) origin=(\w+) at=(\d+) '
$deadline = (Get-Date).AddSeconds($WaitSeconds)
while ((Get-Date) -lt $deadline) {
    Start-Sleep -Seconds 5
    $ta = (Read-Shared $StdoutA); $tb = (Read-Shared $StdoutB)
    $na = $ta.Substring([Math]::Min($markA, $ta.Length))
    $nb = $tb.Substring([Math]::Min($markB, $tb.Length))
    $cA = ([regex]::Matches($na, $rx)).Count
    $cB = ([regex]::Matches($nb, $rx)).Count
    if ($cA -ge $Commands.Count -and $cB -ge $Commands.Count) { break }
}

$ta = (Read-Shared $StdoutA); $tb = (Read-Shared $StdoutB)
$na = $ta.Substring([Math]::Min($markA, $ta.Length))
$nb = $tb.Substring([Math]::Min($markB, $tb.Length))

function Show($tag, $txt) {
    Write-Host "==== instance $tag ====" -ForegroundColor Yellow
    foreach ($m in [regex]::Matches($txt, '\[ls-\w\] (SCHED|RECV|EXEC|SYNC|!! DESYNC|BARRIER)[^\r\n]*')) {
        Write-Host ("  " + $m.Value)
    }
}
Show "A" $na
Show "B" $nb

Write-Host ""
Write-Host "================ LOCKSTEP VERDICT ================" -ForegroundColor Cyan
$fail = $false

# Build "op/seq@stamp" keys per side and compare the SETS. Comparing sets, not
# just counts, is what catches the failure that matters: both sides executing
# the same NUMBER of commands while disagreeing about which, or about when.
function ExecSet($txt) {
    $m = @{}
    foreach ($x in [regex]::Matches($txt, 'EXEC (\w+) seq=(\d+) origin=(\w+) at=(\d+)')) {
        $k = "{0}/{1}@{2}" -f $x.Groups[1].Value, $x.Groups[2].Value, $x.Groups[4].Value
        $m[$k] = $true
    }
    return $m
}
$setA = ExecSet $na
$setB = ExecSet $nb
Write-Host ("  executed: A={0} B={1} of {2} injected" -f $setA.Count, $setB.Count, $Commands.Count)

$onlyA = @($setA.Keys | Where-Object { -not $setB.ContainsKey($_) })
$onlyB = @($setB.Keys | Where-Object { -not $setA.ContainsKey($_) })
$both  = @($setA.Keys | Where-Object { $setB.ContainsKey($_) })

foreach ($k in ($both | Sort-Object)) { Write-Host ("    both: {0}" -f $k) -ForegroundColor Green }
foreach ($k in $onlyA) { Write-Host ("    A ONLY: {0}" -f $k) -ForegroundColor Red }
foreach ($k in $onlyB) { Write-Host ("    B ONLY: {0}" -f $k) -ForegroundColor Red }

if ($setA.Count -eq 0 -or $setB.Count -eq 0) {
    Write-Host "  FAIL: at least one instance executed nothing" -ForegroundColor Red; $fail = $true
} elseif ($onlyA.Count -gt 0 -or $onlyB.Count -gt 0) {
    Write-Host "  FAIL: the two disagree on WHICH command ran at WHICH stamp --" -ForegroundColor Red
    Write-Host "        that is replication, not lockstep." -ForegroundColor Red
    $fail = $true
} elseif ($both.Count -lt $Commands.Count) {
    Write-Host ("  PARTIAL: {0} of {1} injected commands ran (in step, but some never fired)" -f
        $both.Count, $Commands.Count) -ForegroundColor DarkYellow
    $fail = $true
} else {
    Write-Host "  OK: every command executed on BOTH at the SAME game-time stamp" -ForegroundColor Green
}

# A build the game refuses is a game rejection, not a lockstep failure -- but
# the worlds only stay in step if BOTH refused it identically.
$badA = ([regex]::Matches($na, 'EXEC \w+ [^\r\n]*success=false')).Count
$badB = ([regex]::Matches($nb, 'EXEC \w+ [^\r\n]*success=false')).Count
if ($badA -gt 0 -or $badB -gt 0) {
    Write-Host ("  NOTE: builds refused by the game: A={0} B={1}" -f $badA, $badB) -ForegroundColor DarkYellow
    if ($badA -ne $badB) {
        Write-Host "  FAIL: refused on one side but not the other -- the worlds have diverged" -ForegroundColor Red
        $fail = $true
    }
}

$dsA = ([regex]::Matches($na, 'DESYNC')).Count
$dsB = ([regex]::Matches($nb, 'DESYNC')).Count
$syA = ([regex]::Matches($na, '\] SYNC ')).Count
$syB = ([regex]::Matches($nb, '\] SYNC ')).Count
Write-Host ("  hash checkpoints agreeing : A={0} B={1}" -f $syA, $syB)
Write-Host ("  desyncs                   : A={0} B={1}" -f $dsA, $dsB)
if ($dsA -gt 0 -or $dsB -gt 0) {
    Write-Host "  FAIL: the worlds diverged" -ForegroundColor Red; $fail = $true
} elseif ($syA -eq 0 -and $syB -eq 0) {
    Write-Host "  NOTE: no hash checkpoint compared yet -- run longer before trusting 'in sync'." -ForegroundColor DarkYellow
}

Write-Host ("  RESULT: {0}" -f $(if ($fail) { "FAIL" } else { "PASS" })) `
    -ForegroundColor $(if ($fail) { "Red" } else { "Green" })
exit $(if ($fail) { 1 } else { 0 })
