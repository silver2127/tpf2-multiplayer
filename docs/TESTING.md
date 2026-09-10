# Manual test plan

Checks to run by hand before pushing a commit that changes what players see or what travels
between games, and all of them before a release. Every test has an id to quote in a bug report.

## Before you start

- **Two games.** Most tests need two. One PC can run both: instance A normally, instance B in a
  Sandboxie box (`tools\mp_menu_launch.ps1 -Players 2`; see
  [DEVELOPMENT.md](DEVELOPMENT.md#running-several-instances)). The connection (C) and relay (R)
  tests need a second PC on another network.
- **The build under test everywhere.** Install it on every PC (or `tools\deploy_shipping.ps1`;
  `tools\deploy_mod.ps1` for a Lua-only change). Every instance on one PC needs the same bridge build.
- **A test save**: mid-sized, the mod enabled, towns, some road and rail, a few lines. Test on a copy.
- **A pass**, unless a test says otherwise: the Multiplayer window's verdict stays `SYNC` for
  every player, co-op balances match, no new minidump, and no `!! DESYNC`, `DIVERGENCE` or
  `success=false` in the game's stdout ([Where to look](#where-to-look)).
- **Known issues are not regressions.** Check [KNOWN_ISSUES.md](KNOWN_ISSUES.md) before reporting.
- **Snapshot the logs before restarting a game** (`tools\snapshot_logs.ps1`); the game truncates
  them on launch.

Time: the smoke test takes about 15 minutes; everything, about three hours.

## Automated checks first

- [ ] `python tools\luacheck.py` passes (every Lua file parses).
- [ ] `native\build.bat all` builds.
- [ ] `powershell -File tools\pscheck.ps1`, if a PowerShell script changed.
- [ ] The lobby self-tests, if `netpunch/` changed ([NETWORKING.md](NETWORKING.md#self-tests)), and
  `python tools\relay_selftest.py` for the relay.
- [ ] `installer\test_upgrade.ps1`, if the installer changed.
- [ ] During a manual session, with every game still in it:
  `powershell -File tools\soak.ps1 -AssertOnly -Players 2` exits 0 (checks A1-A13 from the logs).

## Which tests a commit needs

| the commit changes | run |
|---|---|
| anything | S |
| `native/src/slice_hook.cpp`, `mod/.../mp/*.lua` replication code | S, B, V, M1-M3 |
| `mod/.../mp/pacing.lua`, `native/src/speedhook.cpp` | S, T, H |
| `native/src/net.cpp`, `bridge_main.cpp`, `savexfer.cpp` | S on one PC and on two, C, X |
| `native/src/menu_hook.cpp` | S, P, L, H |
| `netpunch/lobby.py`, `seal.py`, `mesh.py`, `connect.py` | S, C, L, H, R |
| `netpunch/masterserver.py`, `tools/*_deploy.sh` | P5-P9, R1 |
| `mod/.../mp/companies.lua` | M4-M8 |
| `installer/` | I |
| a release | everything, then E |

## S: smoke test

- [ ] **S1** Both games start to the title menu, which has a **Multiplayer** entry.
- [ ] **S2** A: Multiplayer → HOST GAME. The lobby page opens and the code is on the clipboard.
- [ ] **S3** B: Multiplayer → click the code field → JOIN GAME. Both panels list both players within a
  few seconds, and a chat line reaches each side.
- [ ] **S4** A: START GAME. Both panels say the save is ready; LOAD GAME → **mp_shared** loads on both.
- [ ] **S5** The Multiplayer window shows both players and `SYNC`.
- [ ] **S6** A builds a road; B builds another joined to it. Each appears on the other map and the
  verdict stays `SYNC`.
- [ ] **S7** A builds five short roads quickly. The road tool keeps working and all five reach B.
- [ ] **S8** A places a road depot and buys a bus in it. Both appear on B.
- [ ] **S9** A at 1x, B presses 4x: both run at 1x. A pauses: both pause. Both unpause: both run.
- [ ] **S10** Run the soak check above; it exits 0.
- [ ] **S11** Both return to the title menu and leave the lobby. No crash, no minidump.

## I: install, upgrade, uninstall

- [ ] **I1** Fresh install: the game folder has `alut.dll`, `alut_real.dll`, `tpf2_bridge_mp.dll`,
  `tpf2_menu.dll`, `tpf2_slice.dll`, `tpf2_pluginhost.dll`, the `.cfg` files,
  `netpunch\netpunch.exe` and `mods\mp_lockstep_1\`; the game starts with sound and a Multiplayer entry.
- [ ] **I2** The game's mod list shows **Transport Fever 2 Multiplayer**; a new game has it enabled.
- [ ] **I3** Upgrading over the previous release succeeds, the game still works, and the game-folder
  `.cfg` files are the new ones.
- [ ] **I4** Delete `tpf2_menu.dll`, run the installer's Repair: it comes back.
- [ ] **I5** Uninstall: the game's own `alut.dll` is back, the multiplayer files are gone, and the game
  starts with sound and no Multiplayer entry.
- [ ] **I6** With TpF2 Big Maps installed too, install and uninstall this package in either order:
  Big Maps keeps working.

## P: title menu, panel, public list

- [ ] **P1** The panel opens and closes; while it is open, typing goes into its fields, not the game.
- [ ] **P2** YOUR NAME and the lobby name survive a restart.
- [ ] **P3** Clicking the code field pastes the clipboard, trailing spaces trimmed.
- [ ] **P4** The PASSWORD field is masked.
- [ ] **P5** PUBLIC GAMES fills within about 10 s (no `Server browser unavailable`), with HOST, TYPE,
  PLAYERS, VERSION and SEEN columns. REFRESH reloads it.
- [ ] **P6** Host with PUBLIC ticked: another PC lists the game as **player hosted** within 20 s;
  clicking the row fills the code and JOIN GAME works.
- [ ] **P7** Host with a password and PUBLIC ticked: the row shows `[locked]`; joining needs the password.
- [ ] **P8** The host leaves: the row goes (within 30 s at most).
- [ ] **P9** The project's dedicated server is listed as **dedicated server**.
- [ ] **P10** `master_url=` (empty) in `tpf2_menu_flags.txt` hides the list and the PUBLIC checkbox.

## C: connecting

- [ ] **C1** Host on a router with UPnP: a player on another network joins.
- [ ] **C2** UPnP off, UDP 29471 forwarded to the host: joining works.
- [ ] **C3** UPnP off, nothing forwarded: the joiner gets `could not reach host`, not a hang.
- [ ] **C4** Two PCs on one LAN: joining works.
- [ ] **C5** Locked code: a wrong password gives `wrong password for this code`; the right one joins.
- [ ] **C6** A code from a lobby that has closed gives `could not reach host`.
- [ ] **C7** Three or four players on different networks: everyone sees everyone, chat reaches all,
  and in game everyone shows `SYNC`.
- [ ] **C8** A joiner closes their game mid-session: the others keep playing in `SYNC`.
- [ ] **C9** The host closes their game: joiners see `host closed the lobby` or `host unreachable`.
- [ ] **C10** Windows asks about `netpunch.exe` once; after allowing it, hosting works.

## L: lobby, save sharing, loading

- [ ] **L1** START GAME with nobody else in the lobby: `no players to share with ...`.
- [ ] **L2** START GAME shares the newest save in the save folder, autosaves included: make an
  autosave newer than your last save and check that is the world everyone loads.
- [ ] **L3** A save made without the mod: START GAME refuses (`Not shared: '<save>' does not have the
  Transport Fever 2 Multiplayer mod enabled`) and the chat explains. Enable the mod in that save's
  Mods panel, save, START GAME again: it is shared.
- [ ] **L4** A large save (500 MB or more): progress shows, the transfer completes, it loads everywhere.
- [ ] **L5** A player who joins during a transfer is served when it finishes.
- [ ] **L6** Each game holds at the start until the host's game is running, then all play in step.
- [ ] **L7** After a session, everyone returns to the title menu and leaves; the host hosts again and
  START GAME works without restarting any game.

## B: building

Do each on A and check B at the same moment, then repeat from B. The shape must match and co-op
balances must stay equal.

- [ ] **B1** A straight and a curved road; a road joining two existing roads (a new junction).
- [ ] **B2** Road upgrades: street type, bus lane, tram track, electric tram track.
- [ ] **B3** Rail track, catenary, a bridge, a tunnel.
- [ ] **B4** Rail across a road (a level crossing); rail across rail.
- [ ] **B5** Bulldoze a stretch of road and of track.
- [ ] **B6** A road depot in the middle of a road, and one on a junction.
- [ ] **B7** A bus or truck station placed over town buildings: the same buildings go on both maps.
- [ ] **B8** A rail station: place it, add and remove platform modules, upgrade it.
- [ ] **B9** Bulldoze a depot and a station.
- [ ] **B10** A roadside stop, a signal each way, a waypoint; then remove each.
- [ ] **B11** Both players build at the same time for a minute.
- [ ] **B12** With `dump_egeo=1` in `tpf2_slice.cfg`: after B1-B11, `egeo_a.txt` and `egeo_b.txt` in
  the game folder are identical apart from the first line.

## V: vehicles and lines

- [ ] **V1** Buy a bus, a truck, a tram and a train with several wagons: each appears on B in the same
  depot with the same configuration.
- [ ] **V2** Buy five vehicles as fast as you can click.
- [ ] **V3** Create a line with three or more stops and assign vehicles: they run the same route on both.
- [ ] **V4** Add, remove and reorder a line's stops; delete a line.
- [ ] **V5** Send a vehicle to a depot; reverse a vehicle, including one on its way to a depot; sell one.
- [ ] **V6** Replace a vehicle with a newer model.
- [ ] **V7** Rename a vehicle and change its colour.
- [ ] **V8** Ten minutes at 4x: the stats' vehicle drift stays under 10 m and the verdict `SYNC`.

Not replicated, so not tested: vehicle stop/start, manual departure, maintenance settings.

## M: money and companies

- [ ] **M1** Co-op: after the B and V tests, the stats show the same balance on both.
- [ ] **M2** Take a loan and repay it: balance and loan match.
- [ ] **M3** Ten minutes at 4x: balances still match.
- [ ] **M4** Give A and B different company chips before START GAME. Each one's buildings show as the
  other company's on the other map and cannot be bulldozed by the other player.
- [ ] **M5** Each company's balance changes only with its own spending and income.
- [ ] **M6** A bought vehicle has its company's colour on both maps.
- [ ] **M7** In the window's companies section: switch to another company, create a new one, set a
  password on yours; switching into a locked company asks for the password.
- [ ] **M8** Two players with the same chip number share one company.

## T: time and speed

- [ ] **T1** The lowest speed button wins: A 4x and B 2x run at 2x; B to 4x, both run at 4x.
- [ ] **T2** Either player pausing pauses both; both unpausing resumes.
- [ ] **T3** `/speed 2.5` runs everyone at 2.5x; **-0.5** and **+0.5** change it; `/speed off` or
  **reset** returns to the speed buttons.
- [ ] **T4** In steady play a follower's speed row says `in step with the leader`.
- [ ] **T5** After a pause and unpause, the stats' skew is close to 0.
- [ ] **T6** A much slower PC, or a heavier save on one side: that game catches up (`catching up`)
  instead of falling further behind.

## H: joining a running game

- [ ] **H1** A and B are playing; C joins the lobby. The host's game saves, C loads mp_shared, catches
  up and shows `SYNC` (see KNOWN_ISSUES.md: catch-up does not start if C is too far behind).
- [ ] **H2** `/sync` during play: players already in the game carry on; a player waiting in the lobby
  loads the fresh save.
- [ ] **H3** Players in the game can keep playing while a joiner's transfer runs.

## R: dedicated relay

- [ ] **R1** The relay row joins with no port forwarded on any PC.
- [ ] **R2** The first player is the leader: their START GAME shares a save through the relay, a
  second player loads it, `SYNC`.
- [ ] **R3** With a world stored on the relay: the leader joins and after 10 s everyone continues it.
- [ ] **R4** `/new` within those 10 s discards it; the leader's START GAME shares their own save.
- [ ] **R5** While the leader plays, a fresh save reaches the relay every 2 minutes; a player joining
  late gets a world less than 3 minutes old.
- [ ] **R6** When the leader leaves, the next player becomes leader.
- [ ] **R7** `/resume` sends the stored world to everyone waiting.

## X: security

- [ ] **X1** In a lobby session, on each PC: the bridge's socket is on 127.0.0.1 only. The port is the
  `port=` line of `%LOCALAPPDATA%\tpf2mp\data\tpf2_instance.txt`, then
  `Get-NetUDPEndpoint -LocalPort <port> | Select-Object LocalAddress` shows `127.0.0.1`, never `0.0.0.0`.
- [ ] **X2** `Get-NetTCPConnection -State Listen -LocalPort 7871 -ErrorAction SilentlyContinue` shows
  nothing, and the host's `tpf2_bridge.log` says `save server off`.
- [ ] **X3** Two, three and four games on one PC each get their own port and replicate (S3-S6).
- [ ] **X4** The `[simhook]` lines in `tpf2_bridge.log` show `strangers=0` in a normal session.
- [ ] **X5** B8 passes: construction parameters from other players still load.

## D: player-facing tools

- [ ] **D1** Ctrl+Shift+D hides and shows the Multiplayer window; its stats, chat and companies
  buttons work.
- [ ] **D2** `tools\collect_logs.cmd` puts `tpf2mp-logs-<computer>-<time>.zip` in Downloads, holding the
  data folder, the game's stdout and the lobby logs.

## E: endurance, before a release

- [ ] **E1** Two players, an hour at 4x with normal building, buying and line work: `SYNC` throughout,
  no minidump, and the soak check exits 0 at the end.
- [ ] **E2** Mid-session, the host saves; everyone leaves; the host hosts again from that save: the
  world matches on every player.

## Where to look

| symptom | where |
|---|---|
| `DESYNC ...` | the Multiplayer window names what differs; `!! DESYNC` in stdout |
| an action missing on the other side | stdout `EXEC <op> ... success=`; `tpf2_slice.log`: `CANCEL`, `stays local`, `UNREPLICATED` |
| late or lost commands | stdout `!! LATE`, `NACK`, `RESEND` |
| clocks apart | the stats' skew; stdout `CATCHUP:`, `SPEED2:`, `LOADGATE:` |
| connection trouble | the panel's status line; `lobby_proc.log`; `tpf2_menu.log` |
| a crash | a new `.dmp` in the game's `crash_dump` folder |

Log locations: [DEVELOPMENT.md](DEVELOPMENT.md#logs).

## Reporting a failure

1. Note the test id, the commit (`git rev-parse --short HEAD`) and what each player did.
2. Before restarting anything, `tools\snapshot_logs.ps1 -Tag <test id>` on the rig, or
   `tools\collect_logs.cmd` on each player's PC.
3. Check [KNOWN_ISSUES.md](KNOWN_ISSUES.md) first.
