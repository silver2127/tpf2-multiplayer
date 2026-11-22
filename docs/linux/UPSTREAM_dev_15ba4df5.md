# Windows dev 15ba4df5 integration: generator dimensions in samples

## Merged

Target `15ba4df5366066e89028637ae16218d6f12f5976`, one Windows commit.
Linux parent `e9758e1b9cb3815365eec27ad6e9b21e603eb4b1`.
Resolved both conflicts and staged the integration; merge remains uncommitted.
Windows documentation and import detours are retained. Release remains 0.7.0.7.

## Ported (what and how)

Moved the incoming helper correction into `bigmap/src/generator_memory_text.h`,
already included by both the Windows and native Linux implementations. Its
helper is byte-for-byte identical to upstream. The gate now compares sample
area against `8193 * 8193`, and diagnostics show samples and derived tile
counts. Dimensions are `64 * tiles + 1`; upstream measured 12289 samples at
192 tiles. That measurement is upstream evidence, not a new native observation.
The previous metre-based gate incorrectly bypassed optimization on these maps.

Kept the shared length-indexed anchor scan, Windows imports, Linux anonymous
streams, native test-library CLI and successful-return behavior in the Python
test. Updated native diagnostics fixtures to sample units and added explicit
optimizer-call assertions at/below/above the boundary and for rectangular
maps on both sides of the area threshold. Retained upstream symbolic replay
for all three climates at 130, 160 and 192 tiles and equality at 128 tiles.
Updated Linux installation guidance, port notes, package provenance and Lua
verification reference to this integration.

No engine address, byte pattern, struct offset, calling convention or ownership
contract changes. Existing native routing is documented in
[DEV_6584FD03](../re/linux/DEV_6584FD03.md). The actual lab ELF still passes all
30 patch-site guards and build-id verification. No new engine RE is needed
for the shared Lua correction.

## Not ported

None for this commit. Previous native feature gaps and live-validation limits
remain unchanged. Windows compilation was not performed on this Linux host.

## Live testing

No game launched, gdb attached or desktop input sent. No actor payloads or
saves changed, so no backup/restoration was needed. No renderer, in-game logs,
terrain output or process-memory reduction was observed. Offline Lua 5.2 tests
read installed Fantasia and game resources without changing them. Native
rendered terrain and peak-memory validation remain outstanding.

## Tests

- `tools/linux/build_native.sh`: PASS; pinned soldier SDK, 72/72 CTests,
  including native stream routing/fallback guards, and glibc <=2.31 checks.
- `python3 tools/linux/verify_lua_release.py`: PASS; 29 Lua files with one
  pinned native exception, 32 exact HUD textures. Manifest SHA-256:
  `069da99cba0cef16e9273300adb7efe47e7c52748e27e1de9db326df5c098d14`.
- `python3 bigmap/tools/linux/verify_game.py <lab ELF>`: PASS; build-id
  `3a0e156390b0e6f1e372051c24802c8493ae454a` and all 30 patch sites.
- `bigmap/tools/test_generator_memory.py --native-library
  native/linux/out-soldier/bigmap/libtest_generator_text.so --fantasia-res
  <installed Workshop 2916150031>/res --game-res <installed game>/res`:
  PASS using clone-local lupa 2.8 / Lua 5.2. Temperate/tropical: 57, 62, 70
  names become 10 at 130, 160, 192 tiles; dry: 60, 65, 73 become 10.
  All three pipelines remain identical at 128 tiles. Symbolic replay and
  exact diagnostic strings pass; these are not engine memory measurements.
- Final boundary fixture rerun with `.git/no-workshop`: PASS for diagnostics,
  unknown-op refusal, absent dimensions and explicit rectangular area cases;
  optional Workshop replay skipped on this fixture-only rerun.
- Shared helper equality to Windows, release script syntax, CRLF-aware
  staged whitespace, empty unmerged index and retained MERGE_HEAD: PASS.

Logs: `.git/port-15ba4df5-{build,lua,elf,fantasia,boundary,deps}.log`.
Python dependencies are confined to `.git/port-venv`; no system install.
