# Upstream dev 0610033 integration

Windows target: `0610033052b80c73e3a0357e128819e23dfafb63` (one commit).
Merge retained, staged and uncommitted; no conflicts. Version stays 0.7.0.2.

## Merged

Windows town-development trace, shared TT/TF formatting, comparison tool and
tests, and the cross-platform street-desync investigation. Windows code paths
are retained unchanged.

## Ported

Applied the supplied Linux patch and independently checked its addresses and
ABI against the actual build-35924 ELF. Family canonicalization now recognizes
all 28 list getters and 35 no-list getters instead of one of each. Every getter
is byte-verified before installation; unknown families are still refused.

Native `TPF2MP_TOWN_TRACE=1` enables the diagnostic Develop wrapper and periodic
family digests, written to `data/tpf2_towntrace.txt`. It uses the shared TT/TF
format and SysV arguments, with a verified rel32 call redirect and the existing
town seed hook's clock/context. Boot additionally requires successful seed-hook
installation before enabling the trace. The default remains off.

Added a build target for the supplied real-ELF family fixture and a reproducible
native trace ELF verifier. Detailed anchors, bytes and contracts:
[DEV_0610033](../re/linux/DEV_0610033.md).

## Not ported

Loaded-world validation remains incomplete: live register/context and vector
lifetime proof, actual 28-list step observations, and a Windows/native town
trace comparison after joining. Static checks and fixtures succeeded, but the
real lab launch failed at uid-map setup before the game process started.
All implementation changes are present; this record is marked partial for
that missing live evidence, not for a missing source implementation.
Earlier unrelated port limitations are unchanged.

## Live testing

Backed up native libraries/runtime data and Lua to `.before-port-0610033`,
installed this build and mod, and attempted the native lab with tracing enabled.
Exit 1: `bwrap: setting up uid map: Permission denied`. No title menu, GPU,
world, gdb attachment or gameplay result was observed. Both trees were restored
and hashes/symlink targets compared successfully. No game remains from the run.
Steam and user installations were not changed.

## Tests

- `tools/linux/build_native.sh`: soldier build, 67/67 CTests, glibc baseline.
- `python3 tools/linux/verify_lua_release.py`: 29 Lua files and 32 HUD glyphs.
  This commit changes no Lua; the existing cumulative reference is retained.
- Getter generator `--check`: complete 28+35 inventory matches the real ELF.
- Order-canon ELF verifier: all seven sites and the complete getter inventory.
- Town-trace ELF verifier: both guards and SysV/MT/vector data flow.
- `test_family_canon_elf GAME_ELF`: real vtables, synthetic unsorted lists;
  28 reordered, 35 skipped, zero refused; trace install accepted actual bytes.
- `python3 tools/test_town_trace_diff.py`: comparison classifications pass.

The Windows PE bytes test requires its Windows installation path and was not
run; the added Linux verifier checks the native counterpart. Off-game fixture
results are not loaded-game or cross-platform determinism evidence.
