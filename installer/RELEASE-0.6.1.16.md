**Experimental.** Pre-release for the Steam-networking test, replacing 0.6.1.15: the join through Steam worked on the first try, the save transfer behind it did not. The dedicated server stays on 0.6.1.14 until this is promoted, and the version gate is exact: everyone in a session needs the same version, so players on this build can host and join each other, not the dedicated server.

How to install

Windows (Steam, game build 35924): download `TpF2Multiplayer.msi` above, close the game and run it. It installs into the game folder and the mod into the game's mods, over any older version. Start the game: Main menu → Multiplayer.

Linux and Steam Deck (the Windows game under Proton): set the game to run with Proton 9 or newer, start it once and quit. Download `install_proton.sh` above, close the game and run `sh install_proton.sh`. It finds Steam, the game and the Proton prefix, downloads `TpF2Multiplayer-files.zip` from this release, checks it against `SHA256SUMS.txt` and installs; `--dry-run` shows the plan first. (`install_proton.py` does the same with Python 3.9 or newer.) Full guide: docs/proton/INSTALL.md.

Manual install (any platform): `TpF2Multiplayer-files.zip` holds the files in the game-folder layout.

What changed
- **A save transfer to a player reached through Steam works.** The transfer took a Steam peer for a same-machine peer (it appears at a loopback address), used 8 KB chunks with the no-loss window meant for loopback, and stalled at 15 of 12,781 chunks. A Steam peer now gets 1,100-byte chunks, under Steam's 1,200-byte unreliable limit, with the internet window and its recovery.
- From 0.6.1.15: **Steam's networking carries the connection.** Steam punches through routers on its own and, when no direct path opens, relays through Valve's servers: no port forwarding, no Hamachi, no dependence on the mod's relay. The host's Steam identity rides in the join code; the joiner tries it alongside the usual addresses and whichever answers first wins. Both players must be running the game through Steam, logged in. `tpf2mp_steam_off.txt` in `%LOCALAPPDATA%\tpf2mp\data` turns it off.

What to look for when testing
- Host's `netpunch\lobby_proc.log` (game folder): `JOIN ('127.0.0.1', 621xx)` is a join through Steam, and the transfer line should read `chunks of 1100B (steam)`.
- Joiner's `lobby_proc.log`: `connected to host 127.0.0.1:621xx -- through Steam's networking`, then the save percentage climbing.
- Both `tpf2_bridge.log` files (`%LOCALAPPDATA%\tpf2mp\data`): `[steam] up`, `[steam] session request ... accepted`, `[steam] endpoint ...`.
