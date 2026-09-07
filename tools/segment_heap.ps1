# segment_heap.ps1 -- switch TransportFever2.exe onto the Windows Segment Heap.
#
# WHY
# Profiling a 57x57 km save load showed ONE thread spending ~89% of its working
# time in three unnamed ntdll internals (RVAs 0xcaa9, 0x102da, 0x25f8), worth
# ~451 CPU-seconds of a 780-second load. Those were decompiled to the legacy NT
# heap's free-list walk: allocations larger than _HEAP_LIST_LOOKUP.ArraySize all
# share ONE size-ordered free list, and each request walks it from the smallest
# block until first fit. Cost per allocation therefore grows with the number of
# free blocks, and the number of such allocations grows with world size -- two
# growing factors, which is why the hot site scaled 194x on a load only 12x
# longer.
#
# The Segment Heap (Win10 1803+) uses a different backend with no equivalent
# catch-all list walk, so it should remove the mechanism rather than ease it.
#
# WHY IFEO AND NOT A MANIFEST
# The documented opt-in is an application manifest <heapType>SegmentHeap</heapType>,
# but TransportFever2.exe carries an EMBEDDED manifest (521 bytes, asInvoker, no
# heapType) and an embedded manifest beats an external .manifest file. IFEO sets
# it per-image with no change to the executable, so Steam integrity checks are
# unaffected.
#
# HONEST CAVEAT
# Microsoft does NOT promise the Segment Heap is faster -- it was built to reduce
# memory footprint, and it is slower for some workloads. Our case is unusually
# favourable because a specific NT-heap algorithm was measured as the cost, but
# that is a prediction. Measure it; do not assume it.
#
# SCOPE
# This is HKLM and applies to EVERY process named TransportFever2.exe on this
# machine. It is a single value and -Disable removes it completely.
#
# REQUIRES AN ELEVATED SHELL. This machine has Windows PowerShell 5.1, so
# "powershell", not "pwsh":
#
#   powershell -ExecutionPolicy Bypass -File tools\segment_heap.ps1 -Status
#   powershell -ExecutionPolicy Bypass -File tools\segment_heap.ps1 -Enable
#   powershell -ExecutionPolicy Bypass -File tools\segment_heap.ps1 -Disable
#
# Takes effect on the NEXT process start -- relaunch the game after enabling.
param(
    [switch]$Enable,
    [switch]$Disable,
    [switch]$Status,
    [string]$Exe = 'TransportFever2.exe'
)
$ErrorActionPreference = 'Stop'

$key  = "HKLM:\SOFTWARE\Microsoft\Windows NT\CurrentVersion\Image File Execution Options\$Exe"
$name = 'FrontEndHeapDebugOptions'
$SEGMENT_HEAP = 0x08        # bit 3 = use the segment heap for this image

function Show-Status {
    if (-not (Test-Path $key)) {
        Write-Host "[segheap] $Exe : no IFEO key -- legacy NT heap (default)" -ForegroundColor Gray
        return
    }
    $v = (Get-ItemProperty -Path $key -Name $name -ErrorAction SilentlyContinue).$name
    if ($null -eq $v) {
        Write-Host "[segheap] $Exe : IFEO key exists but no $name -- legacy NT heap" -ForegroundColor Gray
    } elseif ($v -band $SEGMENT_HEAP) {
        Write-Host ("[segheap] $Exe : $name = 0x{0:x} -- SEGMENT HEAP ENABLED" -f $v) -ForegroundColor Green
    } else {
        Write-Host ("[segheap] $Exe : $name = 0x{0:x} -- set, but segment-heap bit NOT on" -f $v) -ForegroundColor Yellow
    }
    $running = @(Get-Process ($Exe -replace '\.exe$','') -ErrorAction SilentlyContinue)
    if ($running.Count) {
        Write-Host "[segheap] NOTE: $($running.Count) instance(s) already running -- they keep the heap" -ForegroundColor Yellow
        Write-Host "[segheap]       they started with. Relaunch for the change to apply." -ForegroundColor Yellow
    }
}

if ($Status -or (-not $Enable -and -not $Disable)) { Show-Status; exit 0 }

$isAdmin = ([Security.Principal.WindowsPrincipal] `
            [Security.Principal.WindowsIdentity]::GetCurrent()
           ).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
if (-not $isAdmin) {
    Write-Host "Writing HKLM needs an ELEVATED shell." -ForegroundColor Yellow
    Write-Host "Open PowerShell as administrator and run:"
    $verb = if ($Enable) { '-Enable' } else { '-Disable' }
    Write-Host "    powershell -ExecutionPolicy Bypass -File `"$PSCommandPath`" $verb" -ForegroundColor Cyan
    exit 1
}

if ($Enable) {
    if (-not (Test-Path $key)) { New-Item -Path $key -Force | Out-Null }
    New-ItemProperty -Path $key -Name $name -PropertyType DWord -Value $SEGMENT_HEAP -Force | Out-Null
    Write-Host "[segheap] enabled for $Exe" -ForegroundColor Green
} elseif ($Disable) {
    if (Test-Path $key) {
        Remove-ItemProperty -Path $key -Name $name -ErrorAction SilentlyContinue
        # Only remove the key itself if WE created it and nothing else lives there;
        # IFEO keys are shared ground (debuggers, mitigations) and blowing one away
        # could take somebody else's setting with it.
        $left = Get-ItemProperty -Path $key -ErrorAction SilentlyContinue
        $props = @($left.PSObject.Properties | Where-Object { $_.Name -notmatch '^PS' })
        if ($props.Count -eq 0 -and -not (Get-ChildItem $key -ErrorAction SilentlyContinue)) {
            Remove-Item -Path $key -Force
            Write-Host "[segheap] removed value and the now-empty IFEO key" -ForegroundColor Green
        } else {
            Write-Host "[segheap] removed $name; left the IFEO key (other values present)" -ForegroundColor Green
        }
    } else {
        Write-Host "[segheap] nothing to remove" -ForegroundColor Gray
    }
}
Write-Host ""
Show-Status
