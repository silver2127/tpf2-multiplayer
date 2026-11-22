# Windows dev 7bace802 integration

## Merged

One Windows commit, `7bace8023633fd38c2d52f346cf06e5754ab85fc`, onto Linux
parent `b136382fa0795aab00d3e933c781b091152a571f` on `port/dev`:
`docs: load-speed findings of 2026-09-28 (bigger material-index chunks change the output: rejected)`.
No conflicts. `bigmap/docs/load-speed-todo.md` remains byte-identical to
upstream. The merge is staged and uncommitted; release remains **0.7.1.1**.

## Ported (what and how)

Retain the September 28 Windows profiling findings as shared documentation.
They cover sidecar lookup/parallel decoding, material-index caching, an unbuilt
material-index sidecar, rejected larger work chunks, previous-world teardown
and serializer waiting. The 32x32 experiment changed 8,164 of 36,992 tile
outputs; preserve the upstream instruction not to change chunk size.
These timings, hashes, CPU counts and PE addresses are Windows evidence,
not native Linux measurements or addresses.

No runtime code, hooks, ABI, offsets, byte guards or settings change, so no
new static or live reverse engineering is needed. Reviewed the existing Linux
material-index investigation (DEV_7E3D3BFA) and profiler scope (DEV_A2E47F2C)
in docs/re/linux. Document the scope in Big Maps' Linux PORT record, link this
integration from README and INSTALL, and package it with Linux releases.
Keep the Lua verifier and BUILDINFO baseline at 65302e5d: its mod tree is
identical to the incoming target. No test additions are needed for this
documentation-only change and package-list entry.

## Not ported

None from this commit. The rejected experiment and explicitly unbuilt ideas
are findings, not implementations to port. Earlier native material-index
acceleration/probe omissions and sidecar live-validation gaps remain as
recorded in their integrations; this merge neither resolves nor expands them.
Native sidecars remain experimental and default off.

## Live testing

No game launched, debugger attached, desktop input sent or lab payload
installed. No new native timing, tile-hash, rendering or cross-platform result
is claimed. Lab actors, saves, Steam and the user's installed mod were
untouched; no backup/restoration was needed and no game was started.

## Tests

- `tools/linux/build_native.sh`: PASS with the pinned soldier SDK, **78/78
  CTests** in 62.77 seconds; glibc <=2.31 compatibility checks passed.
- `python3 tools/linux/verify_lua_release.py`: PASS, 31 exact Lua files,
  zero exceptions, 32 HUD glyphs and two toolbar textures. Manifest SHA-256:
  `1de350a7196751cdbdc1e50bf3b4bb29fe72b547e3a109bf4ec813b84d949673`.
- Incoming documentation byte comparison against 7bace802: PASS. Incoming
  mod tree equality with the retained 65302e5d baseline: PASS.
- `bash -n tools/linux/build_release.sh`, working/staged whitespace checks,
  empty unmerged index and retained target MERGE_HEAD: PASS.
- Windows builds, release packaging and live profiling were not run.

Logs: `.git/port-7bace802-build.log`, `.git/port-7bace802-lua.log` and
`native/linux/out-soldier/Testing/Temporary/LastTest.log` in this clone.
