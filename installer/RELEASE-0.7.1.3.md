## 0.7.1.3 - joining a running game: fresh saves, no more drop-outs, and a lobby that stops leaking memory

Update based on 0.7.1.2, for Windows, Proton and the native Linux game. Everyone in a session, including the dedicated server, must update to 0.7.1.3.

### Joining a running game

- **A newcomer gets a fresh save.** A host's save more than two minutes old is no longer sent to a player who joins: the host takes a new one first. On a slow PC every second of a save's age is about a second more catching up.
- **A player missing Workshop mods was sent the whole save again and again** (every ~20 s on the dedicated server) while their mods were packed, and never got in. They now get the save once, then their mods.
- **The terrain file streamed to a joiner is no longer replaced half way** by another player's start. The joiner's game kept reading the half-finished file and computed its terrain itself: minutes at 70% on a big map.
- **A catch-up now logs how fast it closes the gap**, every ~20 s: the joiner's own speed, the session's, and the time to go.

### The lobby

- **A finished transfer stayed in memory with its whole file.** Every save and terrain file the lobby ever sent was kept until it closed: the dedicated server's lobby reached 17 GB and pushed the game into swap. It now frees each one when its transfer ends.
- **A lobby that stalls no longer drops everyone.** On a machine short of memory the lobby stood 19 s reading a save, missed the players' pings meanwhile, and dropped every player as silent. Time the lobby itself could not listen is no longer counted against them.

### Dedicated server

- **Pauses while a player joins an empty server, and runs at 1x at most while someone joins others**, instead of running on at the players' speed while the newcomer loads.
- **The native Linux plugin host accepts short hooks** (5-13 bytes): it refused the terrain sidecar's hook, so the native server wrote no terrain file.

### Speed

- **Below 1x the command delay follows the session speed**, so a click lands sooner when the session runs slowly, and a rise from below 1x is ramped.

### Big maps

- **The M key no longer opens the minimap** by default. `minimap_key=1` in `tpf2_bigmap.cfg` turns it back on.

### Update

Use the launcher: **Update & play** (Windows and Linux), or install directly: the MSI on Windows, install_proton.sh under Proton, the native .run on Linux. A Linux dedicated server: `tpf2server stop`, `tpf2server install`, `tpf2server configure`, `tpf2server start`. All participants need the same version.

### Validation

The lobby changes each have a test that fails on 0.7.1.2: a finished transfer is freed (bulk_tcp self-check), a joiner whose mods take seconds to pack gets the save once (relay_mod_download_test: 3 sends before, 1 now), a second START leaves a running terrain stream alone (test_terrain_stream), a 10-minute-old save is taken again before it is sent (test_fresh_hotjoin_save), and a 19 s stall of the lobby drops nobody (test_lobby_limits). The lobby, transfer, Steam, live-join and dedicated-mode suites pass.
