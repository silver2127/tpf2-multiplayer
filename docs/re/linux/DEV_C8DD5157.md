# Threaded terrain-sidecar writer: dev c8dd5157

## Scope and static evidence

Target `c8dd515770d41a78d2422b1477c0dc7dd1cbcf5c` changes the writer to
2,048-record windows with per-worker codec scratch, atomic work allocation,
and ordered output. Positive thread requests cap at 16; automatic uses CPUs
minus one, bounded to 1..8. The Windows per-tile SEH guard runs on the encoding
thread. Linux's branch calls the codec directly: it does not provide SEH-like
fault recovery. Neither the synthetic test nor pthread locks prove that live
tile storage remains valid while workers read it.

Fresh `readelf -n` confirms lab ELF build 35924, GNU build-id
`3a0e156390b0e6f1e372051c24802c8493ae454a`. Fresh signature-export searches
and `objdump -d -Mintel` are retained in `meta/live/` beside this job's report.

- `funcsig.csv` anchors SaveGame at `0xc7ec00` through Serializer.cpp's
  signature. Entry bytes: `f3 0f 1e fa 55 48 89 e5 41 57 41 56 41 55 41 54 53`.
  The prologue reads SaveGameId via `[rbp+0x10]` (entry RSP+8), bool via
  `[rbp+0x18]`, and monitor via `[rbp+0x20]`. The six references use SysV
  RDI, RSI, RDX, RCX, R8, R9. The Windows hidden-result-pointer signature
  cannot be copied here. This confirms the earlier candidate, not its live
  world-freeze or terrain-owner contract.
- `CTerrain::GetTileCache(CVec2i) const` is signature-anchored at `0xcf5960`.
  RDI is terrain and RSI packs the two coordinates; `0xcf5978` reads
  `[rdi+0x18]` (`48 8b 47 18`). Grid origin is +0/+4, dimensions +8/+0xc,
  records +0x10. `0xcf59a2..0xcf59c5` calculates
  `records + 40 * ((y-y0)*nx + (x-x0))` and returns the record pointer.
- The CalcMinMaxHeight assertion anchors publication at `0xcf56d0`.
  `0xcf583d` (`49 8b 44 24 08`) loads record+8 as vector pointer;
  `0xcf5842` (`4c 8b 70 08`) loads end from vector+8;
  `0xcf5846` (`48 8b 18`) loads begin from vector+0.
  The following loop reads uint16 samples. These reads support the shared
  grid view, but do not identify which CTerrain instance a save must capture.

No research address is added to production code; no new hook is enabled.
The existing verifier checks the build-id and all 30 guarded Linux sites.
The inherited capture/serving gap is detailed in
[DEV_60D237C5.md](DEV_60D237C5.md) and
[DEV_0871BFA6.md](DEV_0871BFA6.md).

## Live attempt

Backed up both native actor payloads using `cp -a` to `.before-port-c8dd515`
siblings, installed this soldier build and merged Lua, set `autoload=1`, and
used the native configuration with `newgame_density=0` to avoid resource edits.
The prescribed lab launch, bounded to 170 seconds, exited 1 immediately:
`bwrap: setting up uid map: Permission denied`.
No game, Vulkan device, title menu, save, terrain operation, gdb attachment or
worker-thread memory observation was reached. No host policy or Steam change
was attempted. Restored both payloads in a finally block and verified every
file hash and symlink target. No save changed and no game remains running.

Evidence: `meta/live/launch.txt`, `restoration.json`, `elf-id.txt`,
`signature-anchors.txt`, `savegame.asm`, `terrain.asm`, `verify-game.txt`, and
archived actor logs/data. Those actor archives can contain historical runs;
they do not show gameplay by this build.

## Missing proof / not ported

The native game-side threaded writer and `terrain_sidecar_threads` remain
unported. Still needed: live saved-world terrain ownership; a freeze/lifetime
boundary covering all worker joins; eligible tile and vector lifetime; safe
worker reads including native pager fault interaction; native path/backend
resolution; and capture/hash/restore validation on manual and automatic saves.
Linux cannot silently inherit Windows per-tile SEH recovery. A live validated
ownership contract or an appropriate guarded-copy design is needed before
calling this header on game-owned data. The existing native sidecar serving
and load-publication gaps also remain. No speedup is claimed.

## Offline coverage

CMake now builds the shared sidecar test with pthread linkage and assertions
active in Release. The test's fake allocation was corrected from 0x20 to
0x28 bytes: a 0x10-byte prefix plus the three 8-byte vector pointers requires
0x28. The earlier fixture wrote its final pointer outside the allocation.
The threaded fixture now has 4,500 records (1,500 eligible), genuinely
crossing three windows. Checks compare complete files at 2/4/7/automatic
threads against one thread, restore sample values, reject corrupt/stale data,
and exercise Linux UTF-8 paths, folder lookup, capacity rejection and
refingerprinting. Tests allocate their own storage; they prove neither live
ownership nor a Windows/Linux in-game round trip.
