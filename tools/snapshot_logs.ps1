<#
.SYNOPSIS
Copy every instance's logs into a timestamped folder BEFORE a restart wipes them.

The game truncates stdout.txt (where the Lua log() lines go) and the lockstep
data files on every launch. A restart right after a report therefore destroys
the evidence of the run being reported (lost a B-vs-C divergence, 2026-09-02).
Run this first, then kill/relaunch. Folders are named by Sandboxie box, not by
lockstep letter -- letters are assigned per launch and swap between boxes.

  tools\snapshot_logs.ps1 [-Tag <word>]     -> %LOCALAPPDATA%\tpf2mp\runs\<stamp>[-tag]\{native,GameAgent,GameAgent2}\
#>
param([string]$Tag = "")

$stamp = Get-Date -Format "yyyyMMdd-HHmmss"
if ($Tag -ne "") { $stamp = "$stamp-$Tag" }
$dest = Join-Path $env:LOCALAPPDATA "tpf2mp\runs\$stamp"
New-Item -ItemType Directory -Force $dest | Out-Null

$stdoutRel = "Program Files (x86)\Steam\userdata\*\1066780\local\crash_dump\stdout.txt"
$dataRel   = "AppData\Local\tpf2mp\data"
# The Lua writes some side files (egeo_*.txt) to the process CWD = the GAME dir,
# not the data dir. Collecting only the data dir silently dropped every geometry
# dump from seven run snapshots, so a height diff could not be re-checked after
# the fact. Each source therefore has a game dir too.
$gameRel = "Program Files (x86)\Steam\steamapps\common\Transport Fever 2"
$sources = @(
    @{ name = "native";     stdout = "C:\$stdoutRel";                                        data = (Join-Path $env:LOCALAPPDATA "tpf2mp\data");                    game = "C:\$gameRel" },
    @{ name = "GameAgent";  stdout = "C:\Sandbox\$env:USERNAME\GameAgent\drive\C\$stdoutRel";  data = "C:\Sandbox\$env:USERNAME\GameAgent\user\current\$dataRel";  game = "C:\Sandbox\$env:USERNAME\GameAgent\drive\C\$gameRel" },
    @{ name = "GameAgent2"; stdout = "C:\Sandbox\$env:USERNAME\GameAgent2\drive\C\$stdoutRel"; data = "C:\Sandbox\$env:USERNAME\GameAgent2\user\current\$dataRel"; game = "C:\Sandbox\$env:USERNAME\GameAgent2\drive\C\$gameRel" }
)
$gameFiles = @("egeo_*.txt", "tpf2_slice.cfg")
# tpf2_instance.txt and mp_company_cfg.txt are two lines each and they are what
# makes a snapshot SELF-DESCRIBING. Without the identity file nothing afterwards
# can say which LETTER a box held -- and a box's folder contains several
# instances' dashboards, because Sandboxie shows it the real filesystem under
# its own writes. Without the company cfg, "the balances differ" cannot be told
# from "this was companies mode, where they are supposed to". Both were missing,
# and tools\soak.ps1 -FromSnapshot has to guess when they are.
$dataFiles = @("lockstep_dash_*.txt", "lockstep_status_*.txt", "tpf2_events_*.txt", "lockstep_inject_*.txt",
               "tpf2_slice.log", "tpf2_capture_*.txt", "mp_company_*.log",
               "tpf2_instance.txt", "mp_company_cfg.txt")

$n = 0
foreach ($s in $sources) {
    $out = Join-Path $dest $s.name
    New-Item -ItemType Directory -Force $out | Out-Null
    $so = Get-Item $s.stdout -ErrorAction SilentlyContinue | Select-Object -First 1
    if ($so) { Copy-Item $so.FullName (Join-Path $out "stdout.txt") -Force; $n++ }
    if (Test-Path $s.data) {
        foreach ($pat in $dataFiles) {
            Get-ChildItem (Join-Path $s.data $pat) -ErrorAction SilentlyContinue | ForEach-Object {
                Copy-Item $_.FullName $out -Force; $n++
            }
        }
    }
    if ($s.game -and (Test-Path $s.game)) {
        foreach ($pat in $gameFiles) {
            Get-ChildItem (Join-Path $s.game $pat) -ErrorAction SilentlyContinue | ForEach-Object {
                Copy-Item $_.FullName $out -Force; $n++
            }
        }
    }
}
Write-Host "snapshot: $n file(s) -> $dest"
