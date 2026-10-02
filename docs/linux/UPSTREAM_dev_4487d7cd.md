# Windows dev 4487d7cd integration — release 0.7.1.4 and gameplay timestamp

## Merged

Windows target: `4487d7cddad7cbd6fa90e31d62bc7d1a90f58597`.
Linux parent: `9cf1b824e8321f0f2172230ea1db5c5275e3a1c9`.
Three first-parent commits: `15014d6` (PR #19, including `7de22ea`),
`76fd191` (gameplay timestamp), `4487d7c` (release 0.7.1.4).
No conflicts. All eight incoming Windows/shared files are retained exactly
as upstream. The merge remains staged and uncommitted; the next pending
Windows commit is outside this integration.

## Ported (what and how)

- Native `tpf2_menu.so` exports `Tpf2mpLastGameUiTick`, an atomic
  CLOCK_MONOTONIC millisecond stamp from the existing verified CGameUI update
  detour. It starts at zero, ignores non-current objects, refreshes on updates,
  and clears after CreatePage(2). Only this C symbol is exported; C++ symbols
  remain hidden. Consumers resolve it through the RTLD_LOCAL module handle.
  [ELF evidence, ABI and consumer scope](../re/linux/DEV_4487D7CD.md).
- The native terrain pager has no Windows load-tail allowance or commit
  throttle; its MemAvailable/userfaultfd policy is unchanged. This port adds
  the missing API, not a new memory policy or a claimed Linux speedup.
- Release 0.7.1.4 flows from installer/VERSION into native CMake panel text
  and Linux packaging, and from shared LOBBY_VERSION into peer admission.
  Extended version-gate tests reject the previous 0.7.1.3 release.
- Windows MSI's native-DLL warning now uses a wizard page, retained intact.
  Linux has no MSI control-event or invisible-message-box path; its shell
  installer reports refusals on stderr. No DLL scanner or interactive wizard
  is introduced for ELF installs. Existing native refusal regressions pass.
- Updated README/INSTALL, Lua verifier target, packaged integration record
  and release BUILDINFO. Shared Lua and texture content is unchanged.

## Not ported

None of these incoming changes require an outstanding Linux implementation.
The MSI page is Windows-specific; the native pager does not implement the
Windows throttle that motivated the export. Prior native feature gaps and
live-validation limitations remain. Upstream release-note claims summarize
previous work and are not new locally observed Linux results.

## Live testing

Reviewed the prescribed lab setup and menu controls. Backed up both native
payload directories to `.before-port-4487d7cd` siblings, installed this run's
built libraries and merged Lua, and ran:

`tools/sandbox/tpf2mp-lab run native --root ~/.local/share/tpf2mp-lab`

The isolation check and actual launch both failed with
`bwrap: setting up uid map: Permission denied`. Launch exited 1 before any
game process, so no menu, GPU, rendered world, frame timestamp, return-to-menu
transition, gdb result or load-time improvement was observed. No Proton or
XTEST run was attempted. Both payloads were restored; file-content/symlink
manifests match the backups. No save changed and no game was left running.
Steam and the user's installation were untouched.

Evidence: the job's `meta/live/` contains launch/check logs, static ELF
checks/disassembly, restoration checks, and copies of actor logs/data.
Those copied actor files may predate this attempt; they are not evidence of
a successful launch in this run.

## Tests

- `tools/linux/build_native.sh`: PASS, pinned soldier SDK build and all 79 CTests (62.95 s);
  includes updated menu_load/menu_slot tests of real detours with synthetic
  objects and page transitions. Runtime glibc baseline check <=2.31 passed.
- `python3 tools/linux/verify_lua_release.py`: 31 exact upstream Lua files,
  no exceptions, 32 HUD glyphs and two toolbar textures. Manifest SHA256:
  `f372ad8ebf2800b6b74d457def98c4975123a5551ce0faaca53bb0323a309b5b`.
- `tools/version_gate_test.py`: two tests, host/relay admission and client
  welcome/roster rejection, including 0.7.1.3. Clone-local venv supplies
  pystun3/zstandard; no system packages were installed.
- `tools/linux/test_installer_refusal.py`: four tests pass.
- Actual built-library dlopen/dlsym returns zero; nm shows the timestamp as
  the sole defined dynamic export. Native panel artifact contains v0.7.1.4.
- ELF build ID, update prologue, RTTI, vtable slot, Step2 argument dispatch,
  global UI references and CreatePage prologue checked against the lab ELF.
- Documentation links, shell syntax, upstream-file identity and staged
  whitespace checks; no unresolved index entries.

Logs: `.git/port-4487-{build,lua,version,installer,exports,docs}.log` and
`native/linux/out-soldier/Testing/Temporary/LastTest.log`.
No release archive or Windows MSI build, commit, abort, or publication.
