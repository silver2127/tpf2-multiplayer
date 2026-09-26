EXPERIMENTAL: saves between Steam players go over a direct TCP connection, and live join

This is a test build and a pre-release. The stable release is still 0.6.1.19, and the dedicated test server stays on it. Everyone in a session needs this exact version.

How to install

Windows (Steam, game build 35924): download `TpF2Multiplayer.msi` above, close the game and run it. To go back, install the 0.6.1.19 MSI.

Linux and Steam Deck (Proton): download `install_proton.sh` above and run `sh install_proton.sh`. This copy installs this release.

New, to test
- **Saves between Steam players take a direct TCP connection.** Players who join with a Steam ID connect through Steam, so neither game knew the other's address and every save went through Steam. When a save transfer starts, the host and the joiner now tell each other their addresses over the encrypted Steam connection. The joiner tries the host, and the host tries the joiner. Whichever connects first carries the save at full line speed; if neither can, Steam carries it as before. Only players already in the session see these addresses.
- **Live join** (tearded): with `tpf2mp_live_join.txt` set to 1, the players already in keep their worlds and only the newcomer loads.

Changed
- **Steam transfers use small pieces again.** The 32 KB pieces of 0.6.1.21 to 0.6.1.23 still stalled on a real Steam link after 8 MB. They are off; a file `tpf2mp_steam_big_chunks.txt` in the host's data folder turns them on for testing.

Still in this build: the replay-window and Steam send-rate fixes (0.6.1.23), the new title dialogs and dashboard, the game-speed fix, and the Steam ID join code, CROSS-PLAY and Workshop mods through Steam.

What to send back
- How long a save takes, and whether the log says `taking the save over TCP`. OPEN LOGS in the Multiplayer window gathers the logs.
