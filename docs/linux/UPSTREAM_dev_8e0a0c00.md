# Windows dev 8e0a0c00 integration: launchers on the original version page

## Merged

Windows target: `8e0a0c00efb09aa5fc322fe56b5a7bed3cc0990f`.
Linux parent: `4ef40cbf3474eb65c07c0401c020484c3b2fd385`.
One incoming commit, no conflicts. Merge remains staged and uncommitted.
The shared `tools/publish_release.py` and Windows build workflow are retained
byte-for-byte from upstream. Correct the remaining old download-table tag
reference in `docs/DEVELOPMENT.md`.

## Ported (what and how)

The shared Python publisher needs no Linux platform adaptation. For the new
layout, `v<version>` holds the launchers and is Latest for stable releases;
`<version>` holds update files and is not Latest. The packages repository
continues to hold Windows/Proton and native Linux files at `v<version>`.
Native `.run`, `.tar.gz` and `.sha256` assets retain their names and bytes.
The update-files release is published before the launcher page.

`page v<version>` copies the old payload to both install-file destinations
before removing it from the original page and attaching launchers there.
The annotated-page-tag and `--replace-page` flows are gone. Launcher-only
updates no longer delete/recreate a mod release. Launchers up to 1.2.0 must
be updated first: they expect an MSI on the now launcher-only `v<version>`.

Update the existing Linux offline publisher tests to this contract, replacing
tests for removed helpers. Cover native bytes in both destinations, launcher
placement, publication order, prereleases, checksum rejection, failed second
copy upload before source removal, repeated migration, dry-run suppression,
CLI routing, notes and the Windows-only Proton fallback. Update README and
Linux install guidance, and include this integration record in release packages.
Version stays 0.7.0.5; the cumulative dev 616191b1 Lua/BUILDINFO baseline stays
unchanged, consistent with the prior release-tool integrations.

Reviewed README, Linux install/build/release organization, prior integration
records (especially 9abb2af1), Linux RE evidence in `docs/re/linux/LOGS_TOOLS.md`,
and the Proton installers' packages-first lookup. No native source, game hook,
address, byte pattern, struct offset, calling convention or Lua change is
introduced. No ELF analysis or live ABI investigation is needed for this diff.

## Not ported

None from this commit. Existing native feature gaps and gameplay-validation
limits remain unchanged. Upstream publisher semantics are retained; fake-API
checks do not establish real GitHub release ordering or actual launcher behavior.
No unrelated release migration repair is introduced.

## Live testing

No game, Proton peer, launcher executable, gdb or desktop interaction was run:
this change concerns publishing, not game behavior. All publisher tests use
local fixtures and fake API responses. No real credentials, release requests,
publication or network download was used by these tests. Steam, installed
mods, saves and lab actors were untouched; no actor backup/restoration was
needed. No live gameplay or release-site result is claimed.

## Tests

- `tools/linux/build_native.sh`: PASS; pinned soldier build, 69/69 CTests and
  glibc <=2.31 compatibility checks. Log: `.git/port-native-build.log`.
- `python3 tools/linux/verify_lua_release.py`: PASS; 29 Lua files with one pinned
  cumulative merge and 32 HUD glyphs. Manifest SHA-256:
  `d23c25e33ce4089c724211236a1344e7f710d4a6fb7c431f2d60f4a6838892e2`.
  Log: `.git/port-lua.log`.
- `TMPDIR="$PWD/.git/port-tmp" python3 tools/linux/test_launcher_release.py`:
  PASS, 35 offline tests. Log: `.git/port-launcher.log`.
- `bash -n tools/linux/build_release.sh`: PASS.
- `python3 tools/check_doc_links.py`: FAIL on existing references in unchanged
  documents: 1 dead file link, 7 dead anchors and 80 missing prose paths
  (including nested Big Maps path-resolution diagnostics). No diagnostic names
  an edited document. Log: `.git/port-doc-links.log`.
- Staged whitespace, empty unmerged index, incoming publisher/workflow equality
  and retained MERGE_HEAD checks: PASS.

Temporary publisher fixtures were kept inside the clone. No release package
was built or published. No commit, merge abort, tag or push was made.
