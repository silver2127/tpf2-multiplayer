# Windows dev c74a7b4e integration: TCP addresses behind the master relay

## Merged

Windows target: `c74a7b4ef02beba74453d0f3c21041c2c0512d2d`.
Linux parent: `20a169653207aa9f912e5db2ac09197255eec3a7`.
One incoming Windows commit, no conflicts. Merge stays staged and uncommitted.
Release and lobby handshake version remain 0.7.0.4.

The incoming change updates shared `netpunch/lobby.py` and
`tools/test_tcp_connectivity.py`. Both the host and joiner remember the master's
UDP relay endpoints. The host includes its TCP addresses in the transfer offer
for relayed targets, as it already does for Steam targets. A relayed joiner
tries those addresses in a background thread while UDP proceeds; without
advertised addresses it immediately leaves the transfer to UDP. A direct
joiner continues to use its lobby peer's address.

## Ported (what and how)

The native Linux lobby uses the same Python implementation, as documented in
[NETPUNCH.md](NETPUNCH.md). Retain the upstream change directly, including the
upstream regression for named host addresses, no addresses and direct joins.
Existing Linux lifecycle, paths and socket behavior remain present.

Add a host-side regression to the same test suite: relayed-only and mixed
relay/direct target lists advertise a copy of the host's IPv4/IPv6 addresses;
a direct-only target list does not add the address list. The tests retain
coverage of Linux bound TCP dialing and router-wrapper cleanup.

Advance Lua verification and release BUILDINFO provenance to this commit,
package this record, and link it from README and Linux INSTALL. The Lua and
native libraries require no source changes. No Windows hook, MSVC code, ELF
address, byte pattern, structure offset, ownership contract or ABI changes in
this commit require static or live reverse engineering. Existing evidence and
limitations in `docs/re/linux/` remain applicable.

## Not ported

None for this commit. Earlier native gaps and cross-platform gameplay
validation limits are unchanged.

## Live testing

No game launched, gdb attached, or desktop input sent. No lab actor payload,
actor save, Steam installation or user-installed mod was changed; no backup
or restoration was needed. No rendering, loaded-world, public-master/NAT,
real Steam transport or Windows/native gameplay result is claimed.

Local Python tests used real loopback sockets: the rendezvous test started a
local master and verified relay allocation, bind/forwarding, expiry and a
joiner connecting through the master's relay. Transfer self-tests verified
clean/lossy UDP, a 64 MB TCP transfer, unreachable-TCP fallback and failed-peer
retry. Relay TCP address selection itself was checked with deterministic
mocked dial calls, not a live internet transfer. Router mapping calls were
mocked; no router configuration changed.

## Tests

- `tools/linux/build_native.sh`: PASS; pinned soldier build, 68/68 CTests,
  glibc <=2.31 compatibility checks.
- `python3 tools/linux/verify_lua_release.py`: PASS; 29 Lua files with one
  pinned native exception, 32 exact HUD glyphs. Manifest SHA-256:
  `d23c25e33ce4089c724211236a1344e7f710d4a6fb7c431f2d60f4a6838892e2`.
- `.git/port-venv/bin/python tools/test_tcp_connectivity.py`: PASS, 11 tests,
  including real IPv4/IPv6 streams, authentication and the new host regression.
- Same interpreter, `tools/test_transfer_status.py`: PASS, 3 tests;
  `tools/rendezvous_test.py`: ALL OK.
- Same interpreter, `netpunch/lobby.py --selftest`, `--selftest-relay`,
  `--selftest-transfer`, `--selftest-dual`: all PASS.
- `bash -n tools/linux/build_release.sh`, whitespace and merge-state checks:
  PASS. No unmerged entries; MERGE_HEAD retained.

System Python initially lacked `stun`; a clone-local venv supplies pystun3 and
zstandard. Test temporary files were directed inside `.git/port-tmp`. No
system package installed. No release package or frozen lobby was built; the
existing builder consumes the updated shared sources. Windows executable
and MSI tests were not run on this Linux host.

Logs: `.git/port-c74a7b4-{build,lua,tcp,transfer-status,rendezvous,selftest,selftest-relay,selftest-transfer,selftest-dual,deps}.log`.
