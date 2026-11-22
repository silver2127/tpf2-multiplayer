# Windows dev a813ea9f integration — release 0.7.1.3

## Merged

Windows target: `a813ea9fee2244c63dccffa2ce624b8fe6c7c45a`.
Linux parent: `c2f4ec077d7e9bd804f40f9aa3a2d59545f64de5`.
One incoming release-metadata commit, no conflicts. Retained all four upstream
files byte-for-byte: release notes, installer version, Windows title-panel
fallback and shared lobby version. Merge staged and left uncommitted.

## Ported (what and how)

Release and lobby handshake advance to **0.7.1.3**. All peers, including
dedicated servers, need the same version.

Native CMake already reads `installer/VERSION`, tracks it through
`CMAKE_CONFIGURE_DEPENDS`, and defines `TPF2MP_VERSION_STR`. Both version
labels in `native/linux/src/menu_title_linux.inl` use that definition.
The native rebuild therefore incorporates the Windows title-version change
without duplicating its wide-string fallback in the Linux renderer.
The shared Python lobby applies the new version gate on both platforms.

Updated README/INSTALL current-release links, packaged integration record,
BUILDINFO Lua provenance and Lua verifier reference. The incoming mod tree
is identical to the previous 528294b1 baseline. Extended the existing version
gate tests with 0.7.1.2 rejection for hosts, relays, client welcome and roster.

Reviewed the Linux installation/layout, prior release and latest integration
records, and MENU_GAME/DEV_A896A1CB RE notes. No new engine address, byte
pattern, offset, hook, calling convention or lifetime contract is introduced.
No new disassembly or live RE is required for this metadata-only change.

## Not ported

None from this commit. Release-note descriptions summarize earlier changes;
they are preserved as upstream claims, not new local gameplay measurements.
Existing native limitations remain, including material-index acceleration and
probing, Sandbox town-tool capture, minimap/cargo-filter limitations, and
outstanding live terrain ownership/load-completion validation. Native sidecars
remain on by default; `terrain_sidecar=0` disables them. Changing the release
number does not establish cross-platform gameplay or performance parity.

## Live testing

No game, Proton peer, debugger or desktop input was run. No lab payload or
save was changed, so backup/restoration was unnecessary. Steam and user
installations were untouched. No GPU, rendering, in-game version label or
performance observation is claimed. The version gate tests use real Linux
loopback sockets for host/relay admission and simulated client messages.

## Tests

- `tools/linux/build_native.sh`: PASS, pinned soldier SDK, 79/79 CTests
  in 63.08 seconds; glibc <=2.31 compatibility checks passed.
- `python3 tools/linux/verify_lua_release.py`: PASS, 31 exact upstream Lua
  files, zero exceptions, 32 HUD glyphs and two toolbar textures. Manifest:
  `98911c795d3026193211339e43288137d2844db471e14249791db6fbcb55ba4b`.
- `.git/port-venv/bin/python tools/version_gate_test.py`: PASS, 2 tests,
  including 0.7.1.2 rejection and current-version host/relay admission.
  System Python initially lacked `stun`; installed pystun3 and zstandard only
  in a clone-local venv, with pip cache disabled and temporary files in `.git`.
- Native menu artifact contains `v0.7.1.3` and no `v0.7.1.2` label. This is
  compiled artifact evidence, not a live UI observation.
- Incoming files match upstream; incoming mod tree matches 528294b1.
  Release shell syntax, CRLF-aware whitespace, empty unmerged index and
  retained target MERGE_HEAD checked successfully.
- Windows/MSVC compilation, release packaging and live gameplay were not run.

Logs are retained in `.git/port-a813ea9f-{build,lua,version,deps}.log` and
`native/linux/out-soldier/Testing/Temporary/LastTest.log` in this clone.
