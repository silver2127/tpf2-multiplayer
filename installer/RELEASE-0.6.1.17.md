**Experimental.** Pre-release for the Steam-networking test, replacing 0.6.1.16. With 0.6.1.16 a whole session ran through Steam (join, a 104 MB save, the game), so what is left is speed: that save took about two minutes. The dedicated server stays on 0.6.1.14 until this is promoted, and the version gate is exact: everyone in a session needs the same version, so players on this build can host and join each other, not the dedicated server.

How to install

Windows (Steam, game build 35924): download `TpF2Multiplayer.msi` above, close the game and run it. It installs into the game folder and the mod into the game's mods, over any older version. Start the game: Main menu → Multiplayer.

Linux and Steam Deck (the Windows game under Proton): set the game to run with Proton 9 or newer, start it once and quit. Download `install_proton.sh` above, close the game and run `sh install_proton.sh`. It finds Steam, the game and the Proton prefix, downloads `TpF2Multiplayer-files.zip` from this release, checks it against `SHA256SUMS.txt` and installs; `--dry-run` shows the plan first. (`install_proton.py` does the same with Python 3.9 or newer.) Full guide: docs/proton/INSTALL.md.

Manual install (any platform): `TpF2Multiplayer-files.zip` holds the files in the game-folder layout.

What changed
- **Faster transfers through Steam.** Steam caps each connection at 1 MB/s by default and drops what is offered above that; the measured 0.6.1.16 transfer ran at exactly that against 1.6 MB/s offered. The mod now raises Steam's per-connection send rate and buffers when its Steam side comes up (`[steam] SendRateMax: ... -> ... (set)` in `tpf2_bridge.log`). Whether the client honours it for this API is what this build finds out: compare the save's time with 0.6.1.16.
- From 0.6.1.16: a save transfer to a player reached through Steam uses 1,100-byte chunks and the internet window (it used to take that player for a same-machine one and stall).
- From 0.6.1.15: **Steam's networking carries the connection** when the direct paths fail: no port forwarding, no Hamachi, no dependence on the mod's relay. `tpf2mp_steam_off.txt` in `%LOCALAPPDATA%\tpf2mp\data` turns it off.
