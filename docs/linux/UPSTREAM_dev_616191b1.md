# Windows dev 616191b1 integration: crashed run's dumps in OPEN LOGS

## Merged

Windows target: `616191b15deddd5a2f17402e8422f1acfd24f2a7`.
Linux parent: `02bc737447c268aa405609f7e8f314019a9d02ed`.
One Windows commit, no conflicts. Windows `native/src/logarchive.h` and
`tools/logarchive_test.cpp` are retained byte for byte from the target.
The merge remains staged and uncommitted. Release stays 0.7.0.5.

## Ported (what and how)

Native OPEN LOGS now includes the newest startup archive's `game_stdout.txt`
and every regular `crash_*` file with the `previous_run_` prefix. This also
fills the missing native previous-run game-log step on which upstream's fix
relies. The startup archive already counts the dead run's dumps for the
"since last archive" cutoff; recovering them from that archive keeps them
available after restarting the game.

Selection follows native archive-name ordering, including same-second
suffixes, independently of the retention list's 64-entry storage limit.
Copies preserve the original archive and precede state collection. Dump
copies have no log-tail truncation and share the existing 200 MiB budget;
files exceeding the remaining budget are skipped and noted. Startup archives
do not recursively include previous archives. No Windows-only per-dump
stdout is fabricated; existing archived companions are carried along.

README, INSTALL, release packaging and Lua provenance name this integration.
No game hook, byte pattern, engine offset, ABI or lifetime contract changes.
Existing game log discovery and startup ordering are unchanged. See
[log collection evidence](../re/linux/LOGS_TOOLS.md#dev-616191b1-previous-run-crash-recovery-2026-09-26).

## Not ported

None from this commit. Earlier unrelated native limitations remain unchanged.

## Live testing

No game was launched and no live crash, OPEN LOGS UI, GPU selection or
cross-platform result is claimed. This change concerns filesystem collection
only and adds no unresolved engine contract needing static/live reverse
engineering. The native archiver itself was exercised with isolated filesystem
fixtures, including a synthetic dump larger than 32 MiB. Steam, lab actor
contents and saves were untouched; no backup/restore or live logs were needed.

## Tests

- `tools/linux/build_native.sh`: pinned soldier build, 69/69 CTests and
  glibc <=2.31 checks pass. Log: `.git/port-616191b1-build.log`.
- `python3 tools/linux/verify_lua_release.py`: 29 Lua files (one pinned native
  exception) and 32 HUD glyphs pass. Log: `.git/port-616191b1-lua.log`.
  Manifest SHA-256:
  `d23c25e33ce4089c724211236a1344e7f710d4a6fb7c431f2d60f4a6838892e2`.
- Release shell syntax, whitespace (allowing existing CRLF), byte-identical
  Windows files, empty unmerged index and retained MERGE_HEAD checked.

The extended native `logarchive` regression checks startup capture followed
by OPEN LOGS with the original dump excluded by timestamp, separate current
and previous game logs, byte-exact >32 MiB dump recovery, `crash_*` companion
copying, unrelated-file exclusion, source preservation, manifest provenance,
newest-startup selection, and no recursive startup collection. Existing
identity, redaction, state-tail, retention and liveness checks remain.

The regression failed on the original Linux implementation (missing
previous-run file) and passed with the port. Evidence:
`.git/port-tests/regression-before.log`.
Windows tests were preserved but not executed locally. No publication,
commit or merge abort occurred.
