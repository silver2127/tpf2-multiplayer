# Windows dev ac3b4be3 integration: merge back of completed Linux ports

## Merged

Windows target: `ac3b4be3c9790ff034376bf9cfd86fce103a6fc8`.
Linux parent: `016a6115089578a09ff36fe6a8b031a8f67c8063`.
One incoming merge commit, no conflicts; merge left in progress and uncommitted.

`git log -p HEAD..ac3b4be3` contains only the merge commit. HEAD is its
second parent. Both commits have tree
`ae5eee5c232bdddeb29985a09c42f02324b2a267`; their diff is empty.
The 17-file first-parent diff brings Windows the already completed Linux
integrations: dedicated Workshop registration, recent peer clocks, spare-line
catch-up gating, release 0.7.1.1, tests and integration documentation.
Those changes are already present here, as recorded in
[893be145](UPSTREAM_dev_893be145.md),
[d4a11297](UPSTREAM_dev_d4a11297.md) and
[412aeb8e](UPSTREAM_dev_412aeb8e.md).

## Ported (what and how)

No runtime adaptation is required. Keep all native, Windows and shared runtime
code intact and retain release 0.7.1.1. Following the earlier
[merge-back integration](UPSTREAM_dev_3cc80874.md), advance the exact Lua
verifier and package BUILDINFO provenance to this merge, package this record,
and link it from README and Linux INSTALL. Lua still matches upstream without
checksum exceptions; its manifest is unchanged.

No hook, address, byte pattern, struct offset, ABI or lifetime contract changed.
The existing Linux RE documentation remains applicable; no new static or live
reverse engineering is needed for this identical runtime tree.

## Not ported

None introduced by this commit. Inherited native Sandbox town-tool capture,
minimap, cargo-filter, terrain-sidecar and other documented feature and
live-validation limitations remain unchanged. This merge does not close them
or establish cross-platform gameplay parity.

## Live testing

No game launched, debugger attached or desktop input sent. No lab actor,
save, Steam file or installed mod changed; no backup/restoration was needed
and no game process was started. No GPU, rendered UI, loaded-world or
native/Proton multiplayer result is claimed. Live testing was unnecessary
because the incoming runtime tree is identical to the existing Linux tree.

## Tests

- `tools/linux/build_native.sh`: PASS, pinned soldier SDK, 75/75 CTests
  (52.94 seconds), including glibc <=2.31 compatibility checks.
- `python3 tools/linux/verify_lua_release.py`: PASS, 30 exact Lua files,
  zero exceptions, 32 HUD textures and two toolbar textures. Unchanged manifest:
  `e4f0ff50af42446e7812c2390165c6fd7492489d0d58457737d55e2229c3b851`.
- `bash -n tools/linux/build_release.sh`: PASS. Package provenance and all
  referenced integration records checked; the release package was not built.
- Identical incoming/HEAD trees, active merge target and empty unresolved
  index verified. Unstaged and staged whitespace checks: PASS.

Existing tests were retained without adding runtime regressions because no
runtime code changed.
Logs: clone-local `.git/port-ac3b4be3/`. No dependency or system package was
installed; no publication or release package installation was performed.
