# Windows dev ceee11b1 integration: chat message wrapping

## Merged

Windows target: `ceee11b1fa7265aa31073b31d4a25c9ef85dd507`.
Linux parent: `01712002edb4c9116ed1da1b7e49f0176f2355df`.
One incoming commit, no conflicts. Merge remains staged and uncommitted.
Keep the Windows GDI renderer and its snapshot tests exactly as merged.
Keep the shared Lua chat default of 52 bytes instead of 64, including its
existing explicit-width override and continuation indentation.

## Ported (what and how)

Adapt the title/session lobby chat in `native/linux/src/menu_title_linux.inl`
to the Linux software layer. Measure each message using the actual font size
and chat width, select the newest messages that fit from newest to oldest,
then draw them in chronological order with four scaled pixels between them.
The newest message is retained even if it alone exceeds the box, and drawing
clips it to the log's padded bounds. Messages no longer end in an ellipsis.

`panel_layer.cpp` shares its UTF-8 codepoint wrapping routine between text
measurement and drawing. It wraps on spaces and splits oversized words/links
at codepoint boundaries using the existing stb font advances and kerning.
This replaces the Windows `DrawTextW` measurement/rendering calls without
introducing GDI, MSVC layouts or game ABI dependencies. Font metrics can differ
between Windows and Linux, so line boundaries need not be pixel-identical.

Reviewed README, Linux installation and earlier integration records, sandbox
instructions and `docs/re/linux/MENU_GAME.md`. This diff changes no hook,
ELF site, address, byte pattern, calling convention or struct offset; no new
binary reverse engineering or live ABI probe is necessary. The existing
verified overlay hooks are unchanged.

Advance the Lua verifier and package BUILDINFO to this Windows commit, keeping
the pinned Linux `inject.lua` origin-replay exception. Ship this integration
record with the native package. Release version remains 0.7.0.5.

## Not ported

None from this commit. Existing unrelated native feature gaps remain.
Live chat rendering acceptance remains unverified because the lab could not
start the game; offline pixel tests do not establish in-game visual parity.

## Live testing

Installed the five freshly built native libraries and merged Lua mod into the
native actor only, after copying both payload directories to run-specific
`.before-port-ceee11b1` backups (pre-existing `.before-port` backups were left
untouched). Ran:

`tools/sandbox/tpf2mp-lab run native --root ~/.local/share/tpf2mp-lab`

The launcher immediately exited 1 with `bwrap: setting up uid map: Permission denied`.
The game did not start; no title menu, chat, loaded world, GPU selection or
Proton peer was observed. No gdb attachment or desktop input was used.
Both payload directories were restored by moving their run-specific backups
back into place. No game process remains. Steam and user-installed files were
not changed; no save was loaded or modified.

Evidence copied to this job's `meta/live/`: `attempt.log`, `actor-logs/`,
`actor-data/`. Historical files in those copied directories are not evidence
of a successful run of this candidate.

## Tests

- `tools/linux/build_native.sh`: PASS; pinned soldier build, 70/70 CTests,
  glibc <=2.31 checks. Log: `.git/port-native-build.log`.
- `panel_chat_wrap`: real DejaVu Sans rendering at multiple sizes/scales;
  long words, links and UTF-8, measured height versus unclipped rendering,
  box clipping, newest-message retention and older-message fit/overflow.
  Existing title/session hit-target tests also pass. Font-dependent CTest is
  registered when DejaVu Sans is available (present in the pinned SDK).
- `lua5.2 tools/linux/test_chat_wrap.lua mod/mp_lockstep_1/res/config/game_script/lockstep.lua`:
  PASS; executes the production function, checks default 52-byte splitting,
  words/links, content preservation and explicit/minimum widths. Also registered
  with CTest when a Lua interpreter is available; the soldier SDK lacks one,
  so this test ran separately with host Lua 5.2.
- `python3 tools/linux/verify_lua_release.py`: PASS; 29 Lua files with one
  pinned native exception and 32 exact HUD glyphs. Manifest SHA-256:
  `b5219cc1b9935224f6c59055dbdc74f0e3ddfa7c83e7458db4eb802d12c1e4cd`.
  Log: `.git/port-lua.log`.
- Shell syntax, whitespace, unmerged-index and retained MERGE_HEAD checks pass.
  Windows renderer/test and shared Lua match the incoming commit exactly.

No commit, merge abort, release publication or push was performed.
