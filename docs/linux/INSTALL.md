# TpF2 Multiplayer on Linux: install, uninstall, logs

The [dev `4487d7cd` integration](UPSTREAM_dev_4487d7cd.md) advances the
native package, lobby handshake and panel to **0.7.1.4**. All peers must update.
The menu library now exports a monotonic gameplay-update timestamp, cleared
at the title menu. The Windows MSI warning-page fix is retained. Native Linux
uses a different terrain paging policy; no new loading-speed result is claimed.

The [dev `c4754f26` integration](UPSTREAM_dev_c4754f26.md) retains the Windows
installer’s folder-refusal dialog. Native Linux already prints the reason to
stderr and exits with an error; four installer regressions cover that behavior.
Release remains **0.7.1.3**.

The [dev `721ac61f` integration](UPSTREAM_dev_721ac61f.md) moves save reading, hashing and mod
discovery to a lobby worker. Joiners wait 30 seconds for a silent host; mesh
routing still bypasses it after 12 seconds. Shared lobby; release remains **0.7.1.3**.

The [dev `262353d7` integration](UPSTREAM_dev_262353d7.md) merges
housekeeping and correctness fixes for replay, vehicle/line identity, lobby transfers,
and network input handling. Native bridge epoch handling and kill-switch readers
are covered by Linux regressions; release remains **0.7.1.3**.

The [dev `528294b1` integration](UPSTREAM_dev_528294b1.md) asks for a
fresh host save when a late joiner would receive one over two minutes old,
with fallback if no new save arrives. Host-loop stalls no longer count as
peer silence. Shared lobby; release remains 0.7.1.2.

The [dev `304a4e28` integration](UPSTREAM_dev_304a4e28.md) adds catch-up progress
logging about every 20 seconds: remaining gap, closing rate, local and session
rates, and an ETA when the gap is closing. Shared Lua; release remains 0.7.1.2.

The [dev `86f806df` integration](UPSTREAM_dev_86f806df.md) keeps
terrain streams running across later STARTs, avoiding replacement while a
joiner reads the same sidecar. Shared Linux/Windows lobby; release 0.7.1.2.

The [dev `effa7243` integration](UPSTREAM_dev_effa7243.md) prevents
repeat save transfers while a joining player’s mods are packing, queued or
being delivered. The shared lobby fix applies to native Linux and Windows;
release remains 0.7.1.2.

Current release integration: **0.7.1.4**, Windows dev `4487d7cd`; see the
[integration and validation record](UPSTREAM_dev_4487d7cd.md). All peers, including dedicated
servers, must update. Existing native Sandbox town-tool capture, minimap and
cargo-filter limitations remain. Upstream performance measurements were not
repeated locally; matching release numbers do not establish gameplay parity.
Completed TCP transfers no longer stay in lobby memory through listener
registrations; active transfers retain their handlers until they finish.
Catching-up peers retain recently heard leader clocks when stamping commands
and defer spare-line requests until catch-up completes.
Dedicated servers pause while players join an otherwise empty server, and cap
the voted speed at 1x while someone joins existing players. A joiner stops
holding the session after about 20 minutes without a change in joining count.
Below 1x, command delay follows session speed with a ramp margin; recovery
from a slow session rises in steps, and excess delay falls faster.
Native terrain sidecars are on by default from 0.7.1.2 (the user's decision;
first game use: the project's dedicated server; `terrain_sidecar=0` turns them
off). Material-index acceleration remains unported.

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

From 0.7.0.6 onward, download `tpf2mp-linux-<version>-native.run` and
`tpf2mp-linux-<version>-native.sha256` from the matching tag in
[tpf2-multiplayer-packages](https://github.com/silver2127/tpf2-multiplayer-packages/releases).
The same install files also live on the mod repository's `<version>` release
(without `v`). The `v<version>` page carries the two launchers and is marked
Latest. Existing versions can be migrated with `page v<version>` (upstream
applied this to 0.7.0.5): install files are copied to the packages repository
and `<version>` before being removed from `v<version>`. `--replace-page` is
no longer supported. Launchers up to 1.2.0 must be updated first, since they
expect the MSI on `v<version>`. See [the current release layout](UPSTREAM_dev_8e0a0c00.md).
Verify the `.run` entry in the checksum file before running it:

```sh
sha256sum --check --ignore-missing tpf2mp-linux-<version>-native.sha256
bash tpf2mp-linux-<version>-native.run
```

The `-native.tar.gz` contains the same `tpf2mp-linux-<version>/` directory.
Local builds also retain the original filenames used below. Download or build
the `.run` installer, then run:

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

  This also works after a crash: just start the game again. The last 5 of each kind (startup and OPEN LOGS) are kept. `about.txt` in each
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

The builder also emits byte-identical `-native.run` and `-native.tar.gz` copies
and a `-native.sha256` listing those two filenames. These are the inputs to
`tools/publish_release.py --linux-dir dist/linux`; no manual renaming is needed.
The existing filenames and archive root remain available to local tooling.
See [the release-layout integration](UPSTREAM_dev_4e1e486c.md).

Launcher-only updates use `python3 tools/publish_release.py launcher`
(default: draft; `--publish` publishes). They need no `--linux-dir` and use
`launcher-v<version>` tags with `make_latest=false`; Latest remains the
`v<version>` launcher page. Launcher-only updates no longer delete or re-create
a mod release. Mod releases require `--linux-dir`, upload native files to both
install-file releases, and publish `<version>` before the `v<version>` launcher
page. See [the release-layout integration](UPSTREAM_dev_8e0a0c00.md).

The version defaults to `installer/VERSION`. See `RESUME_STATUS.md` in the source tree for implementation coverage and remaining runtime validation; packaging success alone does not establish multiplayer parity.

### Big Maps in the unified package

`tools/linux/build_native.sh` builds and tests the in-tree `bigmap/linux` plugin.
The release includes `plugins/tpf2_bigmap.so` and its **Linux** configuration;
`--without tpf2_pluginhost.so` omits plugins. The native defaults select depth 11
and cap tiles at 512 (510 on square maps), with terrain paging on by default (missing key also enables it). Set
`terrain_cache_compress=0` and restart after SIGBUS; kernel-origin faults on
evicted pages cannot be served by this pager. Unsupported userfaultfd setup
keeps stock allocation paths. The Windows
configuration requests depth 13; keep the packaged Linux configuration
for its native defaults. Depths 12/13 are available on the verified Steam ELF;
the Windows GOG fallback does not apply to it. See
[dev aaae03f8 integration](UPSTREAM_dev_aaae03f8.md).
Upstream now reports successful depth-13 play, save/reload and multiplayer
with a native Linux dedicated server; every peer needs the same `octree_depth`.
See [dev 63a3b8df integration](UPSTREAM_dev_63a3b8df.md) for attribution
and the distinction from local validation.
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

### Family guard performance (dev 7cacbaaf)

Linux 6.11+ can validate family memory ranges through `PROCMAP_QUERY` without
parsing the process mapping table. Older kernels, or environments denying the
ioctl, retain a fresh buffered snapshot per iteration; very large mapping
counts can still cost simulation time there. No kernel setting is changed by
the native libraries. See [integration evidence](UPSTREAM_dev_7cacbaaf.md).

The [dev `2c05099a` integration](UPSTREAM_dev_2c05099a.md) adds the remaining supplied
Windows RNG seed/distribution/engine compatibility modules, enabled by default.
`TPF2MP_SIM_SEED=0` and `TPF2MP_ENGINE_PARITY=0` disable them for diagnosis.
Static ELF checks and 65 native tests pass; the lab launch was blocked before
the game started, so cross-platform gameplay validation remains outstanding.

The [dev `582a380` integration](UPSTREAM_dev_582a380.md) makes native dedicated
restarts prefer a newer autosave of the hosted `mp_shared` world over the
configured save. Version remains 0.7.

The [dev `cf5f8a0e` integration](UPSTREAM_dev_cf5f8a0e.md) makes load-time company
switches wait for entity queries to answer and reuses live saved player entities.
Version remains 0.7; loaded-world validation is still outstanding.

The [dev `522a303b` integration](UPSTREAM_dev_522a303b.md) reports the slowest hash's
lane breakdown and the cost of post-hash broadcast, drift and comparison work.
Shared Lua regression tests pass; no live performance measurement is claimed.
Version remains 0.7.

The [dev `bd69b864` integration](UPSTREAM_dev_bd69b864.md) adds native UCRT math parity, octree depth 12/13,
target-record indexing and the 0.7.0.2 TCP/resync UI. Placement-distance and
attempt-budget parity remain unported; the lab launch was blocked before game startup.

## Town-development diagnostics (dev 0610033)

Set `TPF2MP_TOWN_TRACE=1` in the game's launch environment to write
`$XDG_DATA_HOME/tpf2mp/data/tpf2_towntrace.txt` (default XDG data home:
`~/.local/share`). Restart without the variable to disable it. The trace is off
by default and requires the verified town-seed hook. Compare captures from the
same session with `python3 tools/town_trace_diff.py NATIVE_TRACE WINDOWS_TRACE`;
Windows enables its half with `towntrace=1` in `tpf2_slice.cfg`.
See [integration and validation limits](UPSTREAM_dev_0610033.md).

## Release 0.7.0.3 (dev d8a3ce57)

Native Linux is at **0.7.0.3**: the `.run` installer, the tarball name, the
staged `VERSION` file and the `BUILDINFO` header all take it from
`installer/VERSION`, and the lobby handshake (`LOBBY_VERSION`) matches. Every
participant, including a dedicated server, needs the same version -- the gate is
an exact release match and a peer without a version fails closed.

The release carries no new native code: it is the version stamp for the fixes
the `0610033`, `11a98cc`/`e43d01dd` and earlier integrations already ported. The
Linux behaviour the release notes promise was rechecked against the unmodified
build-35924 ELF and passes; the octree depth, town-trace and math-parity notes
elsewhere in this file still apply unchanged.
See [integration evidence and validation limits](UPSTREAM_dev_d8a3ce57.md).

The [dev `122a0ce9` integration](UPSTREAM_dev_122a0ce9.md) adds the native resync
view’s in-game x. Closing a running resync hides its view while recovery
continues; Manage Lobby reopens it. Errors and unanswered Ready requests
bring it back automatically. Saving/loading suppresses the x.

Since [dev 45183ac6](UPSTREAM_dev_45183ac6.md), native OPEN LOGS and startup
archives include the installed version, kernel/time zone, module GNU build IDs,
and copied state/config files (last 8 MiB each, after logs). Lobby JSON/JSONL/text
copies mask invitation/password fields. Saves and terrain dumps are excluded.
The native menu uses xdg-open and reports the native archive location.

The [dev `96795a8b` integration](UPSTREAM_dev_96795a8b.md) expands the native public
browser to eight games per page and up to 32 games, and completes archive
runtime/boot metadata and standalone collector credential masking.

The [dev `01044521` integration](UPSTREAM_dev_01044521.md) expands the native public
browser to twelve games per page and up to 48 games, and removes the legacy
panel renderer. Closing a running resync leaves the game visible.

The [release 0.7.0.4 integration](UPSTREAM_dev_e86d5552.md) advances the native package
and shared lobby handshake to **0.7.0.4**. All peers, including dedicated
servers, must update. This release stamps the previously integrated hot-join,
menu and log fixes; existing native feature and live-validation limits remain.

The [dev `c74a7b4e` integration](UPSTREAM_dev_c74a7b4e.md) fixes
TCP save routing after joining through the master's UDP relay: the joiner tries
the host's advertised addresses, or continues on UDP when none are available.
The shared lobby implements this on Linux and Windows; version remains 0.7.0.4.

The [dev `f6e47ef9` integration](UPSTREAM_dev_f6e47ef9.md) adds
the master's TCP pipe fallback for slow save/mod transfers, shared by Linux
and Windows. Native shutdown cleanup is preserved; version remains 0.7.0.4.

The [release 0.7.0.5 integration](UPSTREAM_dev_f67726f8.md) advances
the native package and shared lobby handshake to **0.7.0.5**, stamping the
previously integrated relay address fix and TCP pipe fallback. All peers,
including dedicated servers, must update. Existing native feature and
live-validation limits remain unchanged.

## Bridge missing or failed to load

The [dev ba1fa26e integration](UPSTREAM_dev_ba1fa26e.md) disables hosting and
joining when `tpf2_bridge_mp.so` is not loaded in the game process. The menu
shows an amber explanation. Reinstall the native package and restart the game;
check `data/tpf2_proxy.log` for the attempted path and the `dlopen` error.
Having the file on disk or an old `tpf2_instance.txt` does not establish a load.

The [dev `2f65bae3` integration](UPSTREAM_dev_2f65bae3.md) adds compressed terrain edits
with checksum validation to native capture and replay. Every peer needs this
build; the unchanged 0.7.0.5 version handshake does not detect older terrain
readers. Uncompressed version-1 edits remain readable.

The [dev `616191b1` integration](UPSTREAM_dev_616191b1.md) makes native OPEN LOGS
include the newest startup archive’s game log and `crash_*` files as
`previous_run_*`, so a restart does not hide the crashed run’s dumps.
Copies remain subject to the archive budget. Version remains 0.7.0.5.

The [dev `363c38cc` integration](UPSTREAM_dev_363c38cc.md) retains the Windows
terrain-sidecar concurrent-release fix. Native sidecar capture/serving remains
unported; native pager and alignment batching behavior is unchanged.

The [dev `ceee11b1` integration](UPSTREAM_dev_ceee11b1.md) wraps native lobby
chat to its measured width, including long links, and retains the newest
messages that fit. In-game Lua chat uses a 52-byte default wrap width.
Version remains 0.7.0.5; live rendering validation remains outstanding.

The [dev `0047c19f` integration](UPSTREAM_dev_0047c19f.md) adds shared cargo-filter
wire/replay support but leaves native filter capture unported. Linux line
edits can still lose stop filters. Version remains 0.7.0.5; no loaded-game
cargo-filter parity has been demonstrated.

The [dev `06188ea5` integration](UPSTREAM_dev_06188ea5.md) keeps
command stamps ahead of the fastest peer while a joiner catches up, with a
600-unit sanity cutoff. Shared Lua tests cover the change; no live multiplayer
result is claimed. Version remains 0.7.0.5.

The [dev `f9d34252` integration](UPSTREAM_dev_f9d34252.md) retains Windows’
corrected packed cargo-flag reader and tests shared numeric flag transport.
Native Linux cargo capture remains unported: static layout evidence was
rechecked, but the lab failed before startup. Linux line edits can still
lose stop filters. Version remains 0.7.0.5.

The [dev `bef70213` integration](UPSTREAM_dev_bef70213.md) makes native OPEN LOGS
include the newest startup archive’s mod `*.log` files as `previous_run_*`,
so the crashed run’s host, terrain and bridge diagnostics accompany its dumps.
Version remains 0.7.0.5.

The [dev `ad36a976` integration](UPSTREAM_dev_ad36a976.md) replaces Lua cargo-filter
setters with a native replay request. The Windows writer is retained; the
Linux writer remains unported after static RE and a lab startup failure.
Native cargo capture and replay can still lose stop filters. This supersedes
the earlier `4ccdde5d` mock-based replay claim. Version remains 0.7.0.5.

The [release 0.7.0.6 integration](UPSTREAM_dev_4e857780.md) advances the native package
and shared lobby handshake to **0.7.0.6**. All peers, including dedicated
servers, must update. This commit only stamps earlier changes: native cargo
filter capture/replay and terrain-sidecar capture/serving remain unported.
The upstream cargo-filter and repeated-load validation does not establish
native Linux support; existing gameplay-validation limits still apply.

The [dev `4617fb6f` integration](UPSTREAM_dev_4617fb6f.md) advances
native Linux to **0.7.0.7 / FPT6**, retains commands owed to quiet members,
adds adaptive retransmission and pacing fixes, and displays the version in
the native panel. All peers must update. Direct installers now accompany
the launchers on the version page. Existing native feature limits remain.

## Fantasia generator buffer reuse

The native Big Maps plugin now applies buffer reuse automatically when a
Fantasia map has area above 8193² heightmap samples (128 x 128 tiles,
32 x 32 km). Generator dimensions are `64 * tiles + 1` samples. Enable the normal Fantasia
Workshop mod; no additional low-memory mod is needed. `generator_memory=1`
is the default in the `[tpf2_bigmap]` configuration section; set it to `0`
and restart to disable it. Fantasia's files remain unchanged.

If you installed the earlier `tpf2_bigmap_fantasia_low_memory_1` stand-in,
disable it in the mod list before using the normal Fantasia generator. The
old installer and stand-in files were retired upstream. Check the plugin log
for `generator memory: ... served with buffer reuse`. See
[integration evidence](UPSTREAM_dev_5d73f324.md); native rendered terrain and
peak memory still need live validation.

`generator_memory_budget_pct=50` lets Fantasia spend half of available RAM on
buffers to preserve generation parallelism; values above 90 are capped, and
0 (or negative) requests the fewest buffers. Linux samples `MemAvailable`
at each generator open, additionally capped by `CommitLimit - Committed_AS`
when `vm.overcommit_memory=2`. Failed queries fall back to the fewest buffers.
This is a buffer scheduling budget, not a hard limit on game memory: values
alive together may require more buffers. Stock generators retain their
fewest-buffer path, and maps at/below 128 x 128 tiles remain unchanged.

The [dev `cc0981bb` integration](UPSTREAM_dev_cc0981bb.md) adds the dedicated-server
archive and hosting guide, spreads shared construction checks across updates,
and ports the resync hold fix to Linux: no ten-second deadline, game-local
SDL gesture diagnostics, and focus-loss cleanup. Version remains 0.7.0.7.
Offline regression tests cover these changes; no new live gameplay result is claimed.

The [dev `3cc80874` integration](UPSTREAM_dev_3cc80874.md) records upstream merging
the completed Linux ports through `cc0981bb` back into Windows history. Its
source tree is identical to the previous Linux integration; runtime behavior
and version 0.7.0.7 are unchanged. Lua now matches upstream without exceptions.

The [dev `d9196011` integration](UPSTREAM_dev_d9196011.md) preserves
the release tag when publishing and checks the returned tag afterward, failing
loudly on a mismatch. This shared tooling change leaves native runtime behavior
and version 0.7.0.7 unchanged. Publication checks are tested offline.

The [dev `24b8f636` integration](UPSTREAM_dev_24b8f636.md) guards shared Fences compatibility
loading so a missing or failing module disables multiplayer without aborting
game loading. Offline Lua and native tests pass; no live gameplay result is
claimed. Version remains 0.7.0.7.

The [dev `ea15a156` integration](UPSTREAM_dev_ea15a156.md) batches native guarded page
reads and avoids scanning whole-world vectors for a single road edge. Name
slot pairs are copied in one guarded read. Version remains 0.7.0.7; local
validation uses memory fixtures and ELF checks, with no in-game timing claim.

Native dedicated servers now default to descriptor-set recycling when
`dedicated_render=0`; `dedicated_recycle_sets=0` disables it. Ordinary rendered
sessions do not use it. See [dev 84058da7](UPSTREAM_dev_84058da7.md) for the
verified dispatcher correction and live-test limitations.

### Integration through dev f0212c87

[Integration and test record](UPSTREAM_dev_f0212c87.md). Release remains 0.7.0.7.
The shared multiplayer toolbar button is included with both icon resolutions.
Native Sandbox town-tool capture and the minimap (including climate colors and
M shortcut) remain unported; do not place towns with the native Sandbox tool
in multiplayer sessions. Shared TOWNC replay is present but cross-platform town
determinism was not tested live.

The native save compressor defaults to four libzstd workers (limited by CPU
count); `save_threads=0` disables it, or use 1..16 to choose a worker count.
It falls back to the game's compressor when byte checks or system libzstd support
fail. The Steam polling hook defaults to a 2 ms interval while busy;
`steam_poll_ms=0` disables it, and positive values clamp at 100 ms. The existing
200 ms idle wait is retained. These settings use `tpf2_menu_flags.txt` in the
native module root, falling back to its data folder when the root file is absent.
Restart the game after changing these startup flags. Local lab startup was blocked;
no live saving speedup, polling cost or toolbar appearance is claimed.

The [dev `ac3b4be3` integration](UPSTREAM_dev_ac3b4be3.md) records
upstream merging the completed Linux ports through `412aeb8e` back into Windows
history. Its source tree matches the preceding Linux integration; runtime
behavior and release **0.7.1.1** are unchanged.

The [dev `bde31323` integration](UPSTREAM_dev_bde31323.md) brings the
company registry rewrite, rebuilt COMPANIES tab and native free-color tints.
Release remains 0.7.1.1. Native and shared regression tests pass; the lab launch
was blocked before game startup, so live gameplay validation remains outstanding.

The [dev `0871bfa6` integration](UPSTREAM_dev_0871bfa6.md) retains Windows' complete-sidecar
alignment bypass. Native Linux sidecar serving and this bypass remain unported;
Linux continues its existing alignment path. Static publication metadata was
verified, but the lab failed before startup, preventing live lifetime proof.

The [dev `c8dd5157` integration](UPSTREAM_dev_c8dd5157.md) adds Linux offline coverage for
threaded terrain-sidecar encoding and POSIX file handling. Native game-side
capture/serving remains unported; `terrain_sidecar_threads` has no native
runtime effect. No native save-time improvement is claimed.

The [dev `fdfb79e8` integration](UPSTREAM_dev_fdfb79e8.md) retains
Windows range tracking for multiple terrain versions. Native sidecar serving
and alignment bypass remain unported after fresh static analysis and a lab
startup failure; native alignment behavior is unchanged.

The [dev `2b4fd093` integration](UPSTREAM_dev_2b4fd093.md) adds experimental
native terrain sidecar capture/serving and a complete-grid alignment bypass.
This supersedes the historical implementation-absence statements above.
`terrain_sidecar=0` remains the native default because live terrain lifetime and
load completion are unverified. `terrain_sidecar=1` enables lab trials, with
`terrain_sidecar_write=1` and `terrain_sidecar_threads=0` (automatic, at most 8).
The current lab fails before game startup; no save/load speedup is claimed.

The [dev `5b817efb` integration](UPSTREAM_dev_5b817efb.md) keeps a terrain sidecar
while another served terrain version awaits its pass. Each version skips only
once; a pass that runs releases the file. Native sidecars remain experimental
and default off. Live validation was blocked before game startup.

The [dev `926b9a2c` integration](UPSTREAM_dev_926b9a2c.md) sorts vehicles at a stop and
unload queues by entity ID on native Linux. All peers need these changes;
release remains 0.7.1.1. Static ELF and native regressions pass; the lab
failed before game startup, so live join validation remains outstanding.

The [dev `9602a389` integration](UPSTREAM_dev_9602a389.md) adds experimental
terrain streaming. `terrain_stream=1` (default) reads the host's growing sidecar
only when `terrain_sidecar=1` is explicitly enabled. `terrain_stream=0` disables
stream reading, not lobby sending. Native sidecars remain off by default;
live terrain ownership and lifetime validation are still outstanding.

The [dev `40e12f76` integration](UPSTREAM_dev_40e12f76.md)
adds `terrain_sidecar_read_local=0` for stream-only testing when both peers can
see the host's save folder. The default is 1; native sidecars remain experimental
and require `terrain_sidecar=1`. With local reads and `terrain_stream` both off,
the normal terrain computation runs. This switch does not disable sidecar writes.

The [dev `b6d73041` integration](UPSTREAM_dev_7e3d3bfa.md) retains Windows’
4 GiB maximum automatic free-commit threshold and `commit_tight_mb` override.
Native terrain paging continues to use `MemAvailable`; this Windows setting
has no native effect. Release remains 0.7.1.1.

The [dev `7e3d3bfa` integration](UPSTREAM_dev_7e3d3bfa.md) retains the Windows material-index
optimization and DLL-map profiler support. Native material-index acceleration
remains unported: the Linux selection loop is inlined, and the lab failed
before startup, preventing buffer-lifetime proof. Release remains 0.7.1.1.

The [dev `8066c58f` integration](UPSTREAM_dev_8066c58f.md) retains the default-off Windows
material-index measurement probe. Native `material_index_probe` remains
unported after static investigation and a lab startup failure; Linux produces
no `material_probe.txt`. Windows measurements do not establish native tile
hashes or compression sizes. Release remains 0.7.1.1.

The [dev `65302e5d` integration](UPSTREAM_dev_65302e5d.md)
adds native parallel sidecar decoding at the alignment pass and retains the
file for both terrain versions. Sidecars remain experimental and off by default;
fixture tests pass, but the lab could not start for live validation.

The [dev `a2e47f2c` integration](UPSTREAM_dev_a2e47f2c.md) adds Windows-only
profiling diagnostics; it changes no native installation, settings or runtime
behavior. See the [tooling scope](../re/linux/DEV_A2E47F2C.md) before using
these tools with native Linux captures.

The [dev `7bace802` integration](UPSTREAM_dev_7bace802.md) retains Windows load-speed
findings, including the rejected material-index chunk-size experiment. These
are upstream measurements; native runtime behavior and release 0.7.1.1 are unchanged.

The [dev `0eb9eea2` integration](UPSTREAM_dev_0eb9eea2.md) lowers Windows’ automatic
free-commit threshold to 2..3 GiB (unknown RAM: 3 GiB). Native terrain paging
retains its existing `MemAvailable` policy; `commit_tight_mb` has no native
effect. Release remains 0.7.1.1.

The [dev `a896a1cb` integration](UPSTREAM_dev_a896a1cb.md) fixes the native plugin host’s
rejection of 5–13-byte hooks, including the terrain sidecar’s 13-byte AddTile
hook. Release remains 0.7.1.2; local lab startup was blocked before the game ran.
