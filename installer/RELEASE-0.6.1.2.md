The relay serves late joiners again, world switches keep the lobby, the server browser reaches the list, and codes carry VPN addresses

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
- Dedicated relay: a player joining after the leader's first periodic upload was told the relay was sending its world and never received it. The relay now hands a completed upload on at once.
- A host loading another save mid-session no longer leaves the lobby; on the dedicated relay the leader's load is uploaded even when nobody else is in, so the relay keeps the new world instead of the old one.
- Server browser: the list fetch races IPv6 and IPv4, retries once, and shows the error code instead of "no response".
- Codes carry the host's Hamachi or Tailscale address (or `TPF2MP_VPN_IP` for ZeroTier), and joiners dial it first: two players on one virtual LAN connect without any NAT in the way.
