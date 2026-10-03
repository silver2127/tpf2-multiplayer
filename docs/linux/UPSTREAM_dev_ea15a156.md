# Windows dev ea15a156 integration: native guarded-read batching

## Merged

Target `ea15a156a184bc2eebf0b22475965c05a6eba815`, Linux parent
`a5b8cc7083c98484d3248b3ab2d2af47ad5f549c`. One incoming commit:
`ea15a15` (linux slice: guarded reads stop probing whole vectors a page per
syscall). No conflicts. Merge staged and uncommitted; version remains 0.7.0.7.

## Ported (what and how)

Despite arriving through the Windows dev branch, all five incoming files
are already native Linux implementation/tests. Retain the four production
files exactly as supplied:

- Batch up to 256 page probes per process_vm_readv call, accepting only a
  full count and retrying EINTR; retain the pipe fallback.
- Split vector shape validation from full payload readability. RoadEdgeData
  validates the two world-vector headers and reads only the selected index
  and group data-vector header through guarded reads.
- Copy an entity's Name slot pairs once into bounded thread-local storage,
  then scan the copy instead of issuing a syscall for each pair.

Extend upstream's memory tests to both initialized read mechanisms, exact
batch lengths and boundary holes, selected versus unrelated unreadable road
vector elements, the Name scratch capacity and a partial slot-pair read.
Advance the Lua verifier/package provenance to this commit, package this
record and link it from README and INSTALL.

No new game addresses, byte patterns, offsets, SysV ABI or lifetime contracts
are introduced. Existing movement and train-order verifiers confirm their
unchanged sites against the actual lab ELF. Guarded-read behavior and its
limits are recorded in [SLICE_CORE.md](../re/linux/SLICE_CORE.md#guarded-read-batching--dev-ea15a156-2026-09-27).
Windows production paths and shared Lua are unchanged.

## Not ported

None from this commit. Existing feature gaps and live gameplay validation
limits are unchanged. The upstream 89% simulation-thread figure and claimed
syscall reduction are upstream observations, not measurements made here.

## Live testing

No game launched, debugger attached, or desktop input sent. These changes
introduce no new engine contract needing live RE. Validation used synthetic
memory mappings with real guarded-read syscalls in the soldier test process,
and read-only verification of the lab game ELF. No in-game timing, loaded-save
or cross-platform gameplay result is claimed. Lab actors, Steam, installed
mods and saves were not modified; no backup/restoration was needed.

## Tests

- `tools/linux/build_native.sh`: PASS, pinned soldier SDK, 72/72 CTests
  (52.79 seconds), native libraries built and glibc <=2.31 checks passed.
- `python3 tools/linux/verify_lua_release.py`: PASS, 29 exact Lua files,
  zero exceptions and 32 exact HUD glyphs. Manifest SHA-256:
  `1d049a9505352d73d6d52b3c6a543301960faba89f93c433921f8c8867ff9d01`.
- `python3 tools/linux/verify_movement_elf.py <lab ELF>`: PASS, build-id,
  11 movement byte spans, Add call, menu prologue, stolen boundaries,
  interior-branch checks and filtered sums.
- `python3 tools/linux/verify_train_order_elf.py <lab ELF>`: PASS, build-id,
  16 runtime byte checks, shuffle/seed/type calls, Name RTTI, CALL boundary
  and Update2 branches.
- `bash -n tools/linux/build_release.sh`, working/staged whitespace checks,
  empty unmerged index and retained MERGE_HEAD: PASS.

Build/test logs are retained in this clone under `.git/port-ea15a156-*.log`.
