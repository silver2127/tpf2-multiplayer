# Windows dev f6e47ef9 integration: TCP pipe for slow transfers

## Merged

Windows target: `f6e47ef9def21f2af4905bc05e43f14972b7ae11`.
Linux parent: `76e8911f47ef00c9bde469bc62165965ecac5b46`.
One incoming commit. Conflicts in `netpunch/lobby.py` and
`tools/test_tcp_connectivity.py` resolved; merge staged and uncommitted.
Release and lobby handshake remain 0.7.0.4.

Retain the shared bulk socket handshake, master TCP pairing service and
`GET /pipe`, host slow-transfer selection, joiner pipe handling and deployment
script changes. Both peers dial outward and then use the existing token-checked
bulk stream. At least 16 MiB, 15 seconds since readiness and an estimated
60 seconds remaining are required; each peer gets one pipe attempt. Existing
UDP/Steam transfer proceeds until TCP starts. The master sees bulk save/mod
bytes; lobby control messages remain sealed. Deployment tooling now offers
TCP port 29700; it was syntax-checked only, never executed.

## Ported (what and how)

Linux ships the same Python lobby sources (see [NETPUNCH.md](NETPUNCH.md)).
Move upstream's pipe-discovery thread into the existing rendezvous setup inside
Linux's `try/finally`; keep startup interruption, publisher/rendezvous cleanup,
UPnP removal, native paths and bound TCP dialing intact. Preserve the previous
host relay-address regression alongside all incoming pipe tests.

Extend the Linux shutdown regression to require a started, stalled `/pipe`
request before stopping the host. Verify cleanup still completes. Adapt the
incoming 3 MiB pipe test to drain the receiver queue concurrently and use
bounded waits. The original test hung here: small TCP reads filled the
receiver's 17-entry queue before its synchronous reader returned. The test now
models the real lobby consumer; production backpressure is unchanged.

Advance Lua verifier and release BUILDINFO provenance, package this record,
and link it from README and INSTALL. Lua/native source needs no change.
The diff contains no Windows hooks, MSVC code, executable addresses, offsets,
byte patterns or ABI/lifetime changes. No new static/live reverse engineering
is required; existing `docs/re/linux/` evidence and limitations remain applicable.

## Not ported

None for this commit. Inherited native feature gaps and gameplay-validation
limits remain unchanged.

## Live testing

No game launched, gdb attached or desktop input sent. No lab actor files,
saves, Steam files or user-installed mod were touched; no backup/restore was
needed. No public-master deployment, internet/NAT throughput, rendering or
Windows/native gameplay result is claimed.

Real Linux loopback sockets paired host/joiner by ID and role, rejected an
unmatched pair, carried bytes in both directions and delivered the exact
3 MiB + 17 byte payload through `_pipe_serve`, `_pipe_pull` and `_tcp_read`.
Slow-transfer selection was tested with mocked time/progress and thread launch.
Local master/rendezvous and clean/lossy UDP, TCP/fallback, relay, dual and mesh
self-tests passed. Shutdown tests stalled local HTTP discovery, sent real
SIGTERM and confirmed `/leave` and mocked UPnP cleanup; no router was changed.

## Tests

- `tools/linux/build_native.sh`: PASS, soldier build, 68/68 CTests and glibc
  compatibility checks.
- `python3 tools/linux/verify_lua_release.py`: PASS, 29 Lua files with one
  pinned native exception, 32 exact HUD glyphs. Manifest SHA-256:
  `d23c25e33ce4089c724211236a1344e7f710d4a6fb7c431f2d60f4a6838892e2`.
- `.git/port-venv/bin/python tools/test_tcp_connectivity.py`: PASS, 14 tests.
- Same interpreter, `tools/linux/test_lobby_stop.py`: PASS, 16/16 checks;
  `tools/test_transfer_status.py`: PASS, 3 tests; `tools/test_master_tls.py`
  and `tools/rendezvous_test.py`: ALL OK.
- Same interpreter, `netpunch/lobby.py --selftest`, `--selftest-relay`,
  `--selftest-transfer`, `--selftest-dual`, `--selftest-mesh`: PASS.
- `bash -n tools/linux/build_release.sh tools/masterserver_deploy.sh`,
  whitespace and merge-state checks: PASS; no unresolved index entries,
  MERGE_HEAD retained.

System Python lacked `stun`; installed pystun3 and zstandard in a clone-local
venv with pip caching disabled. Test temporary files used `.git/port-tmp`.
No system package, release package, frozen lobby, Windows executable or MSI
was installed/built. The first pipe-test attempt was terminated after its
queue deadlock; the corrected complete suite passed.

Logs: `.git/port-f6e47ef9-{build,lua,tcp,stop,transfer-status,tls,rendezvous,selftest,selftest-relay,selftest-transfer,selftest-dual,selftest-mesh,deps}.log`.
