# Windows dev 847f2563 integration — 0.7.1.5 lobby compatibility

## Merged

Target `847f25633756ad5e7f6d6b71523564ad35bad41a`, Linux parent
`92476d274d7b5b2115b7bb2d5a94eb01fb032e2b`. One Windows commit,
no merge conflicts. Retained the shared lobby version rollback, release notes
and new compatibility regression. Lobby and release notes match upstream
byte-for-byte. The merge is staged and remains uncommitted.

## Ported (what and how)

Linux clients and dedicated servers use the shared `netpunch/lobby.py`, so
`LOBBY_VERSION = "0.7.1.5"` applies directly. Package version remains 0.7.1.6:
native CMake reads `installer/VERSION`, and both native title-panel labels use
`TPF2MP_VERSION_STR`. The log and public listing use lobby protocol 0.7.1.5.
The host-side recovery fix requires the host to update; clients and servers
can otherwise update independently across these two releases.

Corrected the prior Linux integration's version-gate test lists: 0.7.1.5 is
accepted, while a literal 0.7.1.6 protocol advertisement is rejected (an actual
0.7.1.6 package advertises 0.7.1.5). Host and relay admission still exercise
real loopback connections; client welcome/roster mismatches are simulated.
Extended the upstream regression to assert package version 0.7.1.6 as well.
Updated current README/install guidance, the packaged integration record,
BUILDINFO provenance and the Lua verifier's exact upstream reference.
Historical integration records remain unchanged.

Reviewed README, Linux installation and prior integration records, native
CMake/title labels and RE notes `DEV_192ECD4D` and `NATIVE_IO_LINUX`.
This commit changes no Windows hook, native code, engine address, byte pattern,
ABI, structure offset or lifetime contract. No binary RE or live ABI proof
is needed. Existing native guards and Windows code paths remain intact.

## Not ported

None for this commit. Previously documented native feature and gameplay
validation limitations remain; this protocol compatibility correction does
not establish new cross-platform gameplay results.

## Live testing

No game, Proton peer, debugger or desktop input was run. No lab actor payload,
save, user mod or Steam directory was changed; backup/restoration was not
needed. Loopback lobby tests are automated tests, not live game observations.
No title rendering, GPU, loaded-world or cross-platform gameplay result is
claimed. Upstream release-note validation claims remain upstream text.

## Tests

- `tools/linux/build_native.sh`: PASS with the pinned soldier SDK, 79/79
  CTests (62.84 seconds), followed by passing glibc baseline checks.
- `python3 tools/linux/verify_lua_release.py`: PASS, 31 exact Lua files,
  zero exceptions, 32 HUD glyphs and two toolbar textures. Manifest SHA-256:
  `f7a4d20419e173a3cb2d5aa4ef75155299b3bb6a72ab6eb9f9e1c6cdad9f0007`.
- `.git/port-venv/bin/python tools/version_gate_test.py`: PASS, three tests:
  package/protocol compatibility, host/relay admission, client welcome/roster
  rejection. Initial system-Python run lacked `stun`; installed pystun3 and
  zstandard in a clone-local virtual environment with pip caching disabled.
  No system packages or network settings were changed.
- Built `tpf2_menu.so` contains `v0.7.1.6`; lobby/release-note identity against
  upstream, release script shell syntax and Git whitespace checks passed.
  No unmerged index entries.

Logs: `.git/port-847f2563-{build,lua,version,deps}.log` and
`native/linux/out-soldier/Testing/Temporary/LastTest.log` in this clone.
Windows/MSVC compilation and release archive packaging were not run.
No commit, merge abort, publication or system installation.
