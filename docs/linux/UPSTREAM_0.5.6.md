# Upstream 0.5.6 integration

Upstream: `e4702669ab99ae05074dc5b5540dbc1115e66129` (`v0.5.6`).
Linux release: `0.5.6-linux-dev.1`, for native Steam build 35924.

## Changes

- Merge Windows 0.5.6. The shared Lua resync pump and Python coordinator now
  accept engine speed 3 as well as 0, 1, 2 and 4. Invalid speeds remain rejected.
- Windows enlarged construction params from 8 KB to 64 KB to avoid truncated
  modular-station upgrades. Linux already uses a dynamic record with a 256 KB
  construction-text limit. A new regression fixture verifies complete output
  exceeding 64 KB, including every serialized field.
- Rebuild the Linux lobby with version 0.5.6 and verify all 28 Lua files against
  the pinned Windows release, including the changed resync.lua.
- Retain the 0.5.5 reconnect/hot-join fixes, opaque cached menus, speed-button
  tagging, waypoint capture and earlier native determinism fixes.

## Validation

Steam Runtime soldier build and 37 native CTests; upstream sync Python tests;
production Lua 5.2 resync test with simulated engine calls; frozen lobby tests
for lobby, relay, mesh, mods and transfer; installer extraction, installation,
upgrade, uninstall and user-data preservation in a temporary Steam library.

## Limits

This remains experimental. No completed native/Windows 0.5.6 live gameplay
replay or menu FPS benchmark has been recorded. Automated tests do not establish
those guarantees.

Automatic resync's native save/load/action-hold controller, entity-only company
ownership and managed Workshop registration remain unported. The shared speed
fix does not supply the missing Linux native controller. Some actions, including
names/colors, vehicle stop/maintenance and loans, remain blocked in multiplayer.
In-game lobby reopening, bridge-model replacement, newer modular station
connection handling and Linux in-app updates remain outstanding.

Use matching 0.5.6 peers. Close the game before installing and restart with the
installed launcher. Publishing does not update running instances automatically.
