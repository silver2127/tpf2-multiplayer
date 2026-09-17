# Upstream dev 38432b5f integration

Windows target: `38432b5f0fbbddc6aac213e8fbeafd30affbe72a`.
Baseline: native Linux after [1d0ca473](UPSTREAM_dev_1d0ca473.md).
The merge is staged and uncommitted.

## Merged

All 16 incoming commits, including merge commits:
`eac4b2d`, `ab79e4e`, `afd1a42`, `8fe73e7`, `6eb4854`, `f829de9`,
`459f4d1`, `d942d34`, `c1ec1e2`, `e47ab12`, `6411076`, `156824d`,
`a3fc626`, `dc0da09`, `2e24f55`, `38432b5`.

The lobby conflict was caused by line-ending differences. A three-way merge
with LF-normalized inputs preserved the Linux paths, process shutdown and
transport behavior while adding sender-side transfer stages and host stages.
No conflict markers remain. Windows native sources and updater match the
incoming target byte-for-byte.

All 28 Lua files exactly match the target: dashboard tabs and company rows,
live company status, governor kill switch, saved vehicle/line keys, repeated
command delivery, immediate NACK/holds, and arrival deduplication. Release
provenance and the Lua verifier identify this target. Manifest SHA-256:
`a99ffaf122e08385682cf8a644b9d510b1aca4359bc138d673af6514ee5db6fe`.

## Ported

- Paused tick: statically located Linux GameSim::Step and GameTime advance,
  verified SysV arguments and both counter increments. Six byte guards precede
  a five-byte NOP of the paused call at `0xa61860`; running advance stays intact.
  Existing safe writer/readback and build-id gates apply; `pausedtick=0` opts out.
- World load progress: derived the native menu/bar/monitor chain from the
  `m_progressBar` assertion, shared_ptr accessor and RTTI/vtable. Guarded safe
  reads report 0..99%, once a second with deduplication, then existing sim stages.
  Host accepted loads arm the watch too. Old-world status is held during switch.
- Return to title leaves the lobby asynchronously, without making the game
  thread wait for process teardown. Waiting joiners and world-switch pages
  are unaffected.
- Shared-station gate uses the sim's current company mode as well as the lobby
  configuration. Existing Linux hook/ABI and hover cache remain intact.

Full mappings, bytes and test limits: [RE evidence](../re/linux/DEV_38432B5F.md).

## Not ported

The Windows MSI-backed in-app updater remains Windows-only. Inspected the
new payload/extraction path, its required DLL/EXE manifest, `msiexec /a`,
`msvcrt` activation lock, Linux boot loader, native lobby and `.run`/tarball
release builder. The MSI has no native ELF payload, and Linux has neither an
in-app release activation path nor a native update-asset contract. Extracting
this package cannot update the native port. The change is preserved for Windows;
native updates continue through the existing Linux installer/build workflow.
A native release manifest/payload and loader activation design are still needed.
No attempt was made to execute Windows Installer or install Windows binaries.

No newly introduced native game hook remains unmapped. Earlier automatic
recovery/in-world client load, company rename/ownership and Workshop gaps remain;
this integration does not claim to close them. In particular, the new watch
cannot add automatic in-world client loading where that was already absent.
No game/Steam run or live compatibility claim.

## Tests

- `tools/linux/build_native.sh`: pinned soldier build, **44/44 CTests** and
  glibc <=2.31 validation passed. Expanded movement, menu-load and lobby tests.
- `python3 tools/linux/verify_lua_release.py`: 28 exact files, manifest above.
- `verify_dev_38432b5f_elf.py`: build-id, 10 byte spans/instruction boundaries,
  SysV paused/running call targets, no interior branch in Step, progress RTTI
  and vtable passed against the actual ELF. Existing movement verifier passed.
- Shared tests: hotjoin stages (real loopback transfer), delay/hold, duplicate
  arrival, saved vehicle/line keys, pacing simulator and lobby companies passed.
- Linux lobby stop: **16/16** checks passed.
- Updater: **8 tests, 1 skipped** (Windows MSI conversion unavailable here).
- Windows source equality, merge-state and whitespace checks passed with
  upstream CRLF recognized (`core.whitespace=cr-at-eol`).

Tests needing Python dependencies used a disposable venv under `.git/`; no
system packages were installed. Logs: `.git/port-*.log`. No commit, merge abort,
push, publication or real-game installation was performed.
