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
optimizations and depth-12/13 hooks still need independent Linux work.
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
The soft hot budget defaults to 1024 MiB, with a two-second grace period and a
bounded round-robin scan. It is not an LRU cache: ordinary reads of resident
pages do not refresh their age. Compression can cost CPU and introduce latency;
no frame-rate gain is claimed. Poorly compressible tiles stay resident.

No allocation hooks are enabled if userfaultfd setup fails. The backend uses
`UFFD_USER_MODE_ONLY`, requiring no sysctl changes on the tested system. This
mode cannot service kernel-origin faults: kernel access to a missing terrain
page can produce SIGBUS. See the [Linux userfaultfd documentation](https://docs.kernel.org/admin-guide/mm/userfaultfd.html).
The game paths tested here perform terrain reads/writes in userspace; this is
not proof for every graphics driver, mod or engine path. Compression therefore
remains opt-in. Material grids, copy-on-write sharing and multi-worker restores
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
