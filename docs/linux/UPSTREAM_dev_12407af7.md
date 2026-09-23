# Windows dev 12407af7 integration

## Merged

Target `12407af7c3bb9c5624a34b8b09242f4fbf1eaac0` onto native parent
`0a152505e05c09c946357287dcab69ab0567bb57`. Four commits: `02426ea`,
`6d816e3` (target-set performance findings and corrected hash cost),
`703ebe1` (UCRT math patch), `12407af` (octree patch and placement gap).
No merge conflicts. Staged, uncommitted merge; release version remains 0.7.
Windows paths and incoming reports/patches are retained unchanged.

## Ported (what and how)

- Applied supplied native UCRT sinf/cosf/sincosf/tanf/acosf/atan2f models,
  six guarded GOT redirects and three guarded double-to-float atan2 call
  redirects. Enabled by default; `TPF2MP_LIBM_PARITY=0` disables it.
  Extended guards through complete narrowing instructions and added an ELF
  verifier. Windows peers need the matching AVX2/FMA UCRT behavior.
- Applied supplied depth-12/13 Big Maps port, compact IDs, guarded renderer
  decoder and root expansion. Default remains 11; deeper modes experimental.
  Fixed octree-off handling of invalid depth values. Keep Linux placement
  bounds until the placement-distance gap is resolved.
- Replaced full private target-record searches during insert/erase with a
  per-owner hash index. Preserve Windows history and existing reader order;
  add collision/lifetime regression. Allocations and other scans remain;
  no live performance gain is claimed.
- Advance Lua/release baseline and package this record. No Lua content changed.

Details, addresses, ABI, guards, limitations and evidence:
[DEV_12407AF7.md](../re/linux/DEV_12407AF7.md),
[Big Maps port](../../bigmap/docs/linux/PORT.md).

## Not ported

Placement-distance saturation and `placement_attempts`: static Linux spacing
loops and a candidate 200-attempt counter found, but spacing is inlined into
a larger cost routine. Lab/gdb probes failed before gameplay due to Steam
startup failure. Live register/storage ownership, both founding paths, budget
semantics and decision equivalence remain unproven. No guessed patches.
Oversized Windows-created saves still risk different placement choices on
Linux even with matching octree depth; menu limits alone do not fix this.

Earlier parity gaps remain. Further target-set allocation/reader optimization
and live profiling are deferred; the private index is not a claim that the
entire measured bottleneck has been removed.

## Live testing

Normal lab launcher failed UID-map setup. Isolated system-bwrap fallback
loaded the candidate: boot confirmed full UCRT installation and Big Maps
confirmed depth 13, +-262144 m root and plugin OK. SteamAPI then failed before
the menu (exit 53); gdb run exited 84 on application-load error with four
hardware probes unhit. No GPU, world, saved-game, placement, street-growth or
multiplayer outcome observed. Steam untouched; no game left running. Both
actor payloads restored and verified; no saves loaded or changed.

## Tests

- `tools/linux/build_native.sh`: soldier build, 66/66 CTests and glibc <=2.31
  compatibility checks pass, including UCRT goldens, target order and Big Maps.
- `python3 tools/linux/verify_lua_release.py`: 29 Lua files (one existing
  pinned native exception), 32 HUD textures pass; manifest
  `b67ff99a9d55f248af9ea23290c96bbf0904cd83b533338bcf48d16878023aaf`.
- `tools/linux/verify_libm_parity_elf.py GAME`: build-id, six imports/RELRO,
  BIND_NOW and all three full conversion guards pass.
- `bigmap/tools/linux/verify_game.py GAME`: all 29 guarded sites pass.
- `bigmap/tools/linux/test_octree_depth_elf.py GAME`: original-code Unicorn
  emulation passes depth 12/13 and stock compatibility/overflow checks.
  Unicorn installed only in clone-local `.git/re-venv`.
- `bash -n tools/linux/build_release.sh` and staged whitespace checks pass.

Evidence is archived in this job's `meta/live/`; local build logs also remain
in `.git/port-*.log`. No commit, merge abort or publication.
