# Playing

## Before you start

- Everyone needs Windows, the Steam version of Transport Fever 2 and the **same version** of
  `TpF2Multiplayer.msi` installed (see the [README](../README.md#install)).
- The host advertises the save's required mods. Missing mods are offered in a **Download Mods / Cancel** dialog; Cancel leaves the lobby. **Auto-accept mod downloads** in the multiplayer menu saves your choice for future joins and hotjoins. Downloads must be recognised by the game before it loads the save.
- Deluxe and Early Supporter content are DLC, never transferred. Each player must have the required DLC installed.
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
4. Click **SELECT SAVE** in the lobby and choose your world. The list includes autosaves,
   shows modification dates, and puts the newest files first. Use **NEXT** / **PREVIOUS**
   for more saves, or **REFRESH** after saving a new world. The chosen filename stays visible
   in the lobby. The save must have the Multiplayer mod enabled.
5. Press **START GAME** to send the selected save to everyone. If the file was removed,
   choose another save; the lobby never silently substitutes a different world. With automatic
   loading disabled, open **LOAD GAME** and pick **mp_shared** when the save is ready.

Hosting from an already running world continues to share a fresh snapshot of that world.

Games load at different speeds; each player's game holds at the start until the host's game is
running, so nobody plays ahead.

**Changing world mid-session.** Whatever the host loads, everyone loads. If the host uses the
game's own **LOAD GAME** (from the title screen or from the in-game menu), or starts a **NEW GAME**
or **CONTINUE**, that world is sent to every player and their games load it where they are — no
one has to go back to the title screen. Each player then catches up from the host as if they had
just joined.

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
- If the relay already holds a world, it loads for everyone as soon as the leader arrives. To start
  from your own save instead, the leader types `/new` in chat before it is sent (a few seconds), then
  presses **START GAME**.
- If it does not, the panel asks the leader to press **START GAME**, which sends the leader's most
  recent save to the relay and on to everyone.
- While the leader plays, their game uploads a fresh save to the relay every 2 minutes, so a
  player who joins later gets a recent world.
- The world only advances while players are connected.

## In the game

### The Multiplayer window

A small **Multiplayer** window sits at the top left. **Ctrl+Shift+D** hides and shows it. Its
buttons toggle three sections:

- **lobby** (hidden at first): show or hide the session name, host, connected players and their companies. Available to hosts and joiners during play; larger rosters have previous/next page buttons. Your visibility choice stays in place when the roster changes.
- **stats** (hidden at first): a status line that says whether the worlds match. After a desync
  it says what differs (for example "roads and tracks" or "town buildings (5 fewer here)"), when
  it was first noticed, and that the fix is to reload a save the host makes. Below it, one row per
  player: whether that player's world matches, whether their clock is in step, and for your own
  row the speed and anything that needs attention (late or lost commands, vehicles drifting apart;
  that vehicle check turns off for good once the world has more than 200 vehicles).
  **numbers** shows the raw counters (game times, skew, apply lag, drift, balance) for debugging.
- **chat** (shown): the last lines of the lobby chat, and a **say:** field (Enter sends).
- **companies** (hidden at first): see [Companies](#companies).
- **speed** (shown): your speed vote (1 to 4.5, and -0.25 / +0.25 from your last vote), with the
  session speed and the votes it is the average of. See [Speed and pause](#speed-and-pause).

### Speed and pause

- **The session runs at the average of everyone's speed votes.** Pressing a speed button, in the
  game's own clock or in the Multiplayer window's speed row, is your vote. It does not change your
  game's speed by itself: the click is cancelled, every game records your vote at the same moment,
  and the whole session then runs at the average of the votes, rounded to 0.05. For example, the
  host at 4x and one player voting 1x gives 2.5x.
- Until you vote you have no say, except the host, whose own speed counts until they vote. A player
  who leaves stops counting about 30 seconds later.
- **Only the host pauses.** The host's pause stops the whole session and the host's play resumes it
  at the votes' speed. Resuming with the pause button is not a vote; resuming with a speed button
  also votes for that speed. Other players' pause buttons do nothing.
- A pause is also a sync point: games that are slightly behind run up to the leader's clock before
  they stop.
- **Far behind, your actions are off.** If your game falls more than 15 game units behind the
  fastest other game, anything you build, buy or edit is cancelled until it is back within 2 units,
  and the top of the Multiplayer window says so. A line you create is kept and made once you have
  caught up. Bulldozing a road or track still goes through, and speed buttons and pause still work.
- `/speed 2.5` in the chat sets a session speed for everyone, fractions allowed, overriding the
  votes. Whichever came last counts: a vote after `/speed` hands the speed back to the votes, and
  so does `/speed off`.

### Chat commands

| command | who | does |
|---|---|---|
| `/speed <x>` | anyone | set the session speed until the next speed vote (0 < x < 64) |
| `/speed off` | anyone | back to the average of the speed votes |
| `/sync` | anyone | the host's game saves and shares the save (what a hot join does); `/sync off` cancels |
| `/new` | relay leader, before the world is sent | discard the relay's stored world; START GAME then shares your own save |
| `/desynclogs always`, `ask`, `never` | anyone | what happens to your logs after a desync (see [When something goes wrong](#when-something-goes-wrong)); `/desynclogs` alone shows the current choice |

### Companies

By default everyone plays one shared company (co-op). The host's **SEPARATE COMPANIES** checkbox
(on the HOST A GAME card, and in the lobby) gives every player their own company instead: the host
is company 1 and each joiner gets the next number, including players who join later. Turning it
off puts everyone back on company 1. A relay lobby's leader has the same checkbox.

The chips can still be set by hand before START GAME: left-click your own chip for the next
company number, right-click for the previous (the host, or a relay lobby's leader, can change
anyone's). Players with the same number share a company; different numbers are different
companies with their own money, and buildings and vehicles stay owned by the company that built
them.

Roadside bus, tram and truck stops belong to the company that placed them, on every player's game,
and that company pays for them. Another company cannot bulldoze them, or replace one by placing its
own stop on the same side of the road; the game tells you whose stop it is. Stops placed before this
version keep whatever owner each game gave them.

In game, the **companies** section of the Multiplayer window shows your company's colour and
name. Pick a company from the dropdown and **switch to it** to play that company instead of yours,
**new company** to start a fresh one, and use the **company password** field with **set on mine**
to lock yours (switching into a locked company needs its password). A company is named in the
game's own company window; until then it is named after the player who founded it: "<player>'s
company", then "<player>'s 2nd company" and so on, whoever plays it now.

### AutoSig2

With AutoSig2 enabled, its existing controls work through multiplayer replay:
automatic placement with the selected spacing, **Replace** and **Remove**, and
the **Forward/Backward** direction for those two modes. Place the initial signal
as usual. Its settings are captured with the click; changing controls while the
command is in flight does not change that action.

The initial signal and its follow-up actions appear after the lockstep delay.
Replace/remove follow AutoSig2's route rules, including its branch, station and
route-length limits. As in AutoSig2, the temporary initial signal is removed in
these modes. A replacement uses the selected model and one-way setting. Signals
belonging to another company and waypoints are not editable AutoSig targets;
a plan containing one is refused. A target changed before replay is skipped,
never substituted with a nearby object.

AutoSig2 must be installed and enabled separately; this adapter does not bundle
or edit the Workshop mod. Every participant needs the same multiplayer build.

## Ports and firewalls

- **Steam carries the connection when nothing else does.** Since 0.6.1.15 the mod also connects through Steam's own networking (the same thing Steam games use for invites): the host's SteamID is in the code, and Steam punches through or relays on its own. Both players must be running the game through Steam, logged in.
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

When a shared save is ready it loads by itself. If it does not (the panel says so), open
**LOAD GAME** and pick **mp_shared**.

If the Multiplayer window shows **DESYNC**, a Resync section appears at its top: press **Resync now** once.
In a two-player host lobby, this pauses both games, saves and transfers the host world,
reloads both players and compares the fresh worlds before resuming. An intentional
pause is preserved. Client-only changes are discarded. See [One-click recovery](RESYNC.md)
for progress, retry and supported-session limits. `/sync` serves joining players; it does not reload
players already in the game.

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
