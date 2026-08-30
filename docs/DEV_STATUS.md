# TpF2 Multiplayer — developer status & resume guide (internal)
*(saved 2026-07-26; bug-fix pass 2026-08-06)*

Licensed under the [MIT License](LICENSE). Third-party components are listed in
[THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).

## RESUME HERE (end of 2026-08-07 session)

Run everything through **`tools/autotest.ps1`** — it launches both instances,
loads the save, runs a scenario and prints PASS/FAIL with no human involved.

**Verified in-game this session** (each has a scenario under `tools/scenarios/`):

| Feature | Scenario |
|---|---|
| Chained rail / node reuse | `basic_replication.txt` |
| Edge removal (demolish) | `edge_removal.txt` |
| Mid-span junctions / road intersections | `probe_midspan.txt`, `road_intersection.txt` |
| Station edits in place (`CONMOD`) | `station_edit.txt` |
| Line creation **and replication** | `create_line.txt` |

**Vehicle purchase replicates** (user-confirmed in normal play, 2026-08-10):
buy in one instance and it appears in the other. The long-standing sim wedge is
fixed -- see the sol2 by-value section below. `VEH_REPLAY_ENABLED` is on.

**Two channels are OFF pending bisection:** `EDGEDEL_ENABLED` and
`CONMOD_ENABLED` (both `false` in `mpbridge.lua`). Neither turned out to be the
cause of the "cannot build in the joiner" bug -- that was a native crash in my
own edge-adoption scan -- so they can probably be re-enabled, but only ONE at a
time with a test in between. They are destructive, so they stay off by default.

**NEVER walk the edge maps during a replay.** A scan that called `getComponent`
on every edge id right after a construction replay hard-crashed the joiner
inside `mpbridge.lua_update()`. It is a NATIVE crash -- `pcall` does not catch
it -- and it kills the whole game script silently: the joiner keeps rendering
and reports `Responding = True` while capturing nothing and ignoring the
player's builds. Use the O(1) `nearRecentConstruction` helper instead.

**Historic, now fixed:** dispatching `buyVehicle` from a game script used to
**wedge the sim thread**. Measured twice at the identical point (tick
452, right after the `sendCommand`), once with 26 units and once with **one** —
so it is *not* a payload-size or config-shape problem. The process keeps
answering the UI and reports `Responding = True` while its world is stopped, and
`pcall` never returns. `VEH_REPLAY_ENABLED` is now **false**; see the section at
the bottom before touching this. The watchdog detects the wedge via the sim-hook
tick counter (`Process.Responding` is useless here).

**Next pieces, in the order I'd take them:**

1. **Understand the `buyVehicle` deadlock** — leading theory is that it cannot
   be dispatched from inside the script tick. Cheapest test is to send it from
   a different entry point; the best evidence would be logging the exact config
   the *game itself* passes when you buy by hand, via the existing native hook.
   Do **not** simply re-run `buy_vehicle.txt`; that has now cost two instances.
2. **`ASSIGN`** — `setLine(vehicle, line, stopIndex)` to put a vehicle on a line
   and get it moving. Op is written, never run; it is blocked behind (1).
3. Line **deletion/modification** channels (`deleteLine` / `updateLine`
   signatures known, nothing emits them).
4. Native ECS reads inside the tick hook — foothold proven, sol2 binding at
   `0x140fa6850`.
5. Title-screen menu (task 8) — fully decoded, needs building.

**Environment notes:** the `alut.dll` proxy is installed (revert with
`tools/install_proxy.ps1 -Uninstall`). The `MPTESTING` save has accumulated
test debris — the scenario runner shifts each run onto fresh ground
(`tools/.autotest_offset`), so debris is spread along increasing x.

## API reference, measured from the running game (2026-08-07)

sol2 usertypes refuse `pairs()` — you cannot list a component's members. But you
*can* read them by name, so `mptest.lua` has `PROBE` ops that try a candidate
list against a **real object from the loaded world** and report what resolves.
That beats a blank `.new()`: it shows real values and container lengths. Run
`tools/scenarios/probe_*.txt` and read `mp_test_results_<inst>.txt`.

**All 33 commands** (`api.cmd.make`):

```
bookJournalEntry  buildProposal  buyVehicle  connectTownsAndIndustries
createLine  createTowns  deleteLine  developTown  instantlyUpdateTownCargoNeeds
removeField  removeTown  replaceTerrain  replaceVehicle  reverseVehicle
sellVehicle  sendScriptEvent  sendToDepot  setAnimalState  setCalendarSpeed
setColor  setDate  setGameSpeed  setLine  setName
setSimBuildingClosureTimeStamp  setSimBuildingManualDevelopment  setTownInfo
setUserStopped  setVehicleManualDeparture  setVehicleShouldDepart
setVehicleTargetMaintenanceState  spawnAnimal  updateLine
```

Note there is **no** `addVehicleToLine`/`setVehicleLine` — assigning a vehicle
to a line goes through `setLine`.

**Component shapes**, read off a real line in the test save:

| Type | Members |
|---|---|
| `LINE` | `stops` (vector), `waitingTime` (180), `vehicleInfo` |
| `Line.Stop` | `stationGroup` (entity), `station` (0), `terminal` (0), `loadMode` (0), `stopConfig`, `minWaitingTime`, `maxWaitingTime`, `waypoints` |
| `TRANSPORT_VEHICLE` | `line`, `stopIndex`, `state`, `depot`, `config`, `transportVehicleConfig`, `carrier`, `userStopped` |

The stop type is **`api.type.Line.Stop`**, nested inside `Line` —
`api.type.LineStop` does not exist.

### Reading a sol2 signature out of its error messages

`api.cmd.make.*` entries are **tables**, not functions — they have a `__call`.
Calling one with the wrong arguments produces an error that names the expected
type at a given stack index, and that is enough to walk to the real signature
without ever dispatching a command. Command *factories* only build an object;
nothing reaches the engine, so a wrong guess costs a log line rather than the
process. `createLine` went:

| Attempt | Error | What it told us |
|---|---|---|
| `createLine(line)` | `stack index 2, expected string, received sol.…Line` | first user arg is at **index 2** and must be a **string** |
| `createLine(player, line)` | `stack index 2, expected string, received number` | confirms it — the reported type is arg 1's |
| `createLine(name, line)` | `stack index 3, expected userdata, received sol.ecs::component::Line` | name accepted; arg 2 must be userdata, and a raw `Line` **component is not it** |

The last error leaks the C++ type it actually wants:
`std::variant<CmdData::SetGameSpeed, CmdData::SetCalendarSpeed,
CmdData::UpdateLogo, CmdData::CreateLine, …>`.

**And this is where black-box probing dead-ends — read the docs.** The real
signature turned out to be:

```lua
api.cmd.make.createLine(name, color, playerEntity, line)
--   name          string
--   color         api.type.Vec3f     <-- RGB colour
--   playerEntity  Entity
--   line          api.type.Line
```

Argument 2 is a **colour**. No amount of further probing would have suggested
that; the natural guesses (the Line, the player, a CmdData payload) are all
wrong, and passing `CmdData::CreateLine` — the type the error message itself
names — is rejected as "does not properly reflect the desired type". One fetch
of <https://wiki.transportfever2.com/api/modules/api.cmd.html> answered it.
Verified in-game: `createLine callback success=true`.

The docs also confirmed, exactly matching what the probe had accepted:

```lua
setLine(vehicleEntity, lineEntity, stopIndex)
buyVehicle(playerEntity, depotEntity, config)
sendToDepot(vehicleEntity, sellOnArrival)
updateLine(lineEntity, line)      deleteLine(lineEntity)      setName(entity, name)
```

Probing is still worth it — it is how you learn a call is *accepted*, and it
costs nothing when the docs are silent (as they are on `type.Line`'s stop
fields, which had to be measured off a real line). But reach for the docs the
moment an argument's *meaning* rather than its arity is in question.

Note the deliberate ordering of this hunt: each full launch-and-load cycle costs
~10 minutes, so remaining unknowns get batched into a single probe scenario
rather than discovered one per run.

**Station edits go through `game.interface.upgradeConstruction(id, fileName,
params)`** — not a proposal. This is exactly what the shipped
`res/scripts/mission/constructionupgrader.lua` does, including setting
`params.seed = nil` first, which this project already knew was fatal to reuse.
Editing a station does **not** create or destroy an entity; it rewrites the
construction's `params` in place, which is why edits were invisible to a capture
that only asked "is this id new?".

## CHECK THE OFFICIAL API DOCS FIRST

**https://wiki.transportfever2.com/api/index.html** — full reference for
`api.type`, `api.cmd`, `api.engine`, plus worked examples at
`/api/topics/examples.md.html`.

This was found late, after hours of reverse engineering and two game crashes
spent guessing at things the docs state plainly (e.g. `TransportVehicleConfig.
vehicleGroups` — "each entry is a group size; the sum must equal the total
vehicle count"; omitting it hangs the sim). Before disassembling anything that
has a Lua binding, look it up. Reverse engineering is for the parts with no
binding at all — the UI, the command factories, the ECS internals.

## What this is

A working prototype of true simultaneous multiplayer for Transport Fever 2,
built from scratch: engine reverse engineering → DLL injection → our own
reliable-UDP stack → Lua-API-based replication. Two live instances
(one normal, one in Sandboxie) replicate player actions in both directions.

## What works (all live-tested on the map)

- **Constructions** (depots, shipyards, town buildings): captured with
  fileName + world transform + params, replayed via
  `game.interface.buildConstruction`. Params are serialized; `seed` is
  stripped (else fatal assert).
- **Roads**: full geometry (node positions, tangents, street type id),
  replayed via `streetProposal` with node reuse within 1.5m.
- **Bidirectional**, ~250ms latency end-to-end.
- **Single mod** (`mod/mp_bridge_1`, installed as `mp_bridge_1` in the game
  mods dir): auto-detects instance identity from `tpf2_instance.txt`
  (written by the bridge DLL). Same mod in every instance.
- Vehicles: channel written (capture by model, replay via buyVehicle),
  lightly tested.
- **Time sync** (new 2026-08-06, not yet live-tested): host-authoritative game
  clock. `a` broadcasts `TIME speed=… t=… secday=…`; the client follows. Either
  side can *request* a speed change (`SPEEDREQ`) so both players can pause and
  fast-forward — the host adopts it and re-broadcasts, so there is one writer
  and no tug-of-war. Date drift is corrected in whole days via `setDate` past a
  2-day threshold; the client being *ahead* is only logged, since `setDate`
  cannot safely run the clock backwards.

## 2026-08-06 bug-fix pass

Six defects found by reading the code against the last session's on-disk
artifacts. All are fixed and component-tested; **none of this has been run in
the live game yet** — that is the next session's job (see *How to resume*).

1. **CRLF vs. Lua text mode desynced the events reader.** The bridge wrote the
   events file through an `_O_TEXT` handle, so every line ended CRLF, while the
   mod opened it with `io.open(..., "r")` — also text mode. `f:seek("end")`
   returned the on-disk byte count but `f:read` stripped the CRs, so `consumed`
   was one byte short per line and `eventsOffset` drifted backwards forever.
   Fixed on both sides: `_O_BINARY` in the DLL, `"rb"` in the mod.
2. **The tail thread wedged permanently on long lines.** `char line[256]` +
   `fgets`: on a longer line there was no trailing `\n`, so it rewound and
   re-read the identical line forever. Real capture files contain lines of
   3421 / 1567 / 760 bytes (modular station, harbor, modular terminal), and
   `NetEvent.text[239]` truncated them anyway. Now the tail reads lines of any
   length and the transport splits them into chunks reassembled on receipt.
3. **Chained rail edges deadlocked in `replayEdge`.** It reserved node0, then
   discovered node1 was busy and bailed — leaving node0 reserved with no
   proposal in flight, so that spot stayed pending forever and every later edge
   touching it retried endlessly. Rail is drawn as chains sharing endpoints, so
   it wedged on segment 2 every time, silently. Both endpoints are now resolved
   before either is reserved; reservations are released on failure and expire
   after `RESERVE_TIMEOUT` ticks.
4. **Per-DLL configs were never loaded.** `LoadConfig` built
   `tpf2_mp_<basename>.cfg`, i.e. it looked for `tpf2_mp_tpf2_bridge_a6.cfg` —
   a name nothing on disk ever had. Every bridge silently fell back to the
   shared `tpf2_mp.cfg`, which has no `instance=` and no relay settings, so the
   B→A file relay never ran at all and every A-side DLL claimed instance "a"
   and tailed the same file. The lookup now tries the real conventions in order
   and logs which file it used.
5. **Identity was latched once and never rechecked.** Both bridges write
   `tpf2_instance.txt` into the same directory; the last session had a `b`
   bridge injected into the unsandboxed process, leaving the real dir claiming
   `"b"` and the overlay claiming `"a"`. The mod now rereads identity every 60
   ticks and logs loudly when it changes, the DLL warns when it overwrites a
   different value, and `tools/inject_bridges.ps1` picks targets by sandbox
   membership instead of start order.
6. **Replayed edges echoed back to the peer.** `pollEdges` had no remote
   tagging, so an edge created by replay was captured and shipped straight
   back. Replayed geometry is now recorded and skipped on capture.

Also: the `pollEvents` diagnostic only printed when `size > 0`, so the
"events file is empty / wrong file" case — the one actually worth diagnosing —
produced no output at all. It now logs unconditionally, including the path it
is polling. `ser()` truncated nested params at depth 4 and emitted the string
`"?"` where the engine expects a table; depth is now 8 with an empty-table
fallback.

### Verified so far (outside the game)

- 13-line loopback round-trip through the real transport, including the actual
  3421-byte `modular_station` capture line and chunk boundaries at 1022–1025
  and 2046–2048 bytes: all byte-identical.
- Bridge DLL loaded standalone: logs `[cfg] loaded tpf2_bridge_a7.cfg`
  (previously it fell back silently) and a `data dir` that matches the mod's
  `BASE`.
- Events file written from a live UDP send: 3668 bytes, **CR=0**, and
  `sum(len+1 per line) == file size` — the invariant the Lua offset math needs.
- Mis-injection warning fires when the b DLL lands in the a directory.
- `mpbridge.lua` parses clean.

## Save / map transfer (new 2026-08-06)

The joiner needs the host's world before anything else can be shared. The host
(`instance=a`) serves its save over **TCP**; the joiner pulls it into its own
save directory.

Deliberately not on the event channel: that is line-oriented reliable UDP with
1 KB chunks and a 32-entry ack window, and a real save here is **178 MB**.

- Transfers `<name>.sav` (the world), `<name>.sav.lua` (mod script state — the
  save is incomplete without it) and `<name>.jpg` (load-menu thumbnail).
- Files land as `mp_host_<name>.*`, so a transfer can never overwrite the
  joiner's own saves.
- The save directory is auto-discovered under `Steam\userdata\*\1066780\local\save`;
  override with `save_dir=`.
- Set `auto_pull=1` on the joiner. Off by default — silently downloading 178 MB
  on every launch would be rude. Eventually this is what the menu's *Join*
  button triggers.

Verified locally: 40 MB save + both sidecars, all three byte-identical after
transfer, with host/joiner roles auto-elected. **Not yet run with a real
178 MB save or between two machines** — `peer_ip=127.0.0.1` assumes one box,
and remote play needs `xfer_port` reachable too.

The joiner still loads the downloaded save manually from *Load Game*; wiring
that to a *Join* button is part of the menu work.

## Automated testing (new 2026-08-06)

Driving two live instances by hand is why so much of this project is "built and
verified offline, untested in-game". `mptest.lua` fixes that: scripted in-game
actions plus an external driver that checks what actually replicated.

- **`mod/mp_bridge_1/.../mptest.lua`** — a *separate* game script from
  mpbridge.lua and fully pcall-walled, so a broken test can never take down the
  replication path it exists to test. Idle until a scenario file appears.
- Actions go through the same engine commands a player's clicks do
  (`buildConstruction` / `buildProposal`), so the **capture** path is exercised
  for real rather than faked by writing capture lines directly.
- **`tools/run_mptest.ps1`** — writes a scenario, waits, then reports what the
  host shipped vs. what the joiner received (reading the sandbox overlay for B).
- **`tools/scenarios/basic_replication.txt`** — depot, two joined road
  segments, then three *chained* rail segments: the exact shape that used to
  deadlock the replay side.

Scenario ops: `WAIT <ticks>`, `LOG <text>`, `SNAPSHOT <label>`,
`DEPOT <x> <y>`, `CON <x> <y> <file.con>`, `ROAD <x0> <y0> <x1> <y1>`,
`RAIL <x0> <y0> <x1> <y1>`, `END`.

```
powershell -ExecutionPolicy Bypass -File tools\run_mptest.ps1
```

### `tools/autotest.ps1` — the whole loop, no human (new 2026-08-07)

`run_mptest.ps1` assumes two loaded games and leaves the final check ("did the
joiner replay?") to a person reading stdout inside the sandbox. `autotest.ps1`
closes both ends: it launches both instances, loads the save in each via
synthetic clicks, waits for the bridges to peer, runs the scenario, reads the
joiner's stdout itself, and prints a single PASS/FAIL.

```
powershell -ExecutionPolicy Bypass -File tools\autotest.ps1
powershell -ExecutionPolicy Bypass -File tools\autotest.ps1 -SkipLaunch -KeepOpen
```

Things it encodes, each of which cost a debugging round:

- **NEVER RESIZE THE GAME WINDOW.** This is the big one. A game sitting on a
  fully drawn main menu, resized with `SetWindowPos`, drops straight back to a
  logo-only screen with **no menu at all** — and stays there. It is
  indistinguishable from a hang, and an earlier version of this script resized
  both windows to a convenient 1600x900 on launch, which meant it was breaking
  the very thing it was waiting for. Windows are now used at whatever size they
  open, and CONTINUE is looked up per size in `$ContinueFor` (measured, not
  computed — the menu is *not* laid out proportionally, the UI scale steps with
  resolution). An unknown size fails loudly and tells you to measure it.
- **Launch through Steam** (`steam://rungameid/1066780`), not the exe.
- **"Loading from file `<SAVE>`"** in the game's own stdout is the authoritative
  signal that CONTINUE was accepted. Working-set size is only used afterwards,
  to tell when the load has finished (~370 MB unloaded vs ~4.4 GB loaded).
- **Never click without checking the foreground.** A synthetic click goes to
  whatever window is on top at that point, not to a process. During development
  a capture came back showing a *browser* where the game should have been —
  meaning a click would have landed in someone's meeting. `ClickIfForeground`
  refuses to click unless the game genuinely owns the foreground, and logs each
  skip. Do not remove this guard.
- **Foreground.** Windows refuses `SetForegroundWindow` from a background
  process; attaching to the foreground thread's input queue makes it legal.
- **First click activates only.** Focus, then click — never one click.
- **The menu ignores the keyboard.** `{ENTER}` on the main menu does nothing, so
  there is no coordinate-free path; the mouse is unavoidable.
- **Deploy first, and expect a slow first launch.** The game loads from
  `steamapps\common\...\mods\`, not this repo, and reads Lua once at startup.
  After any deploy it logs `Mods changed, recreating data...` and rebuilds the
  whole asset cache before drawing the menu — minutes, not seconds. Budget for
  it or a slow launch reads as a failure.
- **Identity and pid** come from `tpf2_instance.txt` (`<id>\npid=<n>`) in each
  side's own view of the out dir, so it works after an a/b swap and under
  `-SkipLaunch`.
- **Virgin ground.** Re-running on used ground fails *everything*
  (`!proposalData.errorState.critical`). Every X in the scenario is shifted by a
  per-run offset persisted in `tools/.autotest_offset`.

**Read the summary carefully — it separates two different failures:**

```
host built      : 3 edge(s)
host rejected   : 2 edge(s)  <- game refused; NOT a replication bug
shipped -> got  : 5 -> 5
joiner replayed : ok=5 failed=0
RESULT: PASS
```

A build the *game itself* rejected never enters the replication path, so it
must not be counted against it. Rejections have two ordinary causes: terrain
(rail maxes out around 3–5% grade, and the auto-shift will eventually walk the
test onto a hill) and an endpoint that already carries a node (task 13). This
distinction matters — earlier "some rail builds are being completely missed"
reports were partly this: the host never built them.

Three consecutive runs on 2026-08-07 came back PASS: everything the host built
arrived and replayed, with echo suppression holding.

Line creation is **not** covered:
`api.cmd.make.createLine` exists in the command map but no shipped script calls
it, so its signature is still unknown — that needs discovering before lines can
be tested or replicated.

## Native sim-thread hook (new 2026-08-06, prototype)

First native foothold: a 21-byte detour on `GameSim::Step` (RVA `0x15aa00`),
which runs once per sim step on the Simulation Thread. That gives native code
the same "on the sim thread at a defined point in the frame" guarantee Lua's
`update()` has — the prerequisite for reading ECS state without racing the sim.

Enable with `sim_hook=1` in `tpf2_bridge_mp.cfg` (off by default in code, since
it patches game memory). Watch for `[simhook]` lines in `tpf2_bridge.log`.

Guards, all exercised offline: the exact 21-byte prologue is verified before
patching (a game update means refusal, not corruption), the host process must
be `TransportFever2.exe`, and the target range is bounds/readability checked.

Tested against a stand-in with a byte-identical prologue: 20,000 calls with
varied arguments all returned correctly, handler fired exactly once per call
and saw both arguments, and a mismatched prologue was refused with the target
left intact. **Not yet run in the game.**

Next: resolve `getComponent` / `forEachEntityWithComponent` from the sol2
binding at `0x140fa6850` and call them from inside the handler.

## 2026-08-07: verified live, two instances

First end-to-end run with both instances up. Everything below was confirmed in
the game, not just offline.

- **Rail replication works.** `EDGE kind=rail` captured on A, replayed on B as
  `edge replay (rail): success=true`. Root cause of the long-standing failure
  was one line: `e.type` selects street(0) vs track(1) and was hard-coded to 0,
  so every "rail" became a road — on capture *and* replay. `e.comp.type` must
  stay 0; setting it to 1 fails outright. A valid `streetEdge` is mandatory on
  every edge (else `ResTypeRep<StreetType>::Get(-1)` asserts) but is discarded
  on the resulting track edge.
- **Native sim hook live in both instances** — `GameSim::Step` patched, steady
  **5 Hz** fixed timestep, `frameTime = 200000` µs.
- **Auto-identity across the sandbox boundary** — A→`a`/7771, B→`b`/7772, no
  manual injection, no possibility of the a/b swap that wrecked the last session.
- **Proxy loads pre-menu** — `alut.dll` forwarding works, bridge up before the
  title screen.
- **Echo guard, byte-offset tracking, TIME dedupe, keepalive-gated transport**
  all confirmed working (`peer=up pending=0 dropped=0/0`).
- **Script update rate is 10 Hz**, not the 60 Hz originally assumed. All
  tick-based constants were retuned.

### Known-good vs. still rough

- Rail on a >~7% grade is refused by the engine (correctly) — pick flat ground
  for tests, and check the z values in the result lines when one fails.
- Constructions that auto-generate edges (a depot's access road) have those
  edges captured and shipped separately, so the peer tries to build a road that
  its own `buildConstruction` already created. Needs suppressing.
- Test scenarios must run on **virgin ground**. Re-running over a previous
  run's output makes `buildConstruction` hit
  `!proposalData.errorState.critical` — caught by pcall, but it writes a
  minidump and every step fails.

## Live-test findings, 2026-08-07 (two instances, hours of manual testing)

Verified working in-game:

- native `GameSim::Step` hook, 5 Hz, both instances
- auto-identity across the sandbox boundary (no manual injection, no a/b swap)
- constructions replicate **and are clickable** — replayed builds had no
  `PLAYER_OWNED` (buildConstruction takes no player arg); `setPlayer(entity,
  player)` after the build fixes it
- rails build as rails, with electrification (`cat=1`)
- play/pause propagates both ways
- save transfer, chunked transport, echo suppression, byte-exact offsets

### Not working, with causes

| Symptom | Cause |
|---|---|
| Buying a vehicle never replicates | A vehicle **in a depot is not a world VEHICLE entity**. `getEntities({type="VEHICLE"})` returned exactly 79 with identical ids before and after a purchase, in both instances. Polling cannot see purchases at all. Needs the depot inventory, or the native `buyVehicle` tap (M2: RVA `0x9dca00`). |
| Demolishing rail/road does nothing | Removal detection was only built for **constructions**. Edges have none. **Addressed** — see set diff below. |
| Cannot branch/diverge from existing rail | Two causes. The harness never reused an existing endpoint node (fixed, task 13); and a junction needs the *removal* half of the split to replicate (addressed by the set diff). Connecting into the middle of a span is still not handled. |
| Track upgrades, station edits | Mutations are invisible: capture asks "is this id new?". There is no `edgesToUpdate` either, so replay must be remove+add. **Addressed for edges** (`EDGEMOD`); stations still open. |
| Some rail builds silently dropped | Partly **not a replication bug at all**: the harness's own `buildProposal` was returning `success=false` and those edges were never built locally. Two real causes found — no node reuse (fixed), and terrain too steep for rail. The automated summary now separates "host rejected" from "lost in transit" so this cannot be misread again. |
| Clocks drift ~1 day | Commands apply on arrival, not at an agreed tick. `setDate` correction is a band-aid; only tick-scheduled replay fixes it. |

### The common root

Four of those came from one design limit: **the edge channel detected new ids
instead of diffing the edge set**.

## Edge channel as a set diff (2026-08-07)

`pollEdges` now diffs the whole edge set each sweep instead of asking "is this
id new?", which is what three of the four symptoms above actually needed:

| Change | Emits | Fixes |
|---|---|---|
| id present, not seen before | `EDGE` (as before) | — |
| id gone since last sweep | `EDGEDEL kind p0 p1` | demolish; and the stale half of a junction split |
| id same, properties differ | `EDGEMOD kind p0 p1 type…` | upgrades — electrification, bus lanes, tram track |

Notes that matter for anyone touching this:

- **A junction is a split, not an add.** Building into an existing edge makes
  the game *remove* that edge and add two halves. The two halves always shipped;
  the removal did not, so the peer kept the original edge lying across both —
  "a rail I built got cut in two in the other instance". Removal detection is
  what fixes that, not new splitting logic.
- **Removals are matched by geometry, never by id.** Edge ids depend on build
  order and are not comparable across instances. Both endpoint orientations are
  tried, since `node0`/`node1` order need not agree between the two games.
- **There is no `edgesToUpdate`.** The only bound proposal fields are
  `edgesToAdd` / `edgesToRemove` / `nodesToAdd` / `nodesToRemove` — verified by
  searching the shipped binary for the sol2 field names, not by guessing. An
  upgrade is therefore a remove **plus** an add in one atomic proposal, reusing
  the existing node ids so the edge keeps its connections.
- **Mutation scanning is throttled.** Re-reading properties for all ~9,600 edges
  every sweep is far too expensive, so only a 300-edge slice is checked per
  sweep, wrapping around. Adds and removes stay free — they fall out of the id
  set alone. Worst-case latency for an upgrade to cross is therefore ~15 s.
- Echo suppression needed two more sig tables (`remoteEdgeDelSigs`,
  `remoteEdgeModSigs`): replaying a peer's removal makes the edge vanish locally
  too, which the next sweep would otherwise report as a fresh local demolition
  and send straight back.

### Verified in-game, 2026-08-07

Both run automatically via `autotest.ps1`; both came back PASS.

**Chained rail (`basic_replication.txt`)** — the three-segment chain that had
always dropped its middle segment now builds completely, because `actEdge`
reuses the endpoint node instead of trying to create a second one on top of it:

```
STEP 11  rail -990,-3250 -> -940,-3250  nodes=new/new              success=true
STEP 13  rail -940,-3250 -> -890,-3250  nodes=reuse:281681/new     success=true
STEP 15  rail -890,-3250 -> -840,-3250  nodes=reuse:70775/new      success=true
```

All three shipped and replayed on the joiner (`ok=5 failed=0`).

**Edge removal (`edge_removal.txt`)** — builds two separate rails, deletes only
the first. Host `captured edge removal: rail` → `EDGEDEL kind=rail p0=… p1=…`
arrived at the joiner → `edge removal replay: success=true`. Edge counts across
the run prove the match was by geometry and not indiscriminate:

```
before 9644  ->  after-build 9646 (+2)  ->  after-delete 9645 (-1)
```

Exactly one edge removed; the second rail survived. Demolish replicates for the
first time.

**Mid-span junctions (`probe_midspan.txt`)** — also PASS.

First the question was settled by measurement rather than assumption:
`buildProposal` **rejects** an edge whose endpoint lands mid-span (edges 9646
before, 9646 after, `success=false`). The interactive track tool splits the edge
for you; a raw proposal does not. So `actEdge` now does it explicitly — remove
the old edge, add both halves around a new node, hang the new edge off it, all
in **one** proposal so the network is never momentarily broken. Halves inherit
the original's properties and get Hermite tangents scaled by the split
parameter, so a curved edge keeps its shape (visible in the capture: tangent
`100,0,2.75` splits into two `50,0,1.375`).

```
STEP 7  rail … nodes=new/new split:281676@0.50  success=true
edges 9641 -> 9643   (one edge became two, plus the branch)
```

It replicates: the joiner replayed `edge removal replay: success=true` followed
by all three edges.

**Emission order matters and is load-bearing.** A split produces one removal and
two halves *in the same sweep*. If the halves are replayed first they each land
mid-span on an edge the peer has not deleted yet — which the game rejects, per
the measurement above. `pollEdges` therefore emits **removed → added → changed**,
in that order. Do not reorder those blocks.

Still not addressed: station edits. `EDGEMOD` is implemented on both sides but
**not yet exercised** — the scenario language has no op that upgrades an edge,
so nothing has ever produced an `EDGEMOD` line.

### Station edits (`CONMOD`) — verified 2026-08-07

```
host:   CONMOD p=1400.000,-3400.000,4.950 file=depot/road_depot_era_a.con
               params={["paramX"]=1,["paramY"]=0,["seed"]=-395,["upgrade"]=true,["year"]=1850}
joiner: [mpb-b] station edit replay id=281674 ok=true
```

Two wrong turns worth remembering, because both failed *silently*:

- **The filter was `PLAYER_OWNED`, and that is wrong.** `buildConstruction`
  takes no player argument and produces an **unowned** construction — which is
  precisely why the replay path already had to call `setPlayer` afterwards. So
  every construction this project creates failed that test and no edit was ever
  captured. The correct filter is the one shipped `constructionupgrader.lua`
  uses: skip anything with town buildings attached.
- **A priming race.** The edit scan walks ~120 constructions per sweep, so a
  full cycle over this save takes ~22 s. A station built and then edited inside
  that window is first sampled *after* the edit, leaving no previous value to
  diff against — the edit vanishes. New constructions are now primed at the
  moment they are first seen, while the entity is already in hand.

Also: `ser()` sorts its keys. `pairs()` order is arbitrary and can differ
between two calls on the same table, which would make the param diff fire on
every sweep and spam the peer with meaningless edits.

**Known issue, diagnosed but NOT fixed:** the run still reports FAIL
(`replayed ok=4 failed=1`) even though the edit itself succeeds.
`upgradeConstruction` rebuilds the construction, which destroys and recreates
its *own* access road — so the sweep sees one `EDGEDEL` plus one `EDGE` for
geometry the construction owns, ships both, and both fail on the joiner because
its own `upgradeConstruction` has already regenerated that road. The failures
are inert (nothing is lost or corrupted), but they make a passing run look
broken.

The tempting fix — suppress edges near a construction — is the one that already
caused a regression once: it dropped the road *split* edges a depot needs when
it snaps onto an existing road, so depots arrived unconnected (see the note in
`pollEdges`). The narrower fix is to suppress only *removals* within a short
window of a construction event at that spot, leaving additions flowing. Not
attempted yet, deliberately: it wants its own test rather than being bolted on
at the end of a session.

## Line replication (new 2026-08-07)

A fourth channel, alongside constructions, edges and time.

```
LINE wait=180 stops=1234.5,-678.9;2345.6,-789.0 name=My Bus Route
```

- **Stops travel as station-group COORDINATES**, never ids — same rule the edge
  and construction channels follow, because entity ids depend on build order and
  are not comparable across instances. The joiner resolves each coordinate back
  to a station group within 20 m and refuses the whole line if any stop cannot
  be located, rather than creating a line with a wrong stop.
- **`name` is last in the line** because it can contain spaces.
- **Priming matters.** The test save already carries 26 lines; without
  `linePrimed`, the first sweep after load would ship every one of them to the
  peer. Same trap as the edge channel.
- Replay is `createLine(name, Vec3f colour, playerEntity, line)` — see the
  signature section above for why argument 2 is the one that cannot be guessed.
- `Line.Stop` fields were **measured off a real line**, since the docs describe
  `api.cmd` but not `type.Line`'s contents: `stationGroup`, `station`,
  `terminal`, `loadMode`, `stopConfig`, `minWaitingTime`, `maxWaitingTime`,
  `waypoints`. The type is nested — `api.type.Line.Stop`, not
  `api.type.LineStop`.

**Verified in-game 2026-08-07:**

```
host    createLine callback success=true
        LINE wait=180 stops=-4096.3,-20919.6;-529.1,-23842.9 name=MP Test Line
joiner  [mpb-b] line replay 'MP Test Line' (2 stops): success=true
```

The stops crossed as coordinates and the joiner resolved them against its own
station groups — which is the whole point, since its ids differ.

Not yet handled: line *deletion* and *modification* (`deleteLine` / `updateLine`
signatures are known but no channel emits them), and vehicles assigned to lines.

## BLOCKED: GameInputSvc holds the foreground (2026-08-07, end of session)

GUI automation stopped working part-way through the session and the cause is
**machine state, not this project**. A titleless window owned by the Windows
`GameInputSvc` (GameInput / controller service) holds the foreground and will
not yield, so the game never becomes the active window and clicks only ever
*activate* it instead of pressing anything.

Everything reasonable was tried:

| Attempt | Result |
|---|---|
| `SetForegroundWindow` + `AttachThreadInput` (the usual trick) | fails, foreground stays on the service window |
| ALT-tap + `SPI_SETFOREGROUNDLOCKTIMEOUT=0` (the other usual trick) | fails |
| `PostMessage` WM_LBUTTONDOWN/UP to bypass focus | ignored — TpF2 reads raw input, not window messages |
| `Stop-Service GameInputSvc` / kill the process | **Access denied** — needs elevation |

`WindowFromPoint` confirms the click coordinate is over the game and the menu is
drawn with CONTINUE where expected, so the coordinates are right; the clicks
simply do not register while another window owns focus.

**To unblock:** reboot, or stop `GameInputSvc` from an elevated shell
(`Stop-Service GameInputSvc` — it is Manual start, so it restarts on demand and
only affects controller input).

### `--script` decoded (static analysis, 2026-08-07)

The binary has a **`--script`** switch. Decoded from the disassembly rather than
guessed — the string at `0x142f18b30` has exactly one reference, at
`0x1400b57f7`, inside the startup function `0x1400b4fc0` (the one that also
handles `mods-enabled` / `DefaultRenderPass`):

```asm
mov  rax,[rsp+0xf8]      ; parsed-args vector<std::string>
sub  rax,rsi
sar  rax,5               ; /32 -> element count
cmp  rax,2 / jb  skip    ; needs at least 2 args
cmp  qword [rsi+0x10],8  ; argv[0] length == 8
lea  rdx,"--script"
call memcmp / test/jne skip
lea  rdx,[rsi+0x20]      ; argv[1] -- the next std::string
call 0x140661240         ; hand the path over
```

Two things fall out of that:

- **`--script` must be the FIRST argument.** The code tests `argv[0]` only; it
  does not scan the argument list.
- Following the call chain, `0x140661240` → `0x140c173e0`, and that function
  references the strings **`"update"`** and **`"handleEvent"`**. Those are game
  *script* callback names, so `--script <file>` registers a Lua file with the
  same lifecycle as `res/config/game_script/*.lua` — an extra game script
  without installing a mod.

Useful, but note what it does **not** buy: a game script's `update` runs once a
game is loaded, so this does not obviously press CONTINUE for us. It also could
not be exercised here at all — launching `TransportFever2.exe` directly does not
reach the menu on this machine (it needs to come from Steam), and setting a
Steam launch option needs the Steam UI, which needs focus.

### The replay path could not split edges — FIXED (2026-08-10)

Mid-span junctions were marked verified on the strength of a harness test. That
was wrong, and it matters:

- `mptest.lua`'s `actEdge` **can** split — and the test exercised exactly that.
- `mpbridge.lua`'s `replayEdge` **cannot**. `resolveNode` does
  committed → reserved → `findNodeNear(1.5 m)` → else **create a new node**.
  There is no notion of an endpoint lying partway along an existing edge.

Since `buildProposal` rejects a mid-span node (measured), any road the host
snaps onto an existing road is refused on the peer — observed `success=false`
on 7 of 8 road edges. The host captures **no** `EDGEDEL`, because the host is
snapping to geometry that already exists; it is the *peer* that would have to
split. This is the user-visible "road intersection bug", and it has been broken
from the start rather than being a regression.

**Fixed:** the splitting is ported into `replayEdge`. When an endpoint resolves
to no node but lies on an existing edge, the proposal removes that edge and
re-adds both halves around a new node — Hermite tangents scaled by the split
parameter, halves before the new edge, removal in the same proposal. Confirmed
in real play: `edge replay (road): split 281676@0.50` then `success=true`.

The lesson for the test harness: a scenario that drives `actEdge` proves the
harness, not the replication path. Cover the path a *player* takes — build in
one instance and assert the other, which is also what task 17 is about.

### Harness trap: peer=up is not "ready to replicate"

The bridge DLLs are injected early and peer with each other *during loading*,
long before the Lua game script starts. A scenario run in that window is lost
for good — the joiner's mod primes its read offset to the **current end** of the
events file on startup, so anything that arrived first is skipped rather than
replayed. This produced a genuinely confusing run where the joiner's events file
contained every line and reported `consumed=0`. `autotest.ps1` now waits to see
the joiner's mod actually ticking (a `pollEvents` line) before running anything.

### Process note

Two regressions were introduced during this session by patching faster than
they could be verified: construction-edge suppression (dropped the road-split
edges a depot needs, so depots arrived unconnected) and a `getComponent(-1,…)`
crash on `depot = -1`. Both are fixed. Several bugs stayed invisible for a long
time because `pcall` was swallowing errors with no logging — logging has been
added where found, but assume there are more.

## Open issues

1. **Nothing above is confirmed in-game yet.** Rail replay in particular is
   believed fixed (issue 3) but unproven on the map.
2. **Town-growth constructions replicate too** (noise; harmless collisions).
3. **Road connections** work for replicated/shared roads; town roads diverge
   between instances over time (sim divergence, same seed ≠ same sim).

## How to resume (per session)

1. Launch instance A (normal), instance B sandboxed
   (right-click TransportFever2.exe → Run Sandboxed → GameAgent).
   NOTE: GameAgent box had `Template=BlockPorts` removed (backup:
   C:\Windows\Sandboxie.ini.bak) — required for UDP into the sandbox.
2. Load saves with **"MP Bridge"** enabled in each.
3. Inject — one DLL per instance now, chosen by sandbox membership, not by
   start order:
   ```
   powershell -ExecutionPolicy Bypass -File tools\inject_bridges.ps1
   ```
   Add `-DryRun` to see the pid→DLL mapping without injecting. The script
   refuses to run unless it can positively identify which process is boxed,
   and verifies afterwards that the real dir reads `a` and the overlay `b`.
   (The old three-injection `a3 + a6 + b5H` sequence is obsolete: UDP works in
   both directions, so the file relay is unnecessary and the duplicate A-side
   bridges just double-tailed the same capture file.)
4. Build stuff; watch `recon/m4/out/tpf2_events_*.txt`, `tpf2_bridge.log`, and
   the per-instance stdout:
   - A: `Steam\userdata\<steamid>\1066780\local\crash_dump\stdout.txt`
   - B: same path under `C:\Sandbox\<user>\GameAgent\drive\C\...`

   The line to watch is `[mpb-<inst>] pollEvents: size=… offset=… consumed=…
   retry=… file=…`. `size` growing with `consumed` staying 0 means the mod is
   reading a file nobody writes — check the identity lines directly above it.

## Architecture (short)

- **Lua mod** does ALL engine interaction (capture-by-diff via getEntities +
  edge maps; replay via buildConstruction / streetProposal / buyVehicle).
- **Bridge DLL** is a dumb pipe: file tail ↔ reliable UDP ↔ file write.
  Bidirectional UDP; the file-relay path (`tail_file` / `relay_out` in a cfg)
  still exists but is not needed.
- Full history: `docs/` (REPORT.md = engine recon, M1 = symbol map,
  M2 = live instrumentation, M4a = transport, M5 = bidirectional proof).

## Build & layout

The project tree and the live tree under
`Steam\steamapps\workshop\content\1066780\3710243057\recon\m4\` are kept in
sync (same `src/`). The DLLs must run from the workshop `out/` directory,
because they write their data files next to themselves and the mod's `BASE`
constant points there.

- `bridge\build_v7.bat` — builds `tpf2_bridge_a7.dll` and `tpf2_bridge_b7H.dll`
  (MSVC BuildTools 2022). Copy both to the workshop `out/` dir after building.
- `bridge\out\tpf2_bridge_a7.cfg` — A-side config. B is compiled with
  `HARDCODE_B` and needs no config (Sandboxie file isolation made sidecar
  reads unreliable inside the box).
- `backup\2026-08-06\` — pre-change `mpbridge.lua` and the last session's
  events file.

## Tools

- `tools/inject_bridges.ps1` — pid-checked injector (see above).
- `tools/extract.py`, `tools/find_hooks3.py`, `tools/walk_up.py` — binary
  recon pipeline (needs `python -m venv venv && pip install capstone pefile numpy`)
- `tools/luacheck.py` — rough Lua structure check. **Note:** it ignores its
  argument and always reads the installed copy, and it counts `for … do` as two
  opens so it never reports balanced. For a real check use a Lua parser.
- `bridge/*.bat` — MSVC build scripts (VS2022 BuildTools at
  `C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools`).

## Roadmap (remaining)

0. **Determinism (M3) is parked** by decision, 2026-08-06 — worst case the game
   gets patched (REPORT.md §6 already names the mitigation: force the job
   scheduler single-threaded, seed the stochastic systems identically). The
   probe is fixed and deployed for whenever it is picked up; note that until
   then, events still replay *on arrival* rather than at an agreed tick, which
   diverges the two sims regardless of how deterministic the engine is.
1. Confirm the fixes in-game, rail first; then time sync (pause on one side
   should pause the other).
2. **Title-screen Multiplayer menu** — in progress, see `docs/M6_MENU_UI.md`.
   Hook target `UI::CMenuUI::CreatePageMain` at `0x140667bc0`; button factory
   `0x1407c5d30` decoded; handler-attach site `0x1422518f0` located. Early
   injection is solved via the `alut.dll` proxy (`tools/install_proxy.ps1`) and
   the bridge now self-assigns `a`/`b` from port availability. Remaining: the
   callback object's vtable layout, the layout-attach call, and routing the
   edited peer IP/port to the bridge before `Net_Init`.
3. **Native state reads** — `docs/M7_NATIVE_STATE.md`. The Lua API cannot see
   citizens at all; the native `ComponentType` enum has ~70 entries including
   `SIM_PERSON`, `MOVE_PATH`, `RAIL_VEHICLE`, `PLAYER_OWNED`. Entry point is the
   sol2 binding at `0x140fa6850` (contains both the name→id map and
   `getComponent` / `forEachEntityWithComponent`). Tick hook located:
   `GameSim::Step` at RVA `0x15aa00`, steal 21 bytes.
4. Lines (createLine/updateLine events) — M2 already mapped the create-line
   factory to RVA `0x9dcde0` with the line name in cleartext, which is the
   missing piece for both replication and the test harness.
3. Demolish events + conflict rollback.
4. Company/ownership layer (fork of the hotseat mod: remote player = AI
   company entity, ownership protection, shadow balances, leasing).
5. Determinism validation (M3) for strict sim sync.

## buyVehicle wedges the SIM thread, not the process (measured 2026-08-07)

Dispatching `buyVehicle` with a config copied wholesale from an existing
26-part consist **hung instance A's sim thread**. What matters is how it
presented:

```
[simhook] ticks=428 (+50 in 10s)     <- healthy
[simhook] ticks=452 (+24 in 10s)     <- STEP 2 ran at tick 451
(no further tick lines, ever)
```

- The process kept answering the UI and reported **`Responding = True`** for
  minutes afterwards. Memory even crept up. By every ordinary Windows liveness
  probe the game was fine — its world had simply stopped.
- `pcall` cannot see it. The Lua call never returned, so the harness wrote no
  `STEP 3` line at all; the last thing on disk was the log emitted *before* the
  dispatch, which is the only reason the cause is known.

**So `Process.Responding` is the wrong watchdog** and `autotest.ps1` no longer
uses it as the primary signal. The sim hook already logs `ticks=N` every 10 s
from inside `GameSim::Step` — that counter is the sim's pulse. The watchdog now
samples it across 25 s and declares a wedge if it has not advanced, then kills
the instance and terminates the sandbox so nothing is left stuck.

The harness also now caps a test purchase at 4 units and logs the model and
compartment counts *before* dispatching, since a 26-part consist was never a
reasonable payload for proving the channel works.

### It is NOT the payload size — a single unit wedges it too

Retried with exactly **one** unit, same real `modelId`, config built to the
documented shape (`loadConfig = -1` "automatic", `vehicleGroups` summing to the
unit count). It wedged **identically** — ticks frozen at 452 again, no `STEP 3`,
process still `Responding = True`.

```
buyveh: sample vehicle 258211 reports 26 unit(s), depot 244147, comps=1,1,1,...
buyveh: trimming 26 unit(s) to 1 -- a large consist is not a test payload
[simhook] ticks=452 (+16 in 10s)      <- and then nothing, for 28 s
```

So the 26-part consist was a red herring and the trim fixed nothing.

### The call site is not the cause either — tested and disproven

The next theory was that `buyVehicle` cannot be dispatched from inside the
engine-state script tick without deadlocking. That was testable: a game script
also gets `guiUpdate`, which runs in the **GUI state** — a separate Lua state,
which the `api.cmd` docs say also carries `api.cmd`. The two states share no
memory, so the request is handed over through a file, like everything else that
crosses a boundary here.

Result — half right, and the useful half is the part that failed:

```
STEP 3 OK tick=452 | buyveh QUEUED for the GUI state: depot=244147 units=1 models=3591
  [GUI STATE] dispatching buyVehicle: depot=244147 models=3591
  [GUI STATE] sendCommand returned ok=true      <- caller did NOT deadlock
[simhook] ticks=452 (+22 in 10s)                <- and then nothing, 28 s
```

Dispatching from the GUI state **does** remove the Lua-side deadlock: the call
returns, `STEP 3` completes, the script carries on. But **the sim thread still
wedges** while processing the command, at the same tick as ever. So the call
site was a symptom — from the engine state the caller blocked because the sim
thread was both the victim and the caller.

**The fault is in the command/config itself, applied asynchronously on the sim
thread.** That also means no amount of moving the call around will fix it.

### The reference config, and what it ruled out (2026-08-10)

`probe_vehcfg.txt` dumps a vehicle the GAME built next to what the harness
constructs. No manual purchase needed -- the save is full of game-built
vehicles, and their configs are the same ground truth.

```
                 reference (game-built)        ours
vehicleGroups    {1,1,1,...} one per vehicle   {1}      (identical for 1 unit)
loadConfig       {0}                           {-1}     <- WRONG
purchaseTime     34699000                      55234    <- WRONG (unit)
maintenanceState 0.4468 (worn)                 1.0
reversed/color/logo  false / <userdata> / ""   NOT SET  <- WRONG
```

Two real bugs came out of that:

- **`loadConfig` must be `0`, not `-1`.** The docs describe `-1` as "choose
  automatically", and taking that at face value was a regression on my part --
  `mpbridge.lua` already had `0`, and the game itself never emits `-1`.
- **`purchaseTime` is MILLISECONDS.** A reference vehicle showed `34699000`
  against a current `getGameTime().time` of `55234`; it cannot have been bought
  in the future, so the field is `time * 1000`. (`mpbridge.lua` already did
  this too.)

**Both were fixed, and buyVehicle still wedges.** Setting the remaining unset
fields (`reversed=false`, `color=Vec3f(1,1,1)`, `logo=""`) did not help either.
Every field observable on the reference config now matches, and the sim still
freezes at the buy tick with no callback.

So the config *contents* are not the cause either -- or at least not any field
that can be read off an existing vehicle. Five hypotheses are now dead:
payload size, call site, command family, `loadConfig`/`purchaseTime`, and the
unset part fields.

**The one source of ground truth left is the command the game itself
dispatches.** The native `buyVehicle` hook (RVA `0x9dca00`) already fires on the
factory; extending it to dump the actual argument bytes during a *manual UI
purchase* would show what differs from a script-built config -- including
anything not exposed to Lua. That needs a human at the keyboard to click Buy.

### Narrowed: it is the config, not the command family

The discriminating test (`veh_cmd_probe.txt`): dispatch `setLine` — a vehicle
command taking **only entity ids, no config** — from the same GUI state.

```
[GUI STATE] setLine probe: moving vehicle 137409 from line 134269 to line 22426
[GUI STATE] setLine sendCommand ok=true
[GUI STATE] setLine callback success=true
[simhook] ticks=627 (+50 in 10s)   ... 671 (+44 in 10s)    <- sim ALIVE
```

It worked, the callback reported success, the sim kept ticking, and all six
scenario steps completed. So:

| | dispatched from GUI state | sim |
|---|---|---|
| `setLine` (ids only) | `success=true` | **alive** |
| `buyVehicle` (carries a config) | never calls back | **wedged** |

**Vehicle commands from a script are fine. `TransportVehicleConfig` is what is
wrong.** That is a much smaller problem than "purchases are impossible", and it
means the remaining work is to find which field the engine rejects — not to
rebuild the purchase path natively.

Also worth noting: **assigning a vehicle to a line works.** Getting a vehicle
running is therefore blocked only on acquiring one.

The one experiment that would settle it needs a human: buy a vehicle **by hand
through the UI** while the native `buyVehicle` hook (RVA `0x9dca00`) logs the
config the game itself passes, then diff that against what this harness builds.
Candidate suspects, in order: `purchaseTime` (currently game time — units may be
wrong), `loadConfig = -1` ("automatic" per the docs, but the older code used
`0`), and the unset `VehiclePart` fields `reversed` / `color` / `logo`.

`VEH_REPLAY_ENABLED` is now **false**. Leaving it on means an incoming `VEH`
line silently wedges the *joiner*, which is worse than not replicating
purchases at all.

Next things to try, cheapest first, none of them "retry the same call":

1. Dispatch from **outside** the script tick — e.g. queue the purchase and send
   it from a different entry point — to test the deadlock theory.
2. Compare against a purchase made by hand through the UI, with the native
   buyVehicle hook logging the exact config the game itself passes. That gives a
   known-good payload to diff against, which no amount of guessing will.
3. If both fail, treat purchase as native-only: the hook already *captures*
   buys; the missing half is a native *replay*.

## POST-MORTEM: the echo storm that deleted real track (2026-08-10)

The worst bug of the project so far, and it was self-inflicted. Recorded in full
because the shape of the mistake matters more than the fix.

**Symptom the player saw:** "I built some rails and a station and the rails
bugged out construction."

**What actually happened:**

1. The host built a station. It spawned its own track edges.
2. The joiner replayed the `BUILD`. The construction created edges on the
   joiner too -- but via `buildConstruction`, **not** an edge proposal, so no
   proposal callback ever recorded them.
3. The joiner's next sweep saw them as brand-new *local* builds and shipped them
   back. Measured: **the joiner built nothing and captured 139 edges.**
4. Its set-diff also read the construction churn as *removals* and emitted
   `EDGEDEL`, which the host executed -- `edge removal replay: success=true`,
   on the player's real rails.

Echo suppression should have stopped step 3, but `EDGE_SIG_TTL` was 5 s and a
station's burst takes far longer to replay, so every signature expired first. It
fired 7 times against 139 captures.

**Fixes:** replayed constructions now *adopt* the edges they create; the TTL is
60 s; and removals are never originated by a side that replayed anything in the
last 30 s. Verified: the same scenario now echoes **zero** edges and zero
removals.

### The actual lesson

An addition that turns out to be redundant is harmless -- the peer just reports
"already present". A **deletion is not**. `EDGEDEL` was added without that
asymmetry being designed for, and without a kill switch, straight into a live
save. The echo bug existed before `EDGEDEL`; it was merely *noisy*. Adding a
destructive channel is what turned it into data loss.

**So: every destructive channel gets a flag, defaulted off, from its first
commit.** `EDGEDEL_ENABLED` and `CONMOD_ENABLED` now exist for exactly this, and
are **off**. `CONMOD` is doubly dangerous -- `upgradeConstruction` *replaces* a
construction rather than editing it, and it matches its target by position
(10 m) plus filename, so a stray one can swap a building the player just placed.

Currently under bisection: a report of "it builds, then nothing happens, and the
next one of the same type is broken", which fits `CONMOD` landing on a
just-placed construction. Turn the flags on ONE at a time when testing.
