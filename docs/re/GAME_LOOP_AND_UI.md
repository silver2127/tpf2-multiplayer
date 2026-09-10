# Game loop, saving, loading and the title menu

Build 35924; addresses are RVAs from image base `0x140000000`. Confidence labels are
defined in [README.md](README.md).

## Threads and state

- Three named long-lived threads: `Simulation Thread`, `Render Thread`,
  `Game Init Thread`, plus named `ThreadPool`s (`ThreadPool::ThreadPool(name, numThreads,
  bool)` `0x2381ef0`). Almost all pool work is the renderer; the one sim use is
  `ForEachEntityLoopParallel`, which can spread entity iteration across pool threads.
  [DECOMPILED]
- Game state is double-buffered: `RunGameSimLoop` asserts on
  `m_data->gameStates[m_data->simIdx]` and `simIdx == 1 - oldSimIdx`, flipping each step.
- Randomness is `boost::random::mt19937` passed explicitly by reference
  (`TownDeveloper::Develop`, `SimPersonAtTerminalSystem::GetRandomPlace`,
  `parcel_util::CreateBuilding`, ...), not hidden global state. Pathfinding
  (`simulation_util::path_finder::FindPathLines`) iterates hash maps keyed by edge and node
  ids.
- Reading ECS memory from another thread races the sim and yields plausible but wrong
  values. Native reads belong on the sim thread, inside a `GameSim::Step` detour, which gives
  the same guarantee Lua's `update()` has.
- Determinism was measured, not assumed: two instances on one machine, same save, no
  input, hashed daily over 58 in-game days (79 vehicles, 26 lines) matched at every sample.
  Cross-machine floating point was not part of that test; the mod's live desync detector
  covers it in play.

## Simulation pacing

- **Sim thread:** `CGame::RunGameSimLoop` `0x1184d0` steps once per handshake with the
  main thread. It has no speed clamp. [DECOMPILED]
- **Main thread:** `CGame::Step` `0x118e90` (16-byte prologue) accumulates wall time and,
  every `guiFrameTime` microseconds (int32 at `m_data+0x1a0`, `m_data = CGame+0x168`),
  hands the sim one batch through `CGame::Sync`. Sync recomputes guiFrameTime from the
  measured cost of the last batch, which is how high speeds keep up on a slow machine: by
  stretching the interval. The renderer interpolates vehicles between the last two batches
  with alpha `(totalTime - lastSync) / guiFrameTime`. [DECOMPILED]
- **A batch** is `GameSim::Step(__int64 frameTime, int)` `0x15aa00` (rcx = `GameSim*`,
  rdx = frameTime; it asserts `frameTime >= 1000`). It calls `CGameTime::GetSpeed`
  `0x2877a0` at `0x15aa30` (the pause test) and at `0x15aae4` (the iteration count) and runs
  that many sim iterations. Speed is therefore a whole number of iterations per batch; 0 is
  pause. Its first 21 bytes (`push rbx; push r14; sub rsp,0x68; mov rbx,rdx; mov r14,rcx;
  cmp rdx,0x3e8`) relocate verbatim. [DECOMPILED; both GetSpeed call sites are verified and
  patched live by `speedhook.cpp`]
- **The game clock advances 0.2 units per iteration.** One reading at load can look
  integral because the save stored a round value. [MEASURED]
- **`api.cmd.make.setGameSpeed(n)` accepts any whole number.** Measured on a solo
  instance: 3 → 2.9x, 8 → 4.1x, 16 → 7.4x, 32 → 10.4x (above about 4 the gain is sub-linear
  because the sim cannot keep up, not because of a clamp). Fractions truncate. The UI
  offers 0, 1, 2 and 4. [MEASURED]
- **Fractional speed** (what `speedhook.cpp` does): scale guiFrameTime by lever/target on
  every frame, because Sync rewrites it each batch. Every iteration is still an ordinary
  sim step, so lockstep by step count is untouched. An earlier version that dithered the
  iteration count (1, 2, 1, 2 for 1.5x) made vehicles judder at 5 Hz. [MEASURED]

## Forcing a save from native code

- `api.cmd.make` has no save maker (all 33 enumerated live), and `CmdData::SaveGame`
  (factory `0x9de0e0`) carries GameMetadata, a screenshot and config that a DLL cannot
  build. [MEASURED + static]
- `UI::CGameUI`'s per-frame update `0x5741d0` (21-byte prologue) adds the frame's dt to an
  int64 microsecond accumulator at `this+0x648` and calls `CGameUI::AutoSave` `0x563500`
  once it exceeds autosaveIntervalMinutes (settings+0x130) x 60e6. Writing `2^62` into the
  accumulator fires one full native autosave on the next frame (a 113 MB `autosave_*.sav`
  in the test). `INT64_MAX` does not work: the frame adds dt first and wraps negative.
  [MEASURED]

## Loading a save

- **`CONTINUE` loads the save named in `profile.lua`** (`lastGame[2].saveGameName` under
  `userdata\<steamid>\1066780\local\`), not the newest file. Two instances with different
  profiles load different worlds. [MEASURED]
- **The in-process load chain** (mapped, not called by the mod):
  `UI::CMenuUI::StartSavegame(const LoadGameParams&, const SavegameInfo&)` `0x6785c0` is
  where every UI load path converges (callers in `gameui.cpp`, `ingamemenuui.cpp`,
  `menuui.cpp`; guard `this+0x1988` = "Game initialization is already active!").
  `platform::StandardSaveGameBackend::GetSavegameInfo(const SaveGameId&)` `0x2471830` builds
  a SavegameInfo (SaveGameId: name string at +0x20, saveDirectory at +0x40);
  `FindAllSaveGames(const string&)` `0x24700d0` lists saves; the loader proper is
  `LoadGame` `0x2e5ec0` (sole caller `0x67d130`). LoadGameParams reads mods at +0x68, a bool
  at +0x80, and campaign/mission strings at +0x108/+0x128 (both empty for a plain save).
  Calling StartSavegame means constructing these three structs byte-exactly. [DECOMPILED]

## Title menu

- **`alut.dll` is a static import of `TransportFever2.exe`** with 20 named exports, loaded
  from the game folder before the exe's entry point runs. A forwarding proxy in its place is
  therefore in the process before the title menu is built. [CONFIRMED, import table]
- **`UI::CMenuUI::CreatePage(Page)` `0x663370`** (20-byte steal: pushes plus an
  rsp-relative `lea`) rebuilds a page on every navigation, and again on resolution and mod
  changes ("Mods changed, recreating data..."). It dispatches through a 17-entry jump table
  at `0x663e78`. The main menu builds pages 0 → 2 → 1. [CONFIRMED]

  | page | builder | what |
  |---|---|---|
  | 0 | `0x6614e0` | menu background |
  | 2 | `0x667bc0` | main menu content |
  | 3, 4, 10 | `0x66c2b0` | new-game setup |
  | 5 | `0x6641f0` | reset to default |
  | 6 | `0x664860` | campaign select |
  | 7 | `0x66a240` | mission start |
  | 8, 11, 12 | `0x667430` | load page |
  | 13 | `0x670e60` | settings |
  | 14 | `0x66ba60` | mod browser |
  | 16 | `0x666440` | mission title |

  Pages 1, 9 and 15 are small inline cases.
- **The main-page builder `0x667bc0`** (14-byte steal: `mov rax,rsp` plus seven pushes)
  creates a list widget named `MainMenu` and adds each entry as:
  action context `0x221c930(&ctx, "<key>")` → Button `0x7c5d30(ctx, &iconA, &iconB)` →
  `setWidgetFlag` `0x227f880(btn, 4, 1)` → list-add `0x22d99e0(list, btn, &"list-item")`
  (which is `0x24baf0(list+0x488, w, 1)` followed by `addStyleClass` `0x227a1e0(w,
  style)`). Exit uses `0x2d9ad0` (takes a unique_ptr). `0x2da460` sets the list's selection
  callback; `0x2da030` / `0x2da650` are count and select. [DECOMPILED]
- **Click wiring:** `0x22518f0(button, &connOut, std::function<void()>*)` is
  `button->clickSignal(+0x450).connect(fn)` (boost::signals2); the caller destroys
  `connOut` right after with `0x2357910`. The `std::function` is the standard MSVC 64-byte
  object described in [COMMANDS.md](COMMANDS.md#skipping-a-command-safely). A menu
  lambda's impl is 16 bytes (`{vptr, captured CMenuUI*}`); for the stock entries the vtable
  is at `0x30225d0` with `_Copy` and `_Move` folded into one function, `_Do_call` taking no
  arguments, and `_Delete_this` freeing 0x10 bytes through the game's allocator. A DLL
  supplying its own slot needs its own `_Copy`/`_Move` (the game's hard-code its vtable
  address into the destination). [CONFIRMED, disassembly]
- **Insert during the build.** An entry appended after the builder returns renders
  nothing; one inserted from inside the list-add hook while the builder runs renders.
  [MEASURED]
- Other builder helpers: `0x4c0f40` allocates a binding seeded with a
  `shared_ptr<ModRep const>`; `0x63e6e0` writes a handler into binding+0x38. The in-game UI
  is built by `UI::CGameUI::CreateUI` `0x569f00`. [DECOMPILED]
- The swapchain is created with image usage 0x13, which includes TRANSFER_SRC; that is
  what makes a readback-and-blend overlay possible. [MEASURED]
