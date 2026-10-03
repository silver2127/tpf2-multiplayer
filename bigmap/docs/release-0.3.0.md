# TpF2 Big Maps 0.3.0

Download and run **TpF2BigMaps.msi** with Transport Fever 2 closed. Steam build
35924 is supported. Existing `plugins/tpf2_bigmap.cfg` settings survive upgrades.

## Changes

- New Game aspect ratios through 1:20 (`max_ratio=20`).
- Experimental actual octree depths 12 and 13, keeping 128 m leaves. Depth 13
  supports a spatial root covering edges up to 2,048 tiles / 524.288 km; practical
  generation limits and RAM requirements still apply.
- Fix placement-distance overflow on large maps, plus configurable placement
  attempts (`placement_attempts=50` for the faster option, stock default 200).
- Optional lossless 1 m terrain cache compression, with improved tile encoding,
  reuse of unchanged compressed tiles, and shared compressed copies.
- A temporary larger cache allowance during generation and late loading.
- Optional faster autosave/manual-save compression and material-index processing.
- World-entry phase diagnostics, including town/industry road connection timing.
  Road routing itself is unchanged.

## Enabling optional features

Edit the existing `[tpf2_bigmap]` section in `plugins/tpf2_bigmap.cfg` and restart:

```ini
# Lossless terrain compression; retain stock 1 m samples
terrain_cache_spacing_m=1
terrain_cache_compress=1
terrain_cache_hot_mb=1024
terrain_cache_warm_mb=4096

# Optional performance changes
save_fast=1
material_index_fast=1
placement_attempts=50

# Extended ratios and optional diagnostics
max_ratio=20
world_entry_timings=1
```

These performance options are opt-in; the MSI retains conservative defaults.
Fewer placement attempts can change placement quality or accepted site counts.
Faster saves retain the format but may produce larger files.

For depth 13, also use `octree=1`, `octree_depth=13`, `max_tiles=2048`, and
configure larger size entries as described in [the depth documentation](octree-depth12.md).
Keep matching depth settings when reloading those worlds.

**Do not enable the discontinued 2 m terrain cache.** It caused visible terrain
fragmentation and construction crashes. Use 1 m with the lossless cache option.

Standalone Big Maps uses a three-minute late-loading cache tail. A fresh-gameplay
UI signal is supported when a compatible future multiplayer menu DLL is present;
that development menu DLL is not included in this installer. The latest cache-tail
policy and road timer have regression coverage but still need live-game validation.

The separate Lua generation-memory helper remains a source/tool option and is
not installed by this MSI.

## Validation and packaging

Ten regression scripts passed: ratios, octree, placement distances, material
indexing, saving, terrain cache, terrain compression, warm-up policy, world entry,
and the generation-memory helper. The MSI was administratively extracted and its
plugin, configuration, proxy, and host hashes matched the intended payloads.
Installer coexistence transactions were not rerun for this release.

Earlier lossless-cache gameplay checks included world loading, two connected rail
builds, and save/reload. Terrain backing settled near 2.55 GiB on the tested
114 x 570-tile world; this is not a promise of total process RAM on other maps.

Shared proxy, host, and installer custom-action binaries are unchanged from the
published TpF2 Multiplayer v0.4.19 MSI. See `installer/vendor/VENDORED.md` for hashes.
