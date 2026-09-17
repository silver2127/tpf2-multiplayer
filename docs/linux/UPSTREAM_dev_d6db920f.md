# Upstream dev d6db920f integration: HUD icon-element company styles

Windows target: `d6db920fa7c5689711c7f250daca170e1dd40db2`.
Linux merge parent: `552e606a330cb36d3c254b9b60a11eff88395cc5`, following
[be3b86ae](UPSTREAM_dev_be3b86ae.md). Three incoming commits, no conflicts.
The merge remains staged and uncommitted. This integration is partial.

## Merged

- 4e0ec0f: company class on the actual station/depot icon, owner helper
  refactoring, Windows entry/call hooks and glyph diagnostics; shared
  backgroundColor1 and hover selectors.
- e69a3cf: Python bytes() spelling for the Windows mov rcx,rbx check.
- d6db920: upstream merge bringing these changes onto dev.

Windows code and tests are preserved exactly. All 28 Lua files match the
Windows target; manifest SHA-256:
`610f79e6c144691993e531b228e87fb09f60146fae8d2fa4f930610e6129f0e4`.

## Ported

Shared stylesheet merged unchanged. Linux Lua verification, release BUILDINFO,
packaged integration records and coverage/install documentation identify the
new target. No native patch was added; the existing safe build is retained.

## Not ported

Native icon-element class application, owner-helper integration and glyph
counters. Fresh static analysis found eleven station and five depot carrier
class calls, their icon pointers and native string arguments. It did not
establish the full entity/owner/engine lifetime contract required for safe
hooks. See [DEV_D6DB920F.md](../re/linux/DEV_D6DB920F.md) for exact bytes,
anchors, attempted traces and remaining evidence. Earlier native tinting and
other coverage gaps remain. The shared styles do not enable tinting alone.

## Tests

- `tools/linux/build_native.sh`: soldier build, 45/45 CTests and glibc <=2.31
  import checks passed.
- `python3 tools/linux/verify_lua_release.py`: 28 exact Lua files, zero exceptions.
  Updated baseline now checks the new stylesheet as part of the full manifest.
- `bash -n tools/linux/build_release.sh`: passed.
- Python AST parse of `tools/stationicon_bytes_test.py`: passed. The Windows
  PE byte test was not executed; native ELF evidence was checked separately.

Build and Lua logs: `.git/port-d6db920f-{build,lua}.log`. No game or Steam
was launched; no live visual or cross-platform compatibility claim is made.
No installation, publication, commit or merge abort occurred.
