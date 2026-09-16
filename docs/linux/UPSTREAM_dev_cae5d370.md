# Upstream dev cae5d370 integration

Windows merge target: `cae5d370798ee7d724dfc739fa9d0546fbc505d2`.
This is the requested oldest 20 of 38 pending commits, following the
[3edfbccd integration](UPSTREAM_dev_3edfbccd.md). The remaining 18 commits
are outside this batch. The merge is staged and uncommitted; VERSION is unchanged.

## Merged

Retained all incoming Windows code and shared Lua/Python changes. Resolved
lobby.py by a three-way merge of LF-normalized copies (the conflict was the
whole file), retaining Linux paths, parent monitoring, SIGTERM cleanup, log
retention and public-list fetching. Combined modshare.py's folder-name rules
with the Linux-path documentation. Windows paths remain available.

| Commits | Integration |
| --- | --- |
| 90bbedc, fdfa021, 84a2268, 00e6053 | Dynamic roster, click-to-clear code, stages and recent hot-join snapshot reuse |
| 84e0f62, 06ba9d9, 0677afa, 6904782 | Growable captures, params, asset v2, vectors, placement identity and shared survivor diff |
| 9772ca5 | Shared fragmented control messages, whole mod lists, progress and transfer lifetime |
| a4c4c20, 3a778f1 | Shared history/line retention, acknowledgement/rejoin/feed fixes and roster load gate |
| 92af32e | Shared recovery improvements; native controller limitation below |
| 132286a | Linux Big Maps worktree build/package counterpart; PowerShell stderr fix retained on Windows |
| 6bf39a2, 6471f30, 672d231, 6d6ca22, f484997, f9a7b8b, cae5d37 | Merge history retained |

27 Lua files exactly match incoming. lockstep.lua retains the earlier Linux
integration's dashboard tabs; its reviewed difference is
[UPSTREAM_dev_cae5d370_lua.patch](UPSTREAM_dev_cae5d370_lua.patch).
The verifier pins that combined file rather than discarding earlier work.
The 28-file manifest SHA-256 is
`3076cc4521daadcd6905aae3ad9d8737bb5c64911901797db225763817a7627d`.

## Ported

- Dynamic Linux vehicle configs, integer lists and vector<bool> decoding;
  road/removal/object vectors, construction filenames/params, stop/line names
  and held CreateLine callbacks. Shape checks and corrupt-pointer guards remain.
- Iterative nested Param traversal, without depth/node/text policy caps;
  ancestor cycles and malformed map links still refuse capture.
- Atomic ROADC+CONXP records carry matching placement serials and explicit
  road-companion flags. Growable template-weld bookkeeping preserves Linux
  transaction and foreign allocation ownership checks.
- Native asset capture and replay both use TPAS v2, u32 string lengths and
  growable groups/removals. Old v1 payloads are rejected. No Windows C++ layout
  or calling convention is copied into Linux.
- Complete lobby event lines and roster identities; stage text displayed by
  each player, emitted on loading and advanced from lockstep status. A filled
  join-code field clears on click. Recently shared snapshots can serve another
  hot joiner for 15 seconds of unpaused play, including paused-time accounting
  and session teardown invalidation.
- `build_release.sh` and `auto_install.py --bigmap-repo CHECKOUT` build and ship
  native Big Maps from a selected checkout/worktree. Plugin-only edits count
  as changed inputs. Soldier builds mount plugin source read-only, keep outputs
  in the multiplayer build tree and restrict plugin exports with a version
  script. The existing Linux installer handles copies and plugin configuration.
  No installation was performed in this job. The existing Linux lobby freeze
  uses process exit status; PowerShell's stderr behavior does not apply.

Evidence and static investigations: [DEV_CAE5D370.md](../re/linux/DEV_CAE5D370.md).

## Not ported

- **92af32e native recovery work metrics:** Linux still lacks the prior
  NativeIo pause/drain/save/load/action-hold controller. Re-examined the actual
  SaveGame and AutoSave completion bodies and existing save design. Operation
  lifetime, command-thread ownership across world destruction and safe action
  suppression remain unproven. No fabricated supported status or guessed hook
  was added. The old Linux autosave file watcher retains its 90-second timeout.
- **90bbedc native Workshop registry:** Linux still lacks managed registration.
  Located and disassembled RefreshModList at 0x31ae800 through the source
  signature export; confirmed control-byte iteration and 32-byte slots.
  Linux result/path ownership, backend identities and catalogue receipt layout
  remain missing. The Windows MSVC shadow structure cannot safely be reused.
- **Generalized modular-station endpoint weld:** the Linux predecessor implements
  the older depot/template topology, not Windows's newer station_weld.h path.
  Removed the Linux routine's record ceilings, but did not claim support for
  the generalized station algorithm. Reviewed the existing Linux construction
  layout/ownership evidence and transactional weld against the new Windows
  degree/owned/remap algorithm. A representative Linux modular-station proposal
  proving frozen-index and segment-tag ownership across its compaction is still
  needed; unsupported topologies continue to be refused without mutation.

Other previously documented native gaps (including ownership and some action
channels) remain unchanged. No live multiplayer compatibility claim is made.

## Tests

- Required `tools/linux/build_native.sh`: soldier build, all 40 native CTests and
  glibc <=2.31 check. Regression coverage adds >1 MiB roster tailing, long names,
  hot-join stage/clock behavior, 2,050 parameter entries, 100 nested tables,
  2,049 road edges, long TPAS v2 strings, malformed/truncated v2 and refused v1,
  100 vehicle parts, 1,025 autoload bits, 1,000 sale IDs and 20 held callbacks.
- Required `python3 tools/linux/verify_lua_release.py`: 28 files, manifest above.
- `verify_capture_elf.py GAME`: build-id and 149 existing runtime probes.
  `verify_train_order_elf.py GAME`: 16 signatures and branch/call/RTTI checks.
  Both only read the supplied ELF; no extracted game constructor was executed.
- Python: test_lobby_limits (28), test_sync_operation (21), test_sync_runtime
  (13), mod_download_test (12), Linux lobby-stop (16 checks), keep-logs and
  auto-install (including plugin-only change test).
- Lua 5.2 via clone-local Lupa: con_pair, hist_retention, inject_unbounded,
  late_loader, line_rapid_edit, ser_depth and survivor_diff suites pass.
- Optional Big Maps build: actual selected native checkout, four CTests,
  export and glibc checks, outputs confined to this clone.
- Upstream station_weld_test.cpp compiled with g++: 105-node/104-segment
  Windows-layout fixture passes (does not establish Linux station topology).
- Shell syntax and whitespace checks. Windows test_native_control.py and
  tools/re/asset_stroke_test.py require cmd/MSVC and cannot run on Linux;
  native asset codecs are covered by the Linux CTests instead.

Steam and Transport Fever 2 were never started. No commit, merge abort, push,
installation or publication was performed.
