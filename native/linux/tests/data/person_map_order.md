# Temporary person-map traversal parity (native build 35924)

During the first cargo-terminal replay, both platforms removed the same 94
entities, including seven residents, but retired those residents in different
orders. The affected buildings were 20183, 20835 and 20842. At the later
construction, matching new residents received different recycled IDs. The
original first-terminal trace also captured different temporary-map orders for
identical sets of affected people. This module addresses that second ordering
stage; the separate resident-hash module addresses the source phmap set.

`MakePersonCapacityData` (native `2e69d00`, Windows `212a700`) and the other
`SimEntityUpdateHelper` gatherers put affected people into five temporary state
maps. `ApplySimPersonData` (native `2e6b110`, Windows `2125a90`) walks those maps
without sorting. Native libstdc++ and Windows MSVC unordered maps have different
hashes, bucket policies and iteration order. Changing a resident-set hash alone
does not make the temporary maps equivalent.

## Original Windows oracle

Original executable SHA256:
`782b904a8f7bbdac1f7a18528f1a5c778691e5aa3087c37c351bf6912585175c`.

The isolated machine-code oracle executes original Windows `21239c0`
(operator[]), `211fbc0`/`212e630` (node initialization), `21202f0` (bucket/list
insertion and recursive rehash), and `d4e10`/`c8210` (bucket reset/fill). Only
operator new at `2bf3a80` is replaced with host allocation. Preallocated backing
storage avoids external bucket allocation while the original bucket reset,
list splicing, collision handling and rehash remain executable game code.

The portable model and the actual compiled `Tpf2mpWindowsPersonMapOrder` both
matched **1,088 cases / 575,042 input insertions**, including duplicates, signed
bit patterns, collisions, reversed sequences and every growth boundary through
10,000 entries. Ten original-machine-code golden sequences are checked into
`person_map_order_fixture.h` for repeatable tests without a game executable.
Full oracle scripts/results are preserved in the local desync-fix cache under
`post-release-desync/removal-order-audit`; they are diagnostics rather than
installed game code.

Windows hashes four little-endian ID bytes with FNV-1a64, starting from
`cbf29ce484222325` and multiplying by `100000001b3`. It starts with eight buckets
and a maximum load factor of one. New buckets append to global iteration order;
new unique IDs prepend within an existing bucket. Duplicate insertion does not
change order. Growth multiplies bucket count by eight below 512 buckets, then
by two. Rehash processes the existing global list in order and groups it by
these same rules. The complete first-insertion history matters.

## Implementation and lifetime

The original maps and nodes remain owned by the game. No native map links,
bucket predecessor pointers, keys, payloads or hash fields are changed. Private
records maintain Windows order and contain pointers to the original nodes.
Only the five initial iterator loads and five next-node loads in the verified
Apply function are redirected to those pointers.

| Purpose | Native instruction window |
| --- | --- |
| Bind newly initialized PersonData allocation | `2e73964`, 7 bytes |
| Forget allocation before its real deleter | `2e673c9`, 6 bytes |
| Capture successful operator[] return | `2e669c0`, 14-byte entry detour |
| Initial heads, state maps 0–4 | `2e6b24c` / `2e6b886` / `2e6bb70` / `2e6c1d5` / `2e6cf57` |
| Next nodes, state maps 0–4 | `2e6b879` / `2e6b963` / `2e6c1c8` / `2e6cf4b` / `2e6d0a9` |

`SimEntityUpdateHelper` constructs PersonData at `this+120`. Its actual aggregate
deleter `2e673c0` is reached on normal destruction (`2e6f2e9`), constructor
exception cleanup (`89b135`), and Apply exception cleanup (`89ac9d`). Those
complete contexts are pinned alongside the constructor, Apply function,
operator[] and deleter. This gives the records an allocation lifetime, including
nesting and cross-thread transfer; no timeout or stack-address guess is used.

The insertion wrapper has only trivial locals and performs no state change
before calling the original function. A foreign game allocation exception can
unwind that ordinary CFI frame; the original constructor cleanup reaches the
real deleter and discards the private records. The wrapper adds no C++ cleanup
personality from the loader's static runtime. Internal callbacks use C
allocation and a mutex without calling game functions while locked.

Before traversal, all five maps are checked against the captured counts, keys
and original node identities. An incomplete owner retains native traversal as
a whole and reports an error. Each distinct runtime capture/lifetime failure
is logged once through the boot logger supplied by
`Tpf2mpPersonMapOrderSetLog`; successful callbacks emit no logs. All 13 hooks are
checked before patching, and installation failure restores every attempted
window and identifies incomplete rollback.

The common inline entry preserves every general register, all 16 XMM registers,
flags, MXCSR and stack position apart from the documented loads. Original TEST
instructions run from their trampolines to produce exact flags. The final
next-node site resumes at its original shared test block; its relocated copy
of the original relative jump is never executed.

## Tests and remaining scope

`person_map_order_test.cpp` executes **192 actual installed shims** across all
12 inline sites, both stack alignments and varied register/MXCSR states. It
checks original node bytes remain intact; all five state maps; duplicate
lookups; nested owners; 50 allocation-address reuses; 300 threaded owners;
cross-thread handoff; incomplete-capture behavior and once-only error logging;
changed-context refusal; and all 13 installation rollback points. The separate
`person_map_foreign_unwind_test.cpp` exercises actual foreign runtime exceptions
through the insertion wrapper and original-style owner cleanup.

This module reproduces Windows map ordering **for the recorded insertion
sequence**. `GetSimPersonsForTarget` is a separate unordered source visited
before the resident set; later network/line gatherers also contribute IDs.
Their complete insertion streams must be compared before claiming the combined
fix achieves parity. The additional input trace and a clean replay remain
required. The Windows 0.4.22 Lua files are unchanged.
