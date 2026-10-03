## 0.7.1.5 - the multiplayer panel on macOS (CrossOver), and a lighter dedicated-server lobby

Update based on 0.7.1.4, for Windows, Proton and the native Linux game. Everyone in a session, including the dedicated server, must update to 0.7.1.5.

### macOS (CrossOver)

- **The multiplayer panel now opens under CrossOver.** Clicking the Multiplayer button did nothing on a Mac: the panel draws through a hook in the game's Vulkan loader, and the loader Wine ships begins with an instruction the hook could not move ("vkGetDeviceProcAddr prologue not decodable -- overlay NOT hooked" in tpf2_menu.log). The hook now relocates it. Tested against that exact loader code; not yet seen in a game on a Mac, so logs from a Mac player are welcome.

### The lobby

- **A finished transfer no longer stays in the host's memory until the next one.** The last save and terrain file sent (about 1 GB on a big map) were kept until another player joined; on a dedicated server short of memory that was 1 GB the game could not use.

### Update

Use the launcher: **Update & play** (Windows and Linux), or install directly: the MSI on Windows, install_proton.sh under Proton, the native .run on Linux. A Linux dedicated server: `tpf2server stop`, `tpf2server install`, `tpf2server configure`, `tpf2server start`. All participants need the same version.

### Validation

tools/test_hook_riprel.cpp decodes the Mac player's exact vkGetDeviceProcAddr prologue (a 17-byte cut with one RIP-relative fix-up; the old decoder refused it) and hooks a live function with that prologue, which still reads the right memory through the relocated instruction. Every other hook keeps the unchanged path. The lobby, transfer and live-join suites pass.
