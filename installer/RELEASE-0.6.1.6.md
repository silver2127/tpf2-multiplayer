Mod downloads no longer stall a host with many mods, and both sides say what they are doing

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
- A host packaging a joiner's mods did it inside its lobby loop: with hundreds of Workshop mods the host went silent for the whole zip, joiners saw nothing, and no log said why. Packaging now runs on its own thread while the lobby keeps answering; the joiner sees "the host is packaging the mods you need" with a running count.
- Both sides log the mod request: the host says how many mods were asked for, how many its save lists and their names; the joiner names what it asked for and says so after 20 seconds without an answer.
- A request for mods the host's save no longer lists gets an answer instead of silence.

Reminder for hosts: pressing HOST again starts a new lobby with a new code; joiners on the old code see "host unreachable". Host once, then press START GAME to share the save.
