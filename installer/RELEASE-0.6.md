Frozen joins, separate companies with permissions and colours, and the road-vehicle drift closed

Every player in a session must run this version (the lobby version gate is exact) and the dedicated relay must run it too. Installs of 0.5.7 or newer are offered this update in game; older ones install the MSI by hand.

Joining
- A player joining a running game now freezes the session: the host saves, everyone (the host included) loads that save, the paused worlds are compared, and play resumes at the host's previous speed. Until now the host kept running and the newcomer caught up on the command history; that newcomer's world registered its entities in a different order from the host's, its simulated people split within about 35 game units and the buses on a line drifted at the next stop, on two identical replays, while a session whose members had loaded together stayed locked (2026-09-16). A join is a resync now; on the rig the joined session ran 830 game units with matching hashes, 0.0 m vehicle drift and no people gap.
- Nobody is released until everyone is in: a player who arrives while a round is running is admitted (into the pause directly, or as pending, in which case the same snapshot goes round once more with them before anyone plays); a player who leaves mid-round is dropped and the rest carry on. Only the host leaving ends a round.
- Resync reloads the host's world too, for the same reason.
- A resync's own snapshot load no longer makes a joiner leave the lobby.

Companies
- Separate companies: a lobby setting assigns each player a company (co-op stays the default). Left- or right-click a company chip in the lobby to change it; no switch, new company or dissolve while somebody is still loading in.
- Station permissions: a company decides which companies' vehicles may stop at its stations (the Multiplayer dashboard, companies tab). A line may use another company's stations when permitted.
- Companies have names (default: the founder's, with an ordinal); the dashboard shows your colour and name, renaming is the game's company window; the company -> player -> name map is published for other mods.
- Every player's stations, depots and vehicles show HUD icons, coloured with the owner's company colour: the vehicle icon, the station icon glyph (the blue box stays), the station name label. Another company's vehicle or station window is washed in that company's colour and is read-only.
- A 20-colour palette (distinct, easy to tell apart) shared by the lobby chips, the icons, the windows and the vehicle paint; company paint follows every vehicle buy.

Desync fixes
- Road vehicles: the free-space sum a bus checks before a junction is summed in an order-independent way, and each road edge's vehicle list is kept in name order on every peer, so an exact tie picks the same lead vehicle everywhere.
- A lost command is no longer recovered late: every command is sent three times, a gap holds the queue at once and asks for the missing piece immediately; a command that arrives more than once is queued once.
- The paused branch of the engine's step no longer advances a game-time counter per render batch (a paused host drifted from a paused joiner).
- The line editor gets its new line again (the replay's add is matched by call site); vehicle and line keys travel in the save, so a joiner adopts the host's.
- The world hash samples the sim at a sim time, treats any owned construction as a player construction, and applies the companies state before sampling (a false "town" desync seconds after the load gate). A flag file (tpf2mp_hash_every.txt) forces the hash cadence down to 4 game units for diagnosis.

Lobby and menu
- A host that loads another save mid-session pushes it to every client, who load it in place; the roster shows each joiner's world-load percentage and the host's transfer progress.
- Leaving the world leaves the lobby.
- The in-game updater's payload is the MSI itself; a governor kill switch (tpf2mp_governor_off.txt).

Proton and Linux
- install_proton.py installs this release into the Windows game under Steam Proton (finds Steam, the game and the prefix; verifies the build; repairs the lobby executable for Wine). The lobby executable in this release is shipped in its repaired form. TpF2Multiplayer-files.zip is the MSI's files as an archive for that script and for manual installs.

Tooling
- tools/auto_install.ps1 builds while the games run and installs the moment they close; the relay deploy installs zstandard.

Validation: offline, the full Python suite (sync barrier, runtime and lobby scenarios with two and three simulated engines including a frozen join and a leaver dropped mid-round, the Lua recovery test, the palette, station-icon, road-entries and other byte tests), luacheck and five native targets; on the two-instance rig the previous build reproduced the join drift twice, and this build's frozen join ran 830 game units clean.
