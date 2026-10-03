# Windows dev b4b629a2 integration

## Merged

Windows target `b4b629a2f6fc8fa21884341c4db66e840deb617e` onto Linux
parent `c133fd33759f796b49cba50869d896ff367bb72f`. The incoming history is
`9125a2d6e779ef243e18013110210a7f905d6019` (deterministic script clock fix)
and `b4b629a2` (merge of origin/dev into bigmap-merge). No conflicts.
The merge remains staged and uncommitted. Release version remains 0.7.

## Ported (what and how)

Retain the upstream shared Lua verbatim. The deterministic script wrapper now
ignores incoming state whose clock is at or behind the current script clock
when the world clock has not moved backwards. This prevents another engine
script copy's stale per-frame state from undoing town-growth updates. Fresh
wrappers, actual world-clock rollback and newer incoming state still restore.
The existing exact-echo shortcut is unchanged.

This change contains no executable patches, addresses, platform calls, C++
layouts or ABI changes. Both platforms use the shared wrapper; no separate
native implementation or new static/live reverse engineering is needed.
Earlier native evidence and limitations remain in docs/re/linux/ and the prior
integration records; this integration does not establish new engine contracts.

Advance the Lua verifier to the target commit, preserving the pinned native
origin-replay change in inject.lua. Package this record and link it from README.
Extend the upstream regression to assert unchanged clock, RNG seed and payload
load count immediately after a stale sync, cover equal-clock/different-seed
sync, and check the newer state's metadata and payload are actually restored.
The old test's repeated-update assertion alone could pass even after a rewind.

## Not ported

None from these commits. Earlier native limitations remain in their records.

## Live testing

No game launched; no live town-growth, dedicated-server recovery or
cross-platform result is claimed. This shared Lua-only integration was tested
offline using Lua 5.2 and the installed unmodified Natural Town Growth script
with mocked engine APIs. No lab actors, libraries, mods or saves were changed,
so no backup/restore was needed. Steam was not controlled.

The first offline test invocation failed because the lab game's res symlink
points to absent native/shared-game/res. The successful run read init.lua
from the installed game (read-only) using TPF2MP_GAME_INIT and Workshop item
1954591986 from the Steam Workshop directory. No game files were written.

## Tests

- `tools/linux/build_native.sh`: soldier build, 65/65 CTests and glibc baseline
  checks passed.
- `python3 tools/linux/verify_lua_release.py`: 29 Lua files and 32 HUD textures
  passed. Lua manifest SHA-256:
  `97a41e07fda71c356f2f7023e66709f4521ee210c6e49f0eb56ceef0207104c8`.
- `tools/deterministic_script_test.py`: passed using clone-local
  `.git/port-venv/bin/python` (Lupa 2.8, Lua 5.2), the installed game init.lua
  via TPF2MP_GAME_INIT and the Workshop directory as its positional argument.
  Includes stale/equal/newer state, real reload, RNG isolation, migration,
  resource registration, exception cleanup and pacing/echo scenarios. The
  pacing and echo comparisons each produced 41 capacity writes at tick 900.
- Mutation check: execute the extended test against pre-fix production Lua
  read from HEAD without changing runtime files. It fails at the immediate
  stale-clock/seed assertion, as expected.
- All 29 shipped mod Lua files pass `luac5.2 -p`.
- `bash -n tools/linux/build_release.sh` and CRLF-aware staged whitespace
  checks pass. No unmerged index entries; MERGE_HEAD retained.

Logs in this clone: `.git/port-b4b629a-{build,lua,deterministic,mutation,deps}.log`.
Test dependencies are confined to `.git/port-venv`.
