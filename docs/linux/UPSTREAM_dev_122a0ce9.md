# Windows dev 122a0ce9 integration: close the resync view

## Merged

Windows target: `122a0ce966ae9ce9f737cbda112e8fcb68f9f472`.
Linux parent: `7ae845fde958eccad93f0b9abcb500532b8061e0`.
One Windows commit, no conflicts. Retain the upstream menu implementation,
shared Windows panel rendering and Windows regression test changes exactly.
Pre-existing Linux-branch company-config fixes in menu_hook.cpp are retained.
The merge remains staged and uncommitted. Release stays 0.7.0.3.

## Ported (what and how)

Native `menu_title_linux.inl` draws the resync x only in game and outside
world I/O. Native hit ID 87 avoids the existing ID 83 Back to lobby action.
`panel_linux.cpp` closes the overlay and clears typing focus. Local
`sync_hide` / `sync_show` actions in `lobby_linux.cpp` change view state under
the existing model mutex; they never send a command or cancel recovery.
They also work while a lobby request is pending.

Manual, detected and unavailable notices are dismissed. A running resync
retains its operation and a hidden flag across repeated sync-state events
and already-answered Ready updates. Errors, unanswered Ready updates,
completion, accepted new notices and clear events reset the flag.
Manage Lobby and the host's Resync action reset it explicitly. The panel's
version-based event polling honors the flag even while the overlay is closed.

This is mod-owned state and rendering, using the existing native View snapshot,
SDL input and NativeIo busy indicator. No game address, bytes, struct offset,
calling convention or game-object lifetime contract changes. The existing
engine integration documented in [MENU_GAME](../re/linux/MENU_GAME.md) and
[Native I/O](../re/linux/NATIVE_IO_LINUX.md) is unchanged; no new ELF RE or
live ABI probe is needed. Windows Interlocked/critical-section operations
map to the native model mutex, rather than importing MSVC/Win32 types.

Advance Lua verification and release provenance to this target, package this
record, and link it from README and installation guidance.

## Not ported

None for this commit. Earlier unrelated native limitations remain unchanged.

## Live testing

No game was launched, no gdb attached and no actor payload installed. This
integration was tested with the native renderer, input, model dispatcher and
polling fixtures. No live visual or multiplayer resync result is claimed.
Steam, lab actors, saves and the user's installed mod were untouched; no
restoration was needed.

## Tests

- `tools/linux/build_native.sh`: soldier build, 67/67 CTests and glibc <=2.31
  compatibility checks passed.
- Extended `lobby_ready` covers local-only hide/show while requests are pending,
  repeated progress, error/Ready/complete/clear/notice transitions, and retained
  operation identity. Extended panel tests cover scales, phases, title versus
  in-world x visibility, world I/O exclusion, close dispatch, hidden polling
  and file-driven Manage Lobby reopening.
- `python3 tools/linux/verify_lua_release.py`: 29 Lua files (one pinned native
  exception), 32 HUD glyphs passed. Manifest SHA-256:
  `d23c25e33ce4089c724211236a1344e7f710d4a6fb7c431f2d60f4a6838892e2`.
- Release script shell syntax, port whitespace checks, exact upstream change preservation,
  empty unmerged index and retained MERGE_HEAD checked. No Windows test binary
  or release package executed.

Logs: `.git/port-122a0ce-build.log`, `.git/port-122a0ce-lua.log` in this clone.
