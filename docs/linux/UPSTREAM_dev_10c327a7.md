# Windows dev 10c327a7 integration — release 0.7.1.2

## Merged

One Windows commit, `10c327a70de864cc49ef265ee329dc5ea87a71f9`, onto Linux
parent `f41e95c6c84774bb7897dc0b043979fb09aee83d` on `port/dev`.
No conflicts. Release notes, installer version, Windows title-panel fallback
and shared Python lobby version are retained byte-for-byte from upstream.
The merge remains staged and uncommitted; release is **0.7.1.2**.

## Ported (what and how)

The native title panel already uses `TPF2MP_VERSION_STR` at both version-label
sites (`native/linux/src/menu_title_linux.inl`). Native CMake reads
`installer/VERSION`, tracks it with `CMAKE_CONFIGURE_DEPENDS`, and supplies the
compile definition. Rebuilding therefore updates both native labels without
copying Windows wide-string code. The built `tpf2_menu.so` contains `v0.7.1.2`.
The shared lobby now advertises and requires 0.7.1.2 on every platform.

Updated README and INSTALL release links, the packaged integration record,
BUILDINFO Lua provenance and the verifier target to this commit. Its mod tree
is unchanged from the previous 65302e5d baseline. Added 0.7.1.1 rejection cases
to the existing host/relay admission and client welcome/roster tests.

Reviewed the native menu/version path and existing RE records for sidecars
(DEV_65302E5D) and memory policy (DEV_0EB9EEA2). This commit introduces no
engine address, hook, byte pattern, layout, lifetime or calling convention;
no new static disassembly or live RE is needed.

## Not ported

None from this release-metadata commit. Upstream release notes are preserved
as upstream claims, not new native test results. Earlier limitations remain:
native terrain sidecars are experimental and default off; material-index
acceleration/probing remains unported (UPSTREAM_dev_7e3d3bfa and
UPSTREAM_dev_8066c58f); `commit_tight_mb` is Windows-only and native paging
uses MemAvailable (UPSTREAM_dev_0eb9eea2). Prior company/UI and loaded-world
validation gaps are not resolved by changing the release number.

## Live testing

No game, Proton peer, debugger or desktop input was run. No lab payload or
save was changed, so backup/restoration was unnecessary. Steam and installed
user files were untouched. No rendering, Vulkan device, title-menu screenshot,
performance or cross-platform gameplay result is claimed. The compiled version
string check is artifact evidence, not a live UI observation.

## Tests

- `tools/linux/build_native.sh`: PASS, pinned soldier SDK, 78/78 CTests in
  62.92 seconds; glibc <=2.31 compatibility checks passed.
- `python3 tools/linux/verify_lua_release.py`: PASS, 31 exact Lua files, zero
  exceptions, 32 HUD glyphs and two toolbar textures. Manifest SHA-256:
  `1de350a7196751cdbdc1e50bf3b4bb29fe72b547e3a109bf4ec813b84d949673`.
- `.git/port-venv/bin/python tools/version_gate_test.py`: PASS, 2 tests,
  including previous-release host/relay and client rejection. System Python
  initially lacked pystun3; installed pystun3/zstandard in a clone-local venv.
  Full requirements installation failed because optional miniupnpc needs absent
  Python.h headers; loopback tests need no UPnP and passed without it.
- Native menu artifact contains `v0.7.1.2`; incoming files match upstream;
  incoming mod tree matches 65302e5d; release shell syntax and whitespace pass.
- Windows/MSVC compilation, release packaging and live gameplay were not run.

Logs are retained in `.git/port-10c327a7-*.log` and
`native/linux/out-soldier/Testing/Temporary/LastTest.log` in this clone.
