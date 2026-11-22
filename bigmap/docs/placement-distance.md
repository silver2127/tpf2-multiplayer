# Large-map town placement overflow

Steam build 35924, executable SHA256
`782b904a8f7bbdac1f7a18528f1a5c778691e5aa3087c37c351bf6912585175c`.

During the first depth-13 live preview test, a 228 x 1140 tile map
(58.368 x 291.840 km) showed town markers concentrated in the middle of its
long axis. The preview had reached town placement, before creation of the
playable world's entity octree. This symptom is consistent with the spacing
overflow below; an in-game rerun with the fix is still needed to confirm it
fully explains this particular preview.

## Traced path

* RVA `0x35e1c0`: makes town records, prints Average town size / Town size sum.
* RVA `0x35df50`: supplies heightmap dimensions, resolution and bounding box
  to RandomLocationFactory.
* RVA `0x910430`: factory constructor.
* RVA `0x911590`: generates and optimizes candidate positions.
* RVA `0x9127e0`: combines spacing, slope and obstruction scores.
* RVA `0x910ce0`: spacing score, the patched function.

The spacing function receives a borrowed float output vector, a borrowed
vector of `{int32 x, int32 y, float angle}` candidates, float minimum distance,
a borrowed vector of `{int32 x, int32 y}` exclusions, and float resolution.
Windows x64 ABI: RCX, RDX, XMM2, R9, and `[entry RSP+0x28]` respectively.
Vector layout is three pointers; the function does not resize game vectors.

Candidate-pair squared distances overflow at `0x910de3..0x910de9`;
candidate-exclusion distances have the same problem at `0x910e4b..0x910e51`.
Both square and sum in signed int32, then take the signed minimum and square
root. Separations exceeding sqrt(INT_MAX) pixels (~185.364 km at 4 m/pixel)
can yield negative distances and NaN scores. Larger separations can also
wrap to a smaller positive value, creating false proximity. This threshold
is pairwise separation, not a fixed radius around the map origin.

## Fix

`src/placement_distance.h` replaces the spacing function through a verified
15-byte prologue hook. Coordinate subtraction and squaring use int64; the
result saturates at INT_MAX, preserving the stock initial minimum. This
retains existing scores for all non-overflow inputs, including the singleton
case and too-close penalty. An early component check makes even extreme
int32 endpoints safe. Pairwise symmetry and scratch caching retain the stock
quadratic work pattern. Scratch allocation belongs to the plugin CRT; no
game-owned vector is allocated, freed or resized by the replacement.

The fix is automatic on the supported Steam build and independent of octree
depth and leaf size. GOG is unmeasured and skipped with a log message. Byte
mismatch and hook failure are logged; neither writes an unverified site.

## Verification

`python tools/test_placement_distance.py` runs the original spacing machine
code in Unicorn with only scratch-vector allocation, free and sqrt stubbed,
then compares it with the compiled DLL replacement:

* 80 non-overflow cases match bit for bit, including empty/singleton vectors,
  duplicate candidates, exclusions and integer boundary distances.
* Stock negative-square-root NaNs and positive wrapped distances reproduce;
  the fixed scores match an independent wide-arithmetic reference.
* Random points spanning the current 292 km map, depth-13 dimensions and
  extreme int32 coordinates pass. Inputs and vector metadata stay unchanged.
* Actual installed detour pointer is called through its native ABI; installer
  byte checks and failure paths are exercised through a mock host.

Depth-12/13 octree tests and the New Game menu regression suite also pass.
Live placement with this DLL remains unverified. Restart and regenerate the
preview to apply it; already placed towns are not relocated by this patch.
