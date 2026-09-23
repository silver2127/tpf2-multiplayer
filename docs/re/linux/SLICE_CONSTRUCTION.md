# Slice, construction area: the Linux map

Linux RE for the slice's construction channel:

- the ConstructionEntity decode off the factory's Proposal (`StashConxpFromProposal`, the `lua::Table`
  walker `SerLuaTable`/`SerLuaValue`, `ReadSsoString`);
- the construction branches of `DeferHandler` (UI placement, the Lua replay, the upgrade/module
  shape);
- `MergeTemplateStreet` (welding the template connector of a replayed construction);
- the CONXP / CONUP writers (and the ROADC companion the placement branch writes).

It replaces `native/src/slice_hook.cpp` ~1731-1964, ~2120-2185, ~2240-2475 and the construction
branches of `DeferHandler` (~3683-3712, ~3753-3820, ~3864-3884) for the Linux build. Code goes in
`native/linux/src/slice/slice_construction.{h,cpp}`. The Lua side (`res/scripts/mp/cons.lua`,
`conx.lua`, `inject.lua`) is unchanged by this map.

Binary: `TransportFever2`, Steam build 35924, GNU build-id `3a0e156390b0e6f1e372051c24802c8493ae454a`.
Addresses are Linux RVAs (the PIE links at 0; live = image base + RVA). "Windows" means the RVAs in
`docs/re/*.md` and `slice_hook.cpp`, relative to `0x140000000`.

Status labels, used in every table and in the claim ids `SC-*`:

- **PROVEN**: the instructions, strings or data quoted here show it, and it can be re-derived from
  the binary.
- **INFERRED**: placed by the Windows docs/measurements, by address order, by elimination or by a scan
  whose completeness is not proven. It stays out of code that edits game memory or cancels a player's
  command unless the code verifies it at run time.
- **UNPROVEN**: not established. The feature that depends on it stays OFF (section 10.1).

Nothing here was checked in the running game (static MAP stage).

**Implementation addendum (2026-09-14).**
[CONSTRUCTION_TERRAIN_IMPLEMENTATION.md](../../linux/CONSTRUCTION_TERRAIN_IMPLEMENTATION.md)
corrects the UI Construction address to `tool+0x520` and proves its `.con` params
flow. Linux now uses the unchanged Windows 0.4.22 ROADC/CONXP/CONUP formats,
including connected placement and module/bulldozer upgrades. The template weld
uses the Windows geometry predicates with Linux bool/container ownership fixes.
The original map's OFF gates below are historical research notes, superseded by
that implementation addendum; unproven field meanings are not used as offsets.

**Revision 1 (after independent verification).** Corrected: the factory takes the Proposal and the
Context **by value** and consumes both (SC-BP-ARGS, SC-BP-ENTRY-EDIT, SC-CALLER-UI,
SC-CALLER-LUA-COPY). The completion `std::function` handed to Add can be **empty** (SC-ADD-PAIRING).
Newly proven: the identity of `CommandList::Add` (SC-ADD-ID); what `Construction+0` holds
(SC-CE-PARAMS, now scoped); the empty snap map on the script path (SC-TEMPLATE-SCRIPT); and the
proposal shape produced by `CreateProposalReplace` (SC-SHAPE-REPLACE). The UI weld shape and the
blanket shape table are now UNPROVEN (SC-TEMPLATE-SHAPE, SC-SHAPES), and so is the thread of the Lua
maker (SC-THREAD-LUA).

## How the evidence was produced

- Disassembly: capstone (x86-64) linear sweep from each function start over its FDE size from
  `/home/topsnek/tpf2-re/linux/functions.csv`; RIP-relative loads resolved to `.rodata` strings,
  `.plt` imports (`.rela.plt`) and exported `.dynsym` names.
- References: every `e8`/`e9` rel32, every RIP-relative `lea` and every `R_X86_64_RELATIVE` addend in
  the file whose target is the function.
- Type names: `R_X86_64_RELATIVE` relocations at typeinfo+8 give the typeinfo name string
  (`N3ecs9component9TickEpochE` etc.).
- Member offsets: the game's own Lua usertype registrations. The template argument list in the
  `__PRETTY_FUNCTION__` of each `usertype_metatable<T, …>` (from `funcsig.csv`) gives the ordered
  `(name length, member type)` pairs; the registration function stores the member-pointer values as
  immediates. A pairing is accepted only when the types agree with the struct's constructor,
  destructor or copy constructor.
- Layouts cross-checked against the Proposal/StreetProposal/ConstructionEntity destructors and copy
  constructors and against the game's builders `construction_builder_util::MakeProposalAdd`,
  `{anonymous}::MakeStreetProposal`, `CreateProposalReplace` and `street_util::MakeSegmentAndEntity`
  (all named by their own assert signatures).
- Throwaway scripts (`re_common.py`, `tdis.py`, `sites.py` for the call-site windows, `addfn.py` for the
  std::function slots, `addid*.py` for the Add identity, `fn298.py` for the ConstructionDesc function
  installers, `pflow.py` for the params flow) are in this area's scratch directory.

## 0. What this area hooks: nothing

This area installs no hook and patches no code. It reads and edits the Proposal handed to it by the
BuildProposal factory hook, and it is told at the CommandList::Add hook whether the cancel landed.
Both hooks belong to slice-core. The facts this area relies on:

### 0.1 `make_cmd::BuildProposal` `0x15ee930` (Windows `0x9dc750`)

| item | value | evidence | status |
|---|---|---|---|
| identity | `Command make_cmd::BuildProposal(const ecs::Engine&, construction_builder_util::Proposal, construction_builder_util::Context, bool, bool ignoreErrors)`. Proposal and Context arrive **by value** (or as `&&`; the ABI is the same). The two bool names come from Windows | in the `make_command.cpp` block; writes CmdData variant tag **15** (the Windows dispatch tag of BuildProposal) at `0x15ef2ab c6 45 b8 0f mov byte [rbp-0x48],0xf` before packaging (`0x15ef2af call 0x15ebab0`); its callers are exactly the proposal tools (section 1) and the Lua `buildProposal` lambda, which passes a `scripting::Convert` result | PROVEN (SC-BP-ID); bool names INFERRED |
| FDE size | 2674 | functions.csv | PROVEN |
| first 36 bytes | `f3 0f 1e fa 55 48 89 e5 41 57 41 56 41 55 41 54 53 48 81 ec 00 10 00 00 48 83 0c 24 00 48 81 ec e8 0a 00 00` | file bytes | PROVEN |
| steal | **14** = `endbr64`(4) `push rbp`(1) `mov rbp,rsp`(3) `push r15`(2) `push r14`(2) `push r13`(2); `PrologueSteal(code,14)` returns 14. Byte 14 starts `push r12`; byte 17 is the stack probe `sub rsp,0x1000` | decode | PROVEN (SC-BP-PROLOGUE) |
| rdi | hidden `Command*` return slot | `0x15ee95b mov r12,rdi`; returned `0x15ef2e2 mov rax,r12` | PROVEN |
| rsi | `const ecs::Engine&` | `0x15ee972 mov r13,rsi` → packager `0x15ef2a2 mov rdx,r13` | PROVEN |
| **rdx** | **pointer to a caller-owned Proposal temporary** (by value; Windows r8). The callee **moves out of it** | `0x15ee964 mov [rbp-0x1b10],rdx`; the only reload `0x15ef154 mov rdx,[rbp-0x1b10]` → `0x15ef171 mov rsi,rdx` → `0x15ef17f call 0xde0430`. `0xde0430` is Proposal **move-assignment**: `0xde0440 mov r12,rsi` (source); for `+0x00` it takes `[rsi]`, `[rsi+8]`, `[rsi+0x10]` and zeroes the source (`0xde0474..0xde049d`, `0xde047e mov qword [rsi],0`), and the same for `+0x18` (`0xde04d9 mov qword [r12+0x18],0`), `+0x30` (`0xde055e`), `+0x48` (`0xde05be`), … to `+0x398..+0x3b8` (`0xde1333..0xde1363`). Every caller copy-constructs the temporary right before the call and destroys it after (section 1.1) | PROVEN (SC-BP-ARGS) |
| **rcx** | **pointer to a caller-owned Context temporary** (by value; Windows r9; 0x68 B). The callee **moves the shared_ptr at +0x58/+0x60 out of it** | `0x15ee95e mov r15,rcx` (r15 is next written at `0x15ef204`); reads `0x15ef184 movzx eax,word [r15]` … `0x15ef1ca mov eax,[r15+0x14]`, `0x15ef1d9 movzx eax,word [r15+0x50]`; move-out `0x15ef1de mov rdx,[r15+0x60]` / `0x15ef1e2 mov qword [r15+0x60],0` / `0x15ef1f8 mov rax,[r15+0x58]` / `0x15ef1fc mov qword [r15+0x58],0`, stored at `[rbp-0x16c8]`/`[rbp-0x16c0]`, old control block released (`0x15ef231 lock xadd`). Context copy ctor `0xe39c00` (UI `0xe34822`, TrackModifier `0xed6d5a`); ~Context `0xdd8820` releases the control block at `+0x60` (`0xdd8834`) and destroys `+0x18` (`0xdd8868 jmp 0xdd87e0`); UI destroys its temporary at `0xe348b8`, the Lua wrapper releases `[rbp-0x25e0]` (= its Context `rbp-0x2640` + 0x60) at `0x1971359`. Size: the Lua wrapper value-initialises exactly 13 qwords at `rbp-0x2640` (`0x197129c lea rdi,[rbp-0x2640]`, `0x19712a3 mov ecx,0xd`, `0x19712be rep stosq`) | PROVEN (SC-BP-ARGS, SC-BP-CONTEXT) |
| r8b / r9b | bool / bool ignoreErrors | `0x15ee954 mov [rbp-0x1b04],r8d`, `0x15ee961 mov r14d,r9d`; stored side by side `0x15ef24f` / `0x15ef259`. The Lua lambda `(sol::table, Proposal, optional<Context>, bool)` passes r8d = 1 (`0x1971325`) and its only bool in r9d (`0x1971315`, loaded from `[rbp-0x26cc]`) | PROVEN |
| edit at entry | an in-place edit of `*rdx` at function entry is what the Command carries: the path from entry to `0x15ef17f` has no branch (first jcc `0x15ef21c`), the saved pointer is reloaded only at `0x15ef154`, and the vectors are **transferred by pointer**: first into the payload `rbp-0x1ae0` (`0xde0430`), then into the CmdData (`0x15ef29a call 0x15f39d0` → `0x15f39e1 call 0xe39fc0`, a Proposal move constructor that zeroes its source, `0xe39ffd mov qword [rsi-0x60],0` …), then packaged (`0x15ebab0`) | | PROVEN (SC-BP-ENTRY-EDIT) |
| after return | `*rdx` is an **empty, moved-from** Proposal and Context `+0x58/+0x60` are null. Nothing may read either through a saved pointer (for example from the Add hook) | rows above | PROVEN (SC-BP-CONSUMED) |
| references | **18** direct `call`s, no `jmp`, no RIP-relative `lea`, no relocation | full-file scan | PROVEN (SC-BP-CALLERS) |

### 0.2 `CommandList::Add` `0x15da840` (Windows `0x9d2a00`)

94 direct calls. First bytes `f3 0f 1e fa 55 48 89 e5 41 57 41 56 41 55 41 54 53 48 89 cb`.

**Identity (SC-ADD-ID, PROVEN).** Its funcsig row is an inlined boost name, so the identity comes
from the lambda it builds:

- `0x15dab9e lea rax,[rip+0x43e3a4b]` = `0x59be5f0`, stored into `[rbp-0x100]` (`0x15dabae`).
  `0x59be5f0`/`0x59be5f8` are `R_X86_64_RELATIVE` slots holding `0x15da710` and `0x15da310`, a
  `boost::function` vtable `{manager, invoker}`.
- Manager `0x15da710`: op 4 (get type) returns typeinfo `0x5a24180` (`0x15da726`). Op 3 (check type)
  compares against the name `*ZN11CommandList3AddE7CommandSt8functionIFvRKS0_EESt8weak_ptrI11CmdProgressEEUlRKN5boost8signals210connectionERKSt6vectorIS0_SaIS0_EEE_`
  (`0x15da768 lea rsi,[rip+0x2d49571]` → `0x4323ce0`, then `strcmp`).
- Invoker `0x15da310` holds the assert signature
  `CommandList::Add(Command, std::function<void(const Command&)>, std::weak_ptr<CmdProgress>)::<lambda(const boost::signals2::connection&, const std::vector<Command>&)>`
  (`0x15da66c`), `src/Game/command/CommandList.cpp` (`0x15da678`) and
  `idx >= 0 && idx < (int)commands.size()`.

A closure type belongs to the function that defines the lambda, so `0x15da840` is the body of
`CommandList::Add` (or an out-of-line copy with Add inlined, which has the same ABI).

**Arguments (SC-ADD-ARGS, PROVEN from the signature and the SysV rules; call shape confirmed at the six
sites below).** `rdi` = hidden return slot (destroyed by `0x3190430` right after every site listed in
section 1.1; `0x3190430` is `~Connection` in the menu map, so the return type
`boost::signals2::connection` is INFERRED). `rsi` = `this` (the CommandList). `rdx` = `Command*` (by
value, a pointer to the caller's object, which is the factory's result). `rcx` =
`std::function<void(const Command&)>*` (by value, a pointer to the caller's temporary). `r8` =
`std::weak_ptr<CmdProgress>*` (by value; the construction tools pass a zeroed pair, an empty weak_ptr).

**Pairing (SC-ADD-PAIRING, PROVEN at all six construction-relevant tool sites).** The register loaded
into `rdi` before the factory call is the register loaded into `rdx` before the next `0x15da840` call.
Between the two there is no call, no branch, no branch target and no write to that register.

| site | factory `rdi` ← | Add `rdx` ← | Add `rcx` ← | `_M_manager` [+0x10] | `_M_invoker` [+0x18] |
|---|---|---|---|---|---|
| ConstructionBuilder::MousePressed | `0xe34858 mov rdi,r12` | `0xe3486e mov rdx,r12` | `0xe34871 lea rcx,[rbp-0xf0]` | `0xe27060` (`0xe347e0` lea, `0xe347e7` store) | `0xe341a0` (`0xe347bc`, `0xe347d9`) |
| StreetBuilder::UpdateEngine | `0xe8644a mov rdi,r15` | `0xe86460 mov rdx,r15` | `0xe86463 lea rcx,[rbp-0xf0]` | `0xe6eda0` (`0xe86328`, `0xe86346`) | `0xe867d0` (`0xe86309`, `0xe86321`) |
| ModuleBuilder::MousePressed | `0xe4f6b5 mov rdi,r15` | `0xe4f6cb mov rdx,r15` | `0xe4f6ce lea rcx,[rbp-0x5c0]` | `0xe4d3f0` (`0xe4f569`, `0xe4f573`) | `0xe4c600` (`0xe4f554`, `0xe4f562`) |
| AddModuleComp lambda `0xf225c0` | `0xf2299d mov rdi,r15` | `0xf229b7 mov rdx,r15` | `0xf229b3 lea rcx,[rbp-0x60]` | **0**: `0xf228ae mov qword [rbp-0x50],0` is its only write | **never written** (no access to `[rbp-0x48]`) |
| Bulldozer::Apply | `0xdd4e91 mov rdi,rbx` | `0xdd4ea7 mov rdx,rbx` | `0xdd4eb8 mov rcx,r14` (`0xdd4ea0 lea r14,[rbp-0xf0]`) | `0xdd1270` (`0xdd4907`, `0xdd490e`) | `0xdd62a0` (`0xdd48f1`, `0xdd4900`) |
| TrackModifier::Build | `0xed6da1 mov rdi,r15` | `0xed6dcb mov rdx,r15` | `0xed6dc8 mov rcx,r13` (`0xed6db0 lea r13,[rbp-0xf0]`) | `0xec62c0` (`0xed6d22`, `0xed6d4c`) | `0xed6960` (`0xed6d09`, `0xed6d1b`) |

The AddModuleComp slot scan covered every instruction in `0xf225c0` with an `rbp` displacement in
`[-0x60,-0x40)`. It found `0xf228ae` (the zeroing), `0xf229b3` (the lea) and the read after Add at
`0xf22a23`, nothing else.

So the Windows rule `Add.r8 == factory.rcx` becomes **`Add.rdx == factory.rdi`**. The completion
function **may be empty**: the Add hook must test `_M_manager` `[rcx+0x10] != 0` before calling
`_M_invoker` `[rcx+0x18]`. An empty function means no completion callback for that caller (fire
nothing, like Windows `g_pendingNoCb = 1`). The invoker's signature is `void(const _Any_data&, const
Command&)`: `rdi = rcx`, `rsi = &Command`. After Add every tool calls `0x15d8f30` (~Command) on the
Command and `0xdc3040` (~Proposal) on its Proposal temporary. The tools with a non-empty function then
run manager op 3 (for example `0xe348d0 mov edx,3`).

Not paired: the two `CGameUI::CreateConstructionMenu` factory calls (returns `0x1045737`,
`0x1045a47`) hand the Command to `0x10526d0`, not to `0x15da840` (`0x1045741`, `0x1045a51`).

### 0.3 What the two hooks must honour (consequences of 0.1 and 0.2)

1. Everything that reads or edits the Proposal (the ROADC decode, the CONXP/CONUP stash, dumpprop,
   `MergeTemplateStreet`) runs **at factory entry, before the trampoline is called**. After the
   trampoline `*rdx` is empty.
2. Do not assume `*rcx` is unchanged after the trampoline; do not read Context `+0x58..+0x67` then.
3. Records dropped by pulling a vector's `end` back ride into the Command's payload (the begin/end/cap
   triple moves by pointer). They are leaked when the Command dies, never destroyed twice.
4. The Add hook must not read the Proposal through a pointer saved at the factory. It uses only what
   the factory hook stashed.

## 1. Caller sites the construction branches classify by

The factory hook sees the return address `[rsp]` at entry. The Linux constants:

| role in `DeferHandler` | Windows return | Linux call → **return** | containing function | evidence | status |
|---|---|---|---|---|---|
| UI construction placement (ROADC + CONXP) | `0x419f62` | `0xe3485b` (`e8 d0 a0 7b 00`) → **`0xe34860`** | `0xe343c0` `void UI::ConstructionBuilder::MousePressed(UI::CRendererComponent*, bool)` (own assert sig at `0xe34a39`) | rdi = r12 (`rbp-0x760`), rsi = `[rbp-0x5a8]`. **rdx = r15 = `rbp-0x4b0`** (`0xe3463f lea r15,[rbp-0x4b0]`, its only write): a **copy** of the tool's `m_proposal` `*[rbx+0x850]` made by `0xe34831 call 0xe3e060` (`0xe34827 mov rsi,[rbx+0x850]`, `0xe3482e mov rdi,r15`) and destroyed at `0xe348ac call 0xdc3040`. rcx = `[rbp-0x788]`, a Context copy (`0xe34822 call 0xe39c00`, source `[rbp-0x790]`) destroyed at `0xe348b8`. r8d = `byte [rbx+0x912]` (`0xe347ee`), r9d = 0 (`0xe3483d`). Before the copy the tool asserts `m_proposal` (`0xe345ab je 0xe34a39`) and `!m_proposal->toAdd.empty()` (`0xe345b1..0xe345bf je 0xe34a5d`). Add `0xe34890` → `0xe34895` (Windows `0x419f81`) | PROVEN (SC-CALLER-UI) |
| Lua replay (`MergeTemplateStreet`) | `0xced378` | `0x197132e` (`e8 fd d5 c7 ff`) → **`0x1971333`** | `0x1970a10`, the body of the `api.cmd.make.buildProposal` lambda | the registration `scripting::SetupCommandInterface` `0x19638f0` loads `'buildProposal'` (`0x1963cbe`) and at `0x1963ce0` calls `user_allocate` `0x19604b0`, whose funcsig names `functor_function<SetupCommandInterface(…)::<lambda(sol::table, scripting::Proposal, std::optional<construction_builder_util::Context>, bool, sol::this_state)>>`; the functor of that same lambda type (`0x1972920`, funcsig) tail-jumps `0x19729b4 e9 57 e0 ff ff jmp 0x1970a10`. In `0x1970a10`: `0x1971288 call 0x2e237b0` = `scripting::Convert` (assert signature `construction_builder_util::Proposal scripting::Convert(const street_util::StreetToolkit&, …)` at `0x2e25578`) into `rbp-0x2260`; `0x1971310 call 0xe3e060` copies it into `rbp-0x1ea0`; **rdx = that copy** (`0x197131f`); rcx = `rbp-0x2640` Context (built in place, see 0.1); r8d = 1; r9d = `[rbp-0x26cc]` | PROVEN (SC-CALLER-LUA) |
| script filter for this factory | block `0xcec000..0xcf2000` | **only `0x1971333`** | | SC-BP-CALLERS: the 18 call sites are listed in 1.1. Only `0x197132e` is reached from `SetupCommandInterface` (the Lua maker); the five unnamed ones are placed in UI code by address only (INFERRED), which is all the filter needs | PROVEN (the list); UI placement of the unnamed five INFERRED |
| dumpprop of the street tool | `0x459eb7` (an Add return, so it never matched at the factory) | `0xe8644d` → **`0xe86452`** (Add `0xe86482` → `0xe86487`) | `0xe861b0` `void UI::StreetBuilder::UpdateEngine()` (funcsig) | call list of `0xe861b0` | PROVEN |
| module add/remove, construction window (upgrade shape → CONUP) | `0x42bc7b` | `0xe4f6b8` → **`0xe4f6bd`** (Add `0xe4f6ed` → `0xe4f6f2`) | `0xe4eb30` `void UI::ModuleBuilder::MousePressed(UI::CRendererComponent*, bool)` (funcsig) | rdx = r13 (`0xe4f6a5`), a copy of `m_proposal` `*[rbx+0xa8]` (`0xe4f685 mov rsi,[rbx+0xa8]`, `0xe4f68f call 0xe3e060`), destroyed `0xe4f709` | PROVEN (SC-CALLER-TOOLS) |
| module list of the construction window (upgrade shape → CONUP) | `0x4b71b8` (predicted) | `0xf229a0` → **`0xf229a5`** (Add `0xf229d2` → `0xf229d7`) | `0xf225c0`, a lambda body; its only reference is the invoker thunk `0xf22c50` (`mov rdi,[rdi]; jmp 0xf225c0`), whose address is taken in `void UI::AddModuleComp::UpdateTabs()` (`0xf1c4d1 lea rax,[rip+0x6778]`) | rdx = r14 = `rbp-0x420` (`0xf2298d`), a copy (`0xf2297b call 0xe3e060`, source `[rbp-0xae8]` = `rbp-0x7e0`, the `CreateProposalReplace` result of `0xf22786`, section 6.2), destroyed `0xf229ee`. **Its completion std::function is empty** (0.2) | PROVEN (SC-CALLER-TOOLS) |
| bulldozer (module removal → CONUP stash in `LogBulldoze`) | `0x3eb227` | `0xdd4e94` → **`0xdd4e99`** (Add `0xdd4ebe` → `0xdd4ec3`) | `0xdd4590` `void UI::Bulldozer::Apply(UI::BulldozerAction*, const UI::SelectionInfo&, const UI::BulldozerAction::Result&)` (funcsig) | rdx = r13 = `rbp-0x4b0` (`0xdd48c5`, `0xdd4e81`), filled by an inlined Proposal copy (`0xdd4a19 call 0xdef0f0` StreetProposal copy into r13, then the tail vectors `+0x270`, `+0x288`, `+0x2f0`, `+0x308`, `+0x348`), destroyed `0xdd4ed6` | PROVEN (SC-CALLER-TOOLS) |
| (not this area) street/track upgrade | `0x4790fc` | `0xed6da4` → `0xed6da9` | `0xed69d0` `void UI::TrackModifier::Build()` | | PROVEN |

The Windows code sends any other non-script caller whose proposal has the upgrade shape to the CONUP
path. On Linux that path stays off (section 10.1). The two module-edit callers above are listed so the
log can name them.

**The Lua carrier is a game-owned temporary that the factory consumes** (SC-CALLER-LUA-COPY, PROVEN).
`0x1971310` copies the Convert result into `rbp-0x1ea0`; the factory moves its vectors into the
Command (0.1); `0x1971344` hands the Command to Lua, `0x197134c` destroys the Command, and `0x1971354`
destroys the now-empty temporary with `call 0xdc3040`. Records dropped by moving a vector's end pointer
therefore ride into the Command's payload. They are leaked when the Command dies, never destroyed twice,
which is the property the Windows merge relied on.

### 1.1 Every factory caller passes a fresh copy

| call | containing function | Proposal temporary (rdx) ← copy | destroyed |
|---|---|---|---|
| `0xdd4e94` | Bulldozer::Apply | r13 = `rbp-0x4b0`, inlined copy from `0xdd4a19` | `0xdd4ed6` |
| `0xe3485b` | ConstructionBuilder::MousePressed | r15, `0xe34831` | `0xe348ac` |
| `0xe4f6b8` | ModuleBuilder::MousePressed | r13, `0xe4f68f` | `0xe4f709` |
| `0xe5961d` | `0xe59490` | r14 = `rbp-0x420` (`0xe59512`, `0xe59547 mov rdi,r14`), `0xe595f3` | `0xe5966e` |
| `0xe8644d` | StreetBuilder::UpdateEngine | `rbp-0x4b0` (`0xe86365`, `0xe8641c`), `0xe86423` | `0xe864a2` |
| `0xeaaf74` | `0xeaabb0` | r12, `0xeaaf4b` | `0xeaafcd` |
| `0xed6da4` | TrackModifier::Build | `rbp-0x4b0` (`0xed6d66`, `[rbp-0x848]`), `0xed6d7b` | loaded at `0xed6df1` |
| `0xf229a0` | AddModuleComp lambda | r14 = `rbp-0x420`, `0xf2297b` | `0xf229ee` |
| `0x1045732` / `0x1045a42` | CreateConstructionMenu lambda | rbx, `0x1045706` / `0x1045a0f` | `0x1045751` / `0x1045a61` |
| `0x12409dd` / `0x1240e94` / `0x12413fd` | `0x1240730` / `0x1240bf0` / `0x12410b0` | r12 `0x12409ad` / rbx `0x1240e64` / `[rbp-0x14a0]` `0x12413d7` | `0x1240a36` / `0x1240eed` / `0x1241452` |
| `0x145e1a7` / `0x145e540` / `0x145ef7d` / `0x145fbc0` | CreateSignalView lambda / `0x145e380` / `0x145ece0` / `0x145f8c0` | r12 `0x145e185` / r14 `0x145e519` / r14 `0x145ef57` / r15 `0x145fb8c` | loaded `0x145e1f5` / `0x145e58e` / loaded `0x145efcb` / `0x145fc0e` |
| `0x197132e` | Lua buildProposal wrapper | rbx = `rbp-0x1ea0`, `0x1971310` | `0x1971354` |

`0xe3e060` is the Proposal copy constructor: `0xe3e070 mov r12,rsi` (source), `0xe3e074 mov rbx,rdi`
(destination), `0xe3e08a call 0xdef0f0` (StreetProposal copy), the 4-byte `toRemove` loop `0xe3e1b3`,
and the `toAdd` CE copy `0xe3e286` with stride 0x8f0 (`0xe3e28b`). It writes only its destination.

## 2. `construction_builder_util::Proposal`: the members this area touches (0x3c0 B)

The full layout, including the grids, is in `SLICE_TERRAIN_ASSETS.md` section 1. It agrees with this
table.

| Linux | Windows | member | element | evidence | status |
|---|---|---|---|---|---|
| +0x000 | +0x000 | `proposal.addedNodes` | `CompAndEntity<BaseNode>`, 0x18 B | usertype `StreetProposal` "addedNodes" = 0 (`0x2c40bbf`); stride 0x18 in the StreetProposal copy ctor `0xdef0f4 movabs rdx,0xaaaaaaaaaaaaaaab`, `0xdef194 add rcx,0x18` | PROVEN |
| +0x018 | +0x018 | `proposal.addedSegments` | `SegmentAndEntity`, 0x78 B | usertype = 0x18 (`0x2c40bb4`); ~StreetProposal `0xdc0403..0xdc041f` (`add r12,0x78`, frees `[r12+0x30]`) | PROVEN |
| +0x030 | +0x030 | `proposal.removedNodes` | 0x18 B | usertype = 0x30 (`0x2c40ba9`) | PROVEN |
| +0x048 | +0x048 | `proposal.removedSegments` | 0x78 B | usertype = 0x48 (`0x2c40b9e`); ~StreetProposal `0xdc03b4..0xdc03d7` | PROVEN |
| +0x0d0 | +0x0e0 | `proposal.edgeObjectsToRemove` | `Entity` | usertype = 0xd0 (`0x2c40b93`) | PROVEN |
| +0x0e8 | +0x0f8 | `proposal.edgeObjectsToAdd` | `EdgeObject`, 0x100 B | usertype = 0xe8 (`0x2c40b88`) | PROVEN |
| +0x100..+0x220 | +0x110..+0x170 | new2old/old2new Nodes/Segments/EdgeObjects (6 × `std::map<Entity, std::set<Entity>>`, 48 B) | | usertype 0x100, 0x130, 0x160, 0x190, 0x1c0, 0x1f0 (`0x2c40b7d..0x2c40b46`) | PROVEN |
| **+0x220** | **+0x170** | **`proposal.frozenNodes`** `vector<int>`, indices into addedNodes | 4 B | usertype "frozenNodes" = 0x220 (`0x2c40b3b`), type `std::vector<int>` (funcsig of the StreetProposal metatable); ~StreetProposal `0xdc0294 mov rdi,[rdi+0x220]`; filled with node indices, section 6 | PROVEN (SC-PROP-STREET, SC-PROP-FROZEN) |
| +0x238 | +0x188 | `terrainAlignSkipEdges` `unordered_set<int>` (56 B) | | usertype = 0x238 (`0x2c41164`); name + type in the `usertype_metatable<construction_builder_util::Proposal, …>` funcsig | PROVEN |
| **+0x270** | **+0x1c8** | **`segmentTags`** `vector<std::string>` | 32 B libstdc++ strings | usertype = 0x270 (`0x2c41159`); ~Proposal `0xdc3109 lea rdi,[rbx+0x270]; call 0x9b7f60` (exported `vector<string>` dtor) | PROVEN |
| **+0x288** | **+0x1e0** | **`toRemove`** `vector<ecs::Entity>` | 4 B | usertype = 0x288 (`0x2c4114e`); copy ctor 4-byte loop `0xe3e1b3 mov edi,[rsi+rcx*4]`; ~Proposal `0xdc30f8` | PROVEN |
| **+0x2a0** | **+0x1f8** | **`toAdd`** `vector<ConstructionEntity>` | **0x8f0 B** | usertype = 0x2a0 (`0x2c41143`); ~Proposal `0xdc30b7..0xdc30f3` (`add r12,0x8f0`, `call 0xdc2a00`); copy ctor `0xe3e286 call 0xe3db90` stride 0x8f0; MakeProposalAdd emplaces at `0x1646a17 lea rdi,[r14+0x2a0]` | PROVEN (SC-PROP-TAIL) |
| +0x2b8 | +0x210 | `old2new` `unordered_map<Entity,int>` | | usertype = 0x2b8 (`0x2c41138`) | PROVEN |
| +0x2f0 | +0x250 | `parcelsToRemove` `vector<Entity>` | | usertype = 0x2f0 (`0x2c4112d`) | PROVEN |
| sizeof 0x3c0 | 0x2f8 | | | SLICE_TERRAIN_ASSETS.md (`0xe58283 mov edi,0x3c0`); the move-assign `0xde0430` ends at `+0x3b8` (`0xde1363`) | PROVEN |

MergeTemplateStreet also has to know that `segmentTags` runs parallel to `addedSegments`.
`MakeStreetProposal` compares `segmentTags.size()` (`[r+8]-[r] >> 5`, `0x16422a5..0x16422ba`) with
`addedSegments.size()` (`÷120`, `0x164228e..0x16422a1`) and branches on above/below
(`0x16422c1`, `0x16422c7`). The branch targets were not traced, so this is **INFERRED**
(SC-PROP-TAGS, consistent with the Windows measurement).

## 3. The street records a construction proposal carries

### 3.1 `NodeAndEntity` = `street_util::CompAndEntity<ecs::component::BaseNode>` (0x18 B)

| offset | field | type | evidence | Windows doc |
|---|---|---|---|---|
| +0x00 | comp.position | `CVec3f` | usertype `BaseNode` "position" = 0 (`0x2146c9c`) | x,y,z |
| **+0x0c** | comp.**doubleSlipSwitch** | **`bool` (1 B)**; +0x0d..+0x0f padding | usertype = 0xc (`0x2146c91`), type `bool BaseNode::*` (funcsig `0x2140120`) | "flags u32, 0x7f00 on template nodes" |
| +0x10 | comp.trafficLightPreference | enum (int) | usertype = 0x10 (`0x2146c86`) | "type i32, 2 in every sample" |
| +0x14 | entity (placeholder or real id) | `ecs::Entity` | usertype `NodeAndEntity` "entity" = 0x14 (`0x2c40ff3`), "comp" = 0 | id |

All PROVEN (SC-NODE). **The Windows "flags 0x7f00" is `doubleSlipSwitch = false` plus the byte 0x7f
in padding.** Only the byte at +0x0c means anything.

### 3.2 `street_util::SegmentAndEntity` (0x78 B)

| offset | field | type | evidence |
|---|---|---|---|
| +0x00 | entity (placeholder or real id) | `Entity` | usertype "entity" = 0 (`0x2c4108a`); MakeSegmentAndEntity `0x2f74109 mov [rbx],eax` |
| +0x08 | comp.node0 | `Entity` | usertype "comp" = 8 (`0x2c4107f`); BaseEdge member order `Entity, Entity, CVec3f, CVec3f, BaseEdgeType, int, objects` (funcsig `0x213f200`); `0x2f7410e mov [rbx+8],eax` ← `[r15]` |
| +0x0c | comp.node1 | `Entity` | `0x2f74115 mov [rbx+0xc],eax` ← `[r15+4]` |
| +0x10 | comp.tangent0 | `CVec3f` | `0x2f7411c`/`0x2f74124` ← `[r15+8..+0x13]` |
| +0x1c | comp.tangent1 | `CVec3f` | `0x2f7412b`/`0x2f74133` ← `[r15+0x14..+0x1f]` |
| +0x28 | comp.type (0 ground, 1 bridge, 2 tunnel) | `BaseEdgeType` | `0x2f7413a mov [rbx+0x28],eax` ← `[r15+0x20]` |
| +0x2c | comp.typeIndex | int | `0x2f74149` ← `[r15+0x24]` |
| **+0x30..+0x47** | comp.objects | **`vector<pair<Entity,EdgeObjectType>>`** (8 B elements) | `0x2f7418f/0x2f74193/0x2f74197` begin/end/cap, element loop stride 8 (`0x2f741b0..0x2f741ca`); ~StreetProposal frees `[r12+0x30]` |
| +0x48 | type: 0 street, 1 track | int | usertype "type" = 0x48 (`0x2c41074`); `0x2f741e5 mov dword [rbx+0x48],0` |
| +0x4c | streetEdge.streetType | int | usertype "streetEdge" = 0x4c (`0x2c41057`); BaseEdgeStreet "streetType" = 0 |
| +0x50 | streetEdge.hasBus | bool | BaseEdgeStreet "hasBus" = 4 (`0x2146c07`) |
| +0x54 | streetEdge.tramTrackType | enum | "tramTrackType" = 8 (`0x2146bfc`) |
| +0x58 / +0x5c | streetEdge.precedenceNode0 / 1 | enum | 0xc / 0x10 (`0x2146bf1`, `0x2146be6`); `0x2f741ec movups [rbx+0x4c]` + `0x2f741fb mov [rbx+0x5c],eax` |
| +0x60 | trackEdge.trackType | int | usertype "trackEdge" = 0x60 (`0x2c4102f`); `0x2f741f4 mov dword [rbx+0x60],-1` |
| +0x64 | trackEdge.catenary | **bool (1 B)**; +0x65..+0x67 padding | BaseEdgeTrack "catenary" = 4 (`0x2146c3d`); `0x2f741fe mov byte [rbx+0x64],0` |
| **+0x68** | **a copy of the edge's `ecs::component::TickEpoch`** (8 B, not exposed to Lua) | | `0x2f7406f lea rax,[rip+…]` = typeinfo `N3ecs9component9TickEpochE` → GetComponentTypeIndex `0x2f74086` → data pointer r14 (`0x2f740dd lea r14,[rdx+rax*8]`, 8-byte component) → `0x2f74202 mov rax,[r14]; 0x2f74209 mov [rbx+0x68],rax` |
| +0x70 | playerOwned.player | `Entity` | usertype "playerOwned" = 0x70 (`0x2c41062`), type `std::optional<ecs::component::PlayerOwned>`; `0x2f74210 mov [rbx+0x70],eax` |
| **+0x74** | **playerOwned engaged flag** | **bool (1 B)**; +0x75..+0x77 padding | `0x2f74205 mov byte [rbx+0x74],r12b` (r12 = 1 only when PlayerOwned exists, `0x2f740e9..0x2f740eb`) |

All PROVEN (SC-SEG, SC-SEG-OWNED, SC-SEG-68). The Windows docs name +0x68 "owning construction
(template pieces)" and +0x74 "owned flag (1)". On Linux +0x68..+0x6f is a TickEpoch qword and +0x74
is the `std::optional` engaged **byte**. Read `seg[0x74]` as a byte. The Windows `u32 == 1` test would
include padding bytes that nothing initialises.

## 4. `construction_builder_util::Proposal::ConstructionEntity` (0x8f0 B)

| Linux | Windows | field | type | evidence | status |
|---|---|---|---|---|---|
| sizeof **0x8f0** | 0x8e0 | | | ~Proposal `0xdc30d3 add r12,0x8f0`; copy ctor `0xe3e28b`; SLICE_TERRAIN_ASSETS.md | PROVEN (SC-CE-SIZE) |
| +0x000..+0x447 | +0x000.. | a copy of the `ConstructionDesc` | | MakeProposalAdd `0x1645ff7 mov rsi,[rbp-0xd50]` (its `const ConstructionDesc&` rsi, saved `0x1645358`) and `0x1646001 call 0x153c860(CE, desc)`, a memberwise copy-assign (strings at +0/+0x30/+0x50/…, `0x153c88f mov eax,[r12+0x20]` etc.); CE copy ctor `0xe3dbb2 call 0xdeb9f0` | PROVEN |
| **+0x000** | **+0x000** | **`desc.fileName`** | libstdc++ `std::string` {`char* p` +0, `size_t len` +8, buf +0x10} | usertype `ConstructionDesc` "fileName" = 0 (`0x29a72c6`), type `std::string` (funcsig `0x29c6190`, first member); CE dtor `0xdc29bc..0xdc29dc` (`p == this+0x10` test) | PROVEN (SC-CE-FILENAME) |
| +0x020 | +0x020 | `desc.type` | `ConstructionType` (int) | usertype "type" = 0x20 (`0x29a72bb`); 0xb for an asset group (SLICE_TERRAIN_ASSETS.md) | PROVEN (SC-CE-TYPE) |
| +0x448..+0x737 | +0x460.. | a copy of the `Construction` (the evaluated `.con` result) | | MakeProposalAdd copies its `const Construction&` (rdx → r13, `0x1645341`) memberwise: `0x1645e0d lea rdi,[rbx+0x448]; mov rsi,r13; call 0xa3ebf0`, then `+0x478 ← r13+0x30` (`0x1645e1d`), `+0x490 ← +0x48`, … `+0x680 ← +0x238` (`0x1645f94`) | PROVEN (SC-CE-CONSTRUCTION) |
| **+0x448** | **+0x460** | **`params`**: `Construction+0`, a **`lua::Table`** (48 B, section 5) holding the params the construction was evaluated with | `lua::Table` | section 4.1 | layout PROVEN; meaning **PROVEN for constructions evaluated by the ConstructionRep function** (SC-CE-PARAMS); for the UI placement **INFERRED** (SC-CE-PARAMS-UI) |
| +0x478 | +0x470 | `vector<TransformedModel>` (0x80 B: string +0, string +0x20, Mat4f +0x40) | | Construction copy `0xdbfb80` (`sar rax,7`; two `_M_construct` at +0/+0x20; 4 `movdqu` at +0x40); SLICE_TERRAIN_ASSETS.md; the ConstructionRep function moves its models vector into `result+0x30..+0x48` (`0x33129e3..0x3312a25`) | PROVEN layout; name INFERRED |
| **+0x738** | **+0x728** | **`transf`** | `CMat4f`, 16 floats, identity by default | usertype "transf" = 0x738 (`0x2c4134d`), type `CMat4f`; CE copy `0xe3dbd6..0xe3dc13` (4 × `movdqu` 0x738..0x777); MakeProposalAdd identity init `0x1645c85 mov qword [rbp-0x1f8],0x3f800000` (CE at `rbp-0x930`) | PROVEN (SC-CE-TRANSF) |
| **+0x778** | **+0x768** | **`frozenNodes`**: indices into addedNodes | `vector<int>` | usertype "frozenNodes" = 0x778 (`0x2c41342`); copy `0xe3dc1a call 0xaa8600`; MakeProposalAdd passes `&CE+0x778` to MakeStreetProposal (`0x1645dd3..0x1645de0`), which pushes node indices (section 6) | PROVEN (SC-CE-FROZEN) |
| **+0x790** | **+0x780** | **`segmentsBefore`** = `addedSegments.size()` before the template's pieces | int | usertype "segmentsBefore" = 0x790 (`0x2c41337`); MakeProposalAdd `0x1645ce4 mov rax,[r14+0x20]; sub rax,[r14+0x18]; sar 3; imul 0xeeeeeeeeeeeeeeef` (÷120) → `0x1645d23 mov [rbp-0x1a0],eax` (CE+0x790), before `0x1645de9 call MakeStreetProposal` | PROVEN (SC-CE-SEGBEFORE) |
| **+0x8c0** | **+0x8b0** | **`name`** | `std::string` | usertype "name" = 0x8c0 (`0x2c4132c`); CE dtor `0xdc2a1b`; MakeProposalAdd moves its by-value `std::string name` (r9) into `rbp-0x70` (`0x1645cec`, `0x1646104`) | PROVEN (SC-CE-NAME) |
| +0x8e0 | | `playerEntity` | `Entity` | usertype = 0x8e0 (`0x2c41321`); MakeProposalAdd `0x1646139 mov eax,[rbp+0x10]` (the `Entity` stack arg) → `0x164613c mov [rbp-0x50],eax`; CE copy `0xe3dd89` | PROVEN |
| +0x8e4 | | `setAsHeadquarterHack` | bool | usertype = 0x8e4 (`0x2c41316`); `0x1645d70 mov byte [rbp-0x4c],0` | PROVEN |

The CE usertype's `fileName`, `params` and `hasCargoPlatform` are Lua properties (lambdas), not member
pointers (funcsig of `usertype_metatable<construction_builder_util::Proposal::ConstructionEntity, …>`,
`0x2c216e0`: property, property, 16-char property, then `CMat4f`, `vector<int>`, `int`, `string`,
`Entity`, `bool` members). That is why their offsets come from the ConstructionDesc/Construction copies
above.

### 4.1 Where `Construction+0` comes from (SC-CE-PARAMS)

`ConstructionDesc+0x280` is a `std::function<Construction(const lua::Table& params)>`: manager at
`+0x290`, invoker at `+0x298`. Every maker evaluates it before `MakeProposalAdd`:

- `scripting::Convert` `0x2e237b0` (the Lua replay). It looks up the desc through the ResTypeRep entry
  (`0x2e24d11 mov r13,[rdx+0x20]`) and tests `[r13+0x290]` (`0x2e24d1a`). It calls
  `[r13+0x298]` with rdi = `rbp-0x330` (result), rsi = `r13+0x280` and rdx = `r12+0x20`, the params of
  the scripting CE being converted (`0x2e24d15`, `0x2e24d36`). It then passes that result as
  MakeProposalAdd's `const Construction&` (`0x2e24ddd mov rdx,[rbp-0x5b8]`, `0x2e24ded`).
- `CreateProposalReplace(toolkit, costRep, Entity, const ConstructionDesc&, const lua::Table& params)`
  `0x16476f0`. It copies params (`0x16476ff mov rsi,r9`, `0x1647741 call 0xaf2200`). It inserts key
  `"upgrade"` when absent (`0x1647750 call 0xbfeda0` lookup, `0x1647d58..0x1647d74`). It calls
  `[desc+0x298]` on the copy (`0x1647765..0x1647796`) and passes the result to MakeProposalAdd
  (`0x1647b8f mov rdx,[rbp-0x960]`, `0x1647ba1`).

Installers of that invoker (scan of every `mov [reg+0x298],reg` / `mov [reg+0x290],reg` whose value was
loaded by a RIP-relative `lea` of a code address at most 12 instructions earlier):

| installer | invoker → body | what the body leaves in `result+0` | status |
|---|---|---|---|
| `void ConstructionRep::Add(std::string, ConstructionDesc, bool)` `0x3313560`: `0x3313e52 lea rcx,[rip-0x949]` = `0x3313510`, `0x3313e81 mov [r14+0x298],rcx` | `0x3313510` → `0x3313532 call 0x3312180` (result rdi → `0x33121e7 mov rbx,rdi`, params rdx → `0x331219b mov [rbp-0x478],rdx`) | **a copy of params.** `0x3312985 mov rsi,[rbp-0x478]`, `0x331298f call 0xa3ebf0` (r15 = params). An optional in-place edit follows through the std::function at `desc+0xe0` (`0x331299e..0x33129b9`). Then `0x33129bf mov rsi,r15; 0x33129c2 mov rdi,rbx; 0x33129c5 call 0xaf2200`. No branch leaves that sequence except the skip of the optional call. `0xaf2200` is the `lua::Table` copy constructor: it initialises the header (`0xaf2225..0xaf2240`: root 0, leftmost/rightmost = `this+8`, count 0), clones the source root (`0xaf2258 call 0x9daa40`) and walks to the leftmost/rightmost nodes | PROVEN |
| UI `0xe2f330` (ConstructionBuilder.cpp; strings `.mdl`, `seed`, `paramX`, `paramY`): `0xe2fe1c lea rcx,[rip+0x3a0d]` = `0xe33830`, `0xe2fe38 mov [rbx+0x298],rcx`, keeping the old function in the capture | `0xe33830` → `0xe33852 call 0xe32e30` (`UI::{anonymous}::UpdateConstruction(…)::<lambda(const lua::Table&)>`) | **an empty Table.** `0xe32e63 call 0xe37dd0` default-constructs the result (Table header at `+8..+0x28`, `0xe37dd4..0xe37e05`), and the body never writes `r14+0..+0x2f` (its only use of r14 afterwards is `0xe33778 lea rdi,[r14+0x30]`). The install is reached only when the file's extension equals `.mdl`: `0xe2f3c2` `boost::filesystem::path::extension`, compare `0xe2f443 call 0xa04600`, `0xe2f4bd test edx,edx; 0xe2f4bf jne 0xe2ffb0` skips past `0xe2fe38`; no branch lands between `0xe2fd00` and `0xe2fe40`. The only branches from past the install back into `0xe2f4c5..0xe2fd00` return from three out-of-line blocks entered only from that same path (`0xe2f5f8 je 0xe30628` … `0xe3063d jmp 0xe2f605`; `0xe2f790 je 0xe30608` … `0xe3061d jmp 0xe2f79d`; `0xe2f8fd je 0xe305e8` … `0xe305fd jmp 0xe2f90a`). Each block is preceded by an unconditional `jmp` (`0xe305de`, `0xe305fd`, `0xe3061d`), and no indirect jump exists in the function | PROVEN (the flow); scan completeness INFERRED |
| `0x38157c0`, `0x3b731b0`, `0x2547150` | other objects (`0x38157c0` writes the same pointer to both slots; the other two write only `+0x290`) | not ConstructionDesc functions | INFERRED |

With `MakeProposalAdd` copying `Construction+0` to `CE+0x448` (`0x1645e0d`/`0x1645e14`), this gives:

- **SC-CE-PARAMS, PROVEN.** For a construction evaluated by the ConstructionRep function, `CE+0x448`
  holds the params it was evaluated with. That covers the Lua replay (`scripting::Convert`) and every
  `CreateProposalReplace` producer (module edits, section 6.2), whose desc comes from the rep's own
  entry (Convert `0x2e24d11`; the AddModuleComp lambda `0xf22763 mov r8,[rdx+0x20]`). A module edit's
  table also carries `upgrade`.
- **SC-CE-PARAMS-UI, INFERRED.** The UI placement evaluates its own desc copy (`ConstructionBuilder+0xd8`,
  passed as `0xe355b1 lea r9,[r12+0xd8]` → `0xe2bbc0` → `0x1646c90`). Its Construction (`rbp-0x1480` in
  `0xe35100`, `0xe35386`) was not traced to its producer. The only installers found are the two above,
  and the `.mdl` one leaves params empty, which the non-empty walk guard (section 7) refuses. Measure once
  live before the UI placement cancel is enabled (section 10.1).

Supporting data (not needed by code): the `ecs::component::Construction` usertype (registration
`0x21f5820`, member-pointer immediates `0x21f6990..0x21f69fe`: `0x128, 0x110, 0xf8, 0xe0, 0xc8, 0xb0,
0x98, 0x90, 0x50, 0x20, 0`) pairs `fileName` +0, `params` (`lua::Table`) +0x20, `transf` +0x50,
`timeBuilt` +0x90, `frozenNodes` +0x98 and so on. The game's built construction carries a
`params` table of the same kind.

## 5. `lua::Table` and `lua::Value` (libstdc++)

`lua::Value = std::variant<lua::Nil, bool, double, std::string, lua::Table>` (funcsig of
`Read<float>` `0x9da7c0`). `lua::Table` is a `std::map<Value, Value>`.

| item | value | evidence | status |
|---|---|---|---|
| sizeof(Value) | 0x38: 0x30 payload + index byte | index read at +0x30 in every `Read<T>` | PROVEN |
| index +0x30 | 1 bool, 2 double, 3 `std::string`, 4 `lua::Table`, 0xff valueless | `Read<bool>` `0xa299e7 cmp byte [rdi+0x30],1`; `Read<float>` `0x9da7e7 cmp …,2`; `Read<string>` `0x9dae17 cmp byte [rsi+0x30],3`; `Read<vector<string>>` (needs a table) `0xa0abe7 cmp …,4`; `0xff` tested in `Table::Get` `0x32801e0` | PROVEN (SC-LUA-VALUE); index 0 = Nil is INFERRED (variant order) |
| payloads (+0x00) | bool = byte (`0xa299fa movzx eax,byte [rdi]`), double = 8 B (`0x9da7fe cvtsd2ss xmm0,[rdi]`), string = libstdc++ string, table = the map object | | PROVEN |
| Table (48 B) | key-compare byte +0x00; header node +0x08 (color int +0x08, **root +0x10**, **leftmost +0x18**, **rightmost +0x20**); **node_count +0x28** | `Table::Get` `0x3280180` (funcsig `const Value& lua::Table::Get(const Value&) const`): `0x328019b mov rbx,[rdi+0x10]`, end = `0x32801bb lea r15,[rdi+8]`; copy ctor `0xaf2200` and copy `0xdc61c0`/`0xa3ebf0` write `[t+0x18]`,`[t+0x20]` = t+8 when empty and `0xdc6260 mov [r15+0x28],rdx` (count) | PROVEN (SC-LUA-TABLE) |
| node (0x90 B) | color int +0x00, parent +0x08, **left +0x10**, **right +0x18**, **key Value +0x20** (index +0x50), **value Value +0x58** (index +0x88) | `0x9daa5f mov edi,0x90; call _Znwm`; Get walks `0x32801d3 mov rbx,[rbx+0x10]` / `0x328020d mov rbx,[rbx+0x18]`, key index `0x32801dc movzx eax,byte [rbx+0x50]`, key `0x3280246 lea rsi,[r13+0x20]`; `Table::Put` `0x3280100` returns `node+0x58` (`0x3280111 add rax,0x58`) | PROVEN (SC-LUA-NODE) |
| order | by index first (`index+1`, so valueless sorts first), then value | `0x32801fb add rax,1; 0x32801ff add rdx,1; cmp; seta`; equal indices dispatch through the comparison table `0x59aa3c0` (`0x328024d`) | PROVEN (SC-LUA-ORDER) |

**Walker notes (Linux).** There is no MSVC `_Isnil` byte and no head node reachable as `*map`. The
end sentinel is the header at `t+8`. Walk in order from the root at `t+0x10` using only left (+0x10)
and right (+0x18), with an explicit stack (bounded: a red-black tree of at most 2048 nodes is at most
2·log2(2049) < 23 levels deep, so refuse beyond 64). Check that the visited count equals `[t+0x28]`.
Keys of kinds 2 and 3 are the only ones Windows observed; the Lua literal format (`[k]=v`, `%.14g`,
`%q`, nested `{}`, depth cap 8) is unchanged.

**`std::string` (libstdc++, PROVEN SC-STRING):** `p` at +0x00 always points at the characters;
`len` at +0x08; inline buffer at +0x10, used iff `p == obj+0x10`, which implies `len <= 15`. Strings
are NUL-terminated at `p[len]`. This replaces the MSVC `ReadSsoString` (len +0x10, cap +0x18). It is
also used by `Read<std::string>` `0x9dae20..0x9dae34` and by the CE destructor tests.

## 6. The template connector and the proposal shapes

### 6.1 What the game does at make time

| fact | evidence | status |
|---|---|---|
| `MakeProposalAdd(const StreetToolkit& rdi, const ConstructionDesc& rsi, const Construction& rdx, const CMat4f& rcx, const unordered_map<int,pair<Entity,float>>& r8, std::string name (hidden ref r9), Entity player [rbp+0x10], Proposal& [rbp+0x18])` `0x1645330` | funcsig; `0x164533a mov r15,rdi`, `0x1645341 mov r13,rdx`, `0x1645358 mov [rbp-0xd50],rsi`, `0x1645362 mov r14,[rbp+0x18]`, `0x1645366`/`0x164536d`/`0x1645374` rcx/r8/r9 | PROVEN |
| It builds the CE on its stack (`rbp-0x930`, pointer `[rbp-0xd80]` `0x1645496`), sets `segmentsBefore = addedSegments.size()`, then calls `MakeStreetProposal(…, Proposal&, &CE.frozenNodes, &proposal.terrainAlignSkipEdges, &proposal.segmentTags, &player, …)` (`0x1645de9`), copies desc/Construction/name/player into the CE, and emplaces it into `toAdd` (`0x1646a21 call 0xdca780`, then `0x1646a29 call 0xdc2a00` on the local). It has no `Proposal+0x288` access (the only `+0x288` operands are `rbp`-relative or on r13, the Construction) | section 4 rows; stack args pushed `0x1645da2..0x1645de3` | PROVEN |
| `MakeStreetProposal` `0x1641dc0` appends the template's street pieces to the caller's Proposal. For each frozen node it takes `index = addedNodes.size()` (`0x1643337..0x1643348`, ÷24) and pushes that index into **both** `CE.frozenNodes` (`0x1643453..0x1643467`, through `[rbp-0x7c0]` = its `[rbp+0x20]` arg) and `proposal.frozenNodes` (`0x1643472 mov rsi,[rdx+0x228]` …, vector at +0x220). The template pieces therefore come **after** the records already in the proposal | disassembly | PROVEN (SC-PROP-FROZEN) |
| **Script path: no snapping.** `scripting::Convert` passes MakeProposalAdd an **empty** `unordered_map<int,pair<Entity,float>>` as r8: `0x2e24bc2 lea rax,[rbp-0x580]` / `0x2e24bc9 mov [rbp-0x5c8],rax` (the map) and `0x2e24bd0 add rax,0x30` / `0x2e24bd4 mov [rbp-0x5e0],rax` (its single bucket) are set once before the loop. On every iteration it is rebuilt empty: `0x2e24d4b` bucket_count 1, `0x2e24d56` max_load 1.0f, `0x2e24d66` buckets = `&single_bucket`, `0x2e24d71` before_begin 0, `0x2e24d7c` element_count 0, `0x2e24d87` next_resize 0, `0x2e24d92` single bucket 0. The only call before `0x2e24dd6 mov r8,[rbp-0x5c8]` / `0x2e24ded` is the name's `_M_construct` (`0x2e24dc1`) | disassembly | PROVEN (SC-TEMPLATE-SCRIPT) |
| **UI weld shape.** The UI welds the outer template node onto the road (apron + halves + removed edge); the outer node comes last; the apron is the segment whose PlayerOwned is engaged (+0x74) | Windows decompiled code + differential dumps (`docs/re/PROPOSALS.md`) only. Statically on Linux: MakeStreetProposal writes a segment's `+0x68` and `+0x70` qwords from locals (`0x1642bb9`, `0x1642bc5`), whose origin was not traced | **UNPROVEN** (SC-TEMPLATE-SHAPE): `MergeTemplateStreet` stays OFF (10.1) |

### 6.2 Proposal shapes per producer

| producer / caller | shape | evidence | status |
|---|---|---|---|
| `CreateProposalReplace(toolkit, costRep, Entity, const ConstructionDesc&, const lua::Table&)` `0x16476f0` | exactly `toRemove = [the Entity argument]` and exactly one CE in `toAdd`; `removedNodes`/`edgeObjectsToRemove` come only from the remove step | result r12 (`0x1647706 mov r12,rdi`). Remove step into a separate Proposal `rbp-0x730`: `0x16477b4 call 0xdc2b90` (ctor), `0x1647811 call 0x1640a90` `MakeProposalRemove(StreetToolkit, const Entity&, Proposal&, vector<Entity>&)` with rsi = `&[rbp-0x944]` (the Entity from ecx, `0x1647711`). MakeProposalRemove pushes that Entity once into `toRemove` (`0x1640dfd mov rsi,[rbp-0x138]`, `0x1640e04 lea rdi,[rbx+0x288]`, `0x1640e0b call 0xbfc610`, the `vector<int>::push_back` at `0xbfc614..0xbfc627`); it has no other `+0x288` access. Add step into the result: `0x1647b51 call 0xdc2b90` on r12, `0x1647ba1 call MakeProposalAdd(…, Proposal& = r12)`. Then asserts `result2.proposal.removedNodes.empty()` (`0x1647bbd..0x1647bc7`), `result2.proposal.edgeObjectsToRemove.empty()`, `result2.toRemove.empty()` (`0x1647be3..0x1647bf3`), `result2.toAdd.size() == 1` (`0x1647bf9..0x1647c0f`, `cmp rax,0x8f0`). It copies removedNodes/removedSegments/edgeObjectsToRemove/toRemove from the remove proposal (`0x1647c1c..0x1647c6a`) and asserts `result2.toRemove.size() == 1` (`0x1647c6f..0x1647c83`, `cmp rax,4`). Assert strings are at `0x1647d96..0x1647e93`. The assert handler `0x2fcb860` never returns normally: no `ret` in its FDE, and `0x2fcb8be call 0x2fcb5e0` is followed directly by a landing pad; `0x2fcb5e0` has no `ret` either | PROVEN (SC-SHAPE-REPLACE, SC-ASSERT-NORETURN) |
| AddModuleComp lambda, return `0xf229a5` | the `CreateProposalReplace` shape, verbatim | `0xf2276c lea rdi,[rbp-0x7e0]`, `0xf2277d mov [rbp-0xae8],rdi`, `0xf22786 call 0x16476f0`. The only other uses of `rbp-0x7e0..-0x420` or `[rbp-0xae8]` are `0xf22971` (the copy source) and `0xf22a9f` (the destroy). The calls between `0xf2278b` and the factory are `0xf22923 call 0xdd99f0` (not on that object) and `0xf2297b call 0xe3e060` (the copy) | PROVEN (SC-SHAPE-AMC) |
| ModuleBuilder, return `0xe4f6bd` | the `CreateProposalReplace` shape, if `ModuleBuilder::Step` is the only writer of `m_proposal` | `virtual void UI::ModuleBuilder::Step` `0xe506b0` (ModuleBuilder.cpp): `0xe52972 call 0x16476f0` into `rbp-0xff0`, `0xe52977 mov rdi,[r12+0xa8]`, `0xe52989 call 0xde0430` (move-assign into `*m_proposal`), `0xe52991 call 0xdc3040`. Exclusivity of that writer not proven | INFERRED (SC-SHAPE-MB) |
| ConstructionBuilder placement, return `0xe34860` | one CE in `toAdd` (checked by the game); `toRemove` empty from the builder | `virtual void UI::ConstructionBuilder::Step` `0xe35100`: `0xe355de call 0xe2bbc0` (result r14), `0xe355e3 mov rdi,[r12+0x850]`, `0xe355f2 call 0xde0430`, then `0xe35610..0xe35630` checks toAdd span == 0x8f0, else the assert `m_proposal->toAdd.size() == 1` at `0xe37563`. `0xe2bbc0` returns the Proposal built by `0x1646c90` (make_proposal.cpp): `0x1646cb6 mov [rbp-0xe8],rdi`, `0x1646fe7 call 0xdc2b90` on it, a single `0x1647036 call MakeProposalAdd` with it as `Proposal&` (`0x1647011 push [rbp-0xe8]`), and no `+0x288` access. MakeStreetProposal has no `+0x288` operand. MousePressed asserts `!m_proposal->toAdd.empty()` (1). Other writers of `m_proposal`, and callees of MakeStreetProposal, not checked | `toAdd.size()==1` at Step PROVEN; `toRemove` empty at the factory INFERRED (SC-SHAPE-PLACEMENT) |
| Bulldozer, module removal, return `0xdd4e99` | the `CreateProposalReplace` shape | `{anonymous}::FindSlotToRemove` / ModuleBulldozerAction.cpp `0xefbba0`: `0xefd2a7 call 0x16476f0`, `0xefd2b6 call 0xde0430` into `*[rbp-0x10d8]`. The link from there to `Bulldozer::Apply`'s copy (1.1) was not traced | INFERRED (SC-SHAPE-BULLDOZER) |
| any other caller | unknown | | UNPROVEN (SC-SHAPES) |

**SC-SHAPES (UNPROVEN as a blanket rule).** The Windows table ("placement = toAdd[0] + street pieces,
toRemove empty; module edit = toRemove[0] old + toAdd[0] new") holds only where the rows above say so.
The Linux code checks the shape at run time before arming any construction cancel (section 7).

## 7. Linux offsets per ported routine

| routine | Linux | Windows |
|---|---|---|
| `StashConxpFromProposal` | toAdd `P+0x2a0/+0x2a8`; **require span == 0x8f0** (exactly one CE) and `toRemove` span == 0 (`P+0x288/+0x290`); CE = begin. fileName = string at CE+0x000 (section 5); transf = 16 floats at **CE+0x738**; params = `lua::Table` at **CE+0x448**, walk must yield ≥ 1 entry (refuses the empty `.mdl` case, 4.1). Call **before the trampoline** | `0x1f8`, 0x8e0, MSVC string, `+0x728`, `+0x460`, `0x1e0` |
| `SerLuaTable` / `SerLuaValue` | section 5: root `t+0x10`, end `t+8`, count `t+0x28`; node key `+0x20`/idx `+0x50`, value `+0x58`/idx `+0x88`; Value payload +0, index +0x30 (1 bool byte, 2 double, 3 string, 4 table) | head `+0`, size `+8`, key `+0x20`/tag `+0x40`, value `+0x48`/tag `+0x68`, `_Isnil +0x19` |
| `ReadSsoString` | `{p +0, len +8}`; valid iff `len <= 4096` and (`p == s+0x10 && len <= 15`, or `p` readable for `len+1`) | len `+0x10`, cap `+0x18` |
| `StashConupFromProposal` | `toRemove` `P+0x288`, **span == 4**, the int32 > 0; then the CONXP stash (span == 0x8f0; the empty-toRemove rule does not apply here). Before the trampoline | `0x1e0` |
| `IsUpgradeShape` | **`nrem == 1 && nadd == 1`** with `nrem = span(P+0x288)/4`, `nadd = span(P+0x2a0)/0x8f0` (the proven `CreateProposalReplace` shape, 6.2). Windows accepted `>= 1`; a larger shape would ship only element 0 | `0x1e0`, `0x1f8`/0x8e0, `>= 1` |
| `MergeTemplateStreet` | nodes `P+0x00` (0x18 B: pos +0, **doubleSlipSwitch u8 +0x0c**, id +0x14); segments `P+0x18` (0x78 B: node0 +0x08, node1 +0x0c, t0 +0x10, t1 +0x1c, tail from +0x28, **"owned" = byte +0x74**); removed segments `P+0x48`; StreetProposal frozen `P+0x220`; tags `P+0x270` (32 B); toAdd `P+0x2a0`; CE frozenNodes `+0x778`, segmentsBefore `+0x790`. OFF (SC-TEMPLATE-SHAPE) | `+0x170`, `+0x1c8`, `+0x1f8`, CE `+0x768`/`+0x780`, owned u32 `+0x74`, flags u32 `+0x0c` |
| construction placement branch (ROADC) | the street decoders (owned by slice-proposal) read `P+0x00/+0x18/+0x30/+0x48`, 24/120 B records, and segment +0x08/+0x0c/+0x10/+0x1c/+0x28/+0x2c/+0x48/+0x4c/+0x50 (u8)/+0x54/+0x60/+0x64 (u8). These match section 3 (PROVEN), so the Windows decoders' offsets carry over unchanged. Before the trampoline | same |
| dumpprop diagnostic | Proposal span 0x3c0; Context span 0x68 (0.1). Before the trampoline | 0x240 / 0x480 dumped |

### Linux hazards in `MergeTemplateStreet` (PROVEN from the layouts; the fixes are recommendations)

1. **Do not `memcpy` a `std::string` between tag slots** (Windows: `memcpy(tb + o*32, tb + a*32, 32)`).
   A libstdc++ SSO string's `p` points into its own object. A byte copy leaves the destination's `p`
   pointing at the source slot, and `~string` then frees a pointer into the middle of the vector's
   block. Copy the characters and set `dst.p = dst+0x10` when `src.p == src+0x10`. Alternatively swap
   the two 32-byte objects, fixing each SSO `p` to its new address. The dropped last slot is not
   destroyed (end pointer moved), so taking over a heap buffer from it is safe.
2. **Do not copy a segment's `objects` vector (+0x30..+0x47) between two live records.** The Windows
   weld copies `+0x28..+0x77` from the template apron. That apron is then dropped (not destroyed), so
   only one owner remains. The Windows halves fix, however, copies `+0x28..+0x63` from the *removed*
   edge record, which stays alive in `removedSegments`. If that edge carries edge objects, both
   records own the same buffer and `~Proposal` frees it twice. Copy `+0x28..+0x2f` and `+0x48..+0x67`
   field-wise, and refuse (log, leave the proposal as built) when the objects vector of either record
   is non-empty.
3. The Windows halves fix writes the u32 `0x7f00` at `+0x64`. On Linux that is `trackEdge.catenary =
   false` plus padding. For a street half it is harmless; for a split **track** edge it would drop
   catenary. It also copies only `+0x6c`, the upper half of the TickEpoch qword. See open questions.
4. Node "flags": write only the byte `doubleSlipSwitch` at +0x0c (0), never a u32.
5. Dropping the last record by moving `end` back leaks its resources (strings, objects vectors).
   libstdc++ frees with `operator delete(begin, cap-begin)`, and `cap` is untouched, so this is safe.
   The begin/end/cap triple is moved into the Command by the factory (0.1), so the leak happens when
   the Command dies.
6. **Order.** The merge runs at factory entry, before the trampoline (0.3). After the trampoline the
   Proposal is empty.

## 8. Wire lines (unchanged; the Lua readers)

Written to the instance's inject file (slice-core owns the path and letter).

| line | writer | reader | notes |
|---|---|---|---|
| `CONXP <file> t=<16 floats %.4f,…> params=<lua literal>` | `WriteInjectConxp`, from the Add hook once the cancel landed | `inject.lua:592-624` (`#w >= 3`, `t=` split on `,`, needs 16 numbers; `params=(.*)$`) | seats a `pendingCons` entry with `cancelled = 1` |
| `CONUP <oldEntity> <file> t=<16 floats> params=<lua literal>` | `WriteInjectConup`, from the Add hook once the cancel landed | `inject.lua:471-518` (`#w >= 4`) | resolves oldEntity to a position on this instance; diff via `CM.conDiff` |
| `ROADC <n> <etype> <stype> <ttype> <cat> <m> <re>` then `n×(id x y z)`, `m×(a1 a2 t0 t1)`, `re×(a1 a2 t0 t1)`, `m×(btype bidx)` | `WriteInjectConRoad`, at the factory | `inject.lua:625-722` (`#w >= 8 + n*4 + m*8 + re*8`, optional bridge tail) | parked in `CM.pendingRoadc` |

(SC-WIRE, PROVEN by reading the Lua.) Floats use `%.4f` and params use `%.14g`/`%q`, as on Windows.

## 9. Run-time verification table

Check once at init (image base + RVA). On any mismatch, leave the construction capture, the CONXP/CONUP
cancel and `MergeTemplateStreet` OFF and log. ROADC may keep shipping, since it cancels nothing.

| RVA | bytes | instruction | proves |
|---|---|---|---|
| `0x15ee964` | `48 89 95 f0 e4 ff ff` | `mov [rbp-0x1b10],rdx` | factory saves the Proposal (rdx) |
| `0x15ef154` | `48 8b 95 f0 e4 ff ff` | `mov rdx,[rbp-0x1b10]` | only reload of the saved Proposal |
| `0x15ef17f` | `e8 ac 12 7f ff` | `call 0xde0430` | Proposal moved into the payload |
| `0xde0440` | `49 89 f4` | `mov r12,rsi` | move-assign keeps the source |
| `0xde047e` | `48 c7 06 00 00 00 00` | `mov qword [rsi],0` | … and empties it |
| `0x15ef1e2` | `49 c7 47 60 00 00 00 00` | `mov qword [r15+0x60],0` | Context shared_ptr moved out |
| `0x15ef1fc` | `49 c7 47 58 00 00 00 00` | `mov qword [r15+0x58],0` | Context shared_ptr moved out |
| `0x15f39e1` | `e8 da 65 84 ff` | `call 0xe39fc0` | payload moved into the CmdData |
| `0x15ef2ab` | `c6 45 b8 0f` | `mov byte [rbp-0x48],0xf` | CmdData tag 15 |
| `0x15dab9e` | `48 8d 05 4b 3a 3e 04` | `lea rax,0x59be5f0` | Add builds the CommandList::Add lambda slot |
| `0x15da768` | `48 8d 35 71 95 d4 02` | `lea rsi,'*ZN11CommandList3Add…'` | manager's type name |
| `0xe3485b` | `e8 d0 a0 7b 00` | `call 0x15ee930` | UI placement → factory (return `0xe34860`) |
| `0xe3463f` | `4c 8d bd 50 fb ff ff` | `lea r15,[rbp-0x4b0]` | UI Proposal temporary |
| `0xe34827` | `48 8b b3 50 08 00 00` | `mov rsi,[rbx+0x850]` | copy source = m_proposal |
| `0xe34831` | `e8 2a 98 00 00` | `call 0xe3e060` | Proposal copy into the temporary |
| `0xe345b8` | `48 39 82 a8 02 00 00` | `cmp [rdx+0x2a8],rax` | tool checks toAdd non-empty |
| `0xe34890` | `e8 ab 5f 7a 00` | `call 0x15da840` | UI placement → Add |
| `0x197132e` | `e8 fd d5 c7 ff` | `call 0x15ee930` | Lua buildProposal → factory (return `0x1971333`) |
| `0x1971310` | `e8 4b cd 4c ff` | `call 0xe3e060` | Lua copy of the Convert result |
| `0x19712a3` | `b9 0d 00 00 00` | `mov ecx,0xd` | Context = 13 qwords |
| `0x1963cbe` | `48 8d 35 af c2 5d 02` | `lea rsi,'buildProposal'` | maker name |
| `0x1963ce0` | `e8 cb c7 ff ff` | `call 0x19604b0` | allocates the buildProposal lambda |
| `0x19729b4` | `e9 57 e0 ff ff` | `jmp 0x1970a10` | lambda functor → wrapper body |
| `0xe8644d` | `e8 de 84 76 00` | `call 0x15ee930` | StreetBuilder (return `0xe86452`) |
| `0xe4f6b8` | `e8 73 f2 79 00` | `call 0x15ee930` | ModuleBuilder (return `0xe4f6bd`) |
| `0xf229a0` | `e8 8b bf 6c 00` | `call 0x15ee930` | AddModuleComp lambda (return `0xf229a5`) |
| `0xf22786` | `e8 65 4f 72 00` | `call 0x16476f0` | AddModuleComp builds with CreateProposalReplace |
| `0xf228ae` | `48 c7 45 b0 00 00 00 00` | `mov qword [rbp-0x50],0` | AddModuleComp completion function empty |
| `0xf229b3` | `48 8d 4d a0` | `lea rcx,[rbp-0x60]` | … and that is Add's rcx |
| `0xf22c57` | `e9 64 f9 ff ff` | `jmp 0xf225c0` | invoker thunk of that lambda |
| `0xf1c4d1` | `48 8d 05 78 67 00 00` | `lea rax,0xf22c50` | taken by `AddModuleComp::UpdateTabs` |
| `0xdd4e94` | `e8 97 9a 81 00` | `call 0x15ee930` | Bulldozer::Apply (return `0xdd4e99`) |
| `0xed6da4` | `e8 87 7b 71 00` | `call 0x15ee930` | TrackModifier::Build (return `0xed6da9`) |
| `0x1647c09` | `48 3d f0 08 00 00` | `cmp rax,0x8f0` | CreateProposalReplace: toAdd.size() == 1 |
| `0x1647c7f` | `48 83 f8 04` | `cmp rax,4` | CreateProposalReplace: toRemove.size() == 1 |
| `0x2e24dd6` | `4c 8b 85 38 fa ff ff` | `mov r8,[rbp-0x5c8]` | Convert passes its (empty) snap map |
| `0x2e24d7c` | `48 c7 85 98 fa ff ff 00 00 00 00` | `mov qword [rbp-0x568],0` | snap map element_count 0 |
| `0x33129c5` | `e8 36 f8 7d fd` | `call 0xaf2200` | ConstructionRep function: result+0 = params copy |
| `0xdc0294` | `48 8b bf 20 02 00 00` | `mov rdi,[rdi+0x220]` | StreetProposal.frozenNodes |
| `0xdc0410` | `49 8b 7c 24 30` | `mov rdi,[r12+0x30]` | segment objects vector +0x30 |
| `0xdc041f` | `49 83 c4 78` | `add r12,0x78` | segment stride |
| `0xdef194` | `48 83 c1 18` | `add rcx,0x18` | node stride |
| `0xdc3109` | `48 8d bb 70 02 00 00` | `lea rdi,[rbx+0x270]` | segmentTags |
| `0xdc30f8` | `48 8b bb 88 02 00 00` | `mov rdi,[rbx+0x288]` | toRemove |
| `0xdc30b7` | `4c 8b ab a8 02 00 00` | `mov r13,[rbx+0x2a8]` | toAdd |
| `0xdc30d3` | `49 81 c4 f0 08 00 00` | `add r12,0x8f0` | sizeof(CE) |
| `0x2c40b3b` | `48 c7 85 20 fa ff ff 20 02 00 00` | `mov qword [rbp-0x5e0],0x220` | usertype StreetProposal.frozenNodes |
| `0x2c40bb4` | `48 c7 85 60 f9 ff ff 18 00 00 00` | `mov qword [rbp-0x6a0],0x18` | usertype addedSegments |
| `0x2c40b9e` | `48 c7 85 80 f9 ff ff 48 00 00 00` | `mov qword [rbp-0x680],0x48` | usertype removedSegments |
| `0x2c41143` | `48 c7 85 00 fa ff ff a0 02 00 00` | `mov qword [rbp-0x600],0x2a0` | usertype toAdd |
| `0x2c4114e` | `48 c7 85 f0 f9 ff ff 88 02 00 00` | `mov qword [rbp-0x610],0x288` | usertype toRemove |
| `0x2c41159` | `48 c7 85 e0 f9 ff ff 70 02 00 00` | `mov qword [rbp-0x620],0x270` | usertype segmentTags |
| `0x2c4134d` | `48 c7 85 d0 f9 ff ff 38 07 00 00` | `mov qword [rbp-0x630],0x738` | usertype CE.transf |
| `0x2c41342` | `48 c7 85 e0 f9 ff ff 78 07 00 00` | `mov qword [rbp-0x620],0x778` | usertype CE.frozenNodes |
| `0x2c41337` | `48 c7 85 f0 f9 ff ff 90 07 00 00` | `mov qword [rbp-0x610],0x790` | usertype CE.segmentsBefore |
| `0x2c4132c` | `48 c7 85 00 fa ff ff c0 08 00 00` | `mov qword [rbp-0x600],0x8c0` | usertype CE.name |
| `0x2c40ff3` | `48 c7 85 70 fb ff ff 14 00 00 00` | `mov qword [rbp-0x490],0x14` | usertype NodeAndEntity.entity |
| `0x2c41062` | `48 c7 85 20 fa ff ff 70 00 00 00` | `mov qword [rbp-0x5e0],0x70` | usertype SegmentAndEntity.playerOwned |
| `0x2146c91` | `48 c7 85 70 fb ff ff 0c 00 00 00` | `mov qword [rbp-0x490],0xc` | usertype BaseNode.doubleSlipSwitch |
| `0x2146c3d` | `48 c7 85 b0 fb ff ff 04 00 00 00` | `mov qword [rbp-0x450],4` | usertype BaseEdgeTrack.catenary |
| `0x29a72c6` | `48 c7 85 d0 f4 ff ff 00 00 00 00` | `mov qword [rbp-0xb30],0` | usertype ConstructionDesc.fileName |
| `0x1645d23` | `89 85 60 fe ff ff` | `mov [rbp-0x1a0],eax` | CE.segmentsBefore = addedSegments.size() |
| `0x1645e0d` | `48 8d bb 48 04 00 00` | `lea rdi,[rbx+0x448]` | CE.construction (params table) |
| `0x1645e14` | `e8 d7 8d 3f ff` | `call 0xa3ebf0` | `lua::Table` assign |
| `0x1646001` | `e8 5a 68 ef ff` | `call 0x153c860` | CE+0 ← ConstructionDesc |
| `0x1643342` | `69 c0 ab aa aa aa` | `imul eax,eax,0xaaaaaaab` | frozen index = addedNodes.size() |
| `0x1643472` | `48 8b b2 28 02 00 00` | `mov rsi,[rdx+0x228]` | push into StreetProposal.frozenNodes |
| `0x2f74205` | `44 88 63 74` | `mov [rbx+0x74],r12b` | optional<PlayerOwned> engaged byte |
| `0x2f74209` | `48 89 43 68` | `mov [rbx+0x68],rax` | +0x68 = TickEpoch |
| `0x2f741fe` | `c6 43 64 00` | `mov byte [rbx+0x64],0` | catenary byte |
| `0xe3dbd6` | `f3 41 0f 6f 8c 24 38 07 00 00` | `movdqu xmm1,[r12+0x738]` | CE copy: transf |
| `0xdc2a1b` | `48 8b bf c0 08 00 00` | `mov rdi,[rdi+0x8c0]` | CE name buffer |
| `0x9da7e7` | `80 7f 30 02` | `cmp byte [rdi+0x30],2` | Value index +0x30, 2 = double |
| `0xa299e7` | `80 7f 30 01` | `cmp byte [rdi+0x30],1` | 1 = bool |
| `0x9dae17` | `80 7e 30 03` | `cmp byte [rsi+0x30],3` | 3 = string |
| `0xa0abe7` | `80 7e 30 04` | `cmp byte [rsi+0x30],4` | 4 = table |
| `0x328019b` | `48 8b 5f 10` | `mov rbx,[rdi+0x10]` | Table root |
| `0x32801bb` | `4c 8d 7f 08` | `lea r15,[rdi+8]` | Table end sentinel |
| `0x32801dc` | `0f b6 43 50` | `movzx eax,byte [rbx+0x50]` | node key index |
| `0x32801d3` | `48 8b 5b 10` | `mov rbx,[rbx+0x10]` | node left |
| `0x328020d` | `48 8b 5b 18` | `mov rbx,[rbx+0x18]` | node right |
| `0x3280111` | `48 83 c0 58` | `add rax,0x58` | node value |
| `0x9daa5f` | `bf 90 00 00 00` | `mov edi,0x90` | node size |
| `0xdc6258` | `49 8b 56 28` | `mov rdx,[r14+0x28]` | Table node_count |

## 10. Integration notes (outside this area's files)

### 10.1 Feature gates

| feature | needs | status of the needs | Linux default |
|---|---|---|---|
| ROADC from the UI placement (return `0xe34860`) | SC-CALLER-UI, section 3 | PROVEN | on (cancels nothing) |
| CONXP cancel of the UI placement (`0xe34860`) | SC-CE-PARAMS-UI, SC-SHAPE-PLACEMENT (`toRemove` empty) | INFERRED | **OFF** behind a switch. When on: exact shape (7) and a non-empty params walk, else not cancelled |
| CONUP cancel from the construction window's module list (`0xf229a5`) | SC-SHAPE-AMC, SC-CE-PARAMS, SC-ADD-PAIRING (empty callback) | PROVEN | may be on, gated by `nrem == 1 && nadd == 1`, `toRemove[0] > 0` and a non-empty params walk |
| CONUP cancel from ModuleBuilder (`0xe4f6bd`) | SC-SHAPE-MB | INFERRED | **OFF** behind a switch; same gates when on |
| CONUP stash for the bulldozer's module removal (`0xdd4e99`, `LogBulldoze`) | SC-SHAPE-BULLDOZER | INFERRED | **OFF**; same gates when on |
| CONUP cancel for any other caller by shape | SC-SHAPES | UNPROVEN | **OFF** |
| `MergeTemplateStreet` on the Lua replay (`0x1971333`) | SC-TEMPLATE-SHAPE | UNPROVEN | **OFF**; keep the Windows shape checks for when it is measured |
| dumpprop | 0.1 sizes | PROVEN | diagnostic only, before the trampoline |

A disabled cancel means the native build stands and the existing poll replicates it, as on Windows
when a decode fails. Every OFF row names what to measure in section 12.

### 10.2 Notes to other areas

- **slice-core, factory hook `0x15ee930`:** pass this area `(cmd = rdi, proposal = rdx, ctx = rcx,
  callerRva = [rsp] - base)` at entry, **before** the trampoline, and run every construction step
  (ROADC decode, stash, dumpprop, merge) there. The factory consumes `*rdx` and the shared_ptr at
  `ctx+0x58/+0x60` (0.1, 0.3); nothing may read them after the trampoline. The Windows
  `g_pendingCmd = rcx` becomes `rdi`. The script caller for this factory is exactly `0x1971333`. For
  that caller, the terrain/asset injectors (slice-terrain-assets) run first and `MergeTemplateStreet`
  second (when it is enabled), as on Windows.
- **slice-core, Add hook `0x15da840`:** identity PROVEN (0.2). Match the pending construction cancel on
  `rdx` (SC-ADD-PAIRING); drop a pending cancel whose Command pointer does not match the next Add's
  `rdx` (the CreateConstructionMenu callers never reach Add directly). The completion function in `rcx`
  is a libstdc++ `std::function<void(const Command&)>*` and **may be empty**: fire it as
  `invoker(rcx, rdx)` only when `[rcx+0x10] != 0`. Its temporary stays owned by the caller, which runs
  manager op 3 afterwards. This area needs to be told only whether the cancel landed (write CONXP/CONUP)
  or not (drop the stash). What to leave in Add's `rdi` result slot (a `connection`, destroyed by
  `0x3190430` at every checked site) when Add is skipped is slice-core's open item (see SLICE_LINES.md
  section 5).
- **slice-proposal:** owns `DecodeNodes`/`DecodeEdges`/`DecodeEdgeType`/`WriteInjectConRoad`, which
  the placement branch calls at factory entry. The offsets it needs are in section 3. It also owns
  `LogBulldoze`, which calls this area's `StashConupFromProposal` for the module-removal shape at return
  `0xdd4e99`; that stash is OFF on Linux (10.1).
- **Shared services expected from slice-core:** a fault-safe readable-range check, `ReadInstance`,
  `SessionLive`, `DumpPropOn`, `WriteArmed`, the data dir, the log, the image base and the build-ok
  flag, plus the switches of 10.1.
- **Threads (SC-THREAD-LUA, UNPROVEN):** the thread that runs the Lua `buildProposal` wrapper
  (`0x1970a10`, reached only from the sol functor `0x1972920` registered in `SetupCommandInterface`
  `0x19638f0`) and the one that runs `ConstructionBuilder::MousePressed` are run-time facts not
  established here. The code must not depend on them: keep all pending construction state behind the
  Windows-style atomics or one mutex.
- `SLICE_TERRAIN_ASSETS.md` marks CE+0 "fileName" INFERRED. Section 4 here proves it
  (ConstructionDesc usertype + `0x153c860`).

## 11. Differences from Windows

| item | Windows | Linux |
|---|---|---|
| factory args | rcx ret, r8 Proposal, r9 Context | rdi ret, **rdx Proposal (by value, consumed)**, **rcx Context (by value, shared_ptr moved out)** |
| cancel pairing | `Add.r8 == factory.rcx` | `Add.rdx == factory.rdi` |
| completion callback | always fired when `g_pendingNoCb == 0` | fire only when `_M_manager` `[rcx+0x10] != 0` (empty at `0xf229a5`) |
| Lua caller | range `0xcec000..0xcf2000`, `0xced378` | exactly `0x1971333` |
| UI placement caller | `0x419f62` | `0xe34860` |
| Proposal tail | tags `0x1c8`, toRemove `0x1e0`, toAdd `0x1f8`, frozen `0x170` | `0x270`, `0x288`, `0x2a0`, `0x220` |
| CE | 0x8e0; params `+0x460`; transf `+0x728`; frozen `+0x768`; segmentsBefore `+0x780`; name `+0x8b0` | 0x8f0; `+0x448`; `+0x738`; `+0x778`; `+0x790`; `+0x8c0` |
| lua::Table | MSVC map {head, size}; node key +0x20 (tag +0x40), value +0x48 (tag +0x68); `_Isnil` +0x19 | libstdc++ map {…, root +0x10, count +0x28}; key +0x20 (idx +0x50), value +0x58 (idx +0x88); header sentinel |
| Value | 0x28 B, tag +0x20 | 0x38 B, index +0x30 |
| std::string | len +0x10, cap +0x18, inline iff cap < 16 | p +0, len +8, inline iff p == this+0x10 |
| node +0x0c | "flags u32 0x7f00" | `bool doubleSlipSwitch` + 3 padding bytes |
| segment +0x68 / +0x74 | "construction" / "owned u32" | TickEpoch qword / optional engaged **byte** |
| upgrade shape | `nrem >= 1 && nadd >= 1` | `nrem == 1 && nadd == 1` |

## 12. Open questions (what to measure before an OFF row of 10.1 is switched on)

- **SC-CE-PARAMS-UI / SC-SHAPE-PLACEMENT:** one UI placement of a road-snapped depot and one of a
  free-standing station on Linux with dumpprop and the params walk logged (read only, before the
  trampoline). Expect `toRemove` empty, one CE, and a params table with `paramX`, `paramY`, `seed`.
  Alternatively, trace the Construction `rbp-0x1480` of `ConstructionBuilder::Step` `0xe35100` to its
  producer.
- **SC-TEMPLATE-SHAPE:** a Linux dumpprop capture of that UI placement and of its Lua replay (caller
  `0x1971333`). Confirm the apron/halves/removed-edge records, the outer template node last, and that
  only the template apron has `+0x74` engaged. Alternatively, trace the locals behind MakeStreetProposal's
  `0x1642bb9`/`0x1642bc5`.
- **SC-SHAPE-MB / SC-SHAPE-BULLDOZER:** find every writer of `ModuleBuilder+0xa8`, and the path from
  `ModuleBulldozerAction` `0xefd2b6` to `Bulldozer::Apply`'s inlined copy (`0xdd4a19`), or log the
  shape live.
- `MergeTemplateStreet` halves fix (section 7, hazards 2–3): the Windows code shares the removed edge's
  `objects` buffer, clears catenary and copies half a TickEpoch. Decide with the Windows owner whether
  the Linux port should instead copy only the non-owning fields (`+0x28..+0x2f`, `+0x48..+0x67`) plus
  the full TickEpoch (`+0x68..+0x6f`) from the removed edge, and refuse when edge objects are present.
- The `segmentTags`/`addedSegments` parallel sizing branch targets (SC-PROP-TAGS) were not traced.
- SC-THREAD-LUA: log the thread id at the Lua wrapper's factory call and at the UI placement once live.
- Nothing in this document has been observed in the running Linux game.
