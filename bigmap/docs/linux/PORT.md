> Imported from the native Big Maps checkout `4769cd3` in integration
> [`8c3c02a5`](../../../docs/linux/UPSTREAM_dev_8c3c02a5.md). Live results below
> are historical sibling-port evidence, not observations from this merge run.
> Build this tree with `tools/linux/build_native.sh` at the repository root;
> package with `tools/linux/build_release.sh`. The plugin uses the shared
> `native/src/plugin/tpf2mp_plugin.h`. The standalone build/package commands
> below describe the original sibling repository.

# Linux native port, build 35924

Baseline Windows source: bigmap commit `4f0de6f` (0.4.0).
Linux ELF GNU build-id: `3a0e156390b0e6f1e372051c24802c8493ae454a`.
The host checks this identity; the plugin checks every patch before publishing
any change. `tools/linux/verify_game.py GAME_ELF` independently checks all sites.

## Reverse-engineering evidence

The local Linux function/signature/xref exports were cross-checked against
actual x86-64 disassembly of the Steam ELF. Addresses below are Linux RVAs.

| Site | Evidence and ABI |
| --- | --- |
| `0x11232c0` GetNumTilesNew | Signature in MenuUI.cpp; `edi=size`, `esi=format`, `rdx=AppConfig`. Returns x/y packed in rax. Override at +0x28/+0x2c and 224 clamps at 0x112330b/0x1123323. First 18 bytes have no relative operands. |
| `0x14e05a0` occupancy raster ctor | `rdi=this`, `rsi=Box2`, `xmm0=cellSize`; stores box at +8, cell at +0x18, vector<bool> at +0x20, nx/ny at +0x48/+0x4c. `imul ecx,eax` at 0x14e0644, then sign-extension before allocation. Four direct callers are redirected; no Windows layout assumptions or relocation of its RIP-relative prologue. |
| `0xa84234` octree upper tier | Caller at 0xa84040 checks either tile axis >128; loads 32768 into xmm0 and depth 10 into esi, then calls 0x16a6580. Resize stores symmetric bounds at +8..+0x1c and depth at +0x20. Replace the RIP-relative constant with a private nearby 65536 and depth 11; no shared constant is edited. |
| `0x1149945` size combo call | New Game builds four configured labels or seven experimental labels, then passes their vector as rdi to 0x31443a0. The factory reads/copies strings into its widget; the wrapper passes a borrowed libstdc++ vector view and never changes allocator ownership. |
| `0x1149ae7` ratio formatter call | Hidden result string in rdi, ratio index in esi. Return a constructed 32-byte libstdc++ SSO string. Extend loop bound at 0x1149bf4 and enable gate at 0x1149c01. Other formatter callers are untouched. |

The raster call sites are 0x1066d80, 0x150af14, 0x151e175 and 0x15269cf.
Each retains the original SysV calling convention through a nearby absolute
jump stub. Stub pages are written RW then made RX. mmap uses
MAP_FIXED_NOREPLACE; existing mappings are never displaced.

All patches are preflighted. The size trampoline is installed before any UI
extension; failed patch application attempts rollback. Code pages remain
mapped, so no published branch targets freed memory. With raster disabled,
menu additions cap at 180 tiles; without octree expansion they cap at 256.
The heightmap-coordinate diagonal squared is also kept below INT_MAX: 512x512
would cross that boundary in the stock placement-distance calculation, so a
derived square becomes 510x510. A 512-tile edge remains available on rectangles.

## Build and package

`tools/linux/build.sh` uses Valve's pinned soldier SDK
2.0.20260805.254767 (GCC 8.3, glibc 2.31 baseline). Set TPF2MP_SDK_ROOT if
installed elsewhere. Native exports are limited to Tpf2mpPluginInit.

`tools/linux/package.sh /path/to/tpf2_pluginhost.so` packages the plugin and
shared host. The initial host comes from tpf2-multiplayer Linux commit
`070f96a` / v0.5.6-linux-dev.1. Its ABI header is already vendored in src/;
there is no build-time coupling. BUILDINFO records the host SHA-256. The
installer preserves an existing host and multiplayer launcher.

## Tests and limits

Unit checks cover stock/additional rows, every extended ratio, bounds, malformed
config, float raster sizing, unsupported builds/depths, and patch rollback.
The read-only ELF check covers all twenty guarded sites. Runtime host loading and
byte verification have succeeded in the isolated native multiplayer lab.

The Linux port covers large-map creation through depth 11, sparse density presets,
lossless terrain compression, faster saves and a SIMD terrain min/max scan.
It is not full Windows 0.4.0 feature parity. Material paging, terrain copy sharing,
generation buffer reuse, placement budgets, material-index/refinement/alignment
optimizations still need independent Linux work. Depth 12/13 is ported
(experimental); see "Octree depth 12/13" below.
The configured maximum has not been stress-tested; this is an experimental build.

The isolated native game reached New Game with the shared host alone. The
additional size labels and extended ratio labels render correctly; selecting
32.77 x 32.77 km produced a terrain preview. A 40 x 400-tile world
(10.24 x 102.4 km, beyond the stock 256-tile octree boundary) completed
generation, entered gameplay, advanced the date, and saved successfully.
The saved world reloaded successfully and simulation continued afterward.
The lab retained its multiplayer Lua mod, while
only the native shared plugin host was preloaded; this was not a two-peer
synchronization test.


## Linux sparse density port (dev.2)

The town selection is loaded from `[rbx+0x18]->+0x460` at 0x112e2ef.
Cases 0/1/2 branch to 0.2/0.3/0.4 stores at 0x112e720, 0x112e770 and
0x112e788. The 25-byte default/case-3 tail at 0x112e313 is redirected to
a private RX stub; it stores xmm3 at `[rbp-0x1bc]` and resumes at 0x112e32c.
The stub preserves rax/rdx, keeps case 3 at 0.5, handles indices 4..9 with
0.3 times the six Windows scales, and keeps the unknown-index default at 1.0.
An assembly harness executes the actual emitted stub for every new index.

Industries use the same six scales times the stock Medium multiplier 0.6.
Anchored edits extend all three lists and `industryFreq` in base_mod.lua.
The industry start index is zero-based; the target includes Disabled first,
so runFn reads `industryFreq[start+1]` and `industryFreq[target]` respectively.
Atomic file replacement and an exact-patch comparison protect backups and
subsequent user edits. Unit tests cover repeat application, restoration,
changed anchors, and Steam replacing the game file. The restoration helper
is bundled with the installer and called before uninstalling the plugin.

Live dev.2 checks: all three menus show the six new entries. On the same Small
map and seed, Medium previewed 4 towns/32 industries; Minimal previewed
2 towns/5 industries (the generator's minimum counts apply). A 128x128-tile
Minimal map previewed 6 towns/52 industries and completed world generation.
The generation log confirmed saved industry start/target indices 7/8 and
multipliers 0.06/0.06. Saving and reloading succeeded, retaining those
multipliers and continuing simulation. Stock Medium is 0.6. This test does not measure long-term
industry spawning or multiplayer synchronization.

## Linux memory/performance port (dev.3)

### Terrain cache

The Windows `TerrainCodec` format 3 is reused unchanged. It encodes all 66,049
uint16 samples losslessly; decoding verifies the stored hash. Linux uses a
new `userfaultfd` backend rather than Windows placeholder mappings or exception
handlers. A 135,168-byte page-aligned slot holds one 132,098-byte terrain vector.
The pool reserves 1,048,576 slots virtually; unallocated slots consume no terrain
pages. Slot metadata is separate (about 80 MiB).

Allocation is intercepted only at CTerrain::AddTile's vector append call
`0xcf7696 -> 0xadb7e0` and detached-copy allocation call
`0xcf7783 -> 0x6dbce0`. The shared-vector control block's dispose function at
`0xcf7bb0` owns release (vector begins at control+16). The control block's own
allocator/destructor remains unchanged. Vectors outside the pool retain the
stock allocator/free path. Growth beyond a tile migrates to a stock vector.

A policy thread write-protects candidate tiles, compresses their stable contents,
then discards their original pages. The fault thread restores and verifies the
whole tile, removes protection and wakes blocked users at the original address.
The original implementation used a 1024 MiB soft budget and round-robin
eviction. Dev `ea35eb8a` replaces that policy with recency selection and dynamic
headroom (see below). It is not a true LRU cache: ordinary reads of resident
pages do not refresh their age. Compression can cost CPU and introduce latency;
no frame-rate gain is claimed. Poorly compressible tiles stay resident.

No allocation hooks are enabled if userfaultfd setup fails. The backend uses
`UFFD_USER_MODE_ONLY`, requiring no sysctl changes on the tested system. This
mode cannot service kernel-origin faults: kernel access to a missing terrain
page can produce SIGBUS. See the [Linux userfaultfd documentation](https://docs.kernel.org/admin-guide/mm/userfaultfd.html).
The game paths tested here perform terrain reads/writes in userspace; this is
not proof for every graphics driver, mod or engine path. Compression is on by default since multiplayer 0.7, including with a missing
config key, by the owner's decision on 2026-09-22. If a driver or mod triggers
SIGBUS, set `terrain_cache_compress=0` and restart. Material grids, copy-on-write sharing and multi-worker restores
are not implemented in this backend. Terrain resolution remains 1 m; the
abandoned 2 m Windows cache mode is not ported.

### Save stream and terrain scan

Linux save compression setup is `0xc7b4d0`, called by SaveGame at `0xc7f22c`.
The load of level 3 at `0xc7b524` becomes a local immediate level 1. Buffer
comparison `0xc7b6b1`, allocation `0xc7c3a0`, and size store `0xc7c3aa` change
128 bytes to 64 KiB together. No shared constant is changed and the zstd/save
format stays compatible. Larger compressed files are a possible tradeoff.

The scalar uint16 min/max loop at `0xcf5852..0xcf588c` is replaced by an SSE2
scan. At entry rbx/r14 delimit the samples; at exit eax/edx hold min/max and
rbx is the end. The bridge preserves surrounding live registers and xmm2's
scale. Stock empty-vector handling and float conversion remain intact. XORing
the sign bit permits exact unsigned ordering with SSE2 signed min/max. The
Windows block-copy optimization is not part of this change.

### Validation

- Soldier SDK build passes all four CTest suites: hook/config/assembly bridge,
  density, userfaultfd pager, and shared terrain codec.
- Pager tests verify actual page eviction with mincore, concurrent exact
  restores, writes racing eviction, release/reuse and zero initialization.
  Unsupported kernels skip this test explicitly rather than reporting a pass.
- Native ELF build-id and all twenty guarded sites match build 35924.
- Live test: load the existing 128x128-tile sparse world (6 towns, 52 industries),
  render and simulate, construct two joined tracks, save as `Linux pager rail
  test`, then reload and visually verify the track and terrain. Pager restores
  remained hash-checked throughout. A fresh process then reloaded the same save
  with all three new optimizations disabled, confirming the stock paths still
  read it and retain the constructed tracks. The save log reported 879 ms; this is not a
  before/after save-speed benchmark.
- Separate fresh processes loaded the same original save with dev.2 and the
  dev.3 features. One RSS sample per second; median of seconds 60..120 was
  7.781 GiB vs 4.980 GiB. Sampled peaks were 9.851 GiB vs 9.770 GiB. Neither run
  used swap. This single-run comparison is evidence of settled memory savings,
  not reduced loading peaks or a repeatable performance benchmark. The pager
  later held 16,384 live tiles at roughly 1,024 MiB resident plus 106 MiB packed.
- Testing used the isolated native lab with the multiplayer Lua mod retained,
  but no second peer. Multiplayer synchronization and long-session stability
  remain untested for these additions.

## Memory revisit, 2026-09-22 (partial)

Windows baseline is already merged through `0b249268ab`; this revisit does not
merge history. Sources examined: `alignment_batch.h`, `terrain_compression.h`
(including the 9180629 load-only throttle), `material_compression.h`,
`small_pager.h`, `instance_shrink.h`, and the terrain COW sharing notes.

### Automatic terrain budget (implemented; superseded by ea35eb8a below)

`terrain_cache_hot_mb=0` (now the packaged and built-in default) uses installed
physical MiB / 30, clamped to 256..4096 MiB, exactly the Windows
`AutoTerrainBudgets` hot calculation. Positive explicit settings retain the
128..16384 MiB native limits. Failed RAM discovery uses the 256 MiB floor.
`sysconf(_SC_PHYS_PAGES/_SC_PAGESIZE)` supplies physical memory; this is not a
cgroup limit or an available-memory/headroom controller. Examples: 16 GiB ->
546 MiB; 32 GiB -> 1092 MiB. The live candidate logged 1022 MiB on this lab.

The earlier dedicated-only default proposal is superseded by dev `747fd4d`:
compression now defaults on for every native game, without a rendering-suppression
prerequisite. The kernel-origin fault risk above remains; setup failure still
retains stock allocation paths. No version or simulation change is involved.
The native pager currently never sleeps to throttle faults; no unconditional
throttle was introduced. Windows' dynamic headroom and load-only throttle policy
remain unported and must not be inferred from the new hot-budget calculation.

### Alignment batching (implemented, experimental, default OFF)

The native implementation is opt-in with `alignment_batch_tiles=512`; zero
retains the stock call. This is a prototype pending loaded-world validation,
**not a claim that the load peak is fixed**.

Anchor: the `funcsig.csv` assertion/source mapping identifies the worker at
`0x173cbe0` as `ThreadPool::LoopImpl` for
`TerrainAlignmentSystem::UpdateSubterrains(const std::map<CVec2i,
std::vector<Box2>>&)`. Its caller is `0x173dae0`; it calls `CreateTileBlock`
(`0xda3010`, independently named by its box/grid assertions) at `0x173df44`.
`0x173e3e0` supplies the dirty map at self+0xa0, tests its count at self+0xc8,
calls the update at `0x173e443`, and erases the original map afterwards via
`0x173ecf0`. SysV arguments: RDI = system, RSI = const map. The terrain pointer
is loaded from system+8. Unlike Windows, this is a libstdc++ map, not MSVC's
sentinel tree.

The update loads begin at map+0x18 and compares with the embedded header at
map+8. Nodes have a 32-byte link followed by the 8-byte CVec2i key and three
vector pointers. `lea rax,[r15+0x28]` passes the Box2 vector as the seventh
argument to CreateTileBlock (RDI holds its hidden result pointer). Increment is
the const libstdc++ `_Rb_tree_increment` PLT entry at `0x6dc1c0`. After gathering
88-byte work records, the worker call at `0x173e0a6` completes, publication calls
`0xcf56d0` at `0x173e16f`, and buffers are freed at `0x173e1a9/1b7/1d4`.
Whether publication changes any input used by subsequent batches remains a live
contract; static resemblance to Windows does not settle it.

Seven guards, also included in `linux/sites.json` and checked against the actual
build-id `3a0e156390b0e6f1e372051c24802c8493ae454a` ELF:

| RVA | Verified bytes | Meaning |
|---|---|---|
| 173dae0 | f30f1efa554889e5415741564989fe415541544989f4 | update entry, RDI/RSI saved |
| 173db87 | 4d8b7c2418498d7c2408 | begin and header offsets |
| 173df1f | 498d4728498b76084d8d4620 | node vector, terrain, scale |
| 173e015 | 4c89ffe8a3e1f9fe4989c7 | const iterator increment |
| 173e3ea | 4c8da7a0000000534883bfc800000000 | caller map and count offsets |
| 173e168 | 498b7e084c89eee85c755bff | terrain publication |
| 173e443 | e898f6ffff | redirected update call |

The hook snapshots all keys and borrowed vector triples before processing,
constructs temporary left-chain trees with the native embedded-header layout,
and calls the original once per batch. It never destroys borrowed vectors or
mutates the source tree. Allocation failure or inconsistent traversal falls
back before any publication. All seven guards must pass; otherwise only this
feature stays off. The existing patch transaction handles write rollback.

Tests iterate actual libstdc++ maps using the library's const increment routine,
including empty, single-node, exact and partial batches, batch sizes 0/1/7/512/
65536, source contents and vector-pointer preservation. The host fixture checks
all seven mismatches independently and default-off behavior. These tests do not
substitute for a real terrain output comparison.

### Live attempt and remaining backlog

Evidence is archived in job
`tpf2-multiplayer-dev-revisit-20260922-202412/meta/live/`:
`native-stock-launch*.log`, `native-candidate-launch.log`,
`native-memory*-gdb*.log`, actor data/log snapshots, disassembly, and probe scripts.
The ordinary lab helper failed with `bwrap: setting up uid map: Permission
denied`. A temporary copy used `/usr/bin/bwrap` and omitted the inner soldier
runtime, as in the earlier lab workaround, preserving all filesystem overlays.
No host policy or Steam files were modified.

The candidate was installed into the actor's **data/plugins** directory, which
shadows its root plugins directory, with batching 512, compression on and hot
budget auto. Sparse density was disabled because the lab shares base_mod.lua
read-only. Host logs confirm build match, patch of 173e443, successful pager
setup, 1022 MiB budget and plugin OK. The game then exited 53, reporting that
Steam was not running. Process inspection found neither `steam` nor
`steamwebhelper`; Steam was not started or restarted.

GDB first verified the mapped addresses. Software breakpoints interfered with
byte guards, and a subsequent SIGTRAP-pass trial terminated at a startup trap;
neither is gameplay evidence. The final probe used four hardware breakpoints
and suppressed incidental SIGTRAP delivery. It reached Steam's application-load
error `T:0000067431` and exited with GDB's octal code 0124 (decimal 84), before
any target breakpoint. Every game process exited; no world, GPU selection or
RSS load peak was observed.

| Unfinished item | Static work and actual live attempt | Missing evidence |
|---|---|---|
| Alignment load peak | Native map/call/worker/publication located above; candidate installed; hardware breakpoint at 173dae0 armed, never hit before Steam failure | map ownership, lifetime across worker completion, output equivalence, large-map RSS |
| Dedicated default and loading throttle | Existing Vulkan suppression failure paths reviewed; dedicated=1/render=0 configured; candidate UFFD setup succeeded, world startup failed | successful no-render activation, loading transitions, pressure behavior |
| Material paging | Windows InternCreate/Destroy and vector migration contract reviewed; native byte-grid read accessor d04850 anchored by IBaseGrid assertion; hardware breakpoint armed, never hit | allocation/destruction/assign paths and GPU lifetime, not just reads |
| Terrain COW/dedup | Existing native detached allocation cf7783 and control disposal cf7bb0 reviewed; detached-copy hardware probe armed, never hit | identical live tile pairs, write privatization, full lifetime |
| Small pager | Windows variable blocks contrasted with native CreateTileBlock da3010 and 88-byte gathered records; software breakpoint armed in first GDB attempt, startup trap prevented reaching it | variable allocation/free ownership and useful size distribution |
| Instance shrink | Windows thin/fat relocation constraints reviewed; native a85590 anchored by ModelInstanceList replica-copy assertion; hardware probe armed, never hit | actual publication/move site and embedded-vector ownership; replica copy is not publication |
| Depth 12/13 | Kept gated on preceding work at the time; existing a84234 depth-11 site inspected in live mapped image, first software probe never reached. Since ported offline, see "Octree depth 12/13" | live load, renderer, save/reload, multiplayer |

The supplied pristine save was only `mp_multi_company.sav` (~34 MiB);
NewMPSAVE was absent. No map could be loaded or generated, and no two-instance
multiplayer check could run. The <16 GB / ~52k-tile acceptance is **unmet**.
Native libraries and Lua actor copies were backed up before installation and
restored after archiving. No save was loaded or modified by gameplay.

Validation: soldier build and 63/63 CTests, including the real userfaultfd pager
test; all 27 ELF guard sites; shared Lua release verification; `git diff --check`.

## Pager recency and headroom — dev ea35eb8a (2026-09-22)

Upstream's documentation reports ~790 faults/s on a ~52,000-tile dedicated
server map. This is upstream evidence, not a measurement reproduced here.
The native implementation now:

- Snapshots eligible resident slots, sorts by oldest observed touch, and
  rechecks each slot under its lock before eviction. Scans run every 250 ms
  while above budget, with at most 256 encoding attempts per pass.
- Gives allocations and serviced faults two seconds of grace. Two faults less
  than five seconds apart extend grace to ten seconds. Failed encodes also
  receive grace; reused slots clear their former fault history. Resident reads
  and unprotected writes are invisible to userfaultfd; this is fault recency,
  not access-bit LRU or permanent working-set pinning.
- Recomputes automatic budgets each second from live tile bytes, current
  resident bytes, sysconf physical RAM and `/proc/meminfo` MemAvailable.
  The target is min(live bytes, cap, max(0, resident + available - reserve)).
  Windows PagerHeadroom supplies reserve = physical/7 clamped to 2..12 GiB;
  PagerCapMB supplies cap = physical/4 clamped to 4..8 GiB. Swap is not counted.
  Missing memory information falls back to the old RAM/30 startup budget.
  Positive configuration values retain fixed 128..16384 MiB budgets.
- Logs the current budget and interval faults/s every 30 seconds, dividing
  fault-count changes by actual steady-clock elapsed time.

The target is soft: young/incompressible tiles can exceed it, and it does not
prefault cold tiles. Memory accounting is host-wide, not cgroup-aware. The
Windows load-state controller and full pressure/throttle policy are outside
this integration. No claim is made that the cap fits every map's working set.

No engine address, byte guard, calling convention or ownership contract changed.
The existing append/copy/dispose hooks described above remain guarded. The lab
ELF build-id and all 27 registered patch sites passed `verify_game.py`.
Policy-only changes need no additional engine RE or gdb-derived offset.

The soldier build passed all 63 tests, including actual userfaultfd eviction,
parallel restores, write/evict races and reuse. New deterministic assertions
cover recency grace/rapid-refault/reset and live-size, cap, reserve, pressure
and unavailable-memory budget cases.

Live startup: the standard lab launcher failed UID-map setup. A temporary
copy used system bwrap and omitted the inner soldier wrapper, retaining the
lab filesystem overlays. The candidate logged automatic headroom, a 1022 MiB
startup fallback, successful userfaultfd setup and plugin OK. SteamAPI then
reported no running Steam; no menu, loaded world, GPU selection, 30-second
pager status line or performance comparison was observed. The game's startup
also emitted steam.sh bootstrap messages; no separate Steam command was issued
and Steam was not managed by this job. No game process remained afterwards.
Both actor payload directories were restored and compared equal to backups;
no save was loaded. Evidence: job `tpf2-multiplayer-dev-ea35eb8a3b-20260922-210611`,
`meta/live/` (launch logs, actor logs/data, restoration and build records).
Loaded-big-map throughput and memory behavior still require a working lab
Steam session; implementation completeness is not performance acceptance.

## Octree depth 12/13 (experimental)

Linux counterpart of `bigmap/src/octree_depth12.h` and
[`../octree-depth12.md`](../octree-depth12.md). Configure `octree=1` and
`octree_depth=12` or `13`; the default stays 11. The Linux plugin writes the
same root (+-2^(depth+5) m: 131,072 m at 12, 262,144 m at 13), the same depth
and the same compact node-ID scheme as Windows, so a Linux server can run the
depth Windows players use. Every peer still needs the same depth.
Offline evidence only: no live load, renderer, save/reload or multiplayer run.

### How the Linux sites were found

The ELF is stripped of game symbols, but asserts keep `__PRETTY_FUNCTION__`
strings. Every string mentioning `Octree` was cross-referenced (RIP-relative
displacements) to the functions using it, then matched to the Windows sites by
role, call graph and instruction shape. Function bounds come from `.eh_frame_hdr`.

| Windows (Steam 35924) | Linux RVA | Match evidence | Patched |
| --- | --- | --- | --- |
| `0x2304f8` root/depth site (>128-tile tier) | `0xa84234` | already ported for depth 11: `movss xmm0,[rip+0x3407c48]` (32768.0f); `mov esi,0xa`; `call 0x16a6580` | yes, depth 12/13 value |
| `0xa51b00` `Octree::Resize` | `0x16a6580` | assert `void Octree<Data>::Resize(const Box3&, int) [with Data = ecs::OctreeSystemNode]` / `m_root == nullptr`; stores bounds at +8..+0x1c, depth at +0x20, root at +0x28; no maximum-depth test | no |
| `0xa50c20` find node (starts the descent) | callers `0x16a7e4e`, `0x16a9935`, `0x16a9bff` | each passes `esi=0` (no parent), `edx=0` (ID 0), `r9d=[octree+0x20]` (depth), the root box and `&octree->root` | no |
| `0xa507e0` recursive descent | `0x16aa040` (loop) | the only function allocating `0xa8`-byte nodes that also computes `lea r13d,[r15+r13*8+1]` (child = 8*parent+1+octant); stores the ID at node+8, parent at +0, loose box at +0xc..+0x20, children (0x40-byte array) at +0xa0. GCC inlined the recursion into a loop; stop test `extent > 0.5*width`, as Windows | yes, child step `0x16aa190` |
| `0x853d10` skip query / inlined `CalcOctreeLevel(int)` `0x853d30` | `0x13ec500` / loop `0x13ec528..0x13ec555` | assert `int {anonymous}::CalcOctreeLevel(int)` in `OctreeSkipManager.cpp` (only xref); `test esi,esi; js assert`, then the 32-bit threshold loop; level in `r14d`. A scan of all of `.text` for short backward loops using `shl ...,3`/`*8` with `add` and `cmp` finds this one loop only | yes, `0x13ec530` |
| `0xa503e0` level from box width and depth | `0x16a6620` | identical shape: `[rdi+0x20]-1`, `[rdi+0x14]-[rdi+8]`, `1<<cl` divide loop | no |
| `0x852940` skip-manager level vector | `0x13ee4b0` -> `0x13f09b0` | calls `0x16a6620` at `0x13ef65e`/`0x13ef9d5`, then `0x13f09b0` resizes the 24-byte-element vector to level+1; the query `0x13ec500` compares the level against that vector's size before indexing | no |
| `0xa4ffa0` `DoesNodeChange` (movement) | `0x16a6690` | assert `bool ecs::OctreeSystem::DoesNodeChange(...)`; compares the node's loose box only, never the ID | no |
| `0xa50fb0` remove / `RemoveNodeFromParent` | `0x16a9400` | asserts `RemoveNodeFromParent()` and `ecs::OctreeSystem::Remove(...)`; unlinks by scanning the parent's eight child pointers (+0xa0, 0x40 bytes) | no |
| `0xa51c00` `VisitStaticNodes` | `0x16a6a70` | assert `VisitStaticNodes(ecs::Entity, std::vector<int>&)`; pushes `[node+8]` up the parent chain into `vector<int>`; only callers `0x13ecb90`, `0x13ed960` (skip manager) | no |
| renderer skip use / visit (`0x2c1260`, `0x2bd590`) | `CRenderer::NewUpdate` lambdas (`0xc4e3d0`, `0xc5efe0`, ...) | typed as `unordered_set<int>` / `vector<int>` in their mangled names; 32-bit IDs, no level arithmetic | no |
| heightmap INT_MAX bound (menu) | `Fits`/`Bound`/`Shape` | already present; Linux additionally keeps the heightmap-pixel diagonal squared below INT_MAX (placement distance not widened on Linux) | no change |

The magic numbers 0x49249249/0x09249249 occur in `.text` only inside
division-by-7 and `vector::max_size` constants, not as level thresholds.

### Patched bytes

Child step, `0x16aa190` (20 bytes; reached by fall-through and from
`0x16aa488`/`0x16aa4cb`, no branch lands inside it):

    478d6cef01   lea r13d,[r15+r13*8+1]
    4d63ff       movsxd r15,r15d
    4889c6       mov rsi,rax
    4a8d1cfa     lea rbx,[rdx+r15*8]
    e9e8feffff   jmp 0x16aa08c            ; loop head: rax=[rbx], allocate if null

becomes `jmp [rip+0]; dq stub` plus six NOPs. The stub replays the three stock
moves, then chooses `r13d` exactly as Windows `Depth12Id`: parent ID (read from
`[rax+8]`) below 0x09249249 -> stock `8*r13d+1+octant`; parent in level 10
(<= 0x49249248) -> bank 0, parent in 0x50000000..0x5fffffff -> bank 1, anything
else or `r9d < 1` -> `ud2`; an existing child keeps its stored ID; otherwise
`lock xadd` on the bank counter (starting 0x4fffffff / 0x5fffffff), `ud2` on
reaching 0x60000000 / 0x70000000. It returns to `0x16aa08c` by absolute jump.
Registers: `edi` (the loop's "created" flag), `r8`, `r9`, `r12`, `r14` and all
XMM registers are untouched; it uses `rcx`, `rdx`, `r10`, `r11` and flags, all
dead at the loop head (the next use of each is a write or a call).

Level decoder, `0x13ec530` (37 bytes, after `test esi,esi; js assert`):

    4989d4 41be00000000 b901000000 ba01000000   mov r12,rdx; r14d=0; ecx=1; edx=1
    7410 0f1f00                                 je +16 (flags of the test); nop
    c1e203 4183c601 01d1 39ce 7df3              shl edx,3; inc r14d; add ecx,edx; cmp; jge

becomes `jmp [rip+0]; dq stub` plus NOPs. The stub: `esi >= 0x50000000`
(unsigned) -> `mov r12,rdx; r14d=(esi>>28)+6`; otherwise `test esi,esi` (to
recreate the flags the copied `je` reads) and the 37 stock bytes unchanged,
then jump back to `0x13ec555`. `ecx`/`edx` and flags are dead there.

Root: the existing site gets `patch[9]=depth` and a private float
`2^(depth+5)`; at depth 11 it is exactly the previous 65536/11 patch.

### Installation contract

Invalid `octree_depth` (anything but 11..13) -> `TPF2MP_ERR_FAILED`, nothing
written. Depth 14+ is unsupported, as on Windows. There is no Linux GOG build;
the host's build-id check is the equivalent refusal (`TPF2MP_ERR_BUILD`, no
writes). At depth 12/13 both ID sites are verified before the root; any
mismatch refuses the depth with nothing written (`TPF2MP_ERR_BUILD`). Patches
are published child, level, root: the root/depth change is last. A failed
write rolls back every site, including both ID sites. At depth 11 the ID sites
are neither read nor written, so the published bytes are unchanged.
`octree=0` ignores the depth, as on Windows. `max_tiles` is capped at
512/1024/2048 for depth 11/12/13; the Linux heightmap diagonal bound still
applies, so the Linux menu offers at most about 720-tile edges (2048x64 ->
720x64, square 510x512).

### Validation

- `tools/linux/test_octree_depth_elf.py GAME_ELF` (Unicorn; read-only): checks
  the build-id and all three sites, that each ID site's bytes occur once in
  `.text`, then runs the ORIGINAL Linux descent loop `0x16aa040` with the
  header's site patch and stub (only operator new stubbed) and the original
  decoder `0x13ec528..0x13ec555`. Depth 13 creates 1,620 nodes and depth 12
  1,462 from 158 positions each -- the same counts as the Windows emulation --
  with unique positive IDs, decoded level equal to the parent-chain level,
  128 m tight / 256 m loose leaves, internal level-11 nodes (512 m loose) for
  200 m objects, and reinsertion keeping node identity. The unpatched stock
  overflow is reproduced; levels 0..10 keep their stock IDs; at depths 10 and
  11 the patched loop produces IDs identical to the stock loop. Decoder
  boundaries match stock for levels 0..10 and give 11/12 for both compact banks.
  Mutating the stub's stock branch or the decoder's `+6` makes it fail.
- `test_bigmap` (CTest) runs both stubs natively through the loop's register
  contract (`tests/town_stub.S` harness) against a C++ copy of Windows
  `Depth12Id`, including counter state, existing children, exhaustion and
  refusals (forked child must die with SIGILL), and checks the installer:
  refused depths, mismatches at each site, build refusal, root-last order,
  rollback, the private extents, stub back-links and caps.
- `verify_game.py` covers the two new sites (29 total).

Not validated: live world generation or load, a world wider than +-131,072 m,
renderer culling, save/reload, vehicles crossing the old root boundary, and a
Linux/Windows multiplayer session at depth 12/13. Before relying on it, repeat
the live checklist in `../octree-depth12.md` on Linux.
