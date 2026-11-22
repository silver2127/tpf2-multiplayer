## 0.7.1.6 - the in-game panel with an accented Windows user name, and a join a lone host no longer gives up

Update based on 0.7.1.5, for Windows, Proton and the native Linux game. Everyone in a session, including the dedicated server, must update to 0.7.1.6.

### The in-game panel

- **The Multiplayer window now works when the Windows user name has accents or other non-English letters.** The title-screen menu worked, but in the game the panel never came up: no chat, no Companies tab, no speed votes. The panel looked for the lobby's files and the other players' status under `%LOCALAPPDATA%`, a path the game cannot open when it holds such letters. The failed open stopped the panel's whole refresh, every time, without a line in the log. The panel now reaches the same folders through the short path the multiplayer DLLs already publish, and a file it cannot open counts as missing instead of stopping the refresh. Thanks to the player who tracked this down and reported it.
- **A panel refresh that fails now says why** in the game's stdout log: `[ls-gui] multiplayer panel refresh failed: ...`, once per distinct error. A file the panel cannot open is logged as `[ls-io] io.open threw for ...`.

### Live join

- **A host that had been playing alone no longer abandons the first player's join.** While a joiner's world is prepared, the host holds the game and its periodic roster read is paused. A host that had been alone still counted one player, and after about 15 s it gave up the join as "nobody else in the game". It now reads the player count from the lobby's current control file.

### Native Linux dedicated server

- **The wait between the title menu and loading the save is configurable.** Set `LOAD_DELAY_SEC` (8-120 seconds) in `server.env`, then `tpf2server configure` and `tpf2server restart`. The default stays 45 s; 15 s was tested on a VPS. A Proton server keeps the fixed 45 s.

### Update

Use the launcher: **Update & play** (Windows and Linux), or install directly: the MSI on Windows, install_proton.sh under Proton, the native .run on Linux. A Linux dedicated server: `tpf2server stop`, `tpf2server install`, `tpf2server configure`, `tpf2server start`. All participants need the same version.

### Validation

tools/gui_io_throw_test.py runs the panel's real file code against an `io.open` that throws on a path that is not UTF-8, the way the game's does. The panel finds the lobby through the short path, and an absent player's file reads as missing; 0.7.1.5's code raises the same "file (0000000000000000)" the player saw. tools/test_auto_sync_lua.py covers the roster read during a hold, and the native dedicated test covers the new setting. The Linux port of every change builds and passes its verification.
