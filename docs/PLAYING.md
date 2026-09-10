# Playing

## Before you start

- Everyone needs Windows, the Steam version of Transport Fever 2 and the **same version** of
  `TpF2Multiplayer.msi` installed (see the [README](../README.md#install)).
- Every mod the save uses must be installed on every machine. The multiplayer mod itself
  (**MP Lockstep**) comes with the installer.
- **MP Lockstep has to be enabled in the save.** New games get it automatically: each time the
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
- **PUBLIC GAMES**: the games currently listed, with host, save, players, version and age. Click a
  row to fill in its code; **REFRESH** reloads the list.

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

- **stats** (hidden at first): a column per player with game time, skew, speed, queued and
  late commands, desyncs, apply lag, vehicle drift and balance, plus the **verdict**: `SYNC`
  while the worlds agree, `DESYNC ...` naming what differs.
- **chat** (shown): the last lines of the lobby chat, and a **say:** field (Enter sends).
- **companies** (hidden at first): see [Companies](#companies).

Below the toggles is the speed row: **session speed**, then what your own game is doing to stay
in step ("in step with the leader", "catching up", "easing off", or "this game leads the clock"),
and four buttons: **-0.5** and **+0.5** set a session speed for everyone, **reset** returns to the
normal rule, and **sync** starts a fresh shared save (as `/sync`).

### Speed and pause

- **Everyone plays at one speed.** Normally it is the lowest speed any player has selected with the
  game's own speed buttons. So when anyone pauses, everyone pauses; when everyone unpauses, the
  game resumes.
- A pause is also a sync point: games that are slightly behind run up to the leader's clock before
  they stop.
- `/speed 2.5` (or the -0.5/+0.5 buttons) sets a session speed, fractions allowed; `/speed off`
  (or **reset**) goes back to the speed buttons.

### Chat commands

| command | who | does |
|---|---|---|
| `/speed <x>` | anyone | set the session speed (0 < x < 64) |
| `/speed off` | anyone | back to the lowest player's speed button |
| `/sync` | anyone | the host's game saves and shares the save (what a hot join does); `/sync off` cancels |
| `/new` | relay leader, before the game starts | discard the relay's stored world |
| `/resume` | relay leader | send the relay's stored world to everyone waiting |

### Companies

By default everyone plays one shared company (co-op). To play separate companies, give players
different company chips in the lobby before START GAME: click your own chip to change its number
(the host, or a relay lobby's leader, can change anyone's). Players with the same number share a company; different numbers
are different companies with their own money, and buildings and vehicles stay owned by the company
that built them.

In game, the **companies** section of the Multiplayer window shows your company and how many are
in the session. Use **<** and **>** to select one, **switch to it** to play that company instead
of yours, **new company** to start a fresh one, and the **company password** field with **set on
mine** to lock yours (switching into a locked company needs its password).

## Ports and firewalls

- **Hosting needs UDP port 29471 reachable from the internet.** The lobby tries to open it through
  UPnP. If friends cannot connect, forward UDP 29471 to your PC on your router, or use a dedicated
  relay.
- Joiners need no open ports.
- Windows Defender Firewall asks about `netpunch.exe` the first time; allow it.

## When something goes wrong

The panel shows the lobby's status line. The common ones:

| message | meaning |
|---|---|
| `could not reach host` | the host's port is not reachable (see above), or the code is from a lobby that has closed |
| `bad code: this code is locked -- enter the lobby password` / `wrong password for this code` | type the password before pressing JOIN |
| `lobby full` | the lobby has no free seat |
| `host unreachable` / `host closed the lobby` | the host left or lost connection |
| `no players to share with -- wait for a player to join, then press START GAME` | START GAME was pressed with nobody in the lobby |
| `save transfer failed ... -- press START GAME to retry` | a transfer did not verify; the host presses START GAME again |
| `game already started -- ask the host to press START GAME again` | you joined after the start and the host's game is not in game |
| `The lobby stopped before it reported anything -- see tpf2_menu.log` | `netpunch.exe` could not start or exited at once; run the installer's Repair |

Some messages (on a relay, or for a hot join) say the world "loads by itself". It does not: when the
save is ready, open **LOAD GAME** and pick **mp_shared** as usual.

If the Multiplayer window shows **DESYNC**, the worlds have drifted apart and will not come back by
themselves. Players who are already in the game ignore a new shared save (`/sync` only helps
someone joining), so to recover: the host saves, everyone returns to the title menu and leaves
the lobby, and the host hosts again and presses START GAME, which shares that save.

For a bug report, the quickest way is to double-click `tools\collect_logs.cmd`: it gathers everything
below (plus recent crash dumps and a system summary) into `tpf2mp-logs-<computer>-<time>.zip` in your
Downloads folder, without uploading anything. By hand: attach the files from `%LOCALAPPDATA%\tpf2mp\data\` and the game's log
`<Steam>\userdata\<steamid>\1066780\local\crash_dump\stdout.txt` from **every** player, collected
**before** restarting the game (the game truncates its log on launch). `netpunch\lobby_proc.log`
next to the game helps with connection problems but contains IP addresses; check before posting it
publicly.
