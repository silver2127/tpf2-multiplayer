<#
.SYNOPSIS
M3 determinism gate: compare the two instances' state-hash sequences.

.DESCRIPTION
This is the experiment REPORT.md section 7 calls "the make-or-break" and which was
never run to a conclusion. Two instances load the SAME save and receive NO
input. If their hash sequences ever diverge, the simulation is not deterministic
and lockstep multiplayer is off the table until the cause is found and forced.

Reads "M3 i=<n> day=<d> hash=<h> len=<l>" lines from each instance's stdout and
lines them up by ordinal i, not by wall time -- one process always starts a
moment after the other.

Reporting rules, chosen so this cannot flatter itself:
  * Zero samples on either side is INCONCLUSIVE, never a pass. A run where the
    probe never fired looks identical to a run that matched perfectly if you
    only compare sets.
  * Only the overlapping prefix is compared. If one side has 40 samples and the
    other 12, the verdict covers 12.
  * The FIRST divergence is what matters; everything after it is downstream
    noise. It is printed with its day number so it can be reproduced.
#>
param(
    [string]$StdoutA = "C:\Program Files (x86)\Steam\userdata\*\1066780\local\crash_dump\stdout.txt",
    [string]$StdoutB = "C:\Sandbox\$env:USERNAME\GameAgent\drive\C\Program Files (x86)\Steam\userdata\*\1066780\local\crash_dump\stdout.txt"
)

$ErrorActionPreference = "Stop"

function Read-Shared($pattern) {
    $f = Get-ChildItem $pattern -EA SilentlyContinue | Select-Object -First 1
    if (-not $f) { return $null }
    # shared read: the game holds these open while running
    $fs = [System.IO.File]::Open($f.FullName, 'Open', 'Read', 'ReadWrite')
    $sr = New-Object System.IO.StreamReader($fs)
    $t = $sr.ReadToEnd(); $sr.Close(); $fs.Close()
    return $t
}

function Get-Samples($text) {
    $out = @{}
    if (-not $text) { return $out }
    foreach ($m in [regex]::Matches($text, 'M3 i=(\d+) day=(-?\d+) hash=(\S+) len=(\d+)')) {
        $i = [int]$m.Groups[1].Value
        $out[$i] = [pscustomobject]@{
            Day  = [int]$m.Groups[2].Value
            Hash = $m.Groups[3].Value
            Len  = [int]$m.Groups[4].Value
        }
    }
    return $out
}

$ta = Read-Shared $StdoutA
$tb = Read-Shared $StdoutB
$a = Get-Samples $ta
$b = Get-Samples $tb

Write-Host "=========== M3 DETERMINISM GATE ===========" -ForegroundColor Cyan
Write-Host ("  instance A samples : {0}" -f $a.Count)
Write-Host ("  instance B samples : {0}" -f $b.Count)

if ($a.Count -eq 0 -or $b.Count -eq 0) {
    Write-Host "  RESULT: INCONCLUSIVE -- one or both instances produced no samples." -ForegroundColor DarkYellow
    Write-Host "          The probe only samples when the in-game clock advances, so the" -ForegroundColor DarkYellow
    Write-Host "          usual cause is a paused sim. Check for 'M3 unpausing' / 'M3 probe live'." -ForegroundColor DarkYellow
    foreach ($p in @( ,@("A", $ta) ; ,@("B", $tb) )) {
        $hit = if ($p[1]) { ([regex]::Matches($p[1], 'M3 (probe live|unpausing|running)[^\r\n]*') | Select-Object -Last 1).Value } else { $null }
        Write-Host ("          {0}: {1}" -f $p[0], $(if ($hit) { $hit } else { "no M3 lines at all -- is the mod deployed?" }))
    }
    exit 2
}

# Align on the in-game DAY, not the sample ordinal.
#
# Ordinal alignment is only valid if both instances began sampling at the same
# world time, and they do not start together -- B launches second, so it can sit
# 40 samples behind A in wall-clock while covering the same in-game days. Line
# those up by ordinal and you compare day X against day Y and report a
# divergence that is really a bookkeeping error. The day is the world clock, so
# it is what makes two samples genuinely comparable.
$byDayA = @{}; foreach ($k in $a.Keys) { $byDayA[$a[$k].Day] = $a[$k] }
$byDayB = @{}; foreach ($k in $b.Keys) { $byDayB[$b[$k].Day] = $b[$k] }
$common = ($byDayA.Keys | Where-Object { $byDayB.ContainsKey($_) } | Sort-Object)
if (-not $common) {
    Write-Host "  RESULT: INCONCLUSIVE -- the two instances share no in-game days yet." -ForegroundColor DarkYellow
    Write-Host ("          A covers days {0}..{1}, B covers {2}..{3}. Let the later one catch up." -f
        ($byDayA.Keys | Measure-Object -Minimum).Minimum, ($byDayA.Keys | Measure-Object -Maximum).Maximum,
        ($byDayB.Keys | Measure-Object -Minimum).Minimum, ($byDayB.Keys | Measure-Object -Maximum).Maximum) -ForegroundColor DarkYellow
    exit 2
}
$a = $byDayA; $b = $byDayB

$firstBad = $null
$matched = 0
foreach ($i in $common) {
    if ($a[$i].Hash -eq $b[$i].Hash) { $matched++ }
    elseif (-not $firstBad) { $firstBad = $i }
}

Write-Host ("  compared           : {0} overlapping sample(s)" -f @($common).Count)
Write-Host ("  identical          : {0}" -f $matched)

if ($firstBad) {
    Write-Host ("  RESULT: DIVERGED at in-game day {0}" -f $firstBad) -ForegroundColor Red
    Write-Host ("     A: hash={0} len={1}" -f $a[$firstBad].Hash, $a[$firstBad].Len) -ForegroundColor Red
    Write-Host ("     B: hash={0} len={1}" -f $b[$firstBad].Hash, $b[$firstBad].Len) -ForegroundColor Red
    if ($a[$firstBad].Len -ne $b[$firstBad].Len) {
        Write-Host "     NOTE: snapshot LENGTHS differ, so the worlds hold a different NUMBER" -ForegroundColor Yellow
        Write-Host "     of things -- an entity count divergence, not just a value drift." -ForegroundColor Yellow
    }
    Write-Host "  => Lockstep is not viable until this is found and forced deterministic." -ForegroundColor Red
    Write-Host "     REPORT.md section 6 ranks the suspects: multithreaded ECS first, then float," -ForegroundColor DarkGray
    Write-Host "     then unseeded RNG, then hash iteration order." -ForegroundColor DarkGray
    exit 1
}

Write-Host ("  RESULT: IDENTICAL across all {0} compared samples" -f $matched) -ForegroundColor Green
Write-Host "  => No divergence observed. This is necessary, not sufficient: it covers" -ForegroundColor DarkGray
Write-Host ("     in-game days {0}..{1} on ONE machine with no input. Longer runs, player" -f
    $common[0], $common[-1]) -ForegroundColor DarkGray
Write-Host "     actions, and a second machine each remain untested." -ForegroundColor DarkGray
exit 0
