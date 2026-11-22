# Gameplay terrain-cache RAM experiment (Steam 35924)

The subsequent lossless 1 m replacement is implemented and deployed; see
[terrain-compression.md](terrain-compression.md) for current runtime results.
This document retains the history of the discontinued spacing experiment.

**September 13: 2 m experiment abandoned after rendering failures and a rail
construction crash. Active config is restored to 1 m. Do not deploy the newer
construction-hook prototype in `out` as a validated fix.**

Live inspection of loaded CHONKYIe.sav around Torrance shows terrain features
repeated across adjacent grid cells, large vertical discontinuities at cell
edges and objects above the rendered ground. This exceeds expected 2 m
coarsening. Further RE found metre-indexed alignment regions being interpreted
as 2 m sample coordinates: repeated features existed in CPU caches before
rendering. The deployed alignment bridge refines at stock resolution and
downsamples final aligned regions, but subsequent rail construction still
crashed. A successful load and lower memory total do not establish correctness.
No live cache mutation was attempted.

Construction worker `2146540` combines metre-indexed rectangles with a cache
pointer from `33d7c0` and levels from `33d330`; at 2 m those dimensions disagree.
The earlier construction dump faults at `21467d0`. A narrowly gated prototype
for these calls was built and unit-tested, but not deployed or playtested.
The user chose to stop pursuing 2 m. Preserve this code as experimental history.
Dump `5c723224-5412-4abb-b61b-fe17d073f2b1` records a read access violation in
VCRUNTIME140 memcpy, called from game RVA `25f653f`, through `25dd1b3`,
`34717e` (TextureUploader::Upload<uint16>) and `338cc7` (terrain render update).
The upload still requests 259 x 259 samples (257 plus borders), while
`32ea60` builds its source dimensions from the changed highLevels (131 x 131
at level 7, including borders). The earlier allocation tests did
not cover that upload contract. No post-change autosave was observed in this
failed load; the save-writing optimization was not on the crashing path.

The fix hooks `347090` and expands precisely this 131x131 input to 259x259
using bilinear interpolation, preserving the 2 m vertices and border positions.
The original synchronous upload receives a complete stock-size buffer. This
temporary allocation is 131 KiB per call; persistent caches remain 129x129.
Other input dimensions, offsets and grids pass through. The bridge is installed
before enabling the cache override, including when preserving a saved 2 m world.
`tools/test_terrain_upload.py` checks all output samples against an independent
oracle and executes the original upload instructions in both atlas modes.

`terrain_cache_spacing_m=2` requests a 2 m derived terrain height cache on
world creation and loading. `1` restores 1 m on the next load. `0` leaves the
saved/default resolution alone. The source config defaults to 0; the local
playtest config was explicitly approved for 2 m.

The 4 m source heightmap, 256 m tile dimensions, map extents and octree depth
are preserved. Terrain alignment/deformation is evaluated on a coarser grid;
close-up slopes and ground around tracks, roads and buildings need playtesting.
The high-resolution level is an ECS Terrain field serialized with the world.
Turning the option off does not force a saved 2 m world back to 1 m: use `1`
and reload for that. Keep an original save when comparing appearance.

## Measured baseline

Read-only inspection of the running 114 x 570-tile world (PID 62832,
September 12/13, 2026) found 64,980 tile caches in each of two CTerrain grids.
Each vector had exactly 132,098 bytes of size and capacity (257 x 257 uint16).
The sampled shared-pointer counts were 2 and sampled data matched between
views. These are shared caches: counting both views would double-count them.
None of 1,024 sampled caches per view was all zero. This does not rule out
nonzero constant tiles or other exact duplicate data; those require separate
sampling before making a deduplication claim.

One set of cache payloads is 7.994 GiB. A 129 x 129 cache uses 2.014 GiB,
giving a calculated reduction of **5.980 GiB**. Allocator overhead and other
live allocations affect process totals; this is not yet a measured post-load
working-set or private-commit reduction. The baseline process was around
26.3 GiB private commit / 22.8 GiB working set.

## Engine evidence and hook

### Lossless alternative measured after restoring 1 m

PID 77516 loaded original CHONKYIe on September 13 with highLevels=8,
baseLevels=6 and 4 m source spacing. Read-only inspection found 64,980 unique
257x257 uint16 cache vectors. Process memory shortly after entry was
25,249,058,816 bytes private commit and 22,830,309,376 bytes working set; this
is a recovery baseline, not a new optimization result.

`tools/benchmark_terrain_compression.py` tested 1,024 randomly selected tiles
(seed 20260913), independently compressed with zstd level 1. All four encodings
round-tripped every byte exactly. Plain zstd used 27.11% of the raw bytes,
projecting 2.167 GiB for all cache payloads. Row differences modulo 65536 used
16.18%, projecting 1.294 GiB; separating low/high bytes after row differences
used 15.71%, projecting 1.256 GiB. These are sample extrapolations, excluding
allocator overhead, metadata and an uncompressed active working cache.
No sampled tile was constant, and all 1,024 sampled tiles were distinct.

Raw decode measured approximately 162 microseconds/tile and row-difference
decode 427 microseconds/tile in this Python/NumPy benchmark while the game was
running. These include Python allocation overhead, are single-run measurements,
and do not predict native in-game frame times. Full results and sample hash:
`terrain-compression-benchmark.json`. The raw samples and selected tile indices
remain in `%TEMP%/tpf2-cache-visual/terrain-1m-samples.bin` and `.json`.

No compressed cache hook has been deployed. `33d7c0` returns the shared vector
object directly from a 40-byte grid cell; callers can retain its raw data
pointer. Construction worker `2146540` and wrapper `156a10` are known callers;
virtual and inline access also need auditing. A safe implementation must pin
all tiles while readers use them, synchronize writes/publication (`33cd10`),
and preserve sharing between terrain views. Hooking only the getter and then
evicting vectors on an LRU timer is not safe.

A subsequent implementation uses fixed virtual addresses and synchronized
section mappings to preserve raw-pointer access through compression. See
`terrain-compression.md` for code, native concurrency tests and current status.
It is built and staged, but not deployed or tested in-game. Reserve compressed
storage plus an active uncompressed budget when estimating savings; the whole
6.7 GiB theoretical reduction is not achievable with a working cache/overhead.

An existing texture-streaming limit was also found: AppConfig +0x1d0 overrides
MB limits, with defaults selected by quality (160/640/2560 MiB); `25dc390` and
`25dc010` configure the pool. This concerns rendering textures and has not
been demonstrated to reduce the measured terrain/system-RAM allocation.

Image SHA256:
`782b904a8f7bbdac1f7a18528f1a5c778691e5aa3087c37c351bf6912585175c`.

- `CTerrain` constructor RVA `33c670` starts with terrain entity -1 and
  allocates an empty, zero-dimension Grid (it is not normally a null pointer).
- `CTerrain::SetTerrainEntity`, `33da70`, copies the ECS Terrain component
  into CTerrain and creates the tile grid. The hook changes the authoritative
  component immediately before this first binding. This lifecycle is used for
  both new worlds and loaded worlds. No existing populated grid is resized.
- Component getter `112210` resolves the Terrain from engine and entity.
  The getter and binding prologues are byte-verified before installing.
- Terrain is 0x24 bytes: baseLevels +8; base sample spacing +0xc/+0x10;
  highLevels +0x18. Only highLevels changes, 8 to 7 (or 7 to 8 for restoration).
  Only the measured baseLevels=6, 4 m spacing, highLevels in {7,8}, and
  positive dimensions up to 2048 are accepted. Other layouts pass through.
- `CTerrain::AddTile`, `33cb60`, reads the authoritative highLevels and
  allocates `((1 << highLevels) + 1)^2` uint16 samples.
- `terrain_util::BaseGetHeightmapRefined`, `3c4620`, derives its refinement
  factor from `1 << (highLevels - baseLevels)` and requires highLevels >
  baseLevels. Level 7 preserves that invariant; level 6 is not offered.
- `CTerrain::UpdateCachedTerrainComponent`, `33dbf0`, copies the same
  authoritative component on changes. Patching only CTerrain's copy would
  have produced inconsistent resolutions, so it is not used.
- Terrain loading: `195150` -> `1a20b0` -> `19bfa0` reads the component,
  including the highLevels field, from the save.

## Validation and deployment

`python tools/test_terrain_cache.py` checks native guards, preservation of all
other component bytes, restoration, fresh and already-populated grid handling,
callback ABI, original hook bytes and instruction boundaries, and installer
failure paths. Unicorn executes the original SetTerrainEntity and AddTile
instructions with fixture ECS/allocator calls: level 7 allocates 129 x 129,
level 8 allocates 257 x 257, both keep 256 m tile extents and tile revisions.
This does not substitute for full rendering, construction and save/reload tests.

Keep `terrain_cache_spacing_m=1` for the recovery load and normal play. Existing
depth-13, placement, generation and world-entry optimizations remain enabled.
Do not re-enable 2 m based on allocation or interpolation unit tests alone.

After fixing the renderer mismatch, next playtest: save the current world, restart, reload, check the host log for
`terrain cache: 2 m, highLevels=7`, and compare committed memory after the same
camera view has settled. Inspect tracks/roads/terrain editing, then save and
reload once more. Post-change runtime validation remains pending that restart.
