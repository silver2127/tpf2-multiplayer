# Upstream dev 84058da7 integration: native descriptor recycling

Incoming commit: `84058da77373f7510eae7c16c0e5d9607848241f`.
Linux baseline: `22356f9`, following [491716c3](UPSTREAM_dev_491716c3.md).
The merge has no conflicts and remains staged, uncommitted. Release stays 0.7.0.7.

## Merged

Although received from Windows `dev`, this commit changes native Linux code:
settings, Vulkan wrappers, the descriptor recycler, CMake tests and shared
configuration documentation. Windows production paths are untouched.
The Lua verifier and release BUILDINFO now name this incoming target.

## Ported

Retained the native per-layout spare lists, periodic real resets, untracked
allocation fallback, layout invalidation and driver-error handling. Recycling
requires dedicated mode, successful render suppression, the default-on
`dedicated_recycle_sets` setting, and five unique driver function pointers.
`dedicated_recycle_sets=0` disables it; ordinary rendering remains unaffected.

Fixed the upstream dispatcher search boundary: `vkResetDescriptorPool` lives
at `+0xb28`, beyond the old `+0xae0` bound. A 40-byte runtime guard verifies
the actual reset lookup/store before permitting the longer scan. Any mismatch
leaves descriptor calls with the driver. No address was inferred from Windows.
See [ELF evidence and ABI](../re/linux/DEV_84058DA7.md).

Added settings default/disable/invalid/re-enable tests and dispatcher boundary,
null/duplicate pointer and guard-mutation coverage. Retained the incoming
fake-driver recycler tests.

## Not ported

None from this commit. Existing unrelated native feature gaps remain. Live
activation, loaded-world stability and performance validation remain outstanding.

## Live testing

Backed up native actor libraries/data and Lua mod to `.before-port-84058da`
copies, installed the candidate libraries and merged mod, and requested private
dedicated mode with rendering off, recycling on and `New Game` autoload.
The prescribed lab command failed before game execution with
`bwrap: setting up uid map: Permission denied`. No Vulkan device, title menu,
loaded world, descriptor heartbeat or performance measurement was observed.
No gdb attachment was possible. No desktop input or Steam action was taken.
Both payload directories were restored and `diff -qr` matched the backups;
no game was left running. Job `meta/live/` contains launch/restoration evidence
and the actor logs/data snapshot (pre-existing logs are not new live evidence).

## Tests

- `tools/linux/build_native.sh`: soldier build, 73/73 CTests and glibc <=2.31 checks.
- `python3 tools/linux/verify_lua_release.py`: 29 exact Lua files, zero
  exceptions, 32 exact HUD textures; manifest
  `1d049a9505352d73d6d52b3c6a543301960faba89f93c433921f8c8867ff9d01`.
- `verify_descriptor_dispatch_elf.py <lab ELF>`: build-id, init call, runtime
  guard and all five named dispatcher stores pass.
- Release shell syntax, whitespace checks and retained merge state checked.

Logs are in `.git/port-84058da/`. An intermediate test compile found a duplicate
local fixture name; it was scoped separately before the final successful build.
