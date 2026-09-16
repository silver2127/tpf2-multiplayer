# slice-core on Linux: the map

What `native/linux/src/slice/slice_core*.{h,cpp}` and `slice_install.cpp` stand on: the
`CommandList::Add` hook and its cancel path, how a factory's Command is recognised at Add, the
script-side callers, the libstdc++ layouts the guarded readers decode, and the file and process
facts the skeleton depends on. The Windows reference is `native/src/slice_hook.cpp` (lines
1-500, 3322-3500, 4109-4245) and [../COMMANDS.md](../COMMANDS.md).

Binary: `TransportFever2`, Steam build 35924, x86-64 PIE, GNU build-id
`3a0e156390b0e6f1e372051c24802c8493ae454a`. An RVA is the ELF virtual address; live address =
image base (from `dl_iterate_phdr`) + RVA. Windows RVAs (base `0x140000000`) are quoted only as
cross-references.

| label | meaning |
|---|---|
| PROVEN | reproducible from the file: the bytes, instructions, relocations or strings are quoted |
| INFERRED | follows from evidence plus a stated assumption; kept out of patch code or checked at run time |
| UNPROVEN | open; the code must stay off or self-test |

Method, so every row can be reproduced: function bounds from `functions.csv` (one row per FDE);
capstone linear sweeps from a function start over its FDE size; call sites from a raw scan of
`.text` for `E8`/`E9` rel32 whose target is an FDE start, each confirmed to sit on an instruction
boundary of its containing function; address-taking from a rel32 scan (every 4-byte window,
`target = va + 4 + disp`, confirmed by disassembly) plus the `R_X86_64_RELATIVE` addends in
`.rela.dyn`; names from `funcsig.csv`/`xrefs.csv`, re-checked per function. `funcsig.csv` names
several functions below after an inlined callee (Add itself is listed as a boost
`grouped_list` copy constructor), so no name there is used without the code agreeing.

---

## 1. Build gate  [C-BUILD-1, PROVEN]

`readelf -n` on the game prints `Build ID: 3a0e156390b0e6f1e372051c24802c8493ae454a`, the bytes
`game_image.h` compares (`kTpf2BuildId35924`). The running game reported the same id and a PIE
base (`tpf2_proxy.log`: `image base 0x55dd4d8fc000 build-id 3a0e...454a (build 35924)`). Every
RVA in this file belongs to that id; on any other id the slice must patch nothing.

---

## 2. `CommandList::Add`

Windows: `0x9d2a00`, `? Add(list rcx, Handle* out rdx, Command* r8, std::function* r9)`, steal 18.

### 2.1 Where it is  [C-ADD-1, PROVEN]

**`0x15da840`, 5045 bytes.**

- It is in `CommandList.cpp`'s block. The lambda at `0x15da310` references the assert signature
  `CommandList::Add(Command, std::function<void(const Command&)>, std::weak_ptr<CmdProgress>)::<lambda(const boost::signals2::connection&, const std::vector<Command>&)>`
  (`0x15da66c lea rcx`) and `CommandList.cpp` (`0x15da678`). `CommandList::Swap` follows at
  `0x15dbc00`.
- `0x15da840` installs exactly that lambda as a signals2 slot. `0x15dab9e lea rax,[rip+..]` loads
  `0x59be5f0`, and `.rela.dyn` fills `0x59be5f0 -> 0x15da710` (boost::function manager) and
  `0x59be5f8 -> 0x15da310` (invoker, the lambda).
- Its body is the signature's: it pushes a 0x38-byte Command, moves a `std::function` and moves a
  `weak_ptr` (2.4).
- Each of its 94 call sites passes the Command a factory, `line_util` or a script sink just built
  (Appendix A).

### 2.2 Who calls it  [C-ADD-2, PROVEN]

There are 94 direct `E8` call sites and no `E9` tail jump. No relocation addend equals
`0x15da840`, and no RIP-relative instruction takes its address. An entry hook therefore sees
every call. Appendix A lists them all.

### 2.3 SysV signature  [C-ADD-3, PROVEN]

```
Connection* CommandList::Add(Connection* ret,                               // rdi  hidden return slot, 8 B
                             CommandList* self,                             // rsi
                             Command* cmd,                                  // rdx  by-value Command, passed by reference
                             std::function<void(const Command&)>* done,    // rcx  by value, passed by reference
                             std::weak_ptr<CmdProgress>* progress);        // r8   by value, passed by reference
                                                                            // returns rax = ret
```

Evidence in Add:

- `0x15da85b mov [rbp-0x3d8],rdi` saves `ret`; both exits load `rax` from it (`0x15db061`, and
  the slot path after `0x15db744`).
- `0x15da866 mov [rbp-0x3b8],rsi` saves `self`; `0x15da8c7`/`0x15da8ce` then dereference it to
  `m_data` (2.8).
- `rdx` is the Command: `0x15da862 mov rdi,[rdx+0x28]`, `0x15da87f mov [rdx+0x20],rax`, and
  `0x15da8e1 mov rsi,rdx` into the Command move constructor `0x15d8ea0`.
- `rcx` goes to `rbx` at `0x15da851` and is used as the std::function: `0x15da8ef mov rdx,[rbx+0x10]`
  tests the manager, `0x15da951 movdqu xmm0,[rbx]` and `0x15da955 mov qword [rbx+0x10],0` move it.
- `r8` is moved from: `0x15da87c mov rax,[r8]`, `0x15da8b0 mov rax,[r8+8]`, and
  `0x15da8b4`/`0x15da8bb` zero both words.

A capstone `regs_access` sweep of the entry block finds no read of `r9`, `r10`, `r11`, `rax` or
`xmm0-7` before it is written. The function has no `[rbp+disp]` operand, so it reads no stack
argument, and its first `r9` access is not a read.

The caller side, `0x1444330` (Reverse from the vehicle window):

```
0x1444384  lea rbx,[rbp-0x90]          ; Command slot
0x1444396  call 0x15ebe00              ; make_cmd::Reverse(rdi=rbx, rsi=engine, edx=entity)
0x144439b  lea r13,[rbp-0xa8]          ; Connection slot (8 B, next to the weak_ptr at -0xa0)
0x14443a2  lea r12,[rbp-0x50]          ; std::function (manager at -0x40, zeroed at 0x1444377)
0x14443a6  mov rdx,rbx                 ; cmd == the factory's rdi
0x14443a9  mov rsi,r14                 ; CommandList (r14 = [[rdi]], 0x1444346/0x144435d)
0x14443ac  lea r8,[rbp-0xa0]           ; weak_ptr {0,0}
0x14443b3  mov rcx,r12
0x14443b6  mov rdi,r13
0x14443b9  call 0x15da840
0x14443be  mov rdi,r13 ; call 0x3190430   ; ~Connection
0x14443c6  mov rdi,rbx ; call 0x15d8f30   ; ~Command
0x14443ce  mov rax,[rbp-0x40] ; ... mov edx,3 ; call rax   ; std::function destroy (op 3)
0x14443e4  mov rdi,[rbp-0x98] ; call 0x9e2260             ; weak_ptr release
```

### 2.4 Prologue and steal  [C-ADD-4, PROVEN]

```
0x15da840  f3 0f 1e fa        endbr64
0x15da844  55                 push rbp
0x15da845  48 89 e5           mov  rbp,rsp
0x15da848  41 57              push r15
0x15da84a  41 56              push r14
0x15da84c  41 55              push r13          <- steal ends at 0x15da84e (14 bytes)
0x15da84e  41 54              push r12
0x15da850  53                 push rbx
0x15da851  48 89 cb           mov  rbx,rcx
0x15da854  48 81 ec f8 03 00 00  sub rsp,0x3f8
```

- **Expected bytes:** `f3 0f 1e fa 55 48 89 e5 41 57 41 56 41 55`, **steal 14**.
- `PrologueSteal(code, 14)` (hook_posix.cpp) returns 14, and capstone agrees the boundary is
  there.
- None of the six instructions is RIP-relative or a branch.
- No rel32 or rel8 branch anywhere in `.text`, and no branch inside Add, targets
  `0x15da841..0x15da84d`.

### 2.5 What Add does  [C-ADD-5, PROVEN]

1. `cmd->progress = std::move(*progress)` (Command+0x20/+0x28): `0x15da87c`-`0x15da8c3`.
2. `m_data->commands.push_back(std::move(*cmd))`: `0x15da8ce mov r12,[rax]` loads `m_data`,
   `0x15da8d1`/`0x15da8d6` compare end with cap, `0x15da8e4 call 0x15d8ea0` move-constructs, and
   `0x15da8e9 add qword [r12+8],0x38`. When the vector is full it grows through `0x10524b0`
   (`0x15db080`). No lock is taken on this path.
3. **Empty `done`** (`[rcx+0x10] == 0`, `0x15da8ef`-`0x15da8f6`): jump to `0x15db048`, which runs
   `mov rdi,[rbp-0x3d8]; call 0x3190240` (`*ret = Connection()`) and returns `ret`. No slot is
   connected; the command is still queued.
4. **Non-empty `done`:** it computes `idx = size-1` (`0x15da906`-`0x15da917`), reads the generation
   byte `m_data+0x18` (`0x15da920`) and loads the signal pointer `m_data+0x20` (`0x15da92b`,
   kept in `[rbp-0x3d0]`). If that pointer is null it creates the signal first
   (`0x15da939 je 0x15db0e0`, 2.8). It moves `*done` into a closure
   `{CommandList*, std::function, int idx, u8 gen}` and connects it (slot vtable `0x59be5f0`, 2.1)
   under the signal impl's mutex: `0x15dad1d mov rax,[rbp-0x3d0]`, `0x15dad24 mov rbx,[rax+8]`
   (the impl), `0x15dad31 mov rdi,[rbx+0x18]` (its mutex), `0x15dad5d call pthread_mutex_lock`.
   It returns `Connection(conn)` via `0x15db73d mov rdi,[rbp-0x3d8]; 0x15db744 call 0x3190280`.

Add consumes `*done` and `*progress` by moving from them and leaves `*cmd` moved-from. The caller
always destroys all three afterwards (2.3). **If Add is skipped, nothing leaks and nothing is
freed twice.**

### 2.6 When the game fires `done`  [C-ADD-6, PROVEN; one ordering detail INFERRED]

The slot lambda `0x15da310` has the signature `(function_buffer& rdi, const connection& rsi,
const vector<Command>& rdx)`, and the closure pointer is `[rdi]`.

- `0x15da33a`-`0x15da347`: `cmp byte [ [closure+0] -> m_data + 0x18 ], [closure+0x2c]`. While the
  generation is unchanged it returns and does nothing.
- `0x15da350`-`0x15da378`: asserts `idx >= 0 && idx < (int)commands.size()` (string at
  `0x15da67f`). It then disconnects its own connection (`0x15da37e`-`0x15da4a8`).
- `0x15da4b6`-`0x15da4d4`:
  - `rsi = commands.begin + idx*0x38`;
  - `cmp qword [closure+0x18],0` (the manager), `je __throw_bad_function_call` (`0x15da6f1`);
  - `lea rdi,[closure+8]; call [closure+0x20]`, i.e. `invoker(&fn, &commands[idx])`.

`CommandList::Swap(self, out)` `0x15dbc00` first emits the signal with `out`
(`0x15dbc3d call 0x15e1fe0`), then destroys `out`'s Commands, swaps the vectors and toggles the
generation (`0x15dbca7 xor byte [rax+0x18],1`); 2.8 has the instructions. For a command Added at
generation `g`:

1. **The first Swap after Add** emits while the generation is still `g`, so the slot returns. The
   command then moves into `out` and the generation becomes `g^1`.
2. **Between the two Swaps the batch is handed to the simulation.** `out` is `CGame`'s own vector:
   `CGame::Sync` passes `rdi = [CGame+0x158]` (the CommandList) and `rsi = [CGame+0x160]+0x90`
   (`0xa30efd mov rdi,[r15+0x158]`, `0xa30f04 lea rsi,[rax+0x90]` with `rax = [r15+0x160]`).
   `CGame::RunGameSimLoop` `0xa2ddd0` walks that vector (`0xa2e0b2`/`0xa2e0b9` load begin and end
   of `[CGame+0x160]+0x90`) at stride 0x38 (`0xa2e0de add r15,0x38`) and calls `0x15e2e70` on each
   Command (`0xa2e0e6`); `0x15e2e70` references the string "Simulation Thread: Apply Command"
   (`0x15e2e81`).
3. **The second Swap** emits with that vector while the generation is `g^1`, so the slot fires
   `done(out[idx])`. Swap then destroys the batch.

So `done` runs inside `CGame::Sync` (Swap's only caller, `0xa30f0b`; Sync's only caller is
`CGame::Step`, `0xa31c66`), one Sync after the command left the list, and it receives that batch's
Command. **INFERRED:** that the simulation loop has finished the batch before the second Sync
emits. `Sync` waits on a condition variable (`0xa30d41 pthread_mutex_lock`,
`0xa30ddd pthread_cond_timedwait`), and the handshake was not traced further. Nothing in the
cancel recipe depends on it. (`funcsig.csv` names `0x15e1fe0` after an inlined `auto_buffer`
destructor; its single caller `0x15dbc3d` and its lock of `[impl+0x18]` at `0x15e1ffb`/`0x15e2045`
identify it as the emission.)

### 2.7 Firing `done` from a detour  [C-ADD-7, PROVEN]

`done` is a libstdc++ `std::function` (32 B): `_Any_data` at +0x00, `_M_manager` at +0x10,
`_M_invoker` at +0x18. The invoker is `void(const _Any_data*, const Command*)`. The recipe, taken
from 2.6:

```
if (*(uintptr_t*)(done + 0x10) != 0)                                  // empty -> nothing to fire
    ((void (*)(void*, void*)) *(uintptr_t*)(done + 0x18))(done, cmd); // invoker(&fn, cmd)
```

- Calling it does not consume `fn`; the caller still destroys it.
- On Linux there is no `_Getimpl` indirection. Whether the functor sits inside `_Any_data` or on
  the heap is the invoker's business.
- The Command passed is the one Add received, not yet applied. That is the same thing the Windows
  cancel passed.

### 2.8 `CommandList`  [C-LIST-1, PROVEN]

- `*self` = `m_data`.
- `m_data+0x00` is `std::vector<Command>`.
- `m_data+0x18` is the u8 generation.
- `m_data+0x20` is a **`boost::signals2::signal*`**, a heap object of 0x18 B. It is null until the
  first Add with a non-empty `done` creates it.
  - `+0x00` vptr `0x59be3d8`. Its slot -1 (`0x59be3d0`) is typeinfo `0x5a240a0`, whose name
    (`0x4323ea0`) is
    `N5boost8signals26signalIFvRKSt6vectorI7CommandSaIS3_EEENS0_19optional_last_valueIvEEiSt4lessIiENS_8functionIS8_EENSD_IFvRKNS0_10connectionES7_EEENS0_5mutexEEE`,
    i.e. `boost::signals2::signal<void(const std::vector<Command>&), ..., boost::signals2::mutex>`.
  - `+0x08` the impl (`signal_impl`, a 0x28-byte heap block). `impl+0x18` points to its heap
    mutex.
  - `+0x10` the impl's `shared_ptr` count block (vptr `0x59be440`; typeinfo `0x5a240f0`, name
    `N5boost6detail17sp_counted_impl_pINS_8signals26detail11signal_implI...` at `0x4324000`).
- **The impl is `[[m_data+0x20]+8]`**, not `[m_data+0x20]`.

Evidence, Add's lazy creation (reached from `0x15da939` when `[m_data+0x20]` is null; `r14` =
`m_data`):

- `0x15db0e0 mov edi,0x18; call operator new`; `0x15db0ea lea rcx,[0x59be3d8]`;
  `0x15db0f6 mov [rax],rcx`; `0x15db0f9 mov [rbp-0x3d0],rax` keeps the object.
- `0x15db100 call operator new` (0x28 B) is the impl, kept in `[rbp-0x3f8]` (`0x15db111`).
  `0x15db4bd` allocates 0x28 B more, `0x15db4ca call pthread_mutex_init` initialises it, and
  `0x15db4e3 mov [r15+0x18],r12` stores it in the impl.
- `0x15db535 mov [r15+8],r13` stores the impl in the signal object; `0x15db566 mov [r15+0x10],rax`
  stores the count block, whose vptr `0x59be440` is written at `0x15db55c`/`0x15db563`.
- `0x15db574 mov r12,[r14+0x20]`, then `0x15db57f mov [r14+0x20],rax` publishes the object. A
  previous object, if any, is destroyed (`0x15db588`-`0x15db5aa`, sized delete of 0x18).
  `0x15db5be jmp 0x15da93f` returns to the connect.

Evidence, `CommandList::Swap(rdi = self, rsi = std::vector<Command>& out)` `0x15dbc00`:

- `0x15dbc15 mov rax,[rdi]`, `0x15dbc18 cmp rax,rsi` and `je` implement the assert
  `&commands != &m_data->commands` (string loaded at `0x15dbcd2`).
- `0x15dbc21 mov rax,[rax+0x20]`; `0x15dbc2b test rax,rax; je 0x15dbc42`: without a signal object
  there is no emission.
- `0x15dbc30 mov rdi,[rax+8]`; a null impl goes to `0x15dbcfd call 0x15da090`, the
  `boost::shared_ptr<signal_impl>::operator*` assert.
- `0x15dbc3d call 0x15e1fe0` with `rsi` still `out`: the emission `impl(out)`. `0x15e1fe0` has
  this one caller; it loads `[impl+0x18]` (`0x15e1ffb`) and locks it (`0x15e2045`).
- `0x15dbc42`-`0x15dbc69` destroy `out`'s Commands at stride 0x38 (`call 0x15d8f30`) and set
  `out.end = out.begin`; `0x15dbc72`-`0x15dbca3` swap the two vectors.
- `0x15dbca7 xor byte [rax+0x18],1` toggles the generation, then `0x15dbcab cmp [rax],rsi`
  asserts `m_data->commands.empty()` (string loaded at `0x15dbcf1`).

### 2.9 The Command  [C-CMD-1, PROVEN]

0x38 bytes.

| offset | field | evidence |
|---|---|---|
| +0x00 | `CmdData*`, heap block of 0xd50 B; the variant tag is the u8 at `CmdData+0xd48` (0xff = valueless) | `0x15eb588 mov edi,0xd50; call operator new; 0x15eb59b mov byte [rax+0xd48],0; 0x15eb5a2 mov [rbx],rax`; ~Command `0x15d8f7e movzx eax,byte [rbx+0xd48]` ... `0x15d8f9d mov esi,0xd50; jmp sized delete` |
| +0x08 | a `std::vector` (begin freed by ~Command; move swaps +0x10/+0x18) | `0x15d8f68`-`0x15d8f71`; move ctor `0x15d8ec6`-`0x15d8ef5` |
| +0x20 | `std::weak_ptr<CmdProgress>` {ptr +0x20, ctrl +0x28} | Add `0x15da87f`, `0x15da8c3`; ~Command `0x15d8f40`-`0x15d8f60` (`lock xadd [ctrl+0xc]`) |
| +0x30 | u8 | move ctor `0x15d8f09`-`0x15d8f1d` |

The stride is 0x38: `0x15da8e9` and `0x15dbc5b`. The tag each factory writes is in Appendix B
[C-CMD-2, PROVEN]. The factory writes it at `payload + 0xd48` before the packaging call; e.g.
Reverse sets `payload = rbp-0xd80` and `0x15ebe70 mov byte [rbp-0x38],7`.

**The tag at Add [C-CMD-3, PROVEN].** Once a factory has returned, `[[cmd]+0xd48]` is the tag it
wrote into its payload. The Add detour compares the two before it skips anything:

- The builder `0x15eb570` allocates the CmdData (`0x15eb588 mov edi,0xd50`, `0x15eb58d call new`),
  sets its index to 0 (`0x15eb59b`), stores it in the Command (`0x15eb5a2`). It then calls
  `0x15f1800(CmdData, payload)` with the payload it was given (`0x15eb576 mov r13,rsi`,
  `0x15eb5d4 mov rsi,r13`, `0x15eb5da call`).
- `0x15f1800` compares the two indices (`0x15f1824 movzx eax,[rsi+0xd48]`, `0x15f182b cmp [rdi+0xd48],al`).
  - Equal: it move-assigns through a table and leaves the index as it is.
  - Different: it move-constructs a temporary from the source (`0x15f186a call 0x15f1780`), destroys its
    own alternative, then move-constructs itself from the temporary (`0x15f188d call 0x15f1780`).
- That move constructor `0x15f1780` copies the source index into the destination:
  `0x15f178b movzx eax,[rsi+0xd48]`, then `0x15f17ad movzx eax,[r12+0xd48]` and
  `0x15f17b6 mov [rbx+0xd48],al` after the alternative's move. It leaves 0xff only on its
  exception path (`0x15f18ba`).
- The UI completion invokers test the same byte on the queued command (`0xe867f7 cmp byte [rbx+0xd48],0xf`,
  SLICE_PROPOSAL.md §1).

A mismatch can only stop a cancel, never cause one.

### 2.10 Add's return object: `Connection`  [C-ADD-8, PROVEN]

The object is 8 B: a pointer to a heap 16-byte `boost::signals2::connection` (a `weak_ptr`
{body, `sp_counted_base*`}, weak count at ctrl+0xc).

| RVA | member | callers | evidence |
|---|---|---|---|
| `0x3190240` | `Connection()`: `new {0,0}` | 26 | `0x319024c mov edi,0x10; call operator new; mov [rax],0; mov [rax+8],0; mov [rbx],rax` |
| `0x3190280` | `Connection(const signals2::connection&)` | 61 | copies both words, `lock add [ctrl+0xc],1` |
| `0x31902c0` | copy ctor | 14 | `0x31902d3 mov rbx,[rsi]` then `0x31902db mov rdx,[rbx]`: **no null check** |
| `0x3190300` | move ctor | 59 | steals the pointer, zeroes the source |
| `0x3190320` | copy assign | 0 direct | |
| `0x31903b0` | move assign | 4 | |
| `0x3190430` | `~Connection` | 1660 | `0x319043d mov rbx,[rdi]; 0x3190440 test rbx,rbx; je ret`: null-safe; else weak release + `delete(p, 0x10)` |
| `0x31904b0` | a query/disconnect-style member | 8 | `0x31904bf mov rdx,[rdi]; 0x31904c2 mov rbx,[rdx+8]`: **no null check on `*this`** |

### 2.11 What the callers do with the result  [C-ADD-9, PROVEN]

- **93 of 94** call `0x3190430` on the returned slot as the very next call. Appendix A, "result";
  the heuristic's misses were re-read by hand, e.g. `0xe34882`/`0xe34889 mov [rbp-0x798],r10`,
  then `0xe34895 mov rdi,[rbp-0x798]; call 0x3190430`.
- **`0x10d4b60`** is the exception. Add returns to `0x10d4c34`, and the CreateLine goes through
  `line_util` (§3):
  - it destroys the Command, the `done` and the `weak_ptr`;
  - it copy-constructs the Connection (`0x10d4c80 mov rsi,r13; 0x10d4c86 call 0x31902c0`);
  - it hands the copy to `0x312cc30`, which copies it again with `0x31902c0` at `0x312cc5a` and
    stores it in a vector;
  - it then destroys both Connections (`0x10d4c9a`, `0x10d4ca2`).

### 2.12 Skipping Add: the Linux cancel recipe  [C-ADD-10, PROVEN]

In the typed detour, when the Command is the armed one:

1. Decide on `done` (2.7, C-ADD-11). Fire it before skipping only when the arming decoder asked
   for that and `[done+0x10] != 0`.
2. **Construct `*ret` with the game's own `Connection()`: call `0x3190240(ret)`.** Verify its
   51 bytes first:
   `f3 0f 1e fa 55 48 89 e5 53 48 89 fb bf 10 00 00 00 48 83 ec 08 e8 86 ba 54 fd 48 c7 00 00 00 00 00 48 c7 40 08 00 00 00 00 48 89 03 48 83 c4 08 5b 5d c3`.
   Its `e8` is PC-relative, so the file bytes are the live bytes.
3. Return `ret` without calling the trampoline.

This leaves the caller in exactly the state Add's own empty-`done` path leaves it
(`0x15db048`-`0x15db07f`), minus the queued command.

**Do not write 0 into `*ret`** (the Windows `ZeroAddResult`). It is enough for `~Connection`, but
`0x10d4b60` copy-constructs the result through `0x31902c0`, which dereferences the pointer
(`0x31902d3`/`0x31902db`), and `0x31904b0` does the same. If `0x3190240`'s bytes do not verify,
the Add hook must not be installed, and the factory hooks must then not arm anything (§9).

### 2.13 The `done` policy on Linux  [C-ADD-11, INFERRED]

The Windows rules (COMMANDS.md, "Skipping a command safely") still decide the policy: fire `done`
for tools that wait on it, do not fire it for fire-and-forget commands, and honour an armed
cancel when the replay is already promised. What differs:

- **Many fire-and-forget UI sites pass an empty `done` on Linux.** The manager slot is written 0
  and nothing else: Reverse `0x1444377`, the three SetColor sites
  `0x144404a`/`0x144415a`/`0x144426a`, SetUserStopped `0x14310cd`, `LineEditor::DeleteTerminal`
  `0x10c1711`.
- **The waiting tools store a manager.**

  | tool | manager |
  |---|---|
  | `StreetBuilder::UpdateEngine` | `0xe6eda0` |
  | `ConstructionBuilder::MousePressed` | `0xe27060` (`0xe347e0 lea rdi` -> `[rbp-0xe0]`) |
  | `Bulldozer::Apply` | `0xdd1270` |
  | `TrackModifier::Build` | `0xec62c0` |
  | the stop tool `0xeaabb0` | `0xea53c0` |
  | ProposalAction `0xe59490` | `0xe57670` |

- **Several vehicle and line sites write 0 on one path and a manager on another.** SetLine
  `0x1433521`/`0x1437ee8`/`0x1438633`, SendToDepot `0x142fefc`, UpdateLine `0x10ca703`.

The "done manager" column in Appendix A lists every constant or `lea` written to the manager slot
before the call, in address order. It is a static heuristic that ignores control flow, so the
run-time test of `[done+0x10]` decides. Whether firing early (with the un-applied Command) is safe
for each Linux tool is UNPROVEN until measured; that belongs to each decoder area.

### 2.14 Threads  [C-ADD-12, UNPROVEN]

Which threads call Add, the factories and `done` is a run-time fact this map does not establish.
Static facts that bear on it:

- Add's push_back takes no lock (2.5, `0x15da8ce`-`0x15da8e9`); only the slot connect locks the
  signal impl's mutex.
- `CommandList::Swap` has one caller, `CGame::Sync` (`0xa30f0b`), whose one caller is
  `CGame::Step` (`0xa31c66`). A list-fired `done` therefore runs on whichever thread runs
  `CGame::Step` (2.6).
- `done` can also run synchronously inside the script sink `0xa2d650` (5.2), on whatever thread
  runs that Lua state.
- The two Add sites in engine source files are reached from UI code. `0x1789a7d` is in
  `0x1789880`, called only from `0xfefc28` in the lambda `0xfef880`, whose address is taken at
  `0xfe4246` in `0xfe3f20`. `0x2fbb309` is in `0x2fbae50`, called only from `0x131f200` in
  `0x131f1c0`, whose address is taken at `0x131f860` in `UI::CreateMissingResourcesWindow`
  `0x131f2f0`.

What the code does without that fact: the pending-cancel arm is `thread_local` (armed and
consumed on the same thread, 3.4), shared flags are atomics, and the first hit of Add and of each
factory hook logs `gettid()`. Nothing in the cancel path assumes a single thread.

---

## 3. Naming the command at Add: `factory.rdi == Add.rdx`

### 3.1 Factories return the Command through `rdi`  [C-PTR-1, PROVEN]

Every factory takes the hidden return slot in `rdi` and returns it in `rax`. Reverse shows the
pattern: `0x15ebe37 mov r12,rdi` ... `0x15ebeac mov rax,r12`. The builder `0x15eb570` does the
same (`0x15eb57c mov rbx,rdi` ... `0x15eb5e3 mov rax,rbx`).

**88 of the 94 Add sites** are preceded directly by a factory call: 87 UI/engine sites plus the
legacy script function `0x1d86ff0`. At each of them the factory's `rdi` and Add's `rdx` name the
same object, and **no `call` instruction lies between the factory call and the Add call**.
Appendix A shows the column. Four sites the heuristic could not settle were checked by hand:

| Add returns to | how the object reaches `rdx` |
|---|---|
| `0xeaafb2` | saved at `0xeaaf6d mov [rbp-0x1868],rax`, reloaded `0xeaaf87` |
| `0x10bf850` | `r12`, the factory `rdi` from `0x10bf7ad` |
| `0x1240a17` | via `[rbp-0x14c0]` (`0x12409d6`) |
| `0x1240ece` | via `[rbp-0x14c0]` |

### 3.2 The six other sites  [C-PTR-2, PROVEN]

| Add returns to | origin | pointer match |
|---|---|---|
| `0x10d4c34`, `0x10d9dc4` | CreateLine through `line_util` `0x2eb0fa0`: it saves its own `rdi` at `0x2eb0fb8 mov [rbp-0x138],rdi`, reloads it at `0x2eb1c3a` for `call 0x15efda0` (`0x2eb1c44`) and returns it (`0x2eb1c93`); no call between the `line_util` call and Add | **holds**; the factory hook sees return address **`0x2eb1c49`** |
| `0x1045db8`, `0x1045ed0` | `CGameUI::CreateConstructionMenu` lambda `0x1045460` calls BuildProposal twice, each into `r13`: `0x1045732` (returns to `0x1045737`) and `0x1045a42` (returns to `0x1045a47`). Each result is pushed into a vector by `0x10526d0` (`0x1045741`, `0x1045a51`) and destroyed by `~Command` `0x15d8f30` (`0x1045749`, `0x1045a59`); later each element is move-constructed into `rbx` (`0x1045d93`) and Added | **never**; neither return address is armable (C-PTR-5) |
| `0xa2f5c2`, `0x11225a9` | script command sinks (§5.2): the Command is moved twice (`0xa2f537`, `0xa2f5a5`; `0x1122517`, `0x112258c`) | **never** |

### 3.3 A maker's command never matches  [C-PTR-3, PROVEN]

A Lua `api.cmd.make.*` maker builds into a local and copies the variant into its own return
object. `setGameSpeed`'s maker `0x1950ef0` shows it:

- `0x1950f00 lea r12,[rbp-0x60]` / `0x1950f1e call 0x15eb600`;
- the variant is copied through the table `0x59c3a60` at `0x1950f47`;
- the local is destroyed at `0x1950f5b`.

That value later reaches Add through a sink that moves it again. A command Lua builds this way
therefore cannot equal any factory's `rdi` at Add. The legacy `scripting::AddFunction` `0x1d86ff0`
does build and Add in one place (identity holds, `0x1d87372`/`0x1d87388`). Its factory return
`0x1d8737a` is in the script set (§5.1), so it is never armed.

### 3.4 Consume the armed pointer at the next Add  [C-PTR-4, INFERRED rule on a PROVEN basis]

Rule: an armed pointer is consumed by the first Add on the thread that armed it (`thread_local`).
If `rdx` matches, cancel; if not, drop the arm, log it, and let that Add run. The rule does not
depend on C-ADD-12.

**A stale arm is dropped earlier: when the next top-level factory call starts on the thread.** An arm
goes stale when its Command never reaches Add. Two ways:

- its factory threw after an `onEntry` arm (e.g. `bad_alloc` from `operator new(0xd50)` at `0x15eb58d`);
- an exception unwound the handler that armed it.

Left in place, a stale arm would match a later Add whose Command reuses the same stack slot and
carries the same tag. That later action may have shipped behind `ARMED 0` or not at all, and the Lua
does not replay `ARMED 0` on the originator, so cancelling it would lose the action locally.
Dropping the arm at the next factory call is safe: an arm is only made at an armable site (C-PTR-5),
no armable window contains a call, and no factory calls a factory or Add (C-FAC-4). No arm can still
be waiting for its own Add at that point.

The basis, PROVEN by a capstone sweep of every containing function:

- At each of the **88 direct windows** (factory call to Add call: 87 UI/engine sites and the legacy
  script function), the instructions strictly between the two calls contain no call, jump or ret.
- No direct branch anywhere in those functions lands inside a window, and none of them contains an
  indirect jump.
- The **two CreateLine windows** run through `line_util` `0x2eb0fa0`. After
  `0x2eb1c44 call 0x15efda0`, `line_util` calls only `operator delete` (`0x2eb1c59`, `0x2eb1c81`)
  and `0xaa43f0` (`0x2eb1c65`), which itself calls only `operator delete`. The one other exit is the
  stack-protector failure (`0x2eb1c9a jne 0x2eb1d56`, `0x2eb1d56 call __stack_chk_fail`, noreturn).
  In the callers, the window from `line_util`'s return to Add has no call or branch.

### 3.5 The armable call sites  [C-PTR-5, PROVEN]

A cancel may only be armed at a factory call whose Command reaches Add with no call in between.
`SliceArmCancel` refuses every other return address, including one outside the image.

There are 124 `E8` calls to the 37 factories and no `E9`. 34 are script (§5.1). The other 90:

- **87 direct sites.** A linear sweep of the containing FDE from the factory call finds
  `E8 call 0x15da840` as the next control-flow instruction: no call, jump, jcc or ret in between.
  No direct branch in the function lands in `(return address, Add call]`.
- **1 site through a helper: `0x2eb1c49`**, CreateLine at `0x2eb1c44` inside `line_util` `0x2eb0fa0`.
  - Every path from `0x2eb1c49`, following direct branches, calls only `operator delete` (PLT
    `0x6dbcd0`) and `0xaa43f0`; `0xaa43f0` calls only `operator delete`.
  - Each path ends at `ret` `0x2eb1cb1` or at `0x2eb1d56 call __stack_chk_fail` (PLT `0x6db860`).
  - `0x2eb0fa0` has exactly two callers, both `E8`: `0x10d4c08` and `0x10d9d9c`. No `E9` targets it
    and no `.rela.dyn` addend equals it.
  - After each caller's call, the next control-flow instruction is the Add call (`0x10d4c2f`,
    `0x10d9dbf`).
- **2 sites that are not armable: `0x1045737` and `0x1045a47`** (C-PTR-2). The next calls after them
  are the vector push `0x10526d0` and `~Command` `0x15d8f30`. Copies of their Commands reach Add
  later, at `0x1045db8` and `0x1045ed0`.

**Why it matters.** `SliceShipAndArm` writes `ARMED 1` before Add. At a CreateConstructionMenu site,
the second BuildProposal supersedes the arm, or the first Add carries a moved copy (a mismatch).
Either way the arm is dropped and the command runs natively. The Lua still replays it on the
originator, because `ARMED 1` is on disk. The action is applied twice: the double apply
`honourArmed` exists to prevent. Windows never hit this, because it armed BuildProposal only for
whitelisted caller RVAs.

**The table.** `kArmableSites` (88 rows) and `kHelperCallers` (2 rows) in `slice_core.cpp` are
generated by `gen_armable.py` (implementer's scratch; it drives `functions.csv`, the call index and
capstone). Each row is checked against the image at init:

- the `E8` at `ret-5` must target the site's factory;
- the FNV-1a 32 hash of the window `[ret-5, end)` must match the analysed bytes;
- a direct window must end with the `E8` to Add;
- each helper caller must call the helper, then Add, and its window `[call, end)` must hash to the
  analysed bytes.

`.rela.dyn` and `.rela.plt` have no relocation inside `.text` `[0x6de300, 0x3e8bda5)`, so the file
bytes are the live bytes. Any mismatch fails `SliceCoreStaticChecks`: the slice patches nothing, and
`SliceIsArmableSite` answers false.

---

## 4. The factories

### 4.1 The set  [C-FAC-1, PROVEN]

**37 functions:**

- the callers of the Engine packaging helper `0x15ebab0` (25);
- plus the callers of the Command builder `0x15eb570` (13, one of which is `0x15ebab0` itself).

Every call to them is a direct `E8`. There is no `E9`, no relocation addend equals any of them,
and no RIP-relative `lea` references them. The 37 tags are distinct and cover 0..0x24 (Appendix B).

### 4.2 Prologues  [C-FAC-2, PROVEN]

Appendix B gives the bytes and steal for each of the 37:

- 14 bytes for most;
- 15-16 where a `mov` sits between the pushes;
- 18 for the eight small engine-less ones (`... 41 54 53 4c 8d a5 90 f2 ff ff`).

Every steal is what `PrologueSteal(code, 14)` returns, sits on a capstone instruction boundary,
contains no RIP-relative operand or branch, and no branch inside the function lands within it.

### 4.3 Windows hook ids -> Linux

The funcsig-named rows are PROVEN; the others are INFERRED [C-FAC-3].

| Windows id / RVA | factory | Linux | naming evidence |
|---|---|---|---|
| 0 / `0x9dc750` | BuildProposal | `0x15ee930` | tag 0xf = Windows tag 15; 17 UI callers (StreetBuilder, ConstructionBuilder, ModuleBuilder, Bulldozer::Apply, TrackModifier, ...) |
| 2 / `0x9dca00` | BuyVehicle | `0x15ef3b0` | funcsig |
| 3 / `0x9de380` | SellVehicle | `0x15ecf00` | funcsig |
| 4 / `0x9dddb0` | ReplaceVehicle | `0x15ef8c0` | funcsig |
| 5 / `0x9de6f0` | SendToDepot | `0x15ec220` | funcsig |
| 6 / `0x9dea10` | SetLine | `0x15ed550` | funcsig |
| 7 / `0x9dcde0` | CreateLine | `0x15efda0` | funcsig |
| 8 / `0x9df4e0` | UpdateLine | `0x15f0050` | funcsig |
| 9 / `0x9dd190` | DeleteLine | `0x15ebd00` | funcsig |
| 10 / `0x9ddfe0` | Reverse | `0x15ebe00` | funcsig |
| 12 / `0x9df340` | SetVehicleTargetMaintenanceState | `0x15ec000` | funcsig |
| 13 / `0x9de8a0` | SetColor | `0x15ecb40` | funcsig |
| 14 / `0x9deb70` | SetName | `0x15ee6d0` | funcsig |
| 15 / `0x9de9e0` | SetGameSpeed | `0x15eb600` | callers are exactly COMMANDS.md's: `UI::Clock::TogglePause` `0xf6c288`, `CameraAction::Play`/`Record`, `CGameUI::GameStep`, `CMenuUI::SwitchToGameUI` lambda, DebugViewComp; its maker truncates a double (`0x1950ef5 cvttsd2si`) |
| 16 / `0x9de9b0` | SetDate | `0x15eb7b0` | maker is the `getter<boost::gregorian::date>` functor `0x19729c0`; UI caller in Clock.cpp `0xf6b9b0` |
| 17 / `0x9de870` | SetCalendarSpeed | `0x15eb690` | int maker `0x1950f90`; UI caller in Clock.cpp `0xf70320` |

The funcsig-named tags agree with every Windows dispatch tag COMMANDS.md names (2, 7, 8, 9, 12,
13, 14, 28, 31); that agreement is what names 10, 15 and 22 (`0x15ec150` SetVehicleShouldDepart,
`0x15ee930` BuildProposal, `0x15ec610` ConnectTownsAndIndustries). Tags 0x11, 0x13-0x15, 0x19 and
0x21-0x24 stay unnamed here.

### 4.4 Arguments in typed detours  [C-ABI-1, PROVEN for the quoted calls]

Non-trivially-copyable by-value parameters travel as pointers in the next integer register. A
small float struct by value (CVec3f) travels as `xmm0` = {x, y} and `xmm1` = {z}: CreateLine's
callers load `0x2eb1c32 movq xmm0,[rbp-0xf0]` and `0x2eb1c2a movss xmm1,[rbp-0xe8]` before
`0x2eb1c44`, and the maker does the same at `0x1951b50`/`0x1951b58`. A typed detour must declare
the real float parameters so it passes them on. Each factory's exact signature belongs to its
decoder area.

---

### 4.5 Stack arguments  [C-FAC-4, PROVEN]

A capstone sweep of every factory body over its FDE size looked for four things: an operand
`[rbp+d]` with `d >= 0x10`, `rbp` used as an index, a copy of `rbp` into another register, and an
`rsp`-relative operand beyond the prologue's frame.

- **SaveGame `0x15ed140`** reads a stack argument: `0x15ed176 mov r13d,[rbp+0x10]`. Its caller pushes
  it: `0x10a95a2 push rax` before `0x10a95b4 call 0x15ed140`.
- **Book `0x15ecd80`** reads `[rbp+0x10]`, `[rbp+0x18]`, `[rbp+0x20]` and `[rbp+0x24]` (`0x15ecdbc`,
  `0x15ecdc5`, `0x15ecdc9`, `0x15ecde5`). Its script caller pushes three qwords (`0x1973006`,
  `0x1973013`, `0x1973029`).
- **The other 35 factories** have none of the four.
- No factory references a ymm or zmm register, and none calls another factory or Add.
- xmm registers read before they are written in the entry block: CreateLine 0 and 1, SetColor 0 and 1,
  tag 0x19 0, tag 0x22 0-3, tag 0x23 0 and 1.

Consequence (C-HOOK-7): a detour declared `(6 x uint64_t, 8 x __m128)` receives the complete argument
list of each of the 35, and calling the trampoline with the same list passes it on unchanged. SaveGame
and Book would read the detour's frame instead of their caller's, so slice-core refuses handlers on
them. No decoder area plans one.

---

## 5. Script-side callers

### 5.1 Factory return addresses that are script  [C-SCRIPT-1, PROVEN]

Windows `IsScriptCaller`: `0xcec000..0xcf2000` plus `0xc3848e` (setColor) and `0xc17eff`
(setGameSpeed). On Linux **these 34 return addresses** are exactly the factory call sites in
script code:

`0x1950f23` `0x1950fc1` `0x1951063` `0x1951b68` `0x1969681` `0x1969ab1` `0x1969f2c` `0x196a371`
`0x196a7a1` `0x196abd1` `0x196b033` `0x196b43c` `0x196b929` `0x196bd64` `0x196c234` `0x196c704`
`0x196cbd4` `0x196d43f` `0x196dad8` `0x196e056` `0x196e662` `0x196f04c` `0x196f9d3` `0x197023e`
`0x1971333` `0x1972ad2` `0x1973034` `0x1973c70` `0x1974841` `0x1974ffc` `0x1975884` `0x19763ff`
`0x1977628` `0x1d8737a`

Each lies in a function that `scripting::SetupCommandInterface`'s registration body `0x19638f0`
reaches (§5.3), except `0x1d8737a`, which is in `scripting::AddFunction` `0x1d86ff0`
(`scripting/legacy/cmd_interface.cpp`). Notable rows:

| maker | Linux | Windows |
|---|---|---|
| buildProposal | `0x1971333` | `0xced378` |
| buyVehicle | `0x1974841` | `0xceefae` |
| setColor | `0x196e662` | `0xc3848e` |
| setGameSpeed | `0x1950f23` | `0xc17eff` |
| createLine | `0x1951b68` | |

All other factory call sites (90) are UI or engine code. A range test also separates them on this
build [C-SCRIPT-2, PROVEN]: the script set lies in `[0x1950ef0, 0x1978700)` and
`[0x1d86ff0, 0x1d878da)`, and no other factory call site does. The helper should use the exact
set, and the range only as a consistency check.

### 5.2 Script-issued commands  [C-SCRIPT-3, PROVEN; C-SCRIPT-4, UNPROVEN]

`scripting::SetupCommandInterface` `0x19638f0` takes
`(lua::State&, const GameRes&, const std::function<const GameState&()>&, const std::function<void(Command, const std::function<void(const Command&)>&)>& sink)`.
The parameter list is spelled out in the template arguments of `user_allocate`'s signature at
`0x19524c0`, and the function loads the string "SetupCommandInterface" at `0x1963906`. Its only
caller is `0x1d86a90`, which tail-jumps to it with `rdx`/`rcx` unchanged:
`0x1d86a9a mov r15,rcx` ... `0x1d86b24 mov rcx,r15`, `0x1d86b3a jmp 0x19638f0`.

**How `api.cmd.sendCommand` reaches the sink** (PROVEN):

1. `0x19638fc mov r14,rcx` holds the sink, and nothing writes r14 before
   `0x1963a21 call 0x197fa40`. That is the `std::function` copy constructor (op 2 at `0x197fa67`,
   then `+0x18`/`+0x10`), and it copies the sink to `rbp-0x190`.
2. `0x1963a2d lea "sendCommand"`, then `0x1963a4f call 0x19524c0` with `rdx` = that copy.
   `0x19524c0` is `user_allocate<functor_function<SetupCommandInterface(...)::<lambda(sol::table,
   std::variant<CmdData::...>, sol::optional<sol::protected_function>, sol::this_state)>>>`.
   - It pushes nil (`0x19525a9 call 0x1950990`, which returns 1 at `0x19509b6`; the count is kept
     at `0x19525ae`).
   - It allocates the userdata (`0x1952611 call 0x983b50`, `lua_newuserdata`: the new value gets
     tag `0x47` at `0x983b91` and the payload `Udata+0x28` is returned).
   - It aligns the pointer to 8 bytes (`0x1952616`-`0x195262f`) and copies the sink into it
     (`0x195264d call 0x197fa40`, `rdi` = the aligned block).
   - `0x19526fa`-`0x195270d` push the call wrapper `0x197cf00` as a C closure with
     `nil count + 1 = 2` upvalues (`0x19509e0` -> `lua_pushcclosure` `0x982c10`). Upvalue 1 is
     the nil and upvalue 2 is the userdata.
3. `0x197cf00` reads upvalue 2: `0x197cf88 mov esi,0xfff0b9d6` (`lua_upvalueindex(2)` in Lua 5.2)
   and `0x197cf90 call 0x9828a0` (`lua_touserdata`: the value for tag 2, `Udata+0x28` for tag 7).
   It aligns the pointer the same way (`0x197cf95`-`0x197cf9f`) into r12 and calls `0x197a6f0`
   with `rdx = r12` (`0x197d22d`, `0x197d236`).
4. `0x197a6f0` saves `rdx` (`0x197a71c mov [rbp-0x2948],rdx`). It builds the callback
   `std::function`: the functor comes from `new 0x28` at `0x197ad68`, the manager is `0x19512d0`
   and the invoker `0x19514b0`. It then move-constructs the Command (`0x197adab call 0x15d8ea0`)
   and invokes the sink: `0x197adb7 cmp qword [sink+0x10],0` (empty goes to
   `bad_function_call`), then `0x197add9 call [sink+0x18]` with `rdi = sink`,
   `rsi = &Command`, `rdx = &callback`. The function's two callback paths join at `0x197ad58`
   before that call, and it makes no other `call [..+0x18]` through the saved pointer.

**The three sinks**, one per call of `0x1d86a90`:

| set up at | Lua state passed as `rdi` | sink (invoker) | what it does |
|---|---|---|---|
| `0xa33026` in `0xa31d80` (Game.cpp), `r8d = 0` | `[[[CGame+0x160]+0x00]+0x220]+0x30` | **`0xa2d650`** (manager `0xa2d4c0`, functor `CGame*`) | **no Add.** Moves the Command (`0xa2d67d`) and applies it at once: `0x15e2e70(gameStates[[m_data+0x20]], &cmd)` (`0xa2d690`-`0xa2d698`). It then calls `done(cmd)` itself (`0xa2d6ab call [done+0x18]`) and destroys the Command |
| `0xa330e2` in the same function, `r8d = 1` | `[[[CGame+0x160]+0x08]+0x220]+0x30` | **`0xa2f500`** (manager `0xa2d560` at `0xa330b5`, functor `CGame*` at `0xa330cd`) | Moves the Command (`0xa2f537`), copies `done` (op 2 at `0xa2f57d`), moves the Command again (`0xa2f5a5`), then calls Add on `[CGame+0x158]` (`0xa2f556`, `0xa2f5bd`). Add returns to **`0xa2f5c2`** |
| `0x112bda1` in `UI::CMenuUI::SwitchToGameUI` `0x112ba00`, right after `scripting::SetupGuiInterface` (`0x112bd36`), `r8d = 1` | `[[owner+0x530]+0x30]` | **`0x11224e0`** (manager `0x11182a0`, functor `owner` at `0x112bd81`) | The same body as `0xa2f500`, with the CommandList at `[[owner+0x4c0]+0x158]` (`0x1122536`). Add returns to **`0x11225a9`** |

Each sink's invoker is `void(const _Any_data&, Command&&, const std::function<void(const Command&)>&)`.
It moves from `rsi` and copies `rdx` (op 2); it never moves `rdx`.

Also script-issued: `0x1d873b2`, in the legacy `scripting::AddFunction` (cmd_interface.cpp),
calls factory `0x15ee130` (tag 0x20) and then Add.

**C-SCRIPT-4, UNPROVEN:** in which of these Lua states the mod calls `api.cmd.sendCommand`. That
decides whether a mod-sent command reaches Add at `0xa2f5c2`/`0x11225a9` or is applied at once by
`0xa2d650`.
- The code must not classify an Add by these return addresses beyond logging them.
- Nothing in the cancel path needs to, because a script-sent Command never equals an armed
  factory pointer (3.2, 3.3).

### 5.3 Evidence of registration  [part of C-SCRIPT-1]

- `0x1950ef0`, `0x1950f90`, `0x1951030` and `0x1951aa0` have their addresses taken inside
  `0x19638f0` (`0x1966832`, `0x19663a4`, `0x1965fc7` and `0x19642cb`).
- Each maker body in `0x1969530..0x1977430` is tail-jumped from a
  `sol::function_detail::functor_function<scripting::SetupCommandInterface(...)::<lambda>>` call
  wrapper (e.g. `0x196a220` from `0x196a644` in `0x196a5b0`). That wrapper's address is taken in
  `0x19638f0` or in a `user_allocate<functor_function<SetupCommandInterface...>>` that
  `0x19638f0` calls (e.g. `0x1958ee3` in `0x1958ca0`, called at `0x1965776`).
- The makers never call Add. They copy the Command into a Lua value through `0x197fab0`.

---

## 6. libstdc++ layouts, observed in the game

| type | layout | evidence | status |
|---|---|---|---|
| `std::string` [C-STL-1] | `{char* p; size_t len; char buf[16]}`, `p == buf` when short | `0x1951b1e lea rax,[rbx+0x10]; 0x1951b37 mov [rbx],rax`; `0x1951bb0 mov rdi,[rbp-0x60]; cmp rdi,rbx(+0x10); je` (no free); `0x1d87306 mov rsi,[..]; 0x1d8730d mov rdx,[..+8]; add rdx,rsi` | PROVEN |
| `std::vector` [C-STL-2] | `{begin, end, cap}` | `0x15da8d1 mov rdi,[r12+8]; cmp rdi,[r12+0x10]`; `0x15da906 mov rax,[r14+8]; sub rax,[r14]` | PROVEN |
| `std::function` [C-STL-3] | `_Any_data` 16 B, `_M_manager` +0x10 `bool(_Any_data&, const _Any_data&, int op)` (2 clone, 3 destroy), `_M_invoker` +0x18 | destroy `0x14443ce`-`0x14443e2` (`rdi=rsi=obj, edx=3`); clone `0xa2f572`-`0xa2f58d` (op 2 then copy +0x18, +0x10); invoke `0x15da4d4` | PROVEN |
| `std::map` / `_Rb_tree` [C-STL-4] | map `{cmp 8 B; header node at +8 (color +8, root +0x10, leftmost +0x18, rightmost +0x20); node_count +0x28}`; node `{color, parent +8, left +0x10, right +0x18, value +0x20}`; `end() == &header` | `0xc194b0` (map at +0x10 of its owner): `0xc194d1 lea r12,[rdi+0x18]` (end), `0xc194cd mov rax,[rdi+0x28]` (begin), `0xc194e0 cmp byte [rax+0x24],3` (value +0x20), `call _Rb_tree_increment` (PLT `0x6dc1c0`) until end; `0xef30e8 lea r15,[rbx+0x20]` | header, leftmost, value PROVEN; root, rightmost, count and the node links INFERRED (ABI) |
| `shared_ptr`/`weak_ptr` [C-STL-5] | `{ptr, ctrl}`; `_Sp_counted_base` use +8, weak +0xc, `_M_destroy` vtable +0x18 (boost `sp_counted_base` the same) | `0x15d8f58 lock xadd [rdi+0xc]`; `0x15d8fc1 call [rax+0x18]`; `0x9e2260`; `0x319044e` | PROVEN |

---

## 7. Files, paths and the process

| id | fact | evidence | status |
|---|---|---|---|
| C-FILE-1 | Inside the container the paths are the host paths. The data dir is `/home/topsnek/snap/steam/common/.local/share/tpf2mp/data/`. boot dlopens `tpf2_slice.so` from `$XDG_DATA_HOME/tpf2mp/`, else from its own folder. | `tpf2_proxy.log`: `attached to pid ... from /home/topsnek/snap/steam/common/.local/share/tpf2mp/libtpf2mp_boot.so`, `tpf2_slice.so not present (/home/topsnek/snap/steam/common/.local/share/tpf2mp/tpf2_slice.so)`; `tpf2_bridge.log`: `data dir: /home/topsnek/snap/steam/common/.local/share/tpf2mp/data/`; boot.cpp `ResolveShipped` | PROVEN |
| C-FILE-2 | `tpf2_instance.txt` = `<letter>\npid=<getpid()>\n`, followed by `port=<udp>\n` **only when the bridge's UDP socket is bound** (`Net_LocalPort() != 0`). A reader must accept a file without the port line. The file is written to `<path>.tmp<pid>` and renamed over the real name (atomic): at startup, by `Reidentify` and by `HealIdentity`. The bridge runs in the same process, so `pid` equals the slice's `getpid()`. | bridge_linux.cpp `WriteIdentity` 143-181: `"%s\npid=%d\n"` (160), the port line inside `if (port)` (162-166), temp name (170), `rename` (177); callers 271, 313, 528; the log at 523 says a failed `Net_Init` leaves no `port=` line. Live file: `a\npid=60512\nport=7771\n` | PROVEN |
| C-FILE-3 | Windows 0.4.22 status ends with `  mp=%d`, with `peer=?` or a positive peer time. Lua truncates and rewrites the file without a trailing newline. A valid peer or `mp>=2` establishes multiplayer; incomplete, missing or stale status retains established multiplayer state. A complete fresh solo status releases it. | unchanged lockstep.lua `CM.statusLine`; slice_core.cpp `ComputeSessionLive`; slice_cancel_test.cpp | PROVEN offline; live session transitions pending |
| C-FILE-4 | `lockstep_inject_<L>.txt`: appended by the slice **and** by the mod (`io.open(..., "a")`, lockstep.lua 1378). The reader consumes only up to the last `\n`. `ARMED <n>` and `STREETP` are standalone lines consumed by the next record. | io.lua 57 (`while last > 0 and data:byte(last) ~= 10`); inject.lua 64-72 | PROVEN |
| C-FILE-5 | Both native Linux and unchanged Windows Lua search `tpf2_slice.cfg` in the game working directory, then the selected data directory. The native library root is not a candidate. | slice_core.cpp `OpenCfg`; unchanged stops.lua `CM.cfgFlag`; docs/linux/LUA.md | PROVEN |
| C-FILE-6 | Native boot publishes final `TPF2MP_DATADIR` and `LOCALAPPDATA=<absolute XDG_DATA_HOME or HOME/.local/share>` before loading libraries. Unchanged .22 Lua chooses the first candidate with identity, otherwise its LOCALAPPDATA fallback. Overrides therefore require the bridge identity. HOME and XDG_DATA_HOME remain unchanged. | boot.cpp `PublishLuaEnvironment`; actual preload test_boot_environment.py; docs/linux/LUA.md | PROVEN offline with six directory cases; live map pending |

Implementation notes that follow from C-FILE-2/3/4 (design, not RE):

- **Identity file.** Parse the letter from line 1 and `pid=` from any line; ignore a missing
  `port=` line.
- **Status file.** Retain established multiplayer state across missing, partial or stale
  reads. Only a complete fresh solo status permits native single-player actions.
- **Inject file.** Write each capture, its `ARMED` line included, as one `write()` on an
  `O_APPEND` descriptor. Windows' separate `fopen`/`fprintf` calls could interleave with the
  mod's own appends.

---

## 8. Guarded reads

| id | fact | evidence | status |
|---|---|---|---|
| C-READ-1 | `process_vm_readv(getpid(), ...)` on self reads readable memory, fails with `EFAULT` for page 0 and for a `PROT_NONE` page, and returns a partial count (16 of 32) across an unmapped or `PROT_NONE` page boundary. `write(pipe, badaddr, n)` fails with `EFAULT` without a signal. | throwaway probe (kernel 7.0 host, outside the container), reproduced by two independent verifier probes | PROVEN off-game |
| C-READ-2 | Whether `process_vm_readv` on self (or the pipe probe) works inside the game's pressure-vessel container is **not established**. Supporting, not proof: the Snap Steam client showed `Seccomp: 0`, `Seccomp_filters: 0` in `/proc/<pid>/status` (filters are inherited by children); `pressure-vessel-wrap` contains no `seccomp` string; the kernel's `__ptrace_may_access` returns before the LSM hook for the same thread group. The game was not running during the revision, so its own status was not read. | as stated | **UNPROVEN.** The library self-tests each mechanism at init: a readable address must read fully, an unmapped one must fail with `EFAULT`, and a read straddling a boundary must return a partial count. It uses `process_vm_readv` if that passes, else the pipe probe if that passes. If both fail, every guarded reader returns false, the decoders that need them stay off, and the log says so |

---

## 9. Hook installation

| id | fact | evidence | status |
|---|---|---|---|
| C-HOOK-1 | `InstallHook` patches `FF 25 00 00 00 00 <abs64>` (a `jmp`, not a `call`) and publishes the trampoline before patching. In a typed detour, `__builtin_return_address(0)` is therefore the game caller's return address, and the trampoline behaves as the original function. `PrologueSteal` decodes `endbr64`. | hook_posix.cpp 12, 23-61, 65-68 | PROVEN |
| C-HOOK-2 | No other Linux library patches Add, a factory, `0x3190240` or their stolen bytes. Bridge: `0xc0dc30`, `0xa31c30`, `0x1dc51dc`, `0x1dc5293` (+ cave). Menu: `0x113ca70`, `0x30de940`, `0x1154b20`, and a call redirect at `0x35096e9`. InstallHook works in the live game (`tpf2_bridge.log`: `hooked CGameTime::GetSpeed (c0dc30) and CGame::Step (a31c30)`, `patched 1dc51dc and 1dc5293`). | speedhook_linux.cpp, setplayer_linux.cpp, menu_linux.cpp, overlay_vk_linux.cpp constants; tpf2_bridge.log | PROVEN |
| C-HOOK-3 | Installing from the init thread started by the library's constructor (boot dlopens from an LD_PRELOAD constructor, before `main`) cannot race a running Add or factory: they run only once the title menu or a game exists. | boot.cpp `BootInit`; COMMANDS.md callers are UI and script | INFERRED |
| C-HOOK-4 | Install order (a policy): verify `0x3190240`'s 51 bytes and Add's prologue, then install Add. A factory hook whose handlers can arm a cancel is installed only when Add is in; otherwise its arming handlers are disabled and the log says so. A factory hook that writes `ARMED 1` without a working cancel point promises a replay while the native command also runs (the Windows double-apply). | C-ADD-10; slice_hook.cpp 192-198 | INFERRED (a design rule, not a fact about the game; it can only remove hooks) |
| C-HOOK-5 | **One `InstallHook` per target RVA**, owned by the registry in `slice_install.cpp`: one typed detour and one trampoline per target, with the areas' handlers registered behind it. `InstallHook` (hook_posix.cpp) copies whatever bytes sit at the target into the trampoline and checks nothing. After a first install the target begins `FF 25 00 00 00 00`, so a second install on the same RVA would build a trampoline that jumps into the first detour; the registry's expected-bytes check refuses it instead. Several areas plan handlers on the same targets: BuildProposal `0x15ee930` (SLICE_CONSTRUCTION.md §10, SLICE_TERRAIN_ASSETS.md §9, SLICE_PROPOSAL.md) and Add `0x15da840` (every decoder area's integration notes). | hook_posix.cpp `InstallHook`; the named docs | PROVEN |
| C-HOOK-6 | **Not every function starts with `endbr64`.** The Command builder `0x15eb570` (`55 48 89 e5 41 55 49 89 f5 ...`) and the Engine packaging helper `0x15ebab0` (`55 48 89 e5 41 57 41 56 49 89 ce ...`) begin with `push rbp`. They are called only directly, from the factories (13 and 25 `E8` sites, C-FAC-1), and are not hook targets. The 37 factories, Add and `0x3190240` do start with `f3 0f 1e fa` (Appendix B, C-ADD-4, C-ADD-10). The registry compares each target's full expected prologue and assumes nothing about `endbr64`. hook_posix.cpp's comment "every function starts with one" is too broad; its decoder handles both forms. | capstone at `0x15eb570` and `0x15ebab0`; the call index | PROVEN |
| C-HOOK-7 | **One generic detour per factory is exact** for the 35 factories without stack arguments. A C++ function `uint64_t(uint64_t x6, __m128 x8)` receives every register the SysV ABI can use for their arguments (rdi..r9 for INTEGER, xmm0..xmm7 for SSE) and passes them on unchanged when it calls the trampoline with the same list, whatever each means. Upper ymm halves carry nothing (no factory touches ymm), no factory is variadic, and the return value is rax (the Command slot, C-PTR-1). | C-FAC-4; the SysV AMD64 ABI's parameter classification; the off-game test forwards distinct values through all seven prologue shapes (§12.5) | PROVEN |
| C-HOOK-8 | The patch ranges of our other libraries, refused by `SliceRegisterHook`: bridge `0xc0dc30`+14, `0xa31c30`+15, `0x1dc51d4`+44, `0x1dc5290`+9; menu `0x113ca70`+17, `0x30de940`+16, `0x1154b20`+16, `0x35096e9`+5. | speedhook_linux.cpp `GETTER_EXPECTED[14]`, `CGAME_STEP_EXPECTED[15]`; setplayer_linux.cpp `RVA_A` + `EXPECTED_A[44]`, `RVA_B` + `EXPECTED_B[9]`; menu_linux.cpp `MAINBUILD_EXPECTED[17]`, `LIST_ADD_EXPECTED[16]`, `CREATEPAGE_EXPECTED[16]`; overlay_vk_linux.cpp `RVA_INIT_DEVICE_CALL` (a 5-byte call) | PROVEN from the sources (the plugin host's sites are not in the table) |

---

## 10. Windows -> Linux quick reference

| Windows | Linux |
|---|---|
| `CommandList::Add` `0x9d2a00`, steal 18, relay | `0x15da840`, steal 14, typed detour `(ret rdi, self rsi, cmd rdx, done rcx, progress r8) -> rax` |
| Add's out handle `rdx`, destroyed by `0x2357910`; cancel writes 0 | the return slot `rdi` (`Connection`, 8 B), destroyed by `0x3190430`; cancel constructs it with `0x3190240`, **never 0** |
| command pointer: `Add.r8 == factory.rcx` | `Add.rdx == factory.rdi` |
| `done` = r9, impl at `r9+0x38`, `_Do_call` vtable +0x10 | `done` = rcx; manager `+0x10` (0 = empty), invoker `+0x18`: `invoker(done, cmd)` |
| variant tag `*(u8*)(*cmd + 0xb18)` | `*(u8*)(*cmd + 0xd48)`, CmdData 0xd50 B |
| `IsScriptCaller` `0xcec000..0xcf2000`, `0xc3848e`, `0xc17eff` | the 34 addresses of §5.1 |
| `api.cmd.sendCommand` Add returns to `0x1126f1a` | sink `0xa2f500` (Add returns to `0xa2f5c2`) or `0x11224e0` (`0x11225a9`); sink `0xa2d650` applies without Add; which one the mod's calls use is UNPROVEN (C-SCRIPT-4); legacy `0x1d873b2` |
| PE TimeDateStamp + SizeOfImage gate | GNU build-id gate (`game_image.h`) |
| named mutex | flock on a file in the data dir (design) |
| `VirtualQuery` + SEH | `process_vm_readv` on self, pipe-write fallback, each self-tested at init (§8; in-container behaviour UNPROVEN) |
| 16 hooks through one asm `DeferRelay` | a typed detour for Add; one generic register-forwarding detour per factory (C-HOOK-7) with the areas' handlers behind it (C-HOOK-5); an area's own typed detour for any other target |
| `g_pendingCmd`, `g_pendingNoCb`, `g_pendingHonour` (process-wide) | one `thread_local` arm {Command*, factory tag, `SliceDone` policy, honourArmed, callbacks}, consumed by the next Add on that thread (C-PTR-4) |
| `ZeroAddResult` | `Connection()` into `*ret` |
| per-decoder "cancel landed" flags checked in `DeferHandler` | a `SliceOutcome` passed to the decoder's `landed` callback |
| `IsScriptCaller` decided `cancel` in each branch | `SliceArmCancel` refuses a script caller itself |

---

## 11. Open questions

1. Does `process_vm_readv`, or the pipe probe, pass its self-test inside the pressure-vessel
   container (C-READ-2)? The init self-test answers it in the first live run.
2. Which threads call Add, the factories and `done` (C-ADD-12)? Log `gettid()` on first hits.
3. In which Lua state does the mod call `api.cmd.sendCommand` (C-SCRIPT-4)? That decides whether a
   mod-sent command goes through Add (`0xa2f500`, `0x11224e0`) or is applied at once (`0xa2d650`).
   Counting Adds that return to `0xa2f5c2`/`0x11225a9` while the mod sends commands answers it.
4. Per tool: is firing `done` early, with the un-applied Command, safe on Linux? Do the vehicle
   and line sites that store a manager on one path (SetLine, SendToDepot, UpdateLine) show a
   failure message when it is fired (C-ADD-11)?
5. The names of CmdData tags 0x11, 0x13-0x15, 0x19 and 0x21-0x24 (decoder areas).
6. Appendix A's "done manager" column is a last-write heuristic; for the multi-write sites the
   run-time value decides.
7. Does the simulation loop always finish a batch before the next `CGame::Sync` emits
   (the INFERRED ordering in C-ADD-6)?

What `tpf2_slice.log` records in the first live run to answer them:

- (1) each reader mechanism's self-test result;
- (2) `gettid()` of the first Add, of the first call of each hooked factory, of the first `done`
  fired from the detour, and of every arm and cancel;
- (3) the first Add from each script sink, and the per-sink counts in the 15-second alive line;
- (4) and (6) every cancel's outcome, under the policy its decoder chose.

Question 7 cannot be observed from the slice.

---

## 12. The implementation (`native/linux/src/slice/`)

### 12.1 Files

| file | contents |
|---|---|
| `slice_core.h` | the decoder areas' interface |
| `slice_core_internal.h` | shared by slice-core's own files and the tests: the Add and Connection() constants, the `SliceCoreEnv` seam (image base, data dir, root dir, executable ranges), the init and install entry points |
| `slice_core.cpp` | log, cfg, identity, session, inject writer, guarded readers and their self-test, container readers, the factory table (Appendix B plus C-FAC-4), the script set (§5.1), the table checks |
| `slice_install.cpp` | the registry, the Add detour and cancel, the generic factory detours, the areas' own hooks, the install order, the alive line |
| `slice_core_main.cpp` | the library constructor and the init thread |

### 12.2 Init thread

The library constructor returns at once unless the process image is `TransportFever2`; otherwise it
starts the init thread, which runs these steps in order:

1. **Data dir.** `Tpf2mpDataDirA`. Without one there is nowhere to log or write, so the library
   stays inert.
2. **Lock and log.** `flock(LOCK_EX|LOCK_NB)` on `<data dir>tpf2_slice.lock` (`O_CLOEXEC`, held for the
   process lifetime), then `tpf2_slice.log` opened truncated.
   - If another game holds the lock, this one appends a line to that game's log and stays inert. That
     is the Windows named mutex, per data dir.
   - On a filesystem without flock it goes on unlocked and says so. A second copy inside one process
     is still stopped by the byte checks, because the targets would already begin `FF 25`.
   - The lock file is not the log archive's `.tpf2mp_game.lock`.
3. **Build gate (C-BUILD-1).** Any build other than 35924 logs and stops.
4. **Guarded readers (C-READ-2).** `process_vm_readv` on its own pid is tried first, then the pipe
   probe. Each must pass the self-test: a readable heap range and our own `.rodata` read fully and
   correctly; address 0, address `0x1000` and a `PROT_NONE` page fail; a read straddling into the
   `PROT_NONE` page does not succeed (and `process_vm_readv` returns exactly 16 of 32). If both fail,
   every reader returns false and the cancel point is off.
5. **Table checks.** 37 factories with distinct tags 0..0x24, each steal where `PrologueSteal` lands, the
   34 script return addresses sorted and inside the §5.1 ranges, and in the image an `E8` call to a
   table factory before each of them. A failure patches nothing.
6. **Stop switch.** `TPF2MP_NO_PATCHES=1` stops here.
7. **Registration.** The `SLICE_AREA` entries run sorted by name. They live in the ELF section
   `tpf2mp_slice_areas`, so there is no constructor and no link-order dependency.
8. **Install, in order.**
   1. `Connection()`'s 51 bytes must match.
   2. Add is installed when a handler may arm, an Add observer exists, or a hook needs the cancel
      point, and only if `Connection()` matched and Add's 14 bytes match.
   3. Every factory that has a handler, its prologue verified.
   4. The areas' own hooks. One marked `needsCancel` is skipped while the cancel point is off.
9. **Alive line** every 15 s, with the counts of Adds, arms, cancels (done fired or not), native runs,
   mismatches, supersessions, per-script-sink Adds and factory calls.

### 12.3 The decoder interface (`slice_core.h`)

- **Registration.**
  - `SLICE_AREA(ident, "slice-lines") { ... }` in the area's file.
  - Inside it:
    - `SliceOnFactory({area, factoryRva, onEntry, onReturn, ctx, arms, order})`: refused for an
      unknown RVA, SaveGame or Book. Handlers of one factory run by ascending `order`, ties in
      registration order. Areas register in name order, so a cross-area sequence has to be set with
      `order`: on BuildProposal's Lua caller `0x1971333` the terrain injectors must run before
      `MergeTemplateStreet` (SLICE_CONSTRUCTION.md §10).
    - `SliceOnAdd(area, fn, ctx)`: every Add, before the cancel decision;
    - `SliceRegisterHook({area, name, winId, rva, expected, expectedLen, steal, detour, &trampoline, needsCancel})`.
  - A hook is refused on a factory, Add, `Connection()`, a C-HOOK-8 range, an overlap, or a steal
    that `PrologueSteal` does not land on.
  - Registration is closed outside `SLICE_AREA`.
- **The factory call.** `SliceFactoryCall` holds the entry registers `rdi..r9`, `xmm[8]`, `retAddr`,
  `retRva`, `script`, `tid`, and `rax` in return handlers.
  - Handlers of one factory run in registration order.
  - A hooked function reached from inside a handler on the same thread runs without handlers.
  - The registers passed to the original are the entry values, not the struct.
- **The cancel.**
  - `SliceArmCancel(call, {what, done, honourArmed, beforeFire, landed, ctx})` works only inside this
    thread's handler for `call`. It is refused for a script caller, a null slot, or while
    `SliceCancelAvailable()` is false.
  - The next Add on the thread consumes the arm.
  - `SliceDisarm(call)` drops it silently.
  - `SliceShipAndArm(call, arm, record)` has no mode argument. In multiplayer it arms
    and writes the existing `ARMED 1` + record. A failed write disarms; the central
    verified-player Add barrier blocks the action. Solo writes no capture.
- **Files.**
  - `SliceInstance`: re-read on every call; 1-7 alphanumerics; the `pid=` line must name this process;
    `port=` optional.
  - `SliceSessionLive`: 1 s cache; missing, empty or stale status retains an established session.
    A fresh solo status follows the existing Windows/Lua contract without extra fields.
  - `SliceRecordAppend`, `SliceRecordPrintf`, `SliceRecordFree`.
  - `SliceInjectWrite(record, None|Zero|One)`: one `write(2)` on `O_APPEND`; a final `\n` is added
    when missing; Zero capture writes are refused. A short write retains the existing
    recovery terminator; cancellation remains armed when any payload may have been consumed.
  - `SliceInjectLine`.
  - `SliceCfgFlag`: the strict parse, root then data dir. `SliceDumpPropOn`: 2 s cache.
  - `SliceLog`: one `write(2)` per line.
- **Readers.**
  - `SliceRead`, `SliceReadT`, `SliceReadable`.
  - `SliceReadStdString`: refuses SSO over 15, capacity below len, no NUL, a string over `maxLen`.
  - `SliceReadStdVector`: order, stride divisibility including capacity, `maxCount`, element range
    readable.
  - `SliceWalkStdMap`: libstdc++ `_Rb_tree_increment`, `maxNodes`, cycle bound, node count and
    rightmost must match.
  - `SliceStdFunctionParts`.
- **Callers.**
  - `SliceIsScriptCaller`: the exact 34.
  - `SliceAddCallerKind`: for logs only (C-SCRIPT-4).
  - `SliceFactories`, `SliceFactoryByRva`, `SliceFactoryByTag`, `SliceCommandTag`.

### 12.4 The decision at Add

The multiplayer policy is mandatory cancellation and replay. The Add barrier covers
verified direct player caller windows, including unhandled factories; explicitly
classified script sinks, deterministic simulation, menu bookkeeping and saves are
outside that barrier. This classification does not establish complete coverage of
indirect or previously unverified GUI paths.

After a pointer match, the detour validates the command tag, callback policy and
`prepareCancel` transaction. A failure returns a game-constructed empty Connection
without calling original Add and reports `Blocked`. The failure path never invokes
an arbitrary game callback: decoder-owned `landed` cleanup must prove any counter
or tool state it changes. A missing decoder on a verified player Add is blocked too.

Successful cancellation follows `Never`, `IfPresent` or `Required`, calls
`beforeFire` when appropriate, and reports `CancelledFired` or `CancelledNotFired`.
`Mismatch` and `Superseded` retire stale arms; a mismatched *player* Add still faces
the default barrier. `RanNatively` remains only for non-session harness/pass-through
behavior. The existing `ARMED 1` protocol is unchanged; no generic proposal format
or expanded fields are part of this port.

### 12.5 Off-game tests

Kept in the implementer's scratch directory, not in the repository:

- `test_core.cpp` runs one scenario per process.
- `fake_area.cpp` and the loader check `SLICE_AREA` inside a shared object linked with the production
  flags.
- All six scenarios pass (61 + 112 + 95 + 14 + 14 + 12 checks), and the shared object reports its
  three entries in name order.

| scenario | what it runs |
|---|---|
| files | log truncation; the lock against a second process; identity (port line absent or present, CRLF, pid on any line, foreign pid, bad letters); session (missing, `peer=?`, empty, 20 s stale, from the future, the other letter); the cfg strict parse (every Windows case, root before data dir, a directory in the root); inject records; 8 threads of 300 `ARMED 1` records plus another process appending 2000 Lua-style lines, every record found whole and adjacent |
| readers | both mechanisms: self-test; unmapped, `PROT_NONE`, straddling and 1 MB reads; `std::string` SSO, heap, empty, 100 KB, forged objects; `std::vector` empty, reserved, stride 4 and 0x38, forged; `std::map` 1 node, ~2000 random nodes, early stop, `maxNodes`, an injected cycle, a wrong count, an unreadable parent; `std::set<std::string>`; `std::function` small and heap functors called through `_M_invoker` |
| hooks | a fake image with the real prologues of Add and all 37 factories (each followed by its exact undo and a jump to a stand-in), the real `Connection()` bytes calling a stand-in operator new, and the script call stubs. The real install path, then: all argument registers through five prologue shapes; two areas' handlers in order; no re-entry; a script caller refused; the policy table above (tag mismatch, empty, null and foreign-invoker `done`, mismatch, supersession); the thread-local arm; game exceptions through both detours; an area's own hook; `SliceShipAndArm` live and not live; the log lines |
| noconn / noreads / badprologue | `Connection()` corrupted (Add not hooked, arms refused, `ARMED 0` shipped, the `needsCancel` hook skipped); readers off (Add hooked, arms refused); one factory prologue corrupted (only that factory skipped) |

### 12.6 What stays off or unverified

- **Guarded readers** in the container: decided by the init self-test (C-READ-2). While they fail,
  every arm is refused.
- **SaveGame and Book** get no handlers (C-FAC-4).
- **Threads (C-ADD-12)** are not assumed: arms are per thread and every first hit is logged.
- **The script sinks (C-SCRIPT-4)** are counted and logged only.
- **Firing `done` early** is per tool, left to the decoders (C-ADD-11).
- **Nothing in this section has run in the game.**

---

## Appendix A: every `CommandList::Add` call site

Generated from the disassembly.

- **where:** the function's own game signature or source, else the nearest preceding function
  with a source string ("nbr").
- **factory (tag):** the factory whose call precedes Add in the same function with no call in
  between.
- **done manager (static):** every value written to the `std::function`'s manager slot before the
  call, in address order: `0` = empty; an address = a lambda manager; `copy` = copied from
  another object; `?` = the object is not a local the scan could follow.
- **result:** what the caller does with the returned Connection.

| Add returns to | caller | where | factory (tag) | factory.rdi == Add.rdx | done manager (static) | result |
|---|---|---|---|---|---|---|
| `0xa2f5c2` | `0xa2f500` | Game.cpp (nbr) | - | NO: script sink, Command moved twice (0xa2f537, 0xa2f5a5) | 0/copy | ~Connection now |
| `0xdd4ec3` | `0xdd4590` | UI::Bulldozer::Apply | 0x15ee930 (0xf) | yes | 0xdd1270 | ~Connection now |
| `0xe0b18a` | `0xe0b100` | UI/Actions/CameraAction.cpp (nbr) | 0x15eb600 (0x0) | yes | 0 | ~Connection now |
| `0xe13c07` | `0xe13ab0` | UI::CameraAction::Play | 0x15eb600 (0x0) | yes | 0 | ~Connection now |
| `0xe13d9b` | `0xe13ab0` | UI::CameraAction::Play | 0x15eb600 (0x0) | yes | 0 | ~Connection now |
| `0xe14c0f` | `0xe14a00` | UI::CameraAction::Record | 0x15eb600 (0x0) | yes | 0 | ~Connection now |
| `0xe34895` | `0xe343c0` | UI::ConstructionBuilder::MousePressed | 0x15ee930 (0xf) | yes | 0xe27060 | ~Connection now |
| `0xe4f6f2` | `0xe4eb30` | UI::ModuleBuilder::MousePressed | 0x15ee930 (0xf) | yes | 0/0xe4d3f0 | ~Connection now |
| `0xe59657` | `0xe59490` | UI/Actions/ProposalAction.cpp (nbr) | 0x15ee930 (0xf) | yes | 0xe57670 | ~Connection now |
| `0xe86487` | `0xe861b0` | UI::StreetBuilder::UpdateEngine | 0x15ee930 (0xf) | yes | 0xe6eda0 | ~Connection now |
| `0xeaafb2` | `0xeaabb0` | UI/Actions/StreetTerminalBuilder.cpp (nbr) | 0x15ee930 (0xf) | yes (via [rbp-0x1868], 0xeaaf6d) | 0xea53c0 | ~Connection now |
| `0xec057b` | `0xebfe70` | UI/Actions/TerrainPainter.cpp (nbr) | 0x15ee340 (0x11) | yes | 0/0xebf420 | ~Connection now |
| `0xed6ddd` | `0xed69d0` | UI::TrackModifier::Build | 0x15ee930 (0xf) | yes | 0xec62c0 | ~Connection now |
| `0xf01855` | `0xf017d0` | UI/Actions/bulldozer/ModuleBulldozerAction.cpp (nbr) | 0x15ec420 (0x12) | yes | 0 | ~Connection now |
| `0xf229d7` | `0xf225c0` | UI/Components/AddModuleComp.cpp (nbr) | 0x15ee930 (0xf) | yes | 0 | ~Connection now |
| `0xf6b8b1` | `0xf6b750` | UI/Components/Clock.cpp (nbr) | 0x15eb600 (0x0) | yes | 0xf6ae90 | ~Connection now |
| `0xf6bd08` | `0xf6b9b0` | UI/Components/Clock.cpp (nbr) | 0x15eb7b0 (0x1a) | yes | 0xf6ae50 | ~Connection now |
| `0xf6c2b0` | `0xf6c1e0` | UI::Clock::TogglePause | 0x15eb600 (0x0) | yes | 0 | ~Connection now |
| `0xf7042f` | `0xf70320` | UI/Components/Clock.cpp (nbr) | 0x15eb690 (0x1) | yes | 0 | ~Connection now |
| `0xfcf4c8` | `0xfcf3b0` | UI/Components/FinancesComp.cpp (nbr) | 0x15ecd80 (0x1f) | yes | 0xfcef70 | ~Connection now |
| `0xfcf5d2` | `0xfcf3b0` | UI/Components/FinancesComp.cpp (nbr) | 0x15ecd80 (0x1f) | yes | 0xfcef70/0 | ~Connection now |
| `0xfcf7ab` | `0xfcf690` | UI/Components/FinancesComp.cpp (nbr) | 0x15ecd80 (0x1f) | yes | 0xfcefb0 | ~Connection now |
| `0xfcf8ba` | `0xfcf690` | UI/Components/FinancesComp.cpp (nbr) | 0x15ecd80 (0x1f) | yes | 0xfcefb0/0 | ~Connection now |
| `0xfe796f` | `0xfe77b0` | UI::CGameUI::GameStep | 0x15eb600 (0x0) | yes | 0xfdc020/0 | ~Connection now |
| `0xfe7b0e` | `0xfe7a20` | UI/Components/GameUI.cpp (nbr) | 0x15ecd80 (0x1f) | yes | 0 | ~Connection now |
| `0xfeba31` | `0xfeb560` | UI::CGameUI::AutoSave | 0x15ed140 (0x1b) | yes | 0/0xfe35f0 | ~Connection now |
| `0x102f892` | `0x102f720` | UI/Components/GameUI_menu.cpp (nbr) | 0x15ec420 (0x12) | yes | 0 | ~Connection now |
| `0x102fbf9` | `0x102f720` | UI/Components/GameUI_menu.cpp (nbr) | 0x15ee340 (0x11) | yes | 0/0x102e7b0 | ~Connection now |
| `0x1030a76` | `0x10302e0` | UI/Components/GameUI_menu.cpp (nbr) | 0x15ed8b0 (0x19) | yes | 0/0x102e6d0 | ~Connection now |
| `0x1045db8` | `0x1045460` | UI::CGameUI::CreateConstructionMenu | - | NO: Command pushed into a vector (0x10526d0) then moved (0x15d8ea0) | 0 | ~Connection now |
| `0x1045ed0` | `0x1045460` | UI::CGameUI::CreateConstructionMenu | - | NO: moved from a vector (0x15d8ea0) | 0/0x102e880 | ~Connection now |
| `0x1073ffb` | `0x10738f0` | UI::GenerateStreetsComp::GenerateStreetsComp | 0x15ec610 (0x16) | yes | ? | ~Connection now |
| `0x10a95ec` | `0x10a8b90` | UI::{anonymous}::CreateSaveGameLayout | 0x15ed140 (0x1b) | yes | 0/0x10aa380 | ~Connection now |
| `0x10bec71` | `0x10bebc0` | UI/Components/LineEditor.cpp (nbr) | 0x15ecb40 (0x1c) | yes | 0 | ~Connection now |
| `0x10bf850` | `0x10bf620` | UI/Components/LineEditor.cpp (nbr) | 0x15f0050 (0x5) | yes (r12, 0x10bf7ad) | 0 | ~Connection now |
| `0x10c06a0` | `0x10c0400` | UI/Components/LineEditor.cpp (nbr) | 0x15f0050 (0x5) | yes | 0 | ~Connection now |
| `0x10c1796` | `0x10c12c0` | UI::LineEditor::DeleteTerminal | 0x15f0050 (0x5) | yes | 0 | ~Connection now |
| `0x10c196c` | `0x10c12c0` | UI::LineEditor::DeleteTerminal | 0x15f0050 (0x5) | yes | 0 | ~Connection now |
| `0x10c2624` | `0x10c1a40` | UI/Components/LineEditor.cpp (nbr) | 0x15f0050 (0x5) | yes | 0x10ba710 | ~Connection now |
| `0x10c3083` | `0x10c2bb0` | UI/Components/LineEditor.cpp (nbr) | 0x15f0050 (0x5) | yes | 0x10ba750 | ~Connection now |
| `0x10c9e43` | `0x10c9b50` | UI/Components/LineEditor.cpp (nbr) | 0x15f0050 (0x5) | yes | 0 | ~Connection now |
| `0x10ca703` | `0x10ca2a0` | UI/Components/LineEditor.cpp (nbr) | 0x15f0050 (0x5) | yes | 0/0x10bb3d0 | ~Connection now |
| `0x10d4c34` | `0x10d4b60` | UI/Components/LineEditor.cpp (nbr) | 0x15efda0 (0x3) in line_util | yes, through line_util 0x2eb0fa0 -> CreateLine (ret 0x2eb1c49) | copy | COPIED (0x31902c0 at 0x10d4c86), stored via 0x312cc30, then destroyed |
| `0x10d9dc4` | `0x10d9d00` | UI/Components/LineManager.cpp (nbr) | 0x15efda0 (0x3) in line_util | yes, through line_util 0x2eb0fa0 -> CreateLine (ret 0x2eb1c49) | 0x10d8db0 | ~Connection now |
| `0x110dbfe` | `0x110d970` | UI/Components/LoadGamePage.cpp (nbr) | 0x15ebc00 (0x2) | yes | 0/0x110efd0 | ~Connection now |
| `0x112245e` | `0x11223e0` | UI/Components/MenuUI.cpp (nbr) | 0x15eb600 (0x0) | yes | 0 | ~Connection now |
| `0x11225a9` | `0x11224e0` | UI/Components/MenuUI.cpp (nbr) | - | NO: script sink, Command moved twice (0x1122517, 0x112258c) | 0/copy | ~Connection now |
| `0x112c119` | `0x112ba00` | UI::CMenuUI::SwitchToGameUI | 0x15eb600 (0x0) | yes | ? | ~Connection now |
| `0x11d1594` | `0x11d1070` | UI/Components/ModsBrowser.cpp (nbr) | 0x15ecd80 (0x1f) | yes | 0/0x11d0f10 | ~Connection now |
| `0x1207361` | `0x1206ba0` | UI/Components/SectionTypeComp.cpp (nbr) | 0x15f0050 (0x5) | yes | ? | ~Connection now |
| `0x12193fa` | `0x1218c40` | UI/Components/StationGroupDisplayComp.cpp (nbr) | 0x15f0050 (0x5) | yes | ? | ~Connection now |
| `0x122e129` | `0x122e080` | UI/Components/TownConnectionComp.cpp (nbr) | 0x15ec520 (0x14) | yes | 0 | ~Connection now |
| `0x122e381` | `0x122e1e0` | UI/Components/TownConnectionComp.cpp (nbr) | 0x15ed6c0 (0x15) | yes | 0 | ~Connection now |
| `0x122e9ce` | `0x122e470` | UI/Components/TownConnectionComp.cpp (nbr) | 0x15ed6c0 (0x15) | yes | 0 | ~Connection now |
| `0x1233836` | `0x12335b0` | UI/Components/TownEditorComp.cpp (nbr) | 0x15ed6c0 (0x15) | yes | 0 | ~Connection now |
| `0x1233d56` | `0x1233ad0` | UI/Components/TownEditorComp.cpp (nbr) | 0x15ed6c0 (0x15) | yes | 0 | ~Connection now |
| `0x1240a17` | `0x1240730` | UI/Components/TrafficControlComp.cpp (nbr) | 0x15ee930 (0xf) | yes (via [rbp-0x14c0], 0x12409d6) | 0 | ~Connection now |
| `0x1240ece` | `0x1240bf0` | UI/Components/TrafficControlComp.cpp (nbr) | 0x15ee930 (0xf) | yes (via [rbp-0x14c0]) | 0 | ~Connection now |
| `0x1241437` | `0x12410b0` | UI/Components/TrafficControlComp.cpp (nbr) | 0x15ee930 (0xf) | yes | 0 | ~Connection now |
| `0x12469d4` | `0x1246930` | UI/Components/TrafficLayerComp.cpp (nbr) | 0x15ecb40 (0x1c) | yes | 0 | ~Connection now |
| `0x126f3a3` | `0x126f230` | UI/Components/VehicleManager.cpp (nbr) | 0x15ec000 (0x9) | yes | 0/0x126dbc0 | ~Connection now |
| `0x126f653` | `0x126f4e0` | UI/Components/VehicleManager.cpp (nbr) | 0x15ecb40 (0x1c) | yes | 0/0x126db00 | ~Connection now |
| `0x127425b` | `0x1274130` | UI/Components/VehicleManager.cpp (nbr) | 0x15ec220 (0xb) | yes | 0/0x126d8d0 | ~Connection now |
| `0x127bd29` | `0x127b7a0` | transport/util.h (nbr) | 0x15ef8c0 (0xe) | yes | ? | ~Connection now |
| `0x127c023` | `0x127b7a0` | transport/util.h (nbr) | 0x15ef3b0 (0xd) | yes | ? | ~Connection now |
| `0x127c23c` | `0x127b7a0` | transport/util.h (nbr) | 0x15ecf00 (0xc) | yes | ? | ~Connection now |
| `0x127d0f3` | `0x127cc70` | transport/util.h (nbr) | 0x15ecf00 (0xc) | yes | 0x126b140 | ~Connection now |
| `0x12cb3fa` | `0x12cb380` | UI/Components/debug/DebugViewComp.cpp (nbr) | 0x15eb600 (0x0) | yes | 0 | ~Connection now |
| `0x1326ac3` | `0x1326900` | UI/Components/line_ui_util.cpp (nbr) | 0x15ebd00 (0x4) | yes | 0x13228f0 | ~Connection now |
| `0x132754d` | `0x13273f0` | UI/Components/line_ui_util.cpp (nbr) | 0x15ee6d0 (0x1d) | yes | 0/0x1325110 | ~Connection now |
| `0x132bc0a` | `0x132ba10` | UI::CreateStationTerminalComboBox | 0x15f0050 (0x5) | yes | 0 | ~Connection now |
| `0x132bef2` | `0x132bcf0` | UI::CreateWaypointLaneComboBox | 0x15f0050 (0x5) | yes | 0 | ~Connection now |
| `0x132c53e` | `0x132bfe0` | UI::CreateAlternativeTerminalsButton | 0x15f0050 (0x5) | yes | 0/0x1323eb0 | ~Connection now |
| `0x1343e8a` | `0x1342f60` | UI::FinalizeGuiConfiguration | 0x15eb840 (0x21) | yes | ? | ~Connection now |
| `0x1428806` | `0x1428700` | UI/Util/util.cpp (nbr) | 0x15ee6d0 (0x1d) | yes | 0/0x1428480 | ~Connection now |
| `0x142fefc` | `0x142fdd0` | UI/Util/util.cpp (nbr) | 0x15ec220 (0xb) | yes | 0x142f3b0/0/0x142f580 | ~Connection now |
| `0x1431129` | `0x1431020` | UI/Util/vehicle_button_util.cpp (nbr) | 0x15ebf00 (0x8) | yes | 0 | ~Connection now |
| `0x143122b` | `0x1431020` | UI/Util/vehicle_button_util.cpp (nbr) | 0x15ebf00 (0x8) | yes | 0 | ~Connection now |
| `0x1432b5f` | `0x14327c0` | UI/Util/vehicle_button_util.cpp (nbr) | 0x15ecf00 (0xc) | yes | ? | ~Connection now |
| `0x1433521` | `0x1433220` | UI/Util/vehicle_button_util.cpp (nbr) | 0x15ed550 (0x6) | yes | 0/0x1432c60 | ~Connection now |
| `0x1437ee8` | `0x1437b90` | int UI::vehicle_button_util::{anonymous}::GetStopIndexForVeh | 0x15ed550 (0x6) | yes | 0/0x142fbb0 | ~Connection now |
| `0x1438633` | `0x1438260` | UI::vehicle_button_util::SetLineAllInstallHandlers | 0x15ed550 (0x6) | yes | 0/0x1430460 | ~Connection now |
| `0x144409d` | `0x1444000` | UI/ViewCreator.cpp (nbr) | 0x15ecb40 (0x1c) | yes | 0 | ~Connection now |
| `0x14441ad` | `0x1444110` | UI/ViewCreator.cpp (nbr) | 0x15ecb40 (0x1c) | yes | 0 | ~Connection now |
| `0x14442bd` | `0x1444220` | UI/ViewCreator.cpp (nbr) | 0x15ecb40 (0x1c) | yes | 0 | ~Connection now |
| `0x14443be` | `0x1444330` | UI/ViewCreator.cpp (nbr) | 0x15ebe00 (0x7) | yes | 0 | ~Connection now |
| `0x14465fe` | `0x1446460` | UI/ViewCreator.cpp (nbr) | 0x15ed550 (0x6) | yes | 0 | ~Connection now |
| `0x145e1e1` | `0x145ddc0` | UI::{anonymous}::ActualViewCreator::CreateSignalView | 0x15ee930 (0xf) | yes | 0 | ~Connection now |
| `0x145e577` | `0x145e380` | UI/ViewCreator.cpp (nbr) | 0x15ee930 (0xf) | yes | 0 | ~Connection now |
| `0x145efb7` | `0x145ece0` | UI/ViewCreator.cpp (nbr) | 0x15ee930 (0xf) | yes | 0/0x1442090 | ~Connection now |
| `0x145fbf3` | `0x145f8c0` | UI/ViewCreator.cpp (nbr) | 0x15ee930 (0xf) | yes | 0/0x1442220 | ~Connection now |
| `0x1789a7d` | `0x1789880` | ecs/animal_util.cpp (nbr) | 0x15eb8d0 (0x22) | yes | 0 | ~Connection now |
| `0x1d873b2` | `0x1d86ff0` | scripting::AddFunction | 0x15ee130 (0x20) | yes | 0x1d86ba0 | ~Connection now |
| `0x2fbb309` | `0x2fbae50` | transport/vehicle_util_2.cpp (nbr) | 0x15ef8c0 (0xe) | yes | 0 | ~Connection now |

## Appendix B: the 37 factories

Generated from the disassembly.

- **tag:** the byte written at `payload + 0xd48` before the packaging call (`0x15ebab0` or
  `0x15eb570`).
- **steal / bytes:** the prologue `PrologueSteal(code, 14)` accepts. Verify these bytes at run time
  before patching.
- **UI call sites:** non-script `E8` call sites.
- **script call sites:** the §5.1 set.

Names marked INFERRED are explained in §4.3.

| RVA | make_cmd:: | tag | steal | prologue bytes (steal) | UI call sites | script call sites (return address) |
|---|---|---|---|---|---|---|
| `0x15eb600` | SetGameSpeed (INFERRED: callers) | 0x0 | 18 | `f3 0f 1e fa 55 48 89 e5 41 54 53 4c 8d a5 90 f2 ff ff` | 10 | `0x1950f23` |
| `0x15eb690` | SetCalendarSpeed (INFERRED: Clock.cpp caller, int maker) | 0x1 | 18 | `f3 0f 1e fa 55 48 89 e5 41 54 53 4c 8d a5 90 f2 ff ff` | 1 | `0x1950fc1` |
| `0x15ebc00` | UpdateLogo (funcsig) | 0x2 | 14 | `f3 0f 1e fa 55 48 89 e5 41 57 41 56 41 55` | 1 | - |
| `0x15efda0` | CreateLine (funcsig) | 0x3 | 14 | `f3 0f 1e fa 55 48 89 e5 41 57 41 56 41 55` | 1 | `0x1951b68` |
| `0x15ebd00` | DeleteLine (funcsig) | 0x4 | 14 | `f3 0f 1e fa 55 48 89 e5 41 56 41 55 41 54` | 1 | `0x196a7a1` |
| `0x15f0050` | UpdateLine (funcsig) | 0x5 | 14 | `f3 0f 1e fa 55 48 89 e5 41 57 41 56 41 55` | 13 | `0x1975884` |
| `0x15ed550` | SetLine (funcsig) | 0x6 | 14 | `f3 0f 1e fa 55 48 89 e5 41 57 41 56 41 55` | 4 | `0x196b929` |
| `0x15ebe00` | Reverse (funcsig) | 0x7 | 14 | `f3 0f 1e fa 55 48 89 e5 41 56 41 55 41 54` | 1 | `0x196a371` |
| `0x15ebf00` | SetUserStopped (funcsig) | 0x8 | 14 | `f3 0f 1e fa 55 48 89 e5 41 57 41 56 41 55` | 2 | `0x196c234` |
| `0x15ec000` | SetVehicleTargetMaintenanceState (funcsig) | 0x9 | 14 | `f3 0f 1e fa 55 48 89 e5 41 56 41 55 41 54` | 1 | `0x196b43c` |
| `0x15ec150` | SetVehicleShouldDepart (INFERRED: Windows tag 10) | 0xa | 15 | `f3 0f 1e fa 55 48 89 e5 41 56 49 89 f6 41 55` | 0 | `0x196abd1` |
| `0x15ec220` | SendToDepot (funcsig) | 0xb | 14 | `f3 0f 1e fa 55 48 89 e5 41 57 41 56 41 55` | 2 | `0x196c704` |
| `0x15ecf00` | SellVehicle (funcsig) | 0xc | 14 | `f3 0f 1e fa 55 48 89 e5 41 56 41 55 41 54` | 3 | `0x1969f2c` |
| `0x15ef3b0` | BuyVehicle (funcsig) | 0xd | 14 | `f3 0f 1e fa 55 48 89 e5 41 57 41 56 41 55` | 1 | `0x1974841` |
| `0x15ef8c0` | ReplaceVehicle (funcsig) | 0xe | 14 | `f3 0f 1e fa 55 48 89 e5 41 57 41 56 41 55` | 2 | `0x1974ffc` |
| `0x15ee930` | BuildProposal (INFERRED: Windows tag 15, 17 builder callers) | 0xf | 14 | `f3 0f 1e fa 55 48 89 e5 41 57 41 56 41 55` | 17 | `0x1971333` |
| `0x15ec320` | RemoveField (funcsig) | 0x10 | 14 | `f3 0f 1e fa 55 48 89 e5 41 56 41 55 41 54` | 0 | `0x1969ab1` |
| `0x15ee340` | unnamed | 0x11 | 14 | `f3 0f 1e fa 55 48 89 e5 41 57 41 56 41 55` | 2 | `0x1977628` |
| `0x15ec420` | RemoveTown (funcsig) | 0x12 | 14 | `f3 0f 1e fa 55 48 89 e5 41 56 41 55 41 54` | 2 | `0x1969681` |
| `0x15eb720` | unnamed | 0x13 | 18 | `f3 0f 1e fa 55 48 89 e5 41 54 53 4c 8d a5 90 f2 ff ff` | 0 | `0x196f9d3` |
| `0x15ec520` | unnamed | 0x14 | 14 | `f3 0f 1e fa 55 48 89 e5 41 57 41 56 41 55` | 1 | `0x196e056` |
| `0x15ed6c0` | unnamed | 0x15 | 15 | `f3 0f 1e fa 55 48 89 e5 41 57 41 89 d7 41 56` | 4 | `0x196f04c` |
| `0x15ec610` | ConnectTownsAndIndustries (INFERRED: Windows tag 22) | 0x16 | 15 | `f3 0f 1e fa 55 48 89 e5 41 57 45 89 c7 41 56` | 1 | `0x19763ff` |
| `0x15ec940` | SetSimBuildingManualDevelopment (funcsig) | 0x17 | 14 | `f3 0f 1e fa 55 48 89 e5 41 57 41 56 41 55` | 0 | `0x196cbd4` |
| `0x15eca40` | SetSimBuildingClosureTimeStamp (funcsig) | 0x18 | 14 | `f3 0f 1e fa 55 48 89 e5 41 57 41 56 41 55` | 0 | `0x196b033` |
| `0x15ed8b0` | unnamed | 0x19 | 16 | `f3 0f 1e fa 55 48 89 e5 41 57 49 89 f7 48 89 d6` | 1 | `0x1973c70` |
| `0x15eb7b0` | SetDate (INFERRED: gregorian::date getter maker, Clock.cpp caller) | 0x1a | 18 | `f3 0f 1e fa 55 48 89 e5 41 54 53 4c 8d a5 90 f2 ff ff` | 1 | `0x1972ad2` |
| `0x15ed140` | SaveGame (INFERRED: AutoSave, save dialog) | 0x1b | 15 | `f3 0f 1e fa 55 48 89 e5 41 57 49 89 cf 41 56` | 2 | - |
| `0x15ecb40` | SetColor (funcsig) | 0x1c | 14 | `f3 0f 1e fa 55 48 89 e5 41 57 41 56 41 55` | 6 | `0x196e662` |
| `0x15ee6d0` | SetName (funcsig) | 0x1d | 14 | `f3 0f 1e fa 55 48 89 e5 41 57 41 56 41 55` | 2 | `0x196dad8` |
| `0x15ecc80` | SetVehicleManualDeparture (funcsig) | 0x1e | 14 | `f3 0f 1e fa 55 48 89 e5 41 57 41 56 41 55` | 0 | `0x196bd64` |
| `0x15ecd80` | Book (funcsig) | 0x1f | 14 | `f3 0f 1e fa 55 48 89 e5 41 57 41 56 41 55` | 6 | `0x1973034` |
| `0x15ee130` | SendScriptEvent (INFERRED: legacy AddFunction, strings) | 0x20 | 15 | `f3 0f 1e fa 55 48 89 e5 41 57 49 89 d7 41 56` | 0 | `0x196d43f`, `0x1d8737a` |
| `0x15eb840` | unnamed | 0x21 | 18 | `f3 0f 1e fa 55 48 89 e5 41 54 53 4c 8d a5 90 f2 ff ff` | 1 | - |
| `0x15eb8d0` | unnamed | 0x22 | 18 | `f3 0f 1e fa 55 48 89 e5 41 54 53 4c 8d a5 90 f2 ff ff` | 1 | `0x1951063` |
| `0x15eb980` | unnamed | 0x23 | 18 | `f3 0f 1e fa 55 48 89 e5 41 54 53 4c 8d a5 90 f2 ff ff` | 0 | `0x197023e` |
| `0x15eba20` | unnamed | 0x24 | 18 | `f3 0f 1e fa 55 48 89 e5 41 54 53 4c 8d a5 90 f2 ff ff` | 0 | - |
