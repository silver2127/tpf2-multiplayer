# Scoped animal RNG compatibility

This change is for the native build with ELF build ID
`3a0e156390b0e6f1e372051c24802c8493ae454a`. The reference Windows executable
has SHA-256
`782b904a8f7bbdac1f7a18528f1a5c778691e5aa3087c37c351bf6912585175c`.
All addresses below are image-relative. Installed Lua and capture fields are
unchanged. Live validation of this animal candidate is pending.

The native animal code uses `std::default_random_engine`, a 31-bit LCG with
multiplier 16807 and modulus 2147483647. The corresponding Windows code uses
standard MT19937. The initial spawn sequence therefore selected different
valid positions: the observed first salmon cohort contained 41 native versus
33 Windows entities, shifting subsequent road entity IDs by eight. This
observation establishes the mismatch; the live test must still verify the
combined correction.

## Seed and engine boundaries

| Context | Native | Windows | Windows seed |
| --- | --- | --- | --- |
| Animal movement worker | `1674c60` | `a31a10` | 32-bit result of classic 64-bit hash-combine of FNV-1a hashes of the four little-endian bytes of `begin` and `capture.time` |
| Animal spawn manager | `178c620` | `ae0c10` | Supplied seed truncated to 32 bits, without hashing |

The native worker owns `[RBP-0xf0]`; its seed store is `1674d5d`.
The spawn manager owns `[RBP-0x168]`; its seed store is `178c79e`.
Entry wrappers create a private MT engine for that invocation. The verified
seed-store patches bind the private engine to the exact local LCG address and
reset it at each original initialization. The LCG storage retains its native
layout and seed. An unbound pointer continues through original native code.

The Windows MT implementation uses a doubled 1248-word backing buffer, while
the private engine uses the toolchain's standard MT19937 implementation.
Their observable output streams, including repeated twists, are verified by
executing the original Windows initializer and draw code.

## Complete consumption and escape inventory

The full native worker and all transitive consumers of these local engine
pointers were inspected independently. There are exactly five unit-float
draw sites and three integer draw sites:

| Draw sites | Native implementation | Reachability |
| --- | --- | --- |
| `1674faa..167501c` | Inlined LCG unit float | Movement worker |
| `1786b94`, `1786bb7` | Shared unit helper `1786a10` | Worker random-target helper `1786b40` |
| `1787858`, `178794b` | Shared unit helper `1786a10` | Position helper `17876c0`, reached from worker and spawn |
| `1675a06`, `1675e21` | Shared integer helper `d6f8a0` | Movement worker |
| `1787b71` | Shared integer helper `d6f8a0` | Position helper, weighted candidate selection |

Worker local alias `[RBP-0x148]` is assigned at `16759ff`, `1675ca0`, and
`1675ddc`. Its remaining consumers are position-wrapper calls `1675cb9`,
`1675de3`, `1675e76`, and random-target call `1675d05`. It is not returned
or stored into a persistent object.

The spawn pointer is copied into local capture fields `[RBP-0x100]` and
`[RBP-0x90]`, then passed to `178c4f0` at `178cb5a` and `178cca3`.
That helper keeps it at `[RBP-0x70]` and forwards it to position selection
at `178c583`. Entity creation at `178b330` receives the resulting position
and metadata, without an engine pointer. The adjacent `std::function` is a
copy of the caller's callback; its `178c903` invocation receives the animal
type, without this local engine. Both captures are consumed synchronously.
No other numeric state reads, engine copies, or asynchronous escapes were
found in these two scoped paths.

Both distribution types consume the same private MT stream. Windows unit
floats retain the original unsigned conversion, zero subtraction/addition,
and division by 2^32, including directed rounding and its possible 1.0 result.
Windows integer draws use modulo/rejection over the full 32-bit raw range;
singleton ranges consume no draw. The original Windows standard-library
adapter `38f490` independently confirms the existing inclusive helper's
mapping, although it differs from the native LCG distribution.

The shared integer hook handles only valid bounds on a bound engine.
Other requests call the original distribution. Its recursive call returns
at `d6f9c8`; that exact internal caller always remains native, so a delegated
request cannot change engine midway through its recursive subranges.

## Scope lifetime, ABI and refusal behavior

Seven patches cover the two entry wrappers, two seed stores, shared float,
inline float, and shared integer helper. Build ID, original instruction
contexts, the full 472-byte integer helper, and native float constants are
checked before any patch is applied. Each stolen window ends on an
instruction boundary and contains no unrelocated relative operand. Failed
installation attempts restore every attempted window; an incomplete
rollback retains required trampolines and disables all overrides.

The three mid-function patches preserve all general registers, XMM registers,
stack alignment, and flags. Only the inline float result replaces XMM0;
the resumed comparison overwrites flags before consuming them. The original
seed stores and fallback inline math are replayed by their trampolines.

TLS scopes are nested and thread-local. They are removed on normal return
and on a game exception. A cleanup-only assembly frame uses the game's
dynamically resolved personality and `_Unwind_Resume`, restoring the previous
TLS scope before resuming the same exception. Wrapper locals are trivially
destructible, so the loader's hidden static C++ exception runtime is not
placed on the game's unwind path. A completed scope never remains eligible
when a later LCG reuses its stack address.

## Independent checks

The isolated original-Windows oracle retained original initializer
`ae0ced..ae0d40`, worker hash `a31aeb..a31b92`, and all twist, tempering and
float arithmetic in `14e1f7..14e398` (with the original helper's fixed setup
constants). It passed:

- 320,000 seeded float-bit comparisons over 128 seeds and repeated twists.
- 8,064 forced raw-boundary cases across all four rounding modes.
- 100,042 worker hash comparisons.
- 64,000 original standard-MT integer comparisons, including singleton and
  wide ranges.

`animal_rng_test.cpp` executes copied original native prefixes and installed
patches in a private image. It passes 48 complete-register cases, 23,400
main-thread mixed float/integer calls, 5,200 additional calls across four
threads, and 4,800 unbound original integer value/state comparisons. It also
checks both stack alignments, every seven-hook failure/rollback point, nested
worker/spawn streams, completed-address reuse, original inline fallback,
and the original integer helper's exact recursive return address.

`animal_foreign_unwind_test.cpp` uses a separate dynamic C++ runtime thrower
against the loader's hidden static runtime. It passes 768 worker and 768
spawn calls across three threads, including direct and nested exceptions,
child exceptions caught inside a parent, and later engine-address reuse.
These fixtures test C++ exceptions; they do not claim a separate forced-unwind
runtime test.

Oracle and independent escape-audit artifacts were retained locally in the
`animal-rng-audit` cache directory. No running game or installed file was
modified by these tests. These results establish scoped engine parity and
hook mechanics, rather than parity of unrelated random engines or a complete
live simulation.
