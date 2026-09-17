# Upstream dev 55e97a48 integration

Windows tip: `55e97a48b7dfdf46743fab5a78a8aefdd14f2ed9`.
Linux baseline: the existing `port/dev` merge parent, following 0.5.6 and
`b406a913` dashboard tabs. This integrates the requested oldest 20 commits;
the remaining 38 pending commits are outside this batch. No commit, release,
installation or publication is made. `installer/VERSION` is unchanged.

## Merged

| Commits | Behavior |
| --- | --- |
| eb41fd3 | vehicle-key drift matching and offender names |
| 857e955, a958ec6, b57c932 | line decoder diagnostics and removal of arbitrary bounds |
| 96c1aaa | two-to-four-component versions in shared updater / Windows MSI |
| fd92df9, 95d0629 | exact float waits, delayed native-edit readback, departure logging, slowest-peer speed governor |
| 57942f5, 5dab0e2 | two-unit line-assignment grid, origin retries unbound vehicle keys |
| 4a5f124 | shared host-authorized recovery decline and per-world prompt suppression |
| 5cb5938, c086bfb | Windows automatic build/install and every Sandboxie overlay/plugin refresh |
| d417fe8 | keep-log flag |
| d4dc6a1, 180a8d3 | command completion, train path and halt step diagnostics |
| ec6f9fb | dashboard title-bar close hides it |
| 9a29adf, 93b4681 | company names, dropdown and finances entity names |
| 03909ea, 55e97a4 | Steam persona default and longer player/lobby names |

Both conflicts are resolved and staged. Lobby retains all prior Linux paths,
process and signal support plus upstream PeersLog retention. The Lua dashboard
retains the earlier mutually exclusive tabs and the new shared hide function
used by the title-bar close callback. No incoming Windows native path is edited.

27 Lua files match the exact tip. lockstep.lua is the reviewed combination of
tip plus previously integrated tabs: its full difference is in
[UPSTREAM_dev_55e97a48_tabs.patch](UPSTREAM_dev_55e97a48_tabs.patch).
The verifier pins its SHA-256 rather than silently exempting it. Overall Lua
manifest: `9032b980b7e78b3de6797428df37f2302f0f308bdc01fc89b97a1ef0dc55babc`.
Release BUILDINFO reports this distinction.

## Ported

- Native Linux line capture uses float waits, %.9g, no sorting, no arbitrary
  stops/platforms/waypoints or positive-index ceiling. Readability, structural
  vector checks, nonnegative indices and valid engine modes remain enforced.
  NaN is refused; negative, infinite and fractional waits round-trip. Each
  refusal names its check. Dynamic record allocation now checks size overflow
  and allocation failure instead of imposing the old 1 GiB policy ceiling.
- Roster changes atomically write mp_players.txt with the same normal/relay
  origin mapping as the bridge and company config, so the shared picker can
  show player names. Public/lobby title buffers accept the longer names.
- Linux menu follows the game's already-loaded Steam API via verified SysV C
  exports; it persists auto=, migrates legacy names as typed overrides, avoids
  overwriting active typing/lobby identity, and falls back when Steam is not
  ready. Typed ASCII names support 64 characters and persisted/persona storage
  supports 127 payload bytes plus NUL, as Windows's 128-byte buffers do.
  UTF-8 backspace and byte-boundary truncation preserve whole characters.
- tpf2mp_keep_logs.txt in the resolved Linux data directory makes slice and
  lobby process logs append with session banners. Shared PeersLog checks the
  Linux data directory as well as its upstream io-directory flag. The startup
  archiver copies instead of moving active logs and stops pruning archives;
  existing appending logs remain intact. Remove the flag to restore retention.
- `python3 tools/linux/auto_install.py` builds a complete Linux release when
  tracked inputs change, waits for games to close, and installs through the
  normal guarded installer. Repeat --game for lab copies; plugins accompany
  every install. --once runs one pass. The watcher does not launch or terminate
  games; Sandboxie and MSI remain Windows-only. No watcher/install was run in
  this integration. Linux version packaging already accepts these versions.

RE evidence and ABI notes: [DEV_55E97A48.md](../re/linux/DEV_55E97A48.md).
No new game patch sites are needed. The existing build/prologue/caller byte
checks remain in force.

## Not ported

The native Linux recovery panel's new **Keep playing** button remains unavailable
with the pre-existing recovery-controller gap. Tracing its actual consumers
showed Linux does not create the recovery runtime or implement the required
native hold/pause/drain/save/load acknowledgement protocol. The investigation,
verified existing alternatives, and missing evidence are recorded in the RE
note. Shared host/client decline logic is merged and its tests pass; that does
not establish a functional Linux native recovery UI.

Existing ownership/Workshop/in-app-update and other 0.5.6 gaps are unchanged.
Company name state and its GUI are merged, but do not supply the missing
entity-only ownership support. No live cross-platform gameplay is claimed.

## Tests

- `tools/linux/build_native.sh`: soldier build, **38/38 CTests**, glibc <=2.31.
  New mocked Steam API/name test; expanded native line fixtures (>64 stops and
  waypoints, >8 alternatives, large indices, inf/negative/fractional waits,
  NaN rejection); roster-name tests cover long names, relays and removal.
- `python3 tools/linux/verify_lua_release.py`: passes the pinned 28-file manifest.
- Lua 5.2 through a clone-local Lupa environment: dashboard tabs (5), company
  names, vehicle drift keys, clone assignments, pacing simulation and the
  expanded native line wire test all pass.
- Shared sync readiness/decline (11), updater (5), lobby panel suite pass.
- Linux keep-log regression and mocked automatic installer test pass; release
  shell syntax and diff whitespace checks pass (upstream CRLF respected).

No Steam or Transport Fever 2 process was launched. Tests use mock engines,
mock Steam exports or isolated networking; the real executable was only read.
