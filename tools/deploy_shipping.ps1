# deploy_shipping.ps1 -- lay the game folder out EXACTLY as the MSI will, from the
# dev build outputs, so the rig test exercises the shipping paths (not the old
# workshop out dir):
#
#   <game>\alut.dll                 proxy (stock already renamed alut_real.dll by install_proxy.ps1)
#   <game>\tpf2_bridge_mp.dll       lockstep transport  (proxy loads it: next-to-proxy rule)
#   <game>\tpf2_menu.dll            lobby overlay
#   <game>\tpf2_slice.dll           capture/cancel hooks (proxy loads it at start)
#   <game>\tpf2_bridge_mp.cfg       installer/cfg defaults
#   <game>\tpf2_slice.cfg
#   <game>\netpunch\netpunch.exe    frozen lobby (menu resolves NETDIR = <dll dir>\netpunch)
#   <game>\mods\mp_lockstep_1\**    the Lua mod (deploy_mod.ps1 also refreshes the userdata copy)
#
# Runtime data goes to %LOCALAPPDATA%\tpf2mp\data (created by the DLLs). Instance
# B under Sandboxie gets its own copy of that dir inside the box.
#
#   tools\deploy_shipping.ps1            deploy everything (game must be closed)
#   tools\deploy_shipping.ps1 -Clean     also wipe %LOCALAPPDATA%\tpf2mp\data (fresh identity/logs)
param([switch]$Clean)
$ErrorActionPreference = "Stop"
$Repo = Split-Path -Parent $PSScriptRoot
$Game = $null
foreach ($k in @('HKLM:\SOFTWARE\WOW6432Node\Microsoft\Windows\CurrentVersion\Uninstall\Steam App 1066780',
                 'HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall\Steam App 1066780')) {
    try { $v = (Get-ItemProperty $k -Name InstallLocation -EA Stop).InstallLocation; if ($v -and (Test-Path (Join-Path $v 'TransportFever2.exe'))) { $Game = $v; break } } catch {}
}
if (-not $Game) { $Game = "C:\Program Files (x86)\Steam\steamapps\common\Transport Fever 2" }
if (-not (Test-Path (Join-Path $Game 'TransportFever2.exe'))) { throw "game not found at $Game" }
if (Get-Process TransportFever2 -EA SilentlyContinue) { throw "close both game instances first" }
Write-Host "[ship] game dir: $Game"

function Put($src, $dst) {
    if (-not (Test-Path $src)) { throw "missing build output: $src" }
    Copy-Item $src $dst -Force
    Write-Host ("[ship]   {0,-24} <- {1}  ({2:N0} B, {3:HH:mm})" -f (Split-Path $dst -Leaf), $src.Replace($Repo + '\', ''), (Get-Item $src).Length, (Get-Item $src).LastWriteTime)
}
if (-not (Test-Path (Join-Path $Game 'alut_real.dll'))) { throw "alut_real.dll absent: run tools\install_proxy.ps1 once first" }
Put "$Repo\bridge\out\alut.dll"           (Join-Path $Game 'alut.dll')
Put "$Repo\bridge\out\tpf2_bridge_mp.dll" (Join-Path $Game 'tpf2_bridge_mp.dll')
Put "$Repo\bridge\out\tpf2_menu.dll"      (Join-Path $Game 'tpf2_menu.dll')
# The CANONICAL name, never "whatever is newest".
#
# This used to be `Get-ChildItem tpf2_slice*.dll | Sort LastWriteTime | Last 1`.
# build_slice.bat takes an optional suffix (an injected DLL stays locked for the
# life of the game process, so iterating means building tpf2_slice_foo.dll), and
# every one of those lands in the same out dir -- so the last experiment anyone
# built silently became the shipped artifact, under the shipping name, with
# nothing in the output saying which file it actually was.
$sliceSrc = "$Repo\bridge\out\tpf2_slice.dll"
if (-not (Test-Path $sliceSrc)) {
    throw "missing build output: $sliceSrc -- run bridge\build_slice.bat with NO suffix argument (a suffixed build is a dev iteration and is never shipped)"
}
# A suffixed build that is newer is almost always the one being worked on, and
# shipping the stale canonical DLL instead is just as silent a failure the other
# way round. Say so; do not guess.
$sliceStamp = (Get-Item $sliceSrc).LastWriteTime
Get-ChildItem "$Repo\bridge\out\tpf2_slice*.dll" -EA SilentlyContinue |
    Where-Object { $_.Name -ne 'tpf2_slice.dll' -and $_.LastWriteTime -gt $sliceStamp } |
    ForEach-Object { Write-Warning "[ship] $($_.Name) is NEWER than tpf2_slice.dll and will NOT be shipped -- rebuild without a suffix if that is the one you want" }
Put $sliceSrc                             (Join-Path $Game 'tpf2_slice.dll')
Put "$Repo\installer\cfg\tpf2_bridge_mp.cfg" (Join-Path $Game 'tpf2_bridge_mp.cfg')
Put "$Repo\installer\cfg\tpf2_slice.cfg"     (Join-Path $Game 'tpf2_slice.cfg')
New-Item -ItemType Directory -Force (Join-Path $Game 'netpunch') | Out-Null
Put "$Repo\netpunch\dist\netpunch.exe"    (Join-Path $Game 'netpunch\netpunch.exe')

& "$Repo\tools\deploy_mod.ps1" -Mod mp_lockstep_1 | Select-Object -Last 1

if ($Clean) {
    foreach ($d in @((Join-Path $env:LOCALAPPDATA 'tpf2mp\data'),
                     "C:\Sandbox\$env:USERNAME\GameAgent\user\current\AppData\Local\tpf2mp\data",
                     "C:\Sandbox\$env:USERNAME\GameAgent2\user\current\AppData\Local\tpf2mp\data")) {
        if (Test-Path $d) { Remove-Item "$d\*" -Recurse -Force -EA SilentlyContinue; Write-Host "[ship] cleaned $d" }
    }
}
Write-Host "[ship] done. Launch with tools\mp_menu_launch.ps1 (proxy now loads bridge+menu+slice at start; no injector)." -ForegroundColor Green
