# World-entry profiling and material-index optimization

The user-visible delay is after generating a preview and pressing Start, not
loading a saved game. Steam 35924 only; all native sites are byte-verified.

September 13: the outer entry hook also tracks active generation for the
lossless terrain pager's temporary warm budget. When timing logs are disabled,
an activity-only hook is installed if that budget is needed. Native tests cover
this installation path and verify activity clears after the outer call.
Saved loading starts a separate burst-allocation allowance in the pager. The
latest follow-up retains that allowance until a fresh gameplay UI heartbeat,
with bounded fallback for older menu DLLs. See terrain-compression.md.

## Road connection follow-up

The September 13 running session recorded 287.124 seconds for the outer entry,
229.102 seconds for the stock InitGame timer, 12.423 seconds for trees, and
5.764 seconds for scenery. It logged 56/76 town connections and 579/618 industry
connections. These totals do not isolate road cost.

Read-only decompilation and call-site inspection identify `0x937590` as the
town/industry road-connection stage, called by new-world entry at `0x15871a`.
It gathers network information, prepares obstacle/raster data (`0x9473a0`,
`0x9466f0`), checks connectivity per town pair (`0x935600`), and invokes route
and construction work (`0x935be0`, `0x937180`) for missing connections. It checks
connectivity again after attempted construction. The network changes across
attempts, so reusing results across those mutations is not established safe.

Added a byte-verified, pass-through timer for the entire `0x937590` stage. Its
10-argument ABI includes uint32 arguments 4/5 and uint8 argument 10; native
tests verify truncation, forwarding, instruction boundaries, installer failure
paths and scoped begin/end logging. The stage is installed before the outer
scope hook. It does not change which roads the game builds. A new-world run
with this build is needed to measure its actual share; no road speedup is claimed.

## Live finding

PID 79100 was still displaying the final initialization screen after the
stock log printed `InitGame: 41828.4 ms` and `Create Fields: 652.375 ms`.
A 25-second, 100-ms instruction-pointer sample identified material-index
selection in `0x315f20` (hot unwind fragment `0x315ffa`) as the largest
individual game-code bucket: 428 samples. Texture-cache lookup `0x317120`
and merge work `0x30be00` also appeared. The existing sampler labels win32u
waits as working; its displayed CPU-second estimates and busiest-thread
ranking must NOT be interpreted as actual CPU time. The instruction locations
are useful, but this is not a whole-load critical-path breakdown.

This changes the immediate optimization priority from tree insertion to the
late terrain material-index work. Tree/asset publishing already groups
instances in 64 m spatial cells and performs shared ECS writes; that code
was not parallelized or skipped.

## Experimental material-index fast mode

Set `material_index_fast=1` in the active plugin config. Zero restores stock
code on the next process launch. `src/material_index.h` replaces `0x315f20`
with a pixel-first traversal of the same material-selection algorithm.

Stock visits a rectangular tile region repeatedly in batches advancing by
eight material layers (with a nine-layer inclusive scan and one-layer overlap).
The replacement completes those batches per pixel, avoiding repeated output
scans and recalculation of interpolation coordinates and dithering thresholds.
It preserves the overlap, layer priority, overlay and mask precedence, 0xe9
sentinel handling, fallback material and scalar float interpolation order.
There is no reduction in texture resolution, terrain resolution or octree depth.
Unsupported region geometry and overlapping output/base/overlay/mask storage
fall back to the original function.

`tools/test_material_index.py` executes the original, self-contained machine
code in the test process. Only its two RIP-relative data references are
relocated to copies of the original 0.25 constant and 63x63 dither table.
It compares entire output buffers against the compiled replacement across
156 cases: negative/zero and positive counts, batch boundaries, rectangular
regions, translated tile origins, empty/mixed overlays and special material
IDs. Further cases cover misses through every layer, fallback and sentinel
behavior, alias/geometry fallback, and installer rejection paths.

Initial warm-cache native microbenchmarks with 40 layers and 64x64 regions
showed 1.39x for all-layer misses and 1.08x for top-layer hits. These are small
synthetic loop benchmarks, NOT a measured improvement to total world-entry
time. Overall performance and visual correctness still need an in-game run.

## Stage timing mode

`world_entry_timings=1` enables scoped begin/end messages with QPC elapsed
time, private committed memory, resident working set and system available RAM.
The phases are InitNewGame total (`0x157390`), world allocation (`0x230440`),
terrain preparation (`0x3bcb90`), terrain publication (`0x3bc9e0`), trees
(`0x3b8bd0`) and scenery (`0x3b8660`). These timers do not cover the complete
UI loading screen: material-index/render preparation may continue afterward.

All six sites are preflighted before installation. The outer scope is installed
last, so a partial failure leaves earlier wrappers silent outside the scope.
`tools/test_world_entry.py` checks instruction boundaries, absence of relative
instructions in stolen bytes, every failure position, native argument
forwarding (including ten-argument terrain setup), and timer scope restoration.

The local experimental deployment enables both modes. The source config keeps
them opt-in. Existing depth-13, spacing-overflow, fast-placement and Lua buffer
reuse changes remain independent. No live code was replaced in PID 79100;
the new DLL applies on the next launch.
