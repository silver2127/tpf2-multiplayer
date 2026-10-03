# Windows dev cc0981bb integration: server package, sliced scans and resync hold

## Merged

Target `cc0981bb2808c0475eb2a39cef16781098e97405`, Linux parent
`e360551ff318b6e46acb65ac0d470b02ec4c94c9`. This batch takes five dev
first-parent commits: `9269293`, `5283219`, `1263490`, `499c81c`, and
`cc0981b`. The main merge also imports `fb5b2a6` (resync hold), `5f1d45c`
(PERF lanes/hash/watchers), and `2b52f1e` (sliced construction scans), through
PR merges `1108f63` and `f2be5f4`. No later pending dev commits are included.
The launcher-release test conflict is resolved and staged; the merge is left
in progress and uncommitted. Version remains 0.7.0.7.

## Ported (what and how)

- Retain the shared hosting guide, server environment/public-listing correction,
  release notes, reproducible server archive builder, checksum enforcement,
  workflow artifact and publisher asset changes. The archive includes both
  Proton scripts and the native watchdog. Server setup was inspected, not run.
- Resolve the test conflict using upstream's named payload fixtures and computed
  upload count: each payload twice, two launchers, and direct installers once
  more on the version page. Preserve native release coverage and add a damaged
  server-archive checksum regression. Tests mock all publishing/network calls.
- Retain the shared Lua sliced edit/removal scans, streaming exact hash,
  per-job PERF lanes and opt-in diagnostic train watchers. Advance the Lua
  verifier and native package provenance to this target, retaining the pinned
  native origin-replay exception. Package this integration record.
- Port the Windows hold controller change to `native_control_linux.cpp`:
  retry without a ten-second deadline; a newer request cancels the pending hold.
  Publish `hold_key` while waiting, zero when no hold is pending.
- Linux already observes game-local SDL events instead of global Windows
  GetAsyncKeyState. Expose the tracked SDL scancode (positive) or mouse button
  (negative, e.g. -1 for left) through the panel and NativeIo interfaces.
  These values are diagnostics, not Windows virtual-key codes. Clear tracked
  keys/buttons on SDL focus loss, so releases in another application cannot
  strand a pending hold. Existing Escape handling, input suppression defaults,
  save-time suppression and missing-filter refusal are preserved.

The only platform-specific upstream change uses OS input and the mailbox;
it adds no game address, byte pattern, object layout, calling convention or
lifetime contract. Existing native SDL routing is documented in
[Native IO](../re/linux/NATIVE_IO_LINUX.md). No new ELF reverse engineering or
gdb proof is needed, and no unverified patch site is introduced. Windows
implementation and history remain intact.

## Not ported

None from this batch. Earlier native feature gaps and live-validation limits
remain as recorded in previous integrations. This does not close inherited
cargo-filter or terrain-sidecar gaps. Windows/MSVC tests were not run here.

## Live testing

No game was launched, gdb attached, desktop input sent, or lab actor/save
modified. No backup/restoration was necessary. No renderer, loaded-world
performance improvement, actual focus transition or cross-platform gameplay
result is claimed. SDL tests feed the actual native event filter in a fixture;
controller tests exercise its real polling thread and mailbox with mocked IO.
These are offline/process-level regressions, not live game observations.

## Tests

- `tools/linux/build_native.sh`: PASS, 72/72 CTests in the pinned soldier SDK
  and glibc <=2.31 checks. Controller regression holds a key for 10.5 s,
  verifies no timeout event, completes on release, and cancels another hold
  with a newer release request. Panel tests check key/button diagnostics and
  focus-loss cleanup with both key and mouse held. Existing IO/panel tests run.
- `python3 tools/linux/verify_lua_release.py`: PASS, 29 Lua files with one pinned
  exception, 32 exact HUD glyphs. Manifest SHA-256:
  `e49a423ed668fe64c3e85a21429e6d8b767299305c663db731b0caa84e3f4dd8`.
- `tools/con_slice_test.py`, `tools/hash_stream_test.py`, and
  `tools/perf_lanes_test.py`: PASS using clone-local lupa/Lua 5.2. Coverage
  includes sliced/full scan equivalence, removals, changing records, unreadable
  constructions, hash tails/random bytes/numeric lists/large lanes, diagnostic
  gating, accumulated timings and watcher-state reset.
- `python3 tools/linux/test_launcher_release.py`: PASS, 36 offline tests,
  including the new server checksum rejection before uploads.
- `python3 tools/server/test_native_watchdog.py`: PASS, 3 tests.
- Build server archive twice in `.git/server-package-{a,b}`: identical SHA-256
  `e6adb23d05228b494a5ce4f3e62df126f7698a6c6c66b00a3042951d00c7ef04`.
  Inspect all members against builder manifest: exact normalized source bytes,
  file modes, native watchdog and VERSION present. No archive setup executed.
- Release script shell syntax and CRLF-aware whitespace checks pass. Incoming
  Windows CRLF files retain their bytes; ordinary diff --check reports CRLF
  as whitespace without `core.whitespace=cr-at-eol`.

Logs are clone-local `.git/port-*.log`; Python dependencies are confined to
`.git/port-venv`. No system installation, Steam changes or publication occurred.
