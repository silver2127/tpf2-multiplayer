# Windows 0.4.22 under Steam Proton

This compatibility setup installs the official Windows 0.4.22 DLLs and all 24 Lua
files unchanged. Hosting on Proton needs the narrowly scoped lobby dependency repair
below; joining already worked with the original lobby executable. It runs the Windows
game through Steam's selected Proton version. The native Linux build remains a
separate implementation.

**The released Windows DLLs retain native fallbacks.** This package does not satisfy
the earlier requirement that every action use strict cancellation and replay.
Windows–Proton testing has shown matching hashes during a joined session, including
construction; that observation does not establish determinism across every action
and workload.

## Install

Select Proton in the game's Steam compatibility settings, let Steam finish downloading
the Windows game, and close the game before installing. This setup needs Python 3.11+
and the extracted payload from the [official v0.4.22 release](https://github.com/silver2127/tpf2-multiplayer/releases/tag/v0.4.22).
It does not download files, run Wine, change Steam settings, or register an MSI.

The official `TpF2Multiplayer.msi` SHA-256 is
`1f7aa947bc069aaec5d14e89a28ec2ac98d284a37b3f065302bc9fa923efd08d`.
On Linux, `msiextract -C EXTRACTED TpF2Multiplayer.msi` extracts it. Pass the resulting
`EXTRACTED/Program Files/Steam/steamapps/common/Transport Fever 2` directory below.

```sh
python3 tools/proton/setup.py \
  --game-dir '/path/to/Steam/steamapps/common/Transport Fever 2' \
  --steam-root '/path/to/Steam' \
  --payload-dir '/path/to/extracted/Program Files/Steam/steamapps/common/Transport Fever 2' \
  --dry-run
```

Remove `--dry-run` to install. Replace it with `--verify` to check the installed files.
An optional `--prefix /path/to/pfx` overrides the default prefix at the game's Steam
library's `steamapps/compatdata/1066780/pfx`. For Snap Steam, use the paths visible
inside Snap, normally beneath `~/snap/steam/common/.local/share/Steam`.

The script verifies every extracted file against the checked-in release manifest and
checks the Windows build guard. It preserves the stock audio library as `alut_real.dll`
only when its checksum matches build 35924. Replaced files are copied to timestamped
directories under `<game>/.tpf2mp-proton-backups/`; repeating setup with identical
files makes no changes. Unexpected mod files, managed symlinks, and stale DLLs or a
Python lobby under the prefix's `AppData/Local/tpf2mp` stop setup before installation.

Windows menu and lobby paths need access to the real Steam saves and mods. Setup creates
prefix-local links for `Steam/userdata`, `Steam/steamapps/common`, and
`Steam/steamapps/workshop`, preserving Proton's own `steamapps/libraryfolders.vdf`.
The links also work when created before Proton initializes the prefix. Conflicting
links or directories containing data require manual review; empty directories can be
replaced. A missing workshop directory is allowed until Steam creates it.

## Repair the hosting crash

The released lobby embeds a miniupnpc DLL with 64 duplicated PE relocation entries.
When Proton loads it away from its preferred base, those pointers move twice and
Windows thread-local storage initialization crashes. The menu remains on
"observing NAT" because its lobby process has exited. See [the crash analysis](NAT_CRASH.md).

Create a separate repaired lobby before installation:

```sh
python3 tools/proton/fix_lobby_relocations.py \
  --input '/path/to/extracted/Program Files/Steam/steamapps/common/Transport Fever 2/netpunch/netpunch.exe' \
  --output '/path/to/repaired/netpunch.exe'
```

Add `--repaired-lobby '/path/to/repaired/netpunch.exe'` to the setup and verification
commands above. This is explicit: setup's default remains the original release.
Both input and repaired output are pinned by SHA-256. Only the miniupnpc relocation
padding and PE checksums change; the lobby's Python bytecode, all other 73 archive
members, all Windows mod DLLs, and all Lua remain unchanged. UPnP stays enabled.
The repaired host passed an isolated Proton startup, code/roster publication, and
clean shutdown test. Internet connectivity still depends on the network.

The repaired executable SHA-256 is
`da4fb91961f1aed3c7e7543a837220902a7af11a0ea59412dd7b77bfd001111f`.

## Launch and test

Remove the native Linux preload wrapper from this game's Steam launch options and
start the game normally through Steam. Avoid carrying Linux `TPF2MP_DATADIR` or
`LOCALAPPDATA` overrides into Proton; the Windows DLLs and Lua need Windows paths.
No DLL override is required when Proton loads the game's `alut.dll` normally. If logs
show that the proxy never loaded, the explicit load-order setting is
`WINEDLLOVERRIDES="alut=n,b" %command%`; preserve other existing overrides if any.

Check `<prefix>/drive_c/users/steamuser/AppData/Local/tpf2mp/data/tpf2_proxy.log`
for successful DLL loads and the game directory's `tpf2_menu.log` for the resolved
save/lobby paths. The plugin-host and slice logs are in the runtime data directory.
Verify the multiplayer mod is active in the test map, then compare initial hashes
with a Windows peer before testing rail construction. Follow with vehicle/line actions,
save transfer, late join, and resync. A successful title-screen launch alone does not
validate multiplayer or determinism.

This extracted-payload setup omits the MSI's Windows Segment Heap registry setting and
Windows Installer bookkeeping. To stop loading the multiplayer DLLs, close the game,
verify the preserved stock `alut_real.dll`, and copy it back over `alut.dll`.
Backups preserve previous files; saves and runtime data are not removed by setup.

## Fixture verification

The installer checks can run without changing a game or prefix:

```sh
python3 tools/proton/test_setup.py \
  --payload-dir '/path/to/extracted/Program Files/Steam/steamapps/common/Transport Fever 2' \
  --stock-alut '/path/to/stock/alut.dll' \
  --repaired-lobby '/path/to/repaired/netpunch.exe'

python3 tools/proton/test_lobby_relocations.py \
  --input '/path/to/extracted/Program Files/Steam/steamapps/common/Transport Fever 2/netpunch/netpunch.exe'
```
