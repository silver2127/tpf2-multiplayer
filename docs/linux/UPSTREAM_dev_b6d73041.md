# Windows dev b6d73041 integration

## Merged

Target `b6d73041483deb189e879b08606027b27a7de57f`, one Windows commit,
onto Linux parent `a5e8ddd970118a9b7b61ed7d23622643bdf5acf4`.
No conflicts. All four incoming Windows files are retained byte-for-byte.
The merge is staged and deliberately uncommitted; release remains **0.7.1.1**.
Windows automatic free-commit pressure now uses RAM/8 bounded to 2..4 GiB,
with a 4 GiB fallback for unknown RAM. Positive `commit_tight_mb` overrides it.

## Ported (what and how)

Retained the native MemAvailable policy, following earlier pressure/throttle
integrations. Native terrain paging has no Windows free-commit threshold or
load throttle; this change does not reduce its separate physical RAM reserve.
Documented the Windows-only setting in native configuration, installation and
port notes, and added large-memory policy regressions. See
[source comparison and evidence](../re/linux/DEV_B6D73041.md).
Updated exact Lua/package provenance, README and release record packaging.
No game addresses, instruction bytes, structure offsets or ABI changes.

## Not ported

None newly required for the existing native backend. `commit_tight_mb` tunes
only the Windows controller and has no native effect. Native material/small
paging and Windows commit-controller differences are inherited limitations;
this integration does not add those backends or claim full policy parity.

## Live testing

No game, Proton peer, gdb session or desktop input was run. No native runtime
or hook contract changed; validation uses standalone pager fixtures and the
read-only ELF verifier. No title menu, Vulkan device, loaded-world performance
or multiplayer result is claimed. Lab actor payloads and saves, Steam and the
user's installed mod were untouched. No backup/restoration was necessary and
no game process was started.

## Tests

Validation logs: `.git/port-b6d73041-{build,lua,elf}.log` and
`native/linux/out-soldier/Testing/Temporary/LastTest.log`.

- `tools/linux/build_native.sh`: PASS, soldier build, 78/78 CTests in
  62.86 seconds and glibc <=2.31 checks.
- Native pager: PASS (not skipped), including the new 94 GiB policy cases,
  lossless eviction, parallel restore, write/evict race and reuse; 64 evictions
  and 64 faults. Zero-budget allocation and cold restores each took 1 ms in
  the standalone fixture, not the game.
- `python3 tools/linux/verify_lua_release.py`: PASS, 31 exact upstream Lua
  files, zero exceptions, 32 glyphs and two toolbar textures. Manifest:
  `1de350a7196751cdbdc1e50bf3b4bb29fe72b547e3a109bf4ec813b84d949673`.
- Actual lab ELF build-id and all 39 guarded sites: PASS.
- Release shell syntax, CRLF-aware staged whitespace, upstream file equality,
  correct MERGE_HEAD and empty unmerged index: PASS.
- Windows/MSVC compilation, Windows ctypes tests and live multiplayer were
  not run. No release package was built or published.
