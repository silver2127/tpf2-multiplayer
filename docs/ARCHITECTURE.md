# Architecture

Transport Fever 2 has no multiplayer code. This project adds **lockstep** multiplayer from
the outside: every player runs the same simulation from the same save, and the only thing that
travels is player commands, each applied at the same simulation step on every machine. The
game's simulation is deterministic enough for that (two instances fed the same inputs stay
identical; see [re/GAME_LOOP_AND_UI.md](re/GAME_LOOP_AND_UI.md#threads-and-state)), and a
detector checks it continuously while people play.

## Components

```
TransportFever2.exe
  alut.dll ............... forwarding proxy for the game's own alut_real.dll; loads the four DLLs below
  tpf2_bridge_mp.dll ..... identity, command lines to and from the lobby, sim-loop hooks   <-> tpf2_capture/events_<L>.txt
  tpf2_menu.dll .......... Multiplayer panel, lobby launcher, save placement, forced saves   <-> netpunch lobby_*.jsonl
  tpf2_slice.dll ......... captures the player's commands and cancels them                  -> lockstep_inject_<L>.txt
  tpf2_pluginhost.dll .... loads plugins (none in this repository; TpF2 Big Maps is one)
  mods/mp_lockstep_1 ..... Lua game script: stamps, ships, replays, paces, detects divergence
netpunch.exe ............. the lobby (separate process): NAT traversal, sealed transport, save transfer
```

| component | source | job |
|---|---|---|
| proxy | `native/src/proxy_alut.cpp` | The game imports `alut.dll` statically, so the proxy is loaded at process start. It forwards all 20 exports to `alut_real.dll` and, from a thread started in `DllMain`, loads the bridge, menu, slice and plugin-host DLLs, each from `%LOCALAPPDATA%\tpf2mp\` if present, else from its own folder. |
| bridge | `native/src/bridge_main.cpp`, `net.cpp`, `speedhook.cpp` | Elects and records the instance identity (`tpf2_instance.txt`), tails the mod's capture file and sends each line over its UDP link, writes received lines to the events file, follows `tpf2_bridge_ctl.txt`, and hooks the sim loop for fractional speed. It has no settings file. The UDP socket is bound to 127.0.0.1 in lobby sessions and drops datagrams that are not from its peer. |
| slice | `native/src/slice_hook.cpp` | Hooks the `make_cmd::*` factories and `CommandList::Add`. Decodes a player's command, writes it to the inject file, and cancels the native command when a session is live. Welds the template connector into replayed construction proposals. Checks the exe's build before patching; only one instance per Windows session gets it (a named mutex). |
| menu | `native/src/menu_hook.cpp` | Adds a Multiplayer entry to the title menu's list and draws the panel into the game's Vulkan frames (GDI-rendered, composited over a blurred copy of the frame). Runs `netpunch.exe` in a job object, tails its events, writes `tpf2_bridge_ctl.txt` and `mp_company_cfg.txt`, places the shared save, forces the game's autosave for hot joins and relay uploads, fetches the public list, and adds the mod to new games' mod list. |
| plugin host | `native/src/plugin/` | Loads `*.dll` from `plugins\` in the data folder, then in the game folder, and calls their `Tpf2mpPluginInit` export (ABI 1). The multiplayer DLLs are not plugins. |
| mod | `mod/mp_lockstep_1` | `lockstep.lua` plus modules in `res/scripts/mp/`: turns captures into position-based commands, stamps and ships them, replays everyone's commands through the game-script API, keeps the clocks together, detects divergence, runs companies mode and the in-game Multiplayer window. |
| lobby | `netpunch/` | See [NETWORKING.md](NETWORKING.md). |

## A player's action, end to end

Building a road, with other players connected:

1. **Capture.** The street tool calls `make_cmd::BuildProposal`. The slice's factory hook reads
   the proposal (nodes, edges, tangents, types, removed edges), appends `ARMED 1` and a `ROADE`
   line to `lockstep_inject_<letter>.txt`, and remembers the Command's address.
2. **Cancel.** The tool then calls `CommandList::Add` with that Command. The slice's Add hook
   recognises it, fires the tool's completion callback (the tool waits for it), zeroes Add's
   output handle and skips the call. The road is now built nowhere.
3. **Stamp and ship.** The mod reads the inject file every tick. While this game still has the
   entities, it converts ids to positions (`ROADP`), then `CM.scheduleLocal` gives the command
   a sequence number and a stamp: now + `EXEC_DELAY` (0.6) + how far the fastest other player's
   clock is ahead (at most 15), rounded up to the 0.2 sim-step grid. It queues the command
   locally and appends `LSCMD op=ROADP at=<stamp> origin=<letter> seq=<n> ... params=...` to
   `tpf2_capture_<letter>.txt`.
4. **Transport.** The bridge sends the line to its lobby process over loopback UDP; the lobbies
   carry it to every other player; each receiving bridge appends it to
   `tpf2_events_<letter>.txt`. Between a bridge and its lobby a line travels in the bridge's own
   framing: chunks of up to 1,023 bytes in 1,050-byte UDP packets.
5. **Receive.** Each mod reads its events file every tick, tracks per-sender sequence gaps, and
   queues the command.
6. **Apply.** Every tick each instance sorts its queue by (stamp, sender letter, sequence) and
   applies each command whose stamp step has been reached, the sender's own copy included:
   `execPolyline` resolves the positions against its own world and submits the road through
   `api.cmd.make.buildProposal` / `api.cmd.sendCommand`.
7. **Check.** At common stamps every instance hashes the world and compares with the others
   ([REPLICATION.md](REPLICATION.md#detecting-divergence)).

Other actions follow the same shape with different capture points and replay code;
[REPLICATION.md](REPLICATION.md) lists them, including those that are not cancelled.

## Time

- **The clock is game time**, `game.interface.getGameTime().time`: it is part of the simulation
  and loaded from the same save. It advances 0.2 units per sim step.
- **Stamps are on the step grid, and commands apply by step, not by frame.** `update()` runs
  once per rendered frame, so "the first update after the stamp" is a different sim step on
  different machines. The queue compares `CM.stepOf(stamp)` with `CM.stepOf(now)`.
- **Derived targets come from agreed values only.** When a command must wait (purchases apply
  one per step; commands right after a purchase wait `BIND_GUARD_STEPS` = 10 steps; line edits
  wait 5 steps for a line created in the same batch; retries add 5 steps), the target is
  computed from stamps, which are identical everywhere, and stored as `notBeforeStep`. The
  stamp itself is never rewritten.
- **`EXEC_DELAY` must exceed the actual skew between clocks**, not a latency budget. Stamping
  past the fastest clock means a slow machine pays latency rather than forking. A command that
  arrives after its stamp is logged as `!! LATE`.

## Reliability

Game frames are best-effort in the lobby layer. Above it:

- **The bridge link** (`net.cpp`) numbers its packets, acknowledges them with a 32-bit bitmap,
  resends unacknowledged packets every 250 ms, sends a keepalive every 500 ms, and reassembles
  chunked lines in order. It tracks one remote sender at a time, though, and with three or more
  players one bridge socket hears several senders through the lobby, so its ordering and resends
  cannot be relied on there. The mod's own layer is what recovers lost commands.
- **Per-sender sequence numbers in the mod.** Every sender keeps its last 256 lines. A receiver
  that sees a gap in a sender's sequence sends `LSNACK` after 15 ticks and again every 30 ticks
  (up to 10 times); the heartbeat's `hi=` field exposes a lost last command. The leader also
  answers for other senders from its history ring (4,096 lines).
- **Duplicates are harmless:** a command's `at|origin|seq` is remembered (2,048 entries).

## Pacing and game speed

- **The leader is the session clock.** It is the host (letter `a`), or in a relay lobby the
  player the relay names. Its player's speed buttons set the **session speed**, or `/speed x` when
  that is newer, and it broadcasts the result (`LSEFF`). While a session is live the slice cancels
  every click on a game's speed buttons and pause toggle (`UI::Clock`) and writes `SPEEDBTN <v>` to
  the inject file; the leader's mod takes it as the session speed and a follower's ignores it, so
  a lever only moves through pacing. Speed 0 is a sync point: games behind the leader run until
  they reach its clock, then stop.
- **Followers trim their own speed** around the session speed to track the leader's clock, with
  a PID controller (fixed gains, one decision every 8 ticks, 0.7-1.2x, slew limited). A follower
  more than 3 units ahead of the leader drops to a quarter of the session speed, rising as the gap
  closes. Fractional speeds are written to `tpf2_speed.txt`; the bridge's speed hook scales
  the engine's sim batch interval to match, so every iteration stays an ordinary sim step and
  the game's speed buttons never flip.
- **Load gate.** After loading, a follower holds at speed 0 until it hears the leader (or the
  expected number of players), so nobody starts ahead. The leader never waits. The gate gives up
  after 900 ticks.
- **Catch-up.** A follower more than 8 units behind the leader asks it for the command history
  after its clock (`LSNEED`), holds until the history is complete, then runs at up to 4x until it
  is within half a unit. Meanwhile its heartbeat carries `cu=1` so nobody paces against it. The
  leader never catches up: it is the clock. See [KNOWN_ISSUES.md](KNOWN_ISSUES.md#lockstep-and-pacing)
  for pacing rules that do not always behave as described.

## Sessions

- **Identity.** Each instance has a letter: the host is `a`, joiners are `b`, `c`, ... in roster
  order, and a relay assigns letters that stick to player names. The menu DLL writes it (with the
  loopback relay port, the player count and the leader) into `tpf2_bridge_ctl.txt`; the bridge
  records it in `tpf2_instance.txt`, which the slice and the mod read. The letter prefixes every
  command, orders ties, and namespaces placeholder ids and file names.
- **Starting.** START GAME shares the host's newest save; each joiner's menu DLL places it as
  `mp_shared.sav` and the player loads it. Only a live session cancels anything: the slice
  checks that the mod's status file is fresh and names a peer.
- **Hot join.** When a player joins a running game, the host's menu DLL forces an autosave and
  shares it like START GAME; the newcomer loads it and catches up from the leader's command
  history. See [NETWORKING.md](NETWORKING.md#hot-join).
- **Dedicated relay.** The same flow with a server in the middle that holds the latest world; see
  [NETWORKING.md](NETWORKING.md#dedicated-relay).

## Companies mode

Players can play one shared company (co-op, the default) or separate companies on the same map.

- **Assignment.** Each player picks a company chip in the lobby. At START the menu DLL writes
  `mp_company_cfg.txt`: the mode (`companies` when more than one chip is in use), this player's
  company, the roster, and the letter-to-company map. The map is authoritative: an incoming
  command's company is taken from it, not from the sender.
- **Engine players.** Each other company is a real engine player (`game.interface.addPlayer`) with
  its own wallet. Commands carry their company; builds are reassigned to the company's player
  (`setPlayer`, and locked against other companies' bulldozers), their cost is moved with journal
  entries, vehicles are bought as the company's player, new lines and loans are attributed to it.
- **In game.** The Multiplayer window's companies block can create a company, switch to another
  (with an optional password, compared as a salted hash), and set a password. `CMNEW`, `CMSWITCH`,
  `CMPW` and `CMDEL` apply at the stamp on every instance. Switching swaps the owned entities and
  wallets between the human player and the company's engine player on your machine.
- **Saved state.** The mode, roster, letter map, passwords and engine player ids ride in the save
  (the mod's `save()`/`load()`), so a reloaded session keeps its companies.

## Files

All runtime files are in the data folder, `%LOCALAPPDATA%\tpf2mp\data\` (the environment variable
`TPF2MP_DATADIR` overrides it), unless noted. `<L>` is the instance letter.

| file | written by | read by | content |
|---|---|---|---|
| `tpf2_instance.txt` | bridge | slice, mod, menu | this instance's letter (line 1), process id, bridge port |
| `tpf2_bridge_ctl.txt` | menu | bridge, mod (leader) | `instance=`, `peer=`, `pid=`, `players=`, `speed=`, `sync=`, `xfer=`, `leader=` |
| `lockstep_inject_<L>.txt` | slice; the mod's window (company actions) | mod | captured commands and speed-button clicks |
| `tpf2_capture_<L>.txt` | mod | bridge | outgoing wire lines |
| `tpf2_events_<L>.txt` | bridge | mod | incoming wire lines |
| `lockstep_status_<L>.txt` | mod, every 15 ticks | slice | liveness and the peer's time |
| `lockstep_dash_<L>.txt` | mod | mod's window | dashboard values |
| `tpf2mp_dash.txt` | menu (Ctrl+Shift+D) | mod's window | show or hide the window |
| `tpf2_speed.txt` | mod | bridge | fractional speed target |
| `tpf2_sync_save.txt`, `tpf2_sync_sent.txt` | mod / menu | menu / mod | the hot-join save handshake |
| `mp_company_cfg.txt` | menu, at START | mod | company assignment |
| `mp_company_<L>.log` | mod | people | companies, crossing and plan decisions |
| `tpf2_bridge.log` | bridge | people; forwarded to the host's merged lobby log | connection events and every line sent and received (first 200 characters) |
| `tpf2_slice.log` | slice | people | captures, decodes, cancels |
| `tpf2_names.txt` | menu | menu | the player and lobby names typed in the panel |
| `tpf2_proxy.log`, `tpf2mp_host.log` | proxy, plugin host | people | which DLLs and plugins loaded from where |
| `tpf2_menu.log` | menu, next to its DLL (the game folder) | people | panel and lobby launcher |
| `egeo_<L>.txt` | mod, in the game folder, with `dump_egeo=1` | people | every edge, for diffing |
| `lobby_*.jsonl`, `lobby_state.json`, `lobby_peers.log`, `lobby_proc.log`, `incoming_save.*` | lobby / menu, in the lobby folder | | see [NETWORKING.md](NETWORKING.md#the-lobby-and-the-game) |

The mod's own log lines (`[ls-<L>] ...`) go to the game's stdout,
`<Steam>\userdata\<steamid>\1066780\local\crash_dump\stdout.txt`, which the game truncates at
launch and writes out when it exits.
