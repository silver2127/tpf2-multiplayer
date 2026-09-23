# TpF2 Multiplayer on Linux: install, uninstall, logs

This release is for the **native Linux version** of Transport Fever 2 on Steam. If you run the
Windows version through Proton, it does not apply.

## Requirements

- Transport Fever 2 from Steam, **Linux build 35924**. The installer reads the game binary's GNU
  build-id and refuses any other build: the mod's libraries patch that build only and stay off on
  anything else, so such an install would do nothing. A game update newer than this release needs a
  newer release. When neither `readelf` nor `file` is installed the check is skipped; the loader
  checks the build again when the game starts.
- Steam from Valve or your distribution, or the Steam **Snap**. The **Flatpak** Steam is detected,
  but that setup has not been tested.
- `bash` and the usual command-line tools (coreutils, `sed`, `awk`, `find`).

**Close the game before installing or uninstalling.** Both scripts stop while Transport Fever 2 is
running (`--force` goes on anyway). A running game keeps the libraries it has loaded until it
restarts, but it reads the mod's scripts again every time it loads a game or a save: new scripts next
to the old libraries, or no scripts at all, can break that session.

## Install

This development tree integrates **Windows 0.7 plus dev `8c3c02a5`**, on
top of Linux merge PR #5. The integration and live-test record is
[UPSTREAM_dev_8c3c02a5.md](UPSTREAM_dev_8c3c02a5.md). Earlier Native/Windows frozen join
and company-command replay were exercised on the VPS; see that record for
desktop visual checks and external Steam P2P checks still outstanding.

Canonical simulation ordering is on by default since dev `ad3d66e4`.
`TPF2MP_ORDER_CANON=0` disables it; unset or any other value enables it.
Simulation settings must match every peer, including Windows players whose
sorts default on. Native retained-world joins remain opt-in, and matching
versions alone do not establish gameplay parity. Loaded-game lifetime and
cross-platform validation remain outstanding; see
[UPSTREAM_dev_ad3d66e4.md](UPSTREAM_dev_ad3d66e4.md).

The native implementation includes command capture and replay, deterministic
ordering hooks, company permissions, save selection, load progress, automatic
Workshop registration, and the recovery controller used for frozen joins and
resync. The in-game **Manage Lobby** action opens the native lobby panel;
recovery prompts and progress appear automatically. Native dedicated mode and
VPS service instructions are in [tools/server](../../tools/server/README.md).

The shared Lua is checked against that Windows baseline. One pinned Linux
integration adds an explicit origin-replay marker for native name/colour
commands while preserving the existing Windows packets. All 32 HUD glyph
textures match the Windows baseline.

Station/depot glyphs use their owner's company colour. Entity-window washes
have a native implementation and await live separate-company visual checks.
Vehicle-icon and station-label colours now have native implementations and
fixture coverage; live visual checks remain. Other native
parity additions, including spare-line callbacks, platform assignments and
modular-station connector welding, have fixture coverage and still need their
live gameplay checks. See the integration record for current test limits.

`trainorder=0`, `roadspace=0`, `roadentries=0`, `shiporder=0`, `airorder=0`,
`sharedstations=0` and `pausedtick=0` in the root/data `tpf2_menu_flags.txt`
disable the respective hooks at startup. Peers need matching simulation
settings. `showicons=0`, `foreignwindows=0`, `stationicon=0`, `iconcolor=0` and `windowcolor=0`
disable the corresponding UI features; `tintclass=mpCo` selects the opaque
company class. Creating `tpf2mp_governor_off.txt` in the runtime data folder
disables the Lua speed governor.

Returning to the title menu leaves the lobby. Use the Linux installer for
updates. Installation also removes the obsolete `mods/m3_determinism_1` probe,
with removal shown in `--dry-run`.

Download the `.run` installer, then run:

```sh
bash tpf2mp-linux-<version>.run
```

Paste the printed `tpf2mp-launch %command%` line into **Transport Fever 2 → Properties → Launch Options** in Steam. The installer verifies its packaged checksums before copying files.

The tarball contains the same release for manual extraction:

```sh
tar xzf tpf2mp-linux-<version>.tar.gz
cd tpf2mp-linux-<version>
./install.sh
```

Use `bash tpf2mp-linux-<version>.run --extract NEW_DIRECTORY` to extract without installing. Installer arguments such as `--game` and `--dry-run` also work with the `.run` file.

The installer looks for the game in every Steam folder it knows and in every library listed in
Steam's `libraryfolders.vdf`. When the game is installed more than once (for example under both
the Snap and a native Steam), it lists every copy and picks the one whose Steam ran the game last.
`--game` chooses a copy explicitly.

| option | what it does |
|---|---|
| `--game DIR` | the Transport Fever 2 folder to install into |
| `--launch-options` | print the durable Steam launch wrapper line (default) |
| `--patch-runsh` | use the legacy `run.sh` preload block; Steam updates may undo it |
| `--data-home DIR` | the `XDG_DATA_HOME` the game sees, when it is not the default for your kind of Steam |
| `--dry-run` | print what would change, change nothing |
| `--force` | install even though the game binary is not build 35924, or while the game is running |

Running `install.sh` again, or a newer release's `install.sh`, updates the install in place. Files an
earlier install put into `<data home>/tpf2mp/` that the new release does not have are removed, so the
loader never loads a library of an older release next to newer ones.

## Where things go

`<data home>` is the `XDG_DATA_HOME` **the game** sees. That depends on the kind of Steam, not on
your shell:

| Steam | `<data home>` |
|---|---|
| native | `$XDG_DATA_HOME`, or `~/.local/share` when that is unset |
| Snap | `~/snap/steam/common/.local/share` (the Snap gives Steam and its games `HOME=~/snap/steam/common`) |
| Flatpak (untested) | `~/.var/app/com.valvesoftware.Steam/data` |

| path | what it is |
|---|---|
| `<data home>/tpf2mp/libtpf2mp_boot.so` | the loader, preloaded into the game (see below). It loads the libraries next to it. |
| `<data home>/tpf2mp/tpf2_*.so` | the libraries that do the work: the bridge (identity, the link to the lobby, the game-speed hooks), the title menu's Multiplayer entry and panel, and the others the release contains |
| `<data home>/tpf2mp/netpunch/netpunch` | the lobby |
| `<data home>/tpf2mp/tpf2mp_install.txt` | the list of installed files, used by `uninstall.sh` |
| `<game>/mods/mp_lockstep_1/` | the game-script mod |
| `<data home>/tpf2mp/tpf2mp-launch` | launch wrapper that selects this install and preloads the loader |
| `<game>/run.sh.tpf2mp-orig` | original start script, present only with the legacy `--patch-runsh` installation |

At run time the mod writes to `<data home>/tpf2mp/data/` (identity, the files the game script and the
libraries exchange, and the logs), the lobby writes to `<data home>/tpf2mp/netpunch/`, and earlier
runs' logs are kept in `<data home>/tpf2mp/logs/`. The installer and the uninstaller leave these
three alone.

## How the game loads the mod

By default the installer prints a launch line such as:

```
/home/you/snap/steam/common/.local/share/tpf2mp/tpf2mp-launch %command%
```

Steam runs the wrapper before the game's command. The wrapper sets `XDG_DATA_HOME` to this installation's parent directory and adds its loader to `LD_PRELOAD`. This survives game updates and Steam's **Verify integrity of game files**. An earlier installer’s preload block in `run.sh` is removed during migration.

The loader acts only in the `TransportFever2` executable, removes itself from inherited `LD_PRELOAD`, and logs to `<data home>/tpf2mp/data/tpf2_proxy.log`. It checks the game's build before installing hooks. `LD_PRELOAD` cannot represent a library path containing whitespace or colons, so the installer refuses those locations.

`./install.sh --patch-runsh` is available for the older setup. It adds a preload block to the game's `run.sh` and keeps the original as `run.sh.tpf2mp-orig`. Steam updates and file verification can remove that block; reinstall or switch to the default launch wrapper afterward.

## Uninstall

```sh
./uninstall.sh            # add --purge to also delete <data home>/tpf2mp with its data and logs
```

It removes the files listed in `tpf2mp_install.txt`, the loader and libraries by name should the list
miss one, and the mod folder. It then restores `run.sh`: from `run.sh.tpf2mp-orig` when that is still
the file the block was added to, otherwise by removing the block. A block that was edited by hand is
left for you to undo. Remove the wrapper line from Steam launch options after uninstalling.
`--game DIR`, `--data-home DIR`, `--force` (here: uninstall while the game runs) and `--dry-run`
work as for `install.sh`.

## Optional native 3D previews

The release includes `plugins/tpf2_previews.so`. Its binary checks and startup
hooks pass on build 35924, but rendering and map lifecycle tests remain open,
so native 3D previews default off. Shared previews remain available through
the script fallback.

For development testing, add this to `<data home>/tpf2mp/tpf2mp.cfg`:

```ini
[previews]
enabled=1
```

Restart the game after changing it. The plugin activates only after the
loaded world's rendering thread passes its checks. Native failures return to
the script fallback. Logs are in `<data home>/tpf2mp/data/tpf2mp_host.log`.

## Logs and bug reports

- **Every start saves the previous run's logs** in `<data home>/tpf2mp/logs/<date>-<time>-previous/`:
  - the game's own log, including the mod's script lines;
  - the mod's logs;
  - the lobby's logs;
  - crash dumps written since the last save.

  This also works after a crash: just start the game again. The last 2 are kept. `about.txt` in each
  folder lists what is there, with sizes.
  Create `tpf2mp_keep_logs.txt` in the runtime data folder to keep every archive
  and append mod/lobby logs across starts. Remove it to restore normal retention.
- **`./collect_logs.sh`** packs everything a bug report needs into
  `tpf2mp-logs-<computer>-<time>.zip` in your Downloads folder, without uploading anything:
  - the data folder and the saved runs;
  - the lobby's logs;
  - the game's log and recent crash dumps;
  - what is installed, and a system summary.

  Run it before starting the game again: a start wipes the game's log (though the mod saves a copy
  first).
- By hand, the game's log is `<Steam>/userdata/<account>/1066780/local/crash_dump/stdout.txt`. With
  the Snap, `<Steam>` is `~/snap/steam/common/.local/share/Steam`. `stdout_old.txt` beside it is the
  run before. Crash dumps are the `*.dmp` files in the same folder.

For a multiplayer problem, send the logs of **every** player. Send them privately: the lobby logs
contain IP addresses.

## Troubleshooting

| symptom | what to check |
|---|---|
| no MULTIPLAYER on the title menu, and no `tpf2_proxy.log` in `<data home>/tpf2mp/data/` | Check the wrapper line in Steam launch options and that it points to this installation. With the legacy `--patch-runsh` setup, reinstall after Steam restores `run.sh`. |
| `tpf2_proxy.log` says `UNKNOWN BUILD` | The game was updated past build 35924. Wait for a release for the new build. |
| `tpf2_proxy.log` says `... load FAILED` | The line names the library and the loader's error. Reinstall. |
| the installer says the game was not found | Pass `--game` with the folder that contains `TransportFever2`. |

If a joiner stays on **dialing host**, check the Linux host's firewall: the lobby
list and rendezvous use outbound HTTPS, but joining needs inbound **UDP 29471**
by default. A visible public lobby or a `joiner knocked` log line does not prove
that UDP reaches the host. Confirm the listener with `ss -lunp` and check
`sudo journalctl -k --since '5 minutes ago'` for firewall drops with `DPT=29471`.
For UFW, allow only the intended source and interface; for example, with host
`192.168.0.29`, joining PC `192.168.0.140`, and host interface `enp3s0`:

```sh
sudo ufw allow in on enp3s0 proto udp from 192.168.0.140 to 192.168.0.29 port 29471
sudo ufw status numbered
```

Replace those example addresses and interface with yours, then retry joining and
check `<data home>/tpf2mp/netpunch/lobby_proc.log` and the lobby roster. Use the
configured lobby port if it differs from 29471. The installer leaves firewall
rules to the user; it does not disable the firewall or add broad allow rules.

## Building a release (developers)

```sh
tools/linux/build_release.sh [--build-dir DIR] [--out DIR] [--version V]
                             [--netpunch PATH | --no-netpunch] [--without LIB]...
```

The script:

1. Builds and tests `native/linux` in Valve's pinned Steam Runtime **soldier** SDK, using `native/linux/out-release` by default. The first build downloads about 947 MiB and caches the SDK. It requires `bubblewrap`, `curl`, `tar`, `sha256sum` and `objdump`. The SDK download is checksum verified; builds run with read-only source and no network. Native imports are checked against the glibc 2.31 baseline. `--host-build` is an explicit development fallback that uses the host compiler and may require a newer glibc.
2. Requires every library the loader loads: `libtpf2mp_boot.so`, `tpf2_bridge_mp.so`,
   `tpf2_menu.so`, `tpf2_slice.so` and `tpf2_pluginhost.so`. `--without tpf2_slice.so` or
   `--without tpf2_pluginhost.so` leaves one out on purpose, and `BUILDINFO` says so.
3. Checks the libraries' dynamic exports: the loader exports `clock` and
   `__sprintf_chk`; the latter applies decimal tie compatibility only to the
   verified build's Lua floating-point formatter. Other callers retain libc
   behavior. The libraries loaded by the boot library export nothing.
4. Gets the lobby. `tools/linux/build_netpunch.sh` builds it, and `netpunch/dist-linux/netpunch` must
   then be newer than the start of that build. Without that script, pass `--netpunch PATH`: an
   existing `netpunch/dist-linux/netpunch` is never taken on its own. `--no-netpunch` makes a release
   without the lobby. `BUILDINFO` records where the lobby came from, its sha256 and its date.
5. Lays out `dist/linux/tpf2mp-linux-<version>/` (`lib/`, `mod/`, `netpunch/`, the scripts, this
   file, `BUILDINFO` and `SHA256SUMS`) and produces both `tpf2mp-linux-<version>.tar.gz` and the self-extracting `tpf2mp-linux-<version>.run` installer.

The lobby builder uses pinned Python and manylinux wheels, checks every bundled ELF dependency against glibc 2.31, and supports `--test` for its five local network/transfer tests. Native build provenance, source commit, included libraries and lobby checksum are recorded in `BUILDINFO`.

The version defaults to `installer/VERSION`. See `RESUME_STATUS.md` in the source tree for implementation coverage and remaining runtime validation; packaging success alone does not establish multiplayer parity.

### Big Maps in the unified package

`tools/linux/build_native.sh` builds and tests the in-tree `bigmap/linux` plugin.
The release includes `plugins/tpf2_bigmap.so` and its **Linux** configuration;
`--without tpf2_pluginhost.so` omits plugins. The native defaults cap depth at 11
and tiles at 512 (510 on square maps), with terrain paging on by default (missing key also enables it). Set
`terrain_cache_compress=0` and restart after SIGBUS; kernel-origin faults on
evicted pages cannot be served by this pager. Unsupported userfaultfd setup
keeps stock allocation paths. The Windows
configuration requests unsupported depth 13 and must not replace the Linux one.
See [Big Maps scope and evidence](../../bigmap/docs/linux/PORT.md) and the
[current integration](UPSTREAM_dev_8c3c02a5.md) for remaining Windows features.
The installed `bigmap-density-restore` helper restores the plugin's exact density
patch before upgrade/uninstall; modified patches are left intact and removal stops.
Native live join remains opt-in (`tpf2mp_live_join.txt` = `1`); Windows 0.7 defaults on.

The [dev `60d237c5` integration](UPSTREAM_dev_60d237c5.md) retains the Windows
autosave-sidecar fix. Native Big Maps does not yet capture or restore `.terr`
sidecars, so this fix does not enable them on Linux.

### Optional Big Maps worktree override in development packages

Pass `--bigmap-repo /path/to/tpf2-bigmap` to `tools/linux/build_release.sh`
or `tools/linux/auto_install.py` to build and ship that checkout's native
plugin and configuration. Source is mounted read-only; outputs go in the
multiplayer build directory. Plugin-only source changes trigger a rebuild
and installation after all games close. The selected checkout must contain
`linux/CMakeLists.txt` and `linux/tpf2_bigmap.cfg`. This option never launches
the game. Automatic recovery and Workshop registration remain unsupported as
recorded in the integration notes.

## Experimental Steam transport (0.6.1.28)

Messages v002 is the default, resolved from the game's loaded `libsteam_api.so`.
To compare Legacy, close the game and create `tpf2mp_steam_legacy.txt` in this
installation's runtime `data/` directory. Remove it with the game closed to
return to Messages. Both peers must choose the same mode; check `transport=Messages`
or `transport=Legacy` in the bridge log. An unavailable Messages API disables
Steam transport without falling back. Direct TCP remains preferred for saves;
only transfers actually using Steam measure this comparison. No Linux internet
throughput improvement has been measured for this integration.

Messages now starts with equal 1 MiB/s clamps and adjusts them every five
seconds using active outgoing peers' remote delivery quality. Legacy retains
its fixed configuration. Look for [steam-rate] adjustments; configured rates
are not measured save-transfer throughput. The redesigned Create Game and host
lobby views expose Cross-play through the existing invitation-code switch.

The [dev `a42dab6c` integration](UPSTREAM_dev_a42dab6c.md)
keeps hot-join save requests pending while the host world loads, then
takes the save when the game UI is ready. Version remains 0.7.
