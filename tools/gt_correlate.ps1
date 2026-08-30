# Correlate ground-truth sweeps: which byte offsets track the swept input?
#
# WHY THIS EXISTS
# Four field identifications this session rested on a single observation and all
# four were wrong: -0.83147 "was" a rotation matrix (allocator debris in a
# std::string union), +0x04 "was" the track type (2 on one railway, -48 on the
# next), and a2+0x30/0x48 "were" the removal lists (a 3-node road reported 30
# removals). Each looked convincing because the value matched a prediction.
#
# The fix is not more care, it is a different method: sweep a known input and
# report only offsets whose value tracks it across EVERY sample. A field that
# holds the right value once is noise; a field that follows the input through
# eight values is the field.
#
# Usage:
#   pwsh tools\gt_correlate.ps1 [-Log <tpf2_slice.log>]

param(
    [string]$Log = "",
    [ValidateSet("a","b")][string]$Instance = "a"
)
if ($Log -eq "") {
    $real = "C:\Program Files (x86)\Steam\steamapps\workshop\content\1066780\3710243057\recon\m4\out\tpf2_slice.log"
    $ovl  = "C:\Sandbox\$env:USERNAME\GameAgent\drive\C\Program Files (x86)\Steam\steamapps\workshop\content\1066780\3710243057\recon\m4\out\tpf2_slice.log"
    $Log = if ($Instance -eq "b") { $ovl } else { $real }
}

if (-not (Test-Path $Log)) { Write-Error "no log at $Log"; exit 1 }

# [gt] e<test>.<sample>:<240 hex chars>
$rows = @{}
foreach ($line in Get-Content $Log) {
    if ($line -match '^\[gt\] e(\d+)\.(\d+):([0-9a-f]+)$') {
        $t = [int]$matches[1]; $s = [int]$matches[2]; $hex = $matches[3]
        if (-not $rows.ContainsKey($t)) { $rows[$t] = @{} }
        $rows[$t][$s] = $hex
    }
}

# Do NOT exit when there are no edge-format samples: the chunked-record section
# below (construction / vehicle / line sweeps, tag+test.sample+off:hex) is a
# separate format and must still run. Only the edge-record loop is skipped.
if ($rows.Count -eq 0) {
    Write-Host "(no edge-record 'e<t>.<s>:' samples; checking chunked records)"
}

foreach ($t in ($rows.Keys | Sort-Object)) {
    $samples = $rows[$t].Keys | Sort-Object
    Write-Host ""
    Write-Host ("=== test {0}: {1} samples ===" -f $t, $samples.Count)
    if ($samples.Count -lt 2) { Write-Host "  need >=2 samples"; continue }

    $len = ($rows[$t][$samples[0]].Length) / 2
    $bytes = @{}
    foreach ($s in $samples) {
        $h = $rows[$t][$s]
        $b = New-Object byte[] $len
        for ($i = 0; $i -lt $len; $i++) { $b[$i] = [Convert]::ToByte($h.Substring($i*2,2),16) }
        $bytes[$s] = $b
    }

    # int32 at each 4-byte offset, per sample
    for ($o = 0; $o + 4 -le $len; $o += 4) {
        $vals = @()
        foreach ($s in $samples) { $vals += [BitConverter]::ToInt32($bytes[$s], $o) }
        $uniq = $vals | Select-Object -Unique
        if ($uniq.Count -le 1) { continue }        # constant: not the field

        # EXACT tracking: value equals the swept input in every sample.
        $exact = $true
        for ($i = 0; $i -lt $samples.Count; $i++) {
            if ($vals[$i] -ne $samples[$i]) { $exact = $false; break }
        }
        # MONOTONIC: moves with the input without equalling it (an offset/base).
        $mono = $true
        for ($i = 1; $i -lt $samples.Count; $i++) {
            if ($vals[$i] -le $vals[$i-1]) { $mono = $false; break }
        }

        $tag = if ($exact) { "*** EXACT MATCH ***" } elseif ($mono) { "(monotonic)" } else { "(varies)" }
        "  +0x{0:x2} {1,-20} {2}" -f $o, $tag, ($vals -join ' ')
    }
}

Write-Host ""
Write-Host "EXACT MATCH = that offset held the swept value in every sample. Anything"
Write-Host "else is a candidate at best -- do not identify a field on 'varies' alone."

# ---- cross-test diff -------------------------------------------------------
# A parameter held CONSTANT within a sweep cannot show up as "varies" -- catenary
# is off for all of test 1 and on for all of test 2, so it is invisible to the
# per-test scan above. Diff matching sample indices across two tests instead: the
# only offset that differs in EVERY pair is the field that changed between them.
function Compare-Tests($rows, $ta, $tb) {
    if (-not ($rows.ContainsKey($ta) -and $rows.ContainsKey($tb))) { return }
    $shared = $rows[$ta].Keys | Where-Object { $rows[$tb].ContainsKey($_) } | Sort-Object
    if ($shared.Count -lt 2) { return }
    Write-Host ""
    Write-Host ("=== cross-test {0} vs {1}: {2} paired samples ===" -f $ta, $tb, $shared.Count)
    $len = ($rows[$ta][$shared[0]].Length) / 2
    for ($o = 0; $o + 4 -le $len; $o += 4) {
        $diffAll = $true; $pairs = @()
        foreach ($s in $shared) {
            $ha = $rows[$ta][$s]; $hb = $rows[$tb][$s]
            $ba = New-Object byte[] $len; $bb = New-Object byte[] $len
            for ($i = 0; $i -lt $len; $i++) {
                $ba[$i] = [Convert]::ToByte($ha.Substring($i*2,2),16)
                $bb[$i] = [Convert]::ToByte($hb.Substring($i*2,2),16)
            }
            $va = [BitConverter]::ToInt32($ba,$o); $vb = [BitConverter]::ToInt32($bb,$o)
            if ($va -eq $vb) { $diffAll = $false; break }
            $pairs += ("{0}->{1}" -f $va, $vb)
        }
        if ($diffAll) { "  +0x{0:x2} DIFFERS IN EVERY PAIR: {1}" -f $o, ($pairs -join ' ') }
    }
    Write-Host "  (an offset differing in every pair is the parameter that changed)"
}

$rows2 = @{}
foreach ($line in Get-Content $Log) {
    if ($line -match '^\[gt\] e(\d+)\.(\d+):([0-9a-f]+)$') {
        $t = [int]$matches[1]; $s = [int]$matches[2]
        if (-not $rows2.ContainsKey($t)) { $rows2[$t] = @{} }
        $rows2[$t][$s] = $matches[3]
    }
}
Compare-Tests $rows2 1 2

# ---- chunked records (construction sweeps) --------------------------------
# Construction samples arrive as '[gt] <tag><test>.<sample>+<off>:<hex>' in
# 64-byte chunks: tag 'c' is the raw a3 region, 'v1e0_' etc. are the contents
# of each vector found in it. Reassemble by offset, then run the same two scans
# -- per-test exact match, and cross-test pairwise diff -- over every tag.
$chunks = @{}   # tag -> test -> sample -> @{off -> hex}
foreach ($line in Get-Content $Log) {
    if ($line -match '^\[gt\] ([a-z][a-z0-9_]*?)(\d+)\.(\d+)\+([0-9a-f]+):([0-9a-f]+)$') {
        $tag = $matches[1]; $t = [int]$matches[2]; $s = [int]$matches[3]
        $off = [Convert]::ToInt32($matches[4], 16); $hex = $matches[5]
        if (-not $chunks.ContainsKey($tag)) { $chunks[$tag] = @{} }
        if (-not $chunks[$tag].ContainsKey($t)) { $chunks[$tag][$t] = @{} }
        if (-not $chunks[$tag][$t].ContainsKey($s)) { $chunks[$tag][$t][$s] = @{} }
        $chunks[$tag][$t][$s][$off] = $hex
    }
}

function Assemble($parts) {
    $max = 0
    foreach ($o in $parts.Keys) { $e = $o + $parts[$o].Length / 2; if ($e -gt $max) { $max = $e } }
    $b = New-Object byte[] $max
    foreach ($o in $parts.Keys) {
        $h = $parts[$o]
        for ($i = 0; $i -lt $h.Length / 2; $i++) { $b[$o + $i] = [Convert]::ToByte($h.Substring($i*2,2),16) }
    }
    return $b
}

# Decode MSVC std::string objects found at 32-byte stride in a buffer, so a
# param key/value that is a string is reported as text rather than as bytes.
function Strings32($b) {
    $out = @()
    for ($o = 0; $o + 32 -le $b.Length; $o += 32) {
        $sz  = [BitConverter]::ToInt64($b, $o + 16)
        $cap = [BitConverter]::ToInt64($b, $o + 24)
        if ($cap -lt 15 -or $cap -gt 4096 -or $sz -lt 0 -or $sz -gt $cap) { continue }
        if ($cap -eq 15) {
            $s = [Text.Encoding]::ASCII.GetString($b, $o, [Math]::Min($sz, 15))
            if ($s -match '^[\x20-\x7e]*$') { $out += ("+0x{0:x3} `"{1}`"" -f $o, $s) }
        } else {
            $out += ("+0x{0:x3} <heap string len={1}>" -f $o, $sz)
        }
    }
    return $out
}

foreach ($tag in ($chunks.Keys | Sort-Object)) {
    foreach ($t in ($chunks[$tag].Keys | Sort-Object)) {
        $samples = $chunks[$tag][$t].Keys | Sort-Object
        $bufs = @{}
        foreach ($s in $samples) { $bufs[$s] = Assemble $chunks[$tag][$t][$s] }
        Write-Host ""
        Write-Host ("=== [{0}] test {1}: {2} samples, {3} bytes ===" -f $tag, $t, $samples.Count, $bufs[$samples[0]].Length)
        if ($tag -ne 'c') {
            $strs = Strings32 $bufs[$samples[0]]
            if ($strs.Count -gt 0) { Write-Host "  strings (sample $($samples[0])):"; $strs | Select-Object -First 30 | ForEach-Object { "    $_" } }
        }
        if ($samples.Count -lt 2) { continue }
        $len = ($samples | ForEach-Object { $bufs[$_].Length } | Measure-Object -Minimum).Minimum
        for ($o = 0; $o + 4 -le $len; $o += 4) {
            $vals = @(); foreach ($s in $samples) { $vals += [BitConverter]::ToInt32($bufs[$s], $o) }
            if (($vals | Select-Object -Unique).Count -le 1) { continue }
            $exact = $true
            for ($i = 0; $i -lt $samples.Count; $i++) { if ($vals[$i] -ne $samples[$i]) { $exact = $false; break } }
            $fl = @(); foreach ($s in $samples) { $fl += [BitConverter]::ToSingle($bufs[$s], $o) }
            $exactF = $true
            for ($i = 0; $i -lt $samples.Count; $i++) {
                $v = $fl[$i]
                if ([double]::IsNaN($v) -or [double]::IsInfinity($v) -or [Math]::Abs($v - $samples[$i]) -gt 0.01) { $exactF = $false; break }
            }
            $label = if ($exact) { "*** EXACT MATCH (int) ***" } elseif ($exactF) { "*** EXACT MATCH (float) ***" } else { "(varies)" }
            $shown = if ($exactF -and -not $exact) { ($fl | ForEach-Object { "{0:0.##}" -f $_ }) -join ' ' } else { $vals -join ' ' }
            "  +0x{0:x3} {1,-28} {2}" -f $o, $label, $shown
        }
    }
    # cross-test: 1 vs 2 if both present (same convention as the edge sweep)
    if ($chunks[$tag].ContainsKey(1) -and $chunks[$tag].ContainsKey(2)) {
        $rowsT = @{}
        foreach ($t in 1,2) { $rowsT[$t] = @{}; foreach ($s in $chunks[$tag][$t].Keys) {
            $rowsT[$t][$s] = (($(Assemble $chunks[$tag][$t][$s]) | ForEach-Object { $_.ToString('x2') }) -join '') } }
        Write-Host ("--- [{0}] cross-test 1 vs 2 ---" -f $tag)
        Compare-Tests $rowsT 1 2
    }
}

# ---- byte-level flag test ---------------------------------------------------
# "Differs in every pair" is fooled by uninitialised memory that changes on
# every call: +0x50 tripped it with values like 0, 512, 1090775040. A real flag
# is CONSTANT within each test and DIFFERENT between them -- that cannot happen
# by accident across 8 samples. Reported per byte, since catenary turned out to
# be one byte inside a dword whose other three bytes are noise.
function Compare-Flags($rows, $ta, $tb, $label) {
    if (-not ($rows.ContainsKey($ta) -and $rows.ContainsKey($tb))) { return }
    $shared = @($rows[$ta].Keys | Where-Object { $rows[$tb].ContainsKey($_) } | Sort-Object)
    if ($shared.Count -lt 2) { return }
    $len = [Math]::Min($rows[$ta][$shared[0]].Length, $rows[$tb][$shared[0]].Length) / 2
    $A = @{}; $B = @{}
    foreach ($s in $shared) {
        $ba = New-Object byte[] $len; $bb = New-Object byte[] $len
        for ($i = 0; $i -lt $len; $i++) {
            $ba[$i] = [Convert]::ToByte($rows[$ta][$s].Substring($i*2,2),16)
            $bb[$i] = [Convert]::ToByte($rows[$tb][$s].Substring($i*2,2),16)
        }
        $A[$s] = $ba; $B[$s] = $bb
    }
    $hits = 0
    for ($i = 0; $i -lt $len; $i++) {
        $va = $A[$shared[0]][$i]; $vb = $B[$shared[0]][$i]
        if ($va -eq $vb) { continue }
        $ok = $true
        foreach ($s in $shared) { if ($A[$s][$i] -ne $va -or $B[$s][$i] -ne $vb) { $ok = $false; break } }
        if ($ok) {
            if ($hits -eq 0) { Write-Host ""; Write-Host ("=== FLAG scan [{0}] test {1} vs {2}, {3} pairs ===" -f $label, $ta, $tb, $shared.Count) }
            "  byte +0x{0:x3} *** FLAG: {1:x2} in test {2} / {3:x2} in test {4}, every sample ***" -f $i, $va, $ta, $vb, $tb
            $hits++
        }
    }
    if ($hits -gt 0) { Write-Host "  (constant within each test, different between them; 'differs in every pair' alone is NOT evidence)" }
}

Compare-Flags $rows2 1 2 'edge'
foreach ($tag in ($chunks.Keys | Sort-Object)) {
    $rowsT = @{}
    foreach ($t in $chunks[$tag].Keys) {
        $rowsT[$t] = @{}
        foreach ($s in $chunks[$tag][$t].Keys) {
            $rowsT[$t][$s] = (($(Assemble $chunks[$tag][$t][$s]) | ForEach-Object { $_.ToString('x2') }) -join '')
        }
    }
    Compare-Flags $rowsT 1 2 $tag
}
