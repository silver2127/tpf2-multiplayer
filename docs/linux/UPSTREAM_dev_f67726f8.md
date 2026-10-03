# Windows dev f67726f8 integration: release 0.7.0.5

## Merged

Windows target: `f67726f8ba7a0bfa979af7b513896ae746dd7e50`.
Linux parent: `cdce25b02587ef9fa1c433c0fa1f03d7de821304`.
One incoming Windows commit, no conflicts. Merge staged and uncommitted.

Retain both upstream release notes unchanged (`docs/releases/0.7.0.5.md`
and `installer/RELEASE-0.7.0.5.md`), plus the shared `installer/VERSION`
and `netpunch/lobby.py` LOBBY_VERSION bumps to 0.7.0.5. All participants,
including dedicated servers, must update.

## Ported (what and how)

The native package builder already consumes `installer/VERSION`; the native
lobby uses the same Python version gate as Windows. Advance the Lua verifier
reference and package BUILDINFO provenance to this Windows release, retaining
the pinned Linux origin-replay exception in `inject.lua`. Package this record
and link it from README and Linux INSTALL.

The features described in the release notes were already integrated:
[c74a7b4e](UPSTREAM_dev_c74a7b4e.md) fixes TCP address selection behind the
master's UDP relay; [f6e47ef9](UPSTREAM_dev_f6e47ef9.md) adds the master's
TCP pipe fallback and preserves native lobby shutdown cleanup. This incoming
commit changes only version constants and release documentation. There are no
new Windows hooks, MSVC code, ELF addresses, byte patterns, struct offsets or
ABI/lifetime contracts to port or reverse engineer. Existing `docs/re/linux/`
evidence and its limitations remain applicable.

Extend the existing host admission and client rejection tests with the previous
0.7.0.4 version. Host coverage includes dedicated relay mode; clients reject
old welcome/roster messages before processing game or save state.

## Not ported

None for this commit. Existing native feature gaps and gameplay-validation
limits are unchanged; this release stamp does not establish gameplay parity.

## Live testing

No game launched, gdb attached or desktop input sent. No lab actor payload,
save, Steam file or user-installed mod was changed; no restoration was needed.
No rendering, GPU, loaded-world, internet/NAT throughput or Windows/native
gameplay result is claimed. The measurements in upstream's release notes
were not repeated here.

Tests exercised real Linux loopback host admission in normal and relay modes,
IPv4/IPv6 streams, master TCP pairing and an exact 3 MiB + 17 byte payload
through the pipe. The lobby self-test observed roster/company updates, merged
logs, chat, legacy start, late-join handling and stale incoming-save removal.
Router calls and slow-transfer selection were mocked; no router was changed.

## Tests

- `tools/linux/build_native.sh`: PASS; pinned soldier SDK build, 68/68
  CTests and glibc <=2.31 compatibility checks.
- `python3 tools/linux/verify_lua_release.py`: PASS; 29 Lua files with one
  pinned native exception and 32 exact HUD glyphs. Manifest SHA-256:
  `d23c25e33ce4089c724211236a1344e7f710d4a6fb7c431f2d60f4a6838892e2`.
- `.git/port-venv/bin/python tools/version_gate_test.py`: PASS, 2 tests,
  including previous-release rejection on host, dedicated relay and client.
- Same interpreter, `tools/test_tcp_connectivity.py`: PASS, 14 tests;
  `netpunch/lobby.py --selftest`: PASS.
- `bash -n tools/linux/build_release.sh`, upstream release-file byte equality,
  version consistency, staged whitespace and merge-state checks: PASS.
  No unresolved index entries; MERGE_HEAD retained.

Logs are retained in `.git/port-f67726f8-{build,lua,version,tcp,lobby,deps}.log`.
System Python lacked `stun`; pystun3 and zstandard were installed in the
clone-local `.git/port-venv` with pip caching disabled. Test temporary files
were directed to `.git/port-tmp`. No system package, release package, frozen
lobby, Windows executable or MSI was installed/built.
