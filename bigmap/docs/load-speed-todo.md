# Load-speed to-do (engine code)

Evidence: 20 ms instruction-pointer profile of an in-process reload of a
256x256-tile save, September 15, 2026 (`tpf2-multiplayer/tools/re/profile_load.py`,
PID 60744, load ~70 s). That profile still contained terrain-pager decode
overhead that the loading-budget change removes, so re-profile before choosing
between the items below.

Built, tested offline, not yet measured in game: **bicubic terrain refinement**
(`sub_terrain_util::InternBicubicRefine` `0x3ac6c0`, hottest game range `0x3ac811`,
11.7% of samples in one load window). `terrain_refine_fast` (default 0): 3.4x per
1 m tile call, byte-identical to the original across 4,315 comparisons (159 M
samples); also inlines the 4x4 multiply at `0x2fadc0` (3.5% in the profile). See
`docs/terrain-refine.md`.

## Clean profile, September 15 (loading-budget build, fresh launch, 256x256 save)

Load ~73 s ("Loading from file" 18:47:00 to "Initial material index generation"
18:48:13) versus ~70 s for the earlier in-process reload: growing the pager
budget during loading did not shorten the load and raised the peak to 36.5 GB,
so it was reverted. Share of all samples per 20 s window during the load:

| Window | Top game/plugin work |
|---|---|
| t+120 | InternBicubicRefine chunk `0x3ac811` 11.2%, alignment `0x3b3470` 5.4% and `0x3af850` 5.3%, `0x2fadc0` 3.5% |
| t+140 | tpf2_bigmap pager 24.5% (loading thread 41%), `0x30a55c` 10.1% (39% of that thread), `0x33cd10` CalcMinMaxHeight 2.4% |
| t+160 | tpf2_bigmap pager 28.5% (loading thread 53%, plus unmap/protect), UpdateLodTess `0x334c60` 16.2% |
| t+180 | UpdateLodTess `0x334c60` 46.3% (one thread 86%), pager 12.9% |

Two background threads show win32u.dll at 100% in every window, including at
the title menu (frame loop or overlay busy-wait; not load-specific).

## Next

- [x] **Re-profile a save reload** with the loading-budget build deployed, so
  engine-only percentages rank the items below. Compare load time with the
  ~70 s baseline (stdout "Loading from file" to "Initial material index
  generation"). Done: see the clean profile above.
- [~] **Pager: share tiles between the two terrain versions.** A save load holds
  131,072 live tiles (two CTerrain versions, 17 GiB) and the COW copy path
  (`1dedd0` hook) memcpys each resident tile into a fresh slot (Clone only shares
  already-compressed tiles, and MEASURED it fires for 0.007% of copies). Sharing
  resident sections copy-on-write removes the copy, the second 8.25 GiB and
  2.0-2.7 CPU-seconds per load taken under the pool lock.
  Built behind `terrain_cow_share` and **MEASURED on a 256x256 load: it does not
  work, and the switch is back to 0.** `cow_shared=0` while `live=131072` proves
  the load never reaches the copy hook - both versions allocate their tiles
  independently through `AddTile`, so there is no copy to remove; and where the
  hook does fire (gameplay) every single share was written, 710/710, making it
  pure overhead. Peak was 38.96 GB against 39.02 GB with it off.
  See `docs/terrain-cow-sharing.md` section 0c.
- [x] **Content dedup of identical tiles** (replaces the item above). MEASURED
  September 17 with `terrain_dedup_probe`: at 131,072 live tiles, 64,922 exact
  pairs and 719 still-unfilled zero tiles; after the load, 65,536 distinct.
  Built as `terrain_dedup` (evictions share an identical stored blob instead of
  encoding); see `docs/terrain-cow-sharing.md` section 0d. In-game hit count and
  load time not yet measured.
- [x] **`0x30a55c` identified**: the middle pdata chunk of the uint16 height
  block copy `0x30a540` (row-by-row, one word per iteration, 132 KB per tile
  copy). Replaced with a per-row memcpy in `terrain_minmax_fast`.
- [x] **CalcMinMaxHeight** (inlined in the publication entry `0x33cd10`):
  SSE2 scan behind `terrain_minmax_fast` (default 0). ~24x on the scan, 11.8x
  warm on the block copy (1.3x when the pages are untouched, which is the
  realistic bound). Bit-identical across 62,634 register comparisons and 3,011
  block copies. See `docs/terrain-minmax.md`.
- [x] **Terrain alignment** (`terrain_alignment_util::CalculateHeightMod`
  `0x3b3470`; `0x3af850` turned out to be the uint16 vector resize helper, six
  132 KB allocate-and-fill cycles per call): pooled scratch plus an SSE2 blend
  behind `terrain_align_fast` (default 0). 4.67x with no alignments on the block,
  2.60x with six road strips, 1.57x fully covered; rasterisation is left as stock
  code. Bit-identical across 1,374 comparisons (12.8 M samples). See
  `docs/terrain-alignment-speed.md`.
- [x] **UpdateLodTess parallelisation: not needed.** `0x334c60` already runs per
  tile on the engine ThreadPool (100 chunks on Main/Sim Pool, 24 workers on a
  32-thread CPU); "one thread at 86%" was that thread's share of its OWN samples,
  and at least 8 threads were inside it at once (~175 thread-seconds including
  page faults). See `docs/terrain-lodtess.md`. Remaining options: a bit-exact
  faster per-call loop (signed idiv by powers of two), or less pager faulting in
  this phase; decide from a profile with plugin symbols (`--map`).
- [ ] **New-map creation profile.** Town/industry road connections
  (`0x937590`) took 127.5 s of a ~230 s world entry on a new 256x256 map;
  terrain generation 54.3 s; trees 10.7 s; scenery 5.1 s. Profile which inner
  functions dominate the road stage before judging whether a safe change exists
  (it mutates the network between attempts; results must not change).
- [ ] Unattributed hot ranges left: `0x2fadc0` (the 4x4 matrix multiply the
  refinement inlines; should disappear from the next profile),
  `0x2375664`/`0x23754b0` (the alignment rasteriser, left as stock code),
  `0x27da4d0` (decompression, seen at the title menu too).

Parallel work: each item is developed in its own repo copy and Ghidra project copy, adds only new files, and is wired into src/bigmap.cpp by the coordinator after its tests pass.

Rules for every item: separate cfg switch (default off), byte-verified hook
sites with fallback to the original, an offline test that executes the original
machine code and compares complete outputs, and an in-game load-time measurement
before enabling by default.

## September 28: after the sidecar skip (36,992-tile save, ~55 s load)

Measured with `profile_load.py --map/--stack-at/--follow` and an ETW `-Light`
trace read by `tools/re/trace_stacks.py`.

- [x] **Sidecar record lookup:** ~35,000 probes per AddTile (a forward scan from
  a shared cursor); now outward from each thread's last hit, 2.5 probes.
- [x] **Sidecar decode on the load thread:** ~8 s serial at AddTile; now decoded
  at the pass on up to 16 threads (~1.7 s per terrain version).
- [x] **Material index (`MaterialIndexDetour`):** layer pointers and the dither
  column hoisted; 1.40x -> 1.58x (all miss), 1.51x -> 1.97x (top hit) vs stock.
- [ ] **Material index in the sidecar:** deterministic (36,992/36,992 tile hashes
  equal on two loads), but 2,312 MiB raw, 1,306 MiB run-length, ~334 MiB
  order-0 entropy for ~3-5 s of wall time. Not built.
- [x] **Bigger MaterialIndexAsyncWork chunks: REJECTED.** `0x312990` walks the
  map in 8x8-tile chunks (six immediates). At 32x32 **8,164 of 36,992 tiles came
  out different**, spread over whole tiles and not only chunk edges, and the
  phase used the same ~6-7 cores. Do not change the chunk size.
- [ ] **Previous-world teardown** when loading from inside a world: the load
  thread's gamestate.cpp path releases every old tile through the pager's unmap
  (~39,000 stack events). Absent when loading from the title menu.
- [ ] **Save deserialization** (`game\serializer.cpp` `0x2e5ec0`, ~18 s of the
  load thread) is mostly waiting, not computing; the `-Light` trace has no
  context switches to say on what. A full `trace_load.ps1` capture would.
