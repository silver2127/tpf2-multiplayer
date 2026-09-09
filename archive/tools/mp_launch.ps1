# mp_launch.ps1 -- one command to bring up a lockstep session.
#
#   tools\mp_launch.ps1                       relaunch on whatever save is newest
#   tools\mp_launch.ps1 -Save MPTESTINGII     sync that save host -> peer, make it
#                                             newest on BOTH, relaunch into it
#   tools\mp_launch.ps1 -Dll <path>           inject a specific slice dll
#
# Does: optional save sync, autotest -LaunchOnly (deploys the mod, launches A and
# sandboxed B, clicks CONTINUE politely), then injects the slice into both and
# copies the cfg into the overlay so B has its own switches.
param(
    [string]$Save = "",
    [string]$Dll = ""
)
$ErrorActionPreference = "Stop"
# Exactly one harness at a time: a stale click-loop running alongside a new
# launch fights the user for focus (measured 2026-08-29).
Get-CimInstance Win32_Process -Filter "Name = 'powershell.exe'" |
    Where-Object { $_.ProcessId -ne $PID -and $_.CommandLine -match 'autotest|mp_launch|slice_two_way' } |
    ForEach-Object { Write-Host "[mp] killing stale harness pid=$($_.ProcessId)"; Stop-Process -Id $_.ProcessId -Force -ErrorAction SilentlyContinue }
$R = Split-Path -Parent $PSScriptRoot
$Out = "C:\Program Files (x86)\Steam\steamapps\workshop\content\1066780\3710243057\recon\m4\out"
$Ovl = "C:\Sandbox\$env:USERNAME\GameAgent\drive\C" + $Out.Substring(2)
$SaveA = "$((Get-ChildItem "C:\Program Files (x86)\Steam\userdata" -Directory | Where-Object { Test-Path (Join-Path $_.FullName "1066780") } | Select-Object -First 1).FullName)\1066780\local\save"
$SaveB = "C:\Sandbox\$env:USERNAME\GameAgent\drive\C" + $SaveA.Substring(2)

if (-not $Dll) {
    $Dll = (Get-ChildItem "$R\bridge\out\tpf2_slice*.dll" | Sort-Object LastWriteTime | Select-Object -Last 1).FullName
}
Write-Host "[mp] slice: $Dll"

# Ship the latest native in-game menu DLL to the workshop out dir that proxy_alut
# loads at startup. A running game holds it open, so close instances first
# (autotest relaunches them below). This keeps the lobby UI in lockstep with src.
$MenuSrc = "$R\bridge\out\tpf2_menu.dll"
if (Test-Path $MenuSrc) {
    Get-Process TransportFever2 -ErrorAction SilentlyContinue | Stop-Process -Force -ErrorAction SilentlyContinue
    Start-Sleep -Milliseconds 400
    try { Copy-Item $MenuSrc (Join-Path $Out "tpf2_menu.dll") -Force; Write-Host "[mp] deployed menu DLL -> workshop out" }
    catch { Write-Host "[mp] menu DLL deploy failed: $($_.Exception.Message)" }
}

if ($Save) {
    # CONTINUE loads the newest save, so the synced save must be newest on BOTH.
    $src = Join-Path $SaveA "$Save.sav"
    if (-not (Test-Path $src)) { Write-Host "[mp] no such save on the host: $src"; exit 1 }
    if (-not (Test-Path $SaveB)) { New-Item -ItemType Directory -Force $SaveB | Out-Null }
    $now = Get-Date
    foreach ($ext in @(".sav", ".sav.lua", ".jpg")) {
        $f = Join-Path $SaveA "$Save$ext"
        if (Test-Path $f) {
            Copy-Item $f (Join-Path $SaveB "$Save$ext") -Force
            (Get-Item $f).LastWriteTime = $now
            (Get-Item (Join-Path $SaveB "$Save$ext")).LastWriteTime = $now
        }
    }
    $mb = [int]((Get-Item $src).Length / 1MB)
    Write-Host "[mp] synced save '$Save' (${mb} MB) host -> peer, stamped newest on both"
}

& "$R\tools\autotest.ps1" -LaunchOnly
if ($LASTEXITCODE -ne 0) { Write-Host "[mp] autotest failed ($LASTEXITCODE)"; exit 1 }

$idA = @(Get-Content (Join-Path $Out "tpf2_instance.txt"))
$idB = @(Get-Content (Join-Path $Ovl "tpf2_instance.txt"))
if ($idA[1] -notmatch 'pid=(\d+)') { Write-Host "[mp] no A pid"; exit 1 }; $pidA = [int]$Matches[1]
if ($idB[1] -notmatch 'pid=(\d+)') { Write-Host "[mp] no B pid"; exit 1 }; $pidB = [int]$Matches[1]
Copy-Item (Join-Path $Out "tpf2_slice.cfg") (Join-Path $Ovl "tpf2_slice.cfg") -Force

Write-Host "[mp] injecting into A pid=$pidA and B pid=$pidB"
& "$R\injector\injector.exe" $pidA $Dll | Out-Host
if ($LASTEXITCODE -ne 0) { Write-Host "[mp] inject A failed"; exit 1 }
& "$R\injector\injector.exe" $pidB $Dll | Out-Host
if ($LASTEXITCODE -ne 0) { Write-Host "[mp] inject B failed"; exit 1 }
Start-Sleep -Seconds 8
Write-Host "[mp] A: $((Get-Content (Join-Path $Out 'tpf2_slice.log') -TotalCount 1))"
Write-Host "[mp] B: $((Get-Content (Join-Path $Ovl 'tpf2_slice.log') -TotalCount 1))"
Write-Host "[mp] session up."
