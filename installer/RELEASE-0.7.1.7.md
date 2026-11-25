## 0.7.1.7 - a version mismatch names the versions players installed

Update for Windows and for the Windows game under Proton. **There is no native Linux package this time:** players on the native Linux game stay on 0.7.1.6, and so can a dedicated server. Nothing that crosses the network or touches the world changed, so 0.7.1.7 still speaks lobby version 0.7.1.5 and plays with 0.7.1.6 in either direction. The Linux launcher shows this version as having no native package; keep 0.7.1.6 there, or switch the game to Proton.

### The lobby

- **A failed join now names the multiplayer versions both players installed.** 0.7.1.6 kept lobby version 0.7.1.5, and the version-mismatch message printed that number for your own side, so a 0.7.1.6 player read "you have 0.7.1.5" and took the message for the bug. The other number, the version of whoever was out of date, was the one that mattered. The message now reads like "you have 0.7.1.7; the host has 0.7.1.4". If you see a mismatch, update whoever has the older version shown there.
- **The same holds when joining an older host.** An older host's reject named the joiner's lobby version; the joining side now shows its own version instead. The join, the welcome and the roster carry the release next to the lobby version, so later versions name a 0.7.1.7 player correctly too. A 0.7.1.6 player is named 0.7.1.6.

### Update

Use the launcher: **Update & play** on Windows, or under Proton with the Linux launcher. Or install directly: the MSI on Windows, install_proton.sh under Proton. Native Linux players and servers keep 0.7.1.6; nothing needs to change there.

### Validation

tools/version_gate_test.py checks the wording on both sides of a mismatch, an older host's reject, the release field and that this package still admits 0.7.1.5 and 0.7.1.6 peers. The lobby selftests and the host, mode, limits, live-join, world-switch, Steam and transfer suites pass. Two clean 0.7.1.6 installs on one PC hosted and joined each other through the real menus before this release.
