# Copy-on-write sharing of resident terrain tiles between the two CTerrain versions

Design only, September 15, 2026. Read-only analysis: no game was launched,
attached to or deployed to, and nothing under the Steam directory was modified.
Executable `C:\tools\bin\TransportFever2.exe`, sha256
`782b904a8f7bbdac1f7a18528f1a5c778691e5aa3087c37c351bf6912585175c` (verified in
this session, identical to the Steam 35924 image the plugin targets).
All addresses are RVAs relative to `0x140000000`.

Every number is labelled, as in `runtime-memory-audit.md`:

- **MEASURED**: read out of a running game (plugin log lines quoted below, or
  earlier measurements recorded in `terrain-compression.md` / `load-speed-todo.md`).
- **DERIVED**: computed from decompiled code, from structure sizes, or arithmetic
  on a MEASURED number. Correct if the reading is correct; not itself observed.
- **GUESS**: an inference that neither code nor a measurement pins down.

This answers the open to-do item "Pager: share tiles between the two terrain
versions" (`docs/load-speed-todo.md`). **Nothing here is implemented.**

## 0. Verdict up front

The mechanism is sound and the prize is the largest single item left
(8.25 GiB of load peak at 256x256, 33.0 GiB at 512x512, DERIVED). But the
payoff depends entirely on one ratio that has never been measured: **how many of
the second version's tiles are actually written while both versions exist.**
If the engine writes all of them, page sharing buys a deferral and nothing else.

Recommendation: **build the measurement first** (section 8, stage 1 — roughly a
day of work, no new eviction machinery), then build the full design only if the
measured privatization rate is below ~50%. Do not build stages 2-3 blind.

## 0b. Implementation status (September 15, built and tested offline)

Stages 1 and 2 of section 8 are **built**, behind `terrain_cow_share` (default 0).
Stage 3 (shared eviction, section 4) is not. Not yet measured in game.

Four deliberate deviations from the design above, all simplifications that the
measurement stage does not need:

- **No `Backing` struct.** Sharers form a circular list (`Slot::shared`,
  `Slot::shareNext`) and each sharer owns its own `DuplicateHandle` of the
  section. The section object's own kernel refcount then does the lifetime work,
  so every existing `CloseHandle(s.section)` site stays correct unchanged.
- **Privatization runs entirely under the pool lock** (`Privatize`), copying
  through private `FILE_MAP_ALL_ACCESS` / `FILE_MAP_READ` aliases as section 3b
  requires. A warm 132 KiB `memcpy` under the lock is cheaper than the 184 us
  decode `Restore` already performs there, so there is no `privatizing` flag, no
  unlocked window, and `Release` needs no new spin state.
- **Shared backings are never evicted.** `Evict` and `SoftBlock` refuse while
  `shared`, so section 4's snapshot/cancel/generation machinery is unnecessary.
  Slots become evictable again the moment they privatize or a release collapses
  the ring to one member. During a load this is exactly stage 2's behaviour: the
  loading budget suppresses eviction anyway.
- **`stats.resident` counts unique sections.** Sharing does not increment it,
  privatization does, and `Release` only decrements for the last sharer. Counting
  slots instead would double-count a shared tile and evict far too eagerly
  (section 6, risk 5).

New log counters on the 30 s line: `cow_shared`, `cow_slots`, `cow_privatized`,
`cow_privatize_mb`. **`cow_privatized / cow_shared` is the ratio this stage
exists to measure**: near 0 means the second version is never written and the
full saving is real; near 1 means sharing only defers the copy and this should go
back off.

Tests, all passing: `tools/test_terrain_pager.cpp` gained isolation in both
directions, "sharing costs no section, no encode, no resident backing and exactly
one handle", forced-eviction refusal while shared, privatize-on-write, a
three-member ring surviving releases in order, and 8 threads sharing/writing/
releasing against a forced evictor (2,399 shares, 0 failures, handle count back
to baseline). `tools/test_terrain_cow_share.py` covers the hook wiring through
`CopyTerrainOwned`. The full Python suite (18 files) still passes.

## 0c. MEASURED RESULT (September 15, 256x256 load): the premise is refuted

One load of `New Game6464` with `terrain_cow_share=1`, budgets 3072/4096, on the
94 GiB machine. **Sharing does not work, for two independent reasons, and the
switch has been set back to 0.**

| | measured |
|---|---|
| During the load, at `live=131072` (both versions) | **`cow_shared=0`** |
| Peak working set | **38.96 GB**, against 39.02 GB with sharing off |
| Marker to marker | ~57 s |
| Later, during gameplay | `cow_shared` 19 -> 232 -> 572 -> 710 |
| `cow_privatized` at every one of those samples | **equal to `cow_shared`** (19/19, 232/232, 572/572, 710/710), 91.5 MiB copied |

**1. The load never reaches the copy hook.** `cow_shared=0` while `live` was
exactly 131,072 proves the second version's tiles are not created by the copy
constructor at all. Section 2's "one copy per tile, 65,536 at 256x256" was
DERIVED from `live` reaching 2x, and that inference was wrong. The detach at
`33dd20` only calls `1dedd0` when `block->refs > 1`; during a load each version
builds its own tiles through `CTerrain::AddTile` (`33cb60`) -> resize, where the
block is fresh, so both versions **allocate independently** and there is no copy
to eliminate. Section 9 listed "whether the detach is driven by the first or the
second version" as unknown; the answer is neither.

**2. Where the hook does fire, every sharer is written.** In gameplay the ratio
is 100%: every share privatized, at every sample. That is exactly the "if every
sharer is written" column of section 7 - no saving, and strictly slower than the
eager copy, because each tile now pays a fault, a section create and a 132 KiB
copy instead of one `memcpy`.

The code stays (cfg-gated, default 0, fully tested) because it is the cheap way
to re-measure if the engine's paths ever change, but it must not ship on.

**Where the 8.25 GiB actually is.** Both versions allocate 65,536 tiles each and,
during a load, build them from the same save data - so the tiles are very likely
byte-identical. The lever is therefore **content dedup, not copy-on-write**, and
`terrain_codec.h` already computes a 64-bit content hash for integrity. Cheap
next test, no new machinery: hash both versions' tiles during a load and report
how many collide. Only build dedup if that number is near 65,536.

## 0d. MEASURED (September 17, 256x256 load): the two versions ARE identical, and dedup is built

`terrain_dedup_probe=1` (this commit) hashes every live tile from the pager
worker and counts hash groups. One load of `New Game6464`, probes every 10 s
while loading:

| live | hashed (resident / cold) | distinct | zero tiles | pairs | largest group | note |
|---|---|---|---|---|---|---|
| 24,775 | 24,767 (16,120 / 8,647) | 1 | 24,767 | 0 | 24,767 | first version allocating, nothing filled yet |
| 131,072 | 131,072 (71,376 / 59,696) | 1 | 131,072 | 0 | 131,072 | both versions allocated, all still zero |
| 131,072 | 131,072 (72,386 / 58,686) | 9,965 | 112,678 | 8,430 | 112,678 | filling: the filled tiles already come in pairs |
| **131,072** | **131,072 (43,619 / 87,453)** | **65,432** | **719** | **64,922** | 719 | **both versions full: every filled tile has exactly one twin** |
| 131,072 | 119,042 (17,007 / 102,035) | 65,536 | 0 | 53,506 | 2 | second version being released mid-probe, still pure pairs |
| 65,536 | 65,536 (9,347 / 56,189) | 65,536 | 0 | 0 | 1 | after the load: one version, every tile distinct |

So the lever named at the end of 0c is real: 65,536 of the 131,072 tiles held
during a load are byte-for-byte copies of the other version, and outside the
load window nothing is duplicated (later probes during play: 0-2 pairs).

**Built: `terrain_dedup=1`** (`src/pager_impl.inl`, `Evict`). An eviction
hashes the tile before encoding (the codec's own 64-bit hash plus a second,
independent one) and looks the pair up in an index of stored blobs. A hit
shares the existing blob through the Clone mechanism (one immutable blob,
reference counted, dropped by the first write) and skips the encode; a miss
encodes and registers the new blob. The index is open addressing over blob
pointers with tombstones, rebuilt from the slot table when half full. The
blob leaves the index with its last owner. Both pager tests pass, including
eight writers allocating twins against a forced evictor.

What it saves: the second twin of every pair costs a hash (~60 us) instead of
an encode, and its compressed bytes are kept once. In the run above the pager
encoded 59,696 tiles while both versions existed; with the index, every twin
whose partner was already cold is a hit. What it does NOT save: resident
backing. A twin is evicted when the budget says so, exactly as before; the
8.25 GiB of the second version stays resident on a machine whose loading
allowance keeps it. Freeing it early would mean evicting twins regardless of
budget, which needs a hash per candidate and re-decodes if the engine reads
the released version; not built.

**Measured in the game (September 17, `LONGMAPSAVE`, 103,680 tiles per
version, 207,360 live during the load, 94 GiB machine with the commit charge at
its limit for most of the load):**

| point | evictions | real encodes | reused (blob kept) | dedup hits |
|---|---|---|---|---|
| mid-load, 207,360 live, dedup ON | 165,646 | 13,732 | 0 | 151,928 |
| same map, same point, dedup OFF (16:38 run) | 135,637 | 135,637 | 0 | - |
| load complete, 103,680 live, dedup ON | 498,951 | 104,780 | 130,993 | 263,192 |

Over the whole load only 21% of evictions encoded anything; 53% shared a blob
that already existed. Early in the load most hits are the still-unfilled
zero tiles the loading policy evicts (one blob for all of them); later they are
the second twins. `failures=0`, `dedup_rebuilds=0`. Load time was not timed on
this run (the load ran with `commit_tight=1` flapping the resident target
between 256 MiB and 8 GiB every second, which dominates it; that back-off is
older than this change). Compressed bytes at the end: 1,093 MiB for 45,402 cold
tiles.

## 1. What the engine actually does (DERIVED from the machine code)

The height cache of one tile is a `std::vector<uint16_t>` (66,049 samples,
132,098 bytes) owned by a refcounted control block, not by CTerrain directly.

| RVA | What it is | Evidence |
|---|---|---|
| `33c540` / `33d580` | CTerrain tile grid; cell lookup returns a 0x28-byte cell | `33d580` disassembly (bounds test then `cells + i*0x28`) |
| cell `+0x08` | pointer to the control block: vtable `+0x00`, **strong refs `+0x08`**, weak refs `+0x0c`, the vector at `+0x10` | `33dd20`, `33de30` |
| `33dd20` | **the COW detach.** `if (block && block->refs > 1)` allocate a new 0x28 block (`2bf3a80`), set refs=1/weak=1, copy-construct the vector (`call 1dedd0` at `33dd8c`, returns to `33dd91`), then `lock xadd` the old block's refs down and release. Returns the vector. | full disassembly, this session |
| `33cb60` | `CTerrain::AddTile`: calls `33dd20` at `33cc94`, then resizes to `(1<<highLevels)+1` squared at `33cca5` (returns to `33ccaa`) | `add_tile.c`, byte-verified call in `terrain_compression.h` |
| `33de90` | mutable tile accessor: cell lookup from a tile coord, then `call 33dd20` at `33dec3`. **Reached only indirectly** (no direct E8/E9 in `.text`); it sits in the function table at rva `0x2f94dd0`, next to `33de30` | caller scan, this session |
| `33de30` | the control block's vector destructor (the plugin hooks it) | disassembly; 0 direct callers, table slot `0x2f94d98` |
| `33cd10` | tile publication (`CTerrain` height update). Only caller `aad6c0` (<- `aac810`, TerrainAlignmentSystem). Calls `3c40c0` at `33ce23`, which block-copies through `30a540` into each overlapping tile's cache, then runs the inlined `CalcMinMaxHeight` | `terrain-minmax.md`, caller scan |
| `33d240` | copies one CTerrain's tile grid into another: 4 header ints then the cell vector assign `33c800` | `replicate_terrain_callee.c` |
| `33c4a0` | the cell range copy used by that assign. Per cell it copies the two ints and the two block pointers and does **`lock inc dword ptr [rax + 8]`** | disassembly, this session |
| `241630` | `GameState::Replicate`: type-wise component copy (`22fbe0` + `23e0cc0`), then `call 33d240` at `2416cc`. Callers: `1184d0` `RunGameSimLoop` (**every sim step**) and `1189f0` `StartGameSim` | disassembly; `runtime-memory-audit.md` §1 |

Two consequences, both important:

1. **The engine's own sharing is refcount sharing, not copying.** Replicating a
   game state re-shares all 65,536 tile vectors (`lock inc`). The second version
   costs nothing *in the engine*. It only becomes expensive inside the plugin,
   because the plugin's hook on `1dedd0` turns each *detach* into a fresh 132 KiB
   managed slot.
2. **Steady state is one version; the doubling is load-transient.** MEASURED
   (`out/compression-v2-ready/host-playtest.log`, 64,980-tile map): `live=64980`
   in normal play, `live=129960` = exactly 2x for four consecutive 30 s samples
   spanning each load, then back to `64980`. The same shape appears in the v1 log
   (`out/before-v2-install-20260913/host-v1.log`) and in the 256x256 run quoted in
   `terrain-compression.md` (131,072 = 2 x 65,536 live versions).

## 2. Question 1 — what the copy costs today

### Counts

- **Copies taken, per load: one per tile, i.e. 65,536 at 256x256** (DERIVED from
  `live` reaching exactly 2x the tile count; every tile of the second version is
  a separate managed allocation).
- **Copies avoided by the existing `Clone` path: essentially none.** MEASURED:
  `shared_clones=0` through both loads in the v2 playtest log and `shared_clones=9`
  after a full session. Nine shares out of ~130,000 COW copies is **0.007%**
  (DERIVED). `Clone` only fires when the source is *already compressed*
  (`pager_impl.inl`, `Clone` requires `slots[source].packed`), and during a load
  the budget policy deliberately keeps everything resident, so the source is
  almost never compressed. The `memcpy` branch in `CopyTerrainOwned` takes
  effectively 100% of the traffic.

### Bytes

`SlotBytes` = `(132098 + 32 + 4095) & ~4095` = **135,168 B** (33 pages, 132 KiB
exactly); payload 132,098 B.

| Map | Tiles | One version (slots) | Both versions | Payload only, one version |
|---|---:|---:|---:|---:|
| 256x256 | 65,536 | **8.25 GiB** | 16.50 GiB | 8.06 GiB |
| 512x512 | 262,144 | **33.00 GiB** | 66.00 GiB | 32.25 GiB |
| 114x570 (the logged map) | 64,980 | 8.18 GiB | 16.36 GiB | 7.99 GiB |

All DERIVED (tile count x 135,168). Cross-check, MEASURED on the 64,980-tile map
at `live=129960`: `resident=98627` slots = 12.71 GiB of mapped backing in one
30 s sample, with the rest already encoded — consistent with a 16.36 GiB ceiling
held down by the eviction budget.

These are pagefile-backed **sections**, so they count against the system commit
limit exactly like private bytes (the 512x512 preview already hit `std::bad_alloc`
at a 114.6 GB commit limit — see `terrain_compression.h`). Sharing removes commit,
not merely working set: a second view of the same section charges no new commit.

### CPU

Per copy the plugin does `Allocate()` (NewSlot, `CreateFileMappingW`,
`MapViewOfFile`, `MapViewOfFile3` into the placeholder, `UnmapViewOfFile`, and on
first use a placeholder `VirtualFree` split) and then a 132,098-byte `memcpy`
into pages that have never been touched.

- `memcpy` of a 257x257 tile into never-touched pages: **25.7 us** (MEASURED,
  `terrain-minmax.md` benchmark table; kernel first-touch faults dominate, which
  is why the same copy is 1.13 us warm).
- The four to five section/mapping syscalls: **5-15 us** (GUESS; not benchmarked).
- Total **~31-41 us per tile**, i.e. **2.0-2.7 CPU-seconds per 256x256 load**
  (DERIVED), *plus* a decode of 184 us (MEASURED, format 3) for every source tile
  that happens to be compressed when it is read.
- Worse than the raw CPU: `Allocate()` performs all of that **while holding the
  pool's exclusive SRWLOCK**, against 3-4 eviction threads taking the same lock.

Profile evidence for the aggregate (MEASURED, `load-speed-todo.md`, 20 ms IP
profile of a 256x256 reload): `tpf2_bigmap` was 24.5% of all samples in the
t+140 window and 28.5% at t+160, and **41% / 53% of the loading thread's own
samples**. Over those two 20 s windows that is ~8.2 s and ~10.6 s of that one
thread inside the pager (DERIVED; per-thread shares are of that thread's own
samples). Not all of it is the COW path — restores and evictions share the blame —
but the COW path is the only part that is pure waste.

## 3. Question 2 — the Windows mechanics

The pager already reserves one `MEM_RESERVE_PLACEHOLDER` arena and replaces one
placeholder per slot with a view of a private pagefile-backed section
(`CreateFileMappingW(INVALID_HANDLE_VALUE, ...)`). Sharing needs three new
sequences. All three use only APIs the pager already resolves dynamically
(`VirtualAlloc2`, `MapViewOfFile3`, `UnmapViewOfFile2`; Windows 10 1803+).

### 3a. Sharing at COW time (replaces `Allocate` + `memcpy`)

Under the pool lock, with `s` = source slot, `d` = the new slot from `NewSlot()`:

1. `VirtualProtect(Base(s), SlotBytes, PAGE_READONLY, &old)` — **downgrade** the
   source view. Reducing access on a section view is allowed (the pager already
   drops views to `PAGE_NOACCESS` routinely). Skip if the view is already
   read-only. This step is mandatory: if the source stayed writable the two
   versions would alias.
2. `MapViewOfFile3(backing->section, GetCurrentProcess(), Base(d), 0, SlotBytes,
   MEM_REPLACE_PLACEHOLDER, PAGE_READONLY, nullptr, 0)` — a second view of the
   **same** section at the second version's fixed address.
3. `++backing->refs`, `slots[d].backing = backing`, both slots `readOnly = true`.

Cost: two syscalls, zero bytes, zero commit. No encode, no decode, no copy.

Fallbacks (all must keep today's behaviour): source not managed, source
compressed (use the existing `Clone` blob share), source `blocked`/`evicting`/
`restoring` (either wait as `Release` does, or fall back to `Allocate`+`memcpy`),
`map3` failure (unmap nothing, fall back).

### 3b. Resolving a write fault into a private copy

**Windows cannot upgrade a read-only section view with `VirtualProtect`.** The
existing code already learned this the hard way and documents it in
`pager_impl.inl` (`Restore`, the `writing && s.readOnly` branch: "A read-only
section view cannot be upgraded to writable with VirtualProtect. Remap the SAME
backing section with write access"), and the native tests exercise the remap
transition directly. So the fault path must remap, and when the backing is shared
it must remap onto a *different* section:

In the VEH, faulting slot `i`, `writing == true`:

1. Under the lock: if `backing->refs == 1`, this is today's case — unmap and
   remap the same section `PAGE_READWRITE` (unchanged code). Otherwise set
   `s.privatizing = true`, `++backing->refs` (the faulting thread's own
   reference), release the lock.
2. Outside the lock: `CreateFileMappingW(INVALID_HANDLE_VALUE, ..., SlotBytes)`
   -> new section; `MapViewOfFile(newSection, FILE_MAP_ALL_ACCESS, ...)` -> write
   alias; `MapViewOfFile(backing->section, FILE_MAP_READ, ...)` -> **read alias of
   the shared section**; `memcpy(writeAlias, readAlias, SlotBytes)`; unmap both
   aliases.
   *Copy through a private read alias, never through `Base(i)`.* Reading the
   read-only view would usually work, but an evictor may make it `PAGE_NOACCESS`
   between the check and the copy, and a re-entrant fault inside the handler is
   rejected by the `inFault` guard — i.e. a crash. The alias pattern is exactly
   what `Evict` already uses for its snapshot.
3. Under the lock again: re-check `s.generation` and `s.active`. Then
   `UnmapViewOfFile2(GetCurrentProcess(), Base(i), MEM_PRESERVE_PLACEHOLDER)`;
   `MapViewOfFile3(newSection, ..., Base(i), 0, SlotBytes, MEM_REPLACE_PLACEHOLDER,
   PAGE_READWRITE, nullptr, 0)`; `s.backing = new Backing{newSection, refs=1}`;
   `s.readOnly = false`; `s.privatizing = false`; drop the old backing reference
   (close the section when it reaches 0).
4. `EXCEPTION_CONTINUE_EXECUTION`; the faulting instruction retries against the
   same address, which now holds a private writable copy.

Cost per privatization: one section create, three map/unmap pairs and a warm
132 KiB `memcpy` — about the same as today's eager copy, but paid only for tiles
that are actually written, and paid off the allocation-time critical path.

### 3c. Rejected alternative: `PAGE_WRITECOPY`

Mapping the second view `PAGE_WRITECOPY` would let the kernel do the COW per
4 KiB page, with no user-mode fault work and copies only of the pages actually
written — attractive, because publication often writes a partial region (a block
with six road strips changes 2,130 of 66,049 samples, MEASURED in
`terrain-alignment-speed.md`).

It is rejected because **it breaks the eviction snapshot protocol**: the pager
encodes through a private `FILE_MAP_READ` alias of the section, and that alias
does *not* see a write-copy view's privatised pages. Encoding a `PAGE_WRITECOPY`
view would silently store pre-write data — a lossless cache that loses data.
Making it safe means encoding through the public view instead (downgrade to
`PAGE_READONLY`, encode in place, cancel on a write fault), which also discards
the "was this tile written since restoration?" signal that the blob-reuse
optimisation depends on (`reused=534838` of 960,798 evictions, MEASURED). Keep it
on the shelf: if stage 1 shows that writes are dense but *sparse within a tile*,
this is the variant to revisit, with the snapshot protocol reworked first.

## 4. Question 3 — eviction of a shared slot (the hard part)

### The Backing object

Today `Slot` owns `HANDLE section`. Replace it with a refcounted `Backing`,
mirroring the existing `Packed` (which is already refcounted and already proven
by the 32-clone tests):

```
struct Backing {
    HANDLE   section;
    unsigned refs;        // slots mapping it + transient handler references
    unsigned sharers;     // head of the intrusive sharer list (slot index)
    bool     evicting;    // an unlocked encode of this backing is running
    bool     cancel;      // a fault on ANY sharer arrived during that encode
};
```

`Slot` keeps `generation`, `touched`, `blocked`, `soft`, `readOnly`,
`viewMissing`, `restoring`, gains `privatizing` and `nextSharer`, and swaps
`section` for `Backing* backing`. Backings come from the existing private
`packHeap`.

### Accounting

`stats.resident` must count **unique backings**, not slots, or the budget
double-counts a shared tile and evicts far too eagerly. Add `residentSlots`,
`sharedBackings` and `privatizations` for the log line. The budget test in
`Tick`/`Evict` (`stats.resident*SlotBytes <= budget`) then measures real bytes.

### Evicting a backing with N sharers

1. Under the lock: walk the sharer list. For every sharer whose view is not
   already blocked, `VirtualProtect(Base(k), SlotBytes, PAGE_NOACCESS, &old)`.
   If any protect fails, roll the earlier ones back to `PAGE_READONLY` and bail
   (the existing failure discipline: an intact resident backing is always the
   safe outcome).
2. Still locked: `backing->evicting = true`, `cancel = false`, map the
   `FILE_MAP_READ` alias, record each sharer's `generation`.
3. Unlocked: **one** `Encode` for all N sharers. This is the whole point — a
   shared tile costs one encode, not N.
4. Under the lock: if `backing->cancel`, discard the blob and leave the views as
   the faulting thread left them. Otherwise re-walk the *current* sharer list
   (not the snapshot — see below), allocate one `Packed` with `refs = current
   count`, and for each surviving sharer `UnmapViewOfFile2(..., Base(k),
   MEM_PRESERVE_PLACEHOLDER)` and `slots[k].packed = blob`. Close the section,
   free the Backing.

### Interaction with the existing per-slot generation number

The generation currently answers one question: "is this still the same
allocation?" With sharing there are N answers, and they are not all-or-nothing:

- A sharer released or reused during the encode must simply be **dropped from
  the result**, not invalidate it. Its data cannot have changed — every view was
  `PAGE_NOACCESS` before the encode started, and a released slot's view was
  unmapped, which cannot modify the section.
- Therefore the result is applied by re-walking the live sharer list under the
  lock and comparing each sharer's generation to its snapshot; mismatches are
  skipped. If no sharer survives, the blob is freed and the section closed.
- The backing's own lifetime is guarded by `refs`, not by generations: the
  evicting thread holds a reference across the unlocked encode, so a concurrent
  `Release` of the last sharer cannot close the section underneath it.

### Interaction with `cancel`, `soft` and `restoring`

- **`cancel` moves from the slot to the backing.** A fault on *any* sharer sets
  `backing->cancel` and restores *that* sharer's own view (`VirtualProtect` back
  to `PAGE_READONLY`; the section is still mapped, so this is the cheap path).
  The other sharers stay `PAGE_NOACCESS` and each restores itself on its own next
  access. That is correct but it means `cancelled` can be incremented once per
  sharer; count it once per backing.
- **`soft` (second chance) stays per view.** Soft-blocking is a protection
  change on one address range, and a touch on one version should rescue only that
  version. A backing becomes an eviction candidate when *every* sharer's view is
  blocked and the newest sharer's `touched` is older than `MinAgeMs`; use the
  newest, not the oldest, or one hot version drags a shared tile out of memory
  repeatedly. Add that test to `Tick`'s `ripe`/`excess` computation.
- **`restoring` stays per slot** (each view is restored at its own address), but
  a cold restore now creates a *private* section for that slot only: a compressed
  blob is already shared through `Packed::refs`, so two sharers that both fault
  cold legitimately end up with two sections holding equal bytes. That is a real
  regression of sharing across an eviction cycle; accept it in v1 (the blob is
  still shared, which is the expensive part) and note it as future work.

### The race the to-do list asks about: release versus fault

Three orderings, and how each is resolved:

1. **Release(A) while B faults.** B's handler holds `backing->refs`, so Release
   only decrements; the section survives until B's privatize/restore finishes.
   Release must also unlink A from the sharer list under the lock.
2. **Release(A) while A itself is being privatized by another thread.** `Release`
   already spins on `restoring`; it must spin on `privatizing` too, with the same
   `SwitchToThread` loop, and must not free the slot while a handler holds a
   pointer to it.
3. **Release(last sharer) during a shared encode.** `refs` hits the evictor's own
   reference only; the result is discarded by the "no sharer survives" branch and
   the section is closed there. No leak, no use-after-free.

The general rule that makes this tractable: **a Backing is freed only by its last
reference holder, and every unlocked phase holds one.** That is exactly the
discipline `Packed` already follows, and the existing tests for it
(`RestoreUnlocked` taking `++source->refs`) are the template.

## 5. Question 4 — every site that can write a tile after the copy

Each is a potential write fault under the new scheme. Confirmed sites first.

| Site | RVA | Role | Confidence |
|---|---|---|---|
| Vector resize | `1d5c50` | the tile's initial fill after `AddTile`, and any growth (hooked; growth migrates back to the game allocator) | verified hook |
| `CTerrain::AddTile` | `33cb60` | detaches at `33cc94` then resizes; for a *shared* cell this is a detach that writes nothing afterwards | disassembled |
| The detach itself | `33dd20` | the vector copy ctor `1dedd0` at `33dd8c` is the plugin's hook point (return `33dd91`) | disassembled, byte-verified |
| Mutable tile accessor | `33de90` | cell lookup + detach; the caller then holds a raw `uint16*` and may write anything | disassembled; **callers unresolved** (indirect only, table slot `0x2f94dd0`) |
| Tile publication | `33cd10` -> `3c40c0` (call at `33ce23`) -> block copy `30a540` | the main writer: copies each alignment block's overlap into the tile, then rewrites `minZ`/`maxZ`/`version` in the tile record | disassembled; `terrain-minmax.md` |
| Other publication entries | `3c40c0` from `34cd90`, `9e0790`, `ae8d80` | same block-copy writer reached from three further paths | caller scan; roles not identified |
| Control-block destructor | `33de30` | frees the vector (hooked) | verified hook |

Read-only consumers (they fault, they do not privatize): `33d580` cell lookup
(11 call sites, incl. ViewTerrain's override `34e0c0`, `SwapTerrain 34f291`,
`3c2c10`, `3c3aa0`, `3c5460`, `3c6620`, `aad6c0`, `ae9220`), `33d7c0`
`BaseGetVertices` (`2146540` construction comparison and the `156a24` tail jump),
`UpdateLodTess 334c60`, the refinement `3c4620`/`3ac6c0` (writes a *fresh* vector,
which `30a540` then crops into the cache), the alignment blend `3b3470` (writes
its own result vector), and save serialization.

**Two honest gaps.** (a) The callers of `33de90` are indirect and were not
resolved; anything reaching it writes. (b) Kernel-mode writes into a tile buffer
(e.g. a `ReadFile` straight into the cache during load) would hit a read-only
view and fail with a status code instead of a resumable user-mode fault.
`terrain-compression.md` already flags this class as unverified for cold pages;
making a resident tile read-only *widens* the exposure, because today a resident
tile is always writable. Stage 1 must watch for it.

## 6. Question 5 — what could go wrong, and how the tests would prove it

Ranked by damage.

1. **A missed privatization aliases the two game states.** Writes through one
   version land in the other. This is silent: no crash, no failed assert — wrong
   terrain heights in one state, a diverging save, and in multiplayer a desync.
   This is the single biggest risk in the design.
2. **Write-fault storm.** If the engine writes nearly every tile of the second
   version anyway, every tile pays a fault plus a 132 KiB copy plus a section
   create, which is *more* expensive than today's straight `memcpy`, and the peak
   is unchanged. Stage 1 exists to find this out cheaply.
3. **Fault-handler deadlock or re-entry.** Privatizing must run outside the pool
   lock and must copy through a private alias (section 3b). A re-entrant fault
   inside the handler is rejected by `inFault` and becomes an unhandled AV.
4. **Leaks and handle growth.** A refcount bug leaks sections (handle count
   climbs, commit never returns) or closes one too early (AV at a random later
   read).
5. **Budget mis-accounting.** Counting slots instead of backings makes the pager
   think it is over budget and evict continuously — the camera-stutter failure
   mode already seen once.

### Test plan (`tools/test_terrain_pager.cpp`, the project's standard)

The existing harness already does real mappings, real access violations, real
concurrency and full-buffer comparisons; every item below is an extension of a
pattern already in the file.

- **Isolation, both directions.** Share one resident tile into a second slot;
  write to A, compare the complete 132,098 bytes of B against the unmodified
  reference and vice versa; then evict, restore and compare again.
- **Sharing is free.** Assert `resident` (unique backings) does not change and no
  encode happens when a resident tile is shared; assert handle count is unchanged.
- **One encode for N sharers.** Share 32 ways (the existing 32-clone test shape),
  evict, assert `encodes` grew by exactly 1, all 32 slots carry the same `Packed`
  with `refs == 32`, and every slot still compares byte-exact after restore.
- **Privatization on first write, including after the parent is released** —
  release the source first, then write through a sharer (the existing shared-blob
  test already does this for compressed clones).
- **Cancel during a shared encode.** Fault sharer A while the backing is
  encoding; assert the result is discarded, B is still correct, and `failures`
  stays 0.
- **Release racing a fault.** Loop: share, then release one sharer on one thread
  while another thread reads and writes the other; 3,000 cycles against four
  unlocked encoders, as the existing churn test does. Assert
  `live == 0 && failures == 0` and that handle count returns to baseline.
- **Full race.** Extend the 4,000,000-atomic-increment test so half the tiles are
  shared pairs with disjoint per-thread words, with an evictor re-evicting
  throughout. Lost or duplicated increments are independently observable.
- **Original machine code.** `tools/test_terrain_compression.py` maps
  `TransportFever2.exe` at `0x140000000` in the test process (the pattern
  `test_terrain_refine.py` and `test_terrain_align_fast.py` already use) and runs
  the **stock** `33dd20` and `1dedd0` against a fixture control block, proving the
  hook's replacement is observationally identical for both the shared
  (`refs > 1`) and unshared (`refs == 1`) inputs, including the refcount
  decrement and the released-block path.
- **Installer/guard tests** in the existing style: the feature refuses when the
  APIs, the prologues or the two call instructions do not verify, and a failure
  anywhere leaves the eager-copy path in place.

## 7. Estimated saving

| Map | Today's load peak from terrain slots | With sharing, if no sharer is written | If every sharer is written |
|---|---:|---:|---:|
| 256x256 | 16.50 GiB | 8.25 GiB (**-8.25 GiB**) | 16.50 GiB (no saving; slower) |
| 512x512 | 66.00 GiB | 33.00 GiB (**-33.0 GiB**) | 66.00 GiB |

DERIVED. The true figure sits between the two columns at the measured
privatization rate. For context the same 256x256 load MEASURED 38.2 GB peak
working set on the pre-rework build and 36.5 GB on the loading-budget build
(`terrain-compression.md`, `load-speed-todo.md`), against a ~10.5 GB steady state —
the transient second version is the dominant term in that gap.

CPU saving is additional and unconditional at COW time: **2.0-2.7 CPU-seconds per
256x256 load** (DERIVED, section 2) plus the pool-lock serialisation behind it,
out of the 8-11 seconds per 20 s window the loading thread MEASURED inside the
pager. Privatizations give part of it back, one 132 KiB copy at a time.

## 8. Staging, and what to build first

1. **Measure the privatization rate (build this first).** Keep the eager
   `memcpy`, but after copying, map the *source* read-only and count write faults
   on it during a load; or simpler, add a shadow mode that shares the section,
   privatizes immediately on any write fault, and logs
   `shared / privatized / privatize_bytes`. One new counter set, no eviction
   changes, cfg-gated `terrain_cow_share=0`. The answer to "is this worth it" is a
   single ratio from one 256x256 load.
2. **Sharing plus privatization, sharing disabled for eviction.** A shared
   backing is simply never an eviction candidate until its sharers privatize or
   are released. Correct, simple, and it already captures the whole peak saving
   during a load (evictions during a load are what the loading budget suppresses
   anyway). Ship this if stage 1 is favourable.
3. **Shared eviction** (section 4) — one encode for N sharers, per-view soft
   blocking, backing-based budget accounting. Only worth it if shared tiles
   survive long enough to be evicted, which stage 1 and 2 will show.

Rules inherited from `load-speed-todo.md` and honoured by this design: its own
cfg switch (`terrain_cow_share`, default 0), byte-verified hook sites with a
fallback to the current eager copy on every failure path, an offline test that
executes the game's original machine code and compares complete buffers, and an
in-game load measurement (peak working set, peak commit, load duration) before
the default changes.

## 9. What is uncertain

- **The privatization rate is unknown** and decides everything (section 0).
  `live` reaching exactly 2x proves every tile is *detached*; it does not prove
  every tile is *written*, because `AddTile`'s detach writes nothing after the
  copy and the publication path may touch only part of a tile. The MEASURED write
  counters cannot settle it: `writes=` counts write faults on blocked or
  read-only slots (~80,000 per load on a 64,980-tile map), and today's COW copies
  land in freshly writable slots that never fault.
- **Which engine code reaches `33de90`** is unresolved (indirect calls only).
- **Whether the detach is driven by the first or the second version** was not
  determined. Both give the same byte saving, but if the *written* side is the one
  that is shared, privatization is immediate and the saving collapses.
- The 5-15 us syscall cost per `Allocate` is a GUESS; only the 25.7 us cold
  `memcpy` is MEASURED.
- Kernel-mode writers into a read-only tile view (section 5, gap b) are
  unproven in either direction.
- `PAGE_WRITECOPY` per-page sharing was analysed on paper only, and its
  interaction with the encode alias (section 3c) was not tested.
