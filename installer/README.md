# Installer

`TpF2Multiplayer.msi` is a per-machine, x64 Windows Installer package that adds the multiplayer
mod to an existing Transport Fever 2 installation. It is built with WiX Toolset v7 from
`Package.wxs` and the shared fragment `PluginHost.wxs` by `build_msi.ps1`.

## Requirements

- 64-bit Windows 10 or 11, the Steam version of Transport Fever 2 (build 35924).
- Administrator rights: the package writes into the game folder and to `HKLM`.
- The game closed: the installer replaces `alut.dll`, which the running game holds open.

## Where it installs

The game folder is found from Steam's own uninstall entry
(`HKLM\SOFTWARE\WOW6432Node\Microsoft\Windows\CurrentVersion\Uninstall\Steam App 1066780`,
`InstallLocation`) and from the folder a previous install remembered
(`HKLM\SOFTWARE\silver2127\TpF2 Multiplayer`, `InstallFolder`). When both exist the remembered folder
wins. Without either, the default is `C:\Program Files (x86)\Steam\steamapps\common\Transport Fever 2`.
The folder page refuses a folder without `TransportFever2.exe`, and a silent install fails with the
same message.

| path in the game folder | what it is |
|---|---|
| `alut.dll` | The proxy. The game imports `alut.dll` statically, so it loads before the game's entry point. It forwards every export to `alut_real.dll` and loads the DLLs below, looking in `%LOCALAPPDATA%\tpf2mp\` before its own folder. |
| `alut_real.dll` | The game's original `alut.dll`, moved aside by the installer (not a packaged file). |
| `tpf2_pluginhost.dll` | The plugin host shared with [TpF2 Big Maps](https://github.com/silver2127/tpf2-bigmap). |
| `tpf2_bridge_mp.dll` | Instance identity, the loopback link to the lobby, and the sim-loop and game-speed hooks. |
| `tpf2_slice.dll` | Captures and cancels the player's commands. |
| `tpf2_menu.dll` | The Multiplayer panel on the title menu and the lobby launcher. |
| `tpf2_slice.cfg` | Optional diagnostic settings, all commented out; multiplayer needs none. See [docs/CONFIGURATION.md](../docs/CONFIGURATION.md). |
| `netpunch\netpunch.exe` | The lobby. |
| `mods\mp_lockstep_1\` | The game-script mod. |

It also writes `HKLM\SOFTWARE\silver2127\TpF2 Multiplayer` (`InstallFolder`, `Version`), the
[Segment Heap](#segment-heap) value, and the Apps entry **TpF2 Multiplayer** (publisher `silver2127`;
Repair and Remove, no Modify). It adds no firewall rules, shortcuts or environment variables, and it
does not install `tpf2mp.cfg` (the plugin host's optional settings file; the repository copy is a
reference).

At run time the game side writes `tpf2_menu.log` next to `tpf2_menu.dll` (where it also reads
`tpf2_menu_flags.txt`, if you create one), the lobby's files in `netpunch\`, and, with `dump_egeo=1`,
`egeo_*.txt`. Everything else goes to `%LOCALAPPDATA%\tpf2mp\data\`, which the installer never touches.

## How `alut.dll` is swapped

- **Install** (`PreserveStockAlut`, before the files are copied): if `alut_real.dll` does not exist,
  the game's `alut.dll` is renamed to `alut_real.dll`; the install fails if that is impossible. If
  `alut_real.dll` already exists (an earlier install), the `alut.dll` on disk is treated as an old
  proxy and deleted, unless another product still owns the proxy.
- **Rollback** (`RollbackStockAlut`): a failed install copies `alut_real.dll` back to `alut.dll` when
  no other product owns the proxy.
- **Uninstall** (`RestoreStockAlut`, after the files are removed): `alut_real.dll` is moved back over
  `alut.dll`, unless another product still owns the proxy. It is skipped during an upgrade's removal
  step, so an upgrade keeps `alut_real.dll` and only replaces the proxy.
- **Steam's "Verify integrity of game files"** puts the stock `alut.dll` back (and leaves
  `alut_real.dll` as an unused file). The Multiplayer entry disappears; run the MSI again and choose
  **Repair** to restore the proxy.

"Another product owns the proxy" is answered by `MsiEnumClients` on the proxy's component, machine
wide (`ca\tpf2ca.cpp`, `OtherProxyClients`).

## Coexistence with TpF2 Big Maps

Big Maps is a separate package that loads through the same plugin host. The two install and uninstall
in either order:

- **Shared files under fixed component GUIDs.** `alut.dll`, `tpf2_pluginhost.dll` and the Segment Heap
  value are declared in `PluginHost.wxs`, which is byte-identical in both repositories. Windows
  Installer reference-counts a component by GUID across products: the second install registers as an
  additional owner, and the last one out removes the files.
- **Owner-aware custom actions.** Because MSI's reference count cannot know that a custom action also
  moves `alut.dll`, all three `alut` actions ask `MsiEnumClients` first.
- **Separate settings.** A plugin installed by another package keeps its settings in
  `plugins\<name>.cfg` beside its DLL, so neither installer edits a file the other owns.

The Big Maps repository has `installer\test_coexist.ps1` (real `msiexec` transactions for both orders
against a throwaway folder) and `tools\vendor_host.ps1` (copies the shared binaries from this repository
and records the commit). After changing `PluginHost.wxs`, the proxy, the plugin host or the custom
actions here, re-vendor there.

## Segment Heap

The installer sets one registry value that switches Transport Fever 2 to the Windows Segment Heap:

```
HKLM\SOFTWARE\Microsoft\Windows NT\CurrentVersion\Image File Execution Options\TransportFever2.exe
    FrontEndHeapDebugOptions  (DWORD)  0x08
```

It is the one thing the package changes about how Windows runs the game rather than adding files beside
it.

**Why.** On the legacy heap, every allocation above the dedicated-bucket threshold walks one shared,
size-ordered free list from the smallest block until something fits. The cost of each allocation grows
with the number of free blocks, and the number of allocations grows with the world, so large maps pay far
more than proportionally.

**What it was worth,** measured once on a 57 x 57 km map by sampling every thread through a complete load:

| | legacy heap | segment heap |
|---|---|---|
| CPU time in the allocator's free-list walk | 545 CPU-s | 0 |
| load time | ~960 s | ~65 s |
| private bytes | ~19 GB | ~17.8 GB |

The game's own work was unchanged between the two runs, which is what pins the difference on the
allocator. The same code path runs on every allocation during play, so large maps also stutter less.
Ordinary map sizes gain much less. Microsoft does not promise the Segment Heap is faster in general; it
wins here because this particular legacy-heap algorithm was measured as the cost.

**Scope and removal.** The value applies to any process named `TransportFever2.exe`. It is removed when the
last product that owns it (this one or Big Maps) is uninstalled; the surrounding Image File Execution
Options key is never deleted, because it is shared with debuggers and exploit-mitigation settings. To
switch it by hand, for example to rule it out while debugging, run `tools\segment_heap.ps1 -Status`,
`-Enable` or `-Disable` from an elevated prompt; it takes effect at the next launch.

## The mod and your saves

Transport Fever 2 enables mods per game. On every launch the menu DLL adds the **Transport Fever 2 Multiplayer** mod to
`activeMods` in the game's `settings.lua` if it is missing (keeping a backup as `settings.lua.mpbak`;
`automod=0` in `tpf2_menu_flags.txt` turns this off), so a new game starts with the mod enabled. An
existing save keeps the mod list it was saved with: enable the mod once in that save's **Mods** panel
on the load screen.

## Upgrading

Install the new MSI over the old one; there is no need to uninstall first.

- **One entry in Apps.** Every build has a new ProductCode under one fixed UpgradeCode, and the major
  upgrade removes the previous version in the same transaction. A rebuild with the same version number
  also replaces the installed one. Downgrades are refused ("A newer version of TpF2 Multiplayer is already
  installed. Uninstall it first.").
- **`alut.dll` is never wrapped twice** (see above).
- **`tpf2_slice.cfg` is replaced** with the shipped one on every upgrade and Repair. It holds only optional
  diagnostics, so nothing that matters is lost; to keep a diagnostic switched on across upgrades, keep your
  copy in `%LOCALAPPDATA%\tpf2mp\data\` and delete the game-folder copy. An old `tpf2_bridge_mp.cfg`, if one
  is left behind, is ignored: the bridge has no settings.
- **Runtime data is untouched.**

`installer\VERSION` is the version stamped into the package. Bump it for every release.

## Uninstalling

- **Apps → TpF2 Multiplayer → Uninstall**, run the MSI again and choose **Remove**, or
  `msiexec /x TpF2Multiplayer.msi`. This removes the packaged files and the registry key; the proxy, the
  plugin host and the Segment Heap value go too, and the stock `alut.dll` is restored, unless TpF2 Big Maps
  is still installed and needs them. A folder that still holds runtime files (such as `netpunch\` with the
  lobby's logs) is left behind.
- **By hand**, if all else fails: delete `alut.dll` and rename `alut_real.dll` to `alut.dll`. The other files
  do nothing without the proxy.
- `%LOCALAPPDATA%\tpf2mp` is never removed; delete it yourself if you want your logs gone.

## Silent install

```
msiexec /i TpF2Multiplayer.msi /qn /l*v install.log
msiexec /i TpF2Multiplayer.msi /qn INSTALLFOLDER="D:\SteamLibrary\steamapps\common\Transport Fever 2"
```

`INSTALLFOLDER` must contain `TransportFever2.exe`. Setting `TPF2_SKIP_GAMEDIR_CHECK` to any value skips
that check; it exists for test rigs.

## Building the MSI

Prerequisites:

- Visual Studio 2022 Build Tools with the MSVC x64 toolchain, in its default location (the `.bat` scripts
  call its `vcvars64.bat`).
- Python 3.12 with `pip install pyinstaller -r netpunch\requirements.txt`.
- WiX Toolset v7 as a .NET global tool (`dotnet tool install --global wix`). `build_msi.ps1` runs it from
  `%USERPROFILE%\.dotnet\tools\wix.exe` and adds `WixToolset.UI.wixext` if it is missing.
- WiX v7 requires accepting its Open Source Maintenance Fee EULA (<https://wixtoolset.org/osmf/>): once with
  `wix eula accept wix7`, or per run with `-AcceptWixEula`. The script never accepts it on your behalf.

```
powershell -ExecutionPolicy Bypass -File installer\build_msi.ps1 [-AcceptWixEula] [-Validate]
```

| option | effect |
|---|---|
| `-SkipBuild` | reuse the built DLLs and `netpunch.exe` (the custom-action DLL is still built if missing) |
| `-SkipFreeze` | reuse an existing `netpunch\dist\netpunch.exe` (with a warning) instead of re-freezing |
| `-Validate` | after building, run an administrative install into a temporary folder and list what it extracted |
| `-Version x.y.z` | stamp this version instead of `installer\VERSION` |
| `-AcceptWixEula` | pass `--acceptEula wix7` to `wix build` |

What it does:

1. Builds the native DLLs: `native\build.bat proxy` and `host`, then `menu` and `slice`. If the running game
   holds `tpf2_menu.dll` or `tpf2_slice.dll`, those two are rebuilt under a suffixed name and packaged under
   the plain one; `proxy` and `host` need the game closed. (The `menu` target also copies the new DLL into
   the game folder when it can.)
2. Freezes the lobby with PyInstaller into `netpunch\dist\netpunch.exe`.
3. Builds the custom-action DLL with `ca\build_ca.bat` into `installer\out\tpf2ca.dll`.
4. Runs `wix build -arch x64 -ext WixToolset.UI.wixext` on `Package.wxs` and `PluginHost.wxs` into
   `installer\out\TpF2Multiplayer.msi` (plus a `.wixpdb`). `installer\out\` is git-ignored.

## Testing an upgrade

```
powershell -ExecutionPolicy Bypass -File installer\test_upgrade.ps1 [-Msi <msi>] [-UpgradeMsi <newer msi>] [-KeepSandbox]
```

From an elevated prompt, with the game closed. It creates a throwaway game folder with a stand-in "stock"
`alut.dll`, then installs, upgrades (with the same MSI unless `-UpgradeMsi` is given) and uninstalls,
checking after each step that exactly one Apps entry exists, that `alut_real.dll` is still the stock file
after the upgrade, and that uninstalling restores the stock `alut.dll` and removes the DLLs and `netpunch`.

Know before running it: these are real per-machine transactions that write `HKLM`. It has no guard against a
real TpF2 Multiplayer installation, which shares the UpgradeCode and would be removed by the test's install.
With TpF2 Big Maps installed the first check fails, because the proxy then has another owner.

## Files in this folder

| file | purpose |
|---|---|
| `Package.wxs` | the package: folders, components, upgrade rules, and a copy of `WixUI_InstallDir` with the game-folder check |
| `PluginHost.wxs` | shared with TpF2 Big Maps (keep it byte-identical): proxy, plugin host, Segment Heap value, custom actions and their sequencing |
| `ca\tpf2ca.cpp`, `ca\build_ca.bat` | the custom actions: game-folder check, preserve/rollback/restore of `alut.dll` |
| `build_msi.ps1` | the build script |
| `test_upgrade.ps1` | the install/upgrade/uninstall test |
| `cfg\tpf2_slice.cfg` | the shipped settings file (optional diagnostics, all commented out) |
| `cfg\tpf2mp.cfg` | the plugin host's settings file, for reference (not packaged) |
| `VERSION` | the version stamped into the package |
| `License.rtf` | the MIT license shown by the wizard |
