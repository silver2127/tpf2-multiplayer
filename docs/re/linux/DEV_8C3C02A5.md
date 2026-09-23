# Big Maps integration and unresolved native contracts

Target: Windows dev `8c3c02a5a886151c54133669dae9ef2437644d28` (0.7).
ELF: Steam Linux 35924, build-id `3a0e156390b0e6f1e372051c24802c8493ae454a`.

## Reused native implementation

Import tracked `linux/` and Linux evidence from sibling Big Maps `4769cd3`.
The ABI header is byte-identical to multiplayer's shared header; the include now
uses that header directly. The source does not import Windows addresses or layouts.
[The original evidence](../../../bigmap/docs/linux/PORT.md) covers the SysV ABI,
20 guarded sites, libstdc++ vector/string ownership, patch preflight and rollback.
The independent ELF checker passes against the actual lab binary. Native tests
exercise emitted machine-code stubs and patch refusal. No new patch sites were
introduced in this integration. Historical sibling live results are not this run.

## Static attempt on the remaining Windows features

Read the imported headers and their Windows RE documentation, searched Linux
`funcsig.csv` by source paths and signatures, and disassembled candidates in the
actual ELF using objdump. Full search/disassembly output is in the job's
`meta/live/bigmap-candidates.txt` and `bigmap-static-disassembly.txt`.
These candidates are **not** sufficient patch contracts:

- **Minimap:** the signature export identifies raw-pixel
  `UI::ImageView::SetImage(int,int,int,const vector<unsigned char>&,bool)` at
  `0x30b26c0`. Its prologue saves RDI as the widget in R12, ESI in R15D,
  EDX/ECX/R8/R9D to stack, consistent with SysV's six argument registers.
  This is not the Lua string overload. The token binding/delegation, terrain
  accessor, water/height fields and texture ownership still need tracing live.
  The Windows embedded GUI is retained as source but not installed on Linux.
- **Alignment batching, sidecar, block/small paging and refinement:**
  Terrain.cpp anchors include `GetTileCache` `0xcf5960`; inspect the existing
  AddTile allocation neighborhood `0xcf7400..0xcf7840`. The apparent
  `TerrainAlignmentSystem::Update` at `0x173a700` is a null-node-list check and
  assertion, not the heavy Windows update pass. `GetAffectedBlocks` is
  `0x173bd20`; sub-terrain bicubic refine is `0xd9b850`. Disassembly alone here
  has not established the real batching owner or publication boundaries.
  Linux saved CTerrain ownership, dirty-entry size, shared-vector detachment,
  save/load lifetime and sidecar fingerprints must be proved before serving
  or skipping engine work. Windows record offsets are not used.
- **Material paging/index fast path, COW/dedup and adaptive pager policy:**
  MaterialIndexManager source anchors include GetTex `0xcbcfa0`, LoadTex's
  visitor `0xcc3040`, async work `0xcc7350`. The first two were disassembled;
  neither proves exclusive cell allocation/disposal or fault-safe lifetimes.
  The existing Linux userfaultfd terrain backend remains opt-in, without the
  Windows material/small pools, COW sharing, dedup, warmup/commit throttle or
  adaptive working-set policy. These cannot be copied from VirtualAlloc/VEH.
- **Depth 12/13, placement cost and generation budgets:** OctreeSystem Update
  `0x16a6050` is likewise a null-list assertion path; DoesNodeChange
  `0x16a6690` reads per-entity storage at this+0x40/+0x48. Constructor anchor
  RandomLocationFactory `0x14e66e0` was disassembled. Neither proves every
  packed octree-id consumer or saturating-distance/placement-loop equivalence.
  Keep the verified depth-11 and square-size bounds, not Windows' depth 13.
- **Instance shrinking, generation buffer reuse and world-entry timing:**
  searched ModelInstanceList/GetInstance and heightmap/generation signatures;
  disassembled GetInstance and terrain GetBlock. Borrowed buffer publication,
  worker ownership and all entry phase ABIs remain unproved. No native Lua
  generation-memory installer is enabled merely because the Lua was merged.
- **Travel-time knobs:** PathFactory Compute `0x14e2e90` and its task
  `0x14e13c0` were disassembled; StockListSystem source anchors were searched.
  The Windows shared float cells at `0x3094978/0x309497c` are not Linux
  addresses. Their Linux constant-use closure and all consumers remain missing.

## Live attempt

Installed the newly built native libraries, plugin and merged Lua into backed-up
lab copies; requested autoload and `TPF2MP_ORDER_CANON=1`. Disabled sparse density
for this attempt to avoid writing the lab's shared read-only game resources.
The prescribed launcher failed immediately: `bwrap: setting up uid map:
Permission denied` (exit 1). No game process, menu, Vulkan device, loaded save,
register/lifetime probe or visual result was reached. Consequently none of the
open contracts above acquired live proof; no gdb attachment was possible.
Do not treat this infrastructure failure as evidence that a candidate is wrong.

Libraries/runtime data and Lua were restored from uniquely named
`.before-port-8c3c02a5` backups and compared using `diff -qr`. No save was changed.
The job's `meta/live/` retains launch output, restoration result, copied actor
logs/data (which include pre-existing logs), and static analysis output.

## Live-join default

The seven canonical-order sites and family metadata passed the existing ELF
checker again; Engine::Update was also disassembled. The missing loaded-world
lifetime check from [0115785c](DEV_0115785C.md) remains blocked by launch failure.
Windows keeps 0.7's default-on policy. Native Linux requires an explicit
`1`, `on` or `yes`, read each join; absent, empty and unknown values stay off.
This does not gate a Windows-hosted retained join by peer capability, so mixed
sessions must disable live join on the Windows host until native ordering is
validated. A matching 0.7 version does not establish parity.
