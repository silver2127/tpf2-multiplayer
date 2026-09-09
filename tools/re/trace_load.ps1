# trace_load.ps1 -- ETW capture of a Transport Fever 2 save load.
#
# WHY THIS EXISTS
# tools/profile_load.py samples instruction pointers, which shows where threads
# ARE but not what they are waiting FOR. On a big-map load that limit is binding:
# the one thread burning a core is 73% inside RtlSleepConditionVariableSRW, i.e.
# spin-waiting on a condition variable. RIP sampling cannot name who holds the
# lock, and it cannot see kernel work (page faults committing ~19 GB are
# invisible to user-mode sampling, because a fault does not move RIP).
#
# ETW sees both:
#   CPU                -> context switches AND the ReadyThread chain: which thread
#                         made which other thread runnable. That is the
#                         "who holds the lock" answer.
#   VirtualAllocation  -> every VirtualAlloc/Free with sizes.
#   ReferenceSet       -> hard/soft page faults and working-set behaviour.
#
# TWO CAPTURE MODES
#   RING (default)  Memory buffers that WRAP. Run it for as long as you like --
#                   start before the load, stop when the load finishes -- and you
#                   keep the MOST RECENT slice, which is the slow tail we care
#                   about. Bounded size, always parseable.
#   -FileMode       Streams everything to disk. Complete, but a long CPU trace
#                   runs to many GB, and Windows Performance Analyzer is NOT
#                   installed here -- only tracerpt, which cannot slice by time,
#                   so a huge .etl becomes an unparseable XML expansion.
#                   Use only for short, deliberate captures.
#
# REQUIRES AN ELEVATED SHELL (kernel providers need SeSystemProfilePrivilege).
# This machine has Windows PowerShell 5.1 only -- "powershell", NOT "pwsh".
#
#   Run as administrator, then:
#     powershell -ExecutionPolicy Bypass -File tools\trace_load.ps1 -Manual
#         start it, trigger the load, press a key once the load has FINISHED.
#         Recommended: you get the tail of the load regardless of how long it took.
#
#     powershell -ExecutionPolicy Bypass -File tools\trace_load.ps1 -Seconds 1800
#         same, but stops itself after a fixed time.
#
#     ... -Light        CPU only. Smaller, still answers who-unblocked-whom.
#     ... -FileMode     full capture to disk (see the warning above).
#
# The .etl lands in %LOCALAPPDATA%\tpf2mp\traces.
param(
    [int]$Seconds = 1800,
    [switch]$Manual,
    [switch]$Light,
    [switch]$FileMode,
    [string]$OutDir = "$env:LOCALAPPDATA\tpf2mp\traces"
)
$ErrorActionPreference = 'Stop'

$isAdmin = ([Security.Principal.WindowsPrincipal] `
            [Security.Principal.WindowsIdentity]::GetCurrent()
           ).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
if (-not $isAdmin) {
    Write-Host "ETW kernel tracing needs an ELEVATED shell." -ForegroundColor Yellow
    Write-Host "Open PowerShell as administrator and run:"
    Write-Host "    powershell -ExecutionPolicy Bypass -File `"$PSCommandPath`" -Manual" -ForegroundColor Cyan
    exit 1
}

New-Item -ItemType Directory -Force $OutDir | Out-Null
$stamp = Get-Date -Format 'yyyyMMdd-HHmmss'
$etl = Join-Path $OutDir "load-$stamp.etl"

# A stale session would make -start fail; cancelling is harmless if none runs.
#
# NO "2>&1" HERE. In Windows PowerShell 5.1, redirecting a NATIVE command's
# stderr wraps every line in a NativeCommandError ErrorRecord -- and with
# $ErrorActionPreference='Stop' that is terminating, so the script died here even
# though wpr -cancel had done its job. Let stderr go to the console instead, and
# judge success by $LASTEXITCODE, which is what we actually care about.
$ErrorActionPreference = 'Continue'
& wpr -cancel | Out-Null
$ErrorActionPreference = 'Stop'

# Ring (memory) mode only supports SOME profiles. wpr rejects the whole start
# with 0xc5584017 ("The Logging Mode does not match with the profile passed") if
# any profile is file-only -- VirtualAllocation and ReferenceSet both are.
# CPU is the one that carries ReadyThread, which is the answer we actually came
# for, so ring mode quietly narrows to CPU rather than failing. The memory
# profiles need -FileMode and a deliberately short capture.
$profiles = if ($Light -or -not $FileMode) { @('CPU') } else { @('CPU', 'VirtualAllocation', 'ReferenceSet') }
if (-not $FileMode -and -not $Light) {
    Write-Host "[trace] ring mode supports CPU only -- VirtualAllocation/ReferenceSet need -FileMode." -ForegroundColor DarkGray
    Write-Host "[trace] CPU still carries ReadyThread (who unblocked whom), which is the goal." -ForegroundColor DarkGray
}
$wprArgs = @()
foreach ($p in $profiles) { $wprArgs += @('-start', $p) }
if ($FileMode) { $wprArgs += '-filemode' }

$mode = if ($FileMode) { 'FILE (everything, can get huge)' } else { 'RING (keeps the most recent slice)' }
Write-Host "[trace] profiles: $($profiles -join ', ')" -ForegroundColor Green
Write-Host "[trace] mode    : $mode" -ForegroundColor Green
$ErrorActionPreference = 'Continue'
& wpr @wprArgs
$ErrorActionPreference = 'Stop'
if ($LASTEXITCODE -ne 0 -and $profiles.Count -gt 1) {
    Write-Host "[trace] multi-profile start failed ($LASTEXITCODE) -- retrying CPU only." -ForegroundColor Yellow
    $ErrorActionPreference = 'Continue'
    & wpr -cancel | Out-Null
    if ($FileMode) { & wpr -start CPU -filemode } else { & wpr -start CPU }
    $ErrorActionPreference = 'Stop'
    $profiles = @('CPU')
}
if ($LASTEXITCODE -ne 0) {
    Write-Host "[trace] wpr -start failed ($LASTEXITCODE)." -ForegroundColor Red
    exit 1
}

$games = @(Get-Process TransportFever2 -EA SilentlyContinue)
Write-Host "[trace] $($games.Count) game instance(s): $($games.Id -join ', ')"
Write-Host ""
Write-Host "  >>> NOW: trigger the save load. <<<" -ForegroundColor Cyan
if (-not $FileMode) {
    Write-Host "  (ring mode -- it is fine to let this run the whole load;" -ForegroundColor DarkGray
    Write-Host "   stopping keeps the most recent window, i.e. the slow tail)" -ForegroundColor DarkGray
}
Write-Host ""

if ($Manual) {
    Write-Host "[trace] recording... press any key once the load has FINISHED."
    $null = $Host.UI.RawUI.ReadKey('NoEcho,IncludeKeyDown')
} else {
    $t0 = Get-Date
    while (((Get-Date) - $t0).TotalSeconds -lt $Seconds) {
        Start-Sleep -Seconds 15
        $el = [int]((Get-Date) - $t0).TotalSeconds
        $busy = @(Get-Process TransportFever2 -EA SilentlyContinue |
                  Where-Object { $_.PrivateMemorySize64 -gt 2GB }).Count
        Write-Host ("[trace] {0,5}s elapsed   {1} instance(s) over 2 GB" -f $el, $busy)
    }
}

Write-Host "[trace] stopping and flushing (can take a minute)..."
$ErrorActionPreference = 'Continue'
& wpr -stop $etl
$rc = $LASTEXITCODE
$ErrorActionPreference = 'Stop'
if ($rc -ne 0) { throw "wpr -stop failed ($rc)" }
if (-not (Test-Path $etl)) { throw "wpr -stop reported success but wrote no file" }

$sizeMB = [math]::Round((Get-Item $etl).Length / 1MB, 1)
Write-Host ""
Write-Host "[trace] wrote $etl  ($sizeMB MB)" -ForegroundColor Green
Write-Host "[trace] next: python tools\trace_summary.py `"$etl`" --dump"
