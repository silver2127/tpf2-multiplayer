# What replicates, and how

Every player action is replicated in one of three ways. This page lists each action, the
way it travels, and what is known not to work. How the pieces fit together (capture, stamps,
the clock) is in [ARCHITECTURE.md](ARCHITECTURE.md).

Unconfirmed road and railway routes also have [shared visual previews](BUILD_PREVIEWS.md).
These use optional native 3D rendering with a ground-marking fallback, separate
from the construction commands below.

| mode | what happens |
|---|---|
| **strict** | The slice DLL cancels the player's command inside the engine before it applies. The mod ships it with a stamp, and **every** instance, the player's own included, applies it at that stamp. Nobody's world runs ahead. |
| **replay on peers** | The command applies natively at the click; the other instances apply it at the stamp, a few sim steps later. The player's own game is briefly ahead for that action. |
| **poll** | No hook: the mod notices the change on the originating game (by scanning) and ships it; the others replay it. |

Three rules hold everywhere:

- **Only cancel when something will replay.** The slice cancels nothing unless the mod's
  status file (`lockstep_status_<letter>.txt` in the data folder) was written in the last
  15 s and reports a peer. With the mod off, or when playing alone, every command runs
  natively and the game behaves like the stock game.
- **Never cancel on a failed decode.** If the slice cannot read a command completely, it lets
  the command run natively and the poll or replay path ships what it can. Losing the player's
  action is worse than a visible divergence.
- **Nothing on the wire names an entity id.** Entity ids differ between games. Roads travel as
  positions; constructions as file plus position; vehicles and lines as keys bound on each
  instance (`<letter>:<seq>` for things bought or created in the session, `s:<id>` for things
  that were in the save).

There are no per-channel switches: every instance runs the same protocol, whatever its
settings files say ([CONFIGURATION.md](CONFIGURATION.md)).

## Building

| action | mode | wire | notes |
|---|---|---|---|
| road, street | strict | `ROADP` | Captured at the street tool's factory call, cancelled at `CommandList::Add` with the tool's callback fired. Replayed from positions: an existing node within 1.5 m is reused, else the edge underneath is split, else a node is created; a split within 2.5 m of an end snaps to that end. The originator runs a plan pass and ships its crossing and split decisions. |
| rail track | strict | `ROADP` | Same path with the track type and catenary. Track snaps to an edge within 2.0 m (roads 5.0 m). |
| bridge, tunnel | strict | `ROADP` | Each link carries its BaseEdge type (1 bridge, 2 tunnel) and type index; split halves keep them. |
| upgrade: street/track type, catenary, bus lane, tram track | strict | `ROADP` | The removed edges travel as positions so the replay replaces instead of stacking a second edge. |
| level crossing | strict (part of the track build) | `ROADP` | A track vertex within 4.0 m of a road node shares that node, taking the road's height when they differ by more than 0.25 m (moving the road node instead asserts the engine). Otherwise the road under the vertex is split. Crossings in the middle of a track segment are found analytically; routing through an existing node requires it to be touched (0.75 m) and straight-through. A crossing the engine refuses ("Too much slope") is refused on every instance. |
| demolish road or track | strict | `EDEMO` | Edges are matched by their end nodes (same kind, within 1 m). An edge that carries stops or signals is refused. Orphaned nodes are removed. |
| station, depot, asset, harbour, airport | strict | `CONX` / `CONP` | The slice reads the construction's file, placement and parameters off the proposal and cancels the build; the street pieces travel as `ROADC` and are paired by identity (one placement serial on both records). Every instance builds the same scripted proposal at the stamp. |
| same, when the parameters cannot be read | replay on peers, then corrected | `CONX` / `CONP` | The native build stands and is captured by polling. With other players connected, the originator then bulldozes its own copy and rebuilds the scripted one with the peers (money reconciled); alone it keeps the native build. |
| module edit, station upgrade | strict | `CONU` (`diff=1 strict=1`) | The old construction and the new parameters come off the proposal; every instance upgrades the construction (same file within 10 m) at the stamp. If the cancel does not land, the edit scan ships it instead (every 30 ticks, originator skips). |
| demolish construction | strict | `DEMOLISH` (`strict=1`) | Every instance requires the same file within 2 m. When the slice leaves a bulldoze to run natively, a tracked construction missing for two polls ships a `DEMOLISH` and peers remove the nearest one within 30 m. |
| roadside stop, signal, waypoint: place | strict | `STOPADD` | The engine's own `left` byte and the edge tangent travel with it (a track object's `left` is not its geometric side). |
| stop, signal, waypoint: bulldoze | strict | `STOPDEL` | |
| stop placed on an occupied side (replace) | poll | `STOPREP` + `LUPDATE` | Not cancelled: the engine re-points the old stop's lines, which a script proposal cannot express, so the originator re-ships every affected line after the replace. |

Replay details for constructions:

- The street payload's split edge is found by position; a topology node that cannot be found
  logs `DIVERGENCE` and the build is skipped. The template's own connector pieces are dropped
  from the payload (the template regenerates them).
- A cancelled placement builds with `gatherBuildings=true`, so the engine demolishes the
  footprint's town buildings identically everywhere. For the non-cancelled path the originator
  ships the town buildings it still has nearby ("survivors") together with the radius it gathered
  them in (`srad`: the construction's bounding box + its street payload + 100 m), and the replay
  removes the others 10 m inside that radius. No fixed radius and no cap on the removal count: a
  list whose survivors mostly do not exist on the peer is refused as a `DIVERGENCE`, loudly. The
  street payload pairs with its construction by identity (the entity's frozen nodes, or for a
  cancelled placement the placement serial the slice stamps on both records), never by
  distance or arrival order; a cancelled placement whose payload is missing is refused
  loudly, and an unclaimed payload is logged as a `DIVERGENCE`. The slice's own side has no
  size of its own either: the params walk has no depth or entry cap (a misread pointer fails
  it loudly and the build runs natively behind a `NATIVE` notice) and the street vectors are
  decoded in full.
- Construction parameter strings and keys escape embedded newlines as `\n` in
  their Lua literals. Lua 5.2's default `%q` emits a physical newline after a
  backslash, which splits the line-based command stream and loses module edits.
  Values are preserved exactly; malformed literals are still refused. Offline
  coverage: `tools/params_line_test.py` exercises the line receiver and station
  removal diffs on origin and peer with a mocked engine.
- On failure the replay retries once after clearing the footprint, then asks the originator to
  roll back (`CONFAIL`: it bulldozes its own copy, same file within 1 m).
- The construction gets a name in the proposal (the shipped one, or "`<town> <type>`"), which
  also names and owns its child depot/station entities.

Replay details for stops: the replay builds the engine's own edge-object proposal, keeping every
other object on the edge under its id; the edge is found by its end points within 2 m, else the
nearest centreline within 14 m. One stop per side per street edge; edges frozen into a construction
are refused. A stop that a line uses is removed too: the engine rewrites the line first, as when
the player bulldozes it.

## Vehicles

| action | mode | wire | notes |
|---|---|---|---|
| buy | strict | `VBUY` | The depot window waits for its callback, which is fired. The depot is found by position and file; the vehicle goes into its first depot. At most one buy per tick, so purchases bind to keys in the same order everywhere. A buy the slice leaves to run natively ships once the vehicle exists (or after 1.5 units). If the new vehicle's configuration cannot be read, the buy is not cancelled and does not replicate. |
| sell | strict | `VSELL` | Ships once every vehicle's key is bound, or the bound subset after 8 units. If the vehicle list cannot be read, the sale is not cancelled and does not replicate. |
| send to depot | strict | `VDEPOT` | |
| reverse | strict | `VREV` | |
| replace | strict | `VREPL` | The key is re-bound to the replacement vehicle. If the new configuration cannot be read, the replacement is not cancelled and does not replicate. |
| assign to line | strict | `VLINE` | Waits for the vehicle's and line's keys to bind. |
| rename, recolour | replay on peers | `VNAME` / `VCOLOR` | By key, or by position for constructions. |

Not replicated: stop/start a vehicle, manual departure, "depart now", maintenance targets.

## Lines

| action | mode | wire | notes |
|---|---|---|---|
| create | replay on peers | `LCREATE` | Never cancelled (the line editor needs the new line). Peers read the line back from the originator's data and bind the new line by its stop signature. |
| edit stops | strict | `LUPDATE` | The new stop list is decoded off the command. If decoding fails the edit applies natively and peers read the line back. |
| delete | strict | `LDELETE` | |

Stops are resolved by the station group's position (within 20 m) and the station's position
(within 10 m), because a stop's station index can differ between instances.

## Money, speed, companies

| thing | how |
|---|---|
| loan | Polled every 15 ticks and shipped as the new absolute loan; peers book the difference. The originator skips its own. |
| balance | Not replicated as such: it follows from every instance applying the same actions. Construction replays reconcile the originator's balance; in co-op a gap above 3000 after a construction snaps to the originator's balance. Differences are logged as `$$` lines. |
| game speed, pause | Not replicated as commands. The host's speed buttons set the session speed and pacing applies it on every instance; the other players' speed-button clicks are cancelled. See [ARCHITECTURE.md](ARCHITECTURE.md#pacing-and-game-speed). |
| companies | `CMNEW`, `CMSWITCH`, `CMDEL`, `CMPW` apply at the stamp on every instance; in companies mode every command carries its company, and builds, purchases, lines and loans are attributed to that company's engine player. See [ARCHITECTURE.md](ARCHITECTURE.md#companies-mode). |

## Not replicated

- Terraforming, terrain painting, the asset brush.
- Stop/start, manual departure, "depart now" and maintenance targets for vehicles.
- Map editor and scenario commands (towns, industries, no-costs).
- Town growth itself: it is not sent, it is simulated identically. Its building count is a
  detector lane, so a town that grows differently shows up.

## Detecting divergence

Each instance hashes the parts of the world lockstep keeps identical, by geometry and content,
never by entity id, and compares with the others at common stamps.

| lane | contents | in the verdict |
|---|---|---|
| `v` | vehicle count (vehicles parked in depots are not counted) | yes |
| `c` | player constructions (`file@x,y` plus a hash of their parameters without the seed) and player stops | yes |
| `e` | every edge's end points at 0.1 m | yes |
| `z` | edge heights | detail |
| `p` | vehicle positions at 1 m, compared only at the same sim time | detail |
| `m`, `l` | balance and loan (co-op only) | logged as `$$` |
| `t` | town construction count | desync after it differs at two stamps in a row |
| `n` | number of people | logged as `$$` |

- **Cadence.** The interval starts at 12 game-time units, times min(64, edges/2000 + 1) on big
  maps, then follows the hash's measured cost so every map loses the same share of time to it.
  Each player reports its hash's cost on its heartbeat (`hc=`, the median of its last five stamps),
  and the leader moves every game to the interval on a ladder (12, 24, 36, 48, 72, 96, 144 ... 1536
  units) that keeps the slowest one at or under 8 ms of hash per game unit. It does that with a
  stamped `HASHEVERY` command, so every game switches to the new grid at the same stamp: longer
  at once, shorter only with 25% headroom, at most once a minute. The grid is saved with the world.
- **Big maps are checked too.** Between 2026-09-12 and 2026-09-15 the hash was switched off
  entirely above vanilla's largest size (96 x 96 tiles), because a 224-tile world cost 3.0-3.5 s
  per stamp -- which left the biggest worlds with no desync detection at all. The cadence above
  replaces that: such a world starts at 168 units and settles near 576, so it hashes about once
  every ten minutes at 1x instead of never. The starting interval comes from the edge count, which
  every instance reads from the same save, so no instance hashes on a grid another never reaches.
- **Verdict.** A match logs `SYNC`. A mismatch logs `~~ LAG n/3` twice (a late hash is not a
  desync), then `!! DESYNC` with the differing lanes named.
- **A hash is a sample at a sim time, not a property of the stamp** (2026-09-16). The stamp only
  says which interval the sample fell in; what it describes is the world at the moment it was
  taken. So a game that ENTERS an interval part way through -- every game does, on the first
  update after a load, because the clock resumes at the save's own step -- takes its hash (the
  first one still sets the cadence from the edge count) but does not publish it, and two samples
  of one stamp taken at different sim times are not compared at all: no verdict, no town streak,
  no `$$` gaps, logged as `not comparable, skipped`. Until then they were compared, and every hot
  join reported a desync at its first stamp that was nothing of the kind: on the rig of
  2026-09-16 the joiner loaded a save taken at sim time 31.4 and published its stamp-0 sample at
  31.6 against the host's at 1.8, so 30 game units of ordinary town growth showed up as `t: 8899
  vs 8975`, an edge lane one edge apart and `!! DESYNC t=0`, with both worlds correct. A save is
  taken from a world that is not simulating (the engine's save blocks the game loop for its whole
  duration -- "Saving...: 8319 ms", during which `update()` is not called at all and the saving
  game loses exactly that much game time to its peers), so `savedAt` is the step the file's world
  is at, and a hot joiner starts from the step it says.
- **Vehicle drift.** Instances also exchange sampled vehicle positions; a maximum drift over
  10 m between samples taken at the same sim time counts as a desync. The check pairs every
  vehicle with every other, once per player, so it turns off for good the first time a world
  has more than 200 vehicles; the switch is saved with the world. The `p` lane stays.
- **`dump_egeo=1`** writes every edge to `egeo_<letter>.txt` in the game folder so two instances
  can be diffed (ignore the first line, a per-instance stamp).

The in-game Multiplayer window shows the verdict (`SYNC`, `DESYNC <lanes> vs <letter>`,
`DESYNC town`, `DESYNC vpos`).
