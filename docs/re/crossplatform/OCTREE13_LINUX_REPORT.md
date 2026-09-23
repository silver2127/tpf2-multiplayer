# Octree depth 12/13 for the native Linux Big Maps plugin

Patch: `octree_depth13_linux.patch` (git diff --binary against origin/port/dev 0a15250; applies cleanly, 871 lines).
Worktree it was made in: `scratchpad/octreewt` (detached at origin/port/dev). Nothing was committed or pushed.
Linux ELF copied read-only from the VPS (build-id 3a0e1563...454a) to `scratchpad/TransportFever2.elf`.
Nothing on the VPS was changed.

## Site table (Windows RVA -> Linux RVA)

| Role | Windows | Linux | Patched on Linux |
|---|---|---|---|
| Root/depth, >128-tile tier | 0x2304f8 | 0xa84234 (existing depth-11 site) | yes: extent 2^(d+5), depth byte d |
| Octree::Resize | 0xa51b00 | 0x16a6580 | no |
| Descent start (find node) | 0xa50c20 | callers 0x16a7e4e / 0x16a9935 / 0x16a9bff of 0x16aa040 | no |
| Recursive descent (child ID 8p+1+o) | 0xa507e0 (hooked at entry) | 0x16aa040, recursion inlined into a loop; child step at **0x16aa190** (20 B) | yes: jmp to child stub |
| Skip query, inlined CalcOctreeLevel | 0x853d10 / loop 0x853d30 (39 B) | 0x13ec500 / loop **0x13ec530** (37 B, after test/js at 0x13ec528) | yes: jmp to level stub |
| Level from box width + depth | 0xa503e0 | 0x16a6620 | no |
| Skip-manager level vector | 0x852940 | 0x13ee4b0 -> resize 0x13f09b0 | no |
| DoesNodeChange (movement) | 0xa4ffa0 | 0x16a6690 (bounds only) | no |
| Remove / RemoveNodeFromParent | 0xa50fb0 | 0x16a9400 (child-pointer scan) | no |
| VisitStaticNodes | 0xa51c00 | 0x16a6a70 (callers only in the skip manager) | no |
| Renderer skip use / visit | 0x2c1260 / 0x2bd590 | CRenderer::NewUpdate lambdas (unordered_set<int>) | no |
| Heightmap INT_MAX bound | menu code | Fits/Bound/Shape (already present) | no change |

How they were matched: the Linux binary keeps `__PRETTY_FUNCTION__` assert strings
(`Octree<Data>::Resize`, `int {anonymous}::CalcOctreeLevel(int)`, `OctreeSystem::DoesNodeChange`,
`VisitStaticNodes`, `RemoveNodeFromParent`, ...). RIP-relative xrefs to them gave the functions; the
rest came from call graph and instruction shape (0xa8-byte node alloc + `lea r13d,[r15+r13*8+1]`,
identical width/depth loop). A whole-.text scan found exactly one CalcOctreeLevel-shaped loop and one
child-ID computation, and no branch lands inside either patched range. Details and disassembly are in
the new PORT.md section.

## What was implemented

- `bigmap/linux/octree_depth.h` (new): site constants, expected bytes, and two hand-assembled stubs
  (annotated bytes), plus builders.
  - Child stub (145 B): replays the three stock moves, then chooses r13d exactly as Windows `Depth12Id`,
    using the parent node's stored ID `[rax+8]`: below 0x09249249 = stock arithmetic; level-10 parent -> bank 0
    (0x50000000..), level-11 compact parent -> bank 1 (0x60000000..); existing child keeps its ID; otherwise
    `lock xadd` on process counters (0x4fffffff/0x5fffffff start, never reset); `ud2` (SIGILL) where Windows
    `__fastfail`s (bad parent range, remaining<1, bank exhausted).
  - Level stub (81 B): id >= 0x50000000 -> (id>>28)+6, else `test esi,esi` + the 37 stock bytes unchanged.
- `bigmap_linux.cpp`: accepts octree_depth 11/12/13 (others -> ERR_FAILED, nothing written); max_tiles cap
  512/1024/2048 by depth; at depth 12/13 verifies both ID sites before the root, plans child -> level -> root
  (root last), any mismatch -> ERR_BUILD with no writes; rollback covers all three. Depth 11: same bytes as
  before (ID sites not even read). octree=0 ignores depth (as Windows). Stubs live in the existing Near() page
  (offsets 1024/1280), made RX with the rest.
- `sites.json`: two new guarded sites (29 total). `tpf2_bigmap.cfg`: comment only, default stays 11.
- Docs: new "Octree depth 12/13 (experimental)" section in `bigmap/docs/linux/PORT.md`; INSTALL.md updated.

## Tests run

1. `bigmap/tools/linux/test_octree_depth_elf.py GAME_ELF` (new, Unicorn, read-only): runs the ORIGINAL Linux
   descent loop and decoder with the header's exact site patches/stubs. Depth 13: 1,620 nodes, depth 12: 1,462
   (identical to the Windows emulation's counts for the same 158 positions); unique positive IDs, decoded level =
   parent-chain level, 256 m loose leaves, level-11 internal nodes for 200 m objects, reinsertion keeps identity,
   stock overflow reproduced when unpatched, levels 0..10 keep stock IDs, depth 10/11 patched == stock exactly.
   Mutation check: breaking the stub's stock branch or the decoder's +6 makes it fail. PASS.
2. `verify_game.py`: PASS, 29 sites.
3. C++ build: no gcc/MSYS/WSL distro with a compiler here, so I used `pip install ziglang` (zig 0.16 = clang)
   cross-compiling for x86_64-linux-gnu.2.31: plugin `.so` with `-Wall -Wextra -Werror -fvisibility=hidden`, -O3
   and the tests -O0/-O2: clean.
4. Ran the tests as static musl binaries inside the (stopped, then auto-stopped again) `docker-desktop` WSL distro:
   `test_bigmap` PASS (incl. new native stub execution via a town_stub.S harness vs a C++ copy of Windows
   `Depth12Id`, forked SIGILL checks, installer order/refusals/rollback/caps), `test_density` PASS, `test_pager`
   PASS. `test_alignment` was not built (it includes libstdc++ internals that zig's libc++ lacks; untouched code).

## Not tested

- The soldier-SDK build (GCC 8.3, libstdc++): not run. clang -Werror passed; GCC 8 may warn differently.
- No live game: world generation/load past +-131,072 m, renderer culling, save/reload, vehicles crossing the
  old root boundary, dedicated server, and any Linux<->Windows multiplayer session at depth 12/13.
- Concurrency of the real allocator/octree under the running engine (emulation stubs operator new).

## Open risks (simulation vs Windows at the same depth)

1. **Placement distance is not widened on Linux.** Windows `placement_distance.h` saturates the int32
   dx^2+dy^2 in RandomLocationFactory (town/industry founding, initial AND runtime). Linux still avoids it only
   by capping the menu (heightmap diagonal^2 <= INT_MAX, about 720-tile edges; 2048x64 -> 720x64). A world created
   on Windows beyond that bound (e.g. 1024+ tile edges) and loaded by a Linux server would run the stock,
   overflowing spacing cost on Linux and the saturated one on Windows -> different placement choices -> likely
   desync. This is the main blocker for "Linux server hosts the owner's depth-13 world"; it needs the Linux
   port of placement_distance.h. Depth itself now matches.
2. Compact node IDs are allocation-order dependent (same as Windows, even Windows-Windows). They are only
   consumed by the renderer skip manager (VisitStaticNodes callers and CalcOctreeLevel both in
   OctreeSkipManager), so they should not affect simulation; the audit shows no other ID arithmetic, but it is
   static evidence only.
3. Octree shape (root, depth, child boxes, stop rule) now equals Windows at the same depth, so query result
   sets should match. Any traversal-order or float differences between the GCC and MSVC builds are pre-existing
   and not addressed here.
4. Existing Linux behaviour differs from Windows at depth 11 in one way: Linux always installs the depth-11 root,
   Windows only when a configured size exceeds 256 tiles (default configs exceed it). At 12/13 both always install.
5. Fail-fast is `ud2` (SIGILL) instead of `__fastfail`; the game's crash handler will report it as an illegal
   instruction at the stub page.
6. Plugin version string (`0.4.0-linux-dev.3`) was not bumped.

## Scratch artifacts (not part of the patch)

`scratchpad/tre.py`, `wre.py`, `asm.py` (RE/assembly helpers), `zbuild/` (zig-built binaries), `port_section.md`.
The keystone-engine and ziglang pip packages were installed into the user's Python 3.12.
