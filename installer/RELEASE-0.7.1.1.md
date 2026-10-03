## 0.7.1.1 - no more desync after joining a running game, dedicated servers with Workshop mods

Bugfix update based on 0.7.1, for Windows, Proton and the native Linux game. Everyone in a session, including the dedicated server, must update to 0.7.1.1.

### Changes

- **Joining a running game no longer desyncs it later.** A player who joined and was still catching up could lose track of how far behind the host they were, and the first thing their game did on its own (preparing a spare line for the line editor) was timed in the host's past. The two games then created that line at different times, and a town slowly grew differently on each: a desync many minutes after the join. Every command a joining game sends is now timed from the host's real clock, and the spare line waits until the game has caught up.
- **A dedicated server loads a save that needs a Workshop mod it has on disk.** A server whose Steam client runs offline refused such a save at every start ("the shared save could not start by itself"). Mods placed in the server's managed Workshop folder are now registered with the game at every start.

### Update

Use the launcher: **Update & play** (Windows and Linux), or install directly: the MSI on Windows, install_proton.sh under Proton, the native .run on Linux. A Linux dedicated server: `tpf2server stop`, `tpf2server install`, `tpf2server configure`, `tpf2server start`. All participants need the same version.

### Validation

The join desync was traced on the project's dedicated server: of 186 commands in the session, one, the joiner's automatic spare line, applied 35 game units apart on the two games. tools/actions_off_test.py now checks that a host heard a second ago still counts while the joiner runs at catch-up speed (it fails on 0.7.1); the Lua suites that load the command and line code pass. tools/registry_dedicated_test.py checks the dedicated server's Workshop registry at start, and the server loaded such a save after a restart.
