# Windows dev 2b4fd093 integration

## Merged

Target `2b4fd0937796ceb29d4d9dd5cad8e906ba0ace5d`, one commit adding the
native Linux terrain sidecar. Release remains **0.7.1.1**. Resolved all three
conflicts and staged the merge without committing: PORT.md retains historical
records plus the new implementation; CMake keeps the shared codec test and
registers the native flow as `terrain_sidecar_linux`; config adds sidecar
settings with a default-off experimental gate. Windows code is unchanged.

## Ported (what and how)

Retained the incoming SysV SaveGame/LoadGame/AddTile hooks, matching `.terr`
format, save fingerprinting, threaded capture, per-grid served ranges and
complete-grid alignment bypass. Independently checked all 39 ELF sites and
re-derived arguments, record indexing and publication stores from disassembly.
See [RE evidence](../re/linux/DEV_2B4FD093.md).

Fixed partial installation: trampolines are published directly through the
host; an atomic activation gate leaves callbacks forwarding until every hook
succeeds. Version increments use unsigned wraparound. Added tests for failed
hooks, disabled forwarding, missing-config default and two terrain versions.
Preserved the existing shared codec/thread/corruption suite. Lua verifier and
package provenance now reference the exact incoming commit; package the record.

## Not ported

Production/default-on enablement is withheld. Both static and live attempts
were made, but the lab cannot start a game. `terrain_sidecar=0` is therefore
the compiled and packaged default; setting it to 1 opts into the experimental
implementation. `terrain_sidecar_write=1` and `terrain_sidecar_threads=0`
remain its subordinate defaults. Stock alignment remains the default.

Missing proof: live saved-world terrain selection/lifetime, writable cache
ownership and pager/encoder synchronization, multi-version served-mark and
load-completion lifetime, and full/partial/fingerprint-mismatch save/load
comparisons with subsequent terrain edits. Earlier unrelated gaps remain.

## Live testing

Two native lab attempts, including the final candidate with sidecars explicitly
enabled, exited 1 before game execution: `bwrap: setting up uid map: Permission
denied`. No title menu, GPU/Vulkan selection, gdb attachment, save/load or
alignment skip was observed. No Proton or desktop-input test was attempted.
Both actor payloads were backed up and restored, with matching content hashes
and symlink targets. A second archive copy encountered an existing symlink;
explicit restoration afterward succeeded and was verified against both backups.
No saves changed; no game remains. Steam/user installs were untouched.
Evidence is in job `meta/live/`; copied actor logs/data predate these launches.

## Tests

- `tools/linux/build_native.sh`: PASS, final soldier build and 77/77 CTests;
  glibc symbol baseline <=2.31.
- `python3 tools/linux/verify_lua_release.py`: PASS, 31 Lua files, zero
  exceptions, 32 HUD glyphs and two toolbar textures at the incoming commit.
  Manifest `1de350a7196751cdbdc1e50bf3b4bb29fe72b547e3a109bf4ec813b84d949673`.
- `python3 bigmap/tools/linux/verify_game.py GAME_ELF`: PASS build-id and
  all 39 guarded sites.
- `bash -n tools/linux/build_release.sh`: PASS.
