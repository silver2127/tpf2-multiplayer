Every item below was left unported by the static-only runs of 2026-09-16/17. Each has an integration record
(`docs/linux/UPSTREAM_dev_<sha>.md`) and most an RE note (`docs/re/linux/DEV_<SHA>.md`) with the addresses
already located and the exact contract still unproven. Do not redo the static work: read the note, then
settle the open contract in the running lab game with gdb, and build on it. Most valuable first.

1. **Company tinting on the HUD** (station/depot icons, vehicle icons, station labels, entity-window wash;
   Windows `stationicon`, `windowtint`, palette and counters commits; records `DEV_D6DB920F`, `DEV_50D7588B`,
   `DEV_66C870CF`, `DEV_5FB7AEA2`, `DEV_61578D27`, `DEV_DB8A4776`, `DEV_8E31F1E0`, `DEV_A658FC11`,
   `DEV_59BB258A`, `UPSTREAM_dev_23419163`). Known: Linux `StationItem` constructor `0x1090250` (entity in
   r9d, EnginePtr in rsi, accessor `0x146f0a0`), its callers in `HudIconManager::DoStep` (`0x1095972`) and the
   rebuild function (`0x10920b5`), eleven station and five depot carrier-class calls, the style-class helper
   the native menu already uses. Open: a non-asserting entity -> owner lookup (Linux `GetComponentDataIndex`
   asserts on a missing component; `StationGroup` traversal unproven), when the class must be re-applied
   (post-attach), stale-class cleanup, and the lifetime across a world change. Live plan: load a save with
   stations of two companies in the lab, break at the constructor and the carrier-class calls, read the
   entity id and walk the component lookup under gdb until the owner is proven; then implement the hook the
   way Windows `menu_hook.cpp` does it (class on the painting element, entity + EnginePtr from the
   constructor, a new marker class after attach) and watch the icons change colour.
2. **Automatic resync: the native controller** (`NativeIo` on Windows: pause/drain/save/load/action-hold,
   `StartSavegame` observer, world constructor/destructor hooks; records `UPSTREAM_dev_6cb03915`,
   `UPSTREAM_dev_cae5d370`, `UPSTREAM_dev_55e97a48`, `UPSTREAM_dev_23419163`). Without it a Linux host cannot
   run a resync, frozen joins are inactive, a Linux joiner advertises recovery 0. Open: the Linux equivalents
   of `UI::CMenuUI::StartSavegame`, the `CGameUI` constructor, the world destructor, the command thread's
   quiescence (pause command as a FIFO fence), and safe input suppression. Live plan: break on the save and
   load paths in the running game (SaveGame / AutoSave completion, the menu load callback), record the
   backtraces and the objects involved, then implement `native_io` for Linux behind the same
   `tpf2_native_request.txt` / `tpf2_native_event.txt` contract and drive a resync between the native and the
   Proton instance.
3. **Automatic in-world client load** (`67db330`; same controller: calling the load path for a running
   client's world). Falls out of item 2.
4. **Company rename through the game's company window** (`2a87bb4`; record `UPSTREAM_dev_b5dade06`). Open: the
   Linux `SetName` command path still blocks player clicks; the cancel/replay lifetime at `CommandList::Add`
   and the originating entity. Live plan: rename a company in the lab, break at the Add site, read the command
   object, confirm which entity and callback it carries, then ship it the way the Windows slice does.
5. **Managed Workshop registration** (`90bbedc`; record `UPSTREAM_dev_cae5d370`). Known: `RefreshModList` at
   `0x31ae800`, control-byte iteration, 32-byte slots. Open: result/path ownership, backend identities,
   catalogue receipt layout. Lower priority.
6. **Generalized modular-station endpoint weld** (Windows `station_weld.h`; Linux keeps the older
   depot/template weld). Needs a representative modular-station proposal in the lab to prove frozen-index and
   segment-tag ownership across compaction. Lower priority.

Out of scope: the MSI in-app updater (Windows only by nature) and anything under `installer/`.
