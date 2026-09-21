**Experimental.** This build carries one fix for Linux and Steam Deck players and is published as a pre-release for them to try. The dedicated server stays on 0.6.1.12 until this is promoted, and the version gate is exact: everyone in a session needs the same version, so players on this build can host and join each other, not the dedicated server.

How to install

Windows (Steam, game build 35924): download `TpF2Multiplayer.msi` above, close the game and run it. It installs into the game folder and the mod into the game's mods, over any older version. Start the game: Main menu → Multiplayer.

Linux and Steam Deck (the Windows game under Proton): set the game to run with Proton 9 or newer, start it once and quit. Download `install_proton.sh` above, close the game and run `sh install_proton.sh`. It finds Steam, the game and the Proton prefix, downloads `TpF2Multiplayer-files.zip` from this release, checks it against `SHA256SUMS.txt` and installs; `--dry-run` shows the plan first. (`install_proton.py` does the same with Python 3.9 or newer.) Full guide: docs/proton/INSTALL.md.

Manual install (any platform): `TpF2Multiplayer-files.zip` holds the files in the game-folder layout.

What changed
- A resync no longer fails under Proton with "Could not pause all games ... [Errno 2] No such file or directory: '\tmp\tpf2mp-data\...'". That path came from a `TPF2MP_DATADIR=/tmp/tpf2mp-data` launch option: Windows resolves a path without a drive letter against each program's current drive, so the game used one folder and the lobby looked in another. The mod now makes such a path absolute once, in the game, and every part of it sees the same folder. If you set that launch option, you can also just remove it; the default folder works under Proton. An absolute path is left exactly as given.
- When the lobby cannot reach its runtime folder, the resync message now names the folder and where the lobby was looking from, instead of a temporary file.
- The shell installer reads a `SHA256SUMS.txt` with Windows line endings too, and the build writes it with Unix ones (the 0.6.1.12 install on Linux hit this).

If the Proton fix works for you, say so on the issue or in Discord and this becomes the next regular release.
