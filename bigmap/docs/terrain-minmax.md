# Terrain tile publication: min/max scan and block copy (`terrain_minmax_fast`)

Steam build 35924 (`TransportFever2.exe` sha256 `782b904a…`). Two bit-identical
replacements on the save-load tile publication path, behind one config key,
default off. Files: `src/terrain_minmax.h`, `tools/test_terrain_minmax.py`.

## Evidence

20 ms IP profile of a 256x256-tile save load, September 15 2026
(`tpf2-multiplayer/tools/re/profile_load.py`, see `docs/load-speed-todo.md`).
In the t+140 s window, during tile publication, one loading thread spent

* 39 % of its samples in the pdata range `0x30a55c` (10.1 % of all samples),
* 11-24 % in `0x33cd10`, whose signature is
  `CVec2f CalcMinMaxHeight(float, const vector<unsigned short>&)`
  (`game\terrain\terrain.cpp`),
* 41 % in the plugin's own pager.

The profiler keys samples by pdata function, not by instruction, so it cannot
say which instruction inside a range is hot; the attribution below comes from
the disassembly, not from the profile.

## What the two ranges actually are

### `0x30a55c` is not a function

It is the second pdata chunk of `0x30a540` (chunks: `0x30a540-0x30a55c`
prologue, `0x30a55c-0x30a606` body, `0x30a606-0x30a610` epilogue). The whole
function is 208 bytes, has no calls and no RIP-relative operands:

```
void copy(const uint16* src, uint16* dst, int srcStride, int dstStride,
          int srcX, int srcY, int w, int h, int dstX, int dstY)
  for r < h:            // h <= 0: nothing at all
    for i < w:          // w <= 0: empty rows, no memory touched
      dst[dstY*dstStride + r*dstStride + dstX + i] =
      src[srcY*srcStride + r*srcStride + srcX + i]
```

The two `*Y * *Stride` products are 32-bit `imul` (wrapping) sign-extended to
64-bit; every other index step is 64-bit. The inner loop is
`movzx ecx,[r9+rax]; mov [rax],cx; lea rax,[rax+2]` — one uint16 per iteration,
never vectorised, because the compiler could not prove the buffers do not
overlap.

Callers (from `call_edges.csv`): `0x3c40c0` (the publication walk called by
`0x33cd10`), `sub_terrain_util::GetHeightmap 0x3ac330`,
`terrain_util::BaseGetHeightmapRefined 0x3c4620`, `terrain_util::GetBlock
0x3c4a20`, `0x316590`, `0x32ea60`. On the publication path `0x3c40c0` calls it
once per (published region, overlapping tile) pair, copying the overlap of an
alignment-worker block into that tile's height cache; a full 257x257 tile copy
is 66,049 uint16 (132 KB).

Verdict: it **is** a good target — small, pure, no calls, and a per-row
`memcpy` is provably identical whenever the source and destination byte spans
are disjoint. It is included here.

### `0x33cd10` contains CalcMinMaxHeight, inlined

`0x33cd10` itself is the publication entry (`CTerrain` height update; the
plugin's `terrain_cache.h` already hooks its first 15 bytes). It builds two
`std::function` lambdas, calls `0x3c40c0` (which does the block copies above
and collects the touched tiles), then walks the touched-tile vector and, for
each tile, runs an **inlined** copy of
`` `anonymous-namespace'::CalcMinMaxHeight `` — the two asserts inside the
range carry that `__FUNCSIG__` string, `Terrain.cpp` lines 35 and 48. A scan of
the whole `.text` for references to that string finds exactly two, both in this
function, so there is one inlined copy, not several.

Per tile (`0x33ce80..0x33cf4d`):

```
heights = tileRecord.vector          // 257x257 uint16 = 66,049 values
if (heights.empty()) assert "!vertices.empty()"
count = begin > end ? 0 : (size_t(end - begin) + 1) >> 1     // note the +1
min = max = begin[0]                 // read even when count == 0
for i in 0..count-1:                 // starts again at element 0
    v = begin[i]
    if      (v < min) min = v
    else if (v > max) max = v
fmin = float(min) * scale            // scale = [this+0x34], one mulss each
fmax = float(max) * scale
if (fmax < fmin || unordered) assert "resZ * mi <= resZ * ma"
tileRecord.minZ = fmin; tileRecord.maxZ = fmax; ++tileRecord.version
```

Details that the replacement has to preserve:

* The `else if` still yields the true unsigned min and max: `min <= max` holds
  as an invariant, so a value below `min` can never also be above `max`.
* `count` rounds **up** for an odd byte length, so the last element read can
  straddle `end` by one byte. The replacement reproduces the same read range.
* `begin[0]` is read unconditionally, including when `count == 0`.
* The float parameter is the terrain height scale at `this+0x34`
  (`cvtdq2ps` of a value in 0..65535, which is exact, then one `mulss`).
  It stays in stock code: the patch does not touch floats at all.
* NaN or a negative scale makes `comiss` set CF (unordered, or `fmax < fmin`)
  and the game aborts through the assert. That behaviour is preserved, and is
  covered by the tests, because the patch ends before the conversion.

Callers: `0xaad6c0` (from `0xaac810`), the terrain alignment worker path.

## Design

Both halves are installed by `InstallTerrainMinMaxFast()` under one key,
`terrain_minmax_fast` (default 0). Steam only: it refuses when `g_gog` is set,
and it byte-verifies all three ranges **before** writing anything:
`0x33ce80..0x33cf4d` (the whole per-tile loop, 205 bytes), `0x33cfe6..0x33d002`
(the epilogue that restores rsi, 28 bytes) and `0x30a540..0x30a610` (the whole
copy function, 208 bytes). One log line reports what ended up enabled. The two
halves are independent, so if one fails the other still runs.

### 1. The scan: a 69-byte mid-function patch

`0x33cec1..0x33cf06` (the scan only) is replaced by eleven instructions:

```
mov rcx, rdx            ; begin
mov rdx, rsi            ; end
mov rsi, r11            ; park the tile iterator in a callee-saved register
movabs rax, TerrainMinMaxScan
call rax                ; -> eax = min | (max << 16)
mov r11, rsi
movss xmm2, [r14+0x34]  ; the same reload stock does at 0x33ceab
movzx r10d, ax          ; stock's min register
shr eax, 16
mov ecx, eax            ; stock's max register
jmp 0x33cf06            ; rest of the tile is stock code
```

Why a call is safe here, rather than a function-entry hook: at `0x33cec1` the
frame of `0x33cd10` is fully established, `rsp % 16 == 0`, and `[rsp, rsp+0x20)`
is that function's own outgoing shadow area (it calls `0x3c40c0` with it), so a
call keeps the unwind data valid and clobbers nothing the function still needs.
`rsi` is free because stock reloads it at `0x33ceb1` on the next iteration and
the epilogue restores it from the frame; it is callee-saved, so it carries
`r11` across the call. `rax`, `rdx`, `r8`, `r9`, `xmm0` and `xmm1` are dead at
`0x33cf06`. The test proves that claim mechanically rather than by eye: it
walks every path out of `0x33cf06` and asserts each of those registers is
written before it is read (7 paths). This also does not collide with
`terrain_cache.h`, which steals `0x33cd10`'s first 15 bytes; the ranges are
disjoint and either install order works.

`TerrainMinMaxScan` is SSE2 only (no CPUID, no dispatch): 4x`_mm_loadu_si128`
per iteration, biased by 0x8000 so the signed `_mm_min_epi16`/`_mm_max_epi16`
order unsigned values, then an 8-lane reduction and a scalar tail. Integer min
and max are exact, so "bit-identical" needs no argument about float ordering:
the floats are computed afterwards by unmodified stock code. The function reads
only `[begin, begin+2*count)` — never a byte more, which matters because these
vectors are pager-managed.

### 2. The copy: an entry hook with an overlap fallback

`0x30a540` is hooked with the standard 14-byte steal (`push rbx; push r14;
push r15; sub rsp,0x10; mov ebx,[rsp+0x68]` — exactly 14, on an instruction
boundary, no RIP-relative bytes). The detour computes the first and last row
offsets for both sides (the products replicate the stock 32-bit wrap), and:

* `h <= 0` or `w <= 0`: return; stock touches no memory in either case.
* `w` or `h` above 2^20, or any shared byte between the source and destination
  spans: call the original. Overlap is the only case where stock's
  element-by-element forward order is observable, and the span test is
  deliberately conservative — rows that interleave without any single row
  overlapping also fall back.
* Otherwise one `memcpy` per row, in stock row order. With disjoint spans every
  read still sees the initial source, and destination rows that overwrite each
  other do so in the same order, so the result is identical.

The pager's fault handler (`pager_impl.inl`) filters on the faulting *address*,
not on the faulting instruction, and handles both reads and writes, so a fault
inside `memcpy` restores exactly as one inside the stock loop.

## Test coverage (`tools/test_terrain_minmax.py`)

Runs the **original machine code** in-process: the stock scan bytes run in a
hand-built register harness, and the stock copy function is copied verbatim
into the test process (it is self-contained). Nothing touches the game.

* Byte, boundary and CFG audit: every verified range decodes as whole
  instructions; no branch from outside targets the interior of the patched scan
  or of the stolen prologue; the copy function has no call and no RIP-relative
  operand; the patch decodes to the documented 11 instructions and jumps to
  `0x33cf06`, with `int3` padding.
* Liveness proof over all 7 paths out of `0x33cf06` (calls' argument reads are
  themselves justified from the callees' bytes; the stack-cookie routine is
  checked to touch only rcx).
* 62,634 harness comparisons of stock versus patched, each comparing r10, rcx,
  r11, rdi, rbx, r12, r14, r15, rbp, rax, xmm0, xmm1, xmm2, the whole tile
  record and the assert outcome, and each also checked against an independent
  numpy reference: n = 1..130 exhaustively (all-equal, 0, 65535, 0x7fff/0x8000,
  a low and a high outlier at **every** position), tile sizes 129², 257²±1,
  4096 boundaries with outliers at every SIMD block class, sorted, alternating
  and terrain-like data, the odd-byte and empty count formulas, and `begin >
  end`.
* Float parameter edge values: 0, -0, negative, ±inf, NaN, FLT_MAX, FLT_MIN,
  denormals and random bit patterns — including the cases that abort through
  the stock assert, where the assert-path xmm0/xmm1 and the untouched record
  are compared too.
* 4,000 direct scans against numpy min/max, and guard pages (PAGE_NOACCESS on
  both sides) proving the replacement reads no byte outside stock's range.
* Block copy: 3,011 randomised geometries plus the publication/GetBlock shapes,
  self-overlapping rows, negative and zero strides, int32-wrapping products —
  full destination buffers compared and the source verified unchanged; null
  buffers for `w <= 0`/`h <= 0`; every aliasing shape (forward smear, odd byte
  offset, identical pointers, interleaved rows) and the >2^20 bounds confirmed
  to fall back to the original while exactly adjacent spans do not; guard pages
  on both sides of both buffers.
* Installer: preflights all three ranges before patching, refuses when
  disabled, on GOG and on each byte mismatch, emits exactly one log line, and
  installs one half when the other fails.

Full run: all 14 PASS lines, no failures.

## Benchmark

AMD Ryzen 9 9950X3D, warm cache, one core, native loops (no ctypes cost):

| Work | Stock | Replacement | Ratio |
|---|---|---|---|
| CalcMinMaxHeight, 257x257 random | 36.7 us | 1.51 us | 24.3x |
| CalcMinMaxHeight, 257x257 terrain-like | 35.5 us | 1.48 us | 24.0x |
| CalcMinMaxHeight, 257x257 flat tile | 35.3 us | 1.49 us | 23.7x |
| Block copy 257x257, warm | 13.4 us | 1.13 us | 11.8x |
| Block copy 257x257 into never-touched pages | 33.4 us | 25.7 us | 1.30x |

The last row is the honest ceiling for the copy: when the destination pages
have never been touched, kernel first-touch faults dominate and no user-mode
copy can remove them.

## What is NOT verified

* **In-game load time.** Nothing here has run inside the game. Both halves are
  off by default and need a measured save-load comparison (stdout "Loading from
  file" to "Initial material index generation") before anyone enables them.
* **Call counts per load.** How many publication calls and touched tiles a
  256x256 save produces is not measured; the profile only shows the shares
  quoted above. The per-call costs above therefore cannot be turned into a
  predicted second count.
* **How much of the profiled 39 %/11-24 % is CPU versus faults.** The profiler
  attributes kernel first-touch and pager-restore time to the faulting user
  instruction, and the tile caches are pager-managed. The warm numbers are an
  upper bound on what the replacements can remove; the cold-page row suggests
  the copy's real-world share is smaller than 11.8x.
* GOG: refused outright, never tested there.
