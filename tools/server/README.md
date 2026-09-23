# tools/server -- a dedicated server on a Linux box

Run either the native Linux game or the Windows game under Proton, with a Linux
Steam client and a virtual display; the mod's `dedicated=1` mode does the hosting.
Steam can run offline after installation and authentication. Design and limits:
[docs/DEDICATED_SERVER.md](../../docs/DEDICATED_SERVER.md). The first box is the
project's VPS (76.13.109.115), which also runs the relay and the master server.

| file | what |
|---|---|
| `setup_vps.sh` | root, idempotent: packages (Steam client, Xvfb, xdotool, i386 libs), the `tpf2server` user, `/etc/tpf2mp/server.env`, the units `tpf2mp-xvfb`, `tpf2mp-steam`, `tpf2mp-game`, the `tpf2server` command, the firewall port |
| `steam_login.sh` | the one interactive step, as `tpf2server`: log the client in (password + Steam Guard typed over ssh, pushed into the window with xdotool, never stored), then mark the account for offline mode |
| `game_watchdog.sh` | the `tpf2mp-game` unit: asks Steam to launch the game and again whenever it is gone |
| `tpf2server` | `status`, `install` (the mod, via the release's `install_proton.sh`), `configure` (flags + a headless `settings.lua` from `server.env`), `start/stop/restart`, `logs`, `code`, `say` |
| `server.env.example` | the settings |

## Runbook

### Native Linux runtime

The service also supports the native Linux build 35924. Install that game
through the Steam client, unpack a native multiplayer release, then set these
values in `/etc/tpf2mp/server.env`:

```sh
RUNTIME=native
GAME_DIR="/home/tpf2server/.steam/steam/steamapps/common/Transport Fever 2"
XDG_DATA_HOME=/home/tpf2server/.local/share
NATIVE_RELEASE_DIR=/opt/tpf2mp/releases/tpf2mp-linux-0.6.1.18-native
SAVE=server_world
RENDER=0
```

Run `tpf2server install`, `tpf2server configure`, then start `tpf2mp-game`.
The native watchdog launches the installed `tpf2mp-launch` wrapper and restarts
its own process group on exit or a stale menu heartbeat. Steam must already
have the game installed and the account authenticated; Steam offline mode can
be used after installation. The native executable and the mod need a virtual
X display even with `RENDER=0`.

Native logs and control files are under `$XDG_DATA_HOME/tpf2mp/data`; the lobby
is under `$XDG_DATA_HOME/tpf2mp/netpunch`. The save lives in the Steam account's
`userdata/<account>/1066780/local/save` folder. Put the multiplayer-enabled save
there and set `SAVE` without its `.sav` extension.

For isolated concurrent testing, use separate game/userdata and XDG directories,
displays, and lobby ports. Set `TPF2MP_BRIDGE_PORT` to a distinct loopback port
for each native process. Native local relay ports are selected from available
ports starting at 7773 (host) or 7774 (joiner). Set `TPF2MP_RELAY_PORT` to a
distinct starting port when another runtime already uses those ports; the native
lobby searches up to 32 ports from that value (valid range 1024–65535).

### Proton runtime

```sh
# 1. on the box, as root, from a checkout (or a copy of this directory)
sh tools/server/setup_vps.sh
$EDITOR /etc/tpf2mp/server.env          # lobby name, port (NOT 29471 beside the relay), companies, ...

# 2. the Steam login (once)
sudo -iu tpf2server sh /opt/tpf2mp/server/steam_login.sh
sudo systemctl enable --now tpf2mp-steam

# 3. the game and Proton, through the client (it is offline: install from a library
#    it already knows, or do steps 2-3 online and go offline after; see below)
sudo -iu tpf2server DISPLAY=:9 steam steam://install/1066780
#    Properties -> Compatibility -> Proton is the client's; set it with:
sudo -iu tpf2server DISPLAY=:9 steam steam://run/1066780     # once, so the prefix exists; quit it
# 4. the mod, then the flags and the headless settings
tpf2server install
tpf2server configure
# 5. up
sudo systemctl enable --now tpf2mp-game
tpf2server status
```

Installing the game needs the client online once (offline mode cannot download). The
order that works: `steam_login.sh` logs in online and finishes with the offline mark;
comment that mark out (`WantsOfflineMode 0` in `loginusers.vdf`) for the install, or
simply run the install before the login script's last step by installing from the
client's own window on the virtual display -- `xdotool` and a VNC on `:9` both work.

## Co-hosting with the relay

The relay-only lobby binds UDP/TCP 29471 and the loopback relay ports 7773 -> 7771.
The dedicated server's lobby needs its own port (`LOBBY_PORT`, default 29472 here),
and the relay must move its loopback ports off 7771/7773 so the two do not exchange
frames on the same machine: redeploy the relay with
`--game-relay-port 7783 --game-local-port 7781` (see `tools/relay_deploy.sh`).

## Kernel limits

`setup_vps.sh` installs `sysctl-tpf2mp.conf` as `/etc/sysctl.d/99-tpf2mp.conf`:
`vm.max_map_count` (a big world plus the software renderer hold tens of thousands of
mappings, and the default 65,530 is a crash when reached) and `vm.swappiness=10`. On a
box set up before this file existed, copy it over and run `sysctl --system`; a
`sysctl -w` alone is lost at the next reboot. What each limit is for, and where the
server's time actually goes, is in `docs/DEDICATED_SERVER.md`.

## Where to look

- `tpf2server status` -- units, the game process, the code, joins and leaves.
- `journalctl -u tpf2mp-game -u tpf2mp-steam` -- the watchdog and the client.
- `<game dir>/tpf2_menu.log` -- the mod; every dedicated decision is a `[dedicated]` line.
- `<game dir>/netpunch/lobby_proc.log` -- the lobby.
