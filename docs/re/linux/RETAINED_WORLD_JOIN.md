# Retaining the server world during a join

Investigation: 2026-09-22, native build 35924.

## Decision

Do not remove the host's load from `SyncParticipant` yet. This is a simulation
correctness dependency, not an unnecessary wait. No retained-world join has
been deployed by this investigation.

`docs/RESYNC.md` records the earlier retained-host experiment on September 16:
two identical replays diverged in the person simulation within about 35 game
units, followed by buses diverging at a stop. Loading the same snapshot on
every participant avoided that failure. These are prior recorded results;
they have not been reproduced in this investigation.

## Dependencies confirmed in current code

1. `netpunch/sync_runtime.py` deliberately loads the host for start, join and
   resync, and requires a new Lua world token before acknowledging loading.
2. The native bridge's `ResetWorldFiles` clears transport files and rotates
   the network epoch. It does not rebuild engine systems or reset the running
   Lua module's state.
3. `mod/mp_lockstep_1/res/scripts/mp/net.lua` retains per-world command sequence
   numbers, retransmit rings, received-command tracking and history in memory.
   `resync.lua` holds producers and fingerprints the world; it has no complete
   retained-world epoch transition.
4. Native `slice/movement_linux.cpp` canonicalizes road edge entries to fix one
   ordering-sensitive tie. The boot adapters also reproduce Windows building
   registration, person maps, target sets and persistent route-index behavior.
   Those cross-platform compatibility fixes are not proof that a running world
   and its reloaded snapshot have identical insertion histories, scheduling
   cursors or all engine state rebuilt by loading. The recorded retained-host
   failure was between matching game builds, not merely a Linux/Windows
   container difference.

An equal paused fingerprint cannot establish that two worlds will make the
same future choices. Skipping a load after only that check would repeat the
known failure mechanism.

## Viable research paths and acceptance gate

- Find the first differing person/town system iteration after save/load, then
  preserve its ordering in the snapshot or normalize it identically on both
  platforms. Check scheduling cursors, RNG and other state omitted from saves;
  sorting entity IDs alone is not demonstrated to be sufficient.
- Implement an explicit retained-world Lua/transport epoch transition with
  drained producers, sequence/history reset and fresh acknowledgements.
- Compare two native instances using one retained world and one loaded copy,
  then repeat with a Windows client. Run past the previously observed failure
  interval and exercise births, removals, construction, vehicle arrivals and
  subsequent joins. Any mismatch must keep the barrier held and trigger the
  ordinary shared reload, never release an unverified session.

A narrower optimization for a newcomer arriving during an existing held round
would still require the explicit epoch transition. It would avoid a redundant
second reload only, not the first reload after normal gameplay. It is not
implemented or presented as a general retained-world join.

The separate client test machine is offline. The VPS can benchmark native
loading, but that does not substitute for cross-platform join acceptance.
