EXPERIMENTAL: save transfers over Steam no longer stall

This is a test build and a pre-release. The stable release is still 0.6.1.19, and the dedicated test server stays on it. Everyone in a session needs this exact version.

How to install

Windows (Steam, game build 35924): download `TpF2Multiplayer.msi` above, close the game and run it. To go back, install the 0.6.1.19 MSI.

Linux and Steam Deck (Proton): download `install_proton.sh` above and run `sh install_proton.sh`. This copy installs this release.

Fixes, to test
- **Save transfers over Steam crawled or stalled.** The mod hands a save to Steam through a local socket on your own PC, and that socket had Windows' default 64 KB buffer. Most of every burst was silently dropped before it ever reached Steam: saves moved at about 2 MB/s in 0.6.1.20, and in 0.6.1.21 a transfer stopped on one missing piece and timed out. The socket now has a 16 MB buffer, and a missing piece is re-sent at most once a second instead of 20 times, since Steam is already delivering it. In a test through a simulated Steam link, a 48 MB save went from 21 seconds to about one.

Still in this build: 32 KB pieces over Steam and the game-speed fix (0.6.1.21), and the Steam ID join code, CROSS-PLAY and Workshop mods through Steam (0.6.1.20).

What to send back
- How long a save takes to send. OPEN LOGS in the Multiplayer window gathers the logs.
