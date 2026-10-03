# Windows dev 568e7ea7 integration

## Merged

Target `568e7ea7e3e29c6ff2cacb1ef6932facd25a35b7`, following
[7111aefb](UPSTREAM_dev_7111aefb.md). Release remains **0.7.1**.
Two commits, no merge conflicts; merge staged and left uncommitted:

- `d1ef108`: Windows worker-thread save compression with vendored zstd 1.5.7,
  shared Streams header and configuration documentation.
- `568e7ea`: generator copies retain original times; Windows metadata-only
  opens bypass redirection.

Windows code paths, vendored library and upstream tests are preserved.

## Ported (what and how)

Linux already implements worker-thread saving with system libzstd. Retained
the incoming shared-header extraction and existing native six-call installer,
byte guards, API probe and partial-install takeover gate. Confirmed unchanged
stream logic and rechecked every save call/guard against the game ELF.

Native Big Maps now snapshots the opened original's timestamps with fstat
and applies access/mtime with futimens after writing/flushing the anonymous
served copy. Timestamp failures serve the original. Native metadata queries
already bypass the read-only fopen hook. Added real-stream timestamp tests
with nanosecond precision, including the emitted hook jump and repeated open.
POSIX has no settable creation timestamp. Details and ABI evidence:
[DEV_568E7EA7.md](../re/linux/DEV_568E7EA7.md).

Advanced Lua verifier/package provenance and current integration links;
package the new integration record. Shared Lua content remains unchanged.

## Not ported

None introduced by these two commits. Existing native Sandbox town capture,
minimap, cargo-filter and other previously documented gaps remain unchanged.
Live preview stability and game save/load validation remain outstanding; the
implemented timestamp behavior is covered by native stream tests.

## Live testing

Backed up both native actor payload directories as `.before-port-568e7ea7`,
installed this build and merged Lua, then ran the prescribed native lab command.
It exited 1 with `bwrap: setting up uid map: Permission denied` before the game
started. No title menu, GPU/Vulkan device, preview, save/load or gdb observation
was possible. No Proton test or desktop input was attempted. Both payloads
were restored and recursively compared equal. No save changed, no game was
left running, and Steam/user installations were untouched.

Evidence: job `meta/live/launch.log`, `launch-result.txt`, `restoration.txt`,
ELF/disassembly logs and copied actor logs/data (the latter are pre-existing).

## Tests

- `tools/linux/build_native.sh`: PASS, soldier build, 75/75 CTests, glibc <=2.31.
- `python3 tools/linux/verify_lua_release.py`: PASS, 30 Lua files, no exceptions,
  32 HUD textures and two toolbar textures at the exact incoming commit.
  Manifest `4f1370ac2781c929d3526bdef7b39d17ddf945af5f25160a8707c7e7528f6e3a`.
- Host-built `save_zstd_test.cpp`: PASS fake routing and real libzstd 1.5.7,
  25,165,824 -> 9,894,603 bytes with four workers. Soldier's optional zstd
  runtime reports `none >= 1.4`, so the separate host test supplies this check.
- `verify_save_steam_elf.py`: PASS five full guards and seven call targets.
- `bigmap/tools/linux/verify_game.py`: PASS build-id and all 30 sites.
- `bash -n tools/linux/build_release.sh`: PASS.
- Staged whitespace check with `core.whitespace=cr-at-eol`: PASS; incoming
  Windows CRLF endings are preserved.
