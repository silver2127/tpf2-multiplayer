Frozen joins, separate companies, and the road-vehicle drift closed

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

What's new
- Joining a running game freezes the session: the host saves, everyone (host included) loads that save, the worlds are compared, and play resumes. Nobody is released until every player is in; a player who leaves mid-way is dropped and the rest continue.
- Separate companies: a lobby setting assigns one per player; station permissions decide whose vehicles may stop where; companies have names and a 20-colour palette; every player's stations, depots and vehicles show colour-coded icons, and another company's windows are read-only.
- A host that loads another save mid-session pushes it to every client; the roster shows each joiner's loading progress.
- Linux and Steam Deck: `install_proton.py` installs the Windows game's multiplayer under Proton.

Fixes
- Road vehicles no longer drift apart after a join (the cause was the join itself; see above) and the free-space check before a junction is order-independent.
- A lost or duplicated command is handled at once instead of recovered late; a paused host no longer drifts from a paused joiner.
- False "town" desync seconds after loading; the line editor losing a new line; a joiner not adopting the host's vehicle and line keys.
