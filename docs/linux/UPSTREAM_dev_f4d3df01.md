# Windows dev f4d3df01 integration — startup grace and recovery roster

## Merged

Target `f4d3df0156c44caddbe89920bcc908005b618f8f`, Linux parent `f8d457a`.
Four incoming commits: `ff28221` configurable native startup grace, `1172cf0`
native reload/VPS report, `daf020a` retained-world correctness dependencies,
and `f4d3df0` retained-host trial evidence and recovery roster fix.
No merge conflicts. Merge remains staged and uncommitted. Release stays 0.7.1.5.

## Ported (what and how)

The incoming native Linux implementation applies directly: dedicated settings
parse `dedicated_load_delay_sec` in [8, 120], default 45 seconds. DedicatedTick
uses that grace before loading; the initial eight-second hosting delay, lobby
readiness, loading/native-I/O guards and 60-second retry gate remain intact.
`tpf2server configure` forwards `LOAD_DELAY_SEC` with a 45-second fallback.
Added tests for both accepted boundaries and malformed/out-of-range input.

Shared Lua `syncAlone()` reads the current PID-scoped bridge control file,
releasing only for exactly one player. The native lobby already writes this
PID and player count in `WriteBridgeCtl` (`lobby_linux.cpp`); no Windows path or
ABI translation is needed. Incoming tests cover a cached solo roster during a
100-tick hold, foreign PID and actual solo roster. Added zero, negative,
fractional, empty and nonnumeric roster cases.

Updated Lua release verification to exact incoming bytes, packaged integration
record and BUILDINFO provenance, README and Linux installation notes.
The archived retained-host patch is not applied; its CRLF line endings were
normalized to LF to pass Git whitespace checks. The production sync runtime
continues loading the host. September VPS timings and divergence samples are
upstream historical evidence, not observations made during this integration.

Reviewed existing Linux organisation and RE notes (NATIVE_IO_LINUX,
DEV_192ECD4D and DEV_8C3C02A5). These commits add no engine address, patch site,
byte pattern, offset, calling convention or lifetime contract. The changed
Settings is mod-owned, not an engine layout. No additional binary RE or live
ABI proof is needed; existing native guards are unchanged.

## Not ported

None from these four commits. The failed retained-world experiment remains
research evidence as upstream intended, not an omitted runtime feature.
Previously documented native limitations remain outside this integration.

## Live testing

No game, debugger, Proton peer or desktop input was run. No lab actor, save,
user installation or Steam directory was changed, so no backup or restoration
was necessary. No GPU, startup timing, loaded-world or cross-platform gameplay
result is claimed. Lua tests use real Lua 5.2 and files with simulated engine
calls. The incoming VPS results were not reproduced locally.

## Tests

- `tools/linux/build_native.sh`: PASS, 79/79 CTests in the pinned soldier SDK;
  glibc baseline checks passed.
- `python3 tools/linux/verify_lua_release.py`: PASS, 31 exact upstream Lua files,
  zero exceptions, 32 HUD glyphs and two toolbar textures. Lua manifest SHA-256:
  `f7a4d20419e173a3cb2d5aa4ef75155299b3bb6a72ab6eb9f9e1c6cdad9f0007`.
- `.git/port-venv/bin/python tools/test_auto_sync_lua.py`: PASS, real Lua 5.2
  recovery pump, stale roster, PID guard, pause/release and invalid roster cases.
- Isolated `tpf2server configure` fixture: PASS, unset LOAD_DELAY_SEC writes 45,
  explicit 15 writes 15. Identity command shim avoids sudo; all generated files
  are under a temporary directory in this clone, removed afterward. No services
  or real Steam paths accessed.
- Git whitespace checks: PASS after normalizing the archived patch; no unmerged entries.
- `sh -n tools/server/tpf2server`, `bash -n tools/linux/build_release.sh`: PASS.

Logs: `.git/port-f4d3df01-{build,lua,sync,deps}.log` and
`native/linux/out-soldier/Testing/Temporary/LastTest.log`. Lupa is installed only
in the clone-local `.git/port-venv`. Windows/MSVC and release packaging not run.
