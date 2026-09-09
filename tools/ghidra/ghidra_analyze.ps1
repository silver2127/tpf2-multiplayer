<#
.SYNOPSIS
Import TransportFever2.exe into a Ghidra project and run full auto-analysis.

.DESCRIPTION
This is the long pole: 69.5 MB, 138,112 functions, no PDB. Expect hours, not
minutes. Run it once and leave it; everything else (struct extraction, function
documentation) works against the analysed project afterwards and is fast.

Settings that are NOT defaults, and why:
  * GHIDRA_HEADLESS_MAXMEM=48G -- analyzeHeadless.bat defaults to 2G, which is
    nowhere near enough for a binary this size; it thrashes or dies partway and
    leaves a half-analysed project that looks fine until a lookup comes back
    empty. There is 94 GB on this machine.
  * ParallelGCThreads/CICompilerCount -- the launcher hardcodes 2 of each.
    That is a sensible default for a laptop and a waste of a 32-core box.

No PDB exists for a commercial release build, so Ghidra leans on RTTI to
recover class names -- which is exactly what M1 exploited to turn 138k
anonymous functions into named targets. The RTTI analyser is on by default.
#>
param(
    [string]$Exe      = "C:\Program Files (x86)\Steam\steamapps\common\Transport Fever 2\TransportFever2.exe",
    [string]$ProjDir  = "C:\tools\ghidra_proj",
    [string]$ProjName = "TpF2",
    [string]$Ghidra   = "C:\tools\ghidra_12.1.2_PUBLIC",
    [string]$Jdk      = "C:\Program Files\Eclipse Adoptium\jdk-21.0.12.8-hotspot"
)

$ErrorActionPreference = "Stop"
if (-not (Test-Path $Exe))    { Write-Host "no exe: $Exe" -ForegroundColor Red; exit 1 }
if (-not (Test-Path $Ghidra)) { Write-Host "no ghidra: $Ghidra" -ForegroundColor Red; exit 1 }
if (-not (Test-Path $ProjDir)) { New-Item -ItemType Directory -Path $ProjDir -Force | Out-Null }

# Analyse a COPY at a path with no spaces or parentheses.
#
# analyzeHeadless is a .bat, and cmd's argument parsing mangles
# "C:\Program Files (x86)\..." no matter how PowerShell quotes it -- the
# parentheses terminate the expression and it died with
# "\Steam\steamapps\common\Transport was unexpected at this time".
# Ghidra copies the binary into the project anyway, so nothing is lost.
$work = "C:\tools\bin"
if (-not (Test-Path $work)) { New-Item -ItemType Directory -Path $work -Force | Out-Null }
$localExe = Join-Path $work (Split-Path $Exe -Leaf)
if (-not (Test-Path $localExe) -or (Get-Item $localExe).Length -ne (Get-Item $Exe).Length) {
    Write-Host "[ghidra] staging binary -> $localExe" -ForegroundColor DarkGray
    Copy-Item $Exe $localExe -Force
}
$Exe = $localExe

$env:JAVA_HOME = $Jdk
$env:GHIDRA_HEADLESS_MAXMEM = "48G"
$env:GHIDRA_HEADLESS_JAVA_OPTIONS = "-XX:ParallelGCThreads=8 -XX:CICompilerCount=4"

$log       = Join-Path $ProjDir "analyze.log"
$scriptLog = Join-Path $ProjDir "script.log"

Write-Host "[ghidra] importing $(Split-Path $Exe -Leaf) -> $ProjName" -ForegroundColor Cyan
Write-Host "[ghidra] heap=$($env:GHIDRA_HEADLESS_MAXMEM)  log=$log" -ForegroundColor DarkGray
$sw = [Diagnostics.Stopwatch]::StartNew()

& (Join-Path $Ghidra "support\analyzeHeadless.bat") `
    $ProjDir $ProjName `
    -import $Exe `
    -log $log -scriptlog $scriptLog `
    -overwrite

$sw.Stop()
Write-Host ("[ghidra] finished in {0:N1} min (exit {1})" -f $sw.Elapsed.TotalMinutes, $LASTEXITCODE) -ForegroundColor Cyan
