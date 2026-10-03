# Windows dev 2b466e32 integration (0.6.1.20)

Target: `2b466e32720b1748db438a7a0e24d1441b4e1801`.
Linux parent: `c6732eb81362fb624ae191ec115fe65e92fe2303`.
The oldest 20 pending Windows commits are merged, staged and uncommitted.
Later pending commits are outside this integration. Native implementation is
complete for this batch; live gameplay validation was blocked at Steam startup.

## Merged and ported

| Windows commits | Linux treatment |
|---|---|
| `02dc798` | Shared Workshop description length correction |
| `f935703`, `42f3524` | Preserve Wine heap and Proton installer changes; native Linux does not use Wine heaps |
| `1c24dcd` | Retain Linux pending/accepted-load guard; add 30-minute accepted-load expiry, log retry, clear on world frame |
| `3a89e8d` | Clear same-PID stale Lua recovery record before starting native controller; retain another PID's record |
| `2dd8c67` | Shared holding/checking timeout changes |
| `45a289b` | Native OpenGL panel via guarded SDL swap call; shared GL blitter with Linux resolver and native input scaling |
| `f898165`, `9cf0219` | Shared AutoSig planning, replacement and removal; preserve capture call in inject conflict |
| `68f6dc4` | Shared engine-given purchase-name replication |
| `c0dde37` | Native verified SetUserStopped factory capture/cancel, VSTOP absolute state and shared replay |
| `8188451`, `4cd3a41` | Existing Linux empty Connection constructor already provides correct non-null 16-byte allocation; rechecked ELF |
| `7bee56b`, `a30d99c`, `8597a6a` | Release/history/docs merge through 0.6.1.20 |
| `01e7ee0` | Steam IDs/profile URLs and CROSS-PLAY in native panel, startup argument and live command; repeated code events update clipboard/model without starting a snapshot |
| `22ec51f` | UGC flat API resolution and command handling merged into POSIX tunnel; shared subscription/registration/fallback workflow |
| `c0c4dc7` | Shared 32 KB Steam chunks and bounded 4 MB window; existing native reliable-send selection retained |
| `2b466e3` | Shared host-capacity cap release logic |

Conflicts: inject retains native origin-replay additions and AutoSig capture;
steam_tunnel retains Linux callbacks/sockets/lifecycle while adding UGC;
lobby retains Linux SIGTERM, parent monitoring and cleanup around the new
Steam/classic-code selection. Windows code paths are preserved.

Lua verifier/build provenance now target 2b466e32: 29 Lua files, one pinned
Linux inject merge, 32 exact HUD textures. Manifest:
`7cbd4d1c83ec704d7995c4d8cb73599f0e38bda211dcb33a7057f53edac59cdc`.

## Not ported

No implementation in these 20 commits is left unported. Wine heap tuning is
Windows/Proton-specific and remains there. This does not close historical
validation gaps or include any of the next 14 pending commits.

## Tests and live limitations

See [RE and live attempt](../re/linux/DEV_2B466E3.md). Required soldier build and
53 CTests pass, including VSTOP capture, Steam UGC, code/profile normalization,
repeated code events and stale native recovery records. Lua release verification
and the new actual-ELF verifier pass. A real hidden SDL/OpenGL context passes the
upstream pixel/state tests. Python tests pass for Steam tunnel/code/chunks,
Workshop fallbacks, sync operations, Linux lobby shutdown, vehicle names,
stop/go replay, vehicle line selection and host pacing.

AutoSig integration tests require the actual AutoSig2 Workshop installation;
it was absent, so those two tests could not run. Windows executable/MSVC tests
were not run on Linux. Neither is counted as passed.

Both lab launch attempts and all actor restoration details are recorded in the
RE document. No new live game rendering, purchase, toggle, New Line button,
Workshop download, native/Proton session or 30-minute-load observation is claimed.
