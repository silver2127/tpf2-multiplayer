## 0.7.0.7 - steadier connections, no freeze on busy maps, dedicated server download

Stable update based on 0.7.0.6, for Windows, Proton and the native Linux game. Everyone in a session, including the dedicated server, must update to 0.7.0.7.

### Changes

- **Connections recover better from lost packets.** The game connection's retransmission now adapts its timeout to each player's measured round trip and acknowledges more packets at once (protocol FPT6, contributed by FreeProject089). A player who goes quiet during a long load, an autosave or a big terrain replay keeps receiving what they missed instead of losing those commands, and a very busy session no longer drops commands from a full send queue.
- **No more freeze every few seconds on maps with many constructions.** The check for edited and removed stations, depots and other constructions ran in one go: on a map with about 400 of them the simulation stopped for half a second every 30 ticks. It is now spread over the cycle, with the same checks.
- **"Could not pause all games" during a resync is fixed** for players holding a key in another program, such as a push-to-talk key while talking about the desync. Only keys the game itself receives count now, and there is no 10-second limit.
- **Less work per update.** The train and depot watchers, which only write to the log, are off unless `watch_trains=1` is set in `tpf2_slice.cfg`, and the desync hash is computed faster with identical results.
- **Big Maps: Fantasia Map Generator maps above 32 x 32 km generate without running out of memory.** Generation reuses its temporary buffers: 40 km maps that failed with an out-of-memory error now generate, and the pass uses up to half of the free memory to generate faster. Fantasia's own files are not changed; `generator_memory=0` in `tpf2_bigmap.cfg` turns it off.
- **Dedicated server download.** Each release now carries `TpF2Multiplayer-Server-Linux.tar.gz`, the scripts for running a dedicated server on Linux or a VPS, and a new guide covers Windows and Linux servers: [HOSTING_A_SERVER.md](https://github.com/silver2127/tpf2-multiplayer/blob/main/docs/HOSTING_A_SERVER.md). Servers set up from the earlier example settings were not listed in PUBLIC GAMES; leave `MASTER_URL` empty in `/etc/tpf2mp/server.env`.

### Update

Use the launcher: **Update & play** (Windows and Linux), or install directly: the MSI on Windows, install_proton.sh under Proton, the native .run on Linux. A Linux dedicated server: `tpf2server stop`, `tpf2server install`, `tpf2server configure`, `tpf2server start`. All participants need the same version.

### Known limitations

- A cargo filter set while creating a new line (before its first save) is not carried; set it on the line once it exists.
- The native Linux terrain pager can still thrash on a running big map.

### Validation

The construction scan's coverage and output against the full scan, every removal case (con_slice_test); hash results byte-identical to the previous loop (hash_stream_test); the PERF lanes and the watcher switch (perf_lanes_test); the resync hold with a key held outside the game (test_native_control); the transport's retransmission and kept packets (net_rto_resilience_test), packet sizes, the version gate, the lobby limits and the Steam tunnel tests; the Fantasia pass at 32 and 40 km (test_generator_memory); the release tooling tests. The Linux libraries are built and tested in the pinned Steam Runtime soldier SDK.
