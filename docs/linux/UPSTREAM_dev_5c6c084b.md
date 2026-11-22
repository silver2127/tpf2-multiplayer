# Upstream dev 5c6c084b integration

Windows target: `5c6c084b0e5ad299048920e970e2e8fb79e21002`.
Linux parent: `c9bc9ef8b2fbfd832bb14aeacd2f47cb2739146e`, following
[6e1e4ec7](UPSTREAM_dev_6e1e4ec7.md). Two Windows commits, no conflicts.
Merge staged and uncommitted.

## Merged

- `502b667`: with `tpf2mp_live_join.txt = 1` in the host lobby's I/O directory,
  late joins bypass the recovery round and the roster permits the menu's
  hot-join save. An already held recovery still advertises `join_freeze`.
- `5c6c084`: mark late live joiners so the waiting-member sweep cannot start
  a recovery round for them. For members waiting before the host world loads,
  mark them and write `tpf2_sync_save.txt` to request the menu's save. This
  also covers a dedicated host. Default-off joins retain the frozen path.

Only `netpunch/lobby.py` changed upstream. Its shared changes merge unchanged;
existing Linux-specific lobby adaptations remain intact. Windows paths remain
intact. No Lua, C++ hook, executable address, bytes, offset or ABI changed.

## Ported

No additional platform implementation is necessary. Native
`native/linux/src/lobby_linux.cpp` already parses `join_freeze` in
`ApplyRoster`: roster growth with false selects `SyncStart`. `SyncPoll`
consumes the same request file used by the waiting-member sweep, requests
`MenuGame_ForceAutosave`, waits for a fresh stable save, and sends the lobby
`start` with that path. These paths do not request a native recovery hold.
Existing native build/byte guards and autosave implementation are unchanged;
there is no new engine contract requiring static or live reverse engineering.

Added real loopback Python regressions for both arrival orders, enabled and
absent live-join flag, repeated waiting sweeps and disabling the flag after
admission. Extended the native lobby fixture for a false-freeze roster,
unchanged-roster deduplication, file-request consumption and stable-save
handoff. Engine calls are stubbed in that fixture.

Updated README, installation link, Lua verification baseline and release
BUILDINFO provenance; packaging includes this record and the earlier records.

## Not ported

None from these two commits. Earlier limitations remain, especially canonical
simulation ordering default-off pending live lifetime validation. The opt-in
live-join path's availability does not establish Windows/native catch-up
parity. The existing installation caution remains applicable. See
[the preceding integration](UPSTREAM_dev_6e1e4ec7.md) and its linked records.

## Live testing

No game launch, gdb attachment, XTEST sequence or lab installation was performed.
No lab actor, Steam files or user saves were changed; no restoration was needed.
The loopback tests run production lobby networking with simulated native status;
they are not evidence of in-game pause behavior, a successful game save/load,
catch-up convergence or absence of recovery windows on screen.

## Tests

- `tools/linux/build_native.sh`: soldier build, 57/57 CTests and glibc <=2.31
  checks passed, including the extended `lobby_ready` fixture.
- `python3 tools/linux/verify_lua_release.py`: 29 Lua files (one pinned Linux
  origin-replay integration), 32 exact HUD glyphs; manifest SHA-256
  `b7410c4b3eb76fbe3dd95ded62fc6bcb8852f6afcea04141816c2f1c`.
- `.git/port-venv/bin/python tools/test_live_join_lobby.py`: four loopback
  tests passed.
- `.git/port-venv/bin/python tools/test_auto_sync_lobby.py`: existing two-player
  recovery, retry, snapshot transfer and frozen late-join regression passed.
- Release shell syntax, whitespace and empty unmerged-index checks passed.

Logs: `.git/port-build.log`, `.git/port-lua.log`,
`.git/port-live-join-tests.log`, `.git/port-auto-sync.log`.
Test dependencies were installed in `.git/port-venv`; optional miniupnpc failed
building, so loopback tests used pystun3 and zstandard without UPnP. Nothing
was installed system-wide.
