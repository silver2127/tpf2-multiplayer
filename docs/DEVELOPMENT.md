# Development

## Repository layout

| path | contents |
|---|---|
| `native/src/` | C++ and MASM for the proxy, bridge, slice and menu DLLs; `plugin/` is the plugin host |
| `native/third_party/vk/` | Vulkan headers, for the menu overlay |
| `native/build.bat` | the build script for every DLL |
| `mod/mp_lockstep_1/` | the game-script mod: `res/config/game_script/lockstep.lua` and `res/scripts/mp/*.lua` |
| `netpunch/` | the lobby, dedicated relay and master server ([README](../netpunch/README.md)) |
| `installer/` | the WiX package ([README](../installer/README.md)) |
| `tools/` | deploy, rig, soak and check scripts; `tools/lib/` is the rig library; `tools/ghidra/` and `tools/re/` are the reverse-engineering tools |
| `docs/` | these documents; `docs/re/` is the engine reference; `docs/logo/` is the source of the mod's thumbnail |

## Prerequisites

- Windows 10 or 11 x64, Steam, Transport Fever 2 build 35924.
- Visual Studio 2022 Build Tools with the MSVC x64 toolchain in its default location (`build.bat` calls
  `C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat`).
- Python 3.12 with `pip install -r netpunch\requirements.txt`, plus `pyinstaller` to freeze the lobby,
  `luaparser` for `tools\luacheck.py`, `Pillow` for the logo scripts and `numpy pefile capstone` for
  `tools\re`.
- WiX Toolset v7 to build the MSI ([installer/README.md](../installer/README.md#building-the-msi)).
- Sandboxie-Plus in its default location, to run more than one game on a PC.

The scripts run in Windows PowerShell 5.1 and assume default install locations: Steam in
`C:\Program Files (x86)\Steam` with the game in its default library. `deploy_shipping.ps1` finds the
game through the registry; for the others, edit the path variables at the top of the script.

## Building

`native\build.bat <target> [suffix]` writes to `native\out\`:

| target | builds | notes |
|---|---|---|
| `proxy` | `tpf2_bridge_mp.dll` and `alut.dll` | takes no suffix: close the game first |
| `host` | `tpf2_pluginhost.dll` (and `alut.dll` again) | |
| `menu` | `tpf2_menu.dll` | also copies the new DLL into the game folder when it is not locked |
| `slice` | `tpf2_slice.dll` | |
| `all` | all of the above | |

A DLL loaded by a running game is locked and relinking it fails with LNK1104. A suffix builds beside it
(`build.bat slice 2` makes `tpf2_slice2.dll`); only unsuffixed names ship, and `deploy_shipping.ps1`
warns when a suffixed build is newer than the one it copies.

The lobby is frozen with PyInstaller ([netpunch/README.md](../netpunch/README.md#freezing)); the MSI is
built by `installer\build_msi.ps1` ([installer/README.md](../installer/README.md#building-the-msi)).

## Putting a build into your game

- **The mod.** `python tools\luacheck.py`, then `powershell -File tools\deploy_mod.ps1`
  (`-Mod mp_lockstep_1` is the default; `-Marker <text>` checks that a string made it into the deployed
  Lua). It copies the mod over `<game>\mods\mp_lockstep_1` and deletes any copy in the per-user mods
  folder (`userdata\<id>\1066780\local\mods`): a mod loaded from there gets the id `!mp_lockstep`, and
  saves made with it fail to load elsewhere. Files are copied over, never deleted, so a file removed from
  the repository lingers in the game folder. The game reads mods when it loads a game.
- **Everything.** With the game closed, `powershell -File tools\deploy_shipping.ps1 [-Cfg] [-Clean]` copies
  `alut.dll`, the four DLLs, `netpunch.exe` and the mod into the game folder. Cfg files are copied only
  where missing; `-Cfg` overwrites them. It also copies `tpf2mp.cfg` and, from a sibling `tpf2-bigmap`
  checkout, `plugins\tpf2_bigmap.dll`, which the MSI does not. `-Clean` empties the data folders of the
  native instance and the `GameAgent` and `GameAgent2` boxes. It needs `alut_real.dll` to exist already: install the MSI
  once first.
- **Which copy runs.** The proxy loads each DLL from `%LOCALAPPDATA%\tpf2mp\<name>` before the game folder,
  and the panel runs the lobby from `%LOCALAPPDATA%\tpf2mp\netpunch\` before `<game>\netpunch\`, preferring
  `netpunch.exe` to `lobby.py`. A stale copy in either place silently wins over a fresh build.
  `tpf2_proxy.log` in the data folder names the path of every DLL it loaded.

## Running several instances

- Instance A runs normally. Each additional one runs in its own Sandboxie-Plus box: `GameAgent`,
  `GameAgent2`, `GameAgent3`, ... (copies of the first box's settings in `Sandboxie.ini`). A new box's
  first launch only starts that box's Steam, which then waits on a Steam Cloud prompt because another
  instance holds the cloud session: put the boxed Steam in offline mode (or set `"cloudenabled" "0"` in
  the box's `localconfig.vdf`).
- `powershell -File tools\mp_menu_launch.ps1 [-Players N | -Solo]` stops every running game, starts A
  through Steam and the others in their boxes, and waits until each shows the title menu.
- **Letters come from the lobby.** Two games elect `a` and `b` by which bridge port is free; `c` and
  later exist only after a lobby JOIN, so with three or more players HOST in A, JOIN in the others and
  press START GAME. With exactly two, `tools\click_continue.ps1` can click CONTINUE in both instead; that
  loads each profile's last game (`lastGame` in `profile.lua`), so both profiles must name the same save.
- Only one un-sandboxed game per Windows session gets the slice's hooks.
- Give every instance the same bridge build. A bridge from 0.4.11 or earlier binds all interfaces, and
  one started after a newer bridge binds the same port beside it; the two games' local traffic then mixes.
- Joiners on one PC take relay ports 7774, 7775, ... and never link to each other directly (their LAN
  address is advertised with port 0), so their frames go through the host.
- A boxed instance keeps its own files: the data folder is
  `C:\Sandbox\<user>\<box>\user\current\AppData\Local\tpf2mp\data\`, and the game folder and stdout sit under
  `C:\Sandbox\<user>\<box>\drive\C\...`. A box also sees the real filesystem beneath its own writes, so
  another instance's file can show through: check timestamps.

## Logs

| log | where | contents |
|---|---|---|
| game stdout | `<Steam>\userdata\<steamid>\1066780\local\crash_dump\stdout.txt` | the mod's `[ls-<letter>]` lines; truncated at launch, flushed at exit |
| `tpf2_slice.log` | data folder, truncated at launch | captures, decodes, cancels |
| `tpf2_bridge.log` | data folder | identity, the link, every line sent and received |
| `tpf2_proxy.log`, `tpf2mp_host.log` | data folder | DLL and plugin loading |
| `mp_company_<letter>.log` | data folder | companies, crossing and road-plan decisions |
| `lockstep_dash_<letter>.txt`, `lockstep_status_<letter>.txt` | data folder | the window's values and recent events |
| `tpf2_menu.log` | next to `tpf2_menu.dll` | the panel and the lobby launcher |
| `lobby_proc.log`, `lobby_peers.log` | the lobby folder | the lobby's output; on the host, every player's forwarded logs |
| minidumps | `<Steam>\userdata\<steamid>\1066780\local\crash_dump\*.dmp` | engine asserts; an entity call with a nil id writes one too |

Lines worth searching for: `EXEC <op> ... success=`, `!! DESYNC`, `~~ LAG`, `DIVERGENCE`, `!! LATE`,
`NACK`, `RESEND`, `CATCHUP:`, `LOADGATE:`, `SPEED2:`, `PID:` in stdout; `CANCEL local build`,
`CANCEL fire-and-forget`, `callback NOT fired`, `stays local`, `UNREPLICATED BuildProposal` in
`tpf2_slice.log`.

**Snapshot before restarting a game.** `powershell -File tools\snapshot_logs.ps1 [-Tag name]` copies stdout,
the data-folder files and the game folder's `egeo_*.txt` and `tpf2_slice.cfg` from the native instance and
the `GameAgent`, `GameAgent2` and `GameAgent3` boxes into `%LOCALAPPDATA%\tpf2mp\runs\<timestamp>[-tag]\`. It does not
collect `tpf2_bridge.log`, `tpf2_proxy.log`, `tpf2_menu.log` or minidumps.

## Checks and tests

- **`python tools\luacheck.py [files]`** after every Lua edit. It parses every Lua file under `mod/` and
  reports a UTF-8 BOM, parse errors, more than 200 top-level locals (counting a module factory's body),
  raw newlines inside strings, calls to a `local function` before its definition, and reads of a
  top-level local before its declaration. A plain syntax check misses the last two, which are nil at
  runtime and stop the script.
- **`powershell -File tools\pscheck.ps1`**: PowerShell parse errors, variables whose names differ only in
  case (PowerShell treats them as one), and array literals that flatten. To check specific files pass
  them in-process (`-Path @(...)`); with `-File`, only the first path is checked.
- **Lobby self-tests**: [NETWORKING.md](NETWORKING.md#self-tests); `python tools\relay_selftest.py` for
  the relay.
- **Installer**: `installer\test_upgrade.ps1` ([installer/README.md](../installer/README.md#testing-an-upgrade)).
- **Pacing**: `python tools\pacing_sim.py [--ref <git ref>] [--only <scenario>]` runs the mod's real
  `pacing.lua` in a closed-loop simulation of several games (scenarios `far_ahead`, `hot_join_1x`,
  `hot_join_2x`, `live_start`, `speed_drop`) and compares with a git ref (default `HEAD`).
- **Soak test.** `powershell -File tools\soak.ps1` grades a live session against assertions:
  - Modes: attach to running games (default); `-Launch [-Deploy] [-Players N]` starts the rig first;
    `-AssertOnly` grades without acting; `-FromSnapshot <dir|latest>` grades saved logs; `-Manual` waits
    for a person to perform each UI action; also `-Quick`, `-Only <ids>`, `-SoakSeconds`,
    `-CloseWhenDone`, `-Measure`, `-WhatIf`.
  - Actions: `loan` runs by itself (repays and re-takes a loan through the mod); `road`, `upgrade`,
    `station`, `depot`, `buy`, `assign` and `sell` need `-Manual`, and are otherwise reported as not
    performed.
  - Exit code 0 is PASS, 1 FAIL, 2 unmet preconditions. On a failure it snapshots the logs and writes
    `soak_report.txt` into the snapshot folder.

  | id | passes when |
  |---|---|
  | A1 | no instance crashed, and as many are running as `-Players` |
  | A2 | every instance has a distinct letter and a recent identity file |
  | A3 | every game script updated its dashboard within 60 s |
  | A4 | every verdict is `SYNC` |
  | A5 | no desyncs counted |
  | A6 | every instance hears and agrees with every other |
  | A7 | balance and loan are identical (co-op) |
  | A8 | edge geometry is identical (needs `dump_egeo=1`) |
  | A9 | no new minidump |
  | A10 | no `DIVERGENCE`, failed apply or Lua error in the logs |
  | A11 | game time advanced (paused instances excepted) |
  | A12 | no command left queued |
  | A13 | every action executed on every other instance |

  Limits: the default `-Players` is 3, so A1 fails on a two-instance rig unless you pass `-Players 2`; only
  the native instance and the `GameAgent`/`GameAgent2` boxes are known; and its capture patterns predate
  `CONXP`, `CONUP`, `CDEMO`, `EDEMO`, `STOPX` and `STOPXDEL`, so an action that produces only
  those reads as not performed.
- **In a game**: watch the Multiplayer window's verdict, and after a divergence diff `egeo_<letter>.txt`
  (`dump_egeo=1`) between instances, ignoring the first line.

## Releasing

1. Bump `installer/VERSION` and `LOBBY_VERSION` in `netpunch/lobby.py` (the version shown in the public
   game list).
2. `powershell -ExecutionPolicy Bypass -File installer\build_msi.ps1 -AcceptWixEula -Validate`.
3. Tag the commit `v<version>` and publish `installer\out\TpF2Multiplayer.msi` as a GitHub release asset,
   with its SHA-256 in the notes.
4. If the lobby changed, redeploy the relay with `sh tools/relay_deploy.sh` (it refuses while players are
   connected) and the master server with `sh tools/masterserver_deploy.sh`
   ([NETWORKING.md](NETWORKING.md#dedicated-relay)).

## Plugins

The plugin host loads extra DLLs from `plugins\` without touching the multiplayer DLLs; TpF2 Big Maps is
one. A plugin exports

```c
extern "C" __declspec(dllexport) int Tpf2mpPluginInit(const Tpf2mpHost* host, Tpf2mpPluginInfo* out);
```

(`native/src/plugin/tpf2mp_plugin.h`, ABI major version 1), fills `out` with static `name`, `version` and
`summary` strings, and returns `TPF2MP_OK` (0) or an error (1 ABI, 2 wrong game build, 3 disabled,
4 failed). The host table offers logging, settings lookups (`cfgInt`, `cfgBool`, `cfgStr`), the exe's base
address, a build check (`buildOk`), byte verification and patching by RVA, `installHook` (which takes an
absolute address) and the data folder. Initialisation runs on the host's thread under an exception guard; a
failing plugin is logged and stays loaded. The header's comments promise a few checks the host does not
make (refusing another ABI major, making a failed plugin inert), so check the return values yourself.
Settings: [CONFIGURATION.md](CONFIGURATION.md#tpf2mpcfg-and-plugin-settings).

## Conventions

- **Shared state in the mod is a field of `CM`.** Each module in `res/scripts/mp/` is a factory,
  `return function(CM, K, log) ... end`. A file-scope `local` is private to its module; a reference to it
  from another module is a nil global at runtime and stops the script.
- **Validate destructive channels on the rig before they merge.** Anything that removes or overwrites
  (demolish, replace, rebuild) is proven on a multi-instance rig first: a missing addition is visible and
  harmless, but a removal on the wrong instance destroys work that nothing can rebuild. There is no switch to
  ship a channel "off"; one behaviour ships.
- **Never cancel on a failed decode.** If a hook cannot read the whole command, let the command run.
- **Nothing on the wire names an entity id.** Positions, file names and bound keys only.
- **Apply by sim step.** Derive every wait from agreed stamps (`notBeforeStep`); never pace by frames and
  never rewrite a stamp.
- **Guard entity ids** before any `game.interface` or `api.engine` call: a nil id writes a minidump even
  inside `pcall`.
- **A field identification needs a differential or a sweep** ([re/README.md](re/README.md#rules-learned-the-hard-way)).
- **Snapshot the logs before restarting a game.**

## Other scripts

| script | state |
|---|---|
| `tools/collect_logs.cmd` | for bug reports: double-click it; it zips the data folder, the game-folder and lobby logs, game stdout, recent crash dumps and a system summary into Downloads ([PLAYING.md](PLAYING.md#when-something-goes-wrong)) |
| `tools/input.ps1`, `tools/screenshot.ps1` | input and screenshot helpers for driving and checking the game |
| `tools/segment_heap.ps1` | toggles the Segment Heap setting ([installer/README.md](../installer/README.md#segment-heap)) |
