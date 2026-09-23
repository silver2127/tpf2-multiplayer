# The unchanged Windows Lua on Linux

Linux ships all 24 Lua files from Windows release 0.4.22, commit
`7990a86edd94`, byte for byte. There are no `K.LINUX`, `K.ROOT`, Linux command fields,
or alternate protocol readers. `tools/linux/verify_lua_release.py` compares every
Lua file against that commit; release packaging checks the source and staged mod.

The compatibility work lives in `native/linux/src/boot.cpp`, the other Linux native
libraries, and the launcher. The verified game is native Linux build 35924, GNU
build-id `3a0e156390b0e6f1e372051c24802c8493ae454a`.

## Directory environment published by the loader

Before loading the bridge, menu, slice or plugin host, the preload constructor
publishes these process-local variables. It does this only when `/proc/self/exe`
is named `TransportFever2`; shell utilities retain their original environment.

| Variable | Published value |
|---|---|
| `TPF2MP_DATADIR` | The final directory from `datadir_linux.h`, created and ending in `/`: an absolute existing override, otherwise the XDG/HOME data directory |
| `LOCALAPPDATA` | Absolute `XDG_DATA_HOME`, otherwise `HOME/.local/share` |
| `HOME`, `XDG_DATA_HOME` | Preserved exactly as supplied by Steam or the user |

The unchanged Lua chooses the first candidate holding `tpf2_instance.txt`, testing
`TPF2MP_DATADIR` before `LOCALAPPDATA/tpf2mp/data/`. The bridge writes the identity
file; the loader does not invent one. Without identity the existing Lua defaults
to the latter directory. Thus an explicit data override requires the bridge's
identity, exactly as in the Windows release. UTF-8 paths work through the proven
identity candidate; the original Windows fallback rejects non-ASCII paths without
identity and is unchanged.

The GUI's existing `CM.netDir()` tests `LOCALAPPDATA/tpf2mp/netpunch`, then
`./netpunch`, for `lobby_out.jsonl`. The published `LOCALAPPDATA` makes this the same
folder selected by the Linux lobby. Snap's own `HOME` is respected, so its normal
default is `~/snap/steam/common/.local/share/tpf2mp/`.

The loader removes itself from `LD_PRELOAD` before the game starts children,
while the directory variables remain inherited. No global environment or profile
file is changed.

## Configuration and native file contracts

`SliceCfgFlag` and the unchanged Lua's `CM.cfgFlag` both search for
`tpf2_slice.cfg` in the game working directory first, then the selected data
directory. The native library directory is not an extra candidate. Plugin-host
`tpf2mp.cfg` is a separate native configuration file.

The bridge and lobby publish identity, control and transfer markers using a
temporary file followed by `rename`. This preserves the complete-file behavior
on which the unchanged Lua depends. Capture lines retain the Windows `ARMED 1`
and existing record formats; terrain/assets keep the TPTG/TPAS v1 files and
construction keeps ROADC/CONXP/CONUP. No Lua retry or acknowledgement protocol is
introduced by Linux.

## Verification

`tools/linux/test_boot_environment.py` runs the real preload constructor in an
isolated executable named `TransportFever2`, then executes the exact directory
resolver and GUI `CM.netDir()` source from the unchanged Lua. It covers HOME,
absolute XDG, explicit data overrides, Snap HOME, Unicode/spaces, relative-XDG
fallback, and non-game passthrough. It checks both selected directories, creation
of the data directory, preservation of HOME/XDG, and removal of the preload from
the child environment. Libraries are isolated so this test cannot start a real
bridge or install game hooks.

The remaining sections record static Lua-runtime evidence. Addresses are Linux
RVAs in build 35924 unless another library is named; inference is marked.

## The Lua sandbox: what `os` and `io` the mod gets

The registration tables are Lua 5.2's own. `os` is at 0x59a9460: `clock` 0x994e20, `date` 0x995130,
`difftime` 0x994dc0, `execute` 0x9956c0, `exit` 0x994d30, `getenv` 0x9955d0, `remove` 0x994cf0,
`rename` 0x994ca0, `setlocale` 0x994c30, `time` 0x994fa0, `tmpname` 0x994e60. `io` is at 0x59a8ea0
and includes `popen` 0x990310. The game then removes entries in 0x3279240 `(State*, bool esi, bool dl)`.
Its helpers are Lua API functions:

- 0x982950 `lua_pushnil`: `mov dword [top+8], 0; add top, 0x10`
- 0x9830c0 `lua_setglobal` and 0x982d50 `lua_getglobal`: globals via `luaH_getint(registry, 2)`
- 0x982290 `lua_type`
- 0x32788a0: `lua_pushnil`, then a tail jump to 0x983180 with `esi = -2` and the name, i.e. `t[name] = nil`

What each flag combination removes:

- **`esi` and `dl` both true.** `os`, `collectgarbage`, `debug`, `dofile`, `io`, `loadfile`,
  `package`, `rawequal` and `rawget` become nil, and `string.dump` goes.
- **Otherwise.** `os.execute`, `os.exit`, `os.remove` and `os.rename` go (0x32793ef..0x327942e).
  If `esi` is also true, more goes:
  - `os.getenv`, `os.setlocale` and `os.tmpname` (0x327943c..0x327945a)
  - `collectgarbage`, and 14 entries of `debug`
  - `dofile`, `io`, `loadfile`, `package`, `rawequal` and `rawget`
  - `string.dump`

`esi` is the byte 0x5b372d2 in both callers: MakeState passes `edx = 0` (0x3276fb7) and 0x3282c20
passes `edx = 1` (0x3282cb6). The byte's only writer is 0x32824b0 (`mov [rip], dil`). Its only
caller is `Run2` (`Game/Application.cpp`) at 0x9b1c40, with `edi = 1` when 0x9d7ba0 returns true
(it is `xor eax, eax; ret` in this build) and otherwise a startup option byte `[rbp-0x608]` (its
source was not traced).

The mod needs `io` and `os.getenv`, so it can only run with that byte clear. Game scripts then keep
`io`, `os.getenv`, `os.clock`, `os.date`, `os.time`, `os.setlocale` and `package`, and have no
`os.remove`, `os.rename`, `os.execute` or `os.exit`. That is exactly what the Windows build showed
(io.lua: "The game's Lua has no os.remove"). So `CM.clearFile` empties a file on Linux too, and the
mod never unlinks a file a native writer may hold open.

## Clocks

- `os.clock` is 0x994e20: `call 0x6dbd80` (the PLT stub of `clock`) at 0x994e30,
  `cvtsi2sd`, `divsd [0x3f1d648]` (= 1000000.0), `lua_pushnumber` 0x982970. The only callers of the
  `clock` import are 0x994e30 and 0x318fd38 (rel32 scan of `.text`).
- `libtpf2mp_boot.so` (LD_PRELOAD) exports `clock()` as `CLOCK_MONOTONIC` time since the loader
  started, in `CLOCKS_PER_SEC` units, inside the game process only. So `os.clock` is wall seconds
  since process start, as with the MSVC CRT on Windows. Without the loader it would be glibc's CPU
  time summed over every thread, but then there is no bridge either, no identity file, and the mod
  never leaves `detectInstance`.
- Every `os.clock` use in the mod times a wall interval, so nothing relies on CPU time:
  - lockstep.lua: `CM.pollTimed`, the hash and update PERF timers, and heartbeat `ms=`
  - hash.lua: the lane timings
  - inject.lua: scan and plan ms
  - net.lua: `sampleSimRate`, `projectedPeerMax`, `rttNote`, `heartbeatEcho`, `execDelayTick`, and
    the LSTICK arrival stamps
  - cursors.lua: sample and arrival stamps. Both Lua states are one process, so they share the loader's origin.
- Clocks never meet across machines unmapped. `ms=` only comes back to the peer that sent it, and
  cursor playback maps each sender's clock by the measured delay, so Windows and Linux peers
  interoperate.
- `os.time` and `os.date` are the same on both builds.

## Numbers and the C locale

Every number on the wire and in the files goes out through `string.format` and comes back through
`tonumber`, and both follow `LC_NUMERIC`. The process-wide value changes only through `setlocale`
with `LC_NUMERIC` (1) or `LC_ALL` (6). A single thread's value changes through `uselocale`.

- **The game binary.** Its only `setlocale` call is 0x994c79, inside Lua's `os.setlocale` (0x994c30).
  It imports no `newlocale` or `uselocale`, and has no `std::locale::global`.
- **The libraries in the game folder** (undefined dynamic symbols, `readelf --dyn-syms`):
  - `setlocale` is imported by `libSDL2-2.0.so.0.2500.0` and `libicuuc.so.61.1`.
  - `newlocale` and `freelocale` are imported by `libboost_locale.so.1.76.0` and `libicui18n.so.61.1`.
    A `locale_t` from `newlocale` changes nothing until `uselocale` or an `_l` function is given it.
  - Nothing imports `uselocale`.
- **libicuuc** calls `setlocale` once, at 0x65787 (`mov edi, 5; xor esi, esi`). That is
  `setlocale(LC_MESSAGES, NULL)`, a query.
- **libSDL2** calls `setlocale` 16 times.
  - Ten calls pass category 0, `LC_CTYPE` (`xor edi, edi`; 0x63e2c to 0x64b19). It does not set the
    decimal point.
  - Six calls pass `LC_ALL` (`mov edi, 6`), in the two functions below. Each queries the current name,
    sets `setlocale(LC_ALL, "")` (the environment's locale) and restores the saved name.

| libSDL2 function | query | set `""` | restore | runs |
|---|---|---|---|---|
| 0x140710, the X11 input method (`"XMODIFIERS"`, `"@im=ibus"`, `"@im=none"`) | 0x140bf7 | 0x140c83 | 0x140cb5, right after the input-method open (`call [rax]` 0x140ca7) | from its one caller 0x148576, among the X11 display setup calls (INFERRED: SDL's X11 video init, when the game opens its window, long before a game loads) |
| 0x140ed0, the X11 message box (`"_NET_WM_WINDOW_TYPE_DIALOG"`, `"Too many buttons (%d max allowed)"`) | 0x140f2a | 0x140f5b | 0x141f73, when the box closes | normally in a child process: both callers are in 0x142a10, below |

- **0x142a10** runs the message box in a child process:
  - It calls `pipe` (0x142a41), then `fork` (0x142a4f).
  - The child runs 0x140ed0 (0x142b75), writes the result to the pipe and leaves through `_exit`
    (0x142ba7).
  - The parent waits (`waitpid` 0x142a91, `"msgbox child process failed"`) and reads the button back.
  - So the set at 0x140f5b normally happens in the child, and the game's own locale is untouched.
- **When 0x140ed0 runs in the game process.** Only if `pipe` fails (0x142a49) or `fork` fails
  (0x142a5b) does it run there, from 0x142b08. Then the environment's locale applies to every thread
  while the box is open, the script thread included. Under `de_DE` or `fr_FR` that is a decimal comma.

So the process stays in the "C" locale, with `.` as the decimal point, whatever `LANG` says. The
exceptions:

- The moment the input method is opened, while the window is created.
- An X11 message box whose `pipe` or `fork` failed, for as long as it is open.
- Code outside the game folder: the Steam overlay, the Vulkan driver, or another mod calling
  `os.setlocale`, which the sandbox leaves in. These were not checked (INFERRED not to happen).

The unchanged release does not add a Linux-specific locale override or diagnostic.

## Files, line endings

- Lua opens files in text mode, which on Linux translates nothing, so `f:seek` offsets are bytes.
- Every reader tolerates a CR anyway, so a CRLF from a hand-edited cfg or a Windows peer reads the
  same:
  - `pollEvents` and `pollInject` split on `[^\r\n]+`
  - the cfg parser takes `%S+`
  - `detectInstance` strips `%s`
  - `CM.prefRead` trims `%s*$`
- Both bridges strip `\r` before a capture line goes on the wire.
- Relative opens that remain in the Linux build:
  - the first `tpf2_slice.cfg` candidate
  - `./netpunch`, the lobby folder's last candidate, used only when it holds `lobby_out.jsonl`
  - `egeo_<letter>.txt`, when the existing `dump_egeo` diagnostic is enabled

  These resolve in the game folder, the CWD that Steam's `run.sh` sets on both builds.
