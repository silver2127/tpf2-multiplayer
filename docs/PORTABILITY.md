# Portability status

Turning the dev rig (hardwired to the dev machine) into something a friend can
install and run. Principle: **machine-specifics are discovered at runtime; our
own files are found relative to themselves; never drive the game's native UI by
pixel.** Everything below marked DONE keeps a fallback to the old dev path, so
the local rig still works unchanged.

## Done + verified (this pass, 2026-08-29)

- **Menu DLL path discovery** (`menu_hook.cpp`): `steamPath()` reads
  `HKCU\Software\Valve\Steam\SteamPath` (→ `HKLM\...\WOW6432Node` fallback);
  `resolveSaveDir()` enumerates `<steam>\userdata\*\1066780\local\save`;
  `resolveNetDir()` prefers `%LOCALAPPDATA%\tpf2mp\netpunch`. `SAVE_DIR`/`NETDIR`
  are repointed at these in `Init()`. Verified: discovery returns this machine's
  real save dir (case-insensitive match to the old hardcode). Log line at Init
  prints `save=… net=… our=…`.
- **Log path** is now `ourDirA()` (next to the DLL), not a hardcoded out dir.
- **Frozen Python** — `netpunch.exe` (8.6 MB, PyInstaller onefile from
  `lobby.py`, bundling punch/connect/observe + pystun3 + miniupnpc). Built to
  `netpunch\dist\netpunch.exe`. **Both selftests PASS on the frozen exe**
  (`--selftest` roster+chat; `--selftest-transfer` all 4 clean+lossy cases,
  byte-identical). Removes the Python-on-PATH requirement for the friend.
- **Menu prefers `netpunch.exe`** in `NETDIR` if present, else `python lobby.py`
  (dev fallback). Local rig has no exe in the repo netpunch dir → still uses
  Python, unchanged.
- **Proxy source portable** (`proxy_alut.cpp`): `resolveShipped()` tries
  `%LOCALAPPDATA%\tpf2mp\<dll>`, then next to the proxy, then the old workshop
  path. Rebuilt (`out\alut.dll`) but **NOT redeployed** — the installed proxy is
  untouched; it behaves identically anyway (falls back to workshop).
- **`install_portable.ps1`** — discovers Steam/game/save paths (verified) and
  stages the netpunch layer into `%LOCALAPPDATA%\tpf2mp\netpunch`. Not run yet
  (would flip the local rig onto the portable path — do it with eyes on).
- **Removed dead code**: `LaunchSession`/`LAUNCH_CMD` (hardcoded repo path, no
  callers).

## Remaining — needs the live game or a coupled change (do together, awake)

Priority order (from the full audit). Each is a real ship blocker.

1. **Continue-click → programmatic load** (`menu_hook.cpp clickContinueLoad`).
   The `{3856x2128→349,1110}, …` coord table is the dev machine's resolutions; on any other
   monitor/UI-scale >5% off, the shared save never auto-loads. Fix: RE the
   title-menu "continue"/load-most-recent action (the menu is action-key driven;
   the handler cluster is already mapped in `docs/re/MENU_UI.md`) and call it,
   instead of a synthesized mouse click. **Biggest fragility in shipping code.**
   Needs the live game + Ghidra.

2. **Bridge/slice DLL ↔ Lua `BASE` coupling** (HARD BLOCKER — silent
   no-replication). The bridge/slice DLLs self-locate their I/O dir, but
   `mpbridge.lua:6`, `lockstep.lua:28`, `mptest.lua:18` read/write a HARDCODED
   `BASE = …\3710243057\recon\m4\out\`. On another machine (or if we move the
   DLLs to `%LOCALAPPDATA%`) the two halves point at different dirs and never
   exchange events. Fix: the bridge writes its resolved out-dir to a fixed
   Lua-readable file (e.g. `%LOCALAPPDATA%\tpf2mp\outdir.txt`); the Lua reads
   that, falling back to the hardcoded `BASE`. Safe with the fallback, but must
   be verified with a lockstep replication run — hence not done overnight.
   **This is why the DLLs were NOT moved to LOCALAPPDATA this pass.**

3. **`slice_hook.cpp:114` self-locate** — replace hardcoded `OUT_DIR` with the
   `GetModuleFileNameW`+strip-to-dir pattern `bridge_main.cpp` already uses. Do
   it together with #2 (same coupling).

4. **Proxy redeploy** — deploy the rebuilt portable `out\alut.dll` via
   `install_proxy.ps1`, launch, and confirm `tpf2_menu.log` shows `attached` +
   the paths line (proxy loaded the DLLs). Behaviorally identical (workshop
   fallback), but verify the menu still appears.

5. **`savexfer.cpp:74` Steam root** — derive `userdata` root from registry
   `SteamPath` instead of two guessed `Program Files` prefixes (misses Steam on
   another drive).

6. **`bridge_main.cpp:47` peerIp default `127.0.0.1`** — two-machine play needs
   the lobby's discovered peer address fed into the bridge cfg (or the bridge
   reads it from the lobby). Wiring gap, not a literal.

## Packaging (last)

Once 1–6 land: one installer that discovers the game dir, stages DLLs + netpunch
+ frozen exe into `%LOCALAPPDATA%\tpf2mp`, installs the proxy (or ships a
launcher-injector that starts TF2 and `LoadLibrary`s the DLL, avoiding any
game-file modification), and installs the Lua mods. Distribute as a GitHub
release zip; the Workshop can host the Lua part but not the injector.

## Already portable (no action)

Bridge log path + save-dir enumeration (`bridge_main.cpp`, `savexfer.cpp` — self
-locating / userdata\*), username via `GetUserNameW`, and the **entire netpunch
STUN/UPnP/IPv6 discovery stack** (no baked IP/prefix — the hardest part, done).
