# Offline Workshop registration prototype

Build with `build.bat`. This produces `out/tpf2_workshop_register.dll`
and runs the standalone catalogue tests. It does not install or launch anything.

The prototype supplies ONE explicitly configured ID and folder to the game's
Workshop discovery input. It does not subscribe to Workshop items, edit Steam
manifests, transfer files, or enable the existing mod-sharing feature.

## Test setup (requires a separately authorised game restart)

Place the DLL and matching `tpf2_workshop_register.cfg` in the test instance's
plugin folder. Set `enabled=1`, a numeric Workshop `id`, and `path` to an absolute
UTF-8 folder containing `mod.lua`. Use files matching the host, including any
multiplayer determinism changes. For a useful test the item must be absent from
that instance's Steam subscriptions: an existing Workshop entry takes precedence.
For an explicit replacement experiment, `replace_test_item=1` substitutes only
the configured item in discovery; it does not change Steam's subscription state.
Leave this switch off for the normal missing-item test.

Open the load screen after restarting. The host log should report
`offered=1 catalogue=PRESENT`. That confirms catalogue registration, NOT successful
script execution. Confirm the missing-mod warning is gone, load the save, and
check that the mod's scripts execute and multiplayer hashes match. Test again
after a second catalogue refresh. Remove the plugin or disable it and restart
to end the experiment. Do not save over the original test world.

## Reverse-engineering evidence and limits

Steam build 35924, guarded by the host build check and the exact 20-byte hook
prologue. `ModRep::RefreshModList` at RVA `0x2373210` reads a Workshop result:
flat-map controls at +0, slots at +8, capacity at +24; each 32-byte backend slot
has an integer key and a vector of 64-byte ID/wide-path pairs. The removed-mod
vector at +48 is preserved. Steam's backend is identified at runtime by its `*`
prefix in the table at RVA `0x4147330`.

The replacement flat map supports ONLY the iteration performed by this function;
it is not suitable for hashed lookup. Existing strings are borrowed until the
original call returns. The native map insertion helper at `0x236f740` calls the
engine's string and wide-string copy constructors (`0x98270`, `0xa9e10`), so the
engine owns the copied paths. Its final catalogue at ModRep+0xe0 is inspected for
the expected `*ID`, version 1. No private-container writes are made to ModRep.

The corrected prototype was tested with two instances on 2026-09-14. The sandbox
reported `offered=1 catalogue=PRESENT` for an item absent from its Workshop
discovery, and the user confirmed successful operation after deployment of
prototype-2. The first attempt exposed a missing trailing path separator:
the loader concatenates `mod.lua` directly onto the registered directory. Paths
now retain or receive a trailing separator, covered by regression tests.

This is still an opt-in, single-item prototype. Automatic file transfer is not
enabled or validated by this test. Subsequent work must connect catalogue
readiness to the lobby, verify transferred content, and handle dependencies and
multiple items. Offline tests cover shadow construction, duplicate precedence,
replacement mode, both backends, removed entries, and path concatenation.
