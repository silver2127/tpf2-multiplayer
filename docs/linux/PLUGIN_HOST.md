# The plugin host on Linux

`tpf2_pluginhost.so` is the Linux build of `native/src/plugin/host.cpp` and `cfg.cpp`. It loads native
plugins (`plugins/*.so`) and hands each one the `Tpf2mpHost` table from
`native/src/plugin/tpf2mp_plugin.h` (ABI 1, the same header, unchanged). Why a host exists and why it is not
a mod manager: the comment at the top of `host.cpp`.

**Status (2026-09-12):** tested off-game in a fake `TransportFever2` process (see [Tests](#tests)). Not
yet loaded inside the real game.

| file | what |
|---|---|
| `native/linux/src/plugin/host_linux.cpp` | the host |
| `native/linux/src/plugin/cfg_linux.h`, `cfg_linux.cpp` | the `tpf2mp.cfg` parser: `cfg.cpp`'s rules, UTF-8 paths |
| `native/linux/src/plugin/codewrite_linux.h` | writes into the process's memory through `/proc/self/mem`, with no page protection change ([page races](#page-protection-races)) |
| `native/linux/src/hook_posix.cpp` | `installHook` (the same file the bridge and menu link) |
| `native/linux/src/plugin/sample_plugin_linux.cpp` | `tpf2mp_sample.so`, an example plugin that patches nothing |

The host holds no game RVA and patches nothing itself. Everything it does to game memory is on a plugin's
request.

## When plugins run

`libtpf2mp_boot.so` loads the bridge, menu and slice libraries first, and the host last. The host does its
work **synchronously in its constructor**, inside boot's `dlopen`. Every `Tpf2mpPluginInit` therefore
returns before the game's own static initialisers and `main()` run. That is the header's "before the exe
entry point", held more strictly than the Windows loader thread could.

What that means for a plugin:

- **Init must return promptly and never wait for anything the game does.** `main()` does not start until
  every init has returned. On Windows, init ran on a thread beside the exe, so an init that polled for a
  window or a global only held up its own plugin. Here it hangs the game at start. Wait on a thread of
  your own instead. While plugins load, a watchdog thread logs
  `[host] <plugin>: still inside Tpf2mpPluginInit after 10 s -- the game cannot start until it returns`,
  once per plugin.
- **Init runs under glibc's loader lock.** The lock is recursive, so `dlopen`, `dlsym` and `dladdr` on the
  init thread work. An init that waits for *another* thread calling any of them deadlocks the game at
  start. Tested: `dlsym` on a second thread blocked until init returned. A second thread calling the host
  table does not block, because nothing in the table takes that lock.
- **The game's subsystems do not exist yet**, as the header already says.
- **Other libraries patch the game at the same moment**, from their own threads: see
  [page races](#page-protection-races).

## Code writes and page protections

The 2026-09-14 integration moved the shared writer to `native/linux/src/codewrite_linux.h`. Hook installation, rel32 call redirection, player patches, menu vtable writes and the plugin host now all use `/proc/self/mem`. Game page permissions remain unchanged. If the kernel refuses that mechanism, the patch fails; there is no `mprotect` fallback.

The writer saves the original bytes and verifies the result. A short write attempts to restore its written prefix and reports failure. A hook's trampoline is published before its jump. If a failed write might have become visible to a running thread, the trampoline remains allocated; callers must use the returned success status to decide whether dependent behavior is available. The slice layer exposes `SliceHookInstalled(rva)` for that purpose.

This resolves the previous race in which one library restored `r-x` while another wrote to the same page. It does not make multibyte patches atomic: install hooks before the target can execute, and do not let plugins patch overlapping owned sites.

`native/linux/tests/hooks.cpp` verifies concurrent writes within one RX page, unchanged permissions, hook/trampoline execution, near-jump trampolines and invalid-address refusal. The actual build-35924 game also installed bridge, menu and slice hooks under Snap Steam on 2026-09-14. A second actual-game startup loaded the sample plugin and the enabled native preview plugin: the sample verified its bytes, all eight preview hooks installed, and both initializers returned OK.

## Where plugins and settings are found

Three folders:

| name | where |
|---|---|
| data dir | `$TPF2MP_DATADIR`, else `$XDG_DATA_HOME/tpf2mp/data/`, else `~/.local/share/tpf2mp/data/` (`datadir_linux.h`). Under Snap Steam: `~/snap/steam/common/.local/share/tpf2mp/data/`. |
| lib dir | the folder `tpf2_pluginhost.so` was loaded from. Boot takes the file from `$XDG_DATA_HOME/tpf2mp/`, else `$HOME/.local/share/tpf2mp/` (`Tpf2mpRootDir`), when it is there, and otherwise from its own folder. |
| game dir | the folder of the running executable (`/proc/self/exe`) |

On Windows the host DLL's folder is the game folder in the shipping layout, so two folders cover it. On
Linux the libraries live apart from the Steam-managed game folder, so both are searched. One folder reached
under two names (a symlink) is searched once.

**Plugins** come from `<data dir>plugins/`, then `<lib dir>plugins/`, then `<game dir>plugins/`:

- Only regular files, or links to one, whose names end in `.so` are loaded. Each folder is read in byte
  order.
- Names match exactly. A name found in an earlier folder hides the same name later, so a user's copy in the
  data folder shadows a shipped one.
- The same library reached through two names (a link) is initialised once. Libraries are never
  `dlclose`d.
- `Tpf2mpPluginInit` must be defined by the library itself. `dlsym` also searches a library's dependencies,
  so a library that links against another plugin and has no init of its own would otherwise run that
  plugin's init a second time, under its own name. Such a library is left loaded and not called.

**Settings** come from `tpf2mp.cfg` in the lib dir, then the game dir, then the data dir. The first file
found wins; files are not merged. Then, for each plugin in load order, `plugins/<name>.cfg` beside the
`.so` is merged over the table, and later files win. `enabled` in the plugin's section (default on) is read
after that merge and before `dlopen`, so a disabled plugin's constructors never run. The format is the
Windows one ([CONFIGURATION.md](../CONFIGURATION.md#tpf2mpcfg-and-plugin-settings)).

A settings file that exists but cannot be read (no permission, a directory, an I/O error) is skipped, as on
Windows, but not silently: `could not read <path> (<reason>) -- skipped` for `tpf2mp.cfg`, and
`<plugin>: could not read <path> (<reason>) -- its settings, including enabled, are ignored` for a
plugin's own file.

`TPF2MP_NO_PATCHES=1`, the bridge's switch for ruling our code patches out of a crash, also stops the host
loading any plugin.

## The host table on Linux

| member | Linux behaviour |
|---|---|
| `log` | Appends `[<plugin>] <message>` to `<data dir>tpf2mp_host.log`, one `write` per line on an `O_APPEND` descriptor, so nothing is buffered. The message is cut at 1023 bytes, then prefixed; a newline inside it starts a line with no prefix. The prefix is the plugin whose code made the call: the caller's address is looked up in each plugin's executable segments, so a line from a plugin's own thread or hook still carries its name. A tail call or generated stub falls back to the plugin whose init is running, else `host`. The host takes no lock of its own, so a plugin may log from any thread, and from a signal handler as far as `vsnprintf` allows. `errno` is preserved. |
| `cfgInt` | `strtol` base 0 (`0x` hex; a leading `0` means octal). The whole value must parse, else `def`. Clamped to the `int` range, which is what the Windows build's 32-bit `long` gives. |
| `cfgBool`, `cfgStr` | As on Windows. A pointer from `cfgStr` stays valid for the life of the process, even after a later plugin's `.cfg` replaces the key. `cfg.cpp` overwrote the string in place, which broke the header's promise. |
| `moduleBase` | The executable's load address when `/proc/self/exe` is named `TransportFever2`, else 0. |
| `buildOk` | 1 when `moduleBase` is set and the GNU build-id is build 35924's (`game_image.h`). |
| `verifyBytes` | Returns 0 unless every byte lies inside the executable's `PT_LOAD` extent (RVA 0 to 0x5bd912a for build 35924) and is mapped readable in `/proc/self/maps`. If that file cannot be read, the host logs `/proc/self/maps unreadable (<reason>) -- verifyBytes, patchBytes and installHook refuse` once. Without that line the plugin's "wrong game build" would be the only trace. |
| `installHook` | Takes an absolute address and accepts 14..32 stolen bytes. Refuses invalid arguments, unreadable/non-executable spans and cuts known to split an instruction. Unsupported decoder instructions are logged and remain the plugin author's responsibility. The trampoline is published before the jump; all game-code writes use the shared `/proc/self/mem` writer. Check the return value, not only the trampoline pointer. |
| `patchBytes` | Uses the same image/mapping range checks as `verifyBytes`, then the shared writer. Reads back the result; short writes attempt restoration and fail. Kernel refusal fails without changing page protections. Call `verifyBytes` first to establish the expected original bytes. |
| `dataDir` | Ends in `/`. |

## Faults and exceptions in init

There is **no crash guard**. `host.cpp` wraps init in `__try/__except`. Here a `siglongjmp` out of a
signal handler in foreign C++ would skip destructors and leave locks held, and the game would fail later
somewhere unrelated. So a fault in init ends the game.

A **C++ exception leaving init ends the game too**, because it cannot be caught across this boundary. Each
of our libraries links libgcc statically (`-static-libgcc`, with its symbols hidden). The unwinder raising
the exception belongs to the plugin. In phase 2 it calls the host frame's personality routine, which calls
the host's own copy of `_Unwind_SetGR` on a context the other unwinder built. The copy aborts. This was
tried off-game with a `catch (...)` around the init call:

```
static-runtime plugin                              plugin on libstdc++.so.6 + libgcc_s.so.1
#5 _Unwind_SetGR.cold          tpf2_pluginhost.so   #5 _Unwind_SetGR.cold          tpf2_pluginhost.so
#6 __gxx_personality_v0        tpf2_pluginhost.so   #6 __gxx_personality_v0        tpf2_pluginhost.so
#7 _Unwind_RaiseException_Phase2  t_throw.so        #7 ??                          libgcc_s.so.1
#8 _Unwind_Resume              t_throw.so           #8 _Unwind_Resume              libgcc_s.so.1
#9 Tpf2mpPluginInit.cold       t_throw.so           #9 Tpf2mpPluginInit.cold       t_throw_dyn.so
```

Both runs aborted after the plugin's own destructors ran, so the catch was removed.
`tpf2mp_plugin.h` already rules out exceptions at this boundary. Without the catch, an escaping exception
reaches `std::terminate` ("terminate called after throwing an instance of ...").

In both cases, the `[host] <plugin>: calling Tpf2mpPluginInit (...)` line is written before the call. The
plugin that ended the game is the last one with a "calling" line and no result line after it.

**For every Linux library, not only plugins (INFERRED, not tested with the game):** the same mechanism
applies to any frame of ours that has a landing pad, such as a local with a destructor or a `try` block. If
a C++ exception raised by the game's runtime unwinds through a hook detour with such a frame, the game
aborts instead of the exception reaching the game's own handler.

## Differences from the Windows host

| | Windows `host.cpp` | Linux `host_linux.cpp` |
|---|---|---|
| plugin files | `*.dll`, names case-insensitive | `*.so`, names exact, sorted per folder |
| plugin folders | data, host DLL folder | data, lib, game |
| `tpf2mp.cfg` | host DLL folder, then data | lib, game, then data |
| unreadable settings file | skipped silently | skipped, with a log line |
| init runs on | a thread started from `DllMain` | the loader's thread, in the host's constructor, before `main()` |
| an init that waits for the game | delays that plugin | hangs the game at start; a watchdog logs it after 10 s |
| fault in init | caught by SEH, logged | ends the game; the log names the plugin |
| C++ exception out of init | caught by SEH, logged | ends the game ([above](#faults-and-exceptions-in-init)) |
| log | stdio | one `write` per line, no host lock |
| log prefix | the plugin whose init runs, else `host` | the plugin whose code called |
| `cfgStr` pointer after a later `.cfg` sets the key | overwritten | stays valid |
| `verifyBytes` / `patchBytes` range | any committed memory | inside the executable's `PT_LOAD` extent |
| `patchBytes` write | `VirtualProtect` around `memcpy` | `/proc/self/mem`; refusal disables the patch |
| `installHook` checks | none | mapped `r-x`; a decodable cut on a boundary |
| same library under two names | initialised twice | initialised once |
| init found only in a dependency | (`GetProcAddress` does not search dependencies) | not called |
| `TPF2MP_NO_PATCHES=1` | not read | no plugins loaded |

## Writing a plugin for Linux

```c
extern "C" __attribute__((visibility("default")))
int Tpf2mpPluginInit(const Tpf2mpHost* host, Tpf2mpPluginInfo* out);
```

- **RVAs are the Linux binary's own addresses.** Every RVA passed to `verifyBytes` or `patchBytes`, and every
  offset added to `moduleBase()`, is an ELF virtual address of the Linux `TransportFever2` of build 35924
  (build-id `3a0e156390b0e6f1e372051c24802c8493ae454a`). They are never the Windows RVAs, which are relative
  to 0x140000000 and belong to a different compiler's output. Re-derive every site from the Linux binary.
- Build with `-fPIC -shared -fvisibility=hidden`. A plugin that uses the C++ library adds
  `-static-libstdc++ -static-libgcc -Wl,--exclude-libs,ALL`, as our libraries do: the game runs on the
  Steam Linux Runtime's older libstdc++. Export only `Tpf2mpPluginInit`, and define it in the plugin itself;
  a linker version script does it (`native/linux/exports_boot.map` shows the form).
- Check `abiMajor`, then `size` before any member added after ABI 1's first table. Check `moduleBase` and
  `buildOk`, then `verifyBytes` at every site before `patchBytes` or `installHook`.
- Return from init promptly. Never wait there for the game, or for another thread that loads libraries or
  looks up symbols; start a thread of your own for anything that has to wait.
- Never let an exception leave `Tpf2mpPluginInit`, a hook detour, or any function the game calls.
- Avoid 16-bit immediates (`66` prefix with `81`, `C7` or `B8`-`BF`) in the bytes an `installHook` steals,
  or check the cut against a disassembler yourself: the host cannot check it for you.
- `tpf2mp_sample.so` is the template: it runs every check up to the patch and stops there. Build it with the
  command in its header. In the game it should log `bytes at 0x2fd1420 match build 35924`.

## Build 35924 facts used here

- **Build-id:** `PT_NOTE` at vaddr 0x28c holds `NT_GNU_BUILD_ID`
  `3a0e156390b0e6f1e372051c24802c8493ae454a`, read from the file with pyelftools. `game_image.h` compares
  the loaded image's note against it.
- **Image extent:** the file is `ET_DYN` with four `PT_LOAD` segments.

  | vaddr | flags | memsz |
  |---|---|---|
  | 0x0 | `r-x` | 0x59a6de0 |
  | 0x59a8700 | `rw-` | 0x217038 (RELRO 0x9f900 of it) |
  | 0x5bc0000 | `r--` | 0x310 |
  | 0x5bc1000 | `r-x` | 0x1812a |

  The RVA extent `verifyBytes` and `patchBytes` accept is therefore 0 to 0x5bd912a. The host computes it at
  run time from the loaded image's program headers, not from these numbers.
- **The sample's site, 0x2fd1420:**
  - `functions.csv` has an FDE starting there, size 932.
  - The file's bytes are `f3 0f 1e fa 55 48 89 e5 41 57 41 56 4c 8d 77 10`: `endbr64; push rbp; mov rbp,rsp;
    push r15; push r14; lea r14,[rdi+10h]`, disassembled with capstone.
  - `menu_linux.cpp` calls it as the menu's `tr()` and nothing patches it.
  - It was chosen over `CGameTime::GetSpeed` (0xc0dc30, bytes also verified) because the bridge hooks that
    one from its own thread and could change the bytes mid-check.
- **The prologue decoder's 16-bit immediates:** fixed in the shared decoder. Operand-size `66` selects two-byte immediates for `81`, `C7` and `B8`–`BF` unless REX.W overrides it. The old `Imm16InstructionAt` workaround was removed. The native hook tests cover the previously miscounted instruction sequences and their exact boundaries.

## Log

```
[host] pid=4242 abi=1
[host] data dir: /home/u/snap/steam/common/.local/share/tpf2mp/data/
[host] lib dir: /home/u/snap/steam/common/.local/share/tpf2mp/  game dir: /home/u/.../Transport Fever 2/
[host] config: (none found -- built-in defaults)
[host] game build: MATCHES 35924
[host] 1 plugin(s) found
[host] patchBytes writes through /proc/self/mem (no page protection changes)
[host] tpf2mp_sample: calling Tpf2mpPluginInit (/home/u/.../data/plugins/tpf2mp_sample.so)
[tpf2mp_sample] data dir /home/u/.../data/, greeting '(none)'
[tpf2mp_sample] bytes at 0x2fd1420 match build 35924: a real plugin would patch here (...)
[host] tpf2mp_sample: tpf2mp_sample 1 -> OK (0)
[host]   checks the host table and one build-35924 site; patches nothing
[host] done
```

Other host lines:

- **Settings:** `could not read <cfg> (<reason>) -- skipped`, `merged <cfg>`,
  `could not read <cfg> (<reason>) -- its settings, including enabled, are ignored`, `skipped (enabled=0)`.
- **Loading:** `dlopen FAILED: <reason>`, `no Tpf2mpPluginInit export -- not a plugin, left loaded`,
  `Tpf2mpPluginInit comes from a dependency (<path>) -- not called, left loaded`,
  `the same library as <name> -- not initialised twice`, `no plugins found (looked in ...)`,
  `TPF2MP_NO_PATCHES=1: no plugins loaded`.
- **Runtime:** `/proc/self/mem not opened (<reason>) -- patchBytes changes page protections, ...`,
  `/proc/self/maps unreadable (<reason>) -- ...`, `<plugin>: still inside Tpf2mpPluginInit after 10 s -- ...`.
- **Per call, prefixed with the calling plugin:** every `installHook` and `patchBytes` (installed, written and
  how, refused and why).

## Tests

Off-game, in the plugin-host scratch folder (`test/run_tests.sh`; last run 2026-09-12: 0 failures). Every
case runs with `HOME` and `XDG_DATA_HOME` pointed into the scratch folder, and the real data folder and the
game are never touched.

- **Build:**
  - The host is linked exactly as `native/linux/CMakeLists.txt` links its libraries: static runtime,
    `--no-undefined`, `-z now`, `exports_none.map`. It exports nothing and needs only `libc.so.6` and
    `ld-linux-x86-64.so.2`.
  - `host_linux.cpp` and `cfg_linux.cpp` (with `codewrite_linux.h`) compile without a warning under
    `-Wall -Wextra -Wformat=2 -Wconversion -Wsign-conversion -Wshadow -Werror`.
  - The sample exports only `Tpf2mpPluginInit`.
  - A scratch CMake project with the target block from the integration notes builds both.
- **Process context:** a preloaded stand-in for boot, which only acts in a process image with the expected
  name, `dlopen`s the host from its constructor. That is boot's context.
- **Fake executable:** a PIE `TransportFever2`, linked once with build 35924's build-id and once without.
  It carries hand-assembled targets:
  - a hookable prologue
  - a prologue the decoder cannot read
  - a prologue whose 14-byte cut splits an instruction
  - three prologues with a `66`-prefixed 16-bit immediate: two correct 14-byte cuts the decoder would
    refuse, and one bad cut it would pass
  - a patchable function
  - a pointer table in RELRO
  - two pages of text for a patch across the boundary
- **Case `full`:** 11 plugin files covering every table member and its refusals, plus:
  - config search order and `.cfg` merging, including a merge that switches a plugin back on
  - `enabled=0` from both files
  - a non-ELF `.so`, a folder named `.so`, a library without the export, a plugin returning an error
  - shadowing between folders, a link to an already loaded plugin
  - logging from threads during and after init, the loader-lock behaviour, `cfgStr` pointer stability
  - init finishing before `main()`
  - every patch written through `/proc/self/mem`, pages `r-x` / `r--` before and after
  - the 16-bit-immediate hooks installed with their log lines, and the two correct ones working
- **New cases for the review fixes:**
  - `nomem`: the process is non-dumpable, so `/proc/self/mem` cannot be opened. Every `t_api` check passes
    on the page-protection fallback, and the log says so.
  - `race`: a thread flips a page's protection while the host patches it 3000 times; no fault, and every patch
    goes through `/proc/self/mem`. `test/race_demo.sh` runs the same with the fallback forced
    ([page races](#page-protection-races)).
  - `slow`: a plugin sleeping 11 s in init is named once by the watchdog, and only it.
  - `dep`: a library whose `Tpf2mpPluginInit` is its dependency's is not called; the dependency is called once,
    under its own name.
  - `cfgerr`: an unreadable `tpf2mp.cfg` is skipped with a line and the next one used; an unreadable
    `plugins/<name>.cfg` holding `enabled=0` is reported and the plugin loads.
  - `nomaps`: `/proc` hidden by a tmpfs in a private mount namespace (via `sudo unshare`, then back to the
    user), with only a `/proc/self/exe` link. One `maps unreadable` line, then `could not read /proc/self/maps`
    refusals from `patchBytes` and `installHook`, `verifyBytes` returning 0, and the sample's "differ" line
    after the explanation.
- **Other cases:** a mismatched build-id; `tpf2mp.cfg` only in the game or only in the data folder, or
  nowhere; a process not named `TransportFever2`; `TPF2MP_NO_PATCHES=1`; the host reached through a link
  to the game folder; the sample in three processes.
- **Faults:** a plugin that throws ends the process with SIGABRT, and one that faults ends it with SIGSEGV.
  In both, the last log line is the plugin's own, right after its "calling" line.
- **Parser:** `cfg_linux.cpp` has its own unit test (74 checks, including the `errno` behind a skipped or
  unmerged file) under ASan+UBSan and under TSan, with readers racing a writer.

## Integration status

The shared code writer, immediate decoder fixes, CMake plugin-host target, explicit `Threads`/`dl` links and hidden export checks are integrated. Normal libraries export no symbols; plugins export only their initializer. Release builds run in the pinned soldier SDK with a glibc 2.31 ceiling.

The earlier scratch harness results above document the original host review. Cases describing the removed permission-changing fallback and immediate workaround are historical. The current source-tree hook/core tests cover their replacements.

## Runtime validation still needed

- Loading a real third-party Linux plugin; the existing Windows DLLs are not binary compatible.
- Native preview behavior during a map session and across map unload/reload.
- A full Windows/Linux multiplayer session, including late join, resync and save transfer.

The real-game startup test loaded the host and installed the native game hooks successfully. It did not establish these broader behaviors.
