# Upstream dev 6e5eec30 integration

Windows target: `6e5eec302bb2d9fdfca70fa44499938efdfddb2c`.
Linux parent: `b9c16dc21408fc2474060715eb48337e9096997f`, following
[5a9b3ae0](UPSTREAM_dev_5a9b3ae0.md). One Windows commit, no conflicts.
Merge staged and uncommitted.

## Merged

- Windows resync/join input remains usable by default; `input_hold=1` restores
  the old window-input block.
- Shared registry scope includes only the save's Workshop IDs, begins empty
  at lobby startup and still accepts explicit installs passed as `extra`.
  `None` retains the old unscoped behavior.
- Frozen-join logs distinguish retained worlds from everyone reloading.

Incoming Windows source and shared Python changes are retained unchanged.
Pre-existing Windows company-config fixes on the Linux branch remain. Lua verifier
and release provenance now reference this target; the existing pinned Linux
origin-replay Lua integration is retained.

## Ported

Linux SDL input suppression is now opt-in through `input_hold=1` in
`tpf2_menu_flags.txt` beside `tpf2_menu.so`. Gesture-release checks remain
mandatory. Ordinary panel capture is unchanged. Legacy script-event
suppression remains active while held, matching Windows and preserving drain.
No new game patch or ABI assumption is introduced. See
[implementation evidence](../re/linux/DEV_6E5EEC30.md).

## Not ported

None from this commit. Prior integration limitations remain, including
canonical sorting default-off pending live validation; this increment does
not establish retained-world Windows/native simulation parity.

## Live testing

No game was launched, no gdb/XTEST probe was performed, and neither lab actor
was modified. Input behavior was tested with native fixtures, not observed
on screen. This policy-only change required no new executable mapping.

## Tests

- `tools/linux/build_native.sh`: soldier build, 57/57 CTests and glibc <=2.31
  checks. Expanded `panel_title` covers default/opt-in input and gesture gates;
  `native_io` covers continued legacy-event suppression and forwarding.
- `python3 tools/linux/verify_lua_release.py`: 29 Lua files (one existing
  pinned integration) and 32 exact HUD glyphs. Manifest SHA-256:
  `b7410c4b3eb76fbe3dd95ded62fc6bcb8852f6b05611f6afcea04141816c2f1c`.
- `tools/mod_download_test.py`: 22 tests pass, including registry scoping,
  explicit extras and unlimited rows.
- `tools/relay_mod_download_test.py`: loopback initial/late joins and
  three-batch mod installs pass. A shutdown-time Bad file descriptor diagnostic
  did not fail the test.
- Release shell syntax, unmerged-index and Windows-source preservation checks.

Python tests use `.git/port-venv` with pystun3/zstandard. Full requirements
installation could not build optional miniupnpc on host Python 3.14; these
local tests do not need UPnP. Logs are in `.git/port-*.log`.
