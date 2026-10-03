# Windows dev aaae03f8 integration

## Merged

Merge Windows `aaae03f8f4c4731f7cb7e626b137fa113eed0a23` into native parent
`c3c18b65854fff732a5984cf44e4438f27c072d8`. The two incoming commits are
`df1436c` (Windows GOG depth-12/13 requests fall back to depth 11, with a
matching 512-tile ceiling) and `aaae03f` (batch deployment handles paths
containing parentheses). No conflicts. All seven incoming files are retained
unchanged from the Windows target. Merge stays staged and uncommitted.

## Ported (what and how)

The native counterpart already satisfies the applicable ceiling contract:
`bigmap/linux/bigmap_linux.cpp` uses the same validated depth for `EdgeTiles`
and the root patch. Disabling octree widening caps tiles at 256; explicit
lower caps still apply. Add a 30-case native regression matrix covering depths
11/12/13, widening on/off, and requested ceilings 128/256/512/1024/2048.
It checks both the effective ceiling and the published root depth (or absence
of octree patches when disabled). Existing tests cover byte mismatches,
unsupported builds, root-last publication and rollback.

Do not introduce a GOG selector or silently downgrade unknown Linux binaries.
The native host accepts only Steam ELF build-id
`3a0e156390b0e6f1e372051c24802c8493ae454a`, whose depth-12/13 implementation
was ported in [bd69b864](UPSTREAM_dev_bd69b864.md). Unlike Windows GOG, its
child-ID and level sites are already verified. Native runtime code, SysV ABI,
addresses and byte guards are unchanged. The existing evidence is in
[Big Maps PORT](../../bigmap/docs/linux/PORT.md#octree-depth-1213-experimental)
and [DEV_BD69B864](../re/linux/DEV_BD69B864.md#octree).
Read-only verification against the supplied lab ELF again passes all 29 sites.

The deployment subroutine and registry/path parsing belong to Windows
`build.bat`. Native `build_native.sh` and `build_bigmap.sh` only build/test;
neither has a `-deploy` path. The unified release packages the Linux plugin
and its configuration together in `lib/plugins`, installed by `install.sh`.
No equivalent batch expansion fix is needed. Native host search precedence
(data, library, game plugin directories) is unchanged; these commits do not
add a general duplicate-install detector to the Linux installer.

Correct stale installation text calling depth 13 unsupported; native defaults
remain depth 11/512 and deeper roots remain experimental. Link this record
from README/install documentation and include it in release packaging.
No Lua, texture, release version or verifier baseline change is needed.

## Not ported

None of these two commits requires additional native runtime work. Windows GOG
executable support and cmd.exe parsing are platform-specific, not omitted
Linux features. Existing placement-distance/attempt-budget gaps, terrain
sidecars and pending depth-12/13 live validation are unchanged; this record
does not certify those older features or cross-platform gameplay parity.

## Live testing

No game was launched and no actor payload, save, Steam state or user install
was modified. These changes introduce no native runtime patch or unresolved
ABI contract requiring live RE. No rendering, load/save or gameplay result is
claimed. Upstream's GOG live observations were not repeated locally.

## Tests

- `tools/linux/build_native.sh`: soldier build, **66/66 CTests** and glibc
  <=2.31 compatibility checks pass, including the 30 new ceiling cases.
- `python3 tools/linux/verify_lua_release.py`: 29 Lua files (one pinned native
  exception) and 32 HUD glyphs pass. Manifest SHA-256:
  `b67ff99a9d55f248af9ea23290c96bbf0904cd83b533338bcf48d16878023aaf`.
- `python3 bigmap/tools/linux/verify_game.py <lab-native-ELF>`: GNU build-id
  and all 29 guarded patch sites pass, read-only.
- Shell syntax checks for native, Big Maps and release build scripts pass.
  Staged whitespace checks and an empty unmerged index pass; all seven
  incoming Windows files remain byte-identical to the target.
- Windows DLL/MSVC and batch execution tests were not run on this Linux host.

Build and Lua verification logs are retained in `.git/port-aaae03f8-*.log`.
