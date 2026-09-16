# Resident-set hash parity (native build 35924)

The first terminal demolition produced the same 94 removed entity IDs on both
platforms, but a different order for the seven removed residents. The captured
MakePersonCapacityData input was `[20183, 20835, 20842]`. Its raw resident sets
were:

| Destination | Native raw slot order | Windows raw slot order |
| --- | --- | --- |
| 20183 | 20853, 20851, 20852 | 20853, 20851, 20852 |
| 20835 | 20840, 20839, 20841 | 20841, 20840, 20839 |
| 20842 | 20858 | 20858 |

These are the actual engine tables returned at native `2e6a40e` / Windows
`212afd0`. The Lua `getSimPersonsForDestination` binding copies and rehashes its
result, so its iteration order cannot establish the engine's physical order.
The trace is archived under `post-release-desync/first-terminal-trace` in the
local desync-fix cache. Both breakpoint sets were removed and verified afterward.

Native resident IDs are hashed by sign-extending the int32 ID. Windows hashes
its four little-endian bytes with FNV-1a64 (basis `cbf29ce484222325`, prime
`100000001b3`). Both then multiply by `de5fb9d2630458e9` and add the low/high
halves of the unsigned 128-bit product. The phmap probe algorithm, control-byte
fingerprint, growth policy and lack of a process-address salt are otherwise the
same in the inspected implementations. The helper uses unsigned arithmetic,
including negative-ID bit patterns, without floating point or locale behavior.

The five substitutions affect only the inner resident set:

| Native patch | Function / purpose | Input | Replaced outputs |
| --- | --- | --- | --- |
| 1717127, 14 bytes | AddToDestination, existing destination | R9 ID | RAX hash, RCX hash >> 7, RDX capacity + 1 |
| 1717338, 10 bytes | AddToDestination, new destination | R9 ID | RAX hash, RCX hash >> 7 |
| 170adde, 15 bytes | RemoveFromDestination, lookup before erase | R9 ID | RAX/RDI hash, RDX capacity, R10 capacity + 1 |
| 1721148, 6 bytes | Shared resize, reinsertion | int32 at R14 | R8 hash |
| 17212f4, 7 bytes | Shared tombstone cleanup | slots[RBX] in R13 set | R15/RSI hash |

The last two helpers also serve Lua/private copied sets. A sixth hook at
`1721480` establishes a scope only for the native engine insertion return PC
`17171d9`. Each shared inline adapter additionally checks the exact set pointer.
Other caller paths retain their original instructions through a trampoline.
This preserves the consistency of private sets whose own lookups still use the
native hash. The outer destination-to-set map remains native throughout.

Full original function bytes are pinned for AddToDestination,
RemoveFromDestination, resize, tombstone cleanup and prepare-insert, in addition
to the ELF build ID `3a0e156390b0e6f1e372051c24802c8493ae454a`. All guards run
before any patch. Failed installation restores every attempted window and
reports any unsuccessful restoration explicitly. Installation is intended for
loader initialization before game entry, not for a running simulation.

All inline adapters preserve fifteen GP registers, flags, sixteen XMM registers
and exact stack position, except the documented replacement outputs. The
integer-only helper leaves MXCSR untouched. The scope uses a cleanup-only frame
with the game's dynamic exception personality and unwind-resume function, so a
foreign engine exception restores TLS without entering the loader's static
runtime cleanup machinery.

The actual compiled hash helper also matches the original Windows
`a90466..a904c0` instructions for 100,029 random and signed-boundary inputs.
The independent Windows oracle generated 300 checked-in full table states in
`resident_windows_oracle.h`. The committed native fixture executes the original
native prepare-insert, probe, resize, complete RemoveFromDestination and
compaction code, substituting only CRT allocation/memory leaves. All 300 states
match exactly: counts, capacity, growth budget, every control byte and every
occupied slot/ID. A 4,000-ID growth/erase/compaction/reinsertion sequence also
passes, and the captured home20835 three-resident order is reproduced.
The fixture bytes and SSE constants are in `resident_native_helpers.h`.
Oracle artifacts are under `post-release-desync/removal-order-audit/resident-oracle`
in the local desync-fix cache, including `candidate-hash-report.json`.

`resident_hash_test.cpp` executes the real installed stubs, including the short
near-relay/trampoline paths: both stack alignments; signed-ID boundaries; all
GP/XMM registers, flags and MXCSR; genuine gated and unrelated caller PCs;
nested exact-pointer scopes; three threads; refusal of changed complete
contexts; and all six installation failure points. The separate
`resident_foreign_unwind_test.cpp` covers foreign runtime exceptions and scope
restoration.

This fixes the first ordering stage only. MakePersonCapacityData subsequently
inserts residents into temporary `std::unordered_map` instances, whose Linux
and Windows iteration orders also differ. That second stage requires its own
compatibility change; resident hashing alone does not fix the entire retirement
order.
