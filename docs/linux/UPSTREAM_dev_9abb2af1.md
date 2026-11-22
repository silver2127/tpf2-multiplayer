# Windows dev 9abb2af1 integration: repeatable launcher-page migration

## Merged

Windows target: `9abb2af111d26f7a9ad043d202fcbf591ca42833`.
Linux parent: `4787159f0a3801f01b0f3dff8efd2c1b2747cc17`.
One incoming commit, no conflicts. Merge remains staged and uncommitted.
`tools/publish_release.py` is retained byte-for-byte from upstream.

## Ported (what and how)

The shared Python publisher runs on Linux without platform adaptation. It
creates annotated page tags at the version tag's commit, replaces an existing
lightweight page tag, and retains an existing annotated tag. Both new-version
and page-migration paths use the helper. Page migration requires explicit
`--replace-page` to recreate a published page, including in dry runs, and
removes one earlier migration-note prefix before composing the new notes.
Publication still puts the page before the install-files release; native
assets retain their bytes alongside the Windows/Proton assets.

Extend the existing offline publisher suite for tag creation, replacement,
retention, CLI parsing, source-title preservation and repeated-note removal.
Update README/install guidance and package this integration record. Preserve
version 0.7.0.5 and the dev 616191b1 Lua/BUILDINFO baseline, as in the prior
release-only integrations.

Decision: preserve the actual upstream title behavior. Despite the commit
subject, it copies the source release name verbatim; if an earlier migration
renamed that source to include `(install files)`, the new page inherits that
suffix. There is no suffix-removal code in this commit. Existing annotated
tags are also retained rather than re-dated. No unrelated publisher fix was
introduced, and no external ordering/title result is claimed.

Reviewed README, docs/linux/INSTALL.md, prior integrations (especially
e2957841), native build/release tooling and docs/re/linux/LOGS_TOOLS.md.
No game hook, address, bytes, struct offset, ABI or runtime code changes;
static ELF analysis and live game RE are not needed for this integration.

## Not ported

None from this commit. Inherited native feature gaps and gameplay-validation
limits remain unchanged. The source-title behavior above is shared upstream
behavior, not a Linux port omission.

## Live testing

No game, Proton peer, launcher binary, gdb session or desktop input was run.
The publisher tests use local fixture files and fake API calls only. No real
credentials, release API requests or publication were used. Steam, installed
mods, saves and lab actors were untouched; no backup/restoration was needed.

## Tests

- `tools/linux/build_native.sh`: PASS; pinned soldier build, 69/69 CTests,
  glibc compatibility checks. Log: `.git/port-native-build.log`.
- `python3 tools/linux/verify_lua_release.py`: PASS; 29 Lua files with one
  pinned cumulative integration, 32 HUD glyphs. Manifest SHA-256:
  `d23c25e33ce4089c724211236a1344e7f710d4a6fb7c431f2d60f4a6838892e2`.
  Log: `.git/port-lua.log`.
- `python3 tools/linux/test_launcher_release.py`: PASS; 31 offline tests,
  including native asset preservation, publication order, checksum rejection,
  new annotated-tag flows and page replacement. Log: `.git/port-launcher.log`.
- `bash -n tools/linux/build_release.sh`: PASS.
- Staged whitespace, empty unmerged index, incoming publisher equality and
  retained MERGE_HEAD checks: PASS.

An exploratory invocation of `test_release_assets.py` without its required
DIR/VERSION arguments exited with IndexError; no packaged-output test is
claimed. No release package was built. Native asset handling is covered by
the offline publisher suite. Temporary test files stayed inside this clone.
No commit, merge abort, tag, push or publication was made.
