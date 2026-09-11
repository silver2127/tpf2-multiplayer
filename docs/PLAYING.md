# Playing

## Before you start

- Everyone needs Windows, the Steam version of Transport Fever 2 and the **same version** of
  `TpF2Multiplayer.msi` installed (see the [README](../README.md#install)).
- Every mod the save uses must be installed on every machine: local mods copied into each game's
  `mods` folder, Steam Workshop mods subscribed by each player. Mods are not sent with the save.
  Per-save mod settings need nothing: they travel inside the save. The multiplayer mod itself
  (**Transport Fever 2 Multiplayer**) comes with the installer.
- **The multiplayer mod has to be enabled in the save.** New games get it automatically: each time the
  game starts, the Multiplayer panel adds it to the game's default mod list. For an existing save,
  open the save's **Mods** panel on the load screen once and enable it; the save remembers. The
  shared save carries its mod list to the other players.
- Only the host (or a dedicated relay) needs an open port; see [Ports](#ports-and-firewalls).

## The Multiplayer panel

The title menu gains a **Multiplayer** entry. It opens a panel over the menu:

- **HOST A GAME**: a lobby name, the **HOST GAME** button and a **PUBLIC** checkbox.
- **JOIN A GAME**: a code field (click it to paste) and **JOIN GAME**.
- **YOUR NAME** and an optional **PASSWORD**. Your name and lobby name are remembered; the first
  time you get a random two-word name.
- **PUBLIC GAMES**: the games currently listed, with host, type (dedicated server or player hosted),
  players, version and when each was last seen. Click a row to fill in its code; **REFRESH** reloads
  the list.

While the panel is open, typing goes into its fields and the game does not see it.

## Hosting

1. Title screen → **Multiplayer** → **HOST GAME**.
2. The lobby page opens and the code is copied to your clipboard (it is never shown on screen; the
   **ROOM CODE** button copies it again). Send it to your friends, or tick **PUBLIC** to list the game.
   With a **password**, the code is locked: it is useless without the password, and a public row
   shows `[locked]`.
3. Wait for everyone to appear under **PLAYERS**. Chat works here. Each player has a company chip;
   see [Companies](#companies).
4. Press **START GAME**. The most recent save in your save folder (autosaves count) is sent to
   everyone. When the panel says the save is ready, open **LOAD GAME** and pick **mp_shared**;
   everyone else does the same.

Games load at different speeds; each player's game holds at the start until the host's game is
running, so nobody plays ahead.

## Joining

1. Title screen → **Multiplayer** → **JOIN GAME**.
2. Paste the code, or click a row in **PUBLIC GAMES** (type the password first for a `[locked]`
   row).
3. When the host presses START GAME you receive the save. Open **LOAD GAME** and pick
   **mp_shared**.

**Joining a game that is already running** works too: the host's game saves itself and sends that
save to you, you load it, and your game fast-forwards through what happened since. This is new;
see [KNOWN_ISSUES.md](KNOWN_ISSUES.md).

## Playing on a dedicated relay

A relay is a lobby on a server with no game of its own. Nobody needs an open port, and it keeps
the latest world between sessions. The project runs a public one, which appears in the PUBLIC GAMES
list while it is up.

- The first player to arrive is the **leader**: they have the host role and their game is the
  session's clock. If the leader leaves, the next player in line takes over.
- If the relay already holds a world, it is continued after 10 seconds. The leader can type `/new`
  in chat within that time to discard it and share their own save with START GAME instead.
- While the leader plays, their game uploads a fresh save to the relay every 2 minutes, so a
  player who joins later gets a recent world.
- The world only advances while players are connected.

## In the game

### The Multiplayer window

A small **Multiplayer** window sits at the top left. **Ctrl+Shift+D** hides and shows it. Its
buttons toggle three sections:

- **stats** (hidden at first): a status line that says whether the worlds match. After a desync
  it says what differs (for example "roads and tracks" or "town buildings (5 fewer here)"), when
  it was first noticed, and that the fix is to reload a save the host makes. Below it, one row per
  player: whether that player's world matches, whether their clock is in step, and for your own
  row the speed and anything that needs attention (late or lost commands, vehicles drifting apart).
  **numbers** shows the raw counters (game times, skew, apply lag, drift, balance) for debugging.
- **chat** (shown): the last lines of the lobby chat, and a **say:** field (Enter sends).
- **companies** (hidden at first): see [Companies](#companies).

### Speed and pause

- **Everyone plays at the host's speed.** The host's own speed buttons and pause set the speed for
  the whole session. The other players' speed buttons do nothing while a session runs: the click is
  cancelled, so no game can run off at its own speed.
- A pause is also a sync point: games that are slightly behind run up to the leader's clock before
  they stop.
- `/speed 2.5` in the chat sets a session speed, fractions allowed. Whichever came last counts: a
  host speed button pressed after `/speed` takes over again, and `/speed off` hands the speed back
  to the host's buttons.

### Chat commands

| command | who | does |
|---|---|---|
| `/speed <x>` | anyone | set the session speed (0 < x < 64) |
| `/speed off` | anyone | back to the host's speed buttons |
| `/sync` | anyone | the host's game saves and shares the save (what a hot join does); `/sync off` cancels |
| `/new` | relay leader, before the game starts | discard the relay's stored world |
| `/resume` | relay leader | send the relay's stored world to everyone waiting |
| `/desynclogs always`, `ask`, `never` | anyone | what happens to your logs after a desync (see [When something goes wrong](#when-something-goes-wrong)); `/desynclogs` alone shows the current choice |

### Companies

By default everyone plays one shared company (co-op). To play separate companies, give players
different company chips in the lobby before START GAME: click your own chip to change its number
(the host, or a relay lobby's leader, can change anyone's). Players with the same number share a company; different numbers
are different companies with their own money, and buildings and vehicles stay owned by the company
that built them.

Roadside bus, tram and truck stops belong to the company that placed them, on every player's game,
and that company pays for them. Another company cannot bulldoze them, or replace one by placing its
own stop on the same side of the road; the game tells you whose stop it is. Stops placed before this
version keep whatever owner each game gave them.

In game, the **companies** section of the Multiplayer window shows your company and how many are
in the session. Use **<** and **>** to select one, **switch to it** to play that company instead
of yours, **new company** to start a fresh one, and the **company password** field with **set on
mine** to lock yours (switching into a locked company needs its password).

## Ports and firewalls

- **Most hosts need no port forwarding.** When a friend joins, both lobbies punch through
  their routers to each other with the help of the master server. The lobby also tries UPnP.
- If friends still cannot connect, forward UDP 29471 to your PC on your router, or use a
  dedicated relay. That is needed when the master server is unreachable, or when both
  players are behind a strict (symmetric or carrier-grade) NAT.
- Joiners need no open ports.
- Windows Defender Firewall asks about `netpunch.exe` the first time; allow it.

## When something goes wrong

The panel shows the lobby's status line. The common ones:

| message | meaning |
|---|---|
| `could not reach host` | the lobbies could not punch through (see above), or the code is from a lobby that has closed. Both players need 0.4.16 or later for punching |
| `bad code: this code is locked -- enter the lobby password` / `wrong password for this code` | type the password before pressing JOIN |
| `lobby full` | the lobby has no free seat |
| `host unreachable` / `host closed the lobby` | the host left or lost connection |
| `no players to share with -- wait for a player to join, then press START GAME` | START GAME was pressed with nobody in the lobby |
| `Not shared: '<save>' does not have the Transport Fever 2 Multiplayer mod enabled (see chat)` | the save was made without the mod: load it, enable the mod in its **Mods** panel, save, and press START GAME again |
| `save transfer failed ... -- press START GAME to retry` | a transfer did not verify; the host presses START GAME again |
| `game already started -- ask the host to press START GAME again` | you joined after the start and the host's game is not in game |
| `The lobby stopped before it reported anything -- see tpf2_menu.log` | `netpunch.exe` could not start or exited at once; run the installer's Repair |

Some messages (on a relay, or for a hot join) say the world "loads by itself". It does not: when the
save is ready, open **LOAD GAME** and pick **mp_shared** as usual.

If the Multiplayer window shows **DESYNC**, the worlds have drifted apart and will not come back by
themselves. Players who are already in the game ignore a new shared save (`/sync` only helps
someone joining), so to recover: the host saves, everyone returns to the title menu and leaves
the lobby, and the host hosts again and presses START GAME, which shares that save.

The first desync in a session also opens a **Desync detected** window that asks whether to send
that game's logs to the developers:

- **Always send**: send them now, and in later sessions without asking.
- **Only this once**: send them now and ask again in the next session.
- **Never**: send nothing, and never show the window again on this computer.

A desync keeps being detected until the players reload, so the window appears, and logs are sent,
at most once per session: not again for the same desync, and not after reloading the shared save in
the same lobby.

The logs go to the project's server (the one behind the public games list): the game's log with the
mod's script lines, the mod's DLL logs and files, and the game's graphics settings. Windows user and
computer names, the Steam account number, IP addresses and lobby codes are removed first; player
names and chat can remain. The lobby's own logs and crash dumps are not sent. A chat line from
MULTIPLAYER says when they arrived. Type `/desynclogs always`, `/desynclogs ask` or `/desynclogs never`
in the in-game chat to change your choice; it is kept in `%LOCALAPPDATA%\tpf2mp\data\tpf2mp_prefs.txt`.

For a bug report, send the logs of **every** player. The mod gathers them in one folder,
`%LOCALAPPDATA%\tpf2mp\logs\`:

- Each time the game starts, the previous run's logs are saved there as `<date>-<time>-previous`: the
  game's own log (with the mod's script lines), the logs of the mod's DLLs and the lobby, and the newest
  crash dumps with the game log kept beside each. This happens after a crash too, so after a crash just
  start the game again. The last 2 runs are kept.
- **OPEN LOGS** at the top of the Multiplayer panel copies the running game's logs into a
  `<date>-<time>-now` folder and opens the folder. The last 2 of these are kept as well.

Zip the newest folders and send them. For a fuller report, including a system summary, use `collect_logs.cmd` from the repository's `tools`
folder: double-click it and it zips the data folder, the logs in the game and lobby folders, the
game's log, recent crash dumps and a system summary into `tpf2mp-logs-<computer>-<time>.zip` in your
Downloads folder, without uploading anything. By hand, take the files in `%LOCALAPPDATA%\tpf2mp\data\`
and the game's log, `<Steam>\userdata\<steamid>\1066780\local\crash_dump\stdout.txt`;
`netpunch\lobby_proc.log` next to the game helps with connection problems. The lobby logs contain IP
addresses, so send them privately rather than posting them publicly.
