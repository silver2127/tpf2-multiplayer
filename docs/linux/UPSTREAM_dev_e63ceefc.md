# Upstream dev e63ceefc integration: server performance report

Windows target: `e63ceefcc3d9d0c4d7373531fe8028ecd96260a9`.
Linux parent: `dbb83bae1eef79894692cc465fe09bb3a9d91110`.
One incoming commit; no conflicts. Merge staged and uncommitted; version stays 0.7.

## Merged

Retain the 21-line addition to `docs/DEDICATED_SERVER.md` unchanged. It records
upstream's server measurements for native build `b1ac39f`: nominal 200 ms
batches, lower simulation CPU consumption, remaining lavapipe mapping churn
and memory usage. These are upstream observations, not measurements from this job.

## Ported (what and how)

Shared documentation needs no native counterpart. No Windows or Linux source,
Lua, hook address, patch bytes, struct offset or ABI changes in this commit;
no new static or live reverse engineering is required. Package this integration
record and link it from README. Keep the existing Lua verifier baseline.

The report's `process_vm_readv` attribution should not be read as a complete
description of the current family guard. In this tree,
`native/linux/src/family_canon_linux.inl` uses `PROCMAP_QUERY` with a buffered
mapping snapshot fallback, as recorded in [dev 7cacbaaf](UPSTREAM_dev_7cacbaaf.md)
and its [RE evidence](../re/linux/DEV_7CACBAAF.md).
`native/linux/src/slice/slice_core.cpp` separately uses guarded reads through
`process_vm_readv`/`RawRead`. This documentation-only integration does not alter
either implementation or independently establish the server profile's attribution.

## Not ported

None for this commit. The remaining lavapipe cost is a profiling observation,
not an incoming implementation to port. Earlier integration limitations remain.

## Live testing

No game launched, gdb attached or lab payload installed. No new runtime contract
needs probing for this documentation change. No local performance, rendering,
loaded-world or cross-platform gameplay result is claimed. Lab actors, saves,
the installed mod and Steam were untouched.

## Tests

- `tools/linux/build_native.sh`: soldier SDK build, 65/65 CTests and glibc
  baseline checks passed. Existing compiler warnings did not fail the build.
- `python3 tools/linux/verify_lua_release.py`: 29 Lua files and 32 HUD textures
  passed; manifest SHA-256
  `cbcd41b781d05d4f475926a898d5cd1cf44c637207e20d962279cc4f43863885`.
- `bash -n tools/linux/build_release.sh`: passed. No release package built.
- Upstream server document byte equality, staged whitespace checks and empty
  unmerged index verified; MERGE_HEAD retained.
- No new tests added: runtime behavior is unchanged.

Logs: `.git/port-e63ceef-build.log` and `.git/port-e63ceef-lua.log` in this clone.
