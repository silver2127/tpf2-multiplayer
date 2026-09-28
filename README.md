# TpF2 Multiplayer — Transport Fever 2 multiplayer mod

The [dev `262353d7` integration](docs/linux/UPSTREAM_dev_262353d7.md) merges
housekeeping and correctness fixes for replay, vehicle/line identity, lobby transfers,
and network input handling. Native bridge epoch handling and kill-switch readers
are covered by Linux regressions; release remains **0.7.1.3**.

The [dev `528294b1` integration](docs/linux/UPSTREAM_dev_528294b1.md) asks for a
fresh host save when a late joiner would receive one over two minutes old,
with fallback if no new save arrives. Host-loop stalls no longer count as
peer silence. Shared lobby; release remains 0.7.1.2.

The [dev `304a4e28` integration](docs/linux/UPSTREAM_dev_304a4e28.md) adds catch-up progress
logging about every 20 seconds: remaining gap, closing rate, local and session
rates, and an ETA when the gap is closing. Shared Lua; release remains 0.7.1.2.

The [dev `86f806df` integration](docs/linux/UPSTREAM_dev_86f806df.md) keeps
terrain streams running across later STARTs, avoiding replacement while a
joiner reads the same sidecar. Shared Linux/Windows lobby; release 0.7.1.2.

The [dev `effa7243` integration](docs/linux/UPSTREAM_dev_effa7243.md) prevents
repeat save transfers while a joining player’s mods are packing, queued or
being delivered. The shared lobby fix applies to native Linux and Windows;
release remains 0.7.1.2.

Current release integration: **0.7.1.3**, Windows dev `a813ea9f`; see the
[integration and validation record](docs/linux/UPSTREAM_dev_a813ea9f.md). All peers, including dedicated
servers, must update. Existing native Sandbox town-tool capture, minimap and
cargo-filter limitations remain. Upstream performance measurements were not
repeated locally; matching release numbers do not establish gameplay parity.
Completed TCP transfers no longer stay in lobby memory through listener
registrations; active transfers retain their handlers until they finish.
Catching-up peers retain recently heard leader clocks when stamping commands
and defer spare-line requests until catch-up completes.
Dedicated servers pause while players join an otherwise empty server, and cap
the voted speed at 1x while someone joins existing players. A joiner stops
holding the session after about 20 minutes without a change in joining count.
Below 1x, command delay follows session speed with a ramp margin; recovery
from a slow session rises in steps, and excess delay falls faster.
Native terrain sidecars are on by default from dev `71549cff` in 0.7.1.2;
`terrain_sidecar=0` disables them. Live ownership and load-completion checks
remain outstanding; material-index acceleration remains unported. Enabling
sidecars does not establish new native performance results.

[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](LICENSE)

**Website: [silver2127.github.io/tpf2-multiplayer](https://silver2127.github.io/tpf2-multiplayer/)** ·
[Download the launcher (Windows)](https://github.com/silver2127/tpf2-multiplayer/releases/latest/download/TpF2Multiplayer-Launcher-Windows-Setup.exe) · [Linux](https://github.com/silver2127/tpf2-multiplayer/releases/latest/download/TpF2Multiplayer-Launcher-Linux.AppImage) ·
[Privacy policy](https://silver2127.github.io/tpf2-multiplayer/privacy.html)

**Join The Discord: [https://discord.gg/7VhmtUstqQ](https://discord.gg/7VhmtUstqQ)** ·

**Multiplayer for Transport Fever 2** (Steam, Windows, build 35924). Several players build in one
world at the same time: the roads, track, stations, depots, vehicles and lines one player makes
appear for everyone, applied at the same moment of the simulation on every machine. Play one shared
company together, or separate companies with their own money on the same map.

It is unofficial, reverse-engineered without the engine's source, and **experimental**. Sessions of up to
four players have been run, on one PC and between PCs on different networks. Read
[docs/KNOWN_ISSUES.md](docs/KNOWN_ISSUES.md) before relying on it.

This branch also contains a **native Linux build-35924 port**, with a native dedicated server. Its
current integration includes Windows **release 0.7** plus dev `8c3c02a5`:
[Linux installation](docs/linux/INSTALL.md) ·
[current integration and test evidence](docs/linux/UPSTREAM_dev_8c3c02a5.md) ·
[dedicated server setup](docs/HOSTING_A_SERVER.md). The Windows MSI instructions below apply to the
Windows version.

- Settings must match Windows peers. Loaded-game lifetime and cross-platform validation remain
  outstanding; matching versions do not establish gameplay parity.
- Canonical simulation ordering is on by default (`TPF2MP_ORDER_CANON=0` disables it); see the
  [dev `ad3d66e4` integration](docs/linux/UPSTREAM_dev_ad3d66e4.md).
- Native terrain compression is on by default, and bridge lobby identity is fixed; the release
  version remains 0.7 ([dev `0a35d0a8`](docs/linux/UPSTREAM_dev_0a35d0a8.md)).
- The native terrain pager has recency, automatic memory headroom and fault-rate logging;
  loaded-big-map performance validation remains outstanding
  ([dev `ea35eb8a`](docs/linux/UPSTREAM_dev_ea35eb8a.md)).
- The Windows autosave-sidecar fix is retained
  ([dev `60d237c5`](docs/linux/UPSTREAM_dev_60d237c5.md)); native terrain sidecars are available
  experimentally in [dev `2b4fd093`](docs/linux/UPSTREAM_dev_2b4fd093.md), then enabled by default
  in [dev `71549cff`](docs/linux/UPSTREAM_dev_71549cff.md). Live lifetime validation remains outstanding.

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

**Download the launcher from the [latest release](https://github.com/silver2127/tpf2-multiplayer/releases/latest):
[`TpF2Multiplayer-Launcher-Windows-Setup.exe`](https://github.com/silver2127/tpf2-multiplayer/releases/latest/download/TpF2Multiplayer-Launcher-Windows-Setup.exe) or, on Linux and Steam Deck,
[`TpF2Multiplayer-Launcher-Linux.AppImage`](https://github.com/silver2127/tpf2-multiplayer/releases/latest/download/TpF2Multiplayer-Launcher-Linux.AppImage). Run it and press Update & play.**
It installs the mod, keeps it up to date and starts the game. Everyone in a session needs the same version.

The launcher downloads the install files from [tpf2-multiplayer-packages](https://github.com/silver2127/tpf2-multiplayer-packages/releases)
(the release with the same tag); to install by hand, take `TpF2Multiplayer.msi` from there, close the game and
run it. For versions using the two-launcher layout, the Latest page `v<version>` carries the two launchers;
the mod repository also carries install files on `<version>` (without `v`). Launchers up to 1.2.0
must be updated before installing these versions.

The installer finds the game folder through Steam, keeps the game's `alut.dll` as `alut_real.dll` and puts
the proxy in its place, adds the DLLs, the lobby (the `netpunch\` folder) and the **Transport Fever 2 Multiplayer** mod, and
switches the game to the Windows Segment Heap, which makes very large maps load far faster. Runtime files go
to `%LOCALAPPDATA%\tpf2mp\data\`. It installs alongside
[TpF2 Big Maps](https://github.com/silver2127/tpf2-bigmap) in either order. Details:
[installer/README.md](installer/README.md).

**Linux and Steam Deck (the Windows game under Proton), by hand:** download `install_proton.sh` from the [packages release](https://github.com/silver2127/tpf2-multiplayer-packages/releases/latest) and run
`sh install_proton.sh` (no Python needed; `install_proton.py` is the Python equivalent); it installs the same files into the Proton game. Details, including the lobby
repair Wine needs: [docs/proton/INSTALL.md](docs/proton/INSTALL.md). The native Linux game has its own
build on the `linux-native` branch.

To uninstall, use **Apps → TpF2 Multiplayer → Uninstall**, or run the MSI again and choose **Remove**; the
game's own `alut.dll` is put back. Steam's "Verify integrity of game files" also restores it, which removes the
Multiplayer entry until you run the MSI's **Repair**.

Every release is built by GitHub Actions from the tagged source
([`.github/workflows/build-msi.yml`](.github/workflows/build-msi.yml)); `SHA256SUMS.txt` in the matching packages release (also on the mod repository's `<version>` update-files release) lists the
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
| [docs/HOSTING_A_SERVER.md](docs/HOSTING_A_SERVER.md) | running a dedicated server: on a Windows PC, or on a Linux server or VPS |
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

The [dev `aaae03f8` integration](docs/linux/UPSTREAM_dev_aaae03f8.md) retains
the Windows GOG octree fallback and batch deployment fixes. Native Steam
depths 11/12/13 keep their existing verified patches and ceiling checks.

The [dev `7469fce7` integration](docs/linux/UPSTREAM_dev_7469fce7.md) incorporates
Big Maps Windows GOG documentation; native runtime behavior is unchanged.

The [dev `63a3b8df` integration](docs/linux/UPSTREAM_dev_63a3b8df.md) records
upstream depth-13 gameplay and native dedicated multiplayer results. Native
defaults and placement limits remain unchanged; no local live run was repeated.

The [dev `4616c16a` integration](docs/linux/UPSTREAM_dev_4616c16a.md) adds
upstream's Big Maps performance catalog with native feature/default scope.
Runtime behavior is unchanged; the listed gains are upstream measurements.

The [dev `c9ac009d` integration](docs/linux/UPSTREAM_dev_c9ac009d.md) retains
the standalone Big Maps sync and unified Windows MSI documentation while
preserving native Linux guidance. Runtime behavior is unchanged.

The [dev `0610033` integration](docs/linux/UPSTREAM_dev_0610033.md) fixes native
family ordering to recognize all 28 node lists and adds the opt-in town trace.
Static and fixture checks pass; live validation was blocked at lab startup.

The [dev `e43d01dd` integration](docs/linux/UPSTREAM_dev_e43d01dd.md) carries
upstream's per-tick EDEMO node index and the stop-replay registration that stops
a catch-up scan re-shipping replayed signals and stops. Both are shared Lua; the
native Linux slice already emits the EDEMO and STOPX/STOPXDEL records they rely
on, so no native change was needed. No local live run was possible.

The [dev `d8a3ce57` integration](docs/linux/UPSTREAM_dev_d8a3ce57.md) is
upstream's **release 0.7.0.3** and carries no code. It stamps the native release
0.7.0.3 (`installer/VERSION`, which `tools/linux/build_release.sh` reads for the
`.run` installer, and the shared `LOBBY_VERSION` handshake), advances the Lua
verifier and the release provenance line to this target, and stages the two
missing integration records. Each Linux claim the notes make -- all 28 sorted
node lists, the extra RNG sites, Windows float math, `TPF2MP_TOWN_TRACE=1`,
octree depths 12/13, terrain compression on by default -- was rechecked against
the unmodified game ELF and passes. No local live run was possible.

The [dev `122a0ce9` integration](docs/linux/UPSTREAM_dev_122a0ce9.md) adds the native resync
view’s in-game x. Closing a running resync hides its view while recovery
continues; Manage Lobby reopens it. Errors and unanswered Ready requests
bring it back automatically. Saving/loading suppresses the x.

The [dev `45183ac6` integration](docs/linux/UPSTREAM_dev_45183ac6.md) adds
native OPEN LOGS version/ELF identities, state/config snapshots and masked
lobby streams, and retains five archives per kind. Version remains 0.7.0.3.

The [dev `96795a8b` integration](docs/linux/UPSTREAM_dev_96795a8b.md) expands the native public
browser to eight games per page and up to 32 games, and completes archive
runtime/boot metadata and standalone collector credential masking.

The [dev `01044521` integration](docs/linux/UPSTREAM_dev_01044521.md) expands the native public
browser to twelve games per page and up to 48 games, and removes the legacy
panel renderer. Closing a running resync leaves the game visible.

The [release 0.7.0.4 integration](docs/linux/UPSTREAM_dev_e86d5552.md) advances the native package
and shared lobby handshake to **0.7.0.4**. All peers, including dedicated
servers, must update. This release stamps the previously integrated hot-join,
menu and log fixes; existing native feature and live-validation limits remain.

The [dev `c74a7b4e` integration](docs/linux/UPSTREAM_dev_c74a7b4e.md) fixes
TCP save routing after joining through the master's UDP relay: the joiner tries
the host's advertised addresses, or continues on UDP when none are available.
The shared lobby implements this on Linux and Windows; version remains 0.7.0.4.

The [dev `f6e47ef9` integration](docs/linux/UPSTREAM_dev_f6e47ef9.md) adds
the master's TCP pipe fallback for slow save/mod transfers, shared by Linux
and Windows. Native shutdown cleanup is preserved; version remains 0.7.0.4.

The [release 0.7.0.5 integration](docs/linux/UPSTREAM_dev_f67726f8.md) advances
the native package and shared lobby handshake to **0.7.0.5**, stamping the
previously integrated relay address fix and TCP pipe fallback. All peers,
including dedicated servers, must update. Existing native feature and
live-validation limits remain unchanged.

The [dev `ba1fa26e` integration](docs/linux/UPSTREAM_dev_ba1fa26e.md) refuses
native hosting and joining unless `tpf2_bridge_mp.so` is loaded. The menu
shows an amber explanation and directs players to the loader log.

The [dev `4e1e486c` integration](docs/linux/UPSTREAM_dev_4e1e486c.md) moves install
assets to the packages repository while the mod release carries the launchers.
Native builds now emit the publisher's `-native` assets and checksums; see
[manual native installation](docs/linux/INSTALL.md). Version remains 0.7.0.5.

The [dev `e12ed657` integration](docs/linux/UPSTREAM_dev_e12ed657.md) adds
separate `launcher-v<version>` releases without moving the Latest mod release.
The shared publisher supports Linux AppImages; native version remains 0.7.0.5.

The [dev `2f65bae3` integration](docs/linux/UPSTREAM_dev_2f65bae3.md) adds compressed terrain edits
with checksum validation to native capture and replay. Every peer needs this
build; the unchanged 0.7.0.5 version handshake does not detect older terrain
readers. Uncompressed version-1 edits remain readable.

The [dev `616191b1` integration](docs/linux/UPSTREAM_dev_616191b1.md) makes native OPEN LOGS
include the newest startup archive’s game log and `crash_*` files as
`previous_run_*`, so a restart does not hide the crashed run’s dumps.
Copies remain subject to the archive budget. Version remains 0.7.0.5.

The [dev `363c38cc` integration](docs/linux/UPSTREAM_dev_363c38cc.md) retains the Windows
terrain-sidecar concurrent-release fix. Native sidecar capture/serving remains
unported; native pager and alignment batching behavior is unchanged.

The [dev `8978635d` integration](docs/linux/UPSTREAM_dev_8978635d.md) keeps
launchers on the Latest `<version>` page and install files on `v<version>`
and the packages repository. Native release version remains 0.7.0.5.

The [dev `e2957841` integration](docs/linux/UPSTREAM_dev_e2957841.md) adds
`page v<version>` to migrate an already published release to this layout,
including 0.7.0.5, while preserving its install-file assets.

The [dev `9abb2af1` integration](docs/linux/UPSTREAM_dev_9abb2af1.md) adds
annotated launcher-page tags and explicit `--replace-page`, and removes an
earlier migration note when recreating a page. Native version remains 0.7.0.5.

The [dev `8e0a0c00` integration](docs/linux/UPSTREAM_dev_8e0a0c00.md) supersedes
those earlier release layouts: launchers now stay on `v<version>` (Latest),
with update files on `<version>` and the packages repository's `v<version>`.
Launcher-only releases no longer recreate a mod release. Version remains 0.7.0.5.

The [dev `ceee11b1` integration](docs/linux/UPSTREAM_dev_ceee11b1.md) wraps
native lobby chat to its measured width and retains the newest messages that
fit. Shared in-game chat now wraps at 52 bytes. Offline rendering tests pass;
the lab launch was blocked before game startup.

The [dev `0047c19f` integration](docs/linux/UPSTREAM_dev_0047c19f.md) adds shared
cargo-filter replay records. Native filter capture remains **unported**:
Linux line edits can still lose filters. The lab could not start for the
required property/ABI probe; this integration is partial.

The [dev `06188ea5` integration](docs/linux/UPSTREAM_dev_06188ea5.md) keeps
command stamps ahead of the fastest peer while a joiner catches up, with a
600-unit sanity cutoff. Shared Lua tests cover the change; no live multiplayer
result is claimed. Version remains 0.7.0.5.

The [dev `f9d34252` integration](docs/linux/UPSTREAM_dev_f9d34252.md) retains Windows’
corrected packed cargo-flag reader and tests shared numeric flag transport.
Native Linux cargo capture remains unported: static layout evidence was
rechecked, but the lab failed before startup. Linux line edits can still
lose stop filters. Version remains 0.7.0.5.

The [dev `bef70213` integration](docs/linux/UPSTREAM_dev_bef70213.md) makes native OPEN LOGS
include the newest startup archive’s mod `*.log` files as `previous_run_*`,
so the crashed run’s host, terrain and bridge diagnostics accompany its dumps.
Version remains 0.7.0.5.

The [dev `ad36a976` integration](docs/linux/UPSTREAM_dev_ad36a976.md) replaces Lua cargo-filter
setters with a native replay request. The Windows writer is retained; the
Linux writer remains unported after static RE and a lab startup failure.
Native cargo capture and replay can still lose stop filters. This supersedes
the earlier `4ccdde5d` mock-based replay claim. Version remains 0.7.0.5.

The [release 0.7.0.6 integration](docs/linux/UPSTREAM_dev_4e857780.md) advances the native package
and shared lobby handshake to **0.7.0.6**. All peers, including dedicated
servers, must update. This commit only stamps earlier changes: native cargo
filter capture/replay and terrain-sidecar capture/serving remain unported.
The upstream cargo-filter and repeated-load validation does not establish
native Linux support; existing gameplay-validation limits still apply.

The [dev `4617fb6f` integration](docs/linux/UPSTREAM_dev_4617fb6f.md) advances
native Linux to **0.7.0.7 / FPT6**, retains commands owed to quiet members,
adds adaptive retransmission and pacing fixes, and displays the version in
the native panel. All peers must update. Direct installers now accompany
the launchers on the version page. Existing native feature limits remain.

The [dev `6584fd03` integration](docs/linux/UPSTREAM_dev_6584fd03.md) moves
Fantasia terrain-buffer reuse into the native Big Maps plugin, enabled by
`generator_memory=1`. No additional mod is needed. Offline checks cover all
three climates; native rendered output and peak memory remain unmeasured.
Version remains 0.7.0.7.

The [dev `46ca48ef` integration](docs/linux/UPSTREAM_dev_46ca48ef.md) adds
Fantasia generator size/layer/buffer diagnostics and optimizer refusal reasons
to both native Linux and Windows. Version remains 0.7.0.7.

The [dev `15ba4df5` integration](docs/linux/UPSTREAM_dev_15ba4df5.md) fixes
Fantasia buffer reuse to measure generator dimensions in heightmap samples.
128 x 128 tiles remain unchanged; larger sample areas use the optimizer.
Native build and symbolic pipeline tests pass; live memory savings remain
unmeasured. Version remains 0.7.0.7.

The [dev `5d73f324` integration](docs/linux/UPSTREAM_dev_5d73f324.md) adds
Fantasia generator memory budgeting on native Linux, allowing extra buffers
for parallelism. Release remains 0.7.0.7; native generation timing and peak
memory validation remain outstanding.

The [dev `cc0981bb` integration](docs/linux/UPSTREAM_dev_cc0981bb.md) adds the dedicated-server
archive and hosting guide, spreads shared construction checks across updates,
and ports the resync hold fix to Linux: no ten-second deadline, game-local
SDL gesture diagnostics, and focus-loss cleanup. Version remains 0.7.0.7.
Offline regression tests cover these changes; no new live gameplay result is claimed.

The [dev `3cc80874` integration](docs/linux/UPSTREAM_dev_3cc80874.md) records upstream merging
the completed Linux ports through `cc0981bb` back into Windows history. Its
source tree is identical to the previous Linux integration; runtime behavior
and version 0.7.0.7 are unchanged. Lua now matches upstream without exceptions.

The [dev `d9196011` integration](docs/linux/UPSTREAM_dev_d9196011.md) preserves
the release tag when publishing and checks the returned tag afterward, failing
loudly on a mismatch. This shared tooling change leaves native runtime behavior
and version 0.7.0.7 unchanged. Publication checks are tested offline.

The [dev `24b8f636` integration](docs/linux/UPSTREAM_dev_24b8f636.md) guards shared Fences compatibility
loading so a missing or failing module disables multiplayer without aborting
game loading. Offline Lua and native tests pass; no live gameplay result is
claimed. Version remains 0.7.0.7.

The [dev `ea15a156` integration](docs/linux/UPSTREAM_dev_ea15a156.md) batches native guarded page
reads and avoids scanning whole-world vectors for a single road edge. Name
slot pairs are copied in one guarded read. Version remains 0.7.0.7; local
validation uses memory fixtures and ELF checks, with no in-game timing claim.

The [dev `491716c3` integration](docs/linux/UPSTREAM_dev_491716c3.md) retains
upstream's dedicated-server measurements for the road-entry guarded-read fix.
Runtime behavior and version 0.7.0.7 are unchanged; no local performance
measurement is claimed.

The [dev `84058da7` integration](docs/linux/UPSTREAM_dev_84058da7.md) adds native
dedicated descriptor recycling and corrects its verified Vulkan dispatcher
search boundary. Live activation remains unverified because lab startup failed.

The [dev `52a1630a` integration](docs/linux/UPSTREAM_dev_52a1630a.md) validates
native dedicated descriptor recycling's reset slot at dispatcher +0xb28 and
names failed lookups while retaining the Linux byte guard. Lab startup was
blocked before game execution; runtime activation remains unvalidated.

The [dev `4d03caf7` integration](docs/linux/UPSTREAM_dev_4d03caf7.md) retains
upstream's Sandbox tools and TownInfo research. This documentation-only change
adds no town replication; its Windows measurements do not establish native or
cross-platform town-creation determinism. Version remains 0.7.0.7.

The [dev `02edb897` integration](docs/linux/UPSTREAM_dev_02edb897.md) retains
upstream's dedicated descriptor-recycler measurements. Runtime behavior and
version 0.7.0.7 are unchanged; no local performance measurement is claimed.

The [dev `f0212c87` integration](docs/linux/UPSTREAM_dev_f0212c87.md) adds the
shared multiplayer toolbar, pipe-idle and leader-loss fixes, and imports native
road-read batching, threaded save compression, terrain page initialization and
Steam poll throttling. Native town-tool capture and minimap extensions remain
unported after static RE and blocked lab startup. Release remains 0.7.0.7.

The [dev `ac3b4be3` integration](docs/linux/UPSTREAM_dev_ac3b4be3.md) records
upstream merging the completed Linux ports through `412aeb8e` back into Windows
history. Its source tree matches the preceding Linux integration; runtime
behavior and release **0.7.1.1** are unchanged.

The [dev `bde31323` integration](docs/linux/UPSTREAM_dev_bde31323.md) brings the
company registry rewrite, rebuilt COMPANIES tab and native free-color tints.
Release remains 0.7.1.1. Native and shared regression tests pass; the lab launch
was blocked before game startup, so live gameplay validation remains outstanding.

The [dev `0871bfa6` integration](docs/linux/UPSTREAM_dev_0871bfa6.md) retains Windows' complete-sidecar
alignment bypass. Native Linux sidecar serving and this bypass remain unported;
Linux continues its existing alignment path. Static publication metadata was
verified, but the lab failed before startup, preventing live lifetime proof.

The [dev `c8dd5157` integration](docs/linux/UPSTREAM_dev_c8dd5157.md) adds Linux offline coverage for
threaded terrain-sidecar encoding and POSIX file handling. Native game-side
capture/serving remains unported; `terrain_sidecar_threads` has no native
runtime effect. No native save-time improvement is claimed.

The [dev `fdfb79e8` integration](docs/linux/UPSTREAM_dev_fdfb79e8.md) retains
Windows range tracking for multiple terrain versions. Native sidecar serving
and alignment bypass remain unported after fresh static analysis and a lab
startup failure; native alignment behavior is unchanged.

The [dev `5b817efb` integration](docs/linux/UPSTREAM_dev_5b817efb.md) keeps a terrain sidecar
while another served terrain version awaits its pass. Each version skips only
once; a pass that runs releases the file. Native sidecars remain experimental
and default off. Live validation was blocked before game startup.

The [dev `9602a389` integration](docs/linux/UPSTREAM_dev_9602a389.md) adds
experimental native terrain streaming during joiner loads and guarded save
terrain selection. Sidecars remain disabled by default pending live lifetime
and ownership validation; the lab launch failed before game startup.

The [dev `40e12f76` integration](docs/linux/UPSTREAM_dev_40e12f76.md)
adds `terrain_sidecar_read_local=0` for stream-only testing when both peers can
see the host's save folder. The default is 1; native sidecars remain experimental
and require `terrain_sidecar=1`. With local reads and `terrain_stream` both off,
the normal terrain computation runs. This switch does not disable sidecar writes.

The [dev `b6d73041` integration](docs/linux/UPSTREAM_dev_b6d73041.md) retains Windows’
4 GiB maximum automatic free-commit threshold and `commit_tight_mb` override.
Native terrain paging continues to use `MemAvailable`; this Windows setting
has no native effect. Release remains 0.7.1.1.

The [dev `5fd49a24` integration](docs/linux/UPSTREAM_dev_5fd49a24.md) gives
native terrain-sidecar lookups a cursor per thread, reset for each load and
grid. Sidecars remain experimental and default off; no live speedup is claimed.

The [dev `26564158` integration](docs/linux/UPSTREAM_dev_26564158.md) searches
outward from each native terrain-sidecar worker's last hit and reports probes
per lookup at load completion. Sidecars remain experimental and default off;
fixture probe counts do not establish a live load-time improvement.

The [dev `7e3d3bfa` integration](docs/linux/UPSTREAM_dev_7e3d3bfa.md) retains the Windows material-index
optimization and DLL-map profiler support. Native material-index acceleration
remains unported: the Linux selection loop is inlined, and the lab failed
before startup, preventing buffer-lifetime proof. Release remains 0.7.1.1.

The [dev `8066c58f` integration](docs/linux/UPSTREAM_dev_8066c58f.md) retains the default-off Windows
material-index measurement probe. Native `material_index_probe` remains
unported after static investigation and a lab startup failure; Linux produces
no `material_probe.txt`. Windows measurements do not establish native tile
hashes or compression sizes. Release remains 0.7.1.1.

The [dev `65302e5d` integration](docs/linux/UPSTREAM_dev_65302e5d.md)
adds native parallel sidecar decoding at the alignment pass and retains the
file for both terrain versions. Sidecars remain experimental and off by default;
fixture tests pass, but the lab could not start for live validation.

The [dev `a2e47f2c` integration](docs/linux/UPSTREAM_dev_a2e47f2c.md)
retains Windows profiler follow mode and ETW stack/wait readers as developer
tools. Native runtime behavior and release 0.7.1.1 are unchanged.

The [dev `7bace802` integration](docs/linux/UPSTREAM_dev_7bace802.md) retains Windows load-speed
findings, including the rejected material-index chunk-size experiment. These
are upstream measurements; native runtime behavior and release 0.7.1.1 are unchanged.

The [dev `0eb9eea2` integration](docs/linux/UPSTREAM_dev_0eb9eea2.md) lowers Windows’ automatic
free-commit threshold to 2..3 GiB (unknown RAM: 3 GiB). Native terrain paging
retains its existing `MemAvailable` policy; `commit_tight_mb` has no native
effect. Release remains 0.7.1.1.

The [dev `a896a1cb` integration](docs/linux/UPSTREAM_dev_a896a1cb.md) fixes the native plugin host’s
rejection of 5–13-byte hooks, including the terrain sidecar’s 13-byte AddTile
hook. Release remains 0.7.1.2; local lab startup was blocked before the game ran.
