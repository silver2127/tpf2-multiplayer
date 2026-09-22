<#
.SYNOPSIS
Builds the plugin, then the TpF2 Big Maps MSI (installer\out\TpF2BigMaps.msi).

.DESCRIPTION
Steps, in order (each one stops the script on failure):

  1. build.bat -> out\tpf2_bigmap.dll
     A DLL a running game has loaded stays locked, so the link fails with
     LNK1104 while Transport Fever 2 is open. Close the game.
  2. Check installer\vendor\ for the three binaries SHARED with TpF2 Multiplayer:
       alut.dll             the proxy the game loads in place of its own
       tpf2_pluginhost.dll  the plugin host the proxy loads last
       tpf2ca.dll           the installer custom actions (park/restore alut.dll)
     They are built in the tpf2-multiplayer repository and copied here by
     tools\vendor_host.ps1, which also records the source commit in
     vendor\VENDORED.md. This script never builds them: both packages must ship
     the SAME bytes under the SAME component GUIDs (PluginHost.wxs), or Windows
     Installer cannot treat them as one shared component.
  3. wix build -arch x64 -ext WixToolset.UI.wixext ... Package.wxs PluginHost.wxs

-Validate then runs an administrative install (msiexec /a ... /qn TARGETDIR=<temp>),
which extracts the package without installing anything, and lists the tree.

WiX v7 asks you to accept its Open Source Maintenance Fee EULA once per
machine (wix eula accept wix7) or per invocation (--acceptEula wix7). The
script never accepts it for you: pass -AcceptWixEula to add the per-invocation
flag after reading https://wixtoolset.org/osmf/ .

.PARAMETER SkipBuild
Skip step 1 (package whatever is in out\).

.PARAMETER Validate
After building, extract the MSI with msiexec /a into a temp folder and list it.

.PARAMETER AcceptWixEula
Pass --acceptEula wix7 to wix for this run.

.PARAMETER Version
Package version (three-part). Defaults to installer\VERSION, the single source
of truth for what a release is called. Bump it when cutting a release.

.EXAMPLE
powershell -File installer\build_msi.ps1 -Validate -AcceptWixEula
#>
[CmdletBinding()]
param(
    [switch]$SkipBuild,
    [switch]$Validate,
    [switch]$AcceptWixEula,
    [ValidatePattern('^\d+\.\d+\.\d+$')]
    [string]$Version
)

$ErrorActionPreference = "Stop"

$Installer = $PSScriptRoot
if (-not $Version) {
    $vf = Join-Path $PSScriptRoot "VERSION"
    if (-not (Test-Path $vf)) { throw "installer\VERSION is missing and no -Version was given" }
    $Version = (Get-Content $vf -Raw).Trim()
    if ($Version -notmatch '^\d+\.\d+\.\d+$') { throw "installer\VERSION does not contain a three-part version: '$Version'" }
}
$Repo   = Split-Path -Parent $Installer
$Vendor = Join-Path $Installer "vendor"
$OutDir = Join-Path $Installer "out"
$Msi    = Join-Path $OutDir "TpF2BigMaps.msi"
$Wix    = Join-Path $env:USERPROFILE ".dotnet\tools\wix.exe"

function Say($m, $c = "Cyan") { Write-Host "[msi] $m" -ForegroundColor $c }
function Warn($m) { Write-Host "[msi] $m" -ForegroundColor Yellow }
function Fail($m) { Write-Host "[msi] $m" -ForegroundColor Red; exit 1 }

# Runs a .bat through cmd and returns its exit code. vcvars64.bat prints a
# harmless 'vswhere.exe is not recognized' line on machines without a full
# Visual Studio; hide it. Redirect INSIDE cmd: PowerShell 5.1's own 2>&1 on a
# native command wraps every stderr line in an ErrorRecord, which under
# ErrorActionPreference=Stop aborts the build on that harmless line.
function Run-Bat([string]$bat) {
    if (-not (Test-Path $bat)) { Fail "missing build script: $bat" }
    cmd /c "`"$bat`" 2>&1" | Where-Object { "$_" -notmatch "vswhere|operable program or batch file" } | ForEach-Object { Write-Host "    $_" }
    return $LASTEXITCODE
}

# ---- 1. the plugin --------------------------------------------------------
$bigmapDll = Join-Path $Repo "out\tpf2_bigmap.dll"
if ($SkipBuild) {
    Warn "-SkipBuild: packaging the existing $bigmapDll"
} else {
    Say "running build.bat"
    $rc = Run-Bat (Join-Path $Repo "build.bat")
    if ($rc -ne 0) { Fail "build.bat failed (exit $rc). If it was LNK1104, close the game (it holds tpf2_bigmap.dll) and rerun." }
}
if (-not (Test-Path $bigmapDll)) { Fail "missing: $bigmapDll" }

# ---- 2. the shared binaries -----------------------------------------------
$proxyDll = Join-Path $Vendor "alut.dll"
$hostDll  = Join-Path $Vendor "tpf2_pluginhost.dll"
$caDll    = Join-Path $Vendor "tpf2ca.dll"
foreach ($f in @($proxyDll, $hostDll, $caDll)) {
    if (-not (Test-Path $f)) { Fail "missing shared binary: $f -- run tools\vendor_host.ps1 -FromMsi <TpF2Multiplayer.msi> -Release <tag>" }
}
$vendored = Join-Path $Vendor "VENDORED.md"
if (Test-Path $vendored) { Get-Content $vendored | Select-String '^source (release|commit):' | ForEach-Object { Say "shared binaries: $($_.Line)" } }
else { Warn "vendor\VENDORED.md is missing: the shared binaries' provenance is unrecorded" }

# The shared fragment must be the same file in both repositories. A drift here
# means the two packages disagree about a component GUID, which is exactly the
# failure this whole arrangement exists to prevent. Line endings are not part of
# it: they follow each checkout's core.autocrlf.
$sibling = Join-Path (Split-Path -Parent $Repo) "tpf2-multiplayer\installer\PluginHost.wxs"
if (Test-Path $sibling) {
    $a = [IO.File]::ReadAllText((Join-Path $Installer "PluginHost.wxs")) -replace "`r`n", "`n"
    $b = [IO.File]::ReadAllText($sibling) -replace "`r`n", "`n"
    if ($a -cne $b) { Fail "installer\PluginHost.wxs differs from $sibling -- the two packages must share it (line endings aside)" }
    Say "PluginHost.wxs matches the tpf2-multiplayer copy"
}

# ---- 3. wix ----------------------------------------------------------------
if (-not (Test-Path $Wix)) { Fail "wix.exe not found at $Wix -- dotnet tool install --global wix" }
$eula = @(); if ($AcceptWixEula) { $eula = @("--acceptEula", "wix7") }
$extList = (& $Wix extension list -g @eula 2>&1 | ForEach-Object { "$_" }) -join "`n"
if ($extList -match "WIX7015") { Fail "WiX v7 needs its OSMF EULA accepted: run 'wix eula accept wix7' once, or pass -AcceptWixEula. See https://wixtoolset.org/osmf/" }
if ($extList -notmatch "WixToolset\.UI\.wixext") {
    Say "installing the WiX UI extension (global)"
    & $Wix extension add -g WixToolset.UI.wixext @eula
    if ($LASTEXITCODE -ne 0) { Fail "wix extension add failed" }
}

New-Item -ItemType Directory -Force $OutDir | Out-Null
$wixArgs = @("build") + $eula + @(
    "-arch", "x64",
    "-ext", "WixToolset.UI.wixext",
    "-d", "ProductVersion=$Version",
    "-d", "BigmapDll=$bigmapDll",
    "-d", "BigmapCfg=$(Join-Path $Repo 'cfg\tpf2_bigmap.cfg')",
    "-d", "ProxyDll=$proxyDll",
    "-d", "HostDll=$hostDll",
    "-d", "CaDll=$caDll",
    "-o", $Msi,
    (Join-Path $Installer "Package.wxs"),
    (Join-Path $Installer "PluginHost.wxs")
)
Say "wix $($wixArgs -join ' ')"
Push-Location $Installer
try {
    $wixOut = & $Wix @wixArgs 2>&1 | ForEach-Object { "$_" }
    $rc = $LASTEXITCODE
} finally { Pop-Location }
$wixOut | ForEach-Object { Write-Host "    $_" }
if (($wixOut -join "`n") -match "WIX7015") { Fail "WiX v7 needs its OSMF EULA accepted: run 'wix eula accept wix7' once, or pass -AcceptWixEula. See https://wixtoolset.org/osmf/" }
if ($rc -ne 0) { Fail "wix build failed (exit $rc)" }
if (-not (Test-Path $Msi)) { Fail "wix reported success but $Msi is missing" }
Copy-Item $Msi (Join-Path $OutDir "TpF2BigMaps-$Version.msi") -Force
Say "built $Msi ($([math]::Round((Get-Item $Msi).Length / 1MB, 1)) MB, version $Version)" Green

# ---- validate --------------------------------------------------------------
if ($Validate) {
    $tmp = Join-Path $env:TEMP ("tpf2bigmap_msi_" + (Get-Random))
    Say "administrative install into $tmp"
    $p = Start-Process msiexec -ArgumentList @('/a', $Msi, '/qn', "TARGETDIR=$tmp") -Wait -PassThru
    if ($p.ExitCode -ne 0) { Fail "msiexec /a exit $($p.ExitCode)" }
    Get-ChildItem $tmp -Recurse -File | ForEach-Object { "    " + $_.FullName.Substring($tmp.Length + 1) + "  ($($_.Length) bytes)" }
    Remove-Item $tmp -Recurse -Force -EA SilentlyContinue
}
