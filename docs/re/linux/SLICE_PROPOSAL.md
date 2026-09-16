# Linux: BuildProposal capture and proposal decoding (slice-proposal)

Linux native build 35924 (GNU build-id `3a0e156390b0e6f1e372051c24802c8493ae454a`). Every
address is a Linux RVA (file virtual address; live = image base + RVA). Windows RVAs (base
`0x140000000`) are given only as the reference each item was ported from. Status labels:

| label | meaning here |
|---|---|
| PROVEN | reproducible from the Linux binary: the instructions, strings, relocations or sol2 type strings cited |
| INFERRED | consistent with the Linux binary but resting on Windows measurements, on ABI rules or on UI-string proximity, not shown by Linux code |
| UNPROVEN | not established on Linux; must stay out of patch code |

Tooling: `/home/topsnek/tpf2-re/linux/{functions,xrefs,funcsig}.csv`, capstone linear sweeps
from FDE starts, `.rela.dyn` R_X86_64_RELATIVE relocations for vtables and tables, and the
sol2 `ctti_get_type_name` strings in `.rodata` (they spell every Lua-registered member
pointer with its C++ type).

Revision 2 (after independent verification): the factory **moves** the proposal and the
Context out of the caller (§1); `CommandList::Add` is now PROVEN (§2); frozenNodes, the
`SegmentAndEntity` type values, the optional engaged flag, the objects stride, the edge-object
translation and the edge-object category rule are now PROVEN (§4, §5); the unrouted callers
are identified (§3); the stop replace test is a recorded decision (§6).

## 1. `make_cmd::BuildProposal` (Windows `0x9dc750`, hook id 0)

**Linux `0x15ee930`**, size 2674. [PROVEN]

Identification:

- It sits in the `make_command.cpp` block between `make_cmd::SetName` `0x15ee6d0` and
  `make_cmd::BuyVehicle` `0x15ef3b0` (both named by `__PRETTY_FUNCTION__`).
- Its 18 direct callers (§3) are `UI::StreetBuilder::UpdateEngine`, `UI::TrackModifier::Build`,
  `UI::Bulldozer::Apply`, `UI::ConstructionBuilder::MousePressed`,
  `UI::ModuleBuilder::MousePressed` (all signature-named), the StreetTerminalBuilder and
  ProposalAction commits, the one sol2 caller and ten unrouted UI callbacks. No relocation and
  no `jmp` targets it, so the 18 `call rel32` sites are every caller.
- It stores variant tag 15 (`0x15ef2ab: mov byte [rbp-0x48], 0xf`) before the packaging call
  `0x15ebab0`; tag 15 is BuildProposal in the Windows dispatch table (COMMANDS.md). Every UI
  completion invoker checks the same tag on the queued command:
  `0xe867f7 cmp byte [rbx+0xd48], 0xf` with `rbx = *command` (also `0xeaaa87`, `0xe341c7`,
  `0xed6968`).

### Signature [PROVEN]

SysV; the Command is a class return through a hidden pointer, and both class arguments are
**by value, passed by hidden reference to a caller-owned temporary, and moved from**.

| reg | meaning | evidence |
|---|---|---|
| `rdi` | `Command*` result (echoed in `rax` at `0x15ef2e2`) | `0x15ee95b mov r12, rdi`; `0x15ef2a8 mov rdi, r12` into the packaging call `0x15ebab0` |
| `rsi` | `const ecs::Engine&` | `0x15ee972 mov r13, rsi`; `0x15ef2a2 mov rdx, r13` into `0x15ebab0` (the same slot SetName's named Engine argument takes) |
| `rdx` | `construction_builder_util::Proposal` (0x3c0 B) **by value**; move-assigned into the payload, left empty | `0x15ee964 mov [rbp-0x1b10], rdx`; `0x15ef154 mov rdx, [rbp-0x1b10]`; `0x15ef171 mov rsi, rdx`; `0x15ef17f call 0xde0430` with `rdi = rbx` = payload |
| `rcx` | `construction_builder_util::Context` (0x68 B) **by value**; copied field by field, its map and shared_ptr moved | `0x15ee95e mov r15, rcx`; field copies `0x15ef184..0x15ef1ce`; `+0x18` map moved by `0x15f3360` (`0x15ef18d lea rsi,[r15+0x18]`, `0x15ef197 lea rdi,[rbx+0x3d8]`, `0x15ef1d4 call`); shared_ptr `+0x58/+0x60` moved and zeroed `0x15ef1de..0x15ef1fc` |
| `r8b` | `withCostRep` → payload `+0x428` | `0x15ee954 mov [rbp-0x1b04], r8d`; `0x15ef1ea`; `0x15ef24f mov [rbp-0x16b8], r8b` |
| `r9b` | `ignoreErrors` → payload `+0x429` | `0x15ee961 mov r14d, r9d`; `0x15ef259 mov [rbp-0x16b7], r14b` |
| `[rsp]` at entry | return address into the caller: the routing key | SysV |

`0xde0430` is `Proposal::operator=(Proposal&&)` [PROVEN]: the payload's proposal is first
default-constructed (`0x15ee96b lea rbx,[rbp-0x1ae0]`; `0x15ee975 mov rdi, rbx`;
`0x15ee987 call 0xdc2b90`, the Proposal default constructor). `0xde0430` then frees the
destination's old buffer (`0xde044b mov rdi,[rdi]` … `0xde04a6 call 0x6dbcd0`), takes each
source vector and zeroes the source (`0xde0474 mov rax,[rsi]`; `0xde0477 mov [rbx],rax`;
`0xde047e mov qword [rsi],0`; `+0x18` at `0xde04cb..0xde04fc`; `+0x30` at
`0xde0550..0xde0574`; zeroing of `+0xd0` `0xde0675`, `+0xe8` `0xde0705`, `+0x220` `0xde0c68`,
`+0x270` `0xde0e07`, `+0x288` `0xde0e8b`, `+0x2a0` `0xde0f1b`, `+0x2f0` `0xde0fdc`, `+0x348`
`0xde115e`, `+0x370` `0xde121f`, `+0x398` `0xde134b`), resets the source rb-trees to empty
(`+0x100`: root `0xde083c`, leftmost/rightmost `0xde0848`/`0xde0850`, count `0xde0858`;
`+0x308`: `0xde10b7..0xde10db`) and moves the hash tables through `0xdd9b50` (calls at
`0xde061d`, `0xde0631`, `0xde0f98`). `0x15f3360` is the matching unordered_map move: it frees
the destination's nodes (`0x15f3380` loop), takes the source's buckets and nodes
(`0x15f33a2..0x15f33f1`) and resets the source to its single bucket with no elements
(`0x15f33f5..0x15f341d`).

Consequences for a detour:

- **Read and edit `*rdx` and `*rcx` only at entry, before calling the trampoline.** After
  the original returns, `*rdx` is an empty proposal (vectors `{0,0,0}`, maps reset) and
  `*rcx`'s `+0x18` map and `+0x58/+0x60` shared_ptr are gone. Every decoder (ROADE, EDEMO,
  CDEMO, STOPX, STOPXDEL, …), every template merge and every carrier injection must therefore
  run before the trampoline call.
- An edit made to `*rdx` at entry is exactly what the Command carries: the move steals the
  caller's buffers. A buffer a detour installs becomes owned by the Command and is later freed
  through the game's `operator delete` (PLT `0x6dbcd0`); allocate it with the game's
  `operator new` (PLT `0x6dbce0`), and free a buffer it replaces with the game's
  `operator delete`. (That a statically linked allocator would also work, because both end in
  glibc malloc, is INFERRED; use the game's PLT and there is nothing to infer.)
- The Windows `MergeTemplateStreet` / terrain-carrier technique (edit the caller's proposal,
  then let the factory take it) carries over, provided it runs at entry.

### Payload: `CmdData::BuildProposal` [PROVEN]

`scripting::RegisterUsertypesCmd` `0x1fae030` registers it with the call `0x1ffc790` at
`0x1fae49f`. Arguments: `rsi` = "BuildProposal", `rdx` = "proposal", `rcx` = slot `rbp-0x80`
(`0x1fae423`), `r8` = "context", `r9` = `[rbp-0x88]` = slot `rbp-0x78` (`0x1fae185`,
`0x1fae1e4`). The stack arguments, last push first, are "withCostRep"/`r15`,
"ignoreErrors"/`r14`, "resultProposalData"/`r13`, "resultEntities"/`r12` (pushes
`0x1fae459`, `0x1fae457`, `0x1fae453`, `0x1fae451`, `0x1fae44d`, `0x1fae444`, `0x1fae42e`,
`0x1fae421`). The slots were set up at `0x1fae07b` `r12=rbp-0x58`, `0x1fae091` `r13=rbp-0x60`,
`0x1fae098` `r14=rbp-0x68` and `0x1fae09c` `r15=rbp-0x70`; the values are written at
`0x1fae46f..0x1fae497`. The ctti string
`sol::usertype_metatable<CmdData::BuildProposal, …, const char (&)[9], construction_builder_util::Proposal CmdData::BuildProposal::*, const char (&)[8], construction_builder_util::Context CmdData::BuildProposal::*, const char (&)[12], bool CmdData::BuildProposal::*, const char (&)[13], bool CmdData::BuildProposal::*, const char (&)[19], construction_builder_util::ProposalData CmdData::BuildProposal::*, const char (&)[15], std::vector<ecs::Entity> CmdData::BuildProposal::*, …>`
gives the types in the same order.

| off | field | type | factory write |
|---|---|---|---|
| +0x000 | `proposal` | `Proposal` (0x3c0) | `0x15ef17f` move |
| +0x3c0 | `context` | `Context` (0x68) | `0x15ef19e mov word [rbp-0x1720]` … (`rbp-0x1720` = payload `+0x3c0`) |
| +0x428 | `withCostRep` | bool | `0x15ef24f` (`r8b`) |
| +0x429 | `ignoreErrors` | bool | `0x15ef259` (`r9b`) |
| +0x430 | `resultProposalData` | `ProposalData` | |
| +0xd30 | `resultEntities` | `vector<ecs::Entity>` | |
| +0xd48 | variant index (15) of the Command's payload variant | u8 | `0x15ef29a call 0x15f39d0` builds the variant at `rbp-0xd90` from the payload; tag at `rbp-0x48` = `+0xd48` (`0x15ef2ab`); the invokers read `[*cmd+0xd48]` |

The factory also writes `payload+0x3c9` = Context `+0x09` := 0 (`0x15ef260 mov byte
[rbp-0x1717], 0`) after copying the Context's word at `+0x08` (`0x15ef188`, `0x15ef1aa`): the
Command's copy of that byte is always 0, whatever the caller passed. It then collects the
referenced entities (`0x15ef288 call 0x163e790`, `rdi` = payload, `esi` = 1).

The Lua maker passes `r8d = 1` (`0x1971325`) and `r9d = [rbp-0x26cc]` (`0x1971315`), which is
`0x982730(L, 3 + (0x982290(L, 3) != -1)) != 0` (`0x1970b38..0x1970b7e`): the boolean Lua
argument after the optional context, i.e. `ignoreErrors`, and `withCostRep = true`.

### Prologue

`f3 0f 1e fa 55 48 89 e5 41 57 41 56 41 55 41 54 53 48 81 ec 00 10 00 00 48 83 0c 24 00 ...`

| off | len | bytes | instruction |
|---|---|---|---|
| +0 | 4 | `f30f1efa` | endbr64 |
| +4 | 1 | `55` | push rbp |
| +5 | 3 | `4889e5` | mov rbp, rsp |
| +8 | 2 | `4157` | push r15 |
| +10 | 2 | `4156` | push r14 |
| +12 | 2 | `4155` | push r13 |
| +14 | 2 | `4154` | push r12 |
| +16 | 1 | `53` | push rbx |
| +17 | 7 | `4881ec00100000` | sub rsp, 0x1000 |
| +24 | 5 | `48830c2400` | or qword [rsp], 0 (stack probe) |
| +29 | 7 | `4881ece80a0000` | sub rsp, 0xae8 |

Steal **14** (endbr64 .. push r13; what `PrologueSteal(code, 14)` returns) or 16; both end on
an instruction boundary, and nothing in the first 0x65 bytes is RIP-relative (first:
`0x15ee995 movss xmm0, [rip+..]`). The stolen bytes are rsp-relative only, so the trampoline
is valid when entered with the entry rsp. Verify all 16 bytes at run time.

Alternative without an entry patch: every caller is a 5-byte `e8 rel32` (§3), so
`Tpf2mpRedirectCall(site, 0x15ee930, stub)` per routed caller also works; an entry hook
covers all 18 with one patch and sees the caller as `[rsp]`.

## 2. `CommandList::Add` as seen from these callers

**`0x15da840`**, 94 direct `e8` call sites, no `jmp`, no relocation, no RIP-relative `lea` of
it. [PROVEN; slice-core owns the hook, SLICE_CORE.md §2]

Identity: the lambda `0x15da310` references the assert signature
`CommandList::Add(Command, std::function<void(const Command&)>, std::weak_ptr<CmdProgress>)::<lambda(const boost::signals2::connection&, const std::vector<Command>&)>`
(`0x15da66c`, file `CommandList.cpp` `0x15da678`). `0x15da840` installs that lambda: it loads
`0x59be5f0` (`0x15dab9e lea rax`), and `.rela.dyn` fills `0x59be5f0 → 0x15da710` (manager)
and `0x59be5f8 → 0x15da310` (invoker). A function that constructs the closure of `Add`'s
lambda is `Add`'s body, and its argument shape matches the signature (slice-core C-ADD-3).

At all seven UI callers:

- `rdi` = out handle (`Connection*`, 8 B), destroyed right after by `0x3190430`;
- `rsi` = `CommandList*`; `rdx` = the Command the factory built (**factory `rdi` == Add
  `rdx`** [PROVEN per caller]); `rcx` = `std::function<void(const Command&)>*`
  (libstdc++: functor storage 16 B, manager `+0x10`, invoker `+0x18`); `r8` =
  `std::weak_ptr<CmdProgress>*`, a zeroed 16 B pair (`[..]=0` twice before the call);
- `rax` is not read after the call (next instruction loads `rdi` for `0x3190430`).
- The invokers read `rsi` = `const Command&` and dereference `*command` (`0xe867e5 mov rbx,
  [rsi]`), so `invoker(&functor, cmd)` is the libstdc++ `_M_invoker(const _Any_data&,
  const Command&)` call.

Note for slice-core's cancel: `0x3190430` loads `[rdi]` and returns at once when it is 0
(`0x319043d mov rbx,[rdi]`; `0x3190440 test rbx,rbx`; `0x3190443 je`); otherwise it drops a
weak count. An Add that is skipped must leave the 8-byte out handle 0 (the Linux form of the
Windows `ZeroAddResult`).

## 3. Callers (routing keys)

"ret" is the factory's return address, i.e. `[rsp]` at the factory entry. All sites are
`call rel32` to `0x15ee930`; the bytes are what a run-time check should compare. [PROVEN]

| Windows | Linux function | site / bytes | factory ret | Add call / ret | owner of the content |
|---|---|---|---|---|---|
| `0x459e97` / Add `0x459eb7` | `UI::StreetBuilder::UpdateEngine` `0xe861b0` (sig) | `0xe8644d` `e8de847600` | **`0xe86452`** | `0xe86482` / `0xe86487` | slice-proposal (ROADE) |
| `0x4790fc` | `UI::TrackModifier::Build` `0xed69d0` (sig) | `0xed6da4` `e8877b7100` | **`0xed6da9`** | `0xed6dd8` / `0xed6ddd` | slice-proposal (upgrade ROADE + STREETP) |
| `0x460e0b` | StreetTerminalBuilder commit `0xeaabb0` | `0xeaaf74` `e8b7397400` | **`0xeaaf79`** | `0xeaafad` / `0xeaafb2` | slice-proposal (STOPX) |
| `0x3eb227` / Add `0x3eb245` | `UI::Bulldozer::Apply` `0xdd4590` (sig) | `0xdd4e94` `e8979a8100` | **`0xdd4e99`** | `0xdd4ebe` / `0xdd4ec3` | slice-proposal (EDEMO, CDEMO, STOPXDEL); CONUP stash: slice-construction |
| `0x4311c6` | ProposalAction commit `0xe59490` | `0xe5961d` `e80e537900` | **`0xe59622`** | `0xe59652` / `0xe59657` | slice-terrain-assets |
| `0x419f62` / Add `0x419f81` | `UI::ConstructionBuilder::MousePressed` `0xe343c0` (sig) | `0xe3485b` `e8d0a07b00` | **`0xe34860`** | `0xe34890` / `0xe34895` | slice-construction (ROADC uses the §5 records) |
| `0x42bc7b` | `UI::ModuleBuilder::MousePressed` `0xe4eb30` (sig) | `0xe4f6b8` `e873f27900` | **`0xe4f6bd`** | `0xe4f6ed` / `0xe4f6f2` | slice-construction (shape-routed upgrade) |
| `0xced378` (Lua `api.cmd.make.buildProposal`) | sol2 functor body `0x1970a10` | `0x197132e` `e8fdd5c7ff` | **`0x1971333`** | none (the maker does not call Add) | slice-construction (merge), slice-terrain-assets (carriers) |

Per-caller arguments at the factory call, and the completion functor passed to Add
(`manager` / `invoker` are the libstdc++ `std::function` slots at functor `+0x10` / `+0x18`):

| caller | rdi (Command) | rdx (Proposal temporary) | rcx (Context temporary) | r8d / r9d | Add `rcx`: storage / manager / invoker |
|---|---|---|---|---|---|
| UpdateEngine | `r15`=rbp-0xab0 | rbp-0x4b0, copy (`0xe3e060` at `0xe86423`) of rbp-0x870 | `r14`=rbp-0xa70 | `[rbx+0x209]` / 0 | rbp-0xf0 `{this}` / `0xe6eda0` / `0xe867d0` |
| TrackModifier::Build | `r15`=rbp-0x7c0 | rbp-0x4b0, copy of `[rbx+0x118]+8` (`0xed6d7b`) | rbp-0x6b0 (built by `0xe39c00`) | 1 / 0 | rbp-0xf0 `{this}` / `0xec62c0` / `0xed6960` |
| StreetTerminalBuilder | rbp-0x1840 | `r12`=rbp-0x1600, copy of `[rbx+0xc0]` (`0xeaaf4b`) | `r14`=rbp-0x1800 | 0 / 0 | rbp-0xf0 `{this}` / `0xea53c0` / `0xeaaa60` |
| Bulldozer::Apply | `rbx`=rbp-0x6f0 | `r13`, built inline (StreetProposal copy `0xdef0f0` at `0xdd4a19`, then `0xdd98b0` `0xdd4a86`, `0x9b8040` `0xdd4a99`); destroyed by `0xdc3040` at `0xdd4ed6` | `[rbp-0x738]` (its `+0x18` map destroyed by `0xdd87e0` at `0xdd4f3b`) | 1 / `r15d` | rbp-0xf0 `{r12}` / `0xdd1270` / `0xdd62a0` |
| ProposalAction | `r15`=rbp-0x4d0 | `r14`=rbp-0x420, copy of `[rbx+0x78]` (`0xe595f3`) | `r13`=rbp-0x490 | 1 / arg `sil` | rbp-0x60 `{this, byte}` / `0xe57670` / `0xe593a0` |
| ConstructionBuilder | `r12`=rbp-0x760 | `r15`, copy of `[rbx+0x850]` (`0xe34831`) | `[rbp-0x788]` | `[rbx+0x912]` / 0 | rbp-0xf0 `{this, r12}` / `0xe27060` / `0xe341a0` |
| ModuleBuilder | `r15`=rbp-0xc30 | `r13`, copy of `[rbx+0xa8]` (`0xe4f68f`) | `[rbp-0xc58]` | 1 / 0 | rbp-0x5c0 / `0xe4d3f0` / `0xe4c600` |
| Lua maker | `r15`=rbp-0x2680 | `rbx`=rbp-0x1ea0, copy (`0x1971310`) | `r12`=rbp-0x2640, 13 qwords zeroed (`0x19712be rep stosq`) | 1 / `[rbp-0x26cc]` | n/a |

Every caller hands the factory a disposable proposal: a copy made by the Proposal copy
constructor `0xe3e060` (it allocates new storage and only reads its source, e.g. `0xe3e505`
`_Znwm` for the height grid) or, in the bulldozer, one built inline. The factory empties it,
and the caller destroys the empty shell after Add. The tool's own proposal (`[rbx+0xc0]`,
`[rbx+0x850]`, …) is untouched; only the temporary at `rdx` is moved from.

Identity evidence for the unnamed ones:

- **StreetTerminalBuilder commit `0xeaabb0`** [PROVEN]: `UI::StreetTerminalBuilder` vtable
  `0x5a0b798` (typeinfo `0x5a0b730`) slot 3 = `0xeab1c0` (relocation at `0x5a0b7b0`), which
  tail-jumps to `0xeaabb0` at `0xeab1ef`; slot 6 = `0xeab200` (named
  `UI::StreetTerminalBuilder::Step`), slot 13 = `0xea5440` (named `DoActivate`). The function
  lies between `0xea7380` and `0xeab200`, both `StreetTerminalBuilder.cpp`.
- **ProposalAction commit `0xe59490`** [PROVEN]: its four callers are vtable slots of the
  ProposalAction tools: `UI::TerrainModifier` `0x5a0bcc0` slot 15 `0xeaca40`,
  `UI::TerrainPainter` `0x5a0bda0` slot 15 `0xebc510` and slot 3 `0xebcad0`, `UI::AssetBrush`
  `0x5a09980` slot 15 `0xdb9960` (relocations `0x5a0bd38`, `0x5a0be18`, `0x5a0bdb8`,
  `0x5a099f8`). It sets tool `+0xd0` = 1 (`0xe59503`; Windows `+0xf0`), its invoker clears
  it (`0xe593b4`), and it frees the proposal with sized delete 0x3c0 (`0xe59758`) and the
  ProposalData with 0x900 (`0xe59700`).
- **Lua maker `0x1970a10`** [PROVEN]: the only sol2-area caller; tail-jumped from `0x1972920`
  (`functor_function<scripting::SetupCommandInterface(...)::lambda>`); references the sol2
  getters for `scripting::Proposal` and `construction_builder_util::Context`;
  `SetupCommandInterface` `0x19638f0` registers `buildProposal` with argument names
  `proposal`, `context`, `ignoreErrors` (`0x1963c69..0x1963cbe`) as a `functor_function`
  (`0x1963ce0`).

For BuildProposal, `IsScriptCaller(ret)` reduces to `ret == 0x1971333`.

### Unrouted callers (Windows logs these as "UNREPLICATED")

Each one is a UI callback: a small thunk `jmp`s to it, and the thunk's address is taken by a
RIP-relative `lea` in the widget builder. None is reached by a direct call or a relocation.
Each body copies a proposal (`0xe3e060`), calls the factory and calls Add itself (e.g.
`0x1240730`: `0x12409ad`, `0x12409dd`, `0x1240a12`). The construction-upgrade shape test
(§7 step 8) still applies to them.

| site / ret | containing function | registered by (thunk, lea site, builder) | what it changes | status |
|---|---|---|---|---|
| `0x12409dd` / `0x12409e2` | `0x1240730` | thunk `0x1240be0`, lea `0x123fb95` in `0x123d4f0` (TrafficControlComp: "TrafficControlComp::PlayerOwnedButton" `0x123fa35`, "Player Owned" `0x123fbe4`) | edge ownership: calls `0x2ea1d50` (`0x12407d1`), which builds a `std::function` (storage `rbp-0x40` = player, manager `0x2e98f60` `0x2ea1d81`, invoker `0x2e994a0` `0x2ea1d76`) for `0x2ea1960`; the invoker writes `seg+0x70` = player and `seg+0x74` = 1, or clears `+0x74` when the player is < 0 | writes PROVEN; button pairing INFERRED |
| `0x1240e94` / `0x1240e99` | `0x1240bf0` | thunk `0x12410a0`, lea `0x123f85d` ("TrafficControlComp::TrafficLightButton" `0x123f707`, "Toggle traffic light" `0x123f89c`) | node: calls `construction_util::UpdateBaseNodeCreateProposal(const StreetToolkit&, const Entity&, bool, bool, …)` `0x2ea13a0` at `0x1240c82` | callee PROVEN; action INFERRED |
| `0x12413fd` / `0x1241402` | `0x12410b0` | thunk `0x1241620`, lea `0x123f53b` ("TrafficControlComp::PrecedenceButton" `0x123f40f`, "Toggle precedence" `0x123f58b`) | edge precedence: calls `0x2ea1cc0` (`0x12411d9`), invoker `0x2e98f10` (`0x2ea1ce1`): `mov rax,[rdi]; mov [rsi+0x58],rax` = `precedenceNode0/1` | writes PROVEN; pairing INFERRED |
| `0x145e1a7` / `0x145e1ac` | `0x145ddc0` `ActualViewCreator::CreateSignalView(const ecs::Entity&)::<lambda(bool)>` (sig `0x145e2a9`) | thunks `0x145e320`/`0x145e340`/`0x145e360`, leas `0x14561f0`/`0x14562a5`/`0x1455f17` in `0x1455560` ("The name of the signal", "signal-view", "The name of the waypoint", "One-way") | signal/waypoint one-way toggle; `street_util::MakeOneWayEdgeObjectProposal` `0x2f8afb0` is the matching proposal maker | INFERRED |
| `0x145ef7d` / `0x145ef82` | `0x145ece0` | thunk `0x145f110`, lea `0x1461743` in `0x1460c30` ("Set Railroad Crossing Type", "railroadcrossing-view") | railroad crossing type | INFERRED |
| `0x145fbc0` / `0x145fbc5` | `0x145f8c0` | thunk `0x145fd80`, lea `0x14628d5` in `0x1461ac0` ("Set Bridge Type", "Set Tunnel Type") | bridge / tunnel type | INFERRED |
| `0x145e540` / `0x145e545` | `0x145e380` | thunks `0x145e670` (lea `0x14449be` in `0x14448f0`), `0x145e6a0` (lea `0x1444b29` in `0x1444a60`); no strings | unknown | UNPROVEN |
| `0xf229a0` / `0xf229a5` | `0xf225c0` | thunk `0xf22c50`, lea `0xf1c4d1` in `UI::AddModuleComp::UpdateTabs()` `0xf1b9f0` | a construction-module tab callback | INFERRED |
| `0x1045732` / `0x1045737`, `0x1045a42` / `0x1045a47` | `0x1045460` `CGameUI::CreateConstructionMenu(…)::<lambda(const string&, const std::vector<…>&)>` (sig `0x104603e`, assert `!commands.empty()`) | thunk `0x1046120`, lea `0x1046e6f` in `CreateConstructionMenu` `0x1046540` (between ".generate" `0x1046cac` and ".industries"/"Industries" `0x1046ee4`/`0x1046f0b`) | map-editor generate menu | INFERRED |

These are real player edits of the network (ownership, traffic lights, precedence, one-way
signals, crossing, bridge and tunnel types) that neither the Windows build nor this port
replicates. Routing them is an open question (§9).

## 4. `construction_builder_util::Proposal` (0x3c0 B; Windows 0x2f8)

Sources, all Linux:

- default constructor `0xdc2b90` (writes every member; empty-map headers point at themselves),
- destructor `0xdc3040`, tail-calling the `StreetProposal` destructor `0xdc0280`,
- move assignment `0xde0430` (§1) and copy constructor `0xe3e060`,
- the Lua usertype registration in `scripting::RegisterUsertypesTransport` `0x2c3f1f0`: sol2
  passes each member pointer by reference as `(name, &offset)`, and the offsets are constants
  stored just before the call. `StreetProposal`: `0x2c40b3b..0x2c40bbf` then call
  `0x2caaf00`; `Proposal`: `0x2c4112d..0x2c4116f` then call `0x2ca7a60`. Each name is paired
  with its pointer slot by argument order, and each slot pointer is traced to its `lea`
  (`[rbp-0x7e0]`=rbp-0x6b0 `0x2c40176`, `[rbp-0x7f0]`=rbp-0x6a0 `0x2c40361`,
  `[rbp-0x818]`=rbp-0x690 `0x2c404e3`, `[rbp-0x7d8]`=rbp-0x680 `0x2c40791`, `[rbp-0x7b8]`=
  rbp-0x610 `0x2c3fd2c`, `[rbp-0x7a8]`=rbp-0x600 `0x2c3fd25`, `[rbp-0x798]`=rbp-0x5f0
  `0x2c3fba0`, `[rbp-0x788]`=rbp-0x5e0 `0x2c3f8b3`, `[rbp-0x7b0]`=rbp-0x620 `0x2c40c44`,
  `[rbp-0x7d0]`=rbp-0x630 `0x2c40c5b`, `r12`=rbp-0x640 `0x2c40bdd`),
- the sol2 `ctti_get_type_name` strings (member types, e.g. `street_util::StreetProposal
  construction_builder_util::Proposal::*`),
- `construction_builder_util::GetReferencedEntities(const Proposal&, bool,
  std::vector<ecs::Entity>&)` `0x163e790` (strides and entity offsets),
- `street_util::ProposalStreetGraph::IsNodeLocked(const ecs::Entity&) const` `0x164fd50`
  (frozenNodes),
- sized delete of a heap proposal: `0xe59750 mov esi, 0x3c0`; the Context follows at
  `+0x3c0` in `CmdData::BuildProposal` (§1).

| off | Windows | field | type / element | evidence | status |
|---|---|---|---|---|---|
| +0x000 | +0x000 | `proposal` (street half, 0x238 B) | `street_util::StreetProposal` | registration (`proposal` → slot 0) | PROVEN |
| +0x000 | +0x000 | ↳ `addedNodes` | `vector<NodeAndEntity>`, 0x18 | registration; stride `0x163ea13 add r14, 0x18` | PROVEN |
| +0x018 | +0x018 | ↳ `addedSegments` | `vector<SegmentAndEntity>`, 0x78 | registration; dtor stride `0xdc041f`; `0x163e80c` | PROVEN |
| +0x030 | +0x030 | ↳ `removedNodes` | `vector<NodeAndEntity>`, 0x18 | registration; `0x163e824 mov eax,[r14+0x14]` / `add r14,0x18`; `IsNodeLocked` asserts `!ContainsEntity(m_proposal->removedNodes, entity)` over `[r8+0x30]` (`0x164fd5b`, string `0x1650073`) | PROVEN |
| +0x048 | +0x048 | ↳ `removedSegments` | `vector<SegmentAndEntity>`, 0x78 | registration; dtor stride `0xdc03d7`; `0x163e88f mov eax,[r14]` | PROVEN |
| +0x060 | +0x060 | ↳ (not registered) | `unordered_map`, 56 B | ctor `0xdc2baa..0xdc2c46`; dtor `0xdc03af` | PROVEN shape |
| +0x098 | (+0x0a0) | ↳ (not registered) | `unordered_map`, 56 B | ctor `0xdc2bb5..0xdc2c7a`; dtor `0xdc03a6` | PROVEN shape |
| +0x0d0 | +0x0e0 | ↳ `edgeObjectsToRemove` | `vector<ecs::Entity>`, 4 | registration (slot 0xd0); `0x163e913 add r14, 4` | PROVEN |
| +0x0e8 | +0x0f8 | ↳ `edgeObjectsToAdd` | `vector<StreetProposal::EdgeObject>`, 0x100 | registration (slot 0xe8); dtor stride `0xdc036e`; `0x163e9b2`; push_back `0x2f8acae add qword [rbx+0xf0], 0x100` | PROVEN |
| +0x100 | (+0x110) | ↳ `new2oldNodes` | `std::map`, 48 B | registration (slot 0x100); ctor header `+0x108`; dtor `0xdc0312`; move reset `0xde083c..0xde0858` | PROVEN |
| +0x130 | | ↳ `old2newNodes` | `std::map` | registration (0x130) | PROVEN |
| +0x160 | | ↳ `new2oldSegments` | `std::map` | registration (0x160) | PROVEN |
| +0x190 | | ↳ `old2newSegments` | `std::map` | registration (0x190) | PROVEN |
| +0x1c0 | | ↳ `new2oldEdgeObjects` | `std::map` | registration (0x1c0) | PROVEN |
| +0x1f0 | (+0x160) | ↳ `old2newEdgeObjects` | `std::map` | registration (0x1f0) | PROVEN |
| +0x220 | +0x170 | ↳ `frozenNodes` | `vector<int>`: indices into `addedNodes` | registration (0x220); dtor `0xdc0294` frees without a loop; `IsNodeLocked` (below) | PROVEN |
| +0x238 | +0x188 | `terrainAlignSkipEdges` | unordered set/map, 56 B | registration (0x238); ctor `0xdc2e31`; dtor `0xdbf800` | PROVEN |
| +0x270 | +0x1c8 | `segmentTags` | `vector<std::string>` | registration (0x270); dtor `0x9b7f60` (the vector<string> destroyer) | PROVEN |
| +0x288 | +0x1e0 | `toRemove` | `vector<ecs::Entity>`, 4 | registration (0x288); `0x163ebbb add r14, 4` | PROVEN |
| +0x2a0 | +0x1f8 | `toAdd` | `vector<ConstructionEntity>`, **0x8f0** (Windows 0x8e0) | registration (0x2a0); dtor `0xdc30d3 add r12, 0x8f0`; `0x163ec3a` | PROVEN |
| +0x2b8 | +0x210 | `old2new` | `unordered_map`, 56 B | registration (0x2b8); dtor `0xbfaf70` | PROVEN |
| +0x2f0 | +0x250 | `parcelsToRemove` | vector, trivially destructible | registration (0x2f0); dtor `0xdc309a` | PROVEN |
| +0x308 | +0x268 | (not registered) | `std::map`/`set`, 48 B (header `+0x310`) | ctor `0xdc2f4f..0xdc2f6b`; dtor `0xdc0460`; move reset `0xde10b7..0xde10db` | PROVEN shape |
| +0x338 | +0x278 | grid 1 (Windows / slice-terrain-assets: height modifier) | `{int x0,y0,w,h}` + `vector` of **8-byte** elements at +0x348 (0x28 B) | copy ctor: four dwords `0xe3e479..0xe3e4c6`, count `0xe3e4da sar rax,3`, limit `0xe3e4ef 0x1fffffffffffffff`; dtor frees `+0x348` | shape and element size PROVEN; `Grid<CVec2f>` INFERRED here |
| +0x360 | +0x2a0 | grid 2 (material) | four dwords + `vector` of **1-byte** elements at +0x370 (0x28 B) | copy ctor `0xe3e54e..0xe3e5ca` (`_Znwm` of the byte count, no shift) | shape and element size PROVEN; meaning INFERRED |
| +0x388 | +0x2c8 | grid 3 (mask) | four dwords + `vector<bool>` at +0x398: start `{p +0x398, off +0x3a0}`, finish `{p +0x3a8, off +0x3b0}`, end_of_storage `+0x3b8` (0x38 B) | ctor dword writes at `+0x3a0`/`+0x3b0`; move `0xde1269..0xde1363`; dtor frees `+0x398` | shape PROVEN; meaning INFERRED |

Everything up to `removedSegments` keeps its Windows offset; everything from the first hash
map on moves, because libstdc++ `unordered_map` is 56 B (MSVC 64) and `std::map` 48 B
(MSVC 16). The terrain grids belong to slice-terrain-assets (SLICE_TERRAIN_ASSETS.md names
grid 1 through `GetBaseHeightModBBox(const Grid<CVec2f>&, CVec2f)` `0x1621b20`, called at
`0x1634300` with `rdi = r15+0x338`; this area did not re-derive what `r15` is there) and are
listed only so the layout is complete.

**frozenNodes** [PROVEN]. `IsNodeLocked` `0x164fd50`, `r8 = m_proposal = [rdi+0x20]`
(`0x164fd55`):

1. It asserts that the entity is not in `removedNodes` (`0x164fd5b..0x164fd90`).
2. It finds the entity in `addedNodes` by `NodeAndEntity.entity` (`0x164fdc1 cmp
   [r9+0x14], edx` and the unrolled loop).
3. It turns the hit into an index: `0x164fecd sub rax, r9`, then `0x164feeb sar rdx, 3` and
   `0x164fef2 imul rdx, 0xaaaaaaaaaaaaaaab`, i.e. divide by 0x18.
4. It searches `frozenNodes` for that index as 32-bit ints: `0x164fed7 mov rsi,[r8+0x220]`,
   `0x164fed0 mov r9,[r8+0x228]`, `0x164ff0f cmp edx,[rsi]` … `+4`/`+8`/`+0xc` and the stride-4
   tail `0x164ff8c..0x164ff9e`.
5. It returns found != end (`0x164ffa6 cmp r9, rcx`; `setne al`).

A node that is not among the added nodes and has an id >= 0 is looked up in a set at
`[rdi+0x18]` instead (`0x164fe78..0x164febc`). So `frozenNodes[i]` is an index into
`addedNodes`, and the node it names cannot be changed.

### `construction_builder_util::Context` (0x68 B; Windows 0x70)

| off | field | type | evidence | status |
|---|---|---|---|---|
| +0x00 | `gatherBuildings` | bool | registration `0x2c410a3..0x2c4111a`: slot `[rbp-0x790]`=rbp-0x510 (`0x2c3f4f8`) = 0 | PROVEN |
| +0x01 | `gatherFields` | bool | slot `r14`=rbp-0x4d0 (`0x2c3f259`) = 1 | PROVEN |
| +0x08 | `checkTerrainAlignment` | bool | slot `[rbp-0x798]`=rbp-0x5f0 = 8 | PROVEN |
| +0x09 | unnamed byte; **the factory stores 0 in the Command's copy** | u8 | `0x15ef188`/`0x15ef1aa` copy the word at +0x08, `0x15ef260` then clears payload `+0x3c9` | PROVEN write, meaning UNPROVEN |
| +0x0a | `cleanupStreetGraph` | bool | slot `[rbp-0x788]`=rbp-0x5e0 = 0xa | PROVEN |
| +0x14 | `player` | `ecs::Entity` | slot `r15`=rbp-0x490 (`0x2c3f24f`) = 0x14 | PROVEN |
| +0x04, +0x10 | unnamed (1.0f, 0.75f in the UI and Lua defaults) | | `0xe86354`, `0x19712c6..0x19712d4`; copied `0x15ef191`, `0x15ef1b7` | UNPROVEN meaning |
| +0x18 | unnamed `unordered_map` | 56 B | **moved** into payload `+0x3d8` by `0x15f3360` (§1) | PROVEN shape |
| +0x50 | unnamed word | | copied to payload `+0x410` (`0x15ef1d9`, `0x15ef1f1`) | UNPROVEN meaning |
| +0x58 | shared_ptr `{ptr, control}` | moved into payload `+0x418` and zeroed in the source | `0x15ef1de..0x15ef215` | PROVEN |

Size: the Lua maker zeroes exactly 13 qwords (`0x19712a3 mov ecx, 0xd; rep stosq`) and the
UI's last field is at rbp-0xa10 for a Context at rbp-0xa70 (`0xe863fb`). The bulldozer's
`rcx` is a Context too (its `+0x18` map is destroyed by `0xdd87e0`), so the Windows note
that the bulldozer passes "a 0x70-byte options struct" is the MSVC Context size, not a
different type.

## 5. Records

sol2 note for the component usertypes in `scripting::RegisterComponentsAB` `0x2144fe0`: the
`usertype_metatable` keeps its `(name, member)` arguments in a `std::tuple`, which libstdc++
lays out last element first, so each member offset sits 8 bytes **below** its name pointer.

### `NodeAndEntity` (0x18 B)

| off | field | type | status |
|---|---|---|---|
| +0x00 | `comp.position` | `CVec3f` (x, y, z) | PROVEN |
| +0x0c | `comp.doubleSlipSwitch` | bool | PROVEN |
| +0x0d..+0x0f | not a registered member | | UNPROVEN (Windows wrote `flags` 0x7f00 as a u32 at +0x0c on template nodes; on Linux +0x0c is the bool and +0x0d.. are unaccounted) |
| +0x10 | `comp.trafficLightPreference` | `TrafficLightPreference` (Windows "type", 2 in every sample) | PROVEN |
| +0x14 | `entity` | `ecs::Entity`: placeholder (-1, -2, ...) or existing id | PROVEN (also `IsNodeLocked` `0x164fdc1`) |

Evidence: `NodeAndEntity` registration `0x2c40fe5..0x2c41010` (`comp` → slot rbp-0x4d0 = 0,
`entity` → slot rbp-0x490 = 0x14); `BaseNode` registration call `0x2146ca7` (`position` →
`[rbp-0x570]`=rbp-0x4b0 = 0, `doubleSlipSwitch` → `r14`=`[rbp-0x580]`=rbp-0x490 = 0xc,
`trafficLightPreference` → `rbx`=`[rbp-0x588]`=rbp-0x450 = 0x10; slot pointers set at
`0x214500a`, `0x2144ffa`, `0x214505f`, `0x214507e`, `0x21450e3`, `0x21450ea`); ctti types
`CVec3f`, `bool`, `TrafficLightPreference`; stride and entity read at `0x163e824` /
`0x163e833`.

### `SegmentAndEntity` (0x78 B)

| off | field | type | status |
|---|---|---|---|
| +0x00 | `entity` | `ecs::Entity` (placeholder or existing edge) | PROVEN |
| +0x04 | padding (`comp` is 8-aligned) | | PROVEN by the next offset; Windows "trap: uninitialised" |
| +0x08 | `comp.node0` | `ecs::Entity` | PROVEN |
| +0x0c | `comp.node1` | `ecs::Entity` | PROVEN |
| +0x10 | `comp.tangent0` | `CVec3f` | PROVEN |
| +0x1c | `comp.tangent1` | `CVec3f` | PROVEN |
| +0x28 | `comp.type` | `BaseEdgeType`: **0 NORMAL, 1 bridge, 2 TUNNEL** | PROVEN (below) |
| +0x2c | `comp.typeIndex` | int: index into the bridge types (type 1) or tunnel types (type 2) | PROVEN; "-1 on the ground" INFERRED (Windows) |
| +0x30 | `comp.objects` | `std::vector<std::pair<ecs::Entity, EdgeObjectType>>`, element 8 B | PROVEN |
| +0x48 | `type` | int: **0 street (`streetEdge` valid), 1 track (`trackEdge` valid)**; nothing else is accepted | PROVEN (below) |
| +0x4c | `streetEdge.streetType` | int | PROVEN |
| +0x50 | `streetEdge.hasBus` | **bool** (1 byte; +0x51..+0x53 padding = the Windows "noise") | PROVEN |
| +0x54 | `streetEdge.tramTrackType` | `TramTrackType` (Windows: 0 none, 1 regular, 2 electric) | offset/type PROVEN, values INFERRED |
| +0x58 | `streetEdge.precedenceNode0` | `PrecedencePreference` | PROVEN |
| +0x5c | `streetEdge.precedenceNode1` | `PrecedencePreference` | PROVEN |
| +0x60 | `trackEdge.trackType` | int | PROVEN |
| +0x64 | `trackEdge.catenary` | **bool** (the Windows "low byte") | PROVEN |
| +0x68..+0x6f | not registered (Windows: owning construction at +0x68) | | UNPROVEN |
| +0x70 | `playerOwned` payload → `player` | `ecs::Entity` inside `std::optional<ecs::component::PlayerOwned>` | PROVEN |
| +0x74 | `playerOwned` engaged flag (Windows "owned flag") | bool | PROVEN (below) |
| +0x75..+0x77 | padding (optional is 8 B, record 0x78) | | PROVEN by the offsets |

Evidence:

- `SegmentAndEntity` registration: call `0x2c41095` → `0x2c3d310` → tuple fill in
  `0x2c3c2e0`: `+0xa8`←arg8 / `+0xb0` "trackEdge", `+0xb8`←arg7 / `+0xc0` "streetEdge",
  `+0xc8`←`*r9` / `+0xd0` "playerOwned", `+0xd8` 16 B / `+0xe8` "params" (a property),
  `+0xf0`←`*rcx` / `+0xf8` "type", `+0x100`←`*rdx` / `+0x108` "comp", `+0x110`←`*rsi` /
  `+0x118` "entity". The caller's slots: `rsi`→rbp-0x610 = 0, `rdx`→rbp-0x600 = 8,
  `rcx`→rbp-0x5f0 = 0x48, `r9`→rbp-0x5e0 = 0x70, arg7 `rbx`→rbp-0x510 = 0x4c, arg8 `r14`→
  rbp-0x4d0 = 0x60 (values written `0x2c4102f..0x2c4108a`). Nothing is registered at 0x68.
- `BaseEdge` tuple `0x2145f33..0x21460a4`: `typeIndex` name at `+0xc8`, offset 0x24 at
  `+0xc0`; `type` `+0xd8` / 0x20; `tangent1` `+0xe8` / 0x14; `tangent0` `+0xf8` / 8;
  `node1` `+0x108` / 4; `node0` `+0x118` / 0 (relative to `comp` at +0x08).
- `BaseEdgeStreet` call `0x2146c1d`: `streetType` → `[rbp-0x5a0]`=rbp-0x4f0 = 0, `hasBus` →
  `[rbp-0x578]`=rbp-0x4d0 = 4, `tramTrackType` → `[rbp-0x570]`=rbp-0x4b0 = 8,
  `precedenceNode0` → rbp-0x490 = 0xc, `precedenceNode1` → rbp-0x450 = 0x10.
- `BaseEdgeTrack` call `0x2146c5a`: `trackType` → rbp-0x490 = 0, `catenary` → rbp-0x450 = 4.
- `PlayerOwned` call `0x228d086`: `player` → `r13`=rbp-0x2b0 (`0x228ac47`) = 0.
- ctti member types: `ecs::Entity`, `ecs::component::BaseEdge`, `int`,
  `std::optional<ecs::component::PlayerOwned>`, `BaseEdgeStreet`, `BaseEdgeTrack`;
  `int streetType`, `bool hasBus`, `TramTrackType tramTrackType`; `int trackType`,
  `bool catenary`; BaseEdge `Entity, Entity, CVec3f, CVec3f, BaseEdgeType, int` and
  `objects` as a property over `std::vector<std::pair<ecs::Entity, EdgeObjectType>>`.
- `GetReferencedEntities` reads `node0` `[+0x08]` and `node1` `[+0x0c]` of every added
  segment (`0x163e7d8..0x163e80c`) and `entity` `[+0x00]` of every removed one (`0x163e88f`).
- **`type` values.** `UI::{anonymous}::GetSlopeSteps(const StreetTypeRep&, const
  TrackTypeRep&, const SegmentAndEntity&)` `0xe72c20` reads `[rdx+0x48]` (`0xe72c21`).
  - 0: `ResTypeRep<StreetType>::Get([+0x4c])` (`0xe72c50`/`0xe72c53`).
  - 1: `ResTypeRep<TrackType>::Get([+0x60])` (`0xe72c33`/`0xe72c39`).
  - anything else: assert `refEdge.type == 1` (`0xe72c73`).

  `{anonymous}::Equals(const SegmentAndEntity&, const SegmentAndEntity&)` `0x13bdd10`
  (DustAutomaton.cpp) compares `+0x4c`, byte `+0x50`, `+0x54`, `+0x58`, `+0x5c` when the type
  is 0 (`0x13be2f0..0x13be328`), `+0x60` and byte `+0x64` when it is 1
  (`0x13bdfaa..0x13bdfbf`), and otherwise asserts `a.type == 1` (`0x13be373`). The
  TrackModifier `{anonymous}::Apply(const TrackModifierConfig::EdgeConfig&, SegmentAndEntity&)`
  `0xec7a20` writes the street fields for 0 (`0xec7abc..0xec7ada`, `0xec7b88`) and the track
  fields for 1 (`0xec7b98..0xec7baf`), and otherwise asserts `se.type == 1` (`0xec7c14`).
- **`BaseEdgeType` values.** `con_util_costs::CalcBuildCostEdge` `0x161e5b0` switches on
  `[rbx+0x28]` (`0x161e6a5..0x161e6bc`).
  - 0: uses no type rep.
  - 1: indexes `typeIndex` `[rbx+0x2c]` (`0x161e8f0`) with the `ResTypeRep<BridgeType>::Get`
    checks (`0x161e8f6`, `0x161e917`, `0x161e92d`).
  - 2: indexes with the `ResTypeRep<TunnelType>::Get` checks (`0x161e6ec`, `0x161e70b`,
    `0x161e721`).
  - anything else: `se.comp.type == BaseEdgeType::TUNNEL` (`0x161ea74`).

  `street_util::NeedTunnelEntry` asserts `…type == BaseEdgeType::TUNNEL` after
  `0x2f4b959 cmp dword [rax+0x20], 2` (the raw BaseEdge's type). `MakeStreetProposal` asserts
  `edgeList.edgeType == BaseEdgeType::NORMAL || edgeTypeIndex >= 0` after
  `0x16436b3 mov ecx,[rax]; test ecx,ecx`. So NORMAL = 0 and TUNNEL = 2 by their names, and 1
  is the bridge (the enumerator's name is not in any string).
- **`playerOwned` engaged flag.**
  - `Equals` `0x13bdf7a movzx ecx, byte [r13+0x74]`; `cmp cl, [rax+0x74]`; `jne`;
    `test cl, cl`; `je`; `0x13bdf8c mov ecx, [rax+0x70]`; `cmp [r13+0x70], ecx`. That is
    optional equality: the engaged flags must match, and the payloads are compared only when
    engaged.
  - `Apply` `0xec7ae3..0xec7af9`: assignment writes `+0x70` and sets `+0x74` = 1; reset
    (`0xec7bf0..0xec7bf8`) clears `+0x74`.
  - The ownership invoker `0x2e994a0` (§3) does the same.
- **`objects` element.** `{anonymous}::GetEdgeObjectType(const ecs::Engine*, ecs::Entity,
  ecs::Entity)` `0x2f87a50` fetches the component whose `type_index` is typeinfo `0x5a01bf8`
  (name `N3ecs9component8BaseEdgeE`, relocation at `0x5a01c00`) and walks `objects` at
  component `+0x28/+0x30` (`0x2f87ad2`/`0x2f87ad6`; SegmentAndEntity `+0x30` since `comp` is at
  +0x08). The loop: `0x2f87adf mov eax,[rdx+4]`, `0x2f87ae2 cmp [rdx],ebx`, `0x2f87ae6` /
  `0x2f87af8 add rdx,8`, `0x2f87afc mov eax,[rdx-4]`. It returns `eax` as the
  `EdgeObjectType`. So the element is `{ecs::Entity @+0, EdgeObjectType (4-byte int) @+4}`,
  stride 8.

`EdgeObjectType` values [PROVEN]: enum table `0x59d3fc0` of `{size_t len; const char* name;
int64 value}`: `STOP_LEFT` = 0 (`0x59d3fd0`), `STOP_RIGHT` = 1 (`0x59d3fe8`), `SIGNAL` = 2
(`0x59d4000`); pushed with count 3 at `0x2c3fa3f`.

### `street_util::StreetProposal::EdgeObject` (0x100 B; `edgeObjectsToAdd` element)

| off | field | type | Windows name | status |
|---|---|---|---|---|
| +0x00 | `resultEntity` | `ecs::Entity` (-1 when built by the stop/one-way maker) | "edge entity (-1)" | PROVEN |
| +0x04 | `category` | `EdgeObject::Category`: **0** model has `streetTerminal` metadata without `signal`, **1** `streetTerminal` and `signal`, **2** no `streetTerminal` | "kind" (0 street stop, 2 track object) | rule PROVEN; enumerator names UNPROVEN |
| +0x08 | `segmentEntity` | `ecs::Entity` (collected as a referenced entity when >= 0) | "-1, unused" | PROVEN |
| +0x0c | padding | | | PROVEN (copy ctor skips it) |
| +0x10 | `modelInstance.modelId` | int | modelId | PROVEN |
| +0x14 | `modelInstance.transf` | `CMat4f`, column-major: translation in elements 12..14 = **x +0x44, y +0x48, z +0x4c** | transf, pos +0x44 | PROVEN |
| +0x54 | `modelInstance.transf0` | `std::optional<CMat4f>` | | PROVEN |
| +0x98 | `modelInstance.transformator` | int | | PROVEN |
| +0xa0, +0xb8 | two heap vectors inside `modelInstance` (not registered) | | | PROVEN shape (destructor frees them; copy ctor copies +0xb8 with 4-byte elements `0x164ab1a sar rax, 2`) |
| +0xd0 | `oneWay` | bool | "commit's bool, provisionally oneWay" | PROVEN |
| +0xd1 | `left` | bool | engine `left` | PROVEN |
| +0xd8 | `name` | `std::string` (libstdc++: `p` +0xd8, `len` +0xe0, SSO buffer +0xe8) | name | PROVEN |
| +0xf8 | `playerEntity` | `ecs::Entity` | playerEntity | PROVEN |

Evidence:

- Registration `0x2c40df9..0x2c40eca` (offset slots 0, 4, 8, 0x10, 0xd0, 0xd1, 0xd8, 0xf8,
  paired with `resultEntity`, `category`, `segmentEntity`, `modelInstance`, `oneWay`, `left`,
  `name`, `playerEntity` through the slot pointers listed in §4); ctti types, among them
  `street_util::StreetProposal::EdgeObject::Category street_util::StreetProposal::EdgeObject::*`.
- The StreetProposal destructor loop `0xdc0330..0xdc0378` (stride 0x100, frees `+0xd8` unless
  it points at `+0xe8`, frees `+0xb8`, `+0xa0`).
- `GetReferencedEntities` reads `+0x08` and `+0xf8` (asserted >= 0) in one mode (`0x163e9ca`,
  `0x163e990`) and `+0x00` in the other (`0x163eae5`).
- `ModelInstance` registration `0x210ce20..0x210ce92`: `modelId` → rbp-0x2e0 (`0x210bbe4`) = 0,
  `transf` → rbp-0x2b0 (`0x210c068`, `0x210c0a0`) = 4, `transf0` → rbp-0x2d0 (`0x210bbc4`) =
  0x44, `transformator` → rbp-0x290 (`0x210bf33`) = 0x88; ctti `int`, `CMat4f`,
  `std::optional<CMat4f>`, `int`.
- EdgeObject copy constructor `0x164aa30` (called from sol2 getters `0x2d50cca`, …): it copies
  `+0x00`, `+0x04`, `+0x08`, `+0x10` as dwords (`0x164aa4d..0x164aa95`) and never touches
  `+0x0c`, then `+0xd0`, `+0xd1` bytes (`0x164ab9d`, `0x164abac`), the string
  (`0x164abd3 call 0x163e250`) and `+0xf8` (`0x164abe0`).
- **Translation.** `parcel_util::ParcelFace2BuildingTransf(const std::vector<CVec3f>&)`
  `0x153e250` returns a `CMat4f` through `r12 = rdi`. It writes cos/sin at `+0x00/+0x04`,
  -sin/cos at `+0x10/+0x14`, zero qwords at `+0x08`, `+0x18` and `+0x20`, 1.0 at `+0x28`, and a
  point of the input polygon at `+0x30/+0x34/+0x38`. That point is x/y/z from `[rcx]`,
  `[rcx+4]`, `[rcx+8]` (`0x153e2fa..0x153e312`), optionally the midpoint with the previous
  vertex (factor 0.5 at `0x3e8bdcc`, `0x153e31e..0x153e357`), stored at `0x153e3de..0x153e3ec`.
  1.0 goes to `+0x3c` (`0x153e3f3`). The game's CMat4f therefore keeps the translation in
  elements 12..14 (`+0x30..+0x38`), so an edge object's position is `+0x14+0x30` = `+0x44`.
- **Category.** `0x2f8a2c0` appends an EdgeObject to a `StreetProposal` it returns
  (`0x2f8aad4 mov rsi,[rbx+0xf0]` … `0x2f8acae add qword [rbx+0xf0], 0x100`). It is called by
  the StreetTerminalBuilder (`0xea80ef` in `0xea7380`) and by
  `street_util::MakeOneWayEdgeObjectProposal` `0x2f8afb0` (`0x2f8b251`). It writes:
  - `+0x00` = -1 (`0x2f8a860`, `0x2f8ab09`);
  - `+0x04` = `r13d` (`0x2f8a8bb`, `0x2f8ab0b`/`0x2f8ab1d`), with `r13d = 0x2f86d20(rdi =
    [toolkit+0x38], esi = modelInstance.modelId)` (`0x2f8a70c`, `0x2f8a710`, `0x2f8a76c`,
    `0x2f8a771`);
  - `+0x10` = the same `modelId` (`0x2f8a84a`, `0x2f8a86a`, `0x2f8ab47`).

  `0x2f86d20` returns 1 when `0x2f86bb0` is true, else 0 when `0x2f86ae0` is true, else 2
  (`0x2f86d36..0x2f86d51`). `0x2f86ae0` tests the model's metadata map for the key
  `"streetTerminal"` (built from `0x3f2a91f..0x3f2a92d` at `0x2f86ae8`/`0x2f86b23`).
  `0x2f86bb0` tests `"streetTerminal"` (`0x2f86bdd`) and then `"signal"` (`0x2f86c30`).
  The `r8d` argument is an `EdgeObjectType`: the one-way maker passes
  `GetEdgeObjectType(...)` (`0x2f8b226`, `0x2f8b232`), the StreetTerminalBuilder passes 2 or
  `!left` (`0xea8096..0xea80ab`).

### `ConstructionEntity` (`toAdd` element)

Only what this area uses: stride **0x8f0** [PROVEN: `0xdc30d3`, `0x163ec3a`] and an
`ecs::Entity` at +0x8e0 that `GetReferencedEntities` collects unless it is -1
(`0x163ec4a`) [PROVEN]. The rest belongs to slice-construction.

## 6. Porting the Windows decoders (slice_hook.cpp)

`p` = the factory's `rdx` **at entry, before the trampoline is called** (§1: afterwards it is
empty). Vectors are libstdc++ `{begin, end, cap}`; a count is `(end - begin) / stride` and
must divide exactly.

| Windows function | reads on Linux | changed vs Windows |
|---|---|---|
| `DecodeNodes(base)` | vector at `base+0` (added `p+0x00`, removed `p+0x30`), stride 0x18: x/y/z `+0/+4/+8`, id `+0x14` | no |
| `DecodeEdges(base)` | vector at `base+0x18` (added `p+0x18`, removed `p+0x48`), stride 0x78: node0 `+0x08`, node1 `+0x0c`, t0 `+0x10`, t1 `+0x1c`, btype `+0x28` (0/1/2), bidx `+0x2c` | no |
| `DecodeEdgeType` | first added segment: kind `+0x48` (0 street / 1 track; anything else: refuse), streetType `+0x4c`, hasBus byte `+0x50`, tramTrackType int `+0x54`, trackType `+0x60`, catenary byte `+0x64` | no (bool types and kind values now PROVEN) |
| frozen nodes | `p+0x220` `vector<int>`, stride 4, each an index `< addedNodes count` (refuse otherwise) | Windows `+0x170` |
| ownership | `+0x70` player is valid only when byte `+0x74` != 0 | no |
| `WriteBulldozeInject` (EDEMO) | removedSegments node0/node1/kind; removedNodes id/x/y/z | no |
| `WriteCondemoInject` (CDEMO) | `toRemove` `p+0x288`, stride 4 | Windows `+0x1e0` |
| `LogBulldoze` / `IsUpgradeShape` | `toRemove` `p+0x288` (4), `toAdd` `p+0x2a0` (**0x8f0**), addedSegments `p+0x18` | Windows `+0x1e0`, `+0x1f8`, 0x8e0 |
| `StashStopFromProposal` (STOPX) | edge id = removedSegments[0] `+0x00`; replace test on `edgeObjectsToRemove` `p+0xd0` (below); record = `edgeObjectsToAdd` `p+0xe8` [0]: category `+0x04` (0 or 2 accepted, as Windows), modelId `+0x10`, pos `+0x44/+0x48/+0x4c`, oneWay `+0xd0`, left `+0xd1`, name libstdc++ string `+0xd8`, player `+0xf8` | Windows `+0xe0`, `+0xf8`, MSVC string |
| `ReadObjList` / `StashStopDelFromBulldoze` (STOPXDEL) | `seg+0x30` vector, stride 8, entity `+0x00`, type int `+0x04`; removedSegments[0] vs addedSegments[0] | no |

**Stop replace test: decision.** `edgeObjectsToRemove` holds 4-byte `ecs::Entity`s on Linux
(ctti; stride `0x163e913`). The shipped Windows stash refuses a placement only when
`ReadVec(r8+0xe0, …) >= 0x100`. `ReadVec` returns the byte span (slice_hook.cpp:892), so with
4-byte entities (INFERRED for MSVC) the refusal needs 64 removed objects and never fires for a
real replacement. Windows peers therefore cancel replacing placements and ship them as STOPX,
and the STOPREP path of the Lua poll (stops.lua) is never used for them, although the comment
above the function says otherwise. **The Linux port keeps the shipped behaviour**: it compares
the byte span of `p+0xd0` against 0x100, under one named constant, and logs the removed count.
A Linux originator then behaves exactly like a Windows one in a mixed lobby. Switching both
builds to `begin != end` (the documented intent) is a joint decision for slice-proposal,
lua-linux and the Windows owner (§9).

The Lua lines keep their Windows format: `ARMED`, `STREETP`, `ROADE`, `EDEMO`, `CDEMO`,
`STOPX`, `STOPXDEL` (ROADC, CONXP, CONUP: slice-construction; TERRAINCAP, ASSETCAP:
slice-terrain-assets).

### Implemented cancellation and reference scope

`native/linux/src/slice/slice_proposal.cpp` stages the existing records at the factory
entry and commits them in `SliceArm.prepareCancel`, after the core validates the matching
Add and its completion callback. A refused arm, failed write, superseded command or
blocked Add emits no `NATIVE` or `ARMED 0` record. Outside a multiplayer session the area
returns without capturing. Construction owns `ROADC` together with `CONXP`, so this area
cannot publish a companion for a placement that was not cancelled.

The reference is the pushed Windows release 0.4.22. Its crossing and bridge companion
classification remains in the shared Lua, with the same `ROADE` geometry, removals and
bridge tail. The tests cover those wire records, street properties, stop placement and
removal, demolition, guarded rejection, and cancellation outcomes. No new proposal
payload or extended capture fields are introduced.

Inspector-only node properties, precedence, signal direction, crossing types and bridge
type edits are also uncaptured by the reference Windows hook (its `UNREPLICATED` branch).
Their Linux factory callers are mapped in §3, but enabling a road decoder there would
discard values absent from the existing record. The central multiplayer barrier blocks
those unhandled direct player Adds; they remain a reference coverage limitation.

## 7. DeferHandler routing (BuildProposal branch), Linux keys

The factory detour runs the branch **at entry**, with `p = rdx`, and only then calls the
trampoline (§1). In Windows order, keyed on the factory return address `ret = [rsp] - base`:

1. `ret == 0xe59622` (ProposalAction): terrain / paint / asset brush → slice-terrain-assets.
2. `ret == 0x1971333` (Lua maker): our own replay; never captured. Carrier injection
   (slice-terrain-assets) and `MergeTemplateStreet` (slice-construction) run here, before
   the trampoline.
3. `ret == 0xdd4e99` (Bulldozer::Apply): classify by shape — toRemove+toAdd → CONUP stash
   (slice-construction); toRemove → CDEMO; removed+added segments → STOPXDEL; removals →
   EDEMO. Arm the cancel only when something shipped.
4. `ret == 0xe34860` (ConstructionBuilder): ROADC + CONXP → slice-construction.
5. `ret == 0xeaaf79` (StreetTerminalBuilder): STOPX.
6. `ret == 0xed6da9` (TrackModifier::Build): the upgrade path of ROADE (removals travel).
7. `ret == 0xe86452` (StreetBuilder::UpdateEngine): ROADE.
8. anything else: construction-upgrade shape test (`toRemove >= 1 && toAdd >= 1`, e.g.
   ModuleBuilder `0xe4f6bd`), else log "UNREPLICATED" with the four street counts and `ret`
   (the §3 table names the ten known unrouted rets).

The pending cancel is identified by pointer: record the factory's `rdi`; the Add call that
follows passes the same pointer in `rdx` at every UI caller (§2, §3). A skipped Add must zero
its 8-byte out handle (§2).

## 8. Traps

- The factory **moves** the proposal and the Context. Decode, merge and inject before calling
  the trampoline; never read `rdx` or `rcx` after it.
- Buffers put into the caller's proposal end up owned and freed by the game: allocate them
  with the game's `operator new` (PLT `0x6dbce0`).
- The Command's Context copy has `+0x09` forced to 0 (`0x15ef260`).
- At factory entry the Command is not built yet. After the factory, its payload tag is
  `*(uint8_t*)(*(void**)cmd + 0xd48)` = 15 (§1).
- `hasBus` and `catenary` are bools: the bytes after them are padding, not fields.
- `SegmentAndEntity+0x04` and `EdgeObject+0x0c` are padding.
- `playerOwned.player` (+0x70) is garbage unless `+0x74` is set.
- The two edge-object entity fields are `resultEntity` (+0x00) and `segmentEntity` (+0x08);
  the Windows names ("edge entity", "unused") were guesses from samples that held -1 in both.
- Nothing in this document has been measured in the running Linux game; all of it is
  static.

## 9. Open questions

- `SegmentAndEntity` +0x68..+0x6f (Windows: owning construction) — not registered on Linux
  and not compared by `Equals`.
- `NodeAndEntity` +0x0d..+0x0f (where Windows wrote `flags` 0x7f00).
- StreetProposal's two unregistered hash maps (+0x60, +0x98) and the Proposal map at +0x308.
- Context +0x04, +0x09, +0x10, +0x50 meanings.
- `TramTrackType` values (Windows measurements only; the native code copies the int and does
  not interpret it).
- `EdgeObject::Category` enumerator names (the numeric rule is PROVEN; category 1 has not
  been seen in a sample).
- The terrain grids' types and meanings (slice-terrain-assets).
- The unrouted UI proposals (§3: ownership, traffic lights, precedence, one-way signals,
  crossing, bridge and tunnel types, module tabs, generate menu, `0x145e380`): replicate them
  or keep logging them as UNREPLICATED, on both builds.
- STOPX replace test: keep the shipped Windows threshold (current Linux choice) or move both
  builds to `begin != end` together with lua-linux's STOPREP path.
