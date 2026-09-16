# Upstream 0.5.3 integration

Upstream commit: `66da00db6a36791630d7ebb05b6bf81fb06e7959` (release 0.5.3).
Merged into `linux-native` on top of the validated 0.4.22 port (`0feddb8`).

## Changes

- Retain all native determinism fixes; merge upstream scripts, Windows native
  sources, lobby changes, tests and documentation.
- Resolve the lobby's line-ending conflict with a normalized three-way merge,
  preserving Linux signal handling and the new disconnect-state publication.
- Retain Linux game/profile discovery and use its data directory for managed
  mod downloads, including the new Workshop catalogue/cache helpers.
- Port Windows protocol 5 to POSIX: variable-length packets, per-process peer
  streams, all-peer acknowledgement, bounded retransmission windows, stale
  world rejection, world resets and idempotent new-lobby boundaries.
  The POSIX socket/thread layer retains process-lifetime container storage,
  atomic shutdown flags, monotonic time and an actual thread join.
- Carry capture-tail epochs into queued messages. A coordinated reset truncates
  local capture/events, starts the new tail at zero and atomically publishes
  PID-scoped readiness. A failed file reset reports `ok=0`.
- Pin the Lua release guard to 0.5.3. All 28 files match upstream exactly.
  Lab creation checks that baseline too. `tools/proton/windows-0.4.22.json`
  remains an explicitly historical installer manifest, not the current baseline.

## Validation

- Soldier SDK build and 35 CTests, including the original 29 native tests.
- New real-UDP tests adapted from upstream: protocol-5 datagram sizes,
  malformed packet rejection, world transitions, stale tail/process rejection,
  retransmission and multi-peer acknowledgement with 2, 3 and 10 players.
- New bridge test uses real temporary files and a loopback transport to check
  PID ownership, truncation, readiness, idempotence and failed file resets.
- 41 upstream sync-operation/readiness/runtime/snapshot Python tests.
- 22 sandbox tooling tests; lobby roster/chat/start self-test; 16 orderly
  shutdown checks, including interrupted rendezvous cleanup.
- Lua byte identity verified against the upstream commit above.

## Deployment and scope

This is a source integration and native build, not a new cross-platform gameplay
validation. The previously passing t=1800–3600 replay used 0.4.22 and remains
recorded under its original version. Neither the running test lab nor the
published Linux packages was changed during this merge.

Windows-only native additions still need Linux implementations: the new
resync save/load/action-hold controller and panel, entity-only company ownership
capability, and managed Workshop catalogue registration. The Linux bridge does
not advertise those missing capabilities. Upstream Lua remains unchanged and
rejects operations requiring unavailable native ownership support.
A matched Windows/native 0.5.3 installation and replay are required before
claiming the same live-game guarantees as the prior baseline.

## Follow-up: menu performance and command capture

The first parity follow-up ports these Windows changes to native Linux:

- Opaque multiplayer panel, using the Windows RGB(5,25,40) background.
  Removed the game-frame GPU readback, its fence wait, CPU blur and backdrop
  allocation. The composed image is reused until the layer, hover/press state,
  dimensions or swapchain changes. The panel's existing 500 ms refresh still
  updates lobby state and the caret. One panel-to-frame copy remains per frame.
- `SPEEDBTN` now explicitly carries `button` or `toggle`, using the verified
  Linux caller addresses; Lua no longer has to infer the action type.
- Line creation and update capture now include ordered per-stop waypoints in
  the upstream `wp=` wire format. The Linux Stop vector offset is documented in
  `docs/re/linux/SLICE_LINES.md`; invalid IDs, indices and oversized vectors are
  rejected using the same limits as Windows.

Validation: Soldier build and all 36 native CTests pass. Command tests cover
creation/update waypoint records, multiple stops, invalid data and explicit
speed-button kinds. A mock Vulkan test exercises the real compositor and draw
path: opaque BGRA/RGBA output, row pitch, cached frames, dirty layers, hover and
press transitions, with only one outgoing image copy per presented frame.
The upstream Lua 5.2 waypoint test passes, including per-peer entity remapping,
queued edits, waypoint removal and missing-target rejection.

These checks do not constitute a live cross-platform gameplay or FPS test.
The running lab and published linux-dev.1 assets have not been replaced by this
follow-up. The larger native features listed above remain outstanding, along
with in-game lobby reopening, bridge-model replacement, the newer modular
station connection handling and Linux in-app updates.
