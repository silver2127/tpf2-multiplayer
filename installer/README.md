# TpF2 Multiplayer installer

A per-machine Windows Installer package (`TpF2Multiplayer.msi`, x64) that puts
the lockstep multiplayer mod into an existing Transport Fever 2 installation.
It is built with the WiX Toolset from `Package.wxs` by `build_msi.ps1`.

Status: first cut. The package builds, extracts cleanly and its tables have been
checked; it has not yet been through an install/uninstall cycle on a clean
machine. Read "What it changes in the game folder" before running it.

## Requirements

- 64-bit Windows 10 or 11, Steam, Transport Fever 2 (build 35924 - the hook
  addresses are specific to it).
- Administrator rights: the package writes into the game folder under
  `Program Files (x86)` and to `HKLM`.
- The game must not be running while installing, upgrading or uninstalling
  (the installer swaps `alut.dll`, which the game keeps open).

## What the MSI installs, and where

Everything the package writes lands in the **game folder** (`INSTALLFOLDER`).
The installer finds it from Steam's own registration
(`HKLM\SOFTWARE\WOW6432Node\Microsoft\Windows\CurrentVersion\Uninstall\Steam App 1066780`,
value `InstallLocation`), falls back to the folder a previous install
remembered, then to `<Program Files (x86)>\Steam\steamapps\common\Transport Fever 2`.
You can pick any other folder in the wizard, but it has to contain
`TransportFever2.exe`; the folder page refuses to continue otherwise, and a
silent install fails with the same message.

| Path (relative to the game folder) | What it is |
|---|---|
| `alut.dll` | Our proxy. Every `alut*` export is forwarded to `alut_real.dll`; on load it pulls in the two DLLs below, before the game's own entry point runs. |
| `alut_real.dll` | The game's original `alut.dll`, renamed by the installer (not a packaged file). |
| `tpf2_bridge_mp.dll` | Command replication bridge (UDP peer link, save transfer, sim-step hook). |
| `tpf2_menu.dll` | The in-game MULTIPLAYER menu and lobby panel. |
| `tpf2_slice.dll` | Captures a player's own build/vehicle commands and hands them to lockstep. |
| `tpf2_bridge_mp.cfg`, `tpf2_slice.cfg` | Settings for the two DLLs, commented. Sources: `installer\cfg\`. |
| `netpunch\netpunch.exe` | The frozen lobby (PyInstaller build of `netpunch\lobby.py`): NAT traversal, roster, chat, save transfer. |
| `mods\mp_lockstep_1\**` | The Lua game-script mod, the whole `mod\mp_lockstep_1` tree. |

The package also writes `HKLM\SOFTWARE\silver2127\TpF2 Multiplayer`
(`InstallFolder`, `Version`) so upgrades and uninstalls find the same folder,
and the usual Add/Remove Programs entry (`TpF2 Multiplayer`, publisher
`silver2127`).

### Runtime data lives elsewhere

Nothing is written to the game folder at run time. Identity, event and capture
files, injects, status files, company files and every `.log` go to the
**data folder** `%LOCALAPPDATA%\tpf2mp\data\` (the DLLs create it; the
environment variable `TPF2MP_DATADIR` overrides it, which is how the developer
harness pins its own folder). The installer never touches the data folder, so
uninstalling leaves your logs and captures in place - delete
`%LOCALAPPDATA%\tpf2mp` yourself if you want them gone.

### Configuration lookup

Each DLL reads its `.cfg` from its own folder first (the game folder, where the
installer puts it), then from the data folder, then falls back to built-in
defaults; the first file found wins. To change a setting, edit the copy in the
game folder (needs administrator rights) or delete it there and keep your own
copy in `%LOCALAPPDATA%\tpf2mp\data\`. `tpf2_slice.cfg` is re-read on every
event, so its switches can be flipped while the game runs.

Shipped defaults: bridge on UDP 7771 talking to a peer on 127.0.0.1:7772,
`instance=auto`, save transfer on TCP 7871, `sim_hook=1`, `buy_hook=1`; slice
`enabled=1 suppress=1 merge=1 cancel_vehicle=1` (full lockstep - set
`suppress=0` for observe-only, `enabled=0` to switch the hook off entirely).

## What it changes in the game folder

The only game file the installer modifies is `alut.dll`:

1. Before copying files, if `alut_real.dll` does not exist yet, the game's
   `alut.dll` is renamed to `alut_real.dll`. (If `alut_real.dll` is already
   there - an earlier install, or the developer script `tools\install_proxy.ps1` -
   whatever `alut.dll` is on disk is treated as an old proxy and replaced.)
2. Our proxy is installed as `alut.dll`.
3. If the install fails after step 1, a copy of `alut_real.dll` is put back as
   `alut.dll` during rollback, so the game keeps working.

Uninstalling removes the proxy and renames `alut_real.dll` back to `alut.dll`.
A major upgrade (installing a newer MSI over an older one) leaves
`alut_real.dll` in place and only replaces the proxy.

Steam's **Verify integrity of game files** also restores the stock `alut.dll`
(it overwrites our proxy, and leaves `alut_real.dll` behind as an unused extra
file). After a verify, the multiplayer menu is simply gone; run **Repair** from
Add/Remove Programs, or reinstall, to put the proxy back.

Everything else the package adds is a new file; the game does not care about
extra DLLs, a `netpunch` folder or an extra entry under `mods`.

## After installing: enable the mod per savegame

Transport Fever 2 activates mods per savegame, not globally. For every save you
want to play in multiplayer, open its mod list in the game (the **Mods** panel
of the load/new-game screen) and enable **MP Lockstep**. All players need the
same mod set on the shared save; the host's save is what gets sent to the
joiners. Never enable **MP Bridge** (the older state-replication mod) at the
same time - the two fight over the same world.

The title menu gains a **MULTIPLAYER** entry. The host presses HOST, sends the
code that lands on the clipboard to the others, they press JOIN; the host then
presses START GAME to ship the save.

## Firewall and ports

- **Lobby: UDP 29471 inbound on the host** (`netpunch.exe host`). This is the
  one port that has to be reachable from the internet. The lobby tries to open
  it through UPnP and uses STUN to learn the public address; if neither works,
  forward UDP 29471 to the host machine on the router. Joiners bind an
  ephemeral port and only dial out, so they need no inbound rule.
- **Bridge: UDP 7771/7772 and TCP 7871**, per `tpf2_bridge_mp.cfg`. With the
  shipped `peer_ip=127.0.0.1` these never leave the machine; a remote peer
  address has to be put in the cfg by hand for now (the bridge is not yet fed
  the address the lobby discovered).

Windows Defender Firewall asks about `netpunch.exe` and `TransportFever2.exe`
the first time they listen; allow them on the network profile you use. The MSI
adds no firewall rules of its own.

## Uninstall / revert

- **Add/Remove Programs -> TpF2 Multiplayer -> Uninstall**, or
  `msiexec /x TpF2Multiplayer.msi` (same MSI file or the product from the ARP
  list). This removes every packaged file, the `netpunch` and
  `mods\mp_lockstep_1` folders it created, the registry key, and restores the
  stock `alut.dll` from `alut_real.dll`.
- **Steam -> Verify integrity of game files** restores the stock `alut.dll`
  without uninstalling anything (see above).
- **By hand**, if all else fails: delete `alut.dll`, rename `alut_real.dll` to
  `alut.dll`. The other files are inert without the proxy.
- The data folder `%LOCALAPPDATA%\tpf2mp` is never removed automatically.

## Silent install and upgrades

```
msiexec /i TpF2Multiplayer.msi /qn /l*v install.log
msiexec /i TpF2Multiplayer.msi /qn INSTALLFOLDER="D:\SteamLibrary\steamapps\common\Transport Fever 2"
```

`INSTALLFOLDER` must contain `TransportFever2.exe`; `TPF2_SKIP_GAMEDIR_CHECK=1`
bypasses that check for test rigs only. A newer MSI upgrades an older install in
place (major upgrade, same `UpgradeCode`); a rebuilt package of the same version
number also replaces the installed one. Downgrades are refused.

## Building the MSI

Prerequisites, all on `PATH` or in their default places:

- Visual Studio 2022 Build Tools with the MSVC x64 toolchain (the `.bat`
  scripts call `vcvars64.bat` from the Build Tools install).
- Python 3.12 with `pip install pyinstaller -r netpunch\requirements.txt`.
- WiX Toolset v7 as a .NET global tool: `dotnet tool install --global wix`,
  plus the UI extension: `wix extension add -g WixToolset.UI.wixext`
  (`build_msi.ps1` adds it when missing).
- WiX v7 requires accepting its Open Source Maintenance Fee EULA
  (https://wixtoolset.org/osmf/) - free for individuals and open-source
  projects, a paid fee for commercial organisations. Accept it once with
  `wix eula accept wix7`, or per run with `-AcceptWixEula` below. The script
  does not accept it on your behalf.

```
pwsh installer\build_msi.ps1                       # build DLLs, freeze netpunch, build the CA DLL, wix build
pwsh installer\build_msi.ps1 -SkipFreeze           # reuse netpunch\dist\netpunch.exe (warns)
pwsh installer\build_msi.ps1 -SkipBuild -Validate  # only wix build, then msiexec /a into a temp folder and list the tree
pwsh installer\build_msi.ps1 -Version 0.2.0 -AcceptWixEula
```

Output: `installer\out\TpF2Multiplayer.msi` (plus `tpf2ca.dll`, the custom
action DLL from `installer\ca\`, and a `.wixpdb`). `installer\out\` is ignored
by git.

If the game is running while you build, the DLLs it has loaded are locked and
`link` fails with LNK1104. `build_menu.bat` and `build_slice.bat` accept a name
suffix, and the script retries with one and packages the suffixed file under
the plain name; `build_proxy.bat` does not, so close the game for that step.

### Files in this directory

| File | Purpose |
|---|---|
| `Package.wxs` | The package: folders, components, custom actions, the `WixUI_InstallDir` copy with the game-folder check. |
| `build_msi.ps1` | The build script described above. |
| `ca\tpf2ca.cpp`, `ca\build_ca.bat` | Custom actions: game-folder check, `alut.dll` preserve / rollback / restore. |
| `cfg\tpf2_bridge_mp.cfg`, `cfg\tpf2_slice.cfg` | The shipped configuration files. |
| `License.rtf` | MIT license text shown by the wizard. |
