Large station upgrades stay in lockstep; a resync no longer hangs at "holding"

- Slice: upgrading a modular station with about a dozen modules (~9 KB of parameters) ran natively on the host only. The slice's parameter capture was capped at 8 KB, so the upgrade was not cancelled and replayed at the stamp; the host paid for every click, the joiner later rebuilt the station from one coalesced edit, and the two worlds differed in track edges, heights and money (desync on the rig, 2026-09-16). The capture holds 64 KB now, so those upgrades take the strict path on every instance.
- Resync: a session running at engine speed 3 (where a speed vote can land) could not resync -- every player's "holding" acknowledgement carried speed=3, the barrier accepted only 0/1/2/4 and rejected them silently, so the resync never left its first step. Speeds 0-4 are accepted, in the lobby and in the game's release.
- Everything in 0.5.4 and 0.5.5 (restarted joiner rejoins; lobby follows a completed resync; hot join gets the fresh autosave; first-time players can load a shared save).

Every player must update (the lobby version gate is exact) and the dedicated relay must run this version.

Validation: offline, the sync barrier tests (every engine speed), the Lua recovery test, luacheck, and the 0.5.4 transport/lobby suite; on the two-instance rig the previous build reproduced the station desync and the stuck resync, this build is deployed there for the retest.
