# Windows dev bd69b864 integration

## Merged

Windows `bd69b864737a4c4383c6d73ac8d1dc8fa6f1c8b7` onto native parent
`0a152505e05c09c946357287dcab69ab0567bb57`. The five requested first-parent
commits are 02426ea, 6d816e3, 703ebe1, 12407af and bd69b86; the last also
brings main's 0.7.0.1/0.7.0.2 TCP diagnostics and shared resync lobby history.
No later pending dev commits were integrated. Merge remains staged and uncommitted.

Resolve `bulk_tcp.py` by retaining Linux SO_REUSEPORT in the new listener
factory (both IPv4 and IPv6). Resolve `observe.py` by deleting both TCP and UDP
mappings through Linux's environment-cleaning `_upnpc_run`; use that wrapper
for the incoming TCP mapping call too. Windows paths remain intact.

## Ported (what and how)

- Apply and independently check the supplied native UCRT math implementation:
  six verified GOT imports and three guarded street-developer double-atan2
  call sites. Enabled by default, `TPF2MP_LIBM_PARITY=0` disables it.
- Apply the native Big Maps depth-12/13 patch, retaining default depth 11,
  byte guards, rollback and the existing placement menu cap. Depth-specific
  compact IDs and decoding match the supplied Windows model in fixtures and
  original-code emulation. Both peers must use the same depth.
- Address the reported target-record scan with a private per-owner hash index.
  Preserve record cleanup and all Windows ordering/lifetime checks. This does
  not remove every source of target-set allocation or traversal overhead.
- Port native transfer details, TCP hints and receive-completion handling.
  Resync reuses the native session roster/chat panel with readiness/retry,
  host decline and preflight dismissal; company editing is disabled there.
  Chat stops accepting text during native world I/O. Loading page 16 arms a
  pending stage watch on the lobby thread, the title page clears it, and load
  progress refreshes the footer before wire-message deduplication.
- Keep shared Python/Lua and Windows menu changes. Advance release-packaging
  and Lua-verifier references to this target; retain the pinned native
  origin-replay exception. Installer release is 0.7.0.2.

Static and ABI evidence, guards and the failed live attempt are in
[DEV_BD69B864](../re/linux/DEV_BD69B864.md). The upstream reports contain tests
performed elsewhere; those are not new local live results.

## Not ported

The placement-distance saturation and configurable placement-attempt budget
flagged by 12407af remain unavailable on native Linux. Static investigation
found the 32-bit squared-distance loops inside native optimizer `14e45b0`,
including `14e46dd` and `14e473a`. They are inlined into a larger function;
substituting the standalone Windows helper at its entry would be wrong.
The optimizer state's lifetime/provenance, all spacing copies and the worker
attempt argument still require evidence. The prescribed live attempt failed
before process creation, so gdb could not settle these contracts. No guessed
patch was installed. Menu size limits remain, but do not protect arbitrary
oversized Windows-created saves from placement divergence.

Live math co-installation, depth-13 rendering/load/save, actual resync UI,
target performance and native/Proton gameplay parity remain unvalidated.
This integration is PARTIAL because of the placement gap, not a claim that
passing offline tests establishes cross-platform gameplay parity.

## Live testing

Backed up both native actor payload directories, installed the soldier build,
Big Maps plugin, merged Python and Lua, and invoked the prescribed lab launcher.
It exited 1 immediately: `bwrap: setting up uid map: Permission denied`.
No game PID, GPU, title menu, loaded world, gdb hit or visual result was reached.
Restored both directories; SHA-256/symlink inventories match. Steam, original
user saves/mod and game directory were not changed. No game remains running.
Current evidence is job `meta/live/launch.log` and `restore.json`; archived
actor logs/data are historical files, explicitly not observations from this run.

## Tests

- `tools/linux/build_native.sh`: soldier build, 66/66 CTests and glibc baseline
  pass. Includes upstream math goldens, octree assembly/rollback and expanded
  target collisions/churn, native transfer/stage, shared resync layout/actions.
- `python3 tools/linux/verify_lua_release.py`: 29 Lua files (one pinned native
  exception), 32 HUD textures pass. Manifest SHA-256 remains
  `b67ff99a9d55f248af9ea23290c96bbf0904cd83b533338bcf48d16878023aaf`.
- `verify_libm_parity_elf.py`: all six imports, three complete guarded contexts,
  conversion instructions, original atan2 PLT target and eager binding pass.
- Big Maps `verify_game.py`: build-id and 29 sites pass.
- Big Maps `test_octree_depth_elf.py`: original ELF code emulation passes;
  depth 13 = 1,620 nodes, depth 12 = 1,462; stock overflow reproduced.
- Python `test_tcp_connectivity.py`: nine tests, including real loopback
  IPv4/IPv6 transfer, Linux bound-port reuse and mocked CLI TCP/UDP cleanup.
- `test_transfer_status.py`: three tests pass.
- `test_auto_sync_lobby.py`: two-player simulated-engine recovery/frozen join
  passes. `test_steam_tcp.py`: real loopback TCP plus simulated Steam fallback
  passes. No real Steam peer or router was contacted by these tests.

The Windows-only `tools/menu_title_panel_test.py` could not run because this
Linux host has no `cmd`/MSVC; native panel CTests passed instead.

Python dependencies were installed in clone-local `.git/port-venv`; no system
packages were installed. Optional miniupnpc compilation failed under host Python
3.14; the tests use mocked router APIs and exercise the CLI fallback instead.
Build, static, emulation and Python logs are retained in `.git/port-*.log` and
copied into job `meta/live/offline-*` for the harness. No release was published.
