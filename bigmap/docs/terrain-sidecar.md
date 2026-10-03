# Terrain sidecar: reuse a save's aligned 1 m cache on load

The load-time terrain alignment pass (`docs/alignment-batch.md`) rebuilds the
1 m height cache every load: the bicubic refine turns the 4 m base heightmap
into a 1 m base cache, then the alignment pass cuts every road, track and
construction into it. Batching (`alignment_batch_tiles`) removed its memory
spike (35 GiB -> 8.3 GiB on a 207,360-tile save), so what remains is its
**time**. The finished cache is a pure function of the save, so writing it
beside the `.sav` and restoring it on load of the *same* save skips both the
refine and the pass.

## Status

**Wired since 2026-09-21** (`src/terrain_sidecar_io.h`): every save writes
`<save>.terr` and every load arms it. Before that nothing wrote or armed a
sidecar and `terrain_sidecar=1` was inert. How it works now:

- **SaveGame** (`0x2e97c0`) is detoured. The terrain is captured at the
  detour's *entry*, into `<save>.terr.tmp` with fingerprint 0: the world is
  frozen for the length of the call, so the capture is exactly what the save
  holds. When the engine returns and the `.sav`'s write time has moved, the file
  is stamped with the new `.sav`'s hash (`Refingerprint`) and renamed over
  `<save>.terr`. A failed save leaves no sidecar. Capturing after the engine
  returned would race the first alignment of the resumed simulation.
- **LoadGame** (`0x2e5ec0`) is detoured: it clears `g_alignmentTerrain`, resolves
  the save's path, hashes the `.sav` and arms the sidecar (`ArmForLoad`). The
  first `AddTile` of the load opens it, as before.
- **The path** comes from the `SaveGameId` both functions receive
  (`{ wstring path; string name; string ns }`): `<path>\<name>.sav`, and for an
  id with no path the directory `StandardSaveGameBackend` keeps for `ns`
  (`0xbb23c0` app, `0xbb2db0` backend, `0x2471640` directory lookup, the
  backend's vftable checked against `0x38e5758`). All five sites are
  byte-verified; paths are UTF-8 and opened wide.
- **Found by fingerprint.** Multiplayer loads a *copy* of the host's save as
  `mp_shared.sav`; the copy has the same bytes, so when `<save>.terr` is absent
  or foreign the loader takes any `.terr` in that folder whose header carries
  the save's hash (32 bytes read per candidate).
- **Which terrain.** `g_alignmentTerrain`, trusted only when its grid is sane
  and every full tile's cache lies in the terrain pager's arena.
- **Orphans.** Autosaves rotate; after each write a `.terr` whose `.sav` is gone
  is deleted.
- **Cost** (synthetic tiles, 2026-09-21): 0.17 ms and 22.7 KiB per tile, 17.6 %
  of raw. 8,712 tiles: 1.5 s, 190 MiB. 65,536 tiles: 11 s, 1.4 GiB, on every
  save. There is no size limit by default (the owner's call, 2026-09-21: a slow
  save is fine, the load is what matters); `terrain_sidecar_max_tiles=<n>` skips
  the sidecar above n grid records.
  `terrain_sidecar_write=0` reads sidecars without writing them.
- **Test:** `python tools\test_sidecar_io.py` (16 checks, a non-ASCII folder).
- **Autosaves (fixed 2026-09-22).** An autosave's file is not `<id.name>.sav`
  (the engine names and rotates it: `autosave_<game>_<date>.sav`), so the
  expected file never changed and every autosave's sidecar was discarded ("the
  save was not written"), on the host and in the Sandboxie box alike; manual
  saves were fine. Now, when the expected `.sav` did not change, the one `.sav`
  in that folder written during the call takes the sidecar (two or more: none,
  so a sidecar never carries another save's hash). A hot join's shared save is
  such an autosave. `tools/test_sidecar_io.py` section 5.
- **Not sent to joiners, on purpose.** On a 66,248-tile map the `.terr` is
  1.6 GB beside a 755 MB save, and what it can save is part of the alignment
  pass: 8.9-10.4 s of a 185 s world entry (road connections alone took 125 s).
  Sending it would make a join slower even on a fast link, and far slower
  through a Steam relay; the lobby also holds a transfer in memory. A joiner
  loads stock; the sidecar serves the host's own loads.
- **Not yet measured in a game.** What a served load saves is the refine's and
  the pass's *publication* into served tiles; their compute still runs. The
  first real numbers should come from the `terrain sidecar:` log lines and the
  `alignment pass:` timings of a load with and without the file. A save sent to
  another machine (the dedicated server) needs its `.terr` sent with it.

The rest of this section is the groundwork as first recorded.


Built and tested here: the **file format**, the **grid walk** that reads and
restores every tile's height cache, and the **codec** (the variable-length
`BlockCodec` from `small_codec.h`). `tools/test_terrain_sidecar.cpp` builds a
synthetic grid to the game's exact layout and checks a full round trip, plus
every refusal path. Measured on synthetic terrain-like tiles: **17.6 % of raw**,
about 22 KB per 257x257 tile, so a full 207,360-tile map is roughly **1.2 GiB**
on disk.

Not yet integrated (needs the pass, which `src/alignment_batch.h` owns):

1. **Fingerprint.** Key the file to the save by a 64-bit hash of the `.sav`
   bytes, captured in a `SaveGame` (`0x2e97c0`) post-hook and recomputed in a
   `LoadGame` (`0x2e5ec0`) pre-hook. Same bytes => same aligned terrain, so a
   match is exact; any mismatch (edited save, plugin-less save, or a *different
   save of the same map* — same base heightmap, different roads) makes `Apply`
   a no-op and the pass runs. A content hash of the base heightmap alone is
   NOT enough: it would false-accept a different save of the same map and serve
   the wrong cuts, so the key must be tied to the save's identity.

   The save's resolved path is reachable exactly as the multiplayer menu DLL's
   `AutoLoadCall` does it (native/src/menu_hook.cpp ~2219): the argument is a
   `SaveGameId { std::wstring path@0x00, name@0x20, namespace@0x40 }` (SSO);
   `app = APP_ACCESSOR()`, `mgr = *(app + 200)` is the save manager, and
   `SAVEINFO_GET(info, mgr, id)` fills a 0x110-byte save-info struct that
   carries the absolute `.sav` path. `LoadGame` (`0x2e5ec0`) receives that
   SaveGameId directly; `SaveGame` (`0x2e97c0`) has it too. Hash the resolved
   `.sav` file and place/read `<path>.terr` beside it. Store the hash in
   `static uint64_t TerrainSidecar::g_saveFingerprint`, which `Write` and
   `BeginApply` already take as their `fingerprint` argument.
2. **Write trigger.** After a save completes, walk `g_alignmentTerrain`
   (exposed by the batch detour, `CTerrain` from `self+8`) and write the file.
3. **Read trigger and short-circuit.** The pass CREATES the tiles (`AddTile`,
   `0x33cb60`, is called from the alignment system), so the sidecar cannot
   replace it wholesale. Two options, to decide with the pass owner:
   - Let the pass create the tiles but make its per-block compute a no-op when
     a valid sidecar is loaded, then `Apply` the saved caches. Smallest change,
     but it lives inside the pass.
   - Serve the saved cache from the refine detour and skip the rasterise. This
     is what "the sidecar replaces what the refine produces" would mean, but
     the refine yields *base* heights and the pass cuts them afterwards, so it
     only works if the cut is also suppressed for served tiles.

## Two CTerrain versions, one image

A save load holds two CTerrain versions transiently, each with the full tile
grid (measured: a 256x256 save is 65,536 tiles per version, 131,072 AddTile
calls, two alignment passes of ~13.9 s each = 27.8 s). One sidecar image serves
both, and no second grid is stored, because:

- The two versions' tile caches are byte-identical tile for tile. This is the
  same fact the content-dedup measured (every tile has exactly one byte-identical
  twin in the other version); it is why `terrain_dedup` works.
- Both grids share dimensions and origin, so `RecordIndex(x, y)` is the same in
  both, and `ApplyTile(GridOf(versionB), index)` decodes the same blob into
  version B's tile correctly.

All serving happens at AddTile, and every AddTile (both versions) completes
before either alignment pass runs, so `g_load` need only live from the first
AddTile to the last; `EndApply` after the first pass is correct today, after the
last pass is strictly safe. At save time there is normally one CTerrain, so
`WriteForSave` captures the single live version, which is the state both
transient versions converge to on the next load.

## File format

Little-endian. `TERR`, version 1.

```
FileHeader { u32 magic; u32 version; u64 fingerprint; i32 nx, ny; u32 tiles; u64 headerHash; }
repeated:  TileHeader { u32 recordIndex; u32 bytes; }  then `bytes` of BlockCodec blob
```

`headerHash` catches a torn write; each tile's `BlockCodec` blob carries its own
64-bit content hash, so a flipped byte fails decode rather than restoring wrong
terrain. `Apply` refuses the file unless `magic`, `version`, `headerHash`,
`fingerprint` and `nx`/`ny` all match, only writes into a record whose vector is
already `Samples` long (so it never allocates a tile), and returns -1 on a
corrupt-but-matching file so the caller discards it and lets the pass run.

## Grid layout (RE, Steam 35924)

`CTerrain + 0x18` -> grid `{ i32 x0@0, i32 y0@4, i32 nx@8, i32 ny@0xc, void* records@0x10 }`;
each record is 40 bytes `{ i32 entity@0, {u16* first, *last, *end}@8, i32 version@0x20 }`.
Confirmed from `CTerrain::GetTile` (`0x33d580`) and `AddTile` (`0x33cb60`).
