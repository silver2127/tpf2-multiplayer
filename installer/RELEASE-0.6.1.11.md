Platforms at the stamp, six replay fixes, and nothing phones home

Everyone in a session needs this version, and the dedicated server runs it.

How to install

Windows (Steam, game build 35924)
1. Download `TpF2Multiplayer.msi` below.
2. Close the game and run the MSI. It installs into the game folder and the mod into the game's mods, over any older version.
3. Start the game: Main menu → Multiplayer. There is no in-game update any more: every new version is installed this way.

Linux and Steam Deck (the Windows game under Proton)
1. In Steam, set Transport Fever 2 to run with Proton (Properties → Compatibility; Proton 9 or newer), start it once and quit.
2. Download `install_proton.sh` below, close the game, and run `sh install_proton.sh`. It finds Steam, the game and the Proton prefix, downloads `TpF2Multiplayer-files.zip` from this release, checks it against `SHA256SUMS.txt` and installs; `--dry-run` shows the plan first. It needs only bash, curl, sha256sum and unzip. (`install_proton.py` does the same with Python 3.9 or newer: `python3 install_proton.py`.)
3. Start the game: Main menu → Multiplayer. Full guide: docs/proton/INSTALL.md.

Manual install (any platform): `TpF2Multiplayer-files.zip` holds the files in the game-folder layout.

What changed
- A station clicked into a line gets its platform chosen at the stamp, on every game, from the full stop list. Before, a quick click after the previous one chose against a list that lacked the previous stop.
- The host no longer shares a save its own game cannot load: START GAME stops and names the mods missing on the host's PC.
- Replayed stations are named like the game names them ("<town> Station", "Terminal", "Harbor"), not "Modular station".
- Removed: the in-game updater and the user-local update loader. Mod-hosting sites do not allow a mod that updates itself, and releases are the MSI and the Proton installer anyway.
- Removed: every piece of telemetry. No session heartbeat to the master server, no player statistics on the dedicated server, no desync-report upload and no "send logs?" window. The public game list and the join rendezvous are all the master server does now. For a bug report, send the logs from `%LOCALAPPDATA%\tpf2mp\logs` by hand.
- Releases are built by GitHub Actions from the repository; the workflow file is `.github/workflows/build-msi.yml`.

Fixes, from a pull request by tearded
- A station edit whose parameters held a line break was lost on the wire.
- Buying at a modded depot whose model sits away from its origin was refused.
- The road ownership tool (public / private) now replicates; split roads and bridge halves keep their owner.
- Cancelled buy, sell, assign, send-to-depot and replace actions play their confirmation sound again; assigning a train to a line tries the line's stops in order instead of failing on stop 1.
- Snowball Fences (Workshop) drawing replicates: every segment of a drag, on every game.
- A bulldozed fence segment whose entity id the engine reused no longer ships an empty edit that asserted on every game.

Known
- A line created under an older version keeps the platforms it had; the stamp-time choice applies to stops added from now on.
- Two offline tests that are unrelated to play fail on this build (a boot-resilience check and a DLL byte check); tracked, not in the game.
