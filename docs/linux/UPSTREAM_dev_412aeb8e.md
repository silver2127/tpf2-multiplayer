# Windows dev 412aeb8e integration: release 0.7.1.1

## Merged

Windows target: `412aeb8ecfeff72abd38f246b04be328fbc93b56`.
Linux parent: `897bc92013d85cd5d8e08b41535895099e7fc30d`.
One incoming commit, no conflicts. Merge staged and left uncommitted.

Retained all four incoming files byte-for-byte: `installer/RELEASE-0.7.1.1.md`,
`installer/VERSION`, `native/src/menu_title_panel.inl` and `netpunch/lobby.py`.
The package version, Windows title fallback and shared lobby version advance
from 0.7.1 to 0.7.1.1. Every participant, including dedicated servers, must update.

## Ported (what and how)

Native CMake already tracks and reads `installer/VERSION`, defining
`TPF2MP_VERSION_STR` for both version labels in `menu_title_linux.inl`.
Rebuilding updates the native panel to v0.7.1.1 without another version literal.
The native package builder reads the same file, and the shared Python lobby
uses the updated version for admission on both platforms.

Advanced the exact Lua verifier baseline and package BUILDINFO provenance,
packaged this integration record, and updated README/INSTALL release links.
Extended existing host/dedicated-relay admission and client welcome/roster
rejection tests to cover the previous 0.7.1 release explicitly.

The release notes describe fixes already integrated in
[dev d4a11297](UPSTREAM_dev_d4a11297.md) (recent peer clocks and spare-line
catch-up gating) and [dev 893be145](UPSTREAM_dev_893be145.md) (dedicated
Workshop registration). No engine hook, address, byte pattern, ABI, struct
offset or lifetime contract changes in this commit. No new static or live
reverse engineering is needed; existing native RE evidence remains applicable.

## Not ported

None introduced by this release commit. Inherited Sandbox town-tool capture,
minimap, cargo-filter, terrain-sidecar and other documented native feature and
live-validation limits remain unchanged. Upstream release-note observations
are not local test results or proof of cross-platform gameplay parity.

## Live testing

No game launched, debugger attached or desktop input sent. No lab actor,
save, Steam file or installed mod was changed; no backup/restoration was
needed and no game process was started. No Vulkan device, on-screen title,
loaded-world catch-up, Workshop save acceptance or native/Proton session
observation is claimed. Version tests exercised real Linux loopback host
admission in ordinary and dedicated-relay modes, with mocked client greetings.

## Tests

- `tools/linux/build_native.sh`: PASS, pinned soldier SDK build, 75/75 CTests
  and glibc <=2.31 compatibility checks.
- `python3 tools/linux/verify_lua_release.py`: PASS, 30 exact upstream Lua
  files, zero exceptions, 32 HUD textures and two toolbar textures. Manifest:
  `e4f0ff50af42446e7812c2390165c6fd7492489d0d58457737d55e2229c3b851`.
- `tools/version_gate_test.py` in the existing lobby Python environment: PASS,
  2 tests covering host/dedicated admission and client rejection, including
  explicit previous-version 0.7.1 rejection.
- `bash -n tools/linux/build_release.sh`: PASS.
- Compiled `tpf2_menu.so` contains `v0.7.1.1`; package/lobby versions agree;
  all four incoming files are byte-identical to upstream; no unresolved index
  entries; staged whitespace check: PASS.

The release package itself was not built or installed.
Logs: clone-local `.git/port-412aeb8e/`. Lobby tests use the pre-existing
`~/.cache/tpf2mp/netpunch-build/venv/bin/python`; temporary files stay in the
clone. No dependency, release artifact or system package was installed or
published.
