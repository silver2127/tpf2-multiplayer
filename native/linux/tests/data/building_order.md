# Windows building registration order

The same live baseline has 486 building construction resources on both platforms,
with identical names, metadata, construction IDs, sort priorities and start years.
The internal BuildingTypeRep order differs. For the first new level-one 3x4 home,
Linux enumerates era-A variants `01,04,03,02`; Windows enumerates `01,02,03,04`.
The same sampled candidate index consequently chooses `04` on Linux and `02` on
Windows. Their model/particle counts differ, affecting later entity allocation.

This is an unstable-sort implementation difference. Both GetConstructionTypes
implementations enumerate increasing construction IDs and compare signed
`(priority, yearFrom)` only. There is no filename or ID tie-break. Linux GCC's
sort and the shipped Windows MSVC sort permute equal keys differently. Equal-key
order must therefore match the original Windows algorithm, including its
32-element insertion threshold, median-of-three/nine partition, three-quarter
partition budget, and heap fallback. Merely adding a lexical tie-break would
happen to fix the current vanilla list but would change modded behavior.

The Linux hook is limited to BuildingTypeRep's constructor. At RVA `1868223`,
immediately after `GetConstructionTypes` returns, its private vector at
`RBP-130` contains construction IDs. Sorting those unique numeric IDs restores
the pre-sort enumeration. The Windows sort is then applied using the original
ConstructionRep metadata. This happens before any building records, indices or
name maps are registered; the original constructor builds all of them normally.
No construction/model resources or existing entity components are rewritten.
Both GetRandomType (`1867b90`) and GetReplaceTypes (`1867e70`) enumerate this
catalogue through `1867320`, filter it through `1867730`, and sample through
`1864c40`. Thus initial builds and upgrades share the corrected order.

The two other direct GetConstructionTypes callers (`d1d1ca`, `d1e158`) belong to
map-generation industry selection and remain outside this building-specific
hook. This change does not claim all resource-list ordering is cross-platform
compatible.

Verified original Linux build ID:
`3a0e156390b0e6f1e372051c24802c8493ae454a`. The installer checks all 1,960 bytes of
constructor `1868090`, all 762 bytes of getter `33106f0`, and all 259 bytes of its
comparator/unguarded insertion helper `330ee60`. Only the seven-byte
`MOV RDI,[RBP-110]` at `1868223` is displaced; the trampoline replays it. The
mid-function stub preserves every GP/XMM register, arithmetic flags and RSP,
dynamically aligns its local call, and allocates no memory or calls into game
code. Comparator fields are native ConstructionDesc `1f8` and `28`; Windows
uses `208` and `28`.

Windows references: getter `2463740`, sort `2457590`, partition `2457060`,
median helper `2456e40`, heap adjustment `2457430`. Registration calls the
getter at `b6d328`. The full captured catalogue is the fixed fixture in
`building_order_fixture.h`; it records native order and independently observed
Windows IDs, not the algorithm's own output.

The host test executes the installed patch and its real seven-byte trampoline
in a private mapped image. It checks 48 cases across both stack alignments,
empty/singleton lists and sorting thresholds through 1,024 entries, all GP/XMM
registers, flags, MXCSR, RSP, unchanged metadata/payloads/vector structure, and
all 486 independently captured Windows IDs. Another 1,025 sparse-ID cases cover
signed/extreme priorities, duplicate keys and forced heap fallback. Wrong
build IDs and altered constructor/getter/comparator bytes fail before patching.

An independent oracle maps and executes the original Windows sort/partition/
median/heap machine code, with only the resource getter and memmove bridged to
fixture data. All 1,928 cases (520,827 items) match the helper's exact ID
permutation, including 486 forced-heap cases, equal/mixed/signed-extreme keys,
ascending/descending and shuffled inputs, and sizes around 31/32/33 and
40/41/42 through 1,000. Both the internal sorter and public ascending-ID reset
were checked. The oracle required no source correction. Audit artifacts:
`~/.cache/tpf2mp/town-seed-audit/building-oracle.py`,
`building-oracle-wrapper.cpp`, and `building-oracle-report.json`.
