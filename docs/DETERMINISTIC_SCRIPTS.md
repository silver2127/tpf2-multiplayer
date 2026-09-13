# Experimental deterministic script compatibility

The multiplayer mod registers a `loadGameScript` resource modifier and wraps only
`natural_town_growth.lua`. Workshop files are unchanged. No DLL rebuild is needed.
The modifier entry point is listed in the game's
[resource modifier documentation](https://www.transportfever2.com/wiki/doku.php?id=modding:modifiersfilters).

Activation happens when the game loads resources with both mods enabled. All peers
must have this version of the multiplayer Lua files and matching Workshop code.
The current running worlds do not receive a hot patch. Look for `[mpdet]` in stdout
after loading. A live two-instance validation of modifier activation is still needed.

During the wrapped script's init/load/update/save/handleEvent callbacks, its Lua
random calls use a separate Park-Miller stream seeded by script identity. Explicit
reseeding mixes the requested seed with that identity. Clock-based seeds therefore
agree when callbacks occur at the same simulation time. RNG state is saved under
`__tpf2mp_deterministic_v1` alongside the original script state, and removed from the
table passed to the original load callback. Existing growth factors are retained.

The virtual clock is 2000-01-01 UTC plus the simulation clock in engine units,
quantized to the measured 0.2-unit step. This intentionally defines virtual seconds;
it is not an assertion that engine units equal real seconds or calendar game days.
The 10/180-second timers now advance with the simulation and stop while paused.
`os.date` uses UTC, and calendar-table `os.time` conversions are timezone independent.
Natural Town Growth's town IDs and cargo keys are sorted before iteration.
Repeated update callbacks at the same simulation time are suppressed.

Legacy Natural Town Growth saves have wall-clock timestamps. On their first wrapped
load, cooldown timestamps restart together at virtual now; existing capacities and
growth factors remain intact. This intentionally resets cooldown progress once.
The last capacity-update timestamp allows a refresh immediately. Subsequent loads
restore virtual timestamps and RNG state without migration.

Original functions are restored even when a callback raises an error. GUI callbacks
are not wrapped. This changes Natural Town Growth whenever the multiplayer mod is
enabled, including solo play: switching clocks when a peer connects would itself
break saved cooldowns. To roll back, restore the original multiplayer mod.lua and
load a pre-experiment save; virtual timestamps should not be interpreted as wall
timestamps by the unwrapped mod.

This is not universal mod compatibility. It does not intercept cached function
references captured before resource wrapping, top-level script execution, custom
PRNGs, deferred callbacks, file/network inputs, or native asynchronous effects.
It does not align callbacks across different simulation steps or repair existing
world divergence. The command barrier remains separate work. Other scripts are
deliberately not enabled until their state and callback behavior are checked.

Offline regression: `python tools/deterministic_script_test.py`, using the actual
downloaded Workshop script (default Workshop ID 1954591986). It reproduces the
unmodified failures, then checks clock independence, old-save migration, RNG
save/reload and isolation, pause suppression, UTC conversion, restoration after
errors, GUI exclusion, and the actual modifier registration/filter.
