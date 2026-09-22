# Upstream dev 49b1f277 integration: in-game session redesign

Windows target: `49b1f277e7be3c9ad853dffb14b0e9dcd70ab095` (release remains
0.6.1.28). Linux merge parent: `a2febfcc5dacc28175e24b9d246eb2b1e1f5f521`.
The merge remains staged and uncommitted; there were no conflicts.

## Merged

- `0b9f250`: apply the menu redesign to in-game session management.
- `49b1f27`: merge origin/main into dev, including its resolved Cross-play
  presentation. Windows renderer and tests are preserved as merged.

## Ported

Native setup and active sessions now use the redesigned panel. Hosting a loaded
world shows player/game/password fields, session settings and Host session.
Active sessions show roster paging, expanded chat, invitation copying and Close;
hosts get Resync through the existing native recovery panel. Save picker,
Start game and Leave lobby controls remain title-only. Cross-play in an active
host lobby/session follows the merged Windows hostSteam visibility gate.

The native panel is 780 x 540 logical pixels with viewport scale clamping.
Title-only backdrop and outside-panel input behavior are preserved in-game.
No game hook or ABI changed. See [implementation evidence](../re/linux/DEV_49B1F277.md).
README, installation, release packaging and Lua provenance now reference this
integration; upstream Lua/glyph contents are unchanged.

## Not ported

None from these commits. Inherited limitations remain, including default-off
canonical simulation ordering; see [the preceding integration](UPSTREAM_dev_0620342e.md).
Matching versions do not establish Windows/native simulation parity.

## Live testing

Backed up native actor share/tpf2mp and game/mods/mp_lockstep_1 using cp -a to
.before-port, installed this job's five soldier libraries and merged Lua, and
invoked tools/sandbox/tpf2mp-lab run native with a 180-second limit. The launcher
exited 1 immediately: `bwrap: setting up uid map: Permission denied`.
No game execution, menu, loaded world, GPU selection, XTEST input or gdb probe
was observed. Both directories were restored; no lab game remains running.
Steam and user installations/saves were untouched.

Evidence is in the job's meta/live/launch.txt, restoration.txt, actor-logs/ and
actor-data/. Copied actor logs/data may predate this failed launch and are not
proof this build ran. Live visual validation remains outstanding.

## Tests

- tools/linux/build_native.sh: soldier build, 57/57 CTests and glibc baseline.
- Native panel tests cover in-world host/client/setup, recovery navigation,
  two roster pages and action guards, stale savePicker state, hostSteam gating,
  modal isolation, viewport bounds and disjoint controls across five scales.
- python3 tools/linux/verify_lua_release.py: 29 Lua files (one pinned native
  integration) and 32 glyphs pass against target 49b1f277.
  Manifest: b7410c4b3eb76fbe3dd95ded62fc6bcb8852f6b05611f6afcea04141816c2f1c.
- Release shell syntax, CRLF-aware whitespace and empty unmerged index checks.

The fractional-scale disjoint-control test found a one-pixel overlap between
adjacent roster rows at scale 0.8. Row positions now advance by the same scaled
height as their hit boxes, matching the Windows row-height calculation.
