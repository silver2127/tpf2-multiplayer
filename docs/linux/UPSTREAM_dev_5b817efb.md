# Windows dev 5b817efb integration

## Merged

Target `5b817efb537175821bdc17a1c9b609440c8d10b0`, one commit retaining a
terrain sidecar through both served terrain versions' load passes. Release
remains **0.7.1.1**. Resolved and staged `bigmap/linux/tests/sidecar_test.cpp`;
the merge is deliberately uncommitted. Windows source and tests remain exactly
as supplied by upstream.

## Ported (what and how)

Retained upstream's native Linux `finished` flag per grid, pending-version
query, and explicit skipped/computed argument to PassDone. A skip retains the
file while another served grid awaits completion; a pass that runs releases
it. Each grid skips once per load. Existing Linux activation guards and
unsigned version increments are preserved. No new engine hook or layout is
introduced; all 39 existing ELF guards pass. See
[static and live evidence](../re/linux/DEV_5B817EFB.md).

Conflict resolution retains the Linux disabled-hook forwarding regression and
upstream's two-version regression. Extended the latter so the second grid is
partially served at first completion, remaining tiles are decoded afterward,
both grids publish matching min/max, and duplicate completion does not bump
versions. The next load resets completion state. Updated Lua/package provenance
and packaged this record alongside the preceding integration record.

## Not ported

None of this commit's code changes are omitted. Native sidecars remain
experimental and default off, as before. Production enablement and existing
live cache ownership, pager synchronization, terrain identity/lifetime and
load scheduling validation are still outstanding; this integration does not
claim to resolve those inherited limitations or reproduce Windows timings.

## Live testing

Installed candidate soldier libraries and merged Lua in the native lab actor,
with sidecars/autoload enabled, after backing up both payload directories.
`tools/sandbox/tpf2mp-lab run native --root ~/.local/share/tpf2mp-lab` exited 1
before game execution: `bwrap: setting up uid map: Permission denied`.
No menu, Vulkan device, gdb attachment, save/load, two-pass behavior or visible
gameplay was observed. No Proton or desktop input test was performed.
Both payloads were restored; SHA-256 file and symlink manifests match backups.
No saves changed and no game remains. Steam and user installations were untouched.
Evidence is in this job's `meta/live/`; copied actor logs/data are historical.

## Tests

- `tools/linux/build_native.sh`: PASS, final soldier build, 77/77 CTests,
  glibc symbol baseline <=2.31. First run: 76/77; the combined fixture
  needed to recapture terrain after the disabled-hook test. Fixed setup,
  focused sidecar regression and full rerun passed.
- `python3 tools/linux/verify_lua_release.py`: PASS against the incoming commit:
  31 Lua files, zero exceptions, 32 HUD glyphs and two toolbar textures.
  Manifest `1de350a7196751cdbdc1e50bf3b4bb29fe72b547e3a109bf4ec813b84d949673`.
- `python3 bigmap/tools/linux/verify_game.py GAME_ELF`: PASS build-id and all
  39 guarded sites.
- `bash -n tools/linux/build_release.sh`, staged/unstaged diff whitespace
  checks: PASS. Windows/MSVC tests were not run on Linux.
