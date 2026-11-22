# Windows dev 5d73f324 integration: generator parallelism budget

## Merged

Target `5d73f324a49cb54296f6c616cbedce10dbb5aa27`, one Windows commit,
“bigmap: generator memory spends a memory budget on parallelism”. Linux parent
`f182d895e20877896dd878a4121638e4c496e7c6`. Both conflicts are resolved and
staged; the merge remains in progress and uncommitted. Release stays 0.7.0.7.

## Ported (what and how)

Kept Windows GlobalMemoryStatusEx, cfgInt initialization, import detours and
budget-aware exported test API. Moved the incoming helper and budget suffix
into the existing shared `generator_memory_text.h`, preserving the bounded
anchor scan. Standard C++ snprintf replaces MSVC-only _snprintf_s in this
shared code, with truncation/error refusal. The Lua helper and optimizer are
identical to upstream. Both platforms pass a byte budget that the helper
converts to temporary float-buffer slots. Without a positive slot count the
optimizer retains first-fit fewest-buffer behavior; otherwise it opens slots
and then reuses the longest-idle slot. Required simultaneous values can exceed
the requested budget. Small maps and stock generator behavior stay unchanged.

Linux reads `generator_memory_budget_pct` (default 50, capped at 90; <=0 off)
and samples `/proc/meminfo` on each redirected file open. The chosen native
analogue of free physical memory is MemAvailable. Only strict Linux overcommit
(mode 2) additionally caps it by saturated CommitLimit minus Committed_AS:
in modes 0/1 that subtraction is not a hard allocation ceiling. Missing or
failed required queries fall back to zero, selecting the fewest buffers.
The result uses upstream's divide-before-multiply arithmetic and is appended
to the per-open anonymous stream; Workshop files are untouched.

Retained Linux CLI/testing support and moved the Windows redirect budget
assertions into the reachable main function (the incoming conflict placed
them after the extracted checker returned). Updated config, installation
notes, port record, Lua baseline and package provenance to this commit.

No game address, byte pattern, object layout, ABI or ownership change is
needed. Existing fopen routing and disassembly evidence remain in
[DEV_6584FD03](../re/linux/DEV_6584FD03.md). The actual lab ELF passes its
build-id check and all 30 patch-site guards. No new engine RE contract exists
to settle with gdb for this change.

## Not ported

None from this commit. Earlier native feature gaps remain unchanged.
Windows compilation was not performed on this Linux host.

## Live testing

No game launched, gdb attached, or desktop input sent. No actor payload or
save was modified, so backup/restoration was unnecessary. No Vulkan renderer,
game terrain, generation duration or peak-memory result was observed.
Native rendered-terrain and performance validation remain outstanding.
Offline tests read installed game and Fantasia resources without modifying
them; modeled dependency-chain lengths are not measured engine timings.

## Tests

- `tools/linux/build_native.sh`: PASS, soldier SDK, 72/72 CTests and
  glibc <=2.31 checks. Added generator tests cover config lookup, percentage
  capping, disabled/negative budgets, strict commit exhaustion, modes 0/1,
  missing memory data, overflow-safe arithmetic, proc-style parsing and the
  appended zero budget. Existing stream routing/lifetime/fallback guards pass.
- `python3 tools/linux/verify_lua_release.py`: PASS, 29 Lua files with one
  pinned native exception and 32 HUD textures. Manifest SHA-256
  `069da99cba0cef16e9273300adb7efe47e7c52748e27e1de9db326df5c098d14`.
- `python3 bigmap/tools/linux/verify_game.py
  ~/.local/share/tpf2mp-lab/native/game/TransportFever2`: PASS, all 30 sites,
  build-id `3a0e156390b0e6f1e372051c24802c8493ae454a`.
- `bigmap/tools/test_generator_memory.py --native-library
  native/linux/out-soldier/bigmap/libtest_generator_text.so --fantasia-res
  <Workshop 2916150031>/res --game-res <installed game>/res`: PASS using
  clone-local lupa / Lua 5.2. All climates symbolically equivalent at 130,
  160 and 192 tiles and identical at 128 tiles. Fewest-buffer mode gives 10
  names. At 192 tiles a 30-slot budget gives 34 names: ordered chain lengths
  stock/fewest/budget are 1716/1924/541 (temperate), 1764/1983/549 (dry),
  1728/1936/543 (tropical). Added fixtures check zero/sub-buffer/30-buffer
  budget forwarding and small-map bypass; prior diagnostic/boundary tests pass.
- `test_generation_memory.main()` with RES pointing at the installed game:
  PASS for stock generator symbolic replay, guards, installer/restore and
  idempotence; temporary fixture files confined to `.git/port-tmp`.
- Shared helper/optimizer equality to Windows, release-script syntax,
  CRLF-aware whitespace checks and empty unmerged index: PASS.

Logs: `.git/port-{build,lua,elf,fantasia,stock,deps}.log`. Dependencies are in
`.git/port-venv`; no system installation or Steam changes were made.
