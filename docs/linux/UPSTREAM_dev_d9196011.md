# Windows dev d9196011 integration: retain and verify the published tag

## Merged

Target `d9196011bdc69cdb4cdc46c23e7f8899edab0075`, Linux parent
`66d5d181cf22e0ebe71ae76767e342613ba92188`. One incoming Windows commit:
"publish_release: the publish carries tag_name, and a release that comes out
untagged fails loudly". No conflicts. The merge remains staged and uncommitted.

`tools/publish_release.py` is retained byte-for-byte from upstream. Publishing
includes the intended `tag_name` in the PATCH alongside draft/Latest settings.
For a real publication it then reads the release by ID and fails with the
reported tag and repair PATCH guidance if GitHub returns another tag. Dry runs
skip the read-back. Upstream's updated test expectations are preserved.

## Ported (what and how)

The publisher is shared Python tooling used for Windows and Linux releases;
no separate native implementation is needed. Add three offline regressions:
matching-tag read-back for both Latest settings, rejection of an untagged or
wrong-version result with repair details, and no service access in dry-run mode.
The tests exercise the real GitHub write wrapper with API calls mocked and
HTTP access blocked. Existing tests cover the native payload and release layout.

Advance Lua verification and package BUILDINFO provenance to this target,
include this record in native packages, and link it from README and INSTALL.
Version remains 0.7.0.7 and the shipped Lua manifest is unchanged.
No game hook, ELF address, byte pattern, offset, ABI or lifetime contract
changes, so static disassembly and live reverse engineering are unnecessary.

## Not ported

None from this commit. Earlier native feature gaps and gameplay validation
limits remain unchanged; this tooling-only integration does not close them.

## Live testing

No game launched, debugger attached or desktop input sent. No Steam state,
lab actor, installed mod or save changed; no backup/restoration was needed.
No live GitHub publication or game observation is claimed. All publisher
checks were offline, without credentials or network requests.

## Tests

- `tools/linux/build_native.sh`: PASS, pinned soldier SDK, 72/72 CTests
  (52.82 seconds) and glibc <=2.31 checks. Compiler warnings in unchanged
  native sources are retained in the build log.
- `python3 tools/linux/test_launcher_release.py`: PASS, 39 tests.
- `python3 tools/linux/verify_lua_release.py`: PASS, 29 exact Lua files,
  zero exceptions, 32 exact HUD glyphs. Manifest SHA-256:
  `e49a423ed668fe64c3e85a21429e6d8b767299305c663db731b0caa84e3f4dd8`.
- `bash -n tools/linux/build_release.sh`: PASS; all 76 packaged integration
  records exist. Unstaged/staged whitespace checks and empty unmerged index
  checked; shared publisher equals upstream exactly.

Logs: `.git/port-d9196011-{build,lua,publisher}.log` in this clone.
No publication, commit, merge abort or system installation performed.
