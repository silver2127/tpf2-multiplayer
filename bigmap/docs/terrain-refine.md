# Terrain height refinement: bit-identical fast path

`terrain_refine_fast` (default 0) replaces `sub_terrain_util::InternBicubicRefine`
(Steam 35924, RVA `0x3ac6c0`) with an SSE2 loop that produces the same bytes as
stock for every input. Source: `src/terrain_refine.h`. Test:
`tools/test_terrain_refine.py`. Working notes, listings and decompiles:
`%TEMP%\tpf2-refine`.

## Why

A 20 ms instruction-pointer profile of a 256x256-tile save reload put 11.7% of
all samples in a 20 s window inside this function (pdata chunk
`0x3ac811..0x3ace5e`). It refines the 4 m base heightmap into CTerrain's
257x257 uint16 height cache for every tile. Construction and terrain queries
read those heights, and multiplayer peers must agree, so an approximate result
is not acceptable.

## Evidence

| RVA | What |
| --- | --- |
| `3ac6c0` | `void sub_terrain_util::InternBicubicRefine(int, const std::vector<uint16_t>&, int, int, int, int, int, CVec3f, uint16_t*, int, int, int)` (assert signature, `game\terrain\sub_terrain_util.cpp`) |
| `3c483e` | the only call: a raw E8/E9 scan of `.text` finds one site, in `terrain_util::BaseGetHeightmapRefined` (`3c4620`) |
| `3c4620` | called from `33d090` (EngineTerrain wrapper) <- `aac460` (`TerrainAlignmentSystem::UpdateSubterrains` body, run by `ThreadPool::LoopImpl` `aab290` and its worker lambda `aadf70`), and from `2146540` (construction comparison) |
| `2fadc0` | `CMat4f` product, a leaf with no RIP references, called twice per cell |
| `2bf3a30` | `__security_check_cookie` |
| `221adf0` | assert handler (six sites, all before any loop state) |

Arguments as `3c4620` passes them (decompile `base_get_heightmap_refined.c`):

- `k = 1 << (highLevels - baseLevels)`: 4 for the stock 1 m cache (8 - 6), 2 for
  the 2 m cache (7 - 6). `terrain_cache.h` only ever selects 7 or 8.
- `src` is a `W x H` block from `terrain_util::GetBlock` (`3c4a20`) with a
  one-sample border: `W = ((x1o-1+k)/k + 1) - (x0o/k - 1) + 1` for the caller's
  output rectangle.
- `x0 = y0 = 0`, `x1 = W-1`, `y1 = H-1`, `stride = W*k`, `dx = dy = 0`.
- `out` is a fresh `(W*k) x (H*k)` uint16 vector; `30a540` then crops the
  caller's rectangle out of it.
- `CVec3f` (terrain `+0xc..+0x14`) is passed by pointer and never read.
- For a whole tile: k=4, W=H=67, 65x65 cells into a 268x268 buffer. At 2 m:
  k=2, W=H=131.

The function runs concurrently on ThreadPool workers. The replacement has no
mutable state; it only reads the trampoline pointer set at install.

## Stock semantics (from the machine code)

Stock asserts `k > 1 && k % 2 == 0`, `x0 >= 0`, `y0 >= 0`, `x1 <= srcDim`,
`x1 >= x0` and `y1 >= y0`.

For each `y` in `[y0, y1-1)` and `x` in `[x0, x1-1)`, in that order:

1. It reads the data pointer from the vector again, then the samples
   `a[r][c] = src[(y+r)*srcDim + x + c]` for `r, c` in 0..2. Each is loaded with
   `movzx`, `movd` and `cvtdq2ps`, which is exact. The indices use stock's own
   integer arithmetic: 32-bit products, sign-extended, then 64-bit row steps.
2. It builds the 16-entry G matrix. The operand order below comes from a
   symbolic trace of `0x3ac920..0x3acba0` (`trace_refine.py`):

   ```
   G0  = (((a01+a00)+a10)+a11)*0.25   G1  = (((a02+a01)+a11)+a12)*0.25
   G2  = ((a01-a00)+(a11-a10))*0.5    G3  = ((a02-a01)+(a12-a11))*0.5
   G4  = (((a11+a10)+a20)+a21)*0.25   G5  = (((a12+a11)+a21)+a22)*0.25
   G6  = ((a11-a10)+(a21-a20))*0.5    G7  = ((a12-a11)+(a22-a21))*0.5
   G8  = ((a10-a00)+(a11-a01))*0.5    G9  = ((a12-a02)+(a11-a01))*0.5
   G10 = (a11-a10)-(a01-a00)          G11 = (a12-a11)-(a02-a01)
   G12 = ((a20-a10)+(a21-a11))*0.5    G13 = ((a22-a12)+(a21-a11))*0.5
   G14 = (a21-a20)-(a11-a10)          G15 = (a22-a21)-(a12-a11)
   ```

3. It computes two matrix products. `2fadc0(out, A=rdx, B=r8)` computes, as
   packed `mulps`/`addps` per output row, `R[r][c] = ((B[r][0]A[0][c] +
   B[r][1]A[1][c]) + B[r][2]A[2][c]) + B[r][3]A[3][c]`.
   - First call: `T = product(A=M1, B=G)`. Second call: `C = product(A=T, B=M2)`.
   - M1 is at `2fa7e30/2fa7e50/2fa7e10/2fa7e00`, rows `(1,0,-3,2) (0,0,3,-2) (0,1,-2,1) (0,0,-1,1)`.
   - M2 is at `2f20ba0/2f20a70/2fb4710/2fb4700`, and is M1 transposed.
4. For each `i` in `[0, k)`, it computes `t = float(i)/float(k)` (`cvtsi2ss`,
   `divss`), then `t2 = t*t` and `t3 = t2*t`. Each lane is
   `P[c] = ((C[1][c]*t + C[0][c]) + t2*C[2][c]) + C[3][c]*t3`.
5. For each `j` in `[0, k)`, it computes `s` the same way (`cvtdq2ps`, `divss`)
   and `v = ((P[1]*s + P[0]) + s2*P[2]) + P[3]*s3`.
6. It stores `(uint16_t) cvttss2si(v)`, which truncates without saturation (an
   out-of-range value would give `0x80000000`, stored as 0). The destination is
   `out + 2*sext32((k*y + k/2 + dy - k*y0)*stride + k*x + k/2 + dx - k*x0) + i*2*stride + 2*j`.

All operations are scalar SSE except the products. There is no FMA and no x87,
so results depend only on MXCSR, which stock and the replacement share.

The exact weight of each output on its nine samples is non-negative and sums to
1 for k = 2, 4, 8, 16 and 64 (`weights.py`: min 0, max 0.5625). A u16 input
therefore never produces a value outside 0..65535 beyond float rounding, and
truncation is the only rounding step that matters. The numpy model in the test
also counted 0 out-of-range raw values.

Subnormals cannot occur, so the DAZ and FZ flags cannot matter. Every
coefficient is a non-zero multiple of 0.25 or exactly zero, and
`t3 >= (1/64)^3`, so the smallest non-zero product is about 1e-6. A non-zero
float sum or difference is at least one ulp of its operands, about 1e-13 here.
Both are far above 1.2e-38.

## Replacement

`BicubicRefineImpl` keeps the stock loop order and index arithmetic, including
32-bit wrap and sign extension. It changes three things:

1. **Division hoisting.** `t, t2, t3` for `0..k-1` are computed once per call,
   under the caller's MXCSR, with the same `cvtsi2ss`, `divss` and `mulss`.
   Stock recomputes them for every row and pixel of every cell: 20 `divss` per
   cell at k=4.
2. **Products inlined with only 0 and 1 terms dropped.** Terms such as
   `T[r][0] = ((G0*1 + G1*0) + G2*0) + G3*0` become `G0`, and `x*(-1)` followed
   by `+` becomes `-`, which IEEE defines identically. For finite operands,
   `x*0` is a zero and `y + (±0)` is `y` for any non-zero `y`. So a drop can only
   change the sign of a zero, and `+`, `-`, `*` and `cvttss2si` never let that
   sign reach a non-zero result or an output byte. No other constant is folded:
   `x*3`, `x*(-2)` and so on remain multiplications. Negation is not folded
   either, because it is not symmetric under directed rounding.
3. **Packing.** Independent scalar operations with identical shape run 4-wide.
   Per lane, `mulps/addps/subps` are exactly `mulss/addss/subss`, and IEEE `+`
   and `*` are commutative bit for bit, so `a01+a00` and `a00+a01` are the same
   operation. The per-lane operand order is written out in the header:
   - G uses lanes `(G0,G1,G4,G5)`, `(G2,G3,G6,G7)`, `(G8,G9,G12,G13)` and
     `(G10,G11,G14,G15)`.
   - T is computed per column over the four G rows, then transposed.
   - C is computed per row over four T columns.
   - P uses four lanes per `i`.
   - Pixels use four `j` per row. `cvttps2dq` then keeps the low 16 bits via
     `pslld`, `psrad` and `packssdw`, stored 4 words at a time, with a 2-word
     tail when `k % 4 == 2`.

Everything is explicit SSE2 intrinsics built with the existing `cl /O2`
(`/fp:precise`, no `/arch`), so no CPUID check is needed. The /FAs listing
shows no calls and no FMA in the loop.

**Aliasing.** Each cell reads the vector's data pointer and its nine samples
before writing any of its pixels, as stock does. Output that overlaps the source,
or overlapping writes from a small stride, therefore behave like stock. The test
exercises both.

**Fallback.** The six stock assert conditions, and any `k > 64` (the table
size), call the original through the trampoline with the arguments unchanged,
so an invalid call still asserts inside the game.

**Install.** Steam only: the plugin already refuses unknown builds, and this
feature refuses when `g_gog` is set. The installer then checks the bytes in two
stages:

1. It verifies the 20-byte prologue `40 55 53 41 57 48 8d ac 24 c0 fd ff ff 48 81 ec 40 03 00 00`,
   which is five whole instructions with no RIP references or branches.
2. It verifies 2,815 more bytes: the rest of the function body, the product
   `2fadc0`, and every RIP-relative constant (`0.5f`, `0.25f`, both matrices).

It then installs a 20-byte hook and logs one line. Any mismatch or hook failure
logs and leaves stock code in place.

## Equivalence test (`tools/test_terrain_refine.py`)

The test copies the original function and its product into RWX memory in the
test process, relocating everything they reference:

- RIP constants, placed 16-byte aligned for `movaps`.
- The cookie.
- The call targets: product to the copied product, cookie check to `ret`, and
  asserts to a stub that counts and leaves through the stock epilogue `3ace6e`.

It asserts that the expected call and RIP sets are exactly what it relocated,
and that all branch targets stay inside the function. Both implementations run
on identical arenas, and the complete arenas must be byte-identical, including
unwritten words.

Result on this build:

```
PASS: stock tile shapes, 11 patterns each; numpy float32 model agrees (0 raw values outside 0..65535, as the convex weights predict)
PASS: engine-shaped calls, k in 2..64 powers, random block sizes (1269 cases so far)
PASS: arbitrary offsets, borders, strides and every even k (3769 cases so far)
PASS: output buffer overlapping the source samples
PASS: rounding modes down/up/chop
PASS: 4315 native original-code comparisons, 159270080 refined samples, 81 also against the numpy model; complete buffers identical
PASS: all six stock assert conditions and k > 64 forward the untouched arguments to the original; boundary-valid inputs stay on the fast path
PASS: installer verifies the 20-byte prologue (5 whole instructions, no RIP/branches) and 2815 bytes of body/product/constants; disabled, GOG, each mismatch and hook failure refuse
```

Coverage:

- **Data patterns:** zero, all 0xFFFF, x/y/diagonal ramps, checker, stripes,
  random, spikes, random-walk terrain, and boundary values
  `0,1,2,32767,32768,65533..65535`.
- **Shapes:**
  - Stock tile shapes (k=4 at 67x67, k=2 at 131x131), also compared against an
    independent numpy float32 model that keeps the zero terms.
  - Engine-shaped calls for k = 2..64 in powers of two, with random block sizes.
  - 2,500 arbitrary valid calls over every even k up to 64: offsets,
    `x1 == srcDim` (reads into the next row), empty loops, strides smaller than
    k (overlapping writes), negative dx/dy.
  - 300 calls whose output overlaps the source.
- **Rounding:** MXCSR down, up and chop.
- **Fallback:** k of 0, 1, 3, -2 and INT_MIN, x0 or y0 of -1,
  `x1 > srcDim`, `x1 < x0` and `y1 < y0` all reach the fallback with identical
  arguments, and the relocated stock code asserts on each one. Boundary-valid
  inputs stay on the fast path and match. k = 66, 128 and 256 also use the
  fallback.
- **Installer:** the prologue instruction boundaries, every verified region
  against the exe, and the disabled, GOG, per-region mismatch and hook-failure
  paths.

`tools/test_material_index.py` and `tools/test_world_entry.py` still pass.

## Benchmark

Warm-cache native calls from the same test (ctypes overhead included in both,
best of 5x200 calls), AMD Ryzen 9 9950X3D, one thread:

| Input | Stock | Fast | Per cell | Speedup |
| --- | --- | --- | --- | --- |
| 1 m tile, k=4, 67x67, terrain | 244.9 us | 71.8 us | 58 -> 17 ns | 3.41x |
| 1 m tile, k=4, random | 246.3 us | 73.0 us | 58 -> 17 ns | 3.37x |
| 2 m tile, k=2, 131x131 | 648.0 us | 222.5 us | 39 -> 13 ns | 2.91x |
| k=8, 35x35 | 128.3 us | 39.2 us | 118 -> 36 ns | 3.28x |

At the profiled 11.7% share, a 3.4x faster function would save roughly 8% of
CPU samples in that loading window. That is an estimate, not a measurement.

## Not verified

- It has not been run in the game. The in-game load time, the profile share
  after the change, and the log line have not been observed.
- It has not been checked whether other callers of `3c4620` (construction
  comparison `2146540`) matter to the load profile. They use the same function
  and the same proof applies.
- GOG is not supported (the option is refused there).
- Remaining exact micro-optimisations were left out to keep the proof small:
  preloaded broadcast t vectors, the `t = 0` row reducing to `C[0]`, and a k=4
  specialisation. Per-cell work is now about 17 ns, versus 58 ns stock.
- Equivalence for all 2^144 sample combinations rests on the op-by-op argument
  above. The test is strong evidence, not an exhaustive proof.
