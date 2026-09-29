# Windows dev c4754f26 integration — visible installer folder refusals

## Merged

Windows target: `c4754f262e8a39b3797d7bca03ab3f0114eabaca`.
Linux parent: `2c06e97c634b3363038ffb7c5c225d64e2747a93`.
One incoming commit, no conflicts. Merge staged and left uncommitted.
Release remains **0.7.1.3**.

Retained `installer/Package.wxs` and `installer/ca/tpf2ca.cpp` byte-for-byte
from upstream. When CheckGameDir's warning box is not displayed, it puts the
formatted refusal in `TPF2_GAMEDIR_MSG`. The folder page's Next event then
opens GameDirProblemDlg before the guarded navigation events. The message
property is cleared on every check; substitution and CRLF conversion make
the reason suitable for the dialog's text control.

## Ported (what and how)

The native counterpart is `tools/linux/install.sh`, using `tpf2mp_die` from
`tools/linux/tpf2mp_paths.sh`. It already prints `error: <reason>` to stderr
and exits 1 before installation for missing game files, an undiscovered game,
a discovered Proton-only installation, or a mismatched GNU build ID. This
satisfies the upstream behavior without introducing an MSI dialog or changing
native installation policy. Native loading uses the preload wrapper and ELF
libraries, so Windows alut.dll replacement checks do not apply.

Added `tools/linux/test_installer_refusal.py`: four source-installer black-box
regressions check stderr, exit status, actionable reasons and preservation of
all fixture files and directories. They cover a selected folder missing the
native executable, no discovered game, a discovered Windows/Proton game, and
an actual unrelated ELF (`/bin/true`) with its build ID read by readelf.
No game binary or installed mod is modified by these tests.

Reviewed README, INSTALL, previous integration records (b141b123 and 721ac61f)
and native RE documentation, including LOGS_TOOLS. No engine hook, address,
byte pattern, struct offset, ABI or game-owned lifetime changes in this
commit. Static disassembly and live gdb analysis are unnecessary for this
installer UI fix; all existing native runtime guards remain intact.

Updated README/INSTALL, the Lua verification baseline, release BUILDINFO and
the packaged integration record list. Lua and textures are unchanged upstream.

## Not ported

None from this commit. MSI-specific UI has no native equivalent to implement;
the native refusal diagnostics already work. Earlier native feature gaps and
outstanding gameplay validation remain as recorded in prior integrations.

## Live testing

No game, Proton peer, debugger or desktop input was run. No actor payload or
save was changed, so no backup/restoration was needed. Steam and user
installations were untouched. There is no GPU or on-screen observation, and
no claim of Windows dialog click-through validation. The installer subprocess
observations above used only synthetic folders inside the clone.

## Tests

- `tools/linux/build_native.sh`: PASS, pinned soldier SDK, 79/79 CTests
  (62.79 seconds); glibc <=2.31 checks passed.
- `python3 tools/linux/verify_lua_release.py`: PASS, 31 exact upstream Lua files,
  zero exceptions, 32 HUD glyphs and two toolbar textures. Manifest SHA256:
  `f372ad8ebf2800b6b74d457def98c4975123a5551ce0faaca53bb0323a309b5b`.
- `TMPDIR="$PWD/.git/port-tmp" python3 tools/linux/test_installer_refusal.py`:
  PASS, four tests. Refusals printed their reasons to stderr, returned 1 and
  preserved every fixture file and directory.
- `python3 tools/check_doc_links.py`: PASS, 379 documents, no dead links,
  anchors or missing prose paths.
- Shell syntax, whitespace and upstream installer identity checks: PASS.

Logs: `.git/port-c4754f26-{build,lua,installer,docs}.log`; native details:
`native/linux/out-soldier/Testing/Temporary/LastTest.log`.
No release archive or Windows MSI build was run. No commit, abort or publication.
