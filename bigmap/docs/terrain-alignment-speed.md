# Terrain alignment: `CalculateHeightMod` (Steam 35924)

`terrain_align_fast=1` replaces `terrain_alignment_util::CalculateHeightMod`
(RVA `0x3b3470`, `game\terrain\terrain_alignment_list_util.cpp`) with a
bit-identical faster implementation. Zero (the default) restores stock code on
the next process launch. This item does **not** touch
`sub_terrain_util::InternBicubicRefine` (`0x3ac6c0`, `src/terrain_refine.h`).

## Evidence

20 ms instruction-pointer profile of a 256x256-tile save load, September 15
(`tpf2-multiplayer/tools/re/profile_load.py`; see `docs/load-speed-todo.md`). In
the t+120 s window, two pdata chunks of the terrain alignment file together held
~11% of all samples:

| Chunk | Size | Share | What it is |
|---|---|---|---|
| `0x3b3470` | 2153 B | 5.2-5.4% | `CalculateHeightMod` itself |
| `0x3af850` | 504 B | 5.3-5.6% | `std::vector<unsigned short>::_Resize(n, value)` |

`0x3af850` is reached **only** from `CalculateHeightMod` (`0x3b3671`, `0x3b36b6`)
and from the target constructor `0x3b0190` (`0x3b027e`, `0x3b02b4`), which in
turn is called only by `CalculateHeightMod` — so both chunks are one call site's
cost. `call_edges.csv` confirms the callers of `0x3b3470` are the thread-pool
worker `0xaac460` (`ecs::TerrainAlignmentSystem::UpdateSubterrains`, via
`ThreadPool::LoopImpl` `0xaab290`) and `0x2146540`.

## What the stock function does

```
void CalculateHeightMod(const Box2& box, const CVec2i& size, float scale, float offset,
                        const vector<TerrainAlignment const*>& alignments,
                        vector<unsigned short>& result)
```

`result` holds N = `size.x * size.y` quantised heights (`height = word * scale +
offset`); the stock code asserts that up front. Then, per call:

1. Three rasterisation targets are built — two `PredHeightModRasterizable` with
   the `<=` predicate (vtable `0x2fb64c0`, predicate `0x3b7440`, heights filled
   with `0xffff`, i.e. "keep the lowest") and one with the `>=` predicate (vtable
   `0x2fb64d8`, predicate `0x3b6ee0`, heights filled with 0, "keep the highest").
   Each target owns two `vector<uint16>` of N words: a height and a weight.
   That is six heap blocks and six fills, all through `0x3af850`.
2. Every triangle of every alignment is written into its target's scratch fields
   and rasterised (`0x2375420` sets up the grid mapping, `0x23754b0` walks the
   rows and calls the predicate per span). An alignment with an empty weight
   vector uses a scratch vector of `(1,1,1)` weights. `TerrainAlignment` is read
   at +0x00 (triangles, 36 B each), +0x18 (per-vertex weights, 12 B each) and
   +0x30 (target index 0, 1 or 2).
3. A scalar pass over all N samples: where `wA + wB + wC != 0`, it derives
   `lower`/`higher` bounds from the result height and the three target heights,
   drops the weights whose bound did not win, drops a second weight of exactly
   1.0, forms three products, and writes
   `floor(((lower*wB + wA*hA + higher*wC) / total - offset) / scale + 0.5)`.

## What dominates the cost

Both hot chunks are **work proportional to the whole block, not to the geometry
on it**:

* `0x3af850`'s fill loop is one word per iteration (`movzx eax,[r8]` /
  `mov [rcx],ax` / `add rcx,2` / `dec rdx`), re-reading the fill value from
  memory every iteration, run six times over N words — 396k stores for a 257x257
  1 m tile block — plus six `malloc`/`free` pairs of ~132 KB each, whose first
  touch faults in fresh pages.
* `CalculateHeightMod`'s own hot loop is the final pass: it reads three weight
  words and the result word for **every** sample, even though on a realistic
  tile only a few percent of samples carry any alignment weight (the benchmark
  below: 2130 of 66049 for a block with six road strips).

The rasterisation in between is a third cost, but it lives in other pdata chunks
(`0x23754b0`/`0x2375664`, listed as unattributed hot ranges in
`docs/load-speed-todo.md`) and is **not** touched here.

## Design

`src/terrain_align_fast.h` hooks `0x3b3470` (21-byte prologue) and reimplements
the orchestration while keeping every float-producing engine routine:

* **Pooled scratch.** The six vectors become one pooled buffer of 6N words,
  reused across calls (a lock-protected free list; one entry per concurrent
  worker, grown to the largest block seen, minimum 257x257). Resetting it is two
  `memset`s — `0xff` over the two `<=` height planes, zero over the other four —
  which writes exactly the same N words with exactly the same values as the six
  stock fills. No allocation, no page faults after warm-up.
* **Original rasterisation.** The three targets are built on the stack byte for
  byte as `0x3b0190` builds them (vtable, +0x38 sizes, +0x40 scale, +0x44
  1/scale computed with the same `divss`, +0x48 offset, the two vectors pointing
  into the pooled buffer, everything else zero), and the stock `0x2375420` and
  `0x23754b0` are called with them. Triangles and weights are copied into the
  target with `memcpy` in the stock field order, and the three vertex pairs are
  passed in the same registers.
* **Blended eight at a time.** The final pass tests eight samples with one
  `por`/`pcmpeqw`/`pmovmskb` — `wA + wB + wC == 0` is exactly `(wA|wB|wC) == 0`
  for unsigned words — and only evaluates the 4-wide SSE2 kernel for chunks that
  carry weight.

## Why it is bit-identical

Each float value is produced by the same IEEE single-precision operation on the
same operands, in the same grouping, as the stock code:

* `mulps`/`addps`/`subps`/`divps` are the per-lane equivalents of the stock
  `mulss`/`addss`/`subss`/`divss`; the association of every expression is copied
  from the disassembly (`((wB + wA) + wC)`, `((lower*wB + wA*hA) + higher*wC)`,
  `((1-wB)*wA)*(1-wC)`, …). Multiplication and addition operands are only ever
  swapped where IEEE makes them commutative. No FMA (the build targets SSE2, no
  `/arch:AVX2`), no reassociation, no `/fp:fast`.
* The stock branches are `comiss`+`ja`/`jbe` and `ucomiss`+`jp`/`je`, i.e.
  *ordered* greater-than and *ordered* equality. `cmpgt_ps`/`cmpeq_ps` have the
  same unordered outcome, so NaN operands select the same side as the stock
  fall-through. Selection is `and`/`andnot`/`or` on the mask, so the taken value
  is bit-preserved (including the sign of zero, which the stock `cmova`/`movaps`
  also preserves).
* `floorf` + `cvttss2si` is reproduced without calling the CRT:
  `t = cvttps_epi32(v)` (trunc toward zero), minus one where `v < (float)t`
  (`(float)t` is exact: for |v| >= 2^24 a float is already integral, below that
  the integer is representable), and left untouched where `t` is the
  `0x80000000` integer-indefinite of NaN, ±inf and out-of-range values — for
  which `floorf(v) == v` and the stock conversion also yields `0x80000000`. Only
  the low 16 bits are stored, exactly like the stock `mov word ptr [rbx+rsi*2], ax`.
* Weights are `word / 65535.0f` with word in 0..65535, so they are always in
  [0,1] and never NaN, whatever the alignment data contains.
* **The `totalW > .0f` assert is unreachable**, so its absence from the fast path
  changes nothing: reaching the weight products requires `(wB + wA) + wC != 0`,
  and after the "exactly 1.0" zeroing at most one of the three is 1.0, so at
  least one product is a product of strictly positive factors (each at least
  1/65535, so no underflow: (1/65535)^3 ≈ 3.6e-15, far above FLT_MIN). The
  replacement still detects the condition, stores the samples before it exactly
  as the stock loop would, and then calls the engine's assert (`0x221adf0`) with
  the stock message, file, line `0x4d7` and function string.
* Lanes that the stock code would skip are computed anyway but never stored, and
  their divisor is forced to 1.0, so no divide-by-zero is introduced where stock
  would not divide. Tail blocks are padded with zero weights, which are skipped
  by the same test. Sticky MXCSR status flags may therefore be set by lanes the
  stock code would not have evaluated; nothing in the game reads them, and the
  exception masks are untouched.
* Rounding-mode independence is not assumed: both paths run under the same
  MXCSR, and the tests repeat under round-down, round-up and round-to-zero.

**Fallbacks** (the original runs, with arguments untouched, before anything is
written): `size.x*size.y != result.size()` (the stock assert), a block side < 2
(the stock grid mapping divides by `size-1`), more than 1<<20 samples, an
alignment list whose byte length is not a whole number of pointers, a triangle
vector with `last < first` or more than `INT_MAX` triangles, an alignment type
outside 0..2 *that the stock code would actually index* (a bad type on an empty
triangle vector is never read, so it stays on the fast path), and a failed
scratch allocation.

**Thread-safety.** Workers call this concurrently. The only shared state is the
scratch free list behind an `SRWLOCK`; targets, rasterisers and all indices are
per-call stack data.

**Installer.** Steam only (refuses on GOG), one log line, and it refuses unless
the 21-byte prologue, 2978 bytes of the function body, the target constructor,
the resize it replaces, the three float constants (`65535.0f`, `1.0f`, `0.5f`)
and the assert message all match, plus both predicate vtables checked against
the running module base (they hold relocated pointers).

## Tests

`tools/test_terrain_align_fast.py` maps TransportFever2.exe at its preferred base
`0x140000000` inside the test process and resolves only its CRT imports, so the
stock `CalculateHeightMod`, its constructor, the stock resize (real `malloc`),
the rasteriser and both predicates all run as **original machine code**; the
replacement calls that same mapped rasteriser. Complete result buffers plus
guard words on both sides are compared.

```
PASS: engine tile blocks 257x257/129x129/65x65, 8 triangle shapes x 6 weight patterns per alignment type (144 cases)
PASS: identical geometry in all three targets (height ties, weight 1.0 branches)
PASS: random block shapes 2..199, cell sizes, scales (incl. negative) and offsets
PASS: empty alignment lists, empty triangle vectors, geometry entirely outside the block
PASS: rounding modes down/up/chop
PASS: 1374 native original-code comparisons, 12868596 block samples, 12523 triangles,
      2755120 samples changed by the stock blend; complete result buffers and their guard words identical
PASS: stock assert, blocks with a side < 2, oversized blocks, unindexable alignment types,
      malformed vectors -- forwarded to the original untouched; an unread bad type stays fast
PASS: 16 threads x 20 calls sharing the scratch pool, all identical
PASS: installer verifies the 21-byte prologue (9 whole instructions, no RIP/branches), 2978 bytes of
      body/constructor/resize/constants and both predicate vtables at the running base;
      disabled, GOG, each mismatch and a hook failure refuse
```

Triangle shapes cover road/track strips crossing the block, construction blobs,
full coverage, vertices exactly on sample positions, sub-cell and degenerate
(zero-area, collinear, shorter than the rasteriser's 1e-5 edge cut-off),
geometry entirely outside the block, and triangles straddling each block edge;
weights cover none (the stock `(1,1,1)` scratch path), all ones, the 0 / 1 /
1-1/65535 boundaries, ramps, random and out-of-range values.

## Benchmark

Warm native micro-benchmark on a 257x257 block (the 1 m tile block), per call,
best of 5 rounds of 20 calls, ctypes overhead included in both:

| Block content | Stock | Fast | Speedup | Samples changed |
|---|---|---|---|---|
| no alignments | 126.3 us | 27.1 us | **4.67x** | 0 / 66049 |
| 6 road strips | 159.2 us | 61.2 us | **2.60x** | 2130 / 66049 |
| fully covered | 569.3 us | 362.2 us | **1.57x** | 62014 / 66049 |
| 120 small footprints | 799.8 us | 531.8 us | **1.50x** | 46993 / 66049 |

The "no alignments" row isolates what this change removes: 126 us of allocation
and word-at-a-time filling plus the dense skip pass becomes 27 us of two memsets
and a vector skip pass. The dense rows are dominated by the *unchanged* original
rasteriser, which is why their ratio is lower.

## Unverified

* **No in-game measurement yet.** The load-time effect of `terrain_align_fast=1`
  on a 256x256 save has not been measured; the profile share (~11% for the two
  chunks) and the micro-benchmark are the only evidence so far. Measure before
  enabling by default.
* The `>= 1<<20` samples and side `< 2` limits are precautions; no observed block
  hits them (a 1 m tile block is 257x257 = 66049).
* Scratch memory is retained for the life of the process: 6 x 2 bytes x the
  largest block, per concurrent worker (~0.8 MB each for 257x257).
* The second caller (`0x2146540`) was read but not exercised in the game; it
  passes the same block shapes, so it takes the same path.

## Integration

```cpp
// src/bigmap.cpp, with the other feature headers (after "terrain_refine.h")
#include "terrain_align_fast.h"

// in Tpf2mpPluginInit, with the other cfg reads
g_terrainAlignFast = H->cfgBool("tpf2_bigmap", "terrain_align_fast", 0) != 0;

// with the other installers
installed += InstallTerrainAlignFast();
```

```ini
# cfg/tpf2_bigmap.cfg
# Steam 35924, experimental: pooled scratch and an SSE2 blend for terrain
# alignment (terrain_alignment_util::CalculateHeightMod, re-applies road, track
# and construction terrain cuts to each height-cache block on load and on every
# alignment update). Bit-identical to stock by construction and by test
# (tools/test_terrain_align_fast.py); 0 = stock.
terrain_align_fast=0
```
