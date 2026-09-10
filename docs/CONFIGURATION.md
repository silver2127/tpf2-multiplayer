# Configuration

Settings live in plain text files. Most players never need to touch them; they exist for
testing, for turning off a channel that misbehaves, and for development.

**Keep replication switches identical on every machine in a session.** A player whose switches
differ runs a different protocol (for example, a peer without `road_demolish` ignores road
demolitions and keeps the road).

## Files and where they are read from

| file | read by | where | changes take effect |
|---|---|---|---|
| `tpf2_slice.cfg` | slice DLL, the mod | the game folder, else the data folder | slice: on the next command; mod: within about 5 s (some keys only at load, noted below) |
| `tpf2_bridge_mp.cfg` | bridge DLL | the game folder, else the data folder | at game start |
| `tpf2_menu_flags.txt` | menu DLL | the game folder (next to `tpf2_menu.dll`) only | at game start |
| `tpf2mp.cfg` | plugin host | the game folder, else the data folder | at game start |

- The data folder is `%LOCALAPPDATA%\tpf2mp\data\` ([Environment](#environment)).
- **The first file found wins; files are never merged.** A copy in the data folder is ignored while the
  game folder has one, and the installer puts the game-folder copies back on every upgrade and Repair
  ([installer/README.md](../installer/README.md#upgrading)).
- **`tpf2_slice.cfg` syntax:** one `key=value` per line; lines whose first non-blank character is `#`
  or `;` are comments. The slice treats a switch as on when any line contains the text `key=1`, so
  `key = 1` or `key=true` count as off for it, and `strict_maint=1` also switches on `maint`. The mod
  accepts `1`, `true` or `yes`.
- **No `tpf2_slice.cfg` at all** means real multiplayer: the slice turns on `suppress` and the switches
  that are on in the shipped file (except `cancel_construction`). A file that exists but lacks a switch
  means that switch is off for the slice; the mod uses the defaults listed below for the keys it reads.

## `tpf2_slice.cfg`

### Master switches

| key | shipped | effect |
|---|---|---|
| `enabled` | 1 | `0` makes the slice inert: nothing captured, nothing cancelled; the game behaves like single player. |
| `suppress` | 1 | Cancel captured commands so they apply only through lockstep. `0` is an observe mode: the action happens locally and is also replicated, which duplicates it (debugging only). |
| `merge` | 1 | Weld a replayed construction's template connector onto the shipped road split ([re/PROPOSALS.md](re/PROPOSALS.md#construction-templates-and-the-connector)). |

### Replication switches

Each of these makes a channel strict (cancel and replay at the stamp on every instance); see
[REPLICATION.md](REPLICATION.md). The slice cancels only while a session is live. The tool-based
channels (roads, constructions, stops, demolitions, module edits) also need `suppress=1`; the vehicle
and line switches do not.

| key | shipped | mod default | channel |
|---|---|---|---|
| `cancel_vehicle` | 1 | | reverse a vehicle |
| `cancel_line` | 1 | | assign a vehicle to a line |
| `strict_buy` | 1 | on (read at load) | buy a vehicle |
| `strict_sell` | 1 | | sell vehicles |
| `strict_depot` | 1 | | send a vehicle to a depot |
| `strict_replace` | 1 | | replace a vehicle |
| `strict_line_edit` | 1 | | edit a line's stops, delete a line |
| `strict_module` | 1 | | module edits and construction upgrades |
| `strict_stops` | 1 | | place or bulldoze a stop, signal or waypoint |
| `strict_condemo` | 1 | | bulldoze a construction |
| `road_demolish` | 1 | off | bulldoze roads and track. Read by both the slice (cancel) and the mod (execute): on every instance or none. |
| `cancel_construction` | 1 | | place a construction |
| `conx_strict` | 1 | on | when a construction was not cancelled, the originator bulldozes its native copy and rebuilds it like the peers (money reconciled) |
| `maint` | not shipped | | ship vehicle maintenance-target changes (replay on peers) |
| `strict_maint` | not shipped | | with `maint`, make them strict |

### Timing and pacing (the mod)

| key | default | effect |
|---|---|---|
| `exec_delay` | 0.6 | How far ahead commands are stamped, in game-time units (snapped up to the 0.2 step grid): the latency of every action. Accepted 0.2-5; read once at load. 0.4 is safe on one machine or a LAN; below that a jitter spike puts a command in a peer's past. Same value on every instance. |
| `load_gate` | on | Hold a joining game at speed 0 after loading until the leader is heard. |
| `expect_players` | unset | Number of players including you; when set, the load gate waits for exactly that many. |
| `speed_v2` | on | One session speed (the lowest player's speed button, or `/speed`). `0` selects the older pacer. |
| `speed_auto` | off | Automatic corrections on top: the leader pulses speed 0 when ahead, and a sustained spread lowers the session speed a notch. |
| `speed_frac_pace` | on | Followers trim their speed with the PID controller to track the leader's clock. |
| `pid_kp`, `pid_ki`, `pid_kd` | 0.05, 0.015, 0.02 | PID gains. |
| `pid_dead` | 0.30 | Dead band, game-time units. |
| `pid_min`, `pid_max` | 0.70, 1.20 | Clamp on the multiplier of the session speed. |
| `pid_slew` | 0.05 | Largest change of the multiplier per decision. |
| `hot_join` | on | A game far behind the session asks for command history and catches up. |
| `catchup_speed` | 4 | Speed used while catching up (clamped 1-4). |
| `strict_barrier` | off | Stop simulating while a command a peer announced is missing (released by a watchdog). |

### Replay details (the mod)

| key | default | effect |
|---|---|---|
| `stops_native` | on (read at load) | Replay stops with the engine's own edge-object proposal; `0` rebuilds the edge instead. |
| `stops_del_on_line` | on (read at load) | Allow replaying the removal of a stop that a line uses. |
| `xing_reheight` | off | Move an existing road node to the track's height at a crossing. Asserts the engine; leave off. |
| `conx_terrain_align` | on | Replay constructions with terrain alignment, as the UI does. |
| `company_colors` | on | In companies mode, paint a bought vehicle in its company's colour. |

### Diagnostics

| key | default | effect |
|---|---|---|
| `dumpprop` | 0 | Dump every construction and road proposal to `tpf2_slice.log` (verbose). |
| `dump_egeo` | off | Write every edge the detector hashes to `egeo_<letter>.txt` in the game folder. |
| `xing_debug` | off | Log every street edge the crossing scan considers. |
| `vpos_desync_m` | 10 | Vehicle drift, in metres, that counts as a desync. |
| `groundtruth` | off | Ground-truth sweep mode ([re/README.md](re/README.md#ground-truth-sweeps)). Do not place constructions while it is on. |
| `conparams_dump` | off | Walk and log construction parameters without cancelling anything. |
| `heapcheck` | off | Validate the process heap around proposal hooks (slow). |

## `tpf2_bridge_mp.cfg`

`key=value` starting in the first column, with no spaces around `=`; any other line is ignored. The
bridge also accepts the file names `tpf2_mp_mp.cfg`, `tpf2_mp_tpf2_bridge_mp.cfg` and `tpf2_mp.cfg`
(any of them in the game folder hides a copy in the data folder).

| key | shipped | effect |
|---|---|---|
| `local_port` | 7771 | The bridge's UDP port. With `instance=auto` the first game on a machine takes 7771 and the next 7772. |
| `peer_ip`, `peer_port` | 127.0.0.1, 7772 | Where the bridge sends when there is no lobby (two games on one machine). In a lobby session the menu points it at the lobby's loopback port instead. A 127.x address binds the socket to 127.0.0.1; any other address binds all interfaces, for a direct link to that machine. Packets from any other address are dropped either way. |
| `instance` | auto | `a`, `b`, or `auto` (decided by which port is free). The lobby assigns the real letter. |
| `save_server` | 0 | `1` starts the legacy TCP save server on instance `a` (the pre-lobby path). It answers `peer_ip` only. |
| `xfer_port` | 7871 | Port of the legacy TCP save server. |
| `auto_pull` | 0 | `1` pulls the host's save over `xfer_port` at start; the host needs `save_server=1`. |
| `sim_hook` | 1 | A per-step hook on `GameSim::Step` (a counter; nothing depends on it). |
| `speed_hook` | 1 | Fractional game speed: scales the sim batch interval to the value in `tpf2_speed.txt`. |
| `buy_hook` | 1 | A diagnostic probe on the buy-vehicle factory. Nothing reads its output, and the slice hooks the same function; `0` avoids the double hook. |
| `save_dir` | not shipped | The save folder the legacy save server serves from (default: discovered). |
| `share_save` | not shipped | The save the legacy server shares, by base name (default: the newest). |
| `tail_file` | not shipped | Send the lines of this file instead of `tpf2_capture_<L>.txt` (no spaces in the path). |
| `relay_out` | not shipped | Write outgoing lines to this file instead of sending them over UDP (no spaces in the path). |

## `tpf2_menu_flags.txt`

Not shipped; create it next to `tpf2_menu.dll` (the game folder). One `key=value` per line; keys are
case-sensitive and must not have spaces around them. Unknown keys are ignored.

| key | default | effect |
|---|---|---|
| `master_url` | `https://srv1306562.hstgr.cloud/tpf2mp` (the project's master server) | Base URL of the public game list. The panel reads `<url>/list`, and a host with PUBLIC ticked announces to it. Empty hides the list and the PUBLIC checkbox. |
| `relay_autosave_min` | 2 | How often, in minutes, a relay lobby's leader uploads a fresh save while playing; `0` never. |
| `automod` | on | `automod=0` stops the panel adding the Transport Fever 2 Multiplayer mod to the game's default mod list. |
| `native` | 1 | Insert the Multiplayer entry into the title menu. It is the only way to open the panel: `0` leaves no way in. |
| `slot` | 0 | Position of the Multiplayer entry in the title menu's list (0 = top). |
| `scale` | 0 | Panel scale; 0 = screen height / 1080. |
| `overlay`, `ox`, `oy`, `fontpx` | | Parsed but no longer used. |

## `tpf2mp.cfg` and plugin settings

The plugin host's file, not installed by the MSI. `[section]` headers (a plugin's section is its DLL
name without `.dll`), `key=value` lines with whitespace trimmed, keys case-insensitive; `#` or `;` only at
the start of a line starts a comment, so values may contain them. Booleans take `1/0`, `true/false`,
`yes/no`, `on/off`.

- The host itself reads one key: `enabled` in each plugin's section (default on; `enabled=0` skips loading
  that plugin).
- The base file is `tpf2mp.cfg` in the game folder, else in the data folder. Then, for each plugin found, a
  `.cfg` with the plugin's name next to its DLL (for example `plugins\tpf2_bigmap.cfg`) is merged over the
  settings; later files win, and a plugin's file may set any section. That is how a plugin shipped by
  another installer keeps its settings without editing a file this package owns.
- Plugins are loaded from `plugins\` in the data folder first, then from `plugins\` in the game folder; a
  DLL name found in the data folder hides the game folder's copy.
- Settings are read once, at game start.

## Environment

| variable | effect |
|---|---|
| `TPF2MP_DATADIR` | Use this folder as the data folder instead of `%LOCALAPPDATA%\tpf2mp\data\`. The mod follows it only if the folder already holds `tpf2_instance.txt` (the bridge writes it at start). The lobby folder and the menu log are unaffected. |
| `TPF2MP_LOG_IPS=1` | Log IP addresses unmasked (bridge and lobby). |

## Files the software writes for itself

Not settings, but useful when reading a session: `tpf2_bridge_ctl.txt` (the menu's instructions to the
bridge and the mod: letter, loopback port, player count, session speed request, sync, leader),
`tpf2_speed.txt` (the fractional speed target), `tpf2mp_dash.txt` (window visibility),
`mp_company_cfg.txt` (company assignment) and `tpf2_names.txt` (your player and lobby names), all in the
data folder. [ARCHITECTURE.md](ARCHITECTURE.md#files) lists every file.
