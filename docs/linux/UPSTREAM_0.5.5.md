# Upstream 0.5.5 integration

Upstream: `ecbaf1d8752f7636d88ebc13d0d3084dd3977996` (`v0.5.5`).
Linux release: `0.5.5-linux-dev.1`, for native Steam build 35924.

## Changes since linux-dev.1 for 0.5.3

- Merge Windows 0.5.4 and 0.5.5, retaining Linux paths, signal handling,
  process-lifetime transport storage and native determinism fixes.
- Port transport admission by current world epoch: a restarted game process can
  rejoin without the host's roster remaining frozen around its dead session.
- Evict silent transport members and release their pending acknowledgements;
  clear traffic when nobody remains. Match upstream receive-floor recovery,
  acknowledgement handling, suspend-gap handling and lobby-epoch renaming.
- Include transport admission/eviction logs and member/other-world counts in
  the Linux bridge health log.
- Notify the lobby when Linux starts a hot-join autosave (`sync_taking`). The
  merged lobby waits for that fresh save instead of immediately sending its
  old START GAME snapshot, and queues a fresh start behind an active transfer.
- Keep upstream lobby epoch following and stale-nonce rejection. These are
  transport/lobby changes, not an implementation of Linux's missing native
  resync save/load controller.
- Include the previous Linux follow-up: opaque cached menus without GPU
  background readback or CPU blur, explicit pause-toggle/speed-button tagging,
  and ordered waypoint capture for line creation and edits.
- Pin packaging to 0.5.5. All 28 Lua files are byte-identical to that release.

The Windows first-save-folder fix is already covered by Linux's native profile
selection (explicit profile or the game's open log, then Steam discovery) and
creation of a missing save directory when placing a shared save.

## Validation

- Steam Runtime soldier build and 37 native CTests, including real UDP tests
  for restarted joiners, dead-member eviction, empty-cohort re-admission,
  stale-world rejection, receive-floor recovery and lobby-epoch renaming.
- 42 upstream sync Python tests.
- Three-player production lobby/UDP resync test with a simulated engine.
- Frozen Linux lobby self-tests: basic lobby, relay, mesh, mod sharing and save
  transfer. Sixteen orderly shutdown checks.
- Lua byte identity against the pinned upstream commit.
- The release installer is checked in a temporary Steam library for extraction,
  installation, upgrade, uninstall and preservation of user data.

## Limits

This remains an experimental prerelease. No completed native/Windows 0.5.5
live gameplay replay or menu FPS benchmark has been recorded. Automated tests
and a simulated engine do not establish those guarantees.

Automatic resync's native save/load/action-hold controller, entity-only company
ownership and managed Workshop registration remain unported. Some actions,
including names/colors, vehicle stop/maintenance and loans, remain blocked in
multiplayer. In-game lobby reopening, bridge-model replacement, newer modular
station connection handling and Linux in-app updates remain outstanding.

Use matching 0.5.5 peers. Installation requires closing the game and restarting
with the installed launcher; publishing this package does not update running
instances automatically.
