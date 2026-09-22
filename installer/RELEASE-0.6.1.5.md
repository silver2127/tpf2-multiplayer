A host with the game in another Steam library can share its Workshop mods again

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

Fixes
- A host whose game is installed in a second Steam library (for example `D:\SteamLibrary`) could not find its own Workshop mods, and every joiner who asked for them was rejected with "The host cannot supply all required mods". The host now looks in the game's own library first.
- That rejection now names the mod and the reason, in the host's log and in the message the joiner sees.
- Dedicated relay: a save upload waits briefly for the leader's mod round before it goes out to joiners, so a leader that shares mods is no longer refused mid-push.
