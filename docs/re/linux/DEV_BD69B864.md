# Native evidence for dev bd69b864 (2026-09-23)

Local read-only ELF: `~/.local/share/tpf2mp-lab/native/game/TransportFever2`,
Steam 35924, GNU build-id `3a0e156390b0e6f1e372051c24802c8493ae454a`.
This record separates newly repeated static/fixture checks from upstream live
claims. No game code ran in the attempted lab launch.

## UCRT math

Applied the supplied `crossplatform/linux_ucrt_libm_parity.patch`. Anchors and
Windows/native correspondence are in [STREET_LIBM_REPORT](../crossplatform/STREET_LIBM_REPORT.md).
New `tools/linux/verify_libm_parity_elf.py` reads ELF segments, dynamic symbols,
JUMP_SLOT relocations, eager-binding flags and the actual instructions; all pass.

| Native site | Verified contract |
|---|---|
| `5a471b8`, `5a471c8`, `5a470c0` | sinf, cosf, sincosf GOT relocations |
| `5a470d8`, `5a472b8`, `5a472b0` | tanf, acosf, atan2f GOT relocations |
| `1578b3b`, guard `1578b33` | `cvtss2sd xmm0,xmm0; cvtss2sd xmm1,xmm1; call 6dbc60`; result narrowed to float |
| `1579031`, guard `1579029` | same two conversions and call; `cvtsd2ss xmm0,xmm0` |
| `157a4ac`, guard `157a4a4` | reversed conversion instruction order, same argument registers/callee; result narrowed to xmm1 |
| `6dbc60` | RIP-relative indirect jump through double atan2 import |

The three guards' exact bytes are in `libm_parity_sites_linux.h` and checked
against this ELF. SysV float/double arguments use XMM0/XMM1, result XMM0.
The wrapper accepts doubles that were exactly widened from floats, calls the
float model, then widens its result for the original caller's narrowing.
Sincosf uses XMM0 for x, RDI/RSI for the two output pointers. No game container
or entity lifetime is borrowed by these replacements.

Installation verifies the build-id, six resolved libm symbols and all three
instruction contexts before writing. GOT write failures roll back; call redirect
failures explicitly report a partial site mask. The default-on module has
`TPF2MP_LIBM_PARITY=0` for diagnosis. Compilation disables implicit FMA contraction
and fast-math; explicit model FMA operations remain. Tests check the upstream
Windows-generated golden hashes and installer failure paths. The upstream
exhaustive Windows run was not repeated here. The model targets the measured
AVX2/FMA UCRT behavior; other Windows CPU paths remain an upstream limitation.

## Octree

Applied the supplied depth-12/13 patch. Source/assert-string anchors, SysV
register contracts, exact bytes, branch targets and annotated machine-code
stubs are recorded in [Big Maps PORT](../../../bigmap/docs/linux/PORT.md#octree-depth-1213-experimental).
The local `verify_game.py` passes the build-id and all 29 sites, including root
`a84234`, child-ID loop `16aa190` and level decoder `13ec530`.

Unicorn ran the original ELF descent code with the actual replacement stubs:
1,620 depth-13 nodes and 1,462 depth-12 nodes, decoder boundaries, parent levels,
positive/unique IDs, existing-child identity and stock depth-10/11 equivalence
pass; the original overflow reproduces. This is emulation, not live rendering.
Soldier also executes the assembly stub/register and transactional installer
fixtures. The root patch is installed last; all sites are verified before writes.

## Target ordering performance

The incoming two performance commits are diagnostic reports, not Windows code
changes. Inspection found a full linked-list search over target records in both
ObserveTargetInsert and ObserveTargetErase. Add 1,024 private hash buckets per
owner and an intrusive collision chain; retain a separate doubly linked list
for existing reader walks/cleanup. Empty-record removal updates both lists.
This bounds the search to the target's bucket without new hot-path allocations,
changing game memory, changing MSVC order, or adding any hook/offset/ABI claim.

The existing node-order implementation already hashes entity lookup, so it was
not replaced by the suggested vector index. Native inserted-node discovery,
reader walks and per-record/node allocation remain; this is not a claim to remove
all profiled cost. Tests exercise 3,000 targets (including bucket collisions),
erase/reinsert, nested owners, cleanup, the existing Windows oracle and real
register-preserving stubs. No live speedup or 3x server throughput is claimed.

## Placement gap: actual static attempt, no speculative patch

`funcsig.csv` anchors RandomLocationFactory's constructor at `14e66e0` and
its Run worker at `14e6e30` using the original UrbanSim/RandomLocationFactory.cpp
source path. Disassembly of the neighboring optimizer finds an inlined spacing
calculation in `14e45b0` (unlike Windows' separate `910ce0` helper):

- RDI captured in RBX; `[rdi+10]-[rdi+8]` divided by 12 counts point records.
- RSI captured in R15; `[r15]`/`[r15+8]` bound an 8-byte-stride exclusion loop.
- `[rcx+10]` supplies a float subsequently squared at `14e4671`.
- `14e4650` initializes scratch integers to `7fffffff`.
- `14e46d0..14e46e4` subtracts x/y, squares with 32-bit IMUL and adds in EAX:
  `45 0f af c9 0f af c0 44 01 c8` at `14e46dd`.
- `14e4730..14e4745` repeats the overflowing arithmetic for exclusions;
  `14e473a`: `0f af c9 0f af c0 01 c8`.
- `14e4756` converts the signed minimum to float, multiplies the squared
  resolution, takes SQRTSS, compares minimum distance and writes the output.
- The function continues into other optimizer work after `14e479e`; replacing
  its entry with the Windows helper would discard that work and be incorrect.

This establishes candidate arithmetic, not a complete replacement contract.
The provenance/lifetime of the optimizer state, all inlined spacing copies and
worker attempt-budget argument still need confirmation. A lab launch with the
built libraries was attempted for live probing; bwrap failed before any PID was
available (`setting up uid map: Permission denied`). No gdb attach/register or
lifetime observation was possible. No guessed placement patch was installed.
The existing menu diagonal cap is retained; it does not make an oversized save
created on Windows safe for cross-platform placement parity. Placement attempts
also retain the existing native behavior.

## Live attempt

Only the native actor was used. Both `share/tpf2mp` and `game/mods/mp_lockstep_1`
were copied to `.before-port-bd69b864`, then the soldier libraries, Big Maps plugin,
merged Python and Lua were installed into the actor. The prescribed lab command
exited 1 immediately at namespace setup. No title screen, GPU selection, save,
hook hit, gameplay, UI appearance or cross-platform comparison was observed.
No Steam changes or namespace-policy workaround was attempted. Both trees were
restored and file hashes/symlink inventories matched. No game remains running.
Job `meta/live/launch.log` and `restore.json` are current evidence; copied
`actor-logs`/`actor-data` are pre-existing historical files, not results of this run.
