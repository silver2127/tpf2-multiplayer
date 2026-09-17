# Upstream dev 50d7588b integration: company class prefix correction

Windows target: `50d7588beb4a8689dd1d790a1095607afc507c1f`.
Linux merge parent: `52a54b3b958235696a72a60d0b5723b4f1d69b85`, following
[d6db920f](UPSTREAM_dev_d6db920f.md). One incoming commit, no conflicts.
The merge remains staged and uncommitted. This integration is partial.

## Merged

Preserved the Windows slice change exactly: TintClassPrefix returns `mpWinCo`
or `mpCo` without `!`, because the bang belongs to selectors, not stored
component classes. No hook addresses or ABI contracts changed upstream.

## Ported

Updated Linux integration/install/coverage documentation, Lua baseline and
release BUILDINFO/package records to the new target. All 28 Lua files remain
byte-identical to Windows; manifest SHA-256 is unchanged:
`610f79e6c144691993e531b228e87fb09f60146fae8d2fa4f930610e6129f0e4`.
Recorded the corrected naming contract for future Linux class tagging.

## Not ported

Native company tint class application, including this prefix correction:
Linux has no company-tint implementation to amend. Fresh static ELF analysis
reconfirmed the native class helper and station icon call, but did not establish
the entity/owner and lifetime contracts needed to enable tinting safely.
[DEV_50D7588B.md](../re/linux/DEV_50D7588B.md) records bytes, SysV/libstdc++
evidence, attempted investigation and missing prerequisites. Earlier native
tinting and other integration gaps remain. No guessed patch was introduced.

## Tests

- `tools/linux/build_native.sh`: soldier SDK build, 45/45 CTests and glibc
  <=2.31 import checks passed.
- `python3 tools/linux/verify_lua_release.py`: 28 exact Lua files, zero exceptions.
- `bash -n tools/linux/build_release.sh`: passed.
- Windows slice byte identity against the target and staged whitespace check
  (`core.whitespace=cr-at-eol`): passed.

Build and Lua logs: `.git/port-50d7588b-{build,lua}.log`.
No native tint tests exist and no native runtime behavior changed, so no
implementation-mirroring test was added. Lua verification checks every shared
stylesheet against the new upstream target. Windows source is retained exactly.

No installation, publication, commit or merge abort occurred. No Steam/game
process was launched; offline tests do not establish live visual compatibility.
