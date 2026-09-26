# Windows dev 522a303b integration

## Merged

Windows target `522a303b53af90bcdf1565771e5badcce7d26bfa` onto Linux
parent `ab421796f979970ed236ae8bc283f4023beb6268`. One incoming commit:
“PERF: report the dearest hash's split, and what the stamp costs after the hash”.
No conflicts. Merge staged and uncommitted; release version remains 0.7.

## Ported (what and how)

Retain upstream lockstep.lua byte for byte. The PERF window now retains the
lane breakdown from its longest hash instead of the last hash. Published
stamps additionally measure the LSHASH broadcast, drift-position shipment and
comparison, reporting their average and maximum separately. Reporting resets
the new accumulators alongside the existing update/hash counters. Load-time
samples that are not published still contribute to hash timing, but not to
post-hash timing. Cadence and simulation behavior remain upstream's.

Linux runs this shared Lua; no platform-specific implementation is missing.
No hooks, executable addresses, struct offsets, patch bytes or ABI contracts
change, so new static/live reverse engineering is unnecessary. Existing engine
evidence remains in docs/re/linux/; this change adds no new engine claims.

Advance the Lua verifier and release BUILDINFO baseline to this commit while
preserving the pinned Linux origin-replay exception in inject.lua. Package
this record and link it from README and Linux installation documentation.
Extend the existing sample-time test with controlled expensive/cheap hash
costs, unpublished sample handling, post-hash counts/sum/max, actual PERF text,
and reset/next-window assertions. Production Lua is executed under Lua 5.2.

## Not ported

None from this commit. Earlier native limitations remain in their integration
records; this change does not resolve or revalidate them.

## Live testing

No game launched or gdb attached. This shared Lua diagnostic change was tested
offline with mocked clocks and engine operations. No actual game performance,
rendering or cross-platform gameplay result is claimed. Lab actors, libraries,
mods, saves and Steam were untouched; no backup/restore was necessary.

## Tests

- `tools/linux/build_native.sh`: soldier build, 65/65 CTests and glibc baseline
  checks passed.
- `python3 tools/linux/verify_lua_release.py`: 29 Lua files (one existing pinned
  Linux exception) and 32 HUD textures passed. Manifest SHA-256:
  `b67ff99a9d55f248af9ea23290c96bbf0904cd83b533338bcf48d16878023aaf`.
- `tools/hash_sample_time_test.py`, `tools/hash_cadence_test.py` and
  `tools/hash_bigmap_test.py`: passed with clone-local `.git/port-venv/bin/python`
  (Lupa 2.8, Lua 5.2). No system packages installed.
- Mutation check: the extended sample-time test rejects pre-change lockstep.lua
  read from HEAD at its slowest-split assertion; runtime files were not altered.
- All 29 mod Lua files pass `luac5.2 -p`; release script passes `bash -n`.
- Upstream lockstep.lua byte equality, CRLF-aware staged whitespace check,
  empty unmerged index and retained MERGE_HEAD verified. No release package built.

Logs: `.git/port-522a303-{build,lua,sample,cadence,bigmap,mutation,deps}.log`.
No commit, merge abort, publication or live installation was performed.
