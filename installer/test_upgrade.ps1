<#
.SYNOPSIS
Prove that installing, upgrading and uninstalling the MSI leaves the game folder
exactly as it was found -- in a throwaway folder, never the real game.

.DESCRIPTION
The install touches one file the player cannot replace themselves: the game's own
alut.dll, which is renamed to alut_real.dll so our proxy can forward to it. Every
upgrade therefore has to answer one question: is alut_real.dll still the STOCK
library, or did a previous proxy get wrapped around itself? This script answers it
by running the real msiexec transactions against a stand-in folder whose "stock"
alut.dll has known content.

It checks, in order:
  1. install            -> proxy in place, alut_real.dll == the stock content
  2. upgrade (same MSI)  -> still ONE entry in Apps, version updated, alut_real.dll
                            STILL the stock content (not a proxy)
  3. uninstall          -> alut.dll back to the stock content, our files gone

Needs elevation: the package is per-machine (ALLUSERS=1), so a non-elevated run
fails with error 1925 and nothing is tested. Close the game first -- msiexec
cannot replace files the game holds open.

    powershell -ExecutionPolicy Bypass -File installer\test_upgrade.ps1
#>
[CmdletBinding()]
param(
    [string]$Msi = (Join-Path $PSScriptRoot "out\TpF2Multiplayer.msi"),
    [string]$UpgradeMsi,          # optional second package; defaults to $Msi (same-version upgrade)
    [switch]$KeepSandbox
)

$ErrorActionPreference = "Stop"
$fail = 0
function Ok($m)   { Write-Host "  PASS  $m" -ForegroundColor Green }
function Bad($m)  { Write-Host "  FAIL  $m" -ForegroundColor Red; $script:fail++ }
function Step($m) { Write-Host "`n[$m]" -ForegroundColor Cyan }

if (-not ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()
        ).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    throw "Run this from an elevated PowerShell: a per-machine MSI cannot install otherwise (error 1925)."
}
if (Get-Process TransportFever2 -EA SilentlyContinue) { throw "Close Transport Fever 2 first." }
if (-not (Test-Path $Msi)) { throw "No MSI at $Msi -- build it with installer\build_msi.ps1" }
if (-not $UpgradeMsi) { $UpgradeMsi = $Msi }

$STOCK = "STOCK-ALUT-STANDIN-DO-NOT-EDIT"
$sandbox = Join-Path $env:TEMP ("tpf2mp_upgrade_" + (Get-Random))
$game = Join-Path $sandbox "game"
New-Item -ItemType Directory -Force $game | Out-Null
Set-Content (Join-Path $game "alut.dll") $STOCK -Encoding ascii -NoNewline
Set-Content (Join-Path $game "TransportFever2.exe") "" -Encoding ascii
Write-Host "sandbox: $game"

function Msi([string[]]$a, [string]$logName) {
    $log = Join-Path $sandbox $logName
    $p = Start-Process msiexec -ArgumentList ($a + @('/qn', '/l*v', $log)) -Wait -PassThru
    if ($p.ExitCode -ne 0) {
        Write-Host "  msiexec exit $($p.ExitCode); log tail:" -ForegroundColor Yellow
        Get-Content $log -Tail 12 | ForEach-Object { "    $_" }
    }
    $p.ExitCode
}
function Arp {
    Get-ItemProperty 'HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall\*',
                     'HKLM:\SOFTWARE\WOW6432Node\Microsoft\Windows\CurrentVersion\Uninstall\*' -EA SilentlyContinue |
        Where-Object { $_.DisplayName -eq 'TpF2 Multiplayer' }
}
function AlutReal { if (Test-Path "$game\alut_real.dll") { (Get-Content "$game\alut_real.dll" -Raw) } else { $null } }

try {
    Step "1. install"
    $rc = Msi @('/i', $Msi, "INSTALLFOLDER=$game\", 'TPF2_SKIP_GAMEDIR_CHECK=1') "install.log"
    if ($rc -eq 0) { Ok "installed" } else { Bad "install exit $rc" }
    if (Test-Path "$game\tpf2_menu.dll") { Ok "our files are present" } else { Bad "tpf2_menu.dll missing" }
    if ((AlutReal) -eq $STOCK) { Ok "alut_real.dll is the stock library" } else { Bad "alut_real.dll is NOT the stock library" }
    if ((Get-Content "$game\alut.dll" -Raw) -ne $STOCK) { Ok "alut.dll replaced by the proxy" } else { Bad "alut.dll is still the stock file -- the proxy did not install" }
    $entries = @(Arp); if ($entries.Count -eq 1) { Ok "one entry in Apps (v$($entries[0].DisplayVersion))" } else { Bad "$($entries.Count) entries in Apps" }

    Step "2. upgrade"
    $rc = Msi @('/i', $UpgradeMsi, "INSTALLFOLDER=$game\", 'TPF2_SKIP_GAMEDIR_CHECK=1') "upgrade.log"
    if ($rc -eq 0) { Ok "upgraded" } else { Bad "upgrade exit $rc" }
    $entries = @(Arp)
    if ($entries.Count -eq 1) { Ok "still one entry in Apps (v$($entries[0].DisplayVersion)) -- no side-by-side leftovers" }
    else { Bad "$($entries.Count) entries in Apps after the upgrade" }
    # The point of the whole exercise: the stock library must survive every upgrade.
    if ((AlutReal) -eq $STOCK) { Ok "alut_real.dll is STILL the stock library" }
    else { Bad "alut_real.dll was overwritten -- a proxy got wrapped around itself" }
    if (Test-Path "$game\tpf2_menu.dll") { Ok "our files are present after the upgrade" } else { Bad "files missing after the upgrade" }

    Step "3. uninstall"
    $code = (Arp | Select-Object -First 1).PSChildName
    $rc = Msi @('/x', $code) "uninstall.log"
    if ($rc -eq 0) { Ok "uninstalled" } else { Bad "uninstall exit $rc" }
    if ((Get-Content "$game\alut.dll" -Raw -EA SilentlyContinue) -eq $STOCK) { Ok "alut.dll restored to the stock library" }
    else { Bad "alut.dll was NOT restored" }
    if (-not (Test-Path "$game\alut_real.dll")) { Ok "alut_real.dll cleaned up" } else { Bad "alut_real.dll left behind" }
    foreach ($f in 'tpf2_menu.dll','tpf2_slice.dll','tpf2_bridge_mp.dll','netpunch') {
        if (Test-Path (Join-Path $game $f)) { Bad "$f left behind" }
    }
    if (@(Arp).Count -eq 0) { Ok "no entry left in Apps" } else { Bad "still listed in Apps" }
}
finally {
    if ($KeepSandbox) { Write-Host "`nsandbox kept: $sandbox" }
    else { Remove-Item $sandbox -Recurse -Force -EA SilentlyContinue }
}

Write-Host ""
if ($fail) { Write-Host "$fail check(s) FAILED" -ForegroundColor Red; exit 1 }
Write-Host "all checks passed" -ForegroundColor Green
