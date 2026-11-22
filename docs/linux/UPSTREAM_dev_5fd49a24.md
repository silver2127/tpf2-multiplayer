# Windows dev 5fd49a24 integration: thread-local terrain lookup

## Merged

Target `5fd49a240f2c6893f7b1ca6cf97ffccd9b513fc2`, Linux parent
`57dc2da4d0c09850e7227ef99e8ccf5db4968957`. One incoming commit:
`bigmap: the sidecar's record lookup keeps a cursor per thread -- a load probed ~30,000 records per tile`.
No conflicts; merge staged and uncommitted. Release remains 0.7.1.1.

Preserve the Windows FindRecord/Probe implementation, its generation reset,
once-per-lookup probe accounting and alternating-worker regression. Preserve
`tools/re/profile_load.py`'s busiest-thread count, per-window waits and cores
busy output. This is a Windows diagnostic using Win32 thread contexts and PE
metadata; it is not invoked by the native runtime or build.

## Ported (what and how)

The incoming commit already replaces the Linux shared atomic cursor with a
thread-local grid/index hint. Complete parity by adding an atomic load
generation, advanced by the existing native LoadHook before engine loading.
Surviving workers reset even when an allocator reuses the same grid address.
Relaxed atomic access suffices for this hint; it does not publish terrain data.
Grid changes also reset the hint; misses still scan the complete grid.

Add optional per-call probe measurement without production shared counters.
Extend the native sidecar fixture with 480 records and two strictly alternating
workers, each visiting its own half. Require exactly 720 total probes, then
keep both workers alive across LoadHook and require one probe for record zero
at the same grid address. Also cover wraparound, missing entities, null tile
vectors, changing grids and empty grids. Existing fake save/load/serve tests
remain in the suite.

No new engine hooks, byte patterns, offsets or ABI contracts. Existing runtime
build/byte guards remain intact. See [static evidence](../re/linux/DEV_5FD49A24.md).
The shared Lua and assets are unchanged; keep the release verifier baseline.

## Not ported

None from this runtime change. The Windows profiler remains a Windows tool;
no native profiler replacement is needed for the cursor fix. Existing native
sidecar ownership/lifetime validation gaps remain, and `terrain_sidecar=0`
remains the default. Upstream Windows timing is not a Linux measurement.

## Live testing

No game launched, debugger attached, desktop input sent or lab payload
installed. This change adds only mod-owned lookup state and needs no new live
ABI derivation. No native load-time improvement or engine worker scheduling is
claimed. Steam, lab actors, installed mods and saves were untouched; no backup
or restoration was needed.

## Tests

- `tools/linux/build_native.sh`: PASS, pinned soldier SDK; 78/78 CTests
  (62.93 seconds), including terrain_sidecar_linux, and glibc <=2.31 checks.
- Native alternating-worker regression: exactly 720 probes for 480 records.
  Both persistent workers reset on LoadHook with an unchanged grid address.
- `python3 tools/linux/verify_lua_release.py`: PASS, 31 Lua files,
  zero pinned exceptions, 32 HUD glyphs and two toolbar textures.
  Manifest SHA-256: `1de350a7196751cdbdc1e50bf3b4bb29fe72b547e3a109bf4ec813b84d949673`.
- `python3 bigmap/tools/linux/verify_game.py <lab ELF>`: PASS, build-id
  and all 39 Linux sites. Fresh AddTile disassembly agrees with existing layout.
- Python AST parse of `tools/re/profile_load.py`: PASS. Windows profiler and
  Windows-only serving test were not executed on this Linux host.
- Working/staged whitespace checks, empty unmerged index and retained
  MERGE_HEAD: PASS.

Logs: `.git/port-5fd49a2-build.log`, `.git/port-5fd49a2-lua.log`,
`.git/port-5fd49a2-addtile.asm`, and
`native/linux/out-soldier/Testing/Temporary/LastTest.log`.
