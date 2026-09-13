# One-click recovery after a desync

After a desync, one click saves the host world, transfers it and reloads both
players in place. The implementation passed the local two-game acceptance tests
below.

When a desync is detected, press **Resync now** once. The system holds
both games, saves the host under a unique recovery name, transfers and verifies
that exact save set, reloads both worlds in the existing processes and compares
the freshly loaded, paused worlds. It resumes only after the shared comparison
succeeds. The host's previous speed is restored, including an intentional pause.
Changes present only on the client are replaced by the host's world.

In the game, the **Resync** section at the top of the Multiplayer window shows
the desync and offers **Resync now**; while a recovery runs it reports the
phase, the failed step and its detail. The section shows the window even when
Ctrl+Shift+D hid it and disappears once the worlds match again. There is no
separate Lua popup any more. While the world is held, the native input gate
swallows clicks on game widgets, so the compact native recovery panel appears
for the hold and carries **Retry** after a failure (it reuses a completed,
verified snapshot when one exists) and **Cancel**, which retains the hold.
Closing that panel hides it until the next phase change; completion hides it.
It no longer collapses to a "Multiplayer status" button. During native saving
and loading, the custom overlay stops drawing; the game's own loading screen
remains. To abandon recovery entirely, exit both games normally and start a
new lobby. Neither an error nor a timeout unpauses a diverged world. Log
upload preferences remain independent of recovery.

## Supported session

This first port supports **two players in a player-hosted lobby**, both running
the same package, on the exact supported Windows game build 35924. A whole
executable SHA-256 check guards the build-specific native adapter. Dedicated
relays and additional players do not advertise automatic recovery. Once recovery
has begun, the player roster is fixed; adding or replacing a player requires a
new session. Ordinary lobby start and save selection keep their existing flow.

## Implementation

- `netpunch/sync_operation.py` owns the host-authoritative barrier. Requests and
  acknowledgements name operation, revision, phase and a fresh world epoch.
  The host waits for every original participant, checks exact snapshot hashes
  and fresh paused fingerprints, and retains the hold on errors or disconnects.
- `sync_runtime.py` coordinates the local native and Lua adapters through
  PID-scoped files. Native commands require actual completion acknowledgements;
  enqueueing is not saving or loading successfully. `tpf2_sync_lua.txt` also
  carries the failed step and detail for the Resync section.
- `sync_snapshot.py` owns immutable save bytes, requires `.sav` and `.sav.lua`,
  includes `.jpg` when present and verifies SHA-256 throughout. Unique
  `mp_<epoch-prefix>` names never overwrite unrelated user saves.
- `sync_lobby.py` uses the existing authenticated lobby and reliable save
  transport, repeats control messages, rejects duplicates and keeps recovery
  transfers separate from normal start/hot-join transfers.
- `native_io.cpp` marshals save, pause/drain and load to the observed engine UI
  thread. The native input gate stops new user commands and legacy per-frame
  script events before command creation. A FIFO pause fence drains earlier work.
- `mp/resync.lua` runs before all simulation producers and stops GUI preview
  producers too. A newly loaded Lua state reports its own fresh world token.
  A full paused world hash bypasses the normal running-only hash cadence. It
  also owns the Resync section, whose Resync now goes through
  `tpf2_sync_request.txt`. Control files are published under per-writer
  temporary names so two lobby processes on one data folder cannot lose each
  other's replacement.
- `net.cpp` scopes data and ACKs to a world epoch. The bridge resets queues,
  partial packets and runtime command files together. A tail read begun before
  reset cannot enqueue into the new world. Epoch requests have a separate
  `tpf2_epoch_request.txt` mailbox so roster/speed writes cannot undo a reset.
  Existing exclusive loopback binding and sender-address checks are retained.

All players need the same package: the native wire protocol is now version 4.
The save/load adapter and Lua/network barrier must be deployed together.

## Validation and limits

Automated tests cover the production Lua 5.2 pump and GUI callback; barrier,
snapshot and runtime state machines; real UDP lobby/save transfer with simulated
engines; actual native UDP epoch resets; and native IPC completion publication
under Windows sharing violations. They do not prove in-game save/load behavior.

Run `tools/resync_test.py`, `tools/test_sync_operation.py`,
`tools/test_sync_snapshot.py`, `tools/test_sync_runtime.py`,
`tools/test_auto_sync_lobby.py`, `tools/test_net_epoch.py` and
`tools/test_native_control.py` before a release.
The native tests require Windows and the repository MSVC environment helper.

The archived automatic implementation had successful game tests and a later
unresolved Vulkan Device-lost/MemoryException during simultaneous reloads. This
port suppresses overlay rendering during world I/O and checks overlay submission
and fence completion before reusing resources. These changes address concrete
renderer hazards; they are **not evidence that the old crash is fixed**.

Local acceptance used two real build-35924 games, matching complete candidate
packages and backed-up test saves. Four recovery operations completed, including
host and client GUI requests, speed 1, intentional pause, and a live client build
preview during the hold. All completed epochs produced identical fresh paused
fingerprints. A deliberate client-only journal change was replaced by the host
snapshot. The desync indicator was raised through the diagnostic injection path;
these cases do not claim to reproduce an organic desync.

A deliberate IPC failure after real snapshot transfer held both games. The
native retry button reused that immutable snapshot with a new epoch, without
another native save. A street built after recovery appeared in both worlds,
followed by shared SYNC. Repeated loading produced no observed game crash or
GPU error event in this run. This is a bounded local test, not a general proof
against the archived graphics failure or compatibility with every mod set.

For future changes, repeat these cases with the existing rig and compare both
worlds, logs and installed hashes. Close both games normally before replacing
DLLs, preserve test evidence before restarting, and verify the complete release
package and downloaded assets before switching the launcher feed.
