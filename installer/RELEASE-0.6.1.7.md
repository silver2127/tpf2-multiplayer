Mods go out in batches, so a save with hundreds of Workshop mods can be shared

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
- A joiner who lacked 314 of a save's 486 Workshop mods never got them: the host zipped everything into one blob in memory and dropped the joiner as silent while it did. Mods now go out in batches of about 200 MB, zipped in the background while the previous batch is on its way; the joiner installs each batch as it lands and registers them all at the end. Memory stays bounded and the host keeps answering throughout.
- The joiner's log now says why a transfer was given up and what each batch installed.

A joiner on an older version still receives the mods, as one blob, as before.
