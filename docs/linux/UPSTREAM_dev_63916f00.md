# Windows dev 63916f00 integration — panel file I/O and release mentions

## Merged

Windows target: `63916f006e35ecec8d4af6af024941cd9a07f5f8`.
Linux parent: `1de9b20`.
Two commits: `afb41f5` (optional release webhook role mention) and `63916f0`
(protected GUI file opens, lobby directory fallback, refresh error logging).
No conflicts. All six incoming files remain byte-for-byte upstream.
The merge is staged and uncommitted; release remains 0.7.1.5.

## Ported (what and how)

The shared Lua panel now uses `CM.openFile`, which catches `io.open` exceptions,
treats them as missing files, and logs once per path. Refresh failures are
logged when the error differs from the previous recorded error. The Windows
non-ASCII LOCALAPPDATA fallback resolves the lobby beside the published data
folder. Shared Python and workflow changes allow only the configured Discord
role mention; unset, no role is pinged. No announcement was sent locally.

Native Linux ships this same Lua and uses the same release workflow/tool.
`native/linux/src/boot.cpp:PublishLuaEnvironment` publishes the native data
home as process-local LOCALAPPDATA and the resolved data directory as
TPF2MP_DATADIR. `datadir_linux.h` resolves absolute XDG_DATA_HOME, then HOME,
with a TPF2MP_DATADIR override. Linux UTF-8 paths pass through the shared
protected open; standard data and lobby directories are siblings, including
under a UTF-8 HOME or XDG root. No Windows path conversion is added to native
code. Updated the existing preload/Lua fixture to load the new helper and
supply K to netDir, and added a UTF-8 XDG directory case.

Updated README/INSTALL, packaged integration record, BUILDINFO provenance and
Lua verifier baseline. Reviewed the native source and the existing Linux RE
records (NATIVE_IO_LINUX and DEV_192ECD4D). Neither incoming commit changes
native code, engine addresses, byte patterns, offsets, ABI or lifetimes.
No new binary reverse engineering or live contract proof is needed.

## Not ported

None from these two commits. Existing native feature and live-validation gaps
remain as recorded in earlier integrations.

## Live testing

No game, Proton peer, debugger or desktop input was run. These shared Lua and
release-tool changes were checked offline. No lab actor, save, installed mod
or Steam directory was modified; no backup/restoration was necessary. No live
panel rendering, GPU, gameplay or cross-platform result is claimed. The
preload regression uses a small executable named TransportFever2, not the game.

## Tests

- `tools/linux/build_native.sh`: PASS, pinned soldier SDK, 79/79 CTests
  (62.93 seconds), glibc <=2.31 compatibility checks passed.
- `python3 tools/linux/verify_lua_release.py`: PASS, 31 exact upstream Lua
  files, zero exceptions, 32 HUD glyphs and two toolbar textures. Manifest:
  `0f32d55720205653d6f8811cdb04881efca4fc89a25752c2dc18f68aa5638b76`.
- `tools/gui_io_throw_test.py`: PASS, six tests using real Lua blocks and
  simulated throwing file opens.
- `tools/chat_directory_test.py`: PASS, delivery, no stale fallback and late startup.
- `tools/lobby_panel_test.py`: PASS, two tests. Initial run lacked pystun3;
  installed dependencies in the clone-local venv and reran successfully.
- `tools/linux/test_boot_environment.py`: PASS, seven native path cases and
  non-game passthrough, with the newly built soldier preload library and a
  host-compiled probe. The SDK lacks Lua, so this optional CTest was checked
  separately with `/usr/bin/lua5.2`. Cases cover HOME, XDG, override, Snap,
  UTF-8 HOME, UTF-8 XDG and relative-XDG rejection.
- Release webhook offline dry-run: PASS for unset role, a valid role ID with
  restricted allowed_mentions, and invalid-role rejection; posting mocked to
  fail if invoked. No network post.
- Shell syntax and Git whitespace checks passed; no unmerged entries.

Logs: `.git/port-63916f00-{build,lua,gui,chat,panel,boot,webhook,deps}.log`
and `native/linux/out-soldier/Testing/Temporary/LastTest.log`.
Python dependencies (lupa, pystun3, zstandard) are in `.git/port-venv`;
no system packages were installed. Release packaging and Windows/MSVC builds
were not run.
