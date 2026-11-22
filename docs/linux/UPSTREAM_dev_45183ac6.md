# Windows dev 45183ac6 integration: comprehensive OPEN LOGS

## Merged

Windows target: `45183ac61ae40ca5b598c93c73435927164e79f6`.
Linux parent: `9c0c19a16c9417b6deda96b53bd70751b6f9df93`.
One Windows commit, no conflicts. Preserve the Windows archiver, Proton
winebrowser/host-Steam changes, Windows regression and MSI workflow changes.
The merge remains staged and uncommitted. Release stays 0.7.0.3.

## Ported (what and how)

`native/linux/src/logarchive_linux.h` now includes:

- Installed version from the native install manifest's `version` field, with
  the Windows-style game version file as fallback; unknown is explicit.
- Linux kernel/architecture, local time zone and UTC offset. Module size,
  UTC modification time and GNU build ID replace PE link timestamps for the
  game, native libraries, plugins and installed lobby executable. Identities
  describe files on disk, including when collecting a previous session.
- Data `*.txt` and native `tpf2*.cfg`, game/mod-root configuration and menu
  flags, plugin configuration, and both native and game-fallback lobby
  `*.jsonl`, `*.json`, `*.txt`. State is always copied, never moved, after
  logs/dumps, using the remaining 200 MiB budget and 8 MiB per-file tails.
- Same-length masking of upstream's nine invitation/password string keys in
  lobby copies, including escaped value bytes and multiline whitespace.
  A truncated lobby tail's incomplete first line is blanked so it cannot
  reveal a credential whose key was cut off. Failed redaction removes the
  copy. Original lobby files are untouched. Incoming saves and terrain dumps
  are excluded.
- Five archives per kind. Existing liveness locking, bounded reads,
  permissions, startup log rotation and keep-logs override are preserved.

The native menu already uses `xdg-open` and names the native logs location;
its existing implementation supplies the Proton menu change's native
counterpart. Native Steam stdout discovery already checks host Steam paths
and selects the newest stdout. No Wine functions belong in the native build.

No game hook, machine-code patch, ABI, field offset or lifetime contract changes.
See [log evidence](../re/linux/LOGS_TOOLS.md) for the existing game log contract
and this integration's ELF identity check. No new reverse-engineered site is
required. Lua verification/release provenance advance to this target, and
release packaging includes this record.

## Not ported

None for this commit. Earlier unrelated limitations remain unchanged.

## Live testing

Installed the newly built soldier libraries and current Lua into the native
lab actor after `cp -a` backups. Also backed up userdata to preserve settings
and saves. Existing `.before-port` backups were left untouched; this run used
`.before-port-45183ac6` suffixes.

Ran `tools/sandbox/tpf2mp-lab run native --root ~/.local/share/tpf2mp-lab`.
The launcher exited 1 immediately with `bwrap: setting up uid map: Permission
denied`. The game did not start: no title menu, GPU selection, startup archive,
OPEN LOGS UI or file-manager result was observed. No gdb probe was needed or
performed. No Steam restart or launch outside the lab was attempted.

Restored libraries/data, Lua and userdata from the backups; no game remains
running. Evidence: job `meta/live/launch.txt`, `restoration.txt`, `actor-logs/`
and `data/`. The copied actor logs/data predate this failed launch and are
not evidence of this build executing in-game.

## Tests

- `tools/linux/build_native.sh`: soldier build, 68/68 CTests and glibc <=2.31
  checks passed. New `logarchive` regression covers versions, ELF identities,
  state/tails, both lobby locations, credential masking, exclusions, five-per-
  kind retention, startup move versus state copy, liveness and keep-logs.
- The soldier GCC 8 test target links `stdc++fs`; production code remains
  C/POSIX-based and header-only, with no throwing C++ container operations.
- The archiver's ELF reader returns
  `3a0e156390b0e6f1e372051c24802c8493ae454a` for the actual lab game,
  matching `readelf -n`. Non-ELF input yields no invented identity.
- `python3 tools/linux/verify_lua_release.py`: 29 Lua files (one pinned native
  exception), 32 exact HUD glyphs. Manifest SHA-256:
  `d23c25e33ce4089c724211236a1344e7f710d4a6fb7c431f2d60f4a6838892e2`.
- Release shell syntax, whitespace (allowing existing CRLF), retained upstream
  hunks, empty unmerged index and retained MERGE_HEAD checked.

Build/test evidence: `.git/port-45183ac6-build.log`,
`.git/port-45183ac6-lua.log`, `.git/port-45183ac6-build-id.log`.
Windows/Proton tests were merged but not executed locally. No publication,
commit or merge abort occurred.
