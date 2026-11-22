# Windows dev c9ac009d integration

## Merged

Windows target: `c9ac009d6226f8956e4e94fd8fc38527e9c60113`.
Linux parent: `ddb493fd7c6ff737631d0379fb7e526cd7e49d95`, following
[4616c16a](UPSTREAM_dev_4616c16a.md). One incoming commit:
`bigmap: mirror bigmap/ into the standalone tpf2-bigmap repo on every dev push`.
Resolve and stage the sole conflict in `bigmap/README.md`; leave the merge
uncommitted.

Preserve upstream's workflow, Python mirror tool and six standalone README
fragments byte for byte. Retain the unified Windows MSI install/uninstall/build
wording and all six standalone replacement markers. The workflow still checks
out Windows `dev`; this integration does not configure publication of the
Linux branch. No workflow or remote sync was executed.

## Ported (what and how)

Close the replaceable ABI block before the existing native Linux scope note.
This preserves both the upstream transformation boundary and the native
installation link, depth defaults, limitations and attribution of upstream
playtesting. The Linux note survives an in-memory standalone transformation.
Retain the existing native performance-catalog qualification as well.

Link this record from the root README and include it in Linux release
packaging, following the preceding integration. Runtime code, release version,
native defaults and the bd69b864 Lua verification/BUILDINFO baseline are
unchanged: the incoming commit changes no mod files. The mirror tool is shared
Python/Git tooling already intended for an Ubuntu runner.

No executable patch, address, byte pattern, ABI or struct offset changes.
There is no Linux hook counterpart to derive, so no new static or live reverse
engineering is required. Existing evidence in `docs/re/linux/` and
[Big Maps PORT](../../bigmap/docs/linux/PORT.md) remains applicable.

## Not ported

None from this commit. Earlier runtime feature gaps and live-validation limits
remain as recorded; this tooling/documentation integration does not close them.

## Live testing

No game launch, gdb attachment or actor installation was needed or performed.
No gameplay, rendering or cross-platform result is claimed. Lab actors, saves,
Steam and the user's installed mod were untouched; no restoration was needed.

## Tests

- `tools/linux/build_native.sh`: soldier SDK build, **66/66 CTests passed**,
  glibc <=2.31 compatibility checks passed.
- `python3 tools/linux/verify_lua_release.py`: **29 Lua files** (one pinned
  native integration) and **32 HUD textures** passed. Manifest SHA-256:
  `b67ff99a9d55f248af9ea23290c96bbf0904cd83b533338bcf48d16878023aaf`.
- In-memory sync smoke check: all six blocks have matching fragments and
  transform without leftover markers; Linux scope note survives; plugin ABI
  include rewrites correctly. Workflow, sync script and all six fragments are
  byte-exact upstream. Incoming mod tree matches the retained Lua baseline.
- `bash -n tools/linux/build_release.sh`, merge-state checks and whitespace
  checks passed with `core.whitespace=-blank-at-eof`. Default whitespace check
  reports only the six upstream fragment trailing blank lines, retained for
  replacement text separation.
- No existing Linux tests cover README block placement; no permanent test was
  added for this documentation conflict. No release package, remote workflow
  or end-to-end mirror publication was run.

Logs: `.git/port-c9ac009d-{build,lua,sync}.log` in this disposable clone.
