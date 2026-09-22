# Material-index DataGrid payload lifetime (Steam 35924)

Static analysis only, September 15, 2026. Nothing was attached to or launched.
Exe `C:\tools\bin\TransportFever2.exe` (sha256 782b904a...). RVAs throughout.
Scratch: `%TEMP%\tpf2-material-grid` (`d1/`, `d2/` Ghidra decompiles,
`hooksites.txt`, capstone/pdata scripts `pe.py pdata.py relscan.py scan.py
vcall48.py inline48.py`).

Tags: **[D]** = DERIVED-FROM-CODE, **[G]** = GUESS.

Object under study: `RenderDataManager` (RDM, vtable `2f912d0`) `+0x250` ->
`terrain::DataGrid<unsigned char>` (vtable `2f91320`), cell array at grid `+0x78`
(`begin/end/cap` at `+0x78/+0x80/+0x88`), 0x48-byte cells, payload
`vector<uint8>` at cell `+0x00` (67,601 bytes = 260x260 + canary 'V'),
state `vector<uint8>` at cell `+0x28` (16 bytes). RDM `+0x248` is the *ambient*
DataGrid (same vtable and functions, 1,157-byte payloads) and must be excluded
by size.

## 1. Summary of verdicts

| Design | Verdict | Why |
|---|---|---|
| **A: ownership** (arena allocation at InternCreate, release at InternDestroy) | **SAFE by static analysis, pending runtime validation** | Exactly one allocation site and one free site exist for payload buffers. The two generic helpers that could reallocate are provably not taken for cells, but should be guarded anyway. No kernel/driver consumer exists. |
| **B: in-place decommit** (heap block stays, interior pages decommitted) | **UNKNOWN, not recommended** | Engine-side facts are identical to A (same single free site to restore before). Safety then rests on CRT/NT-heap internals: interior pages of a busy block never touched, no silent reuse. That is not provable from this exe. It has no alias for race-free snapshots, and a missed free path corrupts the heap silently instead of failing at the free. |

Both designs share the most important finding: **payload pages are not only
touched on terrain edits.** Material texture streaming re-reads a tile's payload
and its neighbours' whenever that tile's GPU texture slot is re-created
(section 5). With a pager, panning the camera causes restores. A full "Repainting
entire map" pass (`32ff20`) reads and writes every payload.

## 2. Structure facts

- **[D]** DataGrid<uint8> vtable `2f91320` has exactly 4 slots: 0 `32d840`
  (scalar deleting dtor), 1 `3303b0` InternCreate, 2 `330350` InternDestroy,
  3 `32f4a0` Exists. Slot 4 onward is string data.
- **[D]** Base `IBaseGrid<vector<uint8>>` vtable `2f912f8`: slot 0 `32d970`,
  slots 1-3 `_purecall`.
- **[D]** A byte scan of all `.text` for RIP-relative references finds `2f91320`
  only at `327058` (ctor wrapper `327000`) and `32d864` (dtor `32d840`), and
  `2f912f8` only at `347a27` (base ctor `3479f0`) and `32a70b` (base dtor
  `32a6f0`). **No copy/move constructor or copy-assignment of a DataGrid<uint8>
  exists**, because any such constructor would have to store one of these
  vtables.
- **[D]** The cell array is built once in `3479f0` via `3480a0`
  (`count*0x48` allocation). No `IBaseGrid<vector<uint8>>` method resizes or
  reallocates it. The complete method set comes from funcsig plus
  TextureGrid.cpp assert xrefs: ctor `3479f0`, `GetActualPosAndOffset 3486f0`,
  `GetDataReadAndOffset 348950`, `GetDataWriteAndOffset 348a70`, create
  `349240`, and the state-byte-only helpers `348bc0 348d30 349040 3490d0 349560
  3495f0`, plus dtor `32a6f0`. **Cell payload triples are never moved or
  copied.**
- **[D]** The functions `348430`, `3491f0`, `349490` and `349290` index 0x38-byte
  cells. They belong to the `IBaseGrid<TextureId>` TextureGrids (`+0x258`,
  `+0x260`, `+0x268`), not this grid.
- **[D]** Allocation is operator new `2bf3a80` -> ucrt `malloc`
  (`api-ms-win-crt-heap-l1-1-0`). Delete is `2bf3abc` -> `2bf41c0` -> ucrt
  `free`. A 67,601-byte vector uses MSVC's big-allocation alignment: raw =
  `malloc(n+0x27)`, `ptr = (raw+0x27)&~0x1f`, `[ptr-8] = raw`. Every inline
  deallocation checks `ptr-8-[ptr-8] <= 0x1f` and fast-fails otherwise.
- **[D]** The exe manifest has no `heapType`/`SegmentHeap`. **[G]** The process
  therefore uses the NT heap unless a system policy overrides it. 67,640-byte
  blocks come from heap segments (below the VirtualAlloc threshold), not
  private VirtualAlloc regions.

## 3. Every allocation / free / realloc path for payload buffers

| # | Path | RVA(s) | Reached from | When | Effect on payload buffer |
|---|---|---|---|---|---|
| 1 | **InternCreate** -> `vector<uint8>::resize` helper | `3303b0`, call `3303e1` -> `1d5830`, returns to `3303e6` | only via slot 1: `349240` (tail `jmp [r8+8]` at `349281`), called by BeginBox `314bb0` at `314e1c`/`314e90` (material) and `32ce50` at `32cf19` (ambient) | world entry (initial generation), every edit box, full repaint | **The only allocation [D].** From an empty vector: grow path `1d5872` allocates `n` (`831f0`), memsets zero (`2bf676f`), `_Change_array` (`d6f70`). Then canary `'V'` is written at `3303f8` only if the old size was 0. On an existing size-67,601 cell, resize(67,601) is a no-op (`1d5948 je`). |
| 2 | **InternDestroy** | `330350`, canary compare `33036d`, tail `jmp 1d5830` with n=0 at `33037f` | only via slot 2: `32d840` at `32d89a` (loop over all cells) | grid destruction only | **Not a free [D]**: resize(0) sets `last=first` (`1d594a`) and capacity is kept. It reads the canary byte `[first+67600]`. |
| 3 | **IBaseGrid<vector<uint8>> dtor** | `32a6f0`; payload free at `32a7b8` (`call 2bf3abc`, aligned-header check `32a79c..32a7af`); state vector free `32a774`; cell array free `32a82e` | `32d840` at `32d8b1` (after InternDestroy on every cell); `32d970` at `32d97f` (base-vtable deleting dtor, only live during construction) | world teardown: RDM dtor `32b220` (via deleting dtor `32dcb0`) calls `(*grid->vtbl[0])(grid,1)` for `+0x250` (`param_1[0x4a]`) | **The only free of payload buffers [D].** It checks `first != null` (`32a787`), so a nulled triple is skipped. |
| 4 | `vector<uint8>::_Assign_range` | `1b3d90`; free-old at `1b3dxx` (inline `free` after `cap < newSize`) | FinishBox `3130f0` at `3133aa` (dst = **cell**, src = job cache) | edits, streaming job completion, entry | **Not taken for cells [D]**: the cache vector is a copy of the same cell made in BeginBox (`31521e`), so newSize = 67,601 = capacity and the call reaches `memmove` in place. It would realloc only if a cell's capacity were smaller than 67,601, which never happens. |
| 5 | `vector<uint8>::resize` grow path | `1d5872..1d5914` | only callers with a cell vector: InternCreate/InternDestroy | - | Frees the old buffer only if `n > capacity` on a non-empty vector. **Not reachable for cells [D]** (n is always 67,601 or 0). |
| 6 | Grid cell array reallocation/copy | - | none | - | **Does not exist [D]** (section 2). |
| 7 | DataGrid copy/swap/move | - | none | - | **Does not exist [D]** (vtable xrefs). |

Other `1b3d90` callers (`1c1e07 1f0717 305595 3e82c6 4c35a5 9d872a 269fc18
269fd49`) are not reached with cell vectors. The only accessors that yield a
cell vector are `348950`/`348a70`, and their complete caller set is `3130f0`,
`314bb0`, `317ae0` (material) and `32ce50`, `33a4e0`, `332ce0` (ambient) [D].
`305595` is a struct copy-assign with a flag at `+0xa0` [D].

Completeness evidence:

- Whole-`.text` E8/E9 rel32 scans for callers of `32a6f0 330350 3303b0 32d840
  32d970 348950 348a70 349240 3495f0` (catches funclets and tail jumps the
  Ghidra call graph misses) [D].
- A whole-`.text` sweep for `call/jmp [reg+8|0x10]` whose preceding 14
  instructions load `[x+0x78]` and build a 0x48 stride found only `32d89a`
  (slot 2) and `349281` (slot 1) for this grid [D].
- A whole-`.text` sweep for inline `[base78 + idx9*8 + {0,8,0x10}]` access found,
  in the terrain range, only the accessors, the dtor, and `32c300`/`332ce0`.
  The last two index the RDM `+0x2b8` MaterialListBuffer grid, not this one.
  The 8 hits outside the terrain range contain no RDM `+0x250` load [D].
- **Residual [G]:** the sweeps are pattern-based. A slot-2 call whose cell
  pointer is computed far from the call, or kept across a call, would be
  missed. Runtime counters (section 8) close this.

## 4. Readers and writers of payload bytes after world entry

| Who | RVA | R/W | Payload(s) touched | Thread | Frequency | Pointer leaves user-mode CPU code? |
|---|---|---|---|---|---|---|
| InternCreate zero-fill + canary | `1d58c8` memset, `3303f8` | W | own cell, once | BeginBox's thread (pool worker or caller) | once per cell (first BeginBox covering it) | no |
| **BeginBox** `314bb0`: `349240` create, `348950` read, `1b3d90` copy **cell -> job cache** (`31521e`), `349040`/`349560` state bytes | `315073`, `31521e` | R | every tile in the box (cache copy made once per job per tile) | ThreadPool: reached from `315e30` <- `312990` MaterialIndexAsyncWork (async lambda enqueued by `30b2b0`) or `315720` inline [D] | entry (all tiles), each edit box, full repaint | no (memmove into heap vector) |
| UpdateBoxAsync `313530` -> `315f20` | `315f20` | writes the **job cache**, not the cell [D] | - | pool workers (`30c2e0` LoopImpl) | as above | - |
| **FinishBox** `3130f0`: `348a70` + `1b3d90` copy **cache -> cell** | `313392`, `3133aa` | **W** | the tile being finished | pool workers via `30c8a0` LoopImpl (`30abb0` Enqueue), or the caller when single-threaded (`GetThreadIndex()==0` asserted). The callers are `3163c0` from AsyncWork `312cdf` (worker) and `3164d0` from RDM::Update/`32f670`/`32ff20` (render thread) [D] | entry, edits, repaint | no |
| **Result builder** `317e80` -> `317ae0` -> `348950` + `30a480` byte loop | `31342a` (FinishBox), `312fe3` (UploadMaterialTex lambda `312ef0`); read `317d0a`, copy `317d55` | R | the tile **and its neighbours** (offset table from `30ee00`) copied into a fresh 260x260 result vector (`1d5830` at `317f10` on the caller's local) | pool workers (`30c8a0`, `30c530`) or caller | entry, edits, **texture streaming** | no: plain byte loop |
| InternDestroy canary compare | `33036d` | R | last byte of each cell | teardown thread [G: main/UI thread] | teardown | no |
| **Nothing else.** RDM slot 1 `32fe70`, slot 2 `32f6f0`, `32ff20`, and RDM::Update `3490d0` touch only the cell `+0x28` state vector [D]. The other `+0x250` loads in `302720`, `341069`, `341780`, `3422d4` and `34238d` are `[vtable+0x250]` render-context virtual calls, not this grid [D]. | | | | | | |

**GPU upload does not read payloads [D].** `UpdateBox 318f20` passes the
*result tuple's* vector (`DataAndOffset{lVar17,0}` built from
`vector<tuple<CVec2i,vector<uint8>>>`) to `TextureUploader::Upload<uchar>
346f30`. That calls the render context vfunc `+0x218` with `data.first` of the
result vector. The results are heap copies produced by `317e80`.
(The **ambient** grid, by contrast, is uploaded straight from its cell:
`332ce0` lines 658-662 `348950(+0x248)` -> `346f30`. It is irrelevant here, but
it would matter if the pager were ever extended to ambient cells.)

**Serialization / saves [D, completeness G]:** there is no path from
save/load code to `348950`/`348a70` or to the grid. The persisted source of
truth is the ECS `TerrainTileBrush` (`CTerrain::GetMaterialIndexBrush 33d3a0`,
"materialIndexBrush"). The grid is derived render data, rebuilt at entry.
**Multiplayer:** the engine has none, and tpf2-multiplayer never references it.

**Kernel/driver exposure: none found [D].** Payload bytes are only touched by
`memmove` (ucrt, user mode), explicit byte loops, and `memset`. No `WriteFile`,
no D3D/Vulkan call and no mapped-staging handoff ever receives a payload pointer.
A `rep movsb`/SIMD fault inside ucrt `memmove` is a normal resumable user-mode
access violation. **[G]** Crash-dump writers read through `ReadProcessMemory`
and would just omit cold pages.

## 5. Access frequency after load

- **Per frame [D]:** RDM::Update (`332ce0`, called from `UI::CGameUI::DoRender
  573d70`, `Screenshot 406900`, `WriteEnvMap 409840`, and `795340`/`795740`
  through CGameUI `+0x4f0`) reads only the state bytes of listed tiles. No
  payload access happens without pending work.
- **Texture streaming (camera-driven) [D mechanism / G trigger]:** each frame
  `32c300` stamps the `+0x268` material TextureGrid slot for the tiles in a work
  list and steals an LRU slot (`MoveTexture 349290`) when needed. Tiles whose
  texture is not valid (`348f20` false) go to `UploadMaterialTex 319290`
  (`333ef3`) -> `312ef0` -> `317e80`, which **reads the payloads of the tile and
  its neighbours**. `32c9c0` (`333150`/`333157`) evicts slots over budget
  (`+0x60` vs `+0x90`). [G] The work list is the visible/near-camera set. Budget
  eviction plus camera movement therefore produces repeated payload reads.
- **Terrain edits [D]:** ITerrainListener callbacks (RDM slots 1/2) mark state
  bytes. RDM::Update then calls `BeginMaterialIndex 315720` (`333fdc`), and the
  path above (BeginBox read, FinishBox write, neighbour reads) runs on pool
  workers.
- **Full repaint [D]:** `32ff20` ("Repainting entire map...") calls
  `BeginMaterialIndex` on the whole grid and reads and writes every payload.
  It is invoked from `55ac30`, which is reached only through the
  `std::function<void()>` `lambda_45abdfc3...` (`_Do_call` thunk `57c370`,
  vtable slot at `2ffee80`) [D]. [G] This is a UI/settings callback (the argument
  derives from `FUN_142294b80(...) & 3`). Every tile faults in, so the pager
  needs a warm allowance for it, like the terrain loading allowance.
- **World entry [D]:** the RDM ctor (`327eb0`, "Initial material index
  generation") generates everything asynchronously.

## 6. Byte-verified hook sites

All bytes are read from the exe. Instruction boundaries come from capstone.
None of the stolen ranges contains RIP-relative operands or internal branch
targets.

### Allocation

**A1. Call-site-filtered generic resize (same scheme as the terrain pager).**
Detour `vector<uint8>::resize` `1d5830`. Eligibility: return address ==
`base+0x3303e6`, `v->first==v->last==v->end==nullptr`, `n == 67601`.
```
1d5830: 48 89 4c 24 08        mov [rsp+8], rcx
1d5835: 56                    push rsi
1d5836: 57                    push rdi
1d5837: 41 54                 push r12
1d5839: 41 56                 push r14
1d583b: 41 57                 push r15
1d583d: 48 83 ec 30           sub rsp, 0x30        ; 17 bytes, next insn 1d5841
```
The prologue is byte-identical to the uint16 resize `1d5c50` already hooked by
`terrain_compression.h`. Call-site bytes to verify:
```
3303de: 48 63 d0              movsxd rdx, eax
3303e1: e8 4a 54 ea ff        call 1d5830          ; return 3303e6
3303e6: 48 85 db              test rbx, rbx        ; canary written iff old size 0
```
Return **zeroed** memory (the stock grow path memsets). InternCreate writes the
canary itself at `3303f8`.
`Contains(v->first)` handling is required in this detour for any non-eligible
call: n <= 67601 adjusts `last` and zero-fills; growth migrates to the heap. That
covers InternDestroy's `resize(0)` tail jump (return address = InternDestroy's
caller, e.g. `32d89d`).

**A2 (alternative). Replace InternCreate** `3303b0` (slot 1, DataGrid<uint8>
only; `rcx=grid`, `rdx=vector`). Eligible when the vector is empty and
`[grid+0xc]*[grid+0x10]+1 == 67601`. Then do `[grid+0x60] += dim.x*dim.y`
(int32), allocate, zero, set the triple and write `'V'` at `first+67600`;
otherwise call the original.
```
3303b0: 48 89 5c 24 08        mov [rsp+8], rbx
3303b5: 48 89 74 24 10        mov [rsp+0x10], rsi
3303ba: 57                    push rdi
3303bb: 48 83 ec 20           sub rsp, 0x20        ; 15 bytes, next insn 3303bf
```
A2 avoids a return-address filter on a helper with ~20 call sites. A1 matches
the existing, tested pattern.

### Release before free

**R1 (required). InternDestroy** `330350` (slot 2; `rcx=grid`, `rdx=vector`).
For `Contains(v->first)`: do `[grid+0x60] -= dim.x*dim.y`, optionally check the
canary (a read that faults in the last page), release the slot and set the
triple to `{0,0,0}`, then return. Otherwise call the original. This runs for
every cell immediately before `32a6f0` (`32d89a` precedes `32d8b1`), and
`32a6f0` skips null vectors (`32a787 test rcx,rcx; je`).
```
330350: 48 83 ec 28           sub rsp, 0x28
330354: 4c 8b ca              mov r9, rdx
330357: 8b 51 10              mov edx, [rcx+0x10]
33035a: 0f af 51 0c           imul edx, [rcx+0xc]  ; 14 bytes, next insn 33035e
33035e: 29 51 60              sub [rcx+0x60], edx  ; 17 bytes -> 330361
```
(`330373` is a branch target, outside the stolen range.)

**R2 (recommended safety net). IBaseGrid<vector<uint8>> dtor** `32a6f0`.
Before calling the original, walk cells `[grid+0x78]..[grid+0x80]` (0x48
stride) and release or null any owned `+0x00` vector. This covers
exception-unwind or a hypothetical destroy path that skipped slot 2. For design B
it is the last point to recommit before `free` at `32a7b8`.
```
32a6f0: 48 89 5c 24 08        mov [rsp+8], rbx
32a6f5: 48 89 6c 24 10        mov [rsp+0x10], rbp
32a6fa: 48 89 74 24 18        mov [rsp+0x18], rsi  ; 15 bytes, next insn 32a6ff
32a6ff: 48 89 7c 24 20        mov [rsp+0x20], rdi
32a704: 41 56                 push r14
32a706: 48 83 ec 20           sub rsp, 0x20        ; 26 bytes; 32a70a is RIP-relative lea - do not steal
```
Payload free instruction for reference: `32a7b8: e8 ff 92 8c 02 call 2bf3abc`.

**R3 (defensive, recommended). `_Assign_range` guard** `1b3d90`. If
`Contains(dst->first)` and the new size exceeds 67,601, migrate to the heap
first (as `ResizeTerrainOwned` does). Otherwise let the original `memmove` run
into the owned address.
```
1b3d90: 48 89 5c 24 10        mov [rsp+0x10], rbx
1b3d95: 48 89 6c 24 18        mov [rsp+0x18], rbp
1b3d9a: 48 89 74 24 20        mov [rsp+0x20], rsi  ; 15 bytes, next insn 1b3d9f
1b3d9f: 57                    push rdi
1b3da0: 41 56                 push r14
1b3da2: 41 57                 push r15
1b3da4: 48 83 ec 20           sub rsp, 0x20        ; 24 bytes -> 1b3da8
```
The only cell call site is `3133a7: 48 8b 17 mov rdx,[rdi]` / `3133aa: e8 e1 09
ea ff call 1b3d90` (return `3133af`). A return-address filter is possible, but
`Contains()` alone is sufficient and also cheap.

### Teardown

The same as R1/R2. The chain is RDM deleting dtor `32dcb0` -> `32b220` ->
virtual slot 0 of `+0x250` -> `32d840` (prologue `40 57 48 83 ec 30 48 c7 44 24
20 fe ff ff ff`, 15 bytes to `32d84f`) -> R1 per cell -> R2. Hooking `32d840`
itself is unnecessary. Who deletes the RDM, and on which thread, is [G]:
CGameUI `+0x4f0` owner at world unload.

## 7. Design analysis

### A: ownership, **safe by static analysis**

- Allocation: A1 or A2, filtered to 67,601 bytes (excludes the 1,157-byte ambient
  cells) [D].
- Frees: only `32a6f0`, always preceded by slot 2, so R1 fully handles it and R2
  backstops it [D]. No realloc path is reachable for cells [D]. R3 plus
  `Contains()` in the resize detour turn any unexpected growth into a migration
  instead of an arena free [D].
- No payload moves or copies of the pointer triple exist, so fixed addresses
  stay valid for the grid's whole life [D].
- Fault-and-restore at unchanged addresses works for every consumer: `memmove`,
  byte loops, memset and the canary compare, all user mode [D]. Concurrent pool
  workers read neighbours while other workers write their own tiles. The
  existing lock/section/alias protocol already handles concurrent read/write
  faults [D for the access pattern, per `terrain-compression.md` for the pager].
- If a free were ever missed, the aligned-header check in inline `_Deallocate`
  (`[ptr-8]` must lie within 0x27 bytes before `ptr`) fast-fails at the free.
  The terrain slot layout's 32-byte header area can hold a valid-looking value,
  but a missed free should be treated as fatal, not tolerated [D].
- Sizing: 67,601 + 32-byte header -> 17 pages = **69,632 bytes/slot**.
  25,992 tiles = 1.686 GiB of address space; 256² tiles = 65,536 slots =
  4.25 GiB; 512² = 17.0 GiB of reserve (the placeholder arena needs that much
  address space, not commit) [D arithmetic].
- Budget: streaming and full repaint fault many cells back. Size a warm
  allowance for repaint/entry, and expect camera-driven restores in steady state.
  The audit's "only on edits" assumption was wrong (section 5) [D].
- **Unverified:** (1) pattern-sweep completeness for slot-2 or inline cell
  access (G, low risk); (2) world unload/reload in one process (G);
  (3) `32ff20` full repaint under a budget (G); (4) whether the uploader vfunc
  `+0x218` retains the *result* vector pointer beyond `318f20`. That is
  irrelevant to payloads but was not examined.

### B: in-place decommit, **unknown / not recommended**

- Engine side: the same single free site. R1/R2 must **recommit and restore
  before** `32a7b8` [D]. InternDestroy's canary compare also reads the last page.
- Eviction cannot use a private alias (heap memory is not a section). The
  protocol would be: VirtualProtect READONLY -> snapshot -> VirtualFree
  MEM_DECOMMIT on whole interior pages (about 15-16 of 16.5 pages per payload).
  Writers fault and wait during the snapshot; readers fault after decommit [G:
  works, but racier than A and untested].
- Relies on NT-heap behaviour outside this exe [G]: busy-block interior pages are
  never read or written by the heap; no heap walk, validation or free-fill
  touches them; and the heap's own commit/UCR tracking never assumes those pages
  are committed. If a payload were ever freed while decommitted (a missed path),
  the heap could later hand out pages it believes committed. The result is a
  crash in *unrelated* code, not at the free.
- Restoration needs commit charge inside a heap segment. Failure there is an
  unrecoverable AV.
- Enumeration: no allocation hook is needed. Walk `[RDM+0x250]->+0x78` cells;
  the cell array is fixed and `first` changes only at InternCreate or teardown
  [D].
- Kernel exposure: none, the same as A [D].

## 8. Runtime checks before enabling (either design)

1. Log counts: arena allocations at `3303e6`, InternDestroy releases, and owned
   vectors still live when `32a6f0` returns. The expectation is
   allocations == releases and zero live at teardown, including across a world
   reload.
2. Count `1b3d90`/`1d5830` calls where `Contains()` is true and a migration would
   be needed. The expectation is zero.
3. Log faults per second while panning and zooming with a low resident budget,
   and during a triggered full repaint (`32ff20`).
4. Verify the 16-byte canary and the payload hash round-trip for evicted cells
   (debug build).

## 9. Implementation (September 15, built and tested offline, NOT deployed)

Design A, as `material_cache_compress=1` (default 0):

- `src/pager_impl.inl` is the shared pager body; `terrain_pager.h` and
  `material_pager.h` instantiate it (the terrain pager's code is unchanged).
  Material slots: 69,632 bytes, 2^19 slots (34 GiB placeholder reserve).
- `src/material_codec.h`: per-cell dense alphabet (<= 64 values), rANS
  conditioned on the previous byte, 64-bit content hash. 1,024 live cells:
  20.08% of raw, encode 250 us, decode 198 us. Projected compressed grid:
  0.33 GiB (114x228), 0.83 GiB (256x256), 3.31 GiB (512x512).
- `src/material_compression.h`: A1 allocation filter on `1d5830` (return
  `3303e6`, empty, 67,601), R1 InternDestroy replacement for owned cells
  (int32 accounting at +0x60, release, null triple; the canary is not read so
  teardown does not decompress cold cells), R2 grid-dtor sweep, R3 assign guard
  (migrate to the heap when a larger assign would reallocate). Resident target
  256 MiB, warm 1,024 MiB during world entry or within 15 s of bulk allocation.
- Runtime counters in the 30 s log line: `releases` (expected = cells at world
  teardown), `migrations` and `late_releases` (both expected 0), plus
  `slot_overflows` and `failures`.

Checks: `build.bat -matcodec-test` / `-matpager-test` (live samples, write
faults, noise fallback, both pagers coexisting, 8 workers racing eviction,
handle and backing balance), `tools/test_material_compression.py` (ownership
filter, fault restore, in-place assign, InternDestroy accounting, dtor sweep,
growth migration, exe bytes/boundaries at all five sites, hook order). Still
needed in game: world entry, panning, terrain edits, a full repaint, world
unload/reload, and the counters above.
