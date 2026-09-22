# Upstream dev d129fab7 integration: relay player statistics, porter tooling

Windows target: `d129fab7ac343b62f0c6fcda2f5b2a5a25af312c`.
Linux merge parent: `66212ff` (dev 23419163, see
[UPSTREAM_dev_23419163.md](UPSTREAM_dev_23419163.md)). Four incoming
commits. The merge remains staged and uncommitted. The native integration
status of 23419163 is unchanged (partial; stationicon and the resync title-menu
exception are still not ported, see that record).

## Merged

| Commit | Change | Linux |
| --- | --- | --- |
| ab7782a | `tools/linux_port/`: the Windows -> Linux porter (worker, prompt, hooks) | tooling only, nothing to port |
| 778689c | relay: player statistics (`netpunch/player_stats.py`, `player_stats.json`) | shared Python; packaged in the Linux lobby |
| 1362f17 | merge into drift-check-cap | — |
| d129fab | `installer/RELEASE-0.6.md`: install per platform first, shorter list | shared documentation |

`netpunch/lobby.py` conflicted as a whole file again only because Linux holds
it with LF endings and Windows with CRLF. A three-way `git merge-file` of the
LF-normalised stages (base 23419163, ours HEAD, theirs d129fab7) applied with
no conflict; the result keeps the Linux paths, SIGTERM/parent-PID handling and
adds the `PlayerStats` hooks (join, leave, drop, started, frames, tick, flush
on shutdown). The statistics are active only for `host --relay-only`, i.e. the
dedicated relay; a game-hosting Linux lobby does not create the file.

## Ported

No change touches the game binary, the Windows hooks or MSVC code, so there is
nothing to reverse engineer.

- `lobby.py` now imports `player_stats` at module level. PyInstaller's scan
  picks it up; `tools/linux/build_netpunch.sh` now also fails the build if
  `player_stats` is missing from the frozen Linux lobby.
- Lua/release provenance now targets d129fab7 (`verify_lua_release.py`,
  `build_release.sh` BUILDINFO and packaged record). No Lua file changed.

## Not ported

Nothing new. `tools/linux_port/install_windows_hooks.ps1` is Windows-side
tooling by design.

## Tests

- `tools/linux/build_native.sh`: soldier build, **45/45 CTests**.
- `python3 tools/linux/verify_lua_release.py`: 28 exact Lua files and 32 glyph
  textures against d129fab7.
- `tools/test_player_stats.py` (6 tests) and
  `tools/linux_port/test_port_worker.py` pass.
- With the netpunch build venv: `lobby.py --selftest`, `--selftest-relay`,
  `--selftest-mesh`, `tools/linux/test_lobby_stop.py` (16/16),
  `test_auto_sync_lobby.py`, `test_sync_operation.py`, `test_sync_runtime.py`
  pass.
- Not run: `build_netpunch.sh` (a PyInstaller freeze), and a live relay-only
  session (a short local `host --relay-only` start did not get past STUN
  observation within 8 s, so the stats file path was covered only by the unit
  tests).

No game or Steam launch, installation, publication, commit or merge abort.
