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
| road ownership tool | strict | `ROADP` | Each link carries `own`: 0 for public, otherwise a logical company number resolved to the receiver's player entity. The native `OWNERS` tail contains local player entities only in local IPC; they never travel over the network. Splits and unchanged bridge companions retain their original owners. Replay uses typed PlayerOwned components: the engine silently discards plain Lua tables assigned to this optional field. Captures without the optional tail retain legacy behavior. Ownership capture and replay have automated coverage; an in-game multiplayer test of this fix is still pending. |
| level crossing | strict (part of the track build) | `ROADP` | A track vertex within 4.0 m of a road node shares that node, taking the road's height when they differ by more than 0.25 m (moving the road node instead asserts the engine). Otherwise the road under the vertex is split. Crossings in the middle of a track segment are found analytically; routing through an existing node requires it to be touched (0.75 m) and straight-through. A crossing the engine refuses ("Too much slope") is refused on every instance. |
| demolish road or track | strict | `EDEMO` | Edges are matched by their end nodes (same kind, within 1 m). An edge that carries stops or signals is refused. Orphaned nodes are removed. |
| Snowball Fences / hedges | strict | `FENCE` | Cancelled `CONXP` cursor placements feed the installed mod's planner through a resource wrapper. Start/finish points create no construction; confirmed segments carry model names and transforms, never entity ids. Planning virtualizes build/bulldoze and isolates decorative RNG. Replays add one construction at the command stamp, with the originating company's local player and no building/field gathering or graph cleanup. Geometry is anchored near the fence, not world zero. Cursor previews are cosmetic, using native 3D when available and an outline otherwise; Workshop files stay unchanged. Segments are built with `ignoreErrors=true`, as the mod's own `buildConstruction` does: a segment touches the one before it, and a checked build refused every segment after a drag's first ("Kollision"). Automated coverage includes the real installed Fences planner, but a multiplayer game test remains pending. All participants need the new command handler. |
| terraform, terrain paint | strict | `TERRAIN` | The slice stashes the edit's grid (or paint-only blob) at the factory and cancels the commit; every instance applies it at the stamp (`ARMED 1`), or the peers alone when it had to run natively here (`ARMED 0`). The brush stroke is held until this instance's own replay has applied, so the next part of the stroke is computed against the replayed heights. |
| asset brush | strict | `ASSETS` | The stroke travels as a blob plus the ids of the asset groups it removes; same `ARMED` rule as terrain. |
| station, depot, asset, harbour, airport | strict | `CONX` / `CONP` | The slice reads the construction's file, placement and parameters off the proposal and cancels the build; the street pieces travel as `ROADC` and are paired by identity (one placement serial on both records). Every instance builds the same scripted proposal at the stamp. |
| same, when the parameters cannot be read | replay on peers, then corrected | `CONX` / `CONP` | The native build stands and is captured by polling. With other players connected, the originator then bulldozes its own copy and rebuilds the scripted one with the peers (money reconciled); alone it keeps the native build. |
| module edit, station upgrade | strict | `CONU` (`diff=1 strict=1`) | The old construction and the new parameters come off the proposal; every instance upgrades the construction (same file within 10 m) at the stamp. If the cancel does not land, the edit scan ships it instead (every 30 ticks, originator skips). |
| demolish construction | strict | `DEMOLISH` (`strict=1`) | Every instance requires the same file within 2 m. When the slice leaves a bulldoze to run natively, a tracked construction missing for two polls ships a `DEMOLISH` and peers remove the nearest one within 30 m. A record counts as present only while its entity still carries a CONSTRUCTION component of the recorded file (entity ids are recycled) or a replacement stands within 1 m of its spot; a neighbour further away does not keep it, and a recycled id never ships an edit. |
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
| buy | strict | `VBUY` | The depot window waits for its callback, which is fired. The depot is matched by live construction, transform-position key, file and depot children; a validated cache/local miss falls back to a global construction scan for offset mod geometry, and ambiguous matches are refused; the vehicle goes into its first depot. At most one buy per tick, so purchases bind to keys in the same order everywhere. A buy the slice leaves to run natively ships once the vehicle exists (or after 1.5 units). If the new vehicle's configuration cannot be read, the buy is not cancelled and does not replicate. |
| sell | strict | `VSELL` | Ships once every vehicle's key is bound, or the bound subset after 8 units. If the vehicle list cannot be read, the sale is not cancelled and does not replicate. |
| send to depot | strict | `VDEPOT` | |
| reverse | strict | `VREV` | |
| replace | strict | `VREPL` | The key is re-bound to the replacement vehicle. If the new configuration cannot be read, the replacement is not cancelled and does not replicate. |
| assign to line | strict | `VLINE` | Waits for the vehicle's and line's keys to bind. |
| rename, recolour | replay on peers | `VNAME` / `VCOLOR` | By key, or by position for constructions. |

Not replicated: stop/start a vehicle, manual departure, "depart now", maintenance targets.

Cancelled local vehicle actions restore their original confirmation sounds through
`action_sounds.lua`: buy (including clones), sell, assign line, send to depot and replace.
Only successful replay callbacks enqueue audio; uncancelled actions retain their native
UI feedback, and remote actions remain silent. Batch sales produce one confirmation per
command. Engine-to-GUI save/load sync carries a bounded cosmetic event history, never
network packets or simulation RNG. Initial audio state is deterministic and load/save
preserves it exactly: the engine compares ScriptSave across internal game states at
startup. A fresh GUI establishes a silent baseline, so saved confirmations do not
play again after loading or resync. The GUI uses
the game's [GameUI.playSoundEffect API](https://wiki.transportfever2.com/api/modules/api.gui.html)
and sound-effect names from `soundeffectsutil.lua`; an audio error does not affect replay.
New-line creation retains its existing native callback; ambient and vehicle-running audio
are unchanged. Offline coverage: `tools/action_sounds_test.py`; audible verification pending.

Purchase lookup validates the construction's transform position, file and depot children,
including cached ids. On a registry/local-query miss it scans constructions globally and
matches the same position key and file. This covers depots whose model geometry is offset
from the construction origin (the UEP catenary terminal was missing from the old 6 m query).
Fallback results are cached separately from construction edit state and revalidated on
each purchase; ambiguous query results are refused. `tools/depot_buy_test.py` exercises
the replay with mocked spatial queries, stale ids and neighboring depot types.

Line replay logs the destination line key, requested stop and applied stop on both success
and failure. Automatic stop selection (`-1`, including clones) tries the stops in line
order until one succeeds. An engine rejection schedules the next stop via the existing
retry queue, five simulation steps after the previous agreed target, without changing
the command stamp or dispatching from its callback. Each stop is tried at most once;
the initial stop count bounds the search, and a shortened line reduces that bound.
Explicit stop selections never fall back. A newer assignment, depot order or sale
invalidates the old search, including callbacks still outstanding.

An actual train on `Holz 1` rejected stop 0 and accepted stop 1, then left its depot.
`tools/vehicle_line_choice_test.py` reproduces this at the mocked engine boundary and
checks host/peer target steps, exhaustion, clones, delayed callbacks and superseding
orders. The complete automatic fallback still needs a multiplayer game test; matching
retry targets do not repair differences in peers' route topology or engine results.

## Lines

| action | mode | wire | notes |
|---|---|---|---|
| add a station or waypoint (platform choice) | strict | `LUPDATE` (`asg=`) | The editor picks each stop's platform by path search over the engine's stop list at the click; under lockstep that list lacks a click still on its way, so the platform came out against the wrong predecessor. An update that came out of the editor's assignment pass carries `asg=<0/1>` and every instance re-runs the game's own assignment on the rebuilt list at the stamp (slice, `LINE PLATFORM ASSIGNMENT AT REPLAY`; the slice ships the tag only after checking its path context against the editor's once). Manual terminal picks, alternative terminals, stop settings and removals replay verbatim. Played 2026-09-20: the pass runs on every instance at the stamp and returns the click's platforms on plain double track, which is what vanilla does there too (the game's assignment is a shortest-loop search with no side-of-track preference; it moves a platform only where signals, one-way track or geometry force it). |
| create | strict, from a spare | `LSPARE`, `LCREATE spare=` | Every player owns one empty SPARE line, created in lockstep (same step, same id everywhere) and owned by a hidden pool company, so no line list shows it. A click on New line is cancelled like any strict command, but the slice opens the line editor on the spare at once; at the stamp every instance re-owns, renames, recolours and re-keys that line to `origin:seq` and creates the player's next spare on the same step. Without a spare (one still in flight, or an older build) the editor's callback is held and rides on the replay at the stamp, as before. Why not create natively at the click: an entity allocated off-step shifts every id allocated after it on that game, and the worlds split (rig, 2026-09-12). |
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

- Stop/start, manual departure, "depart now" and maintenance targets for vehicles.
- Map editor and scenario commands (towns, industries, no-costs).
- Town growth itself: it is not sent, it is simulated identically. Its building count is a
  detector lane, so a town that grows differently shows up. The Natural Town Growth Workshop
  script is wrapped so its clock and random numbers follow the simulation:
  [DETERMINISTIC_SCRIPTS.md](DETERMINISTIC_SCRIPTS.md).
- A stop's load settings (`stopConfig`: unload only, maximum load) are not carried by a line
  replay yet ([KNOWN_ISSUES.md](KNOWN_ISSUES.md#replication-gaps)).

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
