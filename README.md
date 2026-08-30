# tpf2-multiplayer

[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](LICENSE)

**Lockstep multiplayer for Transport Fever 2** (Steam, Windows, build 35924). Unofficial,
reverse-engineered, and written without access to the engine source. Two or more players
build in one world at the same time: roads, rails, stations, depots, vehicles and lines
made by one player appear on everyone's map, executed at the same in-game moment. Two
modes share the same mod: **co-op**, where everyone plays one shared company, and
**companies**, where each player owns a separate company and wallet on the same map.

This is an experimental project. It works on the author's two-instance test rig; it is
not yet something you download and double-click. Read [Status](#status) before trying it.

## Status

**Replicates today** (verified live on two instances, both directions, with the world
hash staying identical):

| Area | What |
|---|---|
| Roads and rails | Straight and curved polylines, elevation, junctions, mid-span splits, merges into existing nodes, catenary, track/street type |
| Bridges and tunnels | Bridge/tunnel type and index travel with each edge (road and rail); level crossings are rebuilt as real `RAILROAD_CROSSING` nodes |
| Constructions | Stations, depots, modular terminals: placement (snapped and connected to the road), edits/upgrades, demolition; a station placed over town buildings demolishes the same buildings on every peer |
| Vehicles | Buy, sell, send to depot, reverse; vehicles are matched across peers by purchase key, never by entity id |
| Lines | Create, update, delete, assign a vehicle to a line; stops are matched by station position |
| Companies mode | A remote player's build lands owned by *their* company on your machine, is locked against your bulldozer, and its cost is moved from your wallet to theirs (`bookJournalEntry`) |

**Known limitations**

- Rail cannot cross a road bridge; the engine refuses the proposal.
- **Internet play between two machines is not wired up.** The lobby punches a hole and
  transfers the save, but the lockstep transport's peer address still defaults to
  loopback (`bridge_main.cpp`, `peerIp = "127.0.0.1"`). See
  [docs/PORTABILITY.md](docs/PORTABILITY.md), item 6.
- No installer yet. An MSI is in progress; today the install is a developer procedure
  (scripts under `tools/`, see [Installing](#installing-today-a-developer-procedure)).
- The shared-save auto-load clicks the title menu's **Continue** button at measured
  screen coordinates (`menu_hook.cpp`, `clickContinueLoad`). On another resolution or
  UI scale the click misses and the save has to be loaded by hand
  ([docs/PORTABILITY.md](docs/PORTABILITY.md), item 1).
- The DLLs and the Lua mod currently agree on a hardcoded runtime directory under the
  Steam workshop content folder (PORTABILITY item 2); moving either half breaks
  replication silently.
- Companies mode is selected by a small config file (`mp_company_cfg.txt`: mode, your
  company id, roster) that the lobby does not write yet; the test rig writes it by hand.
- Windows only. The tested setup is two instances on one machine, the second sandboxed
  with Sandboxie-Plus.
- Buy/sell/send-to-depot are applied optimistically on the originator (the engine's UI
  waits for the result and crashes if the command is suppressed); reverse and all
  builds are strict lockstep. See [docs/SLICE_STATUS.md](docs/SLICE_STATUS.md).

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
             | files                                            | jsonl files
   tpf2_bridge_mp.dll  <-- UDP -->  peer                netpunch.exe / lobby.py
                                                       STUN, UPnP, IPv6, hole punch,
                                                       host-relay lobby, save transfer
```

- **Proxy `alut.dll`** (`bridge/src/proxy_alut.cpp`). `alut.dll` is a static import of the
  game executable, so the loader maps it before the exe's entry point. The proxy forwards
  every export to the renamed original and loads the two DLLs below from a thread. It
  contains no freealut code.
- **Bridge** (`bridge/src/bridge_main.cpp`, `net.cpp`, `savexfer.cpp`). Tails the Lua
  capture file, sends new lines to the peer over reliable UDP, writes received lines to
  the events file the Lua reads, and decides which instance it is (`a`/`b`) from port
  availability.
- **Menu overlay** (`bridge/src/menu_hook.cpp`). Detours the title-menu page builder to
  add a **MULTIPLAYER** button and intercepts `vkQueuePresentKHR` to draw the lobby panel
  (HOST / JOIN / roster / chat / START GAME) inside the game's own frame. It drives the
  lobby process through `lobby_in.jsonl` / `lobby_out.jsonl`.
- **Slice** (`bridge/src/slice_hook.cpp`). The capture-and-cancel hook. A player's build
  goes through two consecutive calls in `StreetBuilder::UpdateEngine`: the proposal is
  fully formed at `make_cmd::BuildProposal` and not yet queued at `CommandList::Add`, so
  the hook reads the nodes, edges, tangents, types and removals at the first and cancels
  at the second. Vehicle and line factories are hooked the same way. Suppression is off
  unless `tpf2_slice.cfg` says `suppress=1`.
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
| `bridge/src/` | The DLLs: `proxy_alut.cpp`, `bridge_main.cpp` + `net.cpp` + `savexfer.cpp`, `menu_hook.cpp`, `slice_hook.cpp`, `hook.cpp` (trampolines), the `.asm` relays, and older probes (`args_probe`, `cmdlist_probe`, `payload_probe`, `probe_apply`, `defer_hook`) |
| `bridge/src/vk/` | Vendored Vulkan headers (Khronos, Apache-2.0 OR MIT) |
| `bridge/build_*.bat` | MSVC build scripts; `build_proxy.bat`, `build_menu.bat`, `build_slice.bat` are the ones that matter today |
| `mod/mp_lockstep_1/` | The Lua game-script mod ("MP Lockstep") |
| `mod/mp_bridge_1/`, `mod/m3_determinism_1/` | Earlier state-replication mod and the determinism probe; not used by the lockstep path |
| `netpunch/` | `punch.py`, `observe.py`, `connect.py`, `lobby.py`, `requirements.txt`, `NOTES.md` |
| `injector/` | `injector.cpp`, a plain `LoadLibrary` injector (`injector.exe <pid|name> <dll>`) |
| `tools/` | Install/deploy scripts, the two-instance harness, `luacheck.py`, and the static-analysis toolkit (`extract.py`, `find_sym.py`, `funcsig.py`, `tpfdis.py`, `ghidra_*.ps1`, `ghidra_scripts/`) |
| `tools/scenarios/` | Scripted replication scenarios the harness runs |
| `docs/re/` | Reverse-engineering notes for build 35924 |
| `docs/` | `PORTABILITY.md`, `SLICE_STATUS.md`, `COMPANIES_INTEGRATION.md`, `DEV_STATUS.md`, milestone reports `M1`..`M10` |
| `THIRD_PARTY_NOTICES.md` | Third-party licenses |

## Installing (today: a developer procedure)

Everything below is what the scripts in `tools/` do; read each one before running it.
They modify a file in your game folder and copy files into your Steam directories.

**Prerequisites**

- Windows, Steam, Transport Fever 2 (build 35924; the hook RVAs are specific to it).
- Visual Studio 2022 Build Tools with the MSVC x64 toolchain. The `.bat` scripts call
  `vcvars64.bat` from the Build Tools install and need `cl`, `link` and `ml64`.
- For the lobby: either the frozen `netpunch\dist\netpunch.exe`, or Python 3.12 with
  `pip install -r netpunch\requirements.txt` (`pystun3`, `miniupnpc`). PyInstaller is only
  needed to build the frozen exe: `cd netpunch; python -m PyInstaller --onefile --name netpunch lobby.py`.
- Optional: `pip install luaparser` for `tools\luacheck.py`; Sandboxie-Plus for a second
  instance on the same machine.

**1. Build the DLLs** (from a normal shell; each script sets up MSVC itself)

```
bridge\build_proxy.bat      -> bridge\out\tpf2_bridge_mp.dll and bridge\out\alut.dll
bridge\build_menu.bat       -> bridge\out\tpf2_menu.dll
bridge\build_slice.bat      -> bridge\out\tpf2_slice.dll
```

Build the injector from an x64 developer prompt: `cl /O2 injector\injector.cpp`.

**2. Stage the runtime DLLs.** The proxy resolves `tpf2_bridge_mp.dll` and
`tpf2_menu.dll` from `%LOCALAPPDATA%\tpf2mp\`, then next to itself, then the dev
workshop path `<Steam>\steamapps\workshop\content\1066780\3710243057\recon\m4\out`.
Because `lockstep.lua` and `slice_hook.cpp` still hardcode that last directory as their
I/O location, the working setup today is to put the DLLs there (PORTABILITY item 2).

**3. Install the proxy** with `tools\install_proxy.ps1`. It refuses to run if the proxy is
not built or `tpf2_bridge_mp.dll` is not staged, copies the stock `alut.dll` to
`backup\<date>\alut.dll.orig` and prints its SHA-256, renames the stock file to
`alut_real.dll`, copies `bridge\out\alut.dll` in its place, and verifies the hash again.
It will not overwrite an existing `alut_real.dll`. On the next launch
`tpf2_proxy.log` gains a line before the menu appears.
Revert with `tools\install_proxy.ps1 -Uninstall`, or use Steam's
**Verify integrity of game files**, which restores the stock DLL and deletes the proxy.

**4. Deploy the Lua mod** with `tools\deploy_mod.ps1 -Marker <string-in-the-new-code>`.
The game reads mods from more than one directory, so the script copies
`mod\mp_lockstep_1` to both `<game>\mods\mp_lockstep_1` and
`<Steam>\userdata\<id>\1066780\local\mods\mp_lockstep_1` and greps each target for the
marker. Mods are read at load, so restart any running instance. Then enable
**MP Lockstep** in the save's mod list in-game. Never run it alongside **MP Bridge**.

**5. Stage netpunch** with `tools\install_portable.ps1` (`-WhatIfOnly` for discovery only).
It finds Steam, the game and the save folder from the registry and copies
`netpunch.exe` plus the `.py` files to `%LOCALAPPDATA%\tpf2mp\netpunch`, which is where
the menu DLL looks first; without the exe it falls back to `python lobby.py`.

### Playing

1. Launch the game. The title menu has a **MULTIPLAYER** entry.
2. The host presses **HOST**. A short base32 code is generated and copied to the
   clipboard; send it to your friends however you like.
3. Each joiner copies the code and presses **JOIN**; the panel reads it from the clipboard.
4. The roster and chat live in the same panel.
5. The host presses **START GAME**. The host's newest save (`.sav`, `.sav.lua`, `.jpg`) is
   sent to every joiner, placed as `mp_shared.sav` and stamped newest on all machines,
   and each game clicks **Continue** to load it. A joiner whose transfer failed is told
   to have the host press START GAME again.

What the harness still does by hand after the save has loaded: inject the slice DLL into
each game process with `injector\injector.exe <pid> bridge\out\tpf2_slice.dll` and place
a `tpf2_slice.cfg` (`suppress=1` for real lockstep) next to it. `tools\mp_launch.ps1`
does both for the two-instance rig. The bridge reads its peer address and ports from a
sidecar `.cfg` next to the DLL (`peer_ip=`, `peer_port=`, `local_port=`); that is the
part that is not wired to the lobby yet.

## Developing

- **Two-instance harness.** `tools\autotest.ps1` launches instance A normally and
  instance B inside a Sandboxie-Plus box, loads the save in each, runs a scenario from
  `tools\scenarios\` from both sides and prints a verdict. `tools\mp_launch.ps1` is the
  one-command lockstep session (optional `-Save <name>` syncs a save host to peer, then
  launch, inject, cfg copy). `tools\mp_menu_launch.ps1` brings both instances up and
  stops at the title menu so HOST/JOIN can be exercised. `tools\slice_two_way.ps1` and
  `tools\lockstep_test.ps1` are the narrower lockstep checks.
- **Traces on disk** (in the runtime directory, and in B's sandbox overlay of it):
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
| `MENU_UI.md` | The title-screen menu builder and the Multiplayer button |
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
replaced by a forwarding proxy; Steam's file verification undoes it) and patches game code
in memory while running. Use it at your own risk and keep backups. Multiplayer saves are
ordinary saves and load in the unmodified game.
