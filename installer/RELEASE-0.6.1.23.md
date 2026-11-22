EXPERIMENTAL: the Steam save-transfer stall is fixed, Steam's send rate is actually raised, and a new title menu

This is a test build and a pre-release. The stable release is still 0.6.1.19, and the dedicated test server stays on it. Everyone in a session needs this exact version.

How to install

Windows (Steam, game build 35924): download `TpF2Multiplayer.msi` above, close the game and run it. To go back, install the 0.6.1.19 MSI.

Linux and Steam Deck (Proton): download `install_proton.sh` above and run `sh install_proton.sh`. This copy installs this release.

Fixes, to test
- **Save transfers over Steam stalled at 0%.** Every packet carries a number so a copied packet cannot be replayed, and the receiver accepts only the latest 64. Save pieces shared that numbering with the small status packets. Over Steam a save piece waits in Steam's queue while later status packets overtake it; once 64 had passed it, the piece was thrown away as "too old", and so was every re-send of it. Save pieces now have their own numbering, the receiver accepts the latest 1,024, and a thrown-away packet is logged instead of vanishing.
- **Steam's send rate was never raised.** The setting meant to lift Steam's send-rate limit used the wrong setting numbers. It changed an authentication option and the connection timeout instead. It now sets the send rate.

New
- **Redesigned title dialogs** with native-style tabs and a backdrop, and a refined in-game multiplayer dashboard that can be collapsed and expanded (by tearded).

Still in this build: the game-speed fix (0.6.1.21), and the Steam ID join code, CROSS-PLAY and Workshop mods through Steam (0.6.1.20).

What to send back
- How long a save takes to send. If it stalls, the joiner's log now says whether packets were thrown away as too old: OPEN LOGS in the Multiplayer window gathers it.
