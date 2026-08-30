# TpF2 command map — all command types (build 35924)

Every Command TYPE in the dispatch table, its apply-handler, make_cmd factory,
Lua `api.cmd.make.*` name, and replication class. Steal sizes are measured by
`tools/ghidra_scripts/PrologueBoundaries.java` and control-validated: all 9
already-hooked factories' script sizes matched their hand-measured values.

Class: **sync** = player action, must lockstep-replicate. **gated** =
destructive/editor, ship OFF by default. **capture-only** = never replicate
(local/non-deterministic; a divergence source if sent).

| idx | name | factory | lua maker | player | source file |
|---|---|---|---|---|---|
| 0 | CmdData::Book | `0x9dc5e0` | `api.cmd.make.bookJournalEntry(playerEntity:Entity, journalEntry:JournalEntry, position:Vec3f[optional])` | Y | game\command\make_command.cpp |
| 1 | CmdData::BuildProposal | `0x9dc750` | `api.cmd.make.buildProposal(proposal:SimpleProposal, context:Context[optional], ignoreErrors:bool)` | Y | game\command\make_command.cpp |
| 2 | CmdData::BuyVehicle | `0x9dca00` | `api.cmd.make.buyVehicle(playerEntity:Entity, depotEntity:Entity, config:TransportVehicleConfig)` | Y | game\command\make_command.cpp |
| 3 | CmdData::ConnectTownsAndIndustries | `none (built inline in SetupCommandInterface registration lambda)` | `api.cmd.make.connectTownsAndIndustries(depotEntity:Entity, townEntities:Array[Entity], connections, keep:bool)` | - | SetupCommandInterface @ 0x140d042e0 |
| 4 | CmdData::CreateLine | `0x9dcde0` | `api.cmd.make.createLine(name:String, color:Vec3f, playerEntity:Entity, line:Line)` | Y | game\command\make_command.cpp |
| 5 | CmdData::CreateTowns | `none (built inline in SetupCommandInterface registration lambda)` | `api.cmd.make.createTowns(towns:Array[TownInfo])` | - | SetupCommandInterface @ 0x140d042e0 |
| 6 | CmdData::DeleteLine | `0x9dd190` | `api.cmd.make.deleteLine(lineEntity:Entity)` | Y | game\command\make_command.cpp |
| 7 | CmdData::DevelopTown | `none (built inline in SetupCommandInterface registration lambda)` | `api.cmd.make.developTown(townEntity:Entity, position:Vec2f)` | - | SetupCommandInterface @ 0x140d042e0 |
| 8 | CmdData::InstantlyUpdateTownCargoNeeds | `none (built inline in SetupCommandInterface registration lambda)` | `api.cmd.make.instantlyUpdateTownCargoNeeds(townEntity:Entity, cargoNeeds:Array)` | - | SetupCommandInterface @ 0x140d042e0 |
| 9 | CmdData::RemoveField | `0x9dd820` | `api.cmd.make.removeField(fieldEntity:Entity)` | - | game\command\make_command.cpp |
| 10 | CmdData::RemoveTown | `0x9dd920` | `api.cmd.make.removeTown(townEntity:Entity)` | - | game\command\make_command.cpp |
| 11 | CmdData::ReplaceTerrain | `none (built inline in SetupCommandInterface registration lambda)` | `api.cmd.make.replaceTerrain(map:Map, config:BaseConfig.Terrain, seedText:String, worldEntity:Entity, keepAssets:bool)` | - | SetupCommandInterface @ 0x140d042e0 |
| 12 | CmdData::ReplaceVehicle | `0x9dddb0` | `api.cmd.make.replaceVehicle(vehicleEntity:Entity, config:TransportVehicleConfig)` | Y | game\command\make_command.cpp |
| 13 | CmdData::Reverse | `0x9ddfe0` | `api.cmd.make.reverseVehicle(vehicleEntity:Entity)` | Y | game\command\make_command.cpp |
| 14 | CmdData::SellVehicle | `0x9de380` | `api.cmd.make.sellVehicle(vehicleEntity:Entity)` | Y | game\command\make_command.cpp |
| 15 | CmdData::SendToDepot | `0x9de6f0` | `api.cmd.make.sendToDepot(vehicleEntity:Entity, sellOnArrival:bool)` | Y | game\command\make_command.cpp |
| 16 | CmdData::SendScriptEvent | `none (built inline in SetupCommandInterface registration lambda)` | `api.cmd.make.sendScriptEvent(fileName:String, id, name, params:Object)` | - | SetupCommandInterface @ 0x140d042e0 |
| 17 | CmdData::SetAnimalState | `none (built inline in SetupCommandInterface registration lambda)` | `api.cmd.make.setAnimalState(animalEntity:Entity, movementType, targetChangedElapsed, invalidTileElapsed, movementSpeed, angularSpeed)` | - | SetupCommandInterface @ 0x140d042e0 |
| 18 | CmdData::SetCalendarSpeed | `none (built inline in SetupCommandInterface registration lambda)` | `api.cmd.make.setCalendarSpeed(millisecondsPerDay:Number)` | - | SetupCommandInterface @ 0x140d042e0 |
| 19 | CmdData::SetColor | `0x9de8a0` | `api.cmd.make.setColor(entity:Entity, color:Vec3f)` | Y | game\command\make_command.cpp |
| 20 | CmdData::SetDate | `none (built inline in SetupCommandInterface registration lambda)` | `api.cmd.make.setDate(date)` | - | SetupCommandInterface @ 0x140d042e0 |
| 21 | CmdData::SetGameSpeed | `none (built inline in SetupCommandInterface registration lambda)` | `api.cmd.make.setGameSpeed(speed:Number)` | - | SetupCommandInterface @ 0x140d042e0 |
| 22 | CmdData::SetLine | `0x9dea10` | `api.cmd.make.setLine(vehicleEntity:Entity, lineEntity:Entity, stopIndex:Number)` | Y | game\command\make_command.cpp |
| 23 | CmdData::SetName | `0x9deb70` | `api.cmd.make.setName(entity:Entity, name:String)` | Y | game\command\make_command.cpp |
| 24 | CmdData::SetSimBuildingClosureTimeStamp | `0x9ded80` | `api.cmd.make.setSimBuildingClosureTimeStamp(simBuildingEntity:Entity, closureTimeStamp:Number)` | - | game\command\make_command.cpp |
| 25 | CmdData::SetSimBuildingManualDevelopment | `0x9dee80` | `api.cmd.make.setSimBuildingManualDevelopment(simBuildingEntity:Entity, manual:bool)` | - | game\command\make_command.cpp |
| 26 | CmdData::SetTownInfo | `none (built inline in SetupCommandInterface registration lambda)` | `api.cmd.make.setTownInfo(townEntity:Entity, initialLandUseCapacities:Array[Number])` | - | SetupCommandInterface @ 0x140d042e0 |
| 27 | CmdData::SetUserStopped | `0x9df070` | `api.cmd.make.setUserStopped(vehicleEntity:Entity, userStopped:bool)` | Y | game\command\make_command.cpp |
| 28 | CmdData::SetVehicleManualDeparture | `0x9df170` | `api.cmd.make.setVehicleManualDeparture(vehicleEntity:Entity, manual:bool)` | Y | game\command\make_command.cpp |
| 29 | CmdData::SetVehicleTargetMaintenanceState | `0x9df340` | `api.cmd.make.setVehicleTargetMaintenanceState(vehicleEntity:Entity, value:Number)` | Y | game\command\make_command.cpp |
| 30 | CmdData::SetVehicleShouldDepart | `0x9df270 (unnamed make_command.cpp fn, inferred)` | `api.cmd.make.setVehicleShouldDepart(vehicleEntity:Entity)` | Y | game\command\make_command.cpp |
| 31 | CmdData::SpawnAnimal | `none (built inline in SetupCommandInterface registration lambda)` | `api.cmd.make.spawnAnimal(fileName:String, position:Vec2f)` | - | SetupCommandInterface @ 0x140d042e0 |
| 32 | CmdData::UpdateLine | `0x9df4e0` | `api.cmd.make.updateLine(lineEntity:Entity, line:Line)` | Y | game\command\make_command.cpp |
| 33 | SetNoCosts | `0x9ded50` | `(none)` | - | game\command\apply_command.cpp |
| 34 | SetAnimalState | `0x9de7f0` | `setAnimalState` | - | game\command\apply_command.cpp |
| 35 | SpawnAnimal | `0x9df480` | `spawnAnimal` | - | game\command\apply_command.cpp |
| 36 | Debug_SetSimPersonState | `none (inline in 0xcd4630)` | `(none in api.cmd.make)` | - | game\command\apply_command.cpp |

## Hook table rows (skeptic-verified, ids continue from 10)

```c
// FACTORIES[] rows to APPEND (ids continue from 10; existing 1-9 unchanged; BuildProposal 0x9dc750 NOT re-added -- already live at args_probe.cpp:48).
// Form: { rva, steal, id, "Name", "kind" }   kind = sync | gated | capture-only
// --- vehicle-flat (all ship_gated_off=false = sync; all layouts DECOMPILED) ---
{ 0x9ddfe0, 20, 10, "Reverse",                          "sync" },
{ 0x9df070, 20, 11, "SetUserStopped",                   "sync" },
{ 0x9df340, 20, 12, "SetVehicleTargetMaintenanceState", "sync" },   // value in XMM3 - stub must MOVD xmm3
{ 0x9de8a0, 20, 13, "SetColor",                         "sync" },   // r9 -> CVec3f*, deep-copy 12B
{ 0x9deb70, 15, 14, "SetName",                          "sync" },   // r9 -> std::string*, snapshot bytes
{ 0x9df170, 20, 15, "SetVehicleManualDeparture",        "sync" },
// --- upgrade-terrain-misc ---
{ 0x9de9e0, 21, 16, "SetGameSpeed",                     "sync" },   // NO Engine arg: payload in rdx.lo32
{ 0x9de870, 21, 17, "SetCalendarSpeed",                 "sync" },   // NO Engine arg: payload in rdx.lo32
{ 0x9df270, 20, 18, "SetVehicleShouldDepart",           "sync" },
{ 0x9dd820, 20, 19, "RemoveField",                      "gated" },
{ 0x9dd0b0, 15, 20, "CreateTowns",                      "gated" },  // layout UNRESOLVED - log/passthrough until sweep
{ 0x9dd920, 20, 21, "RemoveTown",                       "gated" },
{ 0x9dd290, 19, 22, "DevelopTown",                      "gated" },  // layout UNRESOLVED
{ 0x9def80, 20, 23, "SetTownInfo",                      "gated" },
{ 0x9dd2e0, 20, 24, "InstantlyUpdateTownCargoNeeds",    "gated" },
{ 0x9dcbf0, 14, 25, "ConnectTownsAndIndustries",        "gated" },  // steal 14: trampoline MUST preserve R8/R9 (homed in stolen bytes); layout UNRESOLVED
{ 0x9dee80, 20, 26, "SetSimBuildingManualDevelopment",  "gated" },
{ 0x9ded80, 20, 27, "SetSimBuildingClosureTimeStamp",   "gated" },
{ 0x9dda20, 17, 28, "ReplaceTerrain",                   "gated" },  // copy r8 config + [rsp+0x28] string BEFORE calling orig
{ 0x9de9b0, 21, 29, "SetDate",                          "gated" },  // accepted-Lua-form INFERRED
{ 0x9de0e0, 21, 30, "SaveGame",                         "capture-only" }, // NEVER cancel/replicate
{ 0x9dc5e0, 18, 31, "Book",                             "sync" },   // loans = money, must lockstep-sync
{ 0x9de490, 17, 32, "SendScriptEvent",                  "gated" },  // layout UNRESOLVED
{ 0x9ded50, 21, 33, "SetNoCosts",                       "capture-only" }, // NEVER cancel/replicate; loud divergence warn
{ 0x9de7f0, 18, 34, "SetAnimalState",                   "capture-only" }, // non-player emitter 0xadf980 = noise; never cancel
{ 0x9df480, 21, 35, "SpawnAnimal",                      "gated" },
```