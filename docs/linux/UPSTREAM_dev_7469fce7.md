# Windows dev 7469fce7 integration

## Merged

Windows target: `7469fce7b14486f7a31306d9018a5c9c835ed62c`.
Linux parent: `a20eee5f85134cf11706bc5a819ea66b685da0ba`.
One incoming Windows first-parent merge, bringing the tpf2-bigmap PR #5
history into this repository. No conflicts; merge stays staged and uncommitted.
The target's first-parent tree diff changes only `bigmap/README.md`.
The additional commits visible in `git log -p HEAD..7469fce7` belong to the
merged standalone Big Maps history, not additional runtime changes in this tree.

Retain the upstream GOG support, fallback and installer documentation. The
Windows GOG fallback and batch deployment fix already arrived in
[aaae03f8](UPSTREAM_dev_aaae03f8.md); the separate Big Maps installer remains
deleted in favor of the unified installer. No Windows code paths change.

## Ported (what and how)

Add a platform-scope note to Big Maps README linking the native installation
instructions and [octree evidence](../../bigmap/docs/linux/PORT.md#octree-depth-1213-experimental).
The upstream 57 x 57 km GOG observation exercised the depth-11 fallback;
it does not validate deeper roots or native Linux gameplay. Native defaults
remain depth 11 / 512 tiles; native depth 12/13 retains its existing offline
validation and outstanding live checks.

The native host still accepts Steam ELF build-id
`3a0e156390b0e6f1e372051c24802c8493ae454a`, with independent byte guards.
No new address, pattern, struct offset, calling convention, lifetime contract
or native code change is introduced, so no new static/live RE is needed.
Read-only verification of all 29 existing Big Maps sites passes again.
The preceding integration's 30-case depth/ceiling matrix remains applicable.

Link this record from the root README and include it in Linux release
packaging. Release version, shared Lua and verifier baseline are unchanged.

## Not ported

None for this commit. Windows GOG PE support and MSI properties do not imply
support for another Linux ELF. Earlier native feature gaps and pending live
depth-12/13 validation remain as documented in the preceding integrations.

## Live testing

No game launched, gdb attached or actor payload installed: this integration
changes documentation and packaging of its record only. Lab actors, saves,
Steam and the user's installed mod were untouched; no restoration was needed.
No local rendering, generation, load/save or multiplayer result is claimed.
Upstream GOG gameplay observations were not repeated locally.

## Tests

- `tools/linux/build_native.sh`: soldier SDK build, 66/66 CTests and glibc
  <=2.31 compatibility checks passed.
- `python3 tools/linux/verify_lua_release.py`: 29 Lua files (one pinned native
  exception) and 32 HUD glyphs passed. Manifest SHA-256:
  `b67ff99a9d55f248af9ea23290c96bbf0904cd83b533338bcf48d16878023aaf`.
- `python3 bigmap/tools/linux/verify_game.py <lab-native-ELF>`: build-id and
  all 29 patch sites passed, read-only.
- `bash -n tools/linux/build_release.sh`, staged whitespace checks, preservation
  of the upstream README beneath the native note, and empty unmerged index
  passed. MERGE_HEAD retained. No release package built.
- No new tests added for this documentation-only change; no Windows executable
  or MSI execution tests run on this Linux host.

Logs are retained in `.git/port-7469fce7-{build,lua,elf}.log` in this clone.
