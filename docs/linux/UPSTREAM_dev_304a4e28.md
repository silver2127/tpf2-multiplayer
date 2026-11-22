# Windows dev 304a4e28 integration — catch-up progress diagnostics

## Merged

Windows target: `304a4e287ca7d34c418658bd31a85772eb779291`.
Linux parent: `5456c5597e3f073829b960de7710a1f83bcf8bc6`.
One incoming commit: `pacing: a catch-up says how fast it closes, every ~20 s`.
No conflicts; merge staged and left uncommitted. Release remains 0.7.1.2.

## Ported (what and how)

Retained upstream `mod/mp_lockstep_1/res/scripts/mp/pacing.lua` byte-for-byte.
Native Linux and Windows use this shared Lua module. During running catch-up,
it samples wall time, simulation time and the gap to the leader. About every
20 seconds it logs the remaining gap, closing rate, local simulation rate,
requested speed and inferred session rate. A closing rate above 0.05 units/s
also produces an estimated number of seconds remaining. Entry and ordinary
pacing handover reset the sample; history-fetch waiting is excluded.

Reviewed README, Linux INSTALL, preceding integration records and native
RE conventions in `docs/re/linux/SLICE_CORE.md` and `SLICE_TIME.md`. This is
logging only: no native hooks, ELF addresses, patch bytes, ABI, struct offsets
or lifetime contracts change. No new static or live RE is required.

Extended `tools/linux/test_fpt6_pacing.py` using the actual pacing factory
and a controlled Lua 5.2 wall clock. It checks the first sample, the 19/20
second boundary, repeated ticks, actual elapsed time for delayed ticks,
local/session/closing rates, ETA, stationary and widening gaps, handover
reset, re-entry reset, and exclusion of time waiting for command history.
Existing history-retry, PID and governor tests still pass.

Advanced the exact Lua verifier baseline and package BUILDINFO to this
commit; packaging includes this record. Added README and INSTALL links.
Windows code paths remain intact; no release package was built.

## Not ported

None from this commit. Existing native feature gaps and outstanding gameplay
validation remain unchanged. Log arithmetic tests do not establish actual
catch-up performance or cross-platform gameplay parity.

## Live testing

No game launched, debugger attached, desktop input sent or actor installed.
No in-game, rendering, GPU or cross-platform observation is claimed. Steam,
lab actors, installed mods and saves were untouched; no backup or restoration
was necessary. The rate examples are deterministic test fixtures, not measured
game performance.

## Tests

- `tools/linux/build_native.sh`: PASS, pinned soldier SDK, 79/79 CTests
  (62.93 seconds) and glibc <=2.31 compatibility checks.
- `python3 tools/linux/verify_lua_release.py`: PASS, 31 exact upstream Lua
  files, zero exceptions, 32 HUD glyphs and two toolbar textures. Manifest:
  `98911c795d3026193211339e43288137d2844db471e14249791db6fbcb55ba4b`.
- `.git/port-venv/bin/python tools/linux/test_fpt6_pacing.py`: PASS.
  System Python lacked `lupa`; installed it only into the clone-local venv,
  with pip caching disabled and temporary files under `.git/port-tmp`.
- `bash -n tools/linux/build_release.sh`: PASS.
- Staged whitespace check with `core.whitespace=cr-at-eol`: PASS. The default
  check flags upstream CRLF endings; retained them for exact Lua equality.
  Upstream Lua equality, empty unmerged index and retained MERGE_HEAD: PASS.

Evidence: `.git/port-304a4e28-{build,lua,pacing,deps}.log` and
`native/linux/out-soldier/Testing/Temporary/LastTest.log`. No system packages,
publication, commit or merge abort.
