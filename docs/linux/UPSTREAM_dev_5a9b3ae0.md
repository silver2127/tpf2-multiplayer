# Upstream dev 5a9b3ae0 integration

Windows target: `5a9b3ae0641478d4cda6b46e846f585866c50184`, following
[49b1f277](UPSTREAM_dev_49b1f277.md). Linux parent:
`db8280530ce65fd7d7dad7eb14687543b6f75a99`. One Windows commit, no conflicts.
Merge staged, not committed. Status: **partial**, because canonical ordering
still lacks local live validation and remains default-off.

## Merged

Preserved Windows freed-ID relay/sort, speed Sync wrapper, byte test and
research changes unchanged. No shared Lua or lobby changes were needed.
Lua verification and release provenance now reference this target (still
experimental 0.6.1.28); the pinned native origin-replay integration is retained.

## Ported

- Sixth native canonical hook: sort the removed-ID vector reached through
  r13+208 at ELF 3256930 before appending to the free deque. Preserve all
  relay registers, runtime byte/build guards and installation rollback.
- Native speed override now runs immediately after a successful CGame::Sync
  at a30cc0. No per-frame interval write remains. Publish each fresh engine
  estimate and defer control changes until that batch boundary. Verify every
  prologue before installation; install the pacing detour last.
- Expanded six-site ABI/rollback fixtures and pacing tests; added a static
  speed ELF verifier and extended the canonical verifier.

See [RE evidence, ABI, bytes and limitations](../re/linux/DEV_5A9B3AE0.md).

## Not ported

Default-on canonical sorting / validated retained-world parity remains
unported. The sixth sort is implemented behind the existing experimental
`TPF2MP_ORDER_CANON=1` gate. Independent static verification passed, but two
local lab launch attempts failed before a world loaded. Live removed-vector
ownership and earlier capacity-map lifetime checks remain missing. Matching
version numbers do not establish Windows/native simulation parity.

## Live testing

Installed this soldier build and Lua into the backed-up native actor. Normal
lab launch failed on UID-map permissions. The isolated outer-bwrap fallback
exited 53 at Steam initialization; the game also attempted unsuccessful Steam
auto-start. No selected GPU, title menu, save, gdb probe or gameplay result was
observed. No XTEST input was sent. Libraries/data, mod and userdata were restored;
no Steam/game/container process remained. Exact logs are in the job's meta/live/.

## Tests

- `tools/linux/build_native.sh`: soldier libraries, 57/57 CTests and glibc
  <=2.31 checks pass, including six canonical shims and pacing regressions.
- `python3 tools/linux/verify_lua_release.py`: 29 Lua files and 32 HUD glyphs
  pass against this target, with the one existing pinned Lua integration.
  Manifest: `b7410c4b3eb76fbe3dd95ded62fc6bcb8852f6b05611f6afcea04141816c2f1c`.
- Both `verify_order_canon_elf.py` and `verify_speedhook_elf.py` pass against
  the lab executable's actual bytes and build-id.
- Release shell syntax, unmerged-index and Windows-source preservation checks.
  The Windows PE test was not run (it requires its hard-coded Windows path).
