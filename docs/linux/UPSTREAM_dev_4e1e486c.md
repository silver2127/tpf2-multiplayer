# Windows dev 4e1e486c integration: launcher-only releases

## Merged

Windows target: `4e1e486c0db07d31631ce9df9346b7aefc16f7ef`.
Linux parent: `39b027d3ae30525e00c4266c0ed15c2b7464eaaa`.
One incoming commit; no conflicts. Keep the merge staged and uncommitted.
Retain upstream's MSI workflow, publisher, Proton installers and their tests,
release statistics and launcher documentation. Install assets move to matching
tags in `silver2127/tpf2-multiplayer-packages`; the mod releases carry the two
launchers. Proton downloads prefer packages with the upstream legacy fallback.
Existing published releases through 0.7.0.5 are left alone.

## Ported (what and how)

The native builder previously produced only unsuffixed `.run` and `.tar.gz`
files, while the new publisher requires `-native.run`, `-native.tar.gz` and
`-native.sha256`. Emit byte-identical suffixed copies and their external
checksum manifest directly from `tools/linux/build_release.sh`. Preserve the
old filenames and the archive's original directory for local installation and
`auto_install.py`. The native installer consumes a local package and has no
remote release lookup to migrate. Update manual download/checksum instructions,
link and package this record. An offline test passes actual builder outputs to
the publisher's validator and checks corrupt/missing asset rejection.

No engine code, executable addresses, bytes, struct offsets, SysV ABI or object
lifetimes change. Review of `docs/re/linux/MENU_GAME.md` confirms the existing
evidence scope; this packaging change requires no new static or live engine RE.
Keep the ba1fa26e Lua verification/BUILDINFO baseline: no incoming mod runtime
files change. Release/handshake version remains 0.7.0.5; this prepares the new
asset layout without inventing a 0.7.0.6 version bump.

## Not ported

None for this commit. Earlier native feature gaps and gameplay validation
limits remain unchanged. External launcher source is not part of this merge.

## Live testing

No game, Proton peer, gdb session, desktop input or remote publisher was run.
No game behavior or launcher download was observed or is claimed. This change
is validated with local package and fake-Steam installer tests. Steam, lab
actors, saves and the installed user mod were untouched; no restore was needed.
No release, tag, workflow or remote repository was published or modified.

## Tests

- `tools/linux/build_native.sh`: soldier build, 68/68 CTests and glibc <=2.31
  checks passed.
- `python3 tools/linux/verify_lua_release.py`: 29 Lua files (one pinned native
  integration), 32 HUD textures passed; manifest SHA-256
  `d23c25e33ce4089c724211236a1344e7f710d4a6fb7c431f2d60f4a6838892e2`.
- `tools/linux/build_release.sh --build-dir native/linux/out-soldier
  --out dist/port-4e1e486 --no-netpunch`: passed, including its 68 native tests,
  packaged Lua check and export verification. This is a development test
  package without a frozen lobby, not a publishable multiplayer bundle.
- `python3 tools/linux/test_release_assets.py dist/port-4e1e486 0.7.0.5`:
  actual builder outputs accepted by `publish_release.linux_files`; old/new
  bytes and executable mode match; corrupt `.run`/tarball and missing installer
  rejected. No publisher CLI or network request was executed.
- `python3 tools/linux/test_installer.py
  dist/port-4e1e486/tpf2mp-linux-0.7.0.5-native.run`: passed extraction/checksums,
  dry-run, wrapper environment, install, aliases, upgrade, loading preflight,
  legacy migration and uninstall/data preservation in a fake Steam tree.
  The first run failed the test's unconditional lobby-presence assertion on
  the deliberate `--no-netpunch` package. Update the test to honor that exact
  BUILDINFO omission marker while still requiring the lobby otherwise; rerun
  passed. No production installer change was needed.
- `python3 tools/linux/test_auto_install.py`: 2/2 passed.
- `python3 tools/proton/test_install.py` and `test_install_sh.py`: passed
  offline install/upgrade/uninstall checks, including Python packages-first
  release selection and legacy fallback. No Windows one-file lobby was
  available; its repair ran only on the suite's pinned-hash paths.
- Shell syntax, diff whitespace, empty unmerged index and retained MERGE_HEAD
  checked. Windows/shared tools remain identical to the incoming commit.

Logs: `.git/port-4e1e486-*.log` in this clone. Test temporary directories were
kept under `.git/port-tmp`; generated package outputs were moved from
`dist/port-4e1e486` to `.git/port-4e1e486-package` after testing. No generated
binaries are staged.
