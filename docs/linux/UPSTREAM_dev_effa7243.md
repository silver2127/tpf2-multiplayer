# Windows dev effa7243 integration — avoid repeat saves during mod delivery

## Merged

Windows target: `effa7243a9d2afb0744c6a9c1eaa72bc5bf6a785`.
Linux parent: `fa0d67b80b567a06d64eae56514a715103f7115c`.
One incoming commit, no conflicts; merge staged and left uncommitted.
Release and lobby handshake remain 0.7.1.2.

Retained `netpunch/lobby.py` and `tools/relay_mod_download_test.py` byte-exact
from upstream, including the test's CRLF line endings. The unstarted-peer
save sweep now waits while a pack job, queued job or mod round exists.
A free transfer pipe during packing no longer triggers another full save.

## Ported (what and how)

Linux runs this same Python host/relay implementation; see [NETPUNCH.md](NETPUNCH.md).
Reviewed README, INSTALL, previous integration records and native RE notes.
No Windows-only hook, address, byte pattern, struct layout, ABI or engine
lifetime changes in this commit; no native counterpart or new static/live
reverse engineering is needed. Windows code paths remain intact.

Retained upstream's new slow-packing regression and one-save assertion.
An adjacent host-mod refusal test used a Windows backslash path and failed
its basename assertion on Linux (`C:\s\world` instead of `world`). Changed
only its save-path fixtures to `os.path.join`, preserving checks on both OSes.
Updated README/INSTALL links, release BUILDINFO and Lua verifier provenance,
and included this record in release packaging. No release package was built.

## Not ported

None for this commit. Inherited native feature gaps and outstanding gameplay
validation remain unchanged; this integration does not establish gameplay parity.

## Live testing

No game launch, debugger, desktop input, actor installation or save mutation.
No engine contract needs a live probe for this shared lobby change. Steam,
user installations and lab actors were untouched; no backup/restore needed.

The relay regression ran real Linux loopback sockets and temporary file IPC,
with synthetic saves/mods and mocked game/catalogue discovery. Initial join,
late join, DLC exclusion, registration before start, three mod batches and
2.5-second packing delay passed; the late joiner got one save push per case.
This is process-level testing, not an in-game or cross-platform observation.

## Tests

- `tools/linux/build_native.sh`: PASS, pinned soldier build, 79/79 CTests
  (62.75 seconds), glibc <=2.31 compatibility checks.
- `python3 tools/linux/verify_lua_release.py`: PASS, 31 exact upstream Lua
  files, zero exceptions, 32 HUD glyphs, two toolbar textures. Manifest:
  `bf8724ef7c9ee0a77eaec8a1acc0e0cf82f624b991669f06eee234b47616bfeb`.
- Clone-local venv Python, `tools/relay_mod_download_test.py`: PASS, all
  three cases including slow packing.
- Same interpreter, `tools/mod_download_test.py`: PASS, 22 tests;
  `tools/test_host_missing_mods.py`: PASS, 5 tests after fixture correction.
- Negative control removed only `and not mods_busy` in memory: the slow-pack
  test failed earlier at the 25-second hotjoin readiness assertion, with
  repeated save-ready messages and a refused unconsented mods transfer.
  It did not reach the push-count assertion; no exact pre-fix count is claimed.
- `bash -n tools/linux/build_release.sh`: PASS. Upstream file equality,
  CRLF-aware whitespace checks and empty unmerged index checked; MERGE_HEAD retained.

System Python initially lacked `stun`; installed pystun3 and zstandard in
`.git/port-venv` with pip caching disabled. Temporary test files stayed in
`.git/port-tmp`; no system packages were installed. Relay shutdown logged a
closed-socket send after success; all positive relay assertions passed.
Evidence: `.git/port-effa7243-{build,lua,relay,mod,host,negative,deps}.log` and
`native/linux/out-soldier/Testing/Temporary/LastTest.log`.
