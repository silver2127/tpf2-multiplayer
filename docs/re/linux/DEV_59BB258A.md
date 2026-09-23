# dev 59bb258a: own-company icon and label colours

Static inspection of Steam Linux build 35924, GNU build-id
`3a0e156390b0e6f1e372051c24802c8493ae454a`, confirmed with `readelf -n`.
The game was read, never executed. All addresses below are ELF virtual
addresses, analysis evidence only; no new patch is installed.

## Windows change

`59bb258ab650db44c4c6dddca4fa80e37cb6a360` removes `owner == local` from
`IconTintForEntity`'s rejection predicate. Both vehicle and station-label
relays call it. Negative/missing owners and missing/nonpositive company
mappings remain untinted. Local-player caching remains for `WindowTint`,
whose foreign-only predicate is unchanged. The surrounding older Windows
comments still say foreign-only; the new executable predicate governs.

Linux has no corresponding tint helper. The owner comparisons in
`movement_linux.cpp` concern visibility, window eligibility or station
permissions, not colour. Removing those comparisons would not port this fix.

## Fresh investigation

Continued the candidates from [DEV_D3135A59.md](DEV_D3135A59.md).
Searched `~/tpf2-re/linux/functions.csv` and `funcsig.csv` for ItemCreator,
AddQuad and both candidate addresses. The function list gives 446 bytes at
`0x1383570` and 766 at `0x1385ec0`; signature exports do not identify a
nullable-colour vehicle draw counterpart. Disassembled both complete bodies
with `objdump -d -Mintel --start-address=... --stop-address=...` against the
supplied native ELF, and inspected the label caller near `0x1389559`.

### Label: colour read now established

At `0x138952e`, `48 0f 44 f1` selects one of the two stack colours.
`0x1389549` (`48 89 f2`) passes it in rdx; the CALL at `0x1389559`
is `e8 12 a0 ff ff`, targeting `0x1383570`. The caller also passes
rdi from `[r15+0x428]`, rsi=r15+0x1d4, and packed geometry in rcx/r8.
The following click registration obtains the entity pointer from rbp-0xd0.

The callee saves rdx in r13 at `0x1383594` (`49 89 d5`). At `0x13836bf`,
`66 41 0f 6f 65 00` is `movdqa xmm4,[r13]`: the colour is a **required,
16-byte-aligned** pointer, not a nullable Windows-style argument. The value
is copied into a temporary vertex at `0x13836d4`; the capacity-available
path copies two 16-byte blocks into the destination and advances it by 32
bytes (`0x1383680..0x13836a0`). The full-capacity path calls `0x1385be0`
with the temporary vertex pointer. This narrows the earlier colour-copy gap,
but the growth helper's complete lifetime behavior was not established here.

A label relay still needs proven station entity/engine flow across the
containing function's paths, safe component lookup and world lifetime, and
preservation of all state live across the inserted helper. A generic Windows
RGBA buffer cannot simply be substituted without respecting the observed
alignment requirement. No such relay is installed.

### Vehicle: candidate is not a colour-pointer draw

`0x1385ec0` saves rdi's pointer argument on the stack, rsi in r14, and edx
in r15d (`0x1385ec6`: `41 89 d7`); xmm0..3 feed geometry arithmetic.
At `0x1386121..0x138612e` it calls `0x137cce0` with the original rdi,
esi=r15d and rdx=r14. Its append path at `0x1386108..0x1386117` writes
**16-byte vertices** containing two eight-byte values, compared with the
label helper's 32-byte coloured vertices. The growth call is
`0x1386163 -> 0x1385d50`; packed geometry returns in rax/rdx.
There is no established nullable colour argument to fill at this candidate.

The remaining vehicle work is to identify the coloured render-buffer path
and its texture/material contract and prove the entity/engine flow for all
four carrier types. Substituting the label helper would change both vertex
layout and geometry ABI without evidence of compatibility.

## Decision and remaining gap

Keep vehicle and station-label tints off, including own-company colours.
The fresh analysis narrows the label ABI but does not establish a complete,
safe native implementation of either tint path. No guessed addresses,
struct offsets or relays were added. Native window wash is an earlier gap
and is unchanged by this commit. Existing byte-verified visibility/window
patches are retained and their ELF verifier passes.

Disposable disassembly captures: `.git/port-59bb258-label.asm` and
`.git/port-59bb258-vehicle.asm`. This record preserves the useful evidence
independently of those captures. No runtime or live-rendering claim is made.
