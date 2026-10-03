## 0.7.0.6 - cargo filters, big maps that load twice, fewer desyncs

Stable update based on 0.7.0.5, for Windows, Proton and the native Linux game. Everyone in a session, including the dedicated server, must update to 0.7.0.6.

### Changes

- **Cargo filters on line stops now work in multiplayer.** A stop's cargo filter set in the line editor was lost on every game, the player's own included, as soon as the edit was replayed. It now reaches every player and stays set.
- **No more crash when a big map is loaded twice in one session.** Hosting a large Big Maps save right after playing it (or reloading it) crashed at 70% of the load. Big Maps' terrain cache released its file twice when several terrain threads finished at once; it is released once now.
- **A player joining far behind no longer desyncs the session.** A game catching up after joining could send commands (a speed vote, its company headquarters) that reached the others after their time; the towns then grew apart. Every command now lands in every game's future.
- **Terrain painting and shaping no longer stall the session.** A paint stroke sent its whole area uncompressed, up to 840 KB each; over a relayed connection every stroke arrived late, the session slowed to a crawl and station placements waited behind it. Terrain edits are now compressed, about 30 times smaller for painting.
- **Chat messages wrap** instead of being cut off, in the lobby and in the game.
- **OPEN LOGS after a crash** now includes the crashed run's crash dumps and its multiplayer logs, not only the next run's.
- **A session no longer starts without the bridge.** When the multiplayer bridge did not load, chat worked but nothing that was built reached the other players; the panel now says why and does not start the session.
- **Releases show the two launchers first.** The release page carries the Windows and Linux launchers, and beside them the direct installers: the MSI, the Proton script and the native Linux installer.

### Update

Use the launcher: **Update & play** (Windows and Linux), or install directly: the MSI on Windows, install_proton.sh under Proton, the native .run on Linux. All participants need the same version.

### Known limitations

- A cargo filter set while creating a new line (before its first save) is not carried; set it on the line once it exists.
- The native Linux terrain pager can still thrash on a running big map.

### Validation

The terrain codec's round trips and 20,000 damaged frames; the Big Maps sidecar test with eight threads releasing while four decode, 200 rounds; the log archive tests; the cargo filter tested live on two instances (the filter read back identical on both); the menu panel's rendering test with long chat messages; the stamp rule tested for joiners 5 to 63 game units behind. The Linux libraries are built and tested in the pinned Steam Runtime soldier SDK.
