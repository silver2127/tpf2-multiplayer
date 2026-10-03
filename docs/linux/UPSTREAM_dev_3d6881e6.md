# Windows dev 3d6881e6 integration — dedicated join pacing

## Merged

Windows target: `3d6881e6655dea189c492efb9df229bd13499252`.
Linux parent: `4c323c9f0ac1b62d2bf61bf5012f8acddd55cb0d`.
One incoming commit: dedicated server paused while a player joins an empty
server, 1x at most while one joins others. No conflicts; merge staged and
left uncommitted. Release remains 0.7.1.2.

## Ported (what and how)

Retained upstream `mod/mp_lockstep_1/res/scripts/mp/pacing.lua` byte for byte.
Recent peers without `cu` count as playing; catching-up peers and roster
members not yet heard count as joining. With no player playing, a joiner
pauses the server; with existing players, joining caps the voted speed at
1x and broadcasts LSEFF when the cap changes. The hold expires after 6000
ticks (about 20 minutes), restarting when the joining count changes.
Existing resync/autosave holds retain priority.

This is shared Lua. Linux `native/linux/src/dedicated_linux.cpp::Configure`
already writes `mp_dedicated.txt` with `dedicated`, `empty_speed`,
`pause_empty` and `pin_batch`; the shared `CM.dedicatedPauseEmpty` reader
feeds the changed tick. Existing speed commands and native pacing remain
unchanged. Reviewed the Linux installation, prior integration and
`docs/re/linux/SLICE_TIME.md` contracts. No new game address, patch bytes,
ABI, struct offset or lifetime assumption is introduced, so no new static
or live reverse engineering was necessary.

Retained the upstream dedicated regression cases and added sub-1x vote
preservation and full-speed restoration coverage. Moved the test's failure
exit and success summary after the newly appended vote tests: upstream's
placement could report success even if these final checks failed. Verified
this with an in-memory deliberately failing final assertion.

Advanced the Lua verifier and release BUILDINFO to this commit, packaged
this integration record, and updated README/INSTALL guidance. Windows
runtime paths are intact; no native C++ changes were needed.

## Not ported

None from this commit. Inherited native feature gaps and live gameplay
validation limits remain unchanged. The pre-existing pacing simulation
failure below is not a missing platform port.

## Live testing

No game launched, debugger attached, desktop input sent, or lab payload
installed. Tests execute real Lua with stub game APIs; they do not establish
live join or cross-platform behavior. Steam, lab actors, saves and the user
installation were untouched, so no backup/restoration was necessary.

## Tests

- `tools/linux/build_native.sh`: PASS, pinned soldier SDK, 79/79 CTests
  in 62.81 seconds, including `dedicated_linux` and `speed_pacing`;
  glibc <=2.31 checks pass.
- `python3 tools/linux/verify_lua_release.py`: PASS, 31 exact Lua files,
  zero exceptions, 32 HUD glyphs and two toolbar textures. Manifest SHA-256:
  `b4ac03a0efb6a44ba6f7a0fa3fd3c9013259d6253c3da67bed539569c4aaaa02`.
- `tools/dedicated_mode_test.py`: PASS, 105 checks on Lua 5.2.
  Negative control: deliberately failing the final vote assertion exits
  nonzero, as required.
- `tools/linux/test_fpt6_pacing.py`: PASS (history retries, PID reset,
  recovery and lag smoothing).
- `tools/pacing_sim.py`: one existing failure, `b stays within 4 of the
  leader from tick 1800` in `slow_joiner`. HEAD and working metrics match;
  rerunning with both Lua inputs loaded from the pre-merge HEAD reproduces
  the same failure. This also matches the incoming commit's warning.
- `bash -n tools/linux/build_release.sh` and merge-state checks: PASS.
  Git whitespace checks pass with `core.whitespace=cr-at-eol`; the plain
  check flags upstream CRLF endings, retained for byte-exact Lua parity. No Windows build, release packaging or publication run.

Host Python lacked lupa and pip. Downloaded the PyPI lupa 2.8 CPython 3.14
manylinux wheel, checked its published SHA-256, and extracted it only under
`.git/port-python`. Shared tests used `PYTHONPATH=$PWD/.git/port-python` and
`TMPDIR=$PWD/.git/port-tmp`; nothing was installed system-wide.

Logs: `.git/port-3d6881e6-{build,lua,dedicated,negative,fpt6,pacing,pacing-parent,deps}.log`;
native CTest detail: `native/linux/out-soldier/Testing/Temporary/LastTest.log`.
No commit, merge abort, push or publication was made.
