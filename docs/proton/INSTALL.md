# Linux and Steam Deck: the Windows game under Proton

`tools/proton/install_proton.sh` (a shell script: bash, curl, sha256sum, unzip)
and `tools/proton/install.py` (Python 3.9 or newer) install TpF2 Multiplayer into
Transport Fever 2 when Steam runs the Windows game through Proton. Both install
the same files as the Windows MSI. Every release page carries copies of them as
`install_proton.sh` and `install_proton.py`.

The multiplayer mod is Windows code; under Proton it runs unchanged. The
separate native Linux build (branch `linux-native`, releases tagged
`-linux-dev`) is for the native Linux game and has its own installer.

## Install

1. In Steam: **Transport Fever 2 → Properties → Compatibility → Force the use of a
   specific Steam Play compatibility tool**, pick a Proton version (Proton 9 or
   newer; Proton 11 is what this was tested with), and let Steam download the
   Windows game.
2. Start the game once and quit, so Proton creates its prefix.
3. Download `install_proton.sh` from the
   [release](https://github.com/silver2127/tpf2-multiplayer/releases) you want,
   close the game, and run:

   ```sh
   sh install_proton.sh
   ```

   It needs only bash, curl, sha256sum and unzip (or bsdtar), which every Linux
   desktop and the Steam Deck have. `install_proton.py` on the same release page
   does the same with Python 3.9 or newer (`python3 install_proton.py`), and can
   additionally repair a lobby executable from a release before 0.6 and check an
   installation (`--verify`). Both take the options below.

   It finds Steam, the game and the Proton prefix (native, Snap and Flatpak
   Steam, every library in `libraryfolders.vdf`, SD cards included), downloads that
   release's `TpF2Multiplayer-files.zip`, checks it against the release's
   `SHA256SUMS.txt`, and installs. `--dry-run` shows the plan and changes nothing.

Everyone in a session needs the same version, Windows or Proton alike.

Other ways to get the files: `--files-zip TpF2Multiplayer-files.zip` (downloaded by
hand), `--msi TpF2Multiplayer.msi` (needs `msiextract` from the `msitools`
package), or `--payload-dir` (an extracted MSI's `Transport Fever 2` folder).
`--steam-root`, `--game-dir` and `--prefix` override the detection.

## What it does

In the game folder, the same as the MSI: keeps the game's own `alut.dll` as
`alut_real.dll` and puts the proxy in its place, installs `tpf2_pluginhost.dll`,
`tpf2_bridge_mp.dll`, `tpf2_menu.dll`, `tpf2_slice.dll`, `plugins/*.dll`,
`netpunch/netpunch.exe` and the `mods/mp_lockstep_1` mod (old files of an earlier
version are removed), and `tpf2_slice.cfg` unless one is already there. It
refuses while the game runs, when `alut.dll` is not the game's own file (another
mod replaced it), when the game is not Steam build 35924, and when the folder
holds the native Linux game.

In the Proton prefix, it links `drive_c/Program Files (x86)/Steam/userdata`,
`steamapps/common` and `steamapps/workshop` to the real Steam folders, so the
menu and the lobby find saves, mods and Workshop items. Proton's own
`steamapps/libraryfolders.vdf` is left alone. The Segment Heap registry switch
the MSI sets on Windows is not applied: it means nothing to Wine.

Replaced files are kept in `<game>/.tpf2mp-proton-backups/<timestamp>/`, and
`<game>/.tpf2mp-proton-manifest.json` records what was installed.

`python3 install_proton.py --verify` checks an installation;
`--uninstall` removes the mod and puts the game's own `alut.dll` back (the shared
proxy and plugin host stay when another product, such as TpF2 Big Maps, still
uses them).

## The Wine heap fix

From 0.6.1.19 the proxy (`alut.dll`) does one more thing when it runs under Wine: before the
game starts, it switches the heap's 1 KB to 32 KB size classes over to Wine's lock-free
front end. Nothing in the installer does this; it is part of the DLL, so installing or
updating the mod is all it takes, and the installer's last lines say whether the version
you installed has it (`--verify` says so too).

Why it matters: Wine only turns that front end on for a size class while more than 4 MiB
of it is alive at once. The game's simulation allocates and frees small buffers all the
time with few alive, so it never qualified, and every such allocation took a lock and
searched a long list of free blocks. Measured on the dedicated server (Proton 9, a
119 MB world): a quarter of the game's CPU and 79% of the simulation thread went to that
one search, and the session could not hold 2x. The rule is the same in Proton 9, Proton
10 and upstream Wine, so choosing another Proton version does not change it. Windows is
not affected (the MSI's Segment Heap switch covers it there).

The start-up step costs about 90,000 allocations and a few tens of milliseconds, once,
and leaves a line in `tpf2_proxy.log`:

```
[proxy] wine heap: 1 heap(s), 80 bin(s) switched to the front end with 90522 allocations in 47 ms
```

To turn it off for a comparison, set the game's Steam launch options to
`TPF2MP_WINE_HEAP=0 %command%`.

## The lobby repair

`netpunch.exe` is a PyInstaller bundle. The miniupnpc DLL inside it (UPnP port
mapping for hosts) has a malformed relocation table: 64 entries appear twice.
Windows loads the DLL at its preferred address and never applies them; Wine
relocates it, applies each entry twice, and the lobby crashes the moment a game
is hosted. Joining does not touch UPnP and was never affected. The analysis is in
[NAT_CRASH.md](NAT_CRASH.md).

The installer repairs the installed `netpunch.exe` (and any copy an older release's
in-game updater left cached in the prefix): the second copy of each entry becomes padding, the
PE checksum is recalculated, and every other byte of every other bundle member
is verified unchanged. The repair is pinned to the exact DLL every release has
shipped; a different DLL is reported, not patched. Since the build that carries
this script, `installer/build_msi.ps1` applies the same repair to the lobby before
packaging, so newer releases need no repair at install time (the installer then
finds nothing to do). `--repair-lobby FILE` applies it to a file by hand.

## Updating

Run the script again (a newer copy from the new release, or `--version X.Y.Z`).
Releases before 0.6.1.11 had an in-game **DOWNLOAD UPDATE** button; a copy it cached
in the prefix takes precedence over the installed files, and the installer repairs
such copies when it finds them.

## Limits

- Tested by installing on one machine (Snap Steam, Proton 11). Hosting and
  joining under Proton were verified with the 0.4.22 payload and the repaired
  lobby; a full session on a current release has not been recorded here.
- The Windows DLLs keep their native fallbacks; cross-platform determinism between
  Windows and Proton players rests on the same code, but has had only short tests.
- Steam's "Verify integrity of game files" restores the game's `alut.dll` and
  removes the mod's entry point; run the installer again afterwards.

## Historical pinned setup

The [0.4.22 extracted-payload instructions](INSTALL_0.4.22.md) document
`setup.py`, its pinned checksums and the original test scope. They remain
available for reproducing that older setup; use the installer above for current releases.
