# Non-blocking saving and a no-pause Esc menu: the Linux design

This covers **Factorio-style non-blocking saving** and a **no-pause in-game menu** for the native
Linux build.
- **Non-blocking saving:** at a safe point, `fork()` the game. The child writes the save from its
  copy-on-write snapshot and calls `_exit()`, while the parent keeps simulating and rendering.
- **No-pause in-game menu:** while a lockstep session is live, opening the Esc menu, or saving from
  it, does not stop the local simulation.

The document has five parts:
1. What saving does today, on which threads, and what stops.
2. Go/no-go and design for the fork save.
3. Go/no-go and design for the no-pause menu.
4. The in-game experiments, in order.
5. The implementation plan.

Windows cannot `fork()`, so none of this applies there.

Binary: `TransportFever2`, Steam build 35924, x86-64 PIE, GNU build-id
`3a0e156390b0e6f1e372051c24802c8493ae454a`. Addresses are Linux RVAs (the PIE links at 0; live =
image base + RVA). Windows addresses appear only as references to `docs/re/*.md`.

Status labels:
- **PROVEN:** the instructions, strings, relocations or files quoted here show it, and anyone can
  re-derive it from the binary or the disk.
- **INFERRED:** it rests on naming, elimination, ABI knowledge, library source or Windows
  measurement. Patch code must guard it with a run-time check.
- **UNPROVEN:** static analysis could not settle it. The experiment in part 4 that settles it is
  named.

Nothing here was run in the game. This is the static DESIGN stage: the game and Steam were not
launched, attached to or signalled, and the save folder was only listed.

## How the evidence was produced

- Four static mappings were used, each with two verification passes: save pipeline, Esc pause,
  fork safety, and multiplayer. Their scripts and dumps are in the session scratch folder
  `wf-save/`. Claims those passes refuted are dropped or corrected here (section 1.7).
- Every address this design patches, or that its safety argument depends on, was disassembled
  again for this document. The tool was capstone, sweeping linearly from the function start given
  by `/home/topsnek/tpf2-re/linux/functions.csv`. Those addresses:
  - the menu flag gate `0x100fc70` and the visibility lambda `0xfddd40`;
  - `CGame::Sync` and `RunGameSimLoop`;
  - the Apply dispatch, the slot-27 thunk and the SaveGame visitor;
  - SaveGame's head, autosave branch and catch handlers;
  - OpenWrite and Commit, and the scope guard;
  - the save dialog lambda, AutoSave, both completion lambdas and `CommandList::Swap`;
  - the backend getter and constructors;
  - the `cout` buffer swaps in Run, `ThreadSafeLocalTime`, `CGame::Lock`;
  - `UI::Clock::DoStep`, and the call sites `0xfe7828` and `0xa31c66`.
- Callers came from a rel32 `e8`/`e9` scan of `.text`, and data references from a RIP-relative
  disp32 scan.
- Stored pointers came from `.rela.dyn` RELATIVE relocations; PLT names from `.rela.plt`; imports
  from `nm -D`.
- Container facts came from the pressure-vessel log
  `SteamLinuxRuntime_soldier/var/slr-app1066780-t20260912T181220.log`, from
  `/var/lib/snapd/seccomp/bpf/snap.steam.steam.src`, and from `/proc` on the host with the game
  not running.

## 0. Verdicts

| Question | Verdict | Conditions |
|---|---|---|
| fork()-based non-blocking saving | **GO WITH CONDITIONS** | Fork at the SaveGame command visitor on the Simulation Thread (§2.2). Before forking, try-lock the three game locks the child needs and park the main thread for a moment (§2.3). A watchdog, a blocking fallback and one child at a time are required (§2.6). The feature is behind a mode switch. It becomes the default only after experiments E1–E5 pass, and `always` only after E9. |
| No-pause Esc menu while a session is live | **GO WITH CONDITIONS** | One 13-byte patch at `0x100fc70`, gated by a live flag; outside a session the stock pause stays (§3.3). E6 must show that the world takes no input behind the menu and that the HUD comes back. A save from the menu stops stalling only with the fork save on. |

What decides it (PROVEN):
- The save runs as an engine command on the Simulation Thread, at a batch boundary, and that
  thread holds no game lock while it runs.
- Every input the save needs is already in CPU memory inside the command, including the
  screenshot.
- The parent needs only three result fields back.
- The menu pause is one byte flag with one reader.

What still needs the game running (UNPROVEN):
- fork latency and copy-on-write cost at this process size;
- how often the child deadlocks on locks glibc does not reset (ICU, iconv, the timezone lock);
- whether world input passes through the menu once the gate is bypassed.

## 1. What saving does today

### 1.1 Entry points (PROVEN)

- **Autosave.** The CGameUI per-frame update is `0x100fb20`, slot 34 (`0x5a11578`) of vtable
  `0x5a11468`; see MENU_GAME.md HJ-01..HJ-03. Its autosave block is `0x10101a7`..`0x10102e7`:
  - It runs only when `GlobalSettings+0x120` (`autosaveIntervalMinutes`) > 0: `0x10101ac mov
    eax,[rax+0x120]; test eax,eax; 0x10101b4 jg`.
  - The frame's µs are added to `CGameUI+0x628` (`0x1010358`..`0x101036d`) unless
    `0x30e9760(uiRoot, &UI::PopupGroupAutosave)` returns non-zero.
  - `0x10102cb imul rax,rax,0x3938700; 0x10102d2 cmp` against the accumulator, then `0x10102e2
    call 0xfeb560` (AutoSave), then `0x10102e7 mov qword [rbx+0x628],0`.
- **Esc-menu save dialog.** `InGameMenuUI::CreatePage` `0x10aa970` calls `CreateSaveGameLayout`
  `0x10a82e0` at `0x10aabfd` and `0x10ade8c`. That installs invoker `0x10aa1b0`, which tail-jumps
  to the save lambda `0x10a8b90`. The completion lambda is `0x10a7ca0`.
- **Crash save.** `0x111e350` jumps to `0xfec150`, which calls SaveGame directly at `0xfec73f`,
  from Run's catch handler (`0x6e0385`). This design does not touch it.
- **No quick save.** The only related input action registered is `IA_GAME_DEBUG_AUTO_SAVE`
  (`0x95920c`).

### 1.2 Main thread: screenshot, flag, command

- **Thread.** `0x100fb20` runs on the process main thread (PROVEN; the esc-pause verification pass
  traced the chain from `_start` through `UI::CCore::Run` `0x30826d0` to
  `CComponent::Step2` `0x305a1c0`).
  - It calls `CGameUI::GameStep` `0xfe77b0` at `0x100fcae`, which calls `CGame::Step` `0xa31c30` at
    `0xfe7828`, which calls `CGame::Sync` `0xa30cc0` at `0xa31c66`. All three sites were re-read,
    and each callee has that single caller.
  - The label `'Render Thread'` at `0x100fb37` is only passed to the empty profiler stub
    `0x3225310` (`endbr64; ret`).
- **AutoSave `0xfeb560`:**
  1. `0xfeb738 call 0xfeb180` gathers camera and GUI data.
  2. `0xfeb797 call 0x192cf10` takes the screenshot: a GPU readback through `IRenderContext` slot
     `0x228`, which is `VulkanRenderContext::GetTextureData` `0x3500df0`. It converts to 8-bit RGB
     at the full framebuffer size (`[rsi+0x43c]`/`[rsi+0x440]` overwrite the `{640,360}` passed in).
  3. Only **then** does it check the flag: `0xfeb79c cmp byte [rbx+0xae8],0; jne 0xfec08b`
     (assertion `!IsCurrentlySaving()`).
  4. `0xfeb7ad mov byte [rbx+0xae8],1`, `0xfeb7b9 mov qword [rbx+0x628],0`; `0xfeb7cb` removes the
     popup with id `0xd05`.
  5. It builds the command with `make SaveGame` `0xfeb9f9 call 0x15ed140` (r9d = 1, isAutosave).
  6. It posts it with `CommandList::Add` `0xfeba2c call 0x15da840`; the completion lambda
     `0x1019f40` is loaded at `0xfeb7d0`.
- **Save dialog lambda `0x10a8b90`:**
  1. It returns at once if its own captured `*waitForCommand` is set: `0x10a8bb9 mov
     rax,[rdi+0x30]; cmp byte [rax],0` → `ret` at `0x10a8be3`. So a repeated click does not assert.
  2. Screenshot at `0x10a8e26`; `0x10a8e2f mov byte [rax],1` sets `*waitForCommand`.
  3. `0x10a8e41 cmp byte [rax+0xae8],0; jne 0x10aa02b` (assertion); `0x10a8e4e` sets `+0xae8 = 1`;
     `0x10a8e55` sets `+0x628 = 0`; `0x10a8e64 call 0xfe18f0` removes popup `0xd05`.
  4. Factory at `0x10a95b4` (r9d = 0), Add at `0x10a95e7`.
  5. It then calls a captured `std::function` stored at closure `+0x90` (`0x10a9c30 lea
     rdi,[r13+0x90]; 0x10a9c37 call [r13+0xa8]`). INFERRED: this closes the menu, as CreatePage's
     close lambdas `0x10af910`/`0x10af990` do.
  6. When the lambda's third argument (`edx`, kept in `[rbp-0x40c]`) is set, it schedules the
     UI-disable lambda `0x10a5700` (`0x10a9f67`). The completion lambda's capture `+0x8c`
     re-enables it (`0x10a7d55` → `0x10a7ee1`/`0x10a7eef`).
- **An assertion throws.** `0x2fcb860` → `0x2fcb5e0` prints and then calls `0x2fcbb20` →
  `__cxa_throw` at `0x2fcbbc7` (PROVEN in the verification passes). A cross-path overlap (an
  autosave while a dialog save is pending, or the reverse) therefore throws on the main thread.
  Which catch handler ends it is INFERRED: probably Run's, which writes a crash save.
- **Layout of `CmdData::SaveGame`**, from the visitor `0x15e3f10` (PROVEN):

  | Offset | Field |
  |---|---|
  | `+0x18` | `GameConfigData` |
  | `+0xe8` | `GuiSaveData` |
  | `+0x190`/`+0x198` | name `std::string` (data, length) |
  | `+0x1b0` | isAutosave |
  | `+0x1b1` | scenario flag |
  | `+0x1b8`/`+0x1bc` | screenshot width/height |
  | `+0x1c0` | pixel vector |
  | `+0x1d8` (qword), `+0x1e0` (dword) | `std::optional<platform::SaveGameError>` result |
  | `+0xd48` (byte) | variant index, `0x1b` = 27 (checked by the dialog callback at `0x10a7cfe`) |

### 1.3 Simulation Thread: the save itself (PROVEN)

- **Loop.** `RunGameSimLoop` `0xa2ddd0` is entered from the thread body `0xa2e260` (call at
  `0xa2e2c6`). `m_data = [CGame+0x160]`, `gameStates[i] = [m_data+8*i]`, `simIdx = [m_data+0x20]`.
  Each turn:
  1. `0xa2de65 call 0xa61250` (`GameSim::Step`).
  2. Lock `m_data+0x38` (`0xa2deda`), `0xa2def6 mov byte [rax+0xd8],1`, unlock (`0xa2df02`),
     `notify_one` (`0xa2df12`).
  3. Lock again (`0xa2df3d`), then `condition_variable::wait` at `0xa2df76` until `[+0xd9]`, which
     it clears at `0xa2df8f`.
  4. **Unlock at `0xa2e1aa`** (reached through `0xa2df96 jne 0xa2e18f`).
  5. Exit test `[m_data+0x30]` (`0xa2dfa3`).
  6. `0xa2e024 call 0xa8a730` (`GameState::Replicate`, old → new).
  7. The apply loop over the vector `m_data+0x90..+0x98`, stride `0x38`: `0xa2e0e6 call 0x15e2e70`.
  8. `0xa2e17e jmp 0xa2de10`.
- **Apply Command `0x15e2e70`.**
  - The closure is `[rbp-0x70] = {[GameState+8] (GameRes), GameState}` (`0x15e2f67`..`0x15e2f7a`).
  - `0x15e2f9b movzx eax, byte [CmdData+0xd48]`; `0x15e2fc1 lea rdx,[0x59be600]`; `0x15e2fcf call
    [rdx+rax*8]`; then `0x15e2fd9 mov byte [Command+0x30], al`.
  - The table `0x59be600` is referenced only there. Its **slot 27** at `0x59be6d8` is a RELATIVE
    relocation to `0x15e4730`, whose bytes are `f3 0f 1e fa e9 d7 f7 ff ff` (`jmp 0x15e3f10`).
  - Apply has two callers: `0xa2e0e6` (the sim loop) and `0xa2d698` (in `0xa2d650`, the synchronous
    apply used by game-script `sendCommand` inside `GameSim::Step` and at init). There is no script
    maker for SaveGame (`docs/re/GAME_LOOP_AND_UI.md` 'Forcing a save').
- **Visitor `0x15e3f10`.** Its arguments are `rdi` = closure (`0x15e3f1a mov r13,rdi`) and `rsi` =
  `CmdData::SaveGame` (`0x15e3f20 mov rbx,rsi`). It has no `endbr64`; it is reached only through
  the thunk. In order:
  1. Complexity estimate `0x1478100` (`0x15e3f44`).
  2. Two resamples through `0x192c870`: a 640x360 thumbnail (`0x15e3fa2`) and an image capped at
     1920x1080 (`0x15e401b`).
  3. `GameMetadata` built by `0xc79a70` (`0x15e404e`); progress adapter vtable `0x59be738`
     (`0x15e40fb`); `SaveGameId` built (`0x15e4197`..`0x15e41df`).
  4. `0x15e421a call 0xc7ec00`, SaveGame, with:
     - `rdi` metadata, `rsi` screenshot, `rdx = rbx+0x18`, `rcx = [r13]` GameRes,
       `r8 = [r13+8]` GameState, `r9 = rbx+0xe8`;
     - on the stack: `SaveGameId*`, `bool [rbx+0x1b0]`, `IProgressMonitor*`.
  5. The optional result comes back **in RAX:RDX**, with no hidden sret: `0x15e4223 mov
     [rbx+0x1d8],rax` and `0x15e4233 mov [rbx+0x1e0],edx`.
  6. The visitor returns `!byte [rbx+0x1e0]` (`0x15e4280 movzx r13d,byte [rbx+0x1e0]`, `0x15e4293
     xor r13d,1`). After SaveGame returns it only destroys locals.
- **SaveGame `0xc7ec00`:**
  1. `0xc7ec5c cmp qword [SaveGameId+8],0; jne 0xc80425`: the path must be empty, so the save goes
     to the standard save folder.
  2. `0xc7ec72` progress `"Saving..."`.
  3. **Backend.** `0xc7ec88 call 0x18ba890`, which returns `[0x5a52d80]`; then `0xc7ec90 call
     0x18ba270`, which is `mov rdi,[rdi]; mov rax,[rdi]; jmp [rax+0x20]`. The runtime type is
     `platform::StandardSaveGameBackend` (ctor `0x3340cf0` stores vptr `0x59dbbf0`), or a
     `ValidatingSaveGameBackend` that forwards every slot to an inner object at `+8` (ctor
     `0x3347400` stores vptr `0x59dbd28`; that ctor has no direct callers). Which one is live is
     INFERRED.
  4. **Autosave rotation comes first.** `0xc7ec35 mov ebx,[rbp+0x18]`; `0xc7eccd test bl,bl; jne
     0xc7fdd0`. That path calls `GetAutoSaveName` via `[vtbl+0x88]` (`0xc7fe72 call r13`), runs the
     `DeleteSavegame` loop `0xc77f70` (`0xc7fec8`..`0xc7fed7`), and jumps back with `0xc7ff46 jmp
     0xc7ecd5`. Old autosaves are deleted **before** any new file is opened.
  5. Mount `[+0x18]` (`0xc7ed60`), a disk-space log `[+0x30]`, then `<name>.sav` via OpenWrite
     `[+0x50]` (`0xc7ee7e`). The timestamped log goes through `ThreadSafeLocalTime` (`0xc7ef1c`).
     Then the zstd iostream chain `0xc7b4d0` with a `tf**` header, and the `GameState`/`GameRes`
     writes.
  6. `<name>.sav.lua` (`0xc7f844`, `0xc7f879`). It runs every game script's `save()` through
     `0xa84260`, then the `game_script_util::Save` lambda `0x1c90b50`, then `[+0x78]` at
     `0x1c90c48`. `res/scripts/serialize.lua` is run in a fresh Lua state (`0x3282300` → DoFile
     `0x327b9e0`).
  7. `<name>.jpg` only when pixels exist (`0xc7fa6f`..`0xc7fab5`); JPEG encoding at `0xc7fc3b`.
  8. Read-back through `GetSavegameInfo` `[+0x80]` (`0xc7fcd9`) and the `SaveGame version=` log
     (`0xc7fce7`).
  9. The `.info` branch is dead: `0xc7ee3f call 0x9d7ba0` is `xor eax,eax; ret`.
- **Catch handlers.** Several handlers in the cold block `0x6f2740` log `"save game failed: "` to
  `std::cerr` (`0x5bbf440`) and resume inside SaveGame (`0x6f2878 jmp 0xc7ff70`, `0x6f298a jmp
  0xc7fd21`). Two others rethrow (`0x6f2b26`, `0x6f2e02`). Most failures therefore come back as a
  `SaveGameError`; which exception types rethrow is INFERRED.
- **Temp file and rename.** Every file is written as `.tmp` and renamed:
  - OpenWrite `0x3344280`: the `.tmp` literal `0x3f4acc4` (`0x33442bc lea rdx,[0x3f4acc8]; 0x33442c3
    lea rsi,[rdx-4]`); mutex `backend+0x40` locked at `0x334443a`; `0x3344458 call 0xa070b0` (path
    + `.tmp`); `0x334459e` `basic_filebuf::open(..., 0x34 = out|trunc|binary)`.
  - Commit `[+0x58]` `0x3341250`: lock at `0x3341293`; `0x3341465 lea ".tmp"`; `0x334149b call
    0x31f6b10`, which renames `path.tmp` to `path` (rdi = rbx = path.tmp, rsi = r13 = path).
  - Commit runs from a scope guard whose destructor `0x9b76f0` (`cmp qword [rdi+0x10],0; call
    [rdi+0x18]`) also runs during unwinding (`0x6f2805`).
  - The binary imports no `fsync` or `fdatasync`.
- **Files a save consists of:** `<name>.sav`, `<name>.sav.lua`, and `<name>.jpg` when a screenshot
  exists. This comes from the code alone: the save folder
  `userdata/125253817/1066780/local/save/` is empty on this machine (UNPROVEN on disk; E1 lists it).

### 1.4 Completion runs on the main thread, one handover later (PROVEN)

- **Swap.** `CommandList::Swap` `0x15dbc00` has one caller, Sync (`0xa30f0b`), which calls it with
  the `m_data+0x38` mutex held (locked at `0xa30d41`, unlocked at `0xa313f2`).
  - It fires the list's signal with the executed vector (`0x15dbc3d call 0x15e1fe0`).
  - It destroys the commands (`0x15dbc5f call 0x15d8f30` per `0x38` entry), swaps in the pending
    vector, and flips `+0x18` (`0x15dbca7`).
- **Autosave callback `0x1019f40`.** Its arguments are `(closure, bool* ok, optional* err)`. It
  removes the popup (`0x1019f74`). On `ok` it checks `+0xae8` (`0x1019f81`) and clears `+0xae8` and
  `+0x628` (`0x1019f8a`, `0x1019f91`).
- **Dialog callback `0x10a7ca0`:**
  - clears `*waitForCommand` (`0x10a7cf7`);
  - checks `[CmdData+0xd48] == 0x1b` (`0x10a7cfe`);
  - checks and clears `+0xae8` and `+0x628` (`0x10a7d0e`..`0x10a7d22`);
  - reads `[CmdData+0x1d8]`, `[CmdData+0x1e0]` and `Command+0x30` (`0x10a7d2d`..`0x10a7d42`).
- **Timing.** `IsCurrentlySaving` is cleared at the first Sync that follows the batch whose apply
  loop ran SaveGame.

### 1.5 What stops during a save, and what does not (PROVEN unless marked)

- **Nothing is paused by a flag or by speed.** The one Simulation Thread is busy inside SaveGame,
  so it does not set `+0xd8` until SaveGame, the rest of the apply loop and the next
  `GameSim::Step` have finished.
- **`CGame::Sync` never blocks.** `0xa30d55` takes `steady_clock::now()` into r14, and `0xa30d90`..
  `0xa30dd9` compute the deadline as `system_now + (r14 - steady_now)`, a time already past. After
  `0xa30ddd pthread_cond_timedwait`, if `+0xd8` is still 0 it unlocks (`0xa30e0c`) and returns 0
  (`0xa30e2d`).
- **Frames keep rendering a frozen world.** `CGame::Step` then clamps `totalTime` (`0xa31c6d je
  0xa31d10` → `0xa31ca2`..`0xa31cad`).
- **The mod's Lua stops.** `update()` is called only from `GameSim::Step` (once per sim iteration,
  `0xa6180a`; once per batch while paused, `0xa61882`), so it does not run during a save and the
  heartbeats stop.
  - A saving leader: followers run on at the last session speed, then slow down once it is back.
  - A saving follower: it falls behind, and catches up when more than `K.CATCHUP_MIN` units behind
    (INFERRED from pacing.lua `pidPace`/`catchUpTick`, ARCHITECTURE.md 'Pacing').
- **The main thread also stalls for the screenshot readback** before posting (`0x192cf10`), in
  every save path. Its size is UNPROVEN (E1).

### 1.6 The other ways the local simulation stops (context)

- **The in-game menu flag `CGameUI+0x597`:** part 3.
- **The `CGame::Lock` token** `std::shared_ptr<bool> CGame::Lock(bool)` `0xa2e300` (PROVEN):
  - It `make_shared`s a bool (`0xa2e34c`), stores the pointer at `CGame+0x280` (`0xa2e350`) and the
    control block at `+0x288` (`0xa2e383`).
  - Sync weak-locks `+0x288` first (`0xa30cdc`..`0xa30d09`). If the token is alive and `+0x280` is
    non-null it returns false (`0xa30e18`..`0xa30e2b`) without polling.
  - The wrapper `0xdcafd0` is called from `0xe28459`, `0xe32d8b`, `0xe7e149`, `0xe7f046` and
    `0xec90c3` (`UI::TrackModifier::SetProposalStatus`).
  - So a construction proposal can freeze a peer and hold back a pending SaveGame command.
    `SLICE_CONSTRUCTION.md` does not mention it; that is for the slice port.
- **Speed 0** (SetGameSpeed): `update()` still runs, so pacing sees it.

### 1.7 Corrections to earlier notes, folded in

- **Temp files.** OpenWrite writes `<file>.tmp` and Commit renames it (§1.3). The mapping claim of
  in-place writes with no rename is refuted.
- **The commit is a scope guard,** not a close callback.
- **Backend type.** It is not proven to be `StandardSaveGameBackend` (§1.3).
- **Dialog re-entry.** A repeated dialog save returns silently. Only cross-path overlap asserts, and
  the assertion throws.
- **Screenshot.** No framebuffer resolve is done: `0xc51ac0` runs with `sil = 0`. There are two
  downscales, not one.
- **Mod commands.** The mod's `api.cmd.sendCommand` from `update()` goes through the synchronous
  apply `0xa2d650` inside `GameSim::Step` (install lea `0xa32fc6`). Only main-thread and GUI-state
  commands (`0xa2f500`) share the swapped CommandList with SaveGame.
- **Threads.** Sync never blocks the main thread, and `'Render Thread'` is a profiler label.
- **`std::cout`/`std::cerr`:**
  - Run `0x9b3d50` installs `Run(...)::ThreadSafeStreamBuffer` (vtable `0x5a00f98`, lea at
    `0x9b49b2`) on `cout` (`0x9b4a05 call basic_ios::rdbuf`) and on `cerr`. Its xsputn `0x9ad830`
    and overflow `0x9ad8c0` lock `this+0x48` (`0x9ad861`, `0x9ad8e8`).
  - It restores the old buffers at `0x9b4c63`/`0x9b4c70`. It then opens `stdout.txt` as an
    `ofstream` (`0x9b4cd7 call 0x9b7a50`) and points `cout` at its filebuf `rbx+8` (`0x9b4d0a`),
    and `cerr` at the same buffer (`0x9b4d2b`).
  - Which buffer is live during play is INFERRED: the filebuf, by code order. §2.4 handles both.
- **Offsets.** The Windows forced-autosave offsets (`+0x648`, settings `+0x130`) do not apply. On
  Linux they are `+0x628` and `GlobalSettings+0x120` (MENU_GAME.md HJ-03/HJ-04).
- **`update()` runs once per sim iteration** on Linux, not once per rendered frame.

## 2. fork()-based non-blocking saving

### 2.1 Verdict: GO WITH CONDITIONS

The Linux build offers a fork point where the saving thread holds no game lock (§1.3). At that point
the state being saved has no other writer (§2.2), and the parent needs back only
`Command+0x30`, `CmdData+0x1d8` and `CmdData+0x1e0`, all proven. The remaining risks are runtime
ones:
- a child blocked on a lock that another thread held at the moment of fork;
- fork latency and copy-on-write cost;
- container differences.

All of them are made survivable by a watchdog with a blocking fallback, and measurable by E1–E9.

### 2.2 The fork point: visitor-table slot 27

Swap the pointer at `base+0x59be6d8` from `base+0x15e4730` to `SaveVisitStub(void* closure, void*
cmd)`.

This is the right place because:
- **No concurrent writer to the snapshot.**
  - The Simulation Thread is the only writer of `gameStates[simIdx]`, and at this instant it is
    inside our stub.
  - The main thread cannot run `ScriptSwapReplicate` or `CommandList::Swap`, because Sync needs
    `+0xd8`, which the sim thread sets only after the next `GameSim::Step` (`0xa2def6`).
  - So the GameState, its script state (`GameState+0x220`) and the command are quiescent (PROVEN).
  - The render path reads the other buffer, `CGame+0x150`, which Sync sets at `0xa30ecb`. That is a
    read, not a write.
  - INFERRED: Sim Pool workers from `GameSim::Step` are idle by the time the apply loop runs.
- **No game lock is held by the forking thread:** `m_data+0x38` was released at `0xa2e1aa`.
- **The inputs are complete.** The pixels were read back on the main thread before `Add`, and the
  config, GUI data, name and flags are in `CmdData`. The child needs no GPU, SDL, OpenAL or Steam.
- **The child runs the stock visitor unchanged** (resamples, metadata, SaveGame), so the parent
  pays only for `fork()` itself.
- **It covers exactly the autosave and the Esc-dialog save.** Slot 27 is reached only through
  Apply. The crash save calls SaveGame directly and is unaffected.
- **Relocation.** The slot is a RELATIVE relocation inside `.data.rel.ro.local`
  (`0x59a8720`..`0x59fef40`), which is covered by `PT_GNU_RELRO` `0x59a8700`..`0x5a48000` (PROVEN,
  `readelf -lW`/`-SW`). The dynamic loader leaves that range read-only after relocation, so the
  install must `mprotect` the page RW, compare-and-swap, and restore `PROT_READ`. The port's own
  vtable swaps (MENU_GAME.md AL-05/HJ-02; `0x5a11578` is in `.data.rel.ro`, the same RELRO range)
  need the same.

What the snapshot contains:
- the state after `GameSim::Step` N, plus `Replicate`, plus the commands before SaveGame in the same
  swapped vector;
- every command the mod issued from `update()`, because those were applied synchronously during the
  Step (§1.7).

Only main-thread commands queued after SaveGame in the same vector are missing. In a live session
the slice cancels UI commands, so that set is expected to be empty (INFERRED).

Alternatives considered:
- **Redirect `call SaveGame` at `0x15e421a`** (bytes `e8 e1 a9 69 ff`) to a stub that returns
  `RAX = RDX = 0` in the parent.
  - For: no RELRO write, and the visitor stores the result itself.
  - Against: the parent still pays for the two resamples, the metadata and the complexity estimate
    (unmeasured).
  - Kept as the fallback install if the slot swap is refused.
- **Fork on the main thread inside Sync while the sim thread is parked.**
  - For: both states are quiescent.
  - Against: the SaveGame command has not been applied yet. The child would have to pick it out of
    the pending list and apply it by hand, and the parent would have to remove it.
  - Rejected as too invasive.
- **Fork after the whole apply loop** (`0xa2e0f0`).
  - For: consistent with same-batch UI commands.
  - Against: it needs a code patch inside `RunGameSimLoop` plus state passed from the visitor, for
    no gain in a live session.
  - Rejected.

### 2.3 Parent sequence (Simulation Thread, inside the stub)

1. **Mode:**
   - `off`: tail-call the original.
   - `observe`: time the original, log, return its result.
   - `live` (only while a session is live) or `always`: continue.
2. **Preflight**, using values the watcher thread caches, so no `/proc` read happens on the sim
   thread:
   - no child already in flight; otherwise wait for it (§2.6);
   - MemAvailable ≥ max(2 GiB, RssAnon / 2) (threshold UNPROVEN, E8);
   - fewer than two fork failures this session.

   If a check fails, call the original synchronously: today's blocking save.
3. **Park the main thread** (switch `park=1`, default on).
   - How: set `parkRequested`. The main thread's `call CGame::Step` at `0xfe7828` in `GameStep`
     (bytes `e8 03 a4 a4 ff`, single caller of `0xa31c30`) is redirected with `Tpf2mpRedirectCall`
     to a gate. The gate blocks on a condition variable while a park is requested, then calls
     `CGame::Step`.
   - Wait at most 50 ms for `mainParked`. If the frame is not running (menu gate set, loading,
     `StopGame`), go on without it and log.
   - Why: `UI::Clock::DoStep` `0xf6c4b0` calls the boost::locale date formatter `0x32081a0` at
     `0xf6c51d`, skipped only when `[this+0x478] < 0` (`0xf6c4dc js`). The formatter uses
     `boost::locale::calendar` and `date_time`, backed by ICU. `GetAutoSaveName` `0x3342cb0` calls
     the same helper at `0x3343200`. A fork while the main thread is inside ICU could leave an ICU
     mutex held in the child.
   - The main thread parked inside `CGameUI::GameStep` is between UI components, so it is not in
     `Clock::DoStep` (PROVEN: sequential on one thread).
   - Whether ICU really takes a mutex on that path is INFERRED.
4. **Try-lock, in this order**, each with `pthread_mutex_trylock`. On any failure, release all,
   sleep 1 ms, and retry for up to 20 ms; then fall back to a blocking save for this command. The
   locks and why each is needed are in §2.5.
   1. **Save backend.** Call the game's getters: `obj = 0x18ba270(0x18ba890())`.
      - If `*obj == base+0x59dbd28`, set `obj = [obj+8]`.
      - Require `*obj == base+0x59dbbf0`; otherwise skip the fork.
      - The mutex is `obj+0x40`.
   2. **Game time mutex** at `base+0x5b36ee0`.
   3. **Log stream.** For `cout` (basic_ios `base+0x5bbf588`, `_M_streambuf` at `+0xe8` =
      `base+0x5bbf670`) and `cerr` (`base+0x5bbf448`, streambuf pointer at `base+0x5bbf530`): if
      the buffer's vptr is `base+0x5a00f98` (`ThreadSafeStreamBuffer`), lock `buffer+0x48`.
5. **`fork()`**: glibc's wrapper, not `_Fork` or a raw `clone`. It locks and resets the malloc
   arenas, stdio, NSS and loader locks around the fork; INFERRED from glibc `posix/fork.c`, as read
   in the fork-safety mapping.
6. **Parent after the fork:**
   1. Unlock step 4 in reverse order and release the park.
   2. Register the child: pid, `pidfd_open(pid)`, the pipe's read end, `t0`, kind
      (`[cmd+0x1b0]`), name (`[cmd+0x190]`/`[cmd+0x198]`).
   3. Write `qword [cmd+0x1d8] = 0` and `dword [cmd+0x1e0] = 0` (a disengaged optional), and
      return `true`.
   4. Apply stores `true` at `Command+0x30` (`0x15e2fd9`). At the next Sync the completion lambda
      takes its success path and clears `+0xae8`.
7. **Log** the fork's wall time, the lock wait and retries, and the park wait.

| When | Main thread | Simulation Thread | Child |
|---|---|---|---|
| frame F | AutoSave: readback, `+0xae8 = 1`, Add | step N | |
| Sync k | swaps the command in, wakes the sim | | |
| | reaches the gate at `0xfe7828`, parks | Replicate, apply → stub: park, try-locks, fork | born |
| | released | writes the result, returns; `GameSim::Step` N+1 | unlocks, signals, streams, visitor, `_exit` |
| Sync k+1 | Swap → callback clears `+0xae8` | step N+1 done | still writing |
| later | | | exits; the watcher writes the done record |

### 2.4 Child sequence (one thread; it never returns into game frames)

1. **Unlock the step-4 locks.** They are default-type `std::mutex`, whose unlock does not check
   the owner (INFERRED). Re-initialising them to zero is equivalent.
2. **Signals:**
   - `SIG_DFL` for SIGSEGV, SIGBUS, SIGILL, SIGFPE, SIGABRT, SIGTRAP and SIGSYS. Breakpad's
     handlers (all its `signal`/`sigaction` call sites are in `0x36c33d0`..`0x36c4520`) would
     otherwise write a minidump from the child, and its GenerateDump clones a process itself.
   - Ignore SIGPIPE.
3. **Silence the game's streams.** Call the game's PLT stub `basic_ios<char>::rdbuf(streambuf*)`
   at `base+0x6dc0e0` with `(base+0x5bbf588, nullptr)` and `(base+0x5bbf448, nullptr)`.
   - A null buffer sets badbit, so every insert and `std::endl` becomes a no-op. That includes the
     catch handlers' `cerr` lines.
   - Result: no stream mutex in the child, and no duplicate or torn lines in `stdout.txt`.
   - Our own code in the child never logs; it writes fixed-size records to a pipe.
4. **Process identity and priority:**
   - `prctl(PR_SET_NAME, "tpf2-save")`;
   - `setpriority` +10; `ioprio_set` best-effort 7;
   - write `500` to `/proc/self/oom_score_adj`. Raising it needs no privilege; whether the snap
     AppArmor profile allows it is UNPROVEN (E3). Ignore a failure.
5. **Do not set `PR_SET_PDEATHSIG`.** A save that has started finishes even if the game exits
   (§2.6).
6. **Optional, off until E5 passes:** `close_range(3, ~0)` except the pipe. This drops inherited
   X11, Vulkan device, io_uring and socket descriptors. The risk is that SaveGame needs a
   descriptor opened before the fork.
7. **Save:**
   1. Write a `start` record.
   2. `ok = originalVisitor(closure, cmd)` inside `try { } catch (...) { }`.
   3. Write a `result` record `{ok, byte [cmd+0x1e0], qword [cmd+0x1d8], times}`.
   4. `_exit(ok ? 0 : 3)`; `_exit(4)` from the catch.

- **Why `_exit`.** `exit()` runs the `__cxa_atexit` handlers. They include the ThreadPool singleton
  deleters (`0x31d16a0` → `0x31d06e0`, whose `std::thread::join` is at `0x31d0733`), which would
  wait for threads that do not exist in the child. They also include OpenAL and Steam teardown
  over descriptors the parent still uses.
- **Why `catch (...)`.** An exception escaping the visitor would reach the thread body `0xa2e260`.
  Its handler (cold `0x6e753e` → `0x6e7448`) stores the exception at `m_data+0x1b0`, and the thread
  function returns. In a process whose last thread returns, glibc calls `exit()` (INFERRED).
- **Caveat.** Our libraries link libstdc++ and libgcc statically. That `catch (...)` in our frame
  catches an exception thrown by the game's libstdc++ rests on the shared Itanium ABI (INFERRED).
  The child only has to reach `_exit`, and SaveGame already turns most failures into a
  `SaveGameError` (§1.3).

### 2.5 Locks the child can meet, and how each is handled

| Lock | Where the save takes it | Who else holds it | Handling |
|---|---|---|---|
| glibc malloc arenas, stdio, NSS, `dl_load_lock` | libc | any thread | glibc `fork()` resets them (INFERRED from glibc source) |
| save backend `obj+0x40` | Mount `0x3344dd3`, OpenWrite `0x334443a`, Commit `0x3341293`, delete `0x3340981` | Save Pool list refresh `0xf2b4a0` → `0xc7e520` → Mount `[+0x18]` at `0xc7e56b`; opening the Load/Save page starts one | try-lock before fork (PROVEN sites) |
| game time mutex `0x5b36ee0` | `ThreadSafeLocalTime` `0x3206c60` (lock at `0x3206ca1`, `localtime` at `0x3206cad`), `ThreadSafeAsctime` `0x3207be0` | log timestamps (`0x9ad1a0`), MinidumpCallback | try-lock before fork |
| `ThreadSafeStreamBuffer+0x48` (only if that buffer is live) | every `cout`/`cerr` write | any logging thread | try-lock if present; the child nulls both streams |
| glibc timezone lock inside `localtime`/`asctime` | the two wrappers above | raw `localtime_r` callers `0x9951ce` (Lua `os.date`), `0x30bfed9`, `0x37da37e` | **not covered** → watchdog |
| glibc iconv (gconv) lock | Lua DoFile path conversion `0x32342b0` → `boost::locale::conv::to_utf` (`libboost_locale.so.1.76.0` imports `iconv_open`, `iconv`, `ucnv_open_61`) | `FileSystem::Resolve` `0x31961d0` and `GetFile` `0x31972b0` on loader threads (INFERRED) | **not covered** → watchdog |
| ICU internal mutexes | `GetAutoSaveName` → `0x32081a0` (autosaves only) | main thread in `UI::Clock::DoStep` (`0xf6c51d`); Save Pool (INFERRED) | main thread parked; others → watchdog |
| file-system resolver caches | DoFile of `serialize.lua` | resource loading | lock status not mapped (UNPROVEN) → watchdog |
| `m_data+0x38`, the CommandList signal mutex | Sync, Add, Swap | main thread | the child never takes them |
| zstd worker pool | none: no pool is created on the save path; only `POOL_free` `0x3dfcf80` is reachable (INFERRED single-threaded compressor) | — | none |

- **Why lock around the fork instead of `pthread_atfork`.** Other code in this process forks too:
  libSDL2, the NVIDIA libraries, PipeWire/Pulse, `steamclient.so` and `crashhandler.so` (per the
  fork-safety mapping). Registered handlers would run for all of them. Locking in the stub scopes
  the locks to our fork. The handlers already registered in the process are harmless: steamclient's
  prepare/parent/child are bare `ret`, and its and the overlay's child handlers only reset
  thread-local fields (PROVEN in that mapping).
- **The game binary imports no fork machinery.** `nm -D` shows no `fork`, `vfork`, `clone`,
  `posix_spawn*`, `pthread_atfork`, `__register_atfork` or `waitpid`. It does import `_exit`,
  `exit`, `__cxa_atexit`, `system`, `popen`, `kill`, `getpid`, `syscall`, `signal`, `sigaction`,
  `pthread_create`, `rename`, `localtime` and `localtime_r`. Our library calls libc's `fork`.

### 2.6 Watchdog, reaping, fallback, overlap and shutdown

- **Watcher thread.** Our library starts it at install, so it never exists in a child. It polls
  the child's pipe and pidfd every 250 ms.
  - The `result` record is authoritative. End of file without one means the child died.
  - Status comes from `waitid(P_PIDFD, pidfd, WEXITED)`. On `ECHILD` (a library set SIGCHLD to
    `SIG_IGN`) the record alone decides.
  - The game binary sets no SIGCHLD disposition: every `signal`/`sigaction` site is Breakpad's, and
    `__sysv_signal` is called only with 13 (SIGPIPE) at `0x3e3e264` and `0x3e3e3a7`. Libraries were
    not checked (UNPROVEN, E0).
  - Never `waitpid(-1)`: `popen`/`pclose` (`0x990362`/`0x9903d4`) and `system` (`0x322d43b`,
    `0x322d5be`, `0x322d6de`) wait for their own children.
- **Progress and timeouts:**
  - progress is `wchar` in `/proc/<pid>/io`;
  - stall: no change for 30 s;
  - hard cap: max(120 s, 10 × the median child duration).

  On a stall or the cap, `SIGKILL` our own child and reap it.
- **After a failure:**
  - Delete `*.tmp` files in the save folder whose mtime is ≥ `t0`.
  - Log the error and show a notice in the panel.
  - For an autosave or a hot-join save: make the next SaveGame blocking (`forceBlockingNext`), and
    have the lobby request it again.
  - For a manual save: the notice asks the player to save again.
  - After two failures in a session, the mode drops to blocking for the rest of that session.
- **Overlap.** A SaveGame command that arrives while a child is alive waits in the stub for that
  child for up to 10 s; after that, kill it and continue. This is the Factorio 2.0.33 rule: the
  exit save waits for the async save.
  - The stock `+0xae8` guard no longer serialises saves, because the first callback cleared the
    flag at the next Sync.
  - Two children must never write the same names: OpenWrite truncates `<name>.tmp` (mode `0x34`).
- **Quit to menu, load, exit:**
  - The child is unaffected when the parent tears down its GameState; it has its own copy.
  - Loading the save that is being written can pair a new `.sav` with an old `.sav.lua`.
    `MenuGame_RequestAutoload` and the Load page should wait while a child is in flight. Whether a
    player can reach this is UNPROVEN (E5).
  - `0xfe22f0` (`IsCurrentlySaving() || 0xe09a20(...)`) is read by `CreatePage` (`0x10ad696`,
    `0x10ad75d`). INFERRED: it greys out menu entries. With a fork save it clears after one
    handover, so it no longer protects the files.
  - Process exit: no `PDEATHSIG`, so the child finishes. INFERRED: Steam's reaper keeps the game
    "running" until it exits. If the game leaves through `exit()`, our library destructor waits up
    to 30 s (UNPROVEN, E5).

### 2.7 Files, atomicity and durability

- **Order on disk:** rotation deletes, commit `.sav`, commit `.sav.lua`, commit `.jpg`. Each is a
  per-file `.tmp` then rename (PROVEN), with no fsync (PROVEN).
- **A killed child can leave:**
  - one autosave fewer, because rotation already ran;
  - a new `.sav` next to an old `.sav.lua`;
  - `*.tmp` orphans.
- **Rule:** a save set is complete only when there is an `ok` record **and** both `.sav` and
  `.sav.lua` have an mtime ≥ `t0`. The `.jpg` is optional.
- **Durability** is the same as the stock save; a parent-side fsync buys nothing the stock save
  has.
- **Possible later improvement:** rotate in the parent after success. That would mean
  re-implementing `GetAutoSaveName`; not in version 1.

### 2.8 Memory and latency (estimates; UNPROVEN until E2, E3, E8)

- **Fork latency.** `fork()` copies page tables and write-protects private memory while holding
  the parent's mmap lock. Published figures are roughly 9–13 ms per GB resident (Redis latency
  documentation, cited by the fork-safety mapping).
  - At an assumed 6 GB late-game RSS that is 55–80 ms on the Simulation Thread.
  - Render and audio threads that page-fault in that window wait too.
  - NVIDIA device mappings are `VM_PFNMAP` and are copied, not skipped (INFERRED from `nv-mmap.c`
    flags).
- **Copy-on-write.** Every page the parent writes while the child is alive is copied once.
  - The ECS is double-buffered and `Replicate` runs every batch.
  - Component change tracking (`NoteComponentAboutToBeChanged` in the speed writer `0x179d250`)
    suggests only changed components are replicated (INFERRED). The cost is then roughly the dirty
    set per batch × batches during the child's life.
  - Upper bound: one extra copy of the parent's private dirty memory for as long as the child
    lives.
- **Child duration** is about a stock save's duration. It is unmeasured on Linux; Windows modelled
  16–36 ticks (`tools/pacing_sim.py` autosave scenarios) and measured a 113 MB autosave.
- **This host:** 31.4 GB RAM, 8 GB swap, `overcommit_memory = 0`, THP `madvise` (the game does not
  madvise, so pages are 4 KiB), 24 CPUs, `RLIMIT_NPROC` 122524.
- **Expectation:** a fork pause of tens to about 150 ms, plus CoW slowdown in the next batches,
  instead of a multi-second stall. Pacing absorbs it: under one unit is inside the PID band
  (INFERRED).

### 2.9 Container and process notes

- **pressure-vessel.** It starts `srt-bwrap` with `--not-a-security-boundary` (log line 14). The
  log has no `--unshare-pid`, `--die-with-parent` or `--seccomp` (0 matches). `libc.so.6` is the
  host's through `overrides/` (log line 1461). Host glibc is 2.43 (`ldd`); kernel 7.0.0-31.
- **Snap seccomp** allows `clone`, `clone3`, `fork` and `vfork` (`snap.steam.steam.src` lines 112,
  113, 162, 659). AppArmor label `snap.steam.steam` (fork-safety mapping).
- **Other packagings** (Flatpak, a native runtime) or a low `RLIMIT_NPROC` can make `fork()` fail
  with EAGAIN or ENOMEM. The stub then saves blocking.
- **io_uring** belongs to the mod.io SDK: `io_uring_queue_init` is called only from `0x3838420`.
  The save path reaches none of it. The child never enters mod.io code.
- **Preloaded objects.** The child inherits the ones already mapped (Steam's
  `gameoverlayrenderer.so`, our `libtpf2mp_boot.so`). Nothing is exec'd, so the environment does
  not matter.

### 2.10 How it plugs into the mod

- **Forced autosave** (hot join, `/sync`, relay uploads).
  - The trigger stays MENU_GAME.md HJ-06: `2^62` into `+0x628` inside the CGameUI update detour,
    guarded by `+0x597 == 0`, `+0xae8 == 0` and `GlobalSettings+0x120 > 0`.
  - The fork save changes only what happens once AutoSave's command reaches slot 27.
  - Still true: with the autosave interval at 0 the force cannot fire (HJ-04), and it cannot fire
    while the stock menu gate is closed (§3.1).
- **When to share the save (SyncPoll in `lobby_linux.cpp`).** Replace "newest `.sav` whose size did
  not change between two polls" with a done record.
  - The watcher writes `DATADIR/tpf2_savefork_done.txt` atomically: `seq=`, `result=ok|failed`,
    `kind=`, `sav=`, `lua=`, `jpg=` (or `-`), `t0_ns=`, `fork_ms=`, `child_ms=`.
  - SyncPoll shares when `seq` is newer than at SyncStart, `result=ok`, and both files exist.
  - The autosave's name is chosen in the child (`GetAutoSaveName`), so the watcher takes the newest
    `*.sav` with mtime ≥ `t0` after the child exits. That is unique because saves are serialised.
  - With the fork save off, keep today's rule but also require `.sav.lua`. That fixes the
    early-share race: `.sav` is committed before `.sav.lua`.
- **Snapshot step S.**
  - The child runs `save()` on the snapshot's own script state:
    - `ScriptSwapReplicate` `0xa848f0` swaps `GameState+0x220` every Sync;
    - the sim-role state runs `update()`;
    - SaveGame saves `gameStates[simIdx]`.
  - So the mod can stamp S inside `save()`, for example `{ cm = ..., at = <game time> }`. The
    joiner's `load()` keeps it, the joiner holds at speed 0 from its first `update()`, and it sends
    `LSNEED t=<S>`.
  - Today `CM.syncBegin` (pacing.lua) writes `step=` into `tpf2_sync_save.txt` at request time.
    Nobody reads it, and it predates the snapshot. `CM.histServe` (net.lua) filters `e.at > S`,
    with S taken from the joiner's own clock.
  - Caveats: `save()` and `load()` also run on every Sync, on the main thread. The stamp must stay
    cheap, and `load()` must adopt it only on a fresh load in the engine state. INFERRED; this is a
    mod change for after the mod workflow finishes.
  - Not fixed by the fork save (known already):
    - commands stamped ≤ S but deferred past S (`notBeforeStep`, `conxQueue`, `retryQueue`) are in
      neither the save nor the history;
    - `hist=1 hfor=` is appended after the greedy `params=` tail (KNOWN_ISSUES.md).
- **Relay periodic uploads** (`RelayPeriodic`, `relay_autosave_min`). Without the host stall, the
  two-minute cadence costs upload bandwidth and a fork pause. Keep the interval above the median
  child duration.
- **Save from the Esc menu during a session.** With the §3 patch the command is applied at the next
  batch and forks. Without it the command waits in the pending list until Sync runs again, which
  happens when the menu closes (INFERRED; E6).
- **Mixed Windows/Linux sessions.** Windows peers still stall on their own saves, so pacing keeps
  its stall tolerance.
- **Determinism.** Compared with a blocking save, the parent skips one `save()` call on its Lua
  state. That cannot matter:
  - `save()` already runs every Sync;
  - autosave timing is per-machine wall time (µs of frame time accumulated at `+0x628`);
  - a `save()` side effect therefore cannot have been lockstep-relevant.

  This mod's `save()` only builds a table (companies.lua `CM.cmSaveState`).

## 3. No-pause Esc menu while a session is live

### 3.1 How the menu pauses today (PROVEN)

- **The menu object.** `InGameMenuUI` is created at `0xffb098` and stored at `CGameUI+0x5d0`
  (`0xffb09d`, `"ingameMenu"`).
  - `0x100818c call 0x3058230` connects lambda `0xfddd40` to `[menu+0x438]+0x160`.
  - `CComponent::HandleVisibilityChange` `0x3058da0` fires that signal. It is reached from
    SetVisible `0x305a910` → `0x3058fa0`.
- **Lambda `0xfddd40(bool visible)`:**
  - First, `0xfddd8a call 0xe09a20` on `[[CGameUI+0x720]+8]`, where `0xe09a20` is `mov
    rax,[rdi+0x58]; movzx eax,byte [rax+0x2e0]; ret`. If that is true, `0xfddd91 jne 0xfddef1`
    returns without touching the flag. INFERRED: camera playback.
  - Show: it saves HUD visibility unless the flag is already set (`0xfddda3 cmp byte
    [rax+0x597],0; jne 0xfdddf2`), then `0xfdddf2` sets it (bytes `c6 80 97 05 00 00 01`). It
    leaves relative mouse mode and hides `+0x5e0`, `+0x758` and the renderer
    (`0xfdde02`..`0xfdde3c`).
  - Hide: `0xfdde68` clears the flag (`c6 80 97 05 00 00 00`) and restores.
- **The gate** in `0x100fb20`:
  - `0x100fc70 80 bb 97 05 00 00 00` is `cmp byte [rbx+0x597],0`; `0x100fc77 0f 85 a3 05 00 00` is
    `jne 0x1010220`.
  - The gated branch runs only `0x3260150` (audio, 0/false), `0x15c8070([+0x778])` and
    `0x11d35d0([+0x770], t, dt)`, then returns through `0x101025b jmp 0x10101f3`.
  - The fall-through `0x100fc7d` steps `0x32627a0`, `0x1243000`, `0x1245620` and `GameStep`
    (`0x100fcae`), then the autosave block.
- **References to `+0x597`:** readers `0x100fc70` and `0xfddda3`, writers `0xfdddf2` and
  `0xfdde68` (a displacement scan of all of `.text`), plus the constructor's dword initialisation
  at `0x1014412`.
- **What opens the menu:**
  - Esc (`IA_MENU_BACK`) handler `0xfe8d80` (setVisible at `0xfe8e61`);
  - `IA_MENU` toggle `0xfde210`;
  - `ShowInGameMenu` `0xfe1920`, also reached from `CMenuUI` `0x1158a20(false)`. INFERRED: that is
    how closing the window opens the menu;
  - the HUD Menu button lambdas `0xfdcc70`/`0xfdccf0`.
- **Effect.** While the flag is 1: no `GameStep`, `CGame::Step` or Sync, so the Simulation Thread
  stays parked at `0xa2df76`. No `update()`, no `guiUpdate` (its only caller is `0xfe7871`), no
  `ScriptSwapReplicate`, and no command swap, so a posted SaveGame waits. The autosave accumulator
  and any forced autosave wait too.

### 3.2 Verdict: GO WITH CONDITIONS

There is one reader and a contained 13-byte patch, and outside a session the flag keeps its stock
meaning. Conditions:
- The bypass is active only while a session is live.
- E6 shows no world input behind the menu and a clean HUD restore.
- A save from the menu stops stalling only together with the fork save.

### 3.3 The minimal verifiable patch

1. **Verify:**
   - the build-id;
   - the 13 bytes at `base+0x100fc70` are `80 bb 97 05 00 00 00 0f 85 a3 05 00 00`;
   - no branch lands inside the patched range. The five jumps in `0x100fb20` that reach this code
     (`0x100ffa5`, `0x1010069`, `0x10100fe`, `0x1010109`, `0x1010387`) all target `0x100fc70`
     itself, and no rel32 in `.text` targets `0x100fc71`..`0x100fc7c` (PROVEN, scan).
2. **Allocate** two pages within ±2 GB with `Tpf2mpAllocNear(base+0x100fc70, 0x2000)`: code (RX)
   and one flag byte (RW).
3. **Stub** (27 bytes; it touches no register):
   ```
   80 3d <rel32 flag> 00     cmp  byte [rip+flag], 0     ; live?
   75 0d                     jne  resume                 ; yes: ignore the menu flag
   80 bb 97 05 00 00 00      cmp  byte [rbx+0x597], 0    ; the original test
   0f 85 <rel32>             jne  base+0x1010220         ; stock gated branch
   resume:
   e9 <rel32>                jmp  base+0x100fc7d         ; stock fall-through
   ```
4. **Patch** `0x100fc70` with `e9 <rel32 stub>` followed by the 8-byte NOP `0f 1f 84 00 00 00 00
   00`.
   - EFLAGS are free: both targets begin by overwriting them before reading them
     (`0x100fc7d mov rdi,[rbx+0x4b0]; …; call`, and `0x1010220 mov rdi,[rbx+0x550]; xor edx,edx`),
     both PROVEN.
   - Install during library initialisation, before any CGameUI exists, so no thread executes those
     bytes while they are written.
5. **The flag** is 1 only while the session is live, using the slice's rule
   (ARCHITECTURE.md 'Sessions'): `lockstep_status_<L>.txt` is fresh and names a peer. Kill switch:
   `escnopause=0`.

Rejected: writing 0 instead of 1 at `0xfdddf8`. On a second `visible=true` (SetVisible notifies
even without a change) the check at `0xfddda3` would save the already-hidden HUD state again, and
closing the menu would leave the HUD hidden.

### 3.4 What running the frame behind the menu changes (UNPROVEN; E6)

- **Frame work resumes:** `GameStep`, `CollisionShapeRep`, tool steps, `CheckAchievements` and the
  autosave accumulator.
- **Audio** gets `running = GetSpeed() > 0`, so game sounds continue behind the menu.
- **World input behind the menu.** The menu hides `+0x5e0` and `+0x758`, leaves relative mouse
  mode and calls `0x11d8e50(renderer, 0)`. Whether that is enough to stop clicks and hotkeys
  reaching tools is unknown.
- **`guiUpdate` keeps running,** so the MP panel and cursors stay live.
- **The session ends while the menu is open:** the flag goes to 0, the stock gate is back on the
  next frame, and the game pauses (stock).
- **The camera-playback exception:** the flag is never set then, which is stock behaviour and
  unchanged.

### 3.5 Pacing consequences

**Stock pause during a live session (INFERRED from pacing.lua; line numbers drift, so functions are
named):**
- **A follower opens the menu:** its heartbeats stop. Peers mark it stale after
  `K.PEER_STALE_TICKS` = 25. On return the PID closes a small gap. More than `K.CATCHUP_MIN` = 8
  units behind, it catches up: hold at 0, `LSNEED`, up to 4x (`CM.catchUpTick`).
- **The leader opens the menu:** followers lose their reference (`pidPace` returns nil) and run on
  at the last session speed. When the leader returns they slow down in steps: `gapIn` = 1.0,
  `persist` = 6, floor 0.25x. With three or more players a follower may catch up against the
  fastest fresh peer.
- **The leader's saves are delayed:** hot-join, `/sync` and relay saves wait until the leader
  closes the menu, and so does a save made from the menu.

**With the patch:**
- There is no local freeze, so heartbeats, pacing and command intake continue. The player simply
  issues nothing while the menu covers the screen.
- The speed levers are untouched.
- A save from the menu forks. The only remaining hitch is the fork pause, which stays inside the
  PID band (INFERRED).
- Windows peers still pause on Esc; the equivalent Windows patch is outside this document.

### 3.6 Related freezes this does not cover

- the `CGame::Lock` token (§1.6);
- `UI::Clock` pause and speed clicks. The capture return addresses are `0xf6b88e` and `0xf6c28d`
  (SLICE_TIME.md); there is no Linux cancel yet;
- the campaign mission-end `SetGameSpeed(0)` (`0xfe7947`);
- a minimised window or a zero-size swapchain stalling the frame loop (UNPROVEN).

## 4. In-game experiments, in order

The user runs these, on test builds. Each experiment names what to log and what counts as a pass.
Numbers from E1–E3 set the thresholds in §2.3 and §2.6.

- **E0: static preflight at load (no patch).**
  - Log:
    - the build-id;
    - the bytes at `0x100fc70` (13), `0x15e4730` (9), `0x15e3f10` (19), `0x15e421a` (5),
      `0xfe7828` (5) and `0xa31c66` (5);
    - the value in slot `0x59be6d8`;
    - the `cout`/`cerr` buffer vptrs (`0x5a00f98` or other);
    - the backend vptr after `0x18ba270(0x18ba890())` (`0x59dbbf0` or `0x59dbd28`);
    - `/proc/self/status` (VmRSS, RssAnon, Threads), the mapping count at the title menu and in
      game, MemAvailable, `RLIMIT_NPROC`, and SIGCHLD's disposition (`sigaction(SIGCHLD, NULL,
      &old)`).
  - Pass: every byte matches; both vptrs are recognised; SIGCHLD is not `SIG_IGN`, or the pipe
    record path is confirmed.
- **E1: observe mode** (slot swapped; the stub times and calls the original).
  - Log per save:
    - kind and name;
    - main-thread time in AutoSave or the dialog around the readback (frame-time spike);
    - sim-thread stub entry and exit (the blocking duration);
    - Sync-false streak length (counted in the `0xfe7828` gate);
    - `+0xae8` and `+0x597` changes per frame;
    - RSS before and after;
    - after completion, the save folder listing: names, sizes, mtimes, any `*.tmp`.
  - Use a small map and a late-game map.
  - Pass: the file set is `{.sav, .sav.lua, .jpg}`; typical and worst durations are recorded.
- **E2: fork probe without saving.**
  - In the stub, `fork()` a child that `_exit(0)`s at once, then call the original synchronously.
  - Log: the fork's wall time; `getrusage` minor faults for the next 20 batches; batch durations
    from `m_data` timing fields `+0xa8`..`+0xd0` (written at `0xa2dea7`..`0xa2e177`; their meaning is
    INFERRED); main-thread frame times; the reap result.
  - Pass: fork ≤ 150 ms on the largest map; no crash; no audio, overlay or Steam glitch; no zombie.
- **E3: fork save, single player** (`mode=always`, not live).
  - Log: park wait, lock retries, fork milliseconds, child records (start, visitor return, result),
    child wall time, the `wchar` trajectory, parent and child RSS peaks, the done record.
  - Equivalence test (test build): after the fork, the parent waits for the child, renames its
    files to `<name>_fork.*`, then runs the original on the unchanged state. Compare the
    decompressed `.sav` and the `.sav.lua`.
  - Pass: identical apart from wall-clock fields; the forked save loads, and two instances loading
    it keep matching hashes.
  - Also confirms whether `oom_score_adj` and `ioprio` are allowed.
- **E4: failure injection** (test switches).
  - Cases: the child raises SIGKILL after its start record; the child sleeps forever before the
    visitor; `fork()` is forced to fail; a save error such as a full disk.
  - Log: watchdog decisions, `.tmp` cleanup, the fallback save's result, `+0xae8` afterwards, the
    next autosave firing, the done record.
  - Pass: no hang beyond the cap; the next save succeeds; no `*.tmp` left; no zombie.
- **E5: overlap and lifecycle.**
  - Cases:
    - a 1-minute autosave interval plus a manual save during a child;
    - a forced autosave (HJ-06) during a child;
    - quitting to the main menu during a child;
    - loading the save being written;
    - exiting the game during a child;
    - `close_range` in the child switched on.
  - Log: stub wait times, results, file sets, when Steam shows the game as stopped.
  - Pass: saves are serialised; no `!IsCurrentlySaving()` assertion; a load never reads a mixed
    set (or it waits); exit completes; with `close_range` on, SaveGame still succeeds.
- **E6: no-pause menu, two instances live** (flag on; test on the leader and on a follower).
  - Keep the menu open for at least 60 s.
  - Log per frame: `+0x597`, `CGame::Step` calls, `CM.ticks` (status file), heartbeats sent and
    received, pacing decisions, locally issued commands, audio.
  - Click where a tool would build behind the menu; press speed and bulldoze hotkeys; save from the
    menu (log click time and slot-27 time); close the menu and check `+0x5e0`, `+0x758` and
    relative mouse mode; end the session with the menu open.
  - Pass: no commands pass through; no DESYNC; no stale peer and no catch-up; the HUD restores; the
    stock pause returns after the session ends.
- **E7: hot join with the fork save.**
  - Host on Linux; joiner on either system; `/sync` and roster growth.
  - Log: `syncBegin`, the forced-autosave frame, fork time and S (once the mod stamps it), child
    duration, done record, SyncPoll share time, transfer, joiner load, `LSNEED t`, history count,
    catch-up time, DESYNC after catch-up.
  - Pass: the host never stalls beyond the fork pause; the joiner's hash lane stays clean.
- **E8: relay periodic uploads,** 30 minutes live, `relay_autosave_min=2`.
  - Log per upload: child duration, fork milliseconds, parent and child RSS peaks, the lowest
    MemAvailable, and any child killed by signal 9 we did not send (the OOM killer).
  - Pass: no host stall; memory stays above the §2.3 gate.
- **E9: soak.**
  - Largest map, 4x, 1-minute autosaves, two hours; open the Load page repeatedly during saves
    (Save Pool contention).
  - Log: watchdog kills, try-lock fallbacks, park timeouts, fork latency p50/p95/max, CoW spikes.
  - Pass before `always` becomes the default: 0 hangs, at most 1 watchdog kill per 100 saves (with
    a successful fallback), fork p95 ≤ 150 ms.

## 5. Implementation plan

Another workflow is porting `native/linux/**`, `tools/linux/**`, `docs/linux/**`, `netpunch/` and
`mod/` right now. Nothing in those trees changes until it lands; that includes `CMakeLists.txt`,
`bridge_linux.cpp`, `speedhook_linux.cpp`, `lobby_linux.cpp`, `menu_linux.cpp`, `panel_linux.cpp`,
the not-yet-written `menu_game_linux.{h,cpp}`, the future `tpf2_slice.so` sources, and the mod.

**Phase 1: new files only, after the port lands, touching no existing file.**
- `native/linux/src/savefork_linux.{h,cpp}`:
  - `bool SaveFork_Install(uintptr_t base, LogFn)`:
    - checks the build-id and the bytes: slot `0x59be6d8` holds `base+0x15e4730`, thunk `f3 0f 1e
      fa e9 d7 f7 ff ff`, visitor prologue `55 48 89 e5 41 57 41 56 41 55 49 89 fd 41 54 53 48 89
      f3`, SaveGame call `e8 e1 a9 69 ff` at `0x15e421a`, stores `48 89 83 d8 01 00 00` at
      `0x15e4223` and `89 93 e0 01 00 00` at `0x15e4233`, `call CGame::Step` `e8 03 a4 a4 ff` at
      `0xfe7828`;
    - swaps the slot (mprotect RW, compare-and-swap, restore);
    - redirects `0xfe7828` to the park gate;
    - starts the watcher thread.
  - `SaveFork_SetMode(off|observe|live|always)`, `SaveFork_SetLive(bool)`, `SaveFork_Busy()`,
    `SaveFork_Stats()`.
  - The stub, the lock-around-fork, the child path, the watcher (pidfd, pipe, `/proc/<pid>/io`),
    the done record, `.tmp` cleanup, and `[savefork]` log lines.
- `native/linux/src/escmenu_linux.{h,cpp}`: `bool EscMenu_Install(uintptr_t base, LogFn)`
  (verify, allocate the stub and flag pages, patch) and `EscMenu_SetLive(bool)`.
- `native/linux/src/live_linux.h` (header-only): live = the instance letter from
  `tpf2_instance.txt` and a fresh `lockstep_status_<L>.txt` naming a peer. The slice's copy replaces
  it once that is ported.

**Phase 2: integration edits, after the port lands.**
- `native/linux/CMakeLists.txt`: add `savefork_linux.cpp` and `escmenu_linux.cpp` to
  `tpf2_bridge_mp`. The bridge is loaded first by `boot.cpp` and already owns runtime patches
  (`speedhook_linux.cpp`).
- `bridge_linux.cpp`: next to `SpeedHook_Install(Log)`, under the same `TPF2MP_NO_PATCHES` guard,
  call `SaveFork_Install` and `EscMenu_Install`. Feed the live state from its control loop, or let
  the modules poll.
- Config: `DATADIR/tpf2_savefork.txt` with `mode=`, `escnopause=` and `park=`. The first release
  ships `mode=observe` to gather E1/E2 numbers; `live` follows E3–E7, and `always` follows E9.
- `lobby_linux.cpp` (`tpf2_menu.so`):
  - SyncPoll uses the done record (§2.10);
  - the status line reads "Hot join: saving in the background";
  - the panel shows `[savefork]` failure notices.
- `menu_game_linux.cpp` (`tpf2_menu.so`): `MenuGame_ForceAutosave` stays as HJ-06.
  `MenuGame_RequestAutoload` and `PlaceSharedSave` wait while `SaveFork_Busy()`, read from a busy
  file because the libraries export no symbols.
- `tpf2_slice.so`, once it exists: move `escmenu` and the live detector next to the `UI::Clock`
  cancel (SLICE_TIME.md). Decide there whether the `CGame::Lock` construction freeze needs
  handling.
- Mod, after the mod workflow:
  - `save()` stamps S;
  - `load()` keeps it in the engine state on a fresh load;
  - `catchUpTick` holds from the first `update()` and asks for `LSNEED t=S`;
  - the `step=` line in `tpf2_sync_save.txt` can go;
  - the deferred-command gap in `histServe` is a separate fix.
- Docs: link this file from MENU_GAME.md §3 and NETWORKING.md 'Hot join'.

**Phase 3:** run E0 → E9 in order. Each phase's default changes only after its experiments pass.
Windows is unchanged.

## 6. Everything still UNPROVEN or INFERRED that the design depends on

1. The on-disk file set (the save folder is empty) → E1.
2. Fork latency, CoW growth and late-game RSS → E2, E3, E8.
3. How often the child deadlocks on locks glibc does not reset (timezone, iconv, ICU, resolver
   caches), and whether the main-thread park lowers it → E9.
4. The backend's runtime type (`0x59dbbf0` or the `0x59dbd28` forwarder) → E0; the install checks
   it anyway.
5. Which `cout`/`cerr` buffer is live during play → E0; handled both ways.
6. That glibc `fork()` does not reset the gconv and timezone locks (from glibc source, not the
   binary).
7. That `catch (...)` in our statically linked frame catches the game's exceptions (INFERRED; the
   child still reaches `_exit` on the paths SaveGame catches).
8. Whether `oom_score_adj` and `ioprio_set` are permitted under the snap AppArmor profile → E3.
9. SIGCHLD disposition as set by libraries → E0; the pipe record makes it non-critical.
10. That the dialog's `std::function` at closure `+0x90` closes the menu → E6.
11. World input behind the menu with the gate bypassed → E6.
12. The meaning of `0xe09a20` (camera playback).
13. Whether the game exits through `exit()`, and how Steam's reaper treats a running save child →
    E5.
14. That `Replicate` copies only changed components (bounds the CoW cost) → E3.
15. That a game time read in `save()` equals the snapshot's time (needed for the S stamp) → E7.
16. That the zstd compressor on the save path is single-threaded (no pool creation reachable).
17. Whether a player can load a save while its child is still writing → E5.
18. The size of the main-thread GPU readback hitch (present in the stock save too) → E1.
19. That Sim Pool workers are idle during the apply loop (snapshot quiescence) → E3's equivalence
    test.

## Appendix: patch sites and byte checks

| Purpose | RVA | Expected bytes or value | Action |
|---|---|---|---|
| visitor table slot 27 | `0x59be6d8` | `base+0x15e4730` (RELATIVE relocation) | swap to `SaveVisitStub` |
| slot-27 thunk | `0x15e4730` | `f3 0f 1e fa e9 d7 f7 ff ff` | verify |
| visitor prologue | `0x15e3f10` | `55 48 89 e5 41 57 41 56 41 55 49 89 fd 41 54 53 48 89 f3` | verify (no `endbr64`) |
| SaveGame call in visitor | `0x15e421a` | `e8 e1 a9 69 ff` → `0xc7ec00` | verify; fallback redirect site |
| result stores | `0x15e4223`, `0x15e4233` | `48 89 83 d8 01 00 00`, `89 93 e0 01 00 00` | verify offsets `+0x1d8`/`+0x1e0` |
| Apply stores the visitor result | `0x15e2fd9` | `88 41 30` | verify `Command+0x30` |
| park gate | `0xfe7828` | `e8 03 a4 a4 ff` → `0xa31c30` | `Tpf2mpRedirectCall` |
| Sync call (reference) | `0xa31c66` | `e8 55 f0 ff ff` → `0xa30cc0` | none |
| menu flag gate | `0x100fc70` | `80 bb 97 05 00 00 00 0f 85 a3 05 00 00` | `jmp stub` + 8-byte NOP |
| gated branch / fall-through | `0x1010220` / `0x100fc7d` | `48 8b bb 50 05 00 00` / `48 8b bb b0 04 00 00` | stub jump targets |
| backend getters | `0x18ba890`, `0x18ba270` | `mov rax,[rip→0x5a52d80]; ret` / `mov rdi,[rdi]; mov rax,[rdi]; jmp [rax+0x20]` | call from the stub |
| backend vptrs | `0x59dbbf0`, `0x59dbd28` | Standard (ctor `0x3340cf0`), Validating (ctor `0x3347400`, inner at `+8`) | runtime compare; mutex `+0x40` |
| game time mutex | `0x5b36ee0` | lock at `0x3206ca1` | try-lock |
| `cout` / `cerr` basic_ios | `0x5bbf588` / `0x5bbf448` | `_M_streambuf` at `+0xe8` (`0x5bbf670` / `0x5bbf530`) | try-lock a `ThreadSafeStreamBuffer` (vptr `0x5a00f98`, mutex `+0x48`); null in the child |
| `basic_ios::rdbuf(streambuf*)` PLT | `0x6dc0e0` | `.rela.plt` name `_ZNSt9basic_iosIcSt11char_traitsIcEE5rdbufEPSt15basic_streambufIcS1_E` | called in the child |
| autosave accumulator / interval / saving flag | `CGameUI+0x628` / `GlobalSettings+0x120` / `CGameUI+0xae8` | MENU_GAME.md HJ-03..HJ-06 | unchanged |
