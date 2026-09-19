Line editing without the wait

Everyone in a session needs this version, and the dedicated relay runs it.

How to install

Windows (Steam, game build 35924)
1. Download `TpF2Multiplayer.msi` below.
2. Close the game and run the MSI. It installs into the game folder and the mod into the game's mods.
3. Start the game: Main menu → Multiplayer. Installs of 0.5.7 or newer get this update offered in game; older ones install the MSI by hand.

Linux and Steam Deck (the Windows game under Proton)
1. In Steam, set Transport Fever 2 to run with Proton (Properties → Compatibility; Proton 9 or newer), start it once and quit.
2. Download `install_proton.sh` below, close the game, and run `sh install_proton.sh`. It finds Steam, the game and the Proton prefix, downloads `TpF2Multiplayer-files.zip` from this release, checks it against `SHA256SUMS.txt` and installs; `--dry-run` shows the plan first. It needs only bash, curl, sha256sum and unzip. (`install_proton.py` does the same with Python 3.9 or newer: `python3 install_proton.py`.)
3. Start the game: Main menu → Multiplayer. Full guide: docs/proton/INSTALL.md.

Manual install (any platform): `TpF2Multiplayer-files.zip` holds the files in the game-folder layout.

What changed
- Adding stops to a line no vehicle runs yet applies on your game at once; the other players get it at the stamp as before. Once a vehicle is on the line, edits go back to the delayed path that keeps every game in step.
- New line opens on a line made ahead of time in lockstep (one spare per player, owned by a hidden pool company, so it never shows in a line list). The editor opens on it as soon as it is yours, named and coloured as your click chose. The first spares are made a few seconds after a session starts.

Fixes
- Upgrading the second track of a double track (or any track beside a parallel one) failed on every game: the replay took the neighbouring track 5 m away for a crossing and the engine refused the build. An upgrade now replaces its edges in place, and a parallel edge is never a crossing.
- A host loading an earlier save mid-session read itself as thousands of units behind, dropped its builds and stamped every command 15 units out, because the other players' heartbeats from the old world still reached the new one. A world switch now starts a new world for every player's bridge.
- Quick stop clicks could put the same station on a line twice when the engine re-resolved the platform between two clicks; a stop is now identified by its station.
