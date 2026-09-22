# native/src/slice

`tpf2_slice.dll` is one translation unit: `slice_hook.cpp` holds the header, the
factory table, the shared helpers and the globals every region uses, and then
includes these parts in order. A part is a continuation of the file, not a
header: the same static symbols, the same forward declarations, the same
assembly relay contracts (`deferrelay_slice.asm`, `trainorderrelay_slice.asm`,
`moveorderrelay_slice.asm` name functions defined here). Include order is
definition order, so a part may use anything defined in an earlier part or in
`slice_hook.cpp` above the includes, and nothing from a later one without a
forward declaration.

| part | holds |
|---|---|
| `vectors_roads.inl` | the game's vectors at their own length, road/rail node and edge decoding, the ROADE/EDEMO/CDEMO/ROADC records |
| `strings_bulldoze.inl` | heap pointer and std::string readers, percent encoding, the bulldozer's classification |
| `vehicles.inl` | vehicle configuration off a buy, VBUY, the ARMED and NATIVE notices |
| `lines.inl` | component::Line decoding, the platform assignment at replay, strict line creation and the spare line |
| `capture.inl` | the vehicle/line command writers, speed and calendar buttons, CaptureFactory (the factory hook's dispatcher) |
| `constructions.inl` | construction params off the proposal (CONXP), stops, signals, module edits, upgrade shape, proposal dumps |
| `station_weld.inl` | MergeTemplateStreet: a script-built station proposal shaped like the UI's (station_weld.h) |
| `terrain_assets.inl` | terrain tools and the asset brush: stash at the factory, inject from the Lua's file |
| `add_hook.inl` | the CommandList::Add hook: cancel, callbacks, stashes, DeferHandler |
| `trainorder.inl` | TRAIN RESERVATION ORDER (trainorder.h) |
| `roadspace.inl` | near-page detours, ROAD FREE SPACE and road entry order |
| `sharedstations.inl` | SHARED STATIONS: the line editor's owner gate in companies mode |
| `ui_tints.inl` | paused tick, icons for every player, company-colour tints on icons, labels and windows |
| `sharedstations_install.inl` | InstallSharedStations (the patch of the gate above) |
| `moveorder.inl` | SHIP AND AIRCRAFT CLAIM ORDER (moveorder.h) |
| `init.inl` | the relay blobs, hook installation, Init and DllMain |

Tests that read the source by text anchors (`tools/*_bytes_test.py` and
others) get the stitched text from `tools/slice_source.py`, which inlines the
includes, so an anchor works wherever its line lives.

Splitting into real compilation units is a later step, region by region, once
a region's globals have a header of their own.
