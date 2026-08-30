# mp_menu_launch.ps1 -- bring up A + sandboxed B and LEAVE THEM AT THE TITLE MENU.
#
# The Multiplayer lobby lives on the title screen (the in-frame Vulkan panel that
# menu_hook renders on main-menu page 2). autotest/mp_launch click CONTINUE and
# load a save, which navigates straight off that page -- wrong for a lobby demo.
# This launches both instances and STOPS at the menu so HOST/JOIN are reachable.
#
#   tools\mp_menu_launch.ps1            launch A + B to the title menu
#   tools\mp_menu_launch.ps1 -Solo      launch only instance A (host-side look)
param([switch]$Solo)
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
$menuLogA = Join-Path $Out "tpf2_menu.log"
$sizeA0 = if (Test-Path $menuLogA) { (Get-Item $menuLogA).Length } else { 0 }
Write-Host "[menu-launch] launching instance A (Steam)"
Start-Process "steam://rungameid/1066780"
Wait-Menu $menuLogA "A" 90 | Out-Null

if (-not $Solo) {
    # B: sandboxed instance. Its menu DLL log lands in the Sandboxie overlay.
    $menuLogB = Join-Path $Ovl "tpf2_menu.log"
    Write-Host "[menu-launch] launching instance B (Sandboxie box '$Box')"
    Start-Process -FilePath $Sbie -ArgumentList "/box:$Box", "`"$Exe`"" -WorkingDirectory (Split-Path $Exe)
    Wait-Menu $menuLogB "B" 90 | Out-Null
}

$n = (Get-Process TransportFever2 -ErrorAction SilentlyContinue | Measure-Object).Count
Write-Host "[menu-launch] done -- $n instance(s) at the title menu."
Write-Host "  A: click MULTIPLAYER -> HOST   (code generates + copies to clipboard)"
if (-not $Solo) { Write-Host "  B: click MULTIPLAYER -> JOIN   (reads the code from the clipboard)" }
