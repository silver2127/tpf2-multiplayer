<#
.SYNOPSIS
Prove that TpF2 Big Maps and TpF2 Multiplayer can be installed over each other
in EITHER order and removed in EITHER order, and that the game folder ends up
exactly as it was found -- in a throwaway folder, never the real game.

.DESCRIPTION
Both packages replace the game's own alut.dll with a proxy and ship the same
plugin host, declared under identical component GUIDs (PluginHost.wxs) so that
Windows Installer reference-counts them. The custom actions that park and
restore the stock alut.dll are refcount-aware for the same reason. This script
runs the real msiexec transactions against a stand-in folder whose "stock"
alut.dll has known content and checks, for each order:

  install A        -> proxy in, alut_real.dll == stock, A's files present
  install B over A -> proxy still in, alut_real.dll STILL stock (not a proxy
                      wrapped around itself), both products' files present,
                      both listed in Apps
  uninstall A      -> B's files present, proxy STILL in, alut_real.dll STILL
                      stock, host DLL still present, A's own files gone
  uninstall B      -> alut.dll back to stock content, alut_real.dll gone, every
                      packaged file gone, nothing listed in Apps

Order 1 is Multiplayer then Big Maps; order 2 is the reverse. It also tracks the
shared Segment Heap registry value (present while either product is installed,
gone after both) and puts back whatever value the machine had before the test,
because that value is real and machine-wide even though the folder is not.

Needs elevation: both packages are per-machine (ALLUSERS=1); a non-elevated run
fails with error 1925 and nothing is tested. Close the game first -- msiexec
cannot replace files the game holds open.

    powershell -ExecutionPolicy Bypass -File installer\test_coexist.ps1
#>
[CmdletBinding()]
param(
    [string]$BigMsi = "",
    [string]$MpMsi  = "",
    [switch]$KeepSandbox
)
$ErrorActionPreference = "Stop"
# $PSScriptRoot is not populated inside param() defaults on PowerShell 5.1
# when run with -File, so resolve the default paths here instead.
$Here = if ($PSScriptRoot) { $PSScriptRoot } else { Split-Path -Parent $MyInvocation.MyCommand.Path }
if (-not $BigMsi) { $BigMsi = Join-Path $Here "out\TpF2BigMaps.msi" }
if (-not $MpMsi)  { $MpMsi  = Join-Path (Split-Path -Parent (Split-Path -Parent $Here)) "tpf2-multiplayer\installer\out\TpF2Multiplayer.msi" }
$fail = 0
function Ok($m)   { Write-Host "  PASS  $m" -ForegroundColor Green }
function Bad($m)  { Write-Host "  FAIL  $m" -ForegroundColor Red; $script:fail++ }
function Step($m) { Write-Host "`n[$m]" -ForegroundColor Cyan }
function Check([bool]$cond, [string]$yes, [string]$no) { if ($cond) { Ok $yes } else { Bad $no } }

if (-not ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()
        ).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    throw "Run this from an elevated PowerShell: a per-machine MSI cannot install otherwise (error 1925)."
}
if (Get-Process TransportFever2 -EA SilentlyContinue) { throw "Close Transport Fever 2 first." }
foreach ($m in @($BigMsi, $MpMsi)) { if (-not (Test-Path $m)) { throw "No MSI at $m" } }

$STOCK = "STOCK-ALUT-STANDIN-DO-NOT-EDIT"
$IFEO  = 'HKLM:\SOFTWARE\Microsoft\Windows NT\CurrentVersion\Image File Execution Options\TransportFever2.exe'
$sandbox = Join-Path $env:TEMP ("tpf2_coexist_" + (Get-Random))
$game = Join-Path $sandbox "game"
New-Item -ItemType Directory -Force $game | Out-Null
Set-Content (Join-Path $game "alut.dll") $STOCK -Encoding ascii -NoNewline
Set-Content (Join-Path $game "TransportFever2.exe") "" -Encoding ascii
Write-Host "sandbox: $game"
Write-Host "MP MSI : $MpMsi"
Write-Host "Big MSI: $BigMsi"

# The Segment Heap value is machine-wide and both packages own it. Remember
# what was there so the machine leaves this test the way it entered it.
$ifeoBefore = (Get-ItemProperty $IFEO -Name FrontEndHeapDebugOptions -EA SilentlyContinue).FrontEndHeapDebugOptions
Write-Host "Segment Heap value before test: $(if ($null -eq $ifeoBefore) { '(absent)' } else { '0x{0:x}' -f $ifeoBefore })"

function Msi([string[]]$a, [string]$logName) {
    $log = Join-Path $sandbox $logName
    $p = Start-Process msiexec -ArgumentList ($a + @('/qn', '/l*v', $log)) -Wait -PassThru
    if ($p.ExitCode -ne 0) {
        Write-Host "  msiexec exit $($p.ExitCode); log tail:" -ForegroundColor Yellow
        Get-Content $log -Tail 12 | ForEach-Object { "    $_" }
    }
    $p.ExitCode
}
function Arp([string]$name) {
    Get-ItemProperty 'HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall\*',
                     'HKLM:\SOFTWARE\WOW6432Node\Microsoft\Windows\CurrentVersion\Uninstall\*' -EA SilentlyContinue |
        Where-Object { $_.DisplayName -eq $name }
}
function AlutIsProxy { (Test-Path "$game\alut.dll") -and ((Get-Content "$game\alut.dll" -Raw) -ne $STOCK) }
function AlutRealIsStock { (Test-Path "$game\alut_real.dll") -and ((Get-Content "$game\alut_real.dll" -Raw) -eq $STOCK) }
function IfeoSet { $v = (Get-ItemProperty $IFEO -Name FrontEndHeapDebugOptions -EA SilentlyContinue).FrontEndHeapDebugOptions; ($null -ne $v) -and (($v -band 8) -ne 0) }

$products = @{
    MP  = @{ name = 'TpF2 Multiplayer'; msi = $MpMsi;  own = @('tpf2_menu.dll', 'tpf2_slice.dll', 'tpf2_bridge_mp.dll', 'netpunch\netpunch.exe', 'mods\mp_lockstep_1\mod.lua') }
    Big = @{ name = 'TpF2 Big Maps';    msi = $BigMsi; own = @('plugins\tpf2_bigmap.dll', 'plugins\tpf2_bigmap.cfg') }
}
$shared = @('alut.dll', 'tpf2_pluginhost.dll')

function OwnFilesPresent($key) { @($products[$key].own | Where-Object { -not (Test-Path (Join-Path $game $_)) }).Count -eq 0 }
function OwnFilesGone($key)    { @($products[$key].own | Where-Object { Test-Path (Join-Path $game $_) }).Count -eq 0 }
function SharedPresent         { @($shared | Where-Object { -not (Test-Path (Join-Path $game $_)) }).Count -eq 0 }

function Install($key, $log) {
    $rc = Msi @('/i', $products[$key].msi, "INSTALLFOLDER=$game\", 'TPF2_SKIP_GAMEDIR_CHECK=1') $log
    Check ($rc -eq 0) "$($products[$key].name) installed" "$($products[$key].name) install exit $rc"
}
function Uninstall($key, $log) {
    $code = (Arp $products[$key].name | Select-Object -First 1).PSChildName
    if (-not $code) { Bad "$($products[$key].name) is not in Apps; cannot uninstall"; return }
    $rc = Msi @('/x', $code) $log
    Check ($rc -eq 0) "$($products[$key].name) uninstalled" "$($products[$key].name) uninstall exit $rc"
}

function RunOrder($first, $second) {
    $tag = "$first-then-$second"
    Step "$tag 1. install $first"
    Install $first "$tag-1.log"
    Check (AlutIsProxy) "alut.dll is the proxy" "alut.dll is still the stock file"
    Check (AlutRealIsStock) "alut_real.dll is the stock library" "alut_real.dll is NOT the stock library"
    Check (SharedPresent) "shared files present (proxy, plugin host)" "a shared file is missing"
    Check (OwnFilesPresent $first) "$first files present" "$first files missing"
    Check (IfeoSet) "Segment Heap value set" "Segment Heap value not set"

    Step "$tag 2. install $second over it"
    Install $second "$tag-2.log"
    Check (AlutIsProxy) "alut.dll is still the proxy" "alut.dll lost"
    Check (AlutRealIsStock) "alut_real.dll is STILL the stock library (no proxy wrapped around itself)" "alut_real.dll was overwritten by a proxy"
    Check (SharedPresent) "shared files present" "a shared file is missing"
    Check (OwnFilesPresent $first) "$first files still present" "$first files damaged by the second install"
    Check (OwnFilesPresent $second) "$second files present" "$second files missing"
    Check ((@(Arp $products[$first].name).Count -eq 1) -and (@(Arp $products[$second].name).Count -eq 1)) "both products listed in Apps" "Apps listing wrong"

    Step "$tag 3. uninstall $first (the other must keep working)"
    Uninstall $first "$tag-3.log"
    Check (AlutIsProxy) "alut.dll is STILL the proxy" "alut.dll was restored to stock while $second still needs the proxy"
    Check (AlutRealIsStock) "alut_real.dll still parked, still stock" "alut_real.dll gone or damaged"
    Check (SharedPresent) "shared files kept for $second" "a shared file was removed with $first"
    Check (OwnFilesGone $first) "$first's own files removed" "$first left files behind"
    Check (OwnFilesPresent $second) "$second files intact" "$second files damaged by $first's uninstall"
    Check (IfeoSet) "Segment Heap value kept for $second" "Segment Heap value removed while $second is installed"
    Check (@(Arp $products[$first].name).Count -eq 0) "$first gone from Apps" "$first still in Apps"

    Step "$tag 4. uninstall $second (last one out restores the game)"
    Uninstall $second "$tag-4.log"
    Check ((Test-Path "$game\alut.dll") -and ((Get-Content "$game\alut.dll" -Raw) -eq $STOCK)) "alut.dll restored to the stock library" "alut.dll was NOT restored"
    Check (-not (Test-Path "$game\alut_real.dll")) "alut_real.dll cleaned up" "alut_real.dll left behind"
    Check (-not (Test-Path "$game\tpf2_pluginhost.dll")) "plugin host removed" "tpf2_pluginhost.dll left behind"
    Check (OwnFilesGone $second) "$second's own files removed" "$second left files behind"
    Check (-not (IfeoSet)) "Segment Heap value removed with the last product" "Segment Heap value left behind"
    Check ((@(Arp $products[$first].name).Count -eq 0) -and (@(Arp $products[$second].name).Count -eq 0)) "nothing left in Apps" "still listed in Apps"
    $left = Get-ChildItem $game -Recurse -File | Where-Object { $_.Name -notin @('alut.dll', 'TransportFever2.exe') }
    Check (@($left).Count -eq 0) "game folder back to exactly its two original files" "leftovers: $(($left | ForEach-Object { $_.FullName.Substring($game.Length + 1) }) -join ', ')"
}

try {
    RunOrder 'MP' 'Big'
    RunOrder 'Big' 'MP'
}
finally {
    # Put the machine-wide value back the way it was, whatever the test did to it.
    if ($null -eq $ifeoBefore) {
        Remove-ItemProperty $IFEO -Name FrontEndHeapDebugOptions -EA SilentlyContinue
    } else {
        New-Item $IFEO -Force -EA SilentlyContinue | Out-Null
        Set-ItemProperty $IFEO -Name FrontEndHeapDebugOptions -Value $ifeoBefore -Type DWord
    }
    Write-Host "`nSegment Heap value restored to: $(if ($null -eq $ifeoBefore) { '(absent)' } else { '0x{0:x}' -f $ifeoBefore })"
    if ($KeepSandbox) { Write-Host "sandbox kept: $sandbox" }
    else { Remove-Item $sandbox -Recurse -Force -EA SilentlyContinue }
}
Write-Host ""
if ($fail) { Write-Host "$fail check(s) FAILED" -ForegroundColor Red; exit 1 }
Write-Host "all checks passed -- both orders, both directions" -ForegroundColor Green
