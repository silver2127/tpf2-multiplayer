<#
.SYNOPSIS
Two-way slice test: A builds a road, B builds a road, both roads execute on BOTH
peers at the same stamp, and the world hashes still agree.

.DESCRIPTION
Step 0  resolve pids from the identity files the bridge wrote (real dir = A,
        Sandboxie overlay = B); refuse to continue unless B's pid is boxed
        (Sbie*.dll mapped) and A's is not.
Step 1  give B its own tpf2_slice.cfg in the overlay (otherwise B reads A's
        real cfg through the sandbox -- one switch would drive both peers).
Step 2  inject the SAME slice DLL A already has into B, unless B has one.
        The DLL's single-instance mutex is only isolated by Sandboxie, and a
        wrong-pid injection returns before the log opens -- so the only proof
        is OVERLAY\tpf2_slice.log appearing with "instance=b".
Step 3  mark both stdouts + both slice logs, then either
          -Auto : append one ROAD line to A's and to B's inject file (no human,
                  exercises the lockstep path only), or
          default: prompt the human to draw one road in A, wait for the
                  capture+cancel in A's slice log, then the same in B.
Step 4  wait for EXEC ROADP ... origin=a AND origin=b on BOTH stdouts; compare
        the op/seq@stamp sets; then wait for at least one SYNC checkpoint after
        the second EXEC and require zero DESYNC.
#>
param(
    [string]$Dll = "",              # default: whatever tpf2_slice*.dll A has loaded
    [switch]$Auto,
    [int]$WaitSeconds = 300,
    [string]$RoadA = "ROAD 1200 -4200 1320 -4200",
    [string]$RoadB = "ROAD 1200 -4600 1320 -4600"
)
$ErrorActionPreference = "Stop"
$Repo    = Split-Path $PSScriptRoot -Parent
$Out     = "C:\Program Files (x86)\Steam\steamapps\workshop\content\1066780\3710243057\recon\m4\out"
$Ovl     = "C:\Sandbox\$env:USERNAME\GameAgent\drive\C" + $Out.Substring(2)
$StdoutA = "C:\Program Files (x86)\Steam\userdata\*\1066780\local\crash_dump\stdout.txt"
$StdoutB = "C:\Sandbox\$env:USERNAME\GameAgent\drive\C\Program Files (x86)\Steam\userdata\*\1066780\local\crash_dump\stdout.txt"
$Injector = Join-Path $Repo "injector\injector.exe"

function Say($m, $c = "Cyan") { Write-Host "[2way] $m" -ForegroundColor $c }
function Fail($m) { Say $m Red; exit 1 }
function Read-Shared($pattern) {
    $f = Get-ChildItem $pattern -EA SilentlyContinue | Select-Object -First 1
    if (-not $f) { return "" }
    $fs = [System.IO.File]::Open($f.FullName, 'Open', 'Read', 'ReadWrite')
    $sr = New-Object System.IO.StreamReader($fs); $t = $sr.ReadToEnd(); $sr.Close(); $fs.Close(); $t
}
function Read-Identity($dir) {
    $f = Join-Path $dir "tpf2_instance.txt"
    if (-not (Test-Path $f)) { return $null }
    $l = @(Get-Content $f)
    if ($l.Count -lt 2 -or $l[1] -notmatch 'pid=(\d+)') { return $null }
    [pscustomobject]@{ Letter = $l[0].Trim(); Pid = [int]$Matches[1] }
}
function Wait-Until([scriptblock]$cond, [string]$what, [int]$secs) {
    $deadline = (Get-Date).AddSeconds($secs)
    while ((Get-Date) -lt $deadline) { if (& $cond) { return $true }; Start-Sleep -Seconds 3 }
    Say "timeout waiting for: $what" Red; return $false
}

# ---- step 0: pids from identity files, cross-checked against Sandboxie -----
$idA = Read-Identity $Out; $idB = Read-Identity $Ovl
if (-not $idA -or $idA.Letter -ne 'a') { Fail "real tpf2_instance.txt is not 'a' (bridge not up in A?)" }
if (-not $idB -or $idB.Letter -ne 'b') { Fail "overlay tpf2_instance.txt is not 'b' (bridge not up in B, or box was cleaned)" }
$pA = Get-Process -Id $idA.Pid -EA SilentlyContinue; $pB = Get-Process -Id $idB.Pid -EA SilentlyContinue
if (-not $pA -or $pA.ProcessName -ne 'TransportFever2') { Fail "A pid $($idA.Pid) is not a running TransportFever2 (stale identity file)" }
if (-not $pB -or $pB.ProcessName -ne 'TransportFever2') { Fail "B pid $($idB.Pid) is not a running TransportFever2 (stale identity file)" }
$boxA = @($pA.Modules | ? { $_.ModuleName -like 'Sbie*.dll' }).Count
$boxB = @($pB.Modules | ? { $_.ModuleName -like 'Sbie*.dll' }).Count
if ($boxA -ne 0) { Fail "A pid $($pA.Id) is sandboxed -- identities are crossed" }
if ($boxB -eq 0) { Fail "B pid $($pB.Id) is NOT sandboxed -- the slice mutex would no-op the second copy" }
$sliceA = @($pA.Modules | ? { $_.ModuleName -like 'tpf2_slice*.dll' } | % ModuleName)
$sliceB = @($pB.Modules | ? { $_.ModuleName -like 'tpf2_slice*.dll' } | % ModuleName)
Say "A pid=$($pA.Id) slice=[$($sliceA -join ',')]   B pid=$($pB.Id) boxed slice=[$($sliceB -join ',')]"
if ($sliceA.Count -eq 0) { Fail "A has no slice DLL loaded -- inject A first (injector.exe $($pA.Id) bridge\out\tpf2_sliceN.dll)" }
if (-not $Dll) { $Dll = Join-Path $Repo ("bridge\out\" + $sliceA[0]) }
if (-not (Test-Path $Dll)) { Fail "DLL not found: $Dll" }

# ---- step 1: B gets its own cfg -------------------------------------------
if (-not (Test-Path (Join-Path $Ovl "tpf2_slice.cfg"))) {
    Copy-Item (Join-Path $Out "tpf2_slice.cfg") (Join-Path $Ovl "tpf2_slice.cfg")
    Say "copied tpf2_slice.cfg into the overlay (B no longer shares A's switches)"
}

# ---- step 2: inject into B ------------------------------------------------
$logB = Join-Path $Ovl "tpf2_slice.log"; $logA = Join-Path $Out "tpf2_slice.log"
if ($sliceB.Count -eq 0) {
    $sizeLogA = (Read-Shared $logA).Length
    Say "injecting $Dll into B pid $($pB.Id)"
    $r = & $Injector $pB.Id $Dll
    Write-Host "  $r"
    if ($LASTEXITCODE -ne 0) { Fail "injector failed" }
    if (-not (Wait-Until { (Test-Path $logB) -and ((Read-Shared $logB) -match 'instance=') } "OVERLAY tpf2_slice.log" 20)) {
        Fail "no overlay slice log appeared: hit the mutex (wrong pid) or _fsopen failed"
    }
    $head = (Read-Shared $logB) -split "`n" | Select-Object -First 4
    $head | % { Write-Host "  B: $_" }
    if ($head[0] -notmatch 'instance=b') { Fail "B's DLL did not read instance=b (overlay identity missing -> fell through to host)" }
    if (($head -join "`n") -match 'HOOK FAILED') { Fail "hook install failed in B" }
    if ((Read-Shared $logA).Length -ne $sizeLogA) { Say "WARNING: A's slice log grew during B's injection -- check A was not re-injected" Yellow }
} else { Say "B already has $($sliceB[0]); not injecting" }

# ---- step 3: mark and drive -----------------------------------------------
$mA = (Read-Shared $StdoutA).Length; $mB = (Read-Shared $StdoutB).Length
$sA = (Read-Shared $logA).Length;    $sB = (Read-Shared $logB).Length
$injA = Join-Path $Out "lockstep_inject_a.txt"; $injB = Join-Path $Ovl "lockstep_inject_b.txt"
$iA = (Read-Shared $injA).Length; $iB = (Read-Shared $injB).Length

if ($Auto) {
    Add-Content $injA -Value $RoadA -Encoding ASCII; Say "A <- $RoadA"
    Add-Content $injB -Value $RoadB -Encoding ASCII; Say "B <- $RoadB"
} else {
    Say "Draw ONE road in instance A now (host window)." Yellow
    if (-not (Wait-Until { (Read-Shared $logA).Substring($sA) -match 'CANCEL local build' } "A capture+cancel" $WaitSeconds)) { Fail "A never captured a road" }
    if (-not (Wait-Until { (Read-Shared $injA).Length -gt $iA } "A inject line" 20)) { Fail "A cancelled but wrote no ROADE line" }
    Say "A captured: $(((Read-Shared $injA).Substring($iA) -split "`n")[0].Substring(0,[Math]::Min(60,((Read-Shared $injA).Substring($iA) -split "`n")[0].Length)))" Green
    Say "Now draw ONE road in instance B (sandboxed window)." Yellow
    if (-not (Wait-Until { (Read-Shared $logB).Substring($sB) -match 'CANCEL local build' } "B capture+cancel" $WaitSeconds)) { Fail "B never captured a road" }
    if (-not (Wait-Until { (Read-Shared $injB).Length -gt $iB } "B inject line" 20)) { Fail "B cancelled but wrote no ROADE line into the OVERLAY inject_b" }
    Say "B captured" Green
}

# ---- step 4: both EXEC on both, same stamps, then a SYNC after the last ----
$rx = 'EXEC (ROADP|ROAD) seq=(\d+) origin=(\w+) at=(\d+)'
function NewA { (Read-Shared $StdoutA).Substring([Math]::Min($mA, (Read-Shared $StdoutA).Length)) }
function NewB { (Read-Shared $StdoutB).Substring([Math]::Min($mB, (Read-Shared $StdoutB).Length)) }
function ExecSet($txt) { $m = @{}; foreach ($x in [regex]::Matches($txt, $rx)) { $m["{0}/{1}/{2}@{3}" -f $x.Groups[1].Value,$x.Groups[2].Value,$x.Groups[3].Value,$x.Groups[4].Value] = $true }; $m }
$ok = Wait-Until {
    $a = NewA; $b = NewB
    ($a -match 'origin=a at=') -and ($a -match 'origin=b at=') -and ($b -match 'origin=a at=') -and ($b -match 'origin=b at=') -and
    ([regex]::Matches($a,$rx).Count -ge 2) -and ([regex]::Matches($b,$rx).Count -ge 2)
} "EXEC of both roads on both peers" $WaitSeconds
# a SYNC checkpoint that lands AFTER the last EXEC is the only hash that covers both roads
$null = Wait-Until { $a = NewA; $i = $a.LastIndexOf('EXEC ROAD'); $i -ge 0 -and $a.Substring($i) -match '\] SYNC |DESYNC' } "hash checkpoint after last EXEC" 240

$na = NewA; $nb = NewB
foreach ($t in @(@("A",$na),@("B",$nb))) {
    Write-Host "==== instance $($t[0]) ====" -ForegroundColor Yellow
    foreach ($m in [regex]::Matches($t[1], '\[ls-\w\] (SCHED|RECV|EXEC|SYNC|!! DESYNC)[^\r\n]*')) { Write-Host "  $($m.Value)" }
}
$setA = ExecSet $na; $setB = ExecSet $nb
$onlyA = @($setA.Keys | ? { -not $setB.ContainsKey($_) }); $onlyB = @($setB.Keys | ? { -not $setA.ContainsKey($_) })
$both = @($setA.Keys | ? { $setB.ContainsKey($_) })
$fail = -not $ok
Write-Host "`n================ TWO-WAY VERDICT ================" -ForegroundColor Cyan
$both | Sort-Object | % { Write-Host "    both: $_" -ForegroundColor Green }
$onlyA | % { Write-Host "    A ONLY: $_" -ForegroundColor Red }; $onlyB | % { Write-Host "    B ONLY: $_" -ForegroundColor Red }
if ($onlyA.Count -or $onlyB.Count) { Write-Host "  FAIL: peers disagree on which/when" -ForegroundColor Red; $fail = $true }
if (-not (($both -join ' ') -match '/a@') -or -not (($both -join ' ') -match '/b@')) { Write-Host "  FAIL: need one road from EACH origin executed on both" -ForegroundColor Red; $fail = $true }
$badA = ([regex]::Matches($na,'EXEC \w+ [^\r\n]*(success=false|ok=false)')).Count; $badB = ([regex]::Matches($nb,'EXEC \w+ [^\r\n]*(success=false|ok=false)')).Count
if ($badA -ne $badB) { Write-Host "  FAIL: game refused on one side only (A=$badA B=$badB)" -ForegroundColor Red; $fail = $true }
$dsA = ([regex]::Matches($na,'DESYNC')).Count; $dsB = ([regex]::Matches($nb,'DESYNC')).Count
$syA = ([regex]::Matches($na,'\] SYNC ')).Count; $syB = ([regex]::Matches($nb,'\] SYNC ')).Count
Write-Host "  hash checkpoints agreeing: A=$syA B=$syB   desyncs: A=$dsA B=$dsB"
if ($dsA -or $dsB) { Write-Host "  FAIL: worlds diverged" -ForegroundColor Red; $fail = $true }
elseif ($syA -eq 0 -and $syB -eq 0) { Write-Host "  NOTE: no hash checkpoint yet -- not proven in sync" -ForegroundColor DarkYellow; $fail = $true }
Write-Host ("  RESULT: {0}" -f $(if ($fail) {"FAIL"} else {"PASS"})) -ForegroundColor $(if ($fail) {"Red"} else {"Green"})
exit $(if ($fail) {1} else {0})
