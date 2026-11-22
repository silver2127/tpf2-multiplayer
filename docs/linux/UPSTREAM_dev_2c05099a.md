# Windows dev 2c05099a integration

Merged `2c05099a8d2b9b3a6849a88770ab4924e419068b` into `port/dev`, based on
`linux-native`. No conflicts. The merge remains staged and uncommitted;
version stays 0.7. Windows code is unchanged.

## Merged

Retained the cross-platform RNG investigation and supplied native patch verbatim.
Applied that patch to the Linux implementation instead of leaving it as inert
documentation.

## Ported

- Eight simulation seed sites now use Windows FNV-1a/hash-combine behavior.
- MSVC integer distributions and forward shuffles replace the selected GNU
  implementations; town-building and town-name engines use MT19937, with native
  calling conventions and engine storage respected.
- AirConnectParts uses Windows float conversion order; two endpoint clamps
  are removed to match Windows.
- Both boot modules default on. `TPF2MP_SIM_SEED=0` and
  `TPF2MP_ENGINE_PARITY=0` disable them for diagnosis; simulation settings must
  match peers.
- Fixed an upstream rollback gap: include the failing hook itself when its
  write may already be visible. Added eleven partial-write failure cases.
- Added a reproducible actual-ELF verifier for all new guards, stolen instruction
  boundaries, interior branch checks and constants.

[RE evidence and ABI contracts](../re/linux/DEV_2C05099A.md) distinguish the
upstream emulation results from checks repeated in this integration.

## Not ported

None of the supplied implementation is omitted. Upstream explicitly excludes
other map/UI/audio paths, two terrain endpoint copies, StreetGenerator and CRT
rand investigations; they are not implementations introduced by this commit.
Cross-platform gameplay parity remains unproved, especially callback semantic
identity, transformator lifetime/reentrancy and draw branch order.

## Live testing

Backed up native lab libraries, mod and userdata as `.before-port-2c05099a`,
installed this build and merged Lua, then invoked the required lab launcher.
It failed before starting the game: `bwrap: setting up uid map: Permission denied`.
No title menu, renderer, gdb probe or simulation result exists for this run.
The proposed native/Proton soak and disabled-switch control could not proceed.
Restored all three trees and checked SHA-256/symlink inventories. Left no game
running and did not change Steam or host sandbox policy. Job evidence is under
`meta/live/`, including launch failure and restoration inventories.

## Tests

- `tools/linux/build_native.sh`: final soldier build, 65/65 CTests, glibc baseline passed.
- `python3 tools/linux/verify_lua_release.py`: 29 Lua files and 32 HUD textures passed.
- `verify_random_parity_elf.py` on the actual lab ELF: 28 guarded ranges,
  19 stolen ranges, constants, clamps and enclosing-function branch checks passed.
- New upstream `sim_seed` and `engine_parity` tests retain MSVC oracle vectors,
  executable detours, wrong-build/tampered-context refusal and float edge cases;
  added partial-write rollback coverage passes.
