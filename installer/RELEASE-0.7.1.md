## 0.7.1 - faster dedicated servers, shorter autosaves, Sandbox towns, a Multiplayer button

Feature update based on 0.7.0.7, for Windows, Proton and the native Linux game. Everyone in a session, including the dedicated server, must update to 0.7.1.

### Changes

- **Dedicated servers run at full speed.** A Linux server's simulation had been running at about half speed: every road vehicle entering a road segment cost around 1,000 system calls. It now costs a few, and the engine is back on its normal 200 ms step. The simulation thread went from 82% to 22% of a core on the project's server, with the same results.
- **Shorter autosave and join pauses.** A save's compression now runs on worker threads instead of the simulation thread, on Windows, Proton and native Linux. On the project's server a 1.2 GB world saves in 3 seconds instead of 7-9, so everyone waits less at every autosave and every hot join. The saved file is unchanged in format and loads as before. `save_threads=0` in `tpf2_menu_flags.txt` turns it off.
- **Big maps load faster on Linux.** New terrain was being set up in a way that interrupted every CPU core for each page written; the slowest loading step (at 70%) went from 1-4 minutes to under a minute.
- **Less wasted work while loading and on headless servers.** The game's Steam status thread no longer spins at full speed during a load (Windows and Linux; `steam_poll_ms=0` turns the change off). A dedicated server that draws nothing no longer re-creates its graphics buffers every frame (`dedicated_recycle_sets=0` turns that off).
- **Sandbox mode's town tool works in multiplayer.** A town placed with it is built on every player's game. Before, only the placing player got it, which caused a desync and then a resync that threw the town away.
- **A Multiplayer button in the main toolbar**, next to the minimap's. It shows and hides the Multiplayer window, the same as Ctrl+Shift+D.
- **The New Game map preview no longer regenerates endlessly.** With Big Maps installed (0.7.0.7), the preview threw its map away and started again forever, on every generator and map size.
- **Big Maps minimap:** it has its own toolbar button, **M** opens it (except while typing), and its colours follow the map's climate, so dry maps are no longer green.
- **A host leaving no longer strands the other player at a quarter speed.** In a two-player game where the host left, the remaining player stayed at the slowest speed until they restarted.
- **Large saves no longer fail to transfer after 120 seconds** over the master server's pipe; they used to fall back to the slower path.
- **Fences compatibility can no longer stop the game script**: if its file fails to load, multiplayer turns off with a message, as other modules do.

### Update

Use the launcher: **Update & play** (Windows and Linux), or install directly: the MSI on Windows, install_proton.sh under Proton, the native .run on Linux. A Linux dedicated server: `tpf2server stop`, `tpf2server install`, `tpf2server configure`, `tpf2server start`. All participants need the same version.

### Known limitations

- A cargo filter set while creating a new line (before its first save) is not carried; set it on the line once it exists.

### Validation

Measured on the project's dedicated server (Linux, 8 vCPU, a 37,000-tile Big Maps world): simulation step, per-thread CPU, system calls and load phases before and after each change; a multithreaded save checked with `zstd -t` and a full decompress, then loaded by a joining game and by the server itself. Tests: the page-batched reads and the road sort's batched names against the one-at-a-time path (test_movement_linux, test_train_order_linux); the descriptor recycler against a fake driver (test_descriptor_recycle); the save stream routing against a fake that behaves like multithreaded zstd, and a real round trip in boost's call pattern (test_save_zstd on Linux; save_zstd_win_test and an in-process self-test before patching on Windows); the served generator copy's times (test_generator_memory); the terrain pager's zero fill (test_pager). The Linux libraries are built and tested in the pinned Steam Runtime soldier SDK; the Windows DLLs are built with MSVC.
