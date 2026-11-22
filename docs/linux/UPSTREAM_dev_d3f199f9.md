# Windows dev d3f199f9 integration: 0.7.1 release notes

## Merged

Windows target: `d3f199f9f414b69a5098b11dc3e0ac1028f752c2`.
Linux parent: `0372714b938b163212b487f438404162102a90a5`.
One incoming commit, no conflicts. Merge staged and left uncommitted.
Release remains **0.7.1**.

Retained `installer/RELEASE-0.7.1.md` byte-for-byte from upstream. It now
includes Windows/Proton worker-thread save compression, the New Game preview
fix and their validation, and removes the obsolete Linux-only save limitation.
These are documentation changes; this commit introduces no implementation.

## Ported (what and how)

No runtime adaptation is needed. The preceding
[568e7ea7 integration](UPSTREAM_dev_568e7ea7.md) already retained Windows
compression and ported generator timestamps to native Linux. Linux keeps its
existing system-libzstd stream routing, byte-verified installer and timestamp
fallback. The existing [RE evidence](../re/linux/DEV_568E7EA7.md) applies;
no address, pattern, ABI, offset or lifetime contract changes in this merge.
No new static or live RE was necessary.

Advanced the exact Lua verification baseline, package BUILDINFO provenance,
and current README/INSTALL integration links to this commit. The package
builder includes this record. Shared Lua and all Windows code stay unchanged.
No new tests were needed for release prose; existing verification covers the
updated baseline and build. Upstream measurements remain upstream evidence.

## Not ported

None introduced by this commit. Previously documented native Sandbox town-tool
capture, minimap, cargo-filter and other gaps remain unchanged. This record
does not close the preceding integration's live preview/save/load validation
limits or establish cross-platform gameplay parity.

## Live testing

No game launched, gdb attached or desktop input sent for this documentation-only
change. No title menu, Vulkan device, preview, save/load or performance result
was observed. Lab payloads, saves, Steam and the user's installation were not
modified; backup/restoration was unnecessary. No game process was started.

## Tests

- `tools/linux/build_native.sh`: PASS, pinned soldier SDK build, 75/75 CTests
  and glibc <=2.31 compatibility checks.
- `python3 tools/linux/verify_lua_release.py`: PASS, 30 exact upstream Lua
  files, no exceptions, 32 HUD textures and two toolbar textures.
  Manifest: `4f1370ac2781c929d3526bdef7b39d17ddf945af5f25160a8707c7e7528f6e3a`.
- `bash -n tools/linux/build_release.sh`: PASS.
- Incoming commit scope and byte-for-byte upstream release-notes equality: PASS.
- Staged whitespace check: PASS.
Logs: `.git/port-d3f199f9/build.log` and `.git/port-d3f199f9/lua.log`.
