# Windows dev 46ca48ef integration: Fantasia memory diagnostics

## Merged

Target `46ca48ef6785899fb2dcab64d38929f8616d3f8f`, one Windows commit.
Retains the optimizer's unknown-operation and pinned-final-value diagnostics,
the appended generator helper and updated upstream tests. The conflict in
`bigmap/src/generator_memory.h` is resolved by retaining its shared-header
include and moving the upstream helper/text changes into that shared header.
The Windows import detours remain intact. Merge remains uncommitted.
Release stays 0.7.0.7; Lua verification and package provenance name this target.

## Ported (what and how)

Both platforms now emit `_tpf2_bigmap_generate(result, params)` at the original
return line and append the same helper after the embedded optimizer. It logs
map dimensions, layer count and distinct buffer-name count, then optimizes
only above 32768 * 32768 square metres. Smaller maps log why they remain
unchanged. The optimizer logs unknown schemas and pinned-name refusals.
Allocation size and copying include the helper; the prior Linux length-indexed
anchor scan is preserved, including short-input, CRLF and repeated-anchor
refusal behavior. Linux CMake embeds the merged optimizer automatically.

No new engine address, byte sequence, offset, calling convention or ownership
contract is introduced. Linux retains the guarded fopen PLT route documented
in [DEV_6584FD03](../re/linux/DEV_6584FD03.md); all 30 guards were checked
against the actual lab ELF, build-id
`3a0e156390b0e6f1e372051c24802c8493ae454a`.
No additional engine reverse engineering is required for these Lua diagnostics.

## Not ported

None from this commit. Prior native feature gaps and outstanding live terrain
and peak-memory validation are unchanged.

## Live testing

No game was launched for this diagnostic-only change. No lab payload, save,
Steam installation or desktop state was modified. The tests execute emitted
Lua 5.2 code and symbolically compare pipelines using installed game/Workshop
resources read-only; they do not demonstrate actual in-game log output,
rendered terrain, Vulkan selection or reduced process memory.

## Tests

- `tools/linux/build_native.sh`: PASS; soldier SDK, 72/72 CTests and glibc
  <=2.31 check, including native generator routing/fallback tests.
- `python3 tools/linux/verify_lua_release.py`: PASS; 29 Lua files with one
  pinned native exception and 32 exact HUD textures. Lua manifest SHA-256:
  `069da99cba0cef16e9273300adb7efe47e7c52748e27e1de9db326df5c098d14`.
- `python3 bigmap/tools/linux/verify_game.py <lab ELF>`: PASS; build-id and
  all 30 patch sites.
- Lua 5.2 test of the native library's emitted text: PASS for all three
  installed Fantasia climates, including exact diagnostic output, original
  line-number preservation and symbolic pipeline equivalence. At 40 km,
  temperate/tropical reduce 325 -> 10 names and dry 328 -> 10; at 32 km each
  pipeline is unchanged and emits the threshold explanation. Added fixtures
  verify unknown-op refusal preserves the input and logs its type, the exact
  threshold, and absent dimensions. These are not engine memory measurements.
- Exact source comparison: diagnostic helper matches upstream; optimizer
  byte-identical to upstream.
- Release script syntax, whitespace and unmerged-index checks pass.

Logs: `.git/port-46ca48ef-{build,lua,elf,fantasia}.log`. Lupa was installed
only into `.git/port-46ca48ef-venv`, with no system-wide installation. The first
symbolic run found the lab resource symlink unavailable outside its mount;
the final command uses the installed game's resource directory read-only.
Windows compilation was not available on this host.
