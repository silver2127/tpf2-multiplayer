# Upstream dev 8e31f1e0 integration: tint diagnostics

Windows target: `8e31f1e01a0275c4fe8b39c65a56a362af4c45c7`.
Linux merge parent: `81ca6e59ed95744ee19aff156a89bc19664dbbb4`, following
[db8a4776](UPSTREAM_dev_db8a4776.md). One Windows commit, no conflicts.
The merge remains staged and uncommitted. Status: partial native integration.

## Merged

`native/src/slice_hook.cpp` remains byte-identical to upstream. It counts
station-label tint requests/results and reports periodic totals, logs the
first six window binds before ownership checks, and names the first six
station-icon no-owner failures by lookup stage. Windows paths are intact.
No Lua or lobby changes arrived. All 28 Lua files match the target exactly;
manifest SHA-256 is
`6de7dc1e234f7c07131674197103914f9803ccdd38ac5888673a96bcfd7ba8de`.

## Ported

Update the Linux Lua verifier and release BUILDINFO to the exact target,
package this integration record, and advance README/install/resume coverage.
No native tint behavior was enabled: Linux has no equivalent tint helpers
in which to place these diagnostics.

## Not ported

StationLabelTint counters/first-six results/alive totals, unconditional
first-six WindowTint bind logging, and StationIconTint's staged no-owner
logs. A fresh static ELF investigation followed the window binding through
its string setter and checked the label draw and HUD component access paths.
It established further ID-setter evidence, but not native style mutation,
label relay state/engine lifetime, or a safe StationGroup-to-owner traversal.
See [RE evidence, bytes, ABI and missing proof](../re/linux/DEV_8E31F1E0.md).
Adding counters without these underlying tint paths would not report the
upstream decisions. No guessed hooks or ineffective native flags were added.
Earlier icon/window/station tint and other integration gaps remain unchanged.

## Tests

- `tools/linux/build_native.sh`: soldier build, all 45 CTests and glibc <=2.31
  checks passed. Log: `.git/port-8e31f1e-build.log`.
- `python3 tools/linux/verify_lua_release.py`: 28 exact Lua files; manifest above.
- `python3 tools/linux/verify_dev_d3135a59_elf.py <native-game>`: build-id,
  ten guard spans, five branches, instruction boundaries/no interior targets,
  PlayerOwned RTTI and ViewCreator vtable passed.
- `bash -n tools/linux/build_release.sh`, staged whitespace validation
  (allowing upstream CRLF), upstream source equality and empty unmerged index.

No native behavior changed, so no synthetic tint tests were added. The existing
native tests ran in full. No live rendering/cross-platform test is claimed;
no game/Steam launch, installation, publication, commit or merge abort occurred.
