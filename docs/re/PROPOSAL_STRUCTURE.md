# The proposal structure, from assert strings

Field names recovered from assertion text compiled into the binary, attributed
to the functions that reference them. Image base `0x140000000`; all addresses
below are RVAs.

## How this was obtained, and why not the obvious way

RTTI recovered 58,624 vftable symbols with genuine C++ type names, including
`construction_builder_util::Proposal`, `ProposalData`,
`Proposal::ConstructionEntity`, `CmdData::BuildProposal` and
`scripting::Proposal::ConstructionEntity`. But chasing the vftable of `Proposal`
returns only `boost::signals2` template instantiations that mention it in their
parameter lists — because **`Proposal` is not polymorphic**. No virtual methods
means no vftable, means RTTI cannot locate the class itself.

Function names are no help either: of 76,873 "named" functions, 69,880 are
`Unwind*` metadata and 4,926 are `Catch_All*`. A stripped release build carries
no symbols to demangle, so there is no function called `BuildProposal` to find.

Assertion strings are the seam that works. They are literal text in the binary,
they spell out field paths, and the function referencing one is by definition
code that operates on that field. This is what M1's summary means by
"assert-guided".

## Structure

Derived from the assert paths; **not yet cross-checked against the captured
bytes** in `GROUND_TRUTH_applyProposal.md`, so treat as provisional.

```
ProposalData
  .proposal                      construction_builder_util::Proposal
      .toAdd                     constructions to add
      .addedNodes
      .addedSegments
      .removedNodes
      .edgeObjectsToAdd
      .edgeObjectsToRemove
  .errorState                    .critical, .Empty()
  .entity2tn                     map: entity -> TpNetLink  (has .end(), so std::map/unordered_map)
  .journalPlayerEntity           WHICH PLAYER the change is attributed to
  .result
      .nodeLanes
      .segmentLanes
```

Related asserts seen elsewhere: `resultConstructions.empty()`,
`planConstructions.empty()`, `constructions.size() == entities...`,
`constructionEntity.GetId() >= 0`, `constructionEntity != ecs::Entity...`.

## Functions

| RVA | evidence |
|---|---|
| `9e76e0` | **applyProposal**. Asserts on `proposalData.errorState.critical`, `errorState.Empty()`, `proposalData.proposal.toAdd.size`, `it != proposalData.entity2tn.end`, `proposalData.journalPlayerEntity` — so its parameter is a `ProposalData`, not a bare `Proposal` |
| `b01bc0`, `b18aa0`, `b1b100` | `CreateProposalData` |
| `b1b100` | `SplitEdgesWithEdgeObjects` — the road/rail split path, i.e. what a junction build does |
| `a152e0` | operates on `result.proposal.addedNodes/addedSegments/edgeObjectsToAdd` and `result2.proposal.removedNodes/edgeObjectsToRemove` |
| `45a0b0` | `proposalData.result.nodeLanes`, `.segmentLanes` |
| `4a4490`, `4a4a20` | errorState checks |

## Why this matters for lockstep

1. **A proposal is a semantic diff, not an opaque blob.** It is add/remove lists
   over nodes, segments, edge objects and constructions. That is expressible as
   positions, types and params — the same vocabulary the current Lua replication
   already ships. Serialising a command therefore does not require deep-copying
   a C++ pointer graph, which was the main risk to the whole pivot.

2. **`journalPlayerEntity` means commands are already player-attributed.** The
   engine records which player a change belongs to. N-way lockstep needs exactly
   that, and it appears to exist rather than needing to be invented.

3. **`errorState.critical` is the engine's own validity gate.** The Lua side has
   been inferring rejection from side effects; this is the authoritative signal,
   and `applyProposal` asserts on it.

## Open

- Map these names onto byte offsets and check them against the live capture.
  `applyProposal`'s decompiled field accesses cluster at `0x38-0x50`,
  `0x90-0xb0`, `0xf8-0x110`, `0x148-0x160` — spacings consistent with several
  `std::vector`s (3 pointers each), which fits the add/remove list structure
  above. Unverified.
- Does `applyProposal` return a value the caller uses? Ghidra recovered no
  signature (`undefined FUN_1409e76e0(void)`), and the same is true of the
  known-good `buyVehicle_factory` control — so this is the baseline quality of
  the analysis, not a problem specific to this function. Needed before the hook
  can suppress a local command.
- `CmdData::BuildProposal` is the command-payload type. Its layout is the thing
  a lockstep transport would actually carry.


## Construction linkage -- RESOLVED (2026-08-28, Ghidra + differential dump)

How a construction in a proposal is tied to its street pieces. Sources: headless
decompiles in `C:\tools\ghidra_out\linkage\` (scripting::Convert 0x20e72f0,
MakeProposalAdd 0xa18ca0, MakeStreetProposal 0xa19ca0 -> 0x21d84c0,
UI::ConstructionBuilder::MousePressed 0x419aa0, make_cmd::BuildProposal 0x9dc750)
and the live UI-vs-Lua proposal diff (`tools/dumpprop_diff.py`, `dumpprop_vecs.py`,
hook cfg `dumpprop=1`).

`construction_builder_util::Proposal` (760 B) = `street_util::StreetProposal`
(0x188) + construction fields:

| offset | field |
|---|---|
| +0x000 | `vector<NodeAndEntity> addedNodes` (24 B: x y z, flags u32 @+0x0c, type @+0x10, id @+0x14) |
| +0x018 | `vector<SegmentAndEntity> addedSegments` (120 B: placeholder id @0, node0 @+0x08, node1 @+0x0c, t0 @+0x10, t1 @+0x1c, streetType @+0x48, flags @+0x64, **construction entity @+0x68, player @+0x70, owned @+0x74**) |
| +0x030 / +0x048 | removedNodes / removedSegments |
| +0x0e0 / +0x0f8 | edgeObjectsToRemove / edgeObjectsToAdd (0x100 B) |
| +0x170 | `vector<int>` **frozen node INDICES** (into addedNodes; the depot's inner connector node) |
| +0x188 | `unordered_set<int>` construction edge indices (likely terrainAlignSkipEdges) |
| +0x1c8 | `vector<string> segmentTags` (parallel to addedSegments) |
| +0x1e0 | `vector<int> toRemove` |
| +0x1f8 | `vector<ConstructionEntity> toAdd`, stride **0x8e0** |
| +0x210 | `unordered_map old2new` |

`Proposal::ConstructionEntity` (0x8e0): fileName/params/transf/name/player,
**`vector<int> frozenNodes` @+0x768 = INDICES into addedNodes**, **`int
segmentsBefore` @+0x780** = addedSegments.size() before the template's edges
were appended. There is NO frozenEdges in the proposal; frozenEdges is derived
at apply (0x9ee3b0) and stored on the ecs Construction component (+0x90).

**Why script placements collided.** `scripting::Convert` runs the .con template
and calls `MakeProposalAdd` with an EMPTY nodes2snap map, so `0x21d84c0` appends
the template's connector (inner node, outer node, apron segment; node flags
0x7f00; segment +0x68 = construction, +0x70 = player, +0x74 = 1) AFTER the
script's own edges, at raw template coordinates. The UI calls the same
`MakeProposalAdd` with `GetNodes2snap` (15 m radius): `0x21d84c0` welds the
connector's outer node onto an existing edge -- endpoint (t in {0,1}) or, for
0 < t < 1, `0x21d8d80` = RemoveSegment(old) + AddSegment x2 -- which is exactly
the "apron + two halves + removed edge" shape the hook captures. Placeholder
ids are allocated as (min existing placeholder) - 1, descending, so regenerated
pieces land below the script's own; a script base of -100001 pushed them to
-100004.. and the apply asserted (`create_proposal_data.cpp:897`,
`it != result.result.boundingVolumes.end()`).

**Consequence for replay (merge v3, slice_hook.cpp `MergeTemplateStreet`):**
never compact or reorder addedNodes/addedSegments -- every linkage is an index.
Ship only the split node + halves + removal from Lua; at the factory's entry
re-point the template connector's outer end onto the split node, recompute its
straight tangents, set our node flags to 0x7f00, and drop the template's outer
node, which is the LAST record so no index shifts.

### Split-half records and naming (2026-08-28, live diff of the same depot UI vs script)

In the UI's proposal a split half is the ORIGINAL edge's 120-byte record with
new endpoints and tangents: +0x48 street type of the road (16 for the town
road, while the construction's apron is 29), +0x4c = 0x200, +0x2c = -1, +0x64
= 0x7f00, +0x6c = the same value the removed record carries. The UI orders the
apron FIRST (index 0, `segmentsBefore` = 0) and the halves after; the script
path appends the template apron last -- both orders apply.

`ConstructionEntity.name` must be SET in a script proposal: the apply uses it
to give the construction and its child entities (VEHICLE_DEPOT, stations) their
NAME and PLAYER_OWNED components (probe P9, 2026-08-28). A child without them
crashes the client when selected. (A run that blamed the name for
`Construction not possible` was actually failing on the halves' street type.)
