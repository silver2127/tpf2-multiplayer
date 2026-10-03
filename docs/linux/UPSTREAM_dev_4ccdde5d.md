# Windows dev 4ccdde5d integration: cargo list write-back

## Merged

Windows target: `4ccdde5d2ba5ba6a78f1ac862689d8897146f58a`.
Linux parent: `7b6b6827064739989e8b3696f80fd557567dcd5a`.
One incoming commit, no conflicts. Shared `res/scripts/mp/lines.lua`
remains byte-identical to the Windows target. Merge is staged and uncommitted.
Release and lobby handshake remain 0.7.0.5.

## Ported (what and how)

Shared `CM.lineApplyCargo` reads each load/unload/maxLoad list once, fills
that local list, assigns it back to StopConfig, then assigns StopConfig
back to the stop. The Lua mod is used by both platforms, so the incoming
implementation supplies the Linux replay fix without native code changes.
No address, patch, structure offset or calling convention changes.

Extended the existing native line-wire regression with detached copies for
both Stop.stopConfig and its nested list getters. It verifies explicit
write-back, appending to nonempty lists, empty and malformed filters, numeric
flags and fractional limits, plus create/update wire/replay/snapshot round
trips at 2, 30, 33, 65 and 1024 entries. The same regression fails against
pre-merge lines.lua at the first cargo read-back assertion.

Advanced Lua verification and package BUILDINFO provenance to this target,
retained the pinned native origin-replay exception, packaged this integration
record and linked it from README and INSTALL.

## Not ported

None from this commit: its only runtime change is shared Lua.
The earlier native cargo **capture** gap remains unchanged, as documented in
[dev f9d34252](UPSTREAM_dev_f9d34252.md) and its
[static/live investigation](../re/linux/DEV_F9D34252.md).
Linux-origin line edits can still lose filters because capture omits cfg=.
This replay-only integration does not claim to close that gap or prove the
real Linux userdata contract. No new native RE was needed for this change.

## Live testing

No game was launched. The regression models copy-on-read properties and
exercises production Lua; it is not a real engine userdata or multiplayer
observation. No GPU, save, or cargo-filter UI result is claimed. Steam and
lab actor files were untouched; no backup/restore was necessary.

## Tests

- `tools/linux/build_native.sh`: PASS, pinned soldier SDK, 70/70 CTests
  and glibc <=2.31 checks. Log: `.git/port-4ccdde5d-build.log`.
- `lua5.2 tools/linux/test_slice_line_wire.lua "$PWD"`: PASS, copy-on-read
  regression and existing line wire checks. Log: `.git/port-4ccdde5d-line-wire.log`.
- Negative control with HEAD's old lines.lua: fails at first filter read-back,
  as expected. Log: `.git/port-4ccdde5d-negative.log`.
- `python3 tools/linux/verify_lua_release.py`: PASS, 29 Lua files, one pinned
  cumulative native exception and 32 exact HUD textures.
  Log: `.git/port-4ccdde5d-lua.log`. Manifest SHA-256:
  `c64ed649c9387d843e4206af53f7ad5fcca1b96e37985165f22c88499a19c46c`.
- Release shell syntax, CRLF-aware staged whitespace, upstream lines.lua
  equality, empty unmerged index and retained MERGE_HEAD checks pass.

Final status: DONE for this incoming commit. No commit, merge abort or publication.
