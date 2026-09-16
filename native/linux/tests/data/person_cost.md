# Windows person travel-cost compatibility, game build 35924

The native and Windows games derive different walking/driving cost multipliers
from the same person ID. Windows hashes both the ID and the mode-specific salt
using FNV-1a64 over four little-endian bytes. Native uses identity integer hashes.
This changes the chosen travel mode even after destination choices match.

Windows helpers are `0x975670` (walking) and `0x973af0` (driving), both
`float(int)`. Each computes a 64-bit mixed hash, reduces it modulo 10000, then
performs separate float multiplication by `0x38d1b717` (0.0001f) and addition
of 0.5f. The full mixed hash uses unsigned 64-bit wrap throughout. Salts are
`0x00bcaaf4` and `0x0006a8d4`; their folded Windows constants are documented in
`windows_person_cost_linux.cpp`.

The native compiler inlines this calculation at five sites: reachable walking,
reachable driving (two year-dependent branches), actual walking paths, and
actual driving paths. The compatibility adapters replace only the mixed hash;
the original modulo and SSE instructions compute the final multiplier. Exact
ELF build ID and instruction context checks restrict these patches to the
analyzed executable. The path-walking site has a different stack alignment
because the original function has already pushed an outgoing argument.

Independent offline harnesses executed the original Windows instruction bodies
without running the game entry point or imports. Both the production helpers'
full 64-bit hashes and final float bit patterns matched in 220,006 cases each,
including negative int representations, boundary values, and random IDs.
The small checked-in float fixtures retain selected examples; the inline-hook
integration tests also exercise the register and stack requirements.

Before this correction, the departure-seed-only live snapshot at t=30 had
identical destinations for all 718 people but 24 walking/driving mode differences
across 23 people. The combined native candidate subsequently passed exact live
snapshots at t=30,60,72,144,300,600 against the Windows build running under Proton.
All 718 captured person records, destination choices, other captured fields,
component-state membership and person-capacity callback order match at every
checkpoint. Moving-person counts match at 53,133,159,287,440,364 respectively.
The complete comparison returned exit 0 and `allEqual: true`; evidence is
`~/.cache/tpf2mp/desync-fix-20260914/people-combined-live/report.json`, request
`0373d10b0a8c`. The frozen native boot SHA-256 is
`77e948f4efd949d15219778a1e9b256f91cbf466ea5e600e57dc23cc70d81515`.

This live result validates the combined integer/seed/cost/float corrections for
the observed person fields over ten simulation minutes. It does not isolate the
cost correction's contribution or establish full simulation determinism.
Position, detailed path progress and model transforms are outside those six
snapshots. A separate t=720 diagnostic found all 360 moving world-transform
matrices exactly equal and complete geometry capture. Its path schema remains
incomplete for walkers and optional properties; full movement validation is
pending. Installed Lua, network capture fields and returned clocks remain unchanged.

A later town-growth mismatch first appears at hash stamp 756, after matching
stamp 744: Proton adds a street while native adds a building. That separate
branch is under investigation. It does not change the six observed person
snapshot results through t=600, but confirms that complete simulation parity
has not yet been achieved.
