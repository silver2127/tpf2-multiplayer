# Proposals: roads, track, constructions and edge objects

Build 35924; addresses are RVAs from image base `0x140000000`. Confidence labels are
defined in [README.md](README.md).

Every construction-family action (road, track, upgrade, station, depot, module edit,
stop, signal, bulldoze, terraform) is one command type, `CmdData::BuildProposal`, made
by `make_cmd::BuildProposal` `0x9dc750` from a `construction_builder_util::Proposal`
(r8) and a `Context` (r9). Lua's `api.cmd.make.buildProposal(proposal, context,
ignoreErrors)` builds the same structure through `scripting::Convert` `0x20e72f0`.

## `construction_builder_util::Proposal` (0x2f8 bytes)

A `street_util::StreetProposal` (0x188 bytes) followed by the construction fields.
Vectors are MSVC `{begin, end, capacity}` triplets.

| offset | field | element | status |
|---|---|---|---|
| +0x000 | addedNodes | NodeAndEntity, 24 B | MEASURED |
| +0x018 | addedSegments | SegmentAndEntity, 120 B | MEASURED |
| +0x030 | removedNodes | 24 B, entity at +0x14 | DECOMPILED (MakeProposalRemove) |
| +0x048 | removedSegments | 120 B, entity at +0x00 | DECOMPILED, used live |
| +0x0e0 | edgeObjectsToRemove | | DECOMPILED |
| +0x0f8 | edgeObjectsToAdd | 0x100 B | MEASURED (six live records) |
| +0x170 | frozen node indices | `vector<int>`, indices into addedNodes | DECOMPILED |
| +0x188 | construction edge indices | `unordered_set<int>` (probably terrain-alignment skip) | DECOMPILED |
| +0x1c8 | segmentTags | `vector<string>` parallel to addedSegments (`__module_<slotId>`) | MEASURED |
| +0x1e0 | toRemove | `vector<Entity>` | MEASURED: a bulldoze's target, a module edit's old construction; empty for a placement |
| +0x1f8 | toAdd | ConstructionEntity, 0x8e0 B | MEASURED |
| +0x210 | old2new | `unordered_map` | DECOMPILED |
| +0x250 | not identified | `vector<int>` | DECOMPILED (copy and destructor only) |
| +0x268 | not identified | `std::map` or `std::set` | DECOMPILED (copy and destructor only) |
| +0x278 | baseHeightMod | `Grid<CVec2f>`, cell = {height, base} | DECOMPILED (see [Terrain grids](#terrain-grids-terraform-and-paint)) |
| +0x2a0 | terrain material indices | `Grid<unsigned char>`, 0xff = unchanged | DECOMPILED |
| +0x2c8 | terrain material mask | `Grid<bool>` | DECOMPILED |

`Grid<T>` is `{ int x0, y0, w, h; std::vector<T> data }` (0x28 bytes), row-major:
cell (x, y) is `data[(y - y0) * w + (x - x0)]`. `Grid<bool>` holds a `vector<bool>`, a
`vector<uint32>` of words plus a `size_t` bit count, so it is 0x30 bytes and the Proposal
ends at +0x2f8.

There is no transform matrix in the street half: rotation is baked into the node
positions (placements at several angles compared). A construction's matrix lives in
its ConstructionEntity.

Shapes per tool (MEASURED unless noted):

| action | shape |
|---|---|
| road / track build | added nodes and segments; new pieces carry placeholder ids, existing endpoints positive ids; a mid-span junction also removes the edge it splits |
| street / track upgrade (type, catenary, bus lane, tram) | no added nodes; N added segments replace N removed segments, every endpoint an existing node |
| construction placement | `toAdd[0]` plus the template's street pieces (a one-track modular rail station: 25 track nodes, 24 edges); `toRemove` empty |
| module edit | `toRemove[0]` = the old construction, `toAdd[0]` = the new ConstructionEntity (same file, new params); a modular station re-adds its internal track nodes |
| stop / signal / waypoint placement | the edge removed and re-added, plus one edgeObjectsToAdd record |
| stop / signal bulldoze | the edge removed and re-added without the object |
| construction bulldoze | `toRemove` populated, nothing added |
| terraform | no nodes or segments; the whole edit is `baseHeightMod` (+0x278) (DECOMPILED) |
| paint | no nodes or segments; the material grids (+0x2a0, +0x2c8) (DECOMPILED) |
| asset brush | not mapped; its commit clears old2new (+0x210) first (`0x3d1110` → `0x3500e0`) |

## Node record: NodeAndEntity (24 B)

| offset | field |
|---|---|
| +0x00 | x, y, z (float) |
| +0x0c | flags (u32; 0x7f00 on construction-template nodes) |
| +0x10 | type (i32; 2 in every sample) |
| +0x14 | id: a placeholder (-1, -2, ... in allocation order) or an existing entity |

## Segment record: SegmentAndEntity (120 B)

`{ int entity; BaseEdge comp; int type; BaseEdgeStreet; BaseEdgeTrack; ... }`

| offset | field | status |
|---|---|---|
| +0x00 | entity or placeholder id (continues the node counter) | MEASURED |
| +0x08 / +0x0c | node0 / node1 | MEASURED |
| +0x10 / +0x1c | tangent0 / tangent1, three floats each (real tangents, not the chord) | MEASURED |
| +0x28 | BaseEdge.type: 0 ground, 1 bridge, 2 tunnel | MEASURED |
| +0x2c | BaseEdge.typeIndex: bridge or tunnel type, -1 on the ground | MEASURED |
| +0x30 | objects: vector of 8-B `{entity, EdgeObjectType}` pairs | DECOMPILED, read live |
| +0x48 | edge kind: 0 street, 1 track | sweep EXACT |
| +0x4c | streetType index, -1 on track | sweep EXACT (40 values) |
| +0x50 | hasBus (u8) | controlled differential |
| +0x54 | tramTrackType (i32): 0 none, 1 regular, 2 electric | controlled differential |
| +0x60 | trackType index, -1 on a street | sweep EXACT (8 values) |
| +0x64 | low byte: catenary (upper bytes unrelated) | sweep, 8 pairs |
| +0x68 | owning construction (template pieces) | DECOMPILED + diff |
| +0x70 | player | DECOMPILED + diff |
| +0x74 | owned flag (1) | DECOMPILED + diff |

Traps: +0x04 is uninitialised (one rail sample read 2 and looked like the track type);
+0x51 is noise (reading it as the tram field stamped every tram electrified).

## ConstructionEntity (0x8e0 B)

| offset | field | status |
|---|---|---|
| +0x000 | `std::string` fileName, e.g. `station/rail/modular_station/modular_station.con` | MEASURED |
| +0x050 | description string | string chase |
| +0x070 | UI icon path | string chase |
| +0x460 | params, a `lua::Table` | MEASURED (walked live) |
| +0x728 | transf: Mat4f, 16 floats, translation in [12..14] | MEASURED |
| +0x768 | frozenNodes: `vector<int>`, indices into addedNodes | DECOMPILED |
| +0x780 | segmentsBefore: addedSegments.size() before the template appended its pieces | DECOMPILED |
| +0x8b0 | `std::string` name | DECOMPILED (constructor); content at factory time not measured |

`frozenEdges` is not in the proposal: apply derives it (`0x9ee3b0`) and stores it on the
Construction component (+0x90).

### `lua::Table`

An MSVC `std::map<Variant, Variant>`:

- map: `_Myhead` at +0x00, `_Mysize` at +0x08; `_Myhead->_Left` is `begin()`.
- node: `_Left` +0x00, `_Parent` +0x08, `_Right` +0x10, `_Color` +0x18, `_Isnil` +0x19,
  key Variant +0x20, value Variant +0x48.
- Variant: 0x20-byte payload, u8 tag at +0x20, i.e.
  `std::variant<lua::Nil, bool, double, std::string, lua::Table>`: tag 0 nil, 1 bool
  (payload byte 0), 2 double, 3 `std::string`, 4 nested table. Keys seen: tags 2 and 3.
- Ordering is by tag, then value (numbers before strings, each ascending).

At factory time a road depot carries `paramX, paramY, seed, year`; a modular station
carries its whole `modules` table, booleans in module metadata included. Apply prunes
modules whose slot was not built, and a replace proposal carries `upgrade=true`, so
`game.interface.getEntity(id).params` after apply is not byte-equal to the factory
table.

### MSVC `std::string`

32 bytes: a 16-byte inline buffer, size at +0x10, capacity at +0x18. The text is inline
while capacity < 16, otherwise +0x00 is a heap pointer. Empty = size 0, capacity 15.
The game's `basic_string::assign(dest, src, len)` is `0x83270`.

## Construction templates and the connector

At make time `MakeProposalAdd` `0xa18ca0` → `0x21d84c0` runs the `.con` template and
appends its street connector after the caller's own pieces: an inner node, an outer node
and an apron segment (node flags 0x7f00; segment +0x68 = construction, +0x70 = player,
+0x74 = 1). [DECOMPILED + differential dump]

- The UI calls it with `GetNodes2snap` (15 m radius): the outer node is welded onto an
  existing edge, at an endpoint or, for 0 < t < 1, through `0x21d8d80` =
  RemoveSegment(old) + AddSegment x2. That is why a UI placement arrives as "apron + two
  halves + removed edge".
- `scripting::Convert` (Lua) passes an empty nodes2snap map, so a script placement gets
  the connector at raw template coordinates, unwelded.
- Placeholders for regenerated pieces are allocated as (smallest existing placeholder)
  - 1. Number script placeholders like the UI does (-1, -2, ...): a -100001 base broke
  the bounding-volume bookkeeping (`create_proposal_data.cpp:897`,
  `it != result.result.boundingVolumes.end()`).
- All linkage is by index (frozenNodes, segmentsBefore, Proposal +0x170/+0x188). Never
  compact or reorder addedNodes or addedSegments; the only safe removal is the last
  record.

Consequence for replaying a UI placement from Lua: ship only the split node, the two
halves and the removed edge. At the factory entry the slice's `MergeTemplateStreet`
re-points the template connector's outer end onto the shipped split node, recomputes
straight tangents, sets the node flags to 0x7f00 and drops the template's outer node
(the last record). Split halves must be the original edge's record with new endpoints
and tangents, carrying the road's street type rather than the construction's.

## What a script proposal must carry

All MEASURED:

- **`params.seed`** in every ConstructionEntity. Without it `api.cmd.make.buildProposal`
  returns a bare `false`.
- **`name`**. Apply gives the construction and its child entities (VEHICLE_DEPOT,
  stations) NAME and PLAYER_OWNED only when the name is non-empty (and the player entity
  is valid). An unnamed child crashes the client when a player clicks it: the GUI select
  handler dereferences null in `scripting/legacy/interface.cpp`.
- **Not `game.interface.buildConstruction`** as a substitute. It builds the template's
  street pieces at raw coordinates, construction-owned and never joined to the road, and
  a street proposal cannot remove those pieces afterwards ("Construction not possible").
- **A Context that matches the intent.** `api.type.Context` has `checkTerrainAlignment`,
  `cleanupStreetGraph`, `gatherBuildings`, `gatherFields` and `player`. UI placements run
  with terrain alignment and graph cleanup; with `gatherBuildings=true` apply demolishes
  the footprint's town buildings itself (the make-time `toRemove` of a placement is
  empty). A nil Context is not equivalent to the UI's.

## Why a proposal is refused

"Construction not possible" is raised from four sites, three of them silent:

| site | condition |
|---|---|
| `CreateProposalDataImpl` `0xa07ab0`, segment loop | an added segment the engine matched to an existing entity (`nodeLookup` `0xaa7630`, a map at ctx+0x10) is not in the removal list |
| same, node loop | the same rule for an added node, keyed on its id at +0x14 |
| same, CreateShapes | shape/model generation failed; prints `CreateShapes failed` |
| `street_util::CheckGraph` `0x4a4490` | one of its pre-checks, below |

`CheckGraph` runs, in order:

1. Octree pre-check `0x21e2330`: two added nodes at one position print
   `Can not build due to duplicate base nodes: ...`; then a +-0.01 m box around each added
   node is queried (`0x21dab70`) and a hit fails silently.
2. Graph pre-check `0x21e31e0`: a segment with `node0 == node1`, or endpoints closer than
   0.1 m. There is no other minimum length.
3. Per-edge geometry: "Too much curvature", "Too much slope", "Narrow angle", and the
   one-way stop/waypoint rules.

So a silent refusal almost always means something the proposal adds already exists and
was not removed.

## Level crossings

- A crossing is its own entity with a `RailroadCrossing` component (Lua
  `RAILROAD_CROSSING`): `nodes`, `edges`, `typeIndex`. It is created from an explicit
  list (`street_util::RailroadCrossingProposalData`, 0x90-byte entries) during
  CreateProposalData, never inferred afterwards. [DECOMPILED + live world read]
- Native shape: the road is cut into half, connector, half; the connector spans the track
  footprint (length ~ footprint width / sin(angle)) and the track is cut at both of its
  ends. Every crossing node has exactly 2 street and 2 track edges, all `type=0`,
  `typeIndex=-1`. A near-perpendicular crossing collapses to one node with no connector.
  Track crossing a road at one shared node where the two road halves meet collides.
  [MEASURED on natively built crossings]
- Pipeline: `CreateProposalDataImpl` `0xa07ab0` → visit loop `0x21b56c0` (both
  endpoints of every added segment) → `VisitNodeCrossing` `0x21b9b60` → recorder
  `0x21fe3e0` / `0x21fdf40`. Applied by `AddRailroadCrossings` `0x21acc60` (placeholders
  mapped through old2newIds); `FinishRailroadCrossings` `0x21e94a0` (from `0x21a2c50`)
  fills each entry's type index, self-seeded by the recorder from the era/region crossing
  resources in `res/config/railroad_crossing/`. `api.cmd.make.buildProposal` goes
  through the same pipeline.
- The visitor records at a node where the other kind forms a straight, homogeneous
  two-edge run: the per-edge filter `0x21b0700` requires exactly two incident edges that
  pass `edgeValid` `0x21b5280` and are collinear (`0x21b5460`); the pair predicate
  `0x21b3470` requires every edge at the node to share BaseEdge type/typeIndex.
- **`Crossing.cpp:232-233`**: a four-arm crossing must be two straight lines (arm 0
  anti-parallel to arm 2, arm 1 to arm 3, within ANGLE_TOL). Routing a track through a
  road corner asserts natively; `pcall` and `ignoreErrors` cannot catch it, and every
  instance replaying the same plan crashes together. A node made by splitting is always
  straight-through; an existing node may be a corner. [MEASURED]

## Edge objects: stops, signals, waypoints

`edgeObjectsToAdd` record (0x100 B), built by `street_util::MakeEdgeObjectProposal`
`0x21ef7d0` (the stop tool) and cross-checked against six live records (stops, signals,
a waypoint):

| offset | field |
|---|---|
| +0x00 | edge entity (-1 = the edge rebuilt by this proposal) |
| +0x04 | kind: 0 street stop, 2 track object (signal or waypoint) |
| +0x08 | -1 (entity slot, unused for a fresh placement) |
| +0x10 | modelId |
| +0x14 | Mat4f transf (position at +0x44/+0x48/+0x4c) |
| +0xd0 | the commit's bool argument; provisionally `oneWay` (0 in every sample so far) |
| +0xd1 | the engine's `left` (u8) |
| +0xd8 | `std::string` name |
| +0xf8 | playerEntity |

How the engine edits edge objects (DECOMPILED: the stop tool `0x21ef7d0` and the
bulldozer's `MakeRemoveEdgeObjectsProposal` `0x21f0c60`; consistent with
`res/scripts/mission/proposalutil.lua`):

- Both rebuild the edge: remove it and re-add it as -1. The new segment's `objects`
  carries every untouched object under its **positive** id, which apply treats as
  re-parent (the entity, its station group and its lines are kept); -k means
  `edgeObjectsToAdd[k]`.
- A removed object goes into edgeObjectsToRemove; apply rewrites lines and the station
  group before it dies. Rebuilding an edge without doing this leaves lines on dead ids,
  a fatal assert.
- The type in an `objects` pair is the side: STOP_LEFT 0, STOP_RIGHT 1, SIGNAL 2. One
  object per value per edge; the same value twice is a fatal assert in CreateLanes.
- The stop tool computes `left` as cross(t, p - q) < 0. For a **track** object `left` is
  not the geometric side of its model (two signals geometrically left of their edge
  carried 0 and 1): replay the engine's byte, not geometry. [MEASURED]
- Station-group merging is not in the proposal: apply pairs an opposite-side stop of the
  same kind within 125 m, else joins any group within 200 m. The same placement merges
  the same way everywhere.
- Replacing a compatible stop on an occupied side uses two maps (+0x150/+0x160,
  old2newEdgeObjects) that a Lua SimpleProposal cannot set.
- A line stop refers to a station by index within its group; the group's order is
  edge-frame ([right, left]) and can differ between instances, so resolve by position.

## Terrain grids: terraform and paint

All DECOMPILED. Nothing here has been checked against a live proposal yet: `dumpprop` dumps
only the first 0x240 bytes of the proposal and never reached the grids.

`UI::TerrainModifier` (vftable `0x2fcfbb8`) and `UI::TerrainPainter` (`0x2fd35c8`) are
`UI::ProposalAction`s. While the button is held, each frame builds a candidate proposal: a copy
of the current one (`0x3e3270`) whose grid is replaced by the union of the old grid and this
frame's brush. `ProposalAction::Update` `0x431620` evaluates it with `CreateProposalData`
`0xa072b0` for the preview and keeps it if the evaluation passes.

- **Terraform** (update `0x46adf0`): `CalcHeightMod` `0x468770` returns the new
  `Grid<CVec2f>`. The mode picks the modifier: `TerrainRaiser` `0x467e50` (height ± strength ×
  mask), `TerrainSmoother` `0x467ec0`, `TerrainFlattener` `0x467dc0` (toward the height
  captured at the press, kept at tool+0x198), anything else `TexturedTerrainModifier`
  `0x467f70` (height ± strength × mask × (2 × texel − 1)).
- **Paint** (update `0x46dfd0`): `Paint` `0x46d880` returns `pair<Grid<unsigned char>,
  Grid<bool>>` over 1 m texels (tile × 256 + offset in the tile), and the merge `0x46cbf0`
  copies every new cell that is not 0xff. Its bool argument (tool+0x15c1) sets or clears the
  mask bits. The texel pattern comes from a noise table at a random offset drawn from the
  tool's own generator, so a paint stroke cannot be recomputed on another machine.

### Height cells

A cell is `{height, base}`. `ProposalTerrain::GetHeight` (slot 1 `0x469c50`) returns the
proposal's cell inside the grid and `{h, h}` from the terrain outside it. `CalcHeightMod` fills
the union grid from it (`0x467020`) and its per-cell pass (`0x466da0`) writes only `height`.
So `height` is the **absolute** target height and `base` is the terrain height before the
proposal. The per-cell pass clamps to the terrain's height range and leaves a cell unchanged
where:

- an existing terrain alignment pins it. Alignments are rasterized into `{lo, hi}` intervals,
  initialised to `{-FLT_MAX, FLT_MAX}`, by `0x46b7e0` (modes 0/1/2 = EQUAL, LESS, GREATER);
- raising would pass `hi`, or lowering would pass `lo`;
- lowering a cell near water would take it below water level + 1.

### Commit and apply

- Terraform commits on button release (slot 2 `0x469f40`), and also mid-stroke once the
  evaluated ProposalData's set at +0x5c0 holds more than 30 entries or the grid exceeds
  300,000 cells. The painter commits only on release.
- `ProposalAction::commit` `0x4310d0` makes the Context with `0x431560(player)` (bytes +0x00,
  +0x01, +0x08 and +0x09 zero, player at +0x14), calls `make_cmd::BuildProposal` (returns to
  `0x4311c6`), then `CommandList::Add` `0x9d2a00` (call at `0x4311e4`) with a 24-byte functor
  `{vftable 0x2fc8dd0, tool, bool}`. It sets tool+0xf0 and frees the proposal and its
  ProposalData. The terraform update applies no brush while +0xf0 is set, so a stroke waits for
  its command.
- The callback's `_Do_call` `0x431c60` clears tool+0xf0, calls `0x817f70` on a tool member,
  calls `0x3fec40` when command+0x30 is non-zero, and fires a one-shot `std::function` stored at
  tool+0xf8.
- Execution (`0x9d6e20`) recomputes the ProposalData from the proposal against the executing
  instance's world; the ProposalData the command was made with is overwritten. With a height
  grid, `CreateProposalData` takes the grid's box plus 16 m (`GetBaseHeightModBBox` `0xa0bfa0`),
  LIKELY to gather what the edit touches. `applyProposal` `0x9e76e0` writes heights through an
  `ExtendedTerrainModHeightMap` over ProposalData+0x5f0, and materials through
  `terrain_modification_util::SetTerrainMaterialIndices` `0x3c2c10` (256-texel tiles, one
  `TerrainTileBrush` entity per tile).

### What this means for replication

- `api.type.SimpleProposal` has no field for any of the three grids, so Lua cannot express a
  terraform or a paint stroke. A replay has to put the grids into a script-built proposal at the
  factory, the way `MergeTemplateStreet` edits construction proposals. The game's own vector
  copy constructors allocate with its allocator: `0x0cc990` (8-byte elements), `0x1ded10`
  (1-byte), `0x125480` (4-byte). A `Grid<bool>` also needs its bit count at +0x28.
- Heights are absolute: the same grid applied to the same world gives the same terrain on every
  instance, and nothing about the stroke has to be recomputed.
- A cancelled terraform releases the tool as soon as its callback fires, and the rest of the
  stroke is then computed against terrain that lacks the cancelled part. A stroke that commits
  mid-way loses that part wherever the two parts overlap, unless the tool is kept waiting
  (tool+0xf0) until the replay has run.
