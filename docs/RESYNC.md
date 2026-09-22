# One-click recovery after a desync

After a desync, the host starts a coordinated save, transfer and reload in the
existing game processes. Groups of three or more confirm readiness first. Local
two- and three-game acceptance results are recorded below.

When a desync is detected, only the host can initiate recovery. With two players,
the host presses **Resync now**. With three or more, the host presses **Request
readiness**; this counts as the host's confirmation. Every client must then press
**Ready** in the same panel. It shows the ready count and starts automatically
only when every current participant has confirmed. No new save/load or recovery
hold is initiated before that point. A changed roster invalidates the request;
old confirmations cannot carry over to another request. The system then holds
all games, saves the host under a unique recovery name, transfers and verifies
that exact save set, reloads every world in the existing processes -- the host's
included: a world kept running from memory holds its entities in creation order,
a loaded one in save order, and the person and town simulation consume that
order, so a host that skipped the load diverged from the reloaded clients within
~35 game units (measured 2026-09-16) -- and compares the paused worlds. It
resumes only after the shared comparison succeeds. The host's previous speed is restored, including an intentional pause.
Changes present only on the client are replaced by the host's world.

A single compact native **Multiplayer Resync** panel appears automatically when
an in-game desync is detected. Clients initially wait for the host; the same panel shows
which of the five recovery steps is running. It disappears on successful
completion, leaving no persistent button. The ordinary Multiplayer dashboard
keeps its visibility preference and has no additional Resync section.

During native saving and loading, the custom overlay stops drawing and the
game's own loading screen remains. After a failure, the panel shows the failed
step and detail with host-only **Retry** (reusing a completed, verified snapshot when one
exists). Retry requires fresh readiness from every client in groups of three or
more. Host authority is checked against the authenticated lobby sender, including
commands that bypass the UI. There is no Cancel button: cancelling never released the input hold
and left the players unable to continue. A legacy aborted operation can still
be restarted with **Resync now**. A request temporarily disables its button;
if the host has not confirmed it after five seconds, it can be tried again.

Unsupported lobbies show an explanatory notice that can be dismissed. An active
recovery stays open until completion. To abandon recovery entirely, exit all
games normally and start a new lobby. Neither an error nor a timeout unpauses a
diverged world. Log upload preferences remain independent of recovery.

Log reporting is deduplicated per loaded world within the lobby. After a resync,
a new desync asks again when the preference is `ask`; a previous **Only this once**
answer is not consent for that new report. **Always send** and **Never** remain
preferences. An unanswered earlier prompt keeps its original world identity
when restored, without suppressing a later incident in the recovered world.

## Supported session

The current implementation allows **two or more players in a player-hosted lobby**, all running
the same package, on the exact supported Windows game build 35924. A whole
executable SHA-256 check guards the build-specific native adapter. Dedicated
relays do not advertise automatic recovery. There is no recovery-specific player
limit beyond the existing lobby limit. The roster may change while a recovery
runs: a client that leaves is dropped and the round completes for the others
(its own copy stays held); a player that arrives is admitted -- into the current
phase while everyone is still pausing, otherwise as pending, in which case the
round finishes for its members and, instead of releasing them, sends the same
snapshot round once more under a fresh epoch with the newcomer as a member.
Nobody is released until every member is in. Ordinary lobby start and save
selection keep their existing flow.

## Frozen joins

Since 2026-09-16 a player joining a running player-hosted session is brought in
through this same round, started by the host lobby itself in mode `join` (no
button, no readiness): the session holds, the host saves, everyone -- the host
too -- loads that save, the paused worlds are compared, and play resumes at the
host's previous speed. The earlier shape (the host kept running, the newcomer
loaded an autosave and caught up on the command history) left the joiner's
entities registered in save order against the host's creation order; its
simulated-people count split within ~35 game units and the buses on a line
drifted at the next stop, on two identical replays, while a session whose
members had all loaded together stayed locked. The host's roster event carries
`join_freeze` so the menu DLL takes no hot-join autosave of its own; when
recovery cannot run (a client whose version lacks it, a save transfer in
flight, a relay lobby) the flag is off and the old autosave path still serves
the newcomer. `tools/test_auto_sync_lobby.py` covers the round end to end with
simulated engines, a leaver dropped mid-round and a late joiner.

## Implementation

- `netpunch/sync_operation.py` owns the host-authoritative barrier. Requests and
  acknowledgements name operation, revision, phase and a fresh world epoch.
  The host waits for every original participant, checks exact snapshot hashes
  and fresh paused fingerprints, and retains the hold on errors or disconnects.
- `sync_runtime.py` coordinates the local native and Lua adapters through
  PID-scoped files. Native commands require actual completion acknowledgements;
  enqueueing is not saving or loading successfully. `tpf2_sync_lua.txt` also
  carries the failed step and detail for local diagnostics.
- `sync_snapshot.py` owns immutable save bytes, requires `.sav` and `.sav.lua`,
  includes `.jpg` when present and verifies SHA-256 throughout. Unique
  `mp_<epoch-prefix>` names never overwrite unrelated user saves.
- `sync_lobby.py` uses the existing authenticated lobby and reliable save
  transport, repeats control messages, rejects duplicates and keeps recovery
  transfers separate from normal start/hot-join transfers.
- `native_io.cpp` marshals save, pause/drain and load to the observed engine UI
  thread. The native input gate stops new user commands and legacy per-frame
  script events before command creation. A FIFO pause fence drains earlier work.
- `mp/resync.lua` runs before all simulation producers and stops GUI preview
  producers too. A newly loaded Lua state reports its own fresh world token.
  A full paused world hash bypasses the normal running-only hash cadence. It
  publishes PID-scoped, fresh GUI desync notices in `tpf2_sync_notice.txt`.
  The lobby forwards these only to the local native panel; notices never start
  a recovery. Old-world notices are consumed during a hold to avoid reopening
  the panel after completion. Native buttons use the existing lobby command
  channel. Control files use per-writer temporary names so concurrent writers
  cannot lose each other's replacement.
- `net.cpp` scopes data and ACKs to a world epoch. The bridge resets queues,
  partial packets and runtime command files together. A tail read begun before
  reset cannot enqueue into the new world. Epoch requests have a separate
  `tpf2_epoch_request.txt` mailbox so roster/speed writes cannot undo a reset.
  Existing exclusive loopback binding and sender-address checks are retained.

All players need the same package: the native wire protocol is now version 4.
The save/load adapter and Lua/network barrier must be deployed together.

## Validation and limits

Automated tests cover the production Lua 5.2 pump and GUI callback; barrier,
snapshot and runtime state machines; real UDP lobby/save transfer with simulated
engines; actual native UDP epoch resets; and native IPC completion publication
under Windows sharing violations. They do not prove in-game save/load behavior.

Run `tools/resync_test.py`, `tools/test_sync_operation.py`,
`tools/test_sync_snapshot.py`, `tools/test_sync_runtime.py`,
`tools/test_auto_sync_lobby.py`, `tools/test_net_epoch.py`,
`tools/test_net_multipeer.py`, `tools/test_net_restart.py` and
`tools/test_native_control.py` before a release.
The native tests require Windows and the repository MSVC environment helper.

The archived automatic implementation had successful game tests and a later
unresolved Vulkan Device-lost/MemoryException during simultaneous reloads. This
port suppresses overlay rendering during world I/O and checks overlay submission
and fence completion before reusing resources. These changes address concrete
renderer hazards; they are **not evidence that the old crash is fixed**.

Local acceptance used two real build-35924 games, matching complete candidate
packages and backed-up test saves. Four recovery operations completed, including
host and client GUI requests, speed 1, intentional pause, and a live client build
preview during the hold. All completed epochs produced identical fresh paused
fingerprints. A deliberate client-only journal change was replaced by the host
snapshot. The desync indicator was raised through the diagnostic injection path;
these cases do not claim to reproduce an organic desync.

A deliberate IPC failure after real snapshot transfer held both games. The
native retry button reused that immutable snapshot with a new epoch, without
another native save. A street built after recovery appeared in both worlds,
followed by shared SYNC. Repeated loading produced no observed game crash or
GPU error event in this run. This is a bounded local test, not a general proof
against the archived graphics failure or compatibility with every mod set.

For future changes, repeat these cases with the existing rig and compare both
worlds, logs and installed hashes. Close both games normally before replacing
DLLs, preserve test evidence before restarting, and verify the complete release
package and downloaded assets before switching the launcher feed.

The single-panel UI change has automated Lua notice, runtime and UDP regression
coverage plus an offscreen render check using the actual native panel renderer.
A subsequent local two-player run exercised this UI with an injected checksum
mismatch: the user operated and approved the panel, both worlds reloaded, both
native input gates acknowledged release, and both dashboards returned to SYNC
with zero desyncs at speed 1. Error/Retry interaction was not repeated in this run.

Three-player candidate: `tools/test_auto_sync_lobby.py --players 3` exercises
real UDP with simulated engines, including a delayed third hold, simultaneous
requests, mismatch on the third world, retry and third-player disconnect during
checking. Both remaining players retain the hold and report the disconnect.
The current test additionally verifies host-only initiation and retry, fresh
readiness for every attempt, rejection of stale confirmations, and no engine
recovery before the last participant is ready. `test_sync_readiness.py` covers
roster changes, duplicate confirmations and delayed readiness messages.
The first three-game run completed save/load and native release on all three,
but failed post-recovery connectivity: each instance heard only one peer.
The previous native transport retained a single peer session and rejected other
senders once the world epoch was nonzero. The replacement maintains independent
receive ordering, chunk assembly and ACKs for each established process. A sent
packet stays pending until every recipient present at send time acknowledges it.
World changes reset all streams but keep the established members, and reject
stale worlds. The world epoch is the credential: a process only has it from the
lobby's own control path, so a sender that presents the current epoch under a
session the cohort has not seen is admitted (a game that restarted, or a player
who joined after the resync). Until 2026-09-15 such a sender was refused for the
rest of the lobby's life and every broadcast waited for its dead session, which
held the 32-packet send window for everybody ("pending=33"). A member silent for
the peer timeout (10 s, not even a keepalive) now leaves the cohort and releases
the packets that waited for it (the last one leaving puts the transport back to
"nobody is listening": queued lines are dropped, nothing is kept for the next
joiner); a receiver whose sender no longer retains the packet it waits for
delivers what it had already stashed and acknowledged, then skips to the sender's
floor (logged, the Lua layer NACKs a command gap). After a resync completes, the host lobby advertises the resync epoch
as its transport lobby and serves late joiners the resync snapshot, so a later
joiner starts in the members' world; a member already in that world takes the
new nonce as a rename, not a reset, and a client ignores a nonce it has seen
before (a reordered old roster).

`tools/test_net_multipeer.py` compiles the production C++ transport and tests real
UDP frames from independent simulated peers with 2, 3, 5 and 8 participants across
three world resets each. Coverage includes interleaved/reordered chunks,
duplicates, per-peer ACKs, retransmission, stale world rejection and current-world
admission. `tools/test_net_restart.py` drives the restarted-joiner case: admission,
eviction of the silent old session, the old world rejected, the floor skip, and
re-admission after an eviction.
The lobby recovery test also passes with 5 and 8 simulated engines, including a
delayed final participant and its disconnect. These are automated correctness
tests, not game or load tests.

The corrected local candidate subsequently passed a three-game test: host-issued
readiness progressed through 1/3, 2/3 and 3/3 before recovery began. All three
completed the reload and native release, then retained both peers with fresh
ticks, SYNC, zero desyncs and speed 1. The user confirmed a subsequent construction
action appeared in all three games; the following dashboards independently showed
one applied action on each instance and continued synchronization. This validates
the tested three-player flow, not larger live groups or sustained load. The test
used an injected checksum discrepancy, not organically diverged world objects.


## Integration with 0.5 main

Merged with the current in-game lobby, mod-download and exact-version checks. The
native transport retains the selective-ACK window and does not implicitly ACK old
gaps; each recipient must acknowledge a broadcast. Route changes preserve pending
packets instead of deleting the stream. World epochs reset the cohort explicitly.
The next development version is 0.5.1 to reject released 0.5.0 peers using the old
wire format.

Validation of the combined tree: all native targets built, 28 Lua files parsed,
37 operation/readiness/runtime/snapshot tests passed, Lua recovery and desync-report
regressions passed, and existing version/lobby/crossing checks passed. Real UDP
transport passed for 2/3/5/8 participants and three world resets; three-participant
lobby recovery passed with simulated engines. Native IPC completion, world-epoch,
and ACK-gap/route-preservation regressions passed. No live game reload was run
against this combined tree.
