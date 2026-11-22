# Windows dev 40e12f76 integration

## Merged

Target `40e12f76bb2e123d5fa39d90736defa34d48d1c2`, one Windows commit,
onto Linux parent `3d70ceda4a7e49aa28ac469151a4adb4342287d4`.
Resolved `bigmap/linux/bigmap_linux.cpp` by retaining the Linux trampoline
publication and activation gate and adding the incoming config assignment.
Removed the incoming obsolete temporary trampoline variable. The merge is
staged and deliberately uncommitted. Release remains **0.7.1.1**.
Windows `bigmap.cpp` and Windows config are retained exactly as supplied by
upstream. Shared sidecar changes are merged while preserving the earlier
malformed-stream storage fix; Linux config retains its experimental default off.

## Ported (what and how)

The native plugin reads `terrain_sidecar_read_local` with default 1. Setting
it to 0 makes the shared loader ignore both the save's own `.terr` and sibling
sidecars found by fingerprint. A configured stream directory still arms the
load, allowing a stream to arrive later. Without a stream, stock terrain
computation runs; sidecar writing is unaffected.

The Linux hook fixture now exercises local defaults, matching local and sibling
files ignored in stream-only mode, both inputs disabled, and a successful stream
with a complete local file still visible. Existing incremental, late-arrival,
malformed-stream and post-load-edit regressions also run with local reads off
and the local file present. Plugin initialization tests check default-on,
explicit-off and default restoration. Updated Lua/package provenance and
included this record in release packaging.

Reviewed README, Linux installation/integration records, and
[the existing native sidecar RE evidence](../re/linux/DEV_2B4FD093.md).
This commit changes file selection only: no addresses, byte patterns, struct
offsets, calling conventions or hook contracts change. The actual lab ELF's
build-id and all 39 existing Big Maps guard sites pass the read-only verifier.
No new disassembly or live ABI probe is needed for this switch.

## Not ported

None from this commit. Native sidecars remain experimental and default off
(`terrain_sidecar=0`). Inherited live terrain ownership, lifetime, load scheduling
and cross-platform validation limitations remain; this integration does not
resolve or promote those earlier contracts.

## Live testing

No game, Proton peer, gdb session or desktop input was run. Validation of this
file-selection change uses native hook/file fixtures, not live gameplay.
No title menu, renderer, streamed game load or timing result is claimed.
Lab actor payloads and saves, Steam and the user's installed mod were untouched;
no restoration was necessary and no game process was started.

## Tests

- `tools/linux/build_native.sh`: PASS, soldier build, 78/78 CTests and
  glibc <=2.31 checks (final suite: 62.97 seconds). Repeated after adding
  the config initialization test.
- `python3 tools/linux/verify_lua_release.py`: PASS against this incoming
  commit: 31 exact Lua files, zero exceptions, 32 glyphs and two toolbar
  textures. Manifest:
  `1de350a7196751cdbdc1e50bf3b4bb29fe72b547e3a109bf4ec813b84d949673`.
- `python3 bigmap/tools/linux/verify_game.py GAME_ELF`: PASS, GNU build-id
  and all 39 guarded sites.
- Release shell syntax, CRLF-aware staged whitespace checks, no unmerged
  index entries, and exact upstream Windows source/config comparison: PASS.
  The shared header and Linux config retain reviewed pre-existing differences.
- Windows/MSVC compilation and live multiplayer were not run.

Logs are retained in this clone's
`.git/port-40e12f76-{build,build-final,lua,elf}.log`.
