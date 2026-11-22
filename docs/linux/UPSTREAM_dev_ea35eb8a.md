# Windows dev ea35eb8a — Linux terrain pager policy

Merged `ea35eb8a3b601269a053508bb7db52d84e7810ff` into `port/dev`,
without conflicts. Merge remains staged and uncommitted. Release remains 0.7.
The upstream commit changes only `bigmap/docs/terrain-compression.md`, but
explicitly requests a native pager correction; this integration implements it.

## Ported

Native eviction now selects the oldest eligible fault/allocation recency,
protecting recently restored tiles and extending grace after rapid refaults.
Automatic budgets use live tile bytes and available RAM, preserving Windows'
2..12 GiB headroom and 4..8 GiB resident-cap formulas. Explicit hot budgets
remain fixed. The 30-second status line reports the budget and interval faults/s.
Windows sources, the shared codec and Lua are unchanged.

See [the implementation and evidence](../../bigmap/docs/linux/PORT.md#pager-recency-and-headroom--dev-ea35eb8a-2026-09-22)
for formulas, timing, limits and validation. No new game hooks or ABI assumptions
were needed; all existing guard sites remain intact.

## Not ported

None from this commit. Earlier unrelated feature gaps remain as previously
recorded. Ordinary resident accesses are invisible to userfaultfd; recency
is based on observed faults as upstream requested, not true access-bit LRU.

## Live testing

Installed candidate soldier libraries, native Big Maps and shared Lua into
backed-up lab actor copies, with automatic budget and sparse density disabled.
The standard helper failed `bwrap: setting up uid map: Permission denied`.
The previously documented system-bwrap/no-inner-runtime fallback reached plugin
initialization: 1022 MiB startup fallback, automatic headroom, userfaultfd enabled,
plugin OK. SteamAPI could not find running Steam and the process exited before
any world loaded. Startup emitted steam.sh bootstrap messages; no separate
Steam command was issued. No menu, Vulkan device, map faults/s or speed improvement
was observed. No gdb probe was needed for this policy-only change.

Both actor directories were restored and `diff -qr` passed. No game process
remained and no save was loaded. Job `meta/live/` contains evidence; archived
actor logs also contain historical runs and do not establish this run's results.
Big-map throughput validation remains outstanding.

## Tests

- `tools/linux/build_native.sh`: soldier build, 63/63 CTests and glibc baseline passed.
- Pager regressions: grace/refault/reuse, budget bounds/headroom/missing information;
  real userfaultfd lossless paging, concurrent restore/write/evict and slot reuse passed.
- `python3 bigmap/tools/linux/verify_game.py <lab ELF>`: build-id and 27 sites passed.
- `python3 tools/linux/verify_lua_release.py`: 29 Lua files and 32 HUD textures passed.
- `git diff --check`: passed.
