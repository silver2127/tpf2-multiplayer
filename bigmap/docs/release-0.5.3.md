The installer finds the game on any drive, and the pager policy from the September measurements ships

**This release overwrites existing `plugins/tpf2_bigmap.cfg`, including user edits, on installation, upgrade and repair.** Close the game before installing. It shares the proxy, the plugin host and the installer's custom actions with TpF2 Multiplayer 0.6.1.14 (vendored from that release); installing either package updates both.

- **The installer finds the game in any Steam library, on any drive.** The folder page was pre-filled from Steam's registration of the game or from the folder a previous install remembered, and otherwise showed the stock `C:\Program Files (x86)\Steam` path; a game in a second library meant browsing by hand. When neither holds `TransportFever2.exe`, the installer now reads Steam's own library list (`libraryfolders.vdf`) and takes the first library that has it. Shared with TpF2 Multiplayer.
- **Zoomed-in stutter on a big map** (`terrain pager 3195 <-> 256 MiB` flapping in the host log): the commit-pressure throttle is now sticky, holding until free commit clears the threshold by more than the pager itself gives back, and for at least 30 s. On a machine without a page file the pager's own release cleared the threshold, it re-expanded, and the flag set again, 74 times in one session.
- **The resident caps are the same size on every machine that can afford them**: `terrain_cache_max_mb=0` means a quarter of RAM clamped to 4096..8192 MiB (a 4 GiB cap sat under a fresh large map's working set and cost frames in decodes), `material_cache_max_mb=0` a quarter of that. `-1` removes a cap.
- **A working-set floor.** The level the stutter feedback drove the resident target to is kept and decays over minutes, instead of the target sawtoothing back to the hot budget every quiet second (1,000-2,000 decodes a second on a 32 GiB machine while the camera rested).
- `simulate_physical_mb` (rig only) makes the policy size itself for a smaller machine; the engine is not constrained.
- Releases are built on GitHub Actions and checksummed; the README describes the pagers as shipped.

The pager changes were measured on the developer's machine; if a 32 GB machine behaves differently, the `resident target` lines in `%LOCALAPPDATA%\tpf2mp\data\tpf2mp_host.log` say what the policy did.
