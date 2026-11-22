# Windows dev e2957841 integration: migrate a published release to two launchers

## Merged

Windows target: `e295784170d9a43c7d526c5c826abf817a2096bf`.
Linux parent: `171f767d421efec22776a28b7e82d335faedc9b6`.
One incoming commit, no conflicts. Merge remains staged and uncommitted.
Retain `tools/publish_release.py` byte-for-byte from upstream.

## Ported (what and how)

The shared Python publisher adds `page v<version>` for an existing published
version. It creates a launcher page at the commit named by the version tag,
replaces the old download table with Windows/Linux launcher links, and keeps
the version notes. With publication enabled, the page is published first
(Latest for stable versions), then the original v-tagged install release is
re-created with an install-files title and its asset bytes checked against
available digests. This preserves the selection order older launchers require.
Upstream records having applied this layout to 0.7.0.5; no external release
state was independently queried or modified during this integration.

No platform-specific adaptation is needed: the native `.run`, `.tar.gz` and
`.sha256` assets are preserved by the same shared path as the Windows files.
Extend the existing offline publisher suite to exercise the migration, update
README and native installation guidance, and include this record in packages.
Keep version 0.7.0.5 and the dev 616191b1 Lua/BUILDINFO baseline, following the
previous release-only integration; no runtime sources changed.

Reviewed README, Linux installation and integration records, native build and
release tooling, and the Linux RE records (including DEV_DB8A4776.md). No game
address, patch bytes, struct layout, ABI, ownership or lifetime contract changes
in this commit. Static ELF analysis and live reverse engineering are unnecessary.

## Not ported

None from this commit. Existing native feature gaps and gameplay-validation
limits are unchanged. The external launcher itself was not executed.

## Live testing

No game, Proton peer, launcher binary, gdb session or desktop input was run.
Tests use fixture downloads and an in-memory GitHub service, with no real
credentials, network requests or publication. No live game or release-service
result is claimed. Steam, installed mod, saves and lab actors were untouched;
no actor backup or restoration was needed.

## Tests

- `tools/linux/build_native.sh`: PASS, pinned soldier build, 69/69 CTests,
  glibc compatibility checks.
- `python3 tools/linux/verify_lua_release.py`: PASS, 29 Lua files (one pinned
  native integration), 32 HUD glyphs. Manifest SHA-256:
  `d23c25e33ce4089c724211236a1344e7f710d4a6fb7c431f2d60f4a6838892e2`.
- `python3 tools/linux/test_launcher_release.py`: PASS, 26 offline tests.
  New coverage checks page-before-install publication, all eight install-file
  bytes including native assets, both launcher URLs, retained notes, lightweight
  and annotated tags, default draft behavior, prerelease Latest suppression,
  corrupt native digest rejection before install-release deletion, dry run,
  and invalid source/published page protection. A corrupt asset is detected
  after page publication but before deleting the install release, matching
  upstream behavior. Initial test fixture failures (fake API rejected DELETE)
  were fixed; no publisher behavior was changed.
- `bash -n tools/linux/build_release.sh`, staged whitespace, empty unmerged
  index, incoming publisher equality and retained MERGE_HEAD checks: PASS.

Logs in this clone: `.git/port-native-build.log`, `.git/port-lua.log`,
`.git/port-launcher.log`. Test temporary files stayed under `.git/port-tmp`.
No commit, merge abort, tag, push or publication was made.
