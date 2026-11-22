# Windows dev 02edb897 integration: descriptor-recycler measurements

## Merged

Target `02edb8975ab310ea50df5a129b1a0173839fe2c5`, Linux parent
`3e310c4fcd8235e563c5c0479f7442b60ecc7dc6`. One incoming commit:
`docs: what the descriptor-set recycler measured on the dedicated server`.
No conflicts. Merge staged and uncommitted; version remains 0.7.0.7.

Retain the 15-line addition to `docs/DEDICATED_SERVER.md` unchanged. It records
upstream's settled-world native dedicated-server measurements: 97% descriptor
set reuse, reduced mmap/munmap counts and main-thread CPU, and remaining
frame-preparation costs. These are upstream observations, not this job's
measurements. The camera suggestion remains future work.

## Ported (what and how)

Shared documentation needs no native counterpart. The implementation already
exists through [84058da7](UPSTREAM_dev_84058da7.md) and
[52a1630a](UPSTREAM_dev_52a1630a.md): dedicated render suppression gates the
default-on recycler, with per-layout spare sets, real resets every 600th reset
per pool, and guarded dispatcher-slot validation. Existing static evidence and
local live-validation limits are in [the RE record](../re/linux/DEV_52A1630A.md).

No source, hook, address, byte pattern, ABI, layout or lifetime contract changes
in this commit; no new static or live RE is required. Re-run the existing ELF
verifier as a read-only check. Add this integration record and a README link.
Retain the `52a1630a` Lua verifier baseline and release provenance: the incoming
mod tree is unchanged from that baseline. Windows paths remain intact.

## Not ported

None from this commit. Remaining frame-preparation costs and the camera idea
are observations and future work, not an incoming implementation. Earlier
native feature gaps and local live-validation limits remain unchanged.

## Live testing

No game launched, debugger attached, desktop input sent or lab payload
installed. No local recycler activation, performance, rendering or
cross-platform result is claimed. Steam, lab actors, installed mods and saves
were untouched; no backup or restoration was needed.

## Tests

- `tools/linux/build_native.sh`: PASS, pinned soldier SDK build, 73/73
  CTests (53.08 seconds) and glibc <=2.31 compatibility checks.
- `python3 tools/linux/verify_lua_release.py`: PASS, 29 exact Lua files,
  zero exceptions and 32 exact HUD glyph textures. Manifest SHA-256:
  `1d049a9505352d73d6d52b3c6a543301960faba89f93c433921f8c8867ff9d01`.
- `python3 tools/linux/verify_descriptor_dispatch_elf.py <lab ELF>`: PASS,
  build-id, init call, 40-byte guard and all five named dispatcher stores.
- Incoming dedicated-server document byte-identical to target, and upstream
  mod tree unchanged from the verifier baseline: PASS.
- Working/staged whitespace checks, empty unmerged index and retained
  MERGE_HEAD: PASS.
- No new tests added: this integration changes documentation only.

Logs in this clone: `.git/port-02edb897-build.log`,
`.git/port-02edb897-lua.log` and `.git/port-02edb897-elf.log`.
