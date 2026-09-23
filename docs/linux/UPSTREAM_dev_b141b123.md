# Upstream dev b141b123 integration: shell Proton installer

Windows target: `b141b1238e4559aed4ec1ed97a51e963e455923d`.
Linux merge parent: `7051ea2` (dev d129fab7, see
[UPSTREAM_dev_d129fab7.md](UPSTREAM_dev_d129fab7.md)). One incoming commit.
The merge remains staged and uncommitted. The native integration status is
unchanged from 23419163 (partial; stationicon and the resync title-menu
exception are still not ported, see
[UPSTREAM_dev_23419163.md](UPSTREAM_dev_23419163.md)).

## Merged

| Commit | Change | Linux |
| --- | --- | --- |
| b141b12 | `tools/proton/install_proton.sh` (bash installer for the Windows game under Proton) and its offline test `tools/proton/test_install_sh.py`; README, `docs/DEVELOPMENT.md`, `docs/proton/INSTALL.md`, `installer/RELEASE-0.6.md` name it first | shared tooling and documentation |

No merge conflicts. The README hunk sits in the Windows/Proton install section,
below the Linux-native paragraph, and applied cleanly.

## Ported

No change touches the game binary, the Windows hooks, MSVC code or the Lua mod,
so there is nothing to reverse engineer. The shell installer targets the
Windows game under Proton (it checks the PE header of `TransportFever2.exe`
for build 35924) and refuses nothing Linux-native needs; the native Linux game
keeps its own `.run`/`install.sh` installer ([INSTALL.md](INSTALL.md)).

- Lua/release provenance now targets b141b123 (`verify_lua_release.py`,
  `build_release.sh` BUILDINFO and packaged record, README, INSTALL.md).
  `git diff d129fab b141b12 -- mod/` is empty.

## Not ported

Nothing.

## Tests

- `tools/linux/build_native.sh`: soldier build, **45/45 CTests**.
- `python3 tools/linux/verify_lua_release.py`: 28 exact Lua files and 32 glyph
  textures against b141b123.
- `python3 tools/proton/test_install_sh.py` (new) and
  `python3 tools/proton/test_install.py` pass on this Linux host.

No game or Steam launch, installation, publication, commit or merge abort.
