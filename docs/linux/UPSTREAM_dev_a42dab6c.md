# Upstream dev a42dab6c integration: retain hot-join requests during loading

Windows target: `a42dab6c0c27359bdf7fdc241176f8e72c4d569a`.
Linux parent: `fab2f4642400549796fd072bbe3fd810f52662e5`.
One Windows commit; no conflicts. Merge staged and uncommitted; version remains 0.7.

## Merged

Keep upstream's change to `native/src/menu_hook.cpp` unchanged, alongside
the pre-existing Linux-branch company-config fixes. Windows SyncPoll
retains `tpf2_sync_save.txt` until the game UI exists, logging the wait once.

## Ported (what and how)

Apply the same policy in `native/linux/src/lobby_linux.cpp::SyncPoll`.
While `g_gameUiSeen` is false, leave the request on disk and log once.
Once true, reset the diagnostic latch, unlink the request and call the existing
SyncStart path. Host checks, pending-save coalescing and save publication stay
in that path. Do not return early from SyncPoll: an existing save watcher must
still run while a request waits.

Readiness uses the existing OnGameUiFrame callback and OnMenuPage reset,
not the loading-page state alone. The existing native game-UI bridge is
documented in [MENU_GAME.md](../re/linux/MENU_GAME.md). This change adds no
addresses, binary patches, struct offsets or calling conventions; no new
reverse engineering is needed. Shared Lua and its verifier baseline are unchanged.

## Not ported

None for this commit. Earlier integration limitations are unchanged.

## Live testing

No game was launched. This is a file-request/readiness policy change covered
through the actual production poller in the native fixture; no new engine
contract needs a live probe. No actor files, saves, installed mod or Steam
state were changed. Loaded-world hot-join transfer was not observed in this job.

## Tests

- `tools/linux/build_native.sh`: soldier build, **63/63 CTests passed**
  (including `lobby_ready`), and glibc baseline checks passed.
- `lobby_ready` now exercises repeated polls before a world, a loading menu
  without a game-UI frame, readiness through OnGameUiFrame, exactly one save
  request, another joiner while that save is pending, and existing stable-save
  publication. It uses temporary files and stubbed engine save calls.
- `python3 tools/linux/verify_lua_release.py`: 29 Lua files and 32 HUD glyphs
  passed; manifest SHA-256
  `cbcd41b781d05d4f475926a898d5cd1cf44c637207e20d962279cc4f43863885`.
- `bash -n tools/linux/build_release.sh` and staged whitespace checking with
  `git -c core.whitespace=cr-at-eol diff --cached --check`: passed. The default
  staged check flags the preserved upstream CRLF lines as trailing whitespace.

Build and Lua logs: `.git/port-a42dab6-build.log` and
`.git/port-a42dab6-lua.log` in this disposable clone.
