<#
.SYNOPSIS
Run ONE Ghidra headless script against the already-analysed TpF2 project.

.DESCRIPTION
ghidra_extract.ps1 hardcodes ExportClassMap + DecompileTargets. This is the
generic form, so a new query does not need a new wrapper.

  ghidra_run.ps1 DumpStringXrefs.java C:\tools\ghidra_out 4
  ghidra_run.ps1 FindTypeUsage.java   C:\tools\ghidra_out\x Builder Toolkit

A Ghidra project is locked to ONE process at a time: never run two of these
concurrently, the second will fail on the lock file.
#>
param(
    [Parameter(Mandatory = $true, Position = 0)][string]$Script,
    [Parameter(ValueFromRemainingArguments = $true)][string[]]$ScriptArgs,
    [string]$ProjDir  = "C:\tools\ghidra_proj",
    [string]$ProjName = "TpF2",
    [string]$Program  = "TransportFever2.exe",
    [string]$Ghidra   = "C:\tools\ghidra_12.1.2_PUBLIC",
    [string]$Jdk      = "C:\Program Files\Eclipse Adoptium\jdk-21.0.12.8-hotspot"
)

$ErrorActionPreference = "Stop"
$env:JAVA_HOME = $Jdk
$env:GHIDRA_HEADLESS_MAXMEM = "16G"

$scriptPath = Join-Path $PSScriptRoot "ghidra_scripts"
$headless   = Join-Path $Ghidra "support\analyzeHeadless.bat"

$a = @(
    $ProjDir, $ProjName,
    "-process", $Program,
    "-noanalysis",
    "-scriptPath", $scriptPath,
    "-postScript", $Script
) + $ScriptArgs

Write-Host "[run] $Script $($ScriptArgs -join ' ')" -ForegroundColor Cyan
& $headless @a 2>&1 | Where-Object {
    $_ -match '^\[|ERROR|Exception|INFO  REPORT' -and $_ -notmatch 'INFO  REPORT: Analysis'
}
