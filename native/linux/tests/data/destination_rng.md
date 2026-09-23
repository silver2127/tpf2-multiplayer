# Destination integer RNG compatibility (35924)

This is a native Linux compatibility change. The Windows 0.4.22 Lua files stay
unchanged. It gives every valid call to the common `Random<boost::mt19937>`
integer overload Windows interval semantics and corrects ten person seed calls.
Other engine types, distributions and seed functions are separate.

The Linux exclusive integer wrapper at RVA `0x31b79c0` calls the inclusive
sampler at `0x31b6070`. For width `W`, that sampler uses
`bucket = UINT32_MAX / W`, rejects raw MT words at or above `bucket * W`, and
returns `raw / bucket + minimum`. Even a width of one consumes a word.

Windows' corresponding wrapper `0x2374d60` calls `0x955010`. It uses modulo
with rejection above the last complete bucket of the **2^32** possible raw
words. A width of one consumes no words. Both engines use the same MT19937
word stream; the conversion into an integer interval differs.

The concrete raw word `46662977` gives:

| Exclusive interval | Original Linux | Original Windows / helper |
| --- | ---: | ---: |
| `[0, 10)` | 0 | 7 |
| `[0, 9999999)` | 108771 | 6662981 |

Both symbols identify the same `Random<boost::mt19937>(Engine&, int, int)`
overload. Its interval semantics do not depend on the calling subsystem.
This covers destination batch seeds (`0x1503956`), weighted destination choices
(`0x14fe99a`), idle car model/variant choices (`0x17199bc`/`0x1719a8c`), and
other consumers such as person creation. The Windows batch seed call at
`0x927877` uses `[0, 9999999)`; its worker adds the range's begin index to the
returned seed. Both batch and choice conversions must agree to reproduce the
same local MT streams and destinations.

The loader requires GNU build-id
`3a0e156390b0e6f1e372051c24802c8493ae454a` and verifies both complete native
wrappers before installing the hook. Every valid interval uses Windows modulo
and rejection. Invalid or empty exclusive intervals pass through the original
trampoline and native assertion path. Raw draws come from the original native
inclusive sampler's full signed-range branch, with unsigned subtraction of
`INT_MIN` to recover the raw word. The adapter does not recreate or change the
original MT layout or twist implementation.

`windows_uniform_int` tests signed limits, singleton behavior, power-of-two
ranges, rejection boundaries, successive draws and the concrete witnesses.
`destination_rng` executes copied native sampler bytes in an isolated fake
image to verify two ordinary caller contexts plus a direct function call,
signature refusal, singleton no-draw behavior and original state exhaustion.
Child-process fixtures confirm equal, reversed and signed-boundary invalid
ranges still reach the original assertion target. It does not load or modify
a game.

An independent local oracle on 2026-09-14 mapped the original Windows executable
without running its entrypoint and called `0x955010` with the Windows ABI.
All **3,978** interval/rejection cases matched this helper's returned value and
consumed-word count. Another **25,000** original Windows MT outputs matched
the standard MT19937 stream across five seeds and repeated state exhaustion.
A second oracle executed the complete Windows exclusive wrapper `0x2374d60`:
all **37,762** forced-word/rejection cases across **142** valid signed intervals
matched the actual helper's return value and draw count, including widths of
one and `2^32 - 1`. The local reproduction harnesses and results are under
`~/.cache/tpf2mp/people-destination-review/` as
`windows_sampler_helper_oracle.cpp`, `windows-sampler-helper-results.txt`,
`windows_exclusive_wrapper_oracle.cpp`, and `windows-exclusive-wrapper-results.txt`.

The follow-up departure seed adapter fixes a separate platform difference in
`SimPersonSystem::NoteAtBuildingPersonsLeave`. Native's constructor call at
`0x171197a` receives
`low32((sign_extended_time + 0x2853a3c768) ^ 0x9e3779bd)`. This corresponds to
classic `hash_combine` over tag 4 and the time, with identity integer hashing.
Windows' function `0xa93610` instead hashes both integer values with FNV-1a over
their four little-endian bytes. At time 2800, the seeds are `0xcd94abe5` on
native and `0x37775b60` on Windows.

The native seed formula is invertible in unsigned 32-bit arithmetic. The
adapter recovers exactly the original time bits, computes the Windows seed,
then calls the unchanged native MT constructor. It does not read or change the
clock. The analogous arrival formula uses tag 3. The other eight person
callbacks combine a tag, time and entity ID; the original RAX or RDX register
still contains the pre-entity intermediate, or the exactly reversible
`signext(entity) + K + (intermediate << 6)` value. The intermediate is below
2^38 for every signed int time and these tags. Recovering it preserves all bits.

A separate hook on constructor `0x170a7d0` verifies its complete 151 bytes and
all ten call windows before accepting these returns:

| Tag | Person callback | Constructor return RVA |
| ---: | --- | --- |
| 1 | Line changed | `0x1713a2d` |
| 2 | Line assignment changed | `0x1715fc8` |
| 3 | Walk arrived | `0x171760b` |
| 4 | At building leave | `0x171197f` |
| 5 | Line removed | `0x1713498` |
| 6 | Vehicle changed | `0x1716982` |
| 7 | At terminal changed | `0x17165c5` |
| 8 | At vehicle changed | `0x17112de` |
| 9 | Idle changed | `0x1718ce9` |
| 10 | Move path changed | `0x1718443` |

The assembly entry snapshots original RDX, RAX, RBX, R12 and R15 before C++ or
the constructor can overwrite them. Five pushes align its C++ call and retain
the original return PC immediately above the snapshot. Each verified context
pins which register contains the entity pointer and time intermediate.
Unrecognized callers never dereference those registers. The constructor's
seven stolen bytes stop before its original relative jump. The MT algorithm
and all other callers retain their original behavior.

`windows_person_seed` includes golden outputs from original Windows machine
code, signed boundaries and wraparound. The isolated `destination_rng` hook
test compares all 624 MT state words and the index with states captured from the
original native constructor before hooking. All eight entity gates are tested
with four signed-boundary time/entity pairs and poisoned unused registers;
other callers and every signature refusal are also checked. The actual helpers
matched **100,014** departure, **100,014** arrival, **100,196** idle and
**701,372** other-tag inputs of independent original-Windows-code oracles.
Original Windows and native MT
constructors also produced identical complete states for **10,008** identical
seeds. These oracle results are stored alongside the integer oracle as
`windows-person-seed-oracle.json`, `windows-person-seed-helper-results.json` and
`windows-native-mt-ctor-oracle.json`.

Complete simulation parity still depends on other engine overloads, seed
formulas, distributions and collection order. This integer hook establishes
the common MT overload's interval behavior; it does not establish those other
properties.
