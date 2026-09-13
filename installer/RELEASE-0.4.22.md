Shared build previews and experimental mod compatibility

- Includes merged PR #3: shared road and railway previews, including the native 3D renderer, sender error colors, and preview lifecycle/performance improvements.
- Adds experimental previews for new stations, depots and other construction buildings, carrying placement, rotation and options. Existing station module edits and replacements are not supported. When native rendering is unavailable or rejects a proposal, a placement marker is shown. Oversized/unsupported proposals are omitted.
- Adds multiplayer-side compatibility for Natural Town Growth: saved deterministic Lua RNG, simulation-based timers, sorted iteration, and migration of legacy cooldown timestamps. Workshop files are unchanged. This currently targets Natural Town Growth only and also applies in solo worlds with the multiplayer mod enabled.
- Packages the preview DLL in the MSI so users receive the renderer automatically.
- Retains the 0.4.19 bridge, crossing and command-replay fixes while incorporating the merged preview PR. Unfinished command-barrier work is not included.

Close the game before installing, update all peers, and restart both instances. Keep a pre-update save when trying Natural Town Growth: the first load resets its cooldown progress while retaining saved capacities and growth factors; rollback should use the pre-update save.

Validation: 104 Lua preview checks, preview performance/lifecycle regressions, Natural Town Growth offline regressions, bridge/crossing regressions, native preview contract tests, Lua syntax checks, and MSI administrative extraction/payload verification. New construction 3D rendering and the Natural Town Growth fix have not yet completed live two-instance validation. The earlier restart attempt also did not leave the second instance running; this release does not claim that issue is resolved.
