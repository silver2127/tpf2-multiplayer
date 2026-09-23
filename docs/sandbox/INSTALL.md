# Native Linux and Proton test lab

`tools/sandbox/tpf2mp-lab` creates two separate game instances for testing the native
Linux mod against the Windows mod running through Proton. It uses Steam Linux
Runtime 4's bundled `srt-bwrap` container helper and the installed Steam runtimes;
Steam stays running outside the sandbox for its API.

Each instance has its own game executables, multiplayer programs, saves, settings,
runtime data, temporary files, shader caches, and Proton prefix where applicable.
The large `res`, `dlcs`, and other existing mod directories are shared read-only.
Only the multiplayer mod is copied, and its 24 Lua files must match Windows 0.4.22.
The source game and Steam installation are not updated by setup.

## Prepare

Requirements: Python 3.11+, Steam running, the Windows game installed with
the [Proton hosting repair](../proton/INSTALL.md), the installed native multiplayer
build, Proton 11, Steam Linux Runtime 4, and Steam Linux Runtime soldier.
Launch the installed Proton version normally through Steam once before preparing
the lab so its distribution files and default prefix are initialized.

The native game executable and libraries come from the Linux depot. On this installation
the tested build uses app `1066780`, depot `1066784`, manifest
`2694881574364695876`. Steam's console can download it without replacing the Windows
installation:

```text
download_depot 1066780 1066784 2694881574364695876
```

Steam normally stores it beneath
`ubuntu12_32/steamapps/content/app_1066780/depot_1066784` in its installation directory.
Pass that directory to setup:

```sh
tools/sandbox/tpf2mp-lab setup \
  --steam-root '/path/to/Steam' \
  --native-depot '/path/to/Steam/ubuntu12_32/steamapps/content/app_1066780/depot_1066784' \
  --root "$HOME/.local/share/tpf2mp-lab"

tools/sandbox/tpf2mp-lab check --root "$HOME/.local/share/tpf2mp-lab"
```

For Snap Steam, the Steam root is normally
`~/snap/steam/common/.local/share/Steam`. The native multiplayer installation defaults
to its sibling `tpf2mp` directory; `--native-mod /path/to/tpf2mp` overrides that.
Keep both the Steam root and lab root outside `/tmp`; each instance gets a private
temporary directory mounted there.

Setup copies only one account's `1066780/local` directory, selecting the most recently
updated settings when several accounts have played. It does not copy other games or
other accounts' userdata. Repeating setup with the same sources preserves both lab
instances, including test saves. Use a fresh `--root` to create a new snapshot.

Stock mods use real directories with their shared contents mounted read-only.
Windows skips symlinked mod directories, which previously made the Proton actor
recognize fewer mods than native Linux. Existing labs migrate these links before
the actor's next launch. Repeating setup also migrates inactive actors; it defers
actors whose game or Wine processes are still running. No stock mod assets are
copied, and the multiplayer Lua files are preserved.

To retain a disabled mod across both actors, the launcher honors directory names
under either actor's `disabled-mods/`, plus an optional `excluded_mods` list in
`lab.json`. The existing `native/legacy-removal.json` request also excludes Legacy
Vehicles immediately while its removal watcher is waiting. Migration waits for
that watcher before changing the native source link. A disabled mod is moved out
of the other actor's active mod directory on its next launch and is never mounted.
Private stock-mod contents or unfamiliar links cause a clear error instead of
being overwritten. These exclusions do not edit existing saves' mod requirements.

`check` launches no game. It verifies the private userdata overlay, read-only Steam
files, shared loopback networking, Steam SDK visibility, readable shared assets, and
the self-memory access required by the native hooks. Its temporary files are removed.
If stock-mod migration is pending, it asks for an inactive setup or the next launch;
the check itself does not migrate mod directories.
The launcher validates the installed Steam container helper before use. Using this
helper also supports the nested runtime on Ubuntu systems where the system
`bwrap` profile denies nested namespace creation; no host policy is changed.

## Run the pair

Install the desktop shortcuts with:

```sh
python3 tools/sandbox/install_shortcuts.py --root "$HOME/.local/share/tpf2mp-lab"
```

Open **TpF2 Test — Native Linux** and **TpF2 Test — Proton**. These use
`tools/sandbox/launch.py`, which prevents a second launch of the same actor, records
timestamped logs beneath that actor's `logs/`, and reports launch errors through a
desktop notification. `logs/latest-launch.log` points to the most recent log.

Launch each in a separate terminal:

```sh
tools/sandbox/tpf2mp-lab run native --root "$HOME/.local/share/tpf2mp-lab"
tools/sandbox/tpf2mp-lab run proton --root "$HOME/.local/share/tpf2mp-lab"
```

`run ... --dry-run` prints the sandbox command without starting it. The launcher uses
the Steam runtime directly, so the two instances can coexist. Close an instance
normally through its game window; closing a terminal is not a save operation.

Choose **HOST** in one instance and **JOIN** in the other. The network namespace is
shared: the bridges normally claim UDP 7771 and 7772, the host's local relay uses
7773, and the joiner selects 7774 or the next available port. The host lobby listens
on 29471 while the joiner uses an ephemeral port. Two simultaneous hosts on the same
network namespace would compete for the fixed host ports.

Both instances begin with copies of the same local saves and settings, but each
changes only its own copy. Use the multiplayer save-transfer flow to establish a
common starting map, compare initial hashes, then test the actions under investigation.
The lab preserves each platform's existing mod behavior; it does not add missing
strict cancellation coverage. The native decimal-format compatibility fix now
passes the clean geometry and rail comparison; destination-selection compatibility
is being validated separately. See [the checkpoint](../linux/RESUME_STATUS.md).

## Compare people at exact simulation times

The unchanged Windows Lua's `n` diagnostic counts spatially indexed people. In
the tested world these are moving people; people inside buildings and idle people
are excluded. It is not a population census. `tools/sandbox/people_probe.py` reads
the global person counter, every `SIM_PERSON` component, destination choices,
last destination update, spatial membership and available movement-state
components. It also records `PERSON_CAPACITY` callback order. The public API
returns that order sorted in the tested build; it does not establish the order
of the game's internal C++ component storage.

After loading the same clean baseline in both actors, arm targets that are still
in the future on **both** instances:

```sh
python3 tools/sandbox/people_probe.py arm \
  --root "$HOME/.local/share/tpf2mp-lab" \
  --at 60,72,144,300,600 \
  --output "$HOME/.cache/tpf2mp/people-baseline-test"
```

The command queues a diagnostic using the existing `EVAL` entry point. Check
each actor's `userdata/<account>/1066780/local/crash_dump/stdout.txt` for
`PEOPLE_PROBE armed`; queueing alone does not mean arming succeeded. Late arming
and an already-active diagnostic are rejected. Use a new output directory for
each run. `arm --dry-run` prints the request and generated source without writing
or queueing anything.

The diagnostic temporarily wraps `game.interface.getGameTime`, calls the original
function and returns all of its values unchanged. It restores the original
function before each snapshot's read-only API calls, then waits for the next
target; completion, a snapshot error or the clock moving backwards on world
reload removes its wrapper. An upvalue check
rejects another active probe without creating global variables.
It never sends game commands or changes simulation time/speed. Installed mod Lua
is untouched. This instrumentation can briefly stall the engine while reading
and writing a snapshot; it does not force the engine to visit a missed time.

Snapshots use unique request filenames beneath each actor's private `game/`.
Once the desired targets have passed on both instances, compare and archive them:

```sh
python3 tools/sandbox/people_probe.py compare \
  --root "$HOME/.local/share/tpf2mp-lab" \
  --at 60,72,144,300,600 \
  --output "$HOME/.cache/tpf2mp/people-baseline-test"
```

`compare --at 60,72` can examine an earlier subset without waiting for later
targets. Omitting `--at` compares every armed target. Comparison requires the
requested, captured and post-read times to match exactly on both instances;
missing or late snapshots are errors. It archives snapshots and a JSON report
outside the lab, with separate global counts, movement states and per-person
field/destination differences. Exit status is 0 for matching records, 1 for
differences, and 2 for an incomplete or invalid comparison. Equal population
alone does not constitute a passing result.

Run the eight isolated diagnostic regressions with
`python3 tools/sandbox/test_people_probe.py`. Python and Lua5.2 fixtures verify
clock return preservation under the game's strict global-variable policy,
multiple targets, restoration after errors or a world-clock reset, late and
duplicate rejection, JSONL serialization, comparison timing, and dry-run isolation.

## Files and isolation

The lab contains `lab.json` and `native/` / `proton/` directories. In each instance:

- `game/` contains private executables, multiplayer programs, and the multiplayer mod.
- `userdata/<account>/1066780/local/` contains saves, settings, and game output.
- `share/tpf2mp/` contains the native multiplayer programs and runtime data.
- `compatdata/pfx/` contains the independent Proton prefix and Windows mod data.
- `cache/`, `config/`, `tmp/`, `runtime-var/`, and `logs/` hold that instance's runtime files.

The runtime uses a private writable copy beneath `runtime-var/`. Its source
deployment's empty `.ref` lock file is also overlaid with a private file because
Steam's runtime loader opens that lock for writing; the installed runtime contents
remain read-only. Proton's empty distribution lock gets the same treatment.
Steam's shader-cache directory is overlaid with the actor's `cache/steam-shadercache/`.

The sandbox overlays the existing home path without changing the `HOME` environment
variable. It exposes the Steam SDK and control files through a small `.steam` view,
then overlays the game's real Steam install and userdata paths with the private
copies. This covers the Windows game's observed `Z:/.../Steam/userdata/...` path as
well as the menu's registry-based Steam path. A separate read-only game alias keeps shared resource
symlinks valid after the game-directory overlay.

Run the stock-mod layout regressions without an installed game with
`python3 tools/sandbox/test_mod_layout.py`. They use temporary fixtures, including
real bubblewrap mounts when available, and cover discovery, read-only contents,
active-process refusal, and exclusions while the Legacy removal watcher transitions.

GPU devices, display sockets, Steam IPC, and loopback networking remain available.
This is a filesystem-isolated development lab with shared desktop and Steam services,
not a sandbox for running untrusted software. Original saves are not used as the
instances' writable save directories. Delete the lab only after saving any test work
you want to keep; the launcher provides no automatic save deletion.

## Installed comparison on 2026-09-14

The lab at `~/.local/share/tpf2mp-lab` has been launched twice through both runtimes;
both multiplayer menus are available together. Final launches use the same wrapper
as the installed desktop shortcuts. Native hooks report readiness, and the two
bridges selected `a/7771` and `b/7772`. A joined world has not yet been tested in
this lab. The installation occupies about 2.6 GiB, with large game assets shared.

Both copies use windowed settings and contain the original `New Game.sav` from the
native-versus-Windows desync test (18,120,736 bytes, SHA-256
`70c4fb69795e8a428fa67ab77e262949b993c2fc20d986b0dce2517a13776e99`). Host that save
in the native instance and join from Proton, using the normal save-transfer flow.
Compare initial hashes before building, then place a single rail proposal.

The existing `dump_egeo=1` option is enabled in each private `game/tpf2_slice.cfg`.
After hashing a loaded world, exact geometry strings appear in that instance's
`game/egeo_a.txt` or `game/egeo_b.txt`, according to its elected bridge identity.
These files contain the latest sample, so preserve a baseline before the rail test.
No Lua files were edited; all 24 match the Windows 0.4.22 manifest.

The original saves, settings, executable, and monitored multiplayer payload files
remain unchanged. Steam's external client still records play activity in its app
manifest; the installed game depots remain the Windows version.

During the subsequent live multiplayer test, native loaded
`urbangames_legacy_vehicle_pack/1` from the transferred save while Proton omitted
it and substituted missing models. The user requested removing it from native.
Because the native game still had its archives open, removal of that actor's mod
symlink was queued for process exit. `native/legacy-removal.json` records the
pending/completed result; the link is moved to `native/disabled-mods/`. Shared
source assets and the saved dependency are preserved. The live game is not restarted.
Proton's smaller discovered mod count also suggests that the sandbox's symlinked
mod directories need a separate discovery fix; that is not yet confirmed or changed.
