# Windows dev 192ecd4d integration — release 0.7.1.5

## Merged

Windows target: `192ecd4d275e542ee8844bc88e067bdc03032365`.
Linux parent: `501b47ff3ff690297313687cd7db18adf9f9da81`.
Three commits: `91bc601` (CrossOver Vulkan loader hook), `2d7fecc`
(host-loop transfer lifetime), and `192ecd4` (release 0.7.1.5).
No conflicts. All eight incoming files are retained byte-for-byte from the
Windows target. The merge remains staged and uncommitted.

## Ported (what and how)

- The shared Python host clears `xfer` and `tx` after each pass, releasing
  completed save/mod and terrain sender references without waiting for another
  transfer. Active senders remain owned by the transfer collections. Native
  Linux and dedicated servers use this same `netpunch/lobby.py`.
- The native Vulkan overlay already avoids loader-prologue dependence. Its
  existing guarded dispatcher hook covers the purpose of the CrossOver fix;
  adding the Windows RIP-relative relocation code would be unnecessary.
  [Native mapping, ABI and fresh ELF checks](../re/linux/DEV_192ECD4D.md).
  Windows implementation and its MSVC regression remain intact.
- `installer/VERSION` feeds native CMake and Linux packaging; the built menu
  contains `v0.7.1.5`. Shared `LOBBY_VERSION` gates peers at 0.7.1.5. Extended
  host/relay/client tests reject 0.7.1.4, as well as older or malformed versions.
- Updated README/INSTALL, packaged integration record, release BUILDINFO and
  Lua verifier reference. Lua and textures are unchanged, exactly upstream.

## Not ported

None for these three commits. The Windows/CrossOver loader implementation is
platform-specific; native Linux uses the existing equivalent dispatcher route.
Earlier native feature gaps and validation limitations are unchanged. The
upstream Mac and memory-size claims are not new locally measured results.

## Live testing

No game was launched. This integration changes shared Python lifetime policy
and release metadata; the native overlay needs no new engine contract or
patch. Only the lab ELF was read for static confirmation. No lab payloads,
saves, Steam files, installed mod, desktop input or running game were changed,
so no actor restoration was necessary. No GPU, title panel rendering, loaded
world, gdb result, cross-platform game session or RSS reduction was observed.

The Python tests ran real loopback host/client threads, with simulated engines where noted; they are not game
validation. The new lifetime fixture injects small synthetic senders at the
production host-loop boundary and uses weak references to check collection
while the host still runs, without a subsequent transfer or process exit.
It covers resolved save/mod senders with no recipients, failed recipients,
and pump exceptions, together with a terrain sender in each scenario. Active
senders survive the preceding pass. No gigabyte-sized memory claim is made.

## Tests

- `tools/linux/build_native.sh`: PASS, pinned soldier build, **79/79 CTests**
  (62.85 s) and runtime glibc baseline checks.
- `python3 tools/linux/verify_lua_release.py`: PASS, 31 Lua files, zero merge
  exceptions, 32 HUD glyphs and two toolbar textures. Manifest SHA256:
  `f372ad8ebf2800b6b74d457def98c4975123a5551ce0faaca53bb0323a309b5b`.
- `tools/test_host_transfer_lifetime.py`: PASS, one test with four scenarios.
  An in-memory mutation removing `xfer = tx = None` fails all four scenarios;
  repository production source was not modified for that negative control.
- `tools/version_gate_test.py`: PASS, two tests, including previous-version
  rejection by host/relay and client welcome/roster paths.
- `tools/test_live_join_lobby.py`: PASS, four loopback tests.
- `tools/test_transfer_status.py`: PASS, three tests.
- `tools/test_auto_sync_lobby.py`: PASS, two-player loopback recovery suite,
  simulated engine (resync/retry, readiness, lost ACKs, mismatch, leaver, join).
- Built menu version string, shell syntax, upstream-file identity, documentation
  links and staged whitespace checks passed; no unmerged index entries.

Python tests use `.git/port-venv` with pystun3/zstandard installed locally.
Logs are `.git/port-192ecd4-{build,lua,lifetime,mutation,version,livejoin,transfer,autosync,disassembly}.log`.
The Windows MSVC hook test and release packaging were not executed here.
No commit, merge abort, publication or system package installation.
