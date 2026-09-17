# Upstream dev a658fc11 integration: 20-colour company palette

Windows target: `a658fc113240baeaf5f00131d11fa42c0ffd2812`.
Linux merge parent: `b800234`, following [59bb258a](UPSTREAM_dev_59bb258a.md).
One Windows commit, no conflicts. Merge remains staged and uncommitted;
`installer/VERSION` is unchanged. Nothing is installed or published.

## Merged

Preserve the incoming Windows menu and slice palettes and the shared Lua
vehicle-paint/stylesheet changes exactly. The first 20 companies use the
Trubetskoy palette; generated colours start at company 21. Retain and extend
upstream `tools/palette_sync_test.py` to cover the native Linux lobby too.

All 28 Lua files exactly match the target. Manifest SHA-256:
`6de7dc1e234f7c07131674197103914f9803ccdd38ac5888673a96bcfd7ba8de`.
The Lua verifier, README, installation guide and release BUILDINFO now name
this revision; release packaging includes this integration record.

## Ported

Update `native/linux/src/panel_linux.cpp::CoColor` to the same ordered 20 RGB
triples, fixed-colour bound 20 and golden-angle offset 21. The existing Linux
RGB representation and float arithmetic remain intact. No engine hook, struct
layout or calling convention changes are needed for lobby chips.
Shared Lua provides the new paint and dashboard/stylesheet colours on Linux.

## Not ported

The new palette cannot appear on native vehicle icons, station labels or
window washes because their company-tint hooks remain unimplemented from
previous integrations. Fresh static disassembly rechecked the label colour
input, incompatible vehicle vertex layout and window entity-binding helper;
it did not establish a safe complete render/style hook. See
[attempts, exact bytes and missing evidence](../re/linux/DEV_A658FC11.md).
These consumers remain disabled; no unsafe placeholder was added. This
integration is partial for those existing native tint gaps. Earlier recovery,
ownership and Workshop limitations also remain unchanged.

## Tests

- `tools/linux/build_native.sh`: PASS, soldier SDK build, **45/45 CTests**
  and glibc <=2.31 compatibility checks. Log: `.git/port-a658fc11-build.log`.
- `python3 tools/linux/verify_lua_release.py`: 28 exact upstream Lua files;
  manifest above.
- `python3 tools/palette_sync_test.py`: all five tables contain the same 20
  colours, with matching fallback offsets and native fixed-colour bounds.
  Registered as a native CTest so every required build checks palette drift.
- Extended existing panel test executes production `CoColor` at IDs 1, 7,
  20, 21 and 22, covering both fixed-palette and overflow boundaries.
- Release shell syntax, preserved Windows source equality, merge-state and
  staged whitespace checks.

No Steam/game process was launched; no live rendering or cross-platform
session compatibility is claimed.
