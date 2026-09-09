# tpf2-multiplayer

[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](LICENSE)

**Real multiplayer for Transport Fever 2** (Steam, Windows, build 35924). Unofficial,
reverse-engineered, and written without access to the engine source. Two or more players build in
one world at the same time: roads, rails, stations, depots, vehicles and lines made by one
player appear on the other's map, executed at the same in-game moment. Two modes share
the same mod: **co-op**, where both play one shared company, and **companies**, where each
player owns a separate company and wallet on the same map. Alpha: verified between two
machines on different networks; the lobby seats sixteen, the sync itself has only run
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

**Playing:** *Multiplayer* on the title screen. **HOST GAME** opens a lobby and gives
you a code to hand out (Discord, or tick **PUBLIC** and the game shows up in the
**PUBLIC GAMES** list on everyone's Multiplayer page, OpenTTD style). **JOIN GAME**
takes a pasted code, or click a row in the list to fill it in. A password locks the
code: a public row shows as `[locked]` and needs the password typed below it. The
list is served by a tiny stdlib HTTP service (`netpunch/masterserver.py`, deployed
with `tools/masterserver_deploy.sh`); hosts announce every 30 s, entries expire
after 2 minutes, and nothing is brokered: the code is the join, the list only
repeats it. `master_url=` in `tpf2_menu_flags.txt` points the panel elsewhere;
an empty value hides the list.

**Dedicated relay:** a lobby can live on a server with no game
(`lobby.py host --relay-only`, deployed to the VPS by `tools/relay_deploy.sh` as
the `tpf2mp-relay` service, always listed in PUBLIC GAMES). Everyone joins it;
the first player in is the leader and gets the host role: START GAME uploads
their newest save to the relay, which pushes it to everyone waiting, and hot
joiners get the same treatment. Letters are assigned by the relay and stick to
names, so a returning leader is `a` again. Nobody needs an open port or a
non-CGNAT connection, and the code never changes while the relay runs. The
world only advances while players are connected: the relay carries frames, it
does not run the simulation.

**Uninstalling:** run the same MSI again and choose **Remove**, or use *Apps* in
Windows settings. Either removes every file it added and puts the game's own
`alut.dll` back (unless TpF2 Big Maps is still installed, in which case the shared
proxy stays for it).

**Prerequisites**

- Windows, Steam, Transport Fever 2 (build 35924; the hook RVAs are specific to it).
- Visual Studio 2022 Build Tools with the MSVC x64 toolchain (`cl`, `link`, `ml64`). The
  `native\build.bat` and the scripts in `tools/` (except `install_portable.ps1`) hardcode
  the default locations, Build Tools at `C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools`
  (`build.bat` calls `vcvars64.bat`) and Steam at `C:\Program Files (x86)\Steam`; with
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

## Repository layout

| Path | What it is |
|---|---|
| `native/` | The native side. One script, `build.bat <target>`: `proxy` (the `alut.dll` proxy and the bridge DLL: identity, relay socket, save transfer), `slice` (the command-capture hooks), `menu` (the Vulkan overlay lobby panel), `host` (the plugin host shared with TpF2 Big Maps), or `all`. Sources in `src/`, vendored Vulkan headers in `third_party/`. |
| `mod/mp_lockstep_1/` | The game mod. `res/config/game_script/lockstep.lua` is the entry point; the replication logic is in `res/scripts/mp/*.lua`, one module per concern. |
| `netpunch/` | The lobby: UDP hole punching, host-as-relay star, sealed frames, save transfer. `lobby.py` is what gets frozen into `netpunch.exe`. `masterserver.py` is the public game list it announces to. |
| `installer/` | The WiX package and its custom action; `README.md` there covers building the MSI. |
| `tools/` | Developer scripts: deploy the mod, build and ship the DLLs, launch the multi-instance rig, run the soak test, check the Lua. `tools/ghidra/` and `tools/re/` are the reverse-engineering helpers. |
| `docs/` | Current design and status notes; `docs/re/` the reverse-engineering findings the hooks rest on; `docs/history/` the milestone reports from the first phase. |

## Developing

- Lua: edit under `mod/`, run `python tools\luacheck.py`, then `tools\deploy_mod.ps1 -Mod mp_lockstep_1`
  (the game reads mods at load, so relaunch). Anything shared between the `mp` modules is a
  field of the `CM` table, never a file-scope local.
- DLLs: `native\build.bat <target>` writes to `native\out`; `tools\deploy_shipping.ps1`
  puts a full set into the game folder. A DLL loaded by a running game is locked, so the build
  script accepts a suffix for a side-by-side build (`build.bat slice 2`).
- Rig: `tools\mp_menu_launch.ps1 -Players 3` brings up host and joiners on one machine (Sandboxie
  for the extra instances); `tools\snapshot_logs.ps1` first, because the game truncates its logs
  on launch. `tools\soak.ps1` is the unattended regression run.

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
