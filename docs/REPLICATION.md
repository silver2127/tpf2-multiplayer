# What replicates, and how

Every player action is replicated in one of three ways. This page lists each action, the
way it travels, the switch that controls it, and what is known not to work. How the pieces
fit together (capture, stamps, the clock) is in [ARCHITECTURE.md](ARCHITECTURE.md); every
switch is described in [CONFIGURATION.md](CONFIGURATION.md).

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

Switches live in `tpf2_slice.cfg` and must be the same on every machine in a session: a
player with different switches runs a different protocol. "Shipped" below is the value in the
installer's cfg.

## Building

| action | mode | wire | switch (shipped) | notes |
|---|---|---|---|---|
| road, street | strict | `ROADP` | `suppress=1` | Captured at the street tool's factory call, cancelled at `CommandList::Add` with the tool's callback fired. Replayed from positions: an existing node within 1.5 m is reused, else the edge underneath is split, else a node is created; a split within 2.5 m of an end snaps to that end. The originator runs a plan pass and ships its crossing and split decisions. |
| rail track | strict | `ROADP` | `suppress=1` | Same path with the track type and catenary. Track snaps to an edge within 2.0 m (roads 5.0 m). |
| bridge, tunnel | strict | `ROADP` | `suppress=1` | Each link carries its BaseEdge type (1 bridge, 2 tunnel) and type index; split halves keep them. |
| upgrade: street/track type, catenary, bus lane, tram track | strict | `ROADP` | `suppress=1` | The removed edges travel as positions so the replay replaces instead of stacking a second edge. |
| level crossing | strict (part of the track build) | `ROADP` | `suppress=1`; `xing_reheight=0` | A track vertex within 4.0 m of a road node shares that node (taking the road's height when they differ by more than 0.25 m; moving the road node instead asserts the engine). Otherwise the road under the vertex is split. Crossings in the middle of a track segment are found analytically; routing through an existing node requires it to be touched (0.75 m) and straight-through. A crossing the engine refuses ("Too much slope") is refused on every instance. |
| demolish road or track | strict | `EDEMO` | `road_demolish=1` | Edges are matched by their end nodes (same kind, within 1 m). An edge that carries stops or signals is refused. Orphaned nodes are removed. |
| station, depot, asset, harbour, airport | strict | `CONX` / `CONP` | `cancel_construction=1` | The slice reads the construction's file, placement and parameters off the proposal and cancels the build; the street pieces travel as `ROADC` and are paired by position. Every instance builds the same scripted proposal at the stamp. |
| same, when the parameters cannot be read | replay on peers, then corrected | `CONX` / `CONP` | `conx_strict=1` | The native build stands and is captured by polling. With other players connected, the originator then bulldozes its own copy and rebuilds the scripted one with the peers (money reconciled); alone it keeps the native build. |
| module edit, station upgrade | strict | `CONU` (`diff=1 strict=1`) | `strict_module=1` | The old construction and the new parameters come off the proposal; every instance upgrades the construction (same file within 10 m) at the stamp. If the cancel does not land, the edit scan ships it instead (every 30 ticks, originator skips). |
| demolish construction | strict | `DEMOLISH` (`strict=1`) | `strict_condemo=1` | Every instance requires the same file within 2 m. Without the switch: a tracked construction missing for two polls ships a `DEMOLISH` and peers remove the nearest one within 30 m. |
| roadside stop, signal, waypoint: place | strict | `STOPADD` | `strict_stops=1` | The engine's own `left` byte and the edge tangent travel with it (a track object's `left` is not its geometric side). |
| stop, signal, waypoint: bulldoze | strict | `STOPDEL` | `strict_stops=1` | |
| stop placed on an occupied side (replace) | poll | `STOPREP` + `LUPDATE` | | Not cancelled: the engine re-points the old stop's lines, which a script proposal cannot express, so the originator re-ships every affected line after the replace. |

Replay details for constructions:

- The street payload's split edge is found by position; a topology node that cannot be found
  logs `DIVERGENCE` and the build is skipped. The template's own connector pieces are dropped
  from the payload (the template regenerates them).
- A cancelled placement builds with `gatherBuildings=true`, so the engine demolishes the
  footprint's town buildings identically everywhere. For the non-cancelled path the originator
  ships the town buildings it still has nearby ("survivors"), and the replay removes others
  within 190 m.
- On failure the replay retries once after clearing the footprint, then asks the originator to
  roll back (`CONFAIL`: it bulldozes its own copy, same file within 1 m).
- The construction gets a name in the proposal (the shipped one, or "`<town> <type>`"), which
  also names and owns its child depot/station entities.

Replay details for stops: with `stops_native` (default on) the replay builds the engine's own
edge-object proposal, keeping every other object on the edge under its id; the edge is found by
its end points within 2 m, else the nearest centreline within 14 m. One stop per side per street
edge; edges frozen into a construction are refused. A stop that a line uses is removed only while
`stops_del_on_line` is on (default). `stops_native=0` falls back to rebuilding the edge, which
refuses edges under a line.

## Vehicles

| action | mode | wire | switch (shipped) | notes |
|---|---|---|---|---|
| buy | strict | `VBUY` | `strict_buy=1` | The depot window waits for its callback, which is fired. The depot is found by position and file; the vehicle goes into its first depot. At most one buy per tick, so purchases bind to keys in the same order everywhere. Without the switch the buy applies natively and ships once the vehicle exists (or after 1.5 units). |
| sell | strict | `VSELL` | `strict_sell=1` | Ships once every vehicle's key is bound, or the bound subset after 8 units. |
| send to depot | strict | `VDEPOT` | `strict_depot=1` | |
| reverse | strict | `VREV` | `cancel_vehicle=1` | |
| replace | strict | `VREPL` | `strict_replace=1` | The key is re-bound to the replacement vehicle. |
| assign to line | strict | `VLINE` | `cancel_line=1` | Waits for the vehicle's and line's keys to bind. |
| maintenance target | not replicated by default | `VMAINT` | `maint`, `strict_maint` (neither shipped) | `maint=1` ships it (replay on peers); adding `strict_maint=1` makes it strict. |
| rename, recolour | replay on peers | `VNAME` / `VCOLOR` | | By key, or by position for constructions. |

Not replicated: stop/start a vehicle, manual departure, "depart now".

## Lines

| action | mode | wire | switch (shipped) | notes |
|---|---|---|---|---|
| create | replay on peers | `LCREATE` | | Never cancelled (the line editor needs the new line). Peers read the line back from the originator's data and bind the new line by its stop signature. |
| edit stops | strict | `LUPDATE` | `strict_line_edit=1` | The new stop list is decoded off the command. If decoding fails the edit applies natively and peers read the line back. |
| delete | strict | `LDELETE` | `strict_line_edit=1` | |

Stops are resolved by the station group's position (within 20 m) and the station's position
(within 10 m), because a stop's station index can differ between instances.

## Money, speed, companies

| thing | how |
|---|---|
| loan | Polled every 15 ticks and shipped as the new absolute loan; peers book the difference. The originator skips its own. |
| balance | Not replicated as such: it follows from every instance applying the same actions. Construction replays reconcile the originator's balance; in co-op a gap above 3000 after a construction snaps to the originator's balance. Differences are logged as `$$` lines. |
| game speed, pause | Not commands: the session speed is set by pacing, see [ARCHITECTURE.md](ARCHITECTURE.md#pacing-and-game-speed). |
| companies | `CMNEW`, `CMSWITCH`, `CMDEL`, `CMPW` apply at the stamp on every instance; in companies mode every command carries its company, and builds, purchases, lines and loans are attributed to that company's engine player. See [ARCHITECTURE.md](ARCHITECTURE.md#companies-mode). |

## Not replicated

- Terraforming, terrain painting, the asset brush.
- Stop/start, manual departure and "depart now" for vehicles.
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

- **Cadence.** A stamp every 12 game-time units, times min(8, edges/2000 + 1) on big maps.
- **Verdict.** A match logs `SYNC`. A mismatch logs `~~ LAG n/3` twice (a late hash is not a
  desync), then `!! DESYNC` with the differing lanes named.
- **Vehicle drift.** Instances also exchange sampled vehicle positions; a maximum drift over
  `vpos_desync_m` (10 m) between samples taken at the same sim time counts as a desync.
- **`dump_egeo=1`** writes every edge to `egeo_<letter>.txt` in the game folder so two instances
  can be diffed (ignore the first line, a per-instance stamp).

The in-game Multiplayer window shows the verdict (`SYNC`, `DESYNC <lanes> vs <letter>`,
`DESYNC town`, `DESYNC vpos`).
