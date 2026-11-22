## 0.7.0.4 - hot join letters, bigger server list, better bug-report logs

Stable update based on 0.7.0.3, for Windows, Proton and the native Linux game. Everyone in a session, including the dedicated server, must update to 0.7.0.4.

### Changes

- **Hot join no longer shuffles players.** Each player's letter (the id every game uses for their commands) used to be their position in the sorted player list. A player joining a running game whose name sorted earlier moved an existing player to another letter, and the worlds desynced (people first, then vehicles). The host now fixes each player's letter when they join and keeps it for the session.
- **The server list shows 12 games to a page.** The Join page is taller, the list keeps up to 48 public games, and the heading shows how many there are.
- **The resync view has a close button.** On a desync notice it closes the notice. During a running resync it hides the view and shows the game; the resync goes on, and Manage lobby brings the view back.
- **OPEN LOGS gathers what a bug report needs.** The folder's about.txt starts with the installed version, the system, the time zone of the log times and the build of every library. It also holds the state files beside the logs (the bridge control file, the lockstep streams, resync state) and the lobby's message stream. Invitation codes are masked with `*`, so the folder can be shared safely. Five archives of each kind are kept instead of two.
- **OPEN LOGS works under Proton.** It finds the game's log and crash dumps in the real Steam folder, opens the folder in the desktop's file manager and shows its Linux path.
- **Native Linux:** the installer preloads the loader from its own folder, so the Steam Runtime container no longer makes the mod's data folder read-only (no logs, "Multiplayer command hooks are not ready"). It also names a Proton install instead of saying the game was not found. The title screen no longer freezes on MULTIPLAYER on NVIDIA drivers that give the overlay uncached video memory.

### Update

Windows: close the game and use **Official / Stable** in the existing launcher, then **Update and play**, or run the MSI. Linux / Steam Deck under Proton: run install_proton.sh. Native Linux: close the game and run the `.run` installer. All participants need the same version.

### Known limitations

- The native Linux terrain pager can still thrash on a running big map.
- Routers and firewalls can still block TCP save transfers; see the 0.7.0.2 notes.

### Validation

Menu tests at five UI scales (server list paging, resync view, no overlapping controls), the OPEN LOGS archive test on Windows and under Proton 11.0's Wine, the lobby self-test with a late joiner who sorts first, TCP connectivity, transfer status and Steam Messages tests. The Linux libraries are built and tested (CTest) in the pinned Steam Runtime soldier SDK, and the Linux release check confirms that the bundled Lua matches this Windows release exactly.
