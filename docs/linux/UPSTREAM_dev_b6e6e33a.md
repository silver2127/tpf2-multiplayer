# Windows dev b6e6e33a integration — release 0.7.1.6

## Merged

Target `b6e6e33ad0621d937b72a9018e4d43cf307bf207`, Linux parent
`86d4711198727181dcb8777bd76733f1f8eb94c6`. One release metadata commit,
no conflicts. All four incoming files are retained byte-for-byte: release
notes, installer version, Windows title-panel fallback and shared lobby version.
The merge remains staged and uncommitted.

## Ported (what and how)

Native CMake reads `installer/VERSION` into `TPF2MP_VERSION_STR`; both version
labels in `native/linux/src/menu_title_linux.inl` therefore advance to 0.7.1.6.
The built `tpf2_menu.so` contains `v0.7.1.6`. Linux packaging uses the same
version file, and native clients and dedicated servers use the shared lobby's
0.7.1.6 version gate. Extended existing host/relay and client welcome/roster
tests to reject the previous 0.7.1.5 release.

Updated README, Linux installation notes, packaged integration record,
release BUILDINFO provenance and the Lua verifier's exact upstream reference.
The release notes describe panel I/O, recovery roster and startup grace fixes
already integrated in `63916f00` and `f4d3df01`; no duplicate implementation
is needed here. Windows paths remain intact.

Reviewed Linux CMake, title-panel implementation and prior integration/RE
records (`DEV_192ECD4D`, `NATIVE_IO_LINUX`). This commit introduces no engine
address, byte pattern, struct offset, ABI or lifetime contract. No binary RE
or live ABI proof is needed; existing native guards remain unchanged.

## Not ported

None for this commit. Previously documented native limitations remain;
this release metadata update does not establish gameplay parity.

## Live testing

No game, debugger, Proton peer or desktop input was run. No lab payload,
save, installed mod or Steam directory was changed, so no backup/restoration
was needed. No title rendering, GPU, loaded-world or cross-platform gameplay
result is claimed. Version-gate tests use loopback networking and simulated
client messages; they are not live game tests. Upstream release-note validation
claims are preserved as upstream text, not new local observations.

## Tests

- `tools/linux/build_native.sh`: PASS, pinned soldier SDK, 79/79 CTests
  (62.83 seconds), followed by passing glibc baseline checks.
- `python3 tools/linux/verify_lua_release.py`: PASS, 31 exact Lua files,
  zero exceptions, 32 HUD glyphs and two toolbar textures. Manifest SHA-256:
  `f7a4d20419e173a3cb2d5aa4ef75155299b3bb6a72ab6eb9f9e1c6cdad9f0007`.
- `.git/port-venv/bin/python tools/version_gate_test.py`: PASS, two tests,
  covering host/relay admission and client welcome/roster rejection.
  The initial system-Python attempt lacked `stun`; installed pystun3 and
  zstandard only in a clone-local virtual environment, then reran successfully.
- Built menu version string, all four incoming-file identities, release script
  shell syntax and Git whitespace checks passed. No unmerged index entries.

Logs: `.git/port-b6e6e33a-{build,lua,version,deps}.log` and
`native/linux/out-soldier/Testing/Temporary/LastTest.log`.
Windows/MSVC compilation and release archive packaging were not run.
No commit, merge abort, publication or system package installation.
