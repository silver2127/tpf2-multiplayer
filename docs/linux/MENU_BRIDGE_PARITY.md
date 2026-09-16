# Menu and bridge audit against release 0.4.22

Baseline: pushed `release-0.4.22`, commit `7990a86`. This audit compares the
native source behavior; it does not establish a complete multiplayer runtime
test. The Linux game addresses target Steam build 35924.

| Shipped Windows behavior | Linux implementation |
| --- | --- |
| Native Multiplayer button, configurable `slot=0..7` (default 0), page visibility | `menu_linux.cpp`, `panel_linux.cpp`; the slot reader and insertion position were completed during this audit |
| Host/join, public browser, roster/company selection, chat, mod prompts, clipboard, logs | `panel_linux.cpp`, `lobby_linux.cpp`; Linux process/input/render differences are documented in [MENU_LOBBY.md](MENU_LOBBY.md) |
| Bridge control identity, peer, process, player count, speed, sync, transfer progress, leader | `lobby_linux.cpp::WriteBridgeCtl`, consumed by the bridge and shared Lua |
| Shared save placement and autoload | `menu_game_linux.cpp::MenuGame_PlaceSharedSave`, `MenuGame_RequestAutoload`; keeps the `.sav`, `.sav.lua`, and `.jpg` contract |
| Hot join, `/sync`, relay leader periodic autosave/upload | `lobby_linux.cpp::SyncStart`, `SyncPoll`, `RelayPeriodic`, and `menu_game_linux.cpp::MenuGame_ForceAutosave` |
| Auto-enable installed lockstep mod before startup | `menu_game_linux.cpp::MenuGame_AutoEnableMod` |
| Bridge port election, bind-race re-election, fallback ports, identity healing | `bridge_linux.cpp::InitThread`, `HealIdentity`, `ApplyControl` |
| Capture tailing, long and partial lines, truncation recovery, peer events with LF | `bridge_linux.cpp::TailThread`, `OnPeerLine` |
| Fractional speed by scaling the batch interval, lever preserved | `speedhook_linux.cpp`, using verified Linux fields and call sites |

No additional missing shipped menu/bridge feature was found in these paths.
The Linux forced autosave waits for the main UI frame, refuses overlapping
saves, and checks whether autosave is disabled. These are intentional guards
around Linux's verified engine path. A successful save still requires that
engine path to run; menu visibility can delay it.

`SAVE_NONBLOCKING.md` is a proposed Linux extension. Release 0.4.22's Windows
code has neither background saving nor an Esc pause bypass. Its `ForceAutosave`
requests the game's ordinary save, and the Linux port does the same. Both keep
the game's stock Esc behavior. The fork proposal needs measured child-lock
safety, save/load integrity, memory/latency limits, and watchdog recovery; its
Esc proposal needs the documented E6 world-input and HUD-restore checks. These
unimplemented experiments are outside the pushed-release parity baseline.

Validation added here: `native/linux/tests/menu_slot_test.cpp` reads real flag
files and drives the actual menu detours against mapped mock game callees. It
checks all eight positions, exactly one connected Multiplayer button, normal
list additions outside the builder, missing files, repeated values, rejected
values, and integer overflow. It passed off-game. Autoload, hot join, save
transfer, and Windows/Linux play still require live session validation.
