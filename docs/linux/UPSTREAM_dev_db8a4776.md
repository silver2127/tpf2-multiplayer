# Upstream dev db8a4776 integration: station tint safety and diagnostics

Windows target: `db8a477649d9ed1000023eed4d5cb4afa290d9d3`.
Linux merge parent: `1a1252f42738316d2ff704f970b1ef17b884d6c6`, following
[5fb7aea2](UPSTREAM_dev_5fb7aea2.md). One Windows commit, no conflicts.
The merge is staged and uncommitted. Status: partial native integration.

## Merged

Preserve `native/src/slice_hook.cpp` and `tools/stationicon_bytes_test.py`
exactly as upstream. The Windows change replaces asserting StationGroup
lookup, moves tint to the button root, adds class read-back, a tint-class
override and window/station alive counters. Windows paths remain intact.
All 28 Lua files match the target byte for byte. Manifest SHA-256:
`6de7dc1e234f7c07131674197103914f9803ccdd38ac5888673a96bcfd7ba8de`.

## Ported

Advance Linux Lua verification and release BUILDINFO provenance to db8a4776;
package this record and update README, installation and resume coverage.
No native rendering behavior is newly enabled. The existing native train
Name lookup already uses a non-asserting slot scan; there is no existing
station/window tint helper to update.

## Not ported

The station/window tint changes: non-asserting StationGroup owner lookup,
button-root hook, `tintclass=mpCo`, class-list read-back and alive counters.
Their native tint prerequisites remain absent. A fresh static investigation
confirmed the wrapper's root allocation is preserved in r13, while rax is
clobbered by its canary check; confirmed Linux GetComponentDataIndex reaches
the missing-component assertion; and traced the remaining addStyleClass
xref to metadata/string processing. It did not establish complete
entity/UI-engine lifetime, StationGroup storage and owner access, native
style append or class-list layout, or safe relay state preservation.
See [addresses, bytes, ABI evidence and remaining proof](../re/linux/DEV_DB8A4776.md).
No speculative hook or ineffective native flag was installed. These flags
continue to affect only Windows. Previous ownership, recovery, icon/label
colour and Workshop gaps remain unchanged.

## Tests

- `tools/linux/build_native.sh`: soldier build, 45/45 CTests and glibc <=2.31
  checks passed. Log: `.git/port-db8a477-build.log`.
- `python3 tools/linux/verify_lua_release.py`: 28 exact Lua files; manifest above.
- `python3 tools/linux/verify_dev_d3135a59_elf.py <native-game>`: build-id,
  ten guard spans, five branches, instruction boundaries/no interior targets,
  PlayerOwned RTTI and ViewCreator vtable passed.
- `bash -n tools/linux/build_release.sh`, staged whitespace check (respecting
  upstream CRLF), empty unmerged index and upstream source/test equality passed.

No native tint code was changed, so no synthetic tint tests were added or
claimed. Existing native UI and non-asserting train lookup tests were run.
Windows PE test was not run: it requires a Windows executable at its hard-coded
path. No game/Steam launch, live rendering test, installation, publication,
commit or merge abort occurred.
