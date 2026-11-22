# Windows dev 8978635d integration: launcher pages and install-file releases

## Merged

Windows target: `8978635dcdbb0018a02a597e5c1d3aeee58bf0e5`.
Linux parent: `f5f88fe7c80b24ef7e69e642cc117fadcf95efae`.
One Windows commit, no conflicts. Merge staged and uncommitted.
Retain the upstream publisher, MSI workflow and development guide byte-for-byte.

## Ported (what and how)

The shared Python publisher already handles Linux. Each version now has a
`<version>` page containing the Windows launcher and Linux AppImage, marked
Latest for stable releases, and a `v<version>` release containing install files.
Native `.run`, `.tar.gz` and `.sha256` assets go to both the mod repository's
`v<version>` release and the matching packages release. The page is published
first, then the install files, preserving older launcher selection behavior.
A standalone launcher publication re-creates the newest published stable
`v<version>` release afterward, preserving its name, notes and asset bytes.

Update README and native installation guidance, extend the offline publisher
suite, and include this record in native packages. Fix the old fake release
response to include the real API's `tag_name` field used by the new publisher.
No native installer changes are needed: it consumes local packages and has no
remote release selector. Keep native version 0.7.0.5 and the dev 616191b1
Lua/BUILDINFO baseline; no runtime or Lua content changed.

Reviewed the native layout and RE records, including `MENU_GAME.md` and
`DEV_363C38CC.md`. This commit changes no engine hooks, addresses, byte patterns,
structures or ABI contracts, so static ELF analysis and live RE are unnecessary.

## Not ported

None from this commit. Previous native feature gaps, including terrain-sidecar
capture/serving, and cross-platform gameplay validation limits remain unchanged.
The external launcher is not part of this repository; its behavior is described
by upstream and was not independently executed here.

## Live testing

No game, launcher binary, Proton peer, gdb probe or XTEST sequence was run.
The changed behavior was tested with local payloads and an in-memory GitHub
service; no credentials were read, network requests sent or releases published.
No live game or release-service result is claimed. Steam, lab actors, saves
and the user's installed mod were untouched; no restoration was necessary.

## Tests

- `tools/linux/build_native.sh`: pinned soldier build, **69/69 CTests**, and
  glibc compatibility checks passed.
- `python3 tools/linux/verify_lua_release.py`: **29 Lua files** (one pinned
  native integration) and **32 HUD glyphs** passed. Manifest SHA-256:
  `d23c25e33ce4089c724211236a1344e7f710d4a6fb7c431f2d60f4a6838892e2`.
- `python3 tools/linux/test_launcher_release.py`: **20 offline tests** passed.
  Coverage includes native files on both install releases, launcher-page
  contents and publication order, prereleases, draft default, published-page
  protection, corrupt native payloads, Windows-only Proton fallback, republish
  metadata/bytes/order, digest failure before deletion, dry-run suppression,
  stable install-release selection, and existing launcher regression cases.
- `bash -n tools/linux/build_release.sh`, whitespace, empty unmerged index and
  exact preservation of the three incoming files checked.

Logs: `.git/port-8978635-{build,lua,launcher}.log` in this disposable clone.
No commit, merge abort, tag, push or publication was made.
