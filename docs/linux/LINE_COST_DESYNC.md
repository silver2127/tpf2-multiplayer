# Public transport cost desync — 2026-09-15

## Reproduction and cause

Replayed the user's ten original commands from the identical pre-terminal save:
two street terminals, a line and stop updates, a depot, two vehicle purchases,
and two vehicle-to-line assignments, at their original t=1818–1844.4 stamps.
Before the fix, full person state and transforms match at t=1914; person 25872
first differs by t=1915, choosing public transport on Linux and driving on
Windows after its t=1914.4 destination update. Vehicle desync appears later.
Read-only debugger traces show matching walk/drive costs and destination access
costs, but different reachable-line costs.

Linux's shared LineSectionData cost helper at RVA 0x154f9e0 hashes integers by
identity; Windows's equivalent inline block at RVA 0x96f68a hashes the person
and line as four little-endian bytes and the stop index as two bytes with
FNV-1a64, then combines all three hashes. The existing five walking/driving
hash windows do not cover this helper, called from the line path expander.

## Change

Added a sixth instruction-guarded window at 0x154fa14–0x154fa3e. It recovers the
person from RDX, reads the line/stop from the verified section prefix at RSI,
and supplies the Windows hash in RDI. The game's original SSE arithmetic,
modulo reduction, route search, and all 24 Windows 0.4.22 Lua files remain intact.

## Validation

- Original Windows instruction oracle: 100,245 input triples match the actual
  C++ helper, including negative integer representations and 16-bit boundaries.
- All six installed stubs tested at both stack alignments, preserving live
  GP/XMM registers, flags, MXCSR and stack; rollback tested at every site.
- All 29 soldier SDK CTests pass.
- The first six-window candidate fixes t=1914.4, but a later mismatch remains
  at t=1951.2. It must not be treated as a complete fix.
- Further tracing found identical LineSectionData and differing PathFactory
  seeds. Added guarded windows for time/batch hashing (0x14e15f3–0x14e161c)
  and person/entity-revision hashing (0x14e17d1–0x14e17e0). The latter supplies
  FNV inputs to the original combine operations and replays the intervening R9
  stack load; the complete Revision was saved before the patch.
- Both Windows PathFactory blocks (0x90e98e–0x90ea2d and
  0x90ec6d–0x90ed0e) match the helper in 100,049 cases each. Together with
  the line-cost oracle this is 300,343 original-instruction cases.
- All eight stubs pass register/stack/MXCSR checks and every rollback point.
  The eight-window candidate matches sampled movement through t=2124, but
  two walkers receive exchanged departure times at t=2125.8 and t=2130.6.
  Vehicle warnings return during the longer run. This candidate is incomplete.

Run the isolated oracle with a locally installed, hash-verified Windows build:

```sh
python3 tools/linux/check_line_cost_oracle.py /path/to/TransportFever2.exe
```

Private reproduction evidence and failed-world saves are under
`~/.cache/tpf2mp/resume-20260915/desync/`. The oracle script requires x86-64 Linux
and g++; it never starts the game or loads game imports.

## Path restoration order

The two walkers (22776 and 25697) arrive in the same step near t=2084. The
arrival vector is `[22776,25697]` on Linux and `[25697,22776]` on Windows.
Consequently, waits of 46.42832946777344 and 41.50788497924805 seconds are assigned
to opposite people. The RNG values themselves match.

Tracing the movement node list finds the first ordering difference after the
first terminal command. The path restoration pass removes/re-adds the same
ModelPerson components in a different order. The affected-person set is built
from completed worker batches at native 0x2e73e63–0x2e73f78, then traversed to
prepare the replacement components. This set was separate from the already
adapted affected-network set later in the helper.

Windows 0x2127f40 constructs this set with eight buckets, visits each batch and
entity in order (0x2128170–0x21281ee), and uses the original 0x3dc240/0x428be0
insertion/adoption path already validated by the range insertion oracle.
The native adapter now also reconstructs this set's Windows order from its
completed batches, replacing only its first/next traversal windows at
0x2e73f78 and 0x2e74187. Native set contents and links stay intact; the existing
destructor hook releases either tracked collection, including unwind cleanup.

All five traversal/cleanup stubs pass ABI checks at both stack alignments,
all five installation failure points restore original bytes, and all 29 SDK
tests pass. The movement diagnostic now also captures building waitingTime
(schema 4), so an invisible waiting-time mismatch is detected before departure.
That candidate still exchanged waiting times for people 25082 and 26222.
The t=2084 snapshot catches the mismatch before either person departs.

## Preparation pass order

Read-only traces confirmed that all source batches and the complete affected-
person set traversal now match Windows. A later `PrepareSimPersonData` pass
(native 0x2e70960; Windows 0x212ca50) still traversed the five private person
maps using native linked order. Its removal sequence was 20853,20831,20852,
versus Windows 20852,20831,20853; subsequent additions matched. Removing a
movement node swaps the last node into its slot, so this difference persists
in movement/arrival order and later assigns random waits to the wrong people.

The existing captured Windows map order was used by `ApplySimPersonData`, but
not by preparation. Added first-node and both next-node paths for each of the
five maps in preparation (15 instruction-guarded windows). Both stages now
reuse the same recorded insertion/rehash history and original native nodes.
The original map contents and component operations remain in place.

All 28 installation stages pass rollback checks. The 27 assembly stubs pass
432 GP/XMM/flags/MXCSR/stack checks, and all 29 SDK CTests pass. All 24 installed
Lua files remain identical to Windows 0.4.22.
Current combined candidate boot SHA-256:
`895f62687b0b305029312a111f245bc4e874b37faff123371a54fa93659b4904`.
The exact replay matches current person state through t=2160, excluding only
previous-frame interpolation fields `walker.dist0` and `movePath.dyn0`; all
world transforms match at those samples. At t=2280 two destinations differ:
the same new industrial construction is entity 28434 on Linux and 28435 on
Windows. More person differences appear by t=2400. Vehicle positions still
match in the recorded samples through t=2472. This candidate remains incomplete;
entity allocation during town growth is being traced. Evidence is under
`prepareorder-validation/` and `entity-allocation-trace/`.

## Town tree draw order

With both traversal fixes, current person state matches through t=2160, but
the t=2280 snapshot exposes destination 28434 on Linux versus 28435 on Windows.
Read-only allocation traces show the first difference during residential town
growth: IDs 28413–28420 match, then Linux creates three tree asset groups while
Windows creates four. Subsequent allocations shift by one. The new industrial
building has the same file, parameters, seed and position but a different ID.

CreateBuildingAssets calls native unit-float RNG at 0x1546ae7 for rotation and
0x1546b39 for scale. Windows calls its float distribution at 0x95cde5 for scale
and 0x95cdfa for rotation: scale is `[0.75,1.25]`, rotation is `[0,2*pi]`.
Swapped draws change tree transforms and whether a tree passes placement tests.
The earlier tree adapter covered only the integer model-selection draw.

The guarded 90-byte native window 0x1546ae4–0x1546b3e now draws scale first,
then angle. It uses the already adapted common unit-float core at 0x31b5fc0,
whose full body and constants are verified before installation. This also
preserves Windows's rounded 1.0 endpoint, unlike the unadapted private unit
core at 0x153e8d0. The original sincosf call and matrix arithmetic remain.
The original cosine output slot at RBP-0x1848 temporarily holds scale;
sincosf writes directly to final cosine/sine slots at -0x1860/-0x185c.
No shared RNG state or deferred pairing is introduced. Both tree windows roll
back together on write failure.

Validation:

- The installed draw window runs against the original native MT/float machine
  code in 4,026 cases, checking outputs, all MT state bytes, frame writes,
  nonvolatile registers and stack balance. Cases include draw pairs rounding
  to 1.0 and a twist between the two draws.
- Both installation stages are tested for complete and partial write failure.
- `tools/linux/check_tree_float_oracle.py` executes the original Windows pair
  of call instructions, distribution arithmetic and MT twist in 10,025 cases.
  Only log/floor setup for the known single-u32 float draw is elided; all
  results and complete MT state agree, including forced endpoint cases.
- All 29 soldier SDK CTests pass; all 24 installed mod Lua files are unchanged.
- Exact command replay with boot SHA-256
  `78d89ebfdeb4edf650e50735995bed535fcd005d97dcc9bb43d6cdbf830655e8`
  passes all 12 current-person-state snapshots through t=3600. All 13 entity
  records captured for IDs 28413–28425, including all four trees, now agree
  exactly. The completed replay has zero desyncs and zero measured vehicle
  separation; all common geometry hashes and spatial person counts agree.

Evidence: `~/.cache/tpf2mp/resume-20260915/desync/tree-draw-validation/`.
