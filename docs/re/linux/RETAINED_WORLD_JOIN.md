# Retaining the server world during a join

Investigation: 2026-09-22, native build 35924.

## Decision

Do not remove the host's load from `SyncParticipant` yet. This is a simulation
correctness dependency, not an unnecessary wait. The private native trial below
completed its join but subsequently diverged. No retained-world join is enabled
on the production server.

`docs/RESYNC.md` records the earlier retained-host experiment on September 16:
two identical replays diverged in the person simulation within about 35 game
units, followed by buses diverging at a stop. Loading the same snapshot on
every participant avoided that failure. These are prior recorded results;
the new trial below independently found a person-count difference, at a later
interval.

## Actual native trial, September 22

Two isolated native build 35924 instances on the VPS used the current Linux
performance build, with terrain caches off. The host alone used
[`retained-host-trial.patch`](retained-host-trial.patch) and
`TPF2MP_EXPERIMENT_RETAIN_HOST=1`. This is an archived research patch, not an
installed runtime feature. It skips only a join-mode host load, after the
transport epoch reset, requiring the same world token, current held/paused Lua
acknowledgement and idle native controller. Ordinary client loads and the full
paused fingerprint comparison remain in place.

The first attempt exposed a stale roster: the held Lua pump skipped normal
roster polling, retained `rosterPlayers=1`, and abandoned the operation during
transfer despite the lobby having two members. `syncAlone()` now reads the
PID-scoped bridge roster directly. Its regression test covers a cached solo
roster during a 100-tick hold, a foreign PID and a real solo roster. This fix is
independent of the experimental reload bypass.

After restarting with that correction:

- Host PID 546488 retained world token `1790099245table0x7f35af873e90`.
- Client PID 547074 loaded a new world. Both paused fingerprints matched at
  game time **6060.6**, including **2,298 people**, two vehicles and 4,544 edges.
- The barrier completed and simulation resumed. The save carried a 144-unit
  hash interval; the test invoked the existing leader cadence mechanism to
  schedule four-unit checks, applied on both sides at 6135.6 without lateness.
- Seven full hash/detail samples matched at 6136 through 6160 inclusive.
- At **6164.0**, the retained host reported **2,390 people**, the loaded client
  **2,388**. At **6168.0**, the counts were **2,392** and **2,391** respectively.
  Vehicle position hashes and the main verdict hash still matched. Person
  count is a diagnostic lane, so the dashboard still reported zero desyncs.

The first observed difference was **103.4 game units after the paused snapshot**.
There were no four-unit samples before 6136, so this is not an exact first
divergent engine step. A successful paused comparison and a matching main hash
are insufficient acceptance criteria. This run fails the retained-world gate.
It does not isolate the engine subsystem causing the difference; no new paired
all-reload control was run in this trial.

Evidence: [`retained-host-trial-result.json`](retained-host-trial-result.json)
contains both final statuses and all nine paired samples. Full logs and the
per-second acknowledgement capture are preserved on the VPS under
`/opt/tpf2mp-linux-parity-20260921/retain-host-experiment/`.
Both private games and X servers were stopped and their trial overrides restored.
Production remained configured to reload the host; Steam offline mode was
verified afterward. No join-time speedup is claimed from this failed trial.

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
