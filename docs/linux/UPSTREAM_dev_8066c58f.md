# Windows dev 8066c58f integration

## Merged

One Windows commit, `8066c58f6911585a3e7ba908e6c11281f92840c8`, onto Linux
parent `b531a2c40579cbd924db66b6058ff8d1e96a550f`. No conflicts. Both incoming
Windows files are retained byte-for-byte; merge stays staged and uncommitted.
Release remains **0.7.1.1**.

## Ported (what and how)

Retained the default-off Windows material-index measurement probe. With the
optimized path active, completed tiles report coordinates, FNV hash, run-length
size and order-0 entropy to `material_probe.txt`. This does not implement a
material sidecar. Updated native package provenance, Lua verification reference
and integration documentation. Native runtime behavior remains unchanged.

## Not ported

Native `material_index_probe` remains unported, as does its prerequisite
material-index fast path. Fresh static investigation traced the inlined Linux
selection worker and an alternative FinishBox return-vector boundary; neither
has a proved capture contract for raw tile bytes, coordinates, completion and
buffer lifetime. The lab failed before game startup, preventing live proof.
See [the evidence and resume points](../re/linux/DEV_8066C58F.md).
No unverified hook or ineffective native config switch was added.

## Live testing

Installed the candidate soldier libraries, plugin and Lua in backed-up native
lab copies; requested autoload and disabled density rewriting. The prescribed
launcher exited 1 with `bwrap: setting up uid map: Permission denied`.
No menu, Vulkan device, loaded save, gdb observation or measurement was reached.
No Proton or desktop input was used. Both payloads were restored and matched
backups with `diff -qr`; no save changed and no game remains running.
Steam and the user's installed game/mod were untouched.

## Tests

- `tools/linux/build_native.sh`: PASS, soldier build, 78/78 CTests (62.76 s),
  glibc 2.31 baseline checks passed.
- `python3 tools/linux/verify_lua_release.py`: PASS; 31 exact Lua files,
  zero exceptions, 32 HUD glyphs, two toolbar textures. Manifest
  `1de350a7196751cdbdc1e50bf3b4bb29fe72b547e3a109bf4ec813b84d949673`.
- `python3 bigmap/tools/linux/verify_game.py GAME`: PASS, build-id and all
  39 existing guarded patch sites.
- Release shell syntax, exact upstream Windows file equality and empty unmerged
  index: PASS. Diff whitespace check reports the incoming CRLF line in
  `bigmap.cpp`; its upstream bytes are intentionally preserved.
- No native material probe implementation or test exists; no synthetic fixture
  is presented as proof of an engine hook. Windows compilation and actual
  two-load probe output comparison were not run.

Logs and fresh disassembly are in the job's `meta/live/`. Status: **PARTIAL**.
