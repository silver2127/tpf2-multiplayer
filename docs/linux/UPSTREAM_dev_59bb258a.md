# Upstream dev 59bb258a integration

Windows target: `59bb258ab650db44c4c6dddca4fa80e37cb6a360`.
Baseline: [d3135a59](UPSTREAM_dev_d3135a59.md), native Linux build 35924.
One Windows commit, no merge conflicts. Merge remains staged and uncommitted;
`installer/VERSION` is unchanged.

## Merged

The Windows `IconTintForEntity` change is preserved exactly: own-company
vehicles and station labels now receive company colours too. Unowned entities
and co-op remain untinted; window wash remains foreign-only.

No shared Lua or lobby code changed. All 28 Lua files exactly match this target;
manifest SHA-256 remains
`760005868a5c7b055500cf9c31820854dec28365d7e041f145b2cc7c43ab7e70`.
The Lua verifier, README, installation guide and release BUILDINFO now name
this revision; release packaging includes this integration record.

## Ported

Integration provenance and packaging updated. No native runtime change:
the underlying Linux tint hooks were already unavailable, so there is no
native own-company colour predicate to adjust.

## Not ported

Own-company vehicle-icon and station-label tinting remains unavailable along
with the earlier foreign-company tints. Fresh static RE established the label
helper's aligned 16-byte colour read and contrasted its 32-byte vertices with
the vehicle candidate's 16-byte vertices. It did not establish a compatible
vehicle colour path or a complete safe station relay/component lifetime.
[Exact attempts, bytes, ABI observations and missing evidence](../re/linux/DEV_59BB258A.md).
Native tinting remains disabled; existing visibility and permissions work is
retained. Earlier window wash, recovery, ownership and Workshop gaps remain.

## Tests

- `tools/linux/build_native.sh`: pinned soldier SDK build, **44/44 CTests**
  and glibc <=2.31 compatibility checks pass. Log: `.git/port-59bb258-build.log`.
- `python3 tools/linux/verify_lua_release.py`: 28 exact files, manifest above.
- `python3 tools/linux/verify_dev_d3135a59_elf.py GAME`: build-id, ten guard
  spans, five branch edits, instruction boundaries, RTTI and vtable pass.
- `bash -n tools/linux/build_release.sh`: pass.
- Windows source equality, staged whitespace and merge-state checks: pass.

No new runtime code was introduced, so no synthetic runtime test was added.
Existing native tests cover the retained UI behavior. Steam and Transport
Fever 2 were never started; no installation, publication or live gameplay test.
