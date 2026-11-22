# Windows dev 24b8f636 integration: guarded Fences boot and test repairs

## Merged

Target `24b8f636130debe70a502abc9655c598651b1dfa`, Linux parent
`542b611ce9146416ff88dcbd28786f0c63685d64`. Two incoming commits:
`50d01e0` (guard Fences compatibility loading) and `24b8f63` (repair stale
and flaky tests, remove empty safe.txt). No conflicts. Merge staged and
uncommitted; version remains 0.7.0.7.

## Ported (what and how)

The shared lockstep.lua is retained exactly from upstream. A failed earlier
boot skips Fences; require failures, malformed exports and bind failures now
turn multiplayer off with a diagnostic and an empty game script instead of
raising through game loading. Both platforms use this same Lua implementation.
Add Lua 5.2 regressions reaching this adapter with other factories stubbed:
missing module, wrong export, missing/invalid bind, bind error, later-module
suppression, and skipping Fences after an earlier failure. Linux-shaped loader
environment paths are supplied. Existing Fences tests exercise successful bind,
capture, geometry transport and mocked replay.

Retain upstream's isolated codec constant tables and corrected Windows UI
expectations. The hot-join test still failed locally: the joiner won every
receiving-stage merge, so accepted sender progress was empty although the
roster correctly went from 0% to 100% and then save received/loading. Record
sender proposals at the real merge function, assert they begin at 0% and never
regress, allow an empty accepted-progress list, and retain actual roster and
completion checks. No lobby production behavior changes.

Advance Lua verification and package BUILDINFO provenance to the target;
package this record and link it from README and INSTALL. No game hook,
address, byte pattern, struct offset, SysV ABI or lifetime contract changes.
The existing native contracts documented in docs/re/linux remain untouched;
new ELF analysis and live RE are unnecessary for these shared-code changes.

## Not ported

None from these two commits. Existing native feature gaps and gameplay
validation limits remain unchanged.

## Live testing

No game launched, debugger attached, or desktop input sent. No lab actor,
Steam state, installed mod or save changed; no backup/restoration required.
Hot-join validation uses real host/client threads and UDP loopback with a
fixture save, not the game. No live game or cross-platform gameplay result
is claimed.

## Tests

- `tools/linux/build_native.sh`: PASS, pinned soldier SDK, 72/72 CTests
  (53.06 seconds), native libraries built and glibc <=2.31 checks passed.
- `python3 tools/linux/verify_lua_release.py`: PASS, 29 exact Lua files,
  zero exceptions, 32 exact HUD glyphs. Manifest SHA-256:
  `1d049a9505352d73d6d52b3c6a543301960faba89f93c433921f8c8867ff9d01`.
- Lua 5.2/shared tests: boot_resilience, fences_compat, bridge_companion,
  company_chip, dedicated_mode, params_line, re/asset_lua, test_sync_runtime,
  test_auto_sync_lua and con_recycled_id: PASS.
- hotjoin_stage: original incoming test failed two scheduling assumptions;
  corrected fixture passed six consecutive loopback runs.
- Windows windowcolor_bytes test was not executed: it hardcodes a Windows
  game path and verifies PE sites. Only its shared stylesheet expectation
  changes upstream; no native hook changes are needed. Native fixture tests
  are included in the full build.
- Release shell syntax, packaged record existence, CRLF-aware whitespace,
  upstream Lua equality, empty unmerged index and retained MERGE_HEAD checked.

Host Python lacked lupa. Test dependencies were installed only in
`.git/port-test-venv` (lupa 2.8, pystun3, zstandard). The optional miniupnpc
wheel failed to build; loopback tests do not need UPnP and passed without it.
No system package installed. Logs are `.git/port-24b8f636-*.log` in this clone,
including the original loopback failure and six corrected runs.
