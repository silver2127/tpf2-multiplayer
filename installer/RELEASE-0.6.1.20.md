EXPERIMENTAL: the join code is your Steam ID, and Workshop mods come from the Workshop

This is a test build and a pre-release. The stable release is still 0.6.1.19, and the dedicated test server stays on it. Everyone in a session needs this exact version: a 0.6.1.19 player cannot join a 0.6.1.20 host, and a 0.6.1.20 player cannot join a 0.6.1.19 host.

How to install

Windows (Steam, game build 35924): download `TpF2Multiplayer.msi` above, close the game and run it. It installs over any older version. To go back, install the 0.6.1.19 MSI.

Linux and Steam Deck (Proton): download `install_proton.sh` above and run `sh install_proton.sh`. This copy installs this release, not the latest stable one.

New, to test
- **The join code is your Steam ID.** When your game runs on Steam, HOST GAME copies your 17-digit Steam ID as the code. Friends paste it, or your Steam profile link, into JOIN and connect through Steam's own networking, with no ports and no IP address in the code. A Steam ID is public, so the session key is exchanged over Steam when a player joins; a lobby password still works as before.
- **CROSS-PLAY.** A checkbox on the host card, and in the lobby while you host. Ticking it switches to the classic code, so players without Steam can join: GOG copies, Steam in offline mode, or a game started outside Steam. In the lobby it switches live, copies the new code, and keeps everyone who is already in. A game that is not on Steam always uses the classic code.
- **Workshop mods come from the Workshop.** When you say yes to the mods prompt, your game subscribes to the missing Workshop mods and Steam downloads them; you stay subscribed afterwards. The host sends only its local mods, plus any Workshop item Steam cannot deliver: hidden or removed, Steam offline, or a download that stalls for two minutes. The mods are registered with the running game, so no restart is needed.

What to send back
- Whether joining with a Steam ID worked, and whether the Workshop download started. The lines to look for are `[steam]` in `tpf2_bridge.log` and `through Steam` in the lobby log. OPEN LOGS in the Multiplayer window gathers both.
