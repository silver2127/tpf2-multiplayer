# Slice, vehicles area: the Linux map

Linux RE for the slice's vehicle commands. Scope: the five `make_cmd` factories BuyVehicle,
SellVehicle, ReplaceVehicle, SendToDepot and Reverse; the `TransportVehicleConfig` decode; the packaged
`Command` (variant index and BuyVehicle's result slot); the buy completion callback (the clone's line);
the callers the hooks classify by; and the inject lines the Lua side parses. This replaces
`native/src/slice_hook.cpp` ~1093-1184 (VBUY config helpers), ~1335-1397 and ~1432-1434 (buy callback
thunk, VSELL/VREPL/VDEPOT/VREV writers), ~1483-1495 (`VehiclePayloadReadable`), ~1607-1729 (the vehicle
half of `CaptureFactory`, including the stack-argument reads ~1610-1695), ~3441-3475 (the buy callback at
Add), the id 2/3/4/5/10 rows of `FACTORIES[]` and the matching branch of `DeferHandler` ~3574-3619 for
the Linux build.

Binary: `TransportFever2`, Steam build 35924, GNU build-id `3a0e156390b0e6f1e372051c24802c8493ae454a`.
Addresses are Linux RVAs (the PIE links at 0; the first `PT_LOAD` maps file offset 0 at vaddr 0, so an
RVA in `.text` is also its file offset; live = image base + RVA). "Windows" means the RVAs in
`docs/re/COMMANDS.md` and `slice_hook.cpp`, relative to `0x140000000`.

Status labels: **PROVEN** = the instructions, strings or data quoted here show it and can be
re-derived from the binary; **INFERRED** = placed by address order, by the Windows docs, by a
function's role or by elimination, so it stays out of patch code; **UNPROVEN** = not established, the
code leaves it off. Nothing here was checked in the running game (static MAP stage, revised after
independent verification; section 10 lists what changed).

## How the evidence was produced

- Disassembly: capstone (x86-64) linear sweep from each function start over its FDE size from
  `/home/topsnek/tpf2-re/linux/functions.csv`; RIP-relative string loads resolved and named.
- References to a function start: in both executable `PT_LOAD` segments (`0..0x59a6de0` and
  `0x5bc1000..0x5bd912a`), every `e8`/`e9`/`0f 8x` rel32 and `7x`/`eb`/`e0-e3` rel8 byte candidate whose
  target is in range, kept only on an instruction boundary of its containing FDE; every 4-byte window
  that resolves RIP-relatively into the range with 0-4 trailing immediate bytes, attributed to its
  section and covering instruction; every relocation addend; the raw 8-byte address anywhere in the
  file; `.dynsym`/`.symtab` values inside the bodies.
- Lambda names: a `std::function` manager's op 0 returns its typeinfo with `lea rax,[rip+X]`; the
  `R_X86_64_RELATIVE` relocation at `X+8` points at the mangled type name, which names the enclosing
  function with its full parameter list.
- Parameter locations: SysV applied to that parameter list, checked against the call sites (the number
  of register loads and pushes must match exactly).
- Steal: `hook_posix.cpp` compiled into a harness that feeds `PrologueSteal(code, 14)` the bytes read
  from the game file at each RVA.
- Struct offsets: the sol2 registration `scripting::RegisterUsertypesVehicle` `0x2dcc290`, whose
  member pointers are constants handed to the usertype templates (types from the template signatures
  in `funcsig.csv`); cross-checked against the factories' element copy and destructor loops, and against
  the engine's own size asserts.
- The throwaway scripts live in this area's scratch directory (`vre.py`, `refscan.py`, `refscan2.py`,
  `tinfo.py`, `codeptr.py`, `steal_test.cpp`); the method above is enough to redo any row.

## 1. The five factory hooks

Every factory starts `endbr64; push rbp; mov rbp,rsp;` then three 2-byte `push r1x`, and the first 14
bytes end exactly on an instruction boundary. `PrologueSteal(code, 14)` returns 14 for all five
(PROVEN, harness output below).

| hook id | factory | Windows RVA / steal | Linux RVA | FDE size | first 14 bytes (stolen) | next 8 bytes | steal |
|---|---|---|---|---|---|---|---|
| 2 | BuyVehicle | `0x9dca00` / 15 | `0x15ef3b0` | 1296 | `f30f1efa 55 4889e5 4157 4156 4155` | `41 54 53 48 81 ec f8 0d` | 14 |
| 3 | SellVehicle | `0x9de380` / 20 | `0x15ecf00` | 472 | `f30f1efa 55 4889e5 4156 4155 4154` | `53 48 81 ec 80 0d 00 00` | 14 |
| 4 | ReplaceVehicle | `0x9dddb0` / 15 | `0x15ef8c0` | 1241 | `f30f1efa 55 4889e5 4157 4156 4155` | `41 54 53 48 81 ec f8 0d` | 14 |
| 5 | SendToDepot | `0x9de6f0` / 20 | `0x15ec220` | 255 | `f30f1efa 55 4889e5 4157 4156 4155` | `41 54 53 48 81 ec 88 0d` | 14 |
| 10 | Reverse | `0x9ddfe0` / 20 | `0x15ebe00` | 241 | `f30f1efa 55 4889e5 4156 4155 4154` | `53 48 81 ec 80 0d 00 00` | 14 |

Identity (PROVEN): each RVA is the function that loads its own `__PRETTY_FUNCTION__` in its assert
path (`0x2fcb860` is the assert handler):

| factory | signature string (site) | assert (site) |
|---|---|---|
| BuyVehicle | `Command make_cmd::BuyVehicle(const ecs::Engine&, ecs::Entity, ecs::Entity, TransportVehicleConfig)` (`0x15ef81f`, `0x15ef83e`) | `depotEntity != ecs::Entity()` (`0x15ef832`), `playerEntity != ecs::Entity()` (`0x15ef851`) |
| SellVehicle | `Command make_cmd::SellVehicle(const ecs::Engine&, const std::vector<ecs::Entity>&)` (`0x15ed097`) | `!vehicleEntities.empty()` (`0x15ed0aa`) |
| ReplaceVehicle | `Command make_cmd::ReplaceVehicle(const ecs::Engine&, ecs::Entity, TransportVehicleConfig, bool)` (`0x15efd17`) | `vehicleEntity != ecs::Entity()` (`0x15efd2a`) |
| SendToDepot | `Command make_cmd::SendToDepot(const ecs::Engine&, ecs::Entity, bool)` (`0x15ec2ef`) | `vehicleEntity != ecs::Entity()` (`0x15ec302`) |
| Reverse | `Command make_cmd::Reverse(const ecs::Engine&, ecs::Entity)` (`0x15ebec1`) | `vehicleEntity != ecs::Entity()` (`0x15ebed4`) |

Hook safety (PROVEN):

- **The five starts have exactly 14 references, all direct `e8` calls: 9 UI sites and 5 Lua makers**
  (section 5):
  - BuyVehicle `0x127bffb`, `0x197483c`;
  - ReplaceVehicle `0x127bd01`, `0x2fbb2dd`, `0x1974ff7`;
  - SellVehicle `0x127c214`, `0x127d0cb`, `0x1432b34`, `0x1969f27`;
  - SendToDepot `0x127422f`, `0x142fed4`, `0x196c6ff`;
  - Reverse `0x1444396`, `0x196a36c`.
- Nothing lands inside a stolen range `[start, start+14)` in either executable segment:
  - no `e9`, `0f 8x` or rel8 branch targets it (as a sanity check of the scan, 13 rel8-shaped bytes aim
    within 64 bytes of a start, none inside), and no branch inside the five bodies does;
  - no RIP-relative operand resolves into it. Of the 146 `.text` windows that would, 14 are the
    displacements of the calls above and 128 sit inside unrelated instructions. The other 4 are the
    zero bytes of the multi-byte `nop` padding just before `0x15ebe00` and `0x15ef3b0`, outside every FDE.
- The 25 `.eh_frame` windows are the `pc_begin` fields of the five factories' own FDEs (`0x56af4c0`,
  `0x56af588`, `0x56af780`, `0x56af9c8`, `0x56afa00`; `pc_range` = FDE size), read only by the unwinder.
- No relocation addend points into a stolen range, there is no raw 8-byte copy of such an address in
  the file, and no symbol lies inside a body.
- No other function loads a factory's `__PRETTY_FUNCTION__` string (`xrefs.csv`: the sites in the
  identity table only), so no inlined copy of a factory's asserts exists elsewhere.
- The stolen instructions are rsp-relative only (pushes, `mov rbp,rsp`); the relay must restore rsp
  to its entry value before jumping to the trampoline.

### Argument registers (SysV; `Command` is returned through a hidden pointer)

On Windows `rcx` was the return slot, `rdx` the Engine, `r8`/`r9` the next two arguments and BuyVehicle's
config a by-value copy on the caller's stack (`st[0]` = `[calleeRsp+0x28]`, `CaptureFactory` ~1610).
On Linux the return slot is `rdi`, all five fit in integer registers, and **no stack or xmm argument is
read**. A non-trivially-copyable by-value `TransportVehicleConfig` is a pointer to the caller's
temporary. `ecs::Entity` is a 32-bit int by value (the bodies test `edx`/`ecx`); read the low 32 bits.

| factory | rdi | rsi | rdx / edx | rcx / ecx | r8 | caller's object after entry |
|---|---|---|---|---|---|---|
| BuyVehicle | `Command*` ret | `const Engine&` | player (edx) | depot (ecx) | `TransportVehicleConfig*` | **emptied** (both vectors moved out) |
| SellVehicle | `Command*` ret | `const Engine&` | `const std::vector<ecs::Entity>*` | | | unchanged (copied) |
| ReplaceVehicle | `Command*` ret | `const Engine&` | vehicle (edx) | `TransportVehicleConfig*` | bool (r8d) | **emptied** |
| SendToDepot | `Command*` ret | `const Engine&` | vehicle (edx) | sellOnArrival (ecx, bool) | | |
| Reverse | `Command*` ret | `const Engine&` | vehicle (edx) | | | |

Every factory returns its `rdi` in `rax`: BuyVehicle `0x15ef3c8 mov [rbp-0xe08],rdi` … `0x15ef7e2 mov
rax,[rbp-0xe08]`; ReplaceVehicle `0x15ef8d8` … `0x15efcdb`; SellVehicle `0x15ecf32 mov r12,rdi` …
`0x15ed074 mov rax,r12`; SendToDepot `0x15ec259` … `0x15ec2d8`; Reverse `0x15ebe37` … `0x15ebeac`. The
Engine (`rsi`) reaches the packager `0x15ebab0` as `rdx` in each (`0x15ef3cf`→`0x15ef77e`,
`0x15ef8df`→`0x15efc77`, `0x15ecf35`→`0x15ed028`, `0x15ec25c`→`0x15ec27f`, `0x15ebe3a`→`0x15ebe5a`).

- **BuyVehicle** (PROVEN): `0x15ef3e5 cmp edx,-1` → player assert; `0x15ef3f1 cmp ecx,-1` → depot
  assert (both assert calls, `0x15ef839` and `0x15ef858`, fall into `__stack_chk_fail`, so neither
  returns). Player and depot are stored at payload+0 and +4 (`0x15ef461`, `0x15ef467`, copied to the
  variant at `0x15ef518`, `0x15ef535`).
  - `0x15ef401 mov rbx,r8; 0x15ef404 mov rsi,r8; 0x15ef46d call 0x15eb410`: `0x15eb410` zeroes its
    destination and swaps it with `[rsi]`,`[rsi+8]`,`[rsi+0x10]` (`0x15eb41d`..`0x15eb464`), so the
    caller's vehicles vector is left empty.
  - `0x15ef472`..`0x15ef4ab` move `[r8+0x18]`,`[r8+0x20]`,`[r8+0x28]` (vehicleGroups) out and write 0.
  - **The detour must decode the config before it calls the trampoline.**
  - The caller owns the temporary: HandleVehicleChange `0x127bf16 lea r12,[rbp-0x150]`, filled at
    `0x127bfd8`, passed `0x127bff2 mov r8,r12`, destroyed with `0xaa2f20` after Add (`0x127c036`); the
    Lua maker builds it at `0x197480b` and passes `0x197482c mov r8,r15`.
- **ReplaceVehicle** (PROVEN): `0x15ef8f5 cmp edx,-1` → vehicle assert; `0x15ef905 mov rbx,rcx;
  0x15ef908 mov rsi,rcx; 0x15ef96b call 0x15eb410` and `0x15ef970`..`0x15ef9a9` empty the config exactly as
  BuyVehicle does; `0x15ef912 mov r12d,r8d; 0x15ef9c1 xor r12d,1; 0x15ef9d4 mov [rbp-0xda8],r12b` stores
  the inverted bool at payload+0x38 (`0x15efc96`). Callers pass `r8d` 0 (UI `0x127bcf5`, Lua
  `0x1974fe7`) or 1 (missing-resources dialog `0x2fbb2d1`); with 1 the apply handler skips its
  price-difference booking (section 2).
- **SellVehicle** (PROVEN): `0x15ecf25 mov rax,[rdx]; 0x15ecf28 cmp [rdx+8],rax; je` → assert;
  `0x15ecf3f mov rsi,rdx` → copy `0xaa7410`; 4-byte elements (`0x15ecfa3 sar rax,2`, int copy loop
  `0x15ed00b`..`0x15ed00e`).
- **SendToDepot** (PROVEN): `0x15ec247 cmp edx,-1` → assert; `0x15ec26f mov r15d,ecx; 0x15ec295 mov
  [rbp-0xd8c],r15b` (payload+4, one byte). The name `sellOnArrival` is the Lua maker's parameter name
  (`0x19658d4`, right before `sendToDepot` `0x196590e`, section 5).
- **Reverse** (PROVEN): `0x15ebe25 cmp edx,-1` → assert; vehicle at payload+0 (`0x15ebe60`).

## 2. The packaged Command

This also settles what `SLICE_LINES.md` §7 left open (where the variant index lives).

| offset | field | evidence |
|---|---|---|
| `Command+0x00` | pointer to a 0xd50-byte heap block holding the `CmdData` variant (the "impl") | PROVEN: `0x15eb570` (called by the packager at `0x15ebb1a` with `rdi` = the Command) `0x15eb588 mov edi,0xd50; call operator new; 0x15eb5a2 mov [rbx],rax`; destructor `0x15d8f30`: `0x15d8f76 mov rbx,[rbx]`, `0x15d8f9d mov esi,0xd50; jmp operator delete(void*,size_t)` |
| `impl+0x000` | variant storage | PROVEN: the destructor visits `impl` itself (`0x15d8f90 mov rdi,rbx; call [0x59be2a0+rax*8]`) |
| `impl+0xd48` | u8 variant index (0xff = valueless) | PROVEN: `0x15d8f7e movzx eax,byte [rbx+0xd48]; cmp al,0xff`; the packager copies it with the payload (`0x15ebae3`, `0x15ebb09`); Windows: `+0xb18` |
| `Command+0x08` | `std::vector<ecs::EntityRev>` {begin,end,cap}, 0x10-byte elements | PROVEN: packager `0x15ebb41 call 0x325c310(out, engine, entities)`, result moved to `[rbx+8]`,`[rbx+0x10]`,`[rbx+0x18]` (`0x15ebb46`..`0x15ebb88`); the buy callback passes `&cmd+8` to `ecs::Validate(const Engine&, const std::vector<ecs::EntityRev>&, Entity, bool)` (`0x1272396`); element stride `0x325c537 lea rsi,[rax+0x10]` |
| `Command+0x20/+0x28` | a ref-counted handle | PROVEN only as released by the destructor (`0x15d8f40`..`0x15d8f60`); not used by this area |

Variant index per factory (PROVEN: each factory writes the byte into its local payload, whose storage
starts 0xd48 bytes earlier): BuyVehicle **13** (`0x15ef7a3 mov byte [rbp-0x48],0xd`, storage
`rbp-0xd90`), ReplaceVehicle **14** (`0x15efc9c`), SellVehicle **12** (`0x15ed038`, storage `rbp-0xd80`),
SendToDepot **11** (`0x15ec29c`), Reverse **7** (`0x15ebe70`). The order matches the type string
`std::variant<CmdData::SetGameSpeed, CmdData::SetCalendarSpeed, CmdData::UpdateLogo, CmdData::CreateLine,
…, CmdData::Reverse, …, CmdData::SendToDepot, CmdData::SellVehicle, CmdData::BuyVehicle,
CmdData::ReplaceVehicle, CmdData::BuildProposal, …>` (`0x19f4044`) and the Windows dispatch tags.

The entity list the packager turns into revs: `{player, depot}` for BuyVehicle (`0x15ef4bc`..`0x15ef4e7`,
`0x15eb350(out, &{player, depot}, 2)`), `{vehicle}` for Reverse (`0x15ebe52`), SendToDepot (`0x15ec277`)
and ReplaceVehicle (`0x15ef9e9`). `0x325c310` makes each rev with `ecs::MakeEntityRev` `0x325c290`
(`0x325c3fe`), which asserts `entity.GetId() >= 0` (`0x325c2ac test esi,esi; js 0x325c2ea`; the assert
call `0x325c304` falls into `__stack_chk_fail` `0x325c309`), so no rev can hold a negative id.

### CmdData payloads (at `impl+0`)

| type (index) | +0x00 | +0x04 | +0x08 | +0x38 | evidence (PROVEN) |
|---|---|---|---|---|---|
| BuyVehicle (13) | player `int` | depot `int` | `TransportVehicleConfig` (0x30 B) | **result vehicle `int`**: -1 from the factory, the bought vehicle after apply | factory `0x15ef457 mov dword [rbp-0xda8],-1` → `0x15ef79d mov [rbp-0xd58],eax` (storage+0x38); apply handler `Visitor::operator()(CmdData::BuyVehicle&)` `0x15e3800`: pushes `[rbx]` (`0x15e388d`), `[rbx+4]` (`0x15e3889`), `rbx+8` (`0x15e3884`) to `vehicle_util_engine::BuyVehicle` `0x2fc9b80` and stores its result `0x15e3895 mov [rbx+0x38],eax`; the buy callback reads it `0x1272392` |
| ReplaceVehicle (14) | vehicle `int` | | `TransportVehicleConfig` | u8 = !arg4 | `0x15efa1d`, `0x15efc96`; apply `0x15e4970` reads `cmp byte [r12+0x38],0` (`0x15e4aed`, `0x15e4ba5`) |
| SellVehicle (12) | `std::vector<ecs::Entity>` | | | | `0x15ed01e`..`0x15ed031` |
| SendToDepot (11) | vehicle `int` | sellOnArrival `u8` | | | `0x15ec285`, `0x15ec295` |
| Reverse (7) | vehicle `int` | | | | `0x15ebe60` |

### ReplaceVehicle's bool in the apply handler

`bool {anonymous}::Visitor::operator()(CmdData::ReplaceVehicle&) const` `0x15e4970` (its own signature
string at `0x15e4de7`, assert `cmd.vehicleEntity != ecs::Entity()`), `r12` = the payload:

- `0x15e4aed cmp byte [r12+0x38],0; 0x15e4b06 je 0x15e4dc0`: with the byte 0 (arg4 = 1) `0x15e4dc0` sets
  `[rbp-0x3c0]` to 0 and jumps to `0x15e4b8b`, past the two calls below.
- With the byte non-zero (arg4 = 0), `0x2fae560` runs twice: once on `[rbp-0x3a8]+8` (the target
  vehicle's component record) at `0x15e4b3d`, once on `r12+8` (the command's config) at `0x15e4b75`. The
  difference goes to `[rbp-0x3c0]` (`0x15e4b7a`, `0x15e4b81`). `0x2fae560` loads
  `MetadataMap::Get<model_metadata::Cost>` (`0x2fae5a3`), and `vehicle_util_engine::BuyVehicle` calls
  it too (`0x2fc9d52`).
- `0x15e4ba5 cmp byte [r12+0x38],0; je 0x15e4c4b` again skips, for arg4 = 1, a block that builds an
  entry and calls `0x1863b50` (`0x15e4c42`). The entry holds:
  - a carrier from the inlined `journal_util::GetCarrier` (assert `0x15e4e06`);
  - the negated difference (`0x15e4bdb neg`);
  - the constant `0x20603` (`0x15e4c02`).
- `0x1863b50` loads the typeinfo name `N3ecs9component7AccountE` (`0x1863faa`) and calls `0x1861cd0`,
  whose funcsig is `{anonymous}::Insert(std::vector<JournalEntry>&, const JournalEntry&)` (`0x1863fcf`).
- Both paths then call `vehicle_util::ReplaceVehicleParts` `0x2ec7ca0` (`0x15e4d3d`).

PROVEN: arg4 = 1 skips the `0x2fae560` difference and the `0x1863b50` call; arg4 = 0 runs both.
INFERRED from the callee strings: that step charges the replace's price difference to the company
account, so arg4 = 1 is a free replace.

## 3. `TransportVehicleConfig`

### `TransportVehicleConfig` (0x30 B)

| offset | field | type | evidence |
|---|---|---|---|
| +0x00 | vehicles | `std::vector<TransportVehiclePart>` {begin,end,cap} | PROVEN: member pointer 0 |
| +0x18 | vehicleGroups | `std::vector<int>` | PROVEN: member pointer 0x18; 4-byte copy `0x15ef707 sar rdx,2` |
| size | 0x30 | | PROVEN: CmdData::BuyVehicle holds it at +0x08 with the result at +0x38 |

Registration `0x2dcd224 call 0x2decb10`: `rsi='TransportVehicleConfig'` (`0x2dcd212`), `rdx='vehicles'`
(`0x2dcd200`), `rcx=r15`=`&[rbp-0x3e0]` (`0x2dcc2fe`, no later r15 write) holding 0 (`0x2dcd219`),
`r8='vehicleGroups'` (`0x2dcd1f9`), `r9=r14`=`[rbp-0x4b8]` (`0x2dcd1c9`) = the saved `rbx`=`&[rbp-0x3a0]`
(`0x2dcc2e1`, `0x2dcc3a6`) holding 0x18 (`0x2dcd207`). Types: funcsig `0x2dc9a60`
`std::vector<TransportVehiclePart> TransportVehicleConfig::*`, `std::vector<int> TransportVehicleConfig::*`.

### `TransportVehiclePart` (**0x88 B**, Windows 0x80)

| offset | field | type | Windows | evidence |
|---|---|---|---|---|
| +0x00 | part | `VehiclePart` (0x50 B, below) | | PROVEN: member pointer 0 |
| +0x50 | purchaseTime | `long long` | | PROVEN: member pointer 0x50 |
| +0x58 | maintenanceState | float | | PROVEN: member pointer 0x58 |
| +0x5c | targetMaintenanceState | float | | PROVEN: member pointer 0x5c |
| +0x60 | autoLoadConfig | `std::vector<bool>` (libstdc++, 0x28 B) | +0x60, uint32 words | PROVEN: offset and type from the copy and destructors below; name from the engine's size assert (next paragraph) |

Stride 0x88 (PROVEN): BuyVehicle's vector copy `0x15ef549 sar rax,3; 0x15ef54d imul rax,0xf0f0f0f0f0f0f0f1`
(inverse of 17, so size = span / 0x88) and `0x15ef668 add rbx,0x88; 0x15ef66f add r12,0x88`; element
destructor loop in `0x15eb410` `0x15eb49e add rbx,0x88`; HandleVehicleChange `0x127b85d add r12,0x88` and
`0x127bc3a cmp rax,0x88` (one-part config).

Name at +0x60 (PROVEN), in `vehicle_load_config_util::GetPotentialCapacities` `0x2faabb0`:
- `r13` walks a `TransportVehiclePart` vector in 0x88 steps: `0x2fab014 mov r13,[rax+8]`,
  `0x2fab08b add r13,[rbp-0x168]`, `0x2fab529 add [rbp-0x168],0x88`. It is kept through `[rbp-0x1b8]`
  (`0x2fab122`, `0x2fab1e7`, `0x2fab234`, `0x2fab256`).
- `0x2fab2a0`..`0x2fab2ba` computes `([r13+0x10]-[r13+8])>>2`, the size of `part.loadConfig` (VehiclePart
  +0x08). `0x2fab2c9 jne` → `'mdTv.compartments.size() == ve.part.loadConfig.size()'` (`0x2fab7a7`).
- `0x2fab2cf`..`0x2fab2e3` computes `([r13+0x70]-[r13+0x60])*8 + [r13+0x78] - [r13+0x68]`, the bit count
  of the `vector<bool>` at +0x60.
- `0x2fab2e9 jne 0x2fab7b3` → `'ve.autoLoadConfig.size() == ve.part.loadConfig.size()'` (`0x2fab7c6`).

`vehicle_util_engine::UpdateConfigFromModelIds` `0x2fc5d60` makes the same check from
`r15 = &part.loadConfig`:
- `0x2fc5e52 lea r15,[rax+8]`, `0x2fc65ac add r15,0x88`;
- bit count from `[r15+0x58]`..`[r15+0x70]` against `([r15+8]-[r15])>>2`;
- `0x2fc5ff7 jne 0x2fc66d1` → the same string at `0x2fc66e4`.

Element copy in BuyVehicle `0x15ef67b`..`0x15ef668` (PROVEN): `+0x00` dword; `+0x04` byte; `+0x08` 4-byte
vector (`0x15ef6a7 sar rax,2`, `memmove`); `+0x20` qword and `+0x28` dword (12 bytes); `+0x30` string
(`0x15ef624 mov [r12+0x30],rax` with rax = `r12+0x40`, construct `0x9aca50` from `[rbx+0x30]`,`[rbx+0x38]`);
`+0x50` qword; `+0x58`, `+0x5c` `movss`; `+0x60` → `0xb0dc90` (vector<bool> copy). Destructor
(`0x15eb470`..`0x15eb499`, and a userdata `__gc` `0x2dc90c6`..`0x2dc90ef`): frees `[+0x60]`, `[+0x30]` unless
it is `+0x40`, `[+0x08]`.

Member pointers: the metatable storage built at `0x2dcc6aa` (`operator new(0x148)`, `r12`) keeps each
value next to its name in the reverse-ordered libstdc++ tuple: `[+0xb8]=0x5c` (`0x2dcc7ef`) ↔
`'targetMaintenanceState'` `[+0xc0]` (`0x2dcc776`); `[+0xc8]=0x58` (`0x2dcc7fb`) ↔ `'maintenanceState'`
`[+0xd0]` (`0x2dcc785`); `[+0xd8]=0x50` (`0x2dcc807`) ↔ `'purchaseTime'` `[+0xe0]` (`0x2dcc794`,
`0x2dcc813`); `[+0xe8]=0` (`0x2dcc84f`) ↔ `'part'` `[+0xf0]` (`0x2dcc6c6`, `0x2dcc85b`); `'autoLoadConfig'`
`[+0xb0]` (`0x2dcc767`, a property wrapper without a member pointer). Types (funcsig `0x2dc7f50`):
`"part", VehiclePart TransportVehiclePart::*, "purchaseTime", long long int TransportVehiclePart::*, …
float TransportVehiclePart::*, const char (&)[23], float TransportVehiclePart::*, const char (&)[15],
sol::property_wrapper<…<lambda(const TransportVehiclePart&)>, …>` (15 = `"autoLoadConfig"` + NUL).

### `VehiclePart` (0x50 B)

| offset | field | type | Windows | evidence |
|---|---|---|---|---|
| +0x00 | modelId | int | +0x00 DECOMPILED | PROVEN: member pointer 0 |
| +0x04 | reversed | bool | | PROVEN: member pointer 4; not shipped (the wire has no field for it) |
| +0x08 | loadConfig | `std::vector<int>` | +0x08 sweep | PROVEN: member pointer 8 |
| +0x20 | color | `CVec3f` (3 floats) | +0x20 sweep | PROVEN: member pointer 0x20 |
| +0x30 | logo | `std::string` (libstdc++ 32 B) | | PROVEN: member pointer 0x30; not shipped |

Registration `0x2dcc433 call 0x2debde0`: `rsi='VehiclePart'` (`0x2dcc3ea`), `rdx='modelId'` (`0x2dcc3e1`),
`rcx=r13`=`&[rbp-0x440]` (`0x2dcc31d`/`0x2dcc333`) holding 0 (`0x2dcc428`), `r8='reversed'` (`0x2dcc3d2`),
`r9=[rbp-0x498]`=`&[rbp-0x420]` (`0x2dcc314`/`0x2dcc353`) holding 4 (`0x2dcc41d`); stack
`st0='loadConfig'` (`0x2dcc3fb`), `st1=r12`=`&[rbp-0x400]` (`0x2dcc305`) holding 8 (`0x2dcc412`),
`st2='color'` (`0x2dcc3f1`), `st3=r15`=`&[rbp-0x3e0]` holding 0x20 (`0x2dcc407`), `st4='logo'`
(`0x2dcc3d9`), `st5=rbx`=`&[rbp-0x3a0]` (`0x2dcc2e1`; next rbx write `0x2dcc4dd`) holding 0x30 (`0x2dcc3fc`).
Types: funcsig `0x2dc9d60` `int`, `bool`, `std::vector<int>`, `CVec3f`, `std::string` `VehiclePart::*`.

### `std::vector<bool>` (libstdc++, 0x28 B)

| offset | field | evidence |
|---|---|---|
| +0x00 | `_M_start._M_p` (`unsigned long*`) | PROVEN: copy constructor `0xb0dc90`, `0xb0dcc9 sub rax,[rsi]` |
| +0x08 | `_M_start._M_offset` (u32, 0 in practice) | PROVEN: `0xb0dcd8 mov eax,[r12+8]; 0xb0dcdd sub rsi,rax` |
| +0x10 | `_M_finish._M_p` | PROVEN: `0xb0dcc5 mov rax,[rsi+0x10]` |
| +0x18 | `_M_finish._M_offset` (u32) | PROVEN: `0xb0dcb3 mov edx,[rsi+0x18]` |
| +0x20 | `_M_end_of_storage` | PROVEN: `_M_initialize` `0xab42b6`/`0xab4317` |

Bit count = `(finish.p - start.p) * 8 + finish.offset - start.offset` (`0xb0dcd4 lea rsi,[rdx+rax*8]`,
then `sub rsi,rax`; the same formula in `0x2fab2cf`..`0x2fab2e3`). Words are 64 bits (PROVEN):
- `_M_initialize` `0xab4270`: `0xab4287 lea rdi,[rsi+0x3f]; 0xab4290 sar rax,6; 0xab4294 lea r14,[rax*8]`
  bytes, and `0xab42ab and ebx,0x3f` is the finish offset;
- the engine reads bit `j` as `[[part+0x60] + (j>>6)*8] & (1<<(j&63))` (`0x2fab399`..`0x2fab3b2`).

Windows (MSVC) stored the bits in `uint32` words and the encoder shipped `(end-begin)/4` of them.

### Decode at entry (replaces `VCfgParts` / `WriteVehicleConfig` ~1111-1164)

Read `r8` (BuyVehicle) or `rcx` (ReplaceVehicle) at detour entry, before the trampoline.

- vehicles: `{begin,end,cap}` at cfg+0; `span % 0x88 == 0`, `1 <= n <= 64`, the whole span readable.
  An empty vector is not shipped and not cancelled (Windows `ReadVec` rejects `end == begin`).
- per part `p = begin + i*0x88`: modelId int32 `p+0x00`; loadConfig `std::vector<int>` at `p+0x08`
  (`span % 4 == 0`, at most 0x400 bytes as on Windows); color 3 floats `p+0x20`; autoLoadConfig
  `std::vector<bool>` at `p+0x60` (bit count as above, storage in 64-bit words).
- vehicleGroups `std::vector<int>` at cfg+0x18 (`span % 4 == 0`, at most 0x400 bytes).
- The limits are the Windows ones (INFERRED sufficient). Any failure: write nothing, do not cancel.

## 4. Wire lines (must match the Lua)

One `ARMED <0|1>` line precedes every capture (`inject.lua:64`, `CM.lastArmed`). Integers `%d`,
colours `%.4f`.

| factory | line | Lua reader | notes |
|---|---|---|---|
| BuyVehicle | `VBUY <depot> <n> {<model> <nl> <load>×nl <r> <g> <b> <na> <auto>×na}×n <ng> <group>×ng` | `inject.lua:724-904` (one parser for VBUY and VREPL; fields read at `:736-762`) | depot = the `ecx` entity (the VEHICLE_DEPOT child) |
| BuyVehicle, clone | `VBUYLINE <line>` directly after its VBUY, only when the callback's line is >= 0 | `inject.lua:38-48` (a VBUY that ends a read waits for the next read; `CM.injectNext`), `:866-878` | written from the matched Add (section 6) |
| ReplaceVehicle | `VREPL <vehicle> <config exactly as VBUY>` | `inject.lua:724-806` | vehicle = `edx` |
| SellVehicle | `VSELL <n> <id>×n` | `inject.lua:906-917` | from the `rdx` vector; not shipped when empty or unreadable |
| SendToDepot | `VDEPOT <vehicle> <0|1>` | `inject.lua:980` | `ecx & 1` |
| Reverse | `VREV <vehicle>` | `inject.lua:972-978` | |

**autoLoadConfig on the wire.** `inject.lua:756` passes each part's `<auto>` list and its `nl` to
`CM.autoLoadFlags(words, nSlots)` (`vehicles.lua:629-652`). That function returns `nSlots` flags:
- all 1 when the list is empty;
- `words[j]` when `#words == nSlots` and every word is 0 or 1;
- otherwise bit `j` of `words[floor(j/32)+1]`, with negative words taken +2^32.

The encoder emits `na = ceil(bits/32)` words: for each 64-bit storage word, the low half and then the
high half, printed as `int32`, with every bit at or beyond `bits` cleared (libstdc++ does not guarantee
them zero).
- PROVEN (from the Lua text and the layout above): when `bits == nl`, which the engine asserts
  (section 3), the reader gets back every bit.
  - For `nl = 1` the single word is 0 or 1, so both reader branches give bit 0.
  - For `nl >= 2`, `ceil(nl/32) < nl`, so the reader takes the word branch, and bit `j` of the low/high
    split is bit `j` of the storage.
- When `bits != nl`, the reader still returns `nl` flags, with missing bits as 0.
- UNPROVEN and not needed: that the line is byte-identical to the Windows build's (MSVC's
  `vector<bool>` word count is not in this binary). Only `CM.autoLoadFlags` consumes the field.

## 5. Callers

### Script makers (the replay filter)

The Lua makers are sol2 lambdas of `scripting::SetupCommandInterface` (registration `0x19638f0`). Each
calls its factory once, hands the Command to Lua (`0x197fab0`) and destroys the local (`0x15d8f30`); none
calls `0x15da840`. The mod replays through these (`vehicles.lua`: `api.cmd.make.buyVehicle`,
`replaceVehicle`, `sellVehicle`, `sendToDepot`, `reverseVehicle`), so a factory entered from one of these
return addresses is a replay: never shipped, never cancelled. Windows used `0xceefae` for the buy and
the block `0xcec000..0xcf2000`; on Linux the wrappers are interleaved with other sol2 code, so use exact
addresses.

| maker | call → return address | body | wrapper (tail call) | evidence (PROVEN) |
|---|---|---|---|---|
| buyVehicle | `0x197483c` → **`0x1974841`** | `0x1974630` | `0x1974d80` (`0x1974e14`) | `functor_function<SetupCommandInterface(...)::<lambda(sol::table, ecs::Entity, ecs::Entity, TransportVehicleConfig, sol::this_state)>>`; Windows `0xceefae` |
| replaceVehicle | `0x1974ff7` → **`0x1974ffc`** | `0x1974e20` | `0x1975600` (`0x1975694`) | `<lambda(sol::table, ecs::Entity, TransportVehicleConfig, sol::this_state)>`; `r8d = 0` (`0x1974fe7`) |
| sellVehicle | `0x1969f27` → **`0x1969f2c`** | `0x1969d90` | `0x196a180` (`0x196a214`) | `<lambda(sol::table, ecs::Entity, sol::this_state)>`; builds a one-entity vector (`0x1969edf`..`0x1969efb`) |
| sendToDepot | `0x196c6ff` → **`0x196c704`** | `0x196c580` | `0x196c9b0` (`0x196ca44`) | `<lambda(sol::table, ecs::Entity, bool, sol::this_state)>`; `ecx` from `setne r12b` (`0x196c6bd`, `0x196c6f9`) |
| reverseVehicle | `0x196a36c` → **`0x196a371`** | `0x196a220` | `0x196a5b0` (`0x196a644`) | `<lambda(sol::table, ecs::Entity, sol::this_state)>` |

Name binding (PROVEN). In `0x19638f0` each maker's name is built into a `std::string` and handed to a
set-up routine used only for that maker. Each routine has that one caller, and each wrapper has that one
reference:
- `'buyVehicle'` `0x1963db8` → `r14` (`0x1963dc2`) → `rsi` of `0x1960c30` (`0x1963dd7`, call `0x1963dda`),
  which loads wrapper `0x1974d80` (`0x1960e70`). Parameter names just before: `'playerEntity'`
  `0x1963d63`, `'depotEntity'` `0x1963d76`, `'config'` `0x1963d86`.
- `'replaceVehicle'` `0x1965670` → `rsi` of `0x1958170` (`0x196568f`, `0x1965692`), which loads
  `0x1975600` (`0x19583b0`). Parameter names: `'vehicleEntity'` `0x196562b`, `'config'` `0x196563e`.
- `'reverseVehicle'` `0x1965754` → `rsi` of `0x1958ca0` (`0x1965773`, `0x1965776`), which loads
  `0x196a5b0` (`0x1958ee3`). Parameter name: `'vehicleEntity'` `0x196571f`.
- `'sellVehicle'` `0x1965823` → `rsi` of `0x1959680` (`0x1965842`, `0x1965845`), which loads `0x196a180`
  (`0x19598c3`). Parameter name: `'vehicleEntity'` `0x19657ee`.
- `'sendToDepot'` `0x196590e` is built into `[rbp-0x220]` (`[rbp-0x5e8] = &[rbp-0x220]`, `0x1964130`,
  `0x1964141`). It is copied as the key at `0x1965928`..`0x1965951`, then the inlined set-up pushes
  wrapper `0x196c9b0` as `__call` (`0x1965b7f`..`0x1965ba1`). Parameter names: `'vehicleEntity'`
  `0x19658c1`, `'sellOnArrival'` `0x19658d4`.

### UI sites, and the Add call

At all nine UI sites the factory's `rdi` (the Command) is the `rdx` of the next call, `0x15da840`
(PROVEN per row):
- the register or slot given as `rdi` is not written between the factory call and that call;
- no branch lands in between;
- `rbx`, `r12` and `r13` are callee-saved.

That call also takes:
- `rsi` = the CommandList;
- `rcx` = a `std::function<void(const Command&)>*` (empty at Reverse);
- `r8` = a 16-byte object the caller zeroes first;
- `rdi` = a result object the caller destroys at once with `0x3190430`.

That `0x15da840` is `CommandList::Add` is slice-core's claim (`SLICE_CORE.md` §2.1, C-ADD-1); this area
uses only the call shape.

| factory | return address | function (identification) | rdi ← / Add rdx ← | Add call | Add `rcx`: manager / invoker |
|---|---|---|---|---|---|
| BuyVehicle | **`0x127c000`** | `0x127b7a0` `UI::{anon}::HandleVehicleChange` (named by its callbacks' typeinfo `0x5a1c950`) | `r13` (`0x127bfe5` lea, `0x127bff8`) / `r13` (`0x127c011`) | `0x127c01e` | `rbx`=`rbp-0x60`: `0x126eb30` / `0x1272350` |
| ReplaceVehicle | **`0x127bd06`** | HandleVehicleChange, vehicle != -1 and the new config has parts | `r12` (`0x127bcfe`) / `r12` (`0x127bd17`) | `0x127bd24` | `rbx`: `0x126d820` / `0x126be70` |
| ReplaceVehicle | **`0x2fbb2e2`** | `0x2fbae50` (`'Replacing '` `0x2fbaea8`, `' vehicles.'` `0x2fbaef6`), called from `0x131f1c0` (`'{number} models have been replaced successfully.'`), whose address is taken at `0x131f860` in `UI::CreateMissingResourcesWindow` `0x131f2f0`; `r8d = 1` | `rbx` (`0x2fbb2da`) / `rbx` (`0x2fbb2ed`) | `0x2fbb304` | `r12`=`rbp-0x60`: not read |
| SellVehicle | **`0x127c219`** | HandleVehicleChange, vehicle != -1 and the new config has no parts (`0x127bb12`..`0x127bb20`) | `r12` (`0x127c211`) / `r12` (`0x127c22a`) | `0x127c237` | `rbx`: `0x126d770` / `0x126be30` |
| SellVehicle | **`0x127d0d0`** | `0x127cc70` (called from `0x127d270`); its callback's typeinfo `0x5a1cae0` is a lambda of `UI::VehicleManager::VehicleManager` | `r13` (`0x127d0c8`) / `r13` (`0x127d0de`) | `0x127d0ee` | `r14`=`rbp-0x60`: `0x126b140` / `0x126bf20` |
| SellVehicle | **`0x1432b39`** | `0x14327c0` (called from `0x1432c50`); callback typeinfo `0x5a21488` `vehicle_button_util::CreateSellButton(...)::{lambda()#1}::operator()()::{lambda(const Command&)#2}` | `[rbp-0x108]` (`0x1432b23`) / reloaded `0x1432b39 mov r14,[rbp-0x108]`, `0x1432b57 mov rdx,r14` | `0x1432b5a` | `rbx`: `0x142f430` / `0x142f510` |
| SendToDepot | **`0x1274234`** | `0x1274130` (called from `0x1274380`, `0x1274440`); callback typeinfo `0x5a1c980` `UI::{anon}::SendVehiclesToDepot(CommandList&, EnginePtr, const std::vector<ecs::Entity>&, bool, DialogManager*)::{lambda(const Command&)#1}`; `ecx = 0` (`0x1274227`) | `r13` (`0x127422c`) / `r13` (`0x127423f`) | `0x1274256` | `r12`=`rbp-0x60`: `0x126d8d0` / `0x1272520` |
| SendToDepot | **`0x142fed9`** | `0x142fdd0`, invoker of `vehicle_button_util::CreateSendToDepotButton(...)::{lambda()#1}` (address taken at `0x1431d05` in `0x1431b60`, manager `0x142f9a0` next); `ecx = 0` (`0x142fecc`) | `r13` (`0x142fed1`) / `r13` (`0x142feea`) | `0x142fef7` | `r12`: `0x142f580` / `0x1434830` |
| Reverse | **`0x144439b`** | `0x1444330`, invoker of `UI::{anon}::CreateVehicleContent(...)::{lambda()#6}` (ViewCreator.cpp): address stored at `0x145a7e9`/`0x145a7f0`, manager `0x1440180` at `0x145a7f7`/`0x145a7fe`, connected by `0x3028dd0` at `0x145a805` | `rbx` (`0x1444393`) / `rbx` (`0x14443a6`) | `0x14443b9` | `r12`=`rbp-0x50`: **empty** (manager slot `[rbp-0x40]` = 0 at `0x1444377`) |

These nine UI sites and the five makers are the 14 references of section 1.

Neither UI SendToDepot site passes `sellOnArrival = 1`; only the Lua maker can.

### HandleVehicleChange `0x127b7a0` (PROVEN)

Signature, spelled by its closures' typeinfo name `0x5a1c950`:
`UI::{anon}::HandleVehicleChange(CommandList&, const ModelData&, UI::GameTimePtr,
const transport::CargoTypeRep&, UI::GameStatePtr, UI::EnginePtr, ecs::Entity, ecs::Entity, ecs::Entity,
UI::ViewManager*, UI::DialogManager*, std::shared_ptr<bool>, ecs::Entity, TransportVehicleConfig,
Signal<std::vector<std::string>(ecs::Entity, const TransportVehicleConfig&)>&, std::shared_ptr<bool>)`.

| # | parameter | location | use in the body |
|---|---|---|---|
| 1-6 | CommandList&, ModelData const&, GameTimePtr, CargoTypeRep const&, GameStatePtr, EnginePtr | `rdi`, `rsi`, `rdx`, `rcx`, `r8`, `r9` | saved at `[rbp-0x1e0]`, `[rbp-0x1d8]`, `[rbp-0x1a8]`, `[rbp-0x1e8]`, `[rbp-0x1f0]`, `[rbp-0x1b0]` (`0x127b7bf`..`0x127b812`) |
| 7 | Entity, player | `[rbp+0x10]` | BuyVehicle `edx` (`0x127bfef`) |
| 8 | Entity, depot | `[rbp+0x18]` | BuyVehicle `ecx` (`0x127bfec`); `< 0` → `'No depot found'` (`0x127be20`..`0x127be25`, `0x127c0b9`) |
| 9 | Entity, line | `[rbp+0x20]` | buy lambda+0x30 (section 6) |
| 10, 11 | ViewManager*, DialogManager* | `[rbp+0x28]`, `[rbp+0x30]` | buy lambda+0x38, +0x40 |
| 12 | shared_ptr<bool> (hidden reference) | `[rbp+0x38]` | copied to buy lambda+0x48 |
| 13 | Entity, vehicle | `[rbp+0x40]` | branch `0x127bb08`; the sell vector (`0x127c17f`, `0x127c1d8`) |
| 14 | TransportVehicleConfig (hidden reference) | `[rbp+0x48]` | `0x127b800`..`0x127b80e`; empty test `0x127bb12`..`0x127bb20` |
| 15 | Signal& | `[rbp+0x50]` | `0x127b7e3` |
| 16 | shared_ptr<bool> (hidden reference) | `[rbp+0x58]` | copied to buy lambda+0x00 |

Both callers load exactly six registers and push ten slots (`add rsp,0x50` at `0x127c8ac` and
`0x127e00f`), which fits only if each `*Ptr` takes one register.

Branch: `0x127bb08 cmp dword [rbp+0x40],-1; je 0x127be20` goes to the buy. Otherwise
`0x127bb1c cmp [rcx+8],rax; je 0x127c129` sends an empty config to SellVehicle `0x127c214`, and a config
with parts goes towards ReplaceVehicle `0x127bd01`.

It has exactly two callers (direct calls; no other reference):
- **`0x127c8a0` in `0x127c3f0`**, the invoker of the VehicleManager constructor's lambda #14,
  `lambda(const std::vector<std::pair<ecs::Entity, TransportVehicleConfig>>&)`. Typeinfo `0x5a1ca80`
  comes from manager `0x126ed70` op 0 (`0x126eed0`); invoker and manager are stored together at
  `0x1278d00`/`0x1278d0b` in `0x1277cf0`.
  - It loops over its vector in 0x38-byte `{Entity, TransportVehicleConfig}` steps (`0x127c4b3 mov
    r14,rax`, `0x127c971 add r14,0x38`). For each element it calls HandleVehicleChange with parameter 13 =
    the element's entity (`0x127c872 mov eax,[r14]`, push `0x127c87c`) and parameter 14 = a copy of the
    element's config (`0x127c4fa`.., `0x127c797`.., push `0x127c87b`).
  - An element with entity -1 takes the buy branch. One with entity >= 0 takes the replace or sell
    branch; the clone always passes -1, so this is the only way to reach those two sites.
  - Player = `[lambda+0x38]` (`0x127c89b`). Line and depot are computed once, before the loop:
    - by default, line = -1 (`0x127c425`) and depot = `*(int32*)[lambda+0x40]` (`0x127c421`..`0x127c431`);
    - if the vector is non-empty and its first entity is >= 0 (`0x127c437`..`0x127c444`), they are the
      low and high halves of `0x1276500(lambda[0x28], lambda[0x30], firstEntity)` (`0x127cba0`..`0x127cbb9`).
  - That lambda #14 is the vehicle store's button is INFERRED (`'BuyButton'` `0x1277f3a`, `'vehicle-store'`
    `0x127814d` in the constructor).
- **`0x127e003` in the clone `0x127d690`** (`'Cloning can only be performed for {number} vehicles at
  once'` `0x127e2cf`):
  - it pushes vehicle -1 (`0x127dfea`), so it always buys;
  - line = the low half and depot = the high half of `0x1276500`'s result (`0x127df6b`,
    `0x127df7e mov r12,rax`, `0x127dffa push r12`, `0x127dffc sar r12,0x20`, `0x127e000 push r12`);
  - it is called from `0x127e520` (address taken at `0x1279704` in the constructor) and from `0x127e6b0`
    (`0x127398e` in `0x1272bc0`).
- What `0x1276500` computes is INFERRED; its funcsig row is an inlined name.

## 6. The buy completion callback

Windows: `_Do_call` `0x753820` runs `0x748250` on the lambda at impl+8. The line was read at impl+0x38,
which is lambda+0x30. The result was read at `(*command)+0x38` after checking tag 13 at `+0xb18`. The
64-byte MSVC `std::function` keeps its impl pointer at `+0x38`.

Linux (PROVEN unless marked):

- **The function object.** `std::function<void(const Command&)>` is 32 B: `+0x00` `_Any_data`, which
  holds the functor **pointer** (the lambda is 0x58 B, heap-stored), `+0x10` `_M_manager`, `+0x18`
  `_M_invoker`.
  - At the buy site: `0x127bef9 mov edi,0x58; call operator new; 0x127bf12 mov [rbp-0x60],rax`;
    `0x127bfc2`/`0x127bfc9` put the invoker `0x1272350` at `[rbp-0x48]`; `0x127bfcd`/`0x127bfd4` put the
    manager `0x126eb30` at `[rbp-0x50]`.
  - Add receives `rcx = rbx = rbp-0x60`. `0x127b9a8 lea rbx,[rbp-0x60]` is the last write to rbx before
    the site; the function's only other writes are `0x127b7ad` and `0x127b8bd`, both earlier on the
    only path.
  - Right after Add the caller destroys its own copy with manager op 3 (`0x127c03b`..`0x127c04f`).
- **Manager `0x126eb30`** `(dest rdi, src rsi, op edx)`:
  - op 0 → typeinfo `0x5a1c950` (`0x126ec10`), whose name is HandleVehicleChange's first
    `lambda(const Command&)` (signature in section 5);
  - op 1 → the functor pointer (`0x126ec20`);
  - op 2 → `operator new(0x58)` and a member copy (`0x126eba0`..`0x126ebff`);
  - op 3 → releases the shared_ptr controls at +0x08 and +0x50, then `operator delete(p, 0x58)`
    (`0x126eb5f`..`0x126eb8b`).
- **Invoker `0x1272350`** = `_M_invoke(const _Any_data& rdi, const Command& rsi)`:
  1. `0x1272366 mov rbx,[rdi]` (lambda) and `0x1272363 mov r13,[rsi]` (impl).
  2. `0x1272378 cmp byte [r13+0xd48],0xd; jne 0x12724f0` (`'Unexpected index'`, the `std::get` failure).
  3. `0x1272386 lea rdi,[rbx+0x28]; call 0x146f0a0` gives the Engine.
     - `0x146f0a0` calls `0x1477120` (`mov rdi,[rdi]; mov rax,[rdi]; jmp [rax+0x10]`), a virtual call
       through the EnginePtr, and returns `[rax+0x28]`.
     - The UI makes the same call right before every factory call to get its Engine: `0x127bfe0`,
       `0x127bced`, `0x127c1fb`, `0x127d0b2`, `0x1432b1e`, `0x1274220`, `0x142febd`, `0x144437f`.
  4. `0x1272392 mov edx,[r13+0x38]` (the result), `0x1272396 lea rsi,[r12+8]` (the revs),
     `0x127239b mov ecx,1`, `0x12723a3 call ecs::Validate`.
  5. `0x12723ab cmp eax,-1; je 0x12723e0` → return.
  6. With a live vehicle:
     - `*[lambda+0x00]` is set to 1 the first time (`0x12723b0`, `0x1272490`), with the notification
       `0x3076e60([[lambda+0x38]+0x18], 0x5a50960)`;
     - `0x12723bc mov eax,[rbx+0x30]; test; jns 0x1272400` → `0x127245c call 0x1433220` with
       `r9d` = the line (`0x127243b`) and `r8d` = the vehicle (`0x127243f`); `0x1433220` calls
       `make_cmd::SetLine` at `0x14334f2`;
     - a line below 0 takes the settings and window path (`0xc1a300`, `0x1469420(rdi=[lambda+0x38])`,
       gated and cleared through `*[lambda+0x48]`: `0x12724c0`, `0x12724e1`); that it opens the vehicle
       window is INFERRED.
- **`ecs::Validate` `0x325c4f0`** `(engine, revs, entity, optional)`:
  - looks the entity up in the 0x10-byte revs (`0x325c52f`..`0x325c5bc`);
  - not found with `optional` → returns the entity unchanged, with no call on that path
    (`0x325c5c0 test cl,cl; 0x325c5c8 mov eax,ebx`);
  - not found without it → assert `it != entityRevs.end() || optional` (`0x325c68e`);
  - found → `0x324e820`, then -1 on a stale rev (`0x325c5dd`..`0x325c600`).
- **Firing it on a cancelled buy.** A cancelled buy was never applied, so impl+0x38 holds the factory's
  -1. The revs hold no negative id (section 2), so Validate returns -1 and the invoker returns at
  `0x12723ae`/`0x12723e0`. On that path (PROVEN):
  - the only memory write is its own stack canary (`0x1272372`);
  - the only calls are `0x146f0a0` (`0x127238d`) and Validate (`0x12723a3`);
  - there is no write to the lambda, its done flag or its window flag, and no notification, window or
    SetLine call.

  The one effect not resolved statically is the virtual getter behind lambda+0x28. That it has no side
  effect is INFERRED from its use as the UI's Engine accessor. Firing the callback therefore changes no
  proven state, and not firing it is equivalent. If it is fired, it must run inside the Add detour,
  while the caller's copy (and the captured EnginePtr and shared_ptrs) still exist.
- **Recognising it at the matched Add:**
  - `fn = Add.rcx`; require `*(u64*)(fn+0x18) == base+0x1272350` and `*(u64*)(fn+0x10) == base+0x126eb30`;
  - `lambda = *(void**)fn`, `line = *(int32_t*)(lambda+0x30)`;
  - `impl = *(void**)Add.rdx`, require `impl[0xd48] == 13`; the result is `*(int32_t*)(impl+0x38)`.
  - Windows' log of "command+0x38" on the Command object itself (~3446-3450) reads MSVC's Command
    layout; do not port it.

Buy lambda captures (0x58 B). Offsets come from where HandleVehicleChange fills its stack copy
(`0x127be66`..`0x127bef4`) and copies it into the heap block (`0x127bf1d`..`0x127bfbe`). Each type is
the type of the HandleVehicleChange parameter it comes from (the parameter table in section 5). All
PROVEN:

| offset | filled from | parameter (type) | invoker use |
|---|---|---|---|
| +0x00/+0x08 | shared_ptr copy `0xfa9520` (`0x127be66`..`0x127be86`) of `[rbp-0x1b8]` = `[rbp+0x58]` (`0x127b7c6`, `0x127b820`) | 16, `std::shared_ptr<bool>` | `*[+0x00]` set to 1 on the first live vehicle (`0x12723b0`, `0x1272490`); copied into the SetLine call (`0x127240e`) |
| +0x10 | `[rbp-0x1e0]` = `rdi` (`0x127b7ca`) | 1, `CommandList&` | `rdi` of `0x1433220` (`0x127244f`) |
| +0x18 | `[rbp-0x1f0]` = `r8` (`0x127b804`) | 5, `UI::GameStatePtr` | `rcx` of `0x1433220` (`0x1272443`) |
| +0x20 | `[rbp-0x1e8]` = `rcx` (`0x127b7bf`) | 4, `const transport::CargoTypeRep&` | `rsi` of `0x1433220` (`0x127244b`) |
| +0x28 | `[rbp-0x1b0]` = `r9` (`0x127b812`) | 6, `UI::EnginePtr` | Validate's Engine (`0x1272386`); `rdx` of `0x1433220` (`0x1272447`) |
| +0x30 | `[rbp+0x20]` (`0x127becf`) | 9, `ecs::Entity`, the line | `>= 0` → `0x1433220` with `r9d` = line (`0x12723bc`, `0x127243b`) |
| +0x38 | `[rbp-0x1f8]` = `[rbp+0x28]` (`0x127b7bb`, `0x127b7d1`) | 10, `UI::ViewManager*` | `[+0x38]+0x18` → notification (`0x1272493`..`0x12724a2`); `rdi` of `0x1469420` (`0x12724cd`) |
| +0x40 | `[rbp-0x1c0]` = `[rbp+0x30]` (`0x127b7d8`, `0x127b7e7`) | 11, `UI::DialogManager*` | pushed to `0x1433220` (`0x1272459`) |
| +0x48/+0x50 | shared_ptr copy of `[rbp-0x200]` = `[rbp+0x38]` (`0x127b7ee`, `0x127b7f9`, `0x127be92`..`0x127bef4`) | 12, `std::shared_ptr<bool>` | `*[+0x48]` gates and clears the window path (`0x12724c0`, `0x12724e1`) |

**Replace callback** (HandleVehicleChange lambda #3, typeinfo `0x5a1c970`, `…EUlRK7CommandE1_`):
- manager `0x126d820` (functor `operator new(0x10)` `0x126d883`, deleted with size 0x10 `0x126d861`);
- invoker `0x126be70`:
  - `0x126be74 mov rax,[rdi]; 0x126be77 mov rax,[rax]` reaches a `bool*`; if the bool is 0 it is set
    to 1 (`0x126be83`), then `0x3075360(0)` and a tail jump to `0x3076e60(rax, 0x5a50900)`;
  - **the Command (`rsi`) is never read**;
- firing it sets the manager's done flag once and posts that notification (what the notification does
  is INFERRED).

The sell callback of the same function is lambda #2 (typeinfo `0x5a1c960`, manager `0x126d770`, invoker
`0x126be30`, not read).

## 7. Integration notes (outside this area)

- **Relay (slice-core).**
  - For these five hooks the handler needs `rdi, rsi, rdx, rcx, r8` and the return address `[rsp]` at
    entry; there are no xmm and no stack arguments.
  - The relay must restore rsp before jumping to the 14-byte trampoline.
  - The config (BuyVehicle `r8`, ReplaceVehicle `rcx`) and the sell vector (`rdx`) must be read before
    the original runs.
- **Pending cancel (slice-core).** Identify the command at Add by `Add.rdx == factory.rdi`; it holds at
  all nine UI sites.
- **Callbacks (slice-core decision).**
  - On Windows the buy and the replace fired their callback before cancelling; that the Linux windows
    wait on it is INFERRED from the shared source.
  - Buy: with the result -1 the invoker changes no proven state (section 6). Not firing it is the
    choice that rests on no inference; if it is fired, fire it inside the Add detour.
  - Replace: firing it sets the vehicle manager's done flag and posts a notification.
  - Sell, SendToDepot and Reverse stay fire-and-forget (Reverse's function object is empty anyway).
- **VBUYLINE.** Write it from the matched Add, right after its VBUY, only when the cancel lands and the
  recogniser in section 6 yields a line >= 0. This area supplies the recogniser; slice-core calls it
  from its Add hook.
- **Missing-resources replace** (`0x2fbb2e2`, bool 1): the apply handler skips the price-difference
  booking for it (section 2). Whether it is shipped, cancelled or left alone is an open decision.
- **Add call shape (slice-core).** Observed at all nine sites:
  - `rdi` = a result object destroyed by `0x3190430` right after (`0x127c026`, `0x127bd2c`, `0x14443c1`);
  - `rsi` = the CommandList (`[rbp-0x1e0]` ← HandleVehicleChange `rdi`, `0x127b7ca`);
  - `rdx` = the Command;
  - `rcx` = the `std::function*`, which may be empty;
  - `r8` = a pointer to a 16-byte object the caller zeroes first (`0x127be70`/`0x127be7b`,
    `0x1444361`/`0x144436c`).
  - The Command is destroyed with `0x15d8f30` right after (`0x127c02e`).
- **Shared services (slice-core).** `ARMED`, the inject file, the instance letter, `SessionLive`,
  fault-safe reads for the config walk, and the log.
- **Hook ids.** Keep 2, 3, 4, 5, 10 so logs stay comparable with Windows.

## 8. Differences from Windows

| item | Windows | Linux |
|---|---|---|
| return slot / Engine | rcx / rdx | rdi / rsi |
| BuyVehicle | r8 player, r9 depot, config = by-value copy at `[calleeRsp+0x28]` | edx player, ecx depot, `r8` → caller's temporary, emptied by the factory |
| ReplaceVehicle | r8 vehicle, r9 → config | edx vehicle, rcx → config (emptied), r8d bool |
| SellVehicle | r8 → vector | rdx → vector |
| SendToDepot | r8 vehicle, r9 bool | edx vehicle, ecx bool |
| Reverse | r8 | edx |
| steal | 15 / 20 / 15 / 20 / 20 | 14 for all five |
| script filter | `0xceefae` + block `0xcec000..0xcf2000` | five exact return addresses |
| variant index | `*(cmd)+0xb18` | `*(cmd)+0xd48` |
| BuyVehicle result | `*(cmd)+0x38` | `*(cmd)+0x38` |
| part stride | 0x80 | 0x88 |
| autoLoadConfig | MSVC `vector<bool>`, uint32 words | libstdc++ `vector<bool>` 0x28 B, 64-bit words, bit count from two offsets |
| callback object | 64 B, impl at `+0x38`, `_Do_call` at vftable+0x10 | 32 B, functor pointer `+0`, manager `+0x10`, invoker `+0x18` |
| buy callback identity | `_Do_call == 0x753820` | invoker `0x1272350` (manager `0x126eb30`) |
| clone line | impl+0x38 (lambda+0x30) | `*(fn+0)`+0x30 (lambda+0x30) |
| BuyVehicle UI site | factory returns to `0x74fd88` | `0x127c000` |

## 9. Not established

- That `0x15da840` is CommandList::Add is slice-core's claim (C-ADD-1), not re-derived here; what its
  result object must look like when Add is skipped (slice-core).
- Whether a replace from the missing-resources dialog (`0x2fbb2e2`, bool 1, no price booking) should be
  shipped, cancelled or left alone.
- The sell and send-to-depot callback invokers (`0x126be30`, `0x126bf20`, `0x142f510`, `0x1272520`,
  `0x1434830`); fire-and-forget on Windows, never fired.
- That the EnginePtr virtual getter (`0x1477120` → `[vtbl+0x10]`) has no side effect.
- What `0x1276500` computes; that the constructor's lambda #14 is the store's button; what `0x127cc70` is.
- That the autoLoadConfig words are byte-identical to the Windows build's line (not needed).
- The Command's fields beyond +0x28 and its total size.
- Runtime confirmation: nothing here was observed in the running Linux game.

## 10. Revision after verification

- Hook safety: 14 references (9 UI + 5 Lua), not 15. The scan now covers both executable segments and
  attributes every RIP-relative window.
- UI sites: nine, not ten.
- Buy callback on a cancelled buy: the -1 path does read lambda+0x28 and make one virtual call; "no-op"
  is narrowed to "changes no proven state".
- HandleVehicleChange's store caller is not buy-only: it passes each element's entity, so entities
  >= 0 reach the replace and sell sites.
- Add: the call shape is proven here; the identity stays slice-core's.
- autoLoadConfig's name at +0x60: now proven from the engine's size assert.
- autoLoadConfig encoding: restated as a decode equivalence with the Lua reader (proven); byte identity
  with Windows is split off (unproven, not needed).
- ReplaceVehicle's bool: its effect in the apply handler is proven; the meaning "free replace" is
  inferred.
- Lua maker names: now proven from the registration.
- Buy lambda captures: types proven from HandleVehicleChange's parameter list.
