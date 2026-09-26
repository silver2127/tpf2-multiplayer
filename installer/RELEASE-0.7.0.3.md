## 0.7.0.3 - desync, bulldoze freeze and stop fixes

Stable update based on 0.7.0.2, with the latest main and dev changes, for Windows and the native Linux game. Everyone in a session, including the dedicated server, must update to 0.7.0.3.

### Changes

- **Bulldozing no longer freezes the game.** A dragged bulldoze arrives as one demolition per road or track piece, and each one used to scan every node of the network: 173 of them on one step froze host and joiner for 12.6 s. One node index is now built per burst and shared, with the same matches as the full scan, so every player still agrees.
- **Signals and stops are no longer shipped twice.** After a stall, the catch-up scan re-sent signals and stops that every player had already built, as new ones. That could put a second signal on a track, replace a street stop, or delete a newer object at the same spot. Replayed stops and signals are now known to the scan.
- **Town growth stays in step on the native Linux server.** The Linux build sorted only 1 of the 28 entity lists that Windows sorts each step, so towns grew differently within minutes of a join. It now sorts all 28. The Linux build also draws the same random numbers as Windows at 15 more places and uses Windows' float math functions in the simulation.
- **Dedicated server.** Towns no longer freeze when a per-frame state sync would rewind a script's clock. A restart resumes the world that was being played, not the configured save. A company switch at load waits until the world answers. Two big sim-thread costs on Linux are gone: reading `/proc/self/maps`, and the person target-set order.
- **Hot join.** A save request that arrives while the host's world is still loading now waits for it, and nobody waits for members who are still loading.
- **Big Maps.** An autosave keeps its Big Maps sidecar. Octree depths 12 and 13 work on the native Linux build as on Windows. On GOG they fall back to depth 11 instead of refusing to load. Terrain compression is on by default on native Linux.
- A town development trace for desync reports (off by default; `towntrace=1` in `tpf2_slice.cfg` on Windows, `TPF2MP_TOWN_TRACE=1` on Linux).

### Update

Windows: close the game and use **Official / Stable** in the existing launcher, then **Update and play**, or run the MSI below. Native Linux: close the game and run the `.run` installer below. All participants need the same version.

### Known limitations

- The native Linux terrain pager can still thrash on a running big map.
- Routers and firewalls can still block TCP save transfers; see the 0.7.0.2 notes.

### Validation

Stop-replay, edge-demolition, TCP connectivity and Steam Messages regression tests. The Linux libraries are built and tested (CTest) in the pinned Steam Runtime soldier SDK. The Linux release check confirms that the bundled Lua matches this Windows release exactly.
