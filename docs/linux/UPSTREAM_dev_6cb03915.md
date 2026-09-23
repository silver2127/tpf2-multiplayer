# Upstream dev 6cb03915 integration

Windows target: `6cb03915a10a901d421f960b2d2148cc1e649eee` (0.5.7 plus
subsequent dev changes). This is the remaining 18-commit batch after
[cae5d370](UPSTREAM_dev_cae5d370.md). The merge is staged and uncommitted.
This is a **partial native integration**, not a live compatibility certification.

## Merged

All 18 Windows commits and their Windows implementation paths are retained:

| Commits | Changes |
| --- | --- |
| ea2f0f6 | Remove the obsolete m3_determinism_1 probe on installation |
| a6af077 | Shared recovery keeps the host's held world; clients load the snapshot |
| 67db330, 779966e | Host world switches and explicit start when reusing a hot-join snapshot |
| 7ef52dd, 7e6e498 | Experimental 0.5.7 metadata; relay deployment installs zstandard |
| 3217497 | Road free-space sums; ship/aircraft claim-order diagnostics |
| 87324c7 | Deterministic script wrapper ignores its own per-frame load echoes |
| 5005a0b, df33fb5 | Hash sample time comparability; owed gaps and live catch-up boundaries |
| a64e4ea | Lines may use another company's stations |
| ecb0aa8 | Dashboard sections are mutually exclusive tabs |
| 2476119, 6cb0391 | Proton installer and repaired-lobby comparison/installation |
| 5f34539, be5f6c4, e93cde0, c61fc73 | Merge history |

The whole-file lobby conflict was resolved by three-way merging LF-normalized
index stages, then reviewing its differences against both parents. Linux data
paths, parent monitoring, SIGTERM cleanup, public-list fetching and log retention
remain alongside the new shared world-switch and Wine behavior. The Proton
installation guide now documents the current upstream installer and links the
preserved [0.4.22 pinned instructions](../proton/INSTALL_0.4.22.md).

All **28 Lua files are byte-identical to 6cb03915**, including dashboard tabs:
the earlier Linux tab patch has converged with upstream. The release verifier
now pins that commit without exceptions. Manifest SHA-256:
`abff2739ee5a14c33e72749ce9562f53cd0f83d1b04cf9b4c1cd5fb634155649`.

## Ported

- Both road-space overloads: guarded Linux entry wrappers collect rounded terms
  at four verified accumulator sites. The original engine still performs all
  component access, clipping, predicates and assertions. Shared roadspace.h
  sorts and sums in double; >512 terms returns the original answer. Nested
  calls and exception cleanup preserve per-invocation state. Relays use SysV
  stack alignment and preserve integer/XMM registers and flags.
- Ship and aircraft Update2 observers: guarded SysV wrappers, Linux family
  vectors and the existing guarded Name reader feed upstream moveorder.h.
  Private reusable buffers produce name/rank/id hashes; engine order remains
  unchanged, as upstream intended. Logging is rate-limited to five seconds
  per thread/channel. This does not fix ship/aircraft arbitration.
- Shared-station line-editor gate: the Linux StationFilter owner comparison
  admits foreign stops in companies mode, retaining its original equality
  behavior otherwise. Ownership and demolition rules are unchanged.
- Accepted vanilla UI loads: byte-verified StartSavegame observer copies the
  Linux string basename and queues sharing on the current lobby generation.
  Own autoloads/refused loads are ignored. The host does not load its own world
  twice. PID-scoped Lua tokens cover generated/unnamed worlds and invalidate
  previous-world snapshot reuse. Switch transfers are consumed once; in-game
  Linux clients get a placed mp_shared save and a manual LOAD GAME instruction.
- Fresh hot-join snapshot reuse explicitly emits start with that save path,
  including when the lobby has not previously latched a started session.
- Linux installation removes only mods/m3_determinism_1 after installing the
  current mod; dry-run reports removal. auto_install.py uses this installer for
  each selected native copy after games close. Sandboxie paths are Windows-only.
- Deterministic-script tests accept TPF2MP_GAME_INIT while keeping their Windows
  default, allowing the actual installed script resources to be tested on Linux.
- Shared Python recovery, Lua echo/hash/history/tab behavior, relay zstandard
  deployment and Proton packaging are merged directly; they need no ELF hooks.

Flags `roadspace=0`, `shiporder=0`, `airorder=0`, `sharedstations=0` in
`tpf2_menu_flags.txt` disable their corresponding new hooks (root file precedes
data file). Peers need matching arithmetic/ordering settings. Native hooks
remain behind build-id, byte, read-backend and no-patches checks; failed movement
installation prevents multiplayer readiness. Evidence, ABI and exact patch
sites: [DEV_6CB03915.md](../re/linux/DEV_6CB03915.md).

## Not ported

- **a6af077 native automatic recovery prerequisite:** the shared host-keeps-world
  state machine is merged and tested, but Linux still lacks NativeIo's correlated
  pause/drain/save/load/action-hold controller. The actual SaveGame, AutoSave
  completion and StartSavegame bodies were re-examined against the prior save
  design. Their entry/layout evidence does not establish safe command-thread
  quiescence, completion ownership, input suppression or UI lifetime through
  destruction of a live world. No unsupported native capability is advertised.
- **67db330 automatic in-world client load:** host observation, sharing, protocol
  handling and title-menu autoload are ported. The same missing controller
  prevents safely calling StartSavegame for a running client's world. The
  verified existing loader refuses that state. Clients receive the save and
  must open LOAD GAME and choose mp_shared. No lobby-thread engine call is used.

The prior native Workshop registry, generalized modular-station weld, wider
company/ownership APIs and blocked action-channel limitations are unchanged.
A passing historical 0.4.22 replay is not evidence of current cross-platform
play. No game or Steam process was started in this integration.

## Tests

- Required `tools/linux/build_native.sh`: pinned soldier SDK build, **44/44
  CTests**, and glibc <=2.31 import verification passed. New tests cover shared
  road/move algorithms, Linux relay frames and byte rejection, nested road
  collection, cap fallback, local and foreign-runtime exception cleanup,
  immutable observations, station helper ABI, accepted/refused/own load
  observation, hot-join explicit start, world-token PID/hold/reuse handling,
  host reload suppression and one-use manual switch transfers.
- Required `python3 tools/linux/verify_lua_release.py`: 28 exact files; manifest
  above. `verify_movement_elf.py`: seven complete/significant byte spans, all
  stolen boundaries (including the menu observer), branch-interior checks and all four sum sites. Existing
  `verify_train_order_elf.py` (16 checks) and `verify_capture_elf.py` (149 probes)
  passed against the supplied ELF, read-only.
- Python: 52 sync tests; 28 lobby-limit tests; Linux lobby-stop 16/16 checks;
  auto-install tests; full world-switch loopback scenario including a switch
  during another transfer and ordinary hot joins.
- Lua 5.2 through clone-local Lupa: hash-sample-time, live catch-up gaps,
  shared-station lines and auto-sync suites passed. The actual installed Natural
  Town Growth script and game init.lua passed deterministic_script_test.py,
  including repeated echo loads at different pacing and genuine save reloads.
- Upstream Proton synthetic installer test passed (the pinned-hash repair paths;
  no current Windows lobby payload was available for an actual bundle repair).
  Linux installer black-box fixture passed extraction, checksums, dry-run,
  obsolete-mod removal, install/upgrade/uninstall, aliases and data preservation.
  Only a temporary fake Steam tree inside the clone was installed.
- Shell syntax, Python compilation and whitespace/conflict checks passed
  (`core.whitespace=cr-at-eol` preserves upstream CRLF files).

No commit, merge abort, push, publication, system-wide installation or real-game
installation was performed. Static/fixture verification cannot establish live
simulation performance, UI behavior or multiplayer determinism.
