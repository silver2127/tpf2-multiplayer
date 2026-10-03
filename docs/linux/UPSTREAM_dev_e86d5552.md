# Windows dev e86d5552 integration: release 0.7.0.4

## Merged

Windows target: `e86d555224e1f2415f021557d2fcb5482e0e79ee`.
Linux parent: `7d70c2f58d05dc9a1d0955598f1151b0c868f4b2`.
One Windows commit, no conflicts. Merge retained and staged, not committed.

The incoming commit adds identical release notes in `docs/releases/0.7.0.4.md`
and `installer/RELEASE-0.7.0.4.md`, and advances `installer/VERSION` and
shared `netpunch/lobby.py` LOBBY_VERSION from 0.7.0.3 to 0.7.0.4.
The feature descriptions summarize earlier commits; this commit changes no
native source, Lua, engine hooks, game addresses, offsets or ABI contracts.

## Ported (what and how)

The native release builder already reads `installer/VERSION` for package names,
the staged VERSION and BUILDINFO. Both platforms use the shared lobby version
gate, so the two upstream version changes apply directly on Linux.

Advance the Lua verifier reference and package BUILDINFO provenance to this
Windows target and 0.7.0.4. Retain the existing pinned native origin-replay
exception in inject.lua. Package this integration record and append current
release guidance to README and Linux INSTALL. Extend the existing real host
admission test to explicitly reject 0.7.0.3, including dedicated relay mode.

The menu and archive changes were already integrated in
[122a0ce9](UPSTREAM_dev_122a0ce9.md),
[45183ac6](UPSTREAM_dev_45183ac6.md),
[96795a8b](UPSTREAM_dev_96795a8b.md) and
[01044521](UPSTREAM_dev_01044521.md). No additional native implementation or
static/live reverse engineering is required by this release stamp. Existing
engine evidence in `docs/re/linux/` and its validation limits remain unchanged.

## Not ported

None for this commit. This does not close earlier native feature gaps or
establish cross-platform gameplay parity.

## Live testing

No game launched, no gdb attachment and no desktop input. No lab payload,
actor save, Steam file or user-installed mod was changed; no actor restoration
was needed. The lobby test used loopback peers without a game. No live visual,
GPU, loaded-world or Windows/native multiplayer result is claimed.

## Tests

- `tools/linux/build_native.sh`: PASS; pinned soldier build, 68/68 CTests,
  and glibc <=2.31 compatibility check.
- `python3 tools/linux/verify_lua_release.py`: PASS; 29 Lua files with one
  pinned native exception, 32 exact HUD glyphs. Manifest SHA-256:
  `d23c25e33ce4089c724211236a1344e7f710d4a6fb7c431f2d60f4a6838892e2`.
- `.git/port-venv/bin/python tools/version_gate_test.py`: PASS, 2 tests;
  host admission/rejection and client rejection, including old release 0.7.0.3.
- `.git/port-venv/bin/python netpunch/lobby.py --selftest`: PASS;
  loopback roster, companies, merged logs, chat, start, late join and stale save.
- `bash -n tools/linux/build_release.sh`, whitespace checks, empty unmerged
  index and retained MERGE_HEAD: PASS.

Test setup limitations: system Python initially lacked pystun3. A clone-local
venv supplies pystun3 and zstandard. Installing all requirements failed because
optional miniupnpc needs unavailable Python.h; no system packages were installed.
Loopback tests pass without UPnP; router mapping was not tested.
`python3 tools/linux/test_installer.py` was mistakenly invoked without its
required .run argument and stopped at argument access. No installer operation
ran; no release package was built or installer validation claimed.

Logs: `.git/port-e86d5552-{build,lua,version,lobby,deps,deps-pure,installer}.log`.
