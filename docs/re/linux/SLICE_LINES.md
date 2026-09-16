# Slice, lines area: the Linux map

Linux RE for the slice's line and naming commands. Scope: the six `make_cmd` factories
CreateLine, UpdateLine, DeleteLine, SetLine, SetColor and SetName; the `component::Line` and
`Line::Stop` layout that UpdateLine's decode reads; the callers the hooks classify by; the
`0x15da840` call every UI site makes with the Command and the completion callback it passes; and the
inject lines the Lua side parses. This replaces `native/src/slice_hook.cpp` ~1186-1481 (decode and
writers), the six rows of `FACTORIES[]` and the id 6/7/8/9/13/14 branches of `DeferHandler` and
`CaptureFactory` for the Linux build.

Binary: `TransportFever2`, Steam build 35924, GNU build-id `3a0e156390b0e6f1e372051c24802c8493ae454a`.
Addresses are Linux RVAs (the PIE links at 0; live = image base + RVA). "Windows" means the RVAs in
`docs/re/COMMANDS.md` and `slice_hook.cpp`, relative to `0x140000000`.

Status labels: **PROVEN** = the instructions, strings or data quoted here show it and can be
re-derived from the binary; **INFERRED** = placed by address order, by names, by the Windows docs or by
elimination, so it stays out of patch code; **UNPROVEN** = not established, and anything that would
depend on it stays off. Nothing here was checked in the running game (static map, revised after
independent verification).

## How the evidence was produced

- Disassembly: capstone (x86-64) linear sweep from each function start over its FDE size from
  `/home/topsnek/tpf2-re/linux/functions.csv`; RIP-relative string loads named from `xrefs.csv`.
- References to the factories: a capstone sweep of all 121,725 FDEs in `.text` that resolves every
  immediate call, jump and branch target and every RIP-relative memory operand, and matches them
  against each factory start and the 13 bytes after it. The second executable segment `.bind`
  (`0x5bc1000`, `0x1812a` B, no FDEs) was scanned raw: every 4-byte field as a rel32 (with 0, 1, 2 or
  4 trailing immediate bytes) and every byte as a rel8. Then all 67,854 `SHT_RELA` addends, and a
  raw scan of the file for each of those 84 addresses as an 8-byte value.
- Add sites: for each of the 28 `0x15da840` calls that receive a line-area Command (the 26 UI factory
  sites plus the two callers of line_util), the argument sources, every call up to the caller's
  `0x3190430`, the stores into the `std::function` at `rcx` (`+0x10` manager, `+0x18` invoker), and
  the zeroing of the `r8` object.
- Callbacks: each non-empty invoker disassembled. Lambda owners come from the typeinfo of the manager
  stored next to the invoker. Manager op 0 does `lea rax,[typeinfo]`; the name string is the
  `R_X86_64_RELATIVE` addend at typeinfo+8.
- The throwaway scripts lived in the area's scratch directory; the method above is enough to redo any
  row.

## 1. The six factory hooks

Every factory starts with the same shape: `endbr64; push rbp; mov rbp,rsp;` three 2-byte
`push r1x;` then `push r12/rbx` and `sub rsp, imm32`. The first 14 bytes end exactly on an
instruction boundary. `PrologueSteal(code, 14)` (hook_posix.cpp) decodes each of these
instructions (endbr64 = 4, `55` = 1, `48 89 e5` = ModRM mod 3, `41 5x` = REX push) and returns 14.

| hook id | factory | Windows RVA / steal | Linux RVA | FDE size | first 14 bytes (stolen) | next 8 bytes | steal |
|---|---|---|---|---|---|---|---|
| 6 | SetLine | `0x9dea10` / 18 | `0x15ed550` | 368 | `f30f1efa 55 4889e5 4157 4156 4155` | `41 54 53 48 81 ec 98 0d` | 14 |
| 7 | CreateLine | `0x9dcde0` / 19 | `0x15efda0` | 674 | `f30f1efa 55 4889e5 4157 4156 4155` | `41 54 53 48 81 ec 08 0e` | 14 |
| 8 | UpdateLine | `0x9df4e0` / 19 | `0x15f0050` | 513 | `f30f1efa 55 4889e5 4157 4156 4155` | `41 54 53 48 81 ec c8 0d` | 14 |
| 9 | DeleteLine | `0x9dd190` / 20 | `0x15ebd00` | 241 | `f30f1efa 55 4889e5 4156 4155 4154` | `53 48 81 ec 80 0d 00 00` | 14 |
| 13 | SetColor | `0x9de8a0` / 20 | `0x15ecb40` | 307 | `f30f1efa 55 4889e5 4157 4156 4155` | `41 54 53 48 81 ec a8 0d` | 14 |
| 14 | SetName | `0x9deb70` / 15 | `0x15ee6d0` | 598 | `f30f1efa 55 4889e5 4157 4156 4155` | `41 54 53 48 81 ec d8 0d` | 14 |

Identity: each RVA is the function that loads its own `__PRETTY_FUNCTION__` in its assert path:

| factory | signature string (site) | assert (site) | assert call |
|---|---|---|---|
| SetLine | `Command make_cmd::SetLine(const ecs::Engine&, ecs::Entity, ecs::Entity, int)` (`0x15ed689`) | `vehicleEntity != ecs::Entity()` (`0x15ed69c`) | `0x15ed6a3` |
| CreateLine | `Command make_cmd::CreateLine(std::__cxx11::string, CVec3f, ecs::Entity, ecs::component::Line)` (`0x15efffa`) | `playerEntity != ecs::Entity()` (`0x15f000d`) | `0x15f0014` |
| UpdateLine | `Command make_cmd::UpdateLine(const ecs::Engine&, ecs::Entity, ecs::component::Line)` (`0x15f0202`) | `lineEntity != ecs::Entity()` (`0x15f0215`) | `0x15f021c` |
| DeleteLine | `Command make_cmd::DeleteLine(const ecs::Engine&, ecs::Entity)` (`0x15ebdc1`) | `lineEntity != ecs::Entity()` (`0x15ebdd4`) | `0x15ebddb` |
| SetColor | `Command make_cmd::SetColor(const ecs::Engine&, ecs::Entity, CVec3f)` (`0x15ecc43`) | `entity != ecs::Entity()` (`0x15ecc56`) | `0x15ecc5d` |
| SetName | `Command make_cmd::SetName(const ecs::Engine&, ecs::Entity, const string&)` (`0x15ee8e3`) | `entity != ecs::Entity()` (`0x15ee8f6`) | `0x15ee8fd` |

A failed game assert throws (PROVEN). Every assert call above goes to `0x2fcb860`, which builds the
report and calls `0x2fcb5e0` (637 B, no `ret`). That function reaches `0x2fcbb20`, which calls
`__cxa_allocate_exception` (`0x2fcbb3f`) and `__cxa_throw` (`0x2fcbbc7`).

Hook safety (PROVEN):

- Nothing branches into a stolen range. The sweep finds no call, jump or conditional-branch target in
  `(start, start+14)` anywhere in `.text` (the six bodies included) and none in `.bind`.
- Nothing refers to a factory except the 33 direct `e8` calls listed in section 4. There is no jump
  tail call, no RIP-relative `lea`/`mov`, no relocation addend and no raw 8-byte copy of any address
  in start..start+13.
- The stolen instructions are rsp-relative only (pushes, `mov rbp,rsp`); the relay must restore rsp
  to its entry value before jumping to the trampoline, as on Windows.

### Argument registers (SysV; `Command` is returned through a hidden pointer)

The Windows ABI shifted every argument one slot (rcx = return, rdx = Engine, r8/r9, `[rsp+0x28]`).
On Linux the return slot is `rdi`, all arguments of these six fit in registers, and **no stack
argument is read**. `ecs::Entity` travels by value as a 32-bit int (the bodies test `edx`/`ecx`);
read the low 32 bits only.

| factory | rdi | rsi | edx | ecx / rcx | r8d | xmm |
|---|---|---|---|---|---|---|
| SetLine | `Command*` ret | `const Engine&` | vehicle | line (ecx) | stopIndex | |
| CreateLine | `Command*` ret | `std::string*` name | player | `Line*` (rcx) | | xmm0 = color x (bits 0-31), y (bits 32-63); xmm1 = z (bits 0-31) |
| UpdateLine | `Command*` ret | `const Engine&` | line | `Line*` (rcx) | | |
| DeleteLine | `Command*` ret | `const Engine&` | line | | | |
| SetColor | `Command*` ret | `const Engine&` | entity | | | xmm0 = x, y; xmm1 = z |
| SetName | `Command*` ret | `const Engine&` | entity | `const std::string*` (rcx) | | |

Every factory returns its `rdi` in `rax` (`mov rax,r12` `0x15ebdac`, `mov rax,r12` `0x15ecc2c`,
`mov rax,r14` `0x15ed656`, `mov rax,r15` `0x15ee826`, `mov rax,r13` `0x15effd6`, `mov rax,r13`
`0x15f01de`). Per factory:

- **SetLine** (PROVEN): `0x15ed584 cmp edx,-1` → vehicle assert; `0x15ed568 mov [rbp-0xdc0],rsi` is
  handed to the packager `0x15ebab0` as `rdx` (`0x15ed600`); `0x15ed56f mov [rbp-0xdb4],ecx` is
  pushed into the command's entity list when not -1 (`0x15ed5cb`..`0x15ed5e4`) and stored at
  payload+4 (`0x15ed60d`); `0x15ed598 mov r15d,r8d` is stored at payload+8 (`0x15ed613`); the
  vehicle `ebx` at payload+0 (`0x15ed607`). Which Entity is the line follows from the signature
  order and the vehicle assert on the first one. UI site `0x14385e3 mov r8d,-1`
  (`SetLineAllInstallHandlers`) shows stopIndex can be -1.
- **CreateLine** (PROVEN): `0x15efdb8 movss [rbp-0xe28],xmm1` and `0x15efdc0 movq [rbp-0xe30],xmm0`
  (colour); `0x15efdd7 cmp edx,-1` → player assert; `0x15efdf5 mov r15,rsi` is the source of the
  string construct `0x9b7e90` into CmdData+0x30 (`0x15efec6`); `0x15efde7 mov rbx,rcx` then
  `0x15efe10`..`0x15efe38` move `[rcx]`,`[rcx+8]`,`[rcx+0x10]` out and **zero them**, and copy
  `[rbx+0x18]` (movss), `[rbx+0x20]` (qword), `[rbx+0x28]` (dword). UI caller `0x2eb1c2a movss
  xmm1,[rbp-0xe8]`, `0x2eb1c32 movq xmm0,[rbp-0xf0]` (x,y,z stored from xmm4/xmm3/xmm2 at
  `0x2eb1bda`..`0x2eb1bea`), `0x2eb1c41 mov rsi,rbx`.
- **UpdateLine** (PROVEN): `0x15f0077 cmp edx,-1` → line assert; `0x15f0080`..`0x15f00c4` move the
  stops vector out of `[rcx]..[rcx+0x10]` and **zero it at entry**, so the detour must decode the
  Line before it calls the trampoline; `0x15f011c movss xmm0,[rbx+0x18]`, `0x15f0118 [rbx+0x20]`,
  `0x15f013e [rbx+0x28]`; `0x15f009a mov r14,rsi` → `0x15f017e mov rdx,r14` (packager).
- **DeleteLine** (PROVEN): `0x15ebd25 cmp edx,-1` → line assert; `0x15ebd3a mov r14,rsi` →
  `0x15ebd5a mov rdx,r14` (packager); line `ebx` at payload+0 (`0x15ebd60`).
- **SetColor** (PROVEN): `0x15ecb58 movss [rbp-0xdb8],xmm1`, `0x15ecb60 movq [rbp-0xdc0],xmm0`;
  payload entity `[rbp-0xd90]` (`0x15ecbc9`), x `[rbp-0xd8c]` from `[rbp-0xdc0]` (`0x15ecbe8`),
  y and z as one qword `[rbp-0xd88]` from `[rbp-0xdbc]` (`0x15ecbb4`, `0x15ecbe1`). UI caller
  `0x10bec31 movq xmm0,[r13]`, `0x10bec40 movss xmm1,[r13+8]`. `CVec3f` is 12 bytes of float passed
  in two SSE eightbytes, not by reference as on Windows (`r9 -> 3 floats`).
- **SetName** (PROVEN): `0x15ee6ef mov [rbp-0xde8],rcx`; `0x15ee72e mov rbx,[rcx+8]` (length),
  `0x15ee74a cmp rbx,0xf` (SSO capacity); chars from `mov rsi,[rcx]` (`0x15ee8a3`, `0x15ee8c7`) into
  `memcpy` `0x15ee8ac`. libstdc++ layout `{char* p @0; size_t len @8; buf[16] @0x10}`; `p` always
  points at the characters (inline or heap), unlike MSVC's capacity test.

### The packaged Command (PROVEN)

Variant tags: each factory writes the variant tag byte at local payload+0xd48 before packaging:
CreateLine 3 (`0x15eff6f`), DeleteLine 4 (`0x15ebd70`), UpdateLine 5 (`0x15f0197`), SetLine 6
(`0x15ed61a`), SetColor 28 (`0x15ecbf0`), SetName 29 (`0x15ee7d5`). SetColor 28 matches the Windows
dispatch table.

The tag reaches the Command:

- Packaging. Five factories call `0x15ebab0` with their own return slot as `rdi`: SetLine `r14`
  (`0x15ed5fd`), UpdateLine `r13` (`0x15f0184`), DeleteLine `r12` (`0x15ebd5d`), SetColor `r12`
  (`0x15ecbc6`), SetName `r15` (`0x15ee7d2`). `0x15ebab0` keeps that pointer in `rbx` (`0x15ebaca`)
  and calls `0x15eb570` with it (`0x15ebb17`). CreateLine calls `0x15eb570` itself with `r13`, its
  `rdi` (`0x15efdea`, `0x15eff6c`, `0x15eff9e`).
- `0x15eb570` allocates the heap CmdData: `0x15eb588 mov edi,0xd50`, `0x15eb58d call operator new`,
  `0x15eb59b mov byte [rax+0xd48],0`, `0x15eb5a2 mov [rbx],rax`. It then move-assigns the payload into
  it with `0x15f1800` (`0x15eb5da`). If the tags differ, `0x15f1800` move-constructs with
  `0x15f1780`, and `0x15f1780` copies the source tag: `0x15f17ad movzx eax,[r12+0xd48]`,
  `0x15f17b6 mov [rbx+0xd48],al`.
- Readers confirm the location. The createLine maker reads `0x1951b71 mov r14,[rbp-0xa0]` (its
  Command) and then `0x1951b78 movzx eax,[r14+0xd48]`. The SetLine callbacks load
  `0x1434584 mov r13,[rsi]` and test `0x143458a cmp byte [r13+0xd48],6`; a mismatch leads to
  `Unexpected index` (`0x14347f0`).

So at Add, `[[rdx]+0xd48]` is the command's tag.

Command layout (partial):

- `+0x00`: `CmdData*` (above).
- `+0x08`: a `std::vector<ecs::EntityRev>`. The packager moves it into `[rbx+8]..[rbx+0x18]`
  (`0x15ebb5c`..`0x15ebb88`). The callbacks hand Command+8 to
  `ecs::Validate(const Engine&, const std::vector<EntityRev>&, Entity, bool)`
  (`0x14345a2 add rbx,8`, `0x1434663 call 0x325c4f0`).
- `+0x30`: `success`. The invoker `0x10bb0b0` does `0x10bb0d0 cmp byte [rsi+0x30],0`, and `je` leads
  to the assert `command.success` (`0x10bb171`).

## 2. Structures

### `ecs::component::Line` (0x30 B)

| offset | field | type | evidence |
|---|---|---|---|
| +0x00 | stops | `std::vector<Line::Stop>` {begin,end,cap} | PROVEN: member pointer 0 |
| +0x18 | waitingTime | **float**, default 180.0 | PROVEN: member pointer 0x18, type `float Line::*`; factories copy it with `movss` and default-construct `0x43340000` (`0x15efdff`, `0x15f00d3`) |
| +0x20 | vehicleInfo | `LineVehicleInfo` (12 B copied) | PROVEN: member pointer 0x20; not shipped |
| size | 0x30 | | PROVEN: StationWarningsComp indexes the Line component array as `idx*0x30 + [rcx+0xb8]` (`0x12214c6`..`0x12214ce`) and reads stops begin/end at +0/+8 of it; CreateLine's CmdData puts the name string right after the Line at +0x30 (`0x15efec9`) |

Member pointers: the Lua registration `0x228ab50` calls `0x22ec390` at `0x228b331` with
`rcx='stops'` (`0x228b323`), `r8=rbx` = `&[rbp-0x3c0]` (`0x228b0f4 mov rbx,[rbp-0x4f8]`; slot set at
`0x228ad8b`/`0x228adbf`; no other write to rbx in between), `r9='waitingTime'` (`0x228b31c`),
`st0=[rbp-0x4f0]` = `&[rbp-0x370]` (`0x228ad93`/`0x228adb0`), `st1='vehicleInfo'` (`0x228b30f`),
`st2=[rbp-0x4e8]` = `&[rbp-0x330]` (`0x228acd5`/`0x228ad15`); values written just before:
`[rbp-0x3c0]=0` (`0x228b2d6`), `[rbp-0x370]=0x18` (`0x228b2cb`), `[rbp-0x330]=0x20` (`0x228b2c0`).
Types: funcsig `0x2279500` `usertype_metatable<ecs::component::Line, ..., std::vector<Line::Stop>
Line::*, ..., float Line::*, ..., LineVehicleInfo Line::*>`.

### `ecs::component::Line::Stop` (**0xb8 B**, Windows 0xa8)

| offset | field | type | Windows | evidence |
|---|---|---|---|---|
| +0x00 | stationGroup | `ecs::Entity` (int) | +0x00 INFERRED | PROVEN: member pointer 0, type `ecs::Entity Stop::*` |
| +0x04 | stationTerminal.station | int | +0x04 sweep | PROVEN: see below |
| +0x08 | stationTerminal.terminal | int | +0x08 sweep | PROVEN: see below |
| +0x10 | alternativeTerminals | `std::vector<transport::StationTerminal>`, 8 B each | +0x10 | PROVEN: member pointer 0x10; element copy loop strides 8 copying `[rcx]`,`[rcx+4]` (`0xb83240`..`0xb8325a`) |
| +0x28 | loadMode | `Line::LoadMode` (int), 0..3 | +0x28 | PROVEN: member pointer 0x28; values and use below |
| +0x2c | minWaitingTime | float | "+0x2c or +0x30" | PROVEN: member pointer 0x2c, `float Stop::*`; copied with `movss` (`0xb8326c`/`0xb83286`) |
| +0x30 | maxWaitingTime | float | "+0x2c or +0x30" | PROVEN: member pointer 0x30, `float Stop::*`; `movss` (`0xb8328b`/`0xb8329c`) |
| +0x38 | waypoints | `std::vector<transport::SignalId>`, 8 B each | +0x38 | PROVEN: member pointer 0x38; shipped as the 0.5.3 `wp=` suffix |
| +0x50 | stopConfig | `Line::StopConfig` (two bit vectors +0x50/+0x78, a vector +0xa0) | | PROVEN: member pointer 0x50; not shipped |

Stride 0xb8 (PROVEN, four independent sites):
- `std::vector<Stop>` destructor `0xaa43f0` (called on the factories' local stops vectors,
  `0x15effd1`, `0x15f01d9`, and by the line editor after UpdateLine, `0x10bf863`): loop frees
  `[rbx+0xa0]`, `[rbx+0x78]`, `[rbx+0x50]`, `[rbx+0x38]`, `[rbx+0x10]`, then
  `0xaa4459 add rbx,0xb8`.
- vector copy constructor `0xb83700` (Line copy into the payload, `0x15f0176`): `0xb8373a sar rax,3;
  0xb83746 imul rax,0xd37a6f4de9bd37a7` (the inverse of 23 mod 2^64, so size = span / 0xb8).
- element copy `0xb83190`: `0xb83525 add rbx,0xb8; 0xb8352c add r12,0xb8`.
- StationWarningsComp `0x12213a0` walks stops with `0x12217f6 add r15,0xb8`, `lea r12,[rbx+r15]`
  (`0x1221829`) and sizes them with the same `0xd37a6f4de9bd37a7` (`0x12214ea`).

Member pointers (PROVEN): the Stop registration is `0x228b414 call 0x2285020` (a constant-propagated
`NewUsertypeWithReflection<Stop>`; the names are literals inside it), whose member-pointer arguments in
order are `rdx=&[rbp-0x410]` (`0x228b3d3`/`0x228b3eb`, value 0 at `0x228b3b9`), `rcx=&[rbp-0x400]`
(`0x228b3ee`, 0x10 at `0x228b3ae`), `r8=[rbp-0x508]`=`&[rbp-0x3f0]` (`0x228b10a`/`0x228b1c1`, 0x28
at `0x228b3a3`), `r9=[rbp-0x510]`=`&[rbp-0x3e0]` (`0x228b116`/`0x228b1ba`, 0x2c at `0x228b398`),
`st0=[rbp-0x500]`=`&[rbp-0x3d0]` (`0x228ad9a`/`0x228adc6`, 0x30 at `0x228b38d`),
`st1=[rbp-0x4f8]`=`&[rbp-0x3c0]` (0x38 at `0x228b382`), `st2=[rbp-0x4f0]`=`&[rbp-0x370]` (0x50 at
`0x228b377`). The declaration order and types are in funcsig `0x227b430`: `"stationGroup",
ecs::Entity Stop::*, "station", property_wrapper<lambda>, "terminal", property_wrapper<lambda>,
"alternativeTerminals", std::vector<transport::StationTerminal> Stop::*, "loadMode", LoadMode
Stop::*, "minWaitingTime", float Stop::*, "maxWaitingTime", float Stop::*, "waypoints",
std::vector<transport::SignalId> Stop::*, "stopConfig", StopConfig Stop::*`. Cross-check: the
metatable constructor `0x2283cf0` stores each forwarded value next to its name in the (libstdc++,
reverse-ordered) tuple: `[+0x128]`=first ↔ `'stationGroup'` `[+0x130]`, `[+0xf8]` ↔
`'alternativeTerminals'` `[+0x100]`, `[+0xe8]` ↔ `'loadMode'` `[+0xf0]`, `[+0xd8]` ↔
`'minWaitingTime'` `[+0xe0]`, `[+0xc8]` ↔ `'maxWaitingTime'` `[+0xd0]`, `[+0xb8]` ↔ `'waypoints'`
`[+0xc0]`, `[+0xa8]` ↔ `'stopConfig'` `[+0xb0]` (`0x2283e55`..`0x2283f0b`).

`station` and `terminal` are Lua properties (lambdas), not member pointers, because they live in a
nested `transport::StationTerminal stationTerminal`:
- `stationTerminal` at Stop+4 (PROVEN): in StationWarningsComp, `r12 = stops.begin + i*0xb8`
  (`0x1221811`, `0x1221829`), `0x1221701 movsxd rax,[r12+4]; test eax,eax; js 0x1221c2d` → assert
  `stop.stationTerminal.station >= 0` (`0x1221c40`). The element copy moves +4..+0xb as one qword
  (`0xb831d0`/`0xb831dd`).
- `transport::StationTerminal` = `{int station @0; int terminal @4}` (PROVEN): its registration
  `0x2c3f806 call 0x2cb1750` has name `'StationTerminal'` (`0x2c3f7ba`), `rcx='station'`
  (`0x2c3f7f9`) with `r8=r12` = `[rbp-0x790]` = `&[rbp-0x510]` (`0x2c3f4ea`/`0x2c3f4f8`/`0x2c3f74f`,
  no later r12 write) holding 0 (`0x2c3f7ae`), and `r9='terminal'` (`0x2c3f7f2`) with `st0=r14` =
  `&[rbp-0x4d0]` (`0x2c3f259`, no later r14 write) holding 4 (`0x2c3f79f`). Types: funcsig
  `0x2c26c10` `int StationTerminal::*` twice.
- Hence station @ Stop+0x04, terminal @ Stop+0x08; alternativeTerminals elements are
  `{station @0, terminal @4}`.

### `Line::LoadMode` values (PROVEN)

- The C++ enum has four values. `UI::{anonymous}::ToString(ecs::component::Line::LoadMode)`
  `0x10bab20` (its `__PRETTY_FUNCTION__` at `0x10babae`, LineEditor.cpp) maps 0 to
  `Load if available` (`0x10bab7c`), 1 to `Full load (any)` (`0x10bab90`), 2 to `Full load (all)`
  (`0x10baba0`) and 3 to `Unload only` (`0x10bab4c`). Anything else goes to the assert `false`
  (`0x10babc1`). The decode range is 0..3.
- Lua names only 0..2. The `LineLoadMode` table (name loaded at `0x228b75d`) is filled by a loop over
  `0x59cba00..0x59cba48` in 0x18-byte steps (`0x228b7cd`, `0x228b82f`, `0x228b838`):
  - the key is the string `{len [rbx], chars [rbx+8]}` (`0x228b86d`, `0x228b883`, pushed at
    `0x228b8c5`);
  - the value is `[rbx+0x10]` (`0x228b7f5`), pushed as an integer (`0x228b80c`).

  The rows (`.data.rel.ro.local`, name pointers relocated at +8) are `LOAD_IF_AVAILABLE`=0,
  `FULL_LOAD_ANY`=1, `FULL_LOAD_ALL`=2. Value 3 has no Lua name; a replay passes the number.
- Use: in the `ecs::TransportVehicleSystem::Update2(...)::<lambda()>` FDE `0x1774d30`, `r12` is
  `stops.begin + i*0xb8` (`0x1775650 imul rdx,rdx,0xb8`, `0x1775657 add rdx,[rax]`,
  `0x177565a mov r12,rdx`).
  - `0x177577a mov eax,[r12+0x28]; sub eax,1; cmp eax,1; ja`: 1 and 2 are the full-load modes.
  - `0x1775979 mov eax,[r12+0x28]`; for 0 or 3, `0x177597e`..`0x1775991` jump to `0x17771f1`. That
    path skips the full-load branch but still waits minWaitingTime, when the byte `[r15+0x160]` is
    set (`0x17771f1`/`0x17771f9`). It reads `0x17771ff movss xmm0,[r12+0x2c]` and compares
    `min*1e6 + [r15+0x158]` with the current time (`0x1777213`..`0x177722d`).
  - Every other value first tests maxWaitingTime: `0x1775997 movss xmm0,[r12+0x30]`. If max >= 0
    (`0x17759a5 jb` skips the test when max is negative or NaN) and `now - [r15+0x158] >= max*1e6`,
    `0x17759c4 jge 0x17771f1` takes the same min-wait path.
  - Otherwise `0x17759ca cmp eax,2; je 0x177715d` (FULL_LOAD_ALL) and `0x17759d3 cmp eax,1; jne
    0x1777468`, the assert `stop.loadMode == component::Line::LoadMode::FULL_LOAD_ANY`
    (`0x177747b`).
  - So a value outside 0..3 reaches that assert only while maxWaitingTime has not run out or is
    negative/NaN. The decode is unaffected: accept 0..3.

## 3. Wire lines (must match the Lua)

The writers append to the instance's inject file (slice-core owns the path, instance letter and
file access). One `ARMED <0|1>` line precedes every capture (`inject.lua:64`, `CM.lastArmed`); it says
whether the local command was cancelled.

| factory | line | Lua reader (`mod/mp_lockstep_1/res/scripts/mp/inject.lua`) | notes |
|---|---|---|---|
| SetLine | `VLINE <vehicle> <line> <stopIndex>` | `:991` (`#w >= 4`) → `CM.deferVehCap` | ints; stopIndex may be -1 |
| CreateLine | `LCREATE` | `:1004` | event only; the Lua reads the new line back |
| UpdateLine, decoded | `LUPDATE <line> <wait> <n>` then per stop `<sg> <station> <terminal> <loadMode> <min> <max> <nAlt>` then `<station> <terminal>` × nAlt | `:1007-1079`: decoded iff `#w >= 4 and #w >= 4 + n*7` (`:1024`), read sequentially (`:1027-1037`) | ints; `n` may be 0 (last stop removed) |
| UpdateLine, decode failed | `LUPDATE <line>` | `:1080-1085`, 2 words → `CM.lineSnapshot` read-back, `armed = 0` | never cancelled |
| DeleteLine | `LDELETE <line>` | `:1092` | |
| SetColor | `VCOLOR <entity> <r> <g> <b>` (`%.4f`) | `:919` (`#w >= 5`) | Windows ships only when r >= 0 |
| SetName | `VNAME <entity> <name>` | `:919` (`#w >= 3`); `CM.unescName` `cons.lua:131` decodes `%XX` | bytes 33..126 except `%` and `=` literal, everything else `%%%02X`; not shipped when empty |

Cancel policy.

Windows (`DeferHandler` ~3574-3618; `K.STRICT_OPS`, `lockstep.lua:518`, lists VLINE, LUPDATE and
LDELETE) ships all six. It cancels SetLine, UpdateLine and DeleteLine without firing the completion
callback when a session is live, the caller is not a script maker and everything shipped. CreateLine,
SetColor and SetName are shipped only.

For Linux:

- CreateLine is never cancelled. The Linux UpdateLine asserts, and so throws, on a -1 line
  (`0x15f0077 cmp edx,-1` → `0x15f021c call 0x2fcb860`). That the editor issues UpdateLine(-1)
  after a cancelled CreateLine is INFERRED from Windows.
- Whether a SetLine, UpdateLine or DeleteLine cancel may leave its callback unfired is a per-site
  question; section 4, Callbacks, answers it.
  - Firing with success 0 is wrong at the three SetLine sites with a callback (failure branch) and at
    `0x10ca6fe` (throws).
  - Not firing costs only a UI sound or a refresh at most sites.
  - At `0x132c539` not firing leaves a counter raised. Its effect is UNPROVEN, so that site must fire
    its callback (which ignores `success`) or stay uncancelled.
- Skipping Add at all needs the result-slot rule of section 4. Until slice-core implements that rule
  and the callback rule, these three stay ship-only on Linux.

### UpdateLine decode on Linux (replaces `DecodeLine`/`DecodeLineAt`)

Read `rcx` (the Line) at detour entry, before the trampoline (the factory zeroes the stops vector at
`0x15f0083`).

- waitingTime: float at +0x18; accept only `0 <= w <= 36000` (NaN fails); emit rounded int.
  Windows' int-or-float guess is not needed.
- stops: vector at +0x00; Windows' fallback to +0x18 is not needed. A well-formed empty vector
  (`begin == end`, `cap >= end`, `begin == 0` implies `cap == 0`, else both heap pointers) decodes
  as `n = 0`. Otherwise `span % 0xb8 == 0`, `1 <= n <= 64`, the whole span readable.
- per stop (base + i*0xb8): sg int +0x00 (> 0), station int +0x04 (0..64), terminal int +0x08
  (0..64), loadMode int +0x28 (0..3), min float +0x2c and max float +0x30 (each 0..36000, NaN fails,
  emitted rounded). The min/max order is pinned now; Windows sorted the pair only because it was not.
  When min <= max the output is identical.
- alternativeTerminals: vector at stop+0x10, `span % 8 == 0`, at most 8 entries, each station and
  terminal 0..64.
- Any failure: ship `LUPDATE <line>`, do not cancel.

The offsets are PROVEN (section 2). The numeric ranges are filter policy carried over from Windows'
`DecodeLine`, not facts about the game.

## 4. Callers

Exactly **33 direct `e8` calls** reach the six factories, and nothing else refers to them (section 1).

- Per factory: UpdateLine 14, SetColor 7, SetLine 5, SetName 3, DeleteLine 2, CreateLine 2.
- 6 are the script makers (first table below).
- 27 are the other sites (second table), in 26 functions: 26 UI sites, of which `0x10c12c0` holds
  two, plus line_util's CreateLine call `0x2eb1c44`.

### Script makers (the replay filter)

The Lua makers `api.cmd.make.*` are sol2 lambdas of `scripting::SetupCommandInterface` (registration
`0x19638f0`, strings `createLine` `0x19641a7`, `deleteLine` `0x196468e`, `setColor` `0x1966583`,
`setLine` `0x1966a1a`, `setName` `0x1966b04`, `updateLine` `0x196786d`). Each calls its factory once
and hands the Command back to Lua (`0x197fab0`, then `0x15d8f30` on the local); none calls `0x15da840`.
The mod replays through exactly these (`lines.lua:446/456/463`, `vehicles.lua:450/470/582/730`), so a
factory entered from one of these return addresses is a replay and must not be shipped or cancelled.
On Windows this was the block `0xcec000..0xcf2000` plus `0xc3848e` (setColor) and `0xc17eff`; on Linux
all six are exact addresses in the sol2 block, setColor included.

| maker | call → return address | containing function | evidence it is that maker |
|---|---|---|---|
| createLine | `0x1951b63` → **`0x1951b68`** | `0x1951aa0` | address taken at `0x19642cb` in the registration, right after `'createLine'` |
| deleteLine | `0x196a79c` → **`0x196a7a1`** | `0x196a650` | tail-jumped from `0x196a9e0` `functor_function<SetupCommandInterface(...)::<lambda(sol::table, ecs::Entity, sol::this_state)>>`; the only factory it calls is DeleteLine |
| setLine | `0x196b924` → **`0x196b929`** | `0x196b780` | from `0x196bb40` `<lambda(sol::table, ecs::Entity, ecs::Entity, int, sol::this_state)>` |
| setName | `0x196dad3` → **`0x196dad8`** | `0x196d930` | from `0x196dd90` `<lambda(sol::table, ecs::Entity, const string&, sol::this_state)>` |
| setColor | `0x196e65d` → **`0x196e662`** | `0x196e490` | from `0x196eb60` `<lambda(sol::table, ecs::Entity, CVec3f, sol::this_state)>` |
| updateLine | `0x197587f` → **`0x1975884`** | `0x19756a0` | from `0x1975e20` `<lambda(sol::table, ecs::Entity, const ecs::component::Line&, sol::this_state)>` |

### UI sites, Add, and the completion callbacks

At every UI site the factory's return slot is the `rdx` argument of the next call, `0x15da840`,
with no call in between (PROVEN per row: `rdi` source before the factory = `rdx` source before
`0x15da840`; the carriers are callee-saved registers or frame slots).

`0x15da840` arguments (PROVEN at all 28 line-area sites):

- `rdi`: an 8-byte result slot in the caller's frame.
- `rsi`: the CommandList.
- `rdx`: the `Command*`, so `[[rdx]+0xd48]` is its tag (section 1).
- `rcx`: a `std::function<void(const Command&)>`. The signature is in the invoker's assert string
  `UI::LineEditor::LineEditor(...)::<lambda()>::<lambda(const Command&)>` (`0x10bb15e`) and in the
  manager typeinfo names below. The caller destroys it with manager op 3 after Add
  (`mov rax,[slot+0x10]; test; mov edx,3; call rax`, e.g. `0x10c17ae`..`0x10c17b7`).
- `r8`: a 16-byte caller object. Both qwords are zeroed before Add at every site (e.g.
  `0x10bec07`/`0x10bec12`; at the VehicleManager site the loop head `0x126f6c0`/`0x126f6d3` runs
  before each Add). Its second qword is released afterwards as a weak count, `lock xadd [rdi+0xc],-1`
  and then `call [vtbl+0x18]` at zero (`0x10d4c52`..`0x10d4ce1`, `0x126f681`..`0x126f6a9`; helper
  `0x9e2260`). Calling it a weak_ptr/tracker is INFERRED.

That `0x15da840` is `CommandList::Add` is INFERRED (its funcsig is an inlined boost::signals2 name);
slice-core owns that contract.

Invoker ABI (PROVEN): `rdi` = `&_Any_data` (captures through `[rdi]`), `rsi` = `const Command&`,
success byte at `[rsi+0x30]` (section 1).

Result slot (PROVEN):

- `0x3190430` loads `rbx=[rdi]` and returns at once when it is 0 (`0x319043d`,
  `0x3190443 je 0x3190470`). Otherwise it drops a weak count at `[[rbx+8]+0xc]` and frees the 16-byte
  `rbx` (`0x319044e`, `0x3190463` sized delete 0x10).
- At 27 of the 28 sites the first call after Add is `0x3190430` on Add's `rdi` slot (column "Add →
  0x3190430"). Writing 0 to that slot therefore makes the caller's cleanup a no-op when Add is
  skipped.
- The exception is `0x10d4c2f` (LineList's create-line button). There the result first goes to
  `0x31902c0(rdi=rbx, rsi=r13)`, which dereferences it with no null test (`0x31902d3 mov rbx,[rsi]`,
  `0x31902db mov rdx,[rbx]`), then to `0x312cc30(rdi=[r12], rsi=rbx)`. Only then does `0x3190430`
  run on both (`0x10d4c9a`, `0x10d4ca2`). A zero slot would fault there; that path carries
  CreateLine, which is never cancelled.

| factory | return address | function (owner) | rdi ← / Add rdx ← | Add → 0x3190430 | callback at rcx |
|---|---|---|---|---|---|
| SetColor | `0x10bec4e` | `0x10bebc0` (LineEditor.cpp) | `r12` / `r12` | `0x10bec6c` → `0x10bec74` | empty (manager `[rbp-0x40]=0`, `0x10bec1d`) |
| SetColor | `0x12469b1` | `0x1246930` (VehicleDetailsComp ctor lambda, below) | `rbx` / `rbx` | `0x12469cf` → `0x12469d7` | empty (`0x1246981`) |
| SetColor | `0x126f62b` | `0x126f4e0` (VehicleManager ctor nested lambda, typeinfo `0x5a1cb60`) | `[rbp-0xe8]` / same | `0x126f64e` → `0x126f65a` | invoker `0x126bea0`, manager `0x126db00` (not decoded: never cancelled) |
| SetColor | `0x144407a`, `0x144418a`, `0x144429a` | `0x1444000`, `0x1444110`, `0x1444220` (ViewCreator.cpp) | `rbx` / `rbx` | `0x1444098` → `0x14440a0`; `0x14441a8` → `0x14441b0`; `0x14442b8` → `0x14442c0` | empty (`0x144404a`, `0x144415a`, `0x144426a`) |
| UpdateLine | `0x10bf829` | `0x10bf620` (LineEditor.cpp) | `r12` / `r12` | `0x10bf84b` → `0x10bf853` | empty (`0x10bf7de`) |
| UpdateLine | `0x10c0679` | `0x10c0400` (LineEditor.cpp) | `r12` / `r12` | `0x10c069b` → `0x10c06a3` | empty (`0x10c05f6`) |
| UpdateLine | `0x10c1770`, `0x10c1946` | `0x10c12c0` `UI::LineEditor::DeleteTerminal(int)` | `r13` / `r13` | `0x10c1791` → `0x10c1799`; `0x10c1967` → `0x10c196f` | empty (`0x10c1711`; `0x10c18e0`) |
| UpdateLine | `0x10c25fa` | `0x10c1a40` (LineEditor.cpp) | `r14` / `r15` (`0x10c25f2 mov r15,r14`) | `0x10c261f` → `0x10c2627` | `0x10bb2e0` (manager `0x10ba710`): success only, sound `addStation` |
| UpdateLine | `0x10c305c` | `0x10c2bb0` (LineEditor.cpp) | `r14` / `r14` | `0x10c307e` → `0x10c3086` | `0x10bb310` (`0x10ba750`): success only, sound `addWaypoint` |
| UpdateLine | `0x10c9e1c` | `0x10c9b50` (LineEditor.cpp) | `r12` / `r12` | `0x10c9e3e` → `0x10c9e46` | empty (`0x10c9dbe`) |
| UpdateLine | `0x10ca6e0` | `0x10ca2a0` (LineEditor.cpp) | `r13` / `r13` | `0x10ca6fe` → `0x10ca706` | `0x10bb0b0` (`0x10bb3d0`): success 0 asserts and throws |
| UpdateLine | `0x1207337` | `0x1206ba0` (AddLineAndStops lambda, below) | `[rbp-0x158]` / same | `0x120735c` → `0x1207364` | `0x1206520` (`0x1206900`): ignores success, refresh |
| UpdateLine | `0x12193d0` | `0x1218c40` (AddLineAndStops lambda, below) | `[rbp-0x168]` / `r15` (`0x12193d0 mov r15,[rbp-0x168]`) | `0x12193f5` → `0x12193fd` | `0x12168c0` (`0x1216e60`): ignores success, refresh |
| UpdateLine | `0x132bbdc` | `0x132ba10` `CreateStationTerminalComboBox::<lambda(int)>` (line_ui_util.cpp) | `r12` / `r12` | `0x132bc05` → `0x132bc11` | empty (`0x132bb80`) |
| UpdateLine | `0x132bec4` | `0x132bcf0` `CreateWaypointLaneComboBox` lambda (line_ui_util.cpp) | `r12` / `r12` | `0x132beed` → `0x132bef9` | empty (`0x132be68`) |
| UpdateLine | `0x132c513` | `0x132bfe0` `CreateAlternativeTerminalsButton` lambda (line_ui_util.cpp) | `[rbp-0x128]` / same | `0x132c539` → `0x132c541` | `0x13228e0` (`0x1323eb0`): ignores success, lowers a counter |
| DeleteLine | `0x1326aa0` | `0x1326900` (line_ui_util.cpp) | `r13` / `r13` | `0x1326abe` → `0x1326ac6` | `0x1322950` (`0x13228f0`): success only, sound `removeLine` |
| SetName | `0x1327526` | `0x13273f0` (line_ui_util.cpp) | `r14` / `r14` | `0x1327548` → `0x1327550` | `0x1327640` (`0x1325110`), not decoded (never cancelled) |
| SetName | `0x14287df` | `0x1428700` (UI/Util/util.cpp) | `r13` / `r13` | `0x1428801` → `0x1428809` | `0x1426830` (`0x1428480`), not decoded (never cancelled) |
| SetLine | `0x14334f7` | `0x1433220` (vehicle_button_util.cpp) | `r12` / `r12` | `0x143351c` → `0x1433524` | `0x1434550` (`0x1432c60`): success 0 takes the failure branch |
| SetLine | `0x1437ec1` | `0x1437b90` (vehicle_button_util.cpp) | `rbx` / `rbx` | `0x1437ee3` → `0x1437eeb` | `0x1434240` (`0x142fbb0`): success 0 takes the failure branch |
| SetLine | `0x14385fa` | `0x1438260` `vehicle_button_util::SetLineAllInstallHandlers` | `[rbp-0x110]` / same | `0x143862e` → `0x143863a` | `0x1438990` (`0x1430460`): success 0 takes the failure branch |
| SetLine | `0x14465db` | `0x1446460` (ViewCreator.cpp) | `r12` / `r12` | `0x14465f9` → `0x1446601` | empty (`0x144659d`) |
| CreateLine | `0x2eb1c49` | `0x2eb0fa0` (transport/line_util.cpp; Windows `0x215c26b`) | `[rbp-0x138]`, returned in rax | see below | see below |

"Empty" means the only write to the manager slot `slot+0x10` between function entry and Add is
`mov qword [slot+0x10],0`; the full stretch from that write to the Add call was checked for any other
write or call that could fill it.

CreateLine through line_util (PROVEN). `0x2eb0fa0` stores its own `rdi` at `[rbp-0x138]`
(`0x2eb0fb8`), passes it as CreateLine's `rdi` (`0x2eb1c3a`) and returns it
(`0x2eb1c93 mov rax,[rbp-0x138]`). Its two callers pass that slot to `0x15da840` as `rdx`:

- `0x10d4c08` in `0x10d4b60`, `rbx` / `rbx`, Add `0x10d4c2f`; invoker `0x10d6c30`, manager
  `0x10d44b0`.
- `0x10d9d9c` in `0x10d9d00` (LineManager.cpp), `r14` / `r14`, Add `0x10d9dbf` → `0x10d9dc7`;
  invoker `0x10e1a60`, manager `0x10d8db0`.

So `Add.rdx == CreateLine.rdi` holds on this path too.

### Callbacks (PROVEN code paths)

| class | Add sites | fired with success 0 | never fired |
|---|---|---|---|
| empty `std::function` | UpdateLine `0x10bf84b`, `0x10c069b`, `0x10c1791`, `0x10c1967`, `0x10c9e3e`, `0x132bc05`, `0x132beed`; SetLine `0x14465f9`; SetColor `0x10bec6c`, `0x12469cf`, `0x1444098`, `0x14441a8`, `0x14442b8` | nothing to fire | nothing lost |
| success only (sound) | UpdateLine `0x10c261f` (`0x10bb2e0`), `0x10c307e` (`0x10bb310`); DeleteLine `0x1326abe` (`0x1322950`) | returns at once (`0x10bb2e4`/`0x10bb318`/`0x1322954 cmp byte [rsi+0x30],0` then `ret`) | the sound is not played |
| ignores success (refresh) | UpdateLine `0x120735c` (`0x1206520`), `0x12193f5` (`0x12168c0`) | same as on success: `rdx=[rdi]` (the closure); when the control block `[rdx+0x10]` is non-null with use count `[+8] != 0` (`0x1206527`..`0x1206535`) it tail-calls `0x1323bb0([rdx], -1, -1, -1, -1)` (`0x1206546`..`0x1206552`; `0x12168f2` likewise) | that refresh is skipped |
| ignores success (counter) | UpdateLine `0x132c539` (`0x13228e0`) | `mov rax,[rdi]; mov rax,[rax]; sub dword [rax],1`: lowers the int that `0x132c3f7 add dword [r12],1` (`r12=[r15+0x58]`, `0x132c3eb`) raised before the command was built | the counter stays one too high; what reads it is **UNPROVEN** |
| failure branch | SetLine `0x143351c` (`0x1434550`), `0x1437ee3` (`0x1434240`), `0x143862e` (`0x1438990`) | success 0 does not take the `jne` to the success path (`0x143457e`, `0x143426e`, `0x14389be`; `0x1438990` first requires a live captured weak reference, `0x14389c0`..`0x14389ce`), checks tag 6 (`0x143458a`, `0x1434277`, `0x1438a4d`), validates the entities, then calls `0x1433e00` (`0x1434775`, `0x143447d`, `0x1438d41`), which loads `Unable to find path to stop` (`0x1433e43`), `Train does not have a locomotive` and the path-hint strings and passes them to `tr` `0x2fd1420` | the one-shot `setLine` sound (`0x14347c8`, `0x14344e0`, `0x14389f8`) is not played |
| throws | UpdateLine `0x10ca6fe` (`0x10bb0b0`) | `0x10bb0d7 je 0x10bb15e` → assert `command.success` → `0x2fcb860` throws | the success action, a new `std::function` (invoker `0x10d2010`, manager `0x10bb460`) handed to `0x304ed10` (`0x10bb129`), is skipped |

Sounds:

- The calls are PROVEN. `0x3075360(0)` returns the global at `0x5b360b8` (a Lib/UI/Core.cpp function
  next to `UI::SetGlobalCore` `0x3075310`); the callbacks pass that result and a static
  `std::string` to `0x3076e60`.
- The strings are PROVEN. They are built in `0x95a1b0`: `0x5a509c0` = `removeLine`
  (`0x95aa28`/`0x95aa2f`), `0x5a509a0` = `addStation` (`0x95aa55`/`0x95aa5c`), `0x5a50980` =
  `addWaypoint` (`0x95aa82`/`0x95aa89`), `0x5a50920` = `setLine` (`0x95ab09`/`0x95ab10`).
- That these are UI sounds is INFERRED: `res/scripts/soundeffectsutil.lua` lines 64-71 key its
  sound files by the same names (`removeLine` is commented out there).

The Windows note about a "false toast" matches the failure branch above for SetLine. Its visible
outcome in the Linux game is not observed.

### Owners of the UI functions without their own source string

Each owner below is PROVEN by the lambda typeinfo of the `std::function` built around the function.
The source file is INFERRED from the owner and address order. All four are `UI::` code, which is all
the replay filter needs.

- `0x1206ba0`: body of `UI::{anonymous}::AddLineAndStops(CommandList&, UI::EnginePtr,
  UI::TransportNetworkSystemPtr, UI::HudHelper*, const std::vector<std::pair<ecs::Entity,
  std::tuple<int,int,int>>>&, UI::CBoxLayout*)::<lambda(UI::CCore*, int)>`.
  - The invoker thunk `0x12075b0` (`jmp 0x1206ba0`) is stored at `[rbp-0x168]` (`0x1207e02`) next
    to manager `0x12069d0` at `[rbp-0x170]` (`0x1207e10`), in `0x12075d0`; typeinfo `0x5a1a750`.
  - Its UpdateLine callback `0x1206520` is that lambda's `<lambda(const Command&)>` #2 (manager
    `0x1206900`, typeinfo `0x5a1a740`).
  - Source file: between SectionTypeComp.cpp (`0x11fe920`) and SpeedLimitsComp.cpp (`0x120b050`),
    INFERRED.
- `0x1218c40`: body of `<lambda(UI::CCore*, transport::StationTerminal)>` in a second
  `UI::{anonymous}::AddLineAndStops` (this one also takes
  `std::vector<UI::ManualFocusIterationEntry>&`).
  - The thunk `0x1219740` is stored at `0x121a6bb` next to manager `0x1216f30` (`0x121a6c9`), in
    `0x1219760`. That function carries the `Game/UI/Components/StationGroupTerminalsComp.cpp` source
    string (funcsig `UI::StationGroupTerminalsComp::Refresh`); typeinfo `0x5a1ade0`.
  - Its callback `0x12168c0` belongs to the same lambda (manager `0x1216e60`, typeinfo `0x5a1add0`).
  - Source file StationGroupTerminalsComp.cpp: INFERRED, strong, since an anonymous-namespace lambda
    is created in a function of that file.
- `0x1246930`: invoker of the second
  `UI::VehicleDetailsComp::VehicleDetailsComp(...)::<lambda(const CVec3f&)>`.
  - It is stored at `[rbp-0x48]` (`0x12470bb`) next to manager `0x1246710` at `[rbp-0x50]`
    (`0x12470c6`), in `0x1246ac0`, then handed to `0x30447e0`; typeinfo `0x5a1c068`.
  - Source file VehicleDetailsComp.cpp (next source string `0x1249450`): INFERRED.
- `0x10d4b60`: invoker of the first `UI::LineList::LineList(...)::<lambda()>`.
  - It is stored at `[rbp-0x48]` (`0x10d5225`) next to manager `0x10d4d70` at `[rbp-0x50]`
    (`0x10d5233`), in `0x10d4e80`, and passed to the button click connect `0x3028dd0` (`0x10d5237`;
    menu_linux.cpp); typeinfo `0x5a144f0`. It is LineList's create-line button.
  - Source file LineList.cpp: INFERRED.

## 5. Integration notes (outside this area)

- **Relay (slice-core).** For these six hooks the handler needs `rdi, rsi, rdx, rcx, r8`, **`xmm0`
  and `xmm1`** (SetColor and CreateLine carry the colour there) and the return address `[rsp]` at
  entry; no stack arguments. The relay must preserve every SysV argument register (`rdi, rsi, rdx,
  rcx, r8, r9, rax, xmm0-xmm7`) and restore rsp before jumping to the 14-byte trampoline. A factory's
  own assert throws (section 1), so the detour must not let a C++ exception of its own escape.
- **Pending cancel (slice-core).** Identify the command at Add by `Add.rdx == factory.rdi` (all 26 UI
  sites; CreateLine through line_util too). As a type check, `[[rdx]+0xd48]` must equal the factory's
  tag: CreateLine 3, DeleteLine 4, UpdateLine 5, SetLine 6, SetColor 28, SetName 29.
- **Skipping Add (slice-core).** Write 0 to the 8-byte slot at Add's `rdi`; `0x3190430` then returns
  at once at the 27 sites that destroy the slot first. Not valid at `0x10d4c2f` (CreateLine,
  never cancelled).
- **Callbacks (slice-core).** Use the per-site classes in section 4:
  - Do not fire at the failure-branch and throwing sites.
  - Nothing is lost at empty sites, and only a sound at success-only sites.
  - The two refresh sites behave the same either way (not firing skips one refresh).
  - At `0x132c539`, fire the callback or leave the command uncancelled, because the counter's reader
    is UNPROVEN.

  Firing needs a correct libstdc++ invoke through `[rcx+0x18]` with `rdi=rcx, rsi=Command*`; that
  code belongs to slice-core.
- **Shared services (slice-core).** `ARMED`, the inject file, the instance letter, `SessionLive`, a
  fault-safe memory read for the Line walk, and the log file are expected from slice-core.
- **Hook ids.** Keep 6, 7, 8, 9, 13, 14 so logs stay comparable with Windows.

## 6. Differences from Windows

| item | Windows | Linux |
|---|---|---|
| return slot / args | rcx / rdx, r8, r9, `[rsp+0x28]` | rdi / rsi, rdx, rcx, r8, xmm0, xmm1; no stack args |
| SetLine stop index | `[rsp+0x28]` | `r8d` |
| CreateLine Line | `[rsp+0x28]` | `rcx` |
| SetColor colour | `r9` → 3 floats | `xmm0` (x, y), `xmm1` (z) |
| SetName string | MSVC: len +0x10, cap +0x18, inline iff cap < 16 | libstdc++: p +0, len +8 |
| steal | 15..20 | 14 for all six |
| script filter | address block plus two addresses | six exact return addresses |
| Stop stride | 0xa8 | 0xb8 (stopConfig's two bit vectors are 40 B each in libstdc++) |
| waitingTime | int or float, unrecorded | float |
| min/max wait | unpinned, sorted | min +0x2c, max +0x30 |
| stationGroup +0 | INFERRED | PROVEN |
| Add callback arg | r9, MSVC `std::function` (impl at +0x38) | rcx, libstdc++ `std::function` (manager +0x10, invoker +0x18) |
| Add result | `*out = 0` makes the caller's destructor a no-op | `[rdi slot] = 0` makes `0x3190430` a no-op (27 of 28 sites) |
| tag at Add | not used | `[[rdx]+0xd48]` |

## 7. Not established

- `0x15da840` is CommandList::Add (INFERRED name; slice-core owns the Add hook).
- What reads the int that `0x132bfe0` raises (`0x132c3f7`) and its callback lowers (`0x13228e0`).
  A bounded search found no other reader in that lambda. `0x1329e90` (reached through the thunk
  `0x132aa70` from `0x1329250`) also increments a `[rdi+0x58]` int and acts once it exceeds 2
  (`0x1329ea4`..`0x1329ec1`), but no data flow ties its closure to `0x132bfe0`'s, so they are not
  shown to be the same int. **UNPROVEN**; a cancel from `0x132c539` that skips the callback stays off.
- Runtime confirmation: nothing here was observed in the running Linux game (the SetLine failure
  message, the throw at `0x10bb0b0`, a Lua replay with loadMode 3).
- The source files of `0x1206ba0`, `0x1218c40`, `0x1246930` and `0x10d4b60` (INFERRED; owners PROVEN).
