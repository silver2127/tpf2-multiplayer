<#
.SYNOPSIS
Hash-compares every shipped multiplayer file between this checkout's build outputs
and the game folder plus every Sandboxie overlay of it.

.DESCRIPTION
The installer's "refreshed the overlay" line is not proof: a boxed instance reads
its own overlay copy of the game folder, and one stale DLL, plugin or mod file
there runs silently and looks like a protocol bug between A and B. This lists
every mismatch or missing file and exits 1 if there is one.

Compared: native\out\*.dll (in the game folder root and plugins\), netpunch\dist\netpunch.exe,
and the whole mods\mp_lockstep_1 tree.

  tools\verify_install.ps1                 the game folder and every C:\Sandbox\<user>\*\ overlay of it
  tools\verify_install.ps1 -Quiet          only the summary line and the mismatches
#>
param(
    [string]$Game = "C:\Program Files (x86)\Steam\steamapps\common\Transport Fever 2",
    [switch]$Quiet
)
$ErrorActionPreference = "Stop"
$src = Split-Path -Parent $PSScriptRoot

function FileHash([string]$p) {
    if (Test-Path -LiteralPath $p) { (Get-FileHash -LiteralPath $p -Algorithm MD5).Hash } else { "MISSING" }
}

$roots = @($Game)
$rel = $Game.Substring(3)   # strip "C:\"
foreach ($box in Get-ChildItem "C:\Sandbox\$env:USERNAME" -Directory -ErrorAction SilentlyContinue) {
    $o = Join-Path $box.FullName "drive\C\$rel"
    if (Test-Path -LiteralPath $o) { $roots += $o }
}

# [source, relative target(s)]
$items = @()
foreach ($d in Get-ChildItem "$src\native\out\*.dll") {
    $targets = @($d.Name)
    if (Test-Path -LiteralPath (Join-Path $Game "plugins\$($d.Name)")) { $targets = @("plugins\$($d.Name)") }
    $items += ,@($d.FullName, $targets)
}
$items += ,@("$src\netpunch\dist\netpunch.exe", @("netpunch\netpunch.exe"))
foreach ($f in Get-ChildItem "$src\mod\mp_lockstep_1" -Recurse -File) {
    $items += ,@($f.FullName, @("mods\" + $f.FullName.Substring("$src\mod\".Length)))
}

$bad = 0; $n = 0
foreach ($it in $items) {
    $h = FileHash $it[0]
    foreach ($root in $roots) {
        foreach ($t in $it[1]) {
            $n++
            $g = FileHash (Join-Path $root $t)
            # An overlay holds only what was written inside the box; a file absent
            # there is read from the game folder, which is checked on its own.
            if ($g -eq "MISSING" -and $root -ne $Game) { continue }
            if ($g -ne $h) { $bad++; Write-Host ("MISMATCH {0}  [{1}]  {2}" -f $t, $root, $g) -ForegroundColor Red }
            elseif (-not $Quiet) { Write-Host ("ok  {0}  [{1}]" -f $t, $root) }
        }
    }
}
Write-Host ("{0} placements over {1} root(s): {2} mismatch(es)" -f $n, $roots.Count, $bad) -ForegroundColor ($(if ($bad) { "Red" } else { "Green" }))
if ($bad) { exit 1 }
