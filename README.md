# TpF2 Multiplayer — Transport Fever 2 multiplayer mod

[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](LICENSE)

**Website: [silver2127.github.io/tpf2-multiplayer](https://silver2127.github.io/tpf2-multiplayer/)** ·
[Download the installer](https://github.com/silver2127/tpf2-multiplayer/releases/latest/download/TpF2Multiplayer.msi) ·
[Privacy policy](https://silver2127.github.io/tpf2-multiplayer/privacy.html)

**Join The Discord: [https://discord.gg/7VhmtUstqQ](https://discord.gg/7VhmtUstqQ)** ·

**Multiplayer for Transport Fever 2** (Steam, Windows, build 35924). Several players build in one
world at the same time: the roads, track, stations, depots, vehicles and lines one player makes
appear for everyone, applied at the same moment of the simulation on every machine. Play one shared
company together, or separate companies with their own money on the same map.

It is unofficial, reverse-engineered without the engine's source, and **experimental**. Sessions of up to
four players have been run, on one PC and between PCs on different networks. Read
[docs/KNOWN_ISSUES.md](docs/KNOWN_ISSUES.md) before relying on it.

This branch also contains a **native Linux build-35924 port**. Its current
integration includes Windows **release 0.7** plus dev `8c3c02a5` and a native
dedicated server. See [Linux installation](docs/linux/INSTALL.md),
[current integration and test evidence](docs/linux/UPSTREAM_dev_8c3c02a5.md), and
[dedicated server setup](tools/server/README.md). Canonical simulation ordering is now on by default (`TPF2MP_ORDER_CANON=0`
disables it); see the [dev ad3d66e4 integration](docs/linux/UPSTREAM_dev_ad3d66e4.md).
Settings must match Windows peers. Loaded-game lifetime and cross-platform
validation remain outstanding; matching versions do not establish gameplay parity.
The [dev `0a35d0a8` integration](docs/linux/UPSTREAM_dev_0a35d0a8.md) enables
native terrain compression by default and fixes bridge lobby identity; release
version remains 0.7.
The [dev `ea35eb8a` integration](docs/linux/UPSTREAM_dev_ea35eb8a.md)
adds native terrain pager recency, automatic memory headroom and fault-rate
logging; loaded-big-map performance validation remains outstanding.
The Windows MSI instructions below apply to the Windows version.
The subsequent [dev `60d237c5` integration](docs/linux/UPSTREAM_dev_60d237c5.md)
retains the Windows autosave-sidecar fix; native terrain sidecars remain unported.

## How it works

The game has no network code, so this adds lockstep multiplayer from outside. A forwarding `alut.dll`
loads a few DLLs into the game at start-up: one captures each command a player issues and cancels it
before it applies, one carries commands between the game and a separate lobby process, and one draws the
Multiplayer panel on the title menu. A Lua game-script mod stamps every command with a future simulation
step, sends it to everyone, and replays it through the game's scripting API on every machine at that step,
including the player's own. Everyone starts from the same save, so the worlds stay identical; a detector
compares them continuously. The lobby handles NAT traversal, encryption and sending the save.
[docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) has the details.

## Install

**Download [`TpF2Multiplayer.msi`](https://github.com/silver2127/tpf2-multiplayer/releases/latest/download/TpF2Multiplayer.msi) (the [latest release](https://github.com/silver2127/tpf2-multiplayer/releases/latest); Linux and Steam Deck: `install_proton.sh` from the same page),
close the game, and run it.** Everyone in a session needs the same version. A new version is installed the same
way, over the old one: there is no in-game updater.

The installer finds the game folder through Steam, keeps the game's `alut.dll` as `alut_real.dll` and puts
the proxy in its place, adds the DLLs, the lobby (the `netpunch\` folder) and the **Transport Fever 2 Multiplayer** mod, and
switches the game to the Windows Segment Heap, which makes very large maps load far faster. Runtime files go
to `%LOCALAPPDATA%\tpf2mp\data\`. It installs alongside
[TpF2 Big Maps](https://github.com/silver2127/tpf2-bigmap) in either order. Details:
[installer/README.md](installer/README.md).

**Linux and Steam Deck (the Windows game under Proton):** download `install_proton.sh` from the same release and run
`sh install_proton.sh` (no Python needed; `install_proton.py` is the Python equivalent); it installs the same files into the Proton game. Details, including the lobby
repair Wine needs: [docs/proton/INSTALL.md](docs/proton/INSTALL.md). The native Linux game has its own
build on the `linux-native` branch.

To uninstall, use **Apps → TpF2 Multiplayer → Uninstall**, or run the MSI again and choose **Remove**; the
game's own `alut.dll` is put back. Steam's "Verify integrity of game files" also restores it, which removes the
Multiplayer entry until you run the MSI's **Repair**.

Every release is built by GitHub Actions from the tagged source
([`.github/workflows/build-msi.yml`](.github/workflows/build-msi.yml)); `SHA256SUMS.txt` on the release page lists the
files it produced. The lobby is a Python program frozen with PyInstaller, and unsigned software of that kind is
sometimes flagged by antivirus heuristics. The checksums and the build log are how to check that what you downloaded
is what the source builds.

## Play

1. Title menu → **Multiplayer** → **HOST GAME**. The code is copied to your clipboard: send it to your friends,
   or tick **PUBLIC** to list the game. A password locks the code.
2. Friends open **Multiplayer**, paste the code and press **JOIN GAME**, or click your game in **PUBLIC GAMES**.
3. Press **START GAME**. Your newest save is sent to everyone; when it is ready, everyone opens **LOAD GAME**
   and picks **mp_shared**.

The host needs UDP port 29471 reachable from the internet (the lobby tries UPnP). If that is not possible, use a
the dedicated server in the PUBLIC GAMES list, where nobody needs an open port. New games have the multiplayer mod enabled
automatically; for an existing save, enable it once in the save's Mods panel. The full guide, including the
in-game window, companies and troubleshooting, is [docs/PLAYING.md](docs/PLAYING.md).

## Privacy

Nothing leaves your PC except the session itself. Your player name, chat, game commands, network address and the
host's save go to the other players in the session, directly or through the dedicated server. The project's server
provides the public games list, which lists your game only while **PUBLIC** is ticked, and carries a joining
player's encrypted address note to the host so the two can connect. There is no telemetry, no usage statistics and
no automatic bug or crash reporting: the in-game updater, the session count and the desync-report upload of earlier
versions were removed in 0.6.1.11. When something goes wrong, the logs stay in `%LOCALAPPDATA%\tpf2mp\logs` and you
send them yourself if you report a bug. The full text is the
[privacy policy](https://silver2127.github.io/tpf2-multiplayer/privacy.html).

## Documentation

| document | covers |
|---|---|
| [docs/PLAYING.md](docs/PLAYING.md) | hosting, joining, relays, the in-game window, speed, companies, troubleshooting |
| [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) | the components, a command's path, time and pacing, sessions, files |
| [docs/REPLICATION.md](docs/REPLICATION.md) | what replicates and how, per action, and how divergence is detected |
| [docs/NETWORKING.md](docs/NETWORKING.md) | lobby protocol, join codes, save transfer, dedicated relay, master server |
| [docs/SECURITY.md](docs/SECURITY.md) | the threat model: what is protected and what is not |
| [docs/CONFIGURATION.md](docs/CONFIGURATION.md) | every setting |
| [docs/DEVELOPMENT.md](docs/DEVELOPMENT.md) | building, deploying, the multi-instance rig, logs, tests, releases |
| [docs/KNOWN_ISSUES.md](docs/KNOWN_ISSUES.md) | open bugs, replication gaps, plans that were not built |
| [docs/TESTING.md](docs/TESTING.md) | the manual test plan: what to check before pushing, and before a release |
| [docs/re/](docs/re/README.md) | the engine reference for build 35924 that the hooks rest on |
| [installer/README.md](installer/README.md) | the MSI: what it changes, upgrades, building it |
| [docs/proton/INSTALL.md](docs/proton/INSTALL.md) | Linux and Steam Deck: installing into the Windows game under Proton |
| [netpunch/README.md](netpunch/README.md) | the lobby's source |

## Repository layout

| path | contents |
|---|---|
| `native/` | the DLLs (`build.bat <target>`); `src/plugin/` is the plugin host and its ABI |
| `native/linux/`, `tools/linux/` | native Linux libraries, tests, Steam Runtime builds and `.run`/tarball packaging |
| `bigmap/` | Big Maps, shipped in the same Windows MSI and native Linux package; Linux feature limits: [port record](bigmap/docs/linux/PORT.md) |
| `mod/mp_lockstep_1/` | the game-script mod |
| `netpunch/` | the lobby (the dedicated server runs it too) and the master server (Python) |
| `installer/` | the WiX package |
| `tools/` | deploy, rig, soak-test and check scripts; `tools/ghidra/` and `tools/re/` for reverse engineering |
| `docs/` | the documentation |

## Contributing and credits

Issues and pull requests are welcome, especially reproductions with logs from every player
([what to collect](docs/PLAYING.md#when-something-goes-wrong)). Please keep the project's conventions
([docs/DEVELOPMENT.md](docs/DEVELOPMENT.md#conventions)): destructive replication channels are validated on
the rig before they merge, and a field identification counts only when a differential capture confirms it.

- Companies mode is inspired by, and reuses engine mechanisms proven in, Swiss's **Multiplayer Companies**
  Workshop mod (item 3710243057): runtime `addPlayer`, `setPlayer`, `bookJournalEntry` to a specific player,
  `setBulldozeable`.
- [TpF2 Big Maps](https://github.com/silver2127/tpf2-bigmap) grew out of this project.
- Licensed under the [MIT License](LICENSE). Third-party material is listed in
  [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).

**Disclaimer.** This project is not affiliated with or endorsed by Urban Games. It replaces a file inside your
Transport Fever 2 installation (`alut.dll`, kept as `alut_real.dll`) and patches game code in memory while the
game runs. Use it at your own risk and keep backups of your saves. Multiplayer saves are ordinary `.sav` files;
the mod adds its company assignment to the save's script state.

The [dev `a42dab6c` integration](docs/linux/UPSTREAM_dev_a42dab6c.md)
keeps hot-join save requests pending while the host world loads, then
takes the save when the game UI is ready. Version remains 0.7.

The [dev `7cacbaaf` integration](docs/linux/UPSTREAM_dev_7cacbaaf.md)
removes full mapping-table scans from family guards on Linux 6.11+ and retains
a faster snapshot fallback for older kernels. Version remains 0.7.

The [dev `2c05099a` integration](docs/linux/UPSTREAM_dev_2c05099a.md) adds the remaining supplied
Windows RNG seed/distribution/engine compatibility modules, enabled by default.
`TPF2MP_SIM_SEED=0` and `TPF2MP_ENGINE_PARITY=0` disable them for diagnosis.
Static ELF checks and 65 native tests pass; the lab launch was blocked before
the game started, so cross-platform gameplay validation remains outstanding.

The [dev `582a380` integration](docs/linux/UPSTREAM_dev_582a380.md) makes native dedicated
restarts prefer a newer autosave of the hosted `mp_shared` world over the
configured save. Version remains 0.7.

The [dev `e63ceefc` integration](docs/linux/UPSTREAM_dev_e63ceefc.md) retains
upstream's dedicated-server performance report; runtime code is unchanged.

The [dev `cf5f8a0e` integration](docs/linux/UPSTREAM_dev_cf5f8a0e.md) makes load-time company
switches wait for entity queries to answer and reuses live saved player entities.
Version remains 0.7; loaded-world validation is still outstanding.

The [dev `b4b629a2` integration](docs/linux/UPSTREAM_dev_b4b629a2.md) prevents
per-frame script state sync from rewinding the town-growth clock. Shared Lua
and native tests pass; live growth validation remains outstanding. Version remains 0.7.

The [dev `522a303b` integration](docs/linux/UPSTREAM_dev_522a303b.md) reports the slowest hash's
lane breakdown and the cost of post-hash broadcast, drift and comparison work.
Shared Lua regression tests pass; no live performance measurement is claimed.
Version remains 0.7.

The [dev `bd69b864` integration](docs/linux/UPSTREAM_dev_bd69b864.md) adds native UCRT math parity, octree depth 12/13,
target-record indexing and the 0.7.0.2 TCP/resync UI. Placement-distance and
attempt-budget parity remain unported; the lab launch was blocked before game startup.
