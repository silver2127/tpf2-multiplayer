# Slice, time area: the Linux map

Linux RE for the slice's game-speed and calendar commands. Scope: the three `make_cmd` factories
SetGameSpeed, SetCalendarSpeed and SetDate; the `UI::Clock` speed buttons and pause toggle that issue
SetGameSpeed; the editor's date picker (SetDate) and date speed slider (SetCalendarSpeed); the
`SPEEDBTN` / `SETDATE` / `CALSPEED` inject lines; and the cancel rules while a session is live. This
replaces `native/src/slice_hook.cpp` ~136-156 (ids and callers), the three rows id 15/16/17 of
`FACTORIES[]`, `CaptureSpeedButton` / `CaptureCalendar` ~1497-1605 and the id 15/16/17 branches of
`DeferHandler` ~3556-3572 for the Linux build.

Binary: `TransportFever2`, Steam build 35924, GNU build-id `3a0e156390b0e6f1e372051c24802c8493ae454a`.
Addresses are Linux RVAs (the PIE links at 0; live = image base + RVA). "Windows" means the RVAs in
`docs/re/COMMANDS.md` and `slice_hook.cpp`, relative to `0x140000000`.

Status labels: **PROVEN** = the instructions, strings, relocations or data quoted here show it and can
be re-derived from the binary; **INFERRED** = placed by naming, by the Windows docs or by elimination,
so it stays out of patch code; **UNPROVEN** = not established. Nothing here was checked in the running
game (static map).

Revision (after independent verification): the `DoStep` steal is **14**, not 17 (§7.5); `DoStep`
re-clicks the selected speed button by itself when the engine runs faster than this machine's
performance cap, so `0xf6b88e` is not click-only and a paced or replayed speed can come back as a
capture (§3 "The clamp", §7.7); the button signal, its synchronous delivery, the max-speed function and
the `UI::Clock` vtable slot are now traced; the Lua consumers are cited by their text.

## How the evidence was produced

- Disassembly: capstone (x86-64) linear sweep from each function start over its FDE size from
  `/home/topsnek/tpf2-re/linux/functions.csv`; RIP-relative string loads resolved against `.rodata`.
  `objdump -d --start-address=A --stop-address=B TransportFever2` shows the same instructions.
- Call sites: every `e8`/`e9` rel32 in `.text` whose target is the function, each confirmed to sit on an
  instruction boundary of its containing FDE. Branches into stolen ranges: every `e8`/`e9`/`0f 8x`
  rel32 byte pattern at every offset of `.text`, every rel8 pattern within reach, plus each function's
  own internal jumps.
- Stored pointers: all `SHT_RELA` relocations scanned for an addend equal to the function, a raw 8-byte
  scan of the file, and every REX `8d /r` RIP-relative `lea` in `.text`.
- Steal lengths: a line-for-line Python port of `hook_posix.cpp` `SafeInsnLen`/`PrologueSteal` run on
  the file bytes.
- RTTI: typeinfo name string -> the `R_X86_64_RELATIVE` relocation whose addend is it (typeinfo+8) ->
  the relocation whose addend is the typeinfo (vtable slot -1).
- Field uses: memory operands with the field's displacement in the functions of `0xf60880..0xf715b0`.
- Types: mangled `.dynsym` object names (sol2 statics) that spell out the `CmdData` variant and the
  member types; `__PRETTY_FUNCTION__` strings for signatures.
- The throwaway scripts lived in the area's scratch directory; the method above redoes any row.

## 0. Everything the code depends on

| item | Linux | Windows | status |
|---|---|---|---|
| hook `make_cmd::SetGameSpeed(int)` | `0x15eb600`, steal 18 | `0x9de9e0`, steal 21, id 15 | PROVEN |
| hook `make_cmd::SetCalendarSpeed(int msPerDay)` | `0x15eb690`, steal 18 | `0x9de870`, steal 21, id 17 | PROVEN |
| hook `make_cmd::SetDate(boost::gregorian::date)` | `0x15eb7b0`, steal 18 | `0x9de9b0`, steal 21, id 16 | PROVEN |
| speed buttons, pause toggle **and DoStep's clamp** (Clock ctor `lambda(bool)`) | factory returns to **`0xf6b88e`** | `0x4f0097` | PROVEN |
| `UI::Clock::TogglePause` direct call | factory returns to **`0xf6c28d`** | `0x4efb8f` | PROVEN |
| editor date picker (`CreateDateModifierLayout` lambda) | factory returns to **`0xf6bce5`** | `0x4efe54` | PROVEN |
| editor date speed slider (`MakeDateSpeedLayout` lambda) | factory returns to **`0xf70410`** | `0x4f2af6` | PROVEN |
| the third Windows clock site | none (§2) | `0x4f26ef` | PROVEN absent |
| `UI::Clock::m_commandCount` | int at Clock+`0x478` | | PROVEN |
| clock completion callback | invoker `0xf6af90`, manager `0xf6ae90`, functor = `Clock*` | | PROVEN |
| date completion callback | invoker `0xf6afd0`, manager `0xf6ae50`, functor = `int*` (= Clock+0x478) | | PROVEN |
| counter increments after Add | `0xf6b90b` (buttons), `0xf6bd63` (date) | | PROVEN |
| hook `UI::Clock::DoStep` (counter compensation, clamp detection, §7) | `0xf6c4b0`, **steal 14** | | PROVEN |
| DoStep's clamp calls (emit) | `0xf6c676`, `0xf6c697` | | PROVEN |
| `UI::Clock` vtable | address point `0x5a0e258`, slot 34 (`0x5a0e368`) = DoStep | | PROVEN (not patched) |
| Add call sites (Add = `0x15da840`) | `0xf6b8ac`, `0xf6c2ab`, `0xf6bd03`, `0xf7042a` | | PROVEN (name INFERRED) |

## 1. The three factory hooks

### Identity

`CmdData` is a `std::variant` whose alternative order is spelled out in `.dynsym` object names, e.g.
`_ZZNSt8__detail9__variant15_Move_ctor_baseILb0EJN7CmdData12SetGameSpeedENS2_16SetCalendarSpeedENS2_10UpdateLogo...`
(`0x59be760`): index 0 SetGameSpeed, 1 SetCalendarSpeed, 2 UpdateLogo, 3 CreateLine, 4 DeleteLine,
5 UpdateLine, 6 SetLine, ... 15 BuildProposal, ... 26 SetDate, 27 SaveGame, 28 SetColor, 29 SetName,
... 36 Debug_SetSimPersonState. (Matches the Windows dispatch tags 7/12/13/14/15/22/28/31 and the tags
SLICE_LINES.md reads in its factories.)

The unnamed flat factories in the `make_command.cpp` block build that variant on the stack at
`[rbp-0xd70]`, write the variant index byte at `[rbp-0x28]` (= variant+0xd48), and hand it to the
packager `0x15eb570` (`Command(CmdData&&)`: `0x15eb583 call 0x15d8e60` Command ctor, `0x15eb58d`
`operator new(0xd50)`, `0x15eb5a2 mov [rbx],rax`, `0x15eb5da call 0x15f1800` move). The index byte
identifies each factory (PROVEN):

| factory | Linux RVA | FDE size | value stored | index write |
|---|---|---|---|---|
| SetGameSpeed | `0x15eb600` | 129 | `0x15eb62b mov [rbp-0xd70],esi` | `0x15eb634 movb [rbp-0x28],0` |
| SetCalendarSpeed | `0x15eb690` | 129 | `0x15eb6bb mov [rbp-0xd70],esi` | `0x15eb6c4 movb [rbp-0x28],1` |
| SetDate | `0x15eb7b0` | 129 | `0x15eb7db mov [rbp-0xd70],esi` | `0x15eb7e4 movb [rbp-0x28],0x1a` |

Member types (PROVEN, sol2 statics in `.dynsym`): `usertype_metatable<CmdData::SetGameSpeed, ...,
const char(&)[8], int CmdData::SetGameSpeed::*, ...>` (`...RA8_KcMS4_i...`),
`usertype_metatable<CmdData::SetCalendarSpeed, ..., const char(&)[13], int ...::*>` (`RA13_KcMS4_i`),
`usertype_metatable<CmdData::SetDate, ..., const char(&)[5], boost::gregorian::date ...::*>`
(`RA5_KcMS4_N5boost9gregorian4dateE`). `boost::gregorian::date` is one `uint32` day number, trivially
copyable, so it arrives in `esi` like the ints (the factory stores exactly 32 bits).

### Signatures and registers (SysV)

| factory | rdi | esi | returns |
|---|---|---|---|
| SetGameSpeed | `Command*` hidden return slot | int speed (0 = pause) | `rax = rdi` (`0x15eb65f mov rax,rbx`) |
| SetCalendarSpeed | `Command*` | int milliseconds per day (0 = stopped calendar) | `rax = rdi` (`0x15eb6ef`) |
| SetDate | `Command*` | uint32 boost::gregorian day number = Julian Day Number | `rax = rdi` (`0x15eb80f`) |

No Engine argument and no stack argument (Windows: rcx return, value in the low 32 bits of rdx). A
typed C++ detour `Command* D(Command* ret, int v)` with `__builtin_return_address(0)` as the caller
works exactly like `speedhook_linux.cpp`'s GetSpeed detour: the patch is a `jmp`, so `[rsp]` at detour
entry is the game's return address.

### Prologue, steal and runtime check

All three start with the same 18-byte prologue (PROVEN, bytes from the file):

```
f3 0f 1e fa             endbr64
55                      push rbp
48 89 e5                mov  rbp, rsp
41 54                   push r12
53                      push rbx
4c 8d a5 90 f2 ff ff    lea  r12, [rbp-0xd70]      <- ends at +18
48 89 fb                mov  rbx, rdi              (+18)
```

`PrologueSteal(code, 14)` (hook_posix.cpp, the Python port on the file bytes) decodes endbr64 = 4,
`55` = 1, `48 89 e5` (ModRM mod 3) = 3, `41 54` (REX push) = 2, `53` = 1, and `4c 8d a5 disp32`
(ModRM mod 2, rm 5, not RIP-relative) = 7, and returns **18** for all three. The stolen instructions
are rsp/rbp-relative only; `rbp` is set by the stolen `mov rbp,rsp` itself, so the trampoline is
self-consistent.

Verify the first 61 bytes (0x3d) before patching; they include the index byte and the packager call,
so a check cannot pass on the wrong factory:

| factory | bytes `+0x00..+0x3c` |
|---|---|
| SetGameSpeed `0x15eb600` | `f3 0f 1e fa 55 48 89 e5 41 54 53 4c 8d a5 90 f2 ff ff 48 89 fb 48 81 ec 60 0d 00 00 64 48 8b 04 25 28 00 00 00 48 89 45 e8 31 c0 89 b5 90 f2 ff ff 4c 89 e6 c6 45 d8 00 e8 33 ff ff ff` |
| SetCalendarSpeed `0x15eb690` | identical except `+0x37..+0x3c` = `01 e8 a3 fe ff ff` |
| SetDate `0x15eb7b0` | identical except `+0x37..+0x3c` = `1a e8 83 fd ff ff` |

(The three `call` rel32 values all resolve to `0x15eb570`: `0x15eb63d-0xcd`, `0x15eb6cd-0x15d`,
`0x15eb7ed-0x27d`.)

Hook safety (PROVEN):
- No `e8`/`e9`/`0f 8x` rel32 anywhere in `.text` targets `(start, start+18)` of any of the three.
  Internal jumps: `0x15eb643 je 0x15eb652`, `0x15eb662 jne 0x15eb670` (and the same offsets in the
  other two). The cold landing pads `0x7ce475`, `0x7ce493`, `0x7ce4cf` destroy the local variant and
  call `_Unwind_Resume`; none jumps back.
- No stored pointer: zero relocation addends, zero raw 8-byte occurrences, zero RIP-relative `lea` for
  any of the three. Every caller is a direct `call` (§2).
- No inlined copy elsewhere: the packager `0x15eb570` has 13 direct callers, all inside the
  `make_command.cpp` block `0x15eb000..0x15f1300`.

## 2. Every caller

Exhaustive (the `e8`/`e9` scan above; no other reference exists).

### SetGameSpeed `0x15eb600` (11 callers)

| returns to | containing function (source) | what it is | action |
|---|---|---|---|
| **`0xf6b88e`** | `0xf6b750` `UI::Clock::Clock(...)::<lambda(bool)>` (Clock.cpp) | speed buttons, pause toggle, speed cycler, **and DoStep's clamp** (§3) | **act** (a clamp only per §7.7) |
| **`0xf6c28d`** | `0xf6c1e0` `void UI::Clock::TogglePause()` | pause toggle when no button matches, §3 | **act** |
| `0xe0b164` | `0xe0b100` (CameraAction.cpp block `0xe093e0..0xe0c810`) | `0xe0b105 xor esi,esi` (speed 0), loads "Paused" `0xe0b229` | leave alone |
| `0xe13be4`, `0xe13d78` | `0xe13ab0` `void UI::CameraAction::Play(int)` | camera-path tool | leave alone |
| `0xe14bec` | `0xe14a00` `bool UI::CameraAction::Record(IProgressMonitor&)` | camera-path tool | leave alone |
| `0xfe794c` | `0xfe77b0` `void UI::CGameUI::GameStep(long long int)` | | leave alone |
| `0x112243b` | `0x11223e0` (MenuUI.cpp block, INFERRED) | Windows `menuui.cpp 0x657710` | leave alone |
| `0x112c0ef` | `0x112ba00` `UI::CMenuUI::SwitchToGameUI(bool)::<lambda()>` | | leave alone |
| `0x12cb3d7` | `0x12cb380` (DebugViewComp.cpp block, INFERRED) | Windows debug view `0x795900` | leave alone |
| `0x1950f23` | `0x1950ef0` (sol2 block) | `0x1950ef5 cvttsd2si esi,xmm0` (a Lua number), copies the Command out, no Add. Pushed as a closure upvalue (`0x1966832 lea`) in `scripting::SetupCommandInterface` right after its name string `setGameSpeed` (`0x1966732`) and parameter name `speed` (`0x1966708`): INFERRED `api.cmd.make.setGameSpeed`, what pacing's `CM.setSpeed` calls (Windows `0xc17eff`) | leave alone |

### SetCalendarSpeed `0x15eb690` (2 callers)

| returns to | containing function | what it is | action |
|---|---|---|---|
| **`0xf70410`** | `0xf70320` `MakeDateSpeedLayout(...)` slider lambda (Clock.cpp) | date speed slider, §5 | **act** |
| `0x1950fc1` | `0x1950f90` (sol2 block) | `0x1950f95 mov esi,edx`, no Add; pushed (`0x19663a4 lea`) right after the name string `setCalendarSpeed` (`0x19662a4`): INFERRED `api.cmd.make.setCalendarSpeed` | leave alone |

### SetDate `0x15eb7b0` (2 callers)

| returns to | containing function | what it is | action |
|---|---|---|---|
| **`0xf6bce5`** | `0xf6b9b0` `CreateDateModifierLayout(...)::<lambda()>` (Clock.cpp) | date picker, §4 | **act** |
| `0x1972ad2` | `0x19729c0` (sol.hpp block) | SetupCommandInterface's `lambda(sol::table, boost::gregorian::date, sol::this_state)`: see below | leave alone |

The Lua makers (PROVEN unless marked). `0x19638f0` loads `SetupCommandInterface` (`0x1963906`),
`sendCommand` (`0x1963a2d`) and `make` (`0x1963acd`) and registers the `make` functions. For `setDate`
it builds the name (`0x1966667 lea "setDate"`, `0x1966671 call 0x1951470` into `r14`) and calls
`0x195b2f0` with it (`0x1966686 mov rsi,r14`, `0x1966689 call`). `0x195b2f0`'s own assert signatures are
`user_allocate<functor_function<scripting::SetupCommandInterface(...)::<lambda(sol::table,
boost::gregorian::date, sol::this_state)>>>` (`0x195b93d`) and `pusher<sol::user<T>>::push_with(...Key =
const char* const&...)` (`0x195b95c`); it stores the call thunk `0x1972d50` (`0x195b530 lea`, then
`"__call"` `0x195b548`). `0x1972d50` asserts with the same functor type (`0x1972d9c`) and tail-jumps to
`0x19729c0` (`0x1972de4`), which loads `esi = [r12]` (`0x1972a9c`), calls SetDate (`0x1972acd`), copies
the variant out (`0x1972ae3`) and destroys the Command (`0x1972aeb call 0x15d8f30`) without Add. That
the table is `api.cmd.make` is INFERRED (the `make` string; the table path was not traced). For
`setGameSpeed` and `setCalendarSpeed` the key the closure is finally stored under was not traced, hence
INFERRED. How a Lua `CmdData` becomes a sent Command (a Command is built without the packager in
`0x197a6f0`, ctor call `0x197aca5`) was not traced either. The Windows note "setDate/setCalendarSpeed
inline" does not carry over: on Linux all three makers call the factory.

**Replays and the four acted-on addresses (corrected).** The mod replays through `game.interface.setDate`
/ `setMillisPerDay` (pacing.lua `CM.execCalendar`, registered in `0x1dc7a00`, not a caller of any of the
three factories) and `api.cmd.make.setGameSpeed` (pacing.lua `CM.setSpeed`, returns to `0x1950f23`).
The replay's own factory call never returns to `0xf6b88e`, `0xf6c28d`, `0xf6bce5` or `0xf70410` (PROVEN,
the caller lists above). But **a replayed or paced engine speed can come back as a new SetGameSpeed at
`0xf6b88e`**: `UI::Clock::DoStep` re-clicks the selected speed button with emit whenever
`m_commandCount == 0`, `speeds[sel] != GetSpeed()` and `0 < max < GetSpeed() <= 4`, where `max` is this
machine's run-time performance cap (§3 "The clamp", PROVEN). A follower whose cap is below the session
speed, or a cap that drops while the game runs, therefore issues SetGameSpeed through `0xf6b88e` with
no player input. The capture must tell it apart from a click (§7.7). The calendar replays have no
automatic echo: the spin boxes are set with emit only by the DateWindow click lambda when a player opens
the window (§4), and the slider is never set after its handler is connected (§5).

### The third Windows clock site

Windows filters three clock return addresses (`0x4efb8f`, `0x4f0097`, `0x4f26ef`). On Linux the
functions that load Clock.cpp's `__FILE__`/signature strings lie at `0xf6af90..0xf6e440`, and the Clock
code identified here (ctor `0xf6ec60`, DateWindow lambda `0xf6fe90`, slider handler `0xf70320`) sits
next to them. The nearest other source files are CampaignComp.cpp (last string at `0xf60880`) and
ConstructionList.cpp (first at `0xf715b0`). That whole span `0xf60880..0xf715b0` contains exactly two
SetGameSpeed calls (`0xf6b889`, `0xf6c288`) and four `0x15da840` calls (`0xf6b8ac`, `0xf6bd03`,
`0xf6c2ab`, `0xf7042a`). The whole binary has no other SetGameSpeed call that could be a clock site
(the table above). So the Linux filter has two SetGameSpeed addresses, not three (PROVEN). Which
Windows function `0x4f26ef` belonged to is not known (INFERRED: a separately emitted copy of the button
lambda or of TogglePause).

## 3. `UI::Clock`: speed buttons, pause, DoStep

### Layout (sizeof 0x4d0: `0x1002d40 mov edi,0x4d0` then the ctor call `0x1002d78`)

| offset | field | evidence | status |
|---|---|---|---|
| +0x440 | `CommandList*` | `0xf6c260 mov r14,[rdi+0x440]` → Add `rsi` (`0xf6c29b`) | PROVEN use |
| +0x448 | `GameTimePtr` | DoStep `0xf6c4e2 lea r13,[rdi+0x448]` → `0x1477130` → `0xc0dc30` (CGameTime::GetSpeed, speedhook_linux.cpp) | PROVEN use |
| +0x450 | `std::function<int()>` max speed (manager +0x460, invoker +0x468) | `0xf6b78d cmp [rax+0x460],0`, `0xf6b7a2 call [rax+0x468]` with `rdi=rax+0x450`; DoStep `0xf6c605`/`0xf6c61a`; source below | PROVEN |
| +0x470 | int `m_lastNonZeroSpeedup` | `0xf6c1f6 mov edx,[rdi+0x470]`, assert string `m_lastNonZeroSpeedup > 0` (`0xf6c389`); DoStep writes the engine speed there when > 0 (`0xf6c594`); ctor sets 1 | PROVEN |
| +0x474 | int `sel`, the selected non-pause button | ctor `0xf6ed82 movabs rax,0x100000001` / `0xf6ed8c mov [r12+0x470],rax` (sel = 1); lambda `0xf6b7f9` (index ≥ 1 only); DoStep `0xf6c59a`, `0xf6c65d`, `0xf6c67b` | PROVEN |
| +0x478 | int **`m_commandCount`** | callback assert `m_commandCount > 0` (`0xf6afbf`), DoStep assert `m_commandCount >= 0` (`0xf6c75b`) | PROVEN |
| +0x47c | int speeds[4] = {0, 1, min(max,2), min(max,4)} | `0xf6b7bc mov qword [rdx+0x47c],0x100000000`, `0xf6b7ce` [+0x484], `0xf6b7dc` [+0x488] | PROVEN |
| +0x490, +0x498, +0x4a0 | `DoubleSpinBox*` day, month, year of the date picker | out-params of `CreateDateModifierLayout` (§4); `0xf701c2`/`0xf7020c`/`0xf70256` set day/month/year | PROVEN use |
| +0x4a8 | date label | DoStep `0xf6c500` → `0x311ac20` | PROVEN use |
| +0x4b0 | button pointers [4], one per speeds[i] (i = 0 is the pause button, speed 0) | ctor `0xf6f0ae mov r15,[r12+r13*8+0x4b0]` for r13 = 0..3; TogglePause tests `[buttons[0]+0x440]`; the checked state is the byte `[button+0x440]` (below) | PROVEN use (class name INFERRED) |

The ctor (`0xf6ec60`, `Clock(CommandList& rsi, GameTimePtr rdx, WindowContainer* rcx,
std::function<int()>* r8, bool r9b, const string& [rbp+0x10])`; types from the signature string the
inner lambda asserts with, registers from `0xf6ec74`..`0xf6ec9e`) zeroes +0x478..+0x47f
(`0xf6ed27 mov qword [r12+0x478],0`). Its only direct caller is `0xffae10` (loads
`void UI::CGameUI::CreateUI(...)`'s signature, GameUI.cpp) at `0x1002d78`, with
`r9d = byte [CGameUI+0x508]` (`0x1002cd6`, `0x1002d55`).

### The max-speed function (PROVEN)

`CGameUI::CreateUI` builds the `std::function<int()>` it passes in `r8` on its stack at `[rbp-0x8c0]`:
functor `[CGameUI+0x448]` (`0x1002d11`, `0x1002d26`), manager `0xfd9b00` (`0x1002cfd lea`, `0x1002d2d`),
invoker `0xfde6f0` (`0x1002cde lea`, `0x1002cef`), `0x1002d4e lea rbx,[rbp-0x8c0]`, `0x1002d6c mov
r8,rbx`. `CGameUI+0x448` is the CGameUI ctor's `CGame*` (`0x1012f0d` saves `rdx`, `0x1013072 mov
[r15+0x448],rax`; the ctor's signature string at `0x1018d6f` lists `CGame*` second). The manager clones
one qword (`0xfd9b12`).

- `0xfde6f0`: `v = 0xa2e420(CGame*)`; unless `v > 0.0f` (`0xfde70a comiss` against `0x3e8be6c` = 0.0f)
  return 0; otherwise `v` again (`0xfde723`), floored (below 2^23, `0x3e8bdd0`; minus 1.0f `0x3e8bdc8`
  when the truncation is above), `cvttss2si`, and `cmovle eax,1` (`0xfde780`). So **max = 0 if v <= 0,
  else max(1, floor(v))**.
- `0xa2e420`: `m = [CGame+0x160]` (CGame `m_data`, the offset speedhook_linux.cpp uses). Two
  `std::deque<int>` (0x200-byte nodes): A with start `m+0x118..+0x130` and finish `m+0x138..+0x150`,
  B with start `m+0x168..+0x180` and finish `m+0x188..+0x1a0`. If A holds 9 entries or fewer it returns
  0.0f (`0xa2e47b cmp rsi,9; jbe 0xa2e570`). Otherwise it returns
  `(float)(350000 - avgA) / (float)(avgA + avgB)` (`0xa2e518 mov edx,0x55730`, `0xa2e525 sub edx,r8d`,
  `0xa2e564 add eax,r8d`, `0xa2e56b divss`).

So `max` is a run-time measurement of this machine, not a constant: it can be 0 (not enough samples)
or any value from 1 up, including 1..3. What the two sample queues hold is INFERRED (timings).

### The buttons

Wiring (ctor, PROVEN): for i = 0..3 (`0xf6f181 add r13,1; 0xf6f191 cmp r13,4`) a 0x18-byte closure
`{Clock* @0 (0xf6f112), CommandList* @8 (0xf6f11c), int i @0x10 (0xf6f118)}` goes into a
`std::function<void(bool)>` at `[rbp-0x160]` (`rbx`, `0xf6ee96`) with invoker `0xf6b750` (`0xf6f12a`,
`[rbp-0x148]`) and manager `0xf6b010` (`0xf6f138`, `[rbp-0x150]`), and `0x3126330(rdi=result,
rsi=buttons[i] (0xf6f0f3, 0xf6f120), rdx=rbx)` (`0xf6f157`) connects it. `0x3126330` does
`add rsi,0x450` (`0x3126335`), moves the std::function out of `rdx` (`0x3126365`..`0x312638b`) and
connects it to that signal (`0x312638f call 0xdcdfb0`). So the lambda is a slot of **signal
`button+0x450`**.

The lambda `0xf6b750` (PROVEN, `rdi` = `_Any_data&`, `rsi` = `bool&`):

```
0xf6b777 cmp byte [rsi],0 ; je return            only on "checked"
0xf6b780 mov rbx,[rdi]                           closure
0xf6b789 test [rbx+0x10] ; jle 0xf6b805          index 0 (pause) skips the next block
0xf6b7a2 call [Clock+0x468]                      max speed; if > 0 rebuild speeds[]
0xf6b7ed mov [Clock+0x470], speeds[index]        m_lastNonZeroSpeedup
0xf6b7f9 mov [Clock+0x474], index                sel
0xf6b81c call 0x31263e0(button[j], 0, 0)         uncheck the other three
0xf6b85d [rbp-0x48] = 0xf6af90                   done.invoker
0xf6b86b [rbp-0x50] = 0xf6ae90                   done.manager
0xf6b885 [rbp-0x60] = Clock*                     done functor (local storage)
0xf6b87e mov esi,[Clock+index*4+0x47c]           speed = speeds[index]
0xf6b889 call SetGameSpeed(rdi=r12=[rbp-0xa0], esi)
0xf6b8ac call 0x15da840(rdi=r15=[rbp-0xb8], rsi=r14=[closure+8], rdx=r12, rcx=r13=[rbp-0x60], r8=[rbp-0xb0])
0xf6b8b4 call 0x3190430(r15) ; 0xf6b8bc call 0x15d8f30(r12) ; manager op 3 ; weak release
0xf6b90b add dword [Clock+0x478], 1              m_commandCount++ AFTER Add returned
```

The callback `0xf6af90` (27 bytes, PROVEN):
`f3 0f 1e fa 48 8b 17 8b 82 78 04 00 00 85 c0 7e 0a 83 e8 01 89 82 78 04 00 00 c3`:
`rdx = *functor` (Clock*); if `[rdx+0x478] > 0` decrement it, else assert `m_commandCount > 0`
(`0xf6afab`..`0xf6afc9`). The manager `0xf6ae90` clones by copying one qword (op 2, `0xf6aea2`), so the
functor is the 8-byte `Clock*` in local storage.

### Checking a button emits the signal, synchronously (PROVEN)

- `0xf6c0d0(Clock* this, button, bool checked, bool emit)`: saves `checked` (`0xf6c0da mov r15d,edx`)
  and `emit` (`0xf6c0ec mov [rbp-0x74],ecx`). If checked, it restyles the four buttons (`0x30550d0` over
  `+0x4b0..+0x4d0`, `0xf6c121`..`0xf6c15c`) and this one (`"hide-keybinding-hint-display"`, `0xf6c15e`,
  `0xf6c174 call 0x3055200`). Then it calls `0x31263e0(button, sil=checked, dl=emit)`
  (`0xf6c18b`..`0xf6c197`).
- `0x31263e0`: returns at once when the state is unchanged (`0x31263e4 cmp [rdi+0x440],sil`,
  `0x31263eb je 0x3126440 ret`). Otherwise it stores it (`0x31263fe`), sets widget flag 10 (`0x312640a
  call 0x3058b50`) and, if `emit` (`0x312640f test r12b`), tail-jumps to `0xdd10b0` with
  `rdi = button+0x450`, `esi = new state` (`0x3126420`..`0x3126432`).
- `0xdd10b0` jumps to `0xdd08f0` (`0xdd10c9`), `boost::signals2::detail::signal_impl<...>::operator()`
  (its assert signature at `0xdd0de0`). That function locks (`0xdd0959 pthread_mutex_lock`) and calls
  each slot in place: `0xdd0c89 call [rax+8]` with `rdi` = the slot's function object (`0xdd0c85 add
  rdi,0x20`) and `esi` = the bool (`0xdd0c6a`..`0xdd0c71`). No queue: the lambda runs before
  `0xf6c0d0` returns, on the emitting thread.

Every call of `0xf6c0d0` (the `e8` scan plus tail jumps):

| site | function | checked, emit |
|---|---|---|
| `0xf6c33a`, `0xf6c36a` | TogglePause | 1, 1 |
| `0xf6c3e5`, `0xf6c3fd` (tail `jmp`) | speed cycler `0xf6c3b0` (button 1 → 2 → 3 → 1) | 1, 1 |
| `0xf6c5fa`, `0xf6c6de`, `0xf6c705` | DoStep | 0, 0 |
| `0xf6c6bd`, `0xf6c739` | DoStep | 1, 0 |
| `0xf6c676` | DoStep (clamp) | 0, 1 |
| **`0xf6c697`** | **DoStep (clamp)** | **1, 1** |

The TogglePause and cycler callers are input: the Clock's input action handler `0xf6c410` (tail
`0xf6c49b` → TogglePause, `0xf6c4a3` → cycler; built by `0xf6b6b0`, invoker `0xf6c410`, manager
`0xf6b550`, registered with `UI::CComponent::SetInputActionHandler` at `0xf6f1c8`) and the key commands
in game_ui_key_cmd.cpp (`0x1305ae0` → TogglePause `0x1305bbf`; `0x13059a0` → cycler `0x1305a7f`; both
`downcast<UI::Clock*>`). The other `0x31263e0` calls in the Clock code pass `emit = 0` (`0xf632ae`,
`0xf6b81c`).

So SetGameSpeed returns to `0xf6b88e` on four routes: a click on a button, TogglePause's re-check,
the cycler, and **DoStep's clamp**. The first three are player input; the clamp is not.

### TogglePause `0xf6c1e0` (PROVEN)

```
0xf6c1f6 edx = [this+0x470] ; jle -> assert "m_lastNonZeroSpeedup > 0"
0xf6c21a cmp byte [buttons[0]+0x440],0 ; je 0xf6c330 -> 0xf6c0d0(this, buttons[0], 1, 1)       pause
0xf6c230.. loop: speeds[i] == edx ? -> 0xf6c358 -> 0xf6c0d0(this, buttons[i], 1, 1)            resume
otherwise (no button carries the last speed):
0xf6c25e mov esi,edx ; 0xf6c275 [rbp-0x40]=0 (done.manager = null: EMPTY callback)
0xf6c288 call SetGameSpeed(rdi=rbx=[rbp-0x90], esi)
0xf6c2ab call 0x15da840(rdi=r13=[rbp-0xa8], rsi=[this+0x440], rdx=rbx, rcx=r12=[rbp-0x50], r8=[rbp-0xa0])
0xf6c2b3 call 0x3190430 ; 0xf6c2bb call 0x15d8f30 ; no counter change
```

The usual pause and resume arrive at `0xf6b88e` (through `0xf6c0d0` → signal → lambda, above), and
`0xf6c28d` only when the engine runs a speed no button shows (for example 3 from pacing). Its only
callers are the input handler and the key command above.

### DoStep `0xf6c4b0` (PROVEN)

`virtual void UI::Clock::DoStep(long long int, long long int)` (signature string loaded at `0xf6c748`).

```
0xf6c4c5 edx = [this+0x478] ; 0xf6c4dc js 0xf6c748 -> assert "m_commandCount >= 0" (throws, below)
0xf6c4e2..0xf6c538  date label text (runs on every DoStep)
0xf6c53f eax = [this+0x478] ; test ; jle 0xf6c570
0xf6c549..0xf6c56a  return                        <- while m_commandCount > 0, nothing below runs
0xf6c57b GetSpeed ; > 0 -> [this+0x470] = speed (0xf6c594)
0xf6c59a r12d = speeds[sel] ; 0xf6c5bc == GetSpeed ? -> 0xf6c720: 0xf6c0d0(this, buttons[sel], 1, 0) ; return
0xf6c5c5..0xf6c603  for i = 0..3: speeds[i] == GetSpeed ? -> 0xf6c6a8: check buttons[i] (1,0),
                                   uncheck buttons[i+1..3] (0,0), -> 0xf6c605
                                 : uncheck buttons[i] (0,0) (0xf6c5fa)
0xf6c605 max std::function empty -> throw bad_function_call (0xf6c767)
0xf6c61a max = call [this+0x468] ; 0xf6c625 max <= 0 -> return
0xf6c63b GetSpeed <= max -> return (0xf6c63e jle)
0xf6c654 GetSpeed > 4    -> return (0xf6c657 jg)
0xf6c676 0xf6c0d0(this, buttons[sel], 0, 1)
0xf6c697 0xf6c0d0(this, buttons[sel], 1, 1)
0xf6c69c return
```

So the button highlight and `m_lastNonZeroSpeedup` follow the engine only while `m_commandCount == 0`.

**The clamp.** When `m_commandCount == 0`, `speeds[sel] != GetSpeed()` and `0 < max < GetSpeed() <= 4`,
DoStep re-checks the selected button with emit. On this path `buttons[sel]` is always unchecked when
`0xf6c676` runs: the loop has unchecked every button except a matching `i`, and `i != sel` because
`speeds[sel]` did not match. So `0xf6c676` changes nothing and emits nothing (`0x31263e4`), and
`0xf6c697` flips it to checked and emits `true`. The lambda runs synchronously, calls `max` again,
rebuilds `speeds[]`, sets `m_lastNonZeroSpeedup` and `sel`, issues **SetGameSpeed(speeds[sel]) through
`0xf6b889`, returning to `0xf6b88e`**, calls Add and increments `m_commandCount`, all before DoStep
returns. `sel` starts at 1 and only the lambda's index ≥ 1 block writes it, so `speeds[sel]` is
1, `min(max,2)` or `min(max,4)`: the clamp lowers the speed to what the selected button offers here
and never pauses. In the stock game this pulls an engine speed set by something other than the clock
(a save, the Lua API) down to this machine's cap; the command lands and DoStep stops clamping.

In a session the engine speed comes from pacing (`CM.setSpeed` → `api.cmd.make.setGameSpeed`, integer
lever up to `CM.MAX_SPEED` = 4). On an instance whose `max` is below the session speed and whose selected
button does not show that speed, DoStep clamps without any player input as soon as `m_commandCount` is
0. The bridge does not change what DoStep reads: its GetSpeed entry hook returns the engine's value
unchanged (`speedhook_linux.cpp` `GetSpeedDetour` returns `real`), and fractional pacing changes the
batch interval, not the lever.

Every Clock-TU use of +0x478 is listed here: `0xf6af97`/`0xf6afa4` (callback), `0xf6b90b` (++),
`0xf6c4c5`/`0xf6c53f` (DoStep), `0xf6ed27` (ctor), `0xf6f810`/`0xf6ffa2` (address passed to the date
picker, §4). The other +0x478 operands in the scanned range are in functions without Clock strings
(`0xf66900`, `0xf66c50`, `0xf67cf0` inline `ecs::Engine::GetComponentDataIndex`; `0xf72c50`..`0xf7f4c0`
are past ConstructionList.cpp's first string) and use it as a qword or as `this`: not this field
(INFERRED). None of this is locked (see §11 on threads).

### A failed game assertion throws (PROVEN)

`0x2fcb860` → `0x2fcb5e0` prints ``Assertion `...' failed.`` (`0x2fcb6e4`, `0x2fcb714`) and reaches
`0x2fcbb20` (`0x2fcb826`), which calls `__cxa_allocate_exception` (`0x2fcbb3f`) and `__cxa_throw`
(`0x2fcbbc7`). A callback fired while `m_commandCount == 0` throws a C++ exception through the UI, and
so does a DoStep entered with a negative counter (`0xf6c762`).

## 4. The editor date picker (SetDate)

`UI::{anonymous}::CreateDateModifierLayout(CommandList&, UI::GameTimePtr, int&, bool, bool,
UI::DoubleSpinBox*&, UI::DoubleSpinBox*&, UI::DoubleSpinBox*&)` is `0xf6c780` (PROVEN: it is the only
function that takes the address of the three invoker thunks of that signature's inner lambda). Its
SysV frame: `rdi` return slot, `rsi` CommandList&, `rdx` GameTimePtr, `rcx` **int&**, `r8b`, `r9b`,
`[rbp+0x10/0x18/0x20]` the three `DoubleSpinBox*&` (`0xf6c7a5`..`0xf6c7db`). Its two callers are the
Clock ctor (`0xf6f81f`) and the DateWindow click lambda (`0xf6ffbe`).

Both callers pass `rcx = &Clock->m_commandCount` (PROVEN): the Clock ctor
`0xf6f810 lea rcx,[r12+0x478]` (under `0xf6f7ca cmp byte [rbp-0x25c],0`, the ctor's bool, called with
`r8d = r9d = 1`) and the click lambda `0xf6fe90` (the ctor connects it with `0x3028dd0` at `0xf6f627`,
closure `{Clock* @0, ..., CommandList* @0x10, GameTimePtr @0x18, bool @0x20, widget @0x28}` at
`0xf6f5d0`..`0xf6f5fe`; it builds or re-shows the date window, so "DateWindow lambda" below, a name
that is INFERRED) `0xf6ffa2 lea rcx,[rax+0x478]` with `rax = [closure]` (under `0xf6ff73 cmp byte
[r12+0x20],0`, the same bool copied into its closure at `0xf6f5fe`; called with `r8d = r9d = 0`). The
out-params are Clock+0x490, +0x498, +0x4a0; they are written when `r9b` is set (`0xf6cec6` →
`0xf6d8f0`..`0xf6d90b`), so only the ctor's call stores them. So the date picker shares the clock's
pending-command counter.

Where the bool comes from (data flow PROVEN, meaning UNPROVEN): `r9d = byte [CGameUI+0x508]`
(`0x1002cd6` → `[rbp-0x1318]` → `0x1002d55`). The CGameUI ctor copies qword `[Config+0]` of its last
argument `UI::CGameUI::Config` there (`0x1012fe9 mov rax,[rbp+0xa8]`, `0x101323e mov rdx,[rcx]`,
`0x101324f mov [r15+0x508],rdx`). Its only construction site is `UI::CMenuUI::StartGame(std::unique_ptr<CGame>,
bool, const string&, IProgressMonitor&)` `0x115b000` (assert `!m_game` `0x115cc2d`), which builds Config
at `[rbp-0x80]` (`0x115c0e3 lea r14,[rbp-0x80]`, pushed first at `0x115c3b9`, call `0x115c45c`). Byte 0 is
StartGame's bool when that bool is set (`0x115b01f` saves `edx`; `0x115c0c6 jne 0x115ca28`; `0x115ca43`
→ `0x115c0e0 mov [rbp-0x80],al`) and otherwise `byte [[[[CMenuUI+0x4c0]+0x148]+0x10]+0x95b]`
(`0x115c09c`, `0x115c0cc`..`0x115c0e0`). StartGame's only caller is the
`EnterGameAsynchronously(bool, ...)` inner lambda (`0x115d079`, `edx = byte [closure+0x18]`), reached
from the "New Game" path (`0x1159aa0`, `esi = byte [rbp-0x48c]`) and the "Loading..." path (`0x115a3f3`,
`esi = byte [rbx+0xd8]`). Whether that means editor or sandbox was not established. Nothing in the
patch code depends on it: the captures filter on return addresses.

Wiring (PROVEN): three 0x28-byte closures `{int* count @0, CommandList* @8, day box @0x10, month box
@0x18, year box @0x20}` (`0xf6cef5`..`0xf6cf28` and twice more) with invokers `0xf6c0a0`, `0xf6c0b0`,
`0xf6c0c0` (12-byte thunks ending `jmp 0xf6b9b0`) and managers `0xf6b0a0`, `0xf6b130`, `0xf6b1c0`,
connected by `0x30a5ff0` (`0xf6cf53`, `0xf6cff7`, `0xf6d094`) to signal `box+0x498` (`0x30a5ff5 add
rsi,0x498`). Every change to any of the three spin boxes issues a SetDate.

The lambda `0xf6b9b0` (PROVEN, `rdi` = closure):

```
0xf6b9da month = trunc([[rdi+0x18]+0x450])   0 or > 12 -> bad month (0xa6dff0)
0xf6b9fc year  = trunc([[rdi+0x20]+0x450])   <= 1399 or > 9999 -> bad year (0xa6ddb0, 0x578)
0xf6bb9b day   = trunc([[rdi+0x10]+0x450])   clamped to the month's last day (0xf6bba6 cmova), 0 or > 31 -> bad day
0xf6bc0f..0xf6bc8c  a = (14-m)/12; y = year+4800-a (0x12c0); m' = m+12a-3;
                    JDN = day + (153m'+2)/5 (0x99, *0xcccccccd>>34) + 365y (0x16d) + y/4 - y/100 + y/400 - 32045 (0x7d2d)
                    -> esi                                            (boost::gregorian day_number)
0xf6bc94..          "Day of month is not valid for year" -> std::out_of_range (0xf6bf32)
0xf6bcc6 [rbp-0x60] = [rbx] (int* count) ; 0xf6bcd1 [rbp-0x48] = 0xf6afd0 ; 0xf6bcdc [rbp-0x50] = 0xf6ae50
0xf6bce0 call SetDate(rdi=r12=[rbp-0xb0], esi=JDN)
0xf6bd03 call 0x15da840(rdi=r14=[rbp-0xc8], rsi=r15=[closure+8], rdx=r12, rcx=r13=[rbp-0x60], r8=[rbp-0xc0])
0xf6bd0b call 0x3190430 ; 0xf6bd13 call 0x15d8f30
0xf6bd63 add dword [[rbx]], 1                  (*count)++ AFTER Add returned
```

`esi` is not written between `0xf6bc8c` and the call (the month-length checks use eax/edx/r9 only).
The value range is 2232400 (1400-01-01) to 5373484 (9999-12-31). The callback `0xf6afd0` (19 bytes:
`f3 0f 1e fa 48 8b 17 8b 02 85 c0 7e 06 83 e8 01 89 02 c3`) decrements `*count` or asserts
`commandCount > 0` (`0xf6afe3`..`0xf6b001`).

Reopening the DateWindow can issue SetDate by itself (mechanism PROVEN, trigger INFERRED):
`0x30a60a0(box, bool emit (esi→r13b), double value)` (INFERRED name `DoubleSpinBox::SetValue`) returns
early when the value is unchanged (`0x30a6158 ucomisd [rbx+0x450]`), otherwise stores it and, if `emit`
(`0x30a6240`), fires the `+0x498` signal (`0x30a6292`). Its only calls in the Clock code are the click
lambda's three, all with `emit = 1` (`0xf70201`, `0xf7024b`, `0xf70298`; `esi = 1` at `0xf701d2`,
`0xf7021c`, `0xf70266`), setting day, month and year from the game's date on its path that shows an
existing window. Once the game date has moved on (for example after a `SETDATE` replay), that gives up to
three SetDate commands through `0xf6bce5`, and intermediate ones can combine a new day with the old
month. The stock game does the same. DoStep never touches the spin boxes.

## 5. The editor date speed slider (SetCalendarSpeed)

`std::unique_ptr<UI::CBoxLayout> UI::{anonymous}::MakeDateSpeedLayout(CommandList&, UI::GameTimePtr,
const string&)` `0xf6e440` (PROVEN, its own signature string at `0xf6eb01`), called from the Clock ctor
(`0xf6f76e`, building "DateWindow" `0xf6f747`) and from the DateWindow click lambda (`0xf6ff12`).

- `calSpeedFactors` = {0.0, 0.25, 0.5, 1.0, 2.0, 4.0}: 24 bytes from `.rodata` `0x40d0200`
  (`0xf6e554`, `0xf6e55b`) copied into a vector; assert `it != calSpeedFactors.end()` at `0xf6eb14`.
- The slider is created and set up (`0xf6e5cd`..`0xf6e611`) before its handler exists: the handler
  closure (0x28 B, `0xf6e7cd`) `{CommandList* @0 (0xf6e7fa), text target @8 (0xf6e7e0), vector<float>
  factors @0x10..0x28}` goes into a `std::function` with invoker `0xf70320` (`0xf6e80b`) and manager
  `0xf6b400` (`0xf6e816`) and is connected by `0x30223e0` at `0xf6e832`. The DateWindow click lambda
  calls no slider function (its calls are listed in the scratch disassembly; the layout it adds comes
  from `MakeDateSpeedLayout`). So SetCalendarSpeed through `0xf70410` comes only from the slider's own
  signal (that connecting does not fire the handler is INFERRED).

The handler `0xf70320` (PROVEN, `rdi` = `_Any_data&`, `rsi` = `int&` stop index):

```
0xf7035c factor = factors[[rsi]]
0xf7036c CalSpeedFactorText(factor) -> 0xf70377 set on [closure+8]
0xf70396 xor esi,esi ; 0xf703ae [rbp-0x40]=0 (done.manager = null: EMPTY callback)
0xf703a7 comiss factor, 0.0f (0x3e8be6c) ; jbe 0xf70401            factor <= 0 -> 0
0xf703d7 2000 (int, .rodata 0x3fb4a68) / factor + 0.5 (0x3e8bdcc) ; floor (0xf704a0) ; 0xf703fd cvttss2si esi
0xf7040b call SetCalendarSpeed(rdi=r12=[rbp-0x90], esi)
0xf7042a call 0x15da840(rdi=r13=[rbp-0xa8], rsi=r14=[closure+0], rdx=r12, rcx=rbx=[rbp-0x50], r8=[rbp-0xa0])
0xf70432 call 0x3190430 ; 0xf7043a call 0x15d8f30 ; no counter
```

Values the slider can send: stops 0..5 → 0, 8000, 4000, 2000, 1000, 500 ms per day.

## 6. The Command and the Add call at these four sites

### `Command` (0x38 B, PROVEN)

- ctor `0x15d8e60` zeroes +0x00..+0x28 (qwords) and the byte +0x30.
- +0x00: `CmdData*`, a heap block of 0xd50 (packager `0x15eb58d`/`0x15eb5a2`). **Variant index =
  `*(uint8_t*)(*(CmdData**)cmd + 0xd48)`**: 0 SetGameSpeed, 1 SetCalendarSpeed, 26 SetDate. The dtor
  reads it there (`0x15d8f7e movzx eax,[rbx+0xd48]`, visitor table `0x59be2a0`) and frees 0xd50
  (`0x15d8f9d`). The Lua maker reads the same byte (`0x1950f2e`).
- +0x08: a heap buffer the dtor frees (`0x15d8f68`) (INFERRED: a vector's begin, +0x08..+0x18).
- +0x20/+0x28: `{ptr, control}` released with the weak count (`0x15d8f58 lock xadd [rdi+0xc],-1`,
  `call [vtbl+0x18]`); Add moves the caller's `r8` pair in (`0x15da87c`..`0x15da87f`) (INFERRED: a
  `std::weak_ptr`).
- The caller destroys its Command with `0x15d8f30` right after Add at all four sites, so a skipped Add
  leaks nothing.

### `0x15da840` at these sites (PROVEN; that it is `CommandList::Add` is INFERRED, slice-core owns it)

| site | rdi (result slot) | rsi | rdx | rcx (`std::function`) | r8 | rax after |
|---|---|---|---|---|---|---|
| `0xf6b8ac` buttons | `[rbp-0xb8]`, 8 B (`[rbp-0xb0]` follows) | `[closure+8]` | `[rbp-0xa0]` = factory rdi | `[rbp-0x60]`: manager `0xf6ae90`, invoker `0xf6af90`, functor `Clock*` | `[rbp-0xb0]` zeroed | unused (`0xf6b8b1 mov rdi,r15`) |
| `0xf6c2ab` TogglePause | `[rbp-0xa8]`, 8 B | `[this+0x440]` | `[rbp-0x90]` = factory rdi | `[rbp-0x50]`: manager null | `[rbp-0xa0]` zeroed | unused (`0xf6c2b0 mov rdi,r13`) |
| `0xf6bd03` date picker | `[rbp-0xc8]`, 8 B | `[closure+8]` | `[rbp-0xb0]` = factory rdi | `[rbp-0x60]`: manager `0xf6ae50`, invoker `0xf6afd0`, functor `int*` | `[rbp-0xc0]` zeroed | unused (`0xf6bd08 mov rdi,r14`) |
| `0xf7042a` slider | `[rbp-0xa8]`, 8 B | `[closure+0]` | `[rbp-0x90]` = factory rdi | `[rbp-0x50]`: manager null | `[rbp-0xa0]` zeroed | unused (`0xf7042f mov rdi,r13`) |

- `Add.rdx == factory.rdi` names the command at all four sites (PROVEN), as for the lines area.
- `std::function` is libstdc++'s 32 B: functor storage +0x00..+0x0f, `_M_manager` +0x10, `_M_invoker`
  +0x18 (PROVEN at the button and date sites by the stores above and by the manager's op-2 copy of one
  qword).
- **The result slot.** It is never written before the call at any of the four sites (each slot's only
  writer is Add), and the caller passes it straight to `0x3190430`, which is null-safe:
  `0x319043d mov rbx,[rdi]; 0x3190440 test rbx,rbx; 0x3190443 je ret`. Otherwise it releases `[rbx+8]`
  and frees 0x10 bytes. The slot is 8 bytes (the next local starts 8 bytes above it at every site). A
  skipped Add must therefore **write 8 zero bytes at Add's `rdi`** (PROVEN for these four sites; the
  answer to the open "result object" item in SLICE_LINES.md §5/§7 as far as these sites go).

## 7. Cancel rules on Linux

Carried over from Windows (`CaptureSpeedButton`, `CaptureCalendar`):

1. Act only on the exact return addresses `0xf6b88e`, `0xf6c28d` (SetGameSpeed), `0xf6bce5` (SetDate)
   and `0xf70410` (SetCalendarSpeed). Any other caller runs natively; log it once per caller. At
   `0xf6b88e`, rule 7 decides first whether it is DoStep's clamp.
2. Ranges, else run natively: speed 0..64; JDN 1721426..5373484; ms per day 0..10,000,000. (The Linux
   UI can only produce speed ≤ 4 or the engine's current speed, JDN ≥ 2232400, and ms in
   {0,500,1000,2000,4000,8000}; the Windows ranges are kept so both builds refuse the same values.)
3. No live session (`SessionLive`), no instance letter, or the inject line cannot be written: run
   natively.
4. Otherwise cancel fire-and-forget at Add and **never fire the completion callback**. At `0xf6c28d` and
   `0xf70410` the callback is empty. At `0xf6b88e` and `0xf6bce5` it is the counter decrement: fired
   inside Add it would find `m_commandCount` still 0 in the usual case (the increment comes after Add
   returns) and assert, which throws (§3).

New on Linux, required by §3:

5. **Undo the counter increment of a cancelled `0xf6b88e` or `0xf6bce5` command.** The lambda adds 1 to
   Clock+0x478 after Add returns whether or not Add ran. Nothing will decrement it, so `DoStep` returns
   early from then on: the buttons stop following the engine's speed (the clicked button stays lit
   while pacing changes the speed) and `m_lastNonZeroSpeedup` freezes for the rest of the session.
   - Which int: read Add's `rcx` std::function at the landed cancel. Require manager `base+0xf6ae90` and
     invoker `base+0xf6af90` → `*(Clock**)fn + 0x478`; or manager `base+0xf6ae50` and invoker
     `base+0xf6afd0` → `*(int**)fn`. Any other pair: do not touch memory, log it.
   - When: not inside Add (the increment has not happened). Between Add's return and the increment the
     lambdas run only `0x3190430`, `0x15d8f30`, the manager with op 3 (`0xf6ae90`/`0xf6ae50`: no-op for
     op 3) and a weak release of a null control block (PROVEN, `0xf6b8b1`..`0xf6b90b` and
     `0xf6bd08`..`0xf6bd63`). The next reader is `DoStep`.
   - **Hook `UI::Clock::DoStep` at entry** (Option A, recommended). On entry, for each recorded counter
     whose address equals `this+0x478`, decrement it once per recorded cancel while
     `*(int*)(this+0x478) > 0`, before calling the trampoline. Never write it below 0 (DoStep asserts
     `>= 0` and throws); a record whose counter is already 0 stays pending. Store with each record the
     four button pointers `[Clock+0x4b0..+0x4c8]` and apply it only while they still match, so a new
     Clock allocated at the same address never inherits a stale record. Use an atomic or locked record
     (threads: §11). Hook facts (PROVEN): detour `void(UI::Clock* rdi, long long rsi, long long rdx)`;
     verify 27 bytes `f3 0f 1e fa 55 48 89 e5 41 57 41 56 41 55 41 54 53 48 83 ec 48 8b 97 78 04 00 00`
     (the last instruction is `mov edx,[rdi+0x478]`, which re-checks the offset at run time); **steal
     14** (endbr64 4, `push rbp` 1, `mov rbp,rsp` 3, `push r15` 2, `push r14` 2, `push r13` 2: the port
     of `PrologueSteal(code, 14)` returns 14, the menu_linux.cpp check `PrologueSteal(code,14) == steal`
     holds; steal 17 is also an instruction boundary but only `PrologueSteal(code,17)` returns 17). No
     rel32 branch pattern anywhere in `.text` targets `+1..+17`; the only rel8 pattern within reach
     (`73 08` at `0xf6c4aa`) lies inside the instruction `0xf6c4a8 mov rsi,[rbx+8]` of `0xf6c410`, not on
     a boundary. Internal jump targets are ≥ `0xf6c53f` (`0xf6c538 je 0xf6c53f`; `0xf6c4dc js 0xf6c748`).
     The landing pad `0xf6c76c` jumps out to `0x71c87a`. The one stored pointer is `UI::Clock`'s vtable
     slot 34: string `N2UI5ClockE` `0x40d08a0` → typeinfo `0x5a0e1a0` (`R_X86_64_RELATIVE` at
     `0x5a0e1a8`) → vtable slot -1 `0x5a0e250` (offset-to-top 0) → address point `0x5a0e258` → slot 34
     `0x5a0e368` (`R_X86_64_RELATIVE`, the only relocation with addend `0xf6c4b0`; the raw 8-byte hits
     are that relocation at file offset `0x62db90` and the slot at `0x5a0d368`). The patch replaces
     DoStep's `endbr64`, which a vtable call lands on; the verified hooks already reach their detours
     through `jmp [rip]`, so indirect-branch tracking is not enforced in this process (INFERRED from
     those hooks working in the real game).
   - Option B: replace the increments. `0xf6b90b` `83 80 78 04 00 00 01` (7 B) and
     `0xf6bd60`..`0xf6bd65` `48 8b 03 83 00 01` (6 B; `0xf6bd60` is a branch target from `0xf6bd38`,
     `0xf6beac`, `0xf6beb8`, so the patch must start there) redirected through near stubs that skip the
     add for a cancelled command. Two mid-function patches instead of one entry hook, and it cannot tell
     the clamp (rule 7) from a click; not recommended.
6. **Write the inject line only once the cancel has landed.** Windows wrote `SPEEDBTN` / `SETDATE` /
   `CALSPEED` at the factory, before Add, so a cancel that did not land still shipped a line (a speed
   click could move the lever natively and also become the session speed; a date could apply twice).
   These commands are fire-and-forget: once armed the Add hook always suppresses on a pointer match, so
   in practice it lands. Writing from the landed notification avoids the double apply if it ever does
   not.
7. **DoStep's clamp is not a click** (§3 "The clamp").
   - Detection: the DoStep detour keeps a `thread_local` depth counter around the trampoline call,
     restored on unwind (DoStep can throw: `0xf6c762` assert, `0xf6c767` bad_function_call). A
     SetGameSpeed whose return address is `base+0xf6b88e` while that depth is > 0 on the same thread is
     the clamp. Sound because the only call inside DoStep that delivers `checked = true` to a button
     signal is `0xf6c697` (the table in §3) and delivery is synchronous (`0xdd0c89`). That none of
     DoStep's other callees (`0x1477130`, `0xc0cf50`, `0x3206c50`, `0x32081a0`, `0x311ac20` label text,
     `0xc0dc30` GetSpeed, the max function, `0xf6c0d0` with emit 0) processes player input is INFERRED;
     a click cannot be issued from DoStep's frame otherwise.
   - No live session: run it natively (the stock clamp).
   - Live session: cancel it (fire-and-forget, callback never fired, result slot zeroed as rule 4) and
     write **no** inject line. Letting it run is wrong on every instance: pacing owns the lever. On the
     leader, `CM.paceV2` takes a lever move it did not make as the player's choice (pacing.lua:
     `CM.myCeiling = s -- the player set the speed`), which would lower the session speed to this
     machine's cap; on a follower pacing sets the session speed again and DoStep clamps again. Shipping
     it as `SPEEDBTN` is wrong too: it is not a click, and on the leader `CM.speedButton` would make it
     the session speed.
   - Undo its increment **later, not at the next DoStep**: with `m_commandCount` back at 0 and nothing
     changed, DoStep clamps again every frame (one Command built and cancelled per frame). Recommended:
     apply a clamp record at the first DoStep entry at least about one second after the cancel, so the
     clamp repeats at most about once a second while this machine's `max` stays below the session speed.
     While it is pending DoStep returns early and the highlight does not follow the engine; clicks still
     work (their records are applied at the next DoStep). Log clamp cancels rate-limited.
   - If the DoStep hook is refused at run time (bytes differ), rules 5 and 7 are impossible: then do not
     act at `0xf6b88e` and `0xf6bce5` (run natively, log once). A cancelled click would otherwise freeze
     the clock for the session, and a clamp would ship as a click. Native clicks are what pacing already
     handles for "a game without the slice's hooks" (pacing.lua `CM.paceV2`). `0xf6c28d` and `0xf70410`
     do not depend on the DoStep hook.

## 8. Wire lines (must match the Lua)

Appended to the instance's inject file (slice-core owns the path `<data dir>lockstep_inject_<letter>.txt`,
the letter and file access). No `ARMED` line precedes these. Lua references are by text; the line
numbers are the working tree's on 2026-09-12 (lua-linux is editing these files).

| capture | line | Lua reader | notes |
|---|---|---|---|
| `0xf6b88e` (clicks only, not the clamp), `0xf6c28d` | `SPEEDBTN <speed>` (`%d`) | inject.lua `elseif o == "SPEEDBTN" then` (:124-126) → `function CM.speedButton(v)` (pacing.lua :119-142) | the leader's click becomes the session speed (clamped 0..`CM.MAX_SPEED`, floored), a follower's is ignored; during the load gate it is the player's override |
| `0xf6bce5` | `SETDATE <jdn>` (`%d`) | inject.lua `elseif o == "SETDATE" or o == "CALSPEED" then` (:128-136) → `CM.scheduleLocal("SETDATE", {jdn})` → lockstep.lua `elseif c.op == "SETDATE" or c.op == "CALSPEED" then CM.execCalendar(c)` (:642; 588 at HEAD) → `function CM.execCalendar(c)` (pacing.lua :163-210, `function CM.julianToYmd(jdn)` :150) | every instance, the originator included, applies `game.interface.setDate` at the stamp |
| `0xf70410` | `CALSPEED <ms>` (`%d`) | same branch, `{ms}` → `CM.execCalendar` → `game.interface.setMillisPerDay` | 0 is a real setting (pacing.lua `-- 0 is the slider's stopped calendar`, :168) |

All three are exempt from the solo drop (inject.lua, the `o ~= "SPEEDBTN" and o ~= "SETDATE" and
o ~= "CALSPEED"` list, :80). DoStep's clamp writes no line (§7.7).

## 9. Integration notes (outside this area)

- **Hooks (slice-core).** ids 15 (SetGameSpeed), 16 (SetDate), 17 (SetCalendarSpeed), steal 18 each. The
  handler needs `rdi` (Command*), `esi` (value) and the return address `[rsp]` at entry; no stack or SSE
  argument. A relay must preserve every SysV argument register and restore rsp before the trampoline.
  SetGameSpeed is also entered from the Lua maker (`0x1950f23`, INFERRED to run on the script thread),
  so the detour must be thread-safe and cheap on the pass-through path.
- **Pending cancel (slice-core).** `Add.rdx == factory.rdi` (§6). Fire-and-forget (never fire the
  callback). On skip, zero the 8-byte result slot at Add's `rdi` (§6). Slice-time needs a landed
  notification that carries Add's `rcx` (the `std::function*`), to write the inject line (§7.6) and to
  record the counter to undo (§7.5), and it must be able to arm a cancel that writes no line (the clamp,
  §7.7); the line is slice-time's to write, so that needs no extra API if the notification is the only
  writer.
- **Shared services (slice-core).** `SessionLive`, the instance letter (re-read per capture), inject
  append, the log.
- **DoStep.** Slice-time owns the `UI::Clock::DoStep` `0xf6c4b0` entry hook (steal 14); no other area
  should patch it.
- **Pacing (lua-linux, decision for the lead).** §7.7 cancels DoStep's clamp silently on every instance,
  so a machine's performance cap never changes the session speed. If the leader's cap should limit the
  session instead, that is a Lua rule (for example a `SPEEDCAP <max>` line) and not this capture. No Lua
  change is needed for §7 as written.
- **Windows (not editable here).** `slice_hook.cpp` cancels the same two counter sites without firing
  and without undoing the increment, and does not know the clamp. The game logic is the same build, so
  on Windows (INFERRED, not observed): the clock stops following the engine's speed after the first
  cancelled click; and before that, a clamp on an instance whose cap is below the session speed is
  shipped as `SPEEDBTN`, which on the leader lowers the session speed to that cap.

## 10. Differences from Windows

| item | Windows | Linux |
|---|---|---|
| return slot / value | rcx / low 32 bits of rdx | rdi / esi |
| steal | 21 | 18 (factories), 14 (DoStep) |
| clock return addresses | `0x4efb8f`, `0x4f0097`, `0x4f26ef` | `0xf6c28d`, `0xf6b88e` (no third) |
| date picker / slider | `0x4efe54` / `0x4f2af6` | `0xf6bce5` / `0xf70410` |
| Lua makers | `setGameSpeed` → factory `0xc17eff`; `setDate`/`setCalendarSpeed` inline (`0xcee8de`, `0xc17e5e`) | all three call the factory (`0x1950f23`, `0x1950fc1`, `0x1972ad2`); irrelevant to the exact filter |
| Command payload / tag | 0xb18 B, tag at payload+0xb18 | 0xd50 B, tag at payload+0xd48 |
| Add result on skip | zero `*out` | zero the 8-byte slot at `rdi` (null-safe `0x3190430`) |
| counter after a cancelled click | not handled | undone at the next `DoStep` (§7.5) |
| DoStep's clamp while live | captured as a click (`SPEEDBTN`) | cancelled without a line, undone after a delay (§7.7) |
| inject line | written at the factory | written when the cancel landed (§7.6) |

## 11. Not established

- Runtime confirmation: nothing here was observed in the running Linux game.
- That `0x15da840` is `CommandList::Add`, and whether it can run a completion callback synchronously
  (slice-core). The rules above never fire one, so they do not depend on it.
- Which thread runs the completion callbacks, the button lambdas and `DoStep` (INFERRED: one UI thread).
  Statically `m_commandCount` is changed without a lock (`0xf6afa4`, `0xf6b90b`, `0xf6bd63` through
  `int*`, read at `0xf6c4c5`/`0xf6c53f`). The clamp path DoStep → lambda → factory is synchronous on
  DoStep's thread (§3), so clamp detection does not depend on this; the records of §7.5 must still be
  atomic or locked, and applied only from the DoStep detour.
- What CGameUI+0x508 (Config byte 0, StartGame's bool or a settings byte) means, that is, in which games
  the date picker exists (the Windows docs call these the editor's controls).
- What the two sample queues behind `max` measure (§3), so how often a real machine's cap falls below 4.
- That none of DoStep's other callees processes player input (§7.7).
- The Lua table and final key for `0x1950ef0` / `0x1950f90` (`api.cmd.make.setGameSpeed` /
  `setCalendarSpeed`, INFERRED); how a Lua `CmdData` is sent (`0x197a6f0`).
- Whether a no-op SetDate from reopening the DateWindow (§4) should still ship: the current game day
  number is what `0xc0cf50` returns to `0xf701b5` (INFERRED from its use as the input of the day-number
  split `0xa6e110`), but an intermediate mixed date would still differ from it.
- The source function of the Windows clock site `0x4f26ef`.
