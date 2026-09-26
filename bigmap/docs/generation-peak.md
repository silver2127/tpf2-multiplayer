# Terrain generation peak memory (Steam 35924)

Read-only reverse engineering, September 15, 2026. No game was launched, attached
to or modified; nothing under the Steam directory was written. Executable SHA256
`782b904a8f7bbdac1f7a18528f1a5c778691e5aa3087c37c351bf6912585175c`.
All addresses are RVAs relative to `0x140000000`.

Every number is labelled:

- **MEASURED**: read out of the game's own stdout logs in
  `<userdata>\1066780\local\crash_dump\*stdout*.txt`.
- **DERIVED**: computed from decompiled code. Correct if the decompilation
  reading is correct, but not observed in a live process.
- **GUESS**: an inference the code does not pin down (compression ratios,
  achievable floors, savings of changes that were not built).

This document covers only the `TerrainToolkit` named float maps, which are the
binding constraint when *creating* a new map. Runtime (post-entry) structures are
in `runtime-memory-audit.md`; the 257x257 height cache is in
`terrain-compression.md`. The per-layer op semantics this builds on are in
`generation-op-semantics.md`.

## Summary answers

1. **A "map" is one full-map `std::vector<float>` at 4 m spacing**, exactly
   `w*h` elements with `w = h = 64 * tiles + 1`. DERIVED from code, MEASURED fit
   exact on 7 independent log lines.
2. **All N maps are alive simultaneously.** The printed count is the *live*
   element count of the toolkit's container, and the container has no erase
   path: it is only ever cleared wholesale at teardown. DERIVED, with an
   independent MEASURED cross-check.
3. The peak is therefore *not* lower than the printed law suggests. It is
   slightly **higher**, because two layer ops allocate additional full-size
   scratch rasters that the printed number does not count.

## The printed law

MEASURED, the message text and its two companions come from `0x312400`:
`"* Terrain toolkit used "` (`0x2f8cf00`), `" maps and "` (`0x2f8cef0`),
`" MB...\n"` (`0x2f8cee8`), `"  Pipeline took: "` (`0x2f8ced0`).

DERIVED, `0x312400` is the `TerrainToolkit` teardown. It computes the printed
size as

```
MB = (int64)h * (int64)w * (uint64)N * 4 / 1000000
```

with `w` = int32 at toolkit+0x00, `h` = int32 at toolkit+0x04, `N` = uint64 at
toolkit+0xC0, `4` = `sizeof(float)`. The divisor is `1000000`, so the printed
unit is **decimal MB, not MiB**, and the division truncates. The whole print is
gated on a bool at toolkit+0x168.

MEASURED verification of `w = h = 64*tiles + 1` against real logs. Every line
reproduces exactly, with no fitted parameter:

| tiles | w=h | N | printed MB | computed MB |
|---:|---:|---:|---:|---:|
| 96 | 6,145 | 10 | 1510 | 1510 |
| 192 | 12,289 | 15 | 9061 | 9061 |
| 224 | 14,337 | 10 | 8221 | 8221 |
| 256 | 16,385 | 15 | 16108 | 16108 |
| 320 | 20,481 | 10 | 16778 | 16778 |
| 384 | 24,577 | 15 | 36241 | 36241 |
| 512 | 32,769 | 18 | 77314 | 77314 |

DERIVED, the spacing: `0x313b80` builds the toolkit extent as
`((tilesX - originX) + 1) * 0x40` by `((tilesY - originY) + 1) * 0x40` and
converts tile indices to world units with a `256.0` multiplier. 64 samples per
256 m tile is **4 m spacing**, and the `+1` makes the grid corner-inclusive.

So one map costs `62,500 cells * 4 B = 250 kB per km2`, and the original
hypothesis is confirmed exactly: at N=10 that is **2.500 MB/km2**.

| | per map | N=10 | N=15 | N=18 |
|---|---:|---:|---:|---:|
| 256x256 (w=16,385) | 1.0739 GB | 10.74 GB | 16.11 GB | 19.33 GB |
| 512x512 (w=32,769) | 4.2952 GB | 42.95 GB | 64.43 GB | 77.31 GB |

MEASURED: N tracks the generator in use. Desert stock prints `18 maps`;
with `mod/generation/bigmap_memory.lua` installed (18 -> 15 named buffers) the
same map prints `15 maps`; Temperate and Tropical print `10 maps`. Logs also
contain `16 maps` lines, from an intermediate state of that pass.

## What a map is

DERIVED. The container at toolkit+0xB0 maps `std::string` name to an
`IHeightmap*` (vftable `0x2f84cc0`). There are two implementations:

| class | vftable | size | layout |
|---|---|---:|---|
| `HeightmapNew` | `0x2f8ce98` | 0x28 | vftable, `std::vector<float>` at +0x08, `CVec2i` dims at +0x20 |
| `HeightmapView` | `0x2f8ce60` | 0x18 | vftable, `float*` at +0x08, `CVec2i` dims at +0x10 |

Only `HeightmapNew` owns storage. Every layer dispatcher (`0x39f650` feature,
`0x39fa60` op, `0x39f790` mix, `0x39f910` mix-three) resolves a name to an
`IHeightmap*` and then calls **vtable slot +0x28** to obtain the
`std::vector<float>&` it hands to the layer body.

### Toolkit layout (DERIVED)

| offset | field | evidence |
|---|---|---|
| +0x00 | int32 `w` | `0x312400` print; `0x316f70` allocation count |
| +0x04 | int32 `h` | same |
| +0x08 | `CVec2i` extent `(tiles+1)*64` | `0x313b80` builds it, ctor stores it |
| +0x40 | `std::string` (size +0x50, capacity +0x58) | ctor `0x310e20` |
| +0xA8 | an extra `IHeightmap*`, released at teardown | `0x312400` |
| +0xB0 | name -> `IHeightmap*` container | cleared by `0x3113d0` |
| +0xB8 | container end sentinel | compared in `0x316f70` |
| **+0xC0** | **uint64 live element count = printed N** | incremented in `0x3128a0` |
| +0xF0..+0x110 | `CClock` / `CStopWatch` fields | ctor and teardown |
| +0x118 | mutex | `_Mtx_init_in_situ` in ctor, `_Mtx_destroy_in_situ` in teardown |
| +0x168 | bool, enables the print | `0x312400` |

### Allocation sites

DERIVED. There are exactly **two** sites that create an owning map, and both
build the identical object:

1. **`TerrainToolkit::GetOrCreate`, `0x316f70`.** Locks the mutex at +0x118,
   looks the name up in +0xB0, and on a miss does `operator new(0x28)`
   (`0x2bf3a80`), stores the `HeightmapNew` vftable, calls the sized
   `vector<float>` constructor `0x0fc490` at site `0x317062` with element count
   `h * w`, stores the dims, then inserts via `0x3128a0` at `0x31707b` /
   `0x31709c`.
2. **`ScriptGenerator`, `0x399050`.** The same sequence is inlined, with the
   `0x0fc490` call and the `0x3128a0` inserts at `0x399df9` / `0x399e1b`. This is
   the path that creates a layer's *output* map when the layer is scheduled.

`0x0fc490` `memset`s the whole buffer to zero, so every map is touched in full at
birth.

A third site creates the one **non-owning** entry: the constructor `0x310e20`
does `operator new(0x18)`, stores the `HeightmapView` vftable, the caller's
`float*` and the dims, and inserts it under the pipeline's `heightmapLayer`
name at `0x311017`.

DERIVED consequence: **owned float storage is `(N-1) * w * h * 4`, but the print
counts all N.** The printed figure overstates toolkit-owned memory by one map
(1.07 GB at 256x256, 4.30 GB at 512x512). The viewed buffer is real memory, but
it is the world heightmap owned by the caller, so the process total is
unchanged; only the attribution is.

## Lifetime: all maps are simultaneous

This is the question the whole report turns on, so the evidence is given in
full.

**DERIVED, chain 1 — the printed field is a live count, not a running total.**
`0x3128a0` is the container's insert helper. On an insert of a new key it does:

```
if (*(longlong *)(param_1 + 0x10) == 0x492492492492491) _Xlength_error("list<T> too long");
*(longlong *)(param_1 + 0x10) = *(longlong *)(param_1 + 0x10) + 1;
```

`param_1` is the container at toolkit+0xB0, so `param_1 + 0x10` is
**toolkit+0xC0** — exactly the field the teardown prints. The guard constant
`0x492492492492491` is the MSVC `std::list::max_size()` for a 56-byte node, which
identifies the field as the node list's `_Mysize`, i.e. the number of entries
currently in the container.

**DERIVED, chain 2 — nothing ever leaves the container.**

- `0x311680` (clear body) walks the node list, calls the per-node destructor
  `0x3117c0` on each, frees each node, and sets the size word to 0.
- `0x3113d0` frees a side vector and then calls `0x311680`.
- The only callers of `0x3113d0` are the teardown `0x312400` (at site
  `0x312562`) and `0x3118e0`; `0x3118e0` is reached only from the exception
  unwind funclet `0x2c379f0`, and `0x311680` additionally from the funclet
  `0x2c37a30`.
- Within toolkit code the node destructor `0x3117c0` is called only from the
  `0x311680` clear loop. Its other callers (`0x30dfa0`, `0x30fda0`, `0x31a870`,
  `0xb24dc0`, `0xb25160`, `0x23c9860`) are other users of the same shared
  `pair<string, ptr>` template instantiation, not this container.
- No erase call site exists. There is no path that decrements toolkit+0xC0.

Therefore the printed N is the number of maps alive at teardown, and because
the count only ever rises until that single wholesale clear, **it is also the
peak number alive simultaneously**.

**MEASURED, independent cross-check.** N equals the number of *distinct named
buffers* in the Lua pipeline, exactly: Desert stock has 18 names and logs print
`18 maps`; with the buffer-reuse pass (18 -> 15) the same map prints `15 maps`;
Temperate and Tropical have 10 names and print `10 maps`. A cumulative counter
would be far larger than the distinct-name count, and would not have fallen when
only *naming* changed. This confirms chain 1 without relying on the
decompilation.

### Order of events in one generation run

DERIVED from `0x35ebf0` (the generation host, which owns the `"Pipeline
generation"` `0x2f99bf8` and `"place_assets"` `0x2f99c10` strings):

| site | call | effect |
|---|---|---|
| `0x35efe4` | ctor `0x310e20` | toolkit created; inserts the `heightmapLayer` **view** |
| `0x35f03a` | `MakeSubProgress` `0x349760` | progress plumbing |
| `0x35f05d` | `ScriptGenerator` `0x399050` | runs every layer; creates every named buffer; **frees none** |
| `0x35f168`, `0x35f18a` | `GetOrCreate` `0x316f70` x2 | resolves `forestMap` and `assetsMap` |
| `0x35f1c4` | `place_assets` `0x3a3c60` | consumes those two maps |
| `0x35f0ab` | teardown `0x312400` | prints, then frees **everything at once** |

MEASURED, the log order matches: `Pipeline generation` (x2), `place_assets`,
`* Terrain toolkit used ...`, `  Pipeline took: ...`.

So the peak is held from the end of `ScriptGenerator` through the whole of asset
placement — which MEASURED is the *slowest* stage (`place_assets: 21725.5 ms` on
one large map), so the peak is held for tens of seconds.

A second host, `0x313b80`, builds a toolkit named `"heightmap"` (`0x2f8d400`) the
same way and is used for the New Game **preview**. MEASURED, preview lines print
tiny sizes (`10 maps and 31 MB`, `... 317 MB`, `... 514 MB`) alongside the
full-size line in the same log, which DERIVED means `w`/`h` are free parameters
of the toolkit and every layer op is resolution-agnostic. (For the 31 MB line,
`w*h` is about 775,000 cells, not a `64n+1` square.)

### Pinned names

DERIVED, `0x39a9f0` reads the `GeneratorPipeline` fields:
`heightmapLayer`, `layers`, `parallelFactor`, `forestMap`, `assetsMap`,
`treesMapping`, `assetsMapping`. Of the maps:

- `heightmapLayer` is the **view** — no storage of its own.
- `forestMap` and `assetsMap` are written during the pipeline but **read only by
  `place_assets` at the very end**. They are the two coldest buffers in the run.
- Everything else is a temporary consumed within the layer DAG.

### Uncounted peak on top of the law

DERIVED. Two layer ops allocate full-size scratch that the printed number does
not include:

- `Feature::Ridge` `0x3928b0` allocates an additional `w*h` float vector
  (`0x0fc490` call) plus a rasteriser grid — **+1 map** while it runs.
- `MIX PERCOLATION` (`0x3873e0` -> `Percolation` `0x39ff90`) works on
  caller-allocated `vector<int>`, `vector<uint8>` and `vector<ClusterType>` at
  `w*h`, plus per-cluster 16-byte nodes — GUESS 6-9 B/cell, so roughly +1.5 to
  +2.25 maps' worth while it runs.

`ApplyKernel` (`0x374380`, GAUSS) and `ComputeDistanceMap` (`0x39ec00`) allocate
no extra full raster.

So the true process peak is the printed figure **plus** up to ~2 maps
transiently, plus the ~3 GiB pre-world baseline recorded in
`runtime-memory-audit.md`. At 256x256 with N=15 that is roughly 16.1 + 2.1 + 3
= **21 GB**, which is consistent with the reported ~224-tile ceiling on a 16 GB
machine (224 tiles at N=10 is 8.2 GB, at N=15 is 12.3 GB, both of which fit
where 256 tiles does not).

## Can the existing pager manage these allocations?

Assessed against the three properties the question asks about.

- **Fixed size, one recognizable call site?** Partly. There are two sites
  (`0x317062` inside `0x316f70`, and the inlined copy in `0x399050`), both
  calling `0x0fc490` with count `w*h`, and within one run every map is exactly
  the same size. But the size is **one contiguous 1.07 GB allocation at
  256x256 and 4.30 GB at 512x512**, against 132,098 B and 67,601 B slots in
  `terrain_pager.h` / `material_pager.h`. `pager_impl.inl` is built around one
  section per slot and whole-slot encode/decode; a 1 GB slot would have to be
  sub-paged into chunks with per-chunk codec state. That is a new pager, not a
  reuse.
- **CPU loads and stores only?** Yes, with one caveat. Every layer body reaches
  the data through the `std::vector<float>&` returned by vtable slot +0x28, and
  no kernel API was found writing into these buffers. The caveat is
  `Feature::LoadTexture` `0x396c40`, which **resizes** the output vector
  (`0x0e67d0`) rather than only storing into it; a resize reallocates and would
  move the buffer out from under a fixed-address pager. Per
  `generation-op-semantics.md`, `FEATURE TEXTURE` is not used by the stock
  generators, so it would only need to be refused.
- **Is the access pattern suitable?** **No, for most maps — this is the real
  objection.** Layer ops are full sweeps: `0x0fc490` zero-fills every element at
  birth, and essentially every op writes or reads every element. A compressing
  pager only pays when the working set is a small fraction of the whole. Here
  each pass would decode an entire map and immediately dirty it, so the pager
  would thrash and add encode/decode cost to a stage that is already
  MEASURED at tens of seconds. Layer bodies also run across the whole
  `ThreadPool` (`0x39a780` per-layer task, visitor table `0x2fb0860`), so faults
  would arrive concurrently on many threads on the same allocation.

  The exception is the **cold** maps: `forestMap` and `assetsMap` are written
  during the pipeline and not read again until `place_assets`. Those two are
  genuinely pageable.

Conclusion: the existing pager is the wrong tool for the generation peak in
general, and a reasonable tool for two specific buffers.

## Ranked reduction options

Baseline for savings: Desert with the current Lua pass, N=15. Per map is
1.0739 GB at 256x256 and 4.2952 GB at 512x512, so all savings are quoted in
whole maps where that is the natural unit.

| # | Option | Saving 256x256 | Saving 512x512 | Risk |
|---|---|---:|---:|---|
| 1 | Share/defer the pinned `forestMap` + `assetsMap` in the Lua pass | 2.15 GB | 8.59 GB | Low |
| 2 | Native free-at-last-use (erase from the toolkit when a value dies) | 2.1-3.2 GB | 8.6-12.9 GB | Medium |
| 3 | Halve generation resolution to 8 m and upsample | 12.08 GB | 48.3 GB | High |
| 4 | Chunked compression pager for the two cold maps only | ~0.9 GB | ~3.6 GB | Medium-High |
| 5 | Store maps as 16-bit instead of float32 | 8.05 GB | 32.2 GB | Very high |

### 1. Share or defer the two pinned asset maps (recommended first)

`generation-op-semantics.md` already records that `forestMap` and `assetsMap`
"hold their own buffers for the whole pipeline, although they are first written
only in the asset stage", and that sharing them would save up to two more
buffers per generator. The pass currently refuses to touch pinned names.

Sketch: extend `mod/generation/bigmap_memory.lua` to let a pinned asset map take
over a temporary buffer whose value is dead by the asset stage, keeping the
existing rule that a value read before its first full write gets a fresh
buffer. `tools/test_generation_memory.py` already replays every access
symbolically across 18 shipped pipeline combinations, so the correctness
argument is the existing one.

Saving: 2 maps = **2.15 GB at 256x256, 8.59 GB at 512x512** (DERIVED from the
map size; GUESS that both can be merged in all three stock generators — the
pass reports the achieved count, so this is checkable offline before anything
ships).

Risk: Low. Pure Lua, reversible with `--restore`, no native patch. Cost: sharing
a name serialises layers that used to run in parallel (`0x399050` orders layers
per buffer name), so generation may get slower.

### 2. Native free-at-last-use

This attacks the root cause: **nothing is ever freed**, so the peak is forced to
equal the final total no matter how the layers are ordered. Adding an erase lets
memory be reclaimed without renaming, which also avoids option 1's
parallelism cost.

Sketch: hook `0x316f70` and maintain, from the same op table the Lua pass
already builds, a last-read index per name. After the layer at that index
completes, erase the entry: destroy the `HeightmapNew` and remove the node,
decrementing toolkit+0xC0. The container is already mutex-protected at +0x118,
and `0x3117c0` is the existing per-node destructor, so the free path exists and
only needs to be reachable per-key. Defer creating `forestMap`/`assetsMap`
until the asset stage in the same change.

Saving: GUESS 2-3 maps, i.e. **2.1-3.2 GB at 256x256, 8.6-12.9 GB at
512x512**. It cannot beat the true maximum overlap of live values, which
`generation-op-semantics.md` puts at 12 live temporaries plus the pinned names
for Desert.

Risk: Medium. Erasing a name that is read again resurrects it as a zero-filled
buffer (`0x0fc490` memsets), which would silently change terrain rather than
crash — so this needs the symbolic replay in
`tools/test_generation_memory.py` as a hard gate, and a hash comparison of the
generated heightmap against an unpatched run.

### 3. Halve the generation resolution

The largest single win. Generate at 8 m (`w = h = 32*tiles + 1`), then bilinearly
upsample into the 4 m world heightmap. Memory falls to a quarter.

Sketch: the two `* 0x40` multipliers in `0x313b80` become `* 0x20`. The
mechanism is already proven by the preview path, which MEASURED runs the same
toolkit and the same layer ops at a much smaller `w*h`.

Saving: **12.08 GB at 256x256, 48.3 GB at 512x512** (N=15); against stock
Desert N=18 at 512x512 it is 58.0 GB.

Risk: High, and larger than it looks. The `heightmapLayer` **view** points at the
caller's real 4 m buffer and takes its dims from the same `w`/`h`, so the
"every map is the same size" invariant that `0x316f70` and the print formula
both rely on would be broken; the view would need its own size and an
upsampling step at teardown. Terrain shape changes (river rasterisation and
ridge detail are resolution-sensitive), so this is a different map from the same
seed, not a memory-only change. It would also have to be a user-visible option.

### 4. Chunked compression pager for the cold maps

The only place the plugin's existing strength applies. `forestMap` and
`assetsMap` are written during the pipeline and read only by `place_assets`
(`0x3a3c60`), so they are cold for the whole main run.

Sketch: recognise those two names in a hook on `0x316f70`, back them with a
sub-paged fixed-address allocation (the `pager_impl.inl` mechanism, but chunked
within one allocation rather than one section per slot), and let the VEH fault
them back in during asset placement. Refuse the whole path if
`Feature::LoadTexture` `0x396c40` appears in the pipeline, because it resizes
its output.

Saving: 2 maps at a GUESS 1.5-2:1 on noise-like float data, so roughly
**0.9 GB at 256x256, 3.6 GB at 512x512** — and only while they are cold.

Risk: Medium-High for a modest return. Options 1 and 2 free the same two
buffers outright and are cheaper to build; this is only worth doing if those two
prove impossible.

### 5. Sixteen-bit storage

Halving the element width halves everything: **8.05 GB at 256x256, 32.2 GB at
512x512** (N=15). It is listed for completeness only. It would require
rewriting all 28+ layer implementations (or interposing a conversion on the
vtable +0x28 getter, which would need a float scratch buffer per access and
defeat the purpose), and quantisation changes generated terrain. Not
recommended.

### Not recommended

Reducing N by dropping generator features, or generating in tiles/stripes. The
layer DAG contains global operations — `OP DISTANCE` (`0x386340`), `OP GAUSS`
(`0x386460`) and `MIX PERCOLATION` (`0x3873e0`) all read outside a local
neighbourhood — so a tiled generator would not reproduce the same map.

## What is not established

- Whether both `forestMap` and `assetsMap` can actually be merged in all three
  stock generators. The pass reports its achieved buffer count, so this is
  answerable offline without the game.
- The compression ratio of generation-stage float rasters. No samples were
  captured; the 1.5-2:1 in option 4 is a GUESS, unlike the material-grid ratio
  in `runtime-memory-audit.md`, which was measured.
- The exact scratch cost of `MIX PERCOLATION`; the element width of
  `ClusterType` was not read, so the +1.5 to +2.25 maps figure is bounded, not
  exact.
- The absence of an erase path rests on the direct call sites in the call-graph
  corpus. Indirect calls were not excluded, though the `_Mysize` reasoning and
  the MEASURED name-count agreement both point the same way.
- No live measurement was taken. Every process-peak total here is the printed
  law plus DERIVED additions, not an observed working set.

## Reproduction

The Ghidra project was robocopied to a private clone in the session scratchpad
(never opening `C:\tools\ghidra_proj` directly) and driven with
`tpf2-multiplayer/tools/ghidra/run.ps1 DecompileTargets.java`. Two target files
were used; the decompiles are in the session scratchpad under `decomp\` and
`decomp2\`. Targets, by label:

```
310e20 toolkit_ctor          312400 toolkit_teardown_print  316f70 toolkit_get_or_create
3128a0 toolkit_insert        3113d0 toolkit_clear           311680 toolkit_clear_inner
3117c0 free_helper           3118e0 toolkit_unwind          313b80 pipeline_host
35ebf0 map_generation_host   399050 script_generator        39a780 layer_task
39f650 dispatch_feature      39fa60 dispatch_op             39f790 dispatch_mix
0fc490 vector_float_ctor     396c40 feature_loadtexture     3860a0 feature_data
3a3c60 place_assets_fn       39a9f0 pipeline_desc_reader    3928b0 feature_ridge
39ff90 percolation           374380 apply_kernel_gauss      39ec00 compute_distance_map
157390 generating_terrain (InitNewGame)
```

MEASURED log lines were read from
`C:\Program Files (x86)\Steam\userdata\125253817\1066780\local\crash_dump\*stdout*.txt`
(read-only). The printed-law verification is a one-line check:
`floor((64*tiles+1)^2 * N * 4 / 1e6)` against each `* Terrain toolkit used N maps
and X MB...` line.
