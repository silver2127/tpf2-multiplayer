> This is the historical standalone Big Maps installation guide imported from
> `4769cd3`. In this repository use the [unified Linux installer](../../../docs/linux/INSTALL.md).
> Its configuration is installed in `tpf2mp/plugins/tpf2_bigmap.cfg`, and
> upgrade/uninstall restores density changes through the packaged helper.

# Native Linux big maps (experimental)

For Steam Transport Fever 2 Linux build 35924. This is a native `.so` plugin,
not a Wine/Proton DLL. The package includes the shared native plugin host;
multiplayer is not required.

1. Close the game and extract `tpf2-bigmap-0.4.0-linux-dev.3.tar.gz`.
2. In the extracted directory, run `bash install.sh` without sudo.
3. Use the Steam launch-options line printed by the installer. If multiplayer
   is installed, keep its existing launch options: it loads this plugin too.
4. Open **New Game**. Nine extra size rows extend the menu through
   **130.56 x 130.56 km** (510 x 510 tiles); ratios extend through **1:20**.

The defaults are installed under
`${XDG_DATA_HOME:-$HOME/.local/share}/tpf2mp/data/plugins/tpf2_bigmap.cfg`.
`--prefix /absolute/path/to/tpf2mp` selects a different shared installation.
Snap Steam is detected when XDG_DATA_HOME is unset and no default host exists;
its prefix is `~/snap/steam/common/.local/share/tpf2mp`. Restart
for configuration changes. Existing configs are kept on upgrade; updated
Linux defaults are placed in `.cfg.example`.

Use the plugin when loading worlds that need its expanded octree. Back up
important saves before testing. Larger maps need substantially more RAM and
generation time. Optional terrain compression reduces settled RAM use, but does
not eliminate the peak memory needed while loading or generating a world.

## Supported scope

- Added size rows, stock presets, extended ratios and explicit size cells.
- Six sparse density presets in Towns, Industries and Industry density target.
- Adaptive street occupancy cells to avoid signed 32-bit overflow.
- Depth-11 octree with a 512-tile edge cap and unchanged 128 m leaves.
  The largest square is 510 tiles; the preview distance calculation imposes
  a separate diagonal bound until its Linux overflow fix is ported.
- Exact SSE2 terrain min/max scan and faster zstd saves, enabled by default.
- Experimental lossless terrain compression using Linux userfaultfd, opt-in.
- Byte/build guards and a shared host that coexists with native multiplayer.

Depth 12/13 (1024/2048-tile edges), material compression, terrain copy sharing,
generation buffer reuse and the remaining renderer optimizations are not ported
in this build. Do not use the Windows configuration: its depth-13
setting is rejected explicitly. See PORT.md for evidence and validation limits.

## Sparse density presets

After **Very high**, all three density dropdowns offer Reduced (0.50), Sparse
(0.30), Megalomaniac count at 56 km (0.18), Minimal (0.10), and Megalomaniac
count at 112 km (0.046) / 160 km (0.022). Values are fractions of **Medium**
density, not fixed object counts. Map area and generator constraints affect
actual counts. Stock Low through Very high keep their original values.

The feature is on by default (`newgame_density=1`, including upgrades with an
older config). Restart the game after installing. Select Towns and Industries
on the first New Game page; the later Industry density target controls future
industry spawning. Existing towns/industries are not removed by these settings.

The plugin patches the installed game's `res/config/base_mod.lua` and keeps
`base_mod.lua.bigmap-linux.bak`. Missing or changed anchors stop initialization;
no extra labels are exposed without the matching native town hook. Steam file
verification is handled by patching the restored stock file on the next launch.
The uninstaller restores the backup only when the patched file is unchanged;
it preserves later manual edits and reports a failure instead of deleting them.

Keep density support enabled when loading saves made with the extra industry
presets: their stored indices need the added Lua multipliers. The big-map plugin
must also be present on other machines loading those saves.

## Memory and performance (dev.3)

`save_fast=1` selects zstd level 1 and a 64 KiB stream buffer. The save format is
unchanged; files can be larger. `terrain_minmax_fast=1` uses an exact SSE2 scan
for terrain height bounds. Both default to on even with an older config.
Set either to 0 and restart to disable it.

To try terrain RAM compression, add or change these keys under `[tpf2_bigmap]`
in your installed config and restart:

```ini
terrain_cache_compress=1
terrain_cache_hot_mb=0
```

This keeps the original 1 m terrain samples and restores identical bytes on
access. The budget is a soft limit for uncompressed terrain, not total game RAM.
Zero selects installed RAM / 30, bounded to 256..4096 MiB; a positive value
selects an explicit MiB budget. This does not account for cgroup limits.
The plugin reserves a large virtual address range; use resident memory (RSS),
not virtual size (VIRT), when comparing RAM use. Compressed data and other game
allocations still need RAM. More cache space can reduce decompression activity.

Compression is **off by default**. It requires a Linux kernel that permits
user-mode userfaultfd with missing-page and write-protection support. Unsupported
systems log `terrain compression unavailable` and continue without compression;
no root permissions or kernel setting changes are required by the installer.
The backend handles userspace faults only: a kernel operation directly accessing
an evicted terrain page is outside its supported path. The tested game paths are
loading, rendering, simulation, track construction and save/reload; other mods,
GPU drivers and long sessions need further testing. See PORT.md for details.

One 128x128-tile save settled at 4.98 GiB RSS versus 7.78 GiB with dev.2, using a
1 GiB terrain budget. Loading peaks were about 9.8 GiB in both runs. This is a
single-machine comparison, not a guaranteed saving or a frame-rate benchmark.

## Remove

Run `bash uninstall.sh` (with the same `--prefix` if supplied). It removes only
the big-map plugin and its standalone launcher. Config, saves, multiplayer and
the shared plugin host remain. Remove `tpf2-bigmap-launch` from Steam options
if you used it; an existing multiplayer launch line should stay.

Logs: `<prefix>/data/tpf2mp_host.log`. Look for `tpf2_bigmap ... -> OK (0)` and
`size dropdown: ... stock + 9 native Linux rows`.

Experimental `alignment_batch_tiles=512` enables native alignment batching.
It defaults to 0 (stock): loaded-world lifetime and memory acceptance remain
unvalidated because the revisit lab could not initialize Steam. See PORT.md.
