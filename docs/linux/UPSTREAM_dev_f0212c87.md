# Upstream dev f0212c87 integration

Incoming: `f0212c87e56cc0d5814907fd5d641b1e004f3c87`.
Continues [02edb897](UPSTREAM_dev_02edb897.md); release remains 0.7.0.7.
The 18-commit merge is staged and uncommitted. **Partial:** native Sandbox
town capture and the minimap extensions remain unported after static analysis
and failed lab launches. See [RE evidence](../re/linux/DEV_F0212C87.md).

## Merged

| Commits | Change and Linux disposition |
|---|---|
| cee1e87, 92a4247, 3427fd5, f75eaa6 | Shared multiplayer toolbar button, round disk/content sizing and icon centering retained. Native dash file handling is already available; visual validation blocked. Minimap-specific styling depends on its absent native backend. |
| 4315e9b, 60e48ff, 80be4ad | Native road-name batching and four-read empty/single-entry path, tests and upstream measurements retained. Both sides of the test conflict preserved. |
| d707d16, 0d9aa8e, b5f0981, a5d73d6 | Windows minimap icon, climate colors, M-key focus filtering and path documentation retained; native gaps below. |
| 4132c04 | Shared descriptor recycler moved to native/src; Windows wiring retained, earlier verified Linux dispatcher guards preserved. |
| 405de71 | Shared master pipe measures inactivity in both directions; local slow one-way stream regression passes. |
| e28c4fd | Shared TOWNC parser/replay and Windows capture retained; native town-tool capture/cancellation remains missing. |
| fa6214c | Shared pacing restores session speed after leader silence; targeted two-player and multi-player fixtures pass. |
| 61a670e | Incoming Linux threaded save compression retained with an additional partial-install fix. |
| d98095f | Native terrain pager fills fresh tiles using UFFDIO_COPY from a read-only zero source, providing private writable pages; incoming pager fixture retained. |
| f0212c8 | Incoming Linux and Windows Steam busy-poll throttles retained; native whole-loop bytes and call target independently verified. |

## Ported

The new save and Steam hooks already included native implementations. Checked
all their guard bytes and call targets against the actual Linux ELF, with SysV
arguments documented in the RE record. Added `verify_save_steam_elf.py` for
repeatable validation. Existing Big Maps (30 sites) and descriptor dispatcher
checks pass too. No Windows address was substituted into native code.

Fixed save takeover after partial installation: the first init hook now forwards
to the embedded compressor until all six redirects succeed. A new fixture
exercises guard refusal, each failed redirect boundary, and successful activation.
The real system libzstd round-trip test also passes with four workers.

Resolved `train_order_linux_test.cpp` by keeping both old guarded-tail/cap tests
and incoming batched-name equivalence tests. Fixed an existing `net_restart`
test race found during the build: delivery is observable before the receive
thread emits its diagnostic, so the test now waits for that diagnostic.

Updated release provenance and Lua verification to this exact upstream commit:
30 Lua files, no exceptions, 32 HUD textures and both multiplayer toolbar icons.
Windows implementations remain intact.

## Not ported

- **Native Sandbox town-tool capture/cancellation**, including the new RemoveTown
  diagnostic probe. Static work identifies the tag-0x11 factory `0x15ee340`,
  three callers and the 0x90-byte Linux element copy. The source vector is moved
  from RSI; callback ownership and the true UI caller need live confirmation.
  Linux also reads trailing TownInfo fields that cannot be assumed unused.
  Both lab attempts failed before startup, preventing town placement and gdb
  probes. Shared TOWNC reception/replay is present, but placing a town with the
  native Sandbox tool does **not** replicate it. Avoid that action in sessions.
- **Native minimap extensions:** disk/icon deployment, climate coloring and M
  shortcut. The native base minimap was already unported. Static disassembly
  confirms Linux candidates for raw-pixel ImageView, the climate path, global
  UI core and text-editing byte, with concrete layout differences from Windows.
  Terrain/texture ownership, climate repository/variant layout and focused-widget
  lifetime still lack live proof. The lab failure prevented those probes. Native
  keeps its existing stock M action and does not install a nonfunctional minimap.

Other pre-existing native feature gaps are unchanged. Matching release numbers
alone do not establish feature parity or deterministic cross-platform towns.

## Live testing

Two prescribed native lab launches, including the final built candidate, failed
immediately with `bwrap: setting up uid map: Permission denied`, exit 1.
No game process, title menu, Vulkan device, toolbar, save/load, town placement,
gdb attachment or performance measurement was observed. No Proton run was
attempted after the native launcher failure. Upstream dedicated-server
measurements are retained as upstream evidence, not claimed as local results.

Backed up the native libraries/data and Lua mod as `.before-port-f0212c8`,
installed the candidate, then restored and recursively compared both payloads
after each attempt. No save changed and no game remains running. Steam and user
installations were untouched. Evidence is in the job's `meta/live/`.

## Tests

- `tools/linux/build_native.sh`: soldier build, **75/75 CTests**, glibc <=2.31 checks pass.
- `python3 tools/linux/verify_lua_release.py`: exact 30 Lua files, 32 HUD textures,
  two toolbar textures; manifest `4f1370ac2781c929d3526bdef7b39d17ddf945af5f25160a8707c7e7528f6e3a`.
- `verify_save_steam_elf.py`: 5 whole-function guards and 7 rel32 targets pass.
- `verify_descriptor_dispatch_elf.py`: five slots and 40-byte guard pass.
- `bigmap/tools/linux/verify_game.py`: all 30 native patch sites pass.
- `tools/sandbox_towns_test.py`: all shared Lua 5.2 replay/codec checks pass.
- `tools/test_tcp_connectivity.py`: 15 tests pass, including activity beyond
  the master's one-direction timeout. Router operations are mocked.
- `tools/pacing_sim.py --only leader_leaves_two` and `--only leader_leaves`: pass.
- Full `tools/pacing_sim.py`: one existing `slow_joiner` check fails (within 4
  units after tick 1800); HEAD and merged runs have identical reported results
  for that scenario. Recorded rather than weakening the assertion or changing
  unrelated pacing behavior. All other scenario checks pass.
- Host `test_save_zstd`: real libzstd 1.5.7 round-trip, 25,165,824 input bytes,
  four workers, 9,894,603 output bytes. This is not a live game save.

Python test dependencies were installed only into a clone-local virtualenv.
An initial all-requirements install could not build miniupnpc without Python
headers; the tests mock router operations and pass with pystun3/zstandard/lupa.
