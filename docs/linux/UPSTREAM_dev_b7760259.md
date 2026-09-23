# Upstream dev b7760259 integration

Windows target: `b77602599fbec3e3690de1430e28241c1a803f83`, experimental
0.6.1.25, following [2b466e32](UPSTREAM_dev_2b466e32.md).
The 19-commit merge is resolved and staged, **not committed**. This integration
is **partial**: new canonical simulation ordering remains default-off pending
local live verification. Do not infer Windows/native simulation parity from
the matching lobby version or passing build.

## Merged

- 534a0d7, 9b1621f, 1cd2cdf, 01ab6c1, 8bf5d9e: experimental release metadata
  through 0.6.1.25; db5d8b4/b776025: launcher-readable notes.
- c4d3ee5, 2fa86db, 6274846, a183afe, 5a343ad: Steam endpoint buffers, revised
  chunk selection/retries, separate replay windows, corrected rate IDs,
  bidirectional TCP address offers, throughput diagnostics and backpressure.
  Final upstream behavior enables bounded 32 KB Steam-only chunks; mixed
  Steam/internet transfers use 1,100-byte chunks and reachable TCP is preferred.
- aacbd68/9094bd2: title-dialog redesign and shared Lua dashboard collapse,
  expansion, styles and chat-focus fixes.
- cea5729/cc0dba6: canonical person batches and capacity maps.
- bafa417: opt-in retained-world joins, fingerprint fallback to a fresh loading
  epoch, and frozen retries. b61685c/0c8d69a preserve the upstream merge history.

Resolved steam_tunnel.cpp from LF-normalized index stages, combining the Linux
socket setup with the incoming buffer setup. Resolved lobby.py by preserving
Linux observation cleanup (UPnP removal and tunnel close) and assigning
MY_TCP_ADDRS inside that protected try block. Windows paths remain intact.

## Ported

- Native title rendering and SDL input, including viewport scale limits,
  runtime title artwork/solid fallback, tab focus, covered-menu input capture,
  paging, mod modal, chat/save/roster layout and readiness-disabled start.
  The Linux lobby View now exposes its existing lobbyReady state.
- Shared Steam native tunnel improvements retain Linux ABI/socket handling.
  Shared Python TCP, replay-window, backpressure, diagnostics and live-join
  protocol changes need no platform-specific address substitutions.
- Shared dashboard Lua and styles merge unchanged. The verifier and release
  BUILDINFO now target 0.6.1.25. All 29 Lua files match that target except the
  existing pinned native origin-replay integration in inject.lua; all 32 HUD
  glyphs match. Manifest:
  `b7410c4b3eb76fbe3dd95ded62fc6bcb8852f6b05611f6afcea04141816c2f1c`.
- Experimental native canonical module and person-map sidecars, with build-id,
  byte checks, rollback, all-register ABI fixtures and a standalone ELF verifier.
  Source exists and tests pass, but activation is explicitly gated below.

## Not ported

Default-on canonical ordering / validated native retained-world joining:
`TPF2MP_ORDER_CANON=1` is a developer test switch, default off. Static checks
found and verified all five Linux sites, but both lab launch paths failed
before a world could be loaded. In particular, relinking the nine maps leaves
hash buckets stale; nested-consumer and lifetime evidence is still needed
before enabling it by default. The retained-world lobby flag remains opt-in as
upstream; the flag alone does not establish that peers have matching sorts.
This also limits frozen Windows/native gameplay: Windows sorts are default-on.

The static evidence, exact live failures and required next probes are in
[DEV_B7760259](../re/linux/DEV_B7760259.md). Earlier unrelated native limitations
remain as recorded in previous integrations.

## Live testing

Backed up native libraries/mod/userdata, installed this soldier build and Lua,
requested a 1280x720 window, then tried the normal lab launcher. It failed with
uid-map permission denial. The documented outer-bwrap fallback requested RADV,
then exited 53 because Steam was unavailable; the process scan found no Steam.
No game window, GPU selection, loaded save or gdb probe was reached. No Steam
or user installation was changed. Actor trees were restored and no game remains.
Real Steam speed and in-game visual/cross-peer behavior are not claimed.

## Tests

- `tools/linux/build_native.sh`: soldier build, 55 native CTests and glibc
  baseline check. Added title-dialog and five-site trampoline tests; expanded
  person-map canonical sidecar and native Steam config-ID tests.
- `python3 tools/linux/verify_lua_release.py`: 29 Lua files and 32 glyphs pass.
- `verify_order_canon_elf.py`: build-id, five complete guards, instruction
  boundaries and branch checks against the actual lab ELF pass.
- Sync operation/runtime: 31/17 tests pass, including retained-world fallback.
- Steam backpressure simulation: bounded queues at 256 KiB/s, 1 MiB/s and
  16 MiB/s, loss, ACK gaps, rewind and stalled-peer timeout pass with exact hashes.
- Steam TCP: both dialing directions and no-address fallback pass, 25 MB exact
  files. Fake-tunnel 48 MiB save transfer and delayed-bulk queue tests pass.
- Steam tunnel, seal selftest, automatic sync lobby (simulated engine), 16 Linux
  lobby shutdown checks, six dashboard and two lobby-panel Lua checks pass.
- The upstream MSVC title test cannot run here (`cmd` is unavailable); the native
  title fixture covers the Linux renderer/input implementation instead.

Python dependencies are confined to `.git/test-venv`. Native fixtures and fake
Steam tests are not live gameplay evidence. Full logs accompany the job report
under `meta/live/`; merge and source changes are staged without a commit.
