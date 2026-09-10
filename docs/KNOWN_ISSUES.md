# Known issues and open work

State of the code at version 0.4.10. Items marked *from the code* were found by reading the
source while these docs were rewritten and have not been reproduced in a game; the rest were
observed in play.

## Lockstep and pacing

- **The clock barrier never holds** *(from the code)*. In `CM.barrierTick` a gap larger than
  `K.CATCHUP_MIN` (8 units) is treated as "that peer is catching up" and zeroed before the
  `ahead > K.BARRIER_AHEAD` test, and `BARRIER_AHEAD` is also 8, so the test can never pass.
  Instances are kept together only by pacing and catch-up. The command-gap hold behind
  `strict_barrier=1` still works.
- **Hot-join catch-up does not start for a newcomer more than 60 units behind** *(from the
  code)*. `CM.paceV2` returns early when the spread between clocks exceeds 60 units, before it
  reaches `CM.catchUpTick`, so no history is requested. A save a few minutes old is usually
  further behind than that.
- **The leader role does not move** *(from the code)*. Only the current leader reads
  `tpf2_bridge_ctl.txt` (`CM.speedRequest` is called from the leader's branch), so an instance
  named leader later (a relay lobby after its first leader leaves, or one whose first leader is
  not letter `a`) never learns it, and no instance acts as the session clock.
- **`/pid` has no effect** *(from the code)*. The leader does not run the PID controller,
  joiners never read the ctl file, and the ctl's first `pid=` line is the bridge's process id,
  which the parser picks up instead of the gains.
- **History sent to a hot joiner corrupts parameterised commands** *(from the code)*.
  `CM.histPump` appends `hist=1 hfor=<letter>` after the line's greedy `params=` tail, so
  constructions, upgrades and road demolitions replayed from history arrive with broken
  parameters and without their history flag.
- **`CM.histFind` can answer a NACK with the wrong command** *(from the code)*: it
  substring-matches `origin=a seq=1`, which also matches `seq=12`.
- **The load-time world integrity check never runs** *(from the code)*. It requires
  `CM.ticks == 30` inside a block that only runs every 60 ticks, so damaged road edges (no
  TransportNetwork component) are not reported.
- **Company colours are skipped for strict purchases** *(from the code)*.
  `CM.cmColorNewVehicle` is only called on the non-strict buy path, and `strict_buy=1` is the
  default.

## Native side

- **The bridge's UDP socket accepts packets from anyone** *(from the code)*. It binds all interfaces
  and checks only the packet magic (`net.cpp`), so a reachable port 7771 lets an outsider inject game
  commands that the lobby's sealing never sees. See [SECURITY.md](SECURITY.md#what-it-does-not-do).
- **The legacy save server still listens** *(from the code)*. Instance `a` serves its newest save on
  TCP 7871, all interfaces, without authentication (`savexfer.cpp`); the lobby's transfer replaced it.
- **The bridge link is single-sender** *(from the code)*. With three or more players one bridge
  socket hears several senders; each change of sender resets its sequence state, so the link's
  ordering, de-duplication and chunk reassembly cannot be relied on and recovery falls to the mod's
  NACK layer.
- **`BuyVehicle` is hooked twice.** The bridge's diagnostic buy probe and the slice both patch
  `0x9dca00` from separate init threads; whichever comes second finds the other's jump, and the bridge
  logs `REFUSING to patch` when it loses. The probe's output (`tpf2_buy_<L>.txt`) has no reader;
  `buy_hook=0` in `tpf2_bridge_mp.cfg` removes it.
- **`/pid` text starting with a digit blinds every bridge** *(from the code)*. The menu writes
  `pid=<text>` into `tpf2_bridge_ctl.txt` after the bridge's own `pid=<process id>`, and a bridge that
  reads a numeric `pid=` that is not its own ignores the whole file (including a later letter or peer
  change).
- **With no `tpf2_slice.cfg` at all, construction cancels are off** while the shipped file has
  `cancel_construction=1`: the slice's built-in list of "shipped" switches omits it.
- **Only one instance per Windows session gets the slice hooks.** A second, un-sandboxed game in the
  same session skips them silently (a named mutex).

## Lobby and relay

- **A normal host re-sends its old save to late joiners** *(from the code)*. Once a second the
  host pushes the last START GAME save to every player who has not started. For a hot joiner
  that push can win the race against the host's fresh autosave, which is then ignored ("a save
  transfer is in progress") or finds nobody waiting.
- **A dedicated relay hangs after 702 distinct player names** *(from the code)*. Letters run
  `a`..`zz`; `_origin_letter` repeats after that and `letter_for` loops forever. Letters are
  kept per name in `relay_letters.json` across restarts, so a long-running public relay will
  get there.
- **Joiners on one LAN rarely link directly.** The panel starts joiners with
  `--local-port 0` and `observe()` advertises that requested port, so every joiner's LAN
  candidate is `<lan ip>:0`. Joiners behind the same router then reach each other only through
  NAT hairpinning, otherwise via the host.
- **A stray password hangs a join** *(from the code)*. A joiner with text in the PASSWORD field
  who joins an unlocked code derives the wrong key; the host's plaintext "wrong password" reply
  is dropped by the mesh transport, and the panel stays at "joined the host".
- **The player cap is theoretical.** The roster, with every joiner's profile and mesh links,
  is one UDP datagram; a fully meshed lobby outgrows it at around 45-60 joiners and the send
  error is swallowed.
- **`lobby_proc.log` contains IP addresses** from the connection phase; only the lobby's own log
  lines are redacted.
- **A panel quit can skip clean-up.** The menu kills the lobby 1.5 s after asking it to quit;
  de-listing from the master server and removing the UPnP mapping can take longer.
- **The second joiner on one machine loses its panel.** The overlay's present hook faults
  (`myPresent FAULT exc=c0000005`), is caught, and stops drawing; joining, the save transfer
  and loading still work.
- **Panel messages claim the world loads by itself** (relay leader, hot join). Loading is always
  manual: LOAD GAME → mp_shared.
- **A code taken straight from the clipboard with trailing whitespace is rejected** *(from the
  code)*: pressing JOIN GAME without first clicking the code field validates the clipboard text before
  trimming its end. Clicking the field pastes a cleaned copy.
- **A relay lobby's leader cannot toggle PUBLIC** *(from the code)*: the lobby ignores the `publish`
  command from anyone but a host.

## Replication gaps

- Not replicated: terraforming and painting, vehicle stop/start, manual departure and
  "depart now", map-editor commands. Maintenance targets only with `maint=1`.
- Replacing a stop on an occupied side is not strict (the poll ships it after the fact).
- A bought or replaced vehicle's `reversed` flag is not decoded; in companies mode a
  replacement's cost is not moved to the owning company.
- A level crossing over track on an embankment above the road fails "Too much slope"; it is
  refused on every instance rather than built on one.
- The detector's vehicle count does not see vehicles parked in depots.
- The line route overlay stays stale after a stop is removed until the line is reselected (base
  game, cosmetic).

## Not built

Plans that were written up and not implemented. The full texts were removed with the old docs
and remain in git history at the `v0.4.10` tag:

| plan | where |
|---|---|
| Steam invites and a Steam Networking relay fallback | `git show v0.4.10:docs/STEAM_TRANSPORT_PLAN.md` |
| Strict mode for the remaining actions (vehicle flags, loans through the `Book` factory, terraform, line creation, in-place node edits) | `git show v0.4.10:docs/STRICT_LOCKSTEP_PLAN.md` |
| Hardening strict station placement and module edits (lossy-walk refusal, per-file allowlist, broadcast rollback) | `git show v0.4.10:docs/STATION_STRICT_PLAN.md` |
| Peer-to-peer distribution of the save (`netpunch/swarm.py` exists, unwired) | `git show v0.4.10:docs/SWARM_TRANSFER.md` |
| Loading a save in-process through `CMenuUI::StartSavegame` | [re/GAME_LOOP_AND_UI.md](re/GAME_LOOP_AND_UI.md#loading-a-save) |
| Notes toward a Transport Fever 3 port | `git show v0.4.10:docs/tf3_plan.md` |

The first-phase milestone reports (M1-M10), the original recon report and the old status logs
are there too: `git show v0.4.10:docs/history/README.md`.
