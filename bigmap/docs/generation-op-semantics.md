# Terrain generation op semantics

What each Lua terrain layer does to the native float maps in Transport Fever 2
Steam build 35924, established with Ghidra from `TransportFever2.exe`. RVAs
are relative to the image base `0x140000000`. `mod/generation/bigmap_memory.lua`
and `tools/test_generation_memory.py` use exactly this table.

## From a Lua layer to native code

* Converters: `StackConverter<terrain::Layer>` `0x3709a0` accepts `FEATURE`,
  `OP`, `MIX` and `MIX_THREE`; the param converters are `0x36ff60` (feature),
  `0x371960` (op) and `0x371000` (mix). Name fields are `output` (all),
  `input` (op) and `input1`/`input2` (mix). Unknown type strings throw
  "Unknown generator layer".
* Scheduling: `ScriptGenerator` `0x399050` builds one queue of layer indices per
  buffer name from the name sets `0x3986d0` {output}, `0x398310`
  {output, input} and `0x398460` {output, input1, input2}. A layer starts only
  when it is at the front of every queue it is in (assert
  `dag[o].front() == i`). Layers that share a name run in layer order; all
  others run in parallel.
* Dispatch: the per-layer task `0x39a780` calls the visitor table
  `0x142fb0860`. The dispatchers call `TerrainToolkit` Get-or-create
  `0x316f70` for every name field and pass the float vectors on. Feature
  `0x39f650` passes (toolkit, params, out). Op `0x39fa60` passes (toolkit,
  params, in, out). Mix `0x39f790` passes (toolkit, params, in1, in2, out).
* Implementation tables give variant index and target: feature `0x142fb2120`,
  op `0x142fb2180`, mix `0x142fb21f0`. The converters set the variant
  indices, and `.rdata` holds the short comparands (`HERP`, `PWLERP`, `MESA`,
  `AXPY`, `MASK`, `COMP`, `DATA`, `LESS`), read from the executable.
* Parallel loops: `ThreadPool::Loop` enqueues the chunks
  `[k*block, min((k+1)*block, total))` for every k (`0x3760f0`, the MAP
  instantiation), so every row or element is visited once.

## Allocation

* `0x316f70` takes the toolkit mutex (`+0x118`) and looks the name up in the
  map at `+0xb0`. If the name is absent, it allocates a `HeightmapNew` holding
  `vector<float>(w*h)` and inserts it. The vector constructor is `0x1400fc490`
  and calls `memset(0)`. `ScriptGenerator` creates the output map the same way
  when it schedules a layer. Allocation is therefore lazy, happens at a name's
  first access, and is zero-filled. That covers names that are only read, such
  as tropical's `CUTOFF_POS`.
* Nothing is freed while the pipeline runs. The map is cleared only by
  `0x3113d0`/`0x311680`. Their callers are the pipeline host teardown
  `0x312400` (which prints "Terrain toolkit used N maps and X MB",
  X = w*h*N*4 from the map size), `0x3118e0`, and the exception unwind funclets
  `0x2c37a30`/`0x2c379f0`. The insert helper `0x3128a0` is called only by the
  toolkit constructor `0x310e20`, the host `0x313b80`, `0x316f70` and
  `0x399050`. No erase call site was found.
* The peak named storage is therefore the number of distinct names. An empty
  name resolves to the toolkit's `heightmap` view, which is not a new
  allocation.

## Table

"Old output" means the result depends on the previous contents of the output
map. "All" means every output element is written. "In place" means the output
may be the same vector as an input.

| Layer | RVA (loop body) | Old output | All | In place | Evidence |
|---|---|---|---|---|---|
| FEATURE CONSTANT | `0x385fb0` (`0x324150`) | no | yes | n/a | `assign(size, value)`: `std::fill` over `[begin, begin+size)` |
| FEATURE DATA | `0x3860a0` (`0x382290`, `0x384520`) | no | yes | n/a | both filter paths assign `out[row*w+col]` for all rows and columns from the Lua data map |
| FEATURE NOISE | `0x38ffd0` (`0x38f120`) | no | yes | n/a | every element from coordinate noise |
| FEATURE RIDGED_NOISE | `0x390170` (`0x38ea80`) | no | yes | n/a | every element from coordinate noise |
| FEATURE GRADIENT_NOISE | `0x38fd20` (`0x38ed40`) | no | yes | n/a | both branches assign every element |
| FEATURE WHITE_NOISE | `0x390430` (`0x38eb50`, `0x38ec50`) | no | yes | n/a | `out[i] = random <= p`; `p == 0` writes 0 |
| FEATURE DITHERING | `0x38fb60` (`0x38f290`) | no | yes | n/a | every element from the dither table |
| FEATURE RIDGE | `0x3928b0` (`0x391ad0` -> `0x3922a0`) | no | yes | n/a | rasterises into a private grid and a private noise map; the final loop assigns every `out[row*w+col]` from those |
| FEATURE POINTS | `0x387780` | yes | no | n/a | writes `out[y*w+x] = value` only at the listed points |
| FEATURE RIVER | `0x396660` | assumed yes | not verified | n/a | rasterises river triangles into the output through a ZRasterizable; coverage and depth test were not traced |
| OP MAP | `0x386cb0` (`0x383850`) | no | yes | yes | `out[idx] = map(in[idx])`, optional clamp |
| OP HERP | `0x3869a0` (`0x382fc0`) | no | yes | yes | Hermite of `clamp(in[idx])` |
| OP PWLERP | `0x387190` (`0x383280`) | no | yes | yes | interpolation after a binary search on `in[idx]` |
| OP PWCONST | `0x386f40` (`0x384bc0`) | no | yes | yes | `values[lower_bound(in[idx])]` |
| OP WHITE_NOISE | `0x390650` (`0x38f390`, `0x38f050`) | no | yes | yes | `out = random < in[idx]`; the random stream does not depend on data |
| OP MESA | `0x38ad20` (`0x38a960`) | no | yes | yes | reads `in[idx]` once; the noise depends only on coordinates |
| OP DISTANCE | `0x386340` (`0x385400`, `0x39ec00`) | no | yes | yes | the first pass assigns `out[i] = in[i] > threshold ? FLT_MAX : 0` for all i; the distance passes `0x39ea20`/`0x39eb10` then use only `out` |
| OP GAUSS | `0x386460` (`0x374380`) | no | yes | **no** | `out[r*w+c]` = kernel-weighted sum over a window of `in` |
| OP GRADIENT | `0x386750` (`0x384ed0`, `0x3847b0`, `0x383cf0`) | no | yes | **no** | interior from `in[idx±1]`/`in[idx±w]`, then border rows and columns copied from `out` (see note) |
| OP LAPLACE | `0x386a40` (`0x384870`) | no | yes | **no** | 4-neighbour stencil on the interior, then the same border copies |
| OP AXPY | `0x385c30` (`0x383400`) | **yes** | yes | elementwise | `out[i] = alpha*in[i] + out[i]` |
| MIX MUL | `0x386ea0` (`0x384330`) | no | yes | yes | `out[i] = in1[i]*in2[i]`; SIMD only when ranges do not overlap, scalar otherwise |
| MIX ADD | `0x385700` (`0x384ce0`) | no | yes | yes | `out[i] = in1[i]+in2[i]` |
| MIX COMP | `0x385d60` (`0x383610`, `0x382460`, `0x384950`, `0x384200`, `0x3829f0`) | no | yes | yes | greater, less, equal, max, min per index; converter `0x36fd40` maps any other string to GREATER, so a mode always applies |
| MIX PERCOLATION | `0x3873e0` (`0x3826a0`, `0x382780`) | no | yes | yes | inputs are read only in the first pass, into private seed and mask vectors; last, `out[i] = cluster[i] == 0` for every i |
| MIX MAD | `0x386c10` (`0x384f60`) | **yes** | yes | elementwise | `out[i] = in1[i]*in2[i] + out[i]` |
| MIX MASK | `0x386d70` (`0x3830f0`, `0x382860`) | **yes** | no | elementwise | `out[i] = in2[i]` only where `in1[i] > value` (or `<` for LESS); converter `0x364c00` maps LESS to 1 and anything else to GREATER |

Notes:

* GRADIENT and LAPLACE: in the magnitude/stencil path, row 0 and row h-1
  first copy the not yet written corner cells of rows 1 and h-2. The column
  copy that follows overwrites those cells, so the final map does not depend
  on the old output.
* "Elementwise" for AXPY, MAD and MASK means one index at a time. These ops
  read the old output, so the pass never starts a new value at them.
* Percolation and ridge skip their writes when the progress callback reports
  cancellation. The generation is abandoned in that case.
* Not used by the stock generators and not analysed: OP CLIFF `0x385cd0`,
  ABS `0x385670` and AMB `0x3857a0`; MIX AXPBYPZ `0x385b80`; FEATURE TEXTURE
  `0x396c40` and RASTERIZE `0x387930`; MIX_THREE. The pass leaves any pipeline
  that contains them, or any other unknown layer, untouched.

## Use in the buffer pass

`bigmap_memory.lua` works as follows:

1. It splits each temporary name into values at "All = yes, Old output = no"
   writes.
2. It gives each value a buffer in layer order, reusing a buffer whose value
   has died. It may also let the output of an in-place op take over an input
   that dies at that op.
3. It keeps every alias the stock pipeline already has.
4. A value first read or partially written before any full write keeps a new
   buffer, because the native buffer is zero-filled there.
5. Names referenced outside the layer name fields, and all non-`__t_<n>`
   names, are never renamed or shared.
6. Before renaming, it replays the accesses and requires every read to find
   the value the stock pipeline would read.

Tested stock pipelines (`tools/test_generation_memory.py`, 18 combinations):

| Generator | Water 0 | Water 2 | Water 4 | Lower bound for these semantics |
|---|---|---|---|---|
| Desert | 18 -> 15 | 18 -> 15 | 18 -> 15 | 15 |
| Temperate | 10 -> 9 | 10 -> 9 | 10 -> 9 | 9 |
| Tropical | 10 -> 10 | 10 -> 10 | 10 -> 10 | 10 |

The peaks consist of the following:

* Desert: 12 live temporaries plus `HM` and the pinned forest and asset maps.
* Temperate: 6 temporaries plus 3 fixed names.
* Tropical: 6 temporaries plus `HM`, `CUTOFF_POS` and the pinned forest and
  asset maps.

The pinned forest and asset maps hold their own buffers for the whole
pipeline, although they are first written only in the asset stage. Sharing
them would save up to two more buffers per generator, but it would break the
rule that pinned names are never merged. At 256 x 256 tiles
(16385 x 16385 floats, 1,073,872,900 bytes per map) desert saves about 3.2 GB
of named storage and temperate about 1.07 GB.

## Not verified

* RIVER's write semantics. It is treated as reading the old output; in the
  stock pipelines it only writes the pinned `HM`.
* The renamed pipelines have not run in the game. No rendered heightmap was
  compared and no peak memory was measured. Merged names also serialise
  layers that used to run in parallel.
* The chunk partition was read only for the MAP instantiation of
  `ThreadPool::Loop`; the other instantiations come from the same template.
* The absence of an erase path rests on the direct call sites in the call-graph
  corpus; indirect calls were not excluded.
