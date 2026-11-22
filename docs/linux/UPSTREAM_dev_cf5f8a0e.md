# Windows dev cf5f8a0e integration

## Merged

Windows commit `cf5f8a0e036fefd9ee1fa02991fa0262957619d4`
(companies: the load-time hotseat switch waits for a world that answers),
onto Linux parent `03e17893449512f951c810b82c0a20c4475088f0`.
No conflicts. Merge staged and uncommitted; release version remains 0.7.

## Ported (what and how)

The shared game script now pumps a pending load-time company switch. Saved
state requests the switch, which waits for a construction or line query to
return an entity, logs its wait, and falls back after 3600 attempts. This is a
tick budget, not a guaranteed wall-clock minute. Company setup reuses a saved
player entity when `api.engine.entityExists` confirms it is still present.

Both changed runtime Lua files are byte-identical to the Windows target.
These changes use existing shared game Lua APIs, with no executable addresses,
byte patches, C++ layouts, platform filesystem calls or calling conventions.
No new Linux counterpart or reverse engineering is required. Existing native
ownership support is recorded in
[ENTITY_OWNER_20260922.md](../re/linux/ENTITY_OWNER_20260922.md).

Advance the Lua release verifier to the target commit, retaining the existing
pinned native origin-replay adaptation in `inject.lua`. Include this integration
record in release packaging. Strengthen the upstream Lua 5.2 test by using a
plain CM table: its original fallback metatable turned a cleared request into
a callable value and hid an extra switch. Assert exactly one switch and a nil
request, and cover line-only readiness, throwing queries, the tick before the
timeout, and a request for the already selected company.

## Not ported

None from this commit. Earlier native-port limitations remain in their records.

## Live testing

No game launched. This shared Lua-only change adds no native patch contract;
validation used the production Lua under Lua 5.2 with mocked engine responses.
No loaded-world timing, wallet/asset transfer, or cross-platform gameplay result
is claimed. Steam, lab actors, installed mods and saves were untouched; no
backup/restore or live transcripts were needed.

## Tests

- `tools/linux/build_native.sh`: soldier build, 65/65 CTests and glibc baseline
  checks passed.
- `python3 tools/linux/verify_lua_release.py`: 29 Lua files and 32 HUD textures
  passed. Lua manifest SHA-256:
  `fb45d921e5cd3560901364df9a59eb312d8b7728618c2e863deefaa8b814e55c`.
- All six `tools/company_*test.py` suites passed using clone-local
  `.git/port-venv/bin/python` and Lupa's Lua 5.2 runtime. The extended load-switch
  suite passed 17 assertions.
- All 29 shipped mod Lua files passed `tools/luacheck.py` and `luac5.2 -p`.
- The optional whole-tree `tools/luacheck.py` reported two false positives in
  unchanged `mod/workshop/tpf2_multiplayer_link_1/mod.lua`: apostrophes in its
  long-bracket description string. `luac5.2 -p` accepts that file. The custom
  checker's unrelated heuristic was left unchanged.
- `bash -n tools/linux/build_release.sh` and CRLF-aware staged whitespace
  checks passed. No unmerged index entries; MERGE_HEAD retained.

Logs: `.git/port-cf5f8a0-{build,lua,company,shipped-syntax}.log` in this clone.
Test dependencies are confined to `.git/port-venv`; a company suite's generated
change to tracked `mp_company_perms.txt` was restored afterward.
