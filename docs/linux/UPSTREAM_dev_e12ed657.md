# Windows dev e12ed657 integration: separate launcher updates

## Merged

Windows target: `e12ed657d7849b106f933951c8adc5da608301f6`.
Linux parent: `60187681ba45dd3bb7165d364266350802c8cc24`, following
[4e1e486c](UPSTREAM_dev_4e1e486c.md). One commit, no conflicts;
merge staged and uncommitted. Keep upstream `tools/publish_release.py` and
`docs/DEVELOPMENT.md` unchanged.

## Ported (what and how)

The shared Python publisher runs on Linux without adaptation. Its new
`launcher` mode takes the launcher source release's version, creates a
`launcher-v<version>` draft, attaches Windows Setup.exe and Linux AppImage
under the established names, and sets `make_latest=false` both at creation
and publication. It uses the existing download/checksum validation. Mod
releases still require `--linux-dir`; launcher releases do not need native
payloads or a packages release. The MSI workflow selects `v*` tags only.

Add offline publisher regression tests and native installation documentation;
include this record in native packages. The external launcher's tag filtering
is outside this repository and was not independently executed. The native
installer consumes local packages and has no remote release selector to port.
No runtime files change: retain the ba1fa26e Lua/BUILDINFO baseline and
0.7.0.5 version. Review of `docs/re/linux/MENU_GAME.md` and
`DEV_6E1E4EC7.md` confirms no engine contract is involved. No new address,
byte pattern, struct offset or SysV ABI mapping needs static or live RE.

## Not ported

None from this commit. Earlier native feature gaps and gameplay validation
limits remain unchanged.

## Live testing

No game launch, Proton peer, gdb attachment, XTEST input, launcher execution
or remote publication was performed. The release behavior was exercised only
with in-memory HTTP fixtures and a fake GitHub client; no credentials were
read. No game behavior or real release-service result is claimed. Steam, lab
actors, saves and the user's installed mod were untouched; no restore needed.

## Tests

- `tools/linux/build_native.sh`: pinned soldier build, 68/68 CTests and
  glibc <=2.31 checks passed.
- `python3 tools/linux/verify_lua_release.py`: 29 Lua files (one pinned native
  integration), 32 HUD textures passed; manifest SHA-256
  `d23c25e33ce4089c724211236a1344e7f710d4a6fb7c431f2d60f4a6838892e2`.
- `python3 tools/linux/test_launcher_release.py`: 9 offline tests pass:
  both verified launcher payloads and stable names, draft default, non-Latest
  publication, damaged AppImage rejection before writes, published-release
  protection, dry-run without uploads, Windows-only override, invalid source
  version rejection, and CLI routing/required native directory behavior.
- Release builder shell syntax, whitespace, upstream file equality and merge
  state checked. Logs: `.git/port-e12ed657-{build,lua,launcher}.log`.

No commit, merge abort, tag, push or publication was made. Generated native
outputs and temporary files remain unstaged inside this clone.
