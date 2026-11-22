# Sandbox mode: its tools and what multiplayer needs from them

Build 35924. Measured on 2026-09-27 in a single-player game with the stock **Sandbox mode** mod
enabled, through the multiplayer mod's `EVAL` inject line (`mp/inject.lua`). Labels as in
[README.md](README.md).

## What Sandbox mode is

The stock mod `urbangames_sandbox_1` only sets `game.config.sandboxButton = true` in its `runFn`
(`res/config/base_config.lua` defaults it, and `industryButton`, to `false`). Everything else is the
game's own tools behind that button. [CONFIRMED, mod.lua]

| tool | engine command | multiplayer today |
|---|---|---|
| place a town (`UI::TownBuilder`, [COMMANDS.md](COMMANDS.md#ui-tools)) | `CreateTowns` (`make_cmd` `0x9dd0b0`) | **not replicated**: the town exists on the placer's game only, and the next world check reports a desync |
| demolish a town (`UI::TownBulldozerAction`) | `RemoveTown` (`0x9dd920`) | not replicated |
| town and industry controls | `SetTownInfo`, `DevelopTown`, `setSimBuildingManualDevelopment` | not replicated |
| place an industry | `BuildProposal` from the construction tool (caller `0x419f62`) | goes through the CONXP path like any construction; not yet tested with an industry |

A player's report on 2026-09-27 (two players, 0.7.0.6, a Sandbox save with 96 mods) shows the
first row in the field: one game's world gained 39 streets and 195 town buildings between two hash
stamps with no command sent (`$$ TOWN t=41556 vs b: 516 vs 711`), later the other's gained 56 and
292, and each was undone by a resync. [MEASURED, the players' logs]

## The script API

- `api.type` has `CreateTowns`, `TownInfo`, `RemoveTown`, `SetTownInfo`, `DevelopTown`,
  `Town`, `TownBuilding`, `TownBuildingParams`, `TownConnection`. [MEASURED]
- `api.type.TownInfo.new()` prints as: [MEASURED, `debugPrint`]

  ```
  name = "", position = { x = 0, y = 0 },
  initialLandUseCapacities = { 0, 0, 0 },          -- residential, commercial, industrial
  landUse2CargoNeeds = { {}, {}, {} },             -- cargo type ids per land use
  ```

  `api.type.CreateTowns.new()` is `{ towns = {} }`.
- The vectors are engine containers: `ti.landUse2CargoNeeds[2] = {27, 27, 26}` fails ("expected
  userdata, received table"); assigning the elements works (`local v = ti.landUse2CargoNeeds[2];
  v[1] = 27 ...`). [MEASURED]
- `api.cmd.sendCommand(api.cmd.make.createTowns({ti}), cb)` applies **while the game is paused**:
  the callback reports `success=true` and the town is complete at the same game time. [MEASURED]
- A town the tool placed reads back (`api.engine.getComponent(t, api.type.ComponentType.TOWN)`)
  with `initialLandUseCapacities`, `cargoNeeds` (per land use), `sizeFactor` 1.2,
  `growthTendency` 0, `customCargoNeeds` false, `developmentActive` true. The capacities start
  where the command put them and grow from there. [MEASURED]
- The game script refuses new globals ("creating globals by assignment is not allowed", from
  `res/scripts/init.lua`); `rawset(_G, name, value)` goes around it for a probe. [MEASURED]

## Town creation is deterministic

From one frozen state (a save written at speed 0, game time 765.8), the same `createTowns`
command (a town at (1500, -1500), capacities 148/136/109) produced the identical town twice, in
two loads: [MEASURED]

| | run 1 | run 2 | run 3 (no cargo needs) |
|---|---|---|---|
| town entity | 22590 | 22590 | 22590 |
| town buildings | 158 | 158 | 158 |
| digest of their positions | `3211685293` | `3211685293` | `3211685293` |
| street edges within 900 m | 42 | 42 | 42 |
| digest of their end points | `357314705` | `357314705` | `357314705` |

So a town placed from the same command at the same simulation step comes out the same on every
game: the multiplayer replay can stamp the command like any other.

## The cargo needs are the tool's choice, not the engine's

With `landUse2CargoNeeds` left empty (run 3) the town is created with **empty** needs and the same
layout. The engine does not fill them in, and they do not shape the town. The town tool placed its
town (Palm Bay) with needs `27, 27, 26` / `23, 24, 25`: the tool picks them before it sends the
command. [MEASURED]

For replication this means: capture the command the tool sends on the placing player's game and
ship that `TownInfo` whole (name, position, capacities, needs). A peer must never ask the tool, or
anything else, to choose again.

## The tool's command

`make_cmd::CreateTowns` (`0x9dd0b0`) takes `rdx` -> `std::vector<TownInfo>`; the town tool calls it
from `0x470959` (its Add returns to `0x470977`), the script maker from `0xcedf98`. One `TownInfo`
is **0x90 bytes**: [MEASURED, byte dumps against towns read back through the script API]

| offset | field |
|---|---|
| `+0x00` | `std::string` name: **empty** from the tool; the engine names the town (the same name on every game: "Allentown" twice from one state) |
| `+0x20`, `+0x24` | float x, y |
| `+0x28` | int32 `initialLandUseCapacities[3]` (the tool sent 90/90/90) |
| `+0x34` | 4 bytes the tool leaves uninitialised (10.02 once, -1501.2 the next time) |
| `+0x38`, `+0x50`, `+0x68` | `std::vector<int32>` `landUse2CargoNeeds[3]` |
| `+0x80` | 16 bytes the tool leaves uninitialised |

The uninitialised bytes do not travel and do not matter: every game, the placer's included, builds
the town from the same clean `TownInfo` through the script API.

## Towns in multiplayer (TOWNC, 2026-09-27)

- The slice captures the tool's `CreateTowns` (factory id 19, `native/src/slice/capture.inl`),
  cancels it and writes one `TOWNC <x,y> <cap1> <cap2> <cap3> <needs1> <needs2> <needs3> <name>`
  per town. The position is one `%.9g,%.9g` field, a string on the wire, so every game sets the
  same float32. Nothing is cancelled that did not decode or reach the inject file.
- `TOWNC` is a STRICT op: `mp/sandbox.lua` builds the town with `api.cmd.make.createTowns` at the
  stamp on every game, the placer included (ARMED 1).
- **The tool waits on its callback** (a "building town" bar that stays up otherwise). The Add hook
  fires it at the cancel, as it does the build tools'. Holding it for the placer's replay does not
  work as the line editor's does: `api.cmd.sendCommand` queues a game-script command and adds it on
  **another thread** (made on one tid, added on another), so the claiming thread never sees that
  Add. [MEASURED; the same holds for line creation, see KNOWN_ISSUES.md]
- Two instances, one town placed from the tool: the callback fired, both games applied TOWNC at
  the same step (525) with `success=true`, and every world hash after it matched. [MEASURED]
- `RemoveTown` (`0x9dd920`, factory id 20) is hooked in probe mode: its arguments are logged with
  byte dumps, and it runs natively.

## What is still open

- `RemoveTown`: the argument layout from the probe logs, then the same cancel and replay.
- `SetTownInfo`, `DevelopTown`, `setSimBuildingManualDevelopment` (the town and industry controls).
- Industry placement: it takes the construction path (CONXP); a two-instance test with an industry.
