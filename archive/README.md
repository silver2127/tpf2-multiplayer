# archive

Kept for reference, not maintained. Everything here predates the lockstep mod
(`mod/mp_lockstep_1`) that ships today and is left as it was on the day it was
retired, so relative paths inside these scripts point at where things used to be.

- `mod/mp_bridge_1` -- the first multiplayer mod: STATE replication (poll the world,
  ship what changed). Superseded by lockstep command replication; the two must never
  run together. Its test script `mptest.lua` and the `tools/scenarios/*.txt` it
  executes were the M4-M5 replication harness (`tools/run_mptest.ps1`,
  `tools/autotest.ps1`).
- `mod/m3_determinism_1` -- the M3 probe that measured the simulation deterministic
  (`tools/m3_compare.ps1` reads its hashes). Result in `docs/history/M3_RESULTS.md`.
- `tools/inject_bridges.ps1`, `tools/mp_launch.ps1` -- the injector-era launch path.
  The proxy `alut.dll` loads every DLL at start now; there is no injector.
- `tools/lockstep_test.ps1`, `tools/slice_two_way.ps1` -- the M9 lockstep and first
  slice tests, before the in-game Multiplayer menu (`tools/mp_menu_launch.ps1`) and
  `tools/soak.ps1` replaced them.

The retired build variants (`bridge/build_a4.bat` .. `build_v7.bat`, the probe DLLs
`build_args/cmdlist/defer/payload/probe_apply.bat` and their sources, `injector/`)
were deleted outright on 2026-09-08; `git show v0.4.0:bridge/build_v7.bat` and
friends still have them.
