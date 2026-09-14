# Paused checkpoint — 2026-09-14

User requested saving work and stopping. Active checkout is `C:\Users\james\tpf2-release-0.4.22`, not the older multiplayer checkout.

## Work preserved

- Experimental shared infrastructure: other companies may use infrastructure; owners control changes. Lua ownership guards and native entity-only owner setter avoid cascading line ownership into shared infrastructure. See SHARED_INFRA.md. Live routing remains unverified.
- Station snapping: new station_weld.h handles a unique station connector, including middle-node removal and frozen-index remapping. Old depot-shaped merge assumptions rejected modular stations.
- Switches near signals: road replay now carries edge objects onto the appropriate half when splitting an edge, preserving orientation and object kind.
- Road waypoints: street object side no longer becomes rail side 2 merely because its kind is 2; native proposal capture also accepts kind 1.
- Line waypoints: native capture reads SignalId vectors; waypoint descriptors resolve peer-local IDs and populate actual Line.Stop waypoint containers. Live API probes confirmed api.type.SignalId.new() and modifying the existing waypoint vector are required.

## Validation and remaining work

Passed during development: shared infrastructure tests, crossing replay including both signal split orientations, strict line creation, rapid line edits, and syntax checks of edited Lua files. Synthetic C++ station weld test passed before a subsequent small alias guard change; rerun it.

tools/waypoint_test.py currently fails its negative-case assertion at Lua line 31: fixture `wp=3:301:0` lacks the leading space required by the capture parser. Change fixture to `LUPDATE wp=3:301:0` and rerun; earlier roundtrip, peer-ID remapping and waypoint edit assertions passed. Do not report the suite as passing yet.

Still needed: review and finish waypoint capture/replay tests, road waypoint regression coverage, final native rebuild, and live two-instance tests for station connections, waypoint assignment and switches near signals. Multi-connector stations remain unsupported by the new unique-connector helper. Signal index portability across reversed road lanes needs live validation.

## Build and runtime state

Source version is 0.4.25. Installed active update remains 0.4.24; neither instance was restarted for this work. The 0.4.25 update zip contains the earlier sharing prototype and is stale relative to the transport fixes. Native outputs are also not guaranteed current. No new install, push or release was performed. Latest public release remains v0.4.22.

For future builds set TPF2MP_NO_DEPLOY=1 before native/build.bat proxy or slice. Rebuild/package only after completing validation. Preserve unrelated edits in the older multiplayer and command-barrier worktrees.
