# Windows dev e516c9bc integration — low-speed command delay

## Merged

Windows target: `e516c9bc61a89d3df31eddb339f2857644d6b46d`.
Linux parent: `4c4984fe103719d5efac85215521204c4a408afc`.
One incoming commit: pacing: under 1x the command delay follows the session
speed; a rise from under 1x is ramped. No conflicts. Merge staged and left
uncommitted; release remains 0.7.1.2.

## Ported (what and how)

Retained upstream `net.lua` and `pacing.lua` byte for byte. A running session
below 1x uses a speed-dependent delay floor with the 1.6 ramp margin, while
retaining the measured-rate override, RTT sizing, min/max bounds and repeat
allowance. Paused and >=1x rate floors remain unchanged. Delay reductions
now cover half the gap (at least one simulation step), on the existing grid
and after the existing hysteresis; this reduction rule applies at all speeds.

The leader limits large increases from below 1x using the upstream 1.25
factor, ten-tick interval and 0.05 speed grid/minimum increment. Decreases,
pausing, and increases starting at >=1x retain their existing behavior.
The shared controller writes the existing `tpf2_speed.txt` interface used by
the native speed hook; regression coverage checks that output during a ramp.

Reviewed README, Linux installation/Lua guidance, the previous `3d6881e6`
integration, and the time-slice RE contract in `docs/re/linux/SLICE_TIME.md`.
There are no changed Windows hooks, executable patches, platform APIs,
addresses, ABI contracts or struct offsets in this commit. No native C++
counterpart or new static/live reverse engineering is needed.

Extended existing delay and speed-vote tests for low-speed delay, paused and
1x floors, measured-rate precedence, delay hysteresis/convergence, ramp
cadence, the native speed file, immediate slowdown/pause and >=1x recovery.
Advanced the Lua verifier and release BUILDINFO to this target, included this
record in packaging, and updated README/INSTALL. Windows paths remain intact.

## Not ported

None from this commit. Inherited native feature gaps remain as documented.
The existing slow-joiner simulation failure below is not a missing port.

## Live testing

No game launched, debugger attached, desktop input sent or actor modified.
This change is entirely shared Lua with offline coverage; no new engine
contract requires a live probe. No live gameplay, rendering, command delivery
or cross-platform observation is claimed. Steam, lab payloads and saves were
untouched; no backup or restoration was necessary and no game was started.

## Tests

- `tools/linux/build_native.sh`: PASS, pinned soldier SDK, 79/79 CTests,
  62.88 seconds; library glibc <=2.31 baseline passed.
- `python3 tools/linux/verify_lua_release.py`: PASS, 31 exact upstream Lua
  files, zero exceptions, 32 HUD glyph textures and two toolbar textures.
  Lua manifest SHA-256:
  `bf8724ef7c9ee0a77eaec8a1acc0e0cf82f624b991669f06eee234b47616bfeb`.
- `tools/delay_hold_test.py`, `tools/speed_vote_test.py`,
  `tools/delay_noise_sim.py`, `tools/live_catchup_gap_test.py`,
  `tools/dedicated_mode_test.py`, `tools/linux/test_fpt6_pacing.py`: PASS.
- Negative controls: both extended regression suites exit 1 when their Lua
  input is replaced in memory by pre-merge HEAD; each detects four changed
  behaviors. Working Lua is never replaced on disk.
- `tools/pacing_sim.py`: one failure, `b stays within 4 of the leader from
  tick 1800` in `slow_joiner`. Reproduced with both simulator source inputs
  forced to pre-merge HEAD and `--only slow_joiner`. Other full-run checks
  pass. This matches the upstream commit and prior Linux integration record.
- `bash -n tools/linux/build_release.sh`: PASS. Merge-state and staged
  whitespace checks use `core.whitespace=cr-at-eol` for upstream CRLF Lua.

Host Python lacked lupa. Extracted the PyPI lupa 2.8 CPython 3.14 manylinux
wheel under `.git/port-python`, verifying its published SHA-256
`a591b9947ca347b41a63370e121d6e2b1458fe6dde9ae065029ec10a37f25ff4`.
Tests used `PYTHONPATH=$PWD/.git/port-python` and
`TMPDIR=$PWD/.git/port-tmp`; nothing installed system-wide.

Evidence: `.git/port-e516c9bc-*.log` and
`native/linux/out-soldier/Testing/Temporary/LastTest.log`.
No commit, merge abort, publication or release packaging run.
