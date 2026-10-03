# Free-commit threshold — dev 0eb9eea2

Windows target: `0eb9eea2b05ac132565e6a4bbc3d3a981f3a60ca`.

`bigmap/src/terrain_compression.h::CommitTightBytes` changes its automatic
upper bound and unknown-RAM fallback from 4 to 3 GiB. RAM/8, the 2 GiB lower
bound and positive `g_commitTightMB` overrides are unchanged. Both Windows
terrain and material workers call this helper. Configuration comments and
Windows ctypes expectations change accordingly, including a 20 GiB input
whose result remains 2.5 GiB. All three incoming files are retained exactly.

Rechecked `bigmap/linux/memory_budget.h::TerrainTarget` and
`bigmap/linux/terrain_pager.h::Policy`: the native worker samples
`MemAvailable` once per second and uses resident-based physical RAM headroom.
It has no free-commit predicate or commit-tight throttle. Its reserve remains
RAM/7 bounded to 2..12 GiB, and its resident cap RAM/4 bounded to 4..8 GiB.
The explicit `terrain_cache_hot_mb` setting selects a fixed budget.

Retain the native adaptation established in [dev b6d73041](DEV_B6D73041.md).
Windows free commit is not Linux MemAvailable, and MADV_DONTNEED does not
establish an equivalent release of Linux committed address space. Applying
3 GiB to the native physical reserve would change a different policy.
`commit_tight_mb` therefore remains Windows-only, as the native cfg states.

No game address, byte pattern, structure layout or calling convention changes;
no new engine contract requires static disassembly or live debugger proof.
Existing native pager regressions cover 16/32/94 GiB policies, unknown RAM,
failed availability reads, pressure shortfalls and reclaim accounting. These
are fixture results, not game measurements. The read-only game verifier is
run as part of this integration; see its integration record for results.
