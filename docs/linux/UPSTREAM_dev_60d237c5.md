# Upstream dev 60d237c5 integration: autosave terrain sidecars

Windows target: `60d237c57ce4abcfe03e05fb91db8126abe7bf07`.
Linux parent: `8ef7f7a348e3986d0e30145503341ba2d562f8fb`, following
[8c3c02a5](UPSTREAM_dev_8c3c02a5.md). One Windows commit; no conflicts.
The merge is staged and uncommitted. Result: **PARTIAL**.

## Merged

Retain all three upstream files unchanged: `bigmap/src/terrain_sidecar_io.h`,
`bigmap/tools/test_sidecar_io.py`, and `bigmap/docs/terrain-sidecar.md`.
When the expected save did not change, the Windows sidecar now goes beside
exactly one recent `.sav` in that directory; multiple candidates discard the
capture. The scan allows two seconds before call start. It adds diagnostics
and tests for the autosave filename, no candidate and ambiguous candidates.
Upstream explicitly keeps sidecars out of join transfers.

## Ported (what and how)

Preserve the existing Linux native implementation and Windows code paths.
Link this partial integration from README/install documentation and include
its record in release packages. Lua and HUD glyphs did not change; retain the
8c3c02a5 verifier/BUILDINFO Lua baseline and existing Linux origin-replay patch.
No release version or new executable patch is introduced.

## Not ported

The native autosave-sidecar correction cannot operate because the existing
native Big Maps port has no terrain sidecar capture or restore implementation.
A fresh static investigation located the Linux serializer, backend resolver
and autosave naming route. The real lab launch failed before a process could
be probed. [The RE record](../re/linux/DEV_60D237C5.md) gives exact sites,
disassembly evidence, attempted live work and missing contracts. No Windows
addresses, MSVC layouts or speculative native lifetime assumptions were used.
This remains an inherited feature gap, now also covering this fix.

## Live testing

Installed the newly built native libraries, Big Maps plugin and merged Lua into
backed-up lab directories, then attempted the prescribed native launcher.
It exited 1 with `bwrap: setting up uid map: Permission denied`. No title menu,
GPU selection, loaded world, save, gdb probe or sidecar behavior was observed.
Both actor directories were restored and compared equal with `diff -qr`.
No saves changed, no game remains running, and Steam was untouched.
The job's `meta/live/` holds launch/restoration output and archived actor state.

## Tests

- `tools/linux/build_native.sh`: soldier SDK build, **62/62 CTests passed**,
  glibc compatibility checks passed; includes all four existing Big Maps suites.
- `python3 tools/linux/verify_lua_release.py`: **29 Lua files** (one pinned
  Linux integration) and **32 HUD glyphs** passed. Manifest SHA-256:
  `fb3b58aee11225608e15d1f7d151e4562a5ac03c531f289e3eaa290dbbae7755`.
- `python3 bigmap/tools/linux/verify_game.py <lab-native-ELF>`:
  build-id and **20/20 guarded sites passed**.
- `bash -n tools/linux/build_release.sh`, staged whitespace checks and an
  empty unmerged index passed. Three merged upstream files remain byte-exact.
- Windows DLL-only `test_sidecar_io.py` and `test_terrain_sidecar` were not run.
  No Linux sidecar test exists; no native sidecar implementation was added.
  Build and Lua logs are `.git/port-60d237c-build.log` and
  `.git/port-60d237c-lua.log`, also copied to the job's `meta/live/`.
