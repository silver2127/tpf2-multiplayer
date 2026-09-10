# tpf2-multiplayer

[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](LICENSE)

**Multiplayer for Transport Fever 2** (Steam, Windows, build 35924). Several players build in one
world at the same time: the roads, track, stations, depots, vehicles and lines one player makes
appear for everyone, applied at the same moment of the simulation on every machine. Play one shared
company together, or separate companies with their own money on the same map.

It is unofficial, reverse-engineered without the engine's source, and **experimental**. Sessions of up to
four players have been run, on one PC and between PCs on different networks. Read
[docs/KNOWN_ISSUES.md](docs/KNOWN_ISSUES.md) before relying on it.

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

**Download `TpF2Multiplayer.msi` from the [latest release](https://github.com/silver2127/tpf2-multiplayer/releases),
close the game, and run it.** Everyone in a session needs the same version.

The installer finds the game folder through Steam, keeps the game's `alut.dll` as `alut_real.dll` and puts
the proxy in its place, adds the DLLs, the lobby (`netpunch\netpunch.exe`) and the **Transport Fever 2 Multiplayer** mod, and
switches the game to the Windows Segment Heap, which makes very large maps load far faster. Runtime files go
to `%LOCALAPPDATA%\tpf2mp\data\`. It installs alongside
[TpF2 Big Maps](https://github.com/silver2127/tpf2-bigmap) in either order. Details:
[installer/README.md](installer/README.md).

To uninstall, use **Apps → TpF2 Multiplayer → Uninstall**, or run the MSI again and choose **Remove**; the
game's own `alut.dll` is put back. Steam's "Verify integrity of game files" also restores it, which removes the
Multiplayer entry until you run the MSI's **Repair**.

## Play

1. Title menu → **Multiplayer** → **HOST GAME**. The code is copied to your clipboard: send it to your friends,
   or tick **PUBLIC** to list the game. A password locks the code.
2. Friends open **Multiplayer**, paste the code and press **JOIN GAME**, or click your game in **PUBLIC GAMES**.
3. Press **START GAME**. Your newest save is sent to everyone; when it is ready, everyone opens **LOAD GAME**
   and picks **mp_shared**.

The host needs UDP port 29471 reachable from the internet (the lobby tries UPnP). If that is not possible, use a
dedicated relay from the PUBLIC GAMES list, where nobody needs an open port. New games have the multiplayer mod enabled
automatically; for an existing save, enable it once in the save's Mods panel. The full guide, including the
in-game window, companies and troubleshooting, is [docs/PLAYING.md](docs/PLAYING.md).

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
| [netpunch/README.md](netpunch/README.md) | the lobby's source |

## Repository layout

| path | contents |
|---|---|
| `native/` | the DLLs (`build.bat <target>`); `src/plugin/` is the plugin host shared with TpF2 Big Maps |
| `mod/mp_lockstep_1/` | the game-script mod |
| `netpunch/` | the lobby, dedicated relay and master server (Python) |
| `installer/` | the WiX package |
| `tools/` | deploy, rig, soak-test and check scripts; `tools/ghidra/` and `tools/re/` for reverse engineering |
| `docs/` | the documentation |

## Contributing and credits

Issues and pull requests are welcome, especially reproductions with logs from every player
([what to collect](docs/PLAYING.md#when-something-goes-wrong)). Please keep the project's conventions
([docs/DEVELOPMENT.md](docs/DEVELOPMENT.md#conventions)): destructive replication channels ship switched off
until validated, and a field identification counts only when a differential capture confirms it.

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
