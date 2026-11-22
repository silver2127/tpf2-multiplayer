## 0.7.1.4 - big maps load past 99%, safer lobbies, and fixes for joiners

Update based on 0.7.1.3, for Windows, Proton and the native Linux game. Everyone in a session, including the dedicated server, must update to 0.7.1.4.

### Loading a world

- **A big-map host no longer sits at "loading world 99%" for minutes.** The big-map terrain cache waits for the first game frame before it stops treating the world as loading, and the multiplayer DLL never told it about that frame. On a PC short of memory every terrain tile then waited up to 2 s, for 3 minutes: the host drew 2 frames a second at 99% while everyone else waited. The cache now sees the game start a few seconds after the load ends.

### Joining a running game

- **A joiner's catch-up builds what the others built.** Construction, fence and terrain commands replayed from the history kept a tag inside their parameters, so a joiner refused them all and its world went its own way from the moment it loaded.
- **A planned rail-over-rail crossing gets its junction** on every player's game, the builder's included.
- **Vehicles bought after a sale bind again.** The game reuses a sold vehicle's id; the next vehicle bought into it was skipped, and its line, sale and name commands failed.
- **Lines with waypoints match their key** within 2 m, and replacing a stop keeps its alternatives.
- **The fallback for a live join that does not match** loads the save it was sent, instead of ending in an error.

### The lobby

- **The host reads and hashes the save off its main loop.** A dedicated server short of memory stood 13 s reading a 372 MB save, and its joiners called it unreachable and left. Joiners also wait 30 s for a host now.
- **One malformed message no longer closes the lobby** for everyone, and a message may have at most 8192 pieces.
- **A player renamed by the host keeps their letter**, their TCP stream and their link.
- **A player name with a space** no longer breaks the TCP backup link.
- **Security fixes:** in a password session only a proven frame moves a peer's address; the public server list no longer lets anyone delist or take over other lobbies; long join codes no longer crash the game; broken mod zips no longer fail the whole mod transfer.

### Installer

- **A refused game folder says why.** Next on the folder page did nothing and showed nothing (not Steam build 35924, the GOG build, a foreign alut.dll). The warning about mods with their own DLLs is now a page of its own, and is shown.
