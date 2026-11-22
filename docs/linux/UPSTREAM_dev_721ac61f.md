# Windows dev 721ac61f integration — asynchronous lobby save preparation

## Merged

Windows target: `721ac61f21e870a11820c832a57b8bdb9c1588d0`.
Linux parent: `768cd2cc81ba564cbf1b76cea625a21872ff320e`.
One incoming commit, no conflicts. Merge staged and left uncommitted.
Release and lobby handshake remain **0.7.1.3**.

## Ported (what and how)

Retained `netpunch/lobby.py` byte-for-byte from Windows upstream. Linux uses
this same Python implementation, including on dedicated servers:

- A `save-read` worker reads the save, computes per-file and overall hashes,
  and discovers required mods. It queues its result and diagnostic notes for
  the host loop, which constructs the real transfer with the precomputed hash.
- A placeholder occupies the transfer slot during preparation, keeping its
  targets marked active for keepalives and preventing overlapping transfers.
  Departed targets are excluded; a result whose placeholder was cleared is
  discarded. Read errors become failed status messages on the host loop.
- Joiners tolerate 30 seconds of host silence. Mesh routing still switches
  away after 12 seconds (or a shorter explicitly configured host timeout).

Reviewed README, INSTALL, NETPUNCH, earlier integration records (especially
528294b1 and 262353d7), and the native saving/controller RE notes in
`docs/re/linux/`. This changes preparation of an already-written save in the
separate lobby process, not the game's save writer or Linux fork-save design.
No new ELF site, byte pattern, offset, calling convention or game-owned
lifetime is introduced. Static disassembly and live gdb probes are unnecessary
for this shared-code-only change; existing native guards remain intact.

Retained upstream's slow-read regression and extended the same loopback
fixture with a three-second mod-discovery delay. The new regression requires
both initial and late-join preparation to use the worker and complete without
host-loop stall reports. Updated README/INSTALL, the Lua verification baseline
(unchanged content), package BUILDINFO provenance and packaged integration list.
Windows runtime code paths remain intact.

## Not ported

None from this commit. Existing native feature gaps and outstanding gameplay
validation remain as recorded in earlier integrations. No new cross-platform
gameplay or large-save performance claim is made.

## Live testing

No game, Proton peer, debugger or desktop input was run. No actor payload or
save was changed, so backups/restoration were unnecessary. Steam and user
installations were untouched; there is no GPU or on-screen observation.

Linux loopback tests used real sockets and temporary synthetic save files,
with the host world/menu simulated. Initial and late joiners completed with
three-second save reads and, separately, three-second mod scans; neither case
reported a host-loop stall. An old save waited for its refreshed replacement,
and a recent save was served directly. These are lobby fixture observations,
not live game results or a recreation of upstream's memory-pressure incident.

## Tests

- `tools/linux/build_native.sh`: PASS, pinned soldier SDK, 79/79 CTests
  (62.75 seconds), glibc <=2.31 checks passed.
- `python3 tools/linux/verify_lua_release.py`: PASS, 31 exact upstream Lua files,
  zero exceptions, 32 HUD glyphs and two toolbar textures. Manifest SHA256:
  `f372ad8ebf2800b6b74d457def98c4975123a5551ce0faaca53bb0323a309b5b`.
- `tools/test_fresh_hotjoin_save.py`: PASS, 4 tests, including slow read and
  slow mod discovery, old-save refresh and recent-save reuse.
- `tools/test_lobby_limits.py`: PASS, 33 tests.
- `tools/test_live_join_lobby.py`: PASS, 4 tests.
- `tools/save_fack_order_test.py`: PASS, late acknowledgements and re-request.
- `tools/test_transfer_status.py`: PASS, 3 tests.
- `tools/test_host_missing_mods.py`: PASS, 5 tests.

- Documentation links: PASS, 378 documents, zero dead links, anchors or missing
  prose paths (56 informational orphans). Release shell syntax, whitespace,
  upstream lobby identity and retained merge state checks passed.

Python lobby tests used `.git/port-venv/bin/python`. System Python lacked
`stun`; pystun3, cryptography and zstandard were installed only in that local
venv, with pip caching disabled and temporary files under `.git/port-tmp`.
No system packages installed. Logs: `.git/port-721ac61-*.log`; native detail:
`native/linux/out-soldier/Testing/Temporary/LastTest.log`.
No release archive or Windows build was run. No commit, abort or publication.
