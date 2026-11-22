## 0.7.1.2 - companies rewritten, much faster big-map loads, no more big-map stutter

Update based on 0.7.1.1, for Windows, Proton and the native Linux game. Everyone in a session, including the dedicated server, must update to 0.7.1.2.

### Companies (separate companies mode)

The rewrite tested as 0.7.0.8 is now in the release, on every platform:

- **Companies belong to players, not to lobby slots.** A company is tied to its players' Steam IDs (the lobby name for players without one). A save loaded with a different host keeps everyone in their own company, with their own money and vehicles.
- **Every machine decides alike** which company a command belongs to. New companies start with the same loan as the first one.
- **The COMPANIES tab is rebuilt:** your company and its settings on top, the list of companies below, one panel for the selected company.
- **Company colours** from a palette, one per company. Painting vehicles in the company colour is a switch per company, on by default.
- **Delete a company** with a company that takes over everything, or **Nobody**, which sells and removes everything after a warning.
- **One headquarters per company**, and only the owner can change a building.
- **Old saves are migrated:** the host keeps their company, every other player gets back the company of their lobby slot.

### Big maps load much faster

- **The terrain is not computed again on load.** Every save now writes `<save>.terr` beside it: each terrain tile's finished height data. Loading that save restores the tiles from it and skips the terrain alignment pass, which took 44-75 s on a 50,000-tile map. On Windows and the native Linux game, a Linux dedicated server included (new there: `terrain_sidecar=0` in `tpf2_bigmap.cfg` turns it off if it gives trouble).
- **Joiners get the host's terrain file while they load.** It streams to them after START from a host that has one. Each tile is used as soon as it arrives, and the game waits for the rest only when that is faster than computing it. A joiner on a slow connection loads as before.
- The terrain file is written on all cores (it added 12 s to every big-map save) and read on all cores at load.
- The ground-texture index is built faster (up to 2x per tile, identical result).

### Big maps no longer stutter

- **With little free memory, the game gave back 90% of its terrain at once** and rebuilt it in front of the camera: the stutter around big maps. It now gives back only what it needs.
- **A load under memory pressure could crawl** at one terrain tile a second. Fixed.
- The game counts memory as tight only below 3 GB of free commit (it was up to 10 GB), so it uses more of the memory it has. `commit_tight_mb` in `tpf2_bigmap.cfg` sets it by hand.

### Joining a running game

- Vehicles waiting at a stop, and cargo unloading from them, are handled in the same order on every machine. Two trucks standing at one stop could be loaded differently on the host and a player who joined, and the game desynced later.

### Update

Use the launcher: **Update & play** (Windows and Linux), or install directly: the MSI on Windows, install_proton.sh under Proton, the native .run on Linux. A Linux dedicated server: `tpf2server stop`, `tpf2server install`, `tpf2server configure`, `tpf2server start`. All participants need the same version.

### Validation

The terrain file was tested in game on a 36,992-tile save on Windows: both alignment passes skipped, all 73,984 tile versions served, also with the file streamed to a second game. The native Linux code passes 76 tests (including a growing file, a split record and a wait at the pass); its first game use is the project's dedicated server with this release. tools/test_terrain_stream.py tests the transfer on a lossy, reordering connection. The load-memory fix has a test that fails on 0.7.1.1. The ground-texture index matches the game's own code in 156 comparisons. The companies rewrite passed its model, rules and interface tests (0.7.0.8) and is ported to the native Linux game.
