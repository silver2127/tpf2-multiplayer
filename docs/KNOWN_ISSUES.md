# Known issues and open work

State of the code on `dev` at 0.6.1.11 (2026-09-20). Items marked *from the code* were found by
reading the source and have not been reproduced in a game; the rest were observed in play. When
something here is fixed, the release note says so and the entry goes.

## Lockstep and pacing

- **The leader role does not move** *(from the code)*. Only the current leader reads the lobby's
  leader letter, so an instance named leader later (a relay lobby after its first leader leaves,
  or one whose first leader is not letter `a`) never learns it, and no instance acts as the
  session clock. See [ARCHITECTURE.md](ARCHITECTURE.md#pacing-and-game-speed).
- **History sent to a catching-up follower breaks parameterised commands** *(from the code,
  re-checked 2026-09-20)*. The history pump appends `hist=1 hfor=<letter>` after the line's
  greedy `params=` tail, so constructions and upgrades replayed from history arrive with the
  tail inside their parameters and without their history flag. A frozen join (0.6) loads a
  save instead of history, so this only reaches a follower that fell more than 8 units behind
  during play.
- **Far behind, actions are off.** More than 15 units behind the fastest game, a player's builds,
  buys and edits are cancelled until they are back within 2 units. By design, but it surprises.

## Native side

- **The OpenGL renderer is new and untested in a game** *(2026-09-21)*. Until then the Multiplayer
  window drew only on Vulkan: on OpenGL the MULTIPLAYER button opened a window that never
  appeared, and the lobby's per-frame work never ran (reported: "my son's multiplayer button
  ... doesn't click"). The menu DLL now also hooks SDL2's `SwapBuffers` import and draws the
  same panel with a framebuffer blit (`native/src/gl_overlay.h`, tested offline by
  `tools/test_gl_overlay.py`). `tpf2_menu.log` says `OpenGL: hooked SDL2's SwapBuffers import`
  at start and `OpenGL overlay off: <why>` if the driver refuses it; Vulkan is unchanged.
- **After a crash, the restarted game can lose its instance letter.** The slice reads
  `tpf2_instance.txt` at attach; when the file still names the crashed process (the bridge
  rewrites it a moment later), the slice refuses the letter and logs `identity file pid=... !=
  mine ... -- refusing instance letter`. From then on that game's captures are not shipped:
  everything the player does replicates nowhere, with no other symptom. Seen in the logs of
  [issue #7](https://github.com/silver2127/tpf2-multiplayer/issues/7). Restarting the game
  once more clears it. Fix pending: re-read the identity until the bridge has written it.
- **A host crashed when creating a line** ([issue #7](https://github.com/silver2127/tpf2-multiplayer/issues/7),
  0.6.1.9, two players through the master's relay). Not reproduced on the rig; the crashing
  run's own logs (`%LOCALAPPDATA%\tpf2mp\logs\<stamp>-previous\`) and the game's crash dump are
  still needed.
- **A crash near another company's stops** in companies mode has been seen once and its cause
  is unproven (`GetComponentDataIndex` with an out-of-table entity while a roadside stop was
  being replaced). [SHARED_INFRA.md](SHARED_INFRA.md) has the repro plan.
- **Only one instance per Windows session gets the slice hooks.** A second, un-sandboxed game in
  the same session skips them silently (a named mutex). The rig runs its other instances in
  Sandboxie boxes for that reason ([DEVELOPMENT.md](DEVELOPMENT.md)).
- **A level crossing over track on an embankment above the road** fails the engine's "Too much
  slope" check on every instance and is refused rather than built on one.
- **A four-arm track crossing built as one proposal asserts the engine** and freezes the game;
  build it as two straight lines.
- **The second joiner on one machine loses its panel.** The overlay's present hook faults, is
  caught, and stops drawing; joining, the save transfer and loading still work.

## Lobby and relay

- **A dedicated relay hangs after 702 distinct player names** *(from the code)*. Letters run
  `a`..`zz`; the letter allocator then loops forever. Letters are kept per name in
  `relay_letters.json` across restarts, so a long-running public relay will get there.
- **Joiners on one LAN rarely link directly** *(from the code)*. The panel starts joiners with
  `--local-port 0`, so a joiner's LAN candidate carries port 0 and joiners behind the same
  router reach each other only through NAT hairpinning, otherwise via the host.
- **A relay lobby's leader cannot toggle PUBLIC** *(from the code)*: the lobby ignores the
  `publish` command from anyone but a host.
- **`lobby_proc.log` contains addresses** from the connection phase; the merged
  `lobby_peers.log` redacts its own lines only. Mind that when attaching logs to a report.
- **A player already inside a game loads the shared save by hand.** At the title menu the panel
  starts it in-process; from inside a game it asks for LOAD GAME, then `mp_shared`.
- **The relay must be redeployed for every release.** The version gate is an exact string, so a
  relay one version behind refuses every joiner ([version-checking.md](version-checking.md)).

## Replication gaps

- Not replicated: vehicle stop/start, manual departure, "depart now" and maintenance targets;
  map editor and scenario commands. [REPLICATION.md](REPLICATION.md#not-replicated).
- **A stop's load settings are not carried by line replays** *(from the code, 2026-09-20)*.
  Each line stop has a `stopConfig` (unload only, maximum load) that the line decoder does not
  read and the replay does not set, so a replayed line edit resets those settings on every
  instance, the originator included.
- **Replacing a stop on an occupied side is not strict**: the engine re-points the old stop's
  lines, which a script proposal cannot express, so the poll ships it after the fact and the
  originator re-ships every affected line.
- **A buy, sale or replacement whose data the slice cannot read runs on the player's game
  only** and is logged as such.
- **Placing a roadside stop rebuilds the edge** and the rebuild does not carry every edge
  object; a native edge-object path is the planned fix.
- **Demolishing a construction is not fully strict**: the originator's refund and the passengers
  inside are reconciled after the fact, not applied at the stamp.
- **Shipped in 0.6.1.11 without a session test yet**: the Snowball Fences replay, the road
  ownership tool, action sounds, the automatic stop fallback when a train is assigned, and the
  tighter (1 m) check that decides whether a tracked construction was replaced or removed. A
  station module upgrade is the first thing to try on that last one.
- **Platforms.** A station clicked into a line gets the platform the game's own shortest-loop
  search picks, at the stamp, on every instance. That search has no side-of-track preference:
  on plain double track it is platform 0 in both directions, which is what vanilla does too.
  Lines made before 0.6.1.11 keep the platforms they had.
- The detector's vehicle count does not include vehicles parked in depots (by design; they are
  not world entities).
- The line route overlay stays stale after a stop is removed until the line is reselected (base
  game, cosmetic).

## Not built

Plans that were written up and not implemented. The full texts were removed with the old docs
and remain in git history at the `v0.4.10` tag:

| plan | where |
|---|---|
| Steam invites and a Steam Networking relay fallback | `git show v0.4.10:docs/STEAM_TRANSPORT_PLAN.md` |
| Strict mode for the remaining actions (vehicle flags, loans through the `Book` factory, in-place node edits) | `git show v0.4.10:docs/STRICT_LOCKSTEP_PLAN.md` |
| Hardening strict station placement and module edits (lossy-walk refusal, per-file allowlist, broadcast rollback) | `git show v0.4.10:docs/STATION_STRICT_PLAN.md` |
| Peer-to-peer distribution of the save (the unwired prototype `swarm.py` is at the same tag; it was deleted on 2026-09-10) | `git show v0.4.10:docs/SWARM_TRANSFER.md` |
| Notes toward a Transport Fever 3 port | `git show v0.4.10:docs/tf3_plan.md` |

The in-game updater (0.5.x to 0.6.1.9) was removed in 0.6.1.11, not for lack of use but because
mod-hosting sites do not allow a mod that updates itself; a new version is installed over the
old one. The first-phase milestone reports (M1-M10), the original recon report and the old status
logs are at the same tag: `git show v0.4.10:docs/history/README.md`.
