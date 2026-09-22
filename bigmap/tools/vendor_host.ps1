<#
.SYNOPSIS
Copies the three binaries TpF2 Big Maps SHARES with TpF2 Multiplayer into
installer\vendor\ and records where they came from.

.DESCRIPTION
The proxy (alut.dll), the plugin host (tpf2_pluginhost.dll) and the installer
custom actions (tpf2ca.dll) are built in the tpf2-multiplayer repository. Both
packages must ship the SAME bytes under the SAME component GUIDs (PluginHost.wxs)
so Windows Installer can treat them as one shared component and the two products
can be installed and removed in any order. So this repository never builds them;
it vendors them and writes vendor\VENDORED.md with their source and a SHA-256
per file.

Two sources:
  -FromMsi  a released TpF2Multiplayer.msi -- the one to use for a release. The
            proxy and the host come out of an administrative image (msiexec /a:
            nothing is installed), the custom actions out of the MSI's Binary
            table. These are the bytes that release's players have; rebuilding
            the same commit does not give them (MSVC output is not reproducible).
  checkout  (default) the build outputs of a tpf2-multiplayer checkout:
            native\out\ and installer\out\. -Build builds them first.

It also checks that installer\PluginHost.wxs matches the tpf2-multiplayer copy
(with -Release, the copy at that tag), because a drift there means the two
packages disagree about a GUID -- the exact failure the arrangement exists to
prevent. Line endings are not compared: they follow each checkout's core.autocrlf.

.PARAMETER MpRepo
Path to the tpf2-multiplayer checkout. Defaults to ..\tpf2-multiplayer beside
this repository.

.PARAMETER FromMsi
A released TpF2Multiplayer.msi to vendor from.

.PARAMETER Release
The tpf2-multiplayer tag that MSI was released as (for example v0.4.18).
Required with -FromMsi: recorded in VENDORED.md, and PluginHost.wxs is compared
with that tag's copy (the checkout must have the tag fetched).

.PARAMETER Build
Checkout mode: run native\build.bat host and installer\ca\build_ca.bat in that
checkout first. Close the game: a loaded DLL cannot be relinked (LNK1104).

.EXAMPLE
powershell -File tools\vendor_host.ps1 -FromMsi TpF2Multiplayer.msi -Release v0.4.18

.EXAMPLE
powershell -File tools\vendor_host.ps1 -Build
#>
[CmdletBinding()]
param(
    [string]$MpRepo = "",
    [string]$FromMsi = "",
    [string]$Release = "",
    [switch]$Build
)
$ErrorActionPreference = "Stop"
# $PSScriptRoot is not populated inside param() defaults on PowerShell 5.1
# when run with -File, so resolve paths here instead.
$Here   = if ($PSScriptRoot) { $PSScriptRoot } else { Split-Path -Parent $MyInvocation.MyCommand.Path }
$Repo   = Split-Path -Parent $Here
if (-not $MpRepo) { $MpRepo = Join-Path (Split-Path -Parent $Repo) "tpf2-multiplayer" }
$Vendor = Join-Path $Repo "installer\vendor"
function Say($m, $c = "Cyan") { Write-Host "[vendor] $m" -ForegroundColor $c }
function Fail($m) { Write-Host "[vendor] $m" -ForegroundColor Red; exit 1 }

# PluginHost.wxs as text with CRLF folded to LF: the comparison is about content.
function WxsText([byte[]]$bytes) { ([Text.Encoding]::UTF8.GetString($bytes)) -replace "`r`n", "`n" }

$mineWxs = WxsText ([IO.File]::ReadAllBytes((Join-Path $Repo "installer\PluginHost.wxs")))
$stage = $null
$provenance = @()

try {
    if ($FromMsi) {
        if ($Build)    { Fail "-Build builds a checkout; it does not go with -FromMsi" }
        if (-not $Release) { Fail "-FromMsi needs -Release <tag>: the tpf2-multiplayer tag that MSI was released as" }
        if (-not (Test-Path $FromMsi)) { Fail "no such file: $FromMsi" }
        $FromMsi = (Resolve-Path $FromMsi).Path

        $stage = Join-Path ([IO.Path]::GetTempPath()) ("tpf2bigmap-vendor-" + [guid]::NewGuid().ToString("N").Substring(0, 8))
        New-Item -ItemType Directory -Force $stage | Out-Null
        Say "extracting $FromMsi (administrative image: nothing is installed)"
        $p = Start-Process msiexec.exe -ArgumentList ('/a "{0}" /qn TARGETDIR="{1}"' -f $FromMsi, $stage) -Wait -PassThru
        if ($p.ExitCode -ne 0) { Fail "msiexec /a failed (exit $($p.ExitCode))" }

        $sources = @{}
        foreach ($n in @("alut.dll", "tpf2_pluginhost.dll")) {
            $hits = @(Get-ChildItem $stage -Recurse -File -Filter $n)
            if ($hits.Count -ne 1) { Fail "expected one $n in the MSI, found $($hits.Count)" }
            $sources[$n] = $hits[0].FullName
        }

        # tpf2ca.dll is not an installed file: it is the Binary table row the
        # custom actions run from (PluginHost.wxs, Binary Id="Tpf2CustomActions").
        if (-not ("VendorMsiBinary" -as [type])) {
            Add-Type -TypeDefinition @'
using System; using System.IO; using System.Runtime.InteropServices;
public static class VendorMsiBinary {
    [DllImport("msi.dll", CharSet = CharSet.Unicode)] static extern uint MsiOpenDatabaseW(string path, IntPtr persist, out IntPtr db);
    [DllImport("msi.dll", CharSet = CharSet.Unicode)] static extern uint MsiDatabaseOpenViewW(IntPtr db, string query, out IntPtr view);
    [DllImport("msi.dll")] static extern uint MsiViewExecute(IntPtr view, IntPtr rec);
    [DllImport("msi.dll")] static extern uint MsiViewFetch(IntPtr view, out IntPtr rec);
    [DllImport("msi.dll")] static extern uint MsiRecordReadStream(IntPtr rec, uint field, byte[] buf, ref uint size);
    [DllImport("msi.dll")] static extern uint MsiCloseHandle(IntPtr h);
    // Writes the Binary table row `name` to outPath (the database is opened read-only).
    public static void Extract(string msi, string name, string outPath) {
        IntPtr db, view, rec;
        uint r = MsiOpenDatabaseW(msi, IntPtr.Zero, out db);
        if (r != 0) throw new Exception("MsiOpenDatabase failed: " + r);
        try {
            r = MsiDatabaseOpenViewW(db, "SELECT `Data` FROM `Binary` WHERE `Name`='" + name + "'", out view);
            if (r != 0) throw new Exception("MsiDatabaseOpenView failed: " + r);
            try {
                if ((r = MsiViewExecute(view, IntPtr.Zero)) != 0) throw new Exception("MsiViewExecute failed: " + r);
                if ((r = MsiViewFetch(view, out rec)) != 0) throw new Exception("no Binary row named " + name);
                try {
                    var buf = new byte[65536];
                    using (var f = File.Create(outPath)) {
                        while (true) {
                            uint n = (uint)buf.Length;
                            if ((r = MsiRecordReadStream(rec, 1, buf, ref n)) != 0) throw new Exception("MsiRecordReadStream failed: " + r);
                            if (n == 0) break;
                            f.Write(buf, 0, (int)n);
                        }
                    }
                } finally { MsiCloseHandle(rec); }
            } finally { MsiCloseHandle(view); }
        } finally { MsiCloseHandle(db); }
    }
}
'@
        }
        $sources["tpf2ca.dll"] = Join-Path $stage "tpf2ca.dll"
        [VendorMsiBinary]::Extract($FromMsi, "Tpf2CustomActions", $sources["tpf2ca.dll"])

        # The shared fragment at the released tag must match ours.
        $commit = (git -C $MpRepo rev-parse --verify --quiet "$Release^{commit}")
        if (-not $commit) { Fail "tag $Release is not in $MpRepo -- git -C `"$MpRepo`" fetch --tags" }
        $blob = Join-Path $stage "PluginHost.wxs"
        $g = Start-Process git -ArgumentList @("-C", "`"$MpRepo`"", "cat-file", "blob", "${Release}:installer/PluginHost.wxs") `
                               -RedirectStandardOutput $blob -NoNewWindow -Wait -PassThru
        if ($g.ExitCode -ne 0) { Fail "no installer/PluginHost.wxs at $Release" }
        if ($mineWxs -cne (WxsText ([IO.File]::ReadAllBytes($blob)))) {
            Fail "installer\PluginHost.wxs differs from the $Release copy. Copy theirs over ours; they must match."
        }
        Say "PluginHost.wxs matches the $Release copy"

        $provenance = @(
            "source repo:    https://github.com/silver2127/tpf2-multiplayer",
            "source release: $Release ($((Split-Path -Leaf $FromMsi)), sha256 $((Get-FileHash $FromMsi -Algorithm SHA256).Hash.ToLower()))",
            "source commit:  $($commit.Trim()) (the tag)",
            "extracted:      alut.dll and tpf2_pluginhost.dll from an administrative image, tpf2ca.dll from the Binary table"
        )
    } else {
        if ($Release) { Fail "-Release goes with -FromMsi" }
        if (-not (Test-Path (Join-Path $MpRepo "native\build.bat"))) { Fail "not a tpf2-multiplayer checkout: $MpRepo" }

        if ($Build) {
            foreach ($step in @(@("native\build.bat", "host"), @("installer\ca\build_ca.bat", ""))) {
                Say "running $($step[0]) $($step[1])"
                cmd /c "`"$(Join-Path $MpRepo $step[0])`" $($step[1]) 2>&1" | Where-Object { "$_" -notmatch "vswhere|operable program or batch file" } | ForEach-Object { Write-Host "    $_" }
                if ($LASTEXITCODE -ne 0) { Fail "$($step[0]) failed (exit $LASTEXITCODE). If it was LNK1104, close the game." }
            }
        }

        $sources = @{
            "alut.dll"            = Join-Path $MpRepo "native\out\alut.dll"
            "tpf2_pluginhost.dll" = Join-Path $MpRepo "native\out\tpf2_pluginhost.dll"
            "tpf2ca.dll"          = Join-Path $MpRepo "installer\out\tpf2ca.dll"
        }
        foreach ($k in $sources.Keys) { if (-not (Test-Path $sources[$k])) { Fail "missing $($sources[$k]) -- build it there, or pass -Build" } }

        $theirs = Join-Path $MpRepo "installer\PluginHost.wxs"
        if (-not (Test-Path $theirs)) { Fail "no installer\PluginHost.wxs in $MpRepo" }
        if ($mineWxs -cne (WxsText ([IO.File]::ReadAllBytes($theirs)))) {
            Fail "installer\PluginHost.wxs differs from the tpf2-multiplayer copy. Copy one over the other; they must match."
        }
        Say "PluginHost.wxs matches"

        $commit = (git -C $MpRepo rev-parse HEAD).Trim()
        $short  = (git -C $MpRepo rev-parse --short HEAD).Trim()
        $dirty  = [bool](git -C $MpRepo status --porcelain -- native/src installer/ca)
        $branch = (git -C $MpRepo rev-parse --abbrev-ref HEAD).Trim()
        $provenance = @(
            "source repo:   https://github.com/silver2127/tpf2-multiplayer",
            "source commit: $commit ($short, branch $branch)",
            ("source tree:   " + $(if ($dirty) { "DIRTY in native/src or installer/ca -- the binaries may not match that commit exactly" } else { "clean" }))
        )
    }

    New-Item -ItemType Directory -Force $Vendor | Out-Null
    $lines = @(
        "# Vendored shared binaries",
        "",
        "These three files are built in the tpf2-multiplayer repository and copied here",
        "unchanged. Both packages ship them under the SAME component GUIDs (see",
        "PluginHost.wxs), so they must be the same bytes. Regenerate with",
        "``tools\vendor_host.ps1``; never edit or rebuild them here.",
        ""
    ) + $provenance + @(
        ("vendored on:" + $(if ($FromMsi) { "    " } else { "   " }) + (Get-Date -Format 'yyyy-MM-dd HH:mm')),
        "",
        "| file | bytes | sha256 |",
        "| --- | --- | --- |"
    )
    foreach ($k in ($sources.Keys | Sort-Object)) {
        Copy-Item $sources[$k] (Join-Path $Vendor $k) -Force
        $f = Get-Item (Join-Path $Vendor $k)
        $lines += "| $k | $($f.Length) | $((Get-FileHash $f.FullName -Algorithm SHA256).Hash.ToLower()) |"
        Say "vendored $k ($($f.Length) bytes)"
    }
    Set-Content (Join-Path $Vendor "VENDORED.md") ($lines -join "`n") -Encoding utf8
    Say "wrote vendor\VENDORED.md ($(($provenance | Select-Object -Skip 1 -First 1).Trim()))" "Green"
} finally {
    if ($stage -and (Test-Path $stage)) {
        $resolvedStage = (Resolve-Path -LiteralPath $stage).Path
        $tempRoot = [IO.Path]::GetFullPath([IO.Path]::GetTempPath()).TrimEnd('\') + '\'
        if (-not $resolvedStage.StartsWith($tempRoot, [StringComparison]::OrdinalIgnoreCase) -or
            (Split-Path -Leaf $resolvedStage) -notlike 'tpf2bigmap-vendor-*') {
            throw "Refusing cleanup outside the vendor temporary directory: $resolvedStage"
        }
        Remove-Item -LiteralPath $resolvedStage -Recurse -Force -ErrorAction SilentlyContinue
    }
}
