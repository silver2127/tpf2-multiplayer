# Dedicated server

A dedicated server is a real copy of Transport Fever 2 running the multiplayer mod
with `dedicated=1` in `tpf2_menu_flags.txt`: it hosts a lobby the moment its title
menu is up, loads a world by itself, keeps the game's own autosave going, and hosts
again after a crash. Players join it like any player-hosted game -- it is listed as a
**dedicated server** in PUBLIC GAMES, its join code stays the same across restarts --
and it stands still while nobody is in.

It is not a stripped-down simulation. The game insists on a running, logged-in Steam
client (`SteamAPI_Init` failing throws `"SteamAPI_Init failed"` at start; there is no
offline path in the binary), so the server box runs a Steam client too: logged in once,
then kept in **offline mode**, which lets the same account play online elsewhere.

## What the mode does (menu DLL, `DedicatedTick`, once a second)

| state | action |
|---|---|
| no lobby | host one: `dedicated_lobby` as the name, `dedicated_name` as the player, `dedicated_password`, public per `dedicated_public`, SEPARATE COMPANIES per `dedicated_companies`; `--dedicated` keeps the session secret in `relay_secret.bin` so the code is stable |
| lobby up, no world | load `dedicated_save` (a save name), else the newest save in the save folder, through the shared-save autoload; the lobby counts the host as started, so a joiner comes in through the frozen-join round |
| world up | force the game's own autosave every `dedicated_autosave_min` minutes (0 = never); after a crash the next launch loads the newest save. With players in, the session is paused first (a chat line says so), the save runs, and the session resumes at the players' votes: a save of a large world takes the server ten seconds, during which it would otherwise stand still while everyone else ran on |

A crash to the title menu leaves the lobby (as for any player) and the first row hosts
again. The mod's half (`mp/pacing.lua`, told through `mp_dedicated.txt`): with
`dedicated_empty_speed=N` the world runs at N (1x by default, 0 = paused) when the
last other player leaves, and resumes at the players' votes when one arrives.

**Speed.** Nobody stands at the server's controls, so the server has no vote and
its own lever is never read as a player's pause, and a speed-button command
from its clock is not a vote either: the engine lowers the highest speed the
clock offers when it thinks the simulation cannot keep up, and the clock then
re-sends that lower speed through the same path as a player's click (a server
set to 4x voted 1x this way on 2026-09-21). The players' speed votes (the
speed buttons and the multiplayer window's speed row) set the session speed, as
in any session; with players in and no vote cast yet the world runs at 1x. A
frozen join or a resync resumes at the votes' speed (else the speed the server
paused from, never 0). Nobody can pause a dedicated server's session -- a
player's pause press does nothing there, as it does for any joiner.

**What the server sustains.** The engine hands its simulation one batch per
200 ms and stretches that interval when a batch of `speed` iterations costs
more; the lever still reads 4 while the world runs at 2 and joiners run ahead.
The bridge publishes that interval and the leader's pacing caps the session
speed at what the host actually keeps up with (`SPEED2: the host keeps up
with 2x ...` in its log), trying one step up after an unstretched minute. This
applies to any host, not only a dedicated one; `tpf2mp_governor_off.txt`
disables it with the governor.

Flags (all in `tpf2_menu_flags.txt` next to `tpf2_menu.dll`; see
[CONFIGURATION.md](CONFIGURATION.md)):

```
dedicated=1
dedicated_save=mp_multi_company      # a save NAME in the save folder; empty = the newest save
dedicated_lobby=My Server
dedicated_name=Server
dedicated_password=                  # empty = open
dedicated_public=1
dedicated_companies=1                # each player their own company
dedicated_autosave_min=10
dedicated_empty_speed=1              # while nobody else is in; 0 = paused
```

## A server on Linux (the Windows game under Proton)

The shipped Proton path is the server path: the Linux Steam client runs the Windows
game under Proton, and `install_proton.sh` installs the Windows mod into it. Nothing
here needs a GPU: Xvfb provides the display and Mesa's lavapipe the Vulkan device;
Wine adds nothing to the simulation's cost (native x86 code), only the render goes to
the CPU, at 640x480 and the lowest settings.

`tools/server/` holds the pieces; the README there is the runbook:

1. `setup_vps.sh` -- packages (Steam client, Xvfb, xdotool, 32-bit libraries), a
   `tpf2server` user, the systemd units (`tpf2mp-xvfb`, `tpf2mp-steam`,
   `tpf2mp-game`), the `tpf2server` command.
2. `steam_login.sh` -- the one interactive step: the Steam client logs in on the
   virtual display (password and Steam Guard code typed by the operator over ssh,
   never stored), then is switched to offline mode.
3. The game and Proton are installed through the client; `install_proton.sh` from the
   release installs the mod; `tpf2server configure` writes the flags and a
   headless-sized `settings.lua`.
4. `systemctl start tpf2mp-game` -- the watchdog asks Steam to launch the game and
   relaunches it when it exits.

## Where a server's time goes (measured 2026-09-22, native Linux build)

The test server (8 vCPU EPYC under KVM, 31 GiB, 13% steal) with a 49,000-tile Big
Maps world and two players in, profiled read-only with `perf -t` and `bpftrace`
while the session ran. Numbers for the native Linux build; under Proton only the allocator differs
(see `native/src/wine_heap.h`).

- **The simulation thread, a third of it, parses `/proc/self/maps`.** The mod's
  `Readable()` pointer guard is one `VirtualQuery` on Windows; the port's version
  reads and `sscanf`s the whole mapping table, and the process has 50,000
  mappings. 5.6 parses a second, ~57 ms each. This is the server's largest single
  cost and the port owns it: `docs/re/HOTJOIN_ORDER.md`, "Linux `Readable()`
  costs a /proc/self/maps parse".
- **Why 50,000 mappings:** with `dedicated_render=0` no command buffer runs, but
  the engine still prepares every frame, and lavapipe answers its buffer
  allocations out of a memfd -- 48,000 live mappings, 1.26 GiB, and 6,450
  `mmap`/`munmap` pairs a second at 30 frames a second (~290 per frame) on the
  main thread. That thread sits at ~50% of a core for nobody, and every mapping
  it adds makes every `mmap`, fault and maps-parse in the process dearer. It also
  wants `vm.max_map_count` raised (`tools/server/sysctl-tpf2mp.conf`); the
  default 65,530 is within reach of a world this size. Untried idea, cheap to
  try: park the dedicated camera zoomed right in over empty terrain once the
  world is up -- nobody looks through it, and the engine would then prepare
  almost nothing per frame.
- **`dedicated_fps` is the ceiling on the clock, not just render work.** The
  engine consumes one batch per frame, and a batch is 200 ms of simulation, so
  the frame rate caps the speed: 30 frames a second is 6x, 20 is 4x. Do not lower
  it below `5 x the fastest speed the session should reach`.
- **The batch interval pin works; `tpf2_engine_pace.txt` does not report it.**
  `base=` in that file is the engine's *own* estimate for the next batch, written
  by `Sync` before the speed hook imposes anything (`native/src/speedhook.cpp`,
  `ImposeInterval`). `base=400000` means the engine asked for 400 ms because it
  was behind, not that `pin_batch=1` was ignored -- the imposed value is in
  `tpf2_bridge.log`: `[speed] target 1.70 over lever 2 -> batch interval 235294
  us (engine's own 400000 us)`.
- **The terrain pager's automatic budget is too small for a running world.** With
  the automatic ~1 GiB it faulted 790 tiles a second and the session ran at half
  speed; `terrain_cache_hot_mb=6144` brought that to ~1.5 faults a second.
  `bigmap/docs/terrain-compression.md` has the recency-eviction fix the port
  needs.
- **Memory, after that budget: 27.6 GiB resident, 1.2 GiB swapped, and no
  stalling** -- the cgroup's `memory.pressure` reads 0.00 at every window and the
  sim thread waited 19 ms in 10 s on disk. Swap is not what costs speed here;
  `vm.swappiness=10` keeps it that way.
- **Not worth changing:** glibc tuning (`MALLOC_MMAP_THRESHOLD_` and friends).
  `_int_malloc` is 1.6% of the sim thread and `brk` never moves; the 29% libc
  time measured before the pager budget was raised was the pager decoding tiles,
  not the allocator's shape.

## Limits

- One Steam account per server, in offline mode; the account must own the game and
  every DLC pack a shared save uses.
- The server plays every action through the same lockstep as a player; a desync
  between the server and a client is handled by the same resync.
- Big Maps worlds need the Big Maps plugin installed on the server as on any player.
