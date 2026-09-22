Player-hosted games get a relay when the punch cannot work

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
- Relay fallback for player-hosted games: when a joiner's direct dial and the hole punch both fail (a host behind carrier-grade NAT, a joiner behind a symmetric NAT), the master server lends the pair a UDP port and forwards their frames. The frames stay sealed with the session key; the server reads nothing. A direct path that answers first still wins. Hosts and joiners on older versions are unaffected.
