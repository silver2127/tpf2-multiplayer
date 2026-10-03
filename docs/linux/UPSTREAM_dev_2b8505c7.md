# Windows dev 2b8505c7 integration: pager pressure shortfall

## Merged

Windows target: `2b8505c78b802d09e43b3572177a6ef51cdba4fc`.
Linux parent: `9196fc0` (dev bde31323 integration).
One incoming commit, no conflicts. Merge staged and left uncommitted.
Release remains **0.7.1.1**.

Retained all three incoming files byte-for-byte: terrain and material workers
now calculate the commit shortfall from resident bytes instead of collapsing
to 256 MiB during ordinary tight-commit episodes; Windows ctypes tests cover
the helper. Loading and emergency conditions retain the Windows floor.

## Ported (what and how)

Compared both workers to the native userfaultfd terrain backend. Native
`TerrainTarget` already subtracts only the MemAvailable shortfall from current
resident bytes, with no flat pressure floor and no subtraction from a previous
target. Keep this existing platform adaptation; document it and extend the
native pager regression with the 2724 -> 1188 MiB case, repeated samples,
partial/full reclaim, reserve boundary and exhausted-resident cases.
See [native policy evidence](../re/linux/DEV_2B8505C7.md).

Advanced the exact Lua verifier baseline and package BUILDINFO provenance,
packaged this record and updated README/INSTALL integration links. Shared Lua
and version are unchanged. No new address, byte guard, SysV ABI, struct offset
or lifetime contract needs reverse engineering.

## Not ported

None newly required for the existing native backend: the terrain shortfall
behavior is already present. Windows free-commit/sticky/load policy is specific
to its controller; the native counterpart uses physical-memory headroom.
Native material/small paging and other inherited limitations remain unchanged,
as recorded in the linked RE and Big Maps port records. This change does not
introduce those missing backends or establish full Windows policy parity.

## Live testing

No game launched, debugger attached or desktop input sent. No actor libraries,
Lua, saves, Steam files or installed user mod were changed; no backup or restore
was necessary and no game process was started. The lab ELF was read only for
static byte verification. No Vulkan device, loaded-world behavior, stutter
improvement or native/Proton gameplay observation is claimed. Policy regression
inputs are synthetic, not measurements from the upstream player's map.

## Tests

- `tools/linux/build_native.sh`: PASS, soldier SDK, 75/75 CTests in 59.16 s
  and glibc <=2.31 compatibility checks. Pager test passed the new policy
  cases and real userfaultfd lossless eviction, parallel restore, write/evict
  race and reuse checks (64 evictions, 64 faults); it was not skipped.
- `python3 tools/linux/verify_lua_release.py`: PASS, 31 exact upstream Lua
  files, zero exceptions, 32 HUD glyphs and two toolbar textures. Manifest:
  `1de350a7196751cdbdc1e50bf3b4bb29fe72b547e3a109bf4ec813b84d949673`.
- `python3 bigmap/tools/linux/verify_game.py` against the actual lab ELF:
  PASS, build-id and all 30 guarded patch sites.
- `bash -n tools/linux/build_release.sh`: PASS; package itself not built.
- All three incoming files match upstream exactly; merge target verified,
  no unresolved index entries; staged whitespace check passes.
Logs: clone-local `.git/port-2b8505c7/`. No dependency or system package was
installed, and no release package was published or installed.
