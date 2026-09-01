# mp_menu_launch.ps1 -- bring up A + sandboxed B and LEAVE THEM AT THE TITLE MENU.
#
# The Multiplayer lobby lives on the title screen (the in-frame Vulkan panel that
# menu_hook renders on main-menu page 2). autotest/mp_launch click CONTINUE and
# load a save, which navigates straight off that page -- wrong for a lobby demo.
# This launches both instances and STOPS at the menu so HOST/JOIN are reachable.
#
#   tools\mp_menu_launch.ps1            launch A + B to the title menu
#   tools\mp_menu_launch.ps1 -Solo      launch only instance A (host-side look)
#   tools\mp_menu_launch.ps1 -Players 3 launch A + B + C (C in Sandboxie box GameAgent2)
#
# Each extra player after B is one more Sandboxie box named GameAgent2, GameAgent3,
# ... (a copy of the GameAgent box's settings in C:\Windows\Sandboxie.ini). Boxes
# isolate the game's files, mutex and data dir; they do NOT isolate the network,
# so the bridge and the menu pick free loopback ports per instance.
#
# A FRESH box needs two things before its game stays up (learned 2026-09-01):
#   1. the first launch only starts the boxed Steam and exits; launch again.
#   2. the boxed Steam then blocks on a Steam Cloud "pending sessions" prompt
#      because the other instances hold the game's cloud session. Stop that box
#      (Start.exe /box:<box> /terminate -- ONLY the new box) and put
#      "cloudenabled" "0" in the game's block of the box's
#      userdata\<id>\config\localconfig.vdf. After that it launches like B.
param([switch]$Solo, [int]$Players = 2)
$ErrorActionPreference = "Stop"
$Exe   = "C:\Program Files (x86)\Steam\steamapps\common\Transport Fever 2\TransportFever2.exe"
$Sbie  = "C:\Program Files\Sandboxie-Plus\Start.exe"
$Box   = "GameAgent"
$Out   = "C:\Program Files (x86)\Steam\steamapps\workshop\content\1066780\3710243057\recon\m4\out"
$Ovl   = "C:\Sandbox\$env:USERNAME\$Box\drive\C" + $Out.Substring(2)

function Wait-Menu([string]$logPath, [string]$tag, [int]$timeoutSec = 60) {
    # the menu DLL logs "present state: show=1 ..." once main-menu page 2 is drawn.
    $deadline = (Get-Date).AddSeconds($timeoutSec)
    while ((Get-Date) -lt $deadline) {
        if (Test-Path $logPath) {
            try {
                $tail = Get-Content $logPath -Tail 3 -ErrorAction SilentlyContinue
                if ($tail -match "show=1") { Write-Host "[$tag] title menu is up (Multiplayer button visible)"; return $true }
            } catch {}
        }
        Start-Sleep -Milliseconds 800
    }
    Write-Host "[$tag] timed out waiting for the title menu (check the window manually)"
    return $false
}

Write-Host "[menu-launch] stopping any running games"
Get-Process TransportFever2 -ErrorAction SilentlyContinue | Stop-Process -Force -ErrorAction SilentlyContinue
Start-Sleep -Milliseconds 600

# A: real instance via Steam. Its menu DLL log is the real workshop out path.
# The proxy loads tpf2_menu.dll from the GAME dir and the DLL logs there too
# (the workshop out copy is stale). Boxed instances log to the same path inside
# their Sandboxie overlay.
$GameDir  = Split-Path $Exe
$menuLogA = Join-Path $GameDir "tpf2_menu.log"
$sizeA0 = if (Test-Path $menuLogA) { (Get-Item $menuLogA).Length } else { 0 }
Write-Host "[menu-launch] launching instance A (Steam)"
Start-Process "steam://rungameid/1066780"
Wait-Menu $menuLogA "A" 90 | Out-Null

if ($Solo) { $Players = 1 }
$letters = "ABCDEFGH"
for ($i = 1; $i -lt $Players; $i++) {
    # B, C, ...: sandboxed instances, one box each. Each box's menu DLL log
    # lands in that box's Sandboxie overlay.
    $box = if ($i -eq 1) { $Box } else { "$Box$i" }
    $tag = $letters[$i]
    $ovl = "C:\Sandbox\$env:USERNAME\$box\drive\C" + $GameDir.Substring(2)
    $menuLogX = Join-Path $ovl "tpf2_menu.log"
    Write-Host "[menu-launch] launching instance $tag (Sandboxie box '$box')"
    Start-Process -FilePath $Sbie -ArgumentList "/box:$box", "`"$Exe`"" -WorkingDirectory (Split-Path $Exe)
    Wait-Menu $menuLogX "$tag" 90 | Out-Null
}

$n = (Get-Process TransportFever2 -ErrorAction SilentlyContinue | Measure-Object).Count
Write-Host "[menu-launch] done -- $n instance(s) at the title menu."
Write-Host "  A: click MULTIPLAYER -> HOST   (code generates + copies to clipboard)"
for ($i = 1; $i -lt $Players; $i++) { Write-Host "  $($letters[$i]): click MULTIPLAYER -> JOIN   (reads the code from the clipboard)" }
