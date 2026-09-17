# Upstream dev 5fb7aea2 integration: HUD station/depot wash

Windows target: `5fb7aea269d7947f22a5574c443e28caf82143a0`.
Linux merge parent: `148d7eb5c705bcd336ff0311901dea727b77318b`, following
[a658fc11](UPSTREAM_dev_a658fc11.md). One Windows commit, no conflicts.
The merge remains staged and uncommitted; no version change or publication.

## Merged

Preserve the incoming Windows station/depot style-class hook, its new
`tools/stationicon_bytes_test.py`, and the palette-test delegation change in
`tools/iconcolor_bytes_test.py` exactly. The Windows code path is intact.
All 28 Lua files still match this Windows target exactly; manifest SHA-256:
`6de7dc1e234f7c07131674197103914f9803ccdd38ac5888673a96bcfd7ba8de`.

## Ported

Advance Lua verification and release BUILDINFO provenance to the target.
Include this record in Linux packages; update README, installation and resume
coverage. Explicitly name `stationicon` in the native unavailable-tint startup
log, so it cannot be mistaken for an installed feature.
No new native rendering hook was safely established in this integration.

## Not ported

HUD station/depot company-colour wash. Fresh static investigation located
station-content and wrapper call candidates in HudIconManager::DoStep,
rejected a town-only factory using RTTI/strings, inspected component access
and aggregate-output paths, and traced two style-name binding references.
It did not establish the complete station/depot entity/live-register flow,
StationGroup/PlayerOwned lookup with safe UI-engine lifetime, or the native
style-class method/string contract. See [exact evidence and missing proof](../re/linux/DEV_5FB7AEA2.md).
The tint remains disabled; `stationicon=0` only affects Windows for now.
Previous icon/label/window tint, recovery, ownership and Workshop gaps remain.

## Tests

- `tools/linux/build_native.sh`: soldier SDK build, 45/45 CTests and glibc
  <=2.31 checks passed. Log: `.git/port-5fb7aea-build.log`.
- `python3 tools/linux/verify_lua_release.py`: 28 exact Lua files, manifest above.
- `python3 tools/palette_sync_test.py`: all five 20-colour tables and overflow
  settings match, including Linux's palette.
- `python3 tools/linux/verify_dev_d3135a59_elf.py <native-game>`: build-id,
  existing ten UI guard spans, five branch edits, instruction boundaries,
  targets, PlayerOwned RTTI and ViewCreator vtable passed against the ELF.
- Release shell syntax, staged whitespace, merge-state and incoming Windows
  source/test equality checks passed.

No native hook behavior changed, so existing UI/palette tests were retained;
no synthetic tint test is claimed. Windows PE tests were not executed: they
require the Windows executable at their hard-coded Windows path. No Steam or
game process was started, and no installation or live rendering test occurred.
