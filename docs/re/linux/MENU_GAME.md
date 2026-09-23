# Menu game area: the Linux map

Linux RE for the parts of the title menu that act on the game itself: the save folder and the
files a save consists of, AUTO-LOAD (starting the shared save from the menu's per-frame update),
HOT JOIN's forced autosave, and the automod `settings.lua` edit. This replaces
`native/src/menu_hook.cpp` ~1772-1890 (forced autosave), ~2102-2296 (save placement and
auto-load) and ~2742-2816 (automod) for the Linux build. The code goes in
`native/linux/src/menu_game_linux.{h,cpp}` behind the API in section 7.

Binary: `TransportFever2`, Steam build 35924, GNU build-id `3a0e156390b0e6f1e372051c24802c8493ae454a`.
Addresses are Linux RVAs (the PIE links at 0; live = image base + RVA). "Windows" means the RVAs in
`menu_hook.cpp` and `docs/re/GAME_LOOP_AND_UI.md`, relative to `0x140000000`.

Status labels: **PROVEN** means the instructions, strings, relocations or files quoted here show
it, and anyone can re-derive it from the binary or the disk. **INFERRED** means it rests on
Windows measurements, naming or elimination, and stays out of patch code unless a run-time check
guards it. **MEASURED** means read from a file the game wrote on this machine, or observed in an
off-game experiment where that is said. Nothing here was exercised in the running game. Sections 1
to 8 are the map; section 9 is the implementation built on it, with the facts that stage added.

## How the evidence was produced

- Disassembly: capstone (x86-64) linear sweep from each function start over its FDE size from
  `/home/topsnek/tpf2-re/linux/functions.csv`. RIP-relative targets were resolved by hand, and
  `.rodata` strings and `R_X86_64_RELATIVE` addends were named.
- Callers: every `e8`/`e9` rel32 in `.text` whose target is the function, confirmed to sit on an
  instruction boundary of its FDE. Code references: every RIP-relative disp32 in `.text` that
  resolves to the address (the lea, mov, cmp and call forms). Stored pointers: `.rela.dyn`
  RELATIVE relocations with that addend.
- RTTI: the relocation whose addend is the typeinfo name string gives typeinfo + 8. The
  relocations whose addend is the typeinfo give slot -1 of each vtable, plus every typeinfo that
  names it as a base.
- Caller chains up to `main` came from a breadth-first walk over the direct call edges above.
- Field writers: every `.text` instruction with a memory operand whose displacement is the field
  offset (for example `0x597`, `0x628`, `0xae8`), filtered to the functions that work on the
  class.
- "Skips X" claims: a control-flow graph of the function over its FDE range, built from jumps,
  conditional jumps and fall-through. Calls to `__stack_chk_fail`, `__throw_bad_function_call`,
  `_Unwind_Resume` and the assertion handler `0x2fcb860` are treated as non-returning. The graph
  is searched for every exit reachable without passing X, and each exit and branch found was then
  read by hand.
- On disk (read only): the game's own log `userdata/125253817/1066780/local/crash_dump/stdout.txt`,
  `settings.lua` and `profile.lua` next to it, and the Snap Steam folder layout.
- The throwaway scripts lived in the area's scratch directory. The method above is enough to redo
  any row.

## 1. Save folder and save files

### 1.1 Where the game keeps user data (SAVE-01, SAVE-02, SAVE-03, SAVE-04)

- **SAVE-01 (PROVEN, MEASURED).** The game logs its user data folder at startup. Line 3 of
  `crash_dump/stdout.txt` reads:
  `user data folder: /home/topsnek/snap/steam/common/.local/share/Steam/userdata/125253817/1066780/local/`.
  The string `user data folder: ` is loaded at `0x9b4b27` in Run `0x9b3d50`. That folder holds
  `save/`, `settings.lua`, `profile.lua`, `crash_dump/`, `mods/` and `shader_cache/`.
  Windows gets the same `<steam>\userdata\<id>\1066780\local` layout from the registry.
- **SAVE-02 (PROVEN).** The `savegame` namespace is a mount point on `<root>/save/` with extension
  `.sav`. The Steam platform backend init `0x18d3930` (it also references `SteamAPI_Init failed`)
  builds the backend (`operator new(0x98)` at `0x18d3b92`, `0x3340cf0` at `0x18d3b9d`), then:
  - `0x18d3ba2 lea '.sav'` goes to r14;
  - `0x18d3bc3 call 0xa070b0(r15, paths+0x48, paths+0x88)` joins the root with `save/`;
  - `0x18d3bcf lea 'savegame'` goes to r12;
  - `0x18d3bea call 0x3343ba0(rdi=backend, rsi="savegame", rdx=<root>/save/, rcx=".sav")`.

  The paths struct is initialised in `0x18bd2c0`: `lea 'save/'` at `0x18bd333` goes into
  `+0x88` at `0x18bd33a` (next to `heightmaps/` at `+0x68` and `scenarios/` at `+0xa8`).
  The `userdata` mount is registered the same way at `0x18d3cd3`..`0x18d3cf2`, with root
  `paths+0x48` and extension `.lua`.
- **SAVE-03 (INFERRED).** `paths+0x48` is the SAVE-01 folder, so plain saves live in
  `<userdata>/<id>/1066780/local/save/`. Three things support it: the `userdata` mount uses the
  same `+0x48`, `settings.lua` sits in the logged folder, and `local/save/` exists on disk. The
  code that fills `+0x48` was not traced.
- **SAVE-04 (PROVEN, filesystem).** Under Snap Steam, `HOME` inside the game's container is
  `/home/topsnek/snap/steam/common`. Both `$HOME/.steam/steam` and `$HOME/.steam/root` are
  symlinks to `$HOME/.local/share/Steam`, which holds `userdata/`. There is no `~/.steam` in the
  real home, and no Flatpak copy on this machine. Other Steam installs put the root at
  `~/.steam/steam`, `~/.local/share/Steam` or
  `~/.var/app/com.valvesoftware.Steam/.local/share/Steam` (general knowledge, not measured here).
  - The game picks the account through the Steam API. Code that runs before `main` cannot ask
    Steam, so with several `userdata/<id>/1066780/local` folders it has to choose. The Windows
    rule is newest mtime of the save folder. `settings.lua` is rewritten at every exit (AM-04),
    so its mtime is a better signal. That is a design suggestion, not RE.
  - `config/loginusers.vdf` on this machine has no `MostRecent` key, only per-user `Timestamp`.

### 1.2 The files one save consists of (SAVE-05, SAVE-06, SAVE-07, SAVE-08)

- **SAVE-05 (PROVEN).** `SaveGame(const GameMetadata&, ..., const platform::SaveGameId&, bool,
  IProgressMonitor&)` `0xc7ec00` writes `<name>.sav`, `<name>.sav.lua` and `<name>.jpg` (the last
  only with a screenshot), and no `.info` on Linux. Every file is passed to the backend as a
  *pair* of names `{std::string first @+0; std::string second @+0x20}`: the flat name and the
  folder-layout name. The backend builds the path from `first` alone.
  - The SaveGameId is the 7th argument, `[rbp+0x10]`, loaded into r12 at `0xc7ec2a`. Its name
    `[r12+0x20]`/`[r12+0x28]` is copied into `[rbp-0x440]` at `0xc7ec95`..`0xc7ecc8`, and that
    string's address is kept in `[rbp-0x5e0]`.
  - `0xc7ee16`..`0xc7ee2a`: `pair.first` = name + `.sav` (`0x9bfab0` is `out = std::string + const
    char*`: it constructs from `rsi` and appends `strlen(rdx)` bytes). `0xc7ee2f`..`0xc7ee3a`:
    `pair.second` = `"savegame.sav"` (`0xc77be0` = `std::string(const char*)`). `0xc7ee7e`: backend
    vcall `+0x50(backend, mount, &pair)`.
  - `0xc7ee3f call 0x9d7ba0; test al; jne 0xc7ff90`. `0x9d7ba0` is `xor eax,eax; ret` (FDE size 7),
    so the branch at `0xc7ff90` never runs. That branch builds the pair (`<name>.info`,
    `savegame.info`) at `0xc7ff9e`..`0xc7ffbf` and writes it with vcall `+0x50` at `0xc7ffda`.
    `0x9d7ba0` gates only this `.info` file.
  - `0xc7f844`: `<name>.sav` + `.lua` gives `<name>.sav.lua`, the script state. Its folder-layout
    partner `scriptState.lua` is the `second` built by the getter at `0xc7e80e` (SAVE-06).
  - `0xc7fa6b`..`0xc7fa73` skips when the screenshot buffer is empty. Otherwise `0xc7fa80` builds
    `first` = name + `.jpg`, `0xc7fa93` `second` = `"screenshot.jpg"`, and `0xc7fab5` writes it.
  - Why the folder-layout names are unused: the backend is a `platform::StandardSaveGameBackend`,
    and all three of its file slots read only `pair.first`.
    - RTTI: `N8platform23StandardSaveGameBackendE` (`0x4fe5ce0`) gives typeinfo `0x5a39950`, base
      `N8platform16ISaveGameBackendE` typeinfo `0x5a39940`. Its only vtable is slot 0 `0x59dbbf0`:
      `+0x48` = `0x3340b20`, `+0x50` = `0x3344280`, `+0x60` = `0x3344840`.
    - The Steam platform init `0x18d3930` creates it: `operator new(0x98)` at `0x18d3b92`, then
      ctor `0x3340cf0`, which stores vptr `0x59dbbf0` (`0x3340cf4`/`0x3340d07`). It registers the
      mounts (`0x18d3bea`, `0x18d3c79`, `0x18d3cf2`) and stores the object at
      `[platform+0x50]` (`0x18d3d25`).
    - exists `+0x48` `0x3340b20`: copies `[rdx]`/`[rdx+8]` (`0x3340b4f`/`0x3340b6c`), joins the
      mount root from vcall `+0x28` (`0x3340b88`) with `boost::filesystem::path::operator/=`
      (`0x3340bb7`). rdx is not read again.
    - write `+0x50` `0x3344280`: pair in rbx and `[rbp-0x158]` (`0x33442a5`/`0x33442b5`). It reads
      `[rbx]`/`[rbx+8]` (`0x33442f1`/`0x33442fe`), joins the root from vcall `+0x28` (`0x3344320`,
      `/=` at `0x3344355`), and overwrites rbx at `0x334437f`. `[rbp-0x158]` is read once more,
      at `0x3344631`, as rsi for `0x3346a40` (`0x3344649`).
    - read `+0x60` `0x3344840`: reads `[rdx+8]`/`[rdx]` (`0x3344875`/`0x3344879`) and keeps the pair
      in `[rbp-0x120]`. Its only other use is as rsi for `0x3346a40` (`0x3344bad`, `0x3344bc5`).
    - `0x3346a40` (an unordered_map find-or-insert) reads only `[rsi]`/`[rsi+8]` (hash at
      `0x3346a60`..`0x3346a67`, key copy at `0x3346aba`/`0x3346abd`).
    - The only other `ISaveGameBackend` is `ValidatingSaveGameBackend` (typeinfo `0x5a39998`,
      vtable `0x59dbd28`). Its `+0x48`/`+0x50`/`+0x60` slots `0x33470c0`/`0x33470d0`/`0x33470f0` are
      `mov rdi,[rdi+8]; mov rax,[rdi]; jmp [rax+slot]`, which forward the pair unchanged. No
      typeinfo names either class as a base: each has exactly one reference, its vtable slot -1
      (`0x59dbbe8`, `0x59dbd20`).

  So the files are the same set `placeSaveNewest` copies on Windows: `.sav`, `.sav.lua`, and the
  `.jpg` named without `.sav`.
- **SAVE-06 (PROVEN).** The SavegameInfo getter `0xc7e520` (AL-11) needs `<name>.sav` and reads
  `<name>.sav.lua` when present:
  - `0xc7e59c`..`0xc7e5dc` builds the pair (`<name>.sav`, `savegame.sav`) from `id+0x20`.
  - `0xc7e769 call [backend vtable+0x48]` is an exists test. When it fails, `je 0xc7e9f0` throws
    `std::runtime_error("file not found: " + path)` (string `0xc7ea8f`).
  - `0xc7e7f8`..`0xc7e82a` tests `<name>.sav.lua` and reads it only if present.
- **SAVE-07 (PROVEN).** Saves are written with an empty `SaveGameId.path`: `0xc7ec5c cmp qword
  [r12+8],0; jne 0xc80425` asserts `fileName.path.empty()`.
- **SAVE-08 (INFERRED).** Autosaves are `autosave_*.sav` files in the same `savegame` namespace:
  - `autosave_` is loaded at `0xf2c8de` by the savegame list provider `0xf2c1d0`, which also uses
    `savegame`;
  - `GetAutoSaveName` `0x3342cb0` is a `StandardSaveGameBackend` method;
  - Windows measured a 113 MB `autosave_*.sav` in the save folder.

  The name AutoSave itself passes was not traced.

## 2. AUTO-LOAD

### 2.1 The hook: CMenuUI's per-frame update through its vtable (AL-01 to AL-05)

- **AL-01 (PROVEN).** CMenuUI's RTTI and vtable:
  - `N2UI7CMenuUIE` gives typeinfo `0x5a16e68`.
  - The only relocation holding that typeinfo is vtable slot -1 at `0x5a16f90`, so slot 0 is
    `0x5a16f98` (offset-to-top 0). No other typeinfo names CMenuUI as a base, so no class derives
    from it.
  - The vptr value `0x5a16f98` is loaded only by the destructor `0x1127a50` (`0x1127a55`) and the
    constructor `0x1155c20` (`0x1155c87`).
- **AL-02 (PROVEN).** Slot 34 at `0x5a170a8` holds `0x1140a90` (RELATIVE addend), CMenuUI's
  per-frame update. Windows had slot 33, `0x672b10` in vftable `0x301dc38`. The Itanium ABI has
  two destructor slots (slot 0 `0x1127a50`, slot 1 `0x11286c0`) where MSVC has one, which shifts
  every later slot by one. The identification is by content, not by that arithmetic:
  - `0x1140b9b`/`0x1140c22`: `mov rdi,[rbx+0x5f8]; test` checks the queued load (AL-08).
  - `0x1140c50`: `mov r14,[rbx+0x4c8]; test` checks the running game (AL-06). When non-null, the
    future is moved into `CGameUI+0xad8/+0xae0` (`0x1140c9a`/`0x1140ca1`).
  - `0x1140cf8`: `movzx eax, byte [rbx+0x5e8]; test; jne` waits while initialising (AL-07).
  - Otherwise it takes the result, runs the mod check `0xc77bc0` (`0x1140df3`), and calls
    `0x113fd70(this, &pair.params, &optional<SavegameInfo>)` at `0x1140e26` (AL-16).

  `0x1140a90` has no direct callers, no RIP-relative references and exactly one relocation (the
  slot), so the only way into the function is through vtable slot 34. The slot itself is called by
  Step2 (AL-03), which is reached from more than the frame step (AL-04, AL-19). Other
  `[vptr+0x110]` call sites that might dispatch it were not enumerated: a scan of
  `call [reg+0x110]` finds 75 functions but misses the load-then-`call rax` form Step2 uses. The
  AL-19 gate makes that moot.
- **AL-03 (PROVEN).** Dispatcher and detour ABI. `void UI::CComponent::Step2(long long, long long,
  const UI::CComponent::StepData&)` `0x305a1c0` does:
  - `0x305a2b9 mov rax,[r15]`, `0x305a2bc lea rdx,0xf16c60` (the base no-op: `endbr64; ret`),
    `0x305a2c3 mov rax,[rax+0x110]`, `0x305a2ca cmp rax,rdx; jne 0x305a7a8`;
  - at `0x305a7a8`: `mov rdx,r12; mov rsi,r13; mov rdi,r15; call rax`. r15, r13 and r12 are
    Step2's own rdi, rsi and rdx (`0x305a1ca`/`0x305a1d1`/`0x305a1d6`).

  So slot 34 is `void(UI::CComponent* rdi, int64 rsi, int64 rdx)`. Neither update reads rcx, r8
  or r9 in its entry block. `0x1140a90` uses no xmm register at all, and every xmm register in
  `0x100fb20` is written before it is read. A detour
  `void(void* self, int64_t a, int64_t b)` that calls the original first with the same three
  values is ABI-exact. Our pointer is never `0xf16c60`, so Step2 always takes the indirect path.
- **AL-04 (PROVEN: the frame path and the two other ways in; per-frame cadence INFERRED).**
  - Frame path. `_start` `0x97a270` passes `lea rdi, 0x95e6c0` (`0x97a291`) to
    `__libc_start_main` (GOT `0x5a462e8`, `0x97a298`), so `main` = `0x95e6c0`. From there it is
    direct calls, and no function in the chain is address-taken (no RELATIVE relocation, no
    RIP-relative reference):
    main → Run `0x9b3d50` (`0x95e8e3`) → Run2 `0x9b1a90` (`0x9b56ed`) →
    `bool UI::CCore::Run(UIRenderer*)` `0x30826d0` (`0x9b2cca`) → `0x30803d0` (`0x30828a1`) →
    Step `0x305a830` (`0x30822e6`) → Step2 `0x305a1c0` (`0x305a8dd`) → slot 34 (`0x305a7b1`).
    Each of `0x9b3d50`, `0x9b1a90`, `0x30826d0` and `0x30803d0` has exactly that one caller.
    On this path the update runs on the thread that runs `main`.
  - Not the only path. Step `0x305a830` has two more direct callers (AL-19), so both slot-34
    updates also run outside the frame step:
    - `0x312af90` calls it at `0x312afb2` with `rsi = rdx = 0` (`0x312afae xor edx,edx`,
      `0x312afb0 xor esi,esi`). That function has 16 direct callers and one tail jump (AL-19),
      all UI construction or layout code.
    - The step-listener lambda `0x112e8a0` calls it at `0x112e948` with the t/dt of the Step2
      that runs the listener.
  - Step2 walks the component's children at `0x305a48d` with its own rsi/rdx. So slot 34 of any
    CMenuUI or CGameUI inside a component stepped by those paths runs with (0,0), or with the
    listener's values, possibly several times per frame and from inside UI builders. Which
    components those callers pass was not traced. Their thread is INFERRED to be the UI thread.
  - Consequences for the detours:
    - They must accept `rsi = rdx = 0` and must not count calls as frames.
    - The game itself calls StartSavegame (via `0x113fd70` at `0x1140e26`) and AutoSave (at
      `0x10102e2`) from these same update bodies, whatever the path.
    - Our own actions run only on the frame path, which the AL-19 gate identifies.
  - That the frame step runs once per frame is the Windows measurement. `0x30803d0` makes the
    call at most once per invocation: `0x30822e6` is its only call to Step, followed by the
    epilogue at `0x308230f`.
  - Thread identity. `0x967120` stores `pthread_self()` in `.bss 0x5b37168` (`0x967132`/`0x967138`;
    it stores 1 instead when the `__pthread_key_create` GOT slot `0x5a46348` is null). It is entry
    619 of `.init_array` (`[0x59fef48, +0x1888)`, RELATIVE relocation at `0x5a002a0`), so it runs
    before `main` on the initial thread, which then runs `main` (glibc runs the executable's
    `.init_array` in `__libc_start_main`). `IsMainThread` `0x3231260` compares against that value
    (`0x3231277`, or with 1 at `0x323128d`). `GetGlobalGameUI` `0xfe2a20` returns non-null only
    when it is true.
- **AL-19 (PROVEN). Every Step2 activation, and the frame-step gate.**
  - Step `0x305a830` (`void(CComponent* rdi, int64 rsi, int64 rdx)`: it saves rsi/rdx in r12/r13 at
    `0x305a83f`/`0x305a83a`, builds StepData at `[rbp-0x54]`, and calls Step2 with the same three
    plus `rcx = &StepData` at `0x305a8cd`..`0x305a8dd`; no other argument register or xmm register
    is read) has exactly three direct callers, no RIP-relative reference and no RELATIVE
    relocation:
    1. `0x30822e6` in `0x30803d0` (frame step). `rdi = [[rbp-0x310]+0x370]`, called only when
       `[rdi+0x84] > 2` (`0x30822bd`/`0x30822c4 jle 0x30822eb`); `rsi = [[rbp-0x310]+0xf0]`;
       `rdx = [[rbp-0x310]+0xf8]` if that is > 0, else `[rbp-0x358]` (`0x30822cd`..`0x30822de`).
       The caller overwrites rax, rdi and rsi before reading them again (`0x30822eb`, `0x30822f2`,
       `0x30822fe`).
    2. `0x312afb2` in `0x312af90`, with 0/0; followed by `0x312afb7 mov esi,r12d`, so no volatile
       register is read. Callers of `0x312af90`:
       `0xf3dba3` (`UI::BuildControlComp::UpdateLayout`), `0xfa2338`, `0xfa3455`,
       `0xfddaed` (a lambda stepping its captured `[functor+8]`), `0x114d98f`
       (`CMenuUI::CreatePageNewGame`), `0x1469323`, `0x14c4af2`, `0x1d97c58` (a Lua binding lambda
       under `scripting::AddFunction`), `0x3049eb6`, `0x3049f63`, `0x307a965`
       (`CCore::ShowPopupMenu`), `0x3096b5e`, `0x30f39a2`, `0x3119d16`, `0x31206de`
       (`TextView::DoValidate`), `0x313ac37`; plus the tail jump at `0x1c917c4`.
    3. `0x112e948` in `0x112e8a0`, then `jmp 0x112e90e`, which reads no volatile register.
       `0x112e8a0` is a `std::function` invoker (`rbx = [rdi]`, `rsi = *[rsi]`, `rdx = *[rdx]`)
       that steps `[functor+8]` when its `+0x84 <= 2`. Its only reference is `0x114e6fb` in
       CreatePageNewGame, next to manager `0x1120ce0` (`0x114e709`). The `std::function` goes to
       `0x3052590`, which appends it (`0x305dca0`, 0x20-byte elements) to the vector at
       `[component+0x438]+0x130`. Step2 calls every element of that vector at `0x305a343` with
       pointers to its own rsi/rdx (`0x305a2f4`..`0x305a311`).
  - Step2 `0x305a1c0` is called only at `0x305a8dd` (Step) and `0x305a48d` (its own child loop), and
    has no reference or relocation. So every slot-34 call via Step2 is nested inside exactly one of
    the three Step calls above.
  - Gate: redirect all three sites with `Tpf2mpRedirectCall(site, base+0x305a830, wrapper)`, which
    checks the `e8` opcode and the rel32 target. Each wrapper is `void(void*, int64_t, int64_t)`
    and calls the original with the same values. The frame wrapper (site 1) increments a
    per-thread frame depth; the two nested wrappers (sites 2 and 3) increment a per-thread nested
    depth. A slot-34 detour acts only when frame depth == 1 and nested depth == 0 on the calling
    thread. Otherwise it just calls the original. When a redirect fails, no redirect is kept, and
    autoload and the forced autosave stay off with a log line.
  - Weaker checks do not work. `rdx != 0` excludes site 2 but not site 3, which passes the frame's
    own t/dt.
- **AL-05 (PROVEN).** The slot is in `.data.rel.ro` inside `PT_GNU_RELRO` `[0x59a8700, 0x5a48000)`,
  so it is read-only after relocation. Swapping it means `mprotect(PROT_READ|PROT_WRITE)` on page
  `base+0x5a17000`, one 8-byte store, then `mprotect(PROT_READ)` again. Before writing, check that
  the slot holds `base+0x1140a90` and that the target starts with
  `f3 0f 1e fa 55 48 89 e5 41 57 41 56 49 89 d6 41 55 41 54 53 48 89 fb` (checked in the file).
  Refuse on any mismatch. Store the original pointer before the slot, so the detour never calls
  null.

### 2.2 CMenuUI fields the tick reads (AL-06, AL-07, AL-08)

| id | field | Linux | Windows | evidence | status |
|---|---|---|---|---|---|
| AL-06 | `CGameUI*` while a game runs | `CMenuUI+0x4c8` (qword) | `+0x4e8` | Written at `0x115c470` with the object from `operator new(0xb50)` (`0x115c3aa`) and CGameUI ctor `0x1012ee0` (`0x115c45c`, `this` in rdi, CMenuUI* in rsi = r15). The ctor stores the CGameUI vptr `0x5a11468` at `0x101305a`. StopGame `0x1157bc0` zeroes it at `0x11580f8`; the ctor `0x1155c20` zeroes it at `0x1155d84`. `SetGlobalGameUI` mirrors it (`0x115c965` set, `0x1158105` clear). | PROVEN |
| AL-07 | game initialisation active | `CMenuUI+0x5e8` (byte) | `+0x1988` | StartSavegame `0x113f47e movzx eax, byte [rdi+0x5e8]; jne 0x113fc00` leads to `'CMenuUI::StartSavegame: Game initialization is already active!'`. It is set to 1 at `0x113f82e` and cleared in the ctor at `0x1155f28`. | PROVEN |
| AL-08 | queued load pending | `CMenuUI+0x5f8` (qword, shared state; control block at `+0x600`) | `+0x19a0` | Non-null makes the update process a queued load (`0x1140b9b`, `0x1140c22`). Reset at `0x1140cca`/`0x1140cd5`; zeroed in the ctor at `0x1155f4d`. The type (`JoiningFuture<optional<pair<SavegameInfo, LoadGameParams>>>`, from the Enqueue signature at `0x113c2d0`) is INFERRED. | PROVEN (non-null test) |

Tick rule (the same as Windows):
- `+0x4c8 != 0`: a game runs. Do not load; the player loads `mp_shared` from the in-game menu.
- `+0x5e8 != 0` or `+0x5f8 != 0`: the game is already starting something, so wait.
- Otherwise call StartSavegame.

### 2.3 Structures (AL-12, AL-13, AL-14)

All strings are libstdc++ `std::string` `{char* p; size_t len; char buf[16]}`. An empty string
has `p == &buf`.

- **AL-13 (PROVEN). SaveGameId, 0x60 bytes:** `{std::string path @0x00; std::string name @0x20;
  std::string namespace @0x40}`. Windows' path was a `std::wstring`; on Linux all three are
  `std::string`.
  - `T Read(const Value&) [with T = platform::SaveGameId]` `0x18a4cb0` sets the key `path`
    (`0x18a4d47`) into `+0x00` (move-assign at `0x18a4ddc`), `saveGameName` (`0x18a4def`) into
    `+0x20` (`0x18a4e09`), and `saveGameNamespace` (`0x18a4e20`) into `+0x40` (`0x18a4e3a`).
  - Destructor `0xf31740(rdi)` frees `+0x40`, `+0x20`, `+0x00`.
- **AL-14 (PROVEN). LoadGameParams, 0x130 bytes** (Windows 0x138):

  | offset | member | default (ctor `0x1162170`) | copy ctor `0x101f960` | dtor `0x101da10` |
  |---|---|---|---|---|
  | `+0x00` | `std::string` name | `""` (`0x116219d`..`0x11621ad`) | `0x101f9a4` | `0x101da9b` |
  | `+0x20` | `std::string` path | `""` (`0x11621eb`) | `0x101f9be` | `0x101da89` |
  | `+0x40` | `std::string` namespace | `"savegame"` (lea `0x1162206`, `0x1162228`) | `0x101f9dc` | `0x101da77` |
  | `+0x60` | plain byte (bool-like) | 0 (`0x1162234`) | copied as a byte, no test (`0x101f9e1`/`0x101f9ee`) | not read |
  | `+0x68` | vector of 0x28-byte elements (std::string at +0), payload of the next row | | `0x9b87c0` at `0x101fad1` | loop `0x101dad0`..`0x101db0c` |
  | `+0x80` | engaged byte of `optional<vector>` @`+0x68` | 0 (`0x1162246`) | tested `0x101f9f1`, set `0x101fad6` | tested `0x101da6e` |
  | `+0x88` | vector of 0x40-byte elements (strings at +0 and +0x20), payload of the next row | | `0xc85bc0` at `0x101fb6f` | loop `0x101db20`..`0x101db7b` |
  | `+0xa0` | engaged byte of `optional<vector>` @`+0x88` | 0 (`0x116224d`) | tested `0x101fa07`, set `0x101fb74` | tested `0x101da61` |
  | `+0xa8` | 0x38-byte hash container (libstdc++ unordered_map layout: fields `+0xa8`..`+0xd8` copied at `0x101fae8`..`0x101fb48`), payload of the next row | | `0xc877a0` at `0x101fb4f` | `0xc216e0` at `0x101db8f` |
  | `+0xe0` | engaged byte of `optional<hash container>` @`+0xa8` | 0 (`0x1162254`) | tested `0x101fa24`, set `0x101fb54` | tested `0x101da54` |
  | `+0xe8` | plain byte (bool-like) | 0 (`0x116225b`) | copied as a byte, no test (`0x101fa37`/`0x101fa55`) | not read |
  | `+0xf0` | `std::string` campaign | `""` (`0x1162238`, `0x1162262`, `0x116226d`) | `0x101fa6e` | `0x101da3c` |
  | `+0x110` | `std::string` mission | `""` (`0x1162274`..`0x1162286`) | `0x101fa9b` | `0x101da2b` |

  - `+0x60` cannot be an optional's engaged flag: the payload would have to sit before it, where the
    namespace string is.
  - Calls: default ctor `0x1162170(rdi)`, destructor `0x101da10(rdi)`, copy ctor
    `0x101f960(rdi = dest, rsi = src)`.
  - Mission at `+0x110`, in the Missions lambda `0x113ffa0`:
    - the local string `[rbp-0x470]` is kept in `[rbp-0x498]` (`0x113ffca`/`0x113ffd4`);
    - its length is tested at `0x113ffe0` (`je 0x114077e`, which asserts `'!mission.empty()'`:
      lea `0x1140791`, handler call `0x1140798`);
    - `0x11402b9`/`0x11402c7` copy it into params `+0x110`.
  - Campaign at `+0xf0`: `[rbp-0x4a8]` = `r12+0x30` (`0x11400dd`/`0x11400ef`), the captured
    string that `0x114003b` joins after `'/campaign/'` (`0x1140025`, `0x1140031`).
    `0x11402a6`/`0x11402b4` copy it into params `+0xf0`.
  - The recipe (AL-18) uses only the default ctor, the name at `+0x00` and the dtor, so these two
    bytes do not affect it.
  - Size: StartSavegame's capture copies the params to `+0x78` (`0x113f90e`) and puts the next
    member at `+0x1a8` (`0x113f91a lea rdi,[rbx+0x1a8]`). The `optional<pair<SavegameInfo, LoadGameParams>>` engaged
    byte is at `+0x238`, read at `[r14+0x248]` with the value at `r14+0x10` (`0x1140d56`).
  - Name at `+0x00` and path at `+0x20` come from the Missions lambda (AL-15). It builds the
    SaveGameId from params `+0x20`, `+0x00` and `+0x40` in that order.
  - StartSavegame asserts `params.campaign.empty() == params.mission.empty()` using the lengths
    at `+0xf8` and `+0x118` (`0x113f48d`, `0x113f49b`, `0x113fcb0`).
  - `+0x80` engaged selects mods `+0x68`; otherwise StartSavegame uses `SavegameInfo+0x60`
    (`0x113f5c6`..`0x113f5ce`, `0x113f7f0`).
- **AL-12 (PROVEN). SavegameInfo, 0x108 bytes** (Windows 0x110):
  - `optional<SavegameInfo>` keeps its engaged byte at `+0x108`: `0x113fd91 cmp byte
    [rdx+0x108],0`. The Missions lambda zero-fills `0x22` qwords and tests `[rbp-0x178]` =
    storage `+0x108`.
  - `pair<SavegameInfo, LoadGameParams>.second` is at `+0x108` (`0x1140dcf`).
  - Destructor `0xc84f00(rdi)`.

### 2.4 Calls (AL-09 to AL-11, AL-15 to AL-17)

| id | function | Linux RVA | Windows | registers | evidence | status |
|---|---|---|---|---|---|---|
| AL-09 | `bool UI::CMenuUI::StartSavegame(const LoadGameParams&, const SavegameInfo&)` | `0x113f450` | `0x6785c0` | rdi CMenuUI*, rsi `const LoadGameParams*`, rdx `const SavegameInfo*`; returns al | Signature string `0x4178f60` loaded at `0x113fc9d`. Refuses when AL-07 is set (`0x113fc00`). Returns false without starting when the mod check `0xc77bc0(mgr+0xd8, &info, &mods)` fails (`0x113f790 call; mov r12d,eax; test al; jne 0x113f800`). Otherwise sets `+0x5e8 = 1` and posts the load. | PROVEN |
| AL-10 | app accessor | `0x18ba890` | `0xbb23c0` | none; rax = `*(.bss 0x5a52d80)` | `mov rax,[rip+..]; ret`. Save manager = `*(app+0xc8)` (Windows app+200): `0x1140353` (Missions lambda, becomes the getter's rsi), `0x113f4c8` (StartSavegame), `0x1140ddb` (the update). | PROVEN |
| AL-11 | SavegameInfo getter | `0xc7e520` | `0x2e6ca0` | rdi out `SavegameInfo*` (0x108 bytes, uninitialised), rsi manager, rdx `const SaveGameId*`; returns rax = out | Call site `0x1140360` with rdi=`rbp-0x390`, rsi=`[app+0xc8]`, rdx=SaveGameId (`0x1140347`..`0x1140360`). Constructs out from `0xc7e5eb`; returns rbx at `0xc7e904`. Throws: see below. | PROVEN |
| AL-15 | the model sequence (Missions page lambda) | `0x113ffa0` | CONTINUE lambda `0x65e780` | see below | see below | PROVEN |
| AL-16 | start-or-report wrapper | `0x113fd70` | (none) | rdi CMenuUI*, rsi `const LoadGameParams*`, rdx `const optional<SavegameInfo>*` | `0x113fd91` tests engaged; `0x113fd9a` calls StartSavegame; when not engaged or false it calls `0x304ed10` with a `std::function` (manager `0x11181c0`, invoker `0x115a870`). That this shows an error is INFERRED. | PROVEN (control flow) |
| AL-17 | `std::string(const char*)` | `0x1128b80` | MSVC assign `0x83270` | rdi out (uninitialised), rsi C string | `lea rax,[rdi+0x10]; mov [rdi],rax; strlen (0x9b7670); jmp _M_construct 0x995790`. The same RVA `menu_linux.cpp` uses. `0xc77be0` is an identical copy. | PROVEN |

**AL-11 exception behaviour (PROVEN).** The getter throws `std::runtime_error` in two places:
- `"invalid mount point"` at `0xc7e9ca`/`0xc7e9d4`, when backend vcall `+0x18` (`0xc7e56b`)
  returns a negative mount index. This happens before `out` is initialised.
- `"file not found: <path>"` at `0xc7e9f0`..`0xc7eaa8` (SAVE-06).

Later failures unwind through the cold path `0x6f250b`, which destroys `out` itself
(`call 0xc84f00` at `0x6f25cb`). So after any exception `out` must not be touched or destroyed.
The backend comes from `0x18ba270(app)` (`mov rdi,[rdi]; mov rax,[rdi]; jmp [rax+0x20]`, a
virtual call on `*app`), and the mount index from its vcall `+0x18(backend, &id, 0)`.

**AL-15, the sequence the game uses (PROVEN, Missions page lambda `0x113ffa0`):**

1. `0x114019a call 0x1162170(r13)`: LoadGameParams default ctor.
2. `0x11401c2` sets params `+0x20` (path); `0x114024c` sets params `+0x00` (name, the stem);
   `0x114029a` assigns `"savegame"` to `+0x40`; `0x11402b4`/`0x11402c7` copy campaign and mission.
3. `0x11402dd`: zero-fill the `optional<SavegameInfo>` storage.
4. `0x11402f8`/`0x114031d`/`0x1140342`: construct the SaveGameId `{path=params+0x20,
   name=params+0x00, namespace=params+0x40}` (`0x111b240` = `_M_construct(begin, end)`).
5. `0x1140347 call 0x18ba890`; `0x1140353 mov rsi,[rax+0xc8]`; `0x1140360 call 0xc7e520(&tmp, mgr,
   &id)`.
6. `0x1140378` move tmp into the optional and set engaged; `0x1140387 call 0xc84f00(&tmp)`;
   `0x114038f call 0xf31740(&id)`.
7. `0x114039e call 0x113fd70(menu, &params, &optional)`.
8. `0x11403fd call 0x101da10(&params)`.

**AL-18 (INFERRED).** The id for our shared save is `{path "", name "mp_shared", namespace
"savegame"}`, which resolves to `<userdata>/save/mp_shared.sav`. Supporting it:
- every save is written with an empty path (SAVE-07);
- the params ctor defaults the namespace to `savegame` (AL-14);
- `savegame` is the `save/` mount with `.sav` (SAVE-02);
- the getter appends `.sav` to `id.name` (SAVE-06);
- Windows measured this exact id loading `mp_shared` (`profile.lua` stores `lastGame` as the same
  three fields).

The backend's vcall `+0x18` was not disassembled, so it is not shown that it keys on the
namespace alone.

Recipe for the implementation. It runs inside the AL-02 detour, after the original returns, and
only when the AL-19 gate says frame step (frame depth 1, nested depth 0, on this thread). It also
applies the AL-02 tick rule (`+0x4c8`, `+0x5e8` and `+0x5f8` all 0). The game makes the same
StartSavegame call itself from inside this update body (`0x1140e26` → `0x113fd70` →
`0x113fd9a`). Every call is from the tables above.

```
LoadGameParams p;      0x1162170(&p)                      // name "", path "", namespace "savegame"
p.name = "mp_shared"   // SSO: p==buf after the ctor, 9 chars fit the 16-byte buffer: write buf, len
SaveGameId id;         0x1128b80(&id.path, ""); 0x1128b80(&id.name, "mp_shared"); 0x1128b80(&id.ns, "savegame")
mgr = *(void**)(0x18ba890() + 0xc8)
SavegameInfo info;     0xc7e520(&info, mgr, &id)          // may throw: see AL-11
                       0xf31740(&id)
ok = (char)0x113f450(menu, &p, &info)
                       0xc84f00(&info); 0x101da10(&p)
```

Before calling, check that `<save dir>/mp_shared.sav` exists, which removes the "file not found"
throw. A throw that still happens is caught with the game's own C++ runtime and cleaned up as the
Missions page does: `id` and `p` are destroyed, the getter's `out` never is (9.3). A plain `catch`
in our library cannot do this (EH-01), and nothing above the update would (EH-02).

## 3. HOT JOIN: forcing the game's own autosave

### 3.1 The hook: CGameUI's per-frame update through its vtable (HJ-01, HJ-02)

- **HJ-01 (PROVEN).** `N2UI7CGameUIE` gives typeinfo `0x5a11248`. Its only reference is vtable
  slot -1, so slot 0 is `0x5a11468` (offset-to-top 0) and nothing derives from CGameUI. The vptr
  value is loaded only by the ctor `0x1012ee0` (`0x1013047`, stored `[r15]` at `0x101305a`) and
  the destructor `0xff66d0` (`0xff66fa`).
- **HJ-02 (PROVEN).** Slot 34 at `0x5a11578` holds `0x100fb20`, the CGameUI per-frame update.
  Windows inline-hooked `0x5741d0` with a 21-byte steal. It has no direct callers, no RIP-relative
  references and one relocation, and Step2 dispatches it like AL-03. On the frame path that is
  the `main` thread. It is also reached with (0,0) or listener values through the other Step
  entries (AL-04, AL-19), so the forced autosave uses the AL-19 gate. Registers:
  - rdi CGameUI*;
  - rsi int64 is a time in µs (`[rbp-0x2d8]`): `0x10101c1`..`0x10101db` runs `CheckAchievements`
    `0xff5c90` once `rsi > [this+0xa48] + 999999`, then stores rsi there;
  - rdx int64 is the frame's delta in µs (r14): it is added to the accumulator (HJ-03).

  The names "time" and "dt" are INFERRED; the register use is PROVEN.

  The slot is in the same RELRO range (page `base+0x5a11000`). Verify it holds `base+0x100fb20`
  and that the target starts with
  `f3 0f 1e fa 55 48 89 e5 41 57 41 56 49 89 d6 41 55 41 54 53 48 89 fb`.

### 3.2 The autosave accumulator (HJ-03, HJ-04, HJ-05)

- **HJ-03 (PROVEN).** `CGameUI+0x628` is an int64 microsecond accumulator (Windows `+0x648`),
  initialised to `-2` by the ctor (`0x10133d0`). In the update `0x100fb20`, the autosave block
  does not run on every call:
  - Early return while the UI is hidden: `0x100fc70 cmp byte [rbx+0x597],0; 0x100fc77 jne
    0x1010220`. That branch calls `0x3260150`, `0x15c8070` and `0x11d35d0(…, t, dt)` and returns
    through `0x101025b jmp 0x10101f3`, never reaching `0x10101a7`. +0x628 is then neither grown
    nor compared.
  - Two more paths skip the block, both throwing: `0x1010084 je 0x101064e` calls
    `__throw_future_error(3)` (`0x1010653`), and `0x10100ca je 0x1010658` calls
    `rethrow_exception` (`0x101066d`). The control-flow search from entry finds no other exit that
    avoids `0x10101a7`.
  - `0x10101a7 call 0xc1a300; 0x10101ac mov eax,[rax+0x120]; test eax,eax; 0x10101b4 jg
    0x10102a0`: the rest of the block runs only when `autosaveIntervalMinutes > 0`.
  - `0x10102a0`..`0x10102b9`: gate `0x30e9760([[this+0x18]+0x3f0], &.bss 0x5a4f840)`. When it
    returns 0, the accumulator grows (`je 0x1010358`).
  - `0x1010358`..`0x101036d`: `acc = acc < 0 ? acc + 1 : acc + dt`, then `jmp 0x10102bf`. r14 is
    written only at entry (`0x100fb2c mov r14,rdx`) and at `0x10101c1`, which lies after this block
    on every path.
  - `0x10102bf`..`0x10102d9`: `if (acc > (int64)interval * 60000000)` (`0x10102c4 movsxd`,
    `0x10102cb imul 0x3938700`, `0x10102d2 cmp`, `jle 0x10101ba`). If so, `0x10102e2 call
    0xfeb560(this)` and `0x10102e7 acc = 0`. The comparison runs whatever the gate returned.
  - `+0x597` is written only by the bool callback `0xfddd40`; a displacement scan of `.text`
    finds no other reader or writer of `0x597`, apart from `0x100fc70`. The callback's lambda
    captures the CGameUI (`rax = [[rdi]]`). When its argument is true it saves the visibility of
    `+0x18`, `+0x5e0`, `+0x758` and `+0x5c8`, sets `+0x597 = 1` (`0xfdddf2`) and hides them
    (`0xfdde02`, `0xfdde18`, `0xfdde2b`, `0xfdde3c`). When false it clears `+0x597`
    (`0xfdde68`) and restores them. The CGameUI builder `0xffae10` registers it (lea `0x1008170`,
    manager `0xfe4c40`, `call 0x3058230` at `0x100818c`). It is a persistent state flag; that it is
    the player's "hide UI" toggle is INFERRED.
  - Other writers of +0x628 (the same scan, restricted to CGameUI code):
    - AutoSave `0xfeb7b9` (= 0);
    - AutoSave's completion lambda `0x1019f91` (= 0);
    - `CreateSaveGameLayout` lambdas `0x10a8b90` (`0x10a8e55`, = 0, together with
      `+0xae8 = 1`) and `0x10a7ca0` (`0x10a7d22`, = 0, together with `+0xae8 = 0`).
  - Calls through `0x312af90` (AL-19 site 2) pass dt = 0.
- **HJ-04 (PROVEN).** `autosaveIntervalMinutes` is the int32 at `GlobalSettings+0x120` (Windows
  `settings+0x130`):
  - `GetGlobalSettings` `0xc1a300` returns `*(.bss 0x5a4f748)` and asserts it is non-null.
  - `SetGlobalSettings` `0xc1a2b0` is called from Run2 at `0x9b1b21`.
  - The settings reader `0xc3ea70` stores the `autosaveIntervalMinutes` value into `[r12+0x120]`
    (`0xc3eb7b`..`0xc3eb98`). The writer `0xc35610` reads `[r12+0x120]` for that key
    (`0xc35927`, `0xc35931`).
  - On this machine `settings.lua` has `autosaveIntervalMinutes = 10`. With 0 or a negative
    value, the forced autosave cannot fire.
- **HJ-05 (PROVEN).** `void UI::CGameUI::AutoSave()` `0xfeb560` (rdi CGameUI*) has one direct
  caller, `0x10102e2`.
  - `0xfeb79c cmp byte [rbx+0xae8],0; jne 0xfec08b` fails the assertion `!IsCurrentlySaving()`
    (string `0x3f29308`, signature `0x40f3fb0`).
  - `0xfeb7ad` sets `+0xae8 = 1`; `0xfeb7b9` sets `acc = 0`.
  - The completion lambda `0x1019f40` asserts `IsCurrentlySaving()` (`0x1019f81`/`0x1019f88`),
    then clears `+0xae8` (`0x1019f8a`) and `acc` (`0x1019f91`).
  - The assertion handler `0x2fcb860` never returns: it builds an object and calls `0x2fcb5e0`,
    which prints to the ostream at `.bss 0x5bbf580`. That it then throws is INFERRED.
  - AutoSave has no exit before `0xfeb7ad`: the control-flow search from `0xfeb560` finds no exit
    that avoids it, and the only branch away is the assertion at `0xfeb7a3`. Called with
    `+0xae8 == 0`, it always sets `+0xae8 = 1` and `acc = 0`, unless one of the calls before
    (`0x30e8dd0`, `0x2fd1420` tr("Autosave..."), `0x30e9940`, `0xfeb180`, `0x192cf10`) throws.
  - `+0xae8` has a second pair of writers, the in-game save dialog (`CreateSaveGameLayout`
    lambdas):
    - `0x10a8b90` checks `+0xae8 == 0` (`0x10a8e41`, `jne 0x10aa02b`), then sets `+0xae8 = 1`
      (`0x10a8e4e`) and `acc = 0` (`0x10a8e55`).
    - `0x10a7ca0` checks `+0xae8 != 0` (`0x10a7d0e`, `je 0x10a7fde`), then clears it (`0x10a7d1b`)
      and `acc` (`0x10a7d22`).
    - The displacement scan finds no other writer in CGameUI code; the ctor's
      `mov word [r15+0xae8]` at `0x1013b4e` initialises it together with the next byte. There are
      two more readers:
      - `0xfe22f0` (`0xfe22f4`), called from `InGameMenuUI::CreatePage` (`0x10ad696`,
        `0x10ad75d`) and lambdas `0x10a6623`/`0x10a66a3`;
      - the `bool()` invoker `0xfdb410` (`mov rax,[rdi]; movzx eax,byte [rax+0xae8]; ret`), whose
        only reference is `0x1003846` in the CGameUI builder.

      The qword write `0xe7ced3` is in street-builder code (`0xe7c4b0`, strings
      `StreetBuilderPool`/`action-streetbuilder`) and belongs to another class.
  - Forcing the accumulator while `CGameUI+0xae8 != 0` would make AutoSave run into that
    assertion. **The force must check `+0xae8 == 0` right before the write.** Windows' code did
    not. A manual save that starts while a force is still in +0x628 zeroes acc and cancels the
    force silently; HJ-06 therefore never leaves the force in +0x628 across calls.
- **HJ-06 (PROVEN from HJ-03/HJ-05 control flow and arithmetic).** Force recipe. The force lives
  in `+0x628` only for the duration of one call to the original, so it can never fire later at a
  time we did not choose, and a manual save cannot silently cancel it.
  1. Inside the HJ-02 detour, only when the AL-19 gate says frame step (frame depth 1, nested
     depth 0, this thread) and a force is pending, check all of these right before the write:
     - `byte CGameUI+0x597 == 0` (otherwise the call returns at `0x100fc77` without the block);
     - `byte CGameUI+0xae8 == 0` (otherwise AutoSave asserts, HJ-05);
     - `int32 GlobalSettings+0x120 > 0` (otherwise `0x10101b4` skips the block).
     If any fails, call the original unchanged and keep the request pending.
  2. Save `old = acc`, write `acc = 2^62`, call the original with the same three values.
  3. Undo on every way out. A scope guard (it also runs during unwinding, because the update can
     throw at `0x1010653`/`0x101066d`) restores `acc = old` if `acc >= 2^62` is still true.
  4. After a normal return:
     - `acc >= 2^62`: the block did not run (for example `+0x597` became set inside the call).
       Restore `old`; the request stays pending for a later frame.
     - `acc < 2^62` and `+0xae8 != 0`: a save started in this call. The block reached
       `0x10102e2`, AutoSave set `+0xae8` (`0xfeb7ad`), and acc was reset (`0xfeb7b9`/`0x10102e7`).
       Clear the request.
     - `acc < 2^62` and `+0xae8 == 0`: acc was reset but no save is running (the completion lambda
       `0x1019f40` or `0x10a7ca0` ran inside the call, or AutoSave threw after its reset). Clear the
       force and retry on a later frame, with a small retry limit.
  - Arithmetic: `interval * 60000000 <= 0x7fffffff * 0x3938700 ≈ 1.29e17 < 2^62 ≈ 4.61e18`, so
    `2^62` (or `2^62 + dt`) always passes `0x10102d2`. `2^62 + dt` cannot overflow for any
    `dt < 2^62`. `INT64_MAX + dt` wraps negative, which is why 2^62 is used.
  - All of this is on the thread that runs the frame step, the same thread as the game's own
    read-modify-write of `+0x628`. Windows wrote it from another thread.
- **HJ-07 (PROVEN; thread identity from glibc's `.init_array` order).** The global `g_gameUI` at
  `.bss 0x5a4fb38`:
  - `SetGlobalGameUI` `0xfe29d0` asserts `(bool)g_gameUI != (bool)gameUI`. It is set at
    `0x115c965` right after `CMenuUI+0x4c8` gets the new CGameUI, and cleared at `0x1158105`
    right after StopGame zeroes `+0x4c8`.
  - `GetGlobalGameUI` `0xfe2a20` returns it only when `IsMainThread` `0x3231260` is true:
    `pthread_self()` equals `*(.bss 0x5b37168)` (`0x3231272`/`0x3231277`). When the
    `__pthread_key_create` GOT slot `0x5a46348` is null, the compare is against 1 (`0x323128d`).
  - `0x967120` is the only writer of `0x5b37168` (`0x967138`, `0x967145`). It is `.init_array`
    entry 619 (RELATIVE relocation at `0x5a002a0`), so it runs on the initial thread before
    `main`, and that thread then runs `main` (AL-04).
  - A captured CGameUI `this` is stale once `CMenuUI+0x4c8`/`g_gameUI` is 0. That replaces
    Windows' `g_gameUi = 0` reset on `CreatePage(2)`.

### 3.3 Inline-hook alternative (not recommended)

Both update functions start with the same bytes: `f3 0f 1e fa` endbr64, `55` push rbp,
`48 89 e5` mov rbp,rsp, `41 57` push r15, `41 56` push r14, `49 89 d6` mov r14,rdx, then
`41 55 41 54 53 48 89 fb`. `PrologueSteal(code, 14)` returns 15, and those 15 bytes are
position-independent. The vtable swaps in 2.1 and 3.1 need no code patch and no trampoline, so
they are the recommended hooks.

## 4. Automod: `activeMods` in settings.lua

- **AM-01 (PROVEN, MEASURED).** Location: `<userdata folder>/settings.lua`, here
  `.../userdata/125253817/1066780/local/settings.lua` (4042 bytes). At exit the game logs
  `Saved settings to settings.lua` (stdout line 228 of 240; string at `0xc1ad45` in `0xc1ac10`).
- **AM-02 (PROVEN, MEASURED).** Format: LF line endings, tab indents,
  `function data()\nreturn {\n\tactiveFilters = { },\n\tactiveMods = {\n\t\t{ "_urbangames_deluxe_pack", 1, },\n\t\t{ "_urbangames_preorder_pack", 1, },\n\t},\n...`.
  The Windows edit's `find("activeMods = {")` and `find("return {")` anchors both occur. The
  Windows CRLF detection yields LF here.
- **AM-03 (PROVEN for this path).** The reader that consumes `activeMods` (key at `0xc3eadf`)
  and `autosaveIntervalMinutes` is `0xc3ea70`, called from `T Read(const Value&) [with T =
  AppConfig]` `0xc41a90` (`0xc41ac7`). The chain is Run2 `0x9b1a90` → `0xc1f110` (`0x9b1c26`) →
  `0xc41cc0` (`0xc1f2af`) → `0xc41a90` (`0xc41cee`) → `0xc3ea70`, and Run2 is reached from
  `main` (AL-04). So the settings are parsed after `main()` starts. Whether any other code
  (a static initialiser) reads `settings.lua` earlier was not searched exhaustively; none was
  found.
- **AM-04 (PROVEN, MEASURED + static).** The game rewrites `settings.lua` from memory at exit
  (AM-01). The writer `0xc35610` emits `activeMods` (`0xc35879`). An edit made while the game
  runs is lost, so the edit must land before AM-03.
- **AM-05 (INFERRED: glibc semantics, not measured here).** `boot.cpp` runs from `LD_PRELOAD`
  and dlopens `tpf2_menu.so` in its ELF constructor. glibc runs preloaded objects' constructors,
  and those of objects they dlopen, before it jumps to the executable's entry. The entry here
  (`e_entry` `0x5bc11a0`) is a stub in its own r-x PT_LOAD `0x5bc1000`-`0x5bd912a` that saves
  every register; `_start` is `0x97a270`. A synchronous call in `tpf2_menu.so`'s constructor
  therefore runs before AM-03. A call from the detached `Init` thread is **not** guaranteed to.
- **AM-06 (INFERRED).** The entry to add is `{ "mp_lockstep", 1, }`. The repo's mod folder is
  `mod/mp_lockstep_1`, and the DLC entries use the same `{ name, version }` shape. Windows
  measured this edit fixing a fresh install. The mod is not deployed in the game's `mods/` on this
  machine yet.

## 5. Windows to Linux, side by side

| item | Windows (build 35924) | Linux (build 35924) |
|---|---|---|
| CMenuUI update | vftable `0x301dc38` slot 33 → `0x672b10` | vtable `0x5a16f98` slot 34 (`0x5a170a8`) → `0x1140a90` |
| StartSavegame | `0x6785c0` | `0x113f450` |
| app accessor / save manager | `0xbb23c0` / app+200 | `0x18ba890` (`*(.bss 0x5a52d80)`) / app+0xc8 |
| SavegameInfo getter | `0x2e6ca0(out, mgr, &id)` | `0xc7e520(out, mgr, &id)` |
| SavegameInfo size / dtor | 0x110 / `0x2de250` | 0x108 / `0xc84f00` |
| LoadGameParams size / ctor / dtor | 0x138 / `0x553b70` / `0x5576a0` | 0x130 / `0x1162170` / `0x101da10` |
| SaveGameId | 0x60 `{wstring path; string name; string ns}` | 0x60 `{string path; string name; string ns}`, dtor `0xf31740` |
| CMenuUI running / initialising / queued | `+0x4e8` / `+0x1988` / `+0x19a0` | `+0x4c8` / `+0x5e8` / `+0x5f8` |
| CGameUI update | inline hook `0x5741d0`, steal 21 | vtable `0x5a11468` slot 34 (`0x5a11578`) → `0x100fb20` |
| autosave accumulator / interval | `+0x648` / settings+0x130 | `+0x628` / GlobalSettings+0x120 |
| AutoSave / saving flag | `0x563500` / (not used) | `0xfeb560` / `CGameUI+0xae8` (must be 0) |
| string construction | MSVC assign `0x83270` | `std::string(const char*)` `0x1128b80` or direct SSO |
| save companions | `.sav`, `.sav.lua`, `.jpg` | `.sav`, `.sav.lua`, `.jpg`; no `.info` (`0x9d7ba0` returns false); folder-layout names unused (StandardSaveGameBackend reads `pair.first` only) |
| Step entries (frame gate) | (none: Windows hooked without a gate) | Step `0x305a830` called at `0x30822e6` (frame), `0x312afb2` (0/0), `0x112e948` (step listener) |
| CGameUI hide-UI flag | (not used) | `CGameUI+0x597` (autosave block skipped while set; written only by `0xfddd40`) |
| settings.lua newlines | CRLF or LF (detected) | LF |

## 6. Integration notes (outside this area's files)

- `native/linux/CMakeLists.txt`: add `src/menu_game_linux.cpp` to the `tpf2_menu` sources. Nothing
  else is needed. The guarded call is a top-level `__asm__` block inside that file, so no ASM
  language is required. The static runtime, `--exclude-libs,ALL`, `exports_none.map` and
  `--no-undefined` are the options it was link-tested with (9.5).
- `native/linux/src/menu_linux.cpp`, which is not this area's file:
  - add `#include "menu_game_linux.h"`;
  - in the constructor `MenuLoad()`, before `std::thread(Init).detach()`, open `tpf2_menu.log`
    (today `Init` opens it), then call `MenuGame_AutoEnableMod(Log)` synchronously (AM-05);
  - in `Init`, after the build-id check and before `panel::Init` (which starts the lobby), call
    `MenuGame_Install(g_base, Log)`. Its result needs no handling: the log says what is on.
- Nothing else is needed from other areas. `lobby_linux.cpp` already calls the API in section 7, and
  `panel_linux.cpp` already implements `panel::SetStatus` and `panel::OnGameUiFrame`.
- Deploy: only the rebuilt `tpf2_menu.so`. Automod needs the mod installed as
  `<game>/mods/mp_lockstep_<n>/mod.lua` (`tools/linux/install.sh` puts it there) or under
  `<userdata>/mods`.
- Flags and environment:
  - `automod=0` in `tpf2_menu_flags.txt` beside `tpf2_menu.so` is read by `MenuGame_AutoEnableMod`;
  - `autoload=0` is the lobby's flag: the lobby then never calls `MenuGame_RequestAutoload`;
  - `TPF2MP_USERDATA=<.../1066780/local>` overrides the user data folder (9.4).
- For every area with code between game frames (detours, call-site wrappers): EH-01. With
  `-static-libstdc++ -static-libgcc`, a function of ours that has a try/catch or a destructor to
  run aborts the game in `_Unwind_SetGR` when a game exception unwinds it. Such functions must have
  no LSDA (9.5 shows the check), or must catch with the game's runtime as `tpf2mp_mg_guarded` does.

## 7. The API these facts serve

```
bool MenuGame_Install(uintptr_t gameBase, Tpf2mpLogFn log);   // AL-01..05 + HJ-01/02 swaps, AL-19 redirects
void MenuGame_AutoEnableMod(Tpf2mpLogFn log);                  // SAVE-01/04, AM-01..06
std::string MenuGame_SaveDir();                                // SAVE-01..04: <userdata>/save
bool MenuGame_NewestSave(std::string* path);                   // *.sav there, mp_shared.sav last resort (SAVE-08)
bool MenuGame_PlaceSharedSave(const std::string& src, std::string* placedName);   // SAVE-05/06
void MenuGame_RequestAutoload(const std::string& placedName);  // AL-06..18, taken in the AL-02 detour
bool MenuGame_ForceAutosave();                                 // HJ-03..07, applied in the HJ-02 detour
```

`MenuGame_Install` does two things:
- It swaps the AL-02 and HJ-02 vtable slots (AL-05 checks).
- It redirects the three Step call sites of AL-19 (`0x30822e6`, `0x312afb2`, `0x112e948`, each
  checked by `Tpf2mpRedirectCall` against callee `0x305a830`).

If a redirect fails, autoload and the forced autosave stay off with a log line, because the
detours cannot tell frame calls from nested ones.

`MenuGame_ForceAutosave` only queues a request, and returns true only when all of these hold:
- the HJ-02 swap and all three AL-19 redirects are installed;
- a CGameUI was seen by the gated detour and is still `CMenuUI+0x4c8` / `g_gameUI`;
- `GlobalSettings+0x120 > 0`.

The detour applies the request on a frame-step call (HJ-06). It re-checks `CGameUI+0x597 == 0`,
`CGameUI+0xae8 == 0` and the interval immediately before writing, and never leaves the force
in `+0x628` after the call. Whether a save actually started shows only after that call
(HJ-06 step 4).

## 8. Open questions

- Per-frame cadence of `UI::CCore::Run` → `0x30803d0` on Linux. The call chain and the single
  Step call per `0x30803d0` invocation are proven; the rate is Windows-measured.
- Which components the 16 callers of `0x312af90`, the tail jump at `0x1c917c4` and the listener
  `0x112e8a0` actually step, and on which thread. The AL-19 gate makes the answer unnecessary for
  our detours.
- What triggers the `+0x597` callback `0xfddd40` (registered at `0x100818c`); "hide UI" is
  INFERRED.
- Whether the backend's mount lookup (vcall `+0x18`) uses only `SaveGameId.namespace`, so that
  `path ""` works (AL-18).
- The file name and namespace AutoSave passes (SAVE-08).
- Which Steam account the pre-`main` resolver should pick when several `userdata/<id>` folders
  have `1066780/local` (SAVE-04).
- Whether `0x2fcb5e0` throws or aborts (HJ-05). Either way the `+0xae8` guard is required.
- Everything in section 9 in the running game.
- Whether the game keeps `crash_dump/stdout.txt` open, which the second user data folder rule
  relies on (9.4). `0x9b7a50`, which receives that path, was not traced.
- The C++ runtime inside the game's container (pressure-vessel chooses `libstdc++.so.6` and
  `libgcc_s.so.1`). EH-01 was measured with the host's GCC 15.2 runtime.

## 9. The implementation (`native/linux/src/menu_game_linux.{h,cpp}`)

It was built and tested off-game only (9.5). Every address in the code is one of the rows above,
and a script checks every one the code relies on against the binary (97 checks, 0 failures).

### 9.1 Install and its run-time checks

`MenuGame_Install(base, log)` runs once; later calls return the first result. It trusts the
caller's build-id check for `base`, then does the following:

1. It resolves the game's C++ runtime for the guarded load (9.3).
2. It compares 24 byte strings with build 35924's, in three groups. A group that differs turns off
   what depends on it, and each difference is logged with its address.
   - Gate: Step's prologue at `0x305a830` (22 bytes; it reads only rdi, rsi and rdx).
   - Autoload:
     - the prologues of the functions the load calls: `0x1162170`, `0x101da10`, `0xc7e520`,
       `0xc84f00`, `0x113f450`, `0x1128b80`, and `0xf31740` with its `lea rax,[rbx+50h]`;
     - the whole app accessor `0x18ba890`, including its `.bss 0x5a52d80` displacement and `ret`;
     - the instructions that use the fields: `0x113f47e`, `0x1140353`, `0x1140c50`, `0x1140cf8`,
       `0x1140b9b`.
   - Hot join:
     - the autosave block in the CGameUI update: `0x100fc70` (the +0x597 test and its `jne`),
       `0x10101ac` (interval > 0), `0x1010358` (the growth), `0x10102c4` (the compare with
       `imul 60000000`) and `0x10102e2` (the AutoSave call and the reset);
     - AutoSave's +0xae8 instructions `0xfeb79c` and `0xfeb7ad`;
     - the `.bss` references `0xc1a304` (`0x5a4f748`), `0xfe29e9` and `0xfe2a35` (`0x5a4fb38`).
3. It checks each vtable:
   - slot -1 is the class's typeinfo;
   - the typeinfo's name pointer and string are `N2UI7CMenuUIE` or `N2UI7CGameUIE`;
   - slot 34 holds the update;
   - the update starts with the 23 bytes of AL-05 and HJ-02.
4. It installs the frame gate. All three sites must be `e8` calls of Step before any is touched.
   Then the listener site is redirected, then the 0/0 site, and the frame site last. If a redirect
   fails, the earlier ones are put back and both features stay off.
5. It swaps the slots. The original pointer is published first. The page's protection is read from
   `/proc/self/maps` and restored after the single 8-byte store.

The log line `autoload ON|OFF; forced autosave (hot join) and the in-game signal ON|OFF` sums it up.

### 9.2 The frame gate without destructors

- Each wrapper records its frame address in a per-thread array, one array for frame Steps and one
  for nested Steps.
- A detour acts only when exactly one frame mark and no nested mark lie above its own frame.
- A live wrapper always lies above everything it encloses. So a mark at or below a frame being
  entered belongs to a wrapper that an exception unwound, and it is dropped. The gate recovers on
  the next frame after a throw.
- More than 32 nested wrappers disable any action until they return.
- `panel::OnGameUiFrame` is signalled on every call of the CGameUI update for the object that
  `g_gameUI` names, nested or not.

### 9.3 Exceptions (EH-01, EH-02, EH-03)

- **EH-01 (MEASURED off-game).** The setup: a shared library linked like `tpf2_menu.so`
  (`-static-libstdc++ -static-libgcc -Wl,--exclude-libs,ALL`) sits between frames of a program
  that throws with `libstdc++.so.6` and `libgcc_s.so.1`. That is the game's arrangement: both
  libraries are NEEDED, and `__cxa_throw`, `__cxa_begin_catch`, `__gxx_personality_v0` and
  `_Unwind_Resume` are imports. Results:
  - `catch (...)` in the library: SIGABRT;
  - a local object with a destructor in the library: SIGABRT;
  - no landing pad in the library: the exception passes through, and the program catches it.

  The backtrace is `abort` <- `_Unwind_SetGR.cold` <- the library's static
  `__gxx_personality_v0` <- libgcc_s `_Unwind_RaiseException` <- libstdc++.so.6 `__cxa_throw`. The
  static `_Unwind_SetGR` is working on a context that the dynamic unwinder built. This was measured
  with the host's GCC 15.2 runtimes, not the container's. That the container behaves the same is
  INFERRED: the static copy, which is the one that aborts, is the same either way.
- **EH-02 (PROVEN: the game's LSDA call-site tables).** Consider an exception that leaves a slot-34
  update. For each caller, the return address minus 1 is looked up in its call-site table:
  - Step2 `0x305a1c0`, call `0x305a7b1` (`call rax`, 2 bytes): record `[0x305a5c9, 0x305a7b3)` has
    no landing pad, so the exception passes on (LSDA `0x531c0d0`, call-site encoding uleb128);
  - Step `0x305a830`, call `0x305a8dd`: landing pad `0x305a901`, `catch (Exception)`;
  - `0x30803d0` (call `0x30822e6`) and `CCore::Run` `0x30826d0` (call `0x30828a1`): cleanups only;
  - Run2 `0x9b1a90` (call `0x9b2cca`): `catch (Exception)`, then a cleanup;
  - Run `0x9b3d50` (call `0x9b56ed`): `catch (std::bad_alloc)` twice, `catch (Exception)`, then a
    cleanup;
  - `main` (call `0x95e8e3`): a cleanup only;
  - the other ways into Step: `0x312af90` has no LSDA, and the listener `0x112e8a0` (call
    `0x112e948`) has a record without a landing pad;
  - the game's own getter call in the Missions lambda (`0x1140360`): landing pad `0x11407a9`,
    `catch (std::exception)`, then a cleanup.

  So a `std::runtime_error` from the getter, thrown inside a frame update, finds no handler above
  it. The search phase fails and the game ends in `std::terminate`. The game guards its own call.
- **EH-03 (PROVEN).** `Exception` (typeinfo `0x5a00988`, a `__vmi_class_type_info` named
  `9Exception`) has two bases: `std::exception` and `boost::exception` (typeinfo `0x5a00930`). So
  `catch (Exception)` does not catch a `std::runtime_error`.

What the code does about it:

- The GAME PATH functions have no try/catch and no destructors: the wrappers, both detours, the
  tick, the force and the load body. `check_lsda.py` verifies on the built library that none of
  them has a personality or an LSDA. Our code that can throw lives in noinline helpers that catch
  their own exceptions.
- The load runs in `tpf2mp_mg_guarded(fn, ctx)`. This is assembly identical to GCC's output for
  `try { fn(ctx); } catch (...) { return 1; }`, except for three points:
  - its CIE names the personality through an indirect slot;
  - its handler calls `__cxa_begin_catch` and `__cxa_end_catch` through slots;
  - an unwind that is not a C++ exception (exception class other than `GNUCC++`) is passed on with
    `_Unwind_Resume`.

  Install fills the slots with `dlsym(RTLD_DEFAULT, ...)` and refuses any symbol that resolves
  into this library. Autoload stays off if any is missing.

  After a caught throw:
  - what was built is destroyed as the Missions page does it, and the getter's `out` is never
    destroyed after the getter's own throw;
  - the caught type and its `what()` are logged;
  - the status line asks the player to use LOAD GAME.
- Exceptions from the game's own update pass through the detours. If one unwinds a forced call, the
  2^62 stays in +0x628 until the next call of the update. That call always comes through the
  detour, which first puts the old value back. It does so only on the object being updated or on
  the one `g_gameUI` names. After three such throws the request is dropped.

### 9.4 Save folder, saves and automod (design)

- User data folder: the first rule that gives an answer wins.
  1. `TPF2MP_USERDATA`.
  2. A file open in the process named
     `<...>/userdata/<digits>/1066780/local/crash_dump/stdout.txt`. Run builds that name at
     `0x9b4c90`; that the game keeps it open is not verified.
  3. The only `userdata/<id>/1066780/local` under the Steam roots, deduplicated by realpath:
     `$HOME/.steam/steam`, `$HOME/.steam/root`, `$XDG_DATA_HOME/Steam`,
     `$HOME/.local/share/Steam`, Flatpak's, `$HOME/snap/steam/common/.local/share/Steam`, and the
     root above `steamapps/common/` of the running executable.
  4. With several of those, the one whose `settings.lua` is newest. A folder with that file beats
     one without; between folders without it, the folder's own mtime decides.
- `MenuGame_NewestSave`: the newest regular `*.sav` by mtime (to the nanosecond). `mp_shared.sav`
  counts only when there is no other.
- `MenuGame_PlaceSharedSave`:
  - it refuses a missing, empty or non-`.sav` source;
  - it copies the `.sav`, `<src>.lua` and `<src minus .sav>.jpg` to `*.mptmp` names, synced;
  - it removes the old companions, renames the `.sav`, then renames the companions;
  - a companion the source lacks, or that failed to copy, is removed from the destination;
  - sharing `mp_shared.sav` itself (same device and inode) only stamps it;
  - the placed name is `mp_shared`.
- Automod:
  - `automod=0` (the panel's flag syntax) turns it off;
  - the version is the highest `n` of an `mp_lockstep_<n>/mod.lua` in `<game>/mods` or
    `<userdata>/mods`; with none, the file is not touched;
  - an existing `"mp_lockstep"` anywhere in the file leaves it alone;
  - otherwise it makes Windows' edit: the entry goes after `activeMods = {`, or a new list goes
    after `return {`, and CRLF line endings are kept;
  - it keeps a one-time `settings.lua.mpbak`, and writes `settings.lua.mptmp`, synced, then renamed
    over the original with the original mode.

### 9.5 Off-game tests (in the area's scratch folder)

- `check_tables.py`: 97 checks of the source against the binary: every RVA, byte string, vtable,
  typeinfo, call site, `.bss` target and field displacement.
- `check_lsda.py`: no GAME PATH function has an LSDA, in both the CMake-like build and the test
  build, and the guard's personality pointer is its slot.
- `exp/`: reproduces EH-01.
- `test_hooks`:
  - Setup:
    - a fake image at the real RVAs, with build 35924's bytes copied from the binary at every
      place the code checks;
    - a pop-and-jump into a C++ fake after each checked prologue;
    - the real site bytes inside stubs, with unwind info registered with libgcc_s;
    - vtables and `.bss` pointers filled in, and the RELRO pages read-only;
    - the library with its static runtime; the program with `libstdc++.so.6`.
  - Install: idempotence and page protections.
  - The gate: UI builder and listener steps, inside and outside frames.
  - Every autoload branch: running game, initialising, queued load, missing file, getter throw,
    StartSavegame throw, refusal, layout mismatch, long names, bad names, watchdog.
  - Every force branch: a builder step inside the forced call, saving, hidden UI, block skipped,
    the update throwing three times, reset without a save, interval 0, stale CGameUI.
  - Exceptions escaping a whole frame, and escaping a nested Step.
  - Extra modes: `gatefail`, `bytes` (one hot-join byte differs) and `slot` (CMenuUI's slot already
    taken).
- `test_files`:
  - the folder rules: Snap-style links, several accounts, the override, the executable's own Steam
    root (in a re-executed copy), the open log;
  - the newest save;
  - placement: companions, same file, refusals, missing folder, no temporary files left behind;
  - automod: every branch, the file mode kept, and this machine's settings.lua edited and parsed
    by `luac5.2 -p`.
- Not covered: the real game, a failing `Tpf2mpRedirectCall`, and the 60 s expiry of a force.

### 9.6 First in-game run: what the log should say

`tpf2_menu.log` should show, in order:

1. `[automod] ...` (from the constructor);
2. `[menugame] the load catches with the game's own C++ runtime (.../libstdc++.so.6)`;
3. `[menugame] frame gate: Step calls 30822e6, 312afb2 and 112e948 redirected`;
4. `[menugame] autoload ON ...; forced autosave (hot join) and the in-game signal ON ...`;
5. `[autoload] the title menu's frame update runs`, once the menu shows;
6. `[hotjoin] a game's frame update runs (CGameUI ...)`, once a game runs.

If line 5 never appears, the gate never sees a frame. The watchdog then says so 15 s after a request.
