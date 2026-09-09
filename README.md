# tpf2-multiplayer

[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](LICENSE)

**Real multiplayer for Transport Fever 2** (Steam, Windows, build 35924). Unofficial,
reverse-engineered, and written without access to the engine source. Two or more players build in
one world at the same time: roads, rails, stations, depots, vehicles and lines made by one
player appear on the other's map, executed at the same in-game moment. Two modes share
the same mod: **co-op**, where both play one shared company, and **companies**, where each
player owns a separate company and wallet on the same map. Alpha: verified between two
machines on different networks; the lobby seats eight, the sync itself has only run
between two.

## Installing

**Players:** download `TpF2Multiplayer.msi` from the
[latest release](https://github.com/silver2127/tpf2-multiplayer/releases), close the
game, run it. It finds the game folder from Steam's registry entry, installs the
proxy `alut.dll` (the original is kept as `alut_real.dll`), the lockstep DLLs and
their cfgs, the `mp_lockstep_1` mod into `<game>\mods`, and the frozen lobby into
`<game>\netpunch`. It also sets the Segment Heap switch for `TransportFever2.exe` (a
registry value; big saves load about 15x faster, see `installer/README.md`). Runtime
files go to `%LOCALAPPDATA%\tpf2mp\data`. Installs alongside
[TpF2 Big Maps](https://github.com/silver2127/tpf2-bigmap) in either order.

**Uninstalling:** run the same MSI again and choose **Remove**, or use *Apps* in
Windows settings. Either removes every file it added and puts the game's own
`alut.dll` back (unless TpF2 Big Maps is still installed, in which case the shared
proxy stays for it).

**Prerequisites**

- Windows, Steam, Transport Fever 2 (build 35924; the hook RVAs are specific to it).
- Visual Studio 2022 Build Tools with the MSVC x64 toolchain (`cl`, `link`, `ml64`). The
  build `.bat` files and the scripts in `tools/` (except `install_portable.ps1`) hardcode
  the default locations, Build Tools at `C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools`
  (each `.bat` calls its `vcvars64.bat`) and Steam at `C:\Program Files (x86)\Steam`; with
  another VS edition or Steam library, edit the path variables at the top of each script.
- For the lobby: a frozen `netpunch\dist\netpunch.exe` (not checked in; build it with
  `cd netpunch; python -m PyInstaller --onefile --name netpunch lobby.py`), or Python 3.12
  on `PATH` with `pip install -r netpunch\requirements.txt` (`pystun3`, `miniupnpc`).
- Optional: `pip install luaparser` for `tools\luacheck.py`; Sandboxie-Plus for a second
  instance on the same machine.


### Playing

1. Launch the game. A **MULTIPLAYER** bar appears over the title menu.
2. The host presses **HOST**. A short base32 code is generated and copied to the
   clipboard; send it to your friend however you like.
3. Each joiner copies the code and presses **JOIN**; the panel reads it from the clipboard.
4. The roster and chat live in the same panel.
5. The host presses **START GAME** (with at least one joiner). The host's newest save
   (`.sav`, `.sav.lua`, `.jpg`) is sent to every joiner and placed as `mp_shared.sav` on
   all machines; each player opens **Load Game** and picks it. A joiner whose transfer
   failed is told to have the host press START GAME again.

## Contributing and credits

Issues and pull requests are welcome, especially reproductions with the logs from
`%LOCALAPPDATA%\tpf2mp\data` attached. Please keep the project's habits: every destructive replication channel
ships behind a flag that defaults off, and a field identification counts only when a
differential sweep confirms it.

- Companies mode is inspired by, and reuses the engine mechanisms proven in, Swiss's
  **Multiplayer Companies** Workshop mod (id 3710243057): runtime `addPlayer`,
  `setPlayer`, `bookJournalEntry` to a specific player, `setBulldozeable`.
- [TpF2 Big Maps](https://github.com/silver2127/tpf2-bigmap) grew out of this project:
  maps past the New Game menu's sizes, as a plugin for the same proxy.
- Licensed under the [MIT License](LICENSE). Third-party material is listed in
  [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).

**Disclaimer.** This project is not affiliated with or endorsed by Urban Games. It
modifies a file inside your Transport Fever 2 installation (`alut.dll` is renamed and
replaced by a forwarding proxy; Steam's file verification puts the stock file back) and
patches game code in memory while running. Use it at your own risk and keep backups.
Multiplayer saves are ordinary `.sav` files; the mod's save hook stores nothing of its
own (`save = function() return {} end`).
