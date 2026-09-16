# Native evidence for Windows dev cae5d370

Target: native Steam build 35924, GNU build-id
`3a0e156390b0e6f1e372051c24802c8493ae454a`, confirmed with `readelf -n`
on the supplied lab ELF. All examination was static; no game code was executed.
No new patch sites or engine layout offsets are introduced by this batch.

## Capture and wire changes

The existing Linux factory prologues, caller checks, constructor checks and
build gate remain in place. `tools/linux/verify_capture_elf.py GAME` checks
149 construction/asset probes directly against executable PT_LOAD file spans.
`verify_train_order_elf.py GAME` separately checks the retained train hook.

The existing sources of layout proof are [SLICE_CORE.md](SLICE_CORE.md),
[SLICE_VEHICLES.md](SLICE_VEHICLES.md), [SLICE_PROPOSAL.md](SLICE_PROPOSAL.md),
[SLICE_CONSTRUCTION.md](SLICE_CONSTRUCTION.md),
[SLICE_TERRAIN_ASSETS.md](SLICE_TERRAIN_ASSETS.md), and the corrected
[implementation record](../../linux/CONSTRUCTION_TERRAIN_IMPLEMENTATION.md).

- SysV BuildProposal: output rdi, engine rsi, by-value Proposal storage rdx,
  Context rcx. Proposal construction-add/remove vectors remain +0x2a0/+0x288,
  construction stride 0x8f0, params +0x448, transform +0x738. No Windows
  0x8e0 construction stride or MSVC string offsets are substituted.
- Native libstdc++ strings are pointer/length/SSO-or-capacity at +0/+8/+16;
  vectors retain begin/end/cap ordering, divisibility and readable-span checks.
  A 1 GiB corrupt-span guard remains. Count/filename/text policy limits are
  removed from the changed capture paths. Empty-only and single-object shape
  predicates remain: they establish the command type, rather than clip data.
- Vehicle part stride remains 0x88; autoload +0x60 is libstdc++ vector<bool>,
  with 64-bit words and start/end bit offsets. Growable source storage still
  normalizes into the Windows wire's signed 32-bit words and clears tail bits.
- Param uses the previously verified 0x38 variant and tag +0x30; map node key
  +0x20 and value key+0x38. An explicit traversal stack removes recursion depth
  limits; active ancestor detection and the existing map link/count checks
  reject cycles. Empty tables serialize as `{}`.
- ROADC and CONXP now carry the same process-unique `ps=` serial, and CONXP
  carries `rc=0/1` before params. Both records are still one atomic Linux
  injection preceding the verified cancel. No engine field supplies the serial.
- TPAS v2 uses u32 string lengths, matching incoming Windows code; v1 is
  refused. Linux asset models remain 0x80 records with strings +0/+0x20 and
  matrix +0x40. Decoder checks remaining bytes before allocating model arrays.
- Template-weld bookkeeping is growable; ownership validation and transactional
  writes remain. This does not expand the older Linux weld's accepted topology
  into Windows's newer generalized modular-station algorithm.

## Recovery: attempted, still missing prerequisites

Looked up the source-signature anchors in `~/tpf2-re/linux/funcsig.csv` and
re-disassembled their actual ELF bodies with objdump:

| Function/site | Observed bytes and meaning |
| --- | --- |
| SaveGame `0xc7ec00` | `f3 0f 1e fa 55 48 89 e5`; serializer signature in Serializer.cpp |
| `0xc7ec2a` | `4c 8b 65 10`: SaveGameId is the seventh SysV argument, on stack |
| `0xc7ec5c` | `49 83 7c 24 08 00`: tests that path string's length |
| AutoSave completion `0x1019f40` | `f3 0f 1e fa 55 48 89 e5`; lambda(bool, optional<SaveGameError>) in GameUI.cpp |
| `0x1019f52` / `0x1019f55` | `48 8b 02` / `44 0f b6 26`: loads result through rdx and bool through rsi |
| `0x1019f81` | `80 b8 e8 0a 00 00 00`: tests CGameUI saving flag +0xae8 |
| `0x1019f8a` | `c6 80 e8 0a 00 00 00`: clears that flag |

Also reviewed [SAVE_NONBLOCKING.md](SAVE_NONBLOCKING.md) and
[MENU_GAME.md](MENU_GAME.md). The existing forced-autosave request and file
watcher are not NativeIo's correlated pause/drain/save/load/action-hold API.
These bodies do not establish that API's command-thread identity across world
destruction, operation-specific completion lifetime, or safe input suppression.
Reporting an arbitrary pthread CPU clock as verified engine work would be
incorrect. Thus 92af32e's native `cpu_ui/cpu_command/io_*` status publisher
remains unported with its existing controller prerequisite. Shared Python
liveness/count verification and helper sweeping are merged. No speculative
hook or false supported status is installed. Legacy Linux autosave polling
still has its earlier 90-second timeout.

## Workshop: attempted, still missing ownership evidence

The source signature identifies Linux `ModRep::RefreshModList(const
WorkshopModsResult&)` at `0x31ae800`, length 4910, in Lib/Util/ModRep.cpp.
`ModRep::GetWorkshopId` is at `0x31afe90`; its assert signature string is at
`0x4f8a800`. Actual RefreshModList disassembly confirms:

- `0x31ae82c`: `48 89 b5 98 fd ff ff`, saves the const result argument rsi;
- `0x31ae850`: `48 8d 35 c9 84 98 02`, references `0x5b36d20`;
- `0x31ae8a0`: `48 8d 35 59 84 98 02`, references `0x5b36d00`;
- `0x31ae938`: `f3 0f 6f 31`, loads control bytes for table iteration;
- `0x31ae954`: `48 c1 e0 05`, advances slots with a 32-byte stride.

This identifies a candidate equivalent, not a verified replacement for
Windows's Shadow(Result) contract. The Windows helper uses MSVC strings and
UTF-16 paths, a flat-map shadow and a ModRep+0xe0 catalogue. Linux result
ownership, path representation, backend prefix identities and catalogue
receipt offsets still need proof before injecting foreign storage. The new
unbounded Workshop registry hook therefore remains unported with the existing
registration feature. No guessed offsets or patch bytes are used in code.

## Modular-station topology limit

Compared incoming station_weld.h's degree/owned/remap algorithm with the Linux
transactional weld and the construction evidence above. The Linux predecessor
accepts its older depot endpoint/split shapes, including the CE frozen index
at +0x778, proposal frozen indices +0x220 and segment tags +0x270. Generalized
station compaction has no representative Linux fixture establishing which
indices/tags own foreign allocations across a multi-connector remap. Existing
proof for a single apron does not prove that extension. This batch removes
record ceilings on the supported shapes but keeps unsupported station shapes
unchanged. No guessed remapping is applied to engine-owned containers.
