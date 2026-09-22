# Terrain LOD tessellation at world entry (UpdateLodTess)

Steam build 35924, executable SHA256
`782b904a8f7bbdac1f7a18528f1a5c778691e5aa3087c37c351bf6912585175c`.

Tags: **[M]** measured, **[D]** derived from code, **[G]** guess.

## Verdict

The per-tile calls are independent, and **the engine already runs them in
parallel on its own ThreadPool**. The loop is not serial. The profile line "one
thread at 86%" gives that thread's *own* share, not the load's thread count. A
plugin-side split would compete with the engine's workers for the same cores
and gain nothing, so no hook was built. `src/terrain_lodtess.h` and
`tools/test_terrain_lodtess.py` were deliberately not added, and there is no cfg
key. The work that remains is per-call cost; see Alternatives.

## Profile re-read

Source: `profile_load.py` summaries from September 15 (loading-budget build,
pid 40292, and the earlier reload, pid 60744). The tool samples every thread's
RIP once per tick. It prints per-window totals across all threads, plus the
three busiest threads' own histograms.

| Window | Ticks per thread (the win32u threads, 100% working) | `0x334c60` samples, all threads | Busiest LOD thread |
|---|---:|---:|---|
| pid 40292 t+180 | 704 | 5,241 | tid 51760: 300 working, 86% of those in `334c60` |
| pid 40292 t+160 | 797 | 936 | not in top 3 |
| pid 60744 t+120 | 705 | 5,963 | tid 57808: 361 working, 80% in `334c60` |

A thread yields at most one sample per tick. 5,241 samples against 704 ticks
therefore needs **at least 8 threads inside UpdateLodTess at once** [M]. The
other profile gives at least 9. One tick lasted about 20 s / 704 = 28 ms. The
phase therefore used roughly 6,177 x 28 ms, about 175 thread-seconds, in
`334c60` [M, approximate]. That figure includes kernel page-fault time taken at
faulting instructions inside the function: fresh vector pages, and first
touches of pager-protected heightmaps. Such time is attributed to the user RIP
[G for the size of that share]. In the same window `RecreateAmbientData`
`0x3315c0` ran at 2.1% and `AmbientCreateHeightMap` (`0x32dfbb`) at 1.1%. Only
the world-entry body `32ce50` and ValidateAmbient call both, so this phase is
the RenderDataManager constructor path [D+G]. `tpf2_bigmap.dll` (pager decode)
took 12.9% of that window. At t+200, `334c60` falls below the top-8 cut
(< 216 samples), so no second full pass followed [M].

Hardware of the profiled box: Ryzen 9 9950X3D, 32 logical processors [M].

## Call chain [D]

1. `CMenuUI::StartGame` `676480` -> `RenderDataManager` ctor `327eb0`.
   * The ctor creates "RenderDataManager Pool" (1 thread, RDM+0x40) and
     "RenderDataManager Worker Pool" (max(1, hw/2) threads, RDM+0x48); see
     `327fd8` and `3280c0`.
   * At `329a3f`/`329a61` it enqueues `lambda_4cc9dc9b` (body `33a260`) through
     `ThreadPool::Enqueue` `31e220`. The target is the pool returned by
     `2382810`: "Main Launcher" on the main thread, else "Sim Launcher". Both
     have exactly 1 thread.
   * It then runs "Initial material index generation" (`315720`) on its own
     thread. At `329b9f` it joins the future (`11b0c0`). LOD and material index
     work therefore overlap, and `0x317126` appears in the same profile window.
2. `33a260` takes a pool from `23827c0`. That is "Main Pool" when on the main
   thread or when TLS+0x1d8 is set, else "Sim Pool". Their sizes come from
   startup code `e7f0` and `e830`: `max(1, (3*hw)/4)`, i.e. **24 workers** on a
   32-thread CPU. It then calls
   `ThreadPool::LoopImpl` `320070(pool, ctx, count = W*H, blockSize = 0x60)`.
3. `LoopImpl` runs the body serially only when `count <= 96` (and asserts
   `GetThreadIndex() == 0`). Otherwise `31e7f0` splits the range into
   `ceil(count/96)` chunks. The ThreadPool ctor `2381ef0` sets pool+0xc4 = 100,
   and the chunk count is capped there. A 256x256 map (65,536 tiles) becomes
   **100 chunks of 656 tiles**. Each chunk is a packaged task (`31f3e0`). The
   caller waits on the futures in order.
4. Chunk thunk `33a5b0` -> body `32ce50(ctx, begin, end)` -> per tile
   `334c60` (tessellation on, RDM+0x2f8) or `334aa0` (off).

Every other caller of `334c60` is parallel too:

* `UpdateLodAndInvalidateRenderdata` `334bd0` -> `LoopImpl` `31fe40`: block
  0x60 on the `23827c0` pool, body `32cd00`.
  * It is reached from `3329a0` when the LOD pair at RDM+0x21c/+0x220 changes.
    The ctor sets both to 1.0. The CGameUI ctor passes (setting, 1.0) at
    `55022f`, so it re-runs every tile only when that setting is not 1.0.
  * It is also reached from `332a10` when the tessellation flag toggles.
* `ValidateLod` `338cf0` -> `Enqueue` `31f670` on the Worker Pool, block 1,
  capped at 100 chunks, body `33a5c0`.

## Per-tile independence [D]

The tile grid sits at RDM+0x58: `{x0, y0, W, H, records}`, with 0xe0-byte
records. Index `i` maps to position `(x0 + i%W, y0 + i/W)` and to record
`records + i*0xe0`. Body `32ce50` does the following for one tile:

| Step | Reads | Writes |
|---|---|---|
| `rec+0x38 = 1` | - | own record |
| `334c60(terrain, &pos, RDM+0x2fc, &rec+0x18, &rec+0x30)` | ITiledTerrain slots 0x20 (levels, CTerrain+0x38), 0x28 (resolution, CTerrain+0x28..0x38), 0x30 (tile `vector<uint16>`: ViewTerrain `34e0c0` override hash at +0x80, else CTerrain `33d580`, not decompiled; the EngineTerrain equivalent `156a10`, `CTerrain::BaseGetVertices`, is a lock-free component lookup), 0x18 (+0x3c), 0x50 (+0x40) | own `vector<TerrainTessPatch>` (14 B, resize `326110`) and own float; local `vector<float[5]>` scratch (`326330`), freed on exit |
| `349240(ambientGrid, pos)` | cell locate `3486f0` (const) | own cell vector via DataGrid slot 1 `3303b0` (resize `1d5830`, canary byte); **also `grid+0x60 += cellSize`**, shared and non-atomic |
| `348a70` -> `3315c0` RecreateAmbientData | heights of neighbouring tiles through slot 0x30 (`32def0`), bounds via `3c49c0` (slots 0x08/0x10), CPU-level global `1441cc1d0` | own ambient cell vector; local scratch vectors |
| `3495f0` | cell locate | occupancy bytes of own cell |

* **Inputs are terrain data only.** Neighbour reads go to ITiledTerrain, never
  to other tiles' RDM outputs. No tile reads another tile's output, so the
  order does not matter.
* **Allocation** uses operator new `2bf3a80` and CRT `free` only (thread-safe
  CRT heap).
* **Code inspected:** the complete call and RIP-reference set of `334c60`. The
  calls are the five vcalls, `326110`, `326330`, `free`, the delete/cookie
  helpers `2bf3abc`/`2bf3a30` and the assert `221adf0`. The RIP references are
  the stack cookie and float constants (`2f20a40`, `2f9460c`, `2f94604`,
  `2f9462c`, `2f945e8` = 0.275, `2f9461c` = 32765.0, `2f1e98c` = 1.0,
  `2f1e988` = 0.5).
* **No GPU or driver calls, and no locks,** in the body or in the terrain
  getters decompiled (`33d330`, `33d340`, `33d560`, `33d7f0`, `34e0c0`,
  `156a10`).
* **One shared write:** the counter at DataGrid+0x60, which looks like a
  memory statistic [G for its role]. Stock code already races on it from 24
  workers. Render results do not depend on it.
* **Pager:** heightmap reads may fault into the tpf2-bigmap pager. Stock code
  already faults concurrently on different tiles.

## Why no change was built

The loop already runs on 24 of 32 logical cores, split into 100 chunks. Replacing
`320070` or `32ce50` with a private pool would add threads onto cores the engine
pool already occupies, while material index generation competes for the same
cores. The expected wall-time gain is about 0 [G], and every new hook carries
risk. The task's condition for building ("replace the serial loop") does not
hold.

## Alternatives (not built)

1. **Faster UpdateLodTess per call, bit-exact.** For each interior vertex
   (63x63 on a 65x65 tile) and each simplification level (<= 5), the inner loop
   executes 5 signed 32-bit `idiv` by `r11d = 1<<k`. Three are `%` tests
   (`334f63`, `334f71`, `334f80`) and two are interpolation quotients
   (`334fc7`, `335005`). Each vertex adds one more (`334ede`).
   * About 100k divisions per tile [D].
   * Loop indices are positive, and the quotient dividends are sums of
     `uint16 * non-negative weight`. Shifts and masks are therefore exact
     replacements [D, needs test].
   * The float max/clamp order must be kept.
   * The function's call set is small, so a native original-vs-replacement test
     in the style of `test_material_index.py` is feasible. It needs a fake
     ITiledTerrain vtable and in-process copies of `326110`/`326330`, or stubs.
   * Gain per call: 2-4x on the arithmetic share [G]. The fault share is
     unknown, and the wall gain is bounded by that share and by pager
     contention.
2. **Re-profile with per-thread wait keys** for this phase. The busiest worker
   logged 300 working samples of about 704 ticks, and the rest were idle or
   waiting [M]. Whether the waits were pool idleness at the start and end of the
   phase, pager locks or heap locks decides between (1) and pager work.
   `profile_load.py` prints per-thread wait totals only in its final summary,
   and no wait keys per thread.
3. **Pool size 3/4 -> all logical cores** by patching `e7f0`/`e830`. At most
   +33% workers, applied to every engine pool including simulation work. Small
   and unmeasured [G]. Not recommended.
4. **A second full pass after load** was checked: the profile shows none at
   t+200 [M]. It is only possible when the CGameUI LOD setting is not 1.0 [D].

## Tests

None. Nothing was built. Evidence files, reproducible with
`tools/ghidra/run.ps1 DecompileTargets.java` on a project copy:
`%TEMP%\tpf2-lodtess\d1..d5` (ctor, LoopImpl/Enqueue, pool getters, ThreadPool
ctor, body, accessors), `rdis.py` (capstone), `xref.py` (RIP-relative xrefs to
the pool-size globals).

## Unverified

* Which of the three callers produced the t+160/t+180 samples. RIP-only
  profiling has no stacks. The ambient samples point to the ctor body.
* The actual pool worker counts in the running game. They are derived from
  `hw` and the code, not read live.
* How much of the 175 thread-seconds is page-fault time rather than
  arithmetic.
