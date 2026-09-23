# Linux construction, terrain and assets implementation

Implemented against Steam build 35924, build-id
`3a0e156390b0e6f1e372051c24802c8493ae454a`. The original layout evidence is in
`docs/re/linux/SLICE_CONSTRUCTION.md` and `SLICE_TERRAIN_ASSETS.md`.

## Enabled behavior

- Construction parameters are decoded from libstdc++ maps/variants, with booleans,
  finite numbers, lossless escaped strings and nested tables. Invalid variants,
  cycles, missing entries, excessive depth, and oversized output reject the whole
  capture. They never produce a partially reconstructed cancelled construction.
- Construction placement emits the existing Windows `ROADC` companion when
  street edges are present, followed by `CONXP` in the same inject transaction.
  Free-standing construction and stations with internal tracks use the same
  complete parameter walk. The former standalone-only Linux gate is removed.
- ModuleBuilder (`0xe4f6bd`), AddModuleComp (`0xf229a5`) and bulldozer module
  removal (`0xdd4e99`) emit `CONUP`. New internal station nodes do not disqualify
  an upgrade. AddModuleComp permits the proven empty completion callback; the
  other tools require their callback. As in Windows, CONXP/CONUP serialize the
  first added construction and CONUP uses the first removed ID. Capture is
  committed before cancellation.
- Multiplayer has one execution policy: cancel and replay. A decode, callback,
  or capture-file failure reaches the core's blocked outcome. No `NATIVE` or
  `ARMED 0` fallback is emitted. Solo commands pass through.
- Lua construction replay uses the Windows 0.4.22 template-weld algorithm with
  Linux offsets and ownership handling. Endpoint welds adopt the template apron,
  remap frozen-node indices and `segmentsBefore`, transfer its tag, and drop only
  final records. Split welds reconnect the template outer node, retain every
  construction index and inherit the removed edge's component fields.
- Terrain height/material grids and asset model groups are captured from the
  ProposalAction caller (`0xe59622`), committed before cancellation, and replayed
  into the empty Lua carrier (`0x1971333`) before its factory consumes it.
- TPTG preserves the Windows v1 format, including its 128-byte grid tail and
  112-byte debug context. Linux u64 mask storage is converted to canonical u32 wire
  words; unused bits are cleared. TPAS retains Windows's 511-byte string and
  20,000-model-per-group bounds. Malformed base64, dimensions, sizes, IDs, paths,
  transforms, extra bytes, or mixed proposal contents are rejected.
- Replays allocate everything before touching the carrier. Allocation failures
  release every new block and leave the proposal unchanged. An empty carrier
  consumes a successfully read file once even if decoding/install fails, matching
  unchanged 0.4.22 Lua. Nonempty proposals never consume the file.

## Allocation and exception boundaries

The DLL has a static C++ runtime, so allocating replay objects with its own `new`
would rely on an unproven match with the game's allocator. Calling the game's
ordinary allocating C++ helpers could unwind through the DLL with a foreign
runtime. The implementation avoids both problems:

1. Read the game's `_Znwm` / `_ZdlPv` GOT slots, whose PLT bytes are gated.
2. Locate the already loaded dynamic library owning both functions with `dladdr`.
3. Resolve `_ZnwmRKSt9nothrow_t` from that exact library and require that all three
   resolved symbols belong to the same library and ordinary new/delete match the
   game's GOT. The nothrow wrapper handles allocation exceptions inside the
   game's dynamic runtime.
4. Use that allocator for final CE, TransformedModel, Record48, string and vector
   storage. Build strings/vectors with the proven libstdc++ layout. Allocate at
   final addresses so SSO pointers and empty map sentinels remain valid.
5. Invoke only the CE default constructor, `0xdbec10`, which was independently
   disassembled from the actual game binary: 2233 bytes, no calls or branches,
   ending in `ret` at `0xdbf4c8`. It reads one RIP-relative float constant at
   `0xdbed41`; the rest is straight-line initialization. Its whole-body SHA256 is
   `89ed4a06c9a3f0c1b9aaccb2064e3e7c9f893e9accae6b3ca41da102bbbcc131`.
   Runtime additionally checks the whole-body FNV-1a64 `f1af220929f4f9d0`.

The constructor defaults and asset-specific overrides match the original map's
offset-by-offset analysis. No allocating game vector helper is called. Runtime
initialization verifies 149 instruction probes from the two layout documents and
the new UI provenance evidence below;
failure disables the affected area.

## Tool holds and object lifetime

Keeping just a tool pointer and checking its vtable later cannot distinguish live
storage from freed/reused storage. Holds are enabled only after installation of a
destructor observer. New static evidence from the same binary:

| Derived nondeleting destructor | Common base destructor jump | Bytes |
| --- | --- | --- |
| AssetBrush `0xdb98b0` | `0xdb9929 -> 0xe58980` | `e9 52 f0 09 00` |
| TerrainModifier `0xeb0490` | `0xeb05ac -> 0xe58980` | `e9 cf 83 fa ff` |
| TerrainPainter `0xebc790` | `0xebc856 -> 0xe58980` | `e9 25 c1 f9 ff` |

These are the first destructor slots in vtables `0x5a09980`, `0x5a0bcc0`, and
`0x5a0bda0`. The deleting slots call the respective nondeleting destructors.
`ProposalAction::~ProposalAction` at `0xe58980` takes `this` in rdi. Its first
15 bytes are `f3 0f 1e fa 55 48 8d 05 b4 20 bb 04 48 89 e5`.
The 5-byte near hook steals only ENDBR64 and PUSH RBP, stopping before the
RIP-relative LEA. The detour forgets the pointer before entering the destructor.
It holds no cleanup frame across the original destructor call.

Hold writes still require the proven callback manager/invoker and one of the
three known vtables. A prospective hold token is recorded before the completion
callback and activated only if it survives the callback; destruction inside that
callback invalidates it even when a new tool reuses the exact address/vtable.
Lua's completion marker only requests release; writes occur
on the thread which originally held the tool. The recurring Clock callback and
Add observer service release/4-second timeout. The hold blocks terraform and
asset erase. The game does not consult that flag to block terrain paint or asset
paint; the implementation does not claim that it does.

## Verification

`test_slice_construction_terrain` runs the guarded readers against real
libstdc++ fixtures, tests exact CONUP fields, nested parameter escaping and
rejection, TPTG boundaries across 31/32/33/63/64/65 bits, TPAS/base64 malformed
inputs, every allocation-failure position, read-only/nonempty carrier refusal,
and real vector/string destruction of the transferred storage.

Optional argument: the game's executable path. This maps its ELF segments into
an isolated test image, verifies and executes the actual CE constructor, and
tests initialized file replay using a separately loaded dynamic C++ runtime.
It also verifies cancellation record ordering, owner-thread marker release, and
destructor invalidation with identical reused storage/vtable. Both normal and
ASan/UBSan builds have passed. This is off-game ABI testing, not multiplayer
determinism validation.

## Template ownership and verification limits

The weld uses the existing 0.4.22 geometry predicates: at most 64 nodes and
segments, one construction, an empty terrain-align skip set, nearest matching
node within 15 metres, and the expected final template records. Cases outside
those predicates remain untouched, as in the Windows template helper. This does
not add capture fields or a different Lua protocol.

Linux-specific corrections are necessary for the same behavior:

- Node `+0x0c`, segment `+0x64` and owned `+0x74` are individual bool bytes.
  Windows's `0x7f00` stores include padding; Linux writes the effective false bool
  without assigning meaning to those padding bytes.
- Segment `+0x68` is `TickEpoch`, not a construction ID. Endpoint adoption copies
  the whole tail; split inheritance preserves the same high-word assignment as
  the reference implementation without inventing a new field.
- Endpoint adoption moves the apron object vector, clears its dropped slot and
  retires the old target vector through the game allocator. Split halves receive
  independent copies of removed-edge object lists. No aliased owners remain.
- Segment tags are libstdc++ strings. Moving a short string relocates its pointer
  to the final inline buffer. Moving a long string transfers its allocation and
  frees the former target only after commit.
- All validation, snapshots and allocation precede mutation. Invalid linkage,
  overlapping owned buffers, read-only mappings or allocation failure leave the
  proposal untouched. Only final records are removed; retained indices do not
  shift. The skip set's element count is `+0x250`, not its resize policy at
  `+0x260`.

Tests cover endpoint welds with short/long tags, object ownership, split
inheritance, every clone-allocation failure, missing allocators, alias rejection,
frozen-node guards and nonempty skip sets. Actual-ELF tests execute the verified
constructor and carrier hooks. These are off-game ABI tests; construction
placement/module editing and cross-platform map determinism still need live
multiplayer validation.

With `dumpprop=1`, construction-related BuildProposal entries save bounded
`construction_<instance>_<pid>_<sequence>_<caller>.txt` diagnostics before
move-out. Normal capture/replay does not require this option.

## UI parameter provenance correction (new static evidence)

The original map calls `ConstructionBuilder::Step`'s `rbp-0x1480` temporary the
evaluated Construction. That is incorrect. The Construction is the persistent
member at **tool+0x520**, immediately after ConstructionDesc at tool+0xd8.
The stack temporary is forwarded as the toolkit.

The complete path for `.con` files is:

1. All three direct UpdateConstruction call sites pass `r8 = tool+0x520`:
   `0xe30881 -> call 0xe30889`, `0xe33b48 -> call 0xe33b52`, and
   `0xe349e0 -> call 0xe349e7`; target `0xe2f330`. The callee saves r8 at
   `0xe2f35f` in `[rbp-0x5a8]`.
2. The non-`.mdl` branch (`0xe2f4bf -> 0xe2ffb0`) looks up the rep's desc and
   loads it at `0xe3016a mov r12,[rdx+0x20]`. Its evaluation function at +0x280
   is copied at `0xe30336..0xe30395`, including the invoker at +0x298. After
   copying the rest of the desc, `0xe305de` joins `0xe2fe8c`, where the params are
   copied and seed/paramX/paramY are added (`0xe2feca..0xe2ff20`).
3. `0xe2ff51` passes the desc's function, `0xe2ff58` passes the params, and
   `0xe2ff5e call [rbx+0x298]` evaluates into r15. The result is assigned to the
   saved Construction pointer by `0xe2ff64..0xe2ff6e call 0xe3ace0`.
   `0xe3acfb call 0xa3ebf0` copies the params Table at offset zero before moving
   its remaining vectors. The original map already proves that ConstructionRep's
   evaluator puts the evaluated params in that table (SC-CE-PARAMS).
4. Step pushes `tool+0x520` at `0xe355a1` / `0xe355dd` into `0xe2bbc0`.
   The helper receives it as its first stack argument (`0xe2bbe2`), saves it at
   `0xe2bc2a`, and forwards it as r9 at `0xe2c5f0` into `0x1646c90`
   (`call 0xe2c623`). That helper saves r9 at `0x1646cd9` and forwards it as the
   Construction argument rdx at `0x1647027` to MakeProposalAdd (`0x1647036`).
   MakeProposalAdd's copy into CE+0x448 is already proven in SC-CE-CONSTRUCTION.

Another correction: **UI placement toRemove is not universally empty.** After
moving the generated Proposal into `tool+0x850`, Step iterates IDs from
`tool+0x8f0..+0x8f8`. The fast append at `0xe35698..0xe356a5` and slow append at
`0xe356c1..0xe356cb` add them to Proposal+0x288. These IDs are not extra capture fields: the Linux port follows the unchanged
Windows CONXP/CONUP formats and the shared Lua placement/removal behavior. The
headquarters flag assigned at `0xe356f1` is likewise not a new wire field.

The UI parameter source's additional 32 byte probes cover this provenance chain.
They were checked against the actual build 35924 ELF. This resolves the parameter
provenance question. Template-weld geometry follows the existing Windows runtime
shape predicates described above.
