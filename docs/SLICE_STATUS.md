# Lockstep status — living document (last updated 2026-08-17)

Player builds a road in one instance; it is captured, cancelled locally, and
executed on **both** peers at an agreed game-time stamp.

## Working end to end

| capability | notes |
|---|---|
| capture | `make_cmd::BuildProposal 0x9dc750`, caller `0x459e97` = StreetBuilder |
| cancel without wedging the tool | fires the UI completion callback first — see `CANCEL_POINT.md` |
| execute at an agreed stamp | both peers, identical `at=` |
| road **and** rail | edge type `+0x48`, street type `+0x4c`, track type `+0x60` — all confirmed by ground-truth sweep |
| catenary | low byte of `+0x64`, confirmed by cross-test sweep; carried on the wire |
| curves | real tangents from `+0x10`/`+0x1c` are carried, not synthesised |
| elevation | node `z` carried, not re-derived from terrain |
| multi-node polylines | one proposal, shared nodes |
| **mid-span junctions** | each peer splits its OWN copy, from positions |
| money | real `Context` with `player`; builds are charged |
| latency | ~0.85s (`EXEC_DELAY = 0.6`, per-tick polling) |

## Architecture that settled

**Nothing but positions crosses the wire.** Entity ids are resolved to
coordinates by the originator, which still holds the entities; each peer then
resolves positions against its own world — `findNodeNear` → else split the edge
it lands on → else create. This came from `mp_bridge`, which had already solved
it and named the failure "the road intersection bug".

That also retired the biggest open assumption in the design: entity ids no
longer need to match across peers, so their determinism stopped mattering.

## Desync detector — id-free (rewritten 2026-08-17)

The first detector hashed vehicle positions, entity ids, and construction
counts. Every one of those differs between peers for reasons that are NOT a
desync: positions are render-interpolated (both peers sample the same sim time
on different frames), ids diverge by design now that replication is positional
(each peer allocates its own), seeds are engine-assigned per peer, and town
growth adds constructions and streets at its own pace. A detector that fires on
all of that proves nothing when it fires and hides a real divergence in the
noise — which is exactly what happened for two sessions.

Rule now: hash only what lockstep is supposed to keep identical, by GEOMETRY and
CONTENT, never by id.

| component | what is hashed | in the verdict? |
|---|---|---|
| `v` | vehicle **count** | yes — a vehicle bought on one peer only IS a desync |
| `c` | player-owned constructions: file + position + params with `seed` removed | yes |
| `e` | every edge by its two endpoint positions (0.1 m), direction-independent | yes |
| `t` | count of all other constructions (town growth, industries) | **no** — reported only |

`LSHASH` lines carry `d=v<n>,c<n>:<h>,e<n>:<h>,t<n>` so a mismatch names the
component. Node positions are cached by id (nodes do not move once placed), so
the per-edge cost is one `BASE_EDGE` read plus two cache hits.

Verified 2026-08-17: on a fresh load both peers report identical verdict hashes
across four consecutive stamps while `t` drifts (5332 -> 5334). Zero false
desyncs. Whether `t` diverging is a stale sandbox save or non-deterministic town
growth is the determinism test, deferred.

## Not implemented

- **Vehicles** (`Buy/Sell/Replace/SendToDepot`), **lines** (`Create/Update/Delete/SetLine`),
  **demolish** — factories hooked (observe-only), recon workflow running.
- **The ~17 unnamed command types** — mapping workflow running (signals and
  waypoints are expected among them).
- **Commands originating from instance B** — hook injected into the sandboxed pid
  (overlay log: `instance=b`, 10 hooks, A untouched); two-way test pending.
- Failure policy (a replay that fails on one peer diverges silently), save
  distribution at join, real networking, speed/pause on the wire, connection UI.

## Next

1. ~~Verify the id-free detector agrees on a fresh load~~ DONE: SYNC on both peers across
   four stamps (55232..55364), v79/c46/e9641 identical, t drifting 5332->5334 without firing.
2. Fold in the recon results: vehicles and lines first, gated and sweep-validated.
3. ~~Inject the hook into B~~ DONE; two-way road test pending.
4. Determinism test: same save, both idle ten minutes, compare `t` and `e`.

## Timing constants — hard-won

```
EXEC_DELAY      0.6    ~0.66s; the felt latency
BARRIER_AHEAD   5.0    LOOSE on purpose; a backstop, not a latency budget
HEARTBEAT_EVERY 5      ticks; faster floods the file relay to sandboxed B
```

Two deadlocks came from getting this wrong. The rule that matters is
`EXEC_DELAY > actual peer skew`, **not** `> BARRIER_AHEAD`; treating the barrier
as a latency budget forced the delay up, and tightening it to win the delay back
froze both games. A barrier watchdog and a stale-peer guard now bound the damage
from any residual case, and `!! LATE` measures the real skew.

The game clock is **fractional, stepping 0.2 units** (~0.22s). A single sample
at load read `55234.000000` and looked integer — it was a round value from the
save. Step size settles resolution; one reading does not.

## Ground-truth generator (added 2026-08-17)

The decode method changed from *observe a player build and infer* to *drive a
known value in and read where it lands*. `api.cmd.make.buildProposal` is the
factory call itself; without `sendCommand` it builds the Command and fires the
hook while never touching the world. So a sweep is non-destructive, needs no
restart and no human, and takes about a second.

```
echo GT track  >> lockstep_inject_a.txt     # trackType 0..7, catenary off then on
echo GT street >> lockstep_inject_a.txt     # streetType 0..39
pwsh tools\gt_correlate.ps1                 # which offsets track the input
```

Requires `groundtruth=1` in `tpf2_slice.cfg`. Samples self-identify via a
sentinel in node 0's X (`900000 + test*1000 + index`), so nothing depends on
matching by order. The correlator reports `*** EXACT MATCH ***` only when an
offset held the swept value in **every** sample; "varies" is explicitly not an
identification. That distinction is what four wrong single-sample readings this
week were missing.

First run settled `trackType`, `streetType` (40 values) and catenary in one
pass. Next target: station module params — sweep `constructionProposal` params
the same way.

## Constructions — hybrid path (decided 2026-08-17)

Strict lockstep (capture at the factory, cancel, replay the proposal) is
**not available for constructions**, and it was settled by measurement rather
than assumption:

```
GT con   SimpleProposal.new ok · ConstructionEntity.new ok · fileName ok
         params ok · transf ok (Mat4f.new(4xVec4f)) · playerEntity ok
         name ok · constructionsToAdd[1] ok
         make.buildProposal FAIL: false        <- hook never fired
```

Every API statement M5 could not preserve now succeeds, so the shape is right;
**the factory itself rejects a script-built construction proposal** with a bare
`false`.

> **SUPERSEDED 2026-08-28.** That bare `false` was a MISSING `params.seed`, not a
> policy. With the seed present, `api.cmd.make.buildProposal` accepts the
> ConstructionEntity and `sendCommand` applies it (probe E2c on instance B:
> `ok=true crit=false ent=276244`). The sweep above had "seed stripped", and
> `execConP` stripped it too. Constructions now replay NATIVELY: see CONX below. Independently, `params.modules` is a native `map<int, ModuleInfo>` that
is not in the proposal bytes at all (the `__module_<slot>` strings are per-edge
tags). Two reasons, either sufficient.

So constructions use the path `mp_bridge` measured live (16-module station
included):

| step | where |
|---|---|
| player builds station; hook does **not** cancel caller `419f62` | originator |
| `pollNewConstructions` sees the new `CONSTRUCTION` entity, reads `fileName` / `transf[16]` from `getComponent(CONSTRUCTION)` and `params` from `getEntity(id).params`, schedules `CONP` | originator |
| `execConP`: `params.seed = nil`, `buildConstruction(file, params, transf16)`, then `setPlayer` (else it is unowned and unclickable) | every peer |
| originator skips its own `CONP` (`origin == INSTANCE`); a replayed build is recognised by position (`expectedCons`) and **not echoed back** | all |
| only **player-owned** constructions are shipped (`PLAYER_OWNED.player == getPlayer()`); town growth spawns `building/era_x/*.con` CONSTRUCTION entities continuously and the first poll shipped 20 of them per side in three minutes | originator |

**Trade-off, stated plainly:** the originator applies at T0 and peers at T
(~0.7 s later). That is not strict lockstep for constructions, and construction
entity ids may differ across peers as a result. It is consistent with the
established design — nothing on the wire addresses a construction by id — but
any future command that references a station (line stops) must resolve it by
position, exactly as roads resolve nodes.

Not yet exercised live: this session's next step is one station built in A.

## Station edits — CONU (added 2026-08-17, not yet verified live)

Two shapes of edit exist, both measured by `mp_bridge`: adding a module through
the UI changes params **in place** (same entity id); `upgradeConstruction` --
what the peer uses to replay -- **replaces** the entity (old id retires, a new
one appears at the same spot). A poll that only asks "is this id new?" would
ship the replacement as a duplicate station build. So constructions are tracked
**by position** (`consByKey`):

| event on the originator | detection | ships |
|---|---|---|
| new id at an unknown position | `pollNewConstructions` | `CONP` (build) |
| new id at a known position, same file | same poll -- the entity was replaced | `CONU` (edit) if params differ |
| same id, params changed | `scanConstructionEdits` every 30 ticks | `CONU` |

Replay: `execConU` finds the peer's construction with the same `fileName` within
10 m of the wire position, strips `seed`, and calls `upgradeConstruction`. It
marks `expectedEdit[key]` first so the replacement entity it produces is not
echoed back. Pre-existing constructions from the save are classified 100 per
tick after load (`primeQueue`), so their edits replicate too.

**Known risk, measured by mp_bridge:** on a joiner, `upgradeConstruction` on a
modular station that was created by `buildConstruction` failed with "internal
error" every time while depots succeeded. If that reproduces here, `execConU`
runs a diagnostic no-op self-upgrade with the entity's own params: if THAT fails
too, the construction cannot be upgraded on the peer at all and the fix belongs
in the build replay (network integration the interactive tool performs), not in
the edit channel.

## Ground-truth test-id blocks (sentinel = 900000 + test*1000 + i)

| block | channel | owner sweep |
|---|---|---|
| 1-3 | street/track (edge record) | `GT track`, `GT street` |
| 4-6 | demolish (removal vectors) | `GT demolish` |
| 10-18 | lines | `GT line` |
| 10-12 | construction gate (retired, same ids reused only in old logs) | `GT con` |
| 30-40 | vehicles | `GT vehicle` |

Tags in `[gt]` records may use any `[a-z0-9_]`; the correlator no longer
requires hex letters. `gt_correlate.ps1 -Instance b` reads the overlay log.

## BUG (found + FIXED 2026-08-17, two-way test): 0-new-node roads did not replicate

A road connecting two EXISTING junctions adds zero new nodes (both endpoints
already exist) and one new edge referencing existing entity ids. The capture
guard `if (n < 2) return` (slice_hook.cpp road path) treats that as a failed
decode: the build is correctly NOT cancelled, so it happens locally, but it is
never replicated -> the originating peer gains one edge the other never hears
about. Observed live as e9680 (A) vs e9679 (B) after A built such a road; the
slice log shows `road capture FAILED (n=0)`.

FIX: gate capture on EDGES (`m >= 1`), not new nodes. A 0-new-node road is
`n=0, m>=1` with all edge endpoints positive (existing) ids; the ROADE->ROADP
converter already resolves existing ids to positions via realPos(), so the peer
can rebuild it. The n>=2 assumption predates ROADE (it was correct for the
original all-new-nodes ROADN format).

### Why this is a good outcome for the detector, not a bad one

The desync was PERMANENT (e9680 A vs e9679 B, stuck across every stamp, never
reconverged) and it grew (18-wide at one point, `t` drifting too). First read:
"the id-free detector is still noisy because `e` counts town streets." Corrected
read, from the evidence: both instances loaded the SAME save (MPTESTINGII) and
the FIRST hash matched byte-for-byte (`v79,c45,e9640,t5308` identical), then
stayed matched through the whole idle period. They start identical and the sim
is deterministic (M3: 59/59 over 58 days). So the divergence is CAUSED, not
inherent -- the one unreplicated junction road changed connectivity on A, and
town growth then reacted to a world B did not have, cascading into the widening
`e`/`t` gap.

Conclusion: do NOT make `e` player-only. That would blind the detector to
exactly this class of real, cascading divergence. Including town edges is
correct GIVEN a deterministic sim from an identical start; it fired because
something genuinely diverged. The earlier rule ("don't hash what legitimately
differs between peers") applies to render-interpolated positions, entity ids and
seeds -- values that differ WITHOUT a desync -- not to town growth, which should
match and did until a player edge went missing.

Open, settled by the next clean test: does the world STAY in sync after the fix
when junction-connecting roads are built? If yes, the failed road was the whole
story and town growth is deterministic under the barrier. If it still drifts
with no missed captures, pause/unpause breaks determinism -- a deeper problem.

### BUG 2 (found + FIXED + VERIFIED 2026-08-17): bridge roads dropped as split-halves

Reported: "could not connect road end to road end, only works when it's an
intersection" -- the closing edge of a triangle vanished. Log: `ROADE produced
no usable edges` on a `ROADE 1 ... m=2` (one new midpoint, two edges to two
existing road ends).

Cause: the split-half drop heuristic keyed off "a new node with >=2
positive-endpoint edges." A mid-span SPLIT gives its shared node two such edges
(the regenerated halves) -- but so does a road BRIDGING two existing road ends
through a new midpoint, with no split. Both edges were dropped, so #links < 2
and the road was silently discarded. Because suppress cancels the local build,
the road vanished on BOTH peers -- in sync (no desync), but the player's action
gone.

VERIFIED: a 1-node-2-edge bridge road (triangle closing edge) now captures and
executes success=true on both peers, e9646 identical, 0 desyncs -- and the
worlds stayed in sync through the whole triangle, confirming the earlier
divergence was the one missed road cascading, not non-deterministic town growth.

Fix (lockstep.lua, pure Lua): replace the count heuristic with GEOMETRY. A new
node is a split point only if `findEdgeContaining` finds an existing edge under
its position; a bridge midpoint sits in open space and is kept. The originator
has cancelled its build, so the original edges are intact for the check.

Note: the ROADE->ROADP converter has now had three bugs, all the same shape --
a road shape the original all-new-nodes ROADN format never had to express
(existing-node connections, 0 new nodes, bridge-through-midpoint). Each surfaced
only with a specific hand-drawn road. A ground-truth-style sweep over road
TOPOLOGIES (draw every connection shape once, assert all replicate) would catch
the rest in one pass rather than one bug report at a time.


## Constructions -- native replay (CONX, 2026-08-28)

`buildConstruction` runs the template but builds its street pieces at RAW
coordinates (measured: a depot apron's outer end 0.6 m off the originator's
split node) and never integrates with the road; a street proposal cannot delete
those construction-owned pieces either (`Construction not possible` on the
removal alone). So the peer now replays the ORIGINATOR'S WHOLE PLACEMENT
PROPOSAL, verbatim, in one `buildProposal`:

| step | where |
|---|---|
| hook sees the placement (caller `419f62`), never cancels, writes the street vectors (nodes, edges, removed edges, all with tangents) as a `ROADC` line | originator |
| Lua parks the payload in `pendingRoadc`; `pollNewConstructions` reads fileName / params (seed KEPT) / transf off the new entity into `pendingCons`; `flushConPairs` pairs the two by distance (150 m) and ships one `CONX` | originator |
| `execConX`: `constructionsToAdd[1]` from file/params/transf + `streetProposal` nodes/edges/removals from the wire; the removed street edge is found under the split position and its endpoints stand in for the originator's positive ids; `make.buildProposal` + `sendCommand` | every peer |
| a construction nothing pairs with inside 1 game unit ships as plain `CONP`, replayed by the same native path; fallback to `buildConstruction` only if `make` refuses | all |

The weld machinery (ROADP `weld=` / `apron=`, execPolyline WELD v2) is now
unreachable from ROADC and stays only as history.

### CONX -- VERIFIED LIVE (2026-08-28 17:xx)

Road depot snapped 1 m from a road's end node on A; on B:
`[merge] template seg 2: -5->-6 re-pointed to X=-2 ... nodes 3->2` then
`EXEC CONX seq=2 ... nodes=1 edges=2 rm=1 dropped=0 success=true ent=255112`.
Both instances moved to the SAME new world hash (`1816828999-1199870997`),
edge counts equal (e9644/e9644). First construction replica ever verified
connected AND hash-identical.

Pieces that had to be true at once (each one measured, none guessable):
1. `make.buildProposal` accepts a script ConstructionEntity only with `params.seed` present.
2. The template appends its own connector at make time (`scripting::Convert`
   -> `MakeProposalAdd` with an empty nodes2snap); ship only split node +
   halves + removal, never the apron.
3. Linkage is index-based (`ConstructionEntity+0x768`, `+0x780`,
   `Proposal+0x170/+0x188`): patch records in place, drop only the LAST node.
4. Placeholder ids numbered like the UI (-1, -2, ...), template ids allocate below.
5. Context `nil` (the hand-built one differs in layout and flags).
6. `findEdgeContaining` must sample by distance (1 m) and exclude ends by
   distance (0.3 m), or a split 1 m from an end node is never found.

Open: clicking a script-built construction crashed the client twice earlier
today (GUI select handler); whether the native-shape replica still does is
being probed (NAME / PLAYER_OWNED comparison).

### CONX -- VERIFIED AGAIN, full recipe (2026-08-28 17:2x)

Second verified placement (town road, split mid-edge), after two more fixes:
`[merge] halves inherit the split edge's record (+0x28..0x63), +0x64=0x7f00`
then `EXEC CONX seq=2 ... nodes=1 edges=2 rm=1 dropped=0 success=true
ent=236782` and `named entity 236782 'Östringen Road depot' ok=true`.
User-confirmed connected on the peer.

The complete set of conditions, each one measured and each one necessary:

1. `make.buildProposal` accepts a script ConstructionEntity only with `params.seed`.
2. Ship only split node + halves + removal; the template regenerates its own
   connector at make time (`scripting::Convert` -> `MakeProposalAdd`, empty
   nodes2snap) and a shipped apron collides with it.
3. The hook (`MergeTemplateStreet`, merge v3) re-points the template
   connector's outer end onto our split node at the factory's entry, recomputes
   its straight tangents, sets our node flags to 0x7f00 and drops the
   template's outer node -- the LAST record, so no index shifts.
4. Never compact/reorder proposal vectors: linkage is index-based
   (`ConstructionEntity+0x768/+0x780`, `Proposal+0x170/+0x188`).
5. Placeholder ids numbered like the UI (-1, -2, ...); a -100001 base broke
   the bounding-volume bookkeeping.
6. Context `nil`.
7. `findEdgeContaining` samples by distance (1 m) and excludes ends by
   distance (0.3 m): the UI splits 1 m from an end node.
8. Split halves must be the ORIGINAL edge's record with new endpoints/tangents
   (street type of the road, not the construction's; +0x64=0x7f00; +0x6c
   shared with the removed record). Ours carried the depot's type 29 onto a
   type-16 town road -> `Construction not possible`.
9. The construction NAME goes IN the proposal (`ce.name`): the apply then
   gives the construction AND its child entities (VEHICLE_DEPOT / stations)
   NAME and PLAYER_OWNED, exactly like a UI build (probe P9). An earlier run
   blamed the name for `Construction not possible`; that run's real cause was
   item 8. Without the name the child has neither component and the GUI select
   handler crashes the client on click (minidumps 4057236f, 59c3ba2e).

Open after this: (a) the hook copies +0x48 from B's removed record, which the
sol2 conversion leaves 0 -- the engine accepted it, but confirm the halves'
type on the peer matches; (b) e-count A 9645 vs B 9644 after the placement, MEASURED at one stamp
(constructions identical): around the depot both have apron + two halves, but
on A the road EAST of the split was re-graded by the engine -- node 218270
(111.0,-8228.6) replaced by a new node at (119.0,-8231.5) plus an extra split
at (152.2,-8235.8) -- which is not in the captured proposal. Suspect: the UI's
Context (checkTerrainAlignment=1, cleanupStreetGraph=1) vs the nil context on
the peer. Next try: build an api.type.Context with the UI's flags for CONX; (c) click-crash ROOT CAUSE FOUND (B minidump 4057236f: access violation,
null +0x10, in game\scripting\legacy\interface.cpp called from the GUI select
handler): a script-built construction's SUB-ENTITY (the VEHICLE_DEPOT entity in
CONSTRUCTION.depots) has no NAME and no PLAYER_OWNED component, a UI-built
one has both (probe P3: scripted sub 281744 NAME=- PLAYER_OWNED=-, UI sub
244147 NAME=Y PLAYER_OWNED=Y). `game.interface.setPlayer` on the child fails ('internal error') and a
SetName command names but does not own it; the fix is the proposal name
(recipe item 9). Verify by clicking;
(d) VBUY first live run; (e) tram depot / stations through the same path.

### CONX -- VERIFIED COMPLETE incl. click (2026-08-28 17:5x)

Third verified placement with the name in the proposal: on B
`EXEC CONX seq=2 ... success=true ent=259296` then
`entity 259296 child 281728 childNAME=true childOWNED=true`; the user clicked
the replica on B and the depot window opened (no crash). Depot replication is
now: placed, snapped and integrated with the road, named, owned, clickable --
the same as a UI-built depot on the originator.

## Vehicles -- BuyVehicle replication (VBUY) VERIFIED (2026-08-28 18:0x)

`[cap] BuyVehicle caller=74fd88` -> `VBUY shipped: depot=281753 parts=1` on A;
A's mod: `VBUY: depot 281753 at 448.1,-13014.9 (depot/road_depot_era_a.con), 1
part(s): vehicle/bus/usa/american_post_coach_v2.mdl~0~-1,-1,-1~` -> B:
`EXEC VBUY seq=5 origin=a construction=265093 depot=281748 parts=1
success=true`. User-confirmed: the bus appeared in the peer's depot.

Design: optimistic-local (never cancelled; originator skips its own replay).
The hook decodes the by-value TransportVehicleConfig (parts @0x80 stride:
modelId, loadConfig, color, autoLoadConfig; vehicleGroups) and ships the DEPOT
ENTITY ID -- which is the VEHICLE_DEPOT CHILD, not the construction. The
originator maps child -> parent construction (the one whose
CONSTRUCTION.depots holds it) and ships the construction's position + file;
model ids travel as file names (api.res.modelRep.getName / find). The peer
resolves the construction by position (consByKey, then a 6 m radius scan),
checks the file matches (a train into a road depot is an uncatchable assert),
and buys into `CONSTRUCTION.depots[1]`.

Caller filter: ship 74fd88 (UI), suppress ceefae (sol2 wrapper = our replay);
the first version had these swapped (docs/re/COMMAND_ARGS.md).

Known limits: `reversed` is not decoded (needs a sweep); purchaseTime /
maintenance are engine-assigned; the detector's v-count does not see
depot-parked vehicles (v79 before and after on both sides), so VBUY sync is
not yet hash-verified; vehicle IDENTITY across peers (needed for SetLine /
SellVehicle / SendToDepot / Replace) is the next problem: candidates are the
vehicle's user-visible name or (depot, purchase order).

## Vehicles -- cross-peer identity + Sell / SendToDepot / SetLine (deployed 2026-08-28 18:2x, verification pending)

Entity ids differ between instances, so a vehicle is addressed by a KEY:
- a replicated purchase's `origin:seq` -- each peer records which LOCAL
  vehicle that purchase produced: after the buy, the target depot's vehicle
  that is not in `knownVeh` (`pollVehKeys`, oldest pending key takes the
  smallest new id);
- `s:<id>` for a vehicle that existed at load (`primedVeh`, via
  `getEntities(type="VEHICLE")`) -- same id on both peers from the same save;
  refused for anything else (a stale id on the peer names a different
  vehicle; sellVehicle is irreversible).

Enumerating a depot's parked vehicles: `game.interface.getDepotVehicles`
ERRORS for the construction and for the VEHICLE_DEPOT child alike (probe P10),
`getEntities(type="VEHICLE")` never lists a parked vehicle (79 before and
after a purchase -- parked vehicles are not world entities), so use
`api.engine.system.transportVehicleSystem.getVehiclesWithState(
api.type.enum.TransportVehicleState.IN_DEPOT)` and match each vehicle's
`TRANSPORT_VEHICLE.depot`, which is the VEHICLE_DEPOT CHILD id (probe P16:
en-route train 279317 -> depot=244147, the train depot's child). A scripted
buy whose config lacks `autoLoadConfig` is a native assert
(`vehicle_util_engine::UpdateConfigFromModelIds`, B crashed on probe P13).
Priming waits until `primeQueue` has drained; priming on the first
`consPrimed` tick saw an empty `consByKey` ("primed 0").

Hook: SellVehicle (id 3, r8 = vector<Entity>) -> `VSELL n ids`; SendToDepot
(id 5, r8 vehicle, r9 bool) -> `VDEPOT v sell`; SetLine (id 6, r8 vehicle, r9
line, st[0] stop) -> `VLINE v line stop`. Lua-path (our replay) callers are
the scripting block 0xcec000..0xcf2000 and are not shipped. Lines are still
addressed as `s:<id>` (save lines only) until line identity exists.

### Vehicle identity + SellVehicle VERIFIED (2026-08-28 18:5x)

A: `veh: a:3 <-> local vehicle 277278` then `VSELL: a:3`; B: `EXEC VBUY seq=3
... success=true`, `veh: a:3 <-> local vehicle 271582`, `EXEC VSELL seq=4
origin=a ... sell a:3 success=true`. Same purchase key, each peer's own entity
id, the sale carried. Enumeration of the parked vehicle is via
`transportVehicleSystem.getVehiclesWithState(IN_DEPOT)` + `TRANSPORT_VEHICLE
.depot` (the VEHICLE_DEPOT child) -- see the corrected note above.

## Lines -- identity + Create / Update / Delete (deployed 2026-08-28 19:0x, verification pending)

Same pattern as vehicles. Hook (ids 7/8/9) ships only EVENTS: `LCREATE`,
`LUPDATE <lineId>`, `LDELETE <lineId>`; content is READ BACK from the line
entity on the originator (`lineSnapshot`: name via getName, COLOR component,
LINE.waitingTime, stops as station-group POSITION + station/terminal/loadMode/
min/max wait). A created line is keyed origin:seq -- the originator keys the
next line that appears in `lineSystem.getLines()` after an LCREATE, the peer
keys the next one after its `createLine` succeeds; save lines are s:<id>
(primed from getLines at load). The peer rebuilds `api.type.Line` /
`Line.Stop` with `findStationGroupNear` (20 m) and calls createLine(name,
color, player, line) / updateLine(id, line) / deleteLine(id). VLINE now ships
the line's key. Ordering trap handled: the UI fires UpdateLine right after
CreateLine; the LUPDATE parser keys the fresh line first (pollLineKeys) so the
first stops are not dropped. Not shipped: Line.vehicleInfo (unmeasured).

### Vehicle + line command set COMPLETE (verified 2026-08-28)

All verified live, both directions, SYNC held:
- BuyVehicle (VBUY), SellVehicle (VSELL), SendToDepot (VDEPOT), Reverse (VREV,
  factory id 10 = 0x9ddfe0 steal 20) -- cross-peer vehicle identity via the
  purchase-key registry (origin:seq) / s:<id> for save vehicles.
- CreateLine / UpdateLine / DeleteLine (LCREATE/LUPDATE/LDELETE) + SetLine
  (VLINE) -- line identity registry, content read back, stops by station-group
  position.
- Constructions: roads, rail, depots, station/street/modular_terminal --
  native CONX replay, snapped/integrated/named/clickable.

Reverse: `VREV: s:253938` (A) -> `EXEC VREV ... reverse s:253938 success=true`
(B), hash `1367142294-1550108346` on both.

Caller for Reverse UI: 8b556d (viewcreator.cpp per ACTION_MAP); the sol2
wrapper block 0xcec000..0xcf2000 is suppressed as our own replay.

Open items (all secondary, none block the core loop):
1. A specific depot geometry still fails on the peer with 'Construction not
   possible' (seq=4 in one test built on A, refused on B) -- capture with
   dumpprop when it recurs.
2. Road east of a split re-graded on A only (Context flags) -- one-edge desync;
   try giving CONX a Context with checkTerrainAlignment/cleanupStreetGraph.
3. Line route OVERLAY is stale after removing a stop on BOTH instances (base
   game, cosmetic, self-corrects on reselect) -- not a divergence, data matches.
4. reversed flag on a bought vehicle not decoded; Line.vehicleInfo not shipped
   (updateLine works without it); detector still doesn't hash depot-parked
   vehicles.

## Strict lockstep -- proven for Reverse (2026-08-28)

The optimistic-local model (originator applies at click, peer at stamp) makes
time-sensitive commands diverge: a reversed train runs opposite for the delay
window, positions drift, cargo timing shifts, TOWNS GROW DIFFERENTLY (measured:
c+e climbing after a reverse). Fix = STRICT lockstep: the originator cancels its
own command and replays at the stamp, so both apply at the IDENTICAL game-time.

Mechanism (verified for Reverse, factory id 10):
- Hook arms the cancel at the factory AND sets g_pendingNoCb=1 (fire-and-forget:
  no completion callback to honour, unlike the build tool).
- At CommandList::Add the Command pointer matches (r8 == g_pendingCmd); the
  callback cannot fire (a reverse has none), so the NoCb path SUPPRESSES it
  cleanly (return 1) instead of the old "let it run" fallback.
- Lua STRICT_OPS = { VREV } -> the originator does NOT skip its own replay; it
  applies at the stamp like the peer.

Result: reverse a train on A -> `CANCEL fire-and-forget ... cancelled=1`, both
A and B `EXEC VREV ... at=55274.8` (same stamp), SYNC held desyncs=0. Both
trains reversed once, together, no drift.

Extending to Buy/Sell/SendToDepot/SetLine: each must first be VERIFIED to reach
CommandList::Add (the cancel lands, cancelled++), exactly as Reverse was --
otherwise the originator cancels a command it never replays (buy) or
double-applies (if it does NOT reach Add). The hook cancel is currently gated to
id==10 only; buy/sell/depot stay optimistic until verified. Constructions/lines
are harder: they read their data back AFTER building, so strict needs capturing
the data from the proposal at the hook instead of from the built entity.

### Strict lockstep -- BUY cannot be suppress-cancelled (2026-08-28)

Extending strict to all four vehicle commands crashed the client on BUY. The
cancel DID land (`CANCEL fire-and-forget caller_rva=74fda9`), but the depot
window UI WAITS for the result (the new vehicle entity to display); suppressing
the buy without returning that result asserted and crashed. Reverse has no such
wait -- that is why it is safe.

Rule: STRICT (suppress + replay at stamp) is safe ONLY for a command whose UI
does not wait for a result. Verified safe: Reverse (id 10). Unsafe: BuyVehicle
(id 2) -- stays optimistic. Sell (3) / SendToDepot (5) UNTESTED -- each must be
shown NOT to wait before enabling; the failure mode is a client crash, so verify
conservatively. To make a WAITING command strict would require firing its real
completion callback with a valid result (as the build tool's callback is fired),
not the no-callback suppress -- a bigger change.

Net: Reverse is strict (drift eliminated). Buy/Sell/SendToDepot optimistic.

## Demolish -- manual construction demolish WORKS (2026-08-28)

Bulldoze a player station/depot/building: the mod's scanConstructionEdits
detects the tracked construction gone (debounced DEMOLISH_DEBOUNCE=30 ticks so an
UPGRADE's replace-window is not misread), ships DEMOLISH <x,y>, the peer's
execDemolish finds the construction within 30 m and bulldozes it; expectedDemolish
stops the peer echoing its own bulldoze. Verified: A built a modular_station,
demolished it -> `con: DEMOLISH captured at 291.7,-13713.3` -> B `EXEC DEMOLISH
id=251461 success=true` + `bulldozed by replay -- not echoed`. Constructions
stayed in sync (c hash matched).

Not yet covered: road/rail edge demolish (edges have no persistent position
track; needs an edge-removal detector), and station-placement removals of an
overlapping road (inside the construction's own proposal -- verify CONX srm).

## OPEN (surfaced by the long demolish run): the `e` verdict counts town-grown roads

The demolish run desynced -- but the desync started ~4500 game-units BEFORE the
demolish (t=58164) as a 1-edge difference that cascaded into town-building drift
(t). Root: worldHash's `e` component is ALL edges including town-built roads,
which grow non-deterministically over long runtimes; one diverging town road edge
poisons the verdict forever. Constructions (c), vehicles (v) matched -- player
infrastructure was in sync. Fix direction: exclude town-owned edges from the
verdict the way town buildings are excluded from `t` (hard: edges have no clear
owner). Until then the verdict is only reliable over SHORT runs, not multi-hour
soaks.

## Station-over-buildings replication (2026-08-28/29 session)

A station placed over town buildings auto-demolishes them on the originator
(the UI proposal's toRemove, which cannot travel -- ids differ). The replay
chain that finally works, each step measured:
1. CAPTURE: the station reuses a demolished building's entity id, so
   pollNewConstructions (knownCons gate) never sees it. The parked ROADC now
   RESCUES its construction by position (findConstructionForRoadc) -- id-free.
2. REPLAY: 'Collision' on the peer (buildings still standing). Clear the
   town-owned CONSTRUCTIONs and ASSET_GROUPs inside the FOOTPRINT -- the
   bounding box of the shipped platform-track nodes padded 25 m, NOT a fixed
   disc (40 m left a big station's outer buildings standing and the retry
   built on top of them).
3. RETRY: 1.5 game-units later (the clearing bulldozes are ASYNC -- an
   immediate retry validates against the un-cleared world), with
   ignoreErrors=true (the originator's UI already validated the placement;
   vegetation is not enumerable to clear).
Verified mid-chain: 'cleared 2 town obstacle(s) -- retry in 1.5' ->
'EXEC CONX ... success=true ent=272901', station standing, named, owned.
Remaining risk: PLAYER-owned obstacles are never cleared (deliberate).

## Harness: menu clicks are focus-polite now

PostMessage clicks are IGNORED (the game reads raw input only), so a real
click is unavoidable. autotest.ps1 now saves the foreground window + cursor,
clicks, restores both (~0.6 s), and retries every ~3 s instead of every 1 s.

NOTE (2026-08-29): the station-over-buildings clear is calibrated for the
MODULAR station (user note) -- module layouts change the footprint, so the
track-bbox + local-frame pads are an approximation. Robust close-out planned:
post-build sweep using the built station's bounding volume (probe P33).

## Session tooling + multiplayer UI (2026-08-29, verification pending)

- tools/mp_launch.ps1 -- one command: optional '-Save <name>' syncs the save
  host -> peer (.sav/.sav.lua/.jpg) and stamps it NEWEST on both (CONTINUE
  loads the newest save), then autotest -LaunchOnly + inject both + cfg copy.
- In-game MP status panel: engine update() writes lockstep_status_<inst>.txt
  every 15 ticks (t, peer, skew, desyncs, queued, PAUSED); guiUpdate (separate
  GUI Lua state -- files are the only channel, same as the wire) renders both
  instances' rows in an api.gui Window ('Multiplayer'), self-heals if closed.
- Next: UI actions (resync button -> signal file -> host-side watcher runs
  mp_launch -Save), save-on-demand from the host.

## Roadside stops -- native-shape replay (2026-09-02, verification pending)

Decompiled (workflow, 8 agents): the stop tool and the bulldozer BOTH rebuild
the edge (remove + re-add as entity -1) -- but carry every untouched object in
the new segment's `objects` list under its POSITIVE id (UpdateEngine: id>=0 =
re-parent, entity/station group/lines kept; -k = edgeObjectsToAdd[k]) and put
a removed object in `edgeObjectsToRemove` (Apply rewrites lines + group before
it dies). Our rebuild re-created neighbours as -k and never listed removals:
that -- not the rebuild -- left lines on dead ids (fatal assert, both peers).

Now (`CM.nativeStopProposal`): survivors by id; edgeObjectsToRemove for
deletes; STOPREP (remove+add in one proposal) for the host's same-edge
same-side replace, followed by an LUPDATE of every affected line; stop ops
serialized through the construction queue, line ops queued behind them.
The objects pair's second value is the SIDE (STOP_LEFT=0 / STOP_RIGHT=1 /
SIGNAL=2), not cargo; one object per side per edge (CreateLanes). The host
measures the engine's left-vs-geometry convention from the stop it ships
(`conv`), the peer derives `left` and side from its OWN geometry. Line stops
ship the station position (fields 8-9) and resolve the station index by it.
Proof to grep for on B/C: `EXEC STOPADD ... survivors kept n/n`,
`stops: left convention: engine-left IS/is NOT geometric-left`.
Switches (tpf2_slice.cfg): `stops_native=0` (old rebuild + guards),
`stops_del_on_line=0` (refuse to remove a stop a line uses).
