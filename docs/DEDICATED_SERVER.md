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
| world up | force the game's own autosave every `dedicated_autosave_min` minutes (0 = never); after a crash the next launch loads the newest save |

A crash to the title menu leaves the lobby (as for any player) and the first row hosts
again. The mod's half (`mp/pacing.lua`, told through `mp_dedicated.txt`): with
`dedicated_empty_speed=N` the world runs at N (1x by default, 0 = paused) when the
last other player leaves, and resumes at the players' votes when one arrives.

**Speed.** Nobody stands at the server's controls, so the server has no vote and
its own lever is never read as a player's pause. The players' speed votes (the
speed buttons and the multiplayer window's speed row) set the session speed, as
in any session; with players in and no vote cast yet the world runs at 1x. A
frozen join or a resync resumes at the votes' speed (else the speed the server
paused from, never 0). Nobody can pause a dedicated server's session -- a
player's pause press does nothing there, as it does for any joiner.

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

## Limits

- One Steam account per server, in offline mode; the account must own the game and
  every DLC pack a shared save uses.
- The server plays every action through the same lockstep as a player; a desync
  between the server and a client is handled by the same resync.
- Big Maps worlds need the Big Maps plugin installed on the server as on any player.
