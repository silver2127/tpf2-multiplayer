# install_portable.ps1 -- stage the PORTABLE runtime on any machine.
#
# This is the shipping installer (as opposed to the dev-harness install_proxy.ps1).
# It discovers Steam/game paths from the registry -- nothing is hardcoded to a
# particular user -- and stages the netpunch layer (the frozen netpunch.exe + the
# .py fallback) into a per-user, sandbox-safe location the menu DLL already looks
# for: %LOCALAPPDATA%\tpf2mp\netpunch.
#
# What it does NOW (safe, self-contained):
#   * discover Steam root, the TF2 install dir, and the userdata save folder
#   * deploy netpunch.exe (+ lobby/punch/connect/observe .py) to %LOCALAPPDATA%\tpf2mp\netpunch
#   * report exactly what it found and what remains manual
#
# What it does NOT do yet (see PORTABILITY.md -- these need the coupled DLL/Lua
# fix and are gated off so a half-install can't silently break replication):
#   * relocate the bridge/menu/slice DLLs (they are coupled to the Lua mod's
#     hardcoded BASE path -- must be fixed together)
#   * install the alut proxy (the portable proxy is built but redeploy is a
#     verified step -- use tools\install_proxy.ps1 with eyes on the menu)
[CmdletBinding()]
param([switch]$WhatIfOnly)

$ErrorActionPreference = "Stop"
function Say($m, $c = "Cyan") { Write-Host "[install] $m" -ForegroundColor $c }
function Warn($m) { Write-Host "[install] $m" -ForegroundColor Yellow }

$Repo = Split-Path -Parent $PSScriptRoot

# --- discovery: Steam root (registry, no hardcoded path) ---
$steam = $null
try { $steam = (Get-ItemProperty 'HKCU:\Software\Valve\Steam' -Name SteamPath -EA Stop).SteamPath } catch {}
if (-not $steam) { try { $steam = (Get-ItemProperty 'HKLM:\SOFTWARE\WOW6432Node\Valve\Steam' -Name InstallPath -EA Stop).InstallPath } catch {} }
if ($steam) { $steam = ($steam -replace '/','\').TrimEnd('\') }
if (-not $steam -or -not (Test-Path $steam)) { Warn "Steam not found in the registry -- is Steam installed?"; $steam = $null }
else { Say "Steam root       : $steam" }

# --- discovery: TF2 (appid 1066780) install dir, across all library folders ---
$appid = "1066780"
$gameDir = $null
if ($steam) {
    $libs = @($steam)
    $vdf = Join-Path $steam "steamapps\libraryfolders.vdf"
    if (Test-Path $vdf) {
        Select-String -Path $vdf -Pattern '"path"\s+"(.+?)"' -AllMatches | ForEach-Object {
            $_.Matches | ForEach-Object { $libs += ($_.Groups[1].Value -replace '\\\\','\') }
        }
    }
    foreach ($lib in ($libs | Select-Object -Unique)) {
        $cand = Join-Path $lib "steamapps\common\Transport Fever 2\TransportFever2.exe"
        if (Test-Path $cand) { $gameDir = Split-Path -Parent $cand; break }
    }
}
if ($gameDir) { Say "TF2 install dir  : $gameDir" } else { Warn "TF2 install dir not found (needed later for the proxy install)." }

# --- discovery: userdata save folder (userdata\<id>\1066780\local\save) ---
$saveDir = $null
if ($steam -and (Test-Path (Join-Path $steam "userdata"))) {
    Get-ChildItem (Join-Path $steam "userdata") -Directory -EA SilentlyContinue | ForEach-Object {
        $cand = Join-Path $_.FullName "$appid\local\save"
        if (Test-Path $cand) { $saveDir = $cand }
    }
}
if ($saveDir) { Say "Save folder      : $saveDir" } else { Warn "TF2 save folder not found (launch the game once to create it)." }

# --- stage the netpunch layer into %LOCALAPPDATA%\tpf2mp\netpunch ---
$dest = Join-Path $env:LOCALAPPDATA "tpf2mp\netpunch"
Say "Deploy target    : $dest"
if ($WhatIfOnly) { Say "(-WhatIfOnly: discovery only, nothing copied)"; return }

New-Item -ItemType Directory -Force $dest | Out-Null
$exe = Join-Path $Repo "netpunch\dist\netpunch.exe"
if (Test-Path $exe) {
    Copy-Item $exe (Join-Path $dest "netpunch.exe") -Force
    Say "  + netpunch.exe (frozen -- no Python needed on this machine)"
} else {
    Warn "  netpunch.exe not built. Run: cd netpunch; python -m PyInstaller --onefile --name netpunch lobby.py"
}
foreach ($py in "lobby.py","punch.py","connect.py","observe.py") {
    $src = Join-Path $Repo "netpunch\$py"
    if (Test-Path $src) { Copy-Item $src (Join-Path $dest $py) -Force }
}
Say "  + lobby/punch/connect/observe .py (fallback if Python is present)"

Say "netpunch layer installed. The menu DLL will now resolve NETDIR here and prefer netpunch.exe." Green
Warn "STILL MANUAL (see PORTABILITY.md): DLL/Lua BASE coupling, proxy redeploy, and the Continue-load RE."
