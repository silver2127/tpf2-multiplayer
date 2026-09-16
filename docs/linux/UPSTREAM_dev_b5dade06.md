# Upstream dev b5dade06 integration

Windows target: `b5dade0695318276af46f15b74a8d9da9bc70dd2`.
Baseline: native Linux after [6cb03915](UPSTREAM_dev_6cb03915.md).
The three-commit merge is staged and uncommitted. This is a partial native
integration, not a claim of live cross-platform gameplay validation.

## Merged

- 2a87bb4: company colour/name dashboard, player-based default company names,
  company-window rename routed through CMNAME in shared Lua.
- 48ce22c: left-click next company, right-click previous company.
- b5dade0: SEPARATE COMPANIES lobby setting, automatic chip assignment,
  co-op default, live mode protocol and roster mode.

The whole-file lobby conflict was merged from LF-normalized index stages;
its resulting diff retains every incoming change and existing Linux paths,
parent monitoring, SIGTERM handling, log retention and public-list support.
Windows native code remains exactly as upstream. All 28 Lua files are byte
identical to the target, with no pinned exceptions. Manifest SHA-256:
`f5651d1a4ac8f245c6d81bcbf8100e9ef6f4f9f7d3c9ba8d7d4681836199d0db`.
The verifier and release BUILDINFO now identify this target.

## Ported

- Linux SDL right presses invoke only company chips. Previous selects the next
  lower used id, wrapping to the highest; a sole company is a no-op. Left still
  selects the next used id, then one new id (wrapping at 200). A captured right
  release cannot leak to the game after moving outside or closing the panel.
- Native host card and host/relay-leader lobby controls select separate
  companies. StartRequest carries --companies only for hosts. Live changes go
  through the existing generation-bound queue, with role/readiness checks.
  Roster mode drives the checkbox and legend, including clients. Co-op remains
  the startup default. Added layout space keeps controls and status apart.
- Shared company names/defaults/dashboard use the existing Linux mp_players.txt
  roster export. The new rename request's native prerequisite is missing below.
- Corrected the shared host roster deduplication key to include mode. Without
  this, a solo host's toggle changes no company ids, emits no roster/state and
  leaves a roster-driven native checkbox stuck. Added a loopback regression.

No new game patch is installed. Evidence and the rename investigation are in
[DEV_B5DADE06.md](../re/linux/DEV_B5DADE06.md).

## Not ported

Company-window rename capture (2a87bb4 prerequisite): the Linux SetName path
still blocks player clicks. Rechecked the actual ELF factory, SysV string/entity
arguments, generic UI caller/Add sequence and completion callback. The callback
clears captured state; safe company-specific cancellation/completion lifetime
and originating-entity replay remain unproven. Ordinary VNAME still skips the
origin, so merely shipping a cancelled command is incorrect. No guessed
address or ownership field was enabled. Shared CMNAME and saved/default names
work at the Lua level, but that does not make native window renaming available.

Earlier ownership, automatic recovery/in-world reload and Workshop gaps remain
as documented in previous integrations; lobby assignment does not resolve them.

## Tests

- `tools/linux/build_native.sh`: soldier build, 44/44 CTests and glibc <=2.31.
  Expanded lobby_ready covers chip direction/wrap/no-op, authority/readiness,
  queued mode commands, initial host mode and roster-to-view mode updates.
- `python3 tools/linux/verify_lua_release.py`: 28 exact Lua files, manifest above.
- `tools/lobby_mode_test.py`: real loopback host assignment and mode protocol,
  including mode-only solo-host regression.
- `tools/company_name_test.py` (Lupa Lua 5.2) and `tools/company_chip_test.py`: pass.
- `tools/linux/test_lobby_stop.py`: 16/16 orderly shutdown checks pass.
- Release shell syntax and whitespace checks (upstream CRLF respected) pass.

Python dependencies were installed only into `.git/test-venv` in this clone.
Build/test logs are in `.git/*port*.log` and `.git/lobby-*.log`. No game or Steam
was started, no installation or publication was performed, and no commit made.
