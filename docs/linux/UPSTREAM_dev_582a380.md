# Upstream dev 582a380 integration: dedicated restart save selection

Windows target: `582a3807142914fba83743858de36e0c9a123854`.
Linux parent: `b1ac39f6c3ccc117b190f5f2e7016aecfe9ea092`.
Two Windows commits; no conflicts. Merge staged and uncommitted; version remains 0.7.

## Merged

- `e65460e`: Windows dedicated startup compares the configured save with its
  own latest autosave; upstream source and regression checks retained unchanged.
- `582a380`: shared cross-platform report identifies the VPS by its role rather
  than its root login; retained unchanged.

## Ported (what and how)

`native/linux/src/lobby_linux.cpp::DedicatedStartupSave`, used by DedicatedTick,
scans the existing native save directory with POSIX directory/stat calls.
Only regular `autosave_mp_shared*.sav` files can replace an existing configured
save, and only with a strictly newer nanosecond mtime. Equal or older saves,
unrelated worlds, companion files and directories cannot override it. Missing
or unset configuration retains the existing MenuGame_NewestSave fallback.
The existing shared placement and load route consumes the selected path.

This is filesystem policy, with no new executable addresses, patches, struct
layouts or calling conventions. The native save directory, placement and load
contracts are documented in [MENU_GAME.md](../re/linux/MENU_GAME.md).
No new reverse engineering is required. Shared Lua and its verifier baseline
are unchanged. The integration record is included in release packaging.

## Not ported

None for these two commits. Earlier integration limitations remain unchanged.

## Live testing

No game launched. The actual production selector was exercised off-game with
real temporary files and controlled nanosecond timestamps in `lobby_ready`.
No game restart, loaded-world autosave or cross-platform gameplay observation
is claimed. No lab actors, saves, installed mod or Steam state were changed.

## Tests

- `tools/linux/build_native.sh`: soldier SDK build, 65/65 CTests and glibc
  baseline checks passed.
- `lobby_ready`: configured save alone; older/equal own autosaves; newest of
  multiple own autosaves; newer configured save; unrelated saves, placed copy,
  companions and directories excluded; missing/unset config fallback; no saves.
- `python3 tools/linux/verify_lua_release.py`: 29 Lua files and 32 HUD textures
  passed. Manifest SHA-256:
  `cbcd41b781d05d4f475926a898d5cd1cf44c637207e20d962279cc4f43863885`.
- Optional `python3 tools/dedicated_mode_test.py` could not start: host Python
  lacks `lupa.lua52`. A clone-local dependency install attempt also found no
  pip. No system packages were installed. Native regression tests passed.
- Release script syntax and staged whitespace checks (CRLF-aware) passed;
  unmerged index empty and MERGE_HEAD retained.

Logs are `.git/port-582a380-{build,lua,dedicated,pip}.log` in this clone.
