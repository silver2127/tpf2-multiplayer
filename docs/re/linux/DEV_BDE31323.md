# dev bde31323: company registry and free colors

Target: native Steam build 35924, GNU build-id
`3a0e156390b0e6f1e372051c24802c8493ae454a`.

## Scope and contracts

The incoming Windows merge already contains Linux adaptations in
`company_tint_linux.cpp` and `company_draw_linux.inl`. Retain them:
`pid <player> <company> [<class> [<RRGGBB>]]` now supplies an independent
style class and optional exact RGB. The two-second cache also reads
`me <player>`, so a window belonging to the local human entity remains
untinted after a company switch, even if the lobby chip is stale.
Files without `me` retain the earlier lobby-file fallback. Legacy pid rows
retain their company palette fallback. Black is a valid RGB, distinct from
missing RGB (-1).

`EntityColor` uses the exact RGB for native vehicle vertices and station labels.
Station/depot icon classes and foreign window classes use the supplied class;
the shared stylesheet supplies 326 classes. The short libstdc++ string carrier
has room for all accepted class suffixes through 999 without transferring
heap ownership between C++ runtimes.

No hook site, bytes, entity lookup, register contract, allocation lifetime,
component offset or calling convention changes. The SysV relays and byte guards
remain as documented in [company draw](COMPANY_DRAW_20260921.md), the live
section of [company tint](DEV_D6DB920F.md), and
[entity-only ownership](ENTITY_OWNER_20260922.md). The rewritten shared Lua
still uses that existing entity-only setter capability for hotseat transfers.
No new reverse-engineered engine contract is introduced by the file parser
or RGB conversion.

The Windows roster fix bounds each JSON object before searching for names.
Linux `lobby_linux.cpp::ApplyRoster` already uses parsed object-local `Get`
lookups for stages, companies and letters. A regression exercises empty stages
and companies followed by populated letters, through the real roster handler
and `mp_loading.txt` export.

## Static and fixture evidence

`tools/linux/verify_company_tint_elf.py` checked the actual lab ELF: build-id,
13 whole-instruction anchor spans, 16 style-class calls, two context calls,
branch boundaries and component RTTI. Extended it to check all 24 existing draw
byte spans and disassemble the four vehicle calls to 0x1385ec0 and the label
call to 0x1383570. Draw spans include data and partial final instructions;
only their exact bytes are claimed, not whole-instruction coverage.
All checks pass. No patched byte was changed to make a check pass.

The verifier initially failed on an obsolete source assertion that name
capture still called `SliceEcsIsCompany`. The current generic name factory
records bounded encoded names with `replayOrigin=1`; shared Lua classifies
company renames. Updated that source check to the current implementation.

Extended `company_tint` exercises the real cache with legacy, class-only,
exact RGB (including black), invalid-class and oversized-RGB rows, missing
players, cache refresh, short-string class 326, all accepted suffix lengths,
local/foreign window decisions and legacy fallback. Real draw wrappers against
synthetic native buffers check all six exact RGB vertices, preserved geometry
and return values, and aligned station-label colors. These are fixture results,
not observations from a running game. Palette sync now includes native Linux's
20-color fallback and golden-angle overflow alongside the four upstream tables.

## Live attempt

Built libraries and exact merged Lua were installed only into backed-up native
lab copies. Both the isolation check and the actual native launch failed with
`bwrap: setting up uid map: Permission denied`; launcher exit status 1. The game
never started, so there is no GPU, title menu, loaded world, gdb observation or
visual/multiplayer result. No open new ABI contract required a debugger probe.
Library/data and Lua trees were restored exactly (file SHA-256 and symlink
comparison); existing `.before-port` backups were untouched. Evidence is in
this job's `meta/live/launch-bde31323.log` and `restoration.txt`. Archived actor
logs/data may contain earlier runs and are not evidence of this build running.
