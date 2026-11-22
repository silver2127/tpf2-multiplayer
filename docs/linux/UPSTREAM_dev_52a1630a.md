# Upstream dev 52a1630a integration

Incoming: `52a1630a9dc5780ff146b7ebc144544b8a694c69`.
Baseline: `c5ffa88`, following [84058da7](UPSTREAM_dev_84058da7.md).
Release remains 0.7.0.7. Merge remains staged and uncommitted.

## Merged

One commit fixes the native dedicated Vulkan descriptor reset slot and adds
per-function diagnostics. It changes no Windows production code or shared Lua.
Resolved the conflict in `overlay_vk_linux.cpp`, retaining the earlier Linux
byte guard and adding upstream's named reset-slot pointer validation.
Lua verifier and release provenance now identify this target.

## Ported

Reset must match a fresh gdpa lookup at the verified +0xb28 slot, beyond the
QueueSubmit +0xae0 boundary. Retained null/duplicate-pointer rejection and the
40-byte ELF guard; any failure leaves all descriptor calls unchanged and names
the failed function. Removed the merged duplicate constant and made the final
failure diagnostic precise about descriptor calls. Added misplaced reset-pointer
coverage to the existing overlay test. See [RE evidence](../re/linux/DEV_52A1630A.md).

## Not ported

None from this commit. Live activation and performance validation are outstanding;
no unrelated native feature gap is changed by this integration.

## Live testing

Installed candidate libraries and merged Lua into backed-up native lab payloads.
Requested private dedicated mode with rendering off, recycling on and New Game
autoload. Lab startup failed before game execution: `bwrap: setting up uid map:
Permission denied` (exit 1). No Vulkan device, menu, loaded world, recycler
activity or gdb observation was obtained. Restored both payload directories;
recursive comparisons matched. No game was left running and Steam was untouched.
Evidence is in job `meta/live/`; archived actor logs/data predate this failed run.

## Tests

- `tools/linux/build_native.sh`: soldier build, 73/73 CTests and glibc <=2.31 checks pass.
- `python3 tools/linux/verify_lua_release.py`: 29 exact Lua files, zero exceptions,
  32 exact HUD textures; manifest
  `1d049a9505352d73d6d52b3c6a543301960faba89f93c433921f8c8867ff9d01`.
- `tools/linux/verify_descriptor_dispatch_elf.py <lab ELF>`: build-id, init call,
  reset guard and five named dispatcher stores pass.
- Release shell syntax and staged whitespace checks.

Detailed results are recorded in the job report and `.git/port-52a1630/`.
