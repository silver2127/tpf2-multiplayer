# Upstream dev 23419163 integration: release 0.6

Windows target: `23419163881f81188559362401f62da091da4956`.
Linux merge parent: `4175912` (dev 66c870cf, see
[UPSTREAM_dev_66c870cf.md](UPSTREAM_dev_66c870cf.md)). Sixteen incoming
commits. The merge remains staged and uncommitted. Native integration is partial.

## Merged

| Commit | Change | Linux |
| --- | --- | --- |
| ba1e9dd | hash: any owned construction is a player construction; companies state applied before sampling | shared Lua |
| daca5b9, c4c1e86, 093dfff, 1fef2d5 | stationicon: ctor entity, post-attach `mpAttached`, EnginePtr at load | not ported (below) |
| 679505c | auto_install.ps1: locked lobby exe | Windows only |
| 61129bc | dashboard company colour beside the dropdown | shared Lua |
| 21ede00 | companies: publish company -> player -> name map | shared Lua |
| f6195dd | roadentries: a road edge's vehicle entries in name order | **ported** |
| bb0a988 | resync: a joiner's own snapshot load keeps the lobby | no Linux counterpart (below) |
| f94d8c0, ebcbed7 | frozen joins; a host that loaded its own world counts as started | shared Python; native roster flag ported |
| 2341916 | Release 0.6 (installer VERSION, notes) | shared |
| 8f3f31e, 7cbda89, 66f9daf | merges | — |

`netpunch/lobby.py` conflicted as a whole file only because the Linux side
holds it with LF endings and Windows with CRLF. A three-way merge of the
LF-normalised stages applied cleanly; the result keeps the Linux paths,
SIGTERM/parent-PID handling and adds frozen joins and `host_has_world()`.

## Ported

- **roadentries (f6195dd).** `movement_linux.cpp` redirects the one
  `call EdgeUseManager::Add` in AddToEdgeUseManager (`16c484a`) through a
  relay that passes the engine (r12) along, calls Add, then sorts that edge's
  20-byte entries by Name then entity id with the shared comparator, exactly
  as Windows does. Byte guards, ABI and layout evidence:
  [DEV_23419163.md](../re/linux/DEV_23419163.md). `roadentries=0` disables it;
  peers must agree on it.
- **join_freeze roster flag (f94d8c0).** `lobby_linux.cpp` reads
  `join_freeze`; when true a growing roster takes no hot-join save and shows
  the "holding the game" status.
- Lua/release provenance now targets 23419163 (`verify_lua_release.py`,
  `build_release.sh` BUILDINFO, packaged record). `installer/VERSION` is 0.6.

## Not ported

- **stationicon (daca5b9, c4c1e86, 093dfff, 1fef2d5).** They refine a native
  HUD tint path that Linux has never had. This round located the Linux
  constructor (`1090250`, entity in r9d, EnginePtr in rsi, accessor
  `146f0a0`) and its DoStep and rebuild callers, but class application,
  owner lookup and the post-attach site are still missing; no hook installed.
- **Resync title-menu exception (bb0a988).** Linux has no native resync load
  controller, so there is no own-load title menu to except. Automatic
  recovery on Linux remains unsupported, and with it frozen joins on a Linux
  host; a Windows host falls back to its hot-join save for a Linux joiner.
- Windows installer/auto_install changes do not apply.

## Tests

- `tools/linux/build_native.sh`: soldier build, **45/45 CTests**. New in
  `movement_linux`: edge lookup in Add's layout (valid, negative, out of
  range), id-only and Name sorts (case-insensitive, unnamed first), relay
  argument passthrough including r12 → engine and xmm0 bounds, refusal of odd
  spans and >512 entries without writes, byte-guard mutation refusal. New in
  `lobby_ready`: `join_freeze` takes no autosave or start and clears.
- `python3 tools/linux/verify_lua_release.py`: 28 exact Lua files and 32 glyph
  textures against 23419163.
- `tools/linux/verify_movement_elf.py` on the Linux ELF: 11 spans, the
  roadentries call target and the 20-byte push_back.
- Shared Python with the netpunch venv: `test_auto_sync_lobby.py` (incl.
  frozen join), `test_sync_operation.py`, `test_sync_runtime.py`,
  `resync_load_keeps_lobby_test.py`, `player_construction_owner_test.py`,
  `company_name_test.py`, `tools/linux/test_lobby_stop.py` pass. The
  pefile-based Windows byte tests were not run (Windows executable).

No game or Steam launch, installation, publication, commit or merge abort.
No live cross-platform gameplay validation is claimed.
