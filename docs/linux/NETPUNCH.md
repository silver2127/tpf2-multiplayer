# netpunch on Linux

The lobby (`netpunch/lobby.py` and everything it imports: NAT traversal, roster, chat, the
game-frame relay, the save transfer) runs on Linux from the same sources. The game starts it as a
frozen program: the folder `netpunch/`, which holds the executable `netpunch` and its `_internal/`
libraries, built with PyInstaller by `tools/linux/build_netpunch.sh`. The protocol and the file IPC
are unchanged ([NETWORKING.md](../NETWORKING.md)). Every Linux-specific line sits behind
`sys.platform != "win32"` (one behind `sys.platform.startswith("linux")`), so `netpunch.exe` and the
dedicated relay behave as before. One fix to the host's stop path applies on every platform
(section 1).

This page covers what changed, where the lobby looks for things, how the game must start and stop
it, how to build it, and what was and was not tested. Everything was measured on 2026-09-12 on
Ubuntu (kernel 7.0, glibc 2.43) with Snap Steam and game build 35924. The sources ran with the
host's Python 3.14.4, the frozen build with python-build-standalone CPython 3.12.14.

Status labels: **MEASURED** means it was seen on that machine by a test or a probe named below.
**INFERRED** means it rests on documentation, naming or other distributions' layouts, and was
not run.

## 1. What changed

| file | change |
|---|---|
| `netpunch/linuxpaths.py` (new) | All Linux path logic, stdlib only: the data folder, Steam roots and libraries, the game folder, Workshop and userdata folders, the names scrubbed from desync reports, the CA-bundle fallback, and the environment for a program the frozen build starts. `python3 linuxpaths.py` prints what a machine resolves to. |
| `netpunch/modshare.py` | `steam_root`, `game_dir`, `userdata_mods_dir` and `workshop_dir` take their Linux answers from `linuxpaths`. The Windows code is untouched. |
| `netpunch/desynclogs.py` | Data folder: the `datadir_linux.h` chain. `stdout.txt` and `settings.lua` are looked for in every Steam root. On Linux the scrub also replaces `/home/<user>` (the name up to the next `/` or whitespace) and the login, home-folder and host names. The standalone `main` applies the CA fallback. |
| `netpunch/observe.py` | The `upnpc` CLI fallback is labelled `upnpc` (not `upnpc.exe`). A frozen build restores the caller's `LD_LIBRARY_PATH` before running it. |
| `netpunch/lobby.py` | New `--print-public-list URL` and `--parent-pid PID`. SIGTERM leaves like a quit, once, and is ignored while the lobby is already leaving. A stop that comes before the lobby loop is up exits 130 with a log line instead of a traceback. The CA fallback runs at startup. When the kernel caps the UDP receive buffer below 4 MB, one log line says so. |
| `netpunch/netpunch-linux.spec` (new) | The PyInstaller spec: onedir (`netpunch` plus `_internal/`), no UPX, explicit hidden imports. |
| `tools/linux/build_netpunch.sh` (new) | Builds `netpunch/dist-linux/netpunch/` with a pinned python-build-standalone CPython, checks what went in and that nothing needs a glibc newer than 2.31, and with `--test` runs the self-tests with the executable. |

On every platform, in `cmd_host`: the `try/finally` that sends `/leave` and removes the UPnP
mapping now starts as soon as the observe step returns, and a stop inside the observe step removes
the mapping before it ends the process. Before, a stop between the two left the mapping on the
router; miniupnpc adds it with an empty lease time, which routers usually keep for good. On Windows
the lobby only gets there through Ctrl+C in a console, because the menu DLL ends it through its Job
object.

Unchanged, and passing on Linux: `punch.py`, `mesh.py`, `seal.py`, `connect.py`,
`masterserver.py`. The `SIO_UDP_CONNRESET` ioctl was already guarded by
`hasattr(socket, "SIO_UDP_CONNRESET")`, which is false on Linux. A Linux UDP socket that is not
connected does not report ICMP port-unreachable on a later receive, so there is nothing to switch
off.

## 2. Where things are

### 2.1 The mod's folders

The lobby resolves the same chain as `native/linux/src/datadir_linux.h` and the Lua mod
(`K.BASE`, `CM.netDir`):

1. `TPF2MP_DATADIR`, when it is an absolute path;
2. otherwise `$XDG_DATA_HOME/tpf2mp/data`, when `XDG_DATA_HOME` is absolute;
3. otherwise `$HOME/.local/share/tpf2mp/data`.

The lobby's own folder (its IPC files, where `CM.netDir` looks for `lobby_out.jsonl`) is the
`netpunch/` sibling of `data/`.

MEASURED: inside the steam snap, and in a Steam Runtime container started from it, `HOME` is
`/home/<user>/snap/steam/common` and `XDG_DATA_HOME` is `/home/<user>/snap/steam/common/.local/share`.
So the data folder is `~/snap/steam/common/.local/share/tpf2mp/data`, which the native libraries
already use, and the lobby's folder is `~/snap/steam/common/.local/share/tpf2mp/netpunch`.

### 2.2 Steam and the game

Steam roots are every folder with a `steamapps/` in it, first match first, with symlinks resolved
and duplicates dropped:

1. `TPF2MP_STEAM_ROOT`
2. `STEAM_COMPAT_CLIENT_INSTALL_PATH`
3. `$XDG_DATA_HOME/Steam`
4. Then, for `$HOME`, the account's home from the password database, and `$SNAP_REAL_HOME`:
   - `.steam/root`
   - `.steam/steam`
   - `.local/share/Steam`
   - `snap/steam/common/.local/share/Steam` (the snap, seen from outside)
   - `.var/app/com.valvesoftware.Steam/.local/share/Steam` and `.../data/Steam` (Flatpak)

The game folder is found in this order:

1. `TPF2MP_GAME_DIR`, if it holds the `TransportFever2` binary.
2. The folder above the lobby, or the lobby's own folder.
3. Every library of every root. Libraries come from `steamapps/libraryfolders.vdf` and
   `config/libraryfolders.vdf`, in both the current format and the pre-2021 `"1" "/path"` format.
   In each library the lobby checks `steamapps/common/<installdir>`, taking `installdir` from
   `appmanifest_1066780.acf` (default `Transport Fever 2`).

The Workshop folder is `steamapps/workshop/content/1066780` in the library that holds the game.
The mods folder is `userdata/<account>/1066780/local/mods` of the newest account that has a `save/`
folder, across all roots. Windows uses the same rule.

MEASURED on this machine:

- **Steam root:** `~/snap/steam/common/.local/share/Steam`. Inside the snap, `~/.steam/root` links
  to it. From a normal shell it is found through the password-database home.
- **Library:** one, listed in both vdf files.
- **installdir:** `Transport Fever 2`.
- **Game log:** `userdata/125253817/1066780/local/crash_dump/stdout.txt`, the same place as on
  Windows. `desynclogs --dry-run` gathered it together with `settings.lua` and six files from the
  data folder.
- **Workshop:** 17 items under the root's `steamapps/workshop/content/1066780`.

INFERRED, and tested only on fake folder trees:

- native (non-snap) and Flatpak Steam
- several libraries
- a Workshop item in a second library
- `STEAM_COMPAT_CLIENT_INSTALL_PATH`: Steam sets it for compatibility-tool launches, but it was not
  seen for this game

### 2.3 Environment

| variable | effect |
|---|---|
| `TPF2MP_DATADIR` | the data folder (absolute paths only, as in `datadir_linux.h`) |
| `XDG_DATA_HOME`, `HOME` | the data and lobby folders; Steam roots |
| `TPF2MP_STEAM_ROOT` | a Steam root to search first |
| `TPF2MP_GAME_DIR` | the game folder, ahead of every search: the launcher should set it to the game's working directory |
| `SNAP_REAL_HOME` | also searched for Steam roots and scrubbed names |
| `SSL_CERT_FILE`, `SSL_CERT_DIR` | when either is set, the CA fallback leaves OpenSSL alone |
| `TPF2MP_LOG_IPS`, `TPF2MP_REPORT_URL` | as on Windows |

### 2.4 CA certificates

A frozen build carries its own OpenSSL, which looks only where it was compiled to look. The
python-build-standalone CPython the build uses has OpenSSL built in and looks in
`/etc/ssl/cert.pem` and `/etc/ssl/certs` (MEASURED with `ssl.get_default_verify_paths()`). When
neither holds certificates (no file, no hashed names), `linuxpaths.ssl_cert_fallback` sets
`SSL_CERT_FILE` to the first bundle that exists: `/etc/ssl/certs/ca-certificates.crt`,
`/etc/pki/tls/certs/ca-bundle.crt`, `/etc/ssl/ca-bundle.pem`, `/etc/ssl/cert.pem`, or one of the
same under `/run/host`. The lobby logs it when that happens.

MEASURED: nothing had to change on the Ubuntu host or in the soldier container. In the container
`/etc/ssl/certs` held 136 hashed names, all of them readable, and there was no `/etc/ssl/cert.pem`.
HTTPS to the master server worked in both.
INFERRED: Fedora and Arch provide `/etc/ssl/cert.pem`, so they need nothing either. Neither was run.

## 3. How the game starts and stops it

This is the contract for the Linux menu code (`native/linux/src/lobby_linux.{h,cpp}`). It replaces
`LobbyThread` and `QuitLobbyProc` in `native/src/menu_hook.cpp`.

### 3.1 What a process started by the game sees (MEASURED)

- **Steam's launch line** (`logs/console-linux.txt`):
  `steam-launch-wrapper -- reaper SteamLaunch AppId=1066780 -- SteamLinuxRuntime_soldier/_v2-entry-point --verb=waitforexitandrun -- SteamLinuxRuntime/scout-on-soldier-entry-point-v2 -- "Transport Fever 2/run.sh"`.
  The game runs in the soldier container with the scout runtime layered on top, and `run.sh` puts
  `.` (the game folder) in front of `LD_LIBRARY_PATH`.
- **Python:** the container has no `python3`, so only the frozen program can run there. The
  Windows fallback to `python lobby.py` does not exist on Linux.
- **glibc:** the host's (2.43) is used inside the container here. pressure-vessel takes the newer
  of the host's glibc and soldier's 2.31, so on an older host the lobby gets as little as 2.31
  (section 4).
- **Namespaces:** the network and PID namespaces are the host's. This was measured with the
  runtime's `run` script, from the host and from inside the snap. Steam's `_v2-entry-point` was not
  run, see section 6. `--parent-pid` does not depend on it: the game and the lobby share whatever
  namespace the game is in.
- **TMPDIR:** unset inside the container. The onedir build unpacks nothing, so it does not matter.
- **The libraries on `LD_LIBRARY_PATH`:** the lobby ran with the game folder, the scout pinned
  libraries and the scout runtime on it (17 entries). The bootloader puts `_internal/` first and
  keeps the caller's value in `LD_LIBRARY_PATH_ORIG`. The bundled files need only glibc's own
  libraries (`libc`, `libm`, `libdl`, `libpthread`, `librt`, `libutil`), plus `libz.so.1` for the
  bootloader.

### 3.2 Launch

1. **The program.** `<lobby folder>/netpunch` (section 2.1), mode 0755, with `_internal/` beside
   it. Build it with section 4 and deploy the folder's contents there. There is no Python fallback.
   Without `_internal/` the executable writes `[PYI-<pid>:ERROR] Failed to load Python shared
   library ...` to stderr and exits (MEASURED).
2. **Working directory and `--io-dir`.** Both are the lobby folder, the one `CM.netDir` probes.
3. **Before the start.** As `LobbyThread` does, delete `lobby_out.jsonl` and `lobby_in.jsonl`,
   and for a join also `incoming_save.sav`, `.sav.lua` and `.jpg`.
4. **Argument vector, not a shell string.** Lobby names may contain spaces and `'`.
5. **Standard streams.** stdout and stderr go to `<lobby folder>/lobby_proc.log` (created and
   truncated); stdin comes from `/dev/null`. Never hand it a pipe that nobody drains: a full pipe
   blocks the lobby's logging.
6. **Its own session** (`setsid`), which is mandatory: the program does not make a process group of
   its own (section 3.4), and stopping it signals the group. Pass no file descriptors from the game
   beyond 0-2, or the game's sockets and devices stay open in the lobby. Close them in the child
   (`syscall(SYS_close_range, 3, ~0U, 0)`, falling back to a `close` loop). After `fork` in the
   multithreaded game process, the child may only make async-signal-safe calls: build argv and envp
   before forking.
7. **Environment.** The game's own, minus `LD_PRELOAD` (`boot.cpp` already drops itself from it),
   plus `TPF2MP_GAME_DIR=<the game's working directory>`.
8. **Readiness.** The lobby is ready once the first line appears in `lobby_out.jsonl`. It
   truncates `lobby_in.jsonl` at startup, so a command appended before that line is lost, exactly as
   on Windows (`g_lobbyReady`).

### 3.3 Arguments

These are the Windows ones plus `--parent-pid <the game's getpid()>`:

```
netpunch host --name <player> --game-relay-port <7773 or free> --game-local-port <bridge port>
              --forward-log <data>/tpf2_bridge.log --forward-log <data>/tpf2_menu.log
              [--password <pw>] --lobby-name <lobby> [--publish <master url> [--public]]
              [--no-share-mods] --parent-pid <pid>
netpunch join <CODE> --name <player> --local-port 0 --game-relay-port <7774 or free>
              --game-local-port <bridge port> --forward-log ... [--password <pw>] --parent-pid <pid>
```

The bridge port is the one the bridge reports, as `readBridgePort()` reads it on Windows.

### 3.4 Stopping

1. Append `{"cmd":"quit"}` to `lobby_in.jsonl` and give the lobby a moment to exit. Poll
   `waitpid(pid, WNOHANG)`.
2. Then `kill(-pid, SIGTERM)`, to the process group. Wait about 2 s.
3. Then `kill(-pid, SIGKILL)`, and reap it with `waitpid`.

What the program does, MEASURED with PyInstaller 6.22.3:

- **One process.** The onedir bootloader runs Python in its own process: the pid the launcher got
  is the lobby, it has no child process, and `/proc/<pid>/exe` stays the `netpunch` executable. A
  SIGKILL leaves nothing behind.
- **No process group of its own.** The bootloader does not create a process group, so the launcher
  must, with `setsid()` or `setpgid(0,0)` in the child before exec. Started from `sh -c` without
  either, the onedir program had the shell's process group. The review of the earlier onefile build
  measured the same for its bootloader and that bootloader's Python child. `kill(-pid)` on a
  process that leads no group fails with `ESRCH`.
- **Why the group anyway.** It also holds whatever the lobby starts, such as the `upnpc`
  command-line fallback.
- **SIGTERM** leaves like a quit: a joiner sends its leave, and a host sends its byes, then `/leave`,
  then removes the UPnP mapping. Only the first SIGTERM counts. Later ones are ignored: the group
  signal, `PR_SET_PDEATHSIG` and the `--parent-pid` watch can all arrive.
- **SIGTERM while already leaving** is ignored, and the cleanup goes on. This is step 2 after the
  quit of step 1 was read. A host's cleanup then waits for `/leave` (one POST, up to 6 s) and
  removes the UPnP mapping (a blocking discover of at least 1 s).
- **SIGTERM before the lobby is up**, while observing NAT or dialling the host, ends the program
  with exit status 130 and `[lobby] stopped before the lobby was up` in its log. A host removes its
  UPnP mapping first. This is a common case: `lobby_in.jsonl` is truncated when the lobby starts,
  so a quit sent before the first `lobby_out.jsonl` line is lost, and step 2 does the stopping.

MEASURED with the frozen executable (`test_lifecycle_v2`):

- (a) After a group SIGTERM to a joiner in the lobby, the host logged its LEAVE. The joiner exited
  0 within 5 s, and nothing of the group was left.
- (f) After a group SIGTERM to a joiner still dialling a port nobody answers on, it exited 130
  after 0.02 s, with the log line and no traceback.

MEASURED from the sources with stand-ins (`test_host_stop`). The observe step and the UPnP unmap
were replaced, and a local master answered `/leave` after 2.5 s:

- (h) Group SIGTERM while observing: the unmap ran to its end, then the host exited 130 with the
  log line.
- (i) Quit, then a group SIGTERM 1.5 s later, while the host was waiting for `/leave`: `/leave`
  arrived, the unmap ran, and the host exited 0, 1.7 s after the SIGTERM.
- (j) Two SIGTERMs 1 s apart in a running lobby: the same, exit 0.

INFERRED, because a real host was not run: a real UPnP discover and a slow master can make the
host's cleanup outlast the quit wait plus the SIGTERM wait (1.5 s and 2 s in `lobby_linux.cpp`).
Step 3 then cuts it short, as `TerminateJobObject` did on Windows after 1.5 s.

### 3.5 If the game dies

`--parent-pid` watches the game's `/proc/<pid>/stat` once a second and compares the start time, so
a reused pid is not mistaken for the game and a zombie counts as gone. Once the game is gone the
lobby sends itself SIGTERM, which takes the leave/bye path. A `--parent-pid` that is not running at
startup makes the lobby exit 1 without joining.

MEASURED (`test_lifecycle_v2` (b), (c), source and frozen): when the parent exited, the host logged
LEAVE, the joiner exited 0 within the 6 s bound, and its log gave the reason. With a dead pid, it
exited 1.

INFERRED: Steam's `reaper` and pressure-vessel's `pv-adverb` wait for, or terminate, whatever is
left of the game's process tree. A lobby that outlived the game would keep the game "running" in
Steam, or be killed without a leave. The watch avoids both. `PR_SET_PDEATHSIG` is no substitute:
it fires when the forking *thread* exits, not the process.

### 3.6 Server browser: `--print-public-list`

`netpunch --print-public-list <master url>` does a GET of `<master url>/list` (5 s timeout, the
panel's WinHTTP value) and behaves like this:

- **HTTP 200:** the body goes to stdout and the exit status is 0.
- **Any other status:** one line on stdout, `HTTP <code>`, and exit status 1.
- **Any other failure:** the error on stdout, for example `[Errno 111] Connection refused` or
  `unknown url type: ...`, and exit status 1.
- **stderr:** the lobby writes nothing there, even when the reader closes the pipe early (the exit
  status is then 1). The bootloader writes `[PYI-<pid>:ERROR] ...` lines there when the program
  cannot start at all, for example without its `_internal/` folder.
- **Reading:** read stdout to its end. A list at the master's 500-row limit is about 170-250 KB (the
  test's 500 rows came to 232 KB). That is more than a pipe holds, so a reader that stops early
  blocks the program until it is killed.

This matches what `httpGet()` in `menu_hook.cpp` reports. A start-to-exit run against a local server
took 0.07 s on this machine, and 130 ms over HTTPS to the real master inside the soldier container
(MEASURED), so the panel's 10 s poll can simply run it. Nothing is unpacked or left behind per run.
Name resolution is not covered by the timeout.

## 4. Building

```
tools/linux/build_netpunch.sh [--build-dir DIR] [--dist-dir DIR] [--python PYTHON] [--test]
```

- **Output:** `netpunch/dist-linux/netpunch/`, holding `netpunch` and `_internal/` (the folder
  goes under `--dist-dir`/`DIST_DIR` instead when given). The downloaded Python, the venv and
  PyInstaller's work files go to `netpunch/build-linux/` (or `--build-dir`/`BUILD_DIR`).
  PyInstaller is pinned to 6.22.3 (`PYINSTALLER_VERSION`).
- **The glibc floor.** The game runs with the newer of the host's glibc and soldier's 2.31, so the
  lobby has to start on 2.31. A distribution's Python does not give that. The earlier build, made
  with Ubuntu's Python 3.14, needed `GLIBC_2.38` in libpython, OpenSSL, libffi, expat, `_decimal`
  and miniupnpc, and could not start on Ubuntu 22.04 (2.35) or Debian 12 (2.36).
- **The interpreter.** By default the script downloads python-build-standalone CPython 3.12.14
  (release 20260901, `install_only_stripped`), checks its sha256 and builds with it. That Python
  needs glibc 2.17 and has OpenSSL built in. 3.12 is the series `netpunch.exe` is built with.
  `PBS_RELEASE`, `PBS_VERSION` and `PBS_SHA256` change the pin. `--python` uses another interpreter,
  and that build then has to pass the same check.
- **Packages:** the pure ones and zstandard come from `requirements.txt`, binary wheels preferred.
  miniupnpc comes as its manylinux2014 wheel, which exists for CPython 3.9 to 3.13. Nothing is
  compiled. Only with `--python` and no wheel for that Python is miniupnpc built from source, which
  needs a C compiler and that Python's headers; without `python3-dev` the script fetches
  `libpython3.X-dev` with `apt-get download` (no root). `ALLOW_NO_MINIUPNPC=1` builds without UPnP.
- **Checks after freezing:**
  - the lobby modules, pystun3 and zstandard are in the archive (`pyi-archive_viewer`), and
    zstandard's C backend and miniupnpc are in `_internal/`;
  - `netpunch --print-public-list https://127.0.0.1:9` has to answer with a connection error, not
    `unknown url type` (no ssl module), a traceback or a bootloader error;
  - every ELF file in the output is read with `objdump -T`, and the build fails when one references
    a glibc symbol version newer than `GLIBC_MAX` (default 2.31). It names the files.
- **`--test`:** runs `--selftest`, `-relay`, `-mesh`, `-mods` and `-transfer` with the executable,
  one at a time because they use fixed loopback ports.

MEASURED builds on this machine took 28 s with the 36 MB Python download and the self-tests, and
24 s with the download cached. The folder is 57 MB in 6 files: the executable,
`libpython3.12.so.1.0`, `base_library.zip`, miniupnpc, and zstandard's C and cffi backends. The
cffi backend is 12 MB; zstandard loads it only when its C backend is missing (INFERRED from
zstandard's import policy), so it could be excluded. The newest glibc version referenced is 2.17.

MEASURED where it runs: a root holding nothing but the Steam Runtime soldier platform's own glibc
2.31 (`Debian GLIBC 2.31-13+deb11u14~steamrt2.1+bsrt2.0.1`) with its libz and libgcc_s, started with
`bwrap`. There the five self-tests passed, and `--print-public-list` reached the real master over
HTTPS. The earlier build failed in the same root: ``version `GLIBC_2.38' not found (required by
.../libpython3.14.so.1.0)``.

## 5. Tests run (2026-09-12)

The throwaway test scripts lived in the area's scratch directory. Their checks are listed here.

| test | what it checks | result |
|---|---|---|
| source self-tests, before any change | `lobby.py --selftest`, `-relay`, `-mesh`, `-mods`, `-transfer`; `seal.py`, `mesh.py`, `punch.py --selftest`, `connect.py selftest`, `modshare.py` | all PASS (Linux needed no fix for these) |
| the lobby self-tests after the changes | Python 3.14.4 | all PASS |
| `build_netpunch.sh --test` | the five lobby self-tests with the frozen executable | all PASS |
| paths on fake trees (39 checks) | root order and symlink de-duplication; a second library with a space in its path; `installdir`; the pre-2021 vdf format; Flatpak, snap-from-outside and `SNAP_REAL_HOME` roots; `TPF2MP_GAME_DIR`, `TPF2MP_STEAM_ROOT`, `TPF2MP_DATADIR` and `XDG_DATA_HOME` (and relative values being ignored); the Workshop folder and a Workshop mod; the newest account with a save; scrubbing; the CA fallback (bundle-only folder, hashed folder, explicit env); `child_env` | 39/39 |
| `desynclogs.py --dry-run` on this machine | no login name, host name or Steam id left in the zip; home paths only as `/home/<user>` | PASS |
| scrub of `loading /home/<login> done in 5 ms, next step` | the text after the name is kept | PASS |
| lifecycle v2 (23 checks), source and frozen | (a) group SIGTERM, and the lobby is one process; (b) `--parent-pid` exit; (c) dead `--parent-pid`; (d) `--print-public-list` against a local master (200), a 503, a closed port, a malformed URL and a 500-row list; (e) the real master over HTTPS; (f) group SIGTERM while dialling: exit 130; (g) a reader that closes after 64 B: exit 1, stderr empty | 23/23 both |
| host stop paths (16 checks), source | (h), (i) and (j) of section 3.4 | 16/16 |
| glibc 2.31 root | the five frozen self-tests (`-mods` reading a synthetic zstd save from a fake root) and `--print-public-list` over HTTPS; the earlier build as the negative control | all PASS; the earlier build fails on `GLIBC_2.38` |
| soldier + scout-on-soldier, started from the host | frozen `--print-public-list` over HTTPS and the five self-tests (`-mods` from a fake root), with the game folder and the scout libraries on `LD_LIBRARY_PATH` | all PASS |
| soldier started inside the steam snap | frozen `--print-public-list` over HTTPS, using `PRESSURE_VESSEL_FILESYSTEMS_RO` to show the build folder | PASS, 130 ms |
| save transfer with `SO_RCVBUF` clamped to 212992 B | internet-safe chunk and window on loopback, 24 MB to 2 peers | OK in 2.0 s, the same as with 4 MB |

How the container runs were started: `SteamLinuxRuntime_soldier/run -- <cmd>` with
`PRESSURE_VESSEL_VARIABLE_DIR` pointing to a temporary folder, so nothing was written into Steam's
folders. The scout layer was a copy of `scout-on-soldier-entry-point-v2` in that folder, with
`STEAM_RUNTIME_SCOUT` pointing to `SteamLinuxRuntime/steam-runtime`. The snap variant was
`snap run --shell steam -c 'sh -s' < script`, with the variable folder in the snap's private
`/tmp`. Neither starts Steam or the game.

How the glibc 2.31 root was made: soldier's `files/` tree stores no soname links (its deployed copy
under `var/` has the host's libc in their place), so a scratch folder got copies of its
`ld-2.31.so`, `libc`, `libdl`, `libpthread`, `libm`, `librt`, `libutil`, `libresolv`, `libnss_files`,
`libnss_dns`, `libz` and `libgcc_s` plus their soname links. It was started with
`bwrap --ro-bind <root> / --clearenv`, with the host's `/etc/resolv.conf`, `/etc/ssl` and
`/usr/share/ca-certificates` bound in.

## 6. Not tested, and limits

- **A real `host`.** Its observe step asks public STUN servers for a mapping and adds, then
  removes, a UPnP port mapping on the router; it was not run. miniupnpc is bundled but was never
  imported by a test. The host's stop paths were run from the sources with stand-ins for the observe
  step, the unmap and the master (section 3.4).
- **Real networks:** a join across real NATs or the internet, IPv6, the desync upload `POST`, and
  mod sharing into a real game folder.
- **The game starting the lobby:** this area did not run `lobby_linux.cpp` with this build. Steam's
  `_v2-entry-point` with app id 1066780 was not run either, because it can attach a command to that
  game's container.
- **Other systems:** the glibc 2.31 root stands in for an old host; no Ubuntu 22.04 or Debian 12
  machine was run. Fedora and Arch (CA paths) and native or Flatpak Steam were not run.
- **UDP buffers.** A stock kernel caps UDP receive buffers at 212992 B. This machine allows 4 MB,
  so the new log line did not fire here. The clamped loopback transfer was fine, but a clamped
  internet transfer is untested.
- **Python versions.** The frozen build is CPython 3.12.14; the sources were tested with 3.14.4,
  and the frozen build passed the same tests. The dedicated relay runs the sources with the
  server's `python3`; an older interpreter was not run.

## 7. For other areas

- **Menu (lobby):** section 3. Poll `--print-public-list` for the server browser and read its stdout
  to the end. The header comment of `lobby_linux.cpp` still describes the onefile build (the
  bootloader passing signals on, `/tmp/_MEI*`). The order it relies on (quit, group SIGTERM, group
  SIGKILL) is unchanged. `<folder>/netpunch` is now the executable of a folder, with `_internal/`
  beside it.
- **Deploy and installer:** copy the contents of `netpunch/dist-linux/netpunch/` (the executable
  `netpunch` and `_internal/`) to `$XDG_DATA_HOME/tpf2mp/netpunch/` (Snap Steam:
  `~/snap/steam/common/.local/share/tpf2mp/netpunch/`). Replace `_internal/` as a whole on update,
  and remove it on uninstall. `build_release.sh` already copies a folder given as the lobby.
- **Logs:** a Linux log archive should include `<lobby folder>/lobby_proc.log` and
  `lobby_peers.log` (Windows `logarchive.h` collects `netpunch\*.log`).
- **Repository:** ignore `netpunch/dist-linux/` and `netpunch/build-linux/`. Link this page from
  `netpunch/README.md` and from the self-test list in `docs/NETWORKING.md`.
