# Experimental actual octree depths 12 and 13

Implemented for Steam build 35924, tested offline on 2026-09-12. **Not yet
validated in a running game.** This increases actual depth, keeping 128 m tight
leaf cells; it does not substitute coarser leaves. Depth 12 uses a root of
±131,072 m (1,024-tile / 262.144 km edges). Depth 13 uses ±262,144 m
(2,048-tile / 524.288 km edges).

## Configuration and build

Run `build.bat` to produce `out/tpf2_bigmap.dll`. The build does not deploy.
In the existing `[tpf2_bigmap]` configuration section, set `octree=1`,
`octree_depth=13`, `max_tiles=2048`, and `street_raster=1`. Change a size cell
to the desired dimensions, for example `size7_format0=1280x32` with
`size_label7=Depth13 test`. Replace existing assignments rather than duplicating
keys. This example crosses the old root boundary with a relatively narrow map.
The shipped configuration retains `octree_depth=11` and `max_tiles=512`.

Depth 12/13 is installed even when the configured menu sizes are small, because
loading a world does not pass through the menu sizing hook. Keep the setting
and plugin installed when reopening larger worlds; all multiplayer peers need
matching patches and settings. No game installation or live configuration was
changed during this implementation.

The heightmap remains subject to `(tilesX*64+1)*(tilesY*64+1) <= INT_MAX`.
Explicit cells are reduced along their longer axis until they fit; derived
ratios retain their exact ratio while shrinking. Thus 1,024-tile *edges* are
possible, as are 2,048-tile edges, but a 1,024-by-1,024 square is not.
For example, 2,048 by 254 passes the pixel-count bound; 2,048 by 256
is reduced to 2,046 by 256. Passing this bound does not guarantee enough RAM.
Memory, terrain rendering and
other engine limits can still prevent an otherwise in-range map from working.

## What the binary showed

The inspected Steam executable has SHA-256
`782b904a8f7bbdac1f7a18528f1a5c778691e5aa3087c37c351bf6912585175c`.
Addresses below are RVAs relative to `0x140000000`.

* `0xa51b00`, `Octree::Resize`: independently stores root bounds and depth;
  no maximum-depth test. Requires an empty root before resizing.
* `0xa507e0`, recursive descent: allocates 0xa8-byte nodes, stores a 32-bit
  ID at node+8, and stops when remaining depth is 1. Child ID is
  `8*parentID+1+octant`. The root counts as one depth level.
* `0xa50c20`: starts descent with null parent, ID 0 and the configured depth.
  Its call at `0xa50d1e` and the recursive call at `0xa50be0` establish the
  nine-argument Windows x64 ABI used by the hook.
* `0x853d10`, renderer skip query: the inlined `CalcOctreeLevel(int)` at
  `0x853d30..0x853d57` rejects negative IDs and calculates breadth-first level
  thresholds with 32-bit arithmetic. The exact 39-byte loop occurs once in
  the inspected binary. This is not a proof that all differently compiled
  consumers have been found.
* `0x852940`: the skip manager grows its vector at +0x108 using the level
  returned by `0xa503e0`. That function uses actual box width and configured
  depth. The vector is not fixed to ten levels in this path.
* `0xa4ffa0`, movement: uses node bounds to determine whether a node changes.
* `0xa50fb0`, removal: scans the parent's eight child pointers to unlink the
  node; it does not derive the child slot from the numeric ID.
* `0xa51c00`, `VisitStaticNodes`: publishes 32-bit IDs in `vector<int>`.
  The renderer and skip manager also use 32-bit hash keys. Widening the ID
  field in place would overwrite the bounding box beginning at node+0xc.

Existing RE came from `tpf2-multiplayer/tools/re`, its exported call graph,
`C:/tools/ghidra_out`, and earlier scratchpad octree decompilations. Further
read-only Ghidra exports are in `%TEMP%/tpf2-octree-audit`. The target list
beside this document allows repeating the extraction with the multiplayer
repository's `DecompileTargets.java` script and existing `TpF2` project.

## Why this patch can go two levels deeper

With the original scheme, level 10 ends at ID `0x49249248` (1,227,133,512).
Depth 11 is therefore representable. Level 11 would end at 9,817,068,104:
depth 12 cannot work by changing the depth immediate or making IDs unsigned.

Levels 0 through 10 retain their original IDs. Newly allocated level-11
nodes use `[0x50000000, 0x5fffffff]`; level-12 nodes use
`[0x60000000, 0x6fffffff]`. The two ranges are positive and disjoint from
one another and from stock IDs. The renderer decodes the high nibble plus six
as the level. Level-11 nodes may have children in depth-13 trees.

Insertion determines the new node's level from the parent's ID stored in
memory. It replaces the overflowed incoming child-ID argument on every
recursive call below level 10, before the original code can store or propagate
it. Existing nodes keep their IDs when revisited. This also covers objects
large enough to stop in a level-11 internal node before reaching level 12.

Each level has an independent atomic, monotonically increasing counter with
268,435,456 available IDs. IDs are never recycled or reset within the process;
exhaustion fails fast before crossing into another range. This avoids stale
renderer references aliasing newly allocated nodes. It is an allocation-count
limit, not a claim that those nodes fit in RAM. The rest of the engine's
allocation, subdivision, entity lists and child pointers remain unchanged.

The insertion prologue, decoder loop and root site are all byte-verified
before installation. The root/depth patch is written last. If an earlier step
fails, the insertion hook remains a stock pass-through and the larger-map menu
is refused. A decoder installed before a root-write failure still accepts all
stock IDs. The executable on disk is never patched by this code.

## Validation and limits

`python tools/test_octree_depth12.py` requires `pefile` and `unicorn` plus a
built DLL. It checks:

* Actual compiled detour ABI, including all five stack arguments.
* Actual installer with a mock host: every mismatch and installation failure,
  GOG/invalid-depth rejection, expected bytes for both depths, and root-last ordering.
* Generated machine-code decoder against all old level boundaries and compact
  ID range boundaries.
* Original executable descent instructions in Unicorn, with only allocator
  and empty child-array construction stubbed, and the compiled ID allocator
  applied at entry. Depth 13 creates 1,620 nodes and depth 12 creates 1,462
  from 158 positions each, with unique positive IDs, correct parent-chain
  levels and 128 m tight / 256 m loose leaf boxes. Internal-node reinsertion
  and descent from existing compact-ID parents are exercised at depth 13.
* The unpatched depth-12/13 overflow, reinsertion preserving leaf identity,
  depth-10/11 compatibility, explicit size caps and pixel-count boundary cases,
  and 5,120 generated ratio shapes within INT_MAX.

`python tools/test_newgame_menu.py` also passes. No live game, full renderer,
save/reload or multiplayer validation has been performed. Emulator allocation
stubs do not validate allocator behavior or concurrency in the running engine.

Before relying on the patch, generate a fresh narrow map exceeding 1,024 tiles for depth 13,
build and remove roads and constructions beyond ±131,072 m and in all four
corners, move vehicles across that boundary, inspect near/far render culling,
then save and reload repeatedly. Check for duplicate-node repairs, missing
objects and crashes. Repeat loading a stock world in the same process and
multiplayer replication with matching builds. Existing damaged saves are not
repaired by this patch. Depth 14 and higher are explicitly unsupported.
