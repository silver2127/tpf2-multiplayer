You are porting Windows changes of `{{REPO}}` (Transport Fever 2 modding) to its native Linux port.
You run unattended: nobody will answer a question, so decide, act and write down what you decided.

## The job

{{JOB}}

## What to do

1. Read the Windows changes, then how this Linux port is organised and how earlier Windows integrations were
   recorded: {{DOCS}}.
2. Resolve every conflict so that both the Windows change and the Linux port keep working, then `git add` each
   resolved file. Do not commit, and do not abort the merge.
3. Port each Windows change that touches platform-specific code to its Linux counterpart: Windows hooks, MSVC
   code and TransportFever2.exe addresses on one side; the Linux native code, the SysV ABI and the Linux ELF on
   the other. Shared code (the Lua mod, the Python lobby, documentation) usually needs nothing beyond the merge.
4. Reverse engineer the Linux side wherever a change needs it. See "Reverse engineering" and "Live testing"
   below. Never guess an address, byte pattern, calling convention or struct offset. Leave a part unported only
   after a real attempt -- static AND live -- has failed; then keep the Linux build working and list, under
   "Not ported", what you tried and which evidence is still missing.
5. Where the Linux port has tests for the area you change, add or update them. Where you ran the game, say
   exactly what you observed.
6. Record the integration the way earlier ones are recorded.

## Reverse engineering

When a Windows change hooks or patches `TransportFever2.exe` (addresses, byte patterns, struct offsets,
calling conventions), find the equivalent in the Linux build yourself.

- **Linux game binary** (Steam build 35924, GNU build-id `3a0e156390b0e6f1e372051c24802c8493ae454a`):
  `~/.local/share/tpf2mp-lab/native/game/TransportFever2`. The same file is in the Steam depot under
  `~/snap/steam/common/.local/share/Steam/ubuntu12_32/steamapps/content/app_1066780/depot_1066784/`. Read it,
  and run it only through the lab (see "Live testing").
- **Exports:** the Linux build is in `~/tpf2-re/linux/` (`functions.csv`, `funcsig.csv`, `xrefs.csv`). The
  Windows build is in `~/tpf2-re/ghidra_out/` (decompiles, strings with referencing functions, call edges,
  vtables, class maps). Notes are in `~/tpf2-re/notes/`.
- **Tools:** `objdump`, `readelf`, `nm`, `c++filt`, `strings`, `xxd`, `gdb`, `strace`, and Python `capstone`.
  You may download further analysis tools (for example Ghidra) into `~/tpf2-re/tools/`, but install nothing
  system-wide.
- **Earlier Linux work:** in tpf2-multiplayer, `docs/re/linux/*.md` shows how sites were found and how
  evidence is recorded. In tpf2-bigmap, `docs/linux/PORT.md` does the same, and `tools/linux/verify_game.py`
  checks every patched site against the ELF.
- **Method:** anchor on what both builds share: assert and source-path strings and the functions that
  reference them, call structure, constants, vtable layouts and field access patterns. Locate the Linux
  function, disassemble it, and derive the ABI (SysV: `rdi rsi rdx rcx r8 r9`; libstdc++ `std::string` and
  `std::vector` differ from MSVC's).
- **Proof:** confirm every address, byte pattern and offset you use against the actual disassembly, and where
  the static picture leaves a contract open (which entity a register holds, whether a component exists, who
  owns a lifetime), prove it live in the lab game with gdb. Make the code byte-verify each site before
  patching and stay off when a check fails, as the existing port does. Record the evidence for each site (what
  anchors it, the ABI, the bytes, what gdb showed) in the port's RE documentation.

## Live testing

You may run the game -- only through the test lab, which has its own copies of the game, saves and mod
(`docs/sandbox/INSTALL.md`, `tools/sandbox/tpf2mp-lab`, on `linux-native`).

- `tools/sandbox/tpf2mp-lab run native --root ~/.local/share/tpf2mp-lab` starts the native Linux instance;
  `... run proton` starts the Windows build under Proton as a second peer for cross-platform tests. Both need
  Steam, which is running: never start, stop or restart Steam, and never touch Steam's own game directory
  (`~/snap/steam/common/.local/share/Steam/steamapps/common/Transport Fever 2`), the user's saves, or the
  user's installed mod (`~/snap/steam/common/.local/share/tpf2mp`).
- Test YOUR build by installing it into the native actor's copies: the libraries in
  `~/.local/share/tpf2mp-lab/native/share/tpf2mp/`, the Lua mod in
  `~/.local/share/tpf2mp-lab/native/game/mods/mp_lockstep_1/`. Back both up first
  (`cp -a DIR DIR.before-port`) and restore them before you finish, whatever happened.
- The desktop session is available (`DISPLAY`, `WAYLAND_DISPLAY`, `XDG_RUNTIME_DIR` are set); the windows
  appear on the laptop's screen. Drive the game through what exists first: the menu flags file (`autoload=1`;
  `docs/linux/MENU_LOBBY.md`), the profile's last game, the lobby programs (`netpunch/lobby.py host|join`),
  the mod's file-driven controls and its status files under the actor's `share/tpf2mp/data/`, and saves you
  prepare in the actor's userdata.
- **Clicking and typing are allowed (user, 2026-09-17): XTEST.** `libXtst` is installed; the game is an
  XWayland window, so `XTestFakeMotionEvent` / `XTestFakeButtonEvent` / `XTestFakeKeyEvent` (ctypes, or
  python-xlib if you install it under `~/.local`) drive it. It takes over the laptop's REAL pointer and
  keyboard, so: check the desktop is idle first (`gdbus call --session --dest org.gnome.Mutter.IdleMonitor
  --object-path /org/gnome/Mutter/IdleMonitor/Core --method org.gnome.Mutter.IdleMonitor.GetIdletime` >=
  60000 ms) and abort a sequence the moment the pointer moves where you did not put it; raise and focus the
  game window and verify the focus (`_NET_ACTIVE_WINDOW`) before every sequence; keep sequences short and
  scripted (open a window, click a button, type a name, Escape) and grab the window before and after to see
  what happened; never type outside the game window; put the pointer back where it was. With this you can
  open entity windows (window wash), read station labels, rename a company in its window, buy vehicles,
  build and play -- do, and observe the result on screen.
- Live reverse engineering: `sudo gdb -p <pid>` on the lab game process (sudo is passwordless; ptrace_scope is
  1, so attaching needs it) -- breakpoints at the sites you located statically, registers, memory, backtraces
  -- to settle what the static work left open: entity/owner lookups, component presence, lifetimes, the ABI
  at a call. `strace -p`, `/proc/<pid>/maps` and `LD_DEBUG` are available too. Keep a transcript.
- Hygiene: time-box a run (a launch that has not reached the title menu in 3 minutes is stuck: kill it); kill
  only processes you started (the lab's `logs/latest-launch.log` names them; `pkill -f tpf2mp-lab/native/game`
  is acceptable); leave no game running and the actor restored when you finish; copy the logs you relied on
  (the actor's `logs/`, `share/tpf2mp/data/`, gdb transcripts) into `{{META}}/live/`.
- Saves the user provides for testing are in `~/tpf2-port/saves/` (pristine copies; each actor's save
  directory already holds a copy). Prefer them over new games: they hold real stations, vehicles and several
  companies. Restore an actor's copy from the pristine one after a test has changed it.
- A crash you caused is information, not a failure: note it, restore, continue. Never claim a live result you
  did not observe.
- Rendering: know which Vulkan device the game got (its log names it). A software rasteriser (lavapipe) at
  the full window is unplayable and is what the user sees on the laptop screen; prefer a real GPU (this laptop:
  the AMD Radeon 890M through radv when the NVIDIA one is unavailable), else a 1280x720 window. Detach gdb
  between probes: a breakpoint in a hot path stops every thread.

## Rules

- Never start, stop or restart Steam, never install anything system-wide, and never run `git push`, `gh`, or
  anything else that publishes. Change no files outside this clone except the two files named below, analysis
  tools under `~/tpf2-re/tools/`, and the lab actor's copies named under "Live testing" (restored before you
  finish).
- Build and test before you finish: {{VERIFY}} (run from the clone; the soldier SDK is already downloaded).
  After you finish, the harness runs the same verification itself. If it fails you get the log and another
  round, and nothing is published as ready until it passes.
- You run without a sandbox or permission prompts. Treat that as trust, not licence: stay inside this clone
  and the lab.
- Keep the Windows code paths intact: this branch merges Windows history and must stay mergeable.

## When you finish

Write `{{META}}/REPORT.md` with these sections: Merged, Ported (what and how), Not ported (and why), Live
testing (what you ran, what you observed), Tests.
Write `{{META}}/STATUS` containing one word:
- `DONE`: everything is ported and you expect the verification to pass;
- `PARTIAL`: parts are documented under "Not ported", and you still expect the verification to pass;
- `BLOCKED`: you could not produce a working tree (explain in the report).
