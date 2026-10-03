# Windows dev 26564158 integration: outward terrain lookup

## Merged

Target `26564158e5cf6f83b7c808740449d712fa84d3bb`, Linux parent
`325ecf68a98c8a4e224e401660c83c9425b9ee7a`. One incoming commit:
`bigmap: the record lookup searches outward from the last hit -- per-thread cursors alone still probed ~35,000 per tile`.
Resolve `bigmap/linux/sidecar_linux.h`; stage the merge without committing.
Release remains 0.7.1.1. Windows serving code and its new descending-order
regression remain byte-identical to the target. Lua/assets are unchanged;
retain the existing release-verifier baseline.

## Ported (what and how)

Native FindRecord searches outward from each worker's last successful index,
retaining the prior Linux load-generation reset and optional probe count.
Reset to n-1 so ascending loads start at record zero in one probe. Search
both directions through floor(n/2), then the last hit itself. Unlike upstream's
extra outward step, this visits each record once and keeps misses bounded
by n, with identical first-visit ordering. Windows code remains intact.

Add native per-load probe/call totals and the load-completion ratio, using
relaxed atomics once per lookup and resetting at LoadHook. The denominator
is AddTile calls reaching the sidecar lookup, explicitly labelled in the log.
No per-probe shared atomic is introduced. Tests extend the existing native
fixture for descending traversal and exhaustive small odd/even grids, while
preserving worker reuse, wraparound, missing/null records and hook-flow checks.

No new hooks, ABI, offsets or patch bytes. Existing fail-closed guards remain.
See [RE evidence and algorithm details](../re/linux/DEV_26564158.md).

## Not ported

None from this commit. Existing experimental sidecar ownership/lifetime
validation gaps remain, and `terrain_sidecar=0` remains the default. Windows
load performance observations are not native measurements.

## Live testing

No game launched, gdb attached, XTEST input sent or actor payload installed.
The change only alters mod-owned lookup state and diagnostics; no new live
ABI derivation is required. No native game load-time improvement is claimed.
Steam, actors, saves and installed mods were untouched; no restoration needed.

## Tests

- `tools/linux/build_native.sh`: PASS, final soldier build, 78/78 CTests
  (62.83 seconds) and glibc <=2.31 checks, including strengthened exact-order
  assertions. The initial build also passed all 78 tests.
- Native lookup fixtures: 957 probes for 480 alternating-worker lookups;
  1,438 probes for 480 descending lookups. Persistent workers reset on load.
  Full fake-load logs report 1.0 probes per lookup; the stream catch-up path
  with no AddTile lookups reports 0.0 without division by zero.
- `python3 tools/linux/verify_lua_release.py`: PASS, 31 Lua files, zero
  exceptions, 32 HUD glyph textures and two toolbar textures. Manifest SHA-256
  `1de350a7196751cdbdc1e50bf3b4bb29fe72b547e3a109bf4ec813b84d949673`.
- `python3 bigmap/tools/linux/verify_game.py <lab ELF>`: PASS, build-id and
  all 39 patch sites. Fresh AddTile disassembly confirms the unchanged layout.
- Windows source/test match the merge target exactly. The Windows-only
  serving fixture was not executed on Linux; native hook-flow coverage passes.
- Whitespace checks pass, the unmerged index is empty and MERGE_HEAD remains.

Logs: `.git/port-26564158-build.log`, `.git/port-26564158-final-build.log`,
`.git/port-26564158-lua.log`, `.git/port-26564158-addtile.asm`, and
`native/linux/out-soldier/Testing/Temporary/LastTest.log`.
