# Why a street/construction proposal is refused (build 35924)

RVAs are image-base 0x140000000. Decompiles in `C:\tools\ghidra_out\decomp\road_*.c`
(targets: `tools/ghidra_targets_road1.txt`, `road2.txt`).

Written 2026-08-30 while chasing depot replays that the peer refused with
`critical=true` and an EMPTY message list. Every threshold guessed from geometry
(stub length, nudging) was wrong; this is the engine's actual rule set.

## Where "Construction not possible" comes from

The string is referenced by 8 functions; the two that matter on the build path are
`CreateProposalDataImpl 0xa07ab0` and `street_util::CheckGraph 0x4a4490`
(`game\ui\actions\street_builder_util.cpp`). Four distinct sites can raise it:

| # | Site | Condition | Distinguishable? |
|---|---|---|---|
| 1 | CreateProposalDataImpl ~`0xa07e09` | for each ADDED SEGMENT: `nodeLookup(ctx+0xC0, seg)` returns an id != -1 that is NOT in the proposal's removal id vector (`ProposalData+0x1E0`) | silent |
| 2 | CreateProposalDataImpl (nodes loop) | same rule for each ADDED NODE, keyed on `node+0x14` (its placeholder id) | silent |
| 3 | CreateProposalDataImpl (CreateShapes) | shape/model generation failed | prints `CreateShapes failed` to stdout |
| 4 | CheckGraph | one of the two pre-checks below | silent |

`nodeLookup` (`0xaa7630`, decompile `xing_nodeLookup.c`) is a plain `std::map`
lookup on `ctx+0x10`: placeholder id -> existing entity id, or -1. Sites 1 and 2
therefore mean: **an added element the engine has already matched to an existing
entity must also appear in the removal list.**

## CheckGraph (0x4a4490) in order

```
CheckGraph(streetTypeRep, trackTypeRep, engine, octree, proposal, entities, graph, errorState)
  1. octree pre-check   FUN_1421e2330(engine, octree, proposal)   -> "Construction not possible"
  2. graph pre-check    FUN_1421e31e0(proposal, graphCopy)        -> "Construction not possible"
  3. per-edge geometry: "Too much curvature" / "Too much slope" / "Narrow angle"
     (plus one-way stop/waypoint rules)
```

**Graph pre-check** (`road_precheck_graph.c`) is short and exact -- it rejects only:
- a segment with `node0 == node1` (self loop), or
- a segment whose two node positions are **< 0.1 m** apart.

There is NO minimum stub/segment length beyond 0.1 m. The 6 m and 12 m thresholds
tried on 2026-08-30 were invented; the engine does not have them, which is why
raising them never helped.

**Octree pre-check** (`road_precheck_octree.c`) has two passes:
- duplicate ADDED nodes: positions are hashed; two added nodes at the same position
  print `Can not build due to duplicate base nodes: addedNodes <a> and <b> are both at
  position <p>` and fail. (Greppable in the game's stdout.)
- per added node: an AABB of **+-0.01 m** around it is queried against the octree
  (`FUN_1421dab70`); if the callback raises its flag the proposal fails **silently**.

## What this rules out for the depot-replay failures

Observed on the peer: no `CreateShapes failed`, no `duplicate base nodes` in stdout,
and no curvature/slope/angle message -- so the failure is site 1, 2, or the octree
per-node query. All three are the same shape of problem: **something we ADD is
already there and we did not remove it**, not bad geometry.

## Next step (not done yet)

Decompile the octree callback behind `FUN_1421dab70` and find where `ctx+0x10`
(the placeholder->entity map) is populated during a script-built proposal. That
says exactly which existing entity our split node/segment is colliding with, and
whether the fix is to reuse that entity or to add it to the removal list.
