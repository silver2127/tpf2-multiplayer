Experimental: no content limits, train priority by name, companies with names, hot-join progress

This is an EXPERIMENTAL build. It is published as a GitHub pre-release, so the in-game updater does not offer it; install the MSI by hand. Every player in a session must run it (the lobby version gate is exact) and the dedicated relay must run this version.

Desync fixes
- Trains at a junction: the engine decided which train reserves track first by shuffling its internal train list with a generator seeded from game time. That list order is the engine's registration order, which two identical worlds can disagree on, so two games could pick opposite winners with matching hashes (2026-09-16). Trains now reserve in alphabetical order of their name, plus a bounded seeded jitter so a late-alphabet train still gets through; the order is the same on every game. A hash lane over train names reports a name mismatch as a desync. Kill switch: trainorder=0 in tpf2_menu_flags.txt.
- A cloned vehicle's line assignment was dropped on the host when its key had not bound yet (peers retried, the host did not): seven trucks stayed parked on one side.
- Every hard-coded content limit found by an audit is gone: vehicle configs of any length (was 64 parts), bulk sells (was 256), bulldozes (was 16), street/track types (was 512), line and vehicle names (was 256 bytes), edge objects, construction parameters of any size and depth, asset brush strokes, road proposals of any size, the lobby roster line, the Workshop registry (was 128 mods), the mod-list parser (was 128 mods, ASCII ids only), command history and line-edit history rings, the load gate, resync timeouts (progress-based now), oversized lobby messages (fragmented). Nothing is truncated silently any more: what cannot ship is refused loudly.
- Resync: only the clients load the snapshot; the host keeps the world it took it from.

Multiplayer features
- A host that loads another save (or starts a new game) mid-session pushes it to every client, who load it in place.
- Hot-join progress beside each name in the roster: receiving save N%, loading world, catching up (N s behind).
- A save shared within 15 s of unpaused play serves the next hot joiner instead of a new autosave.
- Companies have names (whoever plays one names it; shown in the finances window); the picker is a dropdown of "players -- company", alphabetical.
- The player name follows the Steam persona name unless typed; names up to 64 characters.
- Clicking a filled join-code field clears it; the in-game dashboard's x closes it.
- The host slows the session gradually while the slowest player falls behind, and raises it again when everyone keeps up.
- A keep-logs flag file stops the mods deleting old logs.

Tooling
- tools/auto_install.ps1 builds while the games run and installs the moment they close.
- Train path changes and halts are logged with their sim step, so two games' logs diff to the step.

Validation: offline, the full Python suite (49 tests), luacheck, five native targets; on the two-instance rig the previous build reproduced the clone drop and the train junction split; this build is deployed there.
