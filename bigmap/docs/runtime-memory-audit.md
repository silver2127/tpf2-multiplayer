# Runtime memory audit for very large maps (Steam 35924)

Read-only reverse engineering, September 15, 2026. No hooks were written, no game
was launched or inspected, and nothing under the Steam directory was modified.
Executable SHA256 `782b904a8f7bbdac1f7a18528f1a5c778691e5aa3087c37c351bf6912585175c`.
All addresses are RVAs relative to `0x140000000`.

Every number is labelled:

- **MEASURED**: observed in a running game (the plugin stage log quoted below, or
  earlier measurements recorded in `terrain-compression.md`).
- **DERIVED**: computed from decompiled code (element sizes, grid dimensions,
  allocation formulas). Correct if the decompilation reading is correct, but not
  observed in a live process.
- **GUESS**: an inference or extrapolation that code does not pin down (instance
  counts, compression ratios, retention where the free path was not found).

The 257x257 terrain height cache is excluded except where other structures
interact with it; see `terrain-compression.md`.

## Measured baseline (256x256 desert map, 65,536 tiles, 65.5 km edge)

MEASURED, plugin world-entry stage log (private / resident GiB):

| point | private | resident |
|---|---:|---:|
| before world allocation | 3.1 | 2.9 |
| after terrain generation | 14.9 | 18.6 |
| begin trees | 6.45 | 10.14 |
| end trees (12.1 s) | 9.8 | 13.5 |
| end scenery assets | 11.6 | 15.2 |
| begin town/industry road connections | 13.4 | 16.9 |
| end InitNewGame | 13.9 | 15.5 |
| later, in game (working set) | | ~18 |

Tree stage: +3.37 GiB private. Scenery: +1.75 GiB. Towns and industries: +1.8 GiB.
The render side (`terrain::RenderDataManager`) and the first replication into the
second game state both happen after `InitNewGame`, so they are not in these stage
deltas. Its replica *log* is (see section 2).

## Ranked findings

Savings assume the densities of the measured desert map. A 512x512 map has 4x the
tiles and area, so area-proportional rows scale by 4x.

| # | Structure | Size 256² | Size 512² | Class | Reduction idea | Est. savings 256² / 512² | Risk |
|---|---|---:|---:|---|---|---:|---|
| 1 | Material-index `DataGrid<uint8>` CPU copy at 1 m (RenderDataManager+0x250): 260x260 bytes per tile, created for every tile at world entry | 4.13 GiB | 16.5 GiB | DERIVED size; retention GUESS (no free path found, "release" only flags) | Pager-style compression of the fixed 67,601-byte cell vectors (hook the resize at `3303e1`) | ~3.5 / ~14 GiB (GUESS ratio >= 8:1) | Medium |
| 2 | Second copy of all static `ModelInstanceList` data in the double-buffered game state, plus a deep-copied replica log during world entry | one extra copy of instance payload, ~1.6 GiB | ~6.3 GiB | DERIVED structure; counts GUESS | Share immutable instance buffers between the two engines (section-mapped COW, as the terrain pager does) | ~1.4 / ~5.5 GiB | High |
| 3 | RenderDataManager per-tile record (0xe0) plus 65x65 x 4-byte `TerrainVertex` vector, for every tile | 1.05 GiB | 4.18 GiB | DERIVED | Pager for the fixed 16,900-byte vectors | ~0.5 / ~2 GiB (GUESS 2:1) | Medium |
| 4 | Octree per-instance references: 12 bytes per tree/asset instance, per engine, in per-node per-model vectors | ~0.9-1.1 GiB | ~3.6-4.4 GiB | DERIVED structure; counts GUESS | `shrink_to_fit` after entry (slack only); structural change not practical | ~0.2 / ~0.9 GiB | Low (shrink) |
| 5 | `TerrainTileHeightmap` base heightmap, 65x65 uint16 per tile, COW-shared between engines | 0.52 GiB | 2.07 GiB | DERIVED | Compress with the same pager (8,450-byte vectors) | ~0.4 / ~1.6 GiB (GUESS) | Medium |
| 6 | `push_back` slack (1.5x growth) left in sim-side instance vectors by the 64 m grouping | ~0.35 GiB | ~1.4 GiB | DERIVED growth; counts GUESS | Shrink both vectors before `AddComponent` at call site `3bc8d1` | ~0.3 / ~1.2 GiB | Low |
| 7 | Emission grids: 16x16 float cells per tile, two layers in the component plus one scratch layer, in both engines, fully copied every sim step | 0.38 GiB | 1.5 GiB | DERIVED (layer role GUESS) | None safe: simulation-visible | 0 | n/a |
| 8 | `GridCollision` (ten uint8 rasters at 16/32/64 m) plus `PassableTiles` (10 m uint16), in both engines | 0.34 GiB | 1.35 GiB | DERIVED | None safe: ship/animal/placement behaviour | 0 | n/a |
| 9 | 64 m asset-cell entities: component records, entity bookkeeping, octree entity ids, both engines | ~0.4-0.5 GiB | ~1.6-2 GiB | GUESS | None practical | 0 | n/a |
| 10 | Ambient `DataGrid<uint8>` at 8 m (RenderDataManager+0x248): 34x34 bytes per tile, all tiles | 0.08 GiB | 0.3 GiB | DERIVED | Same pager as #1 if #1 is built | ~0.07 / ~0.28 GiB | Low |
| 11 | Street occupancy raster (already scaled by the plugin) | ~134 MB | fixed | MEASURED (given) | done | - | - |
| - | Transient at world entry: `Map` tree/asset vectors (16 / 0x44 bytes each), the tree accumulator (<= 1.5x 24 B/tree), grouped per-cell copies, replica log | several GiB of peak | ~4x | DERIVED shape, GUESS size | Reserve exact capacity; release `Map` vectors after use | peak only | Medium |

Sanity check, DERIVED against MEASURED: `terrain-compression.md` records a loaded
114x570 world (64,980 tiles) at 16.7-18.5 GiB private with the height cache already
compressed to ~2.5 GiB. Rows 1, 3, 5, 7, 8 and 10 alone derive to ~6.4 GiB at that
tile count. Add two copies of vegetation and the ~3 GiB pre-world baseline and the
total is roughly the measured figure. This is consistent, not proof: row 1's
retention is the largest unverified term.

## 1. Trees and scenery (`ModelInstanceList`)

### Record layouts

`ecs::component::ModelInstanceList` is a 0x38-byte component (CompVec element
size, `17bbc0` lines 28-53; replica copy `1f3680` line 59):

| offset | field |
|---|---|
| +0x00 | `std::vector<ThinModelInstance>` (0x18-byte elements), `thinInstances` |
| +0x18 | `std::vector<ModelInstance>` (0xc0-byte elements), `fatInstances` |
| +0x30 | `bool dynamic` (`1f3680` line 82, assert `dst.dynamic == src.dynamic`) |

Assert strings name both vectors: `2c6560` "instanceIndex < mil.thinInstances.size()",
"idx < mil.fatInstances.size()". Instance indices >= 0 address thin instances and
`~i` (negative) addresses fat ones (`2c6560` lines 47-133).

**ThinModelInstance, 24 bytes (DERIVED):**

| offset | field | evidence |
|---|---|---|
| +0x00 | int32 model id | tree lambda `15e3c0` line 35 |
| +0x04 | CVec3f position | `15e3c0` lines 36-40 copy MapTree +4..+0x10; `240a3f0` uses +4/+8/+0xc as translation |
| +0x10 | float z rotation (radians) | `240a3f0` sin/cos of +0x10 |
| +0x14 | float uniform scale | `240a3f0` multiplies the basis by +0x14 |

The Lua usertype string (`33c38f0`) has the same shape: int, CVec3f, float, float.
`thin_to_transform` (`240a3f0`) rebuilds the full CMat4f, so no field is redundant.

**ModelInstance, 0xc0 bytes (DERIVED, partial):**

| offset | field | evidence |
|---|---|---|
| +0x00 | int32 model id | `1535f0` line 14 |
| +0x04 | CMat4f transform (0x40) | `1535f0` lines 15-26; `9c9d20` passes `piVar5+1` as the transform |
| +0x44..+0x63 | 0x20 bytes, zero for static assets; copied every step for dynamic lists | `1535f0` lines 27-30; `1f3680` lines 109-120 |
| +0x64..+0x83 | eight int/float fields, zero for static | `1f3680` lines 127-141 |
| +0x84 | byte `transf0` flag | renderer assert `!instance.transf0` (`2bd590` line 414) |
| +0x88 | int32, default -1 | `1535f0` line 37, `2c6560` line 93 |
| +0x90 | std::vector (dynamic per-instance data) | copy ctor `1e5070` line 49; `1f3680` line 143 |
| +0xa8 | std::vector | `1e5070` line 50; `1f3680` line 147 |

For static scenery, only the model id and matrix carry information. 0x80 bytes are
zero and the two vectors are empty (`1535f0` lines 27-43), so 0xbc of 0xc0 bytes
are derivable. The engine cannot store scenery thin, though: the asset placement
job builds a slope-aligned basis from the heightmap gradient and then rotates it
about z (`3a30c0` lines 182-272). Thin instances only encode z rotation and uniform
scale, so conversion would lose terrain tilt. Only assets on (near-)flat ground
could be converted losslessly.

### Who emits what

- **Trees are thin.** The `MapTree` lambda `do_call` (`15e3c0`, vtable
  `2f336a0` slot 2) pushes a 24-byte record with a random rotation and scale.
- **Scenery is fat.** The `MapAsset` lambda (`15e2d0` -> `1535f0`) pushes a
  0xc0-byte record with the `MapAsset` matrix.
- Inputs: `MapTree` is 16 bytes (int + CVec3f; `create_trees` iterates in 4-int
  steps). `MapAsset` is 0x44 bytes (int + CMat4f; `3a14d0` frees in 0x44 units,
  Lua usertype "id"/"transf").

### Hand-off to the ECS (move, with slack) and transients

1. `create_trees` (`3b8bd0`) fills an accumulator `ModelInstanceList` on its stack
   (`local_1438`) by `push_back`. MSVC growth is 1.5x (realloc `3b9ee0` lines 31-37).
2. `publish_model_instances` (`3bc760`) calls `group_model_instances` (`3bdad0`),
   which copies every instance into a hash-list node per 64 m cell (0x42800000 =
   64.0, `3b8bd0` line 154). Node layout: next/prev, key at +0x10/+0x14, thin
   vector at +0x18, fat vector at +0x30 (0x48 bytes). Copies are one `push_back` at
   a time, so each cell vector keeps up to 50% slack.
3. For each cell, `AddComponent<ModelInstanceList>(&&)` (`1691c0`, called at
   `3bc8d1`) moves the vectors into the CompVec: `17bbc0` lines 34-53 steal the
   three pointers and zero the source. **The slack is retained.**
4. The grouped map is destroyed at `3bc995` (`3c21d0`). Only node shells are freed,
   because the vectors were moved out. The accumulator is destroyed right after
   `publish` returns (`3b8bd0` lines 155-156; call `3b8f5b`).

Transient peak inside `publish` (DERIVED): accumulator (<= 1.5x) plus grouped cell
copies (<= 1.5x) plus the replica-log copy (1x, below) plus octree references.
That is about 4x the instance payload before the accumulator is freed.

### Where instances are duplicated

1. **Replica log during world entry.** `AddComponent<ModelInstanceList>` walks the
   engine's replica list (+0x110, 0x28 each). For every active replica it creates a
   `ReplicaCompVec<ModelInstanceList>` and `push_back`s a *copy* of the stored
   component (`1691c0` lines 83-159 -> `2612b0`). `2612b0` copy-constructs the thin
   vector (`1e07d0`, call `2612e1`) and the fat vector (`1dfdc0`, call `2612ef`).
   The source is the live CompVec element, so this must be a copy (DERIVED).
   gameStates[0]'s engine already has replica 0 during `InitNewGame`:
   `StartGameSim` (`1189f0`) replicates gameStates[0] -> [1] at `118a26` before it
   registers gameStates[1]'s own replica (`23dd190`, assert "replIdx1 == 0").
   **The measured +3.37 / +1.75 GiB deltas therefore already contain a second, exact-size copy.**
2. **Two complete game states.** `CGame::RunGameSimLoop` (`1184d0`) asserts
   `simIdx == 1 - oldSimIdx` and calls `GameState::Replicate(old, new)` every step
   (`11875d`). `Engine::Replicate` (`23e0cc0`) applies the log (`Replicator::Apply`
   `23dd700`, cases 5-8 re-add components). It then runs per-type copy functions
   (`22fbe0`): Animal, BaseNodeTrafficLight, BoundingVolume, EmissionGrid,
   ModelInstanceList (4 parallel slices), ModelPerson, MovePath(Aircraft),
   ParticleSystem, RoadVehicle, SimBuilding, SimEntityAtBuilding, Town,
   TownConnection, Tram, TransportNetwork, TransportVehicle, VehicleDepot.
   `StartGameSim` asserts both engines' entity-component vectors are identical
   (`1189f0` line 189). The `ModelInstanceList` per-step copy only touches lists
   with `dynamic == true` (`1f3680` line 90), so static vegetation is copied once
   through the log and then **held twice in steady state** (DERIVED).
3. **Octree.** `OctreeSystem::ComponentAddedToEntity` (`a4faa0`) -> `a4f1c0`
   inserts, for every thin and fat instance, a 12-byte `{entity, dataIndex,
   instanceIndex}` into the node's `flat_hash_map<ModelIdOrGeneratorIndex,
   vector>` at node+0x40 (0x20-byte slots): lines 61-116 thin, 120-174 fat,
   growth calls at `a4f37b` / `a4f530`. Each game state has its own octree
   (`allocate_world` resizes GameState+0x48, and `Replicate` calls
   `allocate_world` for an empty destination, `241630` lines 19-21). The dst
   engine's systems see the replayed `AddComponent`s. So: **12 B x instances x 2 engines**, plus slack.
4. **Collision.** Per cell entity only. `collision_util::CalcBoundingBox`
   (`9c9d20`) folds instances into one Box3 BoundingVolume (0x18), and the
   "Vegetation map" is a 32 m uint8 raster (`ae22e0` line 231). No per-instance
   collision copy was found.
5. **Rendering.** `CRenderer::NewUpdate` (`2bd590`) walks the octree per-model
   references and resolves each instance with `GetInstance` (`2c6560`). For thin
   instances that builds a temporary 0xc0 record on the stack. No persistent
   per-instance CPU render cache was found (GUESS: GPU instance buffers are per
   frame; not audited).

### Instance count estimate (GUESS)

Per tree during the tree stage: sim copy 24 B x s, replica-log copy 24 B, and a
sim-engine octree reference 12 B x s, where s is capacity/size slack. A per-cell
fixed cost also applies: CompVec 0x38 + log copy 0x38 + BoundingVolume 0x18 (x2),
asset-group components, entity bookkeeping (+0xa0 / +0xb8 / +0xd0 tables), 0x14
log records and heap headers. At up to 1,048,576 cells that is ~0.3-0.5 GiB. With
s = 1.25-1.5, +3.37 GiB gives **~35-45 million trees** (desert map). Scenery at
192 B x s + 192 B + 12 B x s gives **~3.5-4 million fat assets** from +1.75 GiB. The
second game state's octree, added after `InitNewGame`, is not in these deltas.

One copy of instance payload is then ~0.9 GiB (trees) + ~0.7 GiB (assets). At
512x512 with the same density it is ~4x.

### Hook point for shrink/compaction

Retarget the single call at **`3bc8d1`** in `publish_model_instances`:

```
3bc8c9: 54 24 38 48 8b 4c 24 30      (tail of the argument setup)
3bc8d1: e8 ea c8 da ff               call 0x1691c0  AddComponent<ModelInstanceList>(engine, entity, cell+0x18)
```

The rel32 resolves to `0x3bc8d6 - 0x253716 = 0x1691c0`. Both `create_trees`
(`3b8f51`) and `create_assets` (`3b89e1`) reach it, and no other
`AddComponent<ModelInstanceList>` caller does (15 other callers: vehicles,
constructions, fields, animals). Before forwarding, reallocate the thin vector (+0)
and fat vector (+0x18) of `r8` to their size, then call the original.

- Use the engine's own allocation convention. `operator new` is `2bf3a80`; blocks
  of 0x1000+ bytes carry the real pointer at [-8], so free them the way `3b9ee0`
  lines 65-71 do. Alternatively copy with the engine's vector copy-constructors
  (`1e07d0`, `1dfdc0`), swap, and destroy the old buffers with the engine
  destructors seen in `3c21d0` (`25ad60` fat, `abf20` thin).
- **What could break:** very little, because only capacity changes. The node is
  destroyed immediately afterwards and holds no other pointers into the buffer. A
  wrong free convention would corrupt the heap, and the patch must stay strictly on
  the world-entry thread. The replica log already copies at exact size, so only the
  sim-side copy benefits.
- Saved games are unaffected (vectors serialize by size, `18c400`). Saved worlds
  load through `Load<ModelInstanceList>` instead, which is a different path whose
  slack was not examined.

A larger compaction (row 2) would place static thin/fat buffers in a section-backed
arena and map the replica copy as a shared view. The hooks would be the log copies
at `2612e1` / `2612ef`, plus vector destruction and the reallocation paths used
when a list is edited (bulldozing assets rebuilds the component). Readers to audit
are listed by the assert strings `!mil.fatInstances.empty()` (13 functions),
`mil.thinInstances.empty() && mil.fatInstances.size() == 1` (14, single-instance
entities), and construction code indexing both vectors (`214c780`, `214cc40`).

## 2. Per-tile runtime structures

Tile = 256 m = 2^baseLevels (6) x 4 m spacing (`3c5860`; `ViewTerrain` vtable
slot 4 `34dea0` -> `33d330` returns baseLevels).

| Structure | Element / bytes per tile | Copies | 65,536 tiles | 262,144 tiles | Evidence | Class |
|---|---|---|---:|---:|---|---|
| Height cache 257² uint16 (excluded) | 132,098 | 1 (COW between 2 CTerrain views) | 8.06 GiB raw | 32.3 GiB raw | `33cb60`, pager docs | MEASURED |
| `TerrainTileHeightmap` (base level) | 65x65 uint16 = 8,450 + 0x18 handle | 1 (COW) | 0.52 GiB | 2.07 GiB | `ae9ca0` line 90 resizes (2^6+1)²; replica push uses COW copy `1dedd0` (`1751c0` line 189) | DERIVED |
| `TerrainTile` component | 0xc | 2 engines | <2 MB | <7 MB | `33cb60` line 42 | DERIVED |
| CTerrain tile grid cell | 0x28 + shared_ptr block 0x28 | 2 views | ~5 MB | ~20 MB | `33c540` | DERIVED |
| RDM tile record + `TerrainVertex` | 0xe0 + 65² x 4 B = 17,124 | 1 | 1.05 GiB | 4.18 GiB | `327650` (0xe0), `32bfd0` line 72 and `327eb0` line 1254 (4-byte elements, dim1 = 2^baseLevels+1), `32ce50` fills every tile | DERIVED |
| Material `DataGrid<uint8>` (RDM+0x250) | 260x260+1 = 67,601 + 16 occupancy + 0x48 cell | 1 | 4.13 GiB | 16.5 GiB | see below | DERIVED size, retention GUESS |
| Ambient `DataGrid<uint8>` (RDM+0x248) | 34x34+1 = 1,157 + 1 + 0x48 | 1 | 0.08 GiB | 0.30 GiB | `327eb0` lines 938-951, `32ce50` lines 38-42, `3315c0` "RecreateAmbientData" | DERIVED |
| Texture grids (heightmap, ambient, material GPU textures) | per-group 0x38/0x48 cell + occupancy bytes on CPU | 1 | <10 MB CPU | <40 MB CPU | `3475f0`, `3471f0`, `329ff0` | DERIVED (GPU memory not audited) |
| Emission grid | 16x16 cells x 4 B = 1,024 per layer; 2 component layers + 1 scratch | 2 engines | 0.38 GiB | 1.5 GiB | `2f7370` (width = tiles x 16, two 4x4 block grids of `innerW x innerH` 4-byte vectors), `2f7850` swaps the component buffer with system+0x50, `1f2650`/`1eeb00` copy both vectors every step | DERIVED (scratch role GUESS) |
| GridCollision + PassableTiles | ~2.77 KB | 2 engines | 0.34 GiB | 1.35 GiB | section 3 | DERIVED |
| RDM section records | 0x48 per 4x4 tiles | 1 | 0.3 MB | 1.2 MB | `3273e0` | DERIVED |
| Grass section data | per visible section | 1 | not sized | not sized | `2fe840`/`2feea0` | GUESS (view-bound) |
| `TerrainTileBrush` | per painted tile | 2 | not sized | not sized | `195b90` | GUESS (sparse) |

### Material-index DataGrid (row 1)

- **Construction.** The RenderDataManager ctor (`327eb0` lines 1029-1044) builds
  `DataGrid<unsigned char>` (`327000` -> `3479f0`) with worldResolution (1,1),
  border (2,2), group 1, subSize (4,4) and centre flag 0. `IBaseGrid::CalcDim`
  (`348390`) then gives `(256/1 + 0 + 2*2) * 1 = 260` per axis. The ambient grid
  uses (8,8) / border (1,1) / flag 1 -> `256/8 + 2 = 34`.
- **Cell lifetime.** Vtable `2f91320`: slot 1 `3303b0` = InternCreate, resizing the
  cell vector to `dim.x*dim.y+1` bytes (call `3303e1` -> `1d5830`) and writing
  overflow canary 'V'. Slot 2 `330350` = InternDestroy, which checks the canary and
  resizes to 0. Slot 3 `32f4a0` = Exists. A running byte count lives at DataGrid+0x60
  as an **int32**; a fully materialised 256² grid (4.43e9 bytes) would overflow it,
  so it cannot be used to verify retention.
- **All tiles are generated at entry.** The RDM ctor logs "Initial material index
  generation" and calls `BeginMaterialIndex` (`315720`) on the whole tile grid
  (RDM+0x58) with the async flag. `MaterialIndexAsyncWork` (`312990`) walks the full
  rectangle in 8x8-tile boxes. `FinishBox` results are `vector<tuple<CVec2i,
  vector<uint8>>>` (`30c8a0`), applied via `3163c0`, which reads RDM+0x250.
- **No free path found.** The per-tile "release" `3495f0` used by `32ce50` does not
  free: it writes state byte 2 into each sub-block of the occupancy vector. That
  suggests the CPU copy stays for later partial uploads
  (`TextureUploader<vector<uint8>>`, `346e60`) and `UpdateBox` edits (`318f20`,
  "MaterialListBuffer"). No `InternDestroy` caller was traced in this audit.
- **Interaction with the pager.** `32ce50` computes every tile's vertices and
  ambient data from `ITiledTerrain` at world entry (`334aa0` / `334c60`, `3315c0`),
  which plausibly faults the whole height cache back in. That matches the warm-budget
  need documented in `terrain-compression.md` (GUESS).
- **Reduction.** Material indices are low-entropy (few ground textures per region,
  dithered edges), so they should compress far better than heights (GUESS). Every
  cell allocation is an exact 67,601-byte vector created by one call site (`3303e1`,
  bytes `48 2b 1a ff c0 48 63 d0 | e8 4a 54 ea ff`, target `1d5830`, the same
  `vector<uint8>::resize` helper). The existing pager design therefore carries over:
  filter by return address `3303e6` and size 67,601, fixed-size slots, and
  fault-and-restore at the original address. Readers are CPU-side (worker-pool
  writes, uploader memcpy), which the pager already supports.
- **Risk.** Medium: concurrent `ThreadPool` writers across 8x8 boxes, and GPU upload
  bursts at entry would restore many cells at once.
- **Verify first.** Count live cell vectors, as described at the end.

## 3. Other area-scaled structures

| Structure | Scaling | 256² | 512² | Evidence | Class |
|---|---|---:|---:|---|---|
| GridCollision "Water map", "Water border", "Ships map", "Water obstacle", "Bridges map" | uint8 at 16 m, (edge/16+1)² each | 5 x 16.0 MB | 5 x 64 MB | `ae22e0` (0x41800000 = 16 m), cell count `ae3fa0` lines 29-38, byte vector `1d5830`, `GetCollisionDataAt` returns `uint8*` (`ae3ea0`) | DERIVED |
| "Vegetation map", "Civilisation map" | uint8 at 32 m | 2 x 4.0 MB | 2 x 16 MB | `ae22e0` (0x42000000) | DERIVED |
| "Predators map", "Fish map", "Network map" | uint8 at 64 m | 3 x 1.0 MB | 3 x 4 MB | `ae22e0` (0x42800000) | DERIVED |
| PassableTiles | uint16 at 10 m | 82 MB | 328 MB | `ae56d0` (10.0 from `allocate_world` line 53), `ae4e90` 2-byte elements, `ae5620` | DERIVED |
| All of the above | per GameState (both engines run `allocate_world`) | x2 = ~0.34 GiB | ~1.35 GiB | `241630` lines 19-21 | DERIVED |
| Octree nodes (0xa8) | leaves 128 m, depth 13 | ~0.06 GiB per engine + node vectors | ~0.25 GiB | `octree-depth12.md` | DERIVED/GUESS |
| 64 m asset-cell entities | (edge/64)² max per publish (trees and assets separately) | <= 1.05 M + assets | <= 4.2 M + assets | `3bdad0` | DERIVED bound |
| Street occupancy raster | scaled by plugin | ~134 MB | fixed | given | MEASURED |
| Emission map | (tiles x 16)² floats | see section 2 | | `2f7370` | DERIVED |
| `TerrainAlignmentSystem` blocks | sparse map keyed by block, entries per construction | small | small | `aac880`, `aad180` | GUESS |
| Road-connection stage (`937590` -> `9473a0`, `9466f0`) | transient during entry | not sized | not sized | decompiled, no dense area grid identified | GUESS |
| Pathfinding | graph-based (`TransportNetwork`), not area | - | - | | GUESS |

Town, industry and catchment structures are per entity or per station
(`CatchmentAreaSystem`), not dense area grids. None was found whose size depends
on map area rather than content.

## 4. Candidate reductions in detail

1. **Material DataGrid pager** (row 1). Highest expected yield if retention is
   confirmed. It reuses `terrain_pager.h` with a second slot class of 17 pages
   (69,632 bytes incl. header).
2. **Shared static instance buffers** (row 2). This removes the second engine's
   copy of vegetation. It needs copy-on-write semantics for the rare edit paths
   (bulldozing, asset brush, `ModelPlaceholderFixer::ReplaceIfMissing`) and
   destructor/free interception. Even without the second copy, the replica log
   at world entry still makes one transient copy unless it is also redirected.
3. **Vertex pager** (row 3). `GetVertices` (`32bfd0`) returns raw pointers into the
   vector; the renderer's worker pool reads visible tiles only after entry. Fixed
   16,900-byte vectors make recognition simple. Compression of float/packed
   vertex data is weaker than heights (GUESS 2:1).
4. **Base heightmap compression** (row 5). The same approach as the 257² cache,
   applied to `TerrainTileHeightmap`. Readers include cache rebuilds and
   construction. COW sharing across engines must be preserved, as the pager does.
5. **Shrink after grouping** (row 6) and **octree reference shrink** (row 4).
   Low-risk, modest savings. The octree vectors can be shrunk once after world
   entry on the sim thread (inside a `GameSim::Step` detour); they grow again as
   the world changes.
6. **Entry peak.** `create_trees` could reserve the accumulator to the `MapTree`
   count, removing the 2.5x realloc overlap. The `Map` generation vectors (trees
   16 B, assets 0x44 B each) could be released once consumed if nothing later
   reads them (GUESS: animal spawning and saves were not audited).

Not recommended: coarser emission, collision or passable rasters. They change
simulation results and would break multiplayer determinism between patched and
unpatched peers.

## Live measurement, September 15 (read-only, PID 50208)

The user's loaded 114x228-tile desert world (25,992 tiles, save
`autosave_New Game_1943-02-24_10`) under the installed 0.3.1 plugin was read with
`ReadProcessMemory` only (scripts in the session scratchpad: `inspect_material_grid.py`,
`inspect_cells.py`, `inspect_grids.py`, `sample_material.py`). Process private
9.01 GiB, working set 10.13 GiB after the load settled.

- **Material grid: CONFIRMED retained.** RDM+0x250 is a *pointer* to the
  DataGrid (not embedded). Header +0x70/+0x74 = 115x229 cells, cells at +0x78,
  0x48 bytes each. The payload vector is at cell **+0x00** (not +0x28, which is a
  16-byte state vector). All 25,992 real tiles hold exactly 67,601 bytes:
  **1.636 GiB, 18% of private memory**. Projected: 4.13 GiB at 256x256, 16.5 GiB
  at 512x512. The ambient grid (RDM+0x248, same vtable) holds 1,157 bytes per
  tile, 0.028 GiB.
- **Material bytes are dithered, not flat.** 1,024 sampled cells: 34 distinct
  values overall, a median of 8 per tile, P(equal to left) 0.49, ~34.8k runs per
  tile. zstd level 1/3/9: 25.8% / 25.5% / 23.5%. Per-tile static models:
  order-0 27.7%, conditioned on the left byte 19.9%. Expect a lossless
  saving of ~75-80% of the grid, not the 8:1 guessed above.
- **Render vertices: NOT retained.** The RDM+0x58 grid (114x228 records of 0xe0)
  holds only 224-byte vectors at +0x18 and a few bytes at +0x40/+0x48; no
  16,900-byte vertex vectors exist after load. Row 3's 1.05 GiB does not apply.
- Other RDM DataGrids (vtable `2f91278`, 29x58 cells) are small except one
  66 MB vector.

## How to verify before building anything (read-only)

On a loaded large world, with a read-only process reader like the existing
`inspect-live.py` scripts:

1. **Material grid retention.** Take the `RenderDataManager` (vtable `2f912d0`),
   its DataGrid at +0x250 (vtable `2f91320`), cell array at +0x78, and cell count
   `(+0x70) x (+0x74)`. Sum `vector.end - vector.begin` of each 0x48-byte cell at
   +0x28. Expect 67,601 per materialised tile.
2. **Instance counts.** For both game states' engines, `engine+0x88[type]` gives
   `CompVec<ModelInstanceList>` (dense data at +0x68..+0x70 in 0x38 elements,
   32-element pages at +0x80). Sum thin sizes (/0x18), fat sizes (/0xc0), and
   capacities for slack.
3. **Vertex vectors.** RDM+0x58 grid; each 0xe0 record's vector at +0 should hold
   4,225 elements.
4. **Octree references.** Walk node+0x40 hash maps and sum the vector sizes (/0xc).

## Reproduction

Decompiles for this audit are in `%TEMP%\tpf2-runtime-audit\b1` through `b4`, each
with its `targetsN.txt`. They were generated with
`tpf2-multiplayer/tools/ghidra/run.ps1 DecompileTargets.java` against
`C:\tools\ghidra_proj`. Lambda `_Do_call` slots, vtable slots and LEA reference
sites were read from the executable with `pefile` (read-only). Earlier
decompiles used here are in `%TEMP%\tpf2-world-entry`, `tpf2-gameplay-ram` and
`tpf2-cache-visual`.
