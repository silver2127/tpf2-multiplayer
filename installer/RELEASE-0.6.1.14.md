The installer finds the game on any drive, and a resync no longer fails under Proton

Everyone in a session needs this version, and the dedicated server runs it.

How to install

Windows (Steam, game build 35924): download `TpF2Multiplayer.msi` above, close the game and run it. It installs into the game folder and the mod into the game's mods, over any older version. Start the game: Main menu → Multiplayer.

Linux and Steam Deck (the Windows game under Proton): set the game to run with Proton 9 or newer, start it once and quit. Download `install_proton.sh` above, close the game and run `sh install_proton.sh`. It finds Steam, the game and the Proton prefix, downloads `TpF2Multiplayer-files.zip` from this release, checks it against `SHA256SUMS.txt` and installs; `--dry-run` shows the plan first. (`install_proton.py` does the same with Python 3.9 or newer.) Full guide: docs/proton/INSTALL.md.

Manual install (any platform): `TpF2Multiplayer-files.zip` holds the files in the game-folder layout.

What changed
- The installer finds the game in any Steam library, on any drive. Before, the folder page was pre-filled from Steam's registration of the game or from the folder a previous install remembered, and otherwise showed the stock `C:\Program Files (x86)\Steam` path; a game in a second library meant browsing for the folder by hand, and a folder without `TransportFever2.exe` was refused. Now, when neither of those holds the game, the installer reads Steam's own library list (`libraryfolders.vdf`) and takes the first library that has it. If you still land on the folder page with the wrong folder, pick the one Steam's **Browse local files** opens.
- A resync no longer fails under Proton with "Could not pause all games ... No such file or directory: '\tmp\tpf2mp-data\...'". A `TPF2MP_DATADIR` launch option with a path that has no drive letter is now made absolute once, in the game, so the lobby and the game use the same folder. Removing that launch option also works; the default folder is fine under Proton.
- The resync message names the folder it could not reach and where it was looking from, instead of a temporary file.
- The shell installer for Linux reads a checksum file with Windows line endings too.

0.6.1.13 was the pre-release of the Proton fix; this release replaces it.
