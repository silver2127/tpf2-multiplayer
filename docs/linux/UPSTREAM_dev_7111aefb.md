# Windows dev 7111aefb integration: release 0.7.1

## Merged

Windows target: `7111aefbe3a3e3d199512d4ed2a607d1afb02f08`.
Linux parent: `8a8f8622ba62aed3fc04f9c8777057ce16e0b2de`.
One incoming commit, no conflicts. Merge staged and left uncommitted.

Retained all four incoming files byte-for-byte: `installer/RELEASE-0.7.1.md`,
`installer/VERSION`, `native/src/menu_title_panel.inl` and `netpunch/lobby.py`.
The package version, Windows title fallback and shared lobby version advance
from 0.7.0.7 to 0.7.1. Every participant, including dedicated servers, must update.

## Ported (what and how)

Native CMake already tracks and reads `installer/VERSION`, defining
`TPF2MP_VERSION_STR` for the two version labels in `menu_title_linux.inl`.
Rebuilding therefore updates the native panel to v0.7.1 without duplicating a
version literal. Verified that string in the compiled `tpf2_menu.so`.
The native package builder reads the same version file; Linux and Windows
use the same Python lobby admission/rejection gate.

Advanced the Lua verifier's exact commit reference and package BUILDINFO to
this release. Package this record and the preceding f0212c87 integration
record, and link current release scope from README and Linux INSTALL.
Extended existing host/dedicated-relay admission and client welcome/roster
rejection tests to cover the previous 0.7.0.7 release explicitly.

This commit introduces no engine hook, patch bytes, address, object layout,
ABI or lifetime change. No new static or live RE is necessary. The existing
[RE evidence](../re/linux/DEV_F0212C87.md) and its limits remain applicable.

## Not ported

None introduced by this release-stamp commit. This does not close inherited
feature gaps: native Sandbox town-tool capture/cancellation and minimap
extensions remain unported, as documented in the
[previous integration](UPSTREAM_dev_f0212c87.md). Do not place native Sandbox
towns in multiplayer. Existing cargo-filter capture/replay, terrain-sidecar
and other documented native limitations remain unchanged. Upstream release
notes describe upstream work and measurements; they do not prove native
feature parity or constitute local live validation.

## Live testing

No game launched, gdb attached or desktop input sent. No lab payload, save,
Steam file or installed mod changed, so no backup/restoration was required.
No Vulkan device, on-screen title, save/load, performance or cross-platform
session observation is claimed. Version tests used real Linux loopback host
admission in ordinary and dedicated-relay modes and mocked client greetings.
No upstream performance measurement was repeated.

## Tests

- `tools/linux/build_native.sh`: PASS, pinned soldier SDK build, 75/75 CTests
  and glibc <=2.31 compatibility checks.
- `python3 tools/linux/verify_lua_release.py`: PASS, 30 exact upstream Lua
  files (no exceptions), 32 HUD textures and two multiplayer toolbar textures.
  Manifest: `4f1370ac2781c929d3526bdef7b39d17ddf945af5f25160a8707c7e7528f6e3a`.
- `.git/port-7111aefb/venv/bin/python tools/version_gate_test.py`: PASS, 2 tests,
  including previous-version rejection by host, dedicated relay and clients.
- `bash -n tools/linux/build_release.sh`: PASS.
- Compiled native menu v0.7.1, package/lobby version consistency and exact
  upstream equality of all four incoming files: PASS.

Logs: `.git/port-7111aefb/{build,lua,version,deps}.log`. The system Python
lacked `stun`; installed pystun3 and zstandard only into the clone-local venv,
with pip caching disabled and temporary files inside the clone. No system
package or release artifact was installed or published.
