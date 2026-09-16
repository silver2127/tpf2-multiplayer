# The Multiplayer lobby on Linux

The lobby half of the title menu's Multiplayer panel, ported from
`native/src/menu_hook.cpp` to the native Linux build (Steam build 35924). The
lobby program's side of the same contract is [NETPUNCH.md](NETPUNCH.md), section 3.

| File | What |
|---|---|
| `native/linux/src/lobby_linux.h`, `lobby_linux.cpp` | The lobby process, its events, `tpf2_bridge_ctl.txt`, `mp_company_cfg.txt`, hot join, the public list, OPEN LOGS |
| `native/linux/src/panel_linux.cpp`, `panel.h` | The HOST / JOIN page (now with the public list) and the LOBBY page, their clicks and typing, the SDL clipboard |

Windows functions and where they went:

| menu_hook.cpp | Linux |
|---|---|
| `EnsureLobbyJob`, `QuitLobbyProc`, `TeardownLobby` | `Spawn` (own session, `PR_SET_PDEATHSIG`), `Teardown`, `StopGroup` |
| `LobbyThread`, `StartLobby`, `LeaveLobby` | `LobbyThread`, `Launch`, `lobby::Start`, `lobby::Leave` |
| `applyRoster`, `chatPush`, the event switch | `ApplyRoster`, `ChatPush`, `Dispatch`, `HandleStart` |
| `writeBridgeCtl`, `readBridgePort`, `readBridgePid`, `pickRelayPort`, `relayPortFor` | `WriteBridgeCtl`, `ReadBridgePort`, `ReadBridgePid`, `PickRelayPort` |
| `originLetterFor`, `originName`, `writeCompanyCfg`, `speedFromChat` | `OriginLetterFor`, `OriginName`, `WriteCompanyCfg`, `SpeedFromChat` |
| `SyncStart`, `SyncPoll`, the relay leader's periodic upload | `SyncStart`, `SyncPoll`, `RelayPeriodic` |
| `PubFetchThread`, `PubPoll`, `httpGet` | `PubThread`, `lobby::PublicPoll`; the lobby program fetches |
| `CollectLogsThread` | `lobby::OpenLogs` (`Tpf2mpArchiveLogsSafe` from `logarchive_linux.h`, then `xdg-open`) |
| `RenderPanelLayer` state 2, `OnHit`, `LlKeyboard` chat branch | `RenderLobbyLocked`, `OnHitLocked`, `HandleEventLocked` |
| `ClipboardSet`, `ClipboardGet` | `SDL_SetClipboardText`, `SDL_GetClipboardText` through `dlsym` |
| `doStartLoad`, `placeSaveNewest`, `newestSave`, `ForceAutosave`, AUTO-LOAD | the menu-game area: `menu_game_linux.h` |

## Threads and locks

- **UI thread** (the game's SDL pump): the SDL event filter, `panel::OnMenuPage`.
- **Render thread**: `panel::Frame` and `panel::Hover` from the Vulkan overlay.
- **Lobby thread** (one, for the life of the process): starts and stops the lobby,
  tails its events, writes the shared files, polls for sync saves.
- **Public list thread**: runs `netpunch --print-public-list` when a fetch is due.
- **OPEN LOGS**: a short-lived thread per click.

The panel's state is behind one mutex; the lobby's model behind another. The
order is always panel, then lobby: the panel calls `lobby::*` with its lock held,
and those calls never call back. The lobby's threads report through
`StatusFn` / `DirtyFn` (`panel::SetStatus`, an atomic dirty flag) and never hold
the lobby lock while they do. Nothing slow runs on the UI or render thread: a
click queues a request, the lobby thread does the work.

No `MenuGame_*` call is ever made with a panel or lobby lock held (the menu-game
area may call `panel::SetStatus`, or hold locks of its own on the UI thread).
`panel::Init` therefore:

1. reads the fonts before taking the panel's lock (the Noto fallback is a 16 MB
   read, and the render thread takes that lock every frame through `panel::Visible`);
2. sets the log, the data folder, the flags, the names and the lobby's `Config`
   under the lock;
3. calls `lobby::Init`, which asks `MenuGame_SaveDir`, with the lock released;
4. only then, under the lock, marks the fonts loaded: the panel shows, and takes
   clicks, from that moment, so every click reaches an initialised lobby.

`panel::InstallInput` resolves the SDL functions under the lock and calls
`SDL_GetEventFilter` / `SDL_SetEventFilter` without it (see Input).

**Threads the system refuses.** `std::thread`'s constructor throws
`std::system_error` when `pthread_create` fails (`EAGAIN`, e.g. at `RLIMIT_NPROC`
in a busy container). An exception must not unwind through SDL's C frames or out of
a game thread, so every construction is caught and logged:

| Thread | When refused |
|---|---|
| lobby thread (`lobby::Init`) | HOST / JOIN answer "The lobby could not start its thread -- see tpf2_menu.log" |
| public list thread (`lobby::Init`) | the list's note is "Server browser unavailable (no thread)"; no fetch is attempted |
| OPEN LOGS (`lobby::OpenLogs`, from the SDL filter) | returns false; the panel's status is "Couldn't start gathering the logs -- see tpf2_menu.log" |
| xdg-open's reaper (`OpenFolder`) | logged; a slow `xdg-open` stays a zombie once it ends |

Strings, vectors and the model live in leaked heap structs (`P()`, `S()`): the
threads are still running when `exit()` runs static destructors.

## The lobby process

### Program and folder

The program, as `resolveNetDir` + `LobbyThread` on Windows:

1. `$XDG_DATA_HOME/tpf2mp/netpunch/netpunch` (else `$HOME/.local/share/...`; under
   the Snap, `HOME` is `~/snap/steam/common`), when it is an executable file;
2. `<game dir>/netpunch/netpunch`;
3. `python3 <folder>/lobby.py` from the first of those two folders holding
   `lobby.py`, only when `python3` is on `PATH` (the Steam runtime's container has
   none; NETPUNCH.md ships no Python fallback).

A non-executable `netpunch` is skipped and named in the log.

The lobby folder, which holds `lobby_out.jsonl`, `lobby_in.jsonl`, `lobby_state.json`,
`lobby_proc.log` and a joiner's `incoming_save.*`, is always
`$XDG_DATA_HOME/tpf2mp/netpunch`, created when missing, and is passed as the working
directory and as `--io-dir`. It is the first folder the Lua mod's `CM.netDir` probes
for `lobby_out.jsonl`, so a lobby program kept in the game folder still writes where
the game script looks. Only without a usable `XDG_DATA_HOME` or `HOME` is it the
program's own folder (the Lua side's `./netpunch` then).

### Arguments

The Windows ones plus `--io-dir` and `--parent-pid` (NETPUNCH.md 3.3). Values a
player typed use `--opt=value`, because argparse takes a leading `-` in a separate
argument for another option:

```
host: <prog> host --name=<name> --game-relay-port 7773 --game-local-port <bridge port>
      --forward-log <data>/tpf2_bridge.log --forward-log <data>/tpf2_menu.log
      --forward-log <data>/mp_company_a.log --forward-log <data>/mp_company_b.log
      [--password=<pw>] --lobby-name=<lobby> [--publish=<master_url> [--public]] [--no-share-mods]
      --io-dir=<lobby folder> --parent-pid <game pid>
join: <prog> join <CODE> --name=<name> --local-port 0 --game-relay-port <7774..7805>
      --game-local-port <bridge port> <the four --forward-log> [--password=<pw>]
      --io-dir=<lobby folder> --parent-pid <game pid>
```

- `<bridge port>`: the `port=` line of `<data>/tpf2_instance.txt`, read at each
  launch, 7771 without one.
- A joiner's relay port is the first from 7774 that a UDP socket can bind on the
  wildcard address (Linux refuses that bind while any address holds the port, so
  lobby.py's `127.0.0.1:<port>` counts); the host's is 7773.
- `<CODE>` is trimmed and must be base32 (`A-Z a-z 2-7 =`, at most 200): a crafted
  code cannot smuggle options in. A code field shorter than 8 characters takes the
  clipboard's text instead, as on Windows.
- The log shows the command with `--password=***` and a joiner's code as
  `<code, N chars>`: both are credentials, and `tpf2_menu.log` rides to the host's
  merged `lobby_peers.log` (`--forward-log`) and into OPEN LOGS archives. As with a
  Windows command line, other local users can read the arguments
  (`/proc/<pid>/cmdline`).

The argv shape is checked against lobby.py's real `argparse`: the tests' fake lobby
runs `lobby.main`.

### Environment, descriptors, output

- The game's environment minus `LD_PRELOAD` (the loader already removed itself;
  the Steam overlay has no business in a lobby), plus
  `TPF2MP_GAME_DIR=<game folder>` (NETPUNCH.md 3.2).
- stdin is `/dev/null`; stdout and stderr go to `lobby_proc.log`, truncated at
  each launch (`/dev/null` if it cannot be opened, logged).
- No other descriptor: the child calls `close_range(3, ~0)` (a loop to
  `RLIMIT_NOFILE` where the kernel lacks it), so the bridge's UDP socket and the
  game's device and log files never leak into the lobby.

### Dying with the game

Windows puts `netpunch.exe` in a job object with `KILL_ON_JOB_CLOSE`. Linux:

- **Own session** (`setsid`), so the lobby leads its own process group: stopping it
  signals the group, and the onefile bootloader and the Python child that owns
  udp/29471 go together (`TerminateJobObject`).
- **`PR_SET_PDEATHSIG(SIGTERM)`** for a crash or a kill. The kernel sends it when
  the *thread* that created the child ends, so launches happen only on the lobby
  thread, which never ends. The onefile bootloader forwards SIGTERM to its child
  (MEASURED by the netpunch-linux area, NETPUNCH.md 3.4), and lobby.py leaves on it.
- **`--parent-pid`**: lobby.py's own once-a-second watch of the game
  (NETPUNCH.md 3.5), which also covers anything that defeats the signal.
- **At exit**, a destructor appends `{"cmd":"quit"}` without waiting
  (`DLL_PROCESS_DETACH`).

`prctl` has to run in the child, and the game is multi-threaded, so there is no
`fork` and no `posix_spawn`: the child is made the way glibc's `posix_spawn`
makes its own: `clone(CLONE_VM | CLONE_VFORK | SIGCHLD)` on a private 256 KiB
stack, every signal blocked around the call, and in the child only raw system
calls: handlers that are neither `SIG_DFL` nor `SIG_IGN` reset (glibc's internal
signals 32 and 33 ignored, as glibc does), `prctl`, a `getppid` check (the game may
have died before `prctl` took hold), `setsid`, `dup2`, `close_range`, `chdir`, the
old mask back, `execve`. A failed step's `errno` is written into the shared
argument block and the launch reports it.

**SIGCHLD and `waitpid` on our own children.** In the `TransportFever2` image, the
game neither reaps nor ignores SIGCHLD: the image's only callers of `sigaction`
(PLT `0x6ddd90`) and `signal` (PLT `0x6ddda0`) are one crash-handler family,
`0x36c33d0`, `0x36c37a0`, `0x36c38d0`, `0x36c42d0`. It walks the table at
`.rodata 0x50637d0 = {11, 6, 8, 4, 7, 5}` (SEGV ABRT FPE ILL BUS TRAP):
`0x36c37d3 lea rbx, [rip+0x199fff6]`, then 4-byte steps against 0x98-byte
`struct sigaction` slots from `.bss 0x5b3f820` (`0x36c37da lea rbp`); the restore
path is `signal(sig, SIG_DFL)` (`0x36c3656 xor esi, esi`, `0x36c3666 call signal`).
`popen` (`0x990362`, in Lua's io library next to `os.clock` `0x994e20`) and `system`
(`0x322d43b`, `0x322d5be`, `0x322d6de`) are glibc's and reap their own pids.

That covers the image only. The game's `libSDL2-2.0.so.0.2500.0` imports
`sigaction`, `signal` and `waitpid` too (`readelf --dyn-syms`: `UND
sigaction@GLIBC_2.2.5`, `signal@GLIBC_2.2.5`, `waitpid@GLIBC_2.2.5`). INFERRED
from SDL's sources: `SDL_QuitInit`'s SIGINT / SIGTERM handlers and `waitpid` on
pids SDL started itself. The Steam overlay's preload was not checked. Nothing
depends on the claim: a child reaped by someone else makes `waitid` fail with
`ECHILD`, which `HasExited` reports as gone, and once gone the lobby is never
signalled again (`SweepAndReap`). A handler set to `SIG_IGN` (auto-reaping) ends
the same way.

### Stopping

`Teardown` (LEAVE, and before every new HOST / JOIN): append `{"cmd":"quit"}`, wait
up to 1500 ms (`menu_hook.cpp`); then `kill(-pgid, SIGTERM)` and wait up to 2000 ms
(lobby.py leaves on it, the bootloader passes it on and removes its `/tmp/_MEI*`
folder); then `kill(-pgid, SIGKILL)` and reap (NETPUNCH.md 3.4). An exit is noticed
with `waitid(..., WNOWAIT)`, so the leader is still a zombie, its pid and group id
cannot be reused, while `kill(-pgid, SIGKILL)` sweeps whatever it left behind; then
it is reaped. A lobby that ends by itself is logged with its exit code and the number
of event lines read; with none read, the status says "The lobby stopped before it
reported anything".

**A dead lobby.** A lobby that ended by itself, or whose program is missing or could
not be started, stays on the LOBBY page but is marked dead (`Model::dead`, set before
the reason goes to the status line). Chat, START GAME, PUBLIC and the mods answer
then return "The lobby is not running -- press LEAVE, then HOST or JOIN again."
Before, they returned "Lobby is starting...", which replaced the reason, or were
dropped silently once events had arrived. The panel keeps chat text that was not
sent. A command queued in the up to 200 ms before the exit was noticed is dropped
with the same status. LEAVE works as always.

## Files

| File | Direction | Content |
|---|---|---|
| `<lobby>/lobby_out.jsonl` | lobby -> panel | events, tailed from a byte offset every 200 ms; a shrunken file is read again from the start |
| `<lobby>/lobby_in.jsonl` | panel -> lobby | commands, one `write()` with `O_APPEND` per line |
| `<data>/tpf2_instance.txt` | bridge -> panel | `port=`, `pid=` |
| `<data>/tpf2_bridge_ctl.txt` | panel -> bridge, game script | `instance=`, `peer=127.0.0.1:<relay port>`, `pid=<bridge pid>`, `players=`, `speed=`, `sync=`, `xfer=`, `leader=`; written to a temp file and renamed; skipped when unchanged |
| `<data>/mp_company_cfg.txt` | panel -> game script | `coop`/`companies`, my company, the ids, `letter=company` map; at START |
| `<data>/tpf2_sync_save.txt` | game script -> panel | its existence asks for a sync save; deleted when taken |
| `<data>/tpf2_sync_sent.txt` | panel -> game script | the path of the save that went out |

The commands are escaped JSON (quotes, backslashes, control characters), and the
chat text is made valid UTF-8 first (overlong forms and surrogates too): lobby.py
drops a line it cannot decode.

Letters: the host is `a`, joiners `b`, `c`, ... in roster order skipping the host
(`aa`, `ab`, ... past 26); in a relay lobby the relay's own letters win. A letter
from the relay that is not `[a-z]{1,2}` is ignored (the bridge would refuse it).

Numbers: the game calls `setlocale` (an import of the binary), so JSON numbers,
the `/speed` value and the `scale=` flag are parsed by hand and `speed=` is printed
by hand (`%.4g` without the locale). Tested under `de_DE.UTF-8`.

## Events

| Event | Effect |
|---|---|
| any line | the lobby is ready (it has truncated `lobby_in.jsonl`): chat, START and PUBLIC are allowed |
| `code` | kept, never drawn or logged; the status says "Room code ready -- move the mouse over the game to copy it." (set before the code is stored, so it can never replace what follows); copied to the clipboard on the next UI-thread event, then "Your code is copied" |
| `roster` | players, companies, `you`, `host`, `lobby`, `relay`, `letters`, `stored_age`, `stored_max`; a relay lobby moves the host role; the ctl is rewritten; hot join |
| `chat` | into the 14-line chat and through `/speed` / `/sync`; `!hotjoin ...` is a status at the title menu only, never chat |
| `status` | `detail` into the status line |
| `transfer` | status and `xfer=` (`receiving 40%`, `sending 30%`, `uploading 60%`; cleared at done, failed, dropped and 100%) |
| `mods_prompt` | `share_mods=ask`: YES / NO on the lobby page; `always` / `never` answer at once |
| `mods_ready`, `save_ready` | status; `save_ready` allows loading `incoming_save.sav` |
| `start` | as Windows: ignored in a game; the host loads the save it shared unless it received one; a joiner loads `<lobby>/incoming_save.sav` only after `save_ready`; then `mp_company_cfg.txt`, 400 ms, `MenuGame_PlaceSharedSave(src, &placedName)`, and `MenuGame_RequestAutoload(placedName)` (`autoload=1`) or "open LOAD GAME and pick <placedName>" |

Events from a lobby that was left, or replaced by a new HOST / JOIN, are dropped
(a generation counter).

## Commands

`chat`, `start` (with `save` = the newest save from `MenuGame_NewestSave`, or
without one), `company` (`player`, next id: the next one in use, then a new one,
then 1), `publish` (`on`), `mods` (`accept`), `quit`.

## The public game list

Shown on HOST / JOIN when `master_url` is set (default
`https://srv1306562.hstgr.cloud/tpf2mp`, a trailing `/` dropped, only plain
`http(s)` URLs without blanks or quotes). While the page is up, every 10 s, or
at REFRESH: `<lobby program> --print-public-list <master_url>` in its own session,
with stdout and stderr on drained pipes and a 20 s limit (then the group gets
SIGTERM, then SIGKILL). Exit 0: stdout is the master's `/list` body,
`{"servers":[{name, code, game, type, version, players, max, age, locked}...]}`;
the first 8 rows with a code are shown. Exit 1: stdout's one line says why
("HTTP 503", "[Errno 111] Connection refused") and the note shows it, as
`httpGet()` did (NETPUNCH.md 3.6). No HTTP or TLS runs in the game process.

## OPEN LOGS

A thread runs `Tpf2mpArchiveLogsSafe(false, <game dir>, ...)` from
`logarchive_linux.h`: a `<time>-now` copy of this run's logs beside the earlier
runs the loader saved at start, in `$XDG_DATA_HOME/tpf2mp/logs/`. The status says
where ("Logs gathered in ... -- send the newest folders with a bug report."), and
the folder is opened with `xdg-open` when one is on `PATH` (the Steam runtime may
have none; the status line is the answer then). If the thread cannot be started,
the status says "Couldn't start gathering the logs -- see tpf2_menu.log".

## Input

- The LOBBY page takes every key for the chat (modifiers, Esc and Tab pass) until
  the shared save is placed, as the Windows keyboard hook did. Enter sends; text
  not sent (the lobby is still starting, or not running) is kept.
- Esc on the lobby page is the game's; the close button there is LEAVE.
- **The clipboard runs with the panel's lock released.** The game ships libSDL2
  2.25.0; X11's `GetClipboardText` waits for the clipboard's owner by calling
  `SDL_PumpEvents` (SDL `release-2.24.0` and `release-2.26.0`,
  `src/video/x11/SDL_x11clipboard.c`), and `SDL_PushEvent` calls the event filter
  from inside that pump (`SDL_events.c`, `SDL_EventOK.callback` under the recursive
  `SDL_event_watchers_lock`). The filter used to paste with its non-recursive mutex
  held, a deadlock waiting for a paste from another program. Now a click only
  records what it wants (paste, JOIN with the clipboard's code, copy); the filter
  does it after unlocking, and an event arriving inside a clipboard call passes
  straight to the game. Clipboard calls happen only for input and window events:
  the thread that pumps the window.
- **The filter is installed with the panel's lock released, too.** `SDL_GetEventFilter`
  and `SDL_SetEventFilter` take `SDL_event_watchers_lock`, and `SDL_PushEvent` calls
  the filter under that lock (SDL `release-2.24.0` `SDL_events.c` 1121-1123), where
  the filter takes the panel's lock: the opposite order. `SDL_SetEventFilter` also
  discards every queued event (`SDL_FlushEvents(SDL_FIRSTEVENT, SDL_LASTEVENT)`,
  1173-1177). It runs at the overlay's first present, so events the game had not
  polled yet at that moment are lost, once. Installing on the UI thread right after
  the game's poll (`SdlBackend::Step` `0x3349010`) would lose nothing, but no hook
  there is established to run on that thread, so it stays at the first present.
- **Status lines are logged** (`[panel] status:`), except progress: a line that
  ends in digits and `%` and matches the previous line before them ("Receiving
  save... 42%"). Lines that only begin alike are all logged ("Save ready --
  loading it...", then the autoload's "Save ready -- open LOAD GAME ...").

## Where the game is

Windows reads its own capture of `CGameUI` to tell that a game is running. Here
the lobby is told:

- `panel::OnMenuPage(page)` (already called by `menu_linux.cpp`): page 2 means the
  title menu, and clears "a game ran"; 3 and up cover it.
- `panel::OnGameUiFrame()`: to be called by the menu-game area's CGameUI update
  detour (the HJ-02 vtable swap in `docs/re/linux/MENU_GAME.md`).

In a game = the title menu is covered and a CGameUI frame has run since it was last
shown. Hot join (the host's roster grows in a game: `MenuGame_ForceAutosave`, wait
for the new save to stop growing, `start` with it, `!hotjoin` in chat), the
`tpf2_sync_save.txt` request and the relay leader's periodic upload
(`relay_autosave_min`) need it. Until `OnGameUiFrame` is wired they do not fire,
and the log says so once when a player arrives away from the title menu.

## Flags (`tpf2_menu_flags.txt` beside `tpf2_menu.so`)

`scale`, `master_url`, `relay_autosave_min` (0-60), `share_mods`
(`ask`/`always`/`never`), `autoload` (`0`/`1`). `slot` is `menu_linux.cpp`'s;
`automod` is read by `MenuGame_AutoEnableMod`.

## Differences from Windows

- No job object: an own session, `PR_SET_PDEATHSIG` and `--parent-pid`; stopping
  adds a group SIGTERM between the quit and the kill.
- The lobby folder is always the XDG one, passed as `--io-dir` too.
- The public list goes through the lobby program, not WinHTTP.
- OPEN LOGS opens the archive with `xdg-open` when there is one, instead of Explorer.
- The room code is copied on the next input or window event rather than at once
  (the clipboard belongs to the SDL thread); until then the status says "Room code
  ready -- move the mouse over the game to copy it.", so a host who switched to
  another window knows to come back before pasting.
- A joiner's room code is masked in the logged command (`menu_hook.cpp` 2370
  logged it).
- A lobby that could not start or has exited refuses chat, START GAME, PUBLIC and
  the mods answer with a reason (Windows dropped them or said "starting").
- Installing the SDL event filter discards the events queued at the overlay's
  first present, once (`SDL_SetEventFilter`).
- The code button's placeholder is drawn in Lato, not Consolas; emoji in names show
  as `?` (the game's Lato and Noto CJK faces have none).
- The roster rows stop where the button row starts (15 at 1080p) instead of
  running under it.
- Chat keeps unsent text; chat takes any text the keyboard layout composes.
- `speed=` and `sync=` requests and the hot-join roster count start fresh with each
  new lobby (Windows kept them in globals across lobbies).
- JSON is parsed as a tree with `\uXXXX` decoded (Windows showed escapes verbatim
  and searched the text for keys).

## Tests

`test_lobby.cpp` (in the area's scratch folder) drives the real `panel_linux.cpp`,
`lobby_linux.cpp` and `panel_layer.cpp` off-game: SDL's dummy video driver for
events and the clipboard, a render thread calling `panel::Frame`, a file-system
stub of `menu_game_linux.h`, and `fake_netpunch.py`, which runs the real
`lobby.main` (its `argparse`, SIGTERM handler and `--parent-pid` watch) and
`LobbyIO` with scripted sessions. Scenarios:

| Mode | Covers |
|---|---|
| `host` | `panel::Init` -> `lobby::Init` -> a `MenuGame_SaveDir` stub that calls `panel::SetStatus` (deadlocked while Init held the panel's lock), public list (decoding, row click), PUBLIC, HOST argv, `--io-dir`, `--parent-pid`, `TPF2MP_GAME_DIR`, no `LD_PRELOAD`, `PDEATHSIG` 15, own session, no inherited descriptor, roster, "Room code ready" before the copy (rendered), auto-copy of the code, chat escaping and echo, invalid UTF-8, `/speed` under `de_DE.UTF-8`, `/sync`, chips, the ctl and company files, copy code, START GAME to autoload, keyboard release, a status beginning like the previous one is logged, progress lines logged once, LEAVE |
| `join` | refused code, JOIN from the clipboard with the filter re-entered during the paste (no deadlock), the code masked in the logged command, relay port 7775 with 7774 held, joiner ctl letters, `xfer=`, mods YES, own chip only, `incoming_save.sav` placed, Esc passes |
| `pdeathsig` | the harness is SIGKILLed: a onefile-style bootloader and its child exit inside the 1 s `--parent-pid` poll |
| `quitless` | a lobby ignoring quit: the group SIGTERM ends it after 1.5 s |
| `stubborn` | a lobby ignoring quit and SIGTERM: SIGKILL after 1.5 s + 2 s |
| `exit` | a lobby that dies at once: exit code and status; chat, START GAME and PUBLIC answer "not running"; a second lobby that reports three events and then exits: the same answers, no "stopped before it reported" |
| `hotjoin` | a roster grows in a game: forced autosave, `!hotjoin`, the finished save shared, `tpf2_sync_sent.txt`, the following `start` ignored, `tpf2_sync_save.txt`; nothing at the title menu |
| `nolobby` | no program: notes, status, tried paths; chat on that lobby says "not running", never "starting" (rendered: the text stays in the field); OPEN LOGS with threads refused (status, no `std::terminate`), then the archive and `xdg-open` |
| `publist-fail` | "HTTP 503" in the note, own session, the 10 s cadence, REFRESH |
| `nothread` | `lobby::Init` with threads refused: both refusals logged, `Start` refuses with its reason, the list's note says "no thread" |

Threads are refused by lowering the soft `RLIMIT_NPROC` to 1 around the call
(`pthread_create` then fails with `EAGAIN` for a user with other processes).
Renders of both pages are written as images next to each run and were looked at.
Every scenario also runs under AddressSanitizer and UndefinedBehaviorSanitizer.
