# Upstream dev 66c870cf integration: HUD glyph overlay

Windows target: `66c870cfb4595d361d5c65221516cd1dcb967284`.
Linux merge parent: `26190ab0b96caa96860959f713cac7d144539fbc`, following
[50d7588b](UPSTREAM_dev_50d7588b.md). One incoming commit, no conflicts.
The merge remains staged and uncommitted. Cumulative native integration is partial.

## Merged

Preserved the shared stylesheet and all 32 TGA textures exactly as upstream.
Station/depot carrier selectors choose white-on-transparent glyph masks at
1x/2x resolution. Company classes colour backgroundColor2; backgroundImage1
and the game's blue box remain untouched. Default glyph colour is white.
Window title-bar colours and company swatches remain in the shared sheet.
No Windows native source changed.

## Ported

Linux receives the shared resources through the existing whole-mod packaging.
Updated the Lua baseline, release BUILDINFO, packaged integration record and
README/install/resume documentation. Extended the release verifier to require
byte-exact HUD glyph assets, including when checking a staged package.
All 28 Lua files match upstream without exceptions. Manifest SHA-256:
`003b2eea3d56ab214d8b44364a3e744663f4ea22392fe1566107d07215f7ef32`.
No new hook, ABI adaptation or game offset is required by this shared-only diff.

## Not ported

Visible native company tinting retains the earlier missing class-tagging
prerequisite. This commit adds no new platform-specific omission. Reviewed
native sources and prior investigations, and rechecked the native carrier
class call against the actual ELF. This establishes the carrier styling path,
but not safe entity/owner mapping, StationGroup traversal, stale-class cleanup
or relay/world lifetime. See [DEV_66C870CF.md](../re/linux/DEV_66C870CF.md).
No guessed patch is installed; earlier ownership, recovery and Workshop gaps
remain unchanged. Shared assets alone do not enable native company tinting.

## Tests

- `tools/linux/build_native.sh`: soldier build, 45/45 CTests and glibc <=2.31
  import checks passed. Log: `.git/port-66c870cf-build.log`.
- `python3 tools/linux/verify_lua_release.py`: 28 exact Lua files and 32 exact
  glyph textures passed. Log: `.git/port-66c870cf-lua.log`.
- Verifier exercised on a temporary package copy inside the clone: intact and
  restored copies pass; missing, corrupted and extra glyph textures fail.
- `bash -n tools/linux/build_release.sh`, staged whitespace, upstream shared
  file identity and empty unmerged index checks passed.

No game or Steam launch, installation, publication, commit or merge abort.
Offline checks do not establish live visual or cross-platform compatibility.
