# Windows dev 01044521 integration: twelve public games and one panel renderer

## Merged

Windows target: `010445210f48546dad7da23ad585cd179b9fedeb`.
Linux parent: `b9a015938ab0f896c486bed993ac4324806925f4`.
One Windows commit, no conflicts. Preserve the upstream Windows changes and
existing Linux-branch changes to the Windows source. Merge staged, not committed;
release remains 0.7.0.3.

## Ported (what and how)

The native public-list parser retains 48 games. The Join browser uses a
764-unit panel and twelve rows with page-local hit IDs 60–71; selection uses
the last drawn page size. Host and other pages remain 540 units. Preserve the
native whole-panel screen fitting. Scale complete row coordinates and the
row-count boundary together: multiplying an individually rounded row height
lost the twelfth row at some fitted scales. The compact 540-unit layout still
shows four rows.

Remove the native legacy host/join, lobby and save-picker renderers, their
button/header helpers and unused company-chip palette. Keep MwTitle and
MwStatus because the native current renderer still uses them. Only pages 1–3
render controls. Linux already requires an open page in VisibleLocked, so a
closed running resync remains invisible; explicit tests now also prove that
rendering page zero cannot revive the old panel. Existing recovery hide/show,
error/Ready reopening and world-I/O behavior remain covered.

Windows' removed BuildBackdropImage/CopyBackdrop and g_bd resources have no
native equivalent. Keep the native actively used title backdrop and Vulkan
panel image. These changes use mod-owned state, coordinates and hit IDs, with
no new game addresses, byte patterns, struct offsets, SysV calls or object
lifetimes. The engine contracts in [MENU_GAME](../re/linux/MENU_GAME.md) are
unchanged; no new ELF reverse engineering or live ABI probe is required.

Update palette synchronization tests to compare the three remaining palette
consumers (Windows slice, shared company Lua and shared stylesheet); remove
fixture calls to the deleted native CoColor. Advance Lua verification and
release provenance, package this record and update menu documentation.

## Not ported

None for this commit. Earlier unrelated Linux limitations remain unchanged.

## Live testing

No game launched, no gdb attachment and no desktop input. Validation uses
native renderer/input/model fixtures; no live visual or multiplayer outcome
is claimed. No lab payload was installed and no actor, saves, Steam files or
user-installed mod was changed, so no restoration was necessary.

## Tests

- `tools/linux/build_native.sh`: soldier build, 68/68 CTests and glibc <=2.31
  compatibility checks passed.
- Panel regression: twelve rows at five scales on both 720p and 1080p screens,
  bounded nonoverlapping hit targets, game 12 on row 12, game 13 on page two,
  eight rows on page two of twenty games, game 20 selection, compact four-row
  paging, empty-list reset, unchanged host height, invisible closed resync and
  no controls on closed/unknown pages.
- Lobby regression: sixty returned games retain exactly 48; malformed JSON
  still fails. Existing recovery model and reopening tests pass.
- Palette synchronization passes for all three remaining consumers. The first
  full build exposed this test's references to deleted Windows/native palette
  helpers and the fractional-scale row-count issue; both were fixed before
  the final full build.
- `python3 tools/linux/verify_lua_release.py`: 29 Lua files (one pinned native
  exception), 32 exact HUD glyphs. Manifest SHA-256:
  `d23c25e33ce4089c724211236a1344e7f710d4a6fb7c431f2d60f4a6838892e2`.
- Release script shell syntax, port whitespace checks, empty unmerged index
  and retained MERGE_HEAD checked. Upstream CRLF is preserved in Windows code;
  plain git diff --check flags those CR characters, while cr-at-eol checking
  passes. No Windows/Proton test binaries or release package executed.

Logs in this clone: `.git/port-01044521-build.log`,
`.git/port-01044521-build-initial.log` and `.git/port-01044521-lua.log`.
