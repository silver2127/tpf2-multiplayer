# Experimental generation performance modes

These changes preserve depth 13, 128 m octree leaves, map dimensions and the
4 m heightmap resolution. Restart before measuring a new preview.

## Fast placement

`placement_attempts=50` in the active `plugins/tpf2_bigmap.cfg` reduces the
RandomLocationFactory worker's inner optimization budget from 200 to 50.
Set 200 to restore stock behavior; accepted values are 1 through 200.
The source config defaults to 200; the experimental local deployment uses 50.

Steam 35924 RVA `0x912f59` is `41 b9 c8 00 00 00` (mov r9d,200) in the
worker at `0x912f10`. Only this verified immediate is changed. The worker
calls `0x910f40` with this fourth argument. Four workers, the outer passes,
minimum spacing, water/slope/obstruction checks and requested counts remain
unchanged. Fewer attempts can yield less even placement and fewer accepted
sites. This is a 75% reduction in inner attempts, not a measured 4x speedup
for total generation. Unknown bytes/builds and invalid values are refused.

## Terrain buffer reuse

Run `python tools/install_generation_memory.py` to enable the Lua pass for
the three stock New Game terrain generators. Exact originals are backed up
as `*.gen.lua.bigmap-memory.bak`. Use
`python tools/install_generation_memory.py --restore` to restore those files.
The installer preflights all three files and refuses manual edits conflicting
with a backup. It does not overwrite a differing existing helper module.

`mod/generation/bigmap_memory.lua` examines the completed Lua operation list.
It splits temporary names into values at writes that the native code
overwrites completely. Values whose lifetimes do not overlap then share a
name, and an op verified to work element by element may write its output over
an input that dies there. The op semantics, their RVAs and the allocation
behaviour are in `docs/generation-op-semantics.md`. The pass leaves
operation order, parameters, seeds and stock aliases unchanged. It pins names
referenced outside the layer name fields, keeps non-temporary names and values
read before their first full write, and skips unknown layer or op types. The
native ScriptGenerator (`0x399050`) orders layers per buffer name, so sharing
a name serialises otherwise independent branches.

The tested pipelines use these named buffers:

* Desert: 18 -> 15.
* Temperate: 10 -> 9.
* Tropical: 10 -> 10 (the pass is a no-op).

Each result equals the lower bound for these semantics. At 228 x 1140 tiles,
each buffer has 14593 x 72961 floats, so three buffers total 12,776,638,476
bytes (11.90 GiB). This is an expected reduction in named terrain storage,
not a measured reduction of the entire process peak. Additional scratch
allocations and scheduling affect that peak.

## Fantasia Map Generator

Native Linux uses a byte-verified `fopen` PLT route and anonymous temporary
streams. See [Linux evidence and live limits](../../docs/re/linux/DEV_6584FD03.md).
The PE import and `%TEMP%` details below describe Windows.

The Fantasia Map Generator workshop mod (2916150031) builds its pipeline with
the stock `layersutil` temporaries, but no pass runs over it, and it is far
larger. The game MEASURED 59-63 maps (15.8-16.9 GB) at 128 x 128 tiles
(32 x 32 km) and 52-63 maps (22-38 GB) at 160 and 192 tiles, where it died
with `std::bad_alloc` on a machine with no page file. At 192 x 192 tiles the
game logged 3120 layers over 77 buffer names.

The game passes a generator `mapSizeX`/`mapSizeY` in heightmap samples,
`64 * tiles + 1` (MEASURED 12289 at 192 tiles), not in metres, so 32 x 32 km
is 8193 x 8193.

The plugin handles it with no extra mod (`src/generator_memory.h`,
`generator_memory=1`, on by default). The exe's file-open imports
(`CreateFileW`, `_wfopen`, `fopen`, `std::_Fiopen`) are found by walking its
import directory and pointed at detours. A read-only open of
`fantasia_map_generator.gen.lua`, `_dry` or `_tropical` in any
`res\config\terrain_generators` folder is served from a patched copy in
`%TEMP%\tpf2_bigmap`. The one line `\t\treturn result` becomes
`return _tpf2_bigmap_generate(result, params)`; `bigmap_memory.lua`
(embedded by `build.bat`) and that helper are appended after the last line,
so every line number is kept. The helper logs
`[tpf2_bigmap] generator memory: <samples> (<tiles>), N layers over M buffer names`
and runs the pass when the map is over 8193 x 8193 samples. Maps up to
32 x 32 km (128 x 128 tiles) run Fantasia exactly as shipped. A file whose
anchor is missing or repeated is served unchanged. Fantasia's files on disk
are never written. The host log says which files were served
(`generator memory: ... served with buffer reuse`). If the pass leaves a
pipeline alone, it logs why (an unknown op or a pinned name).

All three Fantasia climates go to 10 names, which is the lower bound: 57-60 at
130 tiles, 62-65 at 160 and 70-73 at 192. `tools/test_generator_memory.py`
runs the DLL's patched text against the original: identical pipeline at 128
tiles, symbolic replay at 130, 160 and 192, path matching, line numbers,
anchor refusal and the `%TEMP%` copy.

Sharing names serialises layers. MEASURED in game at 192 x 192 tiles with the
fewest buffers: 10 maps, 6040 MB, `Pipeline took: 285.199s`, against about
180 s expected from the stock 32 km runs. So the DLL also appends
`_tpf2_bigmap_budget`, `generator_memory_budget_pct` (default 50) of the
memory free when the file is opened (the smaller of free physical memory and
commit headroom). The helper turns that into a buffer count and passes it to
`Optimize(result, maxSlots)`. The pass then opens new buffers until it has
that many, and after that reuses the one idle longest. Without `maxSlots`
(the stock generators) it keeps the fewest-buffers first-fit. The test
measures the longest chain of layers ordered by shared names at 192 tiles:
Fantasia stock 1716-1764, fewest buffers 1924-1983, a 30-buffer budget
541-549 (34 buffers). That chain is a model of the native scheduler, not a
timing. `generator_memory_budget_pct=0` gives the fewest buffers.

## Checks and remaining validation

`tools/test_placement_distance.py` exercises installer refusal paths and runs
the original worker machine code in Unicorn, checking that only the attempt
argument changes and that the remaining native call arguments survive.

`tools/test_generation_memory.py` runs 18 actual shipped Lua pipeline
combinations across Desert, Temperate and Tropical, three water settings and
two seeds (one Desert case uses the 292 km dimensions). It checks:

* unchanged metadata, parameters and pinned names
* stock aliases kept, and new aliases only for in-place-safe ops
* symbolic provenance of every input, and of the old output of every op that
  reads it, using the verified semantics

It also reports before/after counts against a lower bound, and runs guard
cases for unknown schemas, pinned metadata and fresh buffers. Installer backup,
restore, idempotence and conflict refusal are tested in a temporary directory.

These are offline checks, not a native rendered-heightmap comparison or a
live timing/memory benchmark. Measure the same seed and settings after a
restart. Look for the fast-placement startup message and the Lua
`terrain memory: 18 -> 15 named buffers` message, then compare peak private
memory, stage times and resulting town/industry counts. Restore the modes
individually if investigating a difference in results.
