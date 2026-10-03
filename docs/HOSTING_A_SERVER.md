# Hosting a dedicated server

A dedicated server is a copy of Transport Fever 2 that runs a multiplayer world around the clock,
so players can come and go without anyone having to host. It:

- hosts a lobby as soon as the game reaches the title menu, loads a world, and keeps it running;
- is listed as a **dedicated server** in **PUBLIC GAMES**, with a join code that stays the same
  across restarts;
- saves the world with the game's own autosave, and after a crash or restart loads the newest save;
- runs at 1x while nobody is connected (or pauses, if you choose), and follows the players' speed
  votes once someone joins.

It is the full game, not a cut-down simulator. The server needs a PC or a Linux machine that can
run Transport Fever 2, and a Steam account that owns the game.

This page covers setting one up. How it works inside, and where a server spends its time:
[DEDICATED_SERVER.md](DEDICATED_SERVER.md). Every setting: [CONFIGURATION.md](CONFIGURATION.md).

## Before you start

- **A Steam account that owns Transport Fever 2**, plus every DLC the world uses. It can be your
  own: with the server's Steam client in **offline mode**, you can still play on the same account
  from your own PC at the same time. The Linux scripts set offline mode for you; on a Windows
  server PC, use Steam's **Go Offline** before starting the game.
- **The same mod version as your players.** Players can only join a server running the release
  they have. When a new release comes out, update the server as well ([Updating](#updating)).
- **A save with the multiplayer mod enabled.** New games have it already. For an older save,
  open its **Mods** panel on the load screen once and enable **Transport Fever 2 Multiplayer**.
  If the world uses Big Maps or other mods, the server needs them installed too.
- **Enough memory for the world.** The server needs about as much RAM as a player's PC needs to
  play the same map. Normal maps run on an ordinary desktop. The project's test server holds a
  49,000-tile Big Maps world in about 28 GB, on 8 cores with 31 GB of RAM. A GPU is not needed:
  the server draws nothing.
- **A reachable port** (UDP and TCP, 29471 by default). See [Ports](#ports) below.

There are two ways to run a server:

| | best for | effort |
|---|---|---|
| [A Windows PC](#on-a-windows-pc) | a spare or always-on home PC | a text file |
| [A Linux server or VPS](#on-a-linux-server-or-vps) | a rented server, running unattended | a setup script and a few commands |

## Ports

A dedicated server does not use Steam's networking (its Steam client is offline), so players
connect to it directly. Its join code is always the classic letters-and-digits code.

- The server's lobby uses **UDP and TCP on one port, 29471 by default** (`dedicated_port` changes
  it). UDP carries the game; TCP carries the save sent to each player who joins.
- **On a rented server or VPS**, open that port for UDP and TCP in the machine's firewall *and* in
  your provider's firewall or security group, if it has one. The Linux setup script opens it in
  `ufw` for you.
- **At home**, the lobby first tries UPnP on your router and then punches through it with the
  master server's help. That is often enough, but for a server players can always reach, forward
  the port (UDP and TCP) on your router to the server PC.
- Players need no open ports.

## On a Windows PC

1. **Install the mod** on that PC as for playing: the launcher or `TpF2Multiplayer.msi` from the
   [latest release](https://github.com/silver2127/tpf2-multiplayer/releases/latest)
   (see the [README](../README.md#install)).
2. **Start the game once normally, then quit.** If the game shows a news popup, tick
   **DO NOT SHOW AGAIN**: the popup blocks the title menu, and the server cannot dismiss it.
3. **Put the world in the save folder**, if it is not there already:
   `Steam\userdata\<your id>\1066780\local\save\`. Note its name without `.sav`.
4. **Create `tpf2_menu_flags.txt`** in the game folder, next to `tpf2_menu.dll`
   (by default `C:\Program Files (x86)\Steam\steamapps\common\Transport Fever 2\`):

   ```
   dedicated=1
   dedicated_save=my_world
   dedicated_lobby=My Server
   dedicated_name=Server
   dedicated_password=
   dedicated_public=1
   dedicated_companies=1
   dedicated_autosave_min=10
   dedicated_empty_speed=1
   ```

   One `key=value` per line, no spaces before the `=`. What each line does is in
   [Server settings](#server-settings).
5. **Start the game from Steam.** If Windows Defender Firewall asks about `netpunch.exe`, allow it.
   The game goes straight to hosting and loads the world. The game
   window stays blank: the server does not draw, to save the machine's time. Add
   `dedicated_render=1` if you want to watch it.
6. **Check it is up:** the server appears in **PUBLIC GAMES** on another PC, or its code is the
   last `CODE=` line in `netpunch\lobby_proc.log` in the game folder.

To play normally on that PC again, delete `tpf2_menu_flags.txt` (or set `dedicated=0`) and
restart the game.

Windows has no watchdog: if the game process itself crashes, start it again from Steam, and it
loads the newest save. (A crash back to the title menu needs nothing; the server hosts again on
its own.)

## On a Linux server or VPS

The scripts in [`tools/server/`](../tools/server/README.md) run the Windows game under Proton
inside a Linux Steam client, on a virtual display, with systemd keeping it up. They are written
for **Ubuntu 24.04 or newer**. Everything below runs over ssh; the Steam login is the only
interactive step.

### 1. Prepare the machine

Every release from 0.7.0.6 on comes with the server scripts as one download,
`TpF2Multiplayer-Server-Linux.tar.gz`. As root:

```sh
curl -fLO https://github.com/silver2127/tpf2-multiplayer/releases/latest/download/TpF2Multiplayer-Server-Linux.tar.gz
tar xzf TpF2Multiplayer-Server-Linux.tar.gz
sh tpf2mp-server/setup_vps.sh
```

(`tpf2mp-server/VERSION` says which release the scripts came with. A checkout of this
repository works too: `sh tools/server/setup_vps.sh`.)

It installs the Steam client, Xvfb and the libraries they need, creates a `tpf2server` user,
installs the systemd units (`tpf2mp-xvfb`, `tpf2mp-steam`, `tpf2mp-game`) and the `tpf2server`
command, raises the kernel's memory-mapping limit, and opens the lobby port in `ufw`.

### 2. Edit the settings

Open `/etc/tpf2mp/server.env` and set at least:

```sh
LOBBY_NAME="My Server"
SERVER_NAME="Server"
LOBBY_PASSWORD=          # empty = anyone can join
COMPANIES=1              # 1: each player their own company; 0: everyone shares one
SAVE=                    # the save name without .sav; empty = the newest save
LOBBY_PORT=29471         # the example uses 29472; open whichever you pick (UDP + TCP)
MASTER_URL=              # leave EMPTY, or the server is not listed in PUBLIC GAMES
```

Keep `MASTER_URL` empty: an address there replaces the public game list. (Copies of the example
from before 2026-09-27 set it to `http://127.0.0.1:8471`, which only works on the project's own
machine, where the list itself runs; a server set up from one of those is never listed.)
If you change `LOBBY_PORT` after running `setup_vps.sh`, open the new port yourself
(`ufw allow <port>/udp` and `ufw allow <port>/tcp`).

### 3. Log in to Steam and install the game

The client must be online once to download the game. Log in and keep it online:

```sh
sudo -iu tpf2server env TPF2_STAY_ONLINE=1 sh /opt/tpf2mp/server/steam_login.sh
```

It asks for your account name, password and Steam Guard code, and types them into the client on
the virtual display. Nothing is saved except Steam's own login token, as on any PC. Then install
the game, and run it once so Proton creates its prefix:

```sh
sudo -iu tpf2server DISPLAY=:9 steam steam://install/1066780
# wait for the download to finish, then:
sudo -iu tpf2server DISPLAY=:9 steam steam://run/1066780
# after a minute, stop the game:
sudo -u tpf2server pkill -f 'TransportFever2[.]exe'
```

If Steam does not pick Proton for the game by itself, set it in the game's
**Properties → Compatibility** in the client's window (connect a VNC viewer to display `:9`).

Now switch the client to offline mode, so the same account can play online elsewhere, and start it
as a service:

```sh
sudo -iu tpf2server env TPF2_OFFLINE_ONLY=1 sh /opt/tpf2mp/server/steam_login.sh
sudo systemctl enable --now tpf2mp-steam
```

### 4. Install the mod and write the configuration

```sh
tpf2server install      # downloads the latest release's Proton installer and runs it
tpf2server configure    # writes the mod's settings from server.env, and a low-graphics settings.lua
```

Copy your world into the account's save folder, owned by `tpf2server`:
`/home/tpf2server/.steam/steam/userdata/<id>/1066780/local/save/`. Set `SAVE` to its name and run
`tpf2server configure` again.

### 5. Start it

```sh
sudo systemctl enable --now tpf2mp-game
tpf2server status
```

The watchdog asks Steam to launch the game, and launches it again whenever it exits or stops
responding. After a minute or two, `tpf2server status` shows the join code and the server
appears in **PUBLIC GAMES**.

### Running it

| command | what it does |
|---|---|
| `tpf2server status` | the services, the game process, the join code, recent joins and leaves |
| `tpf2server code` | the join code |
| `tpf2server say <text>` | a chat line from the server |
| `tpf2server logs [n]` | the last lines of the mod's log |
| `tpf2server restart` | restarts the game (it loads the newest save) |
| `tpf2server stop` / `start` | stops or starts the game; the Steam client stays up |

The service logs are in `journalctl -u tpf2mp-game -u tpf2mp-steam`.

The scripts also support the native Linux build of the game instead of Proton (`RUNTIME=native`);
see [`tools/server/README.md`](../tools/server/README.md#native-linux-runtime).

## Server settings

On Windows these go in `tpf2_menu_flags.txt`; on Linux, `tpf2server configure` writes them from
`server.env` (the name in brackets).

| flag (`server.env`) | default | what it does |
|---|---|---|
| `dedicated` | 0 | `1` turns the game into a server. |
| `dedicated_save` (`SAVE`) | empty | The save to load, by name without `.sav`. Empty: the newest save in the save folder. |
| `dedicated_lobby` (`LOBBY_NAME`) | empty | The name shown in **PUBLIC GAMES**. |
| `dedicated_name` (`SERVER_NAME`) | the Steam name | The server's name in the player list. |
| `dedicated_password` (`LOBBY_PASSWORD`) | empty | A lobby password; empty lets anyone in. With one, the code is locked and players type the password before **JOIN**. |
| `dedicated_public` | 1 | `1` lists the server in **PUBLIC GAMES**; `0` keeps it unlisted, reachable by code only. |
| `dedicated_companies` (`COMPANIES`) | 0 | `1`: each player gets their own company (**SEPARATE COMPANIES**); `0`: everyone plays one company. |
| `dedicated_autosave_min` (`AUTOSAVE_MIN`) | 10 | Minutes between autosaves; `0` never. The session pauses for the few seconds a save takes. |
| `dedicated_empty_speed` (`EMPTY_SPEED`) | 1 | The world's speed while nobody is connected, `1`-`4`, or `0` to pause. |
| `dedicated_port` (`LOBBY_PORT`) | 29471 | The lobby's UDP and TCP port. |
| `dedicated_render` (`RENDER`) | 0 | `1` draws the game window as usual. |

The full list, including the performance settings, is in
[CONFIGURATION.md](CONFIGURATION.md#tpf2_menu_flagstxt).

## Joining a server

Players join like any other game: **Multiplayer** → **PUBLIC GAMES**, click the server's row, and
**JOIN GAME**. For an unlisted server, send them the code instead. Nobody can pause a dedicated
server; the players' speed votes set the speed.

## Updating

Every release must match between server and players.

- **Windows:** install the new release as usual, with the game closed. `tpf2_menu_flags.txt` is
  kept.
- **Linux:** `tpf2server stop`, `tpf2server install`, `tpf2server configure`, `tpf2server start`.
  To pin a particular release rather than the latest, set `MOD_RELEASE` (for example `0.7.0.6`) in
  `server.env`. To update the server scripts too, download and unpack the new
  `TpF2Multiplayer-Server-Linux.tar.gz` and run `sh tpf2mp-server/setup_vps.sh` again: it keeps
  your `server.env` and the Steam login.

## Changing the world

Put the new save in the save folder, set `dedicated_save` (Windows) or `SAVE` and run
`tpf2server configure` (Linux), then restart the game. The server's autosaves are ordinary saves:
copy the newest one out of the save folder to keep a backup, or to play the world yourself.

## When something goes wrong

| symptom | what to check |
|---|---|
| The server is not in **PUBLIC GAMES** | `dedicated_public=1`; on Linux, `MASTER_URL` empty in `server.env` (then `tpf2server configure` and restart). |
| Players see the server but cannot connect | The port, UDP **and** TCP, in the machine's firewall, your provider's firewall, or your router. |
| Players get in but the world never arrives | TCP on the port; the save goes over TCP first. |
| `Multiplayer version mismatch` | The server and the player run different releases; update the older one. |
| The game starts but never hosts | The news popup (Windows: tick **DO NOT SHOW AGAIN** once; Linux: `tpf2server configure` handles it). Otherwise the mod's log: `tpf2_menu.log` in the game folder, where every server decision is a `[dedicated]` line. |
| The wrong world loads | `dedicated_save` / `SAVE` is the name without `.sav`, and the file is in *this* account's save folder. |
| The world has no multiplayer mod | Enable **Transport Fever 2 Multiplayer** in the save's **Mods** panel and save it again. |

Logs: `tpf2_menu.log` (the mod) and `netpunch/lobby_proc.log` (the lobby) in the game folder; on
Linux also `tpf2server status` and `journalctl -u tpf2mp-game`. Include them when you ask for
help on the [Discord](https://discord.gg/7VhmtUstqQ) or in an issue, after removing anything you
would not want public.
