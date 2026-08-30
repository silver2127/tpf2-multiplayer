# tpf2-multiplayer

[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](LICENSE)

**Lockstep multiplayer for Transport Fever 2** (Steam, Windows, build 35924). Unofficial,
reverse-engineered, and written without access to the engine source. Two players build in
one world at the same time: roads, rails, stations, depots, vehicles and lines made by one
player appear on the other's map, executed at the same in-game moment. Two modes share
the same mod: **co-op**, where both play one shared company, and **companies**, where each
player owns a separate company and wallet on the same map. The lobby already seats up to
8 players, but the lockstep transport itself is point-to-point and has only ever run
between two instances.

This is an experimental project. It works on the author's two-instance test rig; it is
not yet something you download and double-click. Read [Status](#status) before trying it.

## Status

**Replicates today** (verified live on two instances, both directions; the geometry-based
world hash matched over the test runs -- the detector is reliable for short sessions, see
[docs/SLICE_STATUS.md](docs/SLICE_STATUS.md) on town-road drift over multi-hour runs):

| Area | What |
|---|---|
| Roads and rails | Straight and curved polylines, elevation, junctions, mid-span splits, merges into existing nodes, catenary, track/street type |
| Constructions | Stations, depots, modular terminals: placement (snapped and connected to the road) and demolition; a station placed over town buildings clears the town buildings inside the same footprint on every peer (approximate footprint, tuned for the modular station) |
| Vehicles | Buy, sell, send to depot, reverse; vehicles are matched across peers by purchase key, never by entity id |
| Lines | Create, update, delete, assign a vehicle to a line; stops are resolved by station position |

**Implemented, verification still in progress**

- Bridge type/index is carried per edge (road and rail bridges verified live; tunnels use the same field but are untested). Level crossings:
  the replay splits the road at the rail so the engine records a `RAILROAD_CROSSING`;
  verified for the single-node form at a road end, the two-node connector form the
  engine normally builds is still being worked out ([docs/re/LEVEL_CROSSING.md](docs/re/LEVEL_CROSSING.md)).
- Construction module edits/upgrades (`CONU`) are implemented but not yet verified on
  the lockstep path.
- Companies mode (`lockstep.lua`, opt-in): a remote player's build is reassigned to the
  origin company (`setPlayer`), locked against your bulldozer (`setBulldozeable`, constructions
  only) and its cost moved from your wallet to theirs (`bookJournalEntry`) for constructions
  and vehicle purchases; lines and vehicles on lines get ownership only. Not yet
  documented as verified live ([docs/COMPANIES_INTEGRATION.md](docs/COMPANIES_INTEGRATION.md)).

**Known limitations**

- **Internet play between two machines is not wired up.** The lobby punches a hole and
  transfers the save, but the lockstep transport's peer address still defaults to
  loopback (`bridge_main.cpp`, `peerIp = "127.0.0.1"`). The bridge now polls a control
  file (`tpf2_bridge_ctl.txt`, `peer=<ipv4>:<port>`) for exactly this hand-off, but
  nothing writes it yet. See [docs/PORTABILITY.md](docs/PORTABILITY.md), item 6.
- The installer is an **alpha** ([Releases](https://github.com/silver2127/tpf2-multiplayer/releases)):
  it installs everything, but two-machine play has only been exercised as far as
  the transport (see below) and the shared-save auto-load still depends on the
  screen-coordinate click.
- The shared-save auto-load clicks the title menu's **Continue** button at measured
  screen coordinates (`menu_hook.cpp`, `clickContinueLoad`: four measured window sizes,
  scaled if yours is within 5% of one). Otherwise the click is skipped and the save has
  to be loaded by hand (PORTABILITY item 1).
- The host must be reachable: netpunch tries UPnP, STUN and IPv6 and reports which
  worked, but the lobby is host-as-relay and assumes an open or mappable NAT. START GAME
  does nothing with zero joiners.
- Only road/rail builds and Reverse are strict lockstep (cancelled on the originator,
  replayed at the stamp on everyone). Constructions, buy/sell/send-to-depot and all line
  commands are applied on the originator at once and replayed on the peer at the stamp,
  about 0.7 s later (the depot UI waits for the buy result and crashes if it is
  suppressed). See SLICE_STATUS.
- Companies mode is selected by a small config file (`mp_company_cfg.txt` in the data
  directory: mode, your company id, roster) that nothing in the repo writes yet.
- Windows only. The tested setup is two instances on one machine, the second sandboxed
  with Sandboxie-Plus.

## How it works

```
 TransportFever2.exe
 +--------------------------------------------------------------------------+
 |  alut.dll (proxy, forwards all 20 exports to alut_real.dll)              |
 |    -> loads tpf2_bridge_mp.dll   lockstep transport, instance identity   |
 |    -> loads tpf2_menu.dll        Vulkan present hook: MULTIPLAYER lobby  |
 |                                                                          |
 |  tpf2_slice.dll (injected after the save loads)                          |
 |    hooks make_cmd::BuildProposal  -> read the proposal from memory       |
 |    hooks CommandList::Add         -> CANCEL it locally                   |
 |    writes  lockstep_inject_<inst>.txt                                    |
 |                                                                          |
 |  mp_lockstep_1 (Lua game script)                                         |
 |    reads the capture, stamps it with game time + EXEC_DELAY, ships it,   |
 |    and every peer (originator included) replays it at that stamp        |
 +-----------|--------------------------------------------------|-----------+
             | files in %LOCALAPPDATA%\tpf2mp\data              | jsonl files
   tpf2_bridge_mp.dll  <-- UDP -->  peer                netpunch.exe / lobby.py
                                                       STUN, UPnP, IPv6, hole punch,
                                                       host-relay lobby, save transfer
```

- **Proxy `alut.dll`** (`bridge/src/proxy_alut.cpp`). `alut.dll` is a static import of the
  game executable, so the loader maps it before the exe's entry point. The proxy forwards
  every export to the renamed original and loads the two DLLs below from a thread. It
  contains no freealut code.
- **Bridge** (`bridge/src/bridge_main.cpp`, `net.cpp`, `savexfer.cpp`). Tails the Lua
  capture file, sends new lines to the peer over its own reliable UDP, writes received
  lines to the events file the Lua reads, and decides which instance it is (`a`/`b`) from
  port availability. Config is `tpf2_bridge_mp.cfg` next to the DLL (then the data
  directory); defaults are `local_port=7771`, `peer_port=7772`, `peer_ip=127.0.0.1`,
  `instance=auto`.
- **Runtime data directory** (`bridge/src/datadir.h`). Everything written at run time
  (identity, captures, events, injects, status, logs) goes to `%LOCALAPPDATA%\tpf2mp\data`,
  or `TPF2MP_DATADIR` if set; the Lua mod checks the same candidates and takes the one
  holding `tpf2_instance.txt`, which the bridge writes at startup.
- **Menu overlay** (`bridge/src/menu_hook.cpp`). Detours `UI::CMenuUI::CreatePage` only to
  learn when the main title page is on screen, then draws its own MULTIPLAYER bar and the
  lobby panel (HOST / JOIN / roster / chat / START GAME) over the frame from the
  `vkQueuePresentKHR` hook. It drives the lobby process through `lobby_in.jsonl` /
  `lobby_out.jsonl`.
- **Slice** (`bridge/src/slice_hook.cpp`). The capture-and-cancel hook. A player's build
  goes through two consecutive calls in `StreetBuilder::UpdateEngine`: the proposal is
  fully formed at `make_cmd::BuildProposal` and not yet queued at `CommandList::Add`, so
  the hook reads the nodes, edges, tangents, types and removals at the first and cancels
  at the second (firing the build tool's completion callback so the UI does not hang).
  The vehicle and line command factories are hooked the same way. Suppression is
  off unless `tpf2_slice.cfg` says `suppress=1`; a failed decode never cancels.
- **Lua mod** (`mod/mp_lockstep_1/res/config/game_script/lockstep.lua`). The lockstep
  engine: schedules each captured command at `now + EXEC_DELAY` in game-time units,
  exchanges heartbeats, pauses if it runs more than `BARRIER_AHEAD` past the peer, resolves
  everything by *position* (entity ids differ between peers by design), and hashes the
  world by geometry to detect desyncs. Also holds the companies-mode ownership and cost
  transfer.
- **netpunch** (`netpunch/*.py`, frozen to `netpunch.exe` with PyInstaller).
  `observe.py` builds a connectivity profile (STUN, UPnP, IPv6), `punch.py` is the UDP
  hole-punch transport, `connect.py` does the code exchange and connect race, and
  `lobby.py` is the host-as-relay N-player lobby (up to 8 including the host) with
  usernames, chat and a NACK-based chunked save transfer verified by SHA-256.

**Why C++ at all?** The game's Lua API can *issue* commands (`api.cmd.sendCommand`), but it
cannot observe or stop the ones the native UI issues: game scripts only run once a save is
loaded, the title screen is built entirely in C++, and a player's click is applied by the
engine before any script sees it. Lockstep needs the originator to *not* apply its own
command until the agreed stamp, which is only possible by cancelling it inside the engine.
The reverse-engineering that made those hook points safe is written up in `docs/re/`.

## Repository layout

| Path | Contents |
|---|---|
| `bridge/src/` | The DLLs: `proxy_alut.cpp`, `bridge_main.cpp` + `net.cpp` + `savexfer.cpp` + `datadir.h`, `menu_hook.cpp`, `slice_hook.cpp`, `hook.cpp` (trampolines), the `.asm` relays, `simhook`/`buyhook` probes, and older probes (`args_probe`, `cmdlist_probe`, `payload_probe`, `probe_apply`, `defer_hook`, `capture`) |
| `bridge/src/vk/` | Vendored Vulkan headers (Khronos, Apache-2.0 OR MIT) |
| `bridge/build_*.bat` | MSVC build scripts; `build_proxy.bat`, `build_menu.bat`, `build_slice.bat` are the ones that matter today |
| `mod/mp_lockstep_1/` | The Lua game-script mod ("MP Lockstep") |
| `mod/mp_bridge_1/`, `mod/m3_determinism_1/` | Earlier state-replication mod and the determinism probe; not used by the lockstep path |
| `netpunch/` | `punch.py`, `observe.py`, `connect.py`, `lobby.py`, `requirements.txt`, `netpunch.spec`, `NOTES.md` |
| `injector/` | `injector.cpp`, a plain `LoadLibrary` injector (`injector.exe <pid|name> <dll>`) |
| `tools/` | Install/deploy scripts, the two-instance harness, `luacheck.py`, the preview `friendkit/` installer, and the static-analysis toolkit (`extract.py`, `find_sym.py`, `funcsig.py`, `tpfdis.py`, `ghidra_*.ps1`, `ghidra_scripts/`) |
| `tools/scenarios/` | Scripted replication scenarios the harness runs |
| `docs/re/` | Reverse-engineering notes for build 35924 |
| `docs/` | `PORTABILITY.md`, `SLICE_STATUS.md`, `COMPANIES_INTEGRATION.md`, `DEV_STATUS.md`, `CANCEL_POINT.md`, milestone reports `M1`..`M10` |
| `THIRD_PARTY_NOTICES.md` | Third-party licenses |

## Installing

**Players:** download `TpF2Multiplayer.msi` from the
[latest release](https://github.com/silver2127/tpf2-multiplayer/releases), close the
game, run it. It finds the game folder from Steam's registry entry, installs the
proxy `alut.dll` (the original is kept as `alut_real.dll`), the lockstep DLLs and
their cfgs, the `mp_lockstep_1` mod into `<game>\mods`, and the frozen lobby into
`<game>
etpunch`. Runtime files go to `%LOCALAPPDATA%	pf2mp\data`. Uninstall from
*Apps* (restores `alut.dll`); Steam's *Verify integrity of game files* also undoes it.

Never copy the mod into `userdata\<id>\1066780\local\mods`: the game treats a mod
loaded from there as a different mod (`!mp_lockstep`), and saves made with it refuse to
load on machines that installed it normally.

**Developers** -- everything below is what the scripts in `tools/` do; read each one
before running it. They modify a file in your game folder and copy files into your
Steam directories. `tools/deploy_shipping.ps1` lays the game folder out exactly as the
MSI does, from the build outputs.

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

**1. Build the DLLs** (from a normal shell; each script sets up MSVC itself). Create
`bridge\out` first (`mkdir bridge\out`): `build_proxy.bat` does not create it and
`bridge/out/` is gitignored.

```
bridge\build_proxy.bat      -> bridge\out\tpf2_bridge_mp.dll and bridge\out\alut.dll
bridge\build_menu.bat       -> bridge\out\tpf2_menu.dll (also copies it into the workshop
                               directory below when the game is not running)
bridge\build_slice.bat      -> bridge\out\tpf2_slice.dll
```

Build the injector from an x64 developer prompt:
`cl /O2 /Fe:injector\injector.exe injector\injector.cpp`.

**2. Stage the runtime DLLs.** The proxy resolves `tpf2_bridge_mp.dll` and
`tpf2_menu.dll` from `%LOCALAPPDATA%\tpf2mp\`, then next to itself in the game folder,
then the dev workshop path `<Steam>\steamapps\workshop\content\1066780\3710243057\recon\m4\out`.
`tools\install_proxy.ps1` as written checks only that workshop path (`$Out` at the top of
the script). It is the folder of Workshop item 3710243057 (Swiss's Multiplayer Companies
mod): subscribe to it in Steam or create `recon\m4\out` by hand, then copy both DLLs
there -- or edit `$Out`. A `tpf2_bridge_mp.cfg` next to the bridge DLL overrides the defaults.

**3. Install the proxy** with `tools\install_proxy.ps1`. It aborts unless
`bridge\out\alut.dll` exists and `tpf2_bridge_mp.dll` is in the workshop directory,
copies the stock `alut.dll` to `backup\2026-08-06\alut.dll.orig` (a fixed path in the
script) and prints its SHA-256, renames the stock file to `alut_real.dll`, copies
`bridge\out\alut.dll` in its place, and verifies the hash again. It will not overwrite an
existing `alut_real.dll`. On the next launch `tpf2_proxy.log` gains a line before the
menu appears. Revert with `tools\install_proxy.ps1 -Uninstall`, or use Steam's
**Verify integrity of game files**, which puts the stock `alut.dll` back over the proxy
(the leftover `alut_real.dll` can be deleted by hand).

**4. Deploy the Lua mod** with `tools\deploy_mod.ps1` (add `-Marker <string-in-the-new-code>`
to verify the copy landed). It copies `mod\mp_lockstep_1` to `<game>\mods\mp_lockstep_1`
only, and deletes any copy under `userdata\<id>\1066780\local\mods` (a mod loaded from
there gets a different id, `!mp_lockstep`, and taints every save). Mods are read at load,
so restart any running instance. Then enable **MP Lockstep** in the save's mod list
in-game. Never run it alongside **MP Bridge**.

**5. Stage netpunch** with `tools\install_portable.ps1` (`-WhatIfOnly` for discovery only).
It finds the Steam root from the registry, the game from Steam's library list and the
save folder under `userdata`, then copies `netpunch.exe` (if built) plus the `.py` files to
`%LOCALAPPDATA%\tpf2mp\netpunch`, where the menu DLL looks first. Do this even for a dev
setup unless your clone is at `%USERPROFILE%\tpf2-multiplayer`, the only fallback the
menu DLL knows. Without `netpunch.exe` it runs `python lobby.py` from that directory, so
`python` must be on the game process's `PATH` with the two dependencies installed.

### Playing

1. Launch the game. A **MULTIPLAYER** bar appears over the title menu.
2. The host presses **HOST**. A short base32 code is generated and copied to the
   clipboard; send it to your friend however you like.
3. Each joiner copies the code and presses **JOIN**; the panel reads it from the clipboard.
4. The roster and chat live in the same panel.
5. The host presses **START GAME** (with at least one joiner). The host's newest save
   (`.sav`, `.sav.lua`, `.jpg`) is sent to every joiner, placed as `mp_shared.sav` and
   stamped newest on all machines, and each game clicks **Continue** to load it. A joiner
   whose transfer failed is told to have the host press START GAME again.

What the harness still does by hand after the save has loaded: inject the slice DLL into
each game process with `injector\injector.exe <pid> bridge\out\tpf2_slice.dll`, with a
`tpf2_slice.cfg` (`suppress=1` for real lockstep) next to `tpf2_slice.dll` or in the data
directory. `tools\mp_launch.ps1` does both for the two-instance rig. The bridge's peer
address (`peer_ip=`/`peer_port=` in `tpf2_bridge_mp.cfg`, or `peer=` in the control file)
is the part that is not wired to the lobby yet.

## Developing

- **Two-instance harness.** `tools\autotest.ps1` launches instance A normally and
  instance B inside a Sandboxie-Plus box, loads the save in each, runs a scenario from
  `tools\scenarios\` from both sides and prints a verdict. `tools\mp_launch.ps1` is the
  one-command lockstep session (optional `-Save <name>` syncs a save host to peer, then
  launch, inject, cfg copy). `tools\mp_menu_launch.ps1` brings both instances up and
  stops at the title menu so HOST/JOIN can be exercised. `tools\slice_two_way.ps1` and
  `tools\lockstep_test.ps1` are the narrower lockstep checks. All expect Sandboxie-Plus
  at its default path with a box named `GameAgent`, the default Steam location, and read
  traces from the workshop `out` directory -- set `TPF2MP_DATADIR` to it so the DLLs and
  the Lua write there.
- **Traces on disk** (in the data directory, and in B's sandbox overlay of it):
  `tpf2_proxy.log`, `tpf2_bridge.log`, `tpf2_menu.log`, `tpf2_slice.log`,
  `tpf2_instance.txt`, `lockstep_inject_<inst>.txt`, `tpf2_capture_<inst>.txt`,
  `tpf2_events_<inst>.txt`, `lockstep_status_<inst>.txt`, `mp_company_<inst>.log`; lobby
  traffic in `netpunch\lobby_out.jsonl` / `lobby_in.jsonl`. `LSHASH` lines carry the
  per-component world hash (`v` vehicles, `c` constructions, `e` edges, `t` town).
- **Lua check.** `python tools\luacheck.py` parses every `.lua` under `mod\` and counts
  top-level locals (Lua 5.1's 200-local limit crashed both instances once). A syntax
  error makes the game skip the whole mod silently, so run it before deploying.
- **Ground-truth sweeps.** `echo GT track >> lockstep_inject_a.txt` with
  `groundtruth=1` in `tpf2_slice.cfg`, then `tools\gt_correlate.ps1`; this is how the
  proposal offsets were pinned down (see SLICE_STATUS).

**Reverse-engineering notes** (`docs/re/`, all for build 35924, RVAs from image base
`0x140000000`):

| Doc | Subject |
|---|---|
| `ACTION_MAP.md` | Where every player action lands in the binary |
| `APPLYPROPOSAL_CALLERS.md` | Separating player actions from engine churn at `applyProposal` |
| `COMMAND_ARGS.md` | Command factory signatures and what to capture per action |
| `COMMAND_MAP.md` | Every command type, its handler, factory, Lua name and replication class |
| `COMMAND_SERIALIZATION.md` | Where the geometry lives at the `BuildProposal` hook |
| `GROUND_TRUTH_applyProposal.md` | Live `applyProposal` capture |
| `LEVEL_CROSSING.md` | How the engine creates rail-over-road crossings |
| `LOAD_SAVE.md` | Toward loading a save in-process (the Continue-click replacement) |
| `MENU_UI.md` | The title-screen menu builder and why the overlay draws its own button |
| `PROPOSAL_STRUCTURE.md` | Proposal fields recovered from assert strings |
| `UI_CAPTURE_PATH.md` | Where a player's build originates (with a correction) |

Also: [docs/PORTABILITY.md](docs/PORTABILITY.md) (what is still tied to the dev machine
and the order to fix it), [docs/SLICE_STATUS.md](docs/SLICE_STATUS.md) (living status of
the lockstep path, with the recipes and the bugs), [docs/COMPANIES_INTEGRATION.md](docs/COMPANIES_INTEGRATION.md)
(the companies-mode design), and [docs/DEV_STATUS.md](docs/DEV_STATUS.md) (the internal
developer log and resume guide).

## Contributing and credits

Issues and pull requests are welcome, especially reproductions with the trace files
above attached. Please keep the project's habits: every destructive replication channel
ships behind a flag that defaults off, and a field identification counts only when a
differential sweep confirms it.

- Companies mode is inspired by, and reuses the engine mechanisms proven in, Swiss's
  **Multiplayer Companies** Workshop mod (id 3710243057): runtime `addPlayer`,
  `setPlayer`, `bookJournalEntry` to a specific player, `setBulldozeable`. That mod is
  local hot-seat only; this project adds the networking.
- Licensed under the [MIT License](LICENSE). Third-party material is listed in
  [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).

**Disclaimer.** This project is not affiliated with or endorsed by Urban Games. It
modifies a file inside your Transport Fever 2 installation (`alut.dll` is renamed and
replaced by a forwarding proxy; Steam's file verification puts the stock file back) and
patches game code in memory while running. Use it at your own risk and keep backups.
Multiplayer saves are ordinary `.sav` files; the mod's save hook stores nothing of its
own (`save = function() return {} end`).
