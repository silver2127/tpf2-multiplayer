EXPERIMENTAL: faster save transfers over Steam, and the game speed no longer gets stuck low

This is a test build and a pre-release. The stable release is still 0.6.1.19, and the dedicated test server stays on it. Everyone in a session needs this exact version.

How to install

Windows (Steam, game build 35924): download `TpF2Multiplayer.msi` above, close the game and run it. To go back, install the 0.6.1.19 MSI.

Linux and Steam Deck (Proton): download `install_proton.sh` above and run `sh install_proton.sh`. This copy installs this release.

Fixes, to test
- **Save transfers over Steam were slow.** Since the join code became your Steam ID, every save goes through Steam, in 1,100-byte pieces with little data in flight. A 134 MB save took more than a minute at about 1.7 MB/s, while Steam itself had room to spare. When every other player is on Steam, saves now go in 32 KB pieces over Steam's reliable channel, with up to 4 MB in flight. A session that mixes Steam and CROSS-PLAY players keeps the small pieces.
- **The game speed dropped and would not come back up.** With everyone voting 4x, the session fell to 2x, then 1x. The mod limits the session to what the host can simulate. On a host whose game ran every speed about a quarter slow, each limit made the world slower, so it kept stepping down. A limit that costs speed is now lifted, and the session runs at the voted speed.

Still in this build from 0.6.1.20: the Steam ID join code, CROSS-PLAY, and Workshop mods downloaded through Steam.

What to send back
- How long a save takes to send, and whether the game holds the speed you pick. OPEN LOGS in the Multiplayer window gathers the logs.
