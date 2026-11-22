# Windows 0.7 / dev 0a35d0a8 integration

Windows target: `0a35d0a82e5d26ae9bccef3bf864d2b3a2de2ebd`.
Linux merge parent: `eb81ef0b1de113dc893606b8cfc8b1a4af76ccea`.
The merge remains staged and uncommitted. Version stays 0.7.

## Merged

- `747fd4d`: native terrain compression default-on requirement.
- `0a35d0a`: remove the shared Lua roster load hold by default; retain
  `loadgate_roster=1`, leader readiness and command-history prerequisites.
  Require the native menu to publish the lobby nonce to its bridge.

Resolve HOTJOIN_ORDER by keeping the native retained-world opt-in policy and
adding the upstream roster behavior and bridge identity explanation. This change
removes a load gate; it does not change the separate native retained-world policy.
Windows paths and version constants are unchanged. Advance the Lua verifier to
this target, preserving the previously pinned native origin-replay exception.

## Ported (what and how)

Native Big Maps ships `terrain_cache_compress=1` and passes default 1 when reading
an absent key. Explicit 0 still disables it. Existing userfaultfd setup failure
keeps stock allocation paths; all patch guards remain. Both Big Maps Linux docs
and multiplayer INSTALL document default-on, kernel-origin fault risk and the
SIGBUS workaround (`terrain_cache_compress=0`, restart). This supersedes the
compression default limitation in the earlier memory revisit; it does not enable
alignment batching, material paging or any other outstanding memory feature.

The native lobby dispatcher accepts exactly 32 lowercase hex digits from
`transport_lobby.epoch`, matching Windows. It stores the nonce under the model
mutex, writes `lobby=` immediately, and includes it in every subsequent ctl
rewrite. An event before a successful first write survives in the model. A fresh
session resets it with Model; stale child generations cannot replace it. The
existing bridge parser and UDP world filtering need no changes.

These changes add no engine addresses, byte patterns, struct offsets or ABI
assumptions. The existing native pager is reused; the actual build-35924 ELF
passed its build-id and all 27 site checks. See the existing
[Big Maps RE evidence](../../bigmap/docs/linux/PORT.md).

## Not ported

None of these two commits. Earlier documented native feature gaps and retained
join lifetime validation remain outstanding. Successful build/fixtures and pager
startup do not establish loaded-world or cross-platform gameplay acceptance.

## Live testing

Newly built loader, menu, bridge, slice, plugin host, Big Maps plugin/config and
merged Lua were installed only in backed-up native lab actor copies. Sparse
density was disabled for the trial to avoid modifying shared game resources.

The prescribed launcher failed immediately with `bwrap: setting up uid map:
Permission denied` (exit 1). A temporary launcher copy used the earlier documented
system-bwrap/no-inner-soldier workaround while preserving lab filesystem mounts.
The game loaded the plugin: this run's appended host log reports resident budget
1022 MiB, terrain allocation/disposal patches and lossless userfaultfd compression
enabled. It then exited 53: SteamAPI did not locate a running Steam instance.
No title menu, GPU selection, save load, terrain eviction in a world, gdb probe or
two-peer join was observed. No UI input was sent and no save changed.

The failed game automatically invoked a Steam restart helper (`steam.sh
steam://run/1066780//`), an unintended side effect of SteamAPI startup failure.
It could not write its log in the read-only lab view and reported missing
32-bit libGL. We did not invoke Steam ourselves or retry; remaining descendants
of our own launch process group were terminated. No external Steam process was
targeted. No game remains running.

Both payload directories were restored and `diff -qr` against their pre-run
backups returned 0 after both trials. Evidence is in this job's `meta/live/`,
including build/fixture logs, launcher copies, actor logs/data and restore checks.
Actor log copies include older runs; only the final appended startup is this trial.

## Tests

- `tools/linux/build_native.sh`: soldier build, 63/63 CTests, glibc baseline pass.
- Native lobby regression: early event with failed initial write, later ctl
  rewrite, role/speed rewrite, replacement/duplicate nonce, invalid values,
  stale child generation and fresh session reset.
- Big Maps fixture: missing key requests compression, explicit 0 disables,
  explicit 1 enables. Fixture intercepts pager startup to avoid host-permission
  dependence; existing pager tests exercise the backend.
- `python3 tools/linux/verify_lua_release.py`: 29 Lua files, one pinned exception,
  32 exact HUD textures. Manifest
  `cbcd41b781d05d4f475926a898d5cd1cf44c637207e20d962279cc4f43863885`.
- Big Maps ELF checker: build-id and 27 sites pass.
- `tools/late_loader_test.py`: ALL OK using clone-local venv with lupa (system
  Python lacks it); sync operation 31 tests, sync runtime 17 tests and native
  live-join policy test pass.
- Whitespace checks pass with `core.whitespace=cr-at-eol` for upstream CRLF.
