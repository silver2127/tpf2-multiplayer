# Windows dev 30eba2da integration — minimap shortcut default

## Merged

Windows target: `30eba2dad44f48880200ddd2d81ba200b2b66551`.
Linux parent: `9c752ac5430fd70b726f7280cb0fd9b8cb0d90be`.
One incoming commit: `bigmap: the M key no longer opens the minimap by default
(minimap_key=0)`. No conflicts; merge staged and left uncommitted.
Release remains 0.7.1.2.

## Ported (what and how)

Retained both upstream files exactly: `bigmap/cfg/tpf2_bigmap.cfg` now ships
`minimap_key=0`, and the Windows `InstallMinimap` configuration fallback is
also 0. Explicit `minimap_key=1` still enables the existing Windows shortcut;
the toolbar path is unchanged. Corrected the old default-on statement in
`bigmap/docs/minimap.md` and identified its Windows-only interception details.

Reviewed README, Linux INSTALL, the preceding integration, native Big Maps
initialization/configuration and the prior minimap investigation in
[DEV_F0212C87](../re/linux/DEV_F0212C87.md). Native Big Maps has no minimap
backend or M-key interceptor, so it already leaves M to the game. No native
code change or inert configuration key is needed to implement this default.
The missing native minimap and optional shortcut are inherited limitations,
not features introduced by this one-line default change. No address, byte
pattern, ABI, offset or engine contract changes; no new RE is required.

Updated README/INSTALL, release BUILDINFO and Lua verification provenance to
this target, and included this record in release packaging. Shared Lua and
native runtime code are unchanged. There is no Linux minimap test suite to
extend; the Windows DLL minimap suite is not a native Linux runtime test.

## Not ported

None required by this commit. The pre-existing native minimap backend,
toolbar and opt-in M shortcut remain unavailable, as documented in the
linked historical static/live investigation. This integration does not claim
to close those gaps.

## Live testing

No game launched, debugger attached, desktop input sent or lab actor changed.
The default change introduces no native hook or unresolved engine contract.
No new live keyboard, toolbar or cross-platform behavior is claimed. Steam,
lab payloads and saves were untouched; no backup or restoration was needed.

## Tests

- `tools/linux/build_native.sh`: PASS, pinned soldier SDK, 79/79 CTests
  (62.85 seconds), library glibc <=2.31 baseline passed.
- `python3 tools/linux/verify_lua_release.py`: PASS, 31 exact upstream Lua
  files, zero exceptions, 32 HUD glyph textures and two toolbar textures.
  Lua manifest SHA-256:
  `bf8724ef7c9ee0a77eaec8a1acc0e0cf82f624b991669f06eee234b47616bfeb`.
- `bash -n tools/linux/build_release.sh`: PASS.
- Both incoming files match the Windows target byte for byte; no unresolved
  index entries. Staged whitespace check passes.

Evidence: `.git/port-30eba2da-build.log`, `.git/port-30eba2da-lua.log` and
`native/linux/out-soldier/Testing/Temporary/LastTest.log`.
No commit, merge abort, publication or release packaging run.
