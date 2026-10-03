# Pager load-tail policy — dev d8c98ccb

Windows target: `d8c98ccb4e797dfc2cc0bf24d2223cda6dcae256`.
Linux ELF: Steam 35924, GNU build-id
`3a0e156390b0e6f1e372051c24802c8493ae454a`.

## Source comparison and decision

Windows `pager_impl.inl::Tick` (instantiated for terrain and material) and
`small_pager.h::Tick` now treat `throttle` as loading even when the last
allocation burst was more than 15 seconds ago. This bypasses steady-state
minimum age/second-chance delays so a fault waiting for budget can progress
before its two-second timeout. The Windows implementation and regression
are retained byte-for-byte from upstream.

Native `bigmap/linux/terrain_pager.h` does not compile either Windows worker.
`TerrainPager::Allocate` installs real zeroed pages with UFFDIO_COPY and
increments residency without a budget wait. `Serve` decodes a cold tile,
copies it into its original address, updates recency and wakes the fault;
it never reads `budget`. `Policy` independently evicts eligible tiles when
resident bytes exceed budget, scanning every 250 ms with at most 256 encoding
attempts per iteration. New/restored tiles have two seconds of protection,
extended to ten seconds on rapid refault. There is no burst timestamp,
loading/steady branch, soft-block delay, commit throttle or waiting-budget
condition to update. Preserving this policy is the native equivalent for
this bug fix: faults already progress without waiting for reclamation.
This does not promise comparable load throughput or Windows memory control.

Native material/small pools and the Windows commit-throttle controller are
inherited omissions, described in [DEV_8C3C02A5.md](DEV_8C3C02A5.md) and
[DEV_2B8505C7.md](DEV_2B8505C7.md). This commit changes a condition inside
those Windows-only workers; it does not introduce pools or require inventing
a native controller. No native runtime change is warranted.

## Evidence and limits

The existing pager fixture now times 32 allocations with a zero budget,
waits for their eviction, and restores two cold tiles sequentially while the
budget remains zero. Both operations must complete within one second, below
the Windows two-second timeout; the second restore cannot require eviction
of the first tile, which remains recency-protected. Full tile contents are
checked, alongside the existing parallel restore, writer race and reuse tests.
Timing assertions are regression ceilings, not gameplay performance claims.
The UFFD-specific checks skip with status 77 if the kernel denies userfaultfd.

No hook, address, instruction bytes, struct offset, SysV ABI or engine
ownership contract changes. The read-only verifier confirms the actual lab
ELF's build-id and all 39 existing patch sites. No new disassembly or live
gdb contract proof is required. No game was run for this source-policy audit;
no load-time improvement is claimed. See the integration record for test results.
