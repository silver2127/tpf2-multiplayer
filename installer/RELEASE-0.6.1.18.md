**Experimental.** Pre-release on the Steam-networking line, with two contributions merged. The dedicated server stays on 0.6.1.14 until this line is promoted, and the version gate is exact: everyone in a session needs the same version, so players on this build can host and join each other, not the dedicated server.

How to install

Windows (Steam, game build 35924): download `TpF2Multiplayer.msi` above, close the game and run it. It installs into the game folder and the mod into the game's mods, over any older version. Start the game: Main menu → Multiplayer.

Linux and Steam Deck (the Windows game under Proton): set the game to run with Proton 9 or newer, start it once and quit. Download `install_proton.sh` above, close the game and run `sh install_proton.sh`. It finds Steam, the game and the Proton prefix, downloads `TpF2Multiplayer-files.zip` from this release, checks it against `SHA256SUMS.txt` and installs; `--dry-run` shows the plan first. (`install_proton.py` does the same with Python 3.9 or newer.) Full guide: docs/proton/INSTALL.md.

Manual install (any platform): `TpF2Multiplayer-files.zip` holds the files in the game-folder layout.

What changed
- **SELECT SAVE in the lobby** (tearded, pull request #9). Starting a lobby from the title screen used to share the newest save automatically. The host can now pick the world explicitly before START GAME: a list of saves and autosaves, newest first with dates, eight per page. The chosen name stays visible in the lobby, starting without a choice opens the picker, and a save deleted after being chosen reports an error instead of sharing another world. Hosting from a world already loaded works as before.
- **The installer accepts the GOG build when asked** (Kalemillion, pull request #8, in 0.6.1.17 but not in its notes). The game-folder check knows the GOG executable and its own `alut.dll`; `TPF2_ALLOW_GOG=1` on the `msiexec` command line lets it through. The multiplayer DLLs still hook only the Steam build, so this is for the installer's sake, not a GOG multiplayer.
- From 0.6.1.17: **faster transfers through Steam**, if the Steam client honours the raised send-rate cap (`[steam] SendRateMax: ... (set)` in `tpf2_bridge.log`).
- From 0.6.1.15: **Steam's networking carries the connection** when the direct paths fail: no port forwarding, no Hamachi, no dependence on the mod's relay. `tpf2mp_steam_off.txt` in `%LOCALAPPDATA%\tpf2mp\data` turns it off.
