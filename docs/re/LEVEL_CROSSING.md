# Level crossings (rail over road) — how the engine creates them

RVAs are image-base 0x140000000. Decompiles in `C:\tools\ghidra_out\decomp\xing_*.c`.
Status 2026-08-29: mechanism mapped end-to-end; last two decompiles pending
(`xing_toolkit_finish_caller` = 0x21a2c50, `xing_recordCrossing_impl` = 0x21fdf40).

## The model (why geometry alone never works)

A crossing is an explicit ECS component `ecs::component::RailroadCrossing`
(Lua `ComponentType.RAILROAD_CROSSING`). It is created from an explicit list,
`street_util::RailroadCrossingProposalData` (vector of `Data`, stride 0x90),
that rides BESIDE the `StreetProposal` through the whole apply pipeline. It is
never inferred from a shared rail/road node after the fact.

Pipeline (Lua `buildProposal` DOES go through all of it —
`apply_command_BuildProposalVisitor.c:43` calls CreateProposalData):

    CreateProposalDataImpl 0xa07ab0 (create_proposal_data.cpp)
      -> visit loop 0x21b56c0 (construction_util_graph.cpp)
           for each ADDED SEGMENT (stride 0x78, node ids @+0x08/+0x0c):
             VisitNodeCrossing(node0, 100.0f); VisitNodeCrossing(node1, 100.0f)
      -> VisitNodeCrossing 0x21b9b60
      -> recorder 0x21fe3e0 -> impl 0x21fdf40      (writes a Data entry)
    ApplyProposal 0x9e76e0 (scratch list at local_1ea8[0x2f].spare)
      -> toolkit thunk 0x21a3530 -> UpdateEngine 0x21adc50
      -> AddRailroadCrossings 0x21acc60: per Data, maps placeholder node ids via
         old2newIds (asserts "it != old2newIds.end() && GetId() >= 0"), then
         AddComponent<RailroadCrossing> using Data +0x30 / +0x34 / +0x38
    FinishRailroadCrossings 0x21e94a0 (caller 0x21a2c50, streettoolkit.cpp):
         Data[i]+0x30 = typeVector[i]  -- the CROSSING TYPE INDEX, from a separate
         vector<int> whose length must equal the entry count (assert @ line 61).
         Type = RailroadCrossingType resource (res/config/railroad_crossing/
         era_{a,b,c}_{eu,ru,us}_crossing.lua, half_barrier.lua; era/region file
         filter in base_mod.lua ~811-835; picked by the railroadcrossing-menu popup).

## VisitNodeCrossing's gate (the part I had backwards at first)

    edges = collectEdges(graph, node)   -- 0x21b6490: incident edges via the
                                        --  IStreetGraph vtable, keyed by heading
    n = count
    if n < 2: return                    -- lone/dead-end node: nothing
    if n == 2 && pairTest(e0,e1)        -- 0x21b5460: both have a street record
                                        --  AND normalized directions are collinear
                                        --  (dot > threshold)
            && setTest(edges):          -- 0x21b39e0: every edge same type/typeIndex
        return                          -- plain straight-through of ONE kind: skip
    else:
        [RECORD BLOCK]                  -- >2 edges, or mixed kinds => crossing
        groups edges, computes 25 m extents along each pair (edgeAngle 0x21b63d0
        returns the type's WIDTH: w[1]*2+w[0]), and calls the recorder per edge.

So the recording block runs for exactly the node shape our replay produces
(2 rail edges + 2 road halves = 4, heterogeneous). The gate is NOT what stops us.

## Current best hypothesis for "still no crossing" (REVISED)

~~The type vector.~~ WRONG: 0x21a2c50 builds the type vector FROM the entries
(`if Data+0x30 >= 0: typeVec[i] = Data+0x30`) -- the type is self-seeded by the
recorder; the popup only overrides a default. The pipeline is self-sufficient.

The real last gate is the RECORDER: 0x21fe3e0 -> impl 0x21fdf40 dispatches
through THREE callback objects wired by the record block (each a std::function,
asserted non-null, invoked via vftable+0x10 = _Do_call):
  bool(IStreetGraph*, Entity const&, Entity const&)  lambda_a03e5a4c...  -- edge-PAIR predicate (the crossing test)
  bool(IStreetGraph*, Entity const&)                 lambda_0b760da6...  -- per-edge filter (other kind?)
  void(Entity const&, bool)                          lambda_532ab488...  -- the sink that records
An entry is emitted only when those return true. Their bodies are anonymous
(no __FUNCSIG__), so tools/ghidra_scripts/ResolveLambdaVtables.java resolves
the vftable symbols -> _Do_call and decompiles them.

If confirmed, the fix is NOT more geometry: it is supplying a crossing type —
either (a) a Lua-reachable field on the proposal/context (the in-game
`XING-API` probe dumps SimpleProposal/streetProposal fields + any `api.type`
*cross* names to test this), or (b) a native hook that fills the toolkit's type
vector (default = the era/region crossing) before FinishRailroadCrossings.

## Ground truth about crossings on the wire (from captures)

- The build tool SPLITS THE RAIL at the crossing: a crossing is always a rail
  VERTEX on the road (every XING hit at rail u=0.00/1.00), never mid-segment.
- The road's split halves ride as `ROADE` and are deliberately DROPPED by the
  capture; peers regenerate splits from positions. `ROADP` carries only
  pts/links/tans/stype/ttype/cat — no crossing info; the engine derives it.
- "Even on A" is the same replay: strict-lockstep cancels the native build
  (`captured=1 cancelled=1`) and replays through execPolyline on A too.

## Replay-side work already in lockstep.lua (execPolyline)

- Analytic crossing finder (road Hermite sampled every 0.5 m vs rail every 1 m,
  band 2.5 m) with on-disk `XING:` trace per candidate street edge.
- Mid-edge case: split the road (road-typed halves) and route the rail through
  the new node as two edges. Node case: route through the existing road node.
- Rail-vertex case in resolve(): snap a rail vertex to a street node, else split
  the street edge underfoot.
- Probes: `XING-API` (Lua proposal surface) and post-build `RAILROAD_CROSSING`
  on each routed node. Both write to BASE/mp_company_<inst>.log.

## The last gate, fully read (2026-08-29, late)

The recorder's three callbacks (vftable layout in this build: +0x8 _Copy,
+0x10 _Move, +0x18 _Do_call, +0x20 _Target_type, +0x28 _Delete_this):

- pair predicate (lambda_a03e, stateless) -> `0x21b3470`: walks ALL edges at
  the node (`0x452130` = graph vtable+0x40 = `GetNodeSegmentsNew(node, out)`,
  NO exclude arg) and returns true only if every edge has the same BaseEdge
  `type`/`typeIndex` (+0x20/+0x24 -- confirmed: the Lua BaseEdge fields are
  node0,node1,tangent0,tangent1,type,typeIndex,objects) as the reference edge.
- per-edge filter (lambda_0b76; captures group idx, kind flag, node kind) ->
  `0x21b0700`: requires the node to have EXACTLY 2 incident edges, both passing
  `0x21b5280` (edgeValid: each endpoint must map to a NON-negative id in the
  `+0xc0` lookup, i.e. an EXISTING world node -- placeholders fail) and the
  collinearity `pairTest 0x21b5460`.
- sink (lambda_532a; captures graph + current group vector) -> appends the edge
  to a DIRECTED CHAIN with `bool = (this.node0 == prev.node0)` (orientation).

So the visitor recognises a crossing at a node where the OTHER kind forms a
straight, homogeneous, 2-edge run between EXISTING nodes. The count is over ALL
incident edges. Our replay's crossing node (mid-edge case) carries 2 rail edges
+ 2 road halves = 4 -> `count==2` fails; and the road halves end on the fresh
placeholder `mid` node -> `edgeValid` fails too. Native geometry (rail vertex on
the road) has the same shape, so the difference must be WHICH edges the
visitor's graph exposes -- pending: `ProposalStreetGraph::GetNodeSegmentsNew`
(0xa1d130) vs `EngineStreetGraph` (0xa1c870), and the graph builder
`0x21a3390` that CreateProposalDataImpl:592 builds from the proposal.

**Polarity correction (edgeValid 0x21b5280):** it looks up each ENDPOINT of the
edge (GetSegment -> node0, node1) in the toolkit's `+0xc0` map and returns 0 if
the result is >= 0. The per-edge filter (0x21b0700) requires BOTH edges to pass
edgeValid (and count==2, collinear) to declare "plain straight-through" -- so a
node whose edges end on entities that ARE in that map (result >= 0) is NOT a
plain run, and the record block treats it as a crossing candidate. I.e. failing
edgeValid is the path TOWARD recording, not away from it. Which nodes populate
`+0xc0` (0xa1cb30 builds the ProposalStreetGraph from the WHOLE proposal via
toolkit+0x98, +0xb0, +0xc0) decides it -- pending decompile of 0xa1cb30 and the
lookup 0xaa7630.

## GROUND TRUTH from the live world (EVAL probe, 2026-08-29)

29 native level crossings in the MPTESTINGII save. Each is its OWN ENTITY
(AddEntity -> AddComponent RailroadCrossing + ModelInstanceList + BoundingVolume),
Lua `game.interface.getEntities({...},{type="RAILROAD_CROSSING"})` lists them.
Component fields: `nodes=[crossingNode]` (1 or 2), `edges=[roadEdge(s)]`,
`typeIndex=0`. At every crossing node: **exactly 2 street edges + 2 track edges
= 4**, and EVERY edge (road and rail alike) has `type=0, typeIndex=-1`.

Implications: (a) our replay's 4-edge heterogeneous crossing node is the
CORRECT final topology; the visitor's count==2 is evaluated on the proposal
graph during the build (road halves present, the rail segment under visit is
the subject), not on the final world. (b) our road halves wrote
`comp.typeIndex = 0` while native road edges carry -1 -> a type/typeIndex
mismatch that breaks the "homogeneous straight run" tests; FIXED to -1.
(c) `io.open` for EVAL probes must use forward-slash paths + append mode
(doubled-backslash paths silently fail); the inject channel works every tick.

## THE ANSWER — native crossing shape (7 crossings built natively 2026-08-29, read live)

A level crossing is NOT one shared node. It is a SHORT ROAD CONNECTOR spanning
the rail: the road is cut into THREE pieces — half, connector, half — and the
rail is cut at BOTH connector endpoints. `RailroadCrossing.nodes=[A,B]`,
`edges=[connector]`, `typeIndex=0`. E.g. RC 274288: nodes 274254 & 44337 are
8.8 m apart joined by street edge 274273 (the crossing's `edges`); track edges
enter at 274254 and exit at 44337; every node is exactly 2 street + 2 track,
all `type=0 typeIndex=-1`. For a near-perpendicular crossing the span collapses
to ONE node (`nodes=[N]`, `edges=[]`, `typeIndex=3` — a different crossing type).

Why our proposals collided: a rail crossing a road at one shared point where the
road's two halves MEET overlaps road on both sides of the node -> `Collision`.
The engine avoids it by giving the rail a road piece to cross that is exactly
the rail's footprint. Connector length = footprint width / sin(angle).

Replay fix: in splitRoadAt, cut the road at the two points where its centreline
enters/exits the track footprint (width W, tolerance from the samples), add
node A, node B, three road pieces (half, connector A->B, half), and split the
rail segment at BOTH A and B (three rail pieces). For |angle| near 90 deg use the
single-node form. The visitor then sees, at A and at B, the road as a straight
homogeneous 2-edge run with the rail arriving -> records the crossing itself.

Note: the street tool does NOT go through the hooked make_cmd::BuildProposal
factory (no [cap]/DUMPPROP for native builds even ungated) — it applies via a
different entry. Reading the RESULT live via EVAL was the right ground truth.
