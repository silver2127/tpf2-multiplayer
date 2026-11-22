# Windows dev d8c98ccb integration

## Merged

Target `d8c98ccb4e797dfc2cc0bf24d2223cda6dcae256`, one Windows commit,
onto Linux parent `dbd33e8343525f5f84866ef9d03cf5787b6b5ef2`.
No conflicts. The merge is staged and deliberately uncommitted. Release
remains **0.7.1.1**. Both Windows pager implementations and the upstream
terrain regression are retained unchanged.

## Ported (what and how)

Reviewed README, Linux installation/integration records and native Big Maps
RE documentation. Windows throttled faults now select loading eviction even
without a recent allocation burst, avoiding steady-path delays at a load's tail.
Native allocation and UFFD fault restoration already proceed independently of
the eviction budget; there is no throttle/loading predicate to change. Kept
the native runtime policy and extended its pager test to check over-budget
allocation and consecutive cold restores complete without waiting for eviction.
See [source comparison and evidence](../re/linux/DEV_D8C98CCB.md).

Updated Lua/package provenance, release documentation links and packaged this
integration record. No game address, hook guard, layout or ABI changes.

## Not ported

None newly required by this commit. Its Windows-only throttle predicates have
no native counterpart. Existing absence of material/small paging and Windows
commit throttling remains; this integration does not claim to add those
features or resolve other previously documented native limitations.

## Live testing

No game, Proton peer, gdb session or desktop input was run. Tests exercise the
native pager in a standalone process, not a loaded game. No title menu,
renderer, loaded-world timing or cross-platform gameplay result is claimed.
Lab payloads, saves, Steam and the user's installed mod were untouched;
no actor restoration was needed and no game process was started.

## Tests

- `tools/linux/build_native.sh`: PASS, soldier build, 78/78 CTests,
  62.81 seconds total, glibc <=2.31 checks passed.
- Native pager test: PASS (not skipped), zero-budget allocations 1 ms,
  consecutive cold restores 1 ms; full lossless/race/reuse checks passed,
  64 evictions and 64 faults. These are standalone fixture measurements.
- `python3 tools/linux/verify_lua_release.py`: PASS, 31 exact Lua files,
  zero exceptions, 32 glyphs and two toolbar textures against the target.
  Manifest: `1de350a7196751cdbdc1e50bf3b4bb29fe72b547e3a109bf4ec813b84d949673`.
- `python3 bigmap/tools/linux/verify_game.py GAME_ELF`: PASS, actual lab
  ELF build-id and all 39 guarded sites.
- Release shell syntax, staged whitespace, no unmerged index entries and
  exact incoming Windows source/test comparison: PASS.
- Windows/MSVC compilation and live multiplayer were not run.

Logs are retained in `.git/port-d8c98ccb-{build,lua,elf}.log`; pager output
is in `native/linux/out-soldier/Testing/Temporary/LastTest.log`.
