# Commands: from a player's click to the simulation

Build 35924; addresses are RVAs from image base `0x140000000`. Confidence labels are
defined in [README.md](README.md).

## The pipeline

```
UI tool commit method            e.g. UI::StreetBuilder::UpdateEngine 0x459ce0
  -> make_cmd::<Type>(...)       builds a 0x38-byte Command (returned through rcx)
  -> CommandList::Add 0x9d2a00   queues it with a completion callback
                                 ...
sim thread: CGame::RunGameSimLoop 0x1184d0
  -> apply_command 0x9da290      "Simulation Thread: Apply Command"
     -> dispatch table 0x30b10c0 (37 entries), indexed by the command's variant tag
        -> Visitor::operator()(CmdData::<Type>&)
```

- **Every player action goes through `CommandList::Add`**: building, bulldozing,
  terraforming, vehicles, lines, names, loans, game speed, the map editor. Static count:
  82 direct call sites; all but two (`game.cpp` `0x119d30`, `menuui.cpp` `0x67e370`,
  unidentified) pair with a factory. [CONFIRMED, static]
- **Script commands use the same queue.** `api.cmd.sendCommand` reaches Add from
  `scripting/legacy/cmd_interface.cpp` `0x1126cd0` (the call returns to `0x1126f1a`).
  That site issues roughly a hundred Adds a second from a running mod, against exactly one
  Add from the UI per road built. [MEASURED]
- **`applyProposal` `0x9e76e0` is not on the command path.** It runs inside the
  BuildProposal handler (`0x9d6e20` → `construction_builder_util::CreateProposalData`
  `0xa072b0` → `applyProposal`), and 21 of its 22 callers are town growth, industry, save
  loading and scripting. Hooking it catches the engine's own construction. [CONFIRMED]
- **The Command is a handle.** Factories build a 0xB18-byte payload on the stack and wrap
  it with the shared packaging helper `0x9dd6a0`. The BuildProposal payload holds its
  proposal at +0x370. The variant tag is read as `*(u8*)(*cmd + 0xb18)`; the depot
  window's buy callback `0x748250` requires 13 there. [CONFIRMED static; one early live
  read at the packaging helper did not show a valid tag, so check live bytes before
  classifying commands by it]

### Dispatch tags

Named by `__FUNCSIG__` on the handler each entry reaches. The other 25 handlers carry no
assert and are unnamed; the order is not alphabetical.

| tag | handler | CmdData |
|---|---|---|
| 2 | `0x9d54a0` | UpdateLogo |
| 7 | `0x9d5620` | Reverse |
| 8 | `0x9d9e10` | SetUserStopped |
| 9 | `0x9d5720` | SetVehicleTargetMaintenanceState |
| 10 | `0x9d5770` | SetVehicleShouldDepart |
| 12 | `0x9d8c90` | SellVehicle |
| 13 | `0x9d6280` | BuyVehicle |
| 14 | `0x9d8110` | ReplaceVehicle |
| 15 | `0x9d6e20` | BuildProposal |
| 22 | `0x9d70c0` | ConnectTownsAndIndustries |
| 28 | `0x9d98a0` | SetColor |
| 31 | `0x9d6bc0` | Book |

## `CommandList::Add` `0x9d2a00`

```
? CommandList::Add(CommandList* list,                           rcx
                   Handle* out,                                 rdx   written by Add, destroyed by the caller
                   Command* cmd,                                r8
                   std::function<void(Command const&)>* done,   r9
                   ...)
```

- The command is appended to a `std::vector<Command>` (pointer bump +0x38);
  `CommandList::Swap(std::vector<Command>&)` `0x9d2cf0` hands the batch to the sim.
  [CONFIRMED]
- UI call sites discard the return value (e.g. the call at `0x459eb2` is followed by
  `lea rcx,[rsp+0x38]` at `0x459eb7`). [CONFIRMED, disassembly]
- The caller destroys `out` as soon as Add returns. The destructor `0x2357910` checks
  only for null, then dereferences `handle[1]`. **Zero `*out` whenever the call is
  skipped**: a leftover -2 in the caller's stack slot crashed the game at
  `exe+0x235791e`. [MEASURED]
- `done` is the completion callback, the `function(result, success)` the Lua API exposes.

### Skipping a command safely

This is the cancel point the slice DLL uses for strict lockstep. Each rule was learned
from a crash or a wedged tool.

1. **Identify the command by pointer.** A factory returns its Command through the hidden
   pointer in rcx and hands the same pointer back in rax, which the UI passes to Add as
   r8. `Add.r8 == factory.rcx` names exactly the command just built and never matches the
   Lua bridge's Adds. [MEASURED]
2. **Know whether the caller waits on `done`.**
   - *Waits:* `UI::StreetBuilder`, `UI::ConstructionBuilder`, `UI::Bulldozer::Apply`,
     the module and stop tools, the depot window's buy, the vehicle window's replace.
     Skipping Add without calling `done` wedged the street tool for the rest of the
     session; firing `done` first fixed it (nine consecutive cancelled builds). [MEASURED]
   - *Fire-and-forget:* SetLine, Reverse, SellVehicle, SendToDepot, UpdateLine,
     DeleteLine and the other flat vehicle and line commands. Do **not** fire `done` for
     these: the command's success flag is still 0, so the UI takes its failure branch
     (SetLine pops a false "unable to find a path"). [MEASURED]
3. **Resolve the callback like MSVC does.** `r9` is the 64-byte `std::function` object; its
   impl pointer is at `r9+0x38` (`_Getimpl`). A small functor's impl lives inside the
   object (the pointer equals r9, as for the street tool); a large one is a heap block
   (BuyVehicle, ReplaceVehicle); null means an empty function. The impl vftable is
   +0x00 `_Copy`, +0x08 `_Move`, +0x10 `_Do_call`, +0x18 `_Target_type`,
   +0x20 `_Delete_this`; call `_Do_call(impl, Command const&)`. [CONFIRMED against the
   binary for the street tool's 16-byte functor: `_Delete_this` frees 0x10 bytes and
   `_Target_type` is a two-instruction RTTI getter.] Not every functor type shares this
   table: the level-crossing recorder's functions put `_Do_call` at +0x18.
4. **If `done` cannot be fired for a caller that waits, let the command run** (a local
   action that also replicates is recoverable; a dead tool is not), unless the replay has
   already been promised to the Lua side, in which case skip it anyway. Running it as well
   applies the action twice (one click bought two vehicles).
5. Zero `*out`.

The detour relay (`deferrelay_slice.asm`) preserves rcx, rdx, r8, r9, r10, r11, rax and all
six volatile xmm registers, restores rsp to its entry value before jumping to the
trampoline, and spills xmm3 below the callee frame (calleeRsp - 0x78) for the maintenance
factory's float. The caller comes from the return address on the stack:
`RtlCaptureStackBackTrace` returns no frames inside a `VirtualAlloc`'d relay (no unwind
data).

## UI tools

All derive from `UI::IAction` (vftable slot 5 = `Step`; slots 4/12/13 = `PreStep`,
`DoActivate`, `DoDeactivate`). One block, `0x54afb0`-`0x54c9e0`, constructs them.

| tool | ctor | action id | `Step` | commit | command |
|---|---|---|---|---|---|
| `UI::StreetBuilder` | `0x4453b0` | `action-streetbuilder` | `0x4575c0` | `UpdateEngine` `0x459ce0` | BuildProposal |
| `UI::ConstructionBuilder` | `0x40fdb0` | `action-constructionbuilder` | `0x41a9d0` | `MousePressed` `0x419aa0` | BuildProposal |
| `UI::ModuleBuilder` | `0x429d80` | `action-modulebuilder` | `0x42d750` | `MousePressed` `0x42b810` | BuildProposal |
| `UI::StreetTerminalBuilder` | `0x45eb80` | `action-streetterminalbuilder` | `0x4612e0` | `0x460a30` | BuildProposal |
| `UI::TrackModifier` | `0x474e00` | `action-trackmodifier` | `0x47c7b0` | `Build` `0x478d70` | BuildProposal |
| `UI::Bulldozer` | `0x3e2450` | `action-bulldozer` | `0x3ec380` (LIKELY) | `Apply` `0x3eaeb0` | BuildProposal |
| `UI::ProposalAction` | `0x4304d0` | `action-proposal` | | `0x4310d0` | BuildProposal |
| `UI::TerrainModifier` | `0x465620` | (base's) | `0x46ac40` | slot 14 `0x4684a0` → `0x4310d0` | BuildProposal |
| `UI::TerrainPainter` | `0x46d080` | (base's) | `0x46dfd0` | slot 14 `0x46d7d0` → `0x4310d0` | BuildProposal |
| `UI::AssetBrush` | `0x3cc650` | (base's) | `0x3d4930` | slot 14 `0x3d1110` → `0x4310d0` | BuildProposal |
| `UI::TownBuilder` | `0x46f690` | `action-townbuilder` | `0x470cc0` | `0x4705c0` | CreateTowns |
| `UI::CSelector` | `0x437d20` | `action-selector` / `action-inspector` | `0x43b360` | `Select` `0x43b210` | none |
| `UI::CameraAction` | `0x3f7010` | `action-camera` | | `0x404b00`, `0x4059a0` | SetGameSpeed |

Demolishing a town is the exception among the bulldozer subclasses:
`UI::TownBulldozerAction` issues `make_cmd::RemoveTown` `0x9dd920`. The other seven
(`Asset`, `Building`, `Field`, `Module`, `Street`, `StreetConnector`, `StreetTerminal`)
only filter and go through `Bulldozer::Apply`. `UI::IEngine` / `ProposalEngine` `0x4332b0` /
`CachedProposalEngine` `0x3f2110` are a read-only world-query interface for previews and
issue no commands.

Other UI sites, by factory: vehicles in `ui/components/vehiclemanager.cpp` (buy `0x74f5e0`,
sell `0x748b50`, send to depot `0x750670`, maintenance `0x74a490`) and
`ui/util/vehicle_button_util.cpp` (SetLine `0x88afd0` and `0x886a80`, sell `0x887130`, send
to depot `0x88c2f0`, stop/start `0x88aa90`); Reverse from `ui/viewcreator.cpp` `0x8b5510`;
lines from `ui/components/lineeditor.cpp` (UpdateLine at `0x5fe260`, `0x607190`,
`0x5fe9e0`, `0x6033c0`, `0x603a00`, `0x603fa0`, `0x60b970`) and
`ui/components/line_ui_util.cpp` (terminal and waypoint lanes, DeleteLine `0x7bf590`);
CreateLine from `linelist.cpp` `0x610380` / `linemanager.cpp` `0x618ff0` through
`line_util` `0x215c180`; loans from `ui/components/financescomp.cpp` `0x53b660`,
`0x53b8b0` (LIKELY); game speed (SetGameSpeed, from the call graph) from `UI::Clock`
(`TogglePause` `0x4efab0`, `0x4eff50`, `0x4f2640`: the speed buttons and pause toggle),
`UI::CGameUI::GameStep` `0x574a70`, `menuui.cpp` `0x657710` and `CMenuUI::SwitchToGameUI`'s lambda
`0x65eb60`, `UI::CameraAction::Play` and `Record` (the camera-path tool), a debug view `0x795900`,
and `0xc17ed0`, the Lua maker `api.cmd.make.setGameSpeed` (it calls the factory, returning to
`0xc17eff`: measured live, so this maker is not one of the inline ones the table's footnote marks).

## Call sites the hooks classify by

A factory hook sees the factory's return address inside the UI function; an Add hook
sees Add's. They are different numbers for the same click.

| origin | factory returns to | Add returns to | status |
|---|---|---|---|
| `UI::StreetBuilder::UpdateEngine` (roads, track) | `0x459e97` | `0x459eb7` | MEASURED, in code |
| street/track upgrade (type, catenary, bus lane, tram) | `0x4790fc` | | MEASURED, in code |
| `UI::ConstructionBuilder::MousePressed` (stations, depots, assets) | `0x419f62` | `0x419f81` | MEASURED, in code |
| `UI::Bulldozer::Apply` (call at `0x3eb222`) | `0x3eb227` | `0x3eb245` | MEASURED, in code |
| `UI::StreetTerminalBuilder` commit (street stops, signals, waypoints) | `0x460e0b` | | MEASURED, in code |
| `UI::ModuleBuilder` (module add or remove) | `0x42bc7b` | `0x42bc9c` (predicted) | factory return MEASURED |
| `addmodulecomp` (module list in the construction window) | `0x4b71b8` (predicted) | `0x4b71d8` (predicted) | from the call graph |
| `UI::ProposalAction` (terraform, paint, brush) | `0x4311c6` | | MEASURED |
| depot window buy (`vehiclemanager.cpp`) | `0x74fd88` | `0x74fda9` | MEASURED |
| vehicle manager clone (`0x752b50` → `0x747230`, one buy per selected vehicle through `0x74f5e0`) | `0x74fd88` (same buy function) | | DECOMPILED |
| buy completion callback, shared by buy and clone: `_Do_call` `0x753820` → `0x748250(lambda = impl+8)`; reads the result vehicle at `(*command)+0x38` after a type tag of 13 at `+0xb18`; lambda `+0x30` int line: below 0 opens the vehicle window, 0 or more SetLines the new vehicle through `0x88b840` | | | DECOMPILED |
| CreateLine via `line_util` | `0x215c26b` | | MEASURED |
| line editor UpdateLine | `0x6043fd`, `0x6074b5` | | MEASURED |
| vehicle window Reverse (`viewcreator.cpp`) | `0x8b556d` | | MEASURED |
| `UI::Clock` speed buttons and pause toggle (SetGameSpeed) | `0x4efb8f`, `0x4f0097`, `0x4f26ef` | `0x4f00ba` (the buttons) | MEASURED (`0x4f0097`), in code |
| Lua `api.cmd.make.setGameSpeed` | `0xc17eff` | | MEASURED |
| Lua `api.cmd.make.setColor` | `0xc3848e` (a `gamescriptrep.cpp` lambda, outside the wrapper block) | | MEASURED |
| Lua `api.cmd.make.*` sol2 wrappers | `0xcec000`-`0xcf2000` (buildProposal `0xced378`, buyVehicle `0xceefae`) | | MEASURED, in code |
| Lua `api.cmd.sendCommand` | | `0x1126f1a` | MEASURED |

## Factories (`game/command/make_command.cpp`)

"funcsig" = named by its own `__FUNCSIG__` string; "position" = pinned by the strict
alphabetical address order the funcsig-named factories establish. "Hook" is the id the
slice DLL gives the factories it hooks (these ids appear in its logs; they are not
dispatch tags). Steal sizes were measured by `PrologueBoundaries.java` and control-matched
against every hooked factory.

| RVA | `make_cmd::` | evidence | Lua maker | steal | hook |
|---|---|---|---|---|---|
| `0x9dc5e0` | Book | funcsig | `bookJournalEntry(player, entry, position?)` | 18 | |
| `0x9dc750` | BuildProposal | position, 17 builder callers, decompiled, Lua arity | `buildProposal(proposal, context?, ignoreErrors)` | 19 | 0 |
| `0x9dca00` | BuyVehicle | funcsig | `buyVehicle(player, depot, config)` | 15 | 2 |
| `0x9dcbf0` | ConnectTownsAndIndustries | position | `connectTownsAndIndustries(...)` * | 14 | |
| `0x9dcde0` | CreateLine | funcsig | `createLine(name, color, player, line)` | 19 | 7 |
| `0x9dd0b0` | CreateTowns | position, decompiled caller | `createTowns(towns)` * | 15 | |
| `0x9dd190` | DeleteLine | funcsig | `deleteLine(line)` | 20 | 9 |
| `0x9dd290` | DevelopTown | position | `developTown(town, position)` * | 19 | |
| `0x9dd2e0` | InstantlyUpdateTownCargoNeeds | position | `instantlyUpdateTownCargoNeeds(town, needs)` * | 20 | |
| `0x9dd820` | RemoveField | funcsig | `removeField(field)` | 20 | |
| `0x9dd920` | RemoveTown | funcsig | `removeTown(town)` | 20 | |
| `0x9dda20` | ReplaceTerrain | position | `replaceTerrain(...)` * | 17 | |
| `0x9dddb0` | ReplaceVehicle | funcsig | `replaceVehicle(vehicle, config)` | 15 | 4 |
| `0x9ddfe0` | Reverse | funcsig | `reverseVehicle(vehicle)` | 20 | 10 |
| `0x9de0e0` | SaveGame | position | none | 21 | |
| `0x9de380` | SellVehicle | funcsig | `sellVehicle(vehicle)` | 20 | 3 |
| `0x9de490` | SendScriptEvent | position | `sendScriptEvent(file, id, name, params)` * | 17 | |
| `0x9de6f0` | SendToDepot | funcsig | `sendToDepot(vehicle, sellOnArrival)` | 20 | 5 |
| `0x9de7f0` | SetAnimalState | position | `setAnimalState(...)` * | 18 | |
| `0x9de870` | SetCalendarSpeed | position | `setCalendarSpeed(msPerDay)` * | 21 | |
| `0x9de8a0` | SetColor | funcsig | `setColor(entity, color)` (calls the factory from `0xc3848e`) | 20 | 13 |
| `0x9de9b0` | SetDate | position | `setDate(date)` * | 21 | |
| `0x9de9e0` | SetGameSpeed | position | `setGameSpeed(speed)` (calls this factory) | 21 | 15 |
| `0x9dea10` | SetLine | funcsig | `setLine(vehicle, line, stopIndex)` | 18 | 6 |
| `0x9deb70` | SetName | funcsig | `setName(entity, name)` | 15 | 14 |
| `0x9ded50` | SetNoCosts | position | none | 21 | |
| `0x9ded80` | SetSimBuildingClosureTimeStamp | funcsig | `setSimBuildingClosureTimeStamp(building, t)` | 20 | |
| `0x9dee80` | SetSimBuildingManualDevelopment | funcsig | `setSimBuildingManualDevelopment(building, manual)` | 20 | |
| `0x9def80` | SetTownInfo | position | `setTownInfo(town, capacities)` * | 20 | |
| `0x9df070` | SetUserStopped | funcsig | `setUserStopped(vehicle, stopped)` | 20 | |
| `0x9df170` | SetVehicleManualDeparture | funcsig | `setVehicleManualDeparture(vehicle, manual)` | 20 | |
| `0x9df270` | SetVehicleShouldDepart | position | `setVehicleShouldDepart(vehicle)` | 20 | |
| `0x9df340` | SetVehicleTargetMaintenanceState | funcsig | `setVehicleTargetMaintenanceState(vehicle, value)` | 20 | 12 |
| `0x9df480` | SpawnAnimal | position | `spawnAnimal(file, position)` * | 21 | |
| `0x9df4e0` | UpdateLine | funcsig | `updateLine(line, lineData)` | 19 | 8 |
| `0x9df710` | UpdateLogo | position | none | | |

\* Per the command-map analysis these Lua makers build their command inline in the
`SetupCommandInterface` `0xd042e0` registration lambdas rather than calling the factory,
so a factory hook sees only the UI's use of them. The other makers are sol2 wrappers in
`0xcecf60`-`0xcef200` that call exactly one factory and do not call Add themselves.
[static; not re-verified live]

The slice also hooks `CommandList::Add` itself (hook id 1, steal 18: eight pushes plus
`lea rbp,[rsp-0x78]`, stopping short of a RIP-relative load). `BuyVehicle` `0x9dca00` starts with
`48 8B C4 55 41 54 41 57 48 8D A8 28 F5 FF FF` (steal 15).

### Argument ABI

- Factories return `struct Command` by value, so rcx is the hidden return pointer and the
  real arguments shift: rdx = arg1 (`const ecs::Engine&`), r8 = arg2, r9 = arg3, arg4 on
  the stack at `[calleeRsp+0x28]`.
- `ecs::Entity` arguments are passed **by value**: read r8/r9 as integers. A probe that
  only dumps memory behind them logs nothing and looks like the hook never fired.
- SetGameSpeed and SetCalendarSpeed take no Engine: the value is the low 32 bits of rdx.
- SetVehicleTargetMaintenanceState passes its value as a float in xmm3.
- ConnectTownsAndIndustries: its 14-byte steal homes r8/r9 in the stolen bytes, so a
  trampoline must preserve them. ReplaceTerrain: copy the r8 config and the
  `[rsp+0x28]` string before calling the original.

### Arguments the slice decodes

| factory | arguments |
|---|---|
| BuildProposal | `Command* (Command* ret, Engine*, construction_builder_util::Proposal* r8, Context* r9, bool, bool ignoreErrors)`. For `UI::Bulldozer::Apply` r9 is a 0x70-byte options struct, not a Context. Layout: [PROPOSALS.md](PROPOSALS.md). |
| BuyVehicle | r8 player, r9 depot (the VEHICLE_DEPOT child entity, not the construction), st[0] → the by-value `TransportVehicleConfig` copy on the caller's stack |
| ReplaceVehicle | r8 vehicle, r9 → `TransportVehicleConfig` |
| SellVehicle | r8 → `std::vector<Entity>` |
| SendToDepot | r8 vehicle, r9 bool sellOnArrival (low bit) |
| SetLine | r8 vehicle, r9 line, st[0] stop index |
| Reverse | r8 vehicle |
| CreateLine | rdx → name `std::string`, r8 → colour (3 floats), r9 player, st[0] → `component::Line` |
| UpdateLine | r8 line, r9 → `component::Line`. The factory moves the stops out of the caller's temporary, so read it at entry. |
| DeleteLine | r8 line |
| SetColor | r8 entity, r9 → 3 floats in 0..1 |
| SetName | r8 entity, r9 → `std::string` |
| SetVehicleTargetMaintenanceState | r8 vehicle, value in xmm3 |

`TransportVehicleConfig` (0x30 B):

| offset | field | status |
|---|---|---|
| +0x00 | `vector<TransportVehiclePart>`, stride 0x80 | DECOMPILED |
| +0x18 | `vector<int>` vehicleGroups | sweep EXACT |

`TransportVehiclePart` (0x80 B):

| offset | field | status |
|---|---|---|
| +0x00 | int modelId | DECOMPILED |
| +0x08 | `vector<int>` loadConfig | sweep EXACT |
| +0x20 | colour, 3 floats | sweep EXACT |
| +0x60 | `vector<int>` autoLoadConfig | DECOMPILED |

`ecs::component::Line`:

| offset | field | status |
|---|---|---|
| +0x00 | `vector<Line::Stop>`, stride 0xa8 | sweep (span) + DECOMPILED |
| +0x18 | int waitingTime | sweep EXACT |
| +0x1c | LineVehicleInfo, 0x24 B | DECOMPILED |

`Line::Stop` (0xa8 B):

| offset | field | status |
|---|---|---|
| +0x00 | station group entity | INFERRED (the only slot left) |
| +0x04 | int station, index within the group | sweep EXACT |
| +0x08 | int terminal | sweep EXACT |
| +0x10 | `vector<StationTerminal>` alternativeTerminals, 8 B each | sweep (span) |
| +0x28 | int loadMode, 0..3 (0 and 3 take no wait) | DECOMPILED (`TransportVehicleSystem::Update2`) |
| +0x2c | float wait, min or max | sweep EXACT |
| +0x30 | float, the other wait | DECOMPILED |
| +0x38 | `vector<SignalId>` waypoints | DECOMPILED |
