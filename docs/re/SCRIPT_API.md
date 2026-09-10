# Game-script API: measured behaviour

What the Transport Fever 2 Lua API does that its documentation does not say. Each item
was measured in a running game (build 35924), and the mod's code depends on it.

**Read the official reference first:** <https://wiki.transportfever2.com/api/index.html>
(`api.type`, `api.cmd`, `api.engine`, with examples). It states plainly things that cost
this project crashes to rediscover, for example that `TransportVehicleConfig.vehicleGroups`
entries are group sizes that must sum to the vehicle count (omitting it hangs the sim).
Reverse engineering is for what has no binding: the UI, the command factories, the ECS
internals.

## Probing the API

- sol2 usertypes refuse `pairs()`, but members can be read by name. Try a list of
  candidate names against a real object from the loaded world; that shows real values and
  container lengths, which a blank `.new()` does not.
- `api.cmd.make.*` entries are tables with `__call`. A call with the wrong arguments fails
  with `stack index N, expected <type>, received <type>`, which walks you to the arity and
  argument types without dispatching anything (a maker only builds a Command). It cannot
  tell you what an argument *means*: `createLine`'s second argument is a colour, which only
  the documentation says.
- The 33 makers: `bookJournalEntry buildProposal buyVehicle connectTownsAndIndustries
  createLine createTowns deleteLine developTown instantlyUpdateTownCargoNeeds removeField
  removeTown replaceTerrain replaceVehicle reverseVehicle sellVehicle sendScriptEvent
  sendToDepot setAnimalState setCalendarSpeed setColor setDate setGameSpeed setLine setName
  setSimBuildingClosureTimeStamp setSimBuildingManualDevelopment setTownInfo setUserStopped
  setVehicleManualDeparture setVehicleShouldDepart setVehicleTargetMaintenanceState
  spawnAnimal updateLine`. There is no save maker and no separate "add vehicle to line":
  assignment is `setLine`.
- Component members read off real objects: `LINE` = `stops`, `waitingTime`,
  `vehicleInfo`; `api.type.Line.Stop` (not `api.type.LineStop`) = `stationGroup`,
  `station`, `terminal`, `loadMode`, `stopConfig`, `minWaitingTime`, `maxWaitingTime`,
  `waypoints`; `TRANSPORT_VEHICLE` = `line`, `stopIndex`, `state`, `depot`, `config`,
  `transportVehicleConfig`, `carrier`, `userStopped`.

## Crashes `pcall` cannot stop

| call | what happens |
|---|---|
| any entity-taking `game.interface.*` or `api.engine.*` call with a nil or negative id | `scripting::ReadNonNegativeEntity` asserts; the crash handler writes a ~800 KB minidump (a visible stall) before the Lua error reaches `pcall`. Guard every id: `type(id) == "number" and id >= 0`. |
| `game.interface.setBulldozeable` on an entity that is not a CONSTRUCTION | native assert. Check `api.engine.getComponent(id, api.type.ComponentType.CONSTRUCTION)` first. |
| `game.interface.bulldoze(<stop entity>)` | `assert(false)`, fatal. Remove edge objects with a proposal (`edgeObjectsToRemove`). |
| a scripted buy whose vehicle parts lack `autoLoadConfig` | native assert in `vehicle_util_engine::UpdateConfigFromModelIds`. |
| a proposal that routes a track through a road corner | `Crossing.cpp` anti-parallel assert ([PROPOSALS.md](PROPOSALS.md#level-crossings)). |
| rebuilding an edge that holds a line's stop without carrying the stop by id | lines left on dead ids; fatal assert ([PROPOSALS.md](PROPOSALS.md#edge-objects-stops-signals-waypoints)). |

## Constructions

- `api.cmd.make.buildProposal` accepts a script `ConstructionEntity` only when
  `params.seed` is present; otherwise it returns `false`.
- Always set `ConstructionEntity.name`; the child entities need it
  ([PROPOSALS.md](PROPOSALS.md#what-a-script-proposal-must-carry)).
- `game.interface.buildConstruction` builds the template's street pieces raw and
  construction-owned; replay whole placements through `buildProposal` instead.
- Station edits made from script go through
  `game.interface.upgradeConstruction(id, fileName, params)`, as the game's own
  `res/scripts/mission/constructionupgrader.lua` does (it clears `params.seed` first).
- A module edit in the UI keeps the construction's entity id. Do not assume
  `upgradeConstruction` does: one measurement saw the old id retire and a new entity appear
  at the same spot. Track constructions by position.
- `upgradeConstruction` on a modular station that was created by `buildConstruction`
  failed with "internal error" on a joining instance, while depots upgraded fine.
- `game.interface.getEntity(id).params` after apply differs from what was proposed:
  modules for unbuilt slots are pruned.

## Vehicles

- A vehicle parked in a depot is not a world entity. `game.interface.getEntities(...,
  {type = "VEHICLE"})` never lists it, and `game.interface.getDepotVehicles` errors for
  both the construction and its VEHICLE_DEPOT child. Use
  `api.engine.system.transportVehicleSystem.getVehiclesWithState(api.type.enum.TransportVehicleState.IN_DEPOT)`
  and match each vehicle's `TRANSPORT_VEHICLE.depot`, which is the VEHICLE_DEPOT child.
- The depot argument of a buy is that child entity (`CONSTRUCTION.depots[1]`), not the
  construction.
- `transportVehicleSystem.getVehicles()` is not callable; inside a `pcall` it looks like
  an empty result. For vehicles on the map use `game.interface.getEntities({radius = ...},
  {type = "VEHICLE"})`.
- Several purchases in the same tick bind to entities in an order that is not stable
  between instances; issue one buy per sim step.

## Lines

- The UI sends UpdateLine immediately after CreateLine, and one UpdateLine per stop
  operation, so every UpdateLine has to replicate, not only the final state.
- A replicated line exists (in `api.engine.system.lineSystem.getLines()`) a step after its
  createLine applies; anything keyed to it must wait for it.
- The route overlay stays stale after a stop is removed until the line is reselected
  (base game, cosmetic).

## Engine state and GUI state

- `game.interface.sendScriptEvent` exists only in the GUI state; it is nil in the engine
  (`update`) state.
- `guiUpdate` runs in a separate Lua state. Files are the only channel between it and the
  engine script.
- The game's stdout is buffered until exit, so `print()` output is invisible during a
  session; log to files.
- `os.getenv` and `io` are available to game scripts, and the working directory is the
  game folder.
- `require("mp.x")` resolves from the mod's own `res/scripts`. `package.loaded` would carry
  a previous game's module state into the next load, which is why the mod's modules are
  factories (`return function(CM, K, log) ... end`).
- Lua's limit of 200 local variables per function applies to a chunk's top level; the
  single-file script once crossed it and failed to load.

## Game time, speed and saving

- `game.interface.getGameTime().time` is in engine units, not seconds and not days: it
  advances 0.2 per sim step, and in the test save about 2 units per in-game day. It is the
  only clock two instances share (wall clock and per-instance tick counters are not).
- `api.cmd.make.setGameSpeed(n)` accepts any whole number, not only 0/1/2/4; fractions
  truncate ([GAME_LOOP_AND_UI.md](GAME_LOOP_AND_UI.md#simulation-pacing)).
- There is no save command in `api.cmd.make`; see
  [GAME_LOOP_AND_UI.md](GAME_LOOP_AND_UI.md#forcing-a-save-from-native-code).

## Mods and saves

- The game loads mods from `<game>\mods` and also from
  `userdata\<steamid>\1066780\local\mods`. A mod loaded from the per-user folder is recorded
  in saves under a `!`-prefixed id (`!mp_lockstep` instead of `mp_lockstep`), so a save made
  with it reports "mod is not properly installed" on a machine with the normal install.
- Mods are enabled per game. An existing save keeps the mod list it was saved with.
- `CONTINUE` loads the save named in `profile.lua` (`lastGame[2].saveGameName`), not the
  newest file.
