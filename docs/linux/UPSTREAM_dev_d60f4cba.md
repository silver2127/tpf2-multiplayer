# Windows dev d60f4cba integration — 0.7.1.6 release-note baseline

## Merged

Windows target: `d60f4cbaa91d4ac11d9860f6faac340f65594df7`.
Linux parent: `b47d09e8f1f9ac2d3639d28dd44af7f7c1e5f6cb`.
One incoming commit, changing only `installer/RELEASE-0.7.1.6.md`.
No conflicts; the release notes are retained byte-for-byte from upstream.
The merge is staged and left uncommitted.

The notes now describe the update from published 0.7.1.4, carrying forward
CrossOver panel and transfer-memory items from unpublished 0.7.1.5. Players
and servers on 0.7.1.4 must update. Existing 0.7.1.5 installations remain
compatible with 0.7.1.6 through lobby protocol 0.7.1.5.

## Ported (what and how)

No runtime port is required. The CrossOver hook and shared lobby lifetime
changes were already integrated in [dev 192ecd4d](UPSTREAM_dev_192ecd4d.md);
its [native Vulkan evidence](../re/linux/DEV_192ECD4D.md) explains the existing
Linux dispatcher route. The panel I/O and recovery/startup changes are recorded
in [dev 63916f00](UPSTREAM_dev_63916f00.md) and
[dev f4d3df01](UPSTREAM_dev_f4d3df01.md). Compatibility is recorded in
[dev 847f2563](UPSTREAM_dev_847f2563.md).

Reviewed README, Linux installation guidance, prior integration records and
Linux RE documentation. This commit adds no hook, address, byte pattern,
calling convention, field offset or lifetime contract. No new static or live
reverse engineering is needed. Windows code paths and native guards are unchanged.

Updated README and INSTALL with the published upgrade baseline. Advanced the
Lua verifier reference and release BUILDINFO provenance to this Windows target,
and included this record in Linux release packaging. Package/title version
remains 0.7.1.6 and lobby protocol remains 0.7.1.5. Existing compatibility tests
already cover 0.7.1.4 rejection and 0.7.1.5 acceptance; no new test fixture is
needed for the documentation correction.

## Not ported

None for this commit. Previously documented native feature gaps and live
validation limits remain unchanged. Upstream release-note validation claims
are retained as upstream text, not newly observed results.

## Live testing

No game, Proton peer, debugger or desktop input was run. No lab actor files,
saves, user-installed mod or Steam files were changed, so no backup/restoration
was needed. No game process was started. No rendering, GPU, memory reduction,
loaded-world or cross-platform gameplay observation is claimed.

## Tests

- `tools/linux/build_native.sh`: PASS, pinned soldier SDK build, 79/79
  CTests and glibc baseline checks.
- `python3 tools/linux/verify_lua_release.py`: PASS, 31 exact Lua files,
  zero exceptions, 32 HUD glyphs and two toolbar textures. Manifest SHA-256:
  `f7a4d20419e173a3cb2d5aa4ef75155299b3bb6a72ab6eb9f9e1c6cdad9f0007`.
- `bash -n tools/linux/build_release.sh`: PASS.
- Integration record links, unchanged upstream release-note content, staged
  whitespace and merge-state checks: PASS; no unresolved index entries.

Logs: `.git/port-d60f4cba-build.log`, `.git/port-d60f4cba-lua.log` and
`native/linux/out-soldier/Testing/Temporary/LastTest.log` in this clone.
Release archive packaging and Windows/MSVC compilation were not run.
No commit, merge abort, publication or system installation.
