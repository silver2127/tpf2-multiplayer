<#
.SYNOPSIS
Builds every shipped binary, then the TpF2 Multiplayer MSI (installer\out\TpF2Multiplayer.msi).

.DESCRIPTION
Steps, in order (each one stops the script on failure):

  1. bridge\build_proxy.bat   -> bridge\out\alut.dll, bridge\out\tpf2_bridge_mp.dll
     bridge\build_menu.bat    -> bridge\out\tpf2_menu.dll
     bridge\build_slice.bat   -> bridge\out\tpf2_slice.dll
     A DLL that a running game has loaded stays locked, so linking to the plain
     name fails with LNK1104. build_menu/build_slice take a name suffix: on a
     failure the script retries with one, copies the result over the plain name
     if it can, and otherwise packages the suffixed file under the plain name.
     build_proxy has no suffix support; close the game if it fails.
  2. python -m PyInstaller --onefile --name netpunch lobby.py  (in netpunch\)
     -> netpunch\dist\netpunch.exe. -SkipFreeze reuses an existing exe.
  3. installer\ca\build_ca.bat -> installer\out\tpf2ca.dll (the custom actions).
  4. wix build -arch x64 -ext WixToolset.UI.wixext ... installer\Package.wxs

-Validate then runs an administrative install (msiexec /a ... /qn TARGETDIR=<temp>),
which extracts the package without installing anything, and lists the tree.

WiX v7 asks you to accept its Open Source Maintenance Fee EULA once per
machine (wix eula accept wix7) or per invocation (--acceptEula wix7). The
script never accepts it for you: pass -AcceptWixEula to add the per-invocation
flag after reading https://wixtoolset.org/osmf/ .

.PARAMETER SkipBuild
Skip steps 1-3 (package whatever is in bridge\out, netpunch\dist and installer\out).

.PARAMETER SkipFreeze
Skip the PyInstaller step when netpunch\dist\netpunch.exe already exists (warns).

.PARAMETER Validate
After building, extract the MSI with msiexec /a into a temp folder and list it.

.PARAMETER AcceptWixEula
Pass --acceptEula wix7 to wix for this run.

.PARAMETER Version
Package version (three-part). Defaults to the contents of installer\VERSION, which
is the single source of truth for what a release is called -- every 0.1.x MSI up to
2026-08-30 shipped as ProductVersion 0.1.0 because this defaulted to a literal, so
Windows showed the same version for every build and could not tell an upgrade from
a reinstall. Bump installer\VERSION when cutting a release. Also becomes the
ProductVersion preprocessor variable in Package.wxs.

.EXAMPLE
pwsh installer\build_msi.ps1 -SkipFreeze -Validate -AcceptWixEula
#>
[CmdletBinding()]
param(
    [switch]$SkipBuild,
    [switch]$SkipFreeze,
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
$Repo      = Split-Path -Parent $Installer
$Bridge    = Join-Path $Repo "bridge"
$BridgeOut = Join-Path $Bridge "out"
$Netpunch  = Join-Path $Repo "netpunch"
$OutDir    = Join-Path $Installer "out"
$Msi       = Join-Path $OutDir "TpF2Multiplayer.msi"
$Wix       = Join-Path $env:USERPROFILE ".dotnet\tools\wix.exe"

function Say($m, $c = "Cyan") { Write-Host "[msi] $m" -ForegroundColor $c }
function Warn($m) { Write-Host "[msi] $m" -ForegroundColor Yellow }
function Fail($m) { Write-Host "[msi] $m" -ForegroundColor Red; exit 1 }

# Runs a .bat through cmd and returns its exit code. The bridge scripts cd to
# their own folder themselves. vcvars64.bat prints a harmless 'vswhere.exe is
# not recognized' line on machines without a full Visual Studio; hide it.
function Run-Bat([string]$bat, [string]$arg = "") {
    if (-not (Test-Path $bat)) { Fail "missing build script: $bat" }
    cmd /c "`"$bat`" $arg" 2>&1 | Where-Object { "$_" -notmatch "vswhere|operable program or batch file" } | ForEach-Object { Write-Host "    $_" }
    return $LASTEXITCODE
}

# Builds bridge\<bat>, retrying with a suffix when the plain output is locked.
# Returns the path to hand to wix (plain name when possible).
function Build-Suffixable([string]$bat, [string]$plainName) {
    $batPath = Join-Path $Bridge $bat
    $plain   = Join-Path $BridgeOut $plainName
    Say "running $bat"
    $rc = Run-Bat $batPath
    if ($rc -eq 0) { return $plain }

    $suffix = "_msi" + (Get-Date -Format "HHmmss")
    Warn "$bat failed (exit $rc). If that was LNK1104 the game is holding $plainName; retrying as $suffix"
    $rc = Run-Bat $batPath $suffix
    if ($rc -ne 0) { Fail "$bat $suffix failed too (exit $rc) -- see the compiler output above" }

    $built = Join-Path $BridgeOut (($plainName -replace '\.dll$', '') + "$suffix.dll")
    if (-not (Test-Path $built)) { Fail "expected $built after the suffixed build" }
    try {
        Copy-Item $built $plain -Force -ErrorAction Stop
        Say "copied $(Split-Path -Leaf $built) -> $plainName"
        return $plain
    } catch {
        Warn "$plainName is locked; packaging $(Split-Path -Leaf $built) under the name $plainName"
        return $built
    }
}

$menuDll  = Join-Path $BridgeOut "tpf2_menu.dll"
$sliceDll = Join-Path $BridgeOut "tpf2_slice.dll"
$netExe   = Join-Path $Netpunch "dist\netpunch.exe"
$caDll    = Join-Path $OutDir "tpf2ca.dll"

# ---- 1. native DLLs ------------------------------------------------------
if ($SkipBuild) {
    Warn "-SkipBuild: packaging the existing files in bridge\out"
} else {
    Say "running build_proxy.bat"
    $rc = Run-Bat (Join-Path $Bridge "build_proxy.bat")
    if ($rc -ne 0) { Fail "build_proxy.bat failed (exit $rc). If it was LNK1104, close the game (it holds alut.dll / tpf2_bridge_mp.dll) and rerun." }
    $menuDll  = Build-Suffixable "build_menu.bat"  "tpf2_menu.dll"
    $sliceDll = Build-Suffixable "build_slice.bat" "tpf2_slice.dll"
}
foreach ($f in @((Join-Path $BridgeOut "alut.dll"), (Join-Path $BridgeOut "tpf2_bridge_mp.dll"), $menuDll, $sliceDll)) {
    if (-not (Test-Path $f)) { Fail "missing: $f" }
}

# ---- 2. frozen lobby -----------------------------------------------------
if ($SkipBuild) {
    Warn "-SkipBuild: using the existing netpunch.exe"
} elseif ($SkipFreeze -and (Test-Path $netExe)) {
    Warn "-SkipFreeze: reusing $netExe (built $((Get-Item $netExe).LastWriteTime)); lobby.py changes since then are NOT in it"
} else {
    Say "freezing netpunch\lobby.py with PyInstaller"
    Push-Location $Netpunch
    try {
        python -m PyInstaller --noconfirm --onefile --name netpunch lobby.py
        if ($LASTEXITCODE -ne 0) { Fail "PyInstaller failed (exit $LASTEXITCODE). pip install pyinstaller -r requirements.txt" }
    } finally { Pop-Location }
}
if (-not (Test-Path $netExe)) { Fail "missing: $netExe" }

# ---- 3. custom-action DLL ------------------------------------------------
if ($SkipBuild -and (Test-Path $caDll)) {
    Warn "-SkipBuild: using the existing tpf2ca.dll"
} else {
    Say "running ca\build_ca.bat"
    $rc = Run-Bat (Join-Path $Installer "ca\build_ca.bat")
    if ($rc -ne 0) { Fail "build_ca.bat failed (exit $rc)" }
}
if (-not (Test-Path $caDll)) { Fail "missing: $caDll" }

# ---- 4. wix --------------------------------------------------------------
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
    "-d", "MenuDll=$menuDll",
    "-d", "SliceDll=$sliceDll",
    "-d", "NetpunchExe=$netExe",
    "-d", "CaDll=$caDll",
    "-o", $Msi,
    (Join-Path $Installer "Package.wxs")
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
Say "built $Msi ($([math]::Round((Get-Item $Msi).Length / 1MB, 1)) MB, version $Version)" Green

# ---- 5. optional validation ----------------------------------------------
if ($Validate) {
    $tmp = Join-Path $env:TEMP ("tpf2mp_msi_validate_" + (Get-Date -Format "yyyyMMdd_HHmmss"))
    New-Item -ItemType Directory -Force $tmp | Out-Null
    $log = Join-Path $tmp "admin_install.log"
    Say "administrative install (extract only) into $tmp"
    $p = Start-Process msiexec.exe -ArgumentList @("/a", "`"$Msi`"", "/qn", "TARGETDIR=`"$tmp`"", "/l*v", "`"$log`"") -Wait -PassThru
    if ($p.ExitCode -ne 0) { Get-Content $log -Tail 40 | ForEach-Object { Write-Host "    $_" }; Fail "msiexec /a failed (exit $($p.ExitCode)); log: $log" }
    Get-ChildItem $tmp -Recurse -File | Where-Object { $_.FullName -ne $log } | ForEach-Object {
        Write-Host ("    {0,10}  {1}" -f $_.Length, $_.FullName.Substring($tmp.Length + 1))
    }
    Say "extracted tree listed above; the folder is left in place for inspection: $tmp" Green
}
