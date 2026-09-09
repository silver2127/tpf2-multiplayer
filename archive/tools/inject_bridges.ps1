<#
.SYNOPSIS
Injects the v7 bridge DLLs into the two Transport Fever 2 instances, choosing
the target for each by whether the process is sandboxed rather than by the
order you happened to start them in.

.DESCRIPTION
The a and b bridges share one directory and both write tpf2_instance.txt, which
the mod reads to learn which instance it is. Injecting them into the wrong
processes therefore does not fail loudly -- it points an instance at the wrong
pair of capture/event files and looks exactly like a dead bridge. This script
refuses to inject unless it can positively identify which process is boxed.

.PARAMETER Box
Sandboxie box name holding the second instance. Default: GameAgent.

.PARAMETER DryRun
Report what would be injected without doing it.
#>
[CmdletBinding()]
param(
    [string]$Box = "GameAgent",
    [switch]$DryRun
)

$ErrorActionPreference = "Stop"

$OutDir   = "C:\Program Files (x86)\Steam\steamapps\workshop\content\1066780\3710243057\recon\m4\out"
$Injector = Join-Path $PSScriptRoot "..\injector\injector.exe"
$DllA     = Join-Path $OutDir "tpf2_bridge_a7.dll"
$DllB     = Join-Path $OutDir "tpf2_bridge_b7H.dll"
$SbieStart = "C:\Program Files\Sandboxie-Plus\Start.exe"

function Fail($msg) { Write-Host "ABORT: $msg" -ForegroundColor Red; exit 1 }

foreach ($p in @($Injector, $DllA, $DllB)) {
    if (-not (Test-Path $p)) { Fail "missing required file: $p" }
}
$Injector = (Resolve-Path $Injector).Path

# ---- find the two game processes ------------------------------------------
$procs = @(Get-Process -Name "TransportFever2" -ErrorAction SilentlyContinue)
if ($procs.Count -eq 0) { Fail "no TransportFever2.exe running -- start both instances first" }
if ($procs.Count -ne 2) {
    Fail "expected exactly 2 TransportFever2.exe processes, found $($procs.Count) (pids: $($procs.Id -join ', '))"
}

# ---- decide which one is sandboxed ----------------------------------------
# Authoritative source is Sandboxie itself: Start.exe /listpids prints a count
# followed by one pid per line.
$boxedPids = @()
$sbieWorked = $false
if (Test-Path $SbieStart) {
    try {
        $raw = & $SbieStart "/box:$Box" /listpids 2>$null
        $nums = @($raw | ForEach-Object { $_.Trim() } |
                  Where-Object { $_ -match '^\d+$' } | ForEach-Object { [int]$_ })
        if ($nums.Count -ge 1) {
            # first value is the count; the rest are pids
            $boxedPids = @($nums | Select-Object -Skip 1)
            $sbieWorked = $true
        }
    } catch { $sbieWorked = $false }
}

if (-not $sbieWorked) {
    # fallback: a boxed process has Sandboxie's dll mapped into it
    Write-Host "note: Start.exe /listpids unavailable, falling back to module scan" -ForegroundColor Yellow
    foreach ($p in $procs) {
        try {
            if ($p.Modules | Where-Object { $_.ModuleName -like "Sbie*.dll" }) { $boxedPids += $p.Id }
        } catch {
            Fail "cannot read modules of pid $($p.Id) (try running this elevated)"
        }
    }
}

$boxed   = @($procs | Where-Object { $boxedPids -contains $_.Id })
$unboxed = @($procs | Where-Object { $boxedPids -notcontains $_.Id })

if ($boxed.Count -ne 1 -or $unboxed.Count -ne 1) {
    Fail ("could not tell the two instances apart (boxed={0}, unboxed={1}). " -f $boxed.Count, $unboxed.Count)
}

$pidA = $unboxed[0].Id   # instance A = normal process
$pidB = $boxed[0].Id     # instance B = sandboxed process

Write-Host ""
Write-Host "instance A (normal)    pid $pidA  <- tpf2_bridge_a7.dll"
Write-Host "instance B (sandboxed) pid $pidB  <- tpf2_bridge_b7H.dll"
Write-Host ""

if ($DryRun) { Write-Host "dry run -- nothing injected."; exit 0 }

# ---- inject ----------------------------------------------------------------
& $Injector $pidA $DllA
if ($LASTEXITCODE -ne 0) { Fail "injection into A (pid $pidA) failed" }
& $Injector $pidB $DllB
if ($LASTEXITCODE -ne 0) { Fail "injection into B (pid $pidB) failed" }

# ---- verify identity landed correctly on both sides ------------------------
Start-Sleep -Seconds 1
$realId = Join-Path $OutDir "tpf2_instance.txt"
$sbxOut = "C:\Sandbox\$env:USERNAME\$Box\drive\C" + $OutDir.Substring(2)
$sbxId  = Join-Path $sbxOut "tpf2_instance.txt"

function ReadInstance($path) {
    if (-not (Test-Path $path)) { return "(missing)" }
    $first = (Get-Content $path -TotalCount 1)
    if ($null -eq $first) { return "(empty)" }
    return $first.Trim()
}

$idA = ReadInstance $realId
$idB = ReadInstance $sbxId

Write-Host ""
Write-Host "identity check:"
Write-Host "  real dir        -> '$idA'  (expected 'a')"
Write-Host "  sandbox overlay -> '$idB'  (expected 'b')"

if ($idA -eq "a" -and $idB -eq "b") {
    Write-Host "OK: both instances homed correctly." -ForegroundColor Green
} else {
    Write-Host "WRONG: identities are not a/b. Check tpf2_bridge.log for the" -ForegroundColor Red
    Write-Host "       '[m5] WARNING: identity file said ...' line." -ForegroundColor Red
    exit 1
}
