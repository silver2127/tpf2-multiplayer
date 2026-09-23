# Big Maps (bigmap/)

Maps larger than Transport Fever 2's New Game menu will build.

Big Maps lived in its own repository (tpf2-bigmap) until 2026-09-22; it is now
part of TpF2 Multiplayer and ships in the same MSI. Build it with
`native\build.bat bigmap` (or `all`); this folder's `build.bat` still takes the
test targets (`-pager-test`, `-codec-test`, ...).

Experimental [generation performance modes](docs/generation-performance.md)
add a configurable placement budget and conservative Desert terrain-buffer
reuse without changing map resolution or octree depth.

[World-entry performance](docs/world-entry-performance.md) adds stage timings
and an experimental material-index loop optimization based on a live profile.

Large-map preview placement also includes a fix for signed distance-squared
overflow above approximately 185 km separation. See
[placement-distance.md](docs/placement-distance.md) for the reverse-engineered
sites and offline validation; an in-game regeneration check is still pending.

A native plugin for the **tpf2mp plugin host**. It carries no multiplayer code;
its one build-time dependency on the rest of the repo is the plugin ABI,
`native/src/plugin/tpf2mp_plugin.h`.

Target: **Transport Fever 2 build 35924**, in both of its builds: Steam
(2024-12-11) and GOG (2024-12-12). Every address was measured on both, each site
is byte-verified before it is patched, and the plugin refuses to patch anything
else.

The two builds share their code shape but not their addresses — the same
function sits at a different RVA in each — so the plugin keeps a pair of
constants per patched site and byte-verifies the one it is about to write. At
start it reports which build it found:

```
[host] tpf2_bigmap: game build is the GOG 2024-12-12 binary -- all three sites byte-verify; using the GOG layout
```

Some features remain Steam-only because their sites were never measured on the
GOG build, or because the replacement is a Steam code shape. They **degrade with
a log line** rather than failing the load: the density levels, the added size
dropdown rows, the placement/instance/material experiments, and the minimap.
`octree_depth=12`/`13` degrades the same way — see below.

To install on GOG, use **`TpF2BigMaps-<version>-gog.msi`** instead of the
standard package: the standard one refuses a GOG game folder, because the
installer's folder check knows only the Steam executable.

---

## What the game does

The New Game menu turns two dropdown indices into a tile count:

```
CVec2i UI::`anonymous-namespace'::GetNumTilesNew(int sizeIndex,
                                                 int formatIndex,
                                                 const AppConfig&)
RVA 0x674aa0   MenuUI.cpp:264-273
```

and the caller expands that straight into a heightmap:

```
dim         = 1 << terrainLevels        // 64
heightmapPx = tiles * dim + 1
```

**One tile is 256 m** (64 px at 4.0 m/px). Measured directly: a 224 x 224 map
reports a world bounding box of **57,344 m** per side, and 57344 / 224 = 256
exactly. 4.0 m/px is also the round number you would expect a terrain LOD scheme
to use; 3.90625 is not.

Every shipped preset, at 256 m/tile:

| tiles | km | preset |
| --- | --- | --- |
| 18 x 54 | 4.6 x 13.8 | Small 1:3 |
| 22 x 88 | 5.6 x 22.5 | Medium 1:4 |
| 48 x 192 | 12.3 x 49.2 | Megalomaniac 1:4 |
| 54 x 162 | 13.8 x 41.5 | Megalomaniac 1:3 |
| 66 x 132 | 16.9 x 33.8 | Megalomaniac 1:2 |
| 96 x 96 | 24.6 x 24.6 | Megalomaniac 1:1 |

The tile COUNTS come from the `.sav` header (`numTilesX`/`numTilesY` at
+0x10/+0x14 after zstd decompression) and are certain. The km column is derived
from them -- so it is not independent evidence for the tile size, which is why
the bounding box above is what settles it.

Read them yourself:

```python
import zstandard as zstd, struct
head = zstd.ZstdDecompressor().stream_reader(open(sav, 'rb')).read(64)
assert head[:4] == b'tf**'
print(struct.unpack_from('<8i', head, 4)[3:5])   # numTilesX, numTilesY
```

## The two ceilings

`GetNumTilesNew` clamps both axes to 224 tiles:

```
0x674afa:  B9 E0 00 00 00     mov ecx, 0xE0        ; 224
           cmp edx, ecx / cmovle ...               ; applied to x and y
```

So the presets are not the real limit. Two separate ways past them:

**Without this plugin at all.** `settings.lua` has a shipped, undocumented
escape hatch:

```lua
newGameMenuState = {
    worldDimensionsOverride = { 224, 224 },   -- 56 x 56 km
```

Read at `AppConfig+0x28/+0x2c`; if both components are positive it bypasses the
preset table entirely. Both must be **even** (there is a live assert) and it is
still subject to the 224 clamp. Edit it with the game **closed** — the game
rewrites `settings.lua` on exit.

That alone gets you 56 × 56 km = 3,136 km², against the 576 km² every
Megalomaniac variant is capped at. The presets conserve area as you stretch the
ratio; the override does not.

**With this plugin**, past the clamp — because a detour never reaches it.

## The 180-tile wall, and how the plugin gets past it

The game's own 224-tile clamp is not reachable with stock code. "Creating
streets" allocates a `std::vector<bool>` with **one bit per square metre** over
the whole map bounding box, and sizes it with a 32-bit multiply (RVA `0x90d410`):

```
mov    eax, [rbx+0x44]          ; ny
imul   eax, dword [rbx+0x40]    ; nx * ny   <-- 32-bit signed
movsxd rdx, eax                 ; sign-extend into size_t
call   vector<bool>::resize
```

At 224 tiles the map is 57,344 m per side, so `57,345² = 3,288,449,025 > INT_MAX`.
It wraps negative, sign-extends to ~1.8e19, and `resize` throws
`std::length_error`. Nothing catches it: `terminate` → `abort` → SIGABRT with no
message, because it is an **uncaught C++ exception, not an assert**. Confirmed by
resolving the thrown object's RTTI in the minidump (`.?AVlength_error@std@@`).

Stock ceiling: `(width_m + 1) × (height_m + 1) ≤ 2,147,483,647`, i.e. width_m ≤
46,340, i.e. **180 tiles (46.1 km)** on a square map -- tiles must be even, and
182 (46,592 m) already overflows.

### Why we do not just widen the multiply

Because it would make things worse. `nx` and `ny` are stored as `int32` at
`+0x40`/`+0x44` and every access computes an index like `y*nx + x`. A correctly
sized vector would still be addressed with wrapped negative indices past 2³¹
cells — **silent memory corruption instead of a clean abort**. Fixing it properly
means auditing every indexing site in the streets pass, and one missed site has
no symptom.

### What we do instead: scale the cell size (`street_raster=1`)

`cellSize` is an *argument* (`xmm2`), so the plugin detours the constructor and
grows it until the cell count fits a budget. `nx` and `ny` shrink, so every
downstream int32 index stays in range untouched — no audit, no corruption risk.

| map | cell | cells | vs INT_MAX | raster |
| --- | --- | --- | --- | --- |
| 24.6 km (stock) | 1 m | 0.60e9 | 28% | 75 MB — **untouched** |
| 57.3 km | 2 m | 0.82e9 | 38% | 103 MB |
| 114.7 km | 3 m | 1.46e9 | 68% | 183 MB |

Below the budget it is a no-op, so normal maps keep their 1 m grid and behave
exactly as before. The cost above it is road-placement granularity — 2 m instead
of 1 m, against roads 10–20 m wide.

### The 32,768 m wall (the real ceiling, for now)

A 320-tile map (±40,960 m) generates fine and then corrupts itself during play.
The street builder creates **duplicate base nodes** — 2 to 5 nodes at one
position — and every town-development or industry-connect step that touches one
fails down the same chain:

```
Duplicate base nodes found at world position: (36918 / 17379 / 6.75) with entitiy IDs ...
 Trying to merge duplicate nodes with 2005741
 Base node deduplication failed.
 Merging did not succeed, trying to delete now.
 Trying to delete duplicate node 2005741
transition_util.cpp:41 GetNodeShapeAttributes: Assertion `ctx.size() >= 2' failed
```

168 times in half an hour. Towns in the affected band never get streets, so they
generate with **zero population**; terrain LOD is visibly wrong in the same band.

The positions are the tell. All 21 distinct duplicate positions have
`max(|x|,|y|)` between **34,175 and 40,082 m — not one inside 32,768 m** — spread
evenly over all four edges. A 224-tile map (±28,672 m) shows none of it.

**32,768 = 2¹⁵ = 256 tiles ÷ 2 — and it is `ecs::OctreeSystem`'s root box.** Every
street node, construction and vehicle lives in that octree, and its root is a
two-tier *constant*, never derived from the map:

```
0x1402304de  cmp dword [r14], 0x80        ; numTilesX > 128 ?
0x1402304ee  cmp dword [r14+4], 0x80      ; numTilesY > 128 ?
0x1402304f8  movss xmm2, [32768.0f]       ; > 128 tiles: ±32,768 m
0x140230500  mov   edx, 0xa               ;              depth 10
0x140230509  call  Octree::Resize
```

(≤ 128 tiles gets ±16,384 m / depth 9. There is no third branch.) Inserts never
test containment, so an entity past the box just walks to the boundary leaf.
Every query prunes on node boxes *first*, so anything beyond ~32,832 m is
invisible to lookups — and the street builder, finding nothing there, stacks a
new node on top of the old one. That is the whole bug.

The `Duplicate base nodes found` lines are printed by a *load-time* repair pass,
not at creation: the duplicates were made silently during play and reported when
the autosave reloaded. The repair's merge fails on them and its delete path is
what trips `ctx.size() >= 2`. An existing 320-tile save is therefore not
repairable — regenerate.

**The fix (`octree=1`)**: a 13-byte in-place rewrite at `0x1402304f8` that puts
`65536.0f` inline and asks for depth 11:

```
b8 00 00 80 47     mov  eax, 0x47800000    ; 65536.0f
66 0f 6e d0        movd xmm2, eax
31 d2              xor  edx, edx
b2 0b              mov  dl, 0xb            ; depth 11
```

`eax` is dead there and the length is identical, so nothing shifts. The
`32768.0f` in `.rdata` sits in a `{64, 16384, 32768, FLT_MAX, −90}` run with
~100 readers and is deliberately **not** touched. Depth 11 is the cap of the original node-ID scheme, so the default patched
ceiling remains **512 tiles (131 km)** with 128 m leaves. The default patch is
applied only when the configuration asks for a size over 256 tiles.

**Experimental actual depths 12 and 13 are available.** Set `octree_depth=13`
and `max_tiles=2048` for **2,048-tile (524.288 km) edge capacity**, or depth 12
and `max_tiles=1024` for 262.144 km. Both retain **128 m leaves**. The patch
assigns compact IDs to levels 11/12 and updates the renderer's level decoder.
It is byte-verified and tested offline against original engine insertion
instructions, and **validated in a running game** on GOG: a 57 x 57 km map was
created, played and reloaded with no duplicate street nodes and no assertion.
Heightmap area
limits still apply, so the longest maps must be narrow. See
[the implementation and test notes](docs/octree-depth12.md) for configuration,
evidence and remaining live checks. Defaults retain depth 11.

**Steam 35924 only.** Depths 12 and 13 replace two prologues the GOG build does
not share, so there `octree_depth=12`/`13` fall back to the depth-11 root
instead of refusing to load: the plugin still loads, the ceiling stays 512 tiles,
and the log says which depth was asked for and what went in.

```
[host] tpf2_bigmap: octree: octree_depth=13 is Steam 35924 only -- this build keeps depth 11 and loads,
                     so the edge ceiling stays 512 tiles, not 2048
```

That fallback is why the shipped config can keep `octree_depth=13` for both
builds: Steam gets 2,048-tile edges, GOG gets the valid 512-tile root and works.

Terrain LOD at the edge was **not** traced to the same limit. The only
terrain-side 32,768 is an asymmetric legacy vertex packer (tiles −128..895),
which can't produce a four-edge effect. If the edge renders right with `octree=1`
and wrong without, the LOD symptom was octree-driven render culling.

### Industry founding after start

A 320-tile map placed **387 industries — exactly** `round(6711 km² × 0.0576)` —
and then kept founding more. The spawner was reversed to find out why:

- **What it counts (N):** every `Construction` entity with a non-empty
  `simBuildings` list. No filter on road connectivity, level or town distance —
  the 239 unconnected industries count fully.
- **What it targets (T):** `round(map_area_km² × targetMaxNumberPerArea)`. Whole
  map, no town term.
- **Two timers**, both registered only if `spawnIndustries` is true *and* both
  densities are positive: a fixed-cadence one (`spawnTargetTimeSpan / n`, one
  attempt per fire, logs `Connect industry`) and a daily probabilistic one
  (`p = ((T−N)/T)^spawnProbabilityExponent` per newly-supplied building, silent).
- **Both are gated strictly `N < T`.** The static code cannot found at `N ≥ T`.

So the "counts only connected" and "per-town" theories are both refuted, and the
foundings past 387 mean a runtime premise was off — most likely
`targetMaxNumberPerArea` not carrying the density scale. The added density levels
set the target and the start density from the same multiplier, and
`tools/test_newgame_menu.py` asserts both.

**The switch that works regardless:** `Industry density target: Disabled` in the
New Game menu, which sets `spawnIndustries = false` and registers neither timer.
Closures still happen (that system never reads the flag), so closed industries
are not replaced; add `closureProbability = 0` to freeze those too. Raising
road-connect success does **not** touch the founding rate — connectivity is in
neither N nor T.

### Town and industry levels: the density levels

Counts are a **fixed density per km²**, so they scale with area. A 57 × 57 km
map is 3,288 km² — 5.4× the largest map the game ships — and generates ~1,600
industries and ~200 towns, which is neither fun nor quick to generate.

**Two multipliers are applied before ours, and neither defaults to 1.0.** This is
the easy thing to get wrong, and getting it wrong overstates every count by
1.7–3.3×:

| | base density | stock dropdown default | effective |
| --- | --- | --- | --- |
| towns | 0.2 /km² | Medium ×0.3 | 0.06 /km² |
| industries | 0.8 /km² | Medium ×0.6 | 0.48 /km² |

Town multipliers are `{ Low 0.2, Medium 0.3, High 0.4, Very high 0.5 }`, applied
**engine-side** — they are not in the shipped Lua. Read from the dispatch sites
(`0x142f304c8`=0.2, `0x142f28810`=0.3, `0x142f65170`=0.4, inline `0x3f000000`=0.5),
and independently confirmed by a real map: a 57 km map at the old hand-patched
0.0367 /km² generated **36 towns**, and `0.0367 × 0.3 × 3288 = 36.2`. That also
settles a discrepancy this README used to record as unexplained — the "36 towns
where the formula predicts 115" was the ×0.3, nothing to do with
`allowInRoughTerrain`. Industry multipliers are `{ .4, .6, .8, 1.0 }` from
`base_mod.lua:280`.

The plugin adds six levels to the end of the vanilla *Towns*, *Number of
industries* and *Industry density target* dropdowns, after *Very high* (see *How
the levels get there* below). Each level is a scale on the stock **Medium**, which
stays the default. Counts are for a 57 km map:

| level | scale | towns | industries |
| --- | --- | --- | --- |
| Medium (the stock default) | ×1.00 | ~197 | ~1578 |
| Reduced | ×0.50 | ~99 | ~789 |
| Sparse | ×0.30 | ~59 | ~474 |
| Megalomaniac count at 56 km | ×0.18 | ~36 | ~284 |
| Minimal | ×0.10 | ~20 | ~158 |
| Megalomaniac count at 112 km | ×0.046 | ~9 | ~73 |
| Megalomaniac count at 160 km | ×0.022 | ~4 | ~35 |

**A fixed multiplier does not hold a count as the map grows.** The scale needed to
keep Megalomaniac's 36 towns / 290 industries is just `604 / area`, so it falls
off ~4× every time the map's edge doubles: ×0.18 at 57 km, ×0.089 at 82 km,
×0.046 at 115 km, ×0.022 at 164 km. That is why the bottom two rungs exist and
why they are named for a size rather than a number — without them the lowest
setting still produced 631 industries at 115 km and 1,288 at 164 km. Pick the
rung that names the size you are generating:

| rung | at 320² (82 km) | at 448² (115 km) | at 640² (164 km) |
| --- | --- | --- | --- |
| ×0.18 | 72 / 580 | 142 / 1136 | 290 / 2319 |
| ×0.10 | 40 / 322 | 79 / 631 | 161 / 1288 |
| ×0.046 | 19 / 148 | **36 / 290** | 74 / 593 |
| ×0.022 | 9 / 71 | 17 / 139 | **35 / 283** |

The rung names are measured targets, not guesses: Megalomaniac 1:1 is 604 km²,
which gives **36 towns and 290 industries** at the default stock dropdowns, and
×0.18 on a map 5.4× that size (224 tiles) reproduces those counts to within 2% —
hence its name. The ×0.046 and ×0.022 rungs are pinned the same way at 448 and
640 tiles. The names use the km **the game shows** for a size, which is tiles ÷ 4
(a 224-tile map reads "56 km" although it measures 57.3 km), so they match the
size dropdown's rows.

#### How the levels get there

The New Game page draws *Towns*, *Number of industries* and *Industry density
target* from base_mod's own params, so the new levels are extra entries in those
lists — the same way the size rows are extra entries in the size dropdown. Nothing
is added to the page and no stock index moves: the six levels sit after *Very
high*, as indices 4–9. Each list's consumer then has to understand those indices
(`newgame_density=1`, Steam 35924 only):

1. **Number of industries and Industry density target** reach `runFn` as indices
   into `industryFreq = { .4, .6, .8, 1.0 }`. The page stores the industries choice
   as the start index and that + 1 as the target (past *Disabled*), so one
   appended multiplier per level (Medium ×0.6 × scale) serves both.
2. **Towns** never reaches Lua. The preview/generation params refresh
   (`0x14065b620`) maps the index inline — `0 → 0.2, 1 → 0.3, 2 → 0.4, 3 → 0.5`,
   anything else `→ 1.0` — into the params object Start generates from (Start only
   logs the index, and no other code in the exe maps it). The plugin redirects the
   *3 / anything else* tail of that switch to a small stub: 3 stays 0.5, the new
   levels answer Medium ×0.3 × scale, anything else stays 1.0. The instruction the
   other cases jump to stays in place, and the switch is byte-verified first.
3. **The labels and the `industryFreq` tail** go into `res/config/base_mod.lua`.
   The plugin **patches the game's file on every start**: it takes the stock text
   (the file itself, or the `base_mod.lua.bigmapbak` backup once the file is
   patched), applies anchored inserts, requiring each anchor to match exactly once,
   and writes only when the result differs. A game update or Steam's *verify
   integrity* is re-patched on the next start; a file whose anchors moved is left
   alone, levels off, with the reason in the log.

Design points:

**It scales Medium, it does not replace anything.** The four stock levels and the
Medium default behave exactly as in vanilla; the new ones continue below them. The
lower levels are named for the map size at which they reproduce Megalomaniac's own
counts, because "×0.046" means nothing on its own.

**A save made with an added industry level needs Big Maps to load.** `runFn` runs
on *load* as well as at generation, with the index stored in the save, and the
stock `industryFreq` has no entry past *Very high* — without the patch that is a
Lua error, not a fallback. So **every player of a multiplayer game on such a map
needs Big Maps**, and before removing it, set *Number of industries* back to a stock
level (the page remembers the choice). Whenever the levels cannot go live (the GOG
build, a byte mismatch, `newgame_density=0`) the plugin puts the stock
`base_mod.lua` back, and uninstalling does too. Towns only matter at generation, so
no save depends on the stub.

`runFn` prints `[tpf2_bigmap] industry density level N: x…` to the game's stdout
when an added industry level is in use. `tools/test_newgame_menu.py` tests it
offline: the patcher cases through the DLL's own code, the uninstall restore
through rundll32, the Towns stub's machine code called directly, and the patched
`base_mod.lua` under Lua 5.2. The standalone `mod/bigmap_density_1` this replaces
is gone — its settings only ever appeared in the mod list, and with both installed
a map would be scaled twice.

### What we could not do: relabel the size dropdown

The size6 ratio ladder (an opt-in example in the cfg) reuses the *ratio* dropdown, so it still reads "1:1 … 1:5" while
selecting a size. That is not fixable from a mod, and the reason is worth
recording so nobody retries it:

```
res/scripts/mod.lua:165
    local txt = translateModStr(_currentModIdTr, _locale, concatId)
```

`pGetText` resolves a string against the **currently executing mod's** own table
only. A mod's `strings.lua` can retranslate strings its own Lua asks for; it can
never reach a string the base game resolves. `"1:1"` is looked up by the C++ New
Game menu under the base catalog, with no mod in scope.

The msgids are real and confirmed — `'1:1'`, `'1:2'`, `'1:3'` and
`'map-size\x04Megalomaniac'` all live in `res/strings/*/LC_MESSAGES/base.mo` —
so the only ways to change them are editing `base.mo` (a game file, reverted by
Steam) or a DLL hook on the text lookup. The entries this plugin adds are
unaffected: their labels are strings it supplies itself — `size_label<N>` for the
size rows, the Lua it inserts into `base_mod.lua` for the density levels.

The DLL route is viable if it ever becomes worth it. `0x14221d1b0` is
`pgettext(std::string* out, const char* ctx, const char* msgid)` — 172 xrefs, all
UI text, with 11 clean relocatable prologue bytes. Two traps: the `mov r11,rsp`
must be **copied into the trampoline**, not skipped, because `r11` is used later
as a frame base; and the hook must gate on the **context**, not the msgid, since
`"Tiny"`/`"Small"`/`"Large"`/`"Huge"` are the same `char*` literals the town-size
dropdown uses. Call the original first — `*out` is uninitialised on entry — then
append rather than replace, so all 13 languages keep working.

A better idea than relabelling, if the guard below gets written: claim one row
**per size** (4, 5 and 6) instead of spending the ratio dropdown. Then every
label stays honest — "Very Large < Huge < Megalomaniac" is still ordinally true
after a remap, and "1:3" still means 1:3 — with no text hook at all. Nothing on
the New Game page displays a derived tile count to contradict it:
`CreatePageNewGame` (`0x14066c2b0`) never calls `GetNumTilesNew`.

**That remap is gated on a guard this plugin does not yet have.** `Detour()`
receives the raw combo index, and at `0x140674b2f` the engine keys the preset
table on `sizeIndex` when `experimentalMapSizes` (`GlobalSettings+0x2fc`) is set
but on `sizeIndex+1` when it is clear. With the flag clear a `size4`/`size5`
claim would land on a different, stock preset — silently redefining a normal map.
The cfg's size6 example is safe from this only because it claims size 6, which is
unreachable unless the flag is on.

### Adding size rows instead of taking a vanilla one (`add_size_rows`)

`add_size_rows=1` (the default; Steam 35924 only) **appends** entries to the
vanilla size dropdown rather than overwriting a stock preset. The dropdown is built
at one call site (`0x14066ce79`) by a combo factory (`0x142326290`) from a
`vector<std::string>` of names: base_mod.lua's four (Small … Very Large) with
*experimental map sizes* off, the C++'s own seven (Tiny … Megalomaniac) with it on.
The plugin hooks the factory, acts only for that call's return address
(`0x14066ce7e`), records how many stock rows there are, and appends its own
`size_label<N>` rows by rebuilding the vector with the engine's own constructor.

`GetNumTilesNew` gets the **raw combo index** from all three of its callers, so the
detour reads `index >= stock rows` as added row `index − stock rows` and answers it
from that row's `size<N>_format<F>` claims before the engine's own lookup. That is
what lets the rows work with the flag **off** too — there the engine would add 1 to
the index, building a stock preset, and assert past 6. It also sidesteps the
`experimentalMapSizes` keying trap above: an added row is recognised by its
position after the stock rows, whichever list is showing. The shipped cfg defines a
ladder of squares labelled 32 x 32 km to 128 x 128 km — the km the game itself
shows for those sizes (tiles ÷ 4). The ratio dropdown shapes them like the stock
sizes: 1:k keeps about the square's area (short side = side ÷ √k to an even tile
count, long side k times that), the layout of the game's own preset table
(`0x140881070`: Megalomaniac 96², 66×132, 54×162, 48×192), with the long side
capped at 512 tiles. A `size<N>_format<F>` cell set in the cfg overrides that one
shape. A byte mismatch or the GOG build skips the hook and the dropdown stays
stock.

## Install

**Install TpF2 Multiplayer** (`TpF2Multiplayer.msi` from the
[latest release](https://github.com/silver2127/tpf2-multiplayer/releases)): Big
Maps ships inside it. It finds the Transport Fever 2 folder Steam registered,
asks you to confirm it, and puts these in place (besides the multiplayer files):

| file | what |
| --- | --- |
| `alut.dll` | the proxy the game loads in place of its own (the original is kept as `alut_real.dll`) |
| `tpf2_pluginhost.dll` | the plugin host the proxy loads |
| `plugins\tpf2_bigmap.dll` | this plugin |
| `plugins\tpf2_bigmap.cfg` | its settings — the size ladder, `octree`, `street_raster`. Replaced on install, upgrade and repair with the release settings |

At game start the plugin also adds the density levels to the game's own
`res\config\base_mod.lua`, keeping the stock file beside it as
`base_mod.lua.bigmapbak` (see *How the levels get there*).

It also sets the Segment Heap switch for `TransportFever2.exe` (a registry
value, removed on uninstall) — that is what makes a big map load in about a
minute instead of a quarter of an hour; the measurements are in the
[multiplayer installer README](https://github.com/silver2127/tpf2-multiplayer/blob/main/installer/README.md#segment-heap).

### GOG

Use **`TpF2BigMaps-<version>-gog.msi`**. It is the same package with one
difference — it tells the folder check that the GOG build of the game is a
supported target — so it validates the folder exactly like the standard one
instead of refusing it. A standard package can do the same from the command line
with `TPF2_ALLOW_GOG=1`; without it a GOG folder is refused, because the check
knows only the Steam executable and would rather say so than install into a game
it cannot patch.

On GOG the folder dialog starts on the Steam default, which does not exist
there: point it at the game folder once, and every later install remembers it.

Then: **New Game**. The size dropdown has rows after the stock sizes, from
32 x 32 km up to 128 x 128 km, with *experimental map sizes* on or off, and
*Towns*, *Number of industries* and *Industry density target* have six more levels
after *Very high* — pick the one that names the size you chose. Every stock size,
Megalomaniac included, stays vanilla, and the ratio dropdown shapes the added rows
the way it shapes the stock ones. To set a shape yourself, add a
`size<N>_format<F> = <w>x<h>` cell -- both axes even, each up to 512 tiles with
`octree=1`, and the area within the street-raster budget (`street_raster=1` scales
the cell to keep it there).

### The old TpF2 Big Maps installer

Up to 0.5.x Big Maps had its own MSI (`TpF2BigMaps-<version>.msi`), which
coexisted with TpF2 Multiplayer by sharing the proxy and the plugin host under
fixed component GUIDs (`installer/PluginHost.wxs`). The TpF2 Multiplayer MSI now
lists that product's UpgradeCode and removes it when it installs, so the plugin
has one owner; the shared components are reference-counted, so nothing is lost
in between, and the old package's base_mod restore does not run during that
removal (the new plugin re-patches on its next start).

### Virus-scanner findings

The DLLs and the MSI are not code-signed, so any rule of the form *unsigned
image loaded* matches them wherever the game loads them. One report (a mod site's
sandbox scan, 2026-09-20) went further and matched *Unsigned Image Loaded Into
LSASS Process*: its event named `C:\p250ps71\dll\OrqiEaoo.dll`, a randomly
named file that is the sandbox's own monitoring hook, injected into every
process including `lsass.exe`. Nothing here loads into any process but the
game: the installer places the files in the table above and two registry
strings under `HKLM\SOFTWARE\silver2127\TpF2 Big Maps`, registers no service,
no security package, no `AppInit_DLLs`, and the plugin sets no hooks and opens
no other process. The one custom action, on full uninstall only, runs
`rundll32` on the plugin to put the stock `base_mod.lua` back. Compare the
SHA-256 in such a report with the release assets' digests on the GitHub
release page; they will not match.

### Uninstall

Add/Remove Programs → **TpF2 Big Maps**. Puts the stock `res\config\base_mod.lua`
back (the plugin's own restore, run through rundll32 before its files go), then
removes the plugin, its config, and — if TpF2 Multiplayer is not installed — the
proxy, the plugin host, the Segment Heap value, and restores the stock `alut.dll`. Steam's
*Verify integrity of game files* also puts the stock `alut.dll` back without
uninstalling anything; **Repair** from Add/Remove Programs reinstalls the proxy.

## Build

Needs VS 2022 Build Tools.

```
build.bat            -> out\tpf2_bigmap.dll
build.bat -deploy    -> also copies into %LOCALAPPDATA%\tpf2mp\data\plugins\
python tools\test_newgame_menu.py   -> offline test of the New Game rows' base_mod.lua patch (pip install lupa)
```

For a dev setup without the MSI: install the tpf2mp plugin host from the
multiplayer repository (`tpf2_pluginhost.dll` + the `alut.dll` proxy), drop
`out\tpf2_bigmap.dll` and `cfg\tpf2_bigmap.cfg` into `<game>\plugins\`. If an
older build left `<game>\mods\bigmap_density_1` behind, delete it: with it enabled
a map would be scaled twice.

### Building the MSI

```
tools\vendor_host.ps1 -FromMsi TpF2Multiplayer.msi -Release v0.4.18
                                      # alut.dll, tpf2_pluginhost.dll, tpf2ca.dll out of the
                                      # latest TpF2 Multiplayer release MSI; the source goes
                                      # into installer\vendor\VENDORED.md
installer\build_msi.ps1 -Validate -AcceptWixEula
```

The three shared binaries are built in the multiplayer repository and vendored
here unchanged: both packages must ship the same bytes under the same GUIDs.
Vendor them from the **latest multiplayer release MSI** before each release. A
rebuild of the same commit gives different bytes, so an install of one product
could replace the other's copy. `tools\vendor_host.ps1 -Build` vendors from a
checkout's build outputs instead (dev only). `build_msi.ps1` refuses to build if
`PluginHost.wxs` has drifted from the multiplayer copy (line endings aside). WiX v7 asks you to accept its
[OSMF EULA](https://wixtoolset.org/osmf/); `-AcceptWixEula` passes it
per-invocation and nothing accepts it for you.

GitHub Actions runs the same script (`.github/workflows/build-msi.yml`): every push to `dev` or `main` and
every pull request builds `TpF2BigMaps-<version>.msi` and `SHA256SUMS.txt` on a `windows-2022` runner from the
vendored shared binaries and keeps them as the run's artifact; a `v*` tag (which must equal `installer\VERSION`)
also creates a draft GitHub release with them attached, ready to be edited and published. The runner has no
multiplayer checkout beside this one, so the workflow compares `PluginHost.wxs` with the copy at the release
named in `installer\vendor\VENDORED.md` instead. The workflow passes `-AcceptWixEula`, which is the
repository owner accepting the WiX terms for those builds.

## Verifying it worked

`%LOCALAPPDATA%\tpf2mp\data\tpf2mp_host.log` shows the hook lines, the
`octree:` line, `merged ...\plugins\tpf2_bigmap.cfg`, and for the New Game menu
`base_mod.lua: ...`, `density levels: live` and `size rows: live` at start, then
`size dropdown: N stock row(s) + 9 added` each time the page opens. Then generate, save,
and read `numTilesX`/`numTilesY` back out of the `.sav` header with the snippet
near the top of this file — that proves the value survived generation *and*
serialization, which is stronger than trusting the log.

The same log carries the pagers' `resident target` lines: one at start with the
auto budgets for this machine's RAM, a few during the load, and then quiet. See
*Reading the log* under Memory.

## Extended map ratios

New Game ratios can now extend through **1:20** with `max_ratio=20` (Steam).
Both stock and added size rows are supported; map-edge and heightmap limits
still apply. See [map-ratios.md](docs/map-ratios.md) for dimensions and validation.

## Memory: what a big map costs, and how the plugin keeps it in check

**What the game itself needs.** Most of a big map's memory is the engine's own
data, and no setting here changes that. Measured 2026-09-20: a freshly generated
large stock map was **15 GiB private** at the end of generation before the
plugin's caches held a byte, and a 44 MB save of a mid-sized world loaded to
about 6.6 GiB. A 16 GiB machine cannot hold a 128 x 128 world however the caches
are set; a 32 GiB one holds it at around 20 GB.

**What the plugin adds, and controls.** The 1 m terrain cache and the material
grid are the two engine structures that scale with the map (see
[terrain-compression.md](docs/terrain-compression.md) and
[material-grid-lifetime.md](docs/material-grid-lifetime.md)). The plugin routes both through
pagers that keep a *resident target* of tiles uncompressed and hold the rest
compressed, losslessly, decoding a tile when the engine touches it (about
0.2 ms each). Shipped on since 0.3.1. Once a second each pager sets its target
from four rules, in this order:

1. **Loading allowance.** While a world generates or loads, everything live may
   stay resident up to what free RAM and free commit allow, less the machine's
   headroom (`PagerHeadroom`: a seventh of RAM, 2..12 GiB). The loader re-reads
   what was just evicted, so evicting during a load only makes it slower.
2. **Steady state.** Afterwards the target ramps down by 1/16 a second toward
   the *hot* budget, grows by 1/8 when the engine faults 300 or more evicted
   tiles back in per second (`cold restores/s` in the log), and holds between.
   Since 2026-09-20 the target the stutter feedback drove it to is kept as a
   **working-set floor** that decays by 1/256 per quiet second (halves in about
   three minutes) instead of sawtoothing back to hot every second the camera
   rests -- on a 32 GiB machine that sawtooth was 1,000-2,000 decodes a second.
3. **The cap.** `terrain_cache_max_mb` bounds the target: **0 = automatic, a
   quarter of RAM clamped to 4096..8192 MiB** (16 GiB: 4096; 32 GiB and up:
   8192), the same size on every machine that can afford it; `-1` removes it,
   and the target then fills RAM down to the headroom, which is smoothest and
   is what made the game 20 GB on a 32 GiB machine. `material_cache_max_mb`
   is the same for material cells, automatic = a quarter of the terrain cap. A
   cap below the map's working set is paid in decodes: at 300 or more
   `cold restores/s` it is costing frames (a 4096 cap did, on a freshly
   generated large map).
4. **Commit pressure.** When free commit (RAM plus page file) drops under
   `CommitTightBytes` (an eighth of RAM, 2..10 GiB) both pagers throttle to
   256 MiB and evict urgently; when free RAM drops under the headroom they
   shrink by 1/8 a second. The throttle is **sticky**: it holds until free commit
   clears the threshold by more than the pager itself gives back, and for at
   least 30 s. Without that, on a machine with no page file sitting 1 GiB under
   the threshold, the pager's own 3 GiB release cleared it, it re-expanded, and
   the flag set again -- 74 flips in one session, each re-inflating the terrain
   in front of the camera: a stutter at every close zoom (2026-09-20).

**Budgets and knobs** (`plugins\tpf2_bigmap.cfg`, restart to apply):

| key | default | meaning |
| --- | --- | --- |
| `terrain_cache_hot_mb` | `0` = auto: RAM/30, 256..4096 | the steady-state floor (94 GiB: 3195; 32 GiB: 1092) |
| `terrain_cache_warm_mb` | `-1` = auto: RAM/12, up to 8192 | the allowance while loading |
| `terrain_cache_max_mb` | `0` = auto: RAM/4, 4096..8192 | the cap; `-1` = none |
| `material_cache_hot_mb` / `_warm_mb` / `_max_mb` | auto | the same three for material cells (hot RAM/180, 96..1024; warm RAM/48, up to 4096; cap a quarter of the terrain cap) |
| `terrain_cache_evict_per_s` | `4000` | eviction ceiling at rest; `0` = unlimited |
| `simulate_physical_mb` | `0` = off | **rig-only**: the policy sizes itself for a machine of this RAM, free RAM and free commit scaled to the real machine's fractions; the engine is not constrained. `32768` tests the 32 GiB sizing on a bigger PC |

**Reading the log** (`%LOCALAPPDATA%\tpf2mp\data\tpf2mp_host.log`): a
`resident target A -> B MiB (... commit_tight=N pressure=N free=M MiB)` line
per change of target (a handful per session is normal; dozens means the
throttle is flapping), `N cold restores/s, resident target A -> B MiB (working
set exceeds the budget)` when the engine is re-reading evicted tiles, and the
`world entry:` lines with the process's private and resident size at each
stage of a load.

**If the game is bigger than you want.** Set `terrain_cache_max_mb` lower and
watch `cold restores/s`; below 100 the cap is free, above 300 you are paying
frames for RAM. The rest is the map: pick a smaller size.

The experiments this replaced are kept for the record in
[gameplay-memory.md](docs/gameplay-memory.md): the 2 m derived cache
(`terrain_cache_spacing_m=2`, discontinued after terrain fragments, tile seams
and a rail construction crash -- keep `1`), and the early fixed budgets. The
renderer upload mismatch fix (131x131 buffered CPU samples expanded to the
stock 259x259 GPU upload) is part of the 1 m cache and stays.

## Faster autosaves and manual saves

`save_fast=1` uses zstd level 1 and a 64 KiB input buffer for save streams.
A full-save compression benchmark was 2.79x faster with 9.23% larger output;
manual saves of the current world measured 13.7 and 14.4 seconds. A controlled
same-world comparison with stock total save time remains outstanding.
Save contents and format are unchanged. Set `0` and restart to restore stock
compression. See [save-performance.md](docs/save-performance.md) for validation
and limitations.

## Travel-time limits

`travel_time_limit_s` and `cargo_path_time_s` rewrite the two `.rdata` cells
that hold the urbansim's limits (Steam 35924): 1200 s, the 20 minutes at 1x
within which a cargo type must be able to reach a consumer's station (the same
cell bounds people's paths and destination choice), and 6000 s, the longest path
the path builder gives a cargo item. Values are game seconds, 0 keeps stock,
others clamp to 60..86400. Byte-verified; `python tools\test_travel_time.py`.
Shipped off: on a big map, set `travel_time_limit_s=3600` to let demand reach
towns an hour apart.

## Licence

MIT. See `LICENSE`.

## 0.3.1 configuration reset

The MSI replaces existing `plugins/tpf2_bigmap.cfg`, including user edits, with the tuned release config on install, upgrade and repair. It restores depth 13, a 2048-tile edge cap, 50 placement attempts, material-index acceleration, fast saving, loading diagnostics, and lossless terrain compression at 1 m spacing. Hot/warm cache budgets remain 1024/4096 MiB and the maximum ratio remains 1:20. The discontinued 2 m cache is not enabled.

The config uses the package version in the MSI File table so an existing unversioned, modified config is replaced. `tools/test_config_msi.py --wix <wix.exe>` verifies upgrade and repair with an isolated per-user fixture.
