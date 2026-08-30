# Serialising a Command — SOLVED

**The geometry is a `std::vector<Vec3f>` of node positions, reachable at the
`make_cmd::BuildProposal` entry hook.**

```
hook make_cmd::BuildProposal  rva 0x9dc750  (19-byte steal)
  rcx = hidden return slot (0x38 Command)
  rdx = a1   heap pointer (engine/context)
  r8  = a2   caller-owned STACK struct  <-- the geometry
        +0x00  std::vector<Vec3f> begin
        +0x08  std::vector<Vec3f> end      count = (end-begin)/24
        +0x18  vector, 240 bytes, per-segment data (contains the tangent)
  r9  = a3   second caller-owned struct
```

Verified live, two player roads in different places:

```
build 1:  (1148.994, -8395.182, 65.428)     step dx=+15.05  dy=-56.87
          (1164.046, -8452.047, 66.359)
          (1179.099, -8508.913, 67.291)
build 2:  (1322.523, -8341.502, 66.189)     step dx=+16.98  dy=-64.11
          (1339.498, -8405.617, 68.590)
          (1356.474, -8469.731, 70.990)
```

Three collinear nodes each, 24-byte stride, z following terrain. The value
`-56.866` found in the `a2+0x18` vector equals build 1's dy exactly — the
tangent — which independently corroborates the decode.

`a2+0x78`, `a2+0xb8` and `a3+0x30` are 128-byte vectors filled with
`-52802.125` repeated: an uninitialised sentinel, not data.

## Consequence

This is directly usable. `mod/mp_lockstep_1` already executes
`ROAD x0 y0 x1 y1` on both peers at an agreed game-time stamp with zero desyncs.
The node positions above are exactly that vocabulary, so the remaining wiring is:

1. hook `make_cmd::BuildProposal`, read the node vector  (**this document**)
2. cancel the local build  (`M10_DEFERRAL_HOOK.md`, working)
3. schedule it as a lockstep command  (`M9_LOCKSTEP_PROTOTYPE.md`, working)

No native command serialisation is needed at all — ship coordinates, rebuild
through the API that already works.

---

## How it was found (four wrong turns first)

Kept because the wrong turns share one shape, and it is worth recognising.

## Why it matters

Lockstep needs to put a player's action on the wire. `CommandList::Add`
(`0x9d2a00`) is the confirmed universal tap and a player action is separable
from script traffic by caller RVA alone. What is missing is the payload: given a
`Command`, produce bytes a peer can reconstruct it from.

## What the Command is NOT

Measured live, two player road builds in clearly different locations:

| depth | result |
|---|---|
| the 0x38-byte `Command` itself | **12 of 13 fields byte-identical** across both builds. No geometry. |
| one pointer hop out | pointers point back into memory adjacent to the Command — a small-container layout. Only floats present are `1.0` and `0.75`. |
| two pointer hops out | 189 fields compared, 19 differ — and the differing ones are heap addresses and allocator metadata (ASCII `" stack#0"`, `"ton"`). Floats found are identical across both builds, so by construction not coordinates. |

So the `Command` is a **handle**. Its payload is not reachable by walking memory
from it, at least not within two hops. Two rounds of chasing found the allocator,
not the proposal.

## The blocker on the static route

Decompiling `make_cmd::BuildProposal` (`0x9dc750`) — the factory that builds the
Command — yields no struct stores. Ghidra reports
`undefined FUN_1409dc750(void)`: it recovered no signature, so it does not model
the hidden return-slot pointer MSVC uses to return a 0x38-byte struct by value.
With no output parameter modelled, the writes that populate the Command are not
rendered as stores into it.

This is the same baseline limitation seen all evening — the known-good
`buyVehicle_factory` control decompiles equally unsigned — so it is not specific
to this function.

## RESOLVED: the Command is a handle to a 0xB18-byte payload

Applying a prototype worked. Declaring the return type as a 56-byte struct made
Ghidra model the hidden return-slot pointer:

```
Command_56 FUN_1409dc750(Command_56 * __return_storage_ptr__)
```

There are still no stores into the return slot, because the function does this:

```
undefined1 local_1690 [2840];                       // 2840 = 0xB18
FUN_1409db920(local_1690, local_b68);               // build the payload
FUN_1409dd6a0(__return_storage_ptr__, local_1690);  // wrap it into the Command
```

**The payload is 0xB18 bytes**, built on the stack and then wrapped. `0xB18` is
the same offset `ACTION_MAP.md` reports in the dispatch table
(`[payload+0xb18]`) — two independent routes to the same number.

This explains why memory-walking failed: the 56-byte `Command` is a *handle*.
Its own fields are container bookkeeping, so chasing them reaches the allocator,
never the geometry. The data lives in the 2,840-byte payload behind the wrapper.

### The payload's shape

Both follow-up functions decompiled (with prototypes applied):

**`FUN_1409dd6a0` — the wrapper.** Touches the payload at exactly one offset,
`+0xb18`. The payload body is `0xB18` bytes, so this is the **command type tag**
immediately after the data — and it is the same offset `ACTION_MAP.md` reports
the dispatch table indexing with. The wrapper's job: read the type, produce the
56-byte handle.

**`FUN_1409db920` — a COPY CONSTRUCTOR, not the populate step.** `param_1` and
`param_2` have identical offset sets, which is the signature of a copy. Still
useful: the offsets it copies are the payload's field layout.

```
scalars   0x2f8 0x2f9 0x2fc 0x300 0x301 0x302 0x304 0x308 0x30c 0x310
vector A  0x318 0x320 0x328          begin/end/cap
vector B  0x330 0x338 0x340
          0x348 0x350 0x351 0x358 0x360 0x368 0x369
vector C  0xb00 0xb08 0xb10
type tag  0xb18                       (read by the wrapper)
```

> **DISPROVEN by live probe.** Hooking the wrapper and reading those offsets on
> a real player build gave `vecA begin=<ptr> end=0`, `vecB begin==end`, `vecC`
> all zeroes, and `+0xb18 = 0xED3BD08F` — not a dispatch index into a 37-entry
> table. None of these offsets mean what the list below claims.
>
> **Why the inference was bad:** `FUN_1409db920` decompiled as
> `undefined FUN_1409db920(void)` — Ghidra recovered NO signature, so its
> `param_1`/`param_2` are the decompiler's guesses, not real parameters. Their
> offsets are therefore not a struct layout. The contradiction was visible in the
> data and I did not act on it: both "parameters" showed identical offsets out to
> `0xb10`, yet `local_b68` is only `0x2F8` bytes, so those accesses cannot belong
> to it.
>
> Rule this establishes: **offsets taken from a function with no recovered
> signature are not evidence.** Apply a prototype first (see
> `ApplySigDecompile.java`) or verify against live bytes before building on them.

The 8-byte-spaced triplets were read as `std::vector` begin/end/capacity, mapped
onto `addedNodes` / `addedSegments` / `toAdd` from `PROPOSAL_STRUCTURE.md`. That
mapping was inferred from spacing and is now disproven.

### What IS established (live-verified)

- The wrapper `0x9dd6a0` fires **exactly once per command** and is **silent at
  idle** — unlike `CommandList::Add`, which runs ~100/sec from the Lua bridge.
  It is the cleanest per-command hook point found so far.
- At wrapper entry `rdx` is a live payload pointer and `rcx` is the output
  Command slot. Both are stack addresses in `make_cmd::BuildProposal`'s frame.
- Caller is `0x9dc92e`, inside `make_cmd::BuildProposal` as expected.

### RESULT of the differential dump: no geometry in the payload

Two player roads built hundreds of metres apart, full `0xB40` region captured at
the wrapper for each, diffed byte for byte.

**61 of 180 rows differ — and they are all pointers.** Heap addresses that moved
between allocations (`0x1ff76c1efc0` vs `0x1ff734730e0`). No coordinate ever
appears.

> **Tooling warning.** The probe's float scanner reported dozens of "floats"
> around 1.006–1.011 that changed between builds. **All false positives.** They
> are the low four bytes of pointers in the `0x2_003f_xxxx` range, which decode
> as floats near 1.0. Any scanner that reinterprets arbitrary memory as floats
> will manufacture these. Do not treat a float near 1.0 from a pointer-dense
> region as data.

So the payload is itself pointer-dense: the geometry is in heap containers
*behind* it, not in the block. Reconstructing it means walking a C++ object graph
across ~30 live pointers — the problem this whole line of work was trying to
avoid.

### BREAKTHROUGH: the data arrives as ARGUMENTS, and they contain vectors

The mistake in everything above was chasing the factory's OUTPUT. Reading the
decompiled factory (with parameters declared, see below) shows the data comes IN:

```
FUN_1403e7e10(local_b68, a2);          a2 -> the 0x2F8 proposal struct
FUN_14045f3a0(local_870, a3);          a3 -> a second structure
plVar4 = *(longlong **)(a3 + 0x68);    a3 dereferenced at +0x68
FUN_1403e3d30(a3 + 0x18);              and at +0x18
```

ABI note: the return is a 0x38 struct, so `rcx` is the hidden return pointer and
the real args shift — `rdx`=a1, `r8`=a2, `r9`=a3.

**Declaring only the struct return was actively harmful.** It told the decompiler
the function takes nothing else, so every call site rendered with one argument
and the real inputs vanished. A factory that obviously consumes build data looked
like it consumed nothing. `ApplySigDecompile.java` now takes a parameter count.

Live capture at the factory's entry, two roads in different places:

```
#1 caller_rva=459e97  a1=2071086aef0(heap)  a2=503871cd10(stack)  a3=503871c810(stack)
#2 caller_rva=459e97  same slots
```

`a2`/`a3` are caller-owned STACK structures — built by `StreetBuilder` from
player input. Diffing them:

| | rows | differ |
|---|---|---|
| a2 | 16 | 9 |
| a3 | 16 | 5 |

And the differing qwords are **not** noise. They are `std::vector` bounds:

```
a2+0x00   b1 begin=0x207f6e73280 end=0x207f6e732c8   span 0x48 = 72 bytes
          b2 begin=0x206efd5b1f0 end=0x206efd5b238   span 0x48 = 72 bytes
a3+0x30   same shape, span 0x80 = 128 bytes
```

Same size both builds, different addresses — freshly allocated per build, which
is exactly what a per-build geometry container looks like. The constant
`0000803f` (float 1.0) at `a2+0x60` stays put, so that is structure.

**Correction to my own earlier reasoning:** across three rounds I dismissed "all
the differing values are pointers" as allocator churn. In `a2`/`a3` those
pointers ARE the payload, because they are vector bounds. I was treating the
signal as noise because it did not look like a float.

### Next step

`args_probe.cpp` is written and builds. It detects vectors by SHAPE — for every
aligned qword pair, accept as begin/end only if the span is positive, under 2 KB
and a multiple of 4 — then dereferences and dumps the elements. A shape test the
data must satisfy is harder to fool than a guessed offset, which was wrong twice.

It needs one successful capture: inject into instance `a` and build two roads IN
THAT WINDOW (several attempts went to the sandboxed instance instead; focusing
the window programmatically before asking is worth doing).

Expect 72 bytes of elements at `a2+0x00`. For a two-node road segment that is a
plausible size for a small set of `Vec3f` positions plus metadata.

### Recommended change of approach (still valid as a fallback)

Do not serialise the native command. The pieces already proven make a shorter
path:

1. **Detect and cancel** the player's action natively — working today
   (`M10_DEFERRAL_HOOK.md`, three builds cancelled cleanly).
2. **Describe** what was built at a level that is already expressible: the
   lockstep mod's own op vocabulary (`ROAD x0 y0 x1 y1`, `CON <file> x y`, ...).
3. **Re-issue** through `api.cmd`, which `mod/mp_lockstep_1` already executes on
   both peers at an agreed stamp with zero desyncs.

Step 2 is the only open piece, and it does not require the native command at all
— it requires knowing the player's intended geometry, which the UI builder holds
in its own state (`StreetBuilder`'s `this`, field accesses out to `0xac8`), or
which can be read from the world immediately after an *un*-cancelled build.

A viable interim design: let the build happen locally, read the resulting edge
via the Lua API (which `mpbridge` already does reliably), delete it, and re-issue
it as a lockstep command. Visually a brief flicker, but it needs no struct
mapping and every component already exists.

### Superseded plan — differential dump, no inferred offsets

Stop deriving offsets from unsigned decompilations. Dump the payload region
**wholesale** at the wrapper and diff two builds made in different places. The
bytes that change are the geometry, by construction. This is the same technique
that proved the 56-byte Command is a handle (12 of 13 fields identical across
two builds), and it depends on no assumption about layout.

Practical notes for that probe:
- Payload size is `0xB18` per the stack local in `make_cmd::BuildProposal`; dump
  a bit past it to catch a trailing tag.
- Diff, then interpret: pointers will differ between runs as allocations move,
  so treat a differing qword that looks like a heap address as noise and a
  differing float pair/triple as a candidate coordinate.
- Cross-check any candidate against where the road was actually built.

Then, separately: find where the payload is POPULATED. `local_b68` (0x2F8 bytes)
is filled by `0xa17bf0` in the `make_proposal.cpp` range, then copied in. That is
where a player's drag becomes data.

Serialising `0xB18` bytes verbatim will not work regardless — the payload holds
pointers. Whatever the containers turn out to be, their *contents* must travel.

## Superseded: earlier next step

**Apply a function signature in Ghidra, then re-decompile.** Define a 56-byte
`Command` type, set `make_cmd::BuildProposal`'s prototype to return it (or take
it as a first hidden pointer parameter) with `__fastcall`, and the decompiler
will render the field writes. The agent's `__FUNCSIG__` extraction gives real
signatures for ~22,580 functions and can supply the exact parameter list:

```
struct Command __cdecl make_cmd::BuyVehicle(const class ecs::Engine &, class ecs::Entity, ...)
```

is already recovered for a sibling factory, so the shape of these is known.

Secondary options if that stalls:
- Decompile `Visitor::operator()(CmdData::BuildProposal&)` at `0x9d6e20` — the
  handler that *consumes* the command. What it reads is what must be sent.
- `CmdData::BuildProposal` appears in RTTI alongside
  `construction_builder_util::Proposal_CmdData::BuildProposal`, suggesting the
  command holds a `Proposal` whose structure is already partly mapped in
  `PROPOSAL_STRUCTURE.md`.

## Related bug found while testing

`mod/mp_lockstep_1`'s `worldHash()` hashes vehicle positions and the *count* of
constructions. Roads and rail are **edges** — neither. The desync detector is
therefore blind to the exact action type the lockstep prototype was validated
with. The prototype's PASS still stands (both peers executed the same command at
the same stamp, verified from logs), but its "0 desyncs" covers less than it
appears to. Fix before trusting the hash.
