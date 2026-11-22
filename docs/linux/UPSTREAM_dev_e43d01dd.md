# Windows dev e43d01dd integration

Windows target `e43d01ddc55c999c3fdc22d8a31d2add925c0530` onto native parent
`ec8dfe9` (Port Windows dev 0610033 to Linux). Two first-parent commits:

- `11a98cc` edemo: one node index per bulldoze burst.
- `e43d01d` stops: a catch-up scan no longer re-ships replayed signals and stops.

The merge is retained, staged and uncommitted; git reported no conflicts and none
had to be resolved by hand. Version stays 0.7.0.2.

## Merged

Both commits touch shared code only -- the Lua mod
(`res/config/game_script/lockstep.lua`, `res/scripts/mp/cons.lua`,
`res/scripts/mp/stops.lua`) and two Python harnesses
(`tools/edge_demolition_test.py`, the new `tools/stop_replay_known_test.py`).
Neither adds or moves a Windows hook, an MSVC-specific construct or a
`TransportFever2.exe` address, so nothing in `native/`, `native/linux/` or the
Linux ELF site tables is affected. The merged mod tree is byte-identical to the
Windows tree except for the port's one standing divergence,
`res/scripts/mp/inject.lua` (native cancelled VNAME/VCOLOR ask for origin
replay); the new commits do not touch the diverging hunks.

## Ported (what and how)

No Linux counterpart had to be written. What was checked instead is that the
native Linux slice already satisfies the two preconditions the new Lua relies
on, so the shared code behaves here as it does on Windows.

**EDEMO burst (`11a98cc`).** `cons.lua` now builds the node index once per
`update()` tick and drops it whenever the network may have changed: before its
own proposal is sent, and in `lockstep.lua`'s `execute()` for every command whose
`op` is not `EDEMO`. The Linux slice emits EDEMO from the same cancelled
`BuildProposal` path and in the same record shape as Windows --
`native/linux/src/slice/slice_proposal.cpp:395` writes
`EDEMO <edges> <nodes> [<node0> <node1> <kind>]... <nodes...>`, which is what
`CM.execEdgeDemolish` decodes and which
`native/linux/tests/slice_proposal_test.cpp:251` pins. The index is keyed on
`CM.ticks` and matched with the same distance and lower-id tie-break as the old
full scan, so host and peer still agree across platforms: the key arithmetic
(`(floor(x/4)+1048576)*2097152 + ...`, at most 2^42) and the comparison are exact
in the double-precision Lua 5.2 of both builds.

**Stop replay registration (`e43d01d`).** `CM.nativeStopProposal` now registers
what a replay made or removed in `CM.knownStop` on every instance. That is only
correct where the originator also replays, rather than keeping its native build.
On Linux it does: `native/linux/src/slice/slice_proposal.cpp` registers
`STOPX`/`STOPXDEL` on the same cancelled `BuildProposal` factory as Windows
(`SliceProposalKind::Stop` / `StopDelete`, `KindName` at line 189-190, the
`STOPXDEL` writer at line 180), and the shared `inject.lua` ships the resulting
`STOPADD` (line 832) and `STOPDEL` (line 1703) through `CM.scheduleLocal`
*without* `skipOrigin` -- "strict, every instance replays". So the originator of
a Linux stop, signal or waypoint runs `CM.nativeStopProposal` too and records the
replay, exactly as the commit intends. The port's `inject.lua` divergence is in
the VNAME/VCOLOR branch only and does not reach this path.

`tools/linux/verify_lua_release.py` advances its cumulative reference from
`bd69b864` to `e43d01dd`. The pinned `inject.lua` SHA-256 exception is unchanged
and still matches (`df0cf0bb...`).

## Not ported

Nothing. Neither commit has a platform-specific part; no address, byte pattern,
struct offset or calling convention was needed, and none was guessed.

Earlier, unrelated port limitations (notably the placement-distance saturation
from `12407af`) are unchanged.

## Live testing

No game was run: the lab cannot start on this host at present, for two
independent reasons, neither of which is mine to change.

1. **Steam is not running.** The machine had been up 13 minutes and no Steam
   process existed; `~/snap/steam/common/.steam/steam.pid` still held the stale
   pid 6573 from 2026-09-17, with no such process. Transport Fever 2 needs the
   Steam client, and starting Steam is out of remit.
2. **AppArmor blocks Steam's container helper.** `tpf2mp-lab check` and `run`
   both fail at the outer layer with `bwrap: setting up uid map: Permission
   denied`. `kernel.apparmor_restrict_unprivileged_userns` is `1`, so an
   unconfined process that creates a user namespace is transitioned into the
   `unprivileged_userns` profile; that profile has no `attach_disconnected`
   flag, and the kernel audit shows it then refusing the helper's own uid map:
   `apparmor="DENIED" operation="open" info="Failed name lookup - disconnected
   path" profile="unprivileged_userns" name="proc/<pid>/uid_map" comm="srt-bwrap"`.
   `/usr/bin/bwrap` is exempt (its `bwrap` profile has `attach_disconnected` and
   `allow userns`) but stacks its children under `unpriv_bwrap`, whose
   `audit deny capability` then refuses the soldier runtime's nested
   `CLONE_NEWUSER|CLONE_NEWNS` (`DENIED operation="capable" capability=21
   capname="sys_admin" profile="unpriv_bwrap"`).

A host-`bwrap` launch with the soldier container omitted was attempted -- all 30
of the game's shared libraries resolve against the host plus the game directory
-- and it reached the game binary, which then exited at
`SteamAPI_Init(): SteamAPI_IsSteamRunning() did not locate a running instance of
Steam` and asked Steam to relaunch it. That request died inside the sandbox
(read-only filesystem, missing 32-bit libraries); its error dialog was closed and
its processes killed. No title menu, world, GPU, gdb hit or gameplay result was
observed, and no such result is claimed.

The native actor was backed up before the attempt
(`share/tpf2mp.before-port-e43d01dd`, `game/mods/mp_lockstep_1.before-port-e43d01dd`),
this build and the merged mod were installed into it, and both were restored
afterwards; the restored library SHA-256 prefixes and `cons.lua` hash match the
pre-test ones and the temporary backups were removed. The window size was set to
1280x720 for the attempt and put back. Steam, the user's saves and the user's
installed mod were not touched. Transcript and launcher:
job `meta/live/`.

## Tests

- `tools/linux/build_native.sh`: soldier build, 67/67 CTests passed.
- `python3 tools/linux/verify_lua_release.py`: PASS, 29 Lua files and 32 HUD
  glyphs against the new `e43d01dd` reference.
- `tools/edge_demolition_test.py` (new upstream coverage): PASS on Linux --
  "one node index per bulldoze burst (rebuilt after a removal and on a new
  tick), cell index == full scan".
- `tools/stop_replay_known_test.py` (new upstream test): PASS on Linux --
  "replayed adds and removals are known to the catch-up scan; native objects
  still ship; failed replays register nothing".
- The whole lupa Lua harness set (63 files under `tools/` that drive the mod
  through `lupa.lua52`) was run on this tree. The only failures are
  pre-existing and environmental, not caused by the merge:
  `autosig_compat_test.py` / `autosig_modes_test.py` /
  `natural_town_growth_probe.py` (hard-coded Windows workshop paths),
  `lobby_panel_test.py` (no `stun` module), `shared_infra_test.py` (no
  `pefile`), and `boot_resilience_test.py` (5 subcases), which fails
  identically on the pre-merge parent `ec8dfe9`.

These are off-game results. They are not cross-platform determinism evidence.
