# Windows dev 528294b1 integration — fresh late-join saves and stall tolerance

## Merged

Windows target: `528294b16d3c450d322369ea2aa945ef21b8c36d`.
Linux parent: `c2bbf519a47cf4b6839db8ad3a3c70c40b7175be`.
One incoming commit, no conflicts. Merge staged and left uncommitted;
release and lobby handshake remain 0.7.1.2.

## Ported (what and how)

Retained the shared `netpunch/lobby.py` change byte-for-byte from upstream.
A host about to re-serve a save older than 120 seconds, or process an aged
hot-join START queued behind a transfer, requests a fresh save through
`tpf2_sync_save.txt`. This requires a world, recovery runtime and live join;
relay-only servers do not request a local save. The serve hold lasts 120
seconds, with a 150-second request cooldown; unavailable refreshes fall back
to the existing save. Initial START and world-switch requests keep their
existing behavior. Host-loop time more than two seconds beyond select's
wait is credited to peers before the silence sweep, capped at the current time.

Reviewed README, INSTALL, NETPUNCH, MENU_LOBBY, previous integration records,
and the MENU_GAME and NATIVE_IO_LINUX RE notes. Native `lobby_linux.cpp`
already consumes this request in `SyncPoll`, calls the forced-autosave path,
emits `sync_taking`, then emits START when the new save stabilizes. This
explicit request bypasses the recent-save shortcut used by roster hot joins.
No native runtime change, ELF site, byte pattern, ABI, offset or lifetime
contract is introduced; no new static or live RE is needed for this commit.
Windows paths remain intact.

Extended the native lobby fixture to use the exact `fresh save` request with
a cached shared save, verify forced saving and `sync_taking` without premature
START, retain the request through loading, and share the completed save.
Extended the upstream loopback test to complete the refresh and verify the
late joiner's START after the fresh save arrives. Retained stall regression.
Updated README/INSTALL links, verifier and release BUILDINFO provenance, and
included this record in package staging. No release package was built.

## Not ported

None from this commit. Existing native feature gaps and outstanding gameplay
validation are unchanged. Test fixtures do not establish cross-platform
in-game save/load behavior or catch-up performance.

## Live testing

No game launched, debugger attached, desktop input sent or actor installed.
Steam, lab actors, user installations and saves were untouched; no backup or
restoration was needed. No GPU, rendering or in-game result is claimed.
Shared lobby tests used real Linux loopback sockets and temporary synthetic
save files, with the host world and menu simulated. A 600-second-old save
was held for refresh, its replacement released the late joiner, and a
10-second-old save was re-served without a refresh request. Stall testing
used synthetic timestamps, not an induced game or system stall.

## Tests

- `tools/linux/build_native.sh`: PASS, pinned soldier SDK, 79/79 CTests
  (62.80 seconds), including the extended native lobby fixture; glibc <=2.31.
- `python3 tools/linux/verify_lua_release.py`: PASS, 31 exact upstream Lua
  files, zero exceptions, 32 HUD glyphs and two toolbar textures. Manifest:
  `98911c795d3026193211339e43288137d2844db471e14249791db6fbcb55ba4b`.
- `.git/port-venv/bin/python tools/test_fresh_hotjoin_save.py`: PASS, 2 tests.
- `.git/port-venv/bin/python tools/test_lobby_limits.py`: PASS, 33 tests.
- Release shell syntax, CRLF-aware staged whitespace, upstream lobby equality,
  empty unmerged index and expected retained MERGE_HEAD: PASS.

Logs are in
`.git/port-528294b1-{build,lua,fresh,limits,deps}.log`; native test details are
in `native/linux/out-soldier/Testing/Temporary/LastTest.log`.
System Python lacked `stun`; dependencies were installed only into
`.git/port-venv`, with pip caching disabled and temporary files under
`.git/port-tmp`. No system packages were installed.
