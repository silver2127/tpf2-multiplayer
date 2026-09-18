Faster save transfers, a backup link for every frame, and solo play without the lockstep overhead

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
- Save and mod transfers stream over a TCP side channel; the UDP path stays as the fallback. A joiner receives a big save many times faster.
- Every sealed frame travels over UDP and a TCP backup link at once; the first copy to arrive wins. A lossy UDP path no longer stalls the session.
- Playing alone: while nobody else is in the game there is no world hash and no held lever, so a solo host runs at full speed.
- The relay keeps player statistics (player_stats.json).
- A shell installer for Linux and Steam Deck (`install_proton.sh`), no Python needed.

Fixes
- Station icons: the engine handle comes from the item's own constructor, never a cache carried across worlds.
- A map larger than Megalomaniac keeps the hash cost ladder even under a forced cadence.
- A random default player name is never mistaken for a typed one: the Steam persona replaces it.
