# Windows dev a40180f2 integration — TCP transfer memory retention

## Merged

Windows target: `a40180f21683d7a896c3210699b35d4cc5192a60`.
Linux parent: `29fc5668566d6641bcfa11c67e1ed9826623cb65`.
One incoming commit: `lobby: a finished transfer no longer stays in memory
with its whole file`. No conflicts. Merge staged and left uncommitted;
release remains 0.7.1.2.

## Ported (what and how)

Retained upstream `netpunch/bulk_tcp.py` byte for byte. Both platforms use
this Python listener. Bound transfer methods now use `weakref.WeakMethod`;
registering another transfer prunes dead entries. Plain functions remain
strongly held. The handshake resolves the weak method once and retains that
bound method while invoking it, so an accepted stream can finish even after
the lobby drops its transfer reference. Missing owners and incorrect tokens
are rejected before the handler runs.

Reviewed `docs/linux/NETPUNCH.md`, installation instructions and prior
integration/RE records. The three lobby registration sites pass ordinary
Python methods (`_HostSaveTransfer._tcp_serve` and
`_ClientSaveReceiver._tcp_accepted`). Native packaging uses the shared lobby;
no separate Linux implementation needs changing. Linux socket reuse and
IPv4/IPv6 behavior are preserved. There are no new game addresses, patch
bytes, offsets, MSVC code or engine lifetime/ABI contracts, so static ELF
analysis and live game reverse engineering are unnecessary for this commit.

Added `tools/test_bulk_lifetime.py`, a stdlib-only regression covering dead
owner collection, rejection and pruning, plain-function retention and
authenticated streaming, and active-handler ownership through completion.
The active test uses a socket pair and a joinable handshake worker to check
collection after the handler returns without a timing-dependent sleep.
The dropped-owner test fails against the pre-merge implementation.

Advanced README/INSTALL links, Lua verification provenance and package
BUILDINFO to this target; the release package includes this record.
Lua content and Windows runtime paths are unchanged.

## Not ported

None from this commit. Existing native feature gaps and game-validation
limits remain as documented by earlier integrations. The upstream 12-hour
memory-growth observation was not repeated; these tests establish object
ownership, not dedicated-server RSS measurements.

## Live testing

No game launched, debugger attached, desktop input sent or lab actor changed.
Steam, user installations and saves were untouched; no backup/restoration
was needed. No rendering, GPU or cross-platform gameplay result is claimed.

The listener's self-check transferred 64 MiB over Linux loopback with equal
received bytes, rejected a wrong token and a dropped transfer, and confirmed
dead-registration pruning. The new tests exercised loopback TCP and a local
socket pair, including successful streaming after the external owner was
dropped and owner collection after handler completion.

## Tests

- `tools/linux/build_native.sh`: PASS, pinned soldier SDK, 79/79 CTests
  (62.78 seconds), glibc <=2.31 library checks passed.
- `python3 tools/linux/verify_lua_release.py`: PASS, 31 exact Lua files,
  zero exceptions, 32 HUD glyphs and two toolbar textures. Manifest SHA-256:
  `bf8724ef7c9ee0a77eaec8a1acc0e0cf82f624b991669f06eee234b47616bfeb`.
- `python3 netpunch/bulk_tcp.py`: PASS, including upstream ownership test.
- `python3 tools/test_bulk_lifetime.py`: PASS, three tests.
- Negative control: dropped-owner regression fails against
  `HEAD:netpunch/bulk_tcp.py`, as expected (strong reference retains owner).
- `bash -n tools/linux/build_release.sh`, upstream listener byte equality,
  staged whitespace and merge-state checks: PASS.

Logs: `.git/port-a40180f2-{build,lua,bulk,lifetime,negative}.log` and
`native/linux/out-soldier/Testing/Temporary/LastTest.log`.
No commit, merge abort, release packaging, Windows build or publication run.
