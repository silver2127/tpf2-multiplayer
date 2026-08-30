# Deploy a mod from the repo to EVERY location the game may load it from.
#
# WHY THIS EXISTS
# Transport Fever 2 reads mods from more than one directory, and this project has
# a copy of mp_lockstep_1 in two of them:
#
#   steamapps\common\Transport Fever 2\mods\      <- the one actually loaded
#   userdata\<id>\1066780\local\mods\             <- the one that was being edited
#
# Syncing only the second produced a silent no-op: the game loaded the stale copy,
# every new log line was simply absent, and the failure looked like a logic bug in
# the new code rather than a deploy that never arrived. Nothing errors, nothing
# warns -- the old file just runs.
#
# So: copy to all of them, then VERIFY by grepping each target for a marker string
# the new version contains. A copy that reports success without checking is how
# the problem stayed invisible in the first place.
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
    "C:\Program Files (x86)\Steam\steamapps\common\Transport Fever 2\mods\$Mod",
    "$((Get-ChildItem "C:\Program Files (x86)\Steam\userdata" -Directory | Where-Object { Test-Path (Join-Path $_.FullName "1066780") } | Select-Object -First 1).FullName)\1066780\local\mods\$Mod"
)

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
