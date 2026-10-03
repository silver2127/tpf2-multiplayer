# Windows dev 7e3d3bfa integration

## Merged

Target `7e3d3bfac53308c611948118b7bfa4673983e648`, one Windows commit,
onto Linux parent `2bd1bee3f3116491b5fc14bb9b406799e728d3d9`.
No conflicts. Both incoming files are retained byte-for-byte. The merge stays
staged and uncommitted; release remains **0.7.1.1**.

## Ported (what and how)

Preserved Windows material layer-pointer/ID caching (up to 256 layers, then
stock fallback), row arithmetic hoisting and modulo-63 dither-column stepping.
Preserved the Windows profiler's repeatable `--map` MSVC linker-map option.
Updated exact Lua provenance and release-record packaging to this target.
Native runtime behavior remains unchanged. See
[the fresh Linux investigation](../re/linux/DEV_7E3D3BFA.md).

## Not ported

Native material-index acceleration, including this optimization, remains
unported. Static analysis found the matching selection loop inlined in Linux
worker `0xcc41f0`, called from UpdateBoxAsync LoopImpl at `0xcc5d22`. It is
not the standalone Windows nine-argument entry. Dither, layer stride and
interpolation were located; complete register/stack and job-buffer ownership,
aliasing and lifetime contracts still need proof. The lab attempt failed before
game startup, preventing live probes. Linux retains stock selection rather
than installing an unverified hook.

The MSVC-map option applies to the Windows profiler only; no native profiler
was added. Its Windows process/context APIs and DLL maps are intentionally
retained, not interpreted as ELF symbols.

## Live testing

Built and installed candidate libraries and merged Lua into backed-up native
lab copies, requested autoload and disabled lab density rewriting. The prescribed
launcher returned exit 1: `bwrap: setting up uid map: Permission denied`.
No title menu, Vulkan device, loaded save, gdb observation, screenshot or
performance result was reached. No Proton peer or desktop input was used.
Both actor payload directories were restored and `diff -qr` matched their
backups. No save changed and no game remains running. Steam and the user's
installed game/mod were untouched. Evidence and copied actor logs/data are in
the job's `meta/live/`; snapshots include historical logs.

## Tests

- `tools/linux/build_native.sh`: PASS, soldier build, 78/78 CTests (62.89 s),
  glibc baseline checks passed. No native material-index test exists; no native
  algorithm or hook was introduced, so no synthetic hook-parity claim is made.
- `python3 tools/linux/verify_lua_release.py`: PASS, 31 exact Lua files,
  zero exceptions, 32 HUD glyphs and two toolbar textures. Manifest
  `1de350a7196751cdbdc1e50bf3b4bb29fe72b547e3a109bf4ec813b84d949673`.
- `python3 bigmap/tools/linux/verify_game.py GAME`: PASS, actual ELF build-id
  and all 39 existing guarded patch sites.
- Release shell syntax, profiler Python syntax, upstream file equality,
  merge target and empty unmerged index: PASS.
- Windows compilation, live stock/optimized output comparison and native
  performance measurement: not run. No release package built or published.

Build/Lua/ELF logs: `.git/port-7e3d3bfa-{build,lua,elf}.log`; copies are
retained in the job's `meta/live/`. Integration status: **PARTIAL**.
