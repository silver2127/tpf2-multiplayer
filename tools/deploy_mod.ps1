# Deploy a mod from the repo to EVERY location the game may load it from.
#
# WHY THIS EXISTS
# Transport Fever 2 reads mods from more than one directory. This project once kept
# mp_lockstep_1 in two of them:
#
#   steamapps\common\Transport Fever 2\mods\      <- the ONLY target now (what the MSI installs)
#   userdata\<id>\1066780\local\mods\             <- NEVER deploy here (see below)
#
# Two lessons, both learned the hard way:
#  1. Editing only the userdata copy while the game loaded the other one was a
#     silent no-op (stale code ran, nothing warned) -- hence the marker VERIFY.
#  2. A per-user copy is NOT the same mod to the game: mods loaded from
#     userdata\...\local\mods get the id prefix '!' (the save records
#     '!mp_lockstep'), mods from <game>\mods are plain 'mp_lockstep'. A save
#     created while the per-user copy existed demands '!mp_lockstep' and fails on
#     every machine that installed the mod normally ("mod is not properly
#     installed", two-machine test 2026-08-30). So the per-user location is banned.
#
# Usage:
#   pwsh tools\deploy_mod.ps1 [-Mod mp_lockstep_1] [-Marker collectEdges]

param(
    [string]$Mod    = "mp_lockstep_1",
    [string]$Marker = ""
)

$ErrorActionPreference = "Stop"
$repo = Split-Path -Parent $PSScriptRoot
$src  = Join-Path $repo "mod\$Mod"

if (-not (Test-Path $src)) { Write-Error "source not found: $src"; exit 1 }

$targets = @(
    "C:\Program Files (x86)\Steam\steamapps\common\Transport Fever 2\mods\$Mod"
)
# Refuse to leave a per-user copy behind: it shadows the game-folder mod under a
# different id ('!<mod>') and poisons every save made on this machine.
$userCopy = "$((Get-ChildItem "C:\Program Files (x86)\Steam\userdata" -Directory | Where-Object { Test-Path (Join-Path $_.FullName "1066780") } | Select-Object -First 1).FullName)\1066780\local\mods\$Mod"
if (Test-Path $userCopy) { Remove-Item $userCopy -Recurse -Force; Write-Host "  removed per-user copy $userCopy (it changes the mod id to '!$Mod')" }

$deployed = 0
foreach ($t in $targets) {
    $parent = Split-Path -Parent $t
    if (-not (Test-Path $parent)) {
        Write-Host "  skip (no such mods dir): $parent"
        continue
    }
    if (-not (Test-Path $t)) { New-Item -ItemType Directory -Path $t -Force | Out-Null }
    Copy-Item -Path (Join-Path $src '*') -Destination $t -Recurse -Force
    Write-Host "  deployed -> $t"
    $deployed++
}

if ($deployed -eq 0) { Write-Error "no mod directories found - nothing deployed"; exit 1 }

# Verify. Without this the script is just a copy that assumes it worked.
if ($Marker -ne "") {
    Write-Host ""
    Write-Host "verifying marker '$Marker':"
    $bad = 0
    foreach ($t in $targets) {
        if (-not (Test-Path $t)) { continue }
        $hits = @(Get-ChildItem -Path $t -Recurse -Filter *.lua -EA SilentlyContinue |
                  Select-String -SimpleMatch $Marker -EA SilentlyContinue)
        if ($hits.Count -gt 0) {
            Write-Host ("  OK   {0} hit(s) in {1}" -f $hits.Count, $t)
        } else {
            Write-Host ("  FAIL marker absent in {0}" -f $t)
            $bad++
        }
    }
    if ($bad -gt 0) { Write-Error "deploy verification FAILED in $bad location(s)"; exit 1 }
}

Write-Host ""
Write-Host "deploy OK ($deployed location(s)). The game reads mods at LOAD, so restart any running instance."
