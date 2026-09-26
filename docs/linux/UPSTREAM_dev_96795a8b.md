# Windows dev 96795a8b integration: public browser and log collection

## Merged

Windows target: `96795a8b487991275256c739e08903bd6a524f17`.
Linux parent: `2cece4a341416ca1a6e8dacfd44d225b2eeee705`.
Two commits: `f84a3ffe` (native OPEN LOGS patch) and `96795a8b` (larger
public browser). No conflicts. Windows code and the supplied patch are
preserved; merge staged, not committed. Release remains 0.7.0.3.

## Ported (what and how)

The native lobby now retains 32 valid public games. Its existing bounded
512 KiB subprocess response reader already exceeds Windows' new 64 KiB
buffer, so it is retained. Parsing is extracted into a fixture-testable helper.
The native Join browser uses a 660-unit panel, eight 28-unit rows, a counted
heading and page-local hit IDs 40–47. Selection maps through the last drawn
page size; pages no longer collide with company/cross-play controls. Other
pages remain 540 units. Native scaling fits the entire taller panel to the
screen; the compact 540-unit renderer supports four rows.

Most of `linux_open_logs.patch` was already implemented by
[45183ac6](UPSTREAM_dev_45183ac6.md). Retain its richer ELF identities,
fail-closed masking, truncated-tail protection, state/config coverage and
five archives per kind instead of replacing it with the older supplied patch.
Add the missing distribution (`/etc/os-release`), Steam Runtime environment
and `boot/*.so` identities. Apply the supplied standalone collector masking
hunk: all nine invitation/password JSON string keys in staged JSON/JSONL/text
copies are masked, including escaped quotes. Originals remain untouched.

Lua verification and release provenance advance to this Windows target;
release packaging includes this record. No game address, byte pattern, field
offset, SysV ABI or lifetime contract changes. These are mod-owned rendering,
JSON parsing and file collection changes; new binary RE is unnecessary. See
[existing log contracts](../re/linux/LOGS_TOOLS.md) and
[menu mapping](../re/linux/MENU_GAME.md).

## Not ported

None for these two commits. Earlier unrelated port limitations remain.

## Live testing

Copied this soldier build's installed native libraries and the current Lua mod
into the native lab actor after `cp -a` backups with `.before-port-96795a8b`
suffixes. Also backed up userdata. Ran:

`tools/sandbox/tpf2mp-lab run native --root ~/.local/share/tpf2mp-lab`

The launcher exited 1 immediately: `bwrap: setting up uid map: Permission denied`.
No game, title menu, Vulkan device selection, public browser or OPEN LOGS UI
was observed. No gdb probe or desktop input was performed. No Steam changes
or alternate game launch were attempted. Restored libraries/data, Lua and
userdata from the backups. No lab game remains running.

Job `meta/live/launch.txt` and `restoration.txt` record the attempt and cleanup;
`actor-logs/` and `data/` preserve diagnostics. Those copied actor files may
predate this failed launch and do not establish execution of this build.

## Tests

- `tools/linux/build_native.sh`: 68/68 CTests passed; soldier build and glibc
  <=2.31 import checks passed. An initial run caught the old 540-unit expected
  Join height; the fixture was updated to the intended 660 and rerun.
- Native panel regression: 20 fixture games, five scales, disjoint bounded
  controls, eight-row pages, game 9 on page two, game 20 on the last page,
  compact four-row selection, empty-list reset and unchanged host height.
- Native lobby regression: a 40-game response retains exactly 32; malformed
  JSON fails. Existing 512 KiB response bound retained.
- Archive regression adds runtime/distribution and boot-library metadata
  assertions to the existing masking, state/tail, retention and liveness checks.
- `python3 tools/linux/collect_logs_test.py`: actual shell masking block tested
  against all nine keys, escaped quotes/backslashes, multiple JSON lines and
  filenames with spaces; unrelated fields preserved.
- `python3 tools/linux/verify_lua_release.py`: 29 Lua files (one pinned native
  exception), 32 exact HUD glyphs. Manifest SHA-256:
  `d23c25e33ce4089c724211236a1344e7f710d4a6fb7c431f2d60f4a6838892e2`.
- Shell syntax, whitespace, empty unmerged index and retained MERGE_HEAD checked.

Logs: `.git/port-96795a8b-build.log`, `.git/port-96795a8b-lua.log` and
`.git/port-96795a8b-collector.log`. Windows/Proton tests were not run locally.
