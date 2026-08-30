<#
.SYNOPSIS
Run the extraction scripts against the already-analysed Ghidra project.

.DESCRIPTION
Fast (seconds to minutes) because -noanalysis reuses the analysis that
ghidra_analyze.ps1 already paid 24 minutes for. Safe to re-run.

  ExportClassMap    -> classes.csv, class_methods.csv, vftables.csv
  DecompileTargets  -> <label>.c and <label>.fields.txt per target

The .fields.txt files are the actual product. A decompiler renders an unknown
struct as *(int *)(param_1 + 0xe8), and it is that offset -- cross-checked
against the live bytes in docs/re/GROUND_TRUTH_applyProposal.md -- that turns a
guess into a layout.
#>
param(
    [string]$ProjDir  = "C:\tools\ghidra_proj",
    [string]$ProjName = "TpF2",
    [string]$Program  = "TransportFever2.exe",
    [string]$OutDir   = "C:\tools\ghidra_out",
    [string]$Ghidra   = "C:\tools\ghidra_12.1.2_PUBLIC",
    [string]$Jdk      = "C:\Program Files\Eclipse Adoptium\jdk-21.0.12.8-hotspot",
    [string]$Targets  = "",
    [switch]$SkipClassMap
)

$ErrorActionPreference = "Stop"
$T = $PSScriptRoot
if (-not $Targets) { $Targets = Join-Path $T "ghidra_targets.txt" }
if (-not (Test-Path $OutDir)) { New-Item -ItemType Directory -Path $OutDir -Force | Out-Null }

$env:JAVA_HOME = $Jdk
$env:GHIDRA_HEADLESS_MAXMEM = "16G"

$scriptPath = Join-Path $T "ghidra_scripts"
$headless   = Join-Path $Ghidra "support\analyzeHeadless.bat"

function Invoke-GhidraScript($name, $scriptArgs) {
    Write-Host "[extract] $name $($scriptArgs -join ' ')" -ForegroundColor Cyan
    $a = @(
        $ProjDir, $ProjName,
        "-process", $Program,
        "-noanalysis",
        "-scriptPath", $scriptPath,
        "-postScript", $name
    ) + $scriptArgs
    & $headless @a 2>&1 | Where-Object {
        # analyzeHeadless is extremely chatty; keep script output and real errors
        $_ -match '^\[|ERROR|Exception|INFO  REPORT' -and $_ -notmatch 'INFO  REPORT: Analysis'
    }
}

if (-not $SkipClassMap) {
    Invoke-GhidraScript "ExportClassMap.java" @($OutDir)
}
Invoke-GhidraScript "DecompileTargets.java" @($Targets, (Join-Path $OutDir "decomp"), "180")

Write-Host "[extract] done -> $OutDir" -ForegroundColor Cyan
Get-ChildItem $OutDir -Recurse -File -EA SilentlyContinue |
    Select-Object @{n='file';e={$_.FullName.Replace($OutDir,'')}}, @{n='KB';e={[int]($_.Length/1KB)}} |
    Format-Table -AutoSize
