# The game's own log files on Linux (build 35924), for logarchive_linux.h

What `native/linux/src/logarchive_linux.h` and `tools/linux/collect_logs.sh` rely on, with the Linux
evidence. RVAs are file virtual addresses of `TransportFever2` (GNU build-id
`3a0e156390b0e6f1e372051c24802c8493ae454a`). The same facts are summarised at the top of the header.

## Files

All in `<Steam>/userdata/<account>/1066780/local/crash_dump/`:

| file | what it is |
|---|---|
| `stdout.txt` | the game's log, with the Lua script's print lines. Truncated at every start. |
| `stdout_old.txt` | a copy of `stdout.txt` the game takes at start, before truncating it: the run before |
| `lockfile` | the running game's pid; still there after a run that ended without the game's own cleanup |
| `<id>.dmp` | Breakpad minidumps. There is no per-dump `<id>_stdout_old.txt` as on Windows: the binary has no `_stdout_old` string. |

Seen on this machine (Snap Steam, account 125253817): `stdout.txt` and `stdout_old.txt`, no dumps.

## Evidence

**Who calls it, and when.**
- `_start` 0x97a270 hands main to `__libc_start_main`: 0x97a291 `lea rdi, [0x95e6c0]`, 0x97a298
  `call [0x5a462e8]`. 0x5a462e8 is the `R_X86_64_GLOB_DAT` slot of `__libc_start_main@GLIBC_2.2.5`.
- main 0x95e6c0 calls 0x9b3d50 at 0x95e8e3.
- 0x9b3d50 joins `"crash_dump/"` onto the user folder (0x9b4ac1 lea, 0x9b4ad2 call 0x9bfab0) and calls
  0x9b0b10 with it at 0x9b4b22.
- A reverse graph over every direct call/jmp rel32 and RIP-relative lea in `.text` (functions from
  `functions.csv`) finds exactly three functions from 0x9b3d50 up: 0x9b3d50, main and `_start`. None is
  an `.init_array` entry (785 relocated entries, `DT_INIT_ARRAYSZ` 6280), and there is no
  `DT_PREINIT_ARRAY`.
- The ELF entry 0x5bc11a0 is not `_start`. It is a stub in an extra `R E` PT_LOAD at 0x5bc1000 (file
  offset 0x5bc1000, size 0x1812a), outside `.text` and `functions.csv`. It pushes every register,
  calls 0x5bc1250 with its own address, puts the returned address in its return slot, pops, and
  `ret`s there. ld.so runs the constructors of preloaded objects before it jumps to the entry, so the
  loader's constructor (boot.cpp) always runs before any of this.

**0x9b0b10: lockfile, crash marker, stdout_old.txt.**
- It joins `"stdout.txt"` (0x9b0b14), `"stdout_old.txt"` (0x9b0b5d) and `"lockfile"` (0x9b0b7d).
- lockfile exists (0x9b0bc4 -> 0x31f5090 -> `boost::filesystem::detail::status`): it appends
  `"__CRASHDB_CRASH__ Unexpected program termination"` to `stdout.txt` (0x9b0d5e, ofstream with
  `app`) and removes lockfile (0x9b0f0f -> 0x31f6c00 -> `detail::remove`).
- It also has a message for a lockfile whose process is alive: 0x9b12d0 lea
  `"... Process seems to be still running"` (reached from the `jne` at 0x9b0e4b).
- It writes `getpid()` into lockfile: 0x9b1025 `filebuf::open(out)`, 0x9b10a5 -> 0x322d460 `jmp getpid`.
- It copies `stdout.txt` over `stdout_old.txt`: 0x9b1283 `call 0x31f69b0(stdout.txt, stdout_old.txt, 1)`
  (edx = 1 at 0x9b1278). 0x31f69b0 tests that flag (0x31f69ee `test dl, dl`, 0x31f69fc `je`). For a
  nonzero flag it calls `boost::filesystem::detail::copy_file(path const&, path const&, unsigned int,
  error_code*)` (PLT 0x6dceb0) at 0x31f6a33 with options 2 (0x31f6a28 `mov edx, 2`). The `unsigned int`
  signature is the boost >= 1.74 API, where 2 is `copy_options::overwrite_existing`.
- The flag at 0x5a4f64c is set at 0x9b0bff after an unclean end.

**0x9b3d50 then truncates stdout.txt.**
- It joins `"stdout.txt"` again (0x9b4c90), opens it (0x9b4cd7 `call 0x9b7a50`) and points the
  standard streams at it (0x9b4d0a, 0x9b4d2b `basic_ios::rdbuf`).
- 0x9b7a50 builds an ofstream (0x9b7a88 `ios_base::ios_base`, 0x9b7b29 `basic_filebuf` constructor).
  It calls `basic_filebuf<char>::open(const char*, openmode)` at 0x9b7b4c through PLT 0x6dc020 (the
  `R_X86_64_JUMP_SLOT` at 0x5a46e90 names `_ZNSt13basic_filebufIcSt11char_traitsIcEE4openEPKcSt13_Ios_Openmode`),
  with edx = 0x10 (0x9b7b44).
- 0x10 is `ios_base::out` alone, which libstdc++ opens with `fopen` mode `"w"`: `O_TRUNC`.
- Live data agrees. `stdout.txt` (inode born 17:32:58) was last written 18:12:38 and holds only the
  last run. `stdout_old.txt` was written 18:12:21, and the two files differ from byte 477.

**Other functions.**
- 0x9afb90 removes lockfile (0x9afbdf lea, 0x9afc6d -> 0x31f6c00). It is called from Run 0x6e0aad
  (0x6e11df), from the "Minidump Callback" function 0x9b1450 (0x9b1633) and from 0x9b3d50 (0x9b57e8).
- Run2 0x9b1a90 joins `"crash_dump/"` at 0x9b1c62 into `[rbp-0xf80]`. After an unclean end it reads
  `stdout_old.txt` line by line (0x9b2e47, 0x9b3071 `filebuf::open`, 0x9b312e getline) for
  `"__CRASHDB_DUMP__ "` (0x9b3866), and joins `"<id>.dmp"` (0x9b364f) onto that folder (0x9b368a,
  0x9b36dc `path::operator/=`).

## The Snap Steam environment

Read from `/proc/<pid>/environ` and `/proc/<pid>/attr/apparmor/current` of a running Snap Steam
(snap revision 271):

| process | `HOME` | `XDG_DATA_HOME` | AppArmor label | pid namespace |
|---|---|---|---|---|
| steam client (19677) | `~/snap/steam/common` | `~/snap/steam/common/.local/share` | `snap.steam.steam (enforce)` | `pid:[4026531836]` |
| srt-bwrap / steamrt64 (19730) | same | same | `snap.steam.steam (enforce)` | `pid:[4026531836]` |
| pressure-vessel tool (19806) | same | same | `snap.steam.steam (enforce)` | `pid:[4026531836]` |

`pid:[4026531836]` is also the namespace of a host shell, so pressure-vessel shares the host pid
namespace.
- The game inherits this environment, so the run.sh block's `$XDG_DATA_HOME` case resolves to
  `~/snap/steam/common/.local/share/tpf2mp/libtpf2mp_boot.so`. The first, hand-made block used
  `$HOME/.local/share`, which is the same folder.
- The game runs confined. The kernel mediates ptrace (`/sys/kernel/security/apparmor/features/ptrace/mask`
  = `read trace`), and `/var/lib/snapd/apparmor/profiles/snap.steam.steam` has no ptrace allow rule:
  line 90 `#audit deny ptrace (trace)` is commented out, and lines 196-197 say `ptrace (read)` is
  denied by default. `snap connections steam` shows `system-observe`, the interface that grants
  `ptrace (read)`, as not connected.
- Reading `/proc/<pid>/exe` of another process needs ptrace read access, so a scan for another game
  from inside the game is expected to see nothing. That is INFERRED: nothing was run inside the snap.
  logarchive_linux.h therefore finds a live game with an flock liveness lock
  (`<data>/.tpf2mp_game.lock`, a shared lock every game holds) and scans `/proc` only on a
  filesystem without flock.

## Still open (INFERRED, kept out of any decision that could lose data)

- `STEAM_COMPAT_CLIENT_INSTALL_PATH` as a Steam folder candidate: it is not in the Steam client's
  environment, and no game process was running to check.
- The Flatpak Steam paths (`~/.var/app/com.valvesoftware.Steam/...`): no Flatpak Steam here.
- What the game does after `"... Process seems to be still running"` (the control flow after
  0x9b12d0 was not followed).

## dev 45183ac6: archive metadata and state (2026-09-26)

This integration changes mod-owned file collection only. Existing game log
locations and constructor ordering above are unchanged; no new game offsets,
patch bytes or SysV calling conventions are introduced.

The Windows PE link stamp maps to GNU ELF build IDs on native Linux.
`LaBuildId` reads ELF64 little-endian program headers and bounded `PT_NOTE`
records, selecting `NT_GNU_BUILD_ID` with the `GNU\0` owner. It bounds the
program-header table and note spans against file size, caps note segments at
1 MiB and IDs at 64 bytes, and reports unavailable for unsupported/malformed
files. It does not execute the game or map game objects.

The native archive regression executable's read-only identity mode was run on
`~/.local/share/tpf2mp-lab/native/game/TransportFever2`; it returned
`3a0e156390b0e6f1e372051c24802c8493ae454a`, identical to `readelf -n`.
Native version discovery follows `tools/linux/install.sh`'s
`tpf2mp_install.txt` version field. Lobby state discovery follows
`LobbyFolder` / `XdgNetDir` in `lobby_linux.cpp`; the game-folder fallback is
also collected. These are source-level contracts, not inferred game layouts.

The lab launch failed before game execution (`bwrap: setting up uid map:
Permission denied`). Thus no live startup/archive/UI result is claimed.
See [integration and tests](../../linux/UPSTREAM_dev_45183ac6.md).
