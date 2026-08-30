# Command factory arguments — what to capture per action type

Signatures are exact, recovered from `__FUNCSIG__` strings (`funcsig.csv`), not
inferred. All return `struct Command` by value, so **`rcx` is the hidden return
pointer** and the real arguments shift: `rdx`=arg1, `r8`=arg2, `r9`=arg3,
stack=arg4+.

| factory | rva | signature |
|---|---|---|
| `BuyVehicle` | `9dca00` | `(const ecs::Engine&, ecs::Entity, ecs::Entity, TransportVehicleConfig)` |
| `SellVehicle` | `9de380` | `(const ecs::Engine&, const std::vector<ecs::Entity>&)` |
| `SetLine` | `9dea10` | `(const ecs::Engine&, ecs::Entity, ecs::Entity, int)` |
| `UpdateLine` | `9df4e0` | `(const ecs::Engine&, ecs::Entity, ecs::component::Line)` |
| `CreateLine` | `9dcde0` | `(std::string, CVec3f, ecs::Entity, ...)` |
| `BuildProposal` | `9dc750` | no signature string; args established empirically (below) |

## Register map per action

```
BuyVehicle    rdx = &Engine      r8 = Entity (VALUE)   r9 = Entity (VALUE)   stack = TransportVehicleConfig
SellVehicle   rdx = &Engine      r8 = &vector<Entity>
SetLine       rdx = &Engine      r8 = Entity (VALUE)   r9 = Entity (VALUE)   stack = int
UpdateLine    rdx = &Engine      r8 = Entity (VALUE)   r9 = component::Line
CreateLine    rdx = &std::string r8 = &CVec3f (COLOUR) r9 = Entity (VALUE)
BuildProposal rdx = heap ctx     r8 = &struct whose +0x00 is vector<Vec3f> nodes (24B stride)
                                 r9 = &second struct
```

**`ecs::Entity` arguments are VALUES, not pointers.** A probe that only dumps
memory logs nothing for them and looks like the hook did not fire — that is
exactly what happened to `BuyVehicle` here. Read `r8`/`r9` as integers.

## Why this is good news

Every non-construction command is **flat, small data**:

- `SetLine` = two entity ids and an int. That is the vehicle→line assignment
  that never replicated under the old state-diff design.
- `SellVehicle` = a vector of entity ids.
- `BuyVehicle` = two entity ids plus a config struct.
- `CreateLine` = a name string, a colour, an entity id.

No object graphs, no pointer chasing. These are directly serialisable once entity
ids can be mapped across peers — and the lockstep prototype already produced
**identical entity ids on both instances** for the same command (`id=281697`,
`M9_LOCKSTEP_PROTOTYPE.md`), which suggests ids are deterministic under an
identical command history and may need no translation at all. Worth verifying
before relying on it.

## Live capture confirming the hooks

Nine factories hooked simultaneously, all installed, all fired correctly:

```
#1 BuildProposal caller=4311c6   terraform (UI::ProposalAction 0x4310d0)
#2 BuyVehicle    caller=74fd88
```

**Caller sites for BuyVehicle, settled 2026-08-28:** `74fd88` = the UI's buy
(inside the `vehiclemanager.cpp` function at `74f5e0`, ACTION_MAP); `ceefae` =
the sol2 wrapper (`api.cmd.make.buyVehicle`, scripting layer, next to `cee710`
SetVehicleManualDeparture and `ced378` buildProposal). The VBUY replication
filter must suppress `ceefae` (our own replay on the peer) and ship `74fd88`;
the first version had them swapped and silently dropped the player's purchase.

```
#3 CreateLine    caller=215c26b
#4-9 UpdateLine  caller=6043fd / 6074b5   (adding stops to the new line)
```

Note `UpdateLine` fires **once per stop operation**, so a line with several stops
produces several commands — each must be replicated, not just the final state.

Construction actions all produce `BuildProposal`, so they are separated by
`caller_rva`: road from `459e97` (`StreetBuilder`), depot/station from
`ConstructionBuilder` ~`419aa0`, demolish from `Bulldozer` `3eaeb0`, terraform
via `4310d0`.

## Construction actions — captured and decoded

All four produce `BuildProposal`; the caller separates them, exactly as
predicted from `ACTION_MAP.md`.

| action | caller | `a2+0x00` | decode |
|---|---|---|---|
| road | `459e97` (`StreetBuilder`) | 72 B = **3 × Vec3f** | `(943.0,-13310.1,85.4) (989.9,-13337.8,79.0) (1036.9,-13365.6,72.6)` — SOLVED |
| depot | `419f62` (`ConstructionBuilder`) | 48 B = **2 × Vec3f** | `(1116.7,-13199.2,79.4) (1116.7,-13219.2,79.4)` — the access-road stub, 20 m apart, same x and z |
| station | `419f62` (`ConstructionBuilder`) | 600 B = 25 × 24 B | **ambiguous, see below** |
| demolish | `3eb227` (`Bulldozer`) | 600 B at `a2+0x30` | **ambiguous, see below** |

Road independently reconfirms the earlier finding, and `a2+0x18` carries the
tangents — its `(…, 47.0, -27.7)` matches the node-to-node delta exactly.

### `a2` and `a3` are ONE object, not two arguments

`a2 - a3 == 0x70` in every construction capture, and the bytes confirm it:
`a3+0x70` is byte-identical to `a2+0x00`. So the factory is being handed one
proposal structure, and the decompiler's `a2`/`a3` are two views into it. All
offsets below are given relative to **`a3`**, the true base.

| `a3` offset | contents |
|---|---|
| `+0x00` | header: flags, `float 1.0` |
| `+0x30` | vector of 128-byte street-type descriptors (materials) |
| `+0x70` | **vector of nodes** (`= a2+0x00`) |
| `+0x88` | **vector of edges** (`= a2+0x18`) |
| `+0xe8` | 128-byte block, contents are one pointer repeated 16× — pool/reserve, not data |

### Node record — 24 bytes, SOLVED

```c
struct ProposalNode {   // stride 24
    float x, y, z;      // +0x00  world position
    uint32 flags;       // +0x0c  0x00007f00 in every sample
    int32  type;        // +0x10  2 in every sample
    int32  id;          // +0x14  placeholder: -1, -2, -3, ... sequential
};
```

The `id` field is the key discovery. It is a **sequential negative counter**, and
that is what a proposal uses for entities that do not exist yet — the same
convention the Lua API uses in `streetProposal.nodesToAdd`.

### Edge record — 120 bytes, SOLVED

The depot build produced exactly one edge, and it validates the node decode by
pointing back at it:

```c
struct ProposalEdge {   // stride 120
    int32 id;           // +0x00  -3   (continues the same negative counter)
    int32 _pad;         // +0x04
    int32 node0;        // +0x08  -1   <-- node record with id -1
    int32 node1;        // +0x0c  -2   <-- node record with id -2
    float tangent0[3];  // +0x10  (0, -20, 0)
    float tangent1[3];  // +0x1c  (0, -20, 0)
    ...                 // +0x28..+0x5f  mostly zero
    int32 entA;         // +0x68  314485    plausible entity id
    int32 entB;         // +0x70  171839    plausible entity id
};
```

The depot's two nodes are 20 m apart on `y` at equal `x`/`z`, and the tangents
read `(0, -20, 0)` — **the tangent matches the node-to-node delta exactly**, so
the node vector and edge vector are consistent with each other. Two independent
records agreeing is what makes this a decode rather than a guess.

### The "terrain samples" reading was wrong

The station's 600-byte vector was previously written off as ground-levelling
samples because all 25 points share a `y` and `z`. They are not samples: they are
25 nodes with ids `-1 … -25`, and the matching edge vector holds **24** edges
(2880 bytes) — 25 nodes and 24 edges is a connected chain, exactly what a
re-tessellated street alignment looks like. The constant `y`/`z` means the
affected road runs straight along `x`, not that the data is synthetic.

### Method note — the same failure four times

The station's edge vector was invisible because the vector detector rejected any
`span > 0x800`, and the station's is 2880. The depot's 120-byte equivalent sailed
through, which is why the structure looked inconsistent between the two.

That is the fourth time this session a limit chosen for tidiness has hidden the
answer: `nv=0` (a non-callable query), the `|v| > 1.0` float filter, the
`span <= 240` Vec3f cap, and now `span <= 0x800`. The fix is now a rule in the
probe: **truncate the output, never the detection.**

### The construction half — FOUND at `a3+0x1e0 … +0x368`

Widening the scan from `+0x170` to `+0x400` and chasing strings from *inside*
vector elements produced the station's identity immediately:

```
STR  a3+268[000] -> "station/rail/modular_station/modular_station.con"
STR  a3+268[050] -> "Modular train station for cargo and passengers."
STR  a3+268[070] -> "ui/construction/station/rail/modular_station/modular_station.tga"
PATH a3+3a8+010 -> "res/models/model/station/rail/era_c/station_3_main_end_l.mdl"
```

| `a3` offset | span | contents |
|---|---|---|
| `+0x1e0` | 92 | 23 × `int32`: `0, 2, 3, 4, … 23` — slot ids |
| `+0x238` | 768 | 24 × 32-byte `std::string` (size 16, cap 31) — **module names, not yet read** |
| `+0x268` | 2272 | construction record: `fileName` at `+0x00`, description `+0x50`, UI icon `+0x70` |
| `+0x368` | 12 | **`Vec3f` placement position** `(249.5595, -8064.795, 28.3617)` |

The position is corroborated: its `z` of `28.3617` matches the street nodes'
`28.362` exactly, so the construction half and the street half agree about where
the player clicked.

Two earlier readings were wrong and are corrected here. The float `-0.0009` next
to the position is **not** a rotation — it is the low 32 bits of the pointer at
`a3+0x378` reinterpreted as a float. And the `asphalt_01.gtex.lua` /
`street_border.lua` hits resolved from a vector's `end`/`capacity` pointers, so
they belong to an adjacent pool block, not to this proposal.

### What replication needs, and what is left

Rebuilding this in Lua needs three things:

```lua
n.fileName = "station/rail/modular_station/modular_station.con"  -- FOUND
n.params   = { … }                                                -- module list, PARTIAL
n.transf   = api.type.Mat4f.new(…)                                -- position FOUND, rotation not
```

### Module names — CORRECTED: they are per-edge tags, not param keys

Reading `a3+0x238` as `vector<std::string>` gives 24 entries, four distinct
values repeated six times each:

```
__module_8400990   __module_8401000   __module_8401010   __module_8401020
```

The earlier reading of these as *param keys* whose *values* were still to be
found was a single-sample inference of exactly the kind the method rule
forbids, and it is wrong. Three independent sources agree on what they are:

- `res/scripts/.../base_config.lua:71` builds `tag = "__module_" .. slotId`
  and the exe has `modules_util::Tag2SlotId` (RVA `0x38e4368`) to parse it back
- the counts line up exactly: **24 tags == 24 edges** (`a3+0x88`, span 2880 =
  24 × 120) == 4 track modules × the 6 segments `trainstationutil.makeTrack`
  emits per module, with 25 nodes (span 600) after shared-end deduplication
- the slot ids decode via `modular_station.con`'s `GetId`: track slot =
  `8400000 + 1000*i + 10*j`, so these are column `i=1`, rows `j=-1..2` — the
  four `platform_track_catenary.module` entries of a 160 m, 1-track station

So the string vector is **parallel to the edge vector**: it tags each generated
track edge with the module that emitted it. There are no values to locate.

**The `params.modules` map is not in the dumped proposal bytes at all.** It is a
native `map<int, ModuleInfo>` reachable only by pointer; the M8 apply-time probe
found `platform_cargo_era_a.module` three hops down (`ProposalData
+0xe8+0x40+0x38`). Whether it is reachable from the `make_cmd` struct `a3` is
what the construction ground-truth sweep tests.

Also corrected: the proposal's node/edge vectors for a rail station are **track**
edges (`+0x48 = 1`, `+0x4c = -1`, `+0x60 = trackType`), not street nodes as
earlier text said; and the "2 m module pitch" is `makeTrack`'s end segment —
module pitch is 40 m.

### There is no transform in the proposal — settled negative

Scanning all `0x800` bytes of `a3`, plus the full contents of every vector found
inside it, for a 4×4 with an orthonormal 3×3 and a `(x, y, z, 1.0)` translation
row returns **nothing**, across placements at 0° and −90°. The proposal does not
carry a `Mat4f`.

A full diff of the two placements shows only one orientation-sensitive field,
`a3+0x7e0` (`3` vs `2`) — a quadrant index at best, far too coarse to rebuild a
transform from.

**Rotation is instead baked into the node geometry**, which arrives already
transformed into world space. Five placements at different angles make this
unambiguous:

| placement | node0 → node1 delta | angle |
|---|---|---|
| axis-aligned | `(0, −2)` | −90° |
| axis-aligned | `(−2, 0)` | 180° |
| rotated | `(1.111, −1.663)` | −56.26° |

The delta magnitude is exactly `2.0` in every case — the module pitch — so only
the direction changes. That makes the transform recoverable without the matrix:

```
forward = normalise(node1 - node0)
up      = (0, 0, 1)
right   = cross(forward, up)
translation = placement position (a3+0x368)
```

### False positive worth recording

A float of `-0.83147` — exactly `sin(-56.26°)` for the rotated placement — turned
up at `a3+0x238[+0x08]` and looked like the rotation matrix. It is **allocator
debris**. Each 32-byte element is `{ char* ptr; <8 junk bytes>; size=16; cap=31 }`
and the value sits in the uninitialised half of the string union, where a
rotation matrix had recently lived. The layout is what disproved it; the value
alone was entirely convincing. Matching a *predicted* constant is not evidence
unless the field it sits in is also the right shape.

## Edge record — type fields

Decoded by diffing captures of two roads of different types against two
railways. Offsets are within the 120-byte edge record (`a2+0x18` vector).

| offset | road A | road B | rail 1 | rail 2 | meaning |
|---|---|---|---|---|---|
| `+0x00` | -4 | -4 | -4 | -3 | edge placeholder id (follows the node counter) |
| `+0x04` | junk | junk | `2` | `-48` | **NOT a type** — see below |
| `+0x48` | 0 | 0 | 1 | 1 | **edge type: 0 = street, 1 = track** |
| `+0x4c` | `25` | `22` | -1 | -1 | **street type index**, -1 on a track |
| `+0x60` | -1 | -1 | `1` | `1` | **track type index**, -1 on a street |

### `+0x04` looked like the track type and was not

The first railway captured showed `+0x04 == 2`, which read convincingly as a
track type index. A second railway of the same type showed `-48`. The slot is
uninitialised on both streets and tracks; the `2` was coincidence.

This is the third single-sample inference to fail today, after the `-0.83147`
rotation matrix that turned out to be allocator debris. The pattern is now
explicit: **a value that matches expectation in one sample is not evidence.**
`+0x48`, `+0x4c` and `+0x60` are trusted because they were separated by a diff
across builds that differ in exactly one property.

### Ground-truth sweep — settled (2026-08-17)

Driving known values through `api.cmd.make.buildProposal` (factory only, no
`sendCommand`, so the world is never touched) and correlating with
`tools/gt_correlate.ps1`:

```
trackType  0..7,  catenary off:  +0x60 *** EXACT MATCH ***  0 1 2 3 4 5 6 7
trackType  0..7,  catenary ON:   +0x60 *** EXACT MATCH ***  0 1 2 3 4 5 6 7
streetType 0..39:                +0x4c *** EXACT MATCH ***  0 1 2 ... 39
cross-test off vs on:            +0x64 DIFFERS IN EVERY PAIR (low byte 00 -> 01)
```

| offset | field | evidence |
|---|---|---|
| `+0x4c` | `streetType` | tracks the input across 40 values |
| `+0x60` | `trackType` | tracks the input across 8 values, both catenary states |
| `+0x64` low byte | `catenary` | `01` on every on-sample, `00` on every off-sample, 8 pairs |

`+0x64` reads as chaotic dwords because only the low byte is the flag; the upper
three bytes carry unrelated data. 56 samples, one second, no restart, no human
-- against two hand-built railways of the same type for the previous reading.

### Verdict on constructions

| action | status |
|---|---|
| road | geometry fully decoded — replicable |
| depot | geometry fully decoded — replicable |
| station identity + position | decoded |
| station rotation | derivable from node geometry, no matrix needed |
| station module params | **partial** — keys read, values not |

Roads and depots are ready to wire. Stations need the param values, but that is
now the only gap, and it does not block building the end-to-end path.

## Still to capture

Road, depot, station and demolish were missed in the session above (the probe was
injected after those actions). Only terraform was caught, and it has **no node
vector** — unlike a road — which is consistent with terraform not adding nodes.

Next capture should cover: depot, station, station module, demolish — all through
the same `BuildProposal` hook, distinguished by caller.

## Vehicle + line layouts — ground-truth sweep (2026-08-17)

Factory-only sweeps (no sendCommand), correlated by `gt_correlate.ps1`. Every
offset below is an EXACT MATCH: the swept value appeared at that offset in all 8
samples. NaN "matches" from the float scan were correlator false positives
(fixed: the exact-float test now rejects non-finite values).

### TransportVehiclePart (unit record inside BuyVehicle's config)

| offset | field | evidence |
|---|---|---|
| +0x00 | modelId (int) | structurally pinned (recon decompile); sweep t30 not isolated because model choice is constrained |
| +0x08 | loadConfig `vector<int>` | **t31 EXACT** at the vector's [0] |
| +0x20 | color `CVec3f` (3 floats) | **t36 EXACT at +0x20/+0x24/+0x28**, seen in both the unit dump (f2u0_) and the config arg (f2s3_) |
| +0x60 | autoLoadConfig `vector<int>` | recon decompile (DECOMPILED) |

`TransportVehicleConfig` itself = `{ vector<TransportVehiclePart> @+0x00,
vector<int> vehicleGroups @+0x18 }`, 0x30 bytes; **vehicleGroups[0] t35 EXACT**.

### ecs::component::Line stop record (UpdateLine)

The recon could not order these from the static decompile (declaration order said
one thing, an assert string another). The sweep is definitive:

| offset | field | evidence |
|---|---|---|
| +0x04 | **station** (Entity) | **t13 EXACT** |
| +0x08 | **terminal** (int) | **t12 EXACT** |
| +0x2c | a wait field (float) | **t15 EXACT** (min or maxWait) |

Stops are a `vector` at Line+0x18 (t10 stop-count varied the vector span);
alternativeTerminals is a sub-vector whose count tracked t16 (`f8s0alt_ +0x00`).
CreateLine: colour `CVec3f` at r8 (+0x00 = colour.x, t18 EXACT), name std::string
at rdx.

### Status

Vehicles and lines are now DECODED. Remaining before replication: a live readback
(does the sol2 callback expose `res.resultVehicleEntity`; does `getDepotVehicles`
order match across peers) which is not a factory-only sweep -- it needs one real
buy against a real depot. That gates the cross-peer identity scheme, not the
decode.
