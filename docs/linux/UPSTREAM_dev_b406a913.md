# Upstream dev b406a913 integration: dashboard tabs

Upstream: `b406a9139676d63d67f61b4e7b77699c2070f0f8`.
Its parent is `e4702669ab99ae05074dc5b5540dbc1115e66129` (Windows 0.5.6).
Linux base: `070f96a` on `linux-native`, native Steam build 35924.
This is an uncommitted development integration, not a new published release.

## Changes

- Merge the shared `lockstep.lua` dashboard change unchanged. Lobby, stats,
  chat, companies and speed now show one section at a time. Clicking the open
  tab closes it; brackets mark its active label. Chat is the default tab.
- Preserve the selected tab (including all-tabs-closed) across dashboard
  rebuilds. Leaving chat closes its input so hidden fields do not capture keys.
- Retain the upstream `tools/dash_tabs_test.py`, which executes the production
  Lua block under Lua 5.2 with stand-in widgets and compiles the entire script.
- Pin `verify_lua_release.py` to this exact upstream commit. Update release
  BUILDINFO provenance and include this record alongside the historical 0.5.6
  record. No release version is invented; `installer/VERSION` is unchanged.

## Linux-specific assessment

The dashboard uses the game's shared Lua GUI API. It is separate from the native
Linux title-menu Vulkan panel in `native/linux/src/panel_linux.cpp`. The change
introduces no DLL calls, executable addresses, struct offsets or ABI changes;
no native hook or reverse-engineering change is required. The shared Lua file
remains byte-identical to upstream, preserving the Windows path too.

## Validation

- `tools/linux/build_native.sh`: soldier SDK build, 37 native CTests and glibc
  compatibility checks.
- `python3 tools/linux/verify_lua_release.py`: all 28 Lua files byte-identical
  to `b406a913`; manifest SHA-256
  `93e09141763ea6fdd77b3298e56a778b71b9ef015e7c13813b9e58c88efdd5f0`.
- `tools/dash_tabs_test.py` with the existing Lua 5.2/Lupa environment: five
  tests covering default selection, mutual exclusion, toggle-off, closing the
  chat input, and rebuilding with an open tab or all tabs closed.
- `bash -n tools/linux/build_release.sh` and `git -c core.whitespace=cr-at-eol diff --cached --check`
  (upstream Lua CRLF endings are retained for byte identity).
- Additional `tools/lobby_panel_test.py` could not start: the available Python
  environment lacks `stun`. No dependency was installed. This does not affect
  the five dashboard-tab tests or either required verification command.

## Not ported / limits

None from this commit. The pre-existing native resync, company ownership,
Workshop and other gaps documented in `UPSTREAM_0.5.6.md` remain; this dashboard
change does not implement their underlying operations. Tests use stand-in GUI
widgets, not a running game. No Steam/game process was launched, no installation
or publication was performed, and no live cross-platform test is claimed.
