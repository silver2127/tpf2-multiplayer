# Windows dev 86f806df integration — preserve active terrain streams

## Merged

Windows target: `86f806df166b11f54fd4a75e3db18cb58e9aaee4`.
Linux parent: `c8d5dff328a00ff2251b829f8503120115c32209`.
One incoming commit, no conflicts; merge staged and left uncommitted.
Release and lobby handshake remain 0.7.1.2.

The shared host keeps multiple terrain transfers instead of replacing its
single transfer on every START. A peer already receiving the same sidecar
is skipped both before the reader thread starts and when its result arrives.
Sidecar identity uses normalized absolute path, size and nanosecond mtime.
A peer receiving another sidecar leaves its old transfer; other peers keep
receiving it. Feedback, keepalive exemptions, pumping and cleanup account
for all terrain streams. Windows runtime code paths remain intact.

## Ported (what and how)

Linux uses the same Python lobby implementation (see [NETPUNCH.md](NETPUNCH.md));
`netpunch/lobby.py` and `tools/test_lobby_limits.py` retain the upstream bytes.
No native counterpart, hook, address, byte pattern, struct offset or ABI
changes are needed. Reviewed README, INSTALL, earlier integration records
and [native sidecar RE](../re/linux/DEV_2B4FD093.md). This change requires no
new static or live engine reverse engineering.

Retained the upstream second-START regression. Added tests checking lookup
across multiple streams, inactive peer states, normalized path aliases and
same-size rewrites with a deliberately changed timestamp. Updated Linux
README/INSTALL links, Lua verification reference and package BUILDINFO;
release packaging includes this record. No package was built.

## Not ported

None for this commit. Inherited native feature gaps and outstanding sidecar
ownership/lifetime and gameplay validation remain unchanged. The shared
lobby fix does not establish native or cross-platform gameplay parity.

## Live testing

No game launched, debugger attached, desktop input sent, or actor installed.
Steam, lab actors, saves and user installations were untouched; no actor
backup or restoration was necessary. No GPU or in-game observation is claimed.

Python tests exercised real transfer/receiver objects on a simulated wire
with loss and reordering, file-prefix growth, hash rejection, load-done
cancellation and repeat-stream selection. These are automated fixture tests,
not a live game or cross-platform session.

## Tests

- `tools/linux/build_native.sh`: PASS, pinned soldier SDK, 79/79 CTests
  and glibc <=2.31 compatibility checks.
- `python3 tools/linux/verify_lua_release.py`: PASS, 31 exact upstream Lua
  files, zero exceptions, 32 HUD glyphs and two toolbar textures. Manifest:
  `bf8724ef7c9ee0a77eaec8a1acc0e0cf82f624b991669f06eee234b47616bfeb`.
- Clone-local venv Python, `tools/test_terrain_stream.py`: PASS, 8 tests.
- Same interpreter, `tools/test_lobby_limits.py`: PASS, 32 tests.
- Release shell syntax, staged whitespace, upstream lobby/limit-test equality,
  empty unmerged index and retained MERGE_HEAD checked.

Evidence: `.git/port-86f806df-{build,lua,terrain,limits,deps}.log` and
`native/linux/out-soldier/Testing/Temporary/LastTest.log`. System Python lacked
`stun`; installed pystun3 and zstandard only in `.git/port-venv`, with pip
caching disabled and temporary files in `.git/port-tmp`. No system packages,
publication, commit or merge abort.
