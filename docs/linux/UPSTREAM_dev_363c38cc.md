# Upstream dev 363c38cc: synchronized terrain-sidecar release

Windows target: `363c38cca1a979597144921a27ba6cd4aeabc7be`.
Linux parent: `ffb6bf09ccec343fbc23c66873aa26e831b4e213`.
One Windows commit, no conflicts; merge staged and uncommitted. **PARTIAL**.

## Merged

Retain the upstream changes byte-for-byte in `bigmap/src/terrain_sidecar.h`,
`bigmap/src/terrain_serve.h`, and `bigmap/tools/test_terrain_serve.cpp`.
Shared locking protects file/index reads throughout decoding; exclusive
locking removes the loaded state once, freeing it outside the lock. Only the
successful releaser logs completion. Upstream adds a 200-round concurrency test.

## Ported (what and how)

Preserve the working native pager and experimental alignment batching, and
record the native scope in README/install guidance and packaged integration
notes. No native runtime change, release bump or new game patch is introduced.
Lua/HUD contents are unchanged, so their dev 616191b1 verifier baseline remains.

## Not ported

Native terrain-sidecar capture/serving was already absent, so there is no native
loaded-file state or completion callback to synchronize. Fresh static work
traced native terrain records, the alignment worker and publication calls.
The lab failed before game startup, preventing live ownership/lifetime proof.
[RE evidence and missing contracts](../re/linux/DEV_363C38CC.md) explain why
adding only a mutex or copying Windows offsets would not implement this fix.

## Live testing

Installed the built libraries/plugin and merged Lua into backed-up native lab
copies and attempted the prescribed launcher. Immediate exit 1:
`bwrap: setting up uid map: Permission denied`. No menu, GPU, save load,
gdb probe or reload behavior observed. Restored both directories and compared
them equal. No save changed, Steam untouched, no game left running.
Evidence is in this job's `meta/live/`, including launch/restoration transcripts.

## Tests

- `tools/linux/build_native.sh`: **69/69 CTests passed**, soldier build and
  glibc compatibility checks passed, including existing terrain pager and
  alignment batching tests.
- `python3 tools/linux/verify_lua_release.py`: **29 Lua files**, including one
  pinned Linux integration, and **32 HUD glyphs** passed. Manifest SHA-256:
  `d23c25e33ce4089c724211236a1344e7f710d4a6fb7c431f2d60f4a6838892e2`.
- `python3 bigmap/tools/linux/verify_game.py <lab ELF>`: build-id and **29/29
  guarded sites** passed.
- Release script syntax, staged whitespace, empty unmerged index and exact
  preservation of all three changed Windows files checked.
- Windows `test_terrain_serve.cpp` was retained but not executed (Windows APIs
  and hardcoded PE input). No native sidecar implementation/test exists; no
  runtime code was changed to warrant adding a disconnected native fixture.

Build, Lua and ELF check logs are in `meta/live/`.
