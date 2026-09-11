# Configuration

Multiplayer needs no settings. Every replication channel and the pacing are built in, so a
missing, old or damaged settings file cannot switch part of the protocol off, or put one player on
a different protocol from the others. The files below hold diagnostics, one latency setting, panel
preferences and plugin settings.

## Files and where they are read from

| file | read by | where | changes take effect |
|---|---|---|---|
| `tpf2_slice.cfg` | slice DLL (`dumpprop`), the mod (`dump_egeo`, `exec_delay`) | the game folder (for the slice, next to `tpf2_slice.dll`), else the data folder | `dumpprop` within 2 s, `dump_egeo` within about 5 s, `exec_delay` when a game loads |
| `tpf2_menu_flags.txt` | menu DLL | next to `tpf2_menu.dll` (the game folder) only | at game start |
| `tpf2mp.cfg` | plugin host | the game folder, else the data folder | at game start |

- The bridge DLL has no settings file; a `tpf2_bridge_mp.cfg` left from an older version is ignored.
- The data folder is `%LOCALAPPDATA%\tpf2mp\data\` ([Environment](#environment)).
- **The first file found wins; files are never merged.** A copy in the data folder is ignored while
  the game folder has one, and the installer puts the game-folder `tpf2_slice.cfg` back on every
  upgrade and Repair ([installer/README.md](../installer/README.md#upgrading)).

## `tpf2_slice.cfg`

The installer's copy has every key commented out.

| key | read by | default | effect |
|---|---|---|---|
| `dumpprop` | slice | 0 | `1` dumps every construction and road proposal to `tpf2_slice.log` (verbose): for a replay the engine rejects without a message. |
| `dump_egeo` | mod | off | `1` writes every edge the desync hash sees to `egeo_<letter>.txt` in the game folder, to diff two instances after a divergence (ignore the first line, a per-instance stamp). |
| `exec_delay` | mod | 0.4 | How far ahead every command is stamped, in game-time units, snapped up to the 0.2 step grid: the latency of every action. Accepted 0.2-5; anything else, or no value, means 0.4 (0.6 before 2026-09-11). Every received command logs `spare=`, the game time it had left before its stamp; raise this if internet sessions show spare near or below 0 (`!! LATE`). Below 0.4 a jitter spike puts a command in a peer's past. Set the same value on every machine. |

The two readers parse differently:

- **Slice:** a line that is exactly `dumpprop=1` or `dumpprop=0` from its first character, optionally
  followed by blanks. Any other line is ignored, and the last valid line wins.
- **Mod:** `key=value`, with blanks allowed before the key and around the `=`. `dump_egeo` is on for
  `1`, `true` or `yes`.
- A line starting with `#` matches neither, so it works as a comment.

## `tpf2_menu_flags.txt`

Not shipped; create it next to `tpf2_menu.dll`. One `key=value` per line, keys case-sensitive, no
blanks before the `=`. Unknown keys are ignored, and a value that fails its check leaves that
setting at its default.

| key | default | accepted | effect |
|---|---|---|---|
| `master_url` | `https://srv1306562.hstgr.cloud/tpf2mp` (the project's master server) | empty, or an `http://` or `https://` URL without blanks or quotes (a trailing `/` is dropped) | Base URL of the public game list. The panel reads `<url>/list`, and a host with PUBLIC ticked announces to it. Empty hides the list and the PUBLIC checkbox. |
| `relay_autosave_min` | 2 | 0-60 | How often, in minutes, a relay lobby's leader uploads a fresh save while playing; `0` never. |
| `autoload` | 1 | `0`, `1` | `1`: the shared save loads by itself after START GAME. `0`: the player loads it with LOAD GAME. |
| `share_mods` | `ask` | `ask`, `always`, `never` | Mods the shared save needs and you lack: `ask` shows a YES / NO in the panel when the host presses START GAME (no answer in 90 s counts as no), `always` downloads without asking, `never` declines and, when hosting, does not offer your mods to joiners. |
| `slot` | 0 | 0-7 | Position of the Multiplayer entry in the title menu's list (0 = top). |
| `scale` | 0 | 0.5-3 | Panel scale; 0 = screen height / 1080. |
| `automod` | on | a line starting `automod=0` | Stops the panel adding the Transport Fever 2 Multiplayer mod to the game's default mod list. |

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
