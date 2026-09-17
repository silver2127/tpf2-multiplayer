# auto_install.ps1 -- build while the games run, install the moment they are closed.
#
# Sits in a loop. Every few seconds it (1) rebuilds whatever native target or the
# frozen lobby is older than its sources (builds never touch a running game: the
# outputs go to native\out and netpunch\dist), and (2) once no TransportFever2.exe
# is running -- the normal instance and the Sandboxie ones alike -- and the game
# folder is older than the outputs, deploys them exactly as tools\deploy_shipping.ps1
# does, refreshes the Sandboxie overlay of instance B, and (with -Relaunch) brings
# both instances back to the title menu.
#
#   tools\auto_install.ps1                 watch, build, install; leave the games closed
#   tools\auto_install.ps1 -Relaunch       ... and relaunch A + B after an install
#   tools\auto_install.ps1 -Once           one pass (install now if closed and stale), no loop
#   tools\auto_install.ps1 -NoBuild        install what is built, never compile
#
# Stop it with Ctrl+C. It never kills a game.
param([switch]$Relaunch, [switch]$Once, [switch]$NoBuild, [int]$PollSeconds = 5, [string]$Box = 'GameAgent',
      [string[]]$Boxes = @('GameAgent', 'GameAgent2', 'GameAgent3'))

$ErrorActionPreference = 'Stop'
$Repo = Split-Path $PSScriptRoot -Parent
$Game = "C:\Program Files (x86)\Steam\steamapps\common\Transport Fever 2"
foreach ($k in 'HKLM:\SOFTWARE\WOW6432Node\Microsoft\Windows\CurrentVersion\Uninstall\Steam App 1066780',
               'HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall\Steam App 1066780') {
    try { $v = (Get-ItemProperty $k -Name InstallLocation -EA Stop).InstallLocation; if ($v -and (Test-Path (Join-Path $v 'TransportFever2.exe'))) { $Game = $v; break } } catch {}
}
$Overlay = "C:\Sandbox\$env:USERNAME\$Box\drive\C\" + ($Game -replace '^[A-Za-z]:\\', '')

# what is built from what: target -> (output, source globs)
$Targets = @(
    @{ name = 'proxy'; out = 'native\out\tpf2_bridge_mp.dll'; src = @('native\src\net.cpp', 'native\src\net.h', 'native\src\hook.cpp', 'native\src\hook.h', 'native\src\speedhook.*', 'native\src\setplayer_patch.*', 'native\src\cgamesteprelay.asm', 'native\src\setplayerrelay.asm', 'native\src\bridge_main.cpp', 'native\src\proxy_alut.cpp', 'native\src\datadir.h', 'native\src\logarchive.*', 'native\src\update_bootstrap.*') },
    @{ name = 'menu';  out = 'native\out\tpf2_menu.dll';      src = @('native\src\menu_hook.cpp', 'native\src\hook.cpp', 'native\src\hook.h', 'native\src\native_io.*', 'native\src\native_control.*', 'native\src\gameuirelay.asm', 'native\src\logarchive.*', 'native\src\datadir.h') },
    @{ name = 'slice'; out = 'native\out\tpf2_slice.dll';     src = @('native\src\slice_hook.cpp', 'native\src\hook.cpp', 'native\src\hook.h', 'native\src\deferrelay_slice.asm', 'native\src\station_weld.h', 'native\src\datadir.h') },
    @{ name = 'host';  out = 'native\out\tpf2_pluginhost.dll'; src = @('native\src\plugin\*', 'native\src\hook.cpp', 'native\src\hook.h') }
)
# native plugins built in their own sibling checkouts; deploy_shipping.ps1 ships them into
# <game>\plugins. From a worktree (<main>\.claude\worktrees\<name>) the siblings sit next
# to the main checkout. A plugin rebuilt on its own must trigger an install too.
$SiblingRoot = Split-Path $Repo -Parent
if ($Repo -match '^(.*)\\\.claude\\worktrees\\[^\\]+$') { $SiblingRoot = Split-Path $Matches[1] -Parent }
$Plugins = @(
    @{ name = 'bigmap'; repo = (Join-Path $SiblingRoot 'tpf2-bigmap'); out = 'out\tpf2_bigmap.dll'; ok = 'BUILD BIGMAP OK'
       src = @('src\*', 'mod\minimap\*', 'tools\embed_lua.ps1', 'build.bat') }
)

# Mods to REMOVE from the game folder and every box overlay at the next install:
# old recon/test mods that must not stay in anyone's mod list (m3_determinism_1
# was the day-hash determinism probe, 2026-09-16). A running game keeps its
# loaded copy in memory, so this waits for the games to close like everything else.
$RemoveMods = @('m3_determinism_1')
function Say($m, $c = 'Gray') { Write-Host ("[auto-install {0:HH:mm:ss}] {1}" -f (Get-Date), $m) -ForegroundColor $c }
function Newest($paths, $root = $Repo) {
    $t = [datetime]0
    foreach ($p in $paths) { foreach ($f in (Get-ChildItem (Join-Path $root $p) -Recurse -File -EA SilentlyContinue)) { if ($f.LastWriteTime -gt $t) { $t = $f.LastWriteTime } } }
    return $t
}
function GamesRunning { return @(Get-Process TransportFever2 -EA SilentlyContinue).Count }

function BuildStale {
    if ($NoBuild) { return }
    foreach ($t in $Targets) {
        $out = Join-Path $Repo $t.out
        $outT = if (Test-Path $out) { (Get-Item $out).LastWriteTime } else { [datetime]0 }
        if ((Newest $t.src) -gt $outT) {
            Say "building $($t.name) (sources newer than $($t.out))" Cyan
            $env:TPF2_BUILD_NO_DEPLOY = '1'
            $log = cmd /c "`"$Repo\native\build.bat`" $($t.name) 2>&1"   # redirect INSIDE cmd: PS 5.1 turns a native stderr line (a stray vswhere message, 2026-09-16) into a terminating error
            if ($LASTEXITCODE -ne 0 -or -not ($log -match 'BUILD .* OK')) { Say ("build $($t.name) FAILED:`n" + ($log | Select-String 'error' | Out-String)) Red; continue }
            Say "built $($t.name)" Green
        }
    }
    foreach ($p in $Plugins) {
        if (-not (Test-Path (Join-Path $p.repo 'build.bat'))) { continue }
        $out = Join-Path $p.repo $p.out
        $outT = if (Test-Path $out) { (Get-Item $out).LastWriteTime } else { [datetime]0 }
        if ((Newest $p.src $p.repo) -gt $outT) {
            Say "building $($p.name) (sources newer than $out)" Cyan
            $log = cmd /c "`"$($p.repo)\build.bat`" 2>&1"
            if ($LASTEXITCODE -ne 0 -or -not ($log -match $p.ok)) { Say ("build $($p.name) FAILED:`n" + ($log | Select-String 'error' | Out-String)) Red; continue }
            Say "built $($p.name)" Green
        }
    }
    $exe = Join-Path $Repo 'netpunch\dist\netpunch.exe'
    $exeT = if (Test-Path $exe) { (Get-Item $exe).LastWriteTime } else { [datetime]0 }
    if ((Newest @('netpunch\*.py')) -gt $exeT) {
        Say 'freezing netpunch (lobby sources newer than dist\netpunch.exe)' Cyan
        Push-Location (Join-Path $Repo 'netpunch')
        try { $log = cmd /c "python -m PyInstaller --onefile --name netpunch lobby.py --noconfirm --log-level WARN 2>&1" } finally { Pop-Location }   # via cmd: PS 5.1 turns PyInstaller's stderr INFO lines into terminating errors
        if ((Test-Path $exe) -and ((Get-Item $exe).LastWriteTime -gt $exeT)) { Say 'froze netpunch' Green } else { Say ("freeze FAILED:`n" + ($log | Select-Object -Last 5 | Out-String)) Red }
    }
}

function InstallStale {
    # newest built output vs the game folder's copies (the mod tree too)
    $built = Newest @('native\out\alut.dll', 'native\out\tpf2_menu.dll', 'native\out\tpf2_slice.dll', 'native\out\tpf2_pluginhost.dll', 'netpunch\dist\netpunch.exe', 'mod\mp_lockstep_1')
    $deployed = [datetime]0
    foreach ($f in 'alut.dll', 'tpf2_menu.dll', 'tpf2_slice.dll', 'tpf2_pluginhost.dll', 'netpunch\netpunch.exe') {
        $p = Join-Path $Game $f; if (Test-Path $p) { $t = (Get-Item $p).LastWriteTime; if ($t -gt $deployed) { $deployed = $t } }
    }
    foreach ($f in (Get-ChildItem (Join-Path $Game 'mods\mp_lockstep_1') -Recurse -File -EA SilentlyContinue)) { if ($f.LastWriteTime -gt $deployed) { $deployed = $f.LastWriteTime } }
    $why = @()
    if ($built -gt $deployed) { $why += "multiplayer outputs from $built" }
    foreach ($p in $Plugins) {
        $out = Join-Path $p.repo $p.out
        $dst = Join-Path $Game ('plugins\' + (Split-Path $p.out -Leaf))
        if ((Test-Path $out) -and (-not (Test-Path $dst) -or (Get-Item $out).LastWriteTime -gt (Get-Item $dst).LastWriteTime)) {
            $why += "$($p.name) from $((Get-Item $out).LastWriteTime)"
        }
    }
    foreach ($m in $RemoveMods) {
        $paths = @(Join-Path $Game "mods\$m") + ($Boxes | ForEach-Object { "C:\Sandbox\$env:USERNAME\$_\drive\C\" + ($Game -replace '^[A-Za-z]:\\', '') + "\mods\$m" })
        if ($paths | Where-Object { Test-Path $_ }) { $why += "remove $m" }
    }
    if ($why.Count -eq 0) { return $false }
    if ((GamesRunning) -gt 0) {
        $pending = $why -join ', '
        if ($pending -ne $script:LastPending) { Say "install pending ($pending) -- waiting for the games to close" DarkYellow; $script:LastPending = $pending }
        return $false
    }
    $script:LastPending = $null
    Say 'games are closed and the build is newer than the game folder: installing' Cyan
    & powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $Repo 'tools\snapshot_logs.ps1') 2>&1 | Select-Object -Last 1 | ForEach-Object { Say $_ }
    $log = & powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $Repo 'tools\deploy_shipping.ps1') 2>&1
    if ($LASTEXITCODE -ne 0 -or -not ($log -match 'deploy OK')) { Say ("deploy FAILED:`n" + ($log | Select-Object -Last 8 | Out-String)) Red; return $false }
    Say 'deployed to the game folder' Green
    foreach ($m in $RemoveMods) {
        foreach ($d in @(Join-Path $Game "mods\$m") + ($Boxes | ForEach-Object { "C:\Sandbox\$env:USERNAME\$_\drive\C\" + ($Game -replace '^[A-Za-z]:\\', '') + "\mods\$m" })) {
            if (Test-Path $d) { Remove-Item $d -Recurse -Force; Say "removed the old mod $d" Green }
        }
    }
    foreach ($b in $Boxes) {
        $ov = "C:\Sandbox\$env:USERNAME\$b\drive\C\" + ($Game -replace '^[A-Za-z]:\\', '')
        if (-not (Test-Path $ov)) { if ($b -eq $Box) { Say "no Sandboxie overlay at $ov -- instance B not refreshed" DarkYellow }; continue }
        # a boxed instance reads its own overlay copy of the game folder, never the host's --
        # the multiplayer files, the lobby, the mod tree AND the plugins (Big Maps included:
        # the boxes ran a three-day-old tpf2_bigmap.dll without its speedups, 2026-09-16)
        foreach ($f in 'alut.dll', 'tpf2_bridge_mp.dll', 'tpf2_menu.dll', 'tpf2_slice.dll', 'tpf2_pluginhost.dll') {
            $src = Join-Path $Game $f; if (Test-Path $src) { Copy-Item $src (Join-Path $ov $f) -Force }
        }
        New-Item -ItemType Directory -Force (Join-Path $ov 'netpunch'), (Join-Path $ov 'plugins') | Out-Null
        # the box's lobby helper can outlive the game by a few seconds and hold its exe open;
        # a locked copy must not abort the mod/plugin refresh and the relaunch (2026-09-16)
        $lobbyOk = $false
        for ($try = 0; $try -lt 6 -and -not $lobbyOk; $try++) {
            try { Copy-Item (Join-Path $Game 'netpunch\netpunch.exe') (Join-Path $ov 'netpunch\netpunch.exe') -Force -EA Stop; $lobbyOk = $true }
            catch { Start-Sleep -Seconds 2 }
        }
        if (-not $lobbyOk) { Say "the $b overlay's netpunch.exe is still locked (a lingering boxed lobby?) -- lobby NOT refreshed in the box" DarkYellow }
        foreach ($f in (Get-ChildItem (Join-Path $Game 'plugins') -File | Where-Object { $_.Extension -in '.dll', '.cfg' })) {
            Copy-Item $f.FullName (Join-Path $ov ('plugins\' + $f.Name)) -Force
        }
        $mod = Join-Path $ov 'mods\mp_lockstep_1'
        if (Test-Path $mod) { Remove-Item $mod -Recurse -Force }
        Copy-Item (Join-Path $Game 'mods\mp_lockstep_1') $mod -Recurse -Force
        Say "refreshed the $b overlay (multiplayer files, lobby, mod, plugins)" Green
    }
    if ($Relaunch) {
        Say 'relaunching A + B to the title menu' Cyan
        & powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $Repo 'tools\mp_menu_launch.ps1') 2>&1 | Select-String 'title menu|timed out|done' | ForEach-Object { Say $_ }
        if ((GamesRunning) -lt 2) {
            Say 'B did not stay up (first start of a box hands off to Steam) -- launching it again' DarkYellow
            Start-Process -FilePath 'C:\Program Files\Sandboxie-Plus\Start.exe' -ArgumentList "/box:$Box", "`"$Game\TransportFever2.exe`"" -WorkingDirectory $Game -WindowStyle Hidden
        }
    }
    return $true
}

Say "watching $Game (overlay: $Overlay); build on change, install when closed" White
do {
    try {
        BuildStale
        InstallStale | Out-Null
    } catch { Say ("error: " + $_.Exception.Message) Red }
    if ($Once) { break }
    Start-Sleep -Seconds $PollSeconds
} while ($true)
