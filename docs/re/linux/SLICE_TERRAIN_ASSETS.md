# Slice, terrain and asset carriers: the Linux map

This is the Linux RE for the slice's terrain tools (terraform and paint) and its asset brush. It
covers five things:

- the capture off the `UI::ProposalAction` commit (`LogTerrainProposal`, `StashAssetsFromProposal`)
- the terrain replay carrier (`terrain_inject_<L>.bin`, `InjectTerrainFromFile`)
- the asset replay carrier (`asset_inject_<L>.txt`, `InjectAssetsFromFile`)
- the tool hold/release (`HoldTerrainTool`, `ReleaseTerrainTool`)
- the proposal emptiness tests (`TerrainCarrierEmpty`, `ProposalIsEmpty`)

The Linux code replaces `native/src/slice_hook.cpp` ~2477-3320 and their call sites in `DeferHandler`
(~3364-3372, 3421/3484/3505/3528-3535/3551, 3621-3681, 3694-3704).
Code goes in `native/linux/src/slice/slice_terrain_assets.{h,cpp}`.

Binary: `TransportFever2`, Steam build 35924, GNU build-id `3a0e156390b0e6f1e372051c24802c8493ae454a`.
Addresses are Linux RVAs (the PIE links at 0; live = image base + RVA). "Windows" means the RVAs in
`docs/re/*.md` and `slice_hook.cpp`, relative to `0x140000000`.

Status labels:

- **PROVEN**: the instructions, strings or data quoted here show it, and it can be re-derived from
  the binary.
- **INFERRED**: placed by address order, by the Windows docs or by elimination. It stays out of code
  that writes game memory or calls the game.

Nothing here was checked in the running game (static MAP stage, revised after independent
verification: the wait-flag gating (section 5.1), the Context size, the identities in section 0, the
carrier-added log (section 9) and the carrier-grid expectation (section 4.1) were re-derived).

## How the evidence was produced

- **Disassembly:** a capstone (x86-64) linear sweep from each function start over its FDE size, taken
  from `/home/topsnek/tpf2-re/linux/functions.csv`. RIP-relative loads are resolved to `.rodata`
  strings, to `.plt` entries through `.rela.plt`, and to exported `.dynsym` names.
- **Direct callers:** a byte scan of `.text` for `e8`/`e9` rel32 hitting the target, keeping only
  hits that fall on an instruction boundary of their function's sweep.
- **Address references:** a scan of `.text` for RIP-relative operands resolving to the target, and
  of `.rela.dyn` for relocation addends equal to it.
- **Reachability:** a per-function control-flow graph from the sweep (direct branches only; the
  functions checked have no indirect `jmp`), with one edge removed to ask what stays reachable.
- **Vtables:** `R_X86_64_RELATIVE` relocations. The typeinfo is the relocation whose addend is the
  name string; the vtable slot -1 is the relocation whose addend is the typeinfo.
- **Layouts:** from the Proposal constructor, destructor and copy constructor, which touch every
  member, and from the game's own builders (`CreateProposalAddAsset` and the AssetBrush update).
- **Inline ConstructionEntity inits:** diffed offset by offset with a script. The registers they
  store were traced back to their constants.

The throwaway scripts live in the area's scratch directory. The method above is enough to redo any row.

## 0. What this area hooks: nothing

This area installs **no hook and writes no code**. It reads and edits proposal memory handed to it
by other areas' hooks, and it calls game functions. It depends on two hooks owned elsewhere
(slice-core / slice-proposal). The facts this area needs from each are below.

| hook (owner) | Windows | Linux RVA | first 14 bytes (instruction boundary at 14) | what this area needs |
|---|---|---|---|---|
| BuildProposal command factory (slice-core / slice-proposal) | `0x9dc750`, id 15 | `0x15ee930` (FDE 2674) | `f30f1efa 55 4889e5 4157 4156 4155` (next: `41 54 53 48 81 ec 00 10 00 00`, a stack probe) | the Proposal pointer (**rdx**), the Context pointer (**rcx**) and the return address. Proposal and Context are the Windows r8 and r9. |
| `CommandList::Add` (slice-core) | `0x9d2a00`, id 1 | `0x15da840` (FDE 5045) | `f30f1efa 55 4889e5 4157 4156 4155` (next: `41 54 53 48 89 cb`) | the Command (**rdx**) and the completion `std::function*` (**rcx**). The Windows code had them in r8 and r9. |

### 0.1 `0x15ee930` is the BuildProposal command factory (PROVEN)

- **It builds a `CmdData::BuildProposal`.**
  - `0x15ef28d lea r14,[rbp-0xd90]` and `0x15ef29a call 0x15f39d0(rdi=r14, rsi=rbx)` construct a
    local CmdData.
  - `0x15ef2ab c6 45 b8 0f mov byte [rbp-0x48],0xf` writes its variant index at CmdData+0xd48
    (0xd90 - 0x48).
  - `0x15ef2af call 0x15ebab0` packages it (rdi = r12, the Command out slot; rsi = r14).
  - +0xd48 is the CmdData variant index. `~Command` `0x15d8f30` reads it (`0x15d8f7e`), destroys the
    CmdData through `_Variant_storage<…>::_S_vtable` `0x59be2a0`, then frees 0xd50 bytes
    (`0x15d8f9d be 50 0d 00 00`, `_ZdlPvm`).
  - The exported symbol at `0x59c3a60`
    (`_ZZNSt8__detail9__variant15_Copy_ctor_baseILb0EJN7CmdData12SetGameSpeedENS2_16SetCalendarSpeed…E9_S_vtable`)
    spells the variant's 37 alternatives in order. Index 15 is `CmdData::BuildProposal`
    (0 SetGameSpeed … 13 BuyVehicle, 14 ReplaceVehicle, 15 BuildProposal, 16 RemoveField).
- **Where it sits.** It lies in the `make_command.cpp` block, between `make_cmd::SetName` `0x15ee6d0`
  (598 B, ends `0x15ee926`; assert signature at `0x15ee8e3`) and `make_cmd::BuyVehicle` `0x15ef3b0`
  (assert signature at `0x15ef81f`).
- **Lua calls it.** It is what Lua `buildProposal` runs (0.3).

The C++ spelling `make_cmd::BuildProposal` follows the block and the Windows name; no string in the
function spells it, and nothing here depends on it.

**Callers (PROVEN).** There are exactly 18 direct `call rel32` sites in 17 functions, with no `jmp`, no
RIP-relative reference and no relocation addend equal to `0x15ee930`:

- UI builders:
  - `0xdd4e94` (`UI::Bulldozer::Apply` `0xdd4590`)
  - `0xe3485b` (`UI::ConstructionBuilder::MousePressed` `0xe343c0`)
  - `0xe4f6b8` (`UI::ModuleBuilder::MousePressed` `0xe4eb30`)
  - `0xe5961d` (ProposalAction commit `0xe59490`)
  - `0xe8644d` (`UI::StreetBuilder::UpdateEngine` `0xe861b0`)
  - `0xeaaf74` (StreetTerminalBuilder commit `0xeaabb0`)
  - `0xed6da4` (`UI::TrackModifier::Build` `0xed69d0`)
- Other UI functions:
  - `0xf229a0` (`0xf225c0`)
  - `0x1045732` and `0x1045a42` (`0x1045460`, `CGameUI::CreateConstructionMenu` lambda)
  - `0x12409dd` (`0x1240730`), `0x1240e94` (`0x1240bf0`), `0x12413fd` (`0x12410b0`)
  - `0x145e1a7` (`0x145ddc0`, `CreateSignalView` lambda)
  - `0x145e540` (`0x145e380`), `0x145ef7d` (`0x145ece0`), `0x145fbc0` (`0x145f8c0`)
- Lua: `0x197132e` (`0x1970a10`).

The return addresses `0xe59622` and `0x1971333` therefore identify exactly one site each.

### 0.2 BuildProposal: registers and edit-at-entry (PROVEN)

`0x15ee930` saves registers at entry:

- `rdi` (the Command out slot) in r12 (`0x15ee95b`)
- `rcx` in r15 (`0x15ee95e`)
- `rdx` at `[rbp-0x1b10]` (`0x15ee964`)
- `rsi` in r13 (`0x15ee972`)
- `r8d` at `[rbp-0x1b04]`, `r9d` in r14d

The slot `[rbp-0x1b10]` is written only there and read only at `0x15ef154 mov rdx,[rbp-0x1b10]`, then
`0x15ef171 mov rsi,rdx` and `0x15ef17f call 0xde0430` (a scan of every `[rbp-0x1b10]` operand in the
FDE). So an edit made to `*rdx` at function entry is what the command is built from, just as on
Windows.

| caller | Windows return | Linux call → return | registers at the call | evidence |
|---|---|---|---|---|
| `UI::ProposalAction::commit` (terraform, paint, asset brush) | `0x4311c6` (`CALLER_PROPOSALACTION`) | `0xe5961d` → **`0xe59622`** | rdi = Command slot `rbp-0x4d0`; rsi = `[[this+0x38]+0x28]`; **rdx = a local COPY of the tool's proposal** (`rbp-0x420`, made by `0xe3e060` at `0xe595f3`); **rcx = Context** (`rbp-0x490`); r8d = 1; r9d = commit arg 1 (sil) | disassembly of `0xe59490`, section 5 |
| Lua `buildProposal` (the replay carrier) | `0xced378` | `0x197132e` → **`0x1971333`** | rdi = Command slot r15 = `rbp-0x2680` (`0x197119b`); rsi = r13; **rdx = a COPY** (`rbp-0x1ea0`, made by `0xe3e060` at `0x1971310` from the `scripting::Convert` result `rbp-0x2260`, `0x1971288`); **rcx = Context** (`rbp-0x2640`); r8d = 1; r9d = `[rbp-0x26cc]` | identity in 0.3 |

The Lua carrier passed in rdx is a game-owned temporary, destroyed by `~Proposal` `0xdc3040` after
the call (`0x1971354`). **Everything injected into it must come from the game's `operator new`**
(section 4).

### 0.3 `0x1970a10` is the body of Lua `buildProposal` (PROVEN)

The chain from the registered name to the call site:

1. **Registration.** In the registration function `0x19638f0` (its strings include `sendCommand`
   `0x1963a2d` and `make` `0x1963acd`; the lambda types it instantiates are local to
   `scripting::SetupCommandInterface`):
   - `0x1963c69..0x1963c93` build the argument names `proposal`, `context`, `ignoreErrors`, and
     `0x1963caa call 0x1950e10(r12, rbx, 3)` puts them in a vector.
   - `0x1963cbe 48 8d 35 af c2 5d 02 lea rsi,'buildProposal'` builds the key string in r14
     (`0x1963cc8`). This is the only reference to that string in `.text`.
   - `0x1963ce0 e8 cb c7 ff ff call 0x19604b0` passes rsi = r14 (the key) and rcx = r12 (the names).
     It is the only call of `0x19604b0`.
2. **The functor.** `0x19604b0` is
   `sol::detail::user_allocate<functor_function<SetupCommandInterface(…)::<lambda(sol::table, scripting::Proposal, std::optional<construction_builder_util::Context>, bool, sol::this_state)>>>`
   (assert signature at `0x1960afd`).
   - It copies the key (`[rsi]`, `[rsi+8]`) into its `{table, key}` proxy (`0x19604e0..0x1960513`)
     and hands that proxy on (`0x196052c`).
   - It pushes the C closure `0x1972920` (`0x19606f0 48 8d 35 29 22 01 00 lea rsi`,
     `0x19606fd call 0x19509e0`) and stores it as `__call` (`0x1960708`, `0x1960712`).
   - It builds the `__doc__` text `… ) -> Command` (`0x19607bd`, `0x19607e2`, `0x1960877`).
3. **Closure to body.** `0x1972920` is referenced only by that `lea`: no other RIP-relative operand,
   no relocation. It tail-jumps with `0x19729b4 e9 57 e0 ff ff jmp 0x1970a10`. `0x1970a10` has no
   other reference (one rel32 hit, no RIP-relative operand, no relocation), and it calls the factory
   at `0x197132e`.
4. **The pairing is checked against named factories.** Two keys in the same registration function
   follow the same shape and end in factories named by assert signatures:
   - `'buyVehicle'` `0x1963db8` → `0x1963dda call 0x1960c30` → closure `0x1974d80` →
     `jmp 0x1974630` → `0x197483c call 0x15ef3b0` (`make_cmd::BuyVehicle`)
   - `'setName'` `0x1966b04` → `0x1966b26 call 0x195c1d0` → closure `0x196dd90` →
     `jmp 0x196d930` → `0x196dad3 call 0x15ee6d0` (`make_cmd::SetName`)
5. **The mod matches the registered names.** It calls
   `api.cmd.make.buildProposal(api.type.SimpleProposal.new(), ctx, false)` (`terrain.lua:72,82`,
   `assets.lua:131,139`), which is three arguments, like the registered names.

### 0.4 `0x15da840` is `CommandList::Add` (PROVEN; slice-core owns the hook)

- **The lambda.** `0x15da310` carries the assert signature
  `CommandList::Add(Command, std::function<void(const Command&)>, std::weak_ptr<CmdProgress>)::<lambda(const boost::signals2::connection&, const std::vector<Command>&)>`
  (`0x15da66c`) and `…/src/Game/command/CommandList.cpp` (`0x15da678`).
- **Its boost::function vtable.** `.rela.dyn` fills `0x59be5f0 → 0x15da710` (manager) and
  `0x59be5f8 → 0x15da310` (invoker), both `R_X86_64_RELATIVE`. No other relocation has either
  addend. No RIP-relative operand references `0x15da310`. The only one referencing `0x59be5f0` is
  `0x15dab9e 48 8d 05 4b 3a 3e 04 lea rax` inside `0x15da840`. A lambda local to `CommandList::Add`
  is installed only there.
- **Add's parameters.** `0x15da840` takes exactly those parameters (SLICE_CORE.md C-ADD-3):
  - hidden return in rdi, `this` in rsi
  - the Command in rdx, moved into the list through `0x15d8ea0`
  - the `std::function` in rcx, moved (`[rbx+0x10]` zeroed at `0x15da955`)
  - the `weak_ptr` in r8, moved
- **Callers.** It has 94 direct call sites in 85 functions (byte scan).

At the ProposalAction site, `0xe59652 call 0x15da840`:

- rdi = result handle `rbp-0x4e8`
- rsi = `[this+0x60]` (the CommandList)
- rdx = the Command `rbp-0x4d0`
- **rcx = `rbp-0x60`, the completion `std::function`**
- r8 = `rbp-0x4e0` (a zeroed 16-byte pair)

Return address `0xe59657`. After the call come `0x3190430` (on the handle), `0x15d8f30` (on the
Command), `0xdc3040` (on the proposal copy) and `0xdd8820` (on the Context).

## 1. `construction_builder_util::Proposal`: Linux layout (0x3c0 B)

Windows is 0x2f8 B. libstdc++ containers have different sizes, so every offset from +0x060 on moves.

| Linux | Windows | member | element | evidence (Linux) | status |
|---|---|---|---|---|---|
| +0x000 | +0x000 | addedNodes | trivially destructible | ~StreetProposal `0xdc0280`: `0xdc0439 mov rdi,[rbx]` → free | PROVEN (the offset) |
| +0x018 | +0x018 | addedSegments | 0x78 B, `objects` vector at +0x30 | `0xdc0403..0xdc041f`: loop `add r12,0x78`, free `[r12+0x30]` | PROVEN |
| +0x030 | +0x030 | removedNodes | trivial | `0xdc03f1` free `[rbx+0x30]` | PROVEN |
| +0x048 | +0x048 | removedSegments | 0x78 B, vector at +0x30 | `0xdc03b4..0xdc03d7` | PROVEN |
| +0x060 | +0x060 | unordered container (56 B) | | ctor `0xdc2baa` buckets = &+0x90, `+0x68`=1, `+0x80`=1.0f; dtor `0xdc03ab call 0xbfaf70` | PROVEN |
| +0x098 | +0x0a0 | unordered container (56 B) | | ctor `0xdc2bb5` buckets = &+0xc8; dtor `0xdc039f` | PROVEN |
| +0x0d0 | +0x0e0 | edgeObjectsToRemove | trivial | `0xdc038e` free `[rbx+0xd0]` | PROVEN (offset); name from Windows order INFERRED |
| **+0x0e8** | **+0x0f8** | **edgeObjectsToAdd** | 0x100 B: `std::string` at +0xd8 (buf +0xe8), vectors at +0xa0, +0xb8 | `0xdc0317..0xdc036e` loop `add r12,0x100` | PROVEN (shape matches the Windows 0x100 record with its name string at +0xd8) |
| +0x100..+0x220 | +0x110..+0x170 | six `std::map`/`_Rb_tree` (48 B each) | | ctor headers at +0x108/+0x138/+0x168/+0x198/+0x1c8/+0x1f8 (`color=0`, `leftmost=rightmost=&header`); dtor `0xdc0220` ×6 | PROVEN |
| +0x220 | +0x170 | frozen node indices | trivial | `0xdc0294` free `[rdi+0x220]` | PROVEN (offset) |
| (end of StreetProposal) | | | | 0x238 on Linux, 0x188 on Windows | |
| +0x238 | +0x188 | `unordered_set<int>` | element_count **+0x250** | ctor `0xdc2e31` buckets = &+0x268, `+0x240`=1, `+0x258`=1.0f; dtor `0xdc311c call 0xdbf800` | PROVEN |
| **+0x270** | **+0x1c8** | segmentTags `vector<std::string>` | 32 B | `0xdc3110 call 0x9b7f60` (exported `_ZNSt6vectorINSt7__cxx1112basic_string…EED1Ev`) | PROVEN |
| **+0x288** | **+0x1e0** | **toRemove `vector<ecs::Entity>`** | 4 B | `0xdc30f8` free; `CreateProposalAddAsset` `0x1641d5f lea rdi,[r13+0x288]` → `0xbfc610` after the `HasComponent(assetToRemove, AssetGroup)` check; brush `0xdbe5fe lea rdi,[rax+0x288]` → `0xa6dc30` | PROVEN |
| **+0x2a0** | **+0x1f8** | **toAdd `vector<ConstructionEntity>`** | **0x8f0 B** | `0xdc30b7..0xdc30f3` loop `add r12,0x8f0` + `call 0xdc2a00`; `0x1641ced lea rdi,[r13+0x2a0]` → `0xdca780`; brush `0xdbc778`, `0xdbe534` | PROVEN |
| +0x2b8 | +0x210 | old2new `unordered_map<Entity,int>` | element_count **+0x2d0** | ctor `0xdc2e3f` buckets = &+0x2e8, `+0x2d8`=1.0f; AssetBrush commit slot 15 `0xdb9975 lea rdi,[rax+0x2b8]; call 0xdc01c0` (clear: frees the node chain from +0x10, memsets the buckets, zeroes +0x18 and +0x10) | PROVEN |
| +0x2f0 | +0x250 | vector, 4-byte elements | | copy ctor `0xe3e3b3 mov edi,[rsi+rcx*4]`; dtor `0xdc309a` | PROVEN |
| +0x308 | +0x268 | `std::map`/`set` (48 B), node_count **+0x330** | | ctor `0xdc2f4f..0xdc2f7d`; copy `0xe3e463`; dtor `0xdc3095 call 0xdc0460` | PROVEN |
| **+0x338** | **+0x278** | **baseHeightMod `Grid<CVec2f>`**: x0 +0x338, y0 +0x33c, w +0x340, h +0x344; data `vector<CVec2f>` **+0x348/+0x350/+0x358** | 8 B cells {height, base} | see the note below the table | PROVEN |
| **+0x360** | **+0x2a0** | **material `Grid<uint8>`**: header +0x360..+0x36c; data **+0x370/+0x378/+0x380** | 1 B, 0xff = unchanged | copy ctor `0xe3e54e..0xe3e5dc` (no element shift, `_Znwm(bytes)` at `0xe3e5ca`); dtor `0xdc3065` | PROVEN (the unchanged value comes from the Windows docs) |
| **+0x388** | **+0x2c8** | **mask `Grid<bool>`**: header +0x388..+0x394; `std::vector<bool>` **+0x398..+0x3bf** (layout below) | 64-bit words | copy ctor `0xe3e606..0xe3e7f5`; dtor `0xdc3054` | PROVEN |
| sizeof 0x3c0 | 0x2f8 | | | `GetProposal` `0xe58283 mov edi,0x3c0; call _Znwm`; commit frees with `_ZdlPvm(p, 0x3c0)` at `0xe59750` | PROVEN |

Evidence for baseHeightMod at +0x338:

- **Copy ctor:** it copies the four header dwords (`0xe3e479..0xe3e4c6`); `0xe3e4da sar rax,3` plus a
  max of `0x1fffffffffffffff`; `_Znwm` + `memmove` (`0xe3e505`, `0xe3e546`).
- **Dtor:** frees the data at `0xdc3076`.
- **CreateProposalData** `0x162f050` (assert signature
  `ProposalData CreateProposalData(const StreetToolkit&, const CostRep*, const Proposal&, const std::vector<EntityShapeList>*, const Context&)`
  at `0x163646c`; its funcsig row names the inlined `CreateProposalDataImpl`):
  - `r15 = rdi`, the returned ProposalData (`0x162f05a`; r15 is not written again before the
    epilogue). Its +0 is a Proposal: `0x162f443 call 0xdc2b90` with rdi = r15.
  - `0x16342a5 lea rdx,[r15+0x338]` → `[rbp-0xe10]` → `0x16342e9 mov rdi,[rbp-0xe10]` →
    `0x1634300 call GetBaseHeightModBBox(const Grid<CVec2f>&, CVec2f)` `0x1621b20`. That function reads
    `[rdi]`, `[rdi+4]`, `[rdi+8]`, `[rdi+0xc]` and asserts `baseHeightMod.GetSize() > 0`
    (`0x1621c1c`).
  - Just before, `0x16342bf`/`0x16342c6` test `[r15+0x340] * [r15+0x344] > 0`.

The Grid header order is `{int x0, y0, w, h}`. `GetBaseHeightModBBox` `0x1621b20` computes the box
from `x0 = [rdi]`, `y0 = [rdi+4]` and `x0 + w - 1` with `w = [rdi+8]`, `h = [rdi+0xc]`, and asserts
`w*h > 0`. That makes the order PROVEN. Cell addressing (row-major) comes from the Windows docs and is
not needed: the carrier moves whole grids.

### `std::vector<bool>` inside `Grid<bool>` (libstdc++, PROVEN)

| offset | field |
|---|---|
| +0x398 | `_M_start._M_p` (`unsigned long*`) |
| +0x3a0 | `_M_start._M_offset` (unsigned int, always 0 here) |
| +0x3a8 | `_M_finish._M_p` |
| +0x3b0 | `_M_finish._M_offset` (0..63) |
| +0x3b8 | `_M_end_of_storage` |

- **size in bits** = `(finish.p - start.p) * 8 + finish.off - start.off`. Evidence: copy ctor
  `0xe3e685 mov rax,[+0x3a8]; 0xe3e68d sub rax,[+0x398]; 0xe3e6a0 lea r13,[rdx+rax*8]`
  (rdx = `[+0x3b0]`), then `0xe3e6ac sub r13, [+0x3a0]`.
- **64-bit words**:
  - The copy ctor allocates `((bits+0x3f)>>6)*8` (`0xe3e780..0xe3e798`) and sets `_M_finish`
    through `sar rax,6` / `and r13d,0x3f` (`0xe3e7d8..0xe3e7e0`).
  - Its tail loop tests bits `0..0x3f` of each qword (`0xe3e73c cmp ecx,0x3f`, `0xe3e745 add rdx,8`).
  - The painter allocates the same way: `Paint` `0xebd84d lea r12,[r13+0x3f]; shr r12,6; shl r12,3; _Znwm`.
- The Windows `Grid<bool>` is `vector<uint32>` + `size_t bits` (at +0x2f0). **The wire format
  (TPTG v1, u32 words) must be converted** (section 7).

## 2. `Proposal::ConstructionEntity`: Linux (0x8f0 B)

| Linux | Windows | field | evidence | status |
|---|---|---|---|---|
| sizeof **0x8f0** | 0x8e0 | | `emplace_back` `0xdca7a3 lea rdx,[rax+0x8f0]`; `_M_realloc_insert` `0xdca590` magic `0xefe35b4cfaa11e6f` with `sar 4` (÷0x8f0) and `imul …,0x8f0` at `0xdca67d`; ~Proposal step 0x8f0 | PROVEN |
| +0x000 | +0x000 | `std::string` (fileName in the Windows docs) | ctor `0xdbec14 lea rax,[rdi+0x10]; 0xdbec25 mov [rdi],rax` (SSO) | PROVEN string; name INFERRED |
| **+0x020** | +0x020 | **int type, 0xb = asset group** | `0x1641b97 mov dword [rbp-0x910],0xb` (CE at `rbp-0x930`); brush `0xdbc694`, `0xdbd0be`, `0xdbe435`; the default ctor writes 0 (`0xdbec9e`) | PROVEN |
| +0x1fd | +0x20d | byte = 1 for an asset group | `0x1641ba1 mov byte [rbp-0x733],1`; brush `0xdbc69e`, `0xdbe43f` | PROVEN (meaning unknown) |
| **+0x478** | **+0x470** | **`vector<TransformedModel>`** (begin +0x478, end +0x480) | `CreateProposalAddAsset` moves its by-value param into `rbp-0x4b8` (`0x1641c5a..0x1641ca1`); brush `0xdbc762 lea rdi,[rbx+0x478]; call 0xdc1df0` | PROVEN |
| **+0x578** | **+0x550** | **`vector<Record48>`** | `0x1641301 lea rdi,[rbx+0x578]` unchanged until `0x1641c02 call 0xdc1000` (script check: no write to rdi/rsi and no call between); brush `0xdbe2a9 add rax,0x578` → `[rbp-0x1368]` → `0xdbe421 mov rdi,[rbp-0x1368]; 0xdbe4a0 call 0xdc13a0` | PROVEN |
| +0x738 | +0x728 | transf `Mat4f` (16 floats) | `CreateProposalAddAsset` copies its matrix param (rcx) into `rbp-0x1f8..rbp-0x1b9` (`0x1641b27..0x1641b90`); the default ctor writes identity (`+0x738`=1.0f, `+0x74c`=1.0f through rdx=`0x3f80000000000000`, `+0x760`=1.0f, `+0x774`=1.0f) | PROVEN |
| +0x8c0 | +0x8b0 | `std::string` (name in the Windows docs) | ~CE `0xdc2a1b` frees `[+0x8c0]` unless it is `+0x8d0` | PROVEN string; name INFERRED |

The capture and the replay use only +0x020, +0x478 and the size. The replay builds every other
field through the game's own constructor.

**The asset builds all produce one record** (PROVEN by an offset-by-offset diff of the constant
stores, with every register traced to its constant). Compared:

- the inline init in `CreateProposalAddAsset` (211 stores);
- the three inline/out-of-line inits in the AssetBrush update `0xdba100`, which inlines
  `{anonymous}::MakeBuildAssetsProposal` (assert signature at `0xdbe694`, `!modelsToKeep.empty()`
  at `0xdbe6a7`);
- the default ctor `0xdbec10` (213 stores).

They differ in exactly three places, all at +0x020/+0x1fd/+0x738:

- +0x020 is 0 in the ctor and 0xb in the builders.
- +0x1fc/+0x1fd is 0 in the ctor and has byte 1 at +0x1fd in the builders.
- transf is identity in the ctor and the brush, and the param in `CreateProposalAddAsset`.

After construction, every builder stores 0xb, stores 1 and emplaces one Record48
`{0, {}, {}, 0.75f, 2.5f, 0}`, then fills the TM vector and emplaces the CE into `toAdd`.

One observation: in the re-add path the brush writes `[rbp-0x48]` (CE+0x8e8) = `toRemove.size()-1`
at `0xdbe563`. That happens **after** the CE was already moved into the proposal (`0xdbe53b`), so
the stored record keeps the ctor's -1. The brush's commit clears old2new (`0xdb997c`) before
committing, so the `old2new[old] = index` written at `0xdbe581` never reaches the command either.

### `TransformedModel` (0x80 B, PROVEN layout)

| offset | field | evidence |
|---|---|---|
| +0x00 | `std::string` `{char* p; size_t len; buf/cap}` (model name, INFERRED) | `vector<TM>::operator=` `0xdc1e76 call _M_assign` (exported `0x9d8be0`) on `[r15]` ← `[rbx]`; destroy loops compare `[e]` with `e+0x10`. The brush assigns it from a 32-byte string table indexed by the brush entry's model id: `0xdbd756 movsxd rsi,[rbx]; 0xdbd772 shl rsi,5; 0xdbd7b5 add rsi,[rax+0x48]; 0xdbd7c4 _M_assign`. |
| +0x20 | `std::string` | `0xdc1e87 _M_assign` on `+0x20`; destroy loops compare `[e+0x20]` with `e+0x30` |
| +0x40 | `Mat4f`, translation at +0x70/+0x74/+0x78 | four `movdqu` of `[src+0x40..+0x7f]` (`0xdc1e8c..0xdc1eb3`, `0xdc1d49..0xdc1d79`) |
| size 0x80 | | `0xdc1e31 sar rcx,7`; `sub r12,-0x80` loops |

For both strings, `len` is at +0x08. In the CE move ctor, `0xdc318c mov rdx,[r12+8]` and
`0xdc319e mov qword [r12+8],0` are the libstdc++ `_M_string_length`.

### `Record48` (0x48 B, PROVEN)

`vector<Record48>::emplace_back(Record48&&)` `0xdc13a0` and `_M_realloc_insert` `0xdc1000`
(`sar 3` + `imul 0x8e38e38e38e38e39` = ÷0x48) move these fields:

| offset | field |
|---|---|
| +0x00 | int |
| +0x08 | vector (its elements are 0x18-byte vectors: `CreateProposalAddAsset` frees `[e]` every 0x18 at `0x1641c30`) |
| +0x20 | vector |
| +0x38 | float 0.75 |
| +0x3c | float 2.5 |
| +0x40 | uint8 |

The builders store `{int 0; zeros; 0x402000003f400000 at +0x38; byte 0 at +0x40}` (`0x1641b52`
`movabs rax,0x402000003f400000` → `[rbp-0x948]`; brush `0xdbc62a` → `[rbp-0x10f8]`).

## 3. Game functions the asset replay calls (all PROVEN semantics)

In every row rdi is the first argument, rsi the second, and so on (SysV). Verify the listed bytes at
run time, and refuse (leave the carrier alone and log) on any mismatch.

| role | Windows | Linux RVA | SysV signature | semantics evidence | FDE size | first 16 bytes | sha256[:16] of whole FDE |
|---|---|---|---|---|---|---|---|
| CE default ctor | `RVA_CE_CTOR 0x3ceae0` | **`0xdbec10`** | `void(CE* rdi)` | only caller `0xdbe41c` (the brush's MakeBuildAssetsProposal re-add path); matches the inline init (section 2); ends `0xdbf4c8 ret` | 2233 | `f30f1efa488d4710c647100031d2488d` | `89ed4a06c9a3f0c1` |
| CE dtor | `RVA_CE_DTOR 0x3d0430` | **`0xdc2a00`** | `void(CE* rdi)` | called on the local CE after each emplace (`0x1641cff`, `0xdbc787`, `0xdbd1c5`, `0xdbe58b`) and per element by ~Proposal (`0xdc30da`) | 388 | `f30f1efa554889e541554154534889fb` | `c40fd4b76ee78b81` |
| vector<CE> emplace_back(CE&&) | `RVA_VEC_CE_GROW 0x3c8680` + `RVA_CE_COPY_AT 0x3ce460` | **`0xdca780`** | `CE&(vector<CE>* rdi, CE* rsi)`; moves from rsi | end≠cap: `0xdc3140`(end, rsi) = CE **move** ctor (steals string buffers: `0xdc3181..0xdc31a7`), end += 0x8f0; else `0xdca580` = `_M_realloc_insert(this, pos=end, CE&&)` (move-constructs, destroys old with `0xdc2a00`, frees with `_ZdlPv`). The caller must still destroy its moved-from CE with `0xdc2a00`, as the game does. | 87 | `f30f1efa554889e5534889fb4883ec08` | `60bde1ee79d260a9` |
| vector<Record48> emplace_back(Record48&&) | `RVA_VEC_48_GROW 0x3c8b40` | **`0xdc13a0`** | `Record48&(vector* rdi, Record48* rsi)`; moves | full disassembly; slow path `0xdc1000` | 246 | `f30f1efa488b4708483b47100f84be00` | `7e76d4002ffd4ee3` |
| vector<TM> operator=(const&) | `RVA_VEC_TM_ASSIGN 0x3c7780` (assign(first,last) on Windows) | **`0xdc1df0`** | `vector<TM>*(vector<TM>* rdi, const vector<TM>* rsi)`; **source is a vector triple, not (first,last)** | `0xdc1e0c cmp rsi,rdi`; reads `[rsi]`/`[rsi+8]`; realloc path `0xdc1f38 _Znwm` + `0xdc1ce0` (uninitialized copy: `_M_construct` `0xdb9590` from `[src]`,`[src]+[src+8]` and from `[src+0x20]`,`+[src+0x28]`, plus 0x40 matrix bytes); the source is only read | 902 | `f30f1efa554889e54157415641554989` | `1cacb79c574f2aea` |
| vector<Entity> push_back(const&) | `RVA_VEC_INT_GROW 0x0e8060` | **`0xbfc610`** | `void(vector<int>* rdi, const int* rsi)` | whole function: `mov rax,[rdi+8]; cmp rax,[rdi+0x10]; je →{rdx=rsi; rsi=rax; jmp 0xa6dc30}; mov edx,[rsi]; add rax,4; mov [rax-4],edx; mov [rdi+8],rax; ret`. `0xa6dc30` is the same slow path the brush uses for `toRemove`. | 43 | `f30f1efa488b4708483b471074128b16` | `b6fafe32a17d9bf5` |
| (alternative) CreateProposalAddAsset | `0xa13fc0` | `0x1641270` | `Proposal*(Proposal* rdi /*ret, constructed by 0xdc2b90*/, const StreetToolkit* rsi, vector<TM>* rdx /*by-value param, hidden reference, moved from*/, const CMat4f* rcx, int r8d /*entity to remove*/)` | named by its assert signature (`0x1641d78`); with r8d < 0, `0x16412b5 test ecx,ecx; jns 0x1641d30` skips the only use of rsi (`0x1641d30 mov rdi,[rbx+0x98]`), so `stk` is never read; an empty TM vector returns early (`0x16412c6 je 0x1641d04`) | 2891 | `f30f1efa554889e5415741564989ce41` | `5c1ec9a9b3d74ef2` |
| (alternative) ~Proposal | | `0xdc3040` | `void(Proposal* rdi)` | section 1 | 243 | `f30f1efa554889e541554154534889fb` | `7b696266d29b709e` |

Alignment: none of `0xdbec10`, `0xdc3140`, `0xdc2a00`, `0xdc2700`, `0xdc13a0`, `0xdc1000`,
`0xdca780`, `0xdca580`, `0xdc1df0`, `0xdc1ce0`, `0x1641270`, `0xdc3040`, `0xdc2b90` or `0xbfc610`
uses an aligned SSE load or store on memory other than its own stack frame (script check). 16-byte
alignment of caller-provided CE, Record48 or Proposal storage is therefore not required. Use
`alignas(16)` anyway.

**Recommended replay recipe (mirrors the Windows code and `MakeBuildAssetsProposal`'s re-add
path).** Per group:

1. `alignas(16) uint8_t ce[0x8f0]`; call `0xdbec10(ce)`.
2. Store `int 0xb` at `ce+0x20` and `byte 1` at `ce+0x1fd`.
3. Build a zero-filled `alignas(16) uint8_t rec[0x48]` with `0.75f` at +0x38 and `2.5f` at +0x3c,
   then call `0xdc13a0(ce+0x578, rec)`. The moved-from record holds only null vectors, so nothing is
   left to free: `dc13a0` swaps the destination's zero triple into it (`0xdc13e7..0xdc13ff`).
4. Build a read-only source `vector<TM>` triple `{src, src+n*0x80, src+n*0x80}` over our own array.
   Each TM holds two libstdc++ strings `{p → our NUL-terminated buffer, len, 0…}` and the 64 matrix
   bytes. Call `0xdc1df0(ce+0x478, &triple)`. The game allocates every copy; we free our buffers
   afterwards.
5. Call `0xdca780(proposal+0x2a0, ce)`.
6. Call `0xdc2a00(ce)`.

Removals: call `0xbfc610(proposal+0x288, &id)` for each id.

The alternative (`0x1641270` into a temporary 0x3c0-byte Proposal with stk=nullptr, entity -1 and an
identity matrix, then `0xdca780(carrier+0x2a0, tmp.toAdd.begin)` and `0xdc3040(tmp)`) is also backed
by the evidence above. It is not recommended: it adds a by-hidden-reference `vector<TM>` whose
elements must already be game-allocated, and it runs a whole Proposal ctor/dtor per group.

## 4. Allocation: the game's `operator new` (terrain replay)

The Windows vector copy constructors `RVA_VECCOPY_8/1/4` (`0x0cc990`, `0x1ded10`, `0x125480`) have
**no out-of-line Linux counterpart**. The Proposal copy constructor inlines each grid copy as
`_Znwm(bytes)` + `memmove` + set the triple (heights `0xe3e505`/`0xe3e546`, material `0xe3e5ca`,
mask `0xe3e798` + bit loop). The terrain replay does the same: call the game's `operator new`, then
`memcpy`, then write `{begin, end, cap}`. ~Proposal frees each grid with `_ZdlPv`.

| import | PLT stub | stub bytes | GOT slot (`.rela.plt` JUMP_SLOT) | symbol |
|---|---|---|---|---|
| operator new | `0x6dbce0` | `ff 25 0a b0 36 05` (jmp `[rip+0x536b00a]`) | `0x5a46cf0` | `_Znwm@GLIBCXX_3.4` |
| operator delete | `0x6dbcd0` | `ff 25 12 b0 36 05` | `0x5a46ce8` | `_ZdlPv@GLIBCXX_3.4` |
| sized delete | `0x6dbee0` | `ff 25 0a af 36 05` | `0x5a46df0` | `_ZdlPvm@CXXABI_1.3.9` |

The game links `libstdc++.so.6` dynamically (`DT_NEEDED`) with `BIND_NOW`, so the GOT slot holds the
resolved address from load time. Two ways to call it:

- **Preferred:** call through `*(image base + 0x5a46cf0)`, after checking that the stub at
  `base+0x6dbce0` is `ff 25` with disp32 `0x0536b00a`.
- Call the stub at `base+0x6dbce0` itself.

Do **not** use the library's own statically linked `operator new`. It is malloc-backed too, but only
the game's import is guaranteed to pair with the game's `_ZdlPv`.

`operator new` throws `bad_alloc` on failure. Keep the terrain grids small, as the Windows code does
(`TERRAIN_MAX_BYTES` 64 MiB), and do not call it from inside a frame that cannot unwind.
**INFERRED:** failure is practically impossible at these sizes; slice-core decides whether to guard
it.

### 4.1 The Lua carrier's grids before the inject

**PROVEN (binary).** The Proposal copy constructor `0xe3e060` allocates by element count, so an empty
source vector (size 0, whatever its capacity) gives a destination of `begin = end = cap = 0`:

- heights: `0xe3e4e9 je 0xe3e830` → `0xe3e830 xor ecx,ecx`
- material: `0xe3e5c1 je 0xe3e778` → `0xe3e778 xor ecx,ecx`
- mask `vector<bool>`: `0xe3e6af jne 0xe3e780` only when bits ≠ 0, so all five words stay zero
- the same holds for toRemove (`0xe3e152 je 0xe3e820`), toAdd (`0xe3e224`) and +0x2f0

The carrier (rdx at `0x1971333`) is that copy of the `scripting::Convert` result
(`0x1971288 call 0x2e237b0`, `0x1971310 call 0xe3e060`). Convert `0x2e237b0` sets
`r15 = rdi` (the result) at `0x2e237ba`. It zero-initializes the result's grid members inline:
`0x2e23bcf..0x2e23c89` store 0 at +0x338..+0x3b8, with rdi unchanged since entry and no call or
branch before `0x2e23c93`. No other instruction in the function has a memory displacement in
+0x338..+0x3bf, and the r15-relative addresses it hands to callees are +0x18, +0x30, +0x48, +0xd0,
+0xe8 and +0x2b8.

**INFERRED.** For the mod's carriers, `api.type.SimpleProposal.new()` (`terrain.lua:72,82`,
`assets.lua:131,139`), the grids stay empty. The only code that could still fill them is a callee
handed the whole result:

- `0x2f1f680`: rdx = r15 at `0x2e2436c`; it runs only when a scripting-proposal vector at +0x60 is
  non-empty (`0x2e2435b`, `0x2e2435f je 0x2e24b70`)
- `MakeProposalAdd` `0x1645330`: its `Proposal&` is pushed at `0x2e24dce`
- the call prepared at `0x2e23f64`: rdx = r15
- `MakeProposalRemove` `0x1640a90`: its `Proposal&` argument was not traced

That none of them runs or writes a grid for an empty SimpleProposal is not shown.

**So the code must check it:** `InjectTerrainFromFile` writes a grid only if that grid's triple is all
zero (+0x348/+0x350/+0x358, +0x370/+0x378/+0x380, and all five `vector<bool>` words
+0x398..+0x3b8). On any non-zero word it leaves the carrier as built and logs. This is the
`TerrainCarrierEmpty` test of section 6.

## 5. `UI::ProposalAction`: commit, callback and the wait flag

| item | Windows | Linux | evidence | status |
|---|---|---|---|---|
| vtables (address point) | AssetBrush / TerrainModifier / TerrainPainter | `0x5a09980` / `0x5a0bcc0` / `0x5a0bda0` | typeinfo names `N2UI10AssetBrushE` `0x40133d0`, `N2UI15TerrainModifierE` `0x4085a50`, `N2UI14TerrainPainterE` `0x4087d50`; typeinfo objects `0x5a09948`, `0x5a0bb88`, `0x5a0bd48`, each with `__base_type` = ProposalAction's typeinfo `0x5a0a9c8` | PROVEN |
| update (Windows slot 5 `Step`, Linux slot 6) | `0x3d4930` / `0x46ac40` / `0x46dfd0` | `0xdba100` / `0xeb6ba0` (→ `0xeb4300` at `0xeb6bbd`) / `0xebd250` | vtable slot +0x30 | PROVEN (slot); name INFERRED |
| commit request (Windows slot 14, Linux slot 15) | `0x3d1110` / `0x4684a0` / `0x46d7d0` | `0xdb9960` / `0xeaca40` / `0xebc510`; each ends in `jmp 0xe59490` | vtable slot +0x78 | PROVEN |
| `ProposalAction::commit` | `0x4310d0` | **`0xe59490`** `void(this rdi, bool sil, bool dl)` | makes the command and calls Add, as mapped in section 0 | PROVEN |
| tool's Proposal | | `this+0x78` (`unique_ptr`) | `GetProposal` `0xe58250`: create with `_Znwm(0x3c0)` + `0xdc2b90`; commit frees it `0xe59748..0xe59758` | PROVEN |
| tool's ProposalData | | `this+0x80` (0x900 B) | `0xe58b40`: `_Znwm(0x900)` + `0xde1590` | PROVEN |
| **wait flag** | `tool+0xf0` | **`tool+0xd0` (byte)** | set: commit `0xe59503 c6 87 d0 00 00 00 01` (before the command is made; commit does not read it). Cleared: callback invoker `0xe593b4 c6 80 d0 00 00 00 00` (tool = `[functor]`). Which updates it gates: **5.1** | PROVEN |
| completion `std::function` (rcx of Add) | MSVC `{vftable 0x2fc8dd0, tool, bool}`, impl at r9+0x38 | libstdc++ 32 B at `rbp-0x60`: **+0x00 tool**, **+0x08 bool** (commit arg dl), +0x10 `_M_manager` = **`0xe57670`**, +0x18 `_M_invoker` = **`0xe593a0`** | `0xe59543 mov [rbp-0x60],rdi`; `0xe594ef mov [rbp-0x58],dl`; `0xe5951d lea rcx,0xe57670` → `[rbp-0x50]`; `0xe594e8 lea rcx,0xe593a0` → `[rbp-0x48]`; `0xe59629 lea rax,[rbp-0x60]` → rcx. Manager `0xe57670`: op 0 returns typeinfo `0x5a0aa10`, op 1 `*dest = src` (stored locally), op 2 copies 16 bytes, op 3 nothing. | PROVEN |
| callback invoker | `_Do_call 0x431c60` | `0xe593a0` `void(const _Any_data* rdi /* == the std::function */, const Command* rsi)` | clears +0xd0. Then calls `0x13981d0` on `[tool+0x110]` when the bool is set, else on `[tool+0xb8]`. Checks `[rsi+0x30]`. Fires and destroys the one-shot `std::function` at `tool+0xd8` (manager `+0xe8`, invoker `+0xf0`), zeroing +0xe8/+0xf0. This matches the Windows description (`tool+0xf8`, `command+0x30`). | PROVEN |
| Context | 0x70 B, player +0x14 | **0x68 B**, player +0x14 | see below | PROVEN |

**Context is 0x68 bytes.** The Lua lambda's parameter is `std::optional<construction_builder_util::Context>`
(signature at `0x1960afd`). libstdc++ keeps an optional's `_M_engaged` byte directly after its
payload, and `0x1970a10` holds two such optionals, both with the engaged byte at payload+0x68:

- **Payload `rbp-0x25d0`, engaged byte `rbp-0x2568`.**
  - The byte is zeroed at `0x1970b4b c6 85 98 da ff ff 00`.
  - It is set at `0x19719cc c6 85 98 da ff ff 01`, after the payload stores at +0x51/+0x58/+0x60
    (`0x1971998..0x19719ad`).
  - It is tested at `0x197158a` before `~unordered_map` `0xdd87e0` runs on payload+0x18
    (`0x1971b90 lea rdi,[rbp-0x25b8]`).
- **Payload `rbp-0x2560`, engaged byte `rbp-0x24f8`.**
  - The byte is zeroed at `0x1970b6a`.
  - The payload is filled from the first optional (`0x19719e0..0x1971a41`), then the byte is set at
    `0x1971a4d c6 85 08 db ff ff 01`.
  - `0x197128d` tests it and on `jne 0x1971ba8` copies payload +0x00 … +0x60 field by field into the
    Context at `rbp-0x2640`.

Consistent with 0x68:

- the default Context there is 13 zeroed qwords (`0x19712a3 mov ecx,0xd`, `0x19712be rep stosq`)
  with the unordered_map at +0x18..+0x4f (buckets = &+0x48, count 1, max load 1.0f at +0x38) and
  player -1 at +0x14
- the highest store at every init site is the qword at +0x60
- in commit the Context at `rbp-0x490` sits 0x70 below the Proposal copy at `rbp-0x420`, i.e. 0x68
  plus 8 bytes of alignment padding

### 5.1 Where the game reads the wait flag (PROVEN)

A scan of every byte-size operand with displacement 0xd0 covers:

- commit `0xe59490`
- the commit requests `0xdb9960` / `0xeaca40` / `0xebc510` and the painter's slot 3 `0xebcad0`
- the auto-commit helper `0xe58bc0`
- the preview builder `0xe58e70`
- `0xeb6ba0` and `0xeb4300`
- the painter update `0xebd250` and every direct callee
- the AssetBrush update `0xdba100` and every direct callee

It finds exactly three reads: `0xeb4425`, `0xdbac70` and `0xdbd2cd`.

- **TerrainModifier: always held.**
  - In `0xeb4300`, `r15 = rdi` at `0xeb430a`, and nothing writes r15 before the test.
  - The test is `0xeb4425 41 80 bf d0 00 00 00 00 cmp byte [r15+0xd0],0` with `0xeb442d je 0xeb4458`.
    Right after `Brush::UpdateBrushTTB`, a set flag returns 0 (`0xeb442f xor ebx,ebx`), and
    `0xeb6ba0` then skips at `0xeb6bd5`.
  - With the edge `0xeb442d→0xeb4458` removed, the CFG of `0xeb4300` can no longer reach the
    preview `0xeb5afb call 0xe58e70` or the commit request `0xeb6890 call [rax+0x78]`.
- **AssetBrush: held only while its model list is empty.**
  - The list is a `vector<std::string>` at tool+0x120..+0x128. Evidence:
    - ~AssetBrush `0xdb98f5 lea rdi,[rbx+0x120]`; `0xdb98fc call 0x9b7f60`
      (`std::vector<std::string>::~vector`)
    - `0xdba56d sar rax,5` for a random model index
    - the setter `0xdb99d0` empties it when given an empty string (`0xdb99f9`, `0xdb9a32`) and
      otherwise stores that string (`0xdc22e0`)
  - The update copies begin/end into `[rbp-0x1430]`/`[rbp-0x1438]` (`0xdba176 48 8b 8b 20 01 00 00`,
    `0xdba184 48 8b 83 28 01 00 00`). Nothing else in the function writes those two slots.
  - **Empty list:** `0xdba19c je` skips the auto-commit helper. `0xdba2ac`/`0xdba2b3 je 0xdbac70`
    tests the flag: set → `0xdbac7e jmp 0xdba1f1` (return); clear → `0xdbac78 je 0xdba2b9`.
  - **Non-empty list:** the flag is never tested.
    - `0xdba1bd call 0xe58bc0` runs. That helper calls commit request slot 15 at
      `0xe58d6a call [rax+0x78]` (esi = 0, edx = 1), and slot 15 `0xdb9960` jumps to commit
      `0xe59490`; neither reads +0xd0.
    - The brush continues at `0xdba2b9`.
    - The flag is used only as `flag ^ 1`, pushed as an argument to `0xe58e70`
      (`0xdbd2cd..0xdbd2e1`).
  - **The second auto-commit** (`0xdbe669 call 0xe58bc0`, when toRemove > 100) needs an empty list
    (`0xdbd2ef cmp`, `0xdbd2f6 jne 0xdbd310`). In the CFG every path to it, and to `0xdbd2e1`, passes
    `0xdba2b9`, whose predecessors are `0xdba2b3` (list non-empty) and `0xdbac78` (flag clear). So it
    is held along with the rest.
- **TerrainPainter: not held.** Its update `0xebd250` calls the auto-commit helper (`0xebd2ec`) and
  the preview (`0xebddb6`) without any read of +0xd0.

**What `HoldTerrainTool` (writing 1) achieves on Linux:**

- It holds a TerrainModifier stroke: no brush, no preview, no commit request.
- It holds an AssetBrush whose model list is empty (INFERRED: the erase stroke).
- It does **not** hold a TerrainPainter stroke or an AssetBrush with a model selected. Those tools
  keep running and auto-committing while the flag is set.

The Windows doc states the gate only for the terraform update too (`docs/re/PROPOSALS.md` 321-323).

**Hold on Linux.** In the Add hook, with rcx = the `std::function*`:

1. Require `[rcx+0x10] == base+0xe57670` and `[rcx+0x18] == base+0xe593a0`.
2. `tool = [rcx]`.
3. After firing the callback (it clears the flag), set `byte [tool+0xd0] = 1`. Release writes 0.

The identity check replaces the Windows "impl+8". The write is the game's own flag at a PROVEN
offset, so it is safe for every ProposalAction tool; its effect is only what 5.1 shows.

## 6. Linux offsets for each ported routine

| routine | reads/writes (Linux) | Windows |
|---|---|---|
| `LogTerrainProposal` (log counts) | nodes `+0x00`, segs `+0x18`, rmNodes `+0x30`, rmSegs `+0x48`, toRemove `+0x288`, toAdd `+0x2a0`, v250 `+0x2f0` (4-byte ints); set count `+0x250`; old2new count `+0x2d0`; map count `+0x330`; grids `+0x338` / `+0x360` / `+0x388`, data `+0x348` / `+0x370` / `+0x398`, bits from the vector<bool> formula | `+0x198`, `+0x220`, `+0x270`, `+0x2f0` … |
| readable span | `0x3c0` | `0x2f8` |
| `gridsOk` | `hbytes == w*h*8`, `mbytes == w*h`, `kw*kh == bits`, and mask words64 bytes == `((bits+63)/64)*8` | `…*4` with 32-bit words |
| `onlyTerrain` / `StashAssetsFromProposal` "nothing else" | the four street vectors plus toRemove/toAdd; grid data `+0x348`, `+0x370`, `+0x398` | `+0x288`, `+0x2b0`, `+0x2d8` |
| asset stroke decode | toAdd `+0x2a0`, bytes % **0x8f0**; toRemove `+0x288`, bytes % 4; CE type `ce+0x20 == 0xb`; TM vector `ce+0x478` / `ce+0x480`, bytes % 0x80; strings `{p @+0, len @+8}` at TM+0x00 and TM+0x20. A string is valid if `len == strlen` and either (`p == s+0x10` and `len ≤ 15`) or p is a readable heap span; matrix at TM+0x40 (translation +0x70..+0x78) | 0x8e0, +0x470, MSVC SSO (len +0x10, cap +0x18) |
| `TerrainCarrierEmpty` (run-time precondition of the terrain inject, 4.1) | `begin == end` for `+0x00, +0x18, +0x30, +0x48, +0xe8, +0x270, +0x288, +0x2a0, +0x2f0`; and `begin == end == cap == 0` for `+0x348`, `+0x370`, and the vector<bool> `+0x398`, `+0x3a0`, `+0x3a8`, `+0x3b0`, `+0x3b8` (nothing to leak). On failure: log, leave the carrier as built | `0xf8, 0x1c8, 0x1e0, 0x1f8, 0x250`; grid begins `+0x288/+0x2b0/+0x2d8` |
| `ProposalIsEmpty` (marker) | `+0x00, +0x18, +0x30, +0x48, +0x288` empty and toAdd `+0x2a0` end ≤ begin | `0x1e0`, `0x1f8` |
| `InjectTerrainFromFile` | headers: 16 B each at `+0x338`, `+0x360`, `+0x388`. Heights: `_Znwm(n0)` + memcpy → `+0x348 = p, +0x350 = p+n0, +0x358 = p+n0`. Material: → `+0x370/+0x378/+0x380`. Mask: `W = (bits+63)/64`, `p = _Znwm(W*8)`, 64-bit words from the wire's u32 words, `+0x398 = p`, `+0x3a0 = 0`, `+0x3a8 = p + (bits/64)*8`, `+0x3b0 = bits % 64`, `+0x3b8 = p + W*8` (bits == 0: leave all five zero) | `RVA_VECCOPY_*` into `+0x288/+0x2b0/+0x2d8`, bits at `+0x2f0` |
| `InjectAssetsFromFile` | section 3 | section 3 |
| `HoldTerrainTool` / `ReleaseTerrainTool` | section 5 (effect: 5.1) | `impl+8`, `tool+0xf0` |

## 7. Wire formats (no RE; recorded so every platform ships the same bytes)

- **TPTG v1** (`"TPTG"`, u32 1, a 0x80-byte tail, a 0x70-byte context, three `u64 n + data` blobs)
  is the Windows format.
  - **The tail:** a Linux capture has to synthesize the Windows tail: header bytes at tail+0x00 /
    +0x28 / +0x50, `u64 bits` at tail+0x78, the rest zero. (Windows put meaningless pointers there,
    and the reader ignores them.)
  - **The mask:** the capture must emit it as **u32 words**: `ceil(bits/32)` words,
    `w32[2i] = low32(w64[i])`, `w32[2i+1] = high32(w64[i])`. Clear bits ≥ `bits` in the last word, so
    a Linux capture matches a Windows one byte for byte. The Linux inject reverses this:
    `w64[i] = w32[2i] | (u64)w32[2i+1] << 32`.
  - **The context blob** is debug-only: write the Linux Context's 0x68 bytes (PROVEN size, section 5)
    plus 8 zero bytes, or 0x70 zeros.
  - **Heights and material:** heights (`CVec2f` 8-byte cells) and material (u8) are the same bytes on
    both platforms.
- **TPAS v1** (asset stroke) and the `rm <ids>` text are platform-neutral: strings are length +
  bytes, and matrices are 64 raw float bytes.

## 8. Run-time verification table

These bytes encode every offset and callee this area relies on. Check them once at init (against
image base + RVA). On any mismatch, leave both carriers and both captures OFF and log.

| RVA | bytes | instruction | proves |
|---|---|---|---|
| `0x6dbcd0` | `ff 25 12 b0 36 05` | `jmp [rip+0x536b012]` (GOT `0x5a46ce8`) | PLT `_ZdlPv` |
| `0x6dbce0` | `ff 25 0a b0 36 05` | `jmp [rip+0x536b00a]` (GOT `0x5a46cf0`) | PLT `_Znwm` |
| `0xe58283` | `bf c0 03 00 00` | `mov edi,0x3c0` | sizeof(Proposal) |
| `0xe59503` | `c6 87 d0 00 00 00 01` | `mov byte [rdi+0xd0],1` | wait flag set |
| `0xe593b4` | `c6 80 d0 00 00 00 00` | `mov byte [rax+0xd0],0` | wait flag clear |
| `0xe594e8` | `48 8d 0d b1 fe ff ff` | `lea rcx,[rip-0x14f]` (= `0xe593a0`) | callback invoker |
| `0xe5951d` | `48 8d 0d 4c e1 ff ff` | `lea rcx,[rip-0x1eb4]` (= `0xe57670`) | callback manager |
| `0xe5961d` | `e8 0e 53 79 00` | `call 0x15ee930` | ProposalAction → BuildProposal (return `0xe59622`) |
| `0xe59652` | `e8 e9 11 78 00` | `call 0x15da840` | ProposalAction → Add |
| `0x197132e` | `e8 fd d5 c7 ff` | `call 0x15ee930` | Lua carrier → BuildProposal (return `0x1971333`) |
| `0x15ef2ab` | `c6 45 b8 0f` | `mov byte [rbp-0x48],0xf` | the factory builds `CmdData::BuildProposal` |
| `0x1963cbe` | `48 8d 35 af c2 5d 02` | `lea rsi,'buildProposal'` | Lua key |
| `0x19606f0` | `48 8d 35 29 22 01 00` | `lea rsi,0x1972920` | buildProposal closure |
| `0x19729b4` | `e9 57 e0 ff ff` | `jmp 0x1970a10` | closure → Lua carrier body |
| `0xdc3054` | `48 8b bf 98 03 00 00` | `mov rdi,[rdi+0x398]` | mask vector<bool> `_M_start._M_p` |
| `0xdc3065` | `48 8b bb 70 03 00 00` | `mov rdi,[rbx+0x370]` | material data |
| `0xdc3076` | `48 8b bb 48 03 00 00` | `mov rdi,[rbx+0x348]` | height data |
| `0xdc30b7` | `4c 8b ab a8 02 00 00 4c 8b a3 a0 02 00 00` | toAdd end/begin | toAdd `+0x2a0` |
| `0xdc30d3` | `49 81 c4 f0 08 00 00` | `add r12,0x8f0` | sizeof(CE) |
| `0xdc30f8` | `48 8b bb 88 02 00 00` | `mov rdi,[rbx+0x288]` | toRemove |
| `0xdca7a3` | `48 8d 90 f0 08 00 00` | `lea rdx,[rax+0x8f0]` | emplace_back element size |
| `0x1641b97` | `c7 85 f0 f6 ff ff 0b 00 00 00` | CE+0x20 = 0xb | asset group type |
| `0x1641ba1` | `c6 85 cd f8 ff ff 01` | CE+0x1fd = 1 | asset group byte |
| `0x1641ced` | `49 8d bd a0 02 00 00` | `lea rdi,[r13+0x2a0]` | toAdd |
| `0xdbc762` | `48 8d bb 78 04 00 00` | `lea rdi,[rbx+0x478]` | CE TM vector |
| `0xdbe2a9` | `48 05 78 05 00 00` | `add rax,0x578` | CE Record48 vector |
| `0xdc1e31` | `48 c1 f9 07` | `sar rcx,7` | sizeof(TM) |
| `0x16342a5` | `49 8d 97 38 03 00 00` | `lea rdx,[r15+0x338]` | baseHeightMod |
| `0xe3e6a0` | `4c 8d 2c c2` | `lea r13,[rdx+rax*8]` | vector<bool> bit count |
| `0xe3e73c` | `83 f9 3f` | `cmp ecx,0x3f` | 64-bit mask words |
| `0xeb4425` | `41 80 bf d0 00 00 00 00` | `cmp byte [r15+0xd0],0` | TerrainModifier honours the hold (log only) |
| `0xdbac70` | `41 80 bf d0 00 00 00 00` | `cmp byte [r15+0xd0],0` | AssetBrush (empty model list) honours the hold (log only) |
| `0xdba176` | `48 8b 8b 20 01 00 00` | `mov rcx,[rbx+0x120]` | AssetBrush model list begin (log only) |
| functions of section 3 | first 16 bytes / sha256 in the table | | callee identity |

## 9. Integration notes (outside this area's files)

- **slice-proposal / slice-core (BuildProposal hook `0x15ee930`):**
  - For a return address of `0xe59622`, call this area's capture with `(rdx, rcx)`. It arms the
    cancel as the Windows code does; for a stashed asset stroke or height edit, `g_pendingCmd = rdi`
    (the Command slot; Windows used rcx).
  - For a return address of `0x1971333`, call the injectors with `rdx` before `MergeTemplateStreet`
    runs.
  - The `CALLER_PROPOSALACTION` and Lua script-caller constants must be the Linux values above.
- **slice-core (Add hook `0x15da840`):**
  - The completion callback is **rcx**, a libstdc++ `std::function`. For the terrain/asset tools,
    identify it by manager `0xe57670` and invoker `0xe593a0`.
  - Fire it as `invoker(rcx, rdx /* const Command& */)`, then call this area's
    `HoldTerrainTool(tool = [rcx])`.
  - The Command pointer to match against `g_pendingCmd` is `rdx`.
- **Do not port the carrier-added pointer match (`g_terrainCarrierCmd`, `slice_hook.cpp`
  3366-3369 / 3697-3699) as an identity test (PROVEN).**
  - Windows matched Add's Command against the factory's return slot. At the Lua site that slot is
    `rbp-0x2680` in the maker's own frame (`0x197119b`, factory rdi at `0x197132b`).
  - Right after the factory returns, the maker copies the CmdData out:
    `0x1971344 call 0x197fab0(rdi = rbp-0xd90, rsi = [rbp-0x2680])`. `0x197fab0` is the CmdData
    variant copy constructor: `0x197fab4 c6 87 48 0d 00 00 ff` marks the destination valueless, then
    it calls `[0x59c3a60 + index*8]` (`_Copy_ctor_base<…CmdData…>(const&)::_S_vtable`) and copies the
    index.
  - The maker then destroys that Command (`0x197134c call 0x15d8f30`), including its 0xd50-byte
    CmdData (`0x15d8f9d`, `0x15d8fa4`).
  - So the later `sendCommand` Add never receives the factory's object. An address match could only
    be memory reuse, a false positive.
  - The hold release rides the empty marker proposal (`ProposalIsEmpty` at return `0x1971333`) or
    the 4000 ms timeout (`TERRAIN_HOLD_MAX_MS`, `slice_hook.cpp:265`). Log the carrier inject at
    the factory instead.
- **Hold scope (PROVEN, 5.1):** the hold stops TerrainModifier and an AssetBrush with an empty model
  list. A TerrainPainter stroke or an AssetBrush paint stroke keeps committing while held. The code
  must not claim in its log that those tools wait; say "held (effective for terraform and asset
  erase)".
- **Shared helpers this area expects from slice-core:** a readable-memory check, the instance letter
  (`ReadInstance`), `SessionLive`, `DumpPropOn`, `WriteArmed`, the data dir, the log, the image base and
  the build-ok flag.
- The inject file names and base64/TPTG/TPAS formats stay as they are (`terrain.lua`, `assets.lua`
  unchanged).

## 10. Open questions

- **Carrier grids before the inject (4.1).** Whether any `scripting::Convert` callee fills the grids
  for a `SimpleProposal.new()` carrier is INFERRED not to happen. The run-time
  `TerrainCarrierEmpty` test covers it.
- **Unknown field meanings.** The meaning of CE+0x1fd, Record48's fields and the second
  TransformedModel string is unknown. The replay copies them from the game's own builder, so nothing
  depends on it.
- **Windows interop.** Whether a Windows peer's TPTG or TPAS payload replays bit-identically on a
  Linux peer (and the other way) is untested. The formats are converted as in section 7.
- **Strokes that keep committing while held.** A TerrainPainter stroke and an AssetBrush paint
  stroke keep auto-committing while held (5.1). Whether that loses overlap during the replay window,
  as `PROPOSALS.md` describes for terraform, is to be observed (MEASURE).
- **Live check.** Every PROVEN layout still needs a check against a real terraform, paint and asset
  stroke in the running game (MEASURE stage).
