# Upstream dev ef275a3c integration: refresh dashboard identity

Windows target: `ef275a3c392451f78470ecd3c1ed08db58fc7a9b`.
Linux merge parent: `39bc4b0e76740aa425ee42378ec7e0465f25baa5`, following
[b4465474](UPSTREAM_dev_b4465474.md). One Windows commit, no conflicts.
The merge remains staged and uncommitted.

## Merged

Keep `mod/mp_lockstep_1/res/config/game_script/lockstep.lua` byte-exact
upstream, including its LF normalization. The GUI state now calls
`CM.detectInstance` on the first and every fourth dashboard refresh, or on
every refresh while identity is unknown. A bridge rewrite of last session's
letter can therefore update the GUI's status source and per-instance file
paths after startup. Unchanged identity returns early in `mp/io.lua`.

The advertised approximately two seconds is frame-dependent: the ordinary
GUI refresh runs every 30 GUI updates, so polling takes 120 updates; dedicated
mode refreshes every 300 updates and polls every 1,200. This integration
preserves upstream's cadence.

## Ported (what and how)

The shared Lua is also the native Linux implementation. The native bridge's
`WriteIdentity` in `native/linux/src/bridge_linux.cpp` already writes the
same `tpf2_instance.txt` contract (letter followed by `pid=...`). The existing
Linux boot environment provides the shared script's runtime directory.
No native counterpart needs editing, and no executable patch, new address,
byte pattern, struct offset, calling convention or lifetime is introduced.
No new binary reverse engineering is required.

Advance the Lua verifier and release BUILDINFO to this commit, package this
record, and update README/install links. Preserve the pinned Linux
origin-replay change in `mp/inject.lua` and all Windows code paths.

Add `tools/linux/test_gui_identity.lua`, exercising the actual dashboard
read/identity block and `mp/io.lua` with virtual runtime files. It covers
missing identity retries, late b-to-a and a-to-b changes, matching status
selection and inject/capture/events paths, unchanged polling without resetting
consumption offsets or clearing peer status, and absent/empty/throwing reads.
Restoring the old one-time check in memory makes the regression fail at the
late b-to-a status assertion.

## Not ported

None in this commit. Inherited limitations remain as recorded in
[the previous integration](UPSTREAM_dev_b4465474.md) and
[the native parity record](../re/linux/PARITY_20260921.md), including default-off
canonical ordering and outstanding live visual/external Steam P2P checks.
This small shared-script fix does not establish Windows/native gameplay parity.

## Live testing

No game launched, gdb attached, actor installed or save changed. The change
uses an existing shared file contract and is tested offline; no native
contract needs a live probe. No on-screen dashboard, real-time polling latency,
or multiplayer result is claimed. Lab actors and Steam were untouched.

## Tests

- `tools/linux/build_native.sh`: soldier build, 58/58 CTests and glibc <=2.31
  compatibility checks passed.
- `python3 tools/linux/verify_lua_release.py`: 29 Lua files (one pinned Linux
  origin-replay integration) and 32 exact HUD glyphs passed. Lua manifest:
  `fb3b58aee11225608e15d1f7d151e4562a5ac03c531f289e3eaa290dbbae7755`.
- `lua5.2 tools/linux/test_gui_identity.lua`: regression passed, including
  compilation of the complete shipping entry point. Pre-fix negative control
  failed as expected.
- `bash -n tools/linux/build_release.sh`: passed.
- Upstream entry-point byte equality, whitespace checks and empty unmerged
  index passed.
- Logs: `.git/port-ef275a3-build.log`, `.git/port-ef275a3-lua.log`,
  `.git/port-ef275a3-identity.log`.
