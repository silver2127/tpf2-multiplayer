New Line no longer crashes the game, and the Linux side gets faster

Everyone in a session needs this version, and the dedicated test server runs it.

How to install

Windows (Steam, game build 35924): download `TpF2Multiplayer.msi` above, close the game and run it. It installs into the game folder and the mod into the game's mods, over any older version. Start the game: Main menu → Multiplayer.

Linux and Steam Deck (the Windows game under Proton): set the game to run with Proton 9 or newer, start it once and quit. Download `install_proton.sh` above, close the game and run `sh install_proton.sh`. It finds Steam, the game and the Proton prefix, downloads `TpF2Multiplayer-files.zip` from this release, checks it against `SHA256SUMS.txt` and installs; `--dry-run` shows the plan first. (`install_proton.py` does the same with Python 3.9 or newer.) Full guide: docs/proton/INSTALL.md.

Manual install (any platform): `TpF2Multiplayer-files.zip` holds the files in the game-folder layout.

Fixes
- **New Line from a vehicle crashed the game.** Pressing New Line in a vehicle's line picker killed the game instantly, every time, for anyone in a session. The mod cancels the game's create-line command so that every player creates the line at the same moment, and it left the cancelled command without a result for the game to hold on to; that window reads it without checking. It now hands back an empty result the game can carry and release. Present since 0.6.1.9. Reported with crash logs, two crashes in four minutes.
- **A pause that waited for a player could give up too early.** Joining a game whose host was still loading was refused after 45 seconds with "Timed out waiting for all players", and the failed round could leave the session paused. The pause and the check that follows a reload now wait as long as they need; a player who leaves is dropped and the rest carry on.
- **A failed recovery no longer survives a restart.** Its record is cleared when the game starts, so a game that was left paused comes back running.
- **The dedicated server asked for its world twice.** It re-requested the load every minute while loading, so it loaded the same world twice: about seven minutes to be playable instead of four. One request per load now.

Linux and Steam Deck
- **Faster simulation under Proton.** Wine's memory allocator searched a long list for every small block the simulation asked for, which on the dedicated server was a quarter of the whole game's processor time and held sessions at 1x. The mod now switches those size classes onto Wine's fast path at start-up, in about a tenth of a second. Windows is unaffected. `TPF2MP_WINE_HEAP=0` turns it off.
- The Proton installers now say whether the copy they installed carries that change.

Also
- **The Multiplayer window works on the OpenGL renderer.** Transport Fever 2 can render with Vulkan or OpenGL; the window only ever drew on Vulkan, so on OpenGL the MULTIPLAYER button opened nothing and the mod's per-frame work never ran. Both renderers now draw the same window. Reported by a player whose son's game was on OpenGL. New in this release and not yet confirmed in a game on OpenGL: `tpf2_menu.log` says `OpenGL: hooked SDL2's SwapBuffers import` when it is in use.
- The Steam networking transport of 0.6.1.15 to 0.6.1.18 is in this release as a normal feature.
