# Shared infrastructure prototype — 0.4.25

Companies keep ownership of infrastructure. Other companies run services over the
same tracks and use the same stations. There is no leasing fee or Workshop mod
dependency. This is an experimental implementation: live foreign station
selection, pathfinding, maintenance accounting and save/reload still need
verification with two instances. Offline mocks do not prove those engine behaviors.

## Using another company's station on your line

`AddStationInputComponentChecker::IsValidInput` (RVA `609b00`) does not contain a
player-owner test, and that earlier note sent the search to the wrong function.
The gate is in its **caller**. In companies mode a player clicking another
company's station in the line editor got nothing at all — no stop, no message,
no sound — because `UI::_anon_46B7A670::StationFilter::IsValid` (`6095d0`,
vftable slot 1 of `3010848`) rejects the entity before anything else sees it:

```
609621  call 472900            ; GetComponentPtr<ecs::component::PlayerOwned>
609626  test rax, rax
609629  je   609605            ; no owner  -> accept
60962b  mov  eax, [rax]        ; PlayerOwned.player
60962d  test eax, eax
60962f  js   609605            ; owner < 0 -> accept
609631  cmp  eax, [rbx+0x28]   ; <-- THE GATE (filter+0x28 = the local player)
609634  je   609605            ; same owner -> accept
609636  xor  eax, eax          ; another company -> REJECT
```

The selector asks that filter whether an entity under the cursor may be reported
at all (`UI::IFilter` slot 1 through the accept helper at `439ea0`), so a "no"
makes the station invisible to the tool and the click lambdas — `60bea0`, which
actually adds the stop, and `60c040`, the cursor hint — never run. Neither of
those two has an owner test of its own, which is why the refusal is silent.
`StationFilter+0x28` is the local human player entity, threaded from
`UI::CGameUI::CreateUI` (`56a121` → `8bb7f0`, then `mov edi,[rax+0x214]`) through
the line list (`613340`) and the `LineEditor` constructor (`5fa970`, argument 15)
into the filter (`5fc857`). `GameState+0x214` is the same field the entity-window
gates `8b3020` and `8a3c20` compare against and the one `GameState::Replicate`
copies.

### Every owner gate on the path, and what was done about it

The inventory is complete for this build: 43 inlined `PlayerOwned` type-descriptor
sites plus every caller of the two accessors (`472900`, `c5e20`).

| gate | RVA | what it decides | with a foreign owner | action |
|---|---|---|---|---|
| `StationFilter::IsValid` | `609631` | may the line editor see this station/edge at all | silently invisible | **patched** (slice, `sharedstations`) |
| `IsValidInput` | `609b00` | what kind of thing was clicked | no owner test | none needed |
| add-stop / cursor lambdas | `60bea0`, `60c040` | adds the stop, colours the cursor | no owner test | none needed |
| `make_cmd::UpdateLine` | `9df4e0` | builds the command | asserts only `lineEntity != Entity()` | none needed |
| UpdateLine handler (variant tag 5) | `9d9fd0` | writes the new `component::Line` | no owner test — a replayed foreign stop is **not** stripped | none needed |
| `LineSystem::EntityAdded` | `a43400` | line indexing | asserts stops have station groups; reads only the **line's** owner, for the per-player line index | none needed |
| `GetBestLineAssignment` | `215d660` | terminal choice per stop | no `PlayerOwned` read at all | none needed |
| `CalcSectionPaths`, `CalcLineStopTerminal` | `215a050`, `96f3b0` | line section paths | no `PlayerOwned` read | none needed |
| `FindNextFreeTerminal`, `ComputeTerminalConnectivity` | `ad40b0`, `a42540` | which platform a vehicle takes | no `PlayerOwned` read | none needed |
| `station_util::GetCarriers`, `GetTerminalPersonEdges` | `218d720`, `218f370` | what a terminal accepts, how people reach it | no `PlayerOwned` read | none needed |
| `LinesExpander::AddStation` / `VisitLines` | `977410`, `977640` | which lines a person may board from a station | no `PlayerOwned` read | none needed |
| `ChargeRunningCosts`, `HandleVehicleArrived` | `ad11d0`, `ad5f70` | who pays, who is notified | reads the **vehicle's** own owner | left alone (see Money) |
| `UI::GetEntitiesForPlayer` | `73d8d0` | "my stations/vehicles/lines" lists | excludes foreign | left alone — correct for companies mode |
| entity window sections | `8b3020`, `8a3c20` | rename/bulldoze/finances UI | disabled for foreign | left alone — matches the mod's refusals |
| `FindPathToDepot` | `216fa52` | which depot a vehicle services in | requires the depot's owner to match | **left alone on purpose**: B's vehicles service in B's depots |

So the whole feature is one comparison. The patch does not change ownership: the
station stays A's, `cmMayModify` still refuses B's edits and demolition, and the
line, its fleet and its income stay with B.

### The patch

`slice_hook.cpp` (`SHARED STATIONS`) steals the five bytes at `609631` — the
`cmp` and the `je`, two whole instructions, nothing branches into them — and
jumps to a stub that asks a helper and then jumps to the engine's own accept
(`609605`) or reject (`609636`) tail. The helper replays the engine's comparison
and answers "same owner" for a foreign owner only while companies mode is live
(line 1 of `mp_company_cfg.txt`, cached for 2 s because the filter runs on
hover). Outside companies mode there is only one player entity, so that
comparison cannot fail and the patch is a no-op — the mode check is belt and
braces, not the safety. The install refuses unless all 46 guarded bytes match
build 35924 and both `rel32` calls inside them resolve to `8b9e60` and `472900`.

**Kill switch:** `sharedstations=0` in `tpf2_menu_flags.txt` (next to the dlls, or
in the data dir) leaves the comparison alone and foreign stations stay unusable.

### Money

Vanilla has no leasing and none was invented. What the engine does:

- **Station maintenance stays with the station's owner.** Ownership does not
  change, and nothing on the line path re-owns a stop's construction.
- **The line's income and its vehicles' running costs stay with the line's
  company.** `TransportVehicleSystem::ChargeRunningCosts` (`ad11d0`) and
  `HandleVehicleArrived` (`ad5f70`) read the **vehicle's** own `PlayerOwned` and
  book against that player; neither looks at the station.
- **A stops at nothing, B pays nothing to A.** There is no transfer anywhere on
  this path, so A subsidises B's use of the platform. That is the vanilla
  behaviour of a shared station and is left as it is.

## Changes

- Line ownership transfers change the line and its fleet, including parked
  vehicles. They bypass the stock `setPlayer(line, player)` behavior that also
  transfers stop stations, station constructions and track edges.
- Construction ownership transfers retain visiting vehicles' original owners.
  Company switches use the same scoped setters.
- Road and rail proposals use the originating company's engine player in their
  build context. This covers polylines whose result entity list is empty. Since
  the engine charges that player directly, the old refund/charge settlement is
  skipped for those proposals.
- Cancelled construction upgrades, construction demolition and road/track
  replacement captures refuse changes to foreign-owned infrastructure. Existing
  engine ownership restrictions remain intact. Edge demolition that already ran
  natively is still replicated; refusing its replay would create a divergence.
- Co-op uses the existing build context and stock ownership behavior.

The replacement guard also checks the other transport network because a road
proposal can refresh a rail bridge. It currently refuses replacement of a foreign
span even when the engine only intended to refresh it unchanged. Test connections
and roads beneath foreign bridges before treating this as production-ready sharing.

## Native extension

`setplayer_patch.cpp` retains the generic-entity assertion fix at RVA 11677a1.
An additional guarded 18-byte detour at 11673da runs **after** the binding has
initialized its cleanup object. The MASM relay recognizes an explicitly encoded
player argument (`0x60000000 + playerId`) and dispatches to the engine's generic
owner setter block at 11677a3. Ordinary arguments reconstruct the original
construction/line dispatch. Player IDs must fit in 28 bits.

The bridge advertises `entity_owner_v1=1` in its current identity file only after
installation succeeds. Lua will not send encoded arguments without that capability.
An identity-file flag avoids relying on separate CRT environment snapshots. The
new protocol version prevents connecting a 0.4.25 peer to older ownership behavior.

## Validation and next live test

`tools/shared_infra_test.py` checks ownership isolation, visiting depot vehicles,
permission checks through the real capture parser, capability failures, executable
instruction guards, and 14 paths through the actual assembled relay using Unicorn.
`tools/crossing_replay_test.py` additionally checks distinct per-peer company IDs in
real road proposal generation and verifies that empty result lists do not trigger
duplicate settlement. Line-create, bridge companion and version-gate checks pass.

`tools/sharedstations_bytes_test.py` checks the patched site against the installed
executable: the 46 guarded bytes, both `rel32` targets, that the five stolen bytes
are two whole instructions with nothing branching into them, that both jump targets
are the function's own tails, that the function really is vftable slot 1 of the
line editor's filter and really calls `IsValidInput`, that only classes 0 and 1
reach the comparison, that the hand-assembled stub decodes to exactly the intended
instructions, and that neither `make_cmd::UpdateLine` nor the handler at dispatch
tag 5 reads an owner. `tools/shared_stations_line_test.py` drives the real
`lines.lua`/`companies.lua` with a line whose two stops belong to two companies:
the snapshot ships both by position, the replay rebuilds both, the handover uses
the scoped setter so the foreign stop keeps its owner, `cmRepairLineOwners`
follows the vehicles and never a stop, and `cmMayModify` still refuses the edit.

### Next live test — B runs a line through A's station

Two-instance rig, companies mode, A = company 1 (host), B = company 2. Nothing
below has been run: everything above is static analysis and offline mocks.

1. **Setup.** A builds a bus/tram station and a second one some distance away,
   plus the road between them. B builds a depot of its own and nothing else.
   Confirm in both `mp_company_<letter>.log` that the two stations report A's
   pid and the depot B's.
2. **The click — the thing this change is for.** As B, open the line manager,
   New line, "Add station", click **A's** station. It must land as stop 1.
   Expect in B's `tpf2_slice.log`, once:
   `[sharedstations] line editor accepted a stop owned by player <A> (we are <B>)`.
   If the click still does nothing, check the same log for
   `[sharedstations] installed rva=609631 …` — no line means the byte guard or
   the kill switch refused, and the log says which.
3. **Save the line.** Add B's own second stop, close the editor. On BOTH peers
   the new line must list two stops in the same order with the same names.
   In each `lockstep_<letter>.log` look for `EXEC LCREATE … stops=2 success=true`
   and, for each later edit, `EXEC LUPDATE … stops=2 success=true`; a
   `no station group within 20 m` error means the stop did not resolve on that
   peer and the line there is wrong.
4. **Ownership did not move.** On both peers the line is B's (it appears in B's
   line list and not in A's) and A's station is still A's (B's rename/upgrade/
   bulldoze on it are refused with "only its owner can change it"). Watch for
   `CM: reassigned line eid=… -> co2` and for any
   `CM: line … re-owned to the vehicles' company` — the latter is expected only
   if the fleet disagrees with the line, never because of a stop.
5. **Vehicles path and load.** B buys a bus in B's depot and assigns it to the
   line. It must drive to A's station, stop at a platform and pick passengers
   up. Failure modes to name in the log: the no-path toast (cosmetic, see
   `docs` note on our SetLine cancel) versus a real `stops=2` line with a
   vehicle that never leaves the depot; and `CONFAIL` for the station, which
   would mean the station itself was rolled back, not the line.
6. **A is unaffected.** A's own line through the same station keeps running;
   both companies' vehicles use the platforms; A's balance shows the station's
   maintenance and B's shows the bus's running cost and the fares.
7. **Determinism.** Let it run several minutes at >1x with the hash on. No new
   `desync` lines on either peer; `applylag` unchanged.
8. **Save and reload.** `/sync` or a manual save, reload on both. The line still
   has two stops, still belongs to B, A's station still belongs to A.

**Failure modes to watch for.** The known crash class near another company's
roadside stops (0.4.14: heap corruption replaying a terminal that removed an
edge; an AV in `construction_util::ContactCallback::FilterEntity` →
`GetComponentDataIndex` with an out-of-table entity id) is unproven but lives on
exactly this ground. Do step 2 on a **construction** station first, not a
roadside stop; only then repeat with a roadside stop, and if the game dies,
snapshot the logs (`tools/snapshot_logs.ps1`) before anything else and note
whether the last capture was a `STOPX`/`STOPXDEL`. A vanishing station with no
player action is `CONFAIL`, not this. Do not equate successful offline checks
with any of these results.

The local update bundle is built separately from the running installed release;
building it does not activate it or restart either game.
