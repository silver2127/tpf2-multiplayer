# Inline tree selection compatibility (native build 35924)

The town-seed compatibility run preserved every original person through the
first town expansion, but newly allocated entity IDs differed. An allocation
snapshot identified two separate issues: native had allocated eight more salmon
before the new road, and decorative/building choices also differed. This patch
addresses only the independently proven inline tree selection distribution.
It does not claim to repair animal spawning or the residential building choice.

Native `CreateBuildingAssets` at RVA `1546670` seeds a local Boost MT from parcel
coordinates. Its tree model sampler is inlined, so it bypasses the existing
compatible `Random<BoostMT>(int,int)` wrapper. At `1546a18` it computes
`bucket = UINT32_MAX / count`; it rejects draws at or above `count * bucket`,
then selects `raw / bucket` at `1546a91`. A singleton still consumes a draw.

The original Windows counterpart at RVA `95c810` instead implements the proven
Windows modulo distribution at `95ccf6..95cd74`: a singleton returns zero without
a draw, other widths reject the incomplete tail of the 32-bit source range and
select `raw % count`. This finding came from the shipped Windows instructions,
not an assumption about a library version.

The hook replaces the fourteen-byte instruction sequence at `1546a0f`, before
any draw, with an indirect jump. It resumes at `1546a97` with the selected index
in EAX and the original vector begin in RSI. Native CDQE, model/string selection,
proposal construction and all other construction behavior continue unchanged.
All other GP registers, all sixteen XMM registers, flags and the interrupted
stack are preserved. Skipped native temporaries RCX/RDX/R12 and the local bucket
slot are dead or overwritten before their next use.

The existing pure Windows inclusive integer helper supplies the mapping. Its
raw callback executes the original native inclusive function `31b6070` with
`[INT32_MIN, INT32_MAX]`, reversing the signed offset using unsigned arithmetic.
This retains original native MT storage, twists and state advancement. The hook
has no dependency on the common integer hook being installed. The full 279-byte
inclusive function, 270-byte original twist (`14ed8d0`), complete 2,115-byte
CreateBuildingAssets function and the exact ELF build ID are verified before
installation. The zero-width wrap case retains a full raw uint32 draw, and all
nonzero uint32 widths use defined signed-bound translation.

The host fixture privately maps the original verified native instructions and
executes the installed jump with every GP/XMM register live. Its 120 cases cover
both stack alignments, count boundaries, singleton no-draw, controlled rejected
raw tails, flags/MXCSR, adjacent stack memory and every MT state word/index.
Another 25,000 samples cross original native twists and compare results and the
complete state to an independent MT/modulo reference. Fixed original-Windows
witnesses include raw 46,662,977 selecting index 7 for count 10 and 6,662,981 for
count 9,999,999. Wrong build/context/MT bytes are refused; simulated unavailable
and partial writes leave the original entry intact, with explicit status.

No game installation, running process, save or shared Windows 0.4.22 Lua file
was changed by the fixture. A combined live test remains an integration step.

The independent original-Windows oracle executes the actual inline selection
`95cc81..95cd76` and original 665-byte MT twist `918b40`, including its SSE path.
The landed helper, using the original native raw sampler/twist, matches all
128,000 seeded results and every byte of the 2,504-byte MT state, plus 118
controlled tail/singleton/full-span cases with identical draw counts. Evidence:
`~/.cache/tpf2mp/town-seed-audit/tree-oracle.py` and `tree-oracle-report.json`.
