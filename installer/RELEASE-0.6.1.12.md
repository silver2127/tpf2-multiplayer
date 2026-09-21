Turned wagons stay turned, and a lobby that scanners leave alone

Everyone in a session needs this version, and the dedicated relay runs it.

How to install

Windows (Steam, game build 35924): download `TpF2Multiplayer.msi` above, close the game and run it. It installs into the game folder and the mod into the game's mods, over any older version. Start the game: Main menu → Multiplayer.

Linux and Steam Deck (the Windows game under Proton): set the game to run with Proton 9 or newer, start it once and quit. Download `install_proton.sh` above, close the game and run `sh install_proton.sh`. It finds Steam, the game and the Proton prefix, downloads `TpF2Multiplayer-files.zip` from this release, checks it against `SHA256SUMS.txt` and installs; `--dry-run` shows the plan first. (`install_proton.py` does the same with Python 3.9 or newer.) Full guide: docs/proton/INSTALL.md.

Manual install (any platform): `TpF2Multiplayer-files.zip` holds the files in the game-folder layout.

What changed
- Turned wagons stay turned. A train bought with a reversed car (an ICE's tail head, a cab car, any wagon flipped in the depot) came out with that car facing forward on every game, the buyer's included, because the purchase never carried the part's reversed flag. It does now, for purchases and for vehicle replacements. Vehicles bought before this update keep whatever orientation they got; replace them once to fix them. Ported from tearded's fork.
- The lobby program is a plain folder (`netpunch.exe` beside `netpunch\_internal\`) built with a bootloader compiled on the building machine, instead of a one-file exe that unpacked itself at run time. Several virus engines called the self-extracting form a trojan; a program beside its libraries is what their rules treat as normal.
- The release page opens with a Download table saying which file is for which platform, and the Proton installers attached to a release are pinned to that release's version.

Docs
- README rewritten for the CI-built, checksummed releases, the dedicated server and the privacy position; KNOWN_ISSUES.md rewritten for what 0.6.1.11 changed, with the not-replicated list caught up.
