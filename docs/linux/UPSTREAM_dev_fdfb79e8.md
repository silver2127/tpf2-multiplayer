# Windows dev fdfb79e8: per-terrain served ranges

## Merged

Windows target: `fdfb79e8ca1dfd62d3d9b4e7d018b6c04081939c`.
Linux parent: `8e891c6eef80bfde215ee2c22fe0db904c56bf60`.
One commit, no conflicts. Both incoming Windows files are retained exactly:
`bigmap/src/terrain_serve.h` and `bigmap/tools/test_terrain_serve.cpp`.
The merge remains staged and uncommitted. Release remains **0.7.1.1**.

## Ported (what and how)

Recorded the native boundary and fresh investigation, and packaged this record.
Windows now retains up to four grids' ranges and applied counts, so serving a
second CTerrain version no longer discards the first version's ranges.
Native runtime behavior is unchanged; Lua is unchanged.

## Not ported

Native per-grid served-range tracking and complete-sidecar alignment bypass.
The prerequisite native runtime sidecar serving remains absent. Fresh static
analysis reconfirmed metadata layout, but the lab launch failed before startup,
preventing live ownership/lifetime proof. No speculative hook or inert option
was introduced. See [RE evidence and missing contracts](../re/linux/DEV_FDFB79E8.md).
Linux continues its existing alignment path; no load-time improvement is claimed.

## Live testing

Candidate soldier libraries and merged Lua were installed into the native lab
actor after cp -a backups. With autoload=1 and newgame_density=0, the prescribed
launcher exited 1 immediately: `bwrap: setting up uid map: Permission denied`.
No game, GPU selection, menu, save load or gdb observation was reached.
Both payload directories were restored and file/symlink manifests matched.
No save changed, no game remains running, and Steam was untouched.
Job meta/live contains launch/static evidence and archived actor logs/data;
the latter may predate the attempt and do not establish candidate gameplay.

## Tests

Validation results are recorded below. No native runtime code changed, so no
new synthetic test was added for an unavailable feature. The incoming Windows
two-version regression is retained, but was not run (Windows APIs and PE fixture).

- `tools/linux/build_native.sh`: PASS, soldier build, **76/76 CTests**,
  62.32 seconds; glibc <=2.31 checks passed. Includes native alignment, pager
  and the synthetic shared sidecar writer test.
- `python3 tools/linux/verify_lua_release.py`: PASS, 31 exact Lua files,
  zero exceptions, 32 HUD glyphs and 2 toolbar textures. Manifest SHA-256:
  `1de350a7196751cdbdc1e50bf3b4bb29fe72b547e3a109bf4ec813b84d949673`.
- `python3 bigmap/tools/linux/verify_game.py <lab ELF>`: PASS, build-id and
  all 30 guarded sites.
- Release script shell syntax, CRLF-aware staged whitespace, incoming-file
  identity, empty unmerged index and retained MERGE_HEAD checked.
  No Windows build or package archive was run.

Final status: **PARTIAL**. No commit, abort or publication.
