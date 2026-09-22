# The load-time memory spike: the terrain alignment pass, and batching it

Steam 35924. Everything below was measured on 2026-09-17 on a 94 GiB machine
with **no page file**, so the commit limit equals physical RAM (93.6 GB) and
about 44 GB of it is committed by other programs and the kernel before the
game starts. Maps: `LONGMAPSAVE` and `LONGBOI`, both 207,360 tiles (two
CTerrain versions of 103,680).

## What the game does when a save loads

The save holds the 4 m base heightmap and every alignment (the triangle
mesh with weights that a road, track, station or building cuts into the
ground), not the finished 1 m height cache. On load the engine rebuilds the
cache with the bicubic refine and then re-applies every alignment:
`ecs::TerrainAlignmentSystem`'s update (`0xaac810`) hands the set of terrain
blocks dirtied since the last frame (`this+0xa0`, a `std::set`, one entry per
sub-tile block: its `CVec2i` and a vector of the 16-byte alignment items that
touched it; node constructor `0x6fba90`, 0x40 bytes) to `UpdateSubterrains`
(`0xaad6c0`), which for **every entry** gathers the block's alignments,
extracts a work block (`terrain_util::GetBlock`, `0x3c4a20`) and computes a
result block (`CalculateHeightMod`) per region on the thread pool
(`ThreadPool::LoopImpl` at `0xaadc50`), and only then publishes all results
into the height caches (`0x33cd10` at `0xaadcf5`) and frees them
(`0x3b08f0`). In play the set is a few entries. On a save load it is the whole
map.

## Measured

| | LONGMAPSAVE (ETW trace of the 18:10 crash) | LONGBOI (in-process counters) |
|---|---|---|
| entries in the dirty set | | 1,658,880 (8 per tile) |
| result vectors (resize at `0xaac4d9`) | 37,354 whole-tile | 3,317,760 |
| work blocks (`GetBlock` -> `0x310230`) | 99,014 whole-tile | ~9.9 million |
| live at the peak | 31.6 GiB, 25.8 of it these blocks | private 34.2-35.8 GiB |
| result | "Out of memory" assert | assert, or survived at the edge |

The ETW attribution is `tools/re/etl_alloc.py` in the multiplayer repository
(VirtualAlloc events with stacks, paired with their releases); the
in-process counters are the `block_calls= block_sized= result_resizes=`
fields of the pager log line.

## What did not fix it

- **Tile pager: lazy zero tiles, dedup, throttle, section retry.** Necessary
  (the pager itself was adding up to 19 GiB of sections on top, and its
  restore failure at the commit limit was one crash), but the engine's own
  34 GiB remained.
- **Routing the blocks through the tile pager** (`terrain_blocks`, first
  version): never matched, the blocks are not tile-sized on LONGBOI.
- **A small pager** (`small_pager.h`, page-granular spans, `small_codec.h`):
  caught every block, restored 4.9 million of them correctly, but eviction
  could not keep pace with ~50,000 allocations a second against 24 engine
  threads on one lock; the peak stayed at 35 GiB unless the producers were
  throttled. Kept, off by default (`terrain_blocks=0`).

## The fix: `alignment_batch_tiles` (`src/alignment_batch.h`)

A detour on `UpdateSubterrains` (17-byte steal, byte-verified together with
the caller's call site and the iterator's `isnil` test) walks the game's set
read-only with the same MSVC `_Tree` successor step the function uses,
copies each entry's 32-byte value (coordinate plus the vector's three
pointers, which the callee only reads), and calls the original once per
batch of `alignment_batch_tiles` entries (default 512) with a degenerate tree
of its own (a left chain, head as nil). Compute and publication alternate
per batch, which is the per-frame behaviour the engine already has; the
game's set and its vectors are untouched and freed by the caller's own erase.

Result on LONGBOI (19:30 load):

| | before | with batching |
|---|---|---|
| peak private commit | 34.2-35.8 GiB | **8.3 GiB** |
| alignment pass | one call, 1,658,880 entries | 3,240 calls of 512 |
| throttle waits, restore retries | dozens to hundreds | 0 |
| load | assert, or survived at the edge | finished |

The offline test (`tools/test_alignment_batch.cpp`, `build.bat
-alignbatch-test`) builds a `std::set` of entries with real vectors in the
same MSVC layout, checks the walk against the STL's own iteration, the
batches' concatenation, pass-through for small sets and batch 0, chains of
one and exact multiples, and the byte anchors in the real executable.

## Open

- Load time was not timed against stock on the same map; the pass now
  publishes 3,240 times, each with its own per-call overhead.
- The alignment pass is still necessary work. A sidecar file holding the
  finished 1 m cache (`src/terrain_sidecar.h`, `docs/terrain-sidecar.md`)
  skips its result on load: `src/terrain_serve.h` post-hooks
  `CTerrain::AddTile` (`0x33cb60`, the only place a tile record comes to
  life; it returns void, so the record is found by entity from a rotating
  cursor), decodes the saved cache into the fresh vector while it is still
  private after AddTile's own detach, and marks the tile arena slot `served`;
  the block-copy replacement (`0x30a540`) then skips every copy into a served
  slot, so the refine and the pass still compute but publish nothing over it.
  Counters ride on the pager's 30 s line (`sidecar: add_tile= applied=
  copies_skipped=`). Whether the compute itself can be skipped per served
  tile depends on mapping a dirty-set block to its tile, not yet done; the
  pass's wall time (`alignment pass: ... ms`) says what that would save.
  The sidecar's SaveGame/LoadGame hooks and fingerprint are the other
  session's; until they load a file, `terrain_sidecar=1` is inert.
  (Wired 2026-09-21: `src/terrain_sidecar_io.h`, see `docs/terrain-sidecar.md`.)
