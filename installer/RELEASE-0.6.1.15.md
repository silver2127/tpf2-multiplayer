**Experimental.** Pre-release to try one thing: the mod now connects through **Steam's own networking**. The dedicated server stays on 0.6.1.14 until this is promoted, and the version gate is exact: everyone in a session needs the same version, so players on this build can host and join each other, not the dedicated server.

How to install

Windows (Steam, game build 35924): download `TpF2Multiplayer.msi` above, close the game and run it. It installs into the game folder and the mod into the game's mods, over any older version. Start the game: Main menu → Multiplayer.

Linux and Steam Deck (the Windows game under Proton): set the game to run with Proton 9 or newer, start it once and quit. Download `install_proton.sh` above, close the game and run `sh install_proton.sh`. It finds Steam, the game and the Proton prefix, downloads `TpF2Multiplayer-files.zip` from this release, checks it against `SHA256SUMS.txt` and installs; `--dry-run` shows the plan first. (`install_proton.py` does the same with Python 3.9 or newer.) Full guide: docs/proton/INSTALL.md.

Manual install (any platform): `TpF2Multiplayer-files.zip` holds the files in the game-folder layout.

What changed
- **Steam's networking carries the connection.** Steam punches through routers on its own and, when no direct path opens, relays through Valve's servers: no port forwarding, no Hamachi, no dependence on the mod's relay. The host's Steam identity rides in the join code, and the joiner tries it alongside the usual addresses; whichever answers first wins, so a direct connection still wins when there is one. Both players must be running the game through Steam, logged in. `tpf2mp_steam_off.txt` in `%LOCALAPPDATA%\tpf2mp\data` turns it off.
- Also from 0.6.1.14: the installer finds the game on any drive, and a resync no longer fails under Proton because of a Unix-style `TPF2MP_DATADIR`.

What to look for when testing
- On both PCs, `tpf2_bridge.log` (in `%LOCALAPPDATA%\tpf2mp\data`) has a line starting `[steam] up: id=...` once the game is at the title menu.
- In the joiner's `netpunch\lobby_proc.log` (game folder): `[steam] the host is ... on Steam -- dialling it through Steam` and, if that path won, `connected to host 127.0.0.1:621xx -- through Steam's networking`. If a direct address answered first the line names that address instead; to force the Steam path, block UDP 29471 on the host's router or firewall.
- Send both logs, plus the host's `tpf2_bridge.log`, either way.
