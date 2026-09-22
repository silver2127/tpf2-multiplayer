## 0.7

**Big Maps is now part of TpF2 Multiplayer, and joining a running game no longer desyncs it**

> **Everyone needs 0.7**, including the dedicated server. If you had the separate
> *TpF2 Big Maps* installer, this one replaces it: it removes the old package and
> installs Big Maps itself.

### What changed

- **Big Maps ships in this installer.** Maps larger than the New Game menu offers
  (the extra size rows and the six extra town and industry levels) are installed
  with multiplayer as `plugins\tpf2_bigmap.dll` + `plugins\tpf2_bigmap.cfg`. An
  installed *TpF2 Big Maps* is removed automatically, so the plugin has one owner.
  Uninstalling puts the game's own `base_mod.lua` back, as before.
- **Hot join keeps the worlds in step.** A world that kept running and the same
  world loaded from its save used to make different decisions (where people go,
  which town grows next, which new ids things get) because the engine walks its
  entity lists in history order. Every list the simulation reads is now in entity
  order at each step on every player, so a joiner who loads the host's save makes
  the same decisions the host does. In testing a live-joined pair stayed identical
  for hundreds of game units after the join.
- **Live join (experimental, opt-in on the host):** with `tpf2mp_live_join.txt`
  containing `1` in the host's `netpunch` folder, a player who joins a running
  game no longer pauses everyone: no hold, no "checking that all worlds match"
  window. The newcomer loads the host's hot-join save and catches up on the
  command history while the others keep playing. Dedicated servers too.
- **The TCP backup link works for a renamed joiner.** Two players with the same
  name (the second shows as `Name#2`) lost the TCP backup link; it now connects.
  A joiner who comes in through Steam also opens its router port (UPnP) so the
  host can reach it over TCP, and failed connections say why (firewall or nothing
  listening).
- **The in-game Multiplayer window no longer shows the wrong player.** It could
  start with the previous session's player letter ("B (you)", speed "-", no other
  players) and drop your clicks; it now re-checks its identity every couple of
  seconds.
- **Input during a resync:** the game no longer blocks all input while a resync
  holds, only while our own save runs.
- **Mods:** the mod registry names only the Workshop mods the save uses, not every
  one you have subscribed to.
- **Big Maps stalls:** the terrain and material pagers only throttle while a world
  is loading, so a PC short on memory no longer freezes in play.

### How to test

1. Close the game, run the MSI (or the Proton installer on Linux / Steam Deck).
2. Big Maps: **New Game** shows the extra size rows after the stock sizes.
3. Hot join: host a game, let a friend join while it runs, play on for a while;
   the Multiplayer window's **Status** tab should keep saying the worlds match.
   To try live join, put `1` in `netpunch\tpf2mp_live_join.txt` on the host.

### Known limitations

- Live join is new: if a joined game drifts apart, the log line `DESYNC ... DIFFERS`
  names the part of the world that differs; please send both players' logs.
- Waiting passengers at a stop are still queued in arrival order, which a loaded
  world does not know; a busy stop right after a join can still differ.
- An install over the old *TpF2 Big Maps* package works through Windows
  Installer's upgrade table and has not been tried on a player's PC yet.
