<#
.SYNOPSIS
Installs (or removes) the alut.dll proxy that loads the bridge before the game
reaches its title screen.

.DESCRIPTION
alut.dll is a static import of TransportFever2.exe, so Windows maps it before
the exe's entry point runs -- earlier than the title menu is built, which is
what the Multiplayer menu entry needs. This script renames the stock library to
alut_real.dll and installs our forwarding proxy in its place. All 20 exports are
forwarded straight through, so game audio is unaffected.

This modifies a file inside the game installation. It is fully reversible with
-Uninstall, and Steam's "verify integrity of game files" will also restore the
stock DLL (and delete the proxy).

.PARAMETER Uninstall
Restore the stock alut.dll and remove the proxy.
#>
[CmdletBinding()]
param([switch]$Uninstall)

$ErrorActionPreference = "Stop"

$Game  = "C:\Program Files (x86)\Steam\steamapps\common\Transport Fever 2"
$Out   = "C:\Program Files (x86)\Steam\steamapps\workshop\content\1066780\3710243057\recon\m4\out"
$Proxy = Join-Path $PSScriptRoot "..\bridge\out\alut.dll"
$Live  = Join-Path $Game "alut.dll"
$Real  = Join-Path $Game "alut_real.dll"
$Backup = Join-Path $PSScriptRoot "..\backup\2026-08-06\alut.dll.orig"

function Fail($m) { Write-Host "ABORT: $m" -ForegroundColor Red; exit 1 }

if ($Uninstall) {
    if (-not (Test-Path $Real)) { Fail "alut_real.dll not found - proxy does not appear to be installed" }
    Remove-Item $Live -Force
    Rename-Item $Real "alut.dll"
    Write-Host "Stock alut.dll restored; proxy removed." -ForegroundColor Green
    exit 0
}

if (-not (Test-Path $Proxy)) { Fail "proxy not built - run bridge\build_proxy.bat first" }
if (-not (Test-Path (Join-Path $Out "tpf2_bridge_mp.dll"))) {
    Fail "tpf2_bridge_mp.dll missing from $Out - the proxy loads it from there"
}
# Never overwrite an existing alut_real.dll: on a re-run that would replace the
# only surviving copy of the stock library with our proxy.
if (Test-Path $Real) { Fail "alut_real.dll already exists - proxy already installed? use -Uninstall first" }

$hash = (Get-FileHash $Live).Hash
New-Item -ItemType Directory -Force -Path (Split-Path $Backup) | Out-Null
Copy-Item $Live $Backup -Force
Write-Host "backed up stock alut.dll -> $Backup"
Write-Host "  sha256 $hash"

Rename-Item $Live "alut_real.dll"
Copy-Item $Proxy $Live -Force

if ((Get-FileHash $Real).Hash -ne $hash) { Fail "stock dll changed during install - check manually!" }

Write-Host ""
Write-Host "installed:" -ForegroundColor Green
Get-ChildItem $Game -Filter "alut*.dll" | Select-Object Name, Length | Format-Table -AutoSize
Write-Host "Start the game; $Out\tpf2_proxy.log should gain a line before the menu appears."
Write-Host "Revert with:  install_proxy.ps1 -Uninstall"
