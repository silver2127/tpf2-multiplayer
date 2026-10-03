# Terrain stream: the host's sidecar, while the joiner loads

A save's terrain sidecar (`<save>.terr`, [terrain-sidecar.md](terrain-sidecar.md)) holds every tile's finished
height cache. A load of that save restores the tiles from it and skips the alignment pass (44-75 s on a
50k-tile map; [alignment-batch.md](alignment-batch.md)). A joiner never had one: the save it receives is
new to it. Waiting for the sidecar before loading would cost more than it saves on a slow link (a big
map's is ~600 MB, about 16 KB a tile, and it does not compress). So the host **streams** it while the
joiner loads, and the joiner uses as much as has arrived.

## Flow

1. **Host (lobby).** START with a save (`broadcast_start`) begins `begin_terr_stream`:
   - It picks `<save>.terr` beside the shared `.sav` (`_terr_for_save`: the right magic, and not older
     than the save).
   - A worker thread reads and hashes it.
   - The loop sends it as a transfer of kind `terr` (`_HostSaveTransfer`: UDP, TCP or pipe, as a save
     goes) to the peers just started that said `terrain_stream: 1` when they joined.
   - Nothing waits for it, and a later START replaces it.
2. **Joiner (lobby).** `_TerrReceiver` writes `<data>/terrain_stream/<sid>.terr`:
   - Chunks are written **in order**. One that arrives ahead of the written prefix waits in a stash of
     up to 4,096 chunks; past that it is not taken, and the host resends it. So only a valid prefix is
     ever on disk.
   - The file is flushed every lobby tick.
   - It is never held whole in memory, and nothing is shown in the panel.
   - It is verified by SHA-256 at the end. A mismatch deletes the file.
3. **Joiner (plugin), at load.**
   - `ArmForLoad` hashes the save, as for any load.
   - With no sidecar beside it, `BeginIfPending` looks in `terrain_stream/` for a file whose header
     carries that hash (`FindStream`). The file may not have appeared yet, so it looks again every
     250 ms from AddTile (`TryStream`).
   - A stream is loaded as far as it goes (`BeginApply(..., stream=true)`) and kept open.
4. **AddTile.** A tile whose record has arrived is served; a miss re-reads the file's new bytes, at
   most every 100 ms (`RefreshIfDue`). A tile that has not arrived is computed as usual.
5. **The alignment pass** (`SettleStream`, at each CTerrain version's pass):
   - If the stream is still arriving, the pass **waits** for it while the rest, at the rate seen
     during the wait, would arrive sooner than the pass could compute the unserved tiles (1 ms each;
     on Linux every tile, since the pass there computes all unless all are served). It gives up after
     5 s without data, or 300 s in all.
   - Then every tile that arrived but was not served at AddTile is served (`CatchUp`, 8 threads).
   - If every tile is now served, the pass is skipped. Otherwise it runs. On Windows it still skips
     publishing into the served tiles, so a partial stream saves roughly its share.
6. **Done.** When the load releases the sidecar (`EndApply`), the plugin writes `<stream>.done`. The
   joiner's lobby sees it within a second and cancels the rest: after the load, the stream would only
   compete with the game's traffic.

A completed stream stays in `terrain_stream/` until the next one starts, so a reload of the same save
finds it at once.

## Safety

- The plugin uses a stream only for **this** save: its header carries the save's hash, as a sidecar
  beside the save does. Every tile's blob carries its own content hash, so a damaged tile decodes to
  false and is computed instead.
- A CTerrain version whose pass was already skipped is never skipped again, and never gets tiles
  caught up (`VersionFinished`). A big pass later in play always runs.
- An older joiner never gets a `terr` fbegin (the host sends only to peers that said
  `terrain_stream`), and one without the plugin simply never reads the file.

## Switches

`tpf2_bigmap.cfg`: `terrain_stream=0` stops the plugin reading streams. The host lobby's `TERR_STREAM`
turns sending off.

## Tests

- `bigmap/linux/tests/sidecar_test.cpp` section 10, with a real file growing:
  - odd-sized pieces, including a split record;
  - half there at AddTile, the rest during the pass's wait;
  - a stream found only at the pass;
  - a pass in play after a skip.
- `bigmap/tools/test_terrain_serve.cpp`: a stream found only at the pass on Windows serves every tile
  there, and the pass is skipped.
- `tools/test_terrain_stream.py`, a real host transfer and receiver on a lossy, reordering wire:
  - only the in-order prefix is ever on disk;
  - a hash mismatch deletes the file;
  - the done marker stops the transfer;
  - refusals;
  - the host's choice of sidecar.
