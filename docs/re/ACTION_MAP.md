# The action map — where every player action lands in the binary

Build 35924. Image base `0x140000000`; every address below is an **RVA**.

## Headline

There is **one** path, and it is much shorter than the previous documents
assumed. Every player action — road, rail, station, depot, module, demolish,
terraform, buy/sell vehicle, line create/edit, game speed, loans, map editor —
ends in the same two calls:

```
<UI tool>.<commit method>
    -> make_cmd::<CommandType>(...)        builds a 0x38-byte Command
    -> CommandList::Add          0x9d2a00  queues it
                                           |
CGame::RunGameSimLoop  0x1184d0            | later, on the sim thread
    -> apply_command  0x9da290  ("Simulation Thread: Apply Command")
        -> dispatch table 0x30b10c0 [ payload+0xb18 ]   37 command types
            -> Visitor::operator()(CmdData::<Type>&)
```

`CommandList::Add` at **`0x9d2a00`** has **82 direct call sites** and they are,
with two unidentified exceptions, exactly the player-action list. There is no
`signals2` hop, no `packaged_task`, and no need to intercept `applyProposal`.
The Lua path (`api.cmd.sendCommand`) converges on the same function from
`scripting/legacy/cmd_interface.cpp` `0x1126cd0`, so UI and script commands are
the same objects in the same queue.

`applyProposal` (`0x9e76e0`) is **not** on the command path at all — it is one
step *inside* the handler for one command type, and 21 of its 22 callers are
town growth, industry simulation, savegame loading and scripting.

---

## Method — what actually worked this time

Five corpora, extracted once, now queryable offline with plain greps. The Ghidra
project is locked to a single process, so every question answered locally is a
serialised headless run avoided — which is why the extraction was made bulk and
reusable rather than per-question.

| corpus | size | what it gives |
|---|---|---|
| `strings.csv` | 58,466 strings + xrefs | the raw material |
| `funcsig.csv` | **22,580 functions named**, 19,463 unambiguously | **a partial symbol table with full signatures** |
| `func2src.csv` / `src2func.csv` | 724 source files, 6,483 functions | which `.cpp` a function came from |
| `vtable_dump.csv` | 10,044 slots | virtual method slots per RTTI class |
| `call_edges.csv` | 1,025,709 edges | the direct call graph |

The breakthrough is `funcsig.csv`. MSVC's assert/verify macros expand
`__FUNCSIG__`, so a release build carries the **full demangled signature** of
any function containing an assert, as a literal in `.rdata`, referenced only by
that function:

```
struct Command __cdecl make_cmd::BuyVehicle(const class ecs::Engine &, class ecs::Entity,
                                            class ecs::Entity, struct TransportVehicleConfig)
```

referenced by `0x9dca00` — the exact address M2 measured firing once per vehicle
purchase. That converts "FUN_1409dca00 fires when I buy a bus" into a name,
a return type and an argument list, with no decompilation and no guessing.

The same macros expand `__FILE__`, which yields the game's entire source tree
(`train_fever\src\game\ui\actions\streetbuilder.cpp`, …) and attributes ~6.5k
functions to it. Because the linker lays out a translation unit contiguously,
the attributed functions bracket each `.cpp`, so an *unnamed* function can still
be placed in the right file.

RTTI vftables then gave the third, independent axis. `UI::IAction` is a
12-slot interface; **slot 5 is `Step`** and slot 4/12/13 are
`PreStep`/`DoActivate`/`DoDeactivate` — all four confirmed by `__FUNCSIG__` on
several different subclasses. Reading slot 5 of a sibling class therefore names
that class's `Step` without touching it.

Every load-bearing claim below is carried by at least two independent sources of
those five, and the fourteen most important were then decompiled and read.

### Validation against the one previously-known answer

`streetbuilder.cpp`'s attributed functions are
`447ec0 448080 44b440 44c0d0 44d9a0 44e5b0 44f540 450b00 452180 454690 4575c0
459ce0 45a0b0 45ba10 45bd80 45be10` — containing all three addresses
`UI_CAPTURE_PATH.md` had found by hand, and `__FUNCSIG__` names them exactly:
`UI::StreetBuilder::Step` `0x4575c0`, `UI::StreetBuilder::UpdateEngine`
`0x459ce0`, `UI::StreetBuilder::UpdateRenderer` `0x45a0b0`. The method
reproduces the known result before being trusted on the unknown ones.

---

## The UI tool objects

All derive from `UI::IAction`. One factory block at `0x54afb0`–`0x54c9e0`
constructs every one of them (twelve consecutive small functions, one per tool).
That block sits between `gameoptionscomp.cpp` and `gameui.cpp`'s first assert
anchor, so it is *probably* the head of `gameui.cpp` — inferred from layout, not
proven. Most constructors register a string id, which is what pins the identity.

| tool | ctor | action id string | `Step` (vt slot 5) | **commit method** | command |
|---|---|---|---|---|---|
| `UI::StreetBuilder` | `4453b0` | `action-streetbuilder` | `4575c0` | **`UpdateEngine` `459ce0`** | BuildProposal |
| `UI::ConstructionBuilder` | `40fdb0` | `action-constructionbuilder` | `41a9d0` | **`MousePressed` `419aa0`** | BuildProposal |
| `UI::ModuleBuilder` | `429d80` | `action-modulebuilder` | `42d750` | **`MousePressed` `42b810`** | BuildProposal |
| `UI::StreetTerminalBuilder` | `45eb80` | `action-streetterminalbuilder` | `4612e0` | **`460a30`** | BuildProposal |
| `UI::TrackModifier` | `474e00` | `action-trackmodifier` | `47c7b0` | **`Build` `478d70`** | BuildProposal |
| `UI::Bulldozer` | `3e2450` | `action-bulldozer` | `3ec380` (likely) | **`Apply` `3eaeb0`** | BuildProposal |
| `UI::ProposalAction` | `4304d0` | `action-proposal` | — | **`4310d0`** | BuildProposal |
| ↳ `UI::TerrainModifier` | `465620` | (base's id) | `46ac40` | slot 14 `4684a0` → `4310d0` | BuildProposal |
| ↳ `UI::TerrainPainter` | `46d080` | (base's id) | `46dfd0` | slot 14 `46d7d0` → `4310d0` | BuildProposal |
| ↳ `UI::AssetBrush` | `3cc650` | (base's id) | `3d4930` | slot 14 `3d1110` → `4310d0` | BuildProposal |
| `UI::TownBuilder` | `46f690` | `action-townbuilder` | `470cc0` | **`4705c0`** | CreateTowns |
| `UI::CSelector` | `437d20` | (ids `action-selector` / `action-inspector` are registered by the caller, `gameui.cpp` `569f00`) | `43b360` | `Select` `43b210` | none (selection only) |
| `UI::CameraAction` | `3f7010` | `action-camera` | — | `404b00`,`4059a0` | SetGameSpeed |
| `UI::FollowAction` | `41fd20` | `action-follow` | — | — | none |

Note `UI::Bulldozer`'s vftable layout differs from its siblings (it appears to
derive from `IAction` directly rather than `BasicAction`), so its slot-5 `Step`
is **LIKELY**, not confirmed; its `Apply` at `3eaeb0` *is* confirmed by
`__FUNCSIG__`.

`UI::IEngine` / `ProposalEngine` `4332b0` / `CachedProposalEngine` `3f2110` /
`WrappedEngine` are a 20-slot **read-only world-query interface** used to build
the preview. They issue no commands — don't chase them.

---

## Per-action map

`make_cmd` factory names marked `*` are pinned by strictly-alphabetical address
order inside `make_command.cpp` (the 19 factories that carry a `__FUNCSIG__`
prove that ordering) rather than by their own signature string; the rest are
verbatim from `__FUNCSIG__`. Unless a row says otherwise, its call site was
verified to call both the factory and `CommandList::Add` in the same function.
The exceptions, all noted inline, are the three `ProposalAction` subclasses
(which reach the factory one hop later, through `4310d0`), `line_util`
`215c180` (builds the command, its caller queues it) and the sol2 wrappers
(which hand the command to Lua, and `api.cmd.sendCommand` queues it).

### Construction family — all one command type

`make_cmd::BuildProposal` = **`0x9dc750`**, `CmdData::BuildProposal`.

| player action | UI site | file | conf |
|---|---|---|---|
| build road / rail track | `459ce0` `UI::StreetBuilder::UpdateEngine` | `ui/actions/streetbuilder.cpp` | **CONFIRMED** |
| build station / depot / asset / industry | `419aa0` `UI::ConstructionBuilder::MousePressed` | `ui/actions/constructionbuilder.cpp` | **CONFIRMED** |
| add a station module (upgrade a construction) | `42b810` `UI::ModuleBuilder::MousePressed` | `ui/actions/modulebuilder.cpp` | **CONFIRMED** |
| add module from the construction window list | `4b6f00` | `ui/components/addmodulecomp.cpp` | LIKELY |
| build bus/tram stop (street terminal) | `460a30` | `ui/actions/streetterminalbuilder.cpp` | **CONFIRMED** |
| place signals / waypoints, modify track | `478d70` `UI::TrackModifier::Build` | `ui/actions/trackmodifier.cpp` | **CONFIRMED** |
| demolish / bulldoze | `3eaeb0` `UI::Bulldozer::Apply` | `ui/actions/bulldozer.cpp` | **CONFIRMED** |
| **terraform** (raise/lower/level/smooth) | slot 14 `4684a0` → `4310d0` | `terrainmodifier.cpp` → `proposalaction.cpp` | **CONFIRMED** |
| paint terrain texture | slot 14 `46d7d0` → `4310d0` | `terrainpainter.cpp` → `proposalaction.cpp` | **CONFIRMED** |
| asset brush (scatter trees/rocks) | slot 14 `3d1110` → `4310d0` | `assetbrush.cpp` → `proposalaction.cpp` | LIKELY |
| toggle a signal's direction from its window | `89acf0` `ActualViewCreator::CreateSignalView::<lambda>(bool)` | `ui/viewcreator.cpp` | **CONFIRMED** |
| three more entity-window construction edits | `8983c0`, `8990a0`, `89bb50` | `ui/viewcreator.cpp` | LIKELY (file only) |
| town-editor building placement | `71a350`, `71a720`, `71aaf0` | `ui/components/towngrowthcomp.cpp` | LIKELY (file only) |
| construction menu bulk/industry placement | `597e70` `CGameUI::CreateConstructionMenu::<lambda>` | `ui/components/gameui_menu.cpp` | **CONFIRMED** |
| script `api.cmd.make.buildProposal` | `ced1b0` (sol2 wrapper) | `scripting/gamescriptrep.cpp` | **CONFIRMED** |

**Demolishing a town is the one exception**: `UI::TownBulldozerAction` overrides
slot 4 with `499010`, which issues `make_cmd::RemoveTown` `0x9dd920` instead.
The other seven `*BulldozerAction` subclasses (`Asset`, `Building`, `Field`,
`Module`, `Street`, `StreetConnector`, `StreetTerminal`) only override slot 2
(the filter) and go through `Bulldozer::Apply` → BuildProposal.

### Vehicles

| player action | factory | UI site | file | conf |
|---|---|---|---|---|
| buy vehicle | `BuyVehicle` `9dca00` | `74f5e0` | `ui/components/vehiclemanager.cpp` (`vehicle-manager`) | **CONFIRMED** |
| sell vehicle | `SellVehicle` `9de380` | `748b50` (`train-depot-window`), `887130` | `vehiclemanager.cpp`, `ui/util/vehicle_button_util.cpp` | **CONFIRMED** |
| replace vehicle | `ReplaceVehicle` `9dddb0` | `74f5e0`, `220fa80` | `vehiclemanager.cpp`, `transport/vehicle_util_2.cpp` | **CONFIRMED** |
| **assign vehicle to line** | `SetLine` `9dea10` | `88afd0` `UI::vehicle_button_util::SetLineAllDo`, `886a80` `SetLineAllInstallHandlers::<lambda>`, `88b840`, `8b51d0` | `vehicle_button_util.cpp`, `viewcreator.cpp` | **CONFIRMED** |
| send to depot | `SendToDepot` `9de6f0` | `750670`, `88c2f0` | `vehiclemanager.cpp`, `vehicle_button_util.cpp` | **CONFIRMED** |
| stop / start a vehicle | `SetUserStopped` `9df070` | `88aa90` | `vehicle_button_util.cpp` | LIKELY (file only) |
| reverse a vehicle | `Reverse` `9ddfe0` | `8b5510` | `ui/viewcreator.cpp` | LIKELY (file only) |
| set maintenance target | `SetVehicleTargetMaintenanceState` `9df340` | `74a490` | `vehiclemanager.cpp` | LIKELY (file only) |
| manual departure | `SetVehicleManualDeparture` `9df170` | only the sol2 wrapper `cee710` | `scripting/gamescriptrep.cpp` | factory **CONFIRMED**; no direct UI caller found |
| recolour a vehicle | `SetColor` `9de8a0` | `724d30`, `747d90`, `8b4510` | `vehicledetailscomp.cpp`, `vehiclemanager.cpp`, `viewcreator.cpp` | LIKELY (file only) |

### Lines

| player action | factory | UI site | file | conf |
|---|---|---|---|---|
| **create line** | `CreateLine` `9dcde0` | `610380` / `618ff0` → `line_util` helper `215c180` → factory | `linelist.cpp`, `linemanager.cpp`, `transport/line_util.cpp` | **CONFIRMED** (helper decompiled) |
| **add / edit / remove line stops** | `UpdateLine` `9df4e0` | `5fe260` `UpdateLineAssignment`, `607190` `UI::LineEditor::DeleteTerminal`, `5fe9e0`, `6033c0`, `603a00`, `603fa0`, `60b970` | `ui/components/lineeditor.cpp` | **CONFIRMED** |
| edit stop terminal / waypoint lane | `UpdateLine` | `7b43b0` `CreateStationTerminalComboBox::<lambda>`, `7b7420` `CreateAlternativeTerminalsButton::<lambda>`, `7bab50` `CreateWaypointLaneComboBox::<lambda>` | `ui/components/line_ui_util.cpp` | **CONFIRMED** |
| edit line from station window | `UpdateLine` | `6eeb60`, `6fd3a0` | `sectiontypecomp.cpp`, `stationgroupterminalscomp.cpp` | LIKELY (file only) |
| delete line | `DeleteLine` `9dd190` | `7bf590` | `line_ui_util.cpp` | LIKELY (file only) |
| recolour line | `SetColor` `9de8a0` | `60b740` | `lineeditor.cpp` | LIKELY (file only) |

### Everything else

| player action | factory | UI site | file | conf |
|---|---|---|---|---|
| rename anything | `SetName` `9deb70` | `7b8200`, `8829c0` | `line_ui_util.cpp`, `ui/util/util.cpp` | LIKELY (file only) |
| take / repay a loan | `Book` `9dc5e0` | `53b660`, `53b8b0` | `ui/components/financescomp.cpp` | LIKELY (file only) |
| change game speed / pause | `SetGameSpeed*` `9de9e0` | `4efab0`, `4f2640`, `4eff50`, `574a70`, `65eb60`, `657710`, `404920`, `404b00`, `4059a0`, `795900` | `clock.cpp`, `gameui.cpp`, `menuui.cpp`, `cameraaction.cpp` | LIKELY |
| set date | `SetDate*` `9de9b0` | `4efca0` | `clock.cpp` | LIKELY |
| set calendar speed | `SetCalendarSpeed*` `9de870` | `4f29f0` | `clock.cpp` | LIKELY |
| save game | `SaveGame*` `9de0e0` | `563320`, `5ebda0` | `gameui.cpp`, `ingamemenuui.cpp` | LIKELY |
| map editor: create towns | `CreateTowns*` `9dd0b0` | `4705c0` (TownBuilder), `596d80` | `townbuilder.cpp`, `gameui_menu.cpp` | **CONFIRMED** (decompiled) |
| map editor: remove town | `RemoveTown` `9dd920` | `499010` (`TownBulldozerAction` slot 4), `596d80` | `bulldozer/*`, `gameui_menu.cpp` | **CONFIRMED** |
| map editor: replace terrain | `ReplaceTerrain*` `9dda20` | `599470` | `gameui_menu.cpp` | LIKELY |
| map editor: connect towns & industries | `ConnectTownsAndIndustries*` `9dcbf0` | `5c6d30` | `generatestreetscomp.cpp` | LIKELY |
| scenario editor: no-costs toggle | `SetNoCosts*` `9ded50` | `7d0360` | `scenario_editor_ui_util.cpp` | LIKELY |
| town editor: town info / cargo needs | `SetTownInfo*` `9def80`, `InstantlyUpdateTownCargoNeeds*` `9dd2e0` | `711fc0`, `70edb0`, `70fb90`, `711e10` | `towneditorcomp.cpp`, `townconnectioncomp.cpp` | LIKELY |
| mod subscribe (journal entry) | `Book` | `6ca230` | `modsbrowser.cpp` | LIKELY |

---

## The command factories — `game/command/make_command.cpp`

Nineteen are named verbatim by `__FUNCSIG__`; the rest are pinned by the strict
alphabetical address ordering those nineteen establish.

| RVA | `make_cmd::` | evidence |
|---|---|---|
| `9dc5e0` | `Book` | `__FUNCSIG__` |
| **`9dc750`** | **`BuildProposal`** | position + 17 builder callers + decompiled + Lua arity |
| `9dca00` | `BuyVehicle` | `__FUNCSIG__` |
| `9dcbf0` | `ConnectTownsAndIndustries` | position + `generatestreetscomp.cpp` caller |
| `9dcde0` | `CreateLine` | `__FUNCSIG__` |
| `9dd0b0` | `CreateTowns` | position + decompiled `TownBuilder` caller |
| `9dd190` | `DeleteLine` | `__FUNCSIG__` |
| `9dd290` | `DevelopTown` | position (script-only caller) |
| `9dd2e0` | `InstantlyUpdateTownCargoNeeds` | position |
| `9dd820` | `RemoveField` | `__FUNCSIG__` |
| `9dd920` | `RemoveTown` | `__FUNCSIG__` |
| `9dda20` | `ReplaceTerrain` | position |
| `9dddb0` | `ReplaceVehicle` | `__FUNCSIG__` |
| `9ddfe0` | `Reverse` | `__FUNCSIG__` |
| `9de0e0` | `SaveGame` | position + `ingamemenuui.cpp` caller |
| `9de380` | `SellVehicle` | `__FUNCSIG__` |
| `9de490` | `SendScriptEvent` | position + `cmd_interface.cpp` caller |
| `9de6f0` | `SendToDepot` | `__FUNCSIG__` |
| `9de7f0` | `SetAnimalState` | position + `animal_util.cpp` caller |
| `9de870` | `SetCalendarSpeed` | position + `clock.cpp` caller |
| `9de8a0` | `SetColor` | `__FUNCSIG__` |
| `9de9b0` | `SetDate` | position + `clock.cpp` caller |
| `9de9e0` | `SetGameSpeed` | position + clock/menu/camera callers |
| `9dea10` | `SetLine` | `__FUNCSIG__` |
| `9deb70` | `SetName` | `__FUNCSIG__` |
| `9ded50` | `SetNoCosts` | position + scenario-editor caller |
| `9ded80` | `SetSimBuildingClosureTimeStamp` | `__FUNCSIG__` |
| `9dee80` | `SetSimBuildingManualDevelopment` | `__FUNCSIG__` |
| `9def80` | `SetTownInfo` | position |
| `9df070` | `SetUserStopped` | `__FUNCSIG__` |
| `9df170` | `SetVehicleManualDeparture` | `__FUNCSIG__` |
| `9df270` | `SetVehicleShouldDepart` | position |
| `9df340` | `SetVehicleTargetMaintenanceState` | `__FUNCSIG__` |
| `9df480` | `SpawnAnimal` | position |
| `9df4e0` | `UpdateLine` | `__FUNCSIG__` |
| `9df710` | `UpdateLogo` | position |

`make_cmd::BuildProposal` decompiled signature:

```
Command* BuildProposal(Command* ret, Engine*, Proposal* proposal, Context* context,
                       bool, bool ignoreErrors)
```

which matches the mod's own `api.cmd.make.buildProposal(proposal, context,
ignoreErrors)`. All factories share the Command-packaging helper `0x9dd6a0`.

The whole set is also exposed to Lua: `SetupCommandInterface` `0xd042e0`
registers the `api.cmd.make.*` names, and each name has a sol2 wrapper in
`0xcecf60`–`0xcef200` that calls exactly one factory. Those wrappers do **not**
call `CommandList::Add` — `api.cmd.sendCommand` does, via
`cmd_interface.cpp` `0x1126cd0`.

---

## `CommandList::Add` — `0x9d2a00`

Role **CONFIRMED** by decompilation; element type fixed by
`CommandList::Swap(std::vector<Command>&)` `0x9d2cf0` (`__FUNCSIG__`). The name
`Add` is inferred: `0x9d2420`, 1.5 KB earlier in the same file, is
`CommandList::Add::<lambda_7152b1228d6642b2ea6daae905652661>::operator()` — a
lambda that can only have been defined inside `CommandList::Add`.

```
? CommandList::Add(CommandList* list, ?, Command* cmd,
                   std::function<void(Command const&)>* callback, ?* )
```

- Appends the command to a `std::vector<Command>`; the pointer bump is `+0x38`,
  so **`sizeof(Command) == 0x38`**.
- The 4th argument is the completion callback. Every UI call site materialises a
  `std::_Func_impl_no_alloc<<lambda_…>, void, Command const&>` vftable for it —
  that is the same `function(result, success)` the Lua API exposes.
- 82 direct call sites. Seventy-eight are in `game\ui\…`, `scripting\legacy\
  cmd_interface.cpp`, `transport\vehicle_util_2.cpp` or `ecs\animal_util.cpp`
  and pair with a visible `make_cmd` factory. Of the four that do not,
  `linelist.cpp` `0x610380` and `linemanager.cpp` `0x618ff0` reach the factory
  through `line_util` `0x215c180`; `game.cpp` `0x119d30` and `menuui.cpp`
  `0x67e370` are **UNIDENTIFIED**.

---

## The apply side

`CGame::RunGameSimLoop` `0x1184d0` → `apply_command.cpp` `0x9da290`, whose
profiler scope string is `"Simulation Thread: Apply Command"`. It validates the
command's entities (`"Apply: invalid entity: "`), then dispatches through a
**37-entry function-pointer table at RVA `0x30b10c0`**, indexed by a
`std::variant` tag byte read as `*(char*)(*param_2 + 0xb18)` — one pointer
dereference from the executor's second argument, so `+0xb18` is an offset into
the command payload, not into `Command` itself; the exact base is unconfirmed.
Bound check `index > 0x24` throws, i.e. thirty-seven valid indices for
thirty-seven `CmdData` types — they match.

Resolved tag indices (via `__FUNCSIG__` on the handler each entry reaches):

| tag | handler | `CmdData::` |
|---|---|---|
| 2 | `9d54a0` | UpdateLogo |
| 7 | `9d5620` | Reverse |
| 8 | `9d9e10` | SetUserStopped |
| 9 | `9d5720` | SetVehicleTargetMaintenanceState |
| 10 | `9d5770` | SetVehicleShouldDepart |
| 12 | `9d8c90` | SellVehicle |
| 13 | `9d6280` | BuyVehicle |
| 14 | `9d8110` | ReplaceVehicle |
| **15** | **`9d6e20`** | **BuildProposal** |
| 22 | `9d70c0` | ConnectTownsAndIndustries |
| 28 | `9d98a0` | SetColor |
| 31 | `9d6bc0` | Book |

The variant order is **not** alphabetical; the other 25 handlers carry no
assert and are not yet named.

### `Visitor::operator()(CmdData::BuildProposal&)` — `0x9d6e20` — CONFIRMED

The single construction-apply handler. Decompiled, it:

1. reads the proposal from the command payload at **`+0x370`**,
2. calls `construction_builder_util::CreateProposalData` **`0xa072b0`**
   (`__FUNCSIG__`-confirmed; impl at `0xa07ab0`),
3. calls `applyProposal` **`0x9e76e0`**.

This resolves the `9d6f93` / `9d6f8e` row that `APPLYPROPOSAL_CALLERS.md` left
as "unidentified". It is not UI-specific — it is **player-command-specific**,
which is the property that document was actually looking for: it fires once per
successful construction command from *either* the UI or a script, and never for
town growth.

---

## Corrections to earlier documents

1. **`UI_CAPTURE_PATH.md`: `StreetBuilder::UpdateEngine` does not emit a
   `signals2` signal.** Its decompiled body calls `make_cmd::BuildProposal`
   `0x9dc750` and then `CommandList::Add` `0x9d2a00`, directly, in consecutive
   statements. The four `0x3d07f0 / 0x3e3270 / 0x3e4180 / 0x3e9510` calls that
   were read as signal emitters are construct/copy/destroy helpers on the
   proposal object: `0x3e3270` copies a ~768-byte object into a stack local that
   is then passed as `BuildProposal`'s proposal argument, and `0x3d07f0`
   destroys it — the same pair appears, used the same way, in
   `UI::ProposalAction`'s commit `0x4310d0` and at the tail of
   `make_cmd::BuildProposal` itself. The conclusion "UpdateEngine is the right
   interception point" survives; the reasoning about async dispatch does not,
   and the actual mechanism is far easier to hook.

2. **`M2_RESULTS.md` §5a: "Sell vehicle → factory `0x9de8a0`" is wrong.**
   `0x9de8a0` is `make_cmd::SetColor`. The sell factory is `make_cmd::SellVehicle`
   **`0x9de380`**. The same row's apply-side address `0x9d8c90` *is* correct —
   `__FUNCSIG__` names it `Visitor::operator()(CmdData::SellVehicle&)`.

3. **`M2_RESULTS.md` probe3: "CommandList disproven for UI" is wrong.** Eleven
   `apply_command` functions were probed and stayed silent during builds because
   the construction handler is `0x9d6e20`, which carries no assert and so was
   not in the probed set. Construction goes through `CommandList` like
   everything else.

4. **`APPLYPROPOSAL_CALLERS.md`: `0x21ed420` is not terrain modification.** Its
   source file is `transport/street/construction_util_street_upgrade.cpp` and
   its callers are `urbansim/simulation_util.cpp` — town-driven street
   upgrading. The terrain symbols seen in it are inlined helpers. Verdict
   "background, not the player command" is unchanged; the label was not.

5. **`APPLYPROPOSAL_CALLERS.md`: `0x984e30` labelled "script-only".** It is
   `urbansim/street_developer_util.cpp` — town street development, a background
   system. "Script-only" was a frequency artifact of a single sample. Related:
   `0x962940` is `urbansim/parcel_util.cpp`, and `0x95c810` is
   `anonymous-namespace::CreateBuildingAssets` in the same file, confirming the
   town-growth reading.

6. Confirmed unchanged: `0xa74070` is `ecs::SimBuildingSystem::Update2`
   (`__FUNCSIG__` exact), `0xb6e040` is `procedural/buildingtyperep.cpp`.

---

## What this means for the lockstep hook

- **`CommandList::Add` `0x9d2a00` is the single capture point for all player
  actions.** One hook replaces the twelve the previous design implied.
- **Origin is discriminable at the call site.** UI commands arrive from
  `game\ui\…`; replayed script commands arrive from `cmd_interface.cpp`
  `0x1126cd0`. The existing return-address relay already reads this.
- **Type is a byte, once the base is pinned down.** The executor reads the
  variant tag as `*(char*)(*arg2 + 0xb18)` and tag 15 is BuildProposal, so
  classifying a command is one load — but confirm the base pointer against live
  bytes before relying on it.
- **Deferral is structurally clean.** The 4th argument is a
  `std::function<void(Command const&)>` completion callback, so a suppressed
  command has a defined way to report back later.
- **Leave `applyProposal` alone.** `applyProposal` should not be hooked at
  all: 21 of its 22 callers are engine systems, not player commands.

---

## Gaps and honest unknowns

- **25 of 37 dispatch-table indices are unnamed.** Only the twelve handlers with
  asserts could be named. Naming the rest needs decompiling each thunk target
  and matching payload field use — mechanical, not done.
- **`Command` layout beyond `sizeof == 0x38` and `tag @ payload+0xb18` is
  unmapped.** `CmdData::BuildProposal` holds its proposal at `+0x370`
  (from the apply handler); the rest is unverified. Nothing here was
  cross-checked against live bytes, and `PROPOSAL_STRUCTURE.md`'s warning about
  that still applies.
- **The `9dc750` name rests on inference, not its own `__FUNCSIG__`.** Four
  independent supports (alphabetical slot, 17 construction-only callers, a
  decompiled body that calls `make_proposal.cpp` helpers, and an argument list
  matching the Lua `buildProposal` arity) — but no signature string.
- **`UI::Bulldozer::Step`** is slot-inference only; that class's vftable does
  not match its siblings' layout.
- **Rows marked "LIKELY (file only)"** are placed by source file plus the
  factory they call, with no signature string and no decompilation. The factory
  identification is solid; which *button* in that file is not.
- **Nothing here was measured at runtime.** Every claim is static. The
  `CommandList::Add` count of 82 call sites is a static count; how many fire per
  player action, and whether preview/drag produces spurious ones, is untested.
- **Unmapped action types**: keyboard shortcuts (`game_ui_key_cmd.cpp`
  `0x79c420`) were not traced; they most likely re-enter the same tools.
  Camera movement, HUD filters and window state issue no commands at all and are
  correctly out of scope for lockstep.

---

## Reusable tooling added

| path | what |
|---|---|
| `tools/ghidra_scripts/DumpStringXrefs.java` | every string + referencing functions → `strings.csv` |
| `tools/ghidra_scripts/DumpVtables.java` | RTTI vftable contents → `vtable_dump.csv` |
| `tools/ghidra_scripts/DumpCallEdges.java` | whole direct call graph → `call_edges.csv` |
| `tools/ghidra_scripts/DumpPtrTable.java` | raw function-pointer table, following thunks |
| `tools/ghidra_run.ps1` | generic single-script headless runner |
| `tools/funcsig.py` | `strings.csv` → `funcsig.csv` (the symbol table) |
| `tools/func2src.py` | `strings.csv` → `func2src.csv` / `src2func.csv` |
| `tools/src_ranges.py` | source file → address range |
| `tools/whois.py` | RVA → signature + source file + vftable slot |
| `tools/xq.py` | callers / callees / strings, joined with source files |
| `tools/cmdmap.py` | regenerates the per-action table above |

Outputs live in `C:\tools\ghidra_out\`. Regenerate with
`ghidra_run.ps1 DumpStringXrefs.java C:\tools\ghidra_out 4`, then
`funcsig.py` and `func2src.py`. **Never run two Ghidra scripts concurrently** —
the project is locked to one process.
