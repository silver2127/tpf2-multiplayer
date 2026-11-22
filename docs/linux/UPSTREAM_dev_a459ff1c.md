# Windows dev a459ff1c integration — accept the menu export in Linux releases

## Merged

Windows target: `a459ff1c56b21439842789908b478223b6e7bfa8`.
Linux parent: `a03d77b047ad1d02033621eb56d15f6c20dc10e9`.
One incoming commit (`a459ff1`, `build_release.sh: the menu may export
Tpf2mpLastGameUiTick`), no conflicts. Merge staged and left uncommitted.
Release remains **0.7.1.4**.

## Ported (what and how)

The upstream change already targets the native Linux release script. Retained
its conditional allowlist exactly: when `native/linux/exports_menu.map` exists,
`tpf2_menu.so` must export exactly `Tpf2mpLastGameUiTick`. Without that map it
still expects no exports. The loader still exports only `__sprintf_chk` and
`clock`; plugins export only `Tpf2mpPluginInit`, and the other loaded libraries
export nothing. Unexpected or missing exports still fail release packaging.

The previous integration already links the menu with this map and implements
the timestamp. See [its integration record](UPSTREAM_dev_4487d7cd.md) and
[the existing ELF/ABI evidence](../re/linux/DEV_4487D7CD.md). This change fixes
a packaging rejection, not the timestamp implementation. No engine hook,
address, byte pattern, ABI, field offset or game-owned lifetime changes, so
new static disassembly and live gdb probes are unnecessary.

Updated the release script's export comments, README/INSTALL and packaged
integration record list. Extended `test_release_assets.py` to read the actual
packaged ELF libraries with `nm`, checking the menu's sole export and the
loader/other-library/plugin export contracts independently of the script's
map-file condition. Existing asset/checksum rejection checks remain.
Shared Lua, its 4487d7cd verification baseline, release BUILDINFO's Lua source
identifier and all Windows code paths are unchanged.

## Not ported

None from this commit. Prior native feature gaps and outstanding gameplay
validation remain as recorded in earlier integrations.

## Live testing

No game, Proton peer, debugger or desktop input was run: this is a release
packaging change with no runtime code changes. No lab payload or save was
modified, so no backup/restoration was needed. Steam and user installations
were untouched. There is no new on-screen, GPU, gameplay or performance result.

## Tests

Validation commands and evidence for this integration:

- `tools/linux/build_native.sh`: PASS, pinned soldier SDK build, 79/79 CTests
  (62.73 seconds) and glibc <=2.31 baseline checks. Log: `.git/port-a459ff1-build.log`.
- `python3 tools/linux/verify_lua_release.py`: PASS, 31 exact upstream Lua files,
  zero exceptions, 32 HUD glyphs and two toolbar textures. Lua manifest SHA256:
  `f372ad8ebf2800b6b74d457def98c4975123a5551ce0faaca53bb0323a309b5b`. Log: `.git/port-a459ff1-lua.log`.
- `tools/linux/build_release.sh --build-dir native/linux/out-soldier --out dist/linux-a459ff1 --no-netpunch`:
  PASS, 79/79 CTests (62.77 seconds), export checks, staged Lua checks and
  local native archive/installer creation. The lobby is deliberately omitted; BUILDINFO records that choice.
  Log: `.git/port-a459ff1-release.log`.
- `TMPDIR="$PWD/.git/port-tmp" python3 tools/linux/test_release_assets.py dist/linux-a459ff1 0.7.1.4`:
  PASS, packaged exports, asset names, legacy bytes, executable permissions,
  checksums and corrupt/missing asset rejection. Log: `.git/port-a459ff1-assets.log`.
- Export-gate regression on the actual built menu: the pre-merge gate exits 1
  because it expects no export; the merged gate exits 0. PASS. Log:
  `.git/port-a459ff1-export-regression.log`.
- Shell syntax, documentation links (383 documents; no dead file links,
  anchors or prose paths), whitespace and unresolved-index checks: PASS.

Final results are recorded in the job's `meta/REPORT.md`. No Windows MSI build,
commit, merge abort or publication.
