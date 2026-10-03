# Windows dev bef70213 integration: crashed run's mod logs

## Merged

Windows target: `bef70213df4cb65d3b6ae825e8e2f58a08a64121`.
Linux parent: `5a31c4df8f4e438d424dd9af004ff950a001275d`.
One Windows commit, no conflicts. `native/src/logarchive.h` and
`tools/logarchive_test.cpp` remain byte-identical to the Windows target.
Merge remains staged and uncommitted; release remains 0.7.0.5.

## Ported (what and how)

Native OPEN LOGS copies every regular `*.log` from the newest startup
archive as `previous_run_*`, alongside its game log and crash dumps.
This preserves the dead run's host, terrain, bridge and prefixed lobby
logs even after their current-run replacements have been written.
Copies use the existing 32 MiB log-tail limit and shared 200 MiB budget,
precede state files, and leave the source archive intact. Startup does not
recursively collect earlier archives. Existing archive selection and
retention behavior is preserved. The standalone collector already collects
saved-run `*.log` files and needs no modification.

The native regression distinguishes current/dead-run host contents, checks
bridge/terrain/lobby recovery and source preservation, and tests newest-run
selection and no recursive startup collection. Updated README, INSTALL,
package integration records and Lua/BUILDINFO provenance to this target.
The existing pinned native Lua exception is preserved.
See [filesystem evidence](../re/linux/LOGS_TOOLS.md#dev-bef70213-previous-run-mod-logs-2026-09-26).

## Not ported

None from this commit. Earlier unrelated native limitations remain unchanged.
No game hook, patch, address, offset, ABI or lifetime contract is introduced.
No additional static ELF analysis or live reverse engineering is needed.

## Live testing

No game was launched. Tests exercise the native archiver on isolated
filesystem fixtures, including a synthetic crash/restart sequence; they do
not demonstrate a real crash or OPEN LOGS UI interaction. No GPU/rendering
or cross-platform gameplay result is claimed. Steam, the lab actors and saves
were untouched, so no actor backup/restore was necessary.

## Tests

- `tools/linux/build_native.sh`: PASS, pinned soldier SDK, 70/70 CTests
  and glibc <=2.31 checks. Log: `.git/port-bef70213-build.log`.
- `python3 tools/linux/verify_lua_release.py`: PASS, 29 Lua files with one
  pinned native exception and 32 exact HUD glyphs.
  Log: `.git/port-bef70213-lua.log`. Manifest SHA-256:
  `8dba1685def2dc2231c1d6d06fc620b0e2e9eea6f155178a8e4e051e28d070d1`.
- Extended regression failed against the original native header (missing
  previous-run mod log), then passed in soldier with the port.
  Negative-control log: `.git/port-tests/regression-before.log`.
- Release shell syntax, CRLF-aware whitespace, upstream Windows file equality,
  empty unmerged index and retained MERGE_HEAD checks pass.

Windows tests were preserved but not executed. No commit, merge abort or
publication occurred.
