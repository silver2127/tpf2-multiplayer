# Windows dev 491716c3 integration: road-entry performance report

## Merged

Target `491716c34dbd3860c004cd118330b481b22716fc`, Linux parent
`583be8f70800a2d9d9a4f08bd48a2b4d8355850e`. One incoming commit,
`docs: the dedicated server's road-entry page walk, and what fixing it measured`.
No conflicts. Merge staged and uncommitted; version remains 0.7.0.7.

Retain the 22-line addition to `docs/DEDICATED_SERVER.md` unchanged. It records
upstream's native dedicated-server observations after installing `ea15a15`
over 0.7.0.6: nominal 200 ms batches, reduced simulation CPU and guarded-read
syscalls, and remaining lavapipe mapping churn. It also explains why loading
is not a suitable measurement interval. These are upstream observations,
not measurements from this job.

## Ported (what and how)

Shared documentation requires no native counterpart. The implementation it
describes is already integrated in [dev ea15a156](UPSTREAM_dev_ea15a156.md):
256-page guarded-read batches, shape checks for the road manager's world
vectors followed by reads of the selected element, and a single guarded copy
of Name component slot pairs. See the existing
[guarded-read evidence](../re/linux/SLICE_CORE.md#guarded-read-batching--dev-ea15a156-2026-09-27).

This commit changes no source, hook, address, patch bytes, ABI, layout or
lifetime contract. No new static or live RE is needed. Add this record and
link it from README; retain the Lua verifier's `ea15a156` baseline because
the incoming commit changes no Lua or assets. Windows paths remain intact.

## Not ported

None for this commit. The remaining renderer cost is a profiling observation,
not an incoming implementation to port. Earlier feature gaps and gameplay
validation limits remain unchanged.

## Live testing

No game launched, debugger attached, desktop input sent or lab payload
installed. No local in-game timing, rendering, loaded-world or cross-platform
result is claimed. Steam, lab actors, installed mods and saves were untouched;
no backup or restoration was needed.

## Tests

- `tools/linux/build_native.sh`: PASS, pinned soldier SDK build, 72/72
  CTests (52.84 seconds) and glibc <=2.31 checks. Existing compiler warnings
  did not fail the build.
- `python3 tools/linux/verify_lua_release.py`: PASS, 29 exact Lua files,
  zero exceptions and 32 exact HUD glyphs. Manifest SHA-256:
  `1d049a9505352d73d6d52b3c6a543301960faba89f93c433921f8c8867ff9d01`.
- Dedicated-server document byte equality with the incoming target and no
  Lua/asset changes from the verifier baseline: PASS.
- Working/staged whitespace checks, empty unmerged index and retained
  MERGE_HEAD: PASS.
- No new tests added: this integration changes documentation only.

Logs: `.git/port-491716c3-build.log` and `.git/port-491716c3-lua.log` in this clone.
