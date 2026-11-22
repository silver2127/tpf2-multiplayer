# Windows dev 06188ea5 integration: command stamps during catch-up

## Merged

Windows target: `06188ea5f3773f305dffe449484bd9330c139c98`.
Linux parent: `c777f67f356d59a54909df60436558aa68a8ce69`.
One Windows commit, no conflicts. Merge retained and staged, not committed.
Incoming changes affect only shared `mp/net.lua` and comments in `mp/inject.lua`.

## Ported (what and how)

Retained upstream scheduling unchanged: pay the fastest peer's full lead up to
600 game-time units, instead of clipping it to MAX_LEAD (15). Above 600, use
MAX_LEAD as the fallback for a clock judged foreign to this world. The ordinary
execution delay and step-boundary rounding still follow the lead calculation.
The player-action gate remains to avoid a long wait for captured player clicks;
mod-scheduled commands benefit from the full lead too.

Linux loads this same Lua module, so no native counterpart is required. No
addresses, patch bytes, offsets, ABI contracts or native hooks change; no new
static or live RE is needed. Reviewed README, Linux INSTALL and integration
records, and the native IO/time RE notes. Existing native evidence and limits
remain unchanged. The native rename/color origin-replay exception in inject.lua
is preserved; net.lua is byte-identical to the incoming Windows commit.

Extended the existing real-module Lua 5.2 delay/hold regression with speed and
construction commands at gaps -5, 0, 5, 15, 33, 63, 600, 600.01 and 202690.
Checks cover future stamps, queue/wire/announcement/resend agreement, the
foreign-clock diagnostic, projected peer time and explicit extra delay.
Updated the action-gate test description to reflect its remaining purpose.
Advanced the Lua verifier reference and pinned native exception digest, package
BUILDINFO provenance, and packaged integration record. Release remains 0.7.0.5.

## Not ported

None for this commit. Earlier native feature gaps, including cargo-filter
capture documented in [0047c19f](UPSTREAM_dev_0047c19f.md), remain outstanding.
This integration does not establish cross-platform gameplay parity.

## Live testing

No game launched, gdb attached, desktop input sent, or lab payload/save changed.
No actor backup/restoration was necessary. No renderer, loaded-world or live
multiplayer observation is claimed. Shared scheduling was exercised offline
with the real net.lua under Lua 5.2 and stubbed clocks/transport.

## Tests

- `tools/linux/build_native.sh`: PASS; pinned soldier build, 70/70 CTests,
  and glibc <=2.31 checks.
- `python3 tools/linux/verify_lua_release.py`: PASS; 29 Lua files with one
  pinned cumulative native exception and 32 exact HUD glyphs. Manifest SHA-256:
  `8dba1685def2dc2231c1d6d06fc620b0e2e9eea6f155178a8e4e051e28d070d1`.
- `.git/port-venv/bin/python tools/delay_hold_test.py`: ALL OK.
- `.git/port-venv/bin/python tools/actions_off_test.py`: ALL PASS.
- `lua5.2 tools/linux/test_slice_line_wire.lua "$PWD"`: PASS, including
  native rename/color origin replay (mock API).
- `bash -n tools/linux/build_release.sh`: PASS. No release package built.
- CRLF-aware staged whitespace, empty unmerged index, retained MERGE_HEAD:
  PASS.

Python tests use clone-local lupa 2.8 and TMPDIR `.git/port-tmp`.
Logs: `.git/port-06188ea5-{build,lua,delay,actions,line,deps}.log`.
