# Windows dev 9602a389 integration: terrain streaming

## Merged

Target `9602a389bbd39da7335d30c02f840f6aaf7f0863`, one commit, onto Linux
parent `28d13ff37fdf2cdfa93119f37351b116bde1a6b7`. Resolved and staged
`bigmap/linux/bigmap_linux.cpp`, `sidecar_linux.h` and `tests/sidecar_test.cpp`.
The merge remains uncommitted. Release remains **0.7.1.1**.

Retain the incoming host/joiner terrain-transfer protocol, Windows hooks,
configuration and tests. Shared Lua and assets do not change; the Lua verifier
continues to use its exact `926b9a2c` baseline.

## Ported (what and how)

Combine upstream's Linux streaming implementation with existing activation
guards and trampoline publication. Streams are read from the host data
directory, checked against the loaded save fingerprint, incrementally indexed,
and applied at AddTile or caught up at the pass. Complete coverage permits
one bypass per terrain version; otherwise Linux computes the full pass.
Release writes the stream's done marker. Native `terrain_stream=1` is usable
only with the existing experimental `terrain_sidecar=1` opt-in.

Retain upstream's four-candidate terrain selection using self
`process_vm_readv`, replacing the last-seen pointer. Guarded reads reject
inaccessible descriptors; they do not prove that readable memory still belongs
to the saved world. No new hook address or ABI is needed. All 39 static ELF
checks pass; see [RE evidence and limitations](../re/linux/DEV_9602A389.md).

Add native regressions for inaccessible terrain/grid/vector memory and a
stream first appearing after load completion. Disarm discovery at native
PassDone and reset it at every enabled LoadHook. Fix the shared stream index
to retain backing storage for accepted records before a malformed suffix.
The regression decodes that valid prefix safely and refuses the pass bypass.
Windows-specific paths remain intact; the shared malformed-input fix also
benefits Windows. Existing disabled-hook and two-version tests are preserved.

## Not ported

Production enablement and live proof of saved-world terrain identity,
selection-to-capture lifetime, and pass-time cache ownership/pager interaction
remain incomplete. Both static investigation and a candidate lab launch were
attempted; the lab failed before game execution, so gdb could not settle these
contracts. All incoming implementation is present experimentally, but default
native sidecars remain off. No cross-platform gameplay or performance parity
is claimed. Job status is PARTIAL for these unresolved live contracts.

## Live testing

Installed final candidate soldier libraries, Big Maps config with sidecars
and streams enabled, merged Lua and autoload flags into the native actor,
after backing up both payload directories with cp -a. Ran
`tools/sandbox/tpf2mp-lab run native --root ~/.local/share/tpf2mp-lab`.
Exit 1: `bwrap: setting up uid map: Permission denied`, before game startup.
No title menu, Vulkan device, gdb attachment, terrain/save behavior or visible
gameplay was observed. No Proton or desktop-input test was performed.
Both actor payloads were restored and their hashes/symlink targets match the
backups. Saves and Steam were untouched; no game remains. Evidence is in this
job's `meta/live/`; archived actor logs/data include historical material.

## Tests

- `tools/linux/build_native.sh`: PASS, final soldier build, 78/78 CTests
  (62.66 seconds), glibc symbol baseline <=2.31. Includes shared codec,
  native hook-flow, new stream regressions and hook-install failure tests.
- `python3 tools/linux/verify_lua_release.py`: PASS, 31 exact Lua files,
  zero exceptions, 32 HUD glyphs and two toolbar textures. Manifest:
  `1de350a7196751cdbdc1e50bf3b4bb29fe72b547e3a109bf4ec813b84d949673`.
- `tools/test_terrain_stream.py`: PASS, 5 tests, including loss/reordering,
  in-order disk prefixes, hash mismatch, done cancellation and refusals.
- `tools/test_lobby_limits.py`: PASS, 32 tests.
- `netpunch/lobby.py --selftest`, `--selftest-transfer`, `--selftest-mods`:
  PASS, including lossy UDP, TCP and UDP fallback transfer rounds.
- Release-script syntax and staged/unstaged whitespace checks: PASS
  (CRLF-aware for the unchanged Windows file format).
- `bigmap/tools/linux/verify_game.py GAME_ELF`: PASS, build-id and 39 sites.

Python networking tests use `.git/port-venv`: system Python lacked pystun3;
the complete requirements installation hit a Python 3.14 miniupnpc build
failure. Installed pystun3 and zstandard only into that venv; miniupnpc is
optional for these tests. No system packages were installed. Build and test
logs are `.git/port-9602a38-*.log`. Windows/MSVC tests were not run on Linux.
