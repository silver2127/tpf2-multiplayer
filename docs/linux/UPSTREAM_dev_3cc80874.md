# Windows dev 3cc80874 integration: merge back of completed Linux ports

## Merged

Target `3cc80874b6b126d2d615220d067e5c8c7c836e83`, Linux parent
`ec60124c0502144a0ab3f66432e6b33cf6befec8`. One incoming merge commit,
"Merge the Linux ports of dev up to cc0981b (port/dev) into main for 0.7.0.7".
No conflicts. The merge remains in progress and uncommitted.

`git log -p HEAD..3cc80874` contains only that merge commit. Both HEAD and
incoming commit have tree `41ae60ad6aff807edbee67b70795ea099b2f77e0`;
`git diff HEAD 3cc80874` and `git diff 3cc80874^2 3cc80874` are empty.
The 59-file first-parent diff imports work already present on Linux, including
native FPT6, Fantasia generator memory handling, resync hold fixes, tests and
integration records. It introduces no additional source change to this port.
See [the preceding integration](UPSTREAM_dev_cc0981bb.md).

## Ported (what and how)

No runtime adaptation is needed. Preserve the identical Windows and Linux
implementations and release 0.7.0.7. Advance the Lua verifier and package
BUILDINFO provenance to this merge, include this record in native packages,
and link it from README and Linux INSTALL.

The upstream merge now contains Linux's origin-replay change in `inject.lua`.
Remove its checksum exception: all Lua files must match the new upstream
reference byte for byte. The shipped Lua manifest is unchanged.
No new game address, pattern, offset, ABI or lifetime assumption is introduced,
so additional ELF reverse engineering or live probing is unnecessary.

## Not ported

None introduced by this commit. Existing native feature gaps and validation
limits remain, including cargo-filter capture/replay and terrain-sidecar
capture/serving. Their earlier static and live attempts are documented in
[DEV_AD36A976](../re/linux/DEV_AD36A976.md),
[DEV_F9D34252](../re/linux/DEV_F9D34252.md) and
[DEV_363C38CC](../re/linux/DEV_363C38CC.md). This history merge does not close
them or claim Windows gameplay results as native validation.

## Live testing

No game launched, debugger attached or desktop input sent. No lab actor,
save, Steam installation or user mod modified; no backup/restoration needed.
No rendered, loaded-world or cross-platform gameplay observation is claimed.
Live testing was unnecessary for this identical runtime tree.

## Tests

- `tools/linux/build_native.sh`: PASS, pinned soldier SDK, 72/72 CTests
  (52.91 seconds) and glibc <=2.31 checks.
- `python3 tools/linux/verify_lua_release.py`: PASS, 29 exact Lua files,
  zero exceptions, 32 exact HUD glyphs. Unchanged manifest SHA-256:
  `e49a423ed668fe64c3e85a21429e6d8b767299305c663db731b0caa84e3f4dd8`.
- `bash -n tools/linux/build_release.sh`: PASS. Package record existence and
  provenance checked; no release package or Windows binary built.
- Identical incoming/HEAD tree and empty unresolved index verified.
  `git diff --check` and staged whitespace checks: PASS.

Existing tests were run without adding runtime regressions because no runtime
code changed. No dependencies installed, publishing performed or Steam state
changed.
Logs are clone-local `.git/port-3cc80874-{build,lua}.log`.
