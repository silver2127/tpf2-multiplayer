# Lossless 1 m terrain compression (Steam 35924)

Implemented September 13, 2026. Version 2 is built and installed; native tests,
world loading, terrain inspection and two connected rail builds passed.
The resulting save completed in 13.686 seconds and reloaded successfully with
both rail segments intact. Extended gameplay remains untested.
This is separate from the discontinued 2 m experiment. Source heightmaps,
derived 257x257 samples, physical tile dimensions and octree depth are unchanged.

## Native Linux: ON by default for everyone (the owner's call, 2026-09-22)

The Linux port (`bigmap/linux/`, the userfaultfd pager) shipped
`terrain_cache_compress=0` as an opt-in, because `UFFD_USER_MODE_ONLY` cannot
serve a kernel-origin fault on an evicted tile. Without it the native game holds
every tile raw: the dedicated server (31 GB) was OOM-killed at 29.3 GB loading a
~52,000-tile map that Windows holds at 8-10 GB. The owner: "turn it on for
everyone".

- `bigmap/linux/tpf2_bigmap.cfg`: `terrain_cache_compress=1`, and the code's
  built-in default for a missing key is 1 too.
- It stays a cfg switch (`terrain_cache_compress=0` turns it off) and still falls
  back to stock paths when userfaultfd setup fails.
- The Linux docs (`bigmap/docs/linux/PORT.md`, `INSTALL.md`) say it is on by
  default, name the kernel-fault risk and how to turn it off if a graphics driver
  or mod trips it (SIGBUS in the log).
- Released within 0.7: no version change. Compression changes no simulation
  result, so peers with and without it stay in sync.

## Format 3 and pager capacity (September 15, built and tested offline, NOT deployed)

Three changes, all in `src/terrain_codec.h` and `src/terrain_pager.h`:

- **Codec.** Planar prediction (left + up - upper-left), zigzag residuals and a
  static per-tile rANS coder with four neighbour contexts, plus a 64-bit content
  hash checked after every decode. Heights are stored in 5 cm steps, so 99.3% of
  planar residuals are 0 or +-1; an entropy coder beats LZ4 on nibble planes.
  On the 1,024-tile CHONKYIe sample (`%TEMP%\tpf2-cache-visual\terrain-1m-samples.bin`):

  | | format 2 (LZ4 nibbles) | format 3 |
  |---|---|---|
  | payload ratio | 18.06% | 6.95% |
  | cold commit, projected for 64,980 tiles | 1.577 GiB | 0.558 GiB |
  | encode per tile | 160 us | 264 us |
  | decode per tile | 121 us | 184 us |

  Decode speed was bounded by cache latency, not by the rANS state chain: at
  12-bit precision the 64 KiB decode table missed L1 (~2.3 ns/sample), so
  precision is 10 bits (16 KiB table, ratio cost 0.01 points). Other measured
  steps: two interleaved states, 16-bit renormalisation in a [2^15, 2^31) window
  (the division-free encoder update is exact only below 2^31), packed 32-bit
  decode entries, four context candidates loaded before selection, sentinel
  entries for unused contexts and one bounds check per row.
  `tools/decode_experiments.cpp` and `tools/profile_codec_decode.cpp` hold the
  measurements. Decode is ~60 us slower per restored tile than format 2.
- **Blob storage.** Compressed blobs come from a private Win32 heap instead of
  one `VirtualAlloc` each. At ~9 KiB per blob, the 4 KiB commit round-up and
  64 KiB reservation per allocation were ~20% overhead. The fault path's
  `HeapFree` runs under the pool lock, and only this file uses that heap.
- **Capacity.** `MaxSlots` was 131,072. A 256x256 map already reached exactly
  131,072 live versions while two terrain versions coexisted after entry
  (`tpf2mp_host.log`, September 15), and every allocation past the limit silently
  fell back to an uncompressed stock vector. Any map above ~181x181 tiles, and
  every 320..512-tile preset, overflowed. It is now 2^20 (twice the 524,176-tile
  heightmap cap): 129 GiB of placeholder address space and a 32 MiB demand-zero
  slot table. The eviction sweep visits max(256, allocated/128) slots per tick,
  capped at ~40 ms of work.

Checks: `build.bat -codec-test` then `out\test_terrain_codec.exe <samples>`
(degenerate/extreme tiles, every escape boundary, 2,000 random walks, noise
refusal within the cap, no write past the cap, truncation/trailing-byte
rejection, 5,000 single-byte corruptions with none silently accepted, exact
round trip of all real samples); `build.bat -pager-test` (all existing pager
tests, including 4,000,000 concurrent writes racing eviction, on format 3);
all Python tool tests except the MSI test passed against the rebuilt DLL.
Still needed: an in-game load, construction, save and reload with format 3.

### In-game result and the eviction rework (September 15)

Format 3 plus material paging, loaded 256x256 save (PID 59828): settled at
10.5 GB working set / 13.2 GiB private (the same map was ~18 GB working set on
0.3.1); all counters failures=0, slot_overflows=0. Two problems remained and
drove a rework of the shared pager body (`src/pager_impl.inl`, used by both
pagers):

- **Load peak 38.2 GB working set.** While a save load created both terrain
  versions, one eviction thread encoding under the pool lock left up to 110,378
  tiles (14.2 GiB) resident. Encoding now runs outside the lock on the policy
  worker plus up to 3 helper threads; a fault during an encode cancels it, and a
  per-slot generation number discards results for released or reused slots.
- **Camera stutter.** After load the engine re-read ~2,250 evicted terrain tiles
  per second (faults 755k -> 890k over 60 s), each decoded while holding the
  pool lock, so render threads faulting on different tiles queued behind each
  other. Cold restores now decode outside the lock with per-thread scratch
  (other faults on the same slot wait; Release waits for a restore), and a
  second-chance stage first only protects aged excess slots: a touch within 3 s
  costs one VirtualProtect instead of a decode. Over twice the budget, or
  within 15 s of bulk allocation, slots are encoded directly.

New log counters: `soft_blocked`, `soft_rescues`, `cancelled`. Tests: existing
pager tests plus the second-chance sequence, 16 threads restoring 64 cold tiles
while an evictor re-evicts them, and 3,000 allocate/fill/release cycles racing
four unlocked encoders (1,888 cancellations, 0 failures). Not yet measured in
game: peak, stutter and fault rates with this build.

Runtime check, PID 83640 on September 13: the user reported 16 GB after loading;
read-only process inspection measured 16.30 GiB working set and 18.46 GiB private
commit. Logs confirm 1 m restoration, compression enabled and 64,980 live tile
versions, with failures=0 throughout the inspected interval. Several samples
settled at 1,023.9 MiB resident backing plus about 2,207..2,235 MiB cold commit
(roughly 3.2 GiB combined, versus 7.994 GiB stock payload). Other samples showed
larger active sets and temporarily 129,960 live versions; their cause has not
been correlated with a user operation. Fault/eviction counts rise substantially
during activity. This confirms compression and lower backing use, not an
end-to-end loading-speed result or completed gameplay correctness test.

## Version 2 runtime check (September 13)

Installed DLL SHA256:
`F55A8CAB908BAB48C34D88DC714BB9B904224EE8FDE036BF953FCB87A8BE5282`.
PID 67936 loaded the preserved current 114x570-tile desert world. Settled
terrain backing was 1,023.9 MiB resident plus 1,582.1 MiB encoded commit,
about 2.545 GiB combined, versus about 3.2 GiB for version 1. The codec's
compressed payload was about 1,446 MiB. No pager failures were logged.

The loaded process measured 14.44 GiB working set and 16.71 GiB private commit
before rail construction. Version 1 measured 16.30/18.46 GiB, but that was a
newly generated session rather than this saved reload: the entire difference
must not be attributed to version 2. A load sample reached 32.81 GiB working
set while two terrain versions existed; startup peaks remain substantial.

Terrain looked continuous at several camera heights. Two connected electrified
rail segments (220 m straight, 467 m curve) built west of Augusta without a
crash. Shared compressed clones and write-fault counters advanced normally;
failures stayed zero. The separate `n.sav` output was copied, with metadata and
preview, to `MemoryV2-RailTest-20260913`; the original current world remains
preserved as `MemoryBaseline-20260913` (also `m.sav`). No original user saves
were overwritten.

The test save reloaded in the same process and reached the world by 02:28:50.
Both segments were present, terrain remained continuous, and simulation ran
before being paused. A post-reload sample measured 14.78 GiB working set and
16.87 GiB private commit; active backing was still settling. The inspected
pager counters remained at failures=0 after both loads, construction and save.
Test-save SHA256:
`141658BE3C235984D1964A98F2DA1CEF963BCA96FA5DC4A677E575DC5989F08B`.

The temporary 4 GiB budget was observed during bulk allocation, followed by
the normal 1 GiB budget. This heuristic does not cover every loading stage.
Repeat evictions reused compressed blobs hundreds of thousands of times.
These are mechanism checks, not a measured end-to-end loading speedup.

## Configuration

```
terrain_cache_spacing_m=1
terrain_cache_compress=1
terrain_cache_hot_mb=1024
terrain_cache_warm_mb=4096
```

Restart and load a world to enable. To disable, set `terrain_cache_compress=0`
and restart. Compression is a runtime allocation policy, not a save format.
Source configuration defaults to disabled. The resident target accepts
128..8192 MiB. Compressed storage is additional; recent or incompressible tiles
can temporarily exceed the resident target. The pool can manage 131,072 live
tile versions; excess allocations use the stock heap.

The optional warm target accepts 0..8192 MiB. The latest build keeps it during
instrumented new-world entry and through late loading after bulk allocation
(at least 1,024 slots in a one-second allocation window), only while at least
8 GiB RAM is available. Set warm_mb=0 to disable.

The matching new `tpf2_menu.dll` exports `Tpf2mpLastGameUiTick`, captured by its
existing CGameUI update hook. After the last generation/allocation activity,
the pager requires a fresh gameplay UI frame and at least five seconds of
grace before releasing the loading allowance. Paused gameplay still updates
the UI. Stale ticks from the previous world cannot end a subsequent load's
allowance. A cancelled/stalled load has a 15-minute tail cap. If the menu DLL
does not expose the signal, the fallback is a bounded three-minute tail.
Neither timeout expires while instrumented generation remains active.

This follow-up is built and passes native policy tests, but is not installed
over the currently running process. Its actual loading speed remains unmeasured.
The prior runtime results above are for the earlier 15-second-tail build.

## Ownership and synchronization

`terrain_compression.h` intercepts the uint16 vector resize at `1d5c50` only
for the `CTerrain::AddTile` call returning to `33ccaa`, an empty vector, and
66,049 samples. It intercepts the COW copy constructor at `1dedd0` only for
the `33dd20` call returning to `33dd91`, and the shared control block's vector
destructor at `33de30`. All prologues and both call instructions are verified
before hooks install. Managed allocation starts only after all hooks and the
worker succeed; destruction installs first. Unexpected vector growth migrates
back to the game allocator. Existing allocations are never retrofitted.

`terrain_pager.h` reserves a placeholder arena with a fixed 135,168-byte slot
per tile version (132,098 data bytes, 32-byte aligned-allocation header, page
rounding). Resident data uses a pagefile-backed section. To evict a tile:

1. Lock the pool and map a private read alias of its section.
2. Protect the game-facing view as inaccessible. Concurrent engine accesses
   now fault and wait; completed writes are included in the snapshot.
3. Encode row differences modulo 65536, zigzag signed deltas, pack four nibble
   planes (two samples per byte), compress with
   LZ4 1.10.0, and copy the result into ordinary committed virtual memory.
4. Unmap the public view back to its placeholder, unmap the alias, and close
   the section. This releases backing commitment; it is not a working-set trim.
5. Release the lock. A fault decompresses into a new private alias and maps the
   completed section into the original address before allowing the instruction
   to retry. Both reads and writes resume against unchanged addresses.

Version 2 retains immutable compressed data after read restoration and maps
that resident view read-only. Re-eviction reuses the representation without
encoding again. First write remaps the same section at the same address with
read/write permission, then drops that owner's compressed reference. Windows
cannot upgrade a read-only section view with VirtualProtect alone; the native
tests caught this and exercise the remapping transition directly.

COW copies can share a reference-counted immutable compressed representation
without restoring either tile. Each version receives its own fixed address and
private section on restoration. Parent destruction and writes to one version
do not invalidate other versions. Compressed commitment counts unique blobs.

Compressed bytes use VirtualAlloc/VirtualFree rather than the game heap, so
world teardown releases them directly. Fault handling allocates no STL objects,
calls no engine code, does not log, and restores the interrupted last-error
value. It handles only read/write access violations inside active owned slots;
unrelated exceptions pass through. The private alias avoids snapshot races with
engine readers/writers without suspending game threads. Failed eviction keeps
the intact original section. A genuine restore allocation failure propagates
the access violation instead of inventing terrain data.

The worker scans up to 256 slots per 25 ms tick, evicting above-budget resident
tiles at least five seconds old. Age is allocation/last-restoration time, not
an exact access LRU. Frequently accessed resident tiles are not instrumented.
This may cause unnecessary restores under heavy load; logs expose faults and
evictions so that policy can be tuned. Pool transitions currently serialize.

## Measured tests

`build.bat -pager-test` builds the standalone Windows test executable. With the
1,024-tile CHONKYIe sample (seed 20260913, SHA256 in the earlier JSON benchmark):

- V2 LZ4 payload ratio: 0.180614, projected 1.444 GiB for 64,980 tiles.
- Cold commitment including allocation header/4 KiB rounding: projected 1.578 GiB.
- V1 on the same samples: 2.373 GiB payload, 2.497 GiB rounded commitment.
- Allocation/copy/eviction/fault restoration/verification/destruction of all
  1,024 samples took 578 ms in one V2 run while the game was running.
- Every sample round-tripped exactly. 4,000,000 concurrent atomic writes raced
  actual page protection and section eviction without lost/duplicated writes.
- 4,096 simultaneous tile allocations were compressed/restored/destroyed;
  handle counts returned to baseline and all pool backing counters to zero.
- Write faults, address reuse with zero initialization, incompressible fallback,
  COW isolation, compressed destruction, resize migration, installer failures,
  original Steam hook bytes and instruction boundaries passed.
- Read-only restoration followed immediately by a write, 32 shared clones with
  parent released first, and eight concurrent clone/read/write/destroy workers
  racing eviction passed. Unchanged re-eviction performed no new encode.
- Five codec variants were compared by `build.bat -codec-bench`. Nibble planes
  preserved the old encoder's approximate CPU cost while improving space;
  bit planes saved more but more than doubled encode/decode work and were rejected.

With a 1 GiB resident target, V2 sample extrapolation suggests roughly 2.6 GiB of
cache backing including retained representations versus 8 GiB stock, before
other overhead and transient versions.
This is **not** a measured reduction in the running game. Mapped section bytes
must be counted alongside private commitment; a lower process PrivateUsage
number alone would not prove a physical-RAM reduction.

## First gameplay validation

Load an existing original 1 m save, observe compression logs (30-second cadence),
check terrain across tile boundaries, pan/zoom, preview and build rails/roads,
terraform, undo where supported, save to a separate test name, then reload it.
Check the `failures` counter, load duration, frame stalls and total physical
memory as well as resident/cold backing counters. Existing original saves
should be retained. Kernel APIs accessing a cold user buffer directly do not
necessarily deliver a resumable user-mode fault; known height-cache consumers
use CPU loads/copies, but gameplay and serialization integration must verify
that assumption. Source terrain heightmaps are not managed by this pager.

## Dependencies and API references

Vendored [LZ4 1.10.0](https://github.com/lz4/lz4/tree/v1.10.0/lib), BSD 2-clause
license retained in `src/vendor/lz4/LICENSE`; files downloaded from that tag.
The mapping protocol uses Microsoft's documented
[placeholder allocation](https://learn.microsoft.com/en-us/windows/win32/api/memoryapi/nf-memoryapi-virtualalloc2),
[placeholder replacement](https://learn.microsoft.com/en-us/windows/win32/api/memoryapi/nf-memoryapi-mapviewoffile3),
and [placeholder-preserving unmap](https://learn.microsoft.com/en-us/windows/win32/api/memoryapi/nf-memoryapi-unmapviewoffile2).
These require Windows 10 version 1803 or later; dynamically resolved APIs make
unsupported systems refuse this feature before allocating any game tiles.

### Save-load profile and the loading budget (September 15)

Profile of an in-process reload of the 256x256 save (`tools/re/profile_load.py`
in tpf2-multiplayer, 20 ms RIP sampling, PID 60744): "Loading from file" at
18:34:41, "Initial material index generation" at 18:35:50 (~70 s). During the
load the engine's loading threads spent 62-73% of their samples inside
tpf2_bigmap.dll plus `ZwUnmapViewOfSectionEx`/`ZwProtectVirtualMemory`, next to
`33cd10` (tile publication) and `30a55c`; afterwards `334c60` (render data for
every tile) ran at 80% on one thread. Terrain faults rose by ~290,000 during the
reload (~53 CPU-seconds of decoding at 184 us). The 1 s loading minimum age
kept the peak down but made the loader decode what had just been evicted.

The reload also validated material teardown in game: `releases=65536`,
`migrations=0`, `late_releases=0`, `failures=0`.

Policy change: while loading (generation, bulk allocation or the loading
tail) the resident target covers every live allocation, capped at available
RAM minus 12 GiB and never below the configured warm allowance
(`TerrainBudgetMB`, both pagers). Compression then happens after loading on the
below-normal eviction threads. Tested offline (`test_terrain_compression.py`
budget cases); load time with this policy not yet measured.

## 2026-09-17: headroom sized to the machine (0.5.1)

A 32 GiB machine ran a loaded 207,360-tile save at 19.4 GB in the game with
awful performance. Cause: the flat 12 GiB reserve above (MEASURED on this
94 GiB, no-page-file box) is more than a 32 GiB machine has free once the
save is in, so `TerrainBudgetSteady`'s ceiling fell to the hot budget
(1 GiB), the pager evicted everything else, and the engine faulted the
evicted tiles back in through a decode each; the flat 10 GiB commit-tight
threshold also held on a machine with a system-managed page file, which
throttled the pagers on top.

Changes (`terrain_compression.h`, both pagers):

- `PagerHeadroom(physical)` = RAM/7 clamped to 2..12 GiB replaces the flat
  12 GiB in the loading reserve and the steady ceiling; `CommitTightBytes` =
  RAM/8 clamped to 2..10 GiB replaces the flat 10 GiB.
- The steady ceiling budgets from *room* = free RAM plus the pager's own
  target (what it could own), reserving a quarter of the room or the
  headroom, and hands the terrain pager 3/4 of the rest, the material pager
  1/2. Counting free RAM alone let a full pager starve itself.
- Pressure: free RAM under the headroom shrinks the target by 1/8 per second
  and sets the urgent eviction flag, regardless of the decode feedback.

Offline (`test_terrain_compression.py`): a 32 GiB machine after a load with
12 GiB held and 4 GiB free settles at 9,652 MiB instead of 1,092. Not yet
measured on a 32 GiB machine; the `resident target` log lines carry
`pressure=` and `free=` for that.
