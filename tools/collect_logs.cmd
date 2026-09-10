<# : batch launcher -- PowerShell sees this whole block as a comment
@echo off
REM TpF2 Multiplayer / TpF2 Big Maps log collector.
REM Double-click it. It writes tpf2mp-logs-<computer>-<time>.zip to your
REM Downloads folder and opens that folder. Nothing is uploaded anywhere.
set "TPF2_COLLECT_SELF=%~f0"
powershell -NoProfile -ExecutionPolicy Bypass -Command "iex ([IO.File]::ReadAllText($env:TPF2_COLLECT_SELF))"
echo.
pause
exit /b
#>

# ---------------------------------------------------------------------------
# Everything below runs in Windows PowerShell 5.1 (every Windows 10/11 has it).
# Read-only on the player's machine: files are opened for shared reading, so a
# running game keeps writing its logs undisturbed.
# ---------------------------------------------------------------------------
$ErrorActionPreference = "Continue"
$MaxFileBytes = 64MB     # a longer log keeps only its last 64 MB (the end is what matters)
$MaxDumps     = 5        # newest crash dumps, from the last $DumpDays days
$DumpDays     = 14

function Say($m, $c = "Gray") { Write-Host $m -ForegroundColor $c }
$manifest = New-Object System.Collections.Generic.List[string]
function Note($m) { $manifest.Add($m) }

$stamp   = Get-Date -Format "yyyyMMdd-HHmmss"
$name    = "tpf2mp-logs-$($env:COMPUTERNAME)-$stamp"
$stage   = Join-Path $env:TEMP $name
New-Item -ItemType Directory -Force $stage | Out-Null

# Copy a file another process may hold open for writing. Copy-Item refuses
# those; a FileShare.ReadWrite stream does not. Over-long files keep their tail.
function Copy-Shared([string]$src, [string]$destDir, [string]$destName = "") {
    if (-not (Test-Path -LiteralPath $src -PathType Leaf)) { return $false }
    if (-not $destName) { $destName = Split-Path $src -Leaf }
    New-Item -ItemType Directory -Force $destDir | Out-Null
    $dst = Join-Path $destDir $destName
    try {
        $in = [IO.File]::Open($src, 'Open', 'Read', 'ReadWrite, Delete')
        try {
            $len = $in.Length
            if ($len -gt $MaxFileBytes) {
                $in.Seek($len - $MaxFileBytes, 'Begin') | Out-Null
                Note "  TRUNCATED to last $([math]::Round($MaxFileBytes/1MB)) MB of $([math]::Round($len/1MB)) MB: $src"
            }
            $out = [IO.File]::Create($dst)
            try { $in.CopyTo($out) } finally { $out.Dispose() }
        } finally { $in.Dispose() }
        (Get-Item -LiteralPath $dst).LastWriteTime = (Get-Item -LiteralPath $src).LastWriteTime
        Note "  $src ($len bytes)"
        return $true
    } catch {
        Note "  FAILED $src : $($_.Exception.Message)"
        return $false
    }
}

function Copy-Matching([string]$dir, [string[]]$patterns, [string]$destDir) {
    $n = 0
    if (-not (Test-Path -LiteralPath $dir)) { return 0 }
    foreach ($p in $patterns) {
        Get-ChildItem -LiteralPath $dir -File -Filter $p -ErrorAction SilentlyContinue | ForEach-Object {
            if (Copy-Shared $_.FullName $destDir) { $n++ }
        }
    }
    return $n
}

Say ""
Say "Collecting Transport Fever 2 multiplayer / big map logs..." Cyan
Say ""

# ---- where is the game? ----------------------------------------------------
$gameDirs = New-Object System.Collections.Generic.List[string]
function Add-GameDir([string]$d) {
    if (-not $d) { return }
    $d = $d.Trim().Trim('"').TrimEnd('\')
    if (-not (Test-Path -LiteralPath (Join-Path $d "TransportFever2.exe"))) { return }
    $full = (Resolve-Path -LiteralPath $d).Path
    if (-not ($gameDirs | Where-Object { $_ -ieq $full })) { $gameDirs.Add($full) }
}
function Reg([string]$key, [string]$value) {
    try { return (Get-ItemProperty -LiteralPath $key -Name $value -ErrorAction Stop).$value } catch { return $null }
}

# 1. a running game says exactly where it is
Get-Process TransportFever2 -ErrorAction SilentlyContinue | ForEach-Object {
    try { Add-GameDir (Split-Path $_.Path -Parent) } catch {}
}
# 2. folders our installers remembered (TpF2 Multiplayer, TpF2 Big Maps)
Get-ChildItem "HKLM:\SOFTWARE\silver2127", "HKLM:\SOFTWARE\WOW6432Node\silver2127" -ErrorAction SilentlyContinue | ForEach-Object {
    Add-GameDir (Reg $_.PSPath "InstallFolder")
}
# 3. Steam's registration, then every Steam library
Add-GameDir (Reg "HKLM:\SOFTWARE\WOW6432Node\Microsoft\Windows\CurrentVersion\Uninstall\Steam App 1066780" "InstallLocation")
$steam = Reg "HKCU:\Software\Valve\Steam" "SteamPath"
if (-not $steam) { $steam = Reg "HKLM:\SOFTWARE\WOW6432Node\Valve\Steam" "InstallPath" }
if ($steam) { $steam = $steam -replace '/', '\' }
$libs = @()
if ($steam) {
    $libs += $steam
    $vdf = Join-Path $steam "steamapps\libraryfolders.vdf"
    if (Test-Path -LiteralPath $vdf) {
        $libs += [regex]::Matches((Get-Content -LiteralPath $vdf -Raw), '"path"\s+"([^"]+)"') | ForEach-Object { $_.Groups[1].Value -replace '\\\\', '\' }
    }
}
foreach ($l in $libs) { Add-GameDir (Join-Path $l "steamapps\common\Transport Fever 2") }
# 4. GOG
Get-ChildItem "HKLM:\SOFTWARE\WOW6432Node\GOG.com\Games", "HKLM:\SOFTWARE\GOG.com\Games" -ErrorAction SilentlyContinue | ForEach-Object {
    if ("$(Reg $_.PSPath 'gameName')" -like "*Transport Fever 2*") { Add-GameDir (Reg $_.PSPath "path") }
}
Get-ChildItem "HKLM:\SOFTWARE\WOW6432Node\Microsoft\Windows\CurrentVersion\Uninstall", "HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall" -ErrorAction SilentlyContinue | ForEach-Object {
    if ("$(Reg $_.PSPath 'DisplayName')" -like "Transport Fever 2*") { Add-GameDir (Reg $_.PSPath "InstallLocation") }
}
# 5. the usual default folders
foreach ($d in @("${env:ProgramFiles(x86)}\Steam\steamapps\common\Transport Fever 2",
                 "$env:ProgramFiles\Steam\steamapps\common\Transport Fever 2",
                 "$env:ProgramFiles\GOG Galaxy\Games\Transport Fever 2",
                 "${env:ProgramFiles(x86)}\GOG Galaxy\Games\Transport Fever 2",
                 "C:\GOG Games\Transport Fever 2")) { Add-GameDir $d }

# ---- where are the game's own user files (stdout, crash dumps, settings)? --
$userDirs = New-Object System.Collections.Generic.List[string]
function Add-UserDir([string]$d) {
    if (-not $d -or -not (Test-Path -LiteralPath $d)) { return }
    if (-not ((Test-Path -LiteralPath (Join-Path $d "crash_dump")) -or (Test-Path -LiteralPath (Join-Path $d "settings.lua")))) { return }
    $full = (Resolve-Path -LiteralPath $d).Path
    if (-not ($userDirs | Where-Object { $_ -ieq $full })) { $userDirs.Add($full) }
}
if ($steam) {
    Get-ChildItem -LiteralPath (Join-Path $steam "userdata") -Directory -ErrorAction SilentlyContinue | ForEach-Object {
        Add-UserDir (Join-Path $_.FullName "1066780\local")
    }
}
foreach ($d in @("$env:LOCALAPPDATA\Transport Fever 2", "$env:APPDATA\Transport Fever 2",
                 "$([Environment]::GetFolderPath('MyDocuments'))\Transport Fever 2",
                 "$env:LOCALAPPDATA\Urban Games\Transport Fever 2", "$env:APPDATA\Urban Games\Transport Fever 2")) {
    Add-UserDir $d
    Add-UserDir (Join-Path $d "local")
}
foreach ($g in $gameDirs) { Add-UserDir (Join-Path $g "userdata"); Add-UserDir (Join-Path $g "local") }

# ---- 1. mod data folder (logs of the DLLs, the Lua script, plugins) --------
$dataDir = $env:TPF2MP_DATADIR
if (-not $dataDir) { $dataDir = Join-Path $env:LOCALAPPDATA "tpf2mp\data" }
Note "== data folder: $dataDir"
if (Test-Path -LiteralPath $dataDir) {
    $n  = Copy-Matching $dataDir @("*.log", "*.txt", "*.cfg", "*.json", "*.jsonl") (Join-Path $stage "data")
    $n += Copy-Matching (Join-Path $dataDir "plugins") @("*.cfg", "*.log", "*.txt") (Join-Path $stage "data\plugins")
    Say "  mod data folder:  $n file(s)"
} else {
    Note "  (missing)"
    Say "  mod data folder:  not found ($dataDir)" Yellow
}

# ---- 2. game folder(s) -----------------------------------------------------
if ($gameDirs.Count -eq 0) { Say "  game folder:      NOT FOUND -- is Transport Fever 2 installed?" Yellow; Note "== game folder: not found" }
$i = 0
foreach ($g in $gameDirs) {
    $i++
    $dest = Join-Path $stage "game$i"
    Note "== game folder $i : $g"
    $n  = Copy-Matching $g @("*.log", "*.cfg", "tpf2_menu_flags.txt") $dest
    $n += Copy-Matching (Join-Path $g "netpunch") @("*.log", "*.json", "*.jsonl") (Join-Path $dest "netpunch")
    $n += Copy-Matching (Join-Path $g "plugins") @("*.cfg", "*.log", "*.txt") (Join-Path $dest "plugins")

    # what is installed: our binaries (size, date, sha256) and the game build
    $inv = New-Object System.Collections.Generic.List[string]
    $inv.Add("game folder: $g")
    $exe = Join-Path $g "TransportFever2.exe"
    try {
        $fs = [IO.File]::Open($exe, 'Open', 'Read', 'ReadWrite, Delete')
        try {
            $buf = New-Object byte[] 4096; [void]$fs.Read($buf, 0, 4096)
            $nt = [BitConverter]::ToInt32($buf, 0x3c)
            $ts = [BitConverter]::ToUInt32($buf, $nt + 8)
            $si = [BitConverter]::ToUInt32($buf, $nt + 0x50)
            $when = ([DateTime]'1970-01-01Z').AddSeconds($ts).ToUniversalTime().ToString("yyyy-MM-dd HH:mm") + " UTC"
            $known = switch ($ts) { 0x675abcc6 { "Steam build 35924" } default { "unrecognised build" } }
            $inv.Add(("TransportFever2.exe  {0} bytes  PE TimeDateStamp 0x{1:x8} ({2})  SizeOfImage 0x{3:x8}  -> {4}" -f $fs.Length, $ts, $when, $si, $known))
        } finally { $fs.Dispose() }
    } catch { $inv.Add("TransportFever2.exe unreadable: $($_.Exception.Message)") }
    $bins = @()
    $bins += Get-ChildItem -LiteralPath $g -File -ErrorAction SilentlyContinue | Where-Object { $_.Name -match '^(alut\.dll|alut_game\.dll|tpf2.*\.dll)$' }
    $bins += Get-ChildItem -LiteralPath (Join-Path $g "plugins") -File -Filter "*.dll" -ErrorAction SilentlyContinue
    $bins += Get-ChildItem -LiteralPath (Join-Path $g "netpunch") -File -Filter "*.exe" -ErrorAction SilentlyContinue
    foreach ($b in $bins) {
        $h = try { (Get-FileHash -LiteralPath $b.FullName -Algorithm SHA256 -ErrorAction Stop).Hash.ToLower() } catch { "(locked)" }
        $inv.Add(("{0,-40} {1,10} bytes  {2:yyyy-MM-dd HH:mm}  {3}" -f $b.FullName.Substring($g.Length + 1), $b.Length, $b.LastWriteTime, $h))
    }
    $inv.Add("")
    $inv.Add("mods folder: " + ((Get-ChildItem -LiteralPath (Join-Path $g "mods") -Directory -ErrorAction SilentlyContinue | Select-Object -ExpandProperty Name) -join ", "))
    New-Item -ItemType Directory -Force $dest | Out-Null
    $inv | Set-Content -Encoding UTF8 (Join-Path $dest "installed_files.txt")
    Say "  game folder $i :   $n file(s)  ($g)"
}

# ---- 3. game user folder: stdout, crash dumps, active mods -----------------
if ($userDirs.Count -eq 0) { Say "  game log folder:  NOT FOUND (no crash_dump folder)" Yellow; Note "== game user folder: not found" }
$i = 0
foreach ($u in $userDirs) {
    $i++
    $dest = Join-Path $stage "gameuser$i"
    Note "== game user folder $i : $u"
    $n = 0
    $cd = Join-Path $u "crash_dump"
    if (Copy-Shared (Join-Path $cd "stdout.txt") $dest) { $n++ }
    if (Copy-Shared (Join-Path $u "settings.lua") $dest) { $n++ }   # activeMods
    $since = (Get-Date).AddDays(-$DumpDays)
    $dumps = Get-ChildItem -LiteralPath $cd -File -Filter "*.dmp" -ErrorAction SilentlyContinue |
             Where-Object { $_.LastWriteTime -gt $since } | Sort-Object LastWriteTime -Descending | Select-Object -First $MaxDumps
    foreach ($d in $dumps) {
        if (Copy-Shared $d.FullName (Join-Path $dest "crash_dump")) { $n++ }
        Get-ChildItem -LiteralPath $cd -File -Filter "$($d.BaseName)_stdout*.txt" -ErrorAction SilentlyContinue | ForEach-Object {
            if (Copy-Shared $_.FullName (Join-Path $dest "crash_dump")) { $n++ }
        }
    }
    $allDumps = @(Get-ChildItem -LiteralPath $cd -File -Filter "*.dmp" -ErrorAction SilentlyContinue).Count
    Note "  crash dumps: $allDumps in folder, newest $(@($dumps).Count) from the last $DumpDays days copied"
    $mods = Get-ChildItem -LiteralPath (Join-Path $u "mods") -Directory -ErrorAction SilentlyContinue | Select-Object -ExpandProperty Name
    if ($mods) { ("user mods folder: " + ($mods -join ", ")) | Set-Content -Encoding UTF8 (Join-Path $dest "user_mods.txt") }
    Say "  game log folder $i : $n file(s)  ($u)"
}

# ---- 4. system summary -----------------------------------------------------
$sys = New-Object System.Collections.Generic.List[string]
$sys.Add("collected: $(Get-Date -Format 'yyyy-MM-dd HH:mm:ss zzz')")
try {
    $os  = Get-CimInstance Win32_OperatingSystem
    $cpu = Get-CimInstance Win32_Processor | Select-Object -First 1
    $sys.Add("os:  $($os.Caption) $($os.Version) build $($os.BuildNumber)")
    $sys.Add("ram: $([math]::Round($os.TotalVisibleMemorySize/1MB, 1)) GB total, $([math]::Round($os.FreePhysicalMemory/1MB, 1)) GB free")
    $sys.Add("cpu: $($cpu.Name)")
    Get-CimInstance Win32_VideoController | ForEach-Object { $sys.Add("gpu: $($_.Name) driver $($_.DriverVersion)") }
    Get-CimInstance Win32_LogicalDisk -Filter "DriveType=3" | ForEach-Object { $sys.Add("disk $($_.DeviceID) $([math]::Round($_.FreeSpace/1GB)) GB free of $([math]::Round($_.Size/1GB)) GB") }
} catch { $sys.Add("system query failed: $($_.Exception.Message)") }
$running = @(Get-Process TransportFever2 -ErrorAction SilentlyContinue)
$sys.Add("game running while collecting: $($running.Count) instance(s)")
$sys.Add("")
Get-ChildItem "HKLM:\SOFTWARE\silver2127", "HKLM:\SOFTWARE\WOW6432Node\silver2127" -ErrorAction SilentlyContinue | ForEach-Object {
    $sys.Add("installed: $($_.PSChildName)  version $(Reg $_.PSPath 'Version')  folder $(Reg $_.PSPath 'InstallFolder')")
}
Get-ChildItem "HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall", "HKLM:\SOFTWARE\WOW6432Node\Microsoft\Windows\CurrentVersion\Uninstall" -ErrorAction SilentlyContinue | ForEach-Object {
    $dn = "$(Reg $_.PSPath 'DisplayName')"
    if ($dn -match 'TpF2|Transport Fever') { $sys.Add("uninstall entry: $dn  $(Reg $_.PSPath 'DisplayVersion')") }
}
$heap = Reg "HKLM:\SOFTWARE\Microsoft\Windows NT\CurrentVersion\Image File Execution Options\TransportFever2.exe" "FrontEndHeapDebugOptions"
$sys.Add("segment heap switch (FrontEndHeapDebugOptions): $heap")
$sys.Add("TPF2MP_DATADIR: $env:TPF2MP_DATADIR")
$sys | Set-Content -Encoding UTF8 (Join-Path $stage "system.txt")
$manifest | Set-Content -Encoding UTF8 (Join-Path $stage "manifest.txt")

# ---- 5. zip into Downloads -------------------------------------------------
$downloads = $null
try { $downloads = (New-Object -ComObject Shell.Application).Namespace('shell:Downloads').Self.Path } catch {}
if (-not $downloads -or -not (Test-Path -LiteralPath $downloads)) { $downloads = Join-Path $env:USERPROFILE "Downloads" }
if (-not (Test-Path -LiteralPath $downloads)) { $downloads = [Environment]::GetFolderPath('Desktop') }
$zip = Join-Path $downloads "$name.zip"

Add-Type -AssemblyName System.IO.Compression.FileSystem
try {
    # Entries one by one: .NET Framework's CreateFromDirectory writes '\' in
    # entry names, which non-Windows unzip tools turn into flat file names.
    $za = [IO.Compression.ZipFile]::Open($zip, 'Create')
    try {
        Get-ChildItem -LiteralPath $stage -Recurse -File | ForEach-Object {
            $rel = $_.FullName.Substring($stage.Length + 1) -replace '\\', '/'
            [void][IO.Compression.ZipFileExtensions]::CreateEntryFromFile($za, $_.FullName, $rel, [IO.Compression.CompressionLevel]::Optimal)
        }
    } finally { $za.Dispose() }
} catch {
    Say ""
    Say "Could not write the zip: $($_.Exception.Message)" Red
    Say "The collected files are still in: $stage" Yellow
    return
}
Remove-Item -LiteralPath $stage -Recurse -Force -ErrorAction SilentlyContinue

Say ""
Say "Done: $zip ($([math]::Round((Get-Item -LiteralPath $zip).Length / 1MB, 1)) MB)" Green
if ($running.Count -gt 0) {
    Say "Note: the game is still running, so its own log (stdout.txt) may be incomplete." Yellow
    Say "      If you can, close the game and run this again. Do NOT start the game before" Yellow
    Say "      collecting -- it wipes that log on launch." Yellow
}
Say ""
Say "Send this zip privately (DM, not a public channel): the lobby logs contain IP addresses." Yellow
try { Start-Process explorer.exe "/select,`"$zip`"" } catch {}
