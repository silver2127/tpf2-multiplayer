# Windows dev d4a11297 integration: recent peer clocks during catch-up

## Merged

Windows target: `d4a112971455354044cf2c1b37615345579be728`.
Linux parent: `f5194c2643139bdcc8013b81328ad73baa2dface`.
One incoming commit, no conflicts. Merge staged and left uncommitted.
Release remains **0.7.1**.

## Ported (what and how)

Retained upstream `net.lua`, `lines.lua` and `actions_off_test.py` unchanged.
These scripts are shared by Windows and native Linux:

- `fastestPeerClock()` includes peers heard within 15 seconds even when their
  heartbeat has aged out by local simulation ticks. It projects their step/time
  using the local simulation rate, capped at five seconds of age, plus one step.
  The existing tick-based peer bounds and pacing remain unchanged.
- `spareTick()` defers automatic LSPARE requests while `catchingUp2` is true or
  log fetching is pending (`lgFetch` is neither nil nor `done`).

The clock dependency is already implemented on Linux: the loader's game-only
`clock()` interposer in `native/linux/src/boot.cpp` uses `CLOCK_MONOTONIC` and
returns `CLOCKS_PER_SEC` units. Thus the game's Lua `os.clock()` supplies wall
seconds, as Windows does, rather than glibc's process CPU time. See the existing
[clock analysis](LUA.md#clocks) and [native core evidence](../re/linux/SLICE_CORE.md).
This integration changes no address, pattern, ABI, offset or engine lifetime
contract; no new reverse engineering or native hook is needed.

Added behavioral coverage to `tools/line_spare_test.py`: neither active catch-up
nor pending log fetch schedules LSPARE or starts its retry timer; completion
allows the normal request immediately. The existing suite also covers ordinary
spare creation, claiming, callbacks and replay. Incoming action-gate tests cover
recent but tick-stale clocks, the five-second projection cap and expired peers.

Advanced the exact Lua verifier baseline and package BUILDINFO provenance,
packaged this record alongside earlier integrations, and updated README/INSTALL
links. No dependency was installed and Windows runtime code paths remain intact.

## Not ported

None introduced by this commit. Existing native feature gaps and outstanding
live-validation limits remain unchanged. This does not establish cross-platform
hot-join determinism.

## Live testing

No game launched, debugger attached or desktop input sent. This is a shared Lua
change using an existing native clock contract; it was exercised offline on
Lua 5.2. No loaded-world catch-up, GPU, rendering or native/Proton peer result is
claimed. Lab actors, saves, Steam and installed mods were untouched, so no
backup/restoration was needed and no game process was started.

## Tests

- `tools/linux/build_native.sh`: PASS, pinned soldier SDK, 75/75 CTests and
  glibc <=2.31 compatibility checks.
- `python3 tools/linux/verify_lua_release.py`: PASS, 30 exact upstream Lua files,
  zero exceptions, 32 HUD glyphs and two toolbar textures. Lua manifest SHA-256:
  `e4f0ff50af42446e7812c2390165c6fd7492489d0d58457737d55e2229c3b851`.
- All 19 `tools/*test.py` suites referencing `net.lua` or `lines.lua`: PASS,
  including upstream `actions_off_test.py` and expanded `line_spare_test.py`.
  These use the pre-existing netpunch-build Python environment's `lupa.lua52`;
  system Python lacks lupa. Temporary test data was kept in the clone.
- `bash -n tools/linux/build_release.sh`: PASS.
- Incoming files byte-identical to upstream; unresolved index empty. Whitespace
  check passes with `git -c core.whitespace=cr-at-eol diff --cached --check`;
  plain Git flags upstream CRLF endings, which are preserved for exact parity.

Logs: clone-local `.git/port-d4a11297/` (build, Lua verification and individual
Lua suites). Offline evidence only; no upstream live observations are presented
as results of this job.
