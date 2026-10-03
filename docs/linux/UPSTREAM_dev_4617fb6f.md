# Windows dev 4617fb6f integration: FPT6 and release 0.7.0.7

## Merged

Target `4617fb6f0f26ecf88e2633cebb1d8aaf3141833a`, Linux parent
`60ef385678c5d9f82143a46d9b65330564e05ee3`. The three dev first-parent
commits are `8d88dd5` (direct installers on the version page), `1750649`
(merge main / PR #12), and `4617fb6` (retain everything owed to members).
The main merge also imports earlier native fixes. No conflicts; merge stays
staged and uncommitted. No later dev commits are included.

## Ported (what and how)

- Retain the incoming POSIX FPT6 transport: 73-byte packed header, cumulative
  ACK, 64-bit SACK, 900-byte text chunks (978-byte maximum datagram), per-peer
  Jacobson/Karels RTO and Karn sampling. Quiet members retain their packets
  until ACK or eviction; queue overflow warns without dropping chunks;
  retransmissions back off once per peer per pass. Timestamp echoes travel
  without being used as delayed RTT samples. World/process epoch checks remain.
- Shared Lua retains governor lag smoothing, PID anti-windup, and persistent
  history requests after six retries, warning once rather than abandoning
  catch-up with missing commands. Python lobby/Steam routing changes are shared;
  the native bridge compiles the shared Steam tunnel and its new 42100 range
  with 62100 fallback. Native atomic shutdown and temporary-file/rename identity
  publication already implement the Windows changes; keep them intact.
- Retain sticky lobby origin letters in both normal and relay native lobbies,
  CPU-memory Vulkan panel composition, the separate `boot/` preload copy,
  installer Proton diagnosis and bytewise release export sorting brought from main.
  Linux Steam-library discovery already covers the Windows deploy-script change.
- Add the native title and footer version labels, sourced at configure time
  from `installer/VERSION`, with CMake tracking version-file changes. Package
  and shared lobby version are 0.7.0.7; all peers must update for FPT6.
- Advance the Lua verifier and package provenance to this exact target while
  retaining the pinned native origin-replay exception. Package this record.
  Update native FPT6 fixtures (including explicit NO_ACK cumulative fields),
  add native resilience and shared Lua regression coverage, and update offline
  publisher tests for the three direct installers alongside the launchers.

No new game address, byte pattern, engine layout or calling convention is
introduced. Reviewed the existing Linux organization and RE records; this
batch changes transport, presentation and packaging, requiring no new ELF RE.
Windows code paths and history remain intact.

## Not ported

None introduced by this batch. Inherited cargo-filter capture/replay and
terrain-sidecar capture/serving gaps remain as documented in
[ad36a976](UPSTREAM_dev_ad36a976.md) and [363c38cc](UPSTREAM_dev_363c38cc.md).
This integration does not claim to close them or repeat their RE attempts.

## Live testing

No game launched, debugger attached, desktop input sent, or lab actor modified.
No backup/restoration was needed. No loaded-world, GPU, visual menu or real
Windows/native gameplay result is claimed. Transport tests use real POSIX UDP
and the actual native resend loop; Python tests use loopback/fake Steam peers.
These are process-level tests, not game sessions.

## Tests

- `tools/linux/build_native.sh`: PASS, 71/71 CTests in the pinned soldier
  SDK, including native FPT6 packet sizes, epoch/restart handling, 2/3/10-peer
  transport, RTO resilience, sticky ordinary-lobby origin/config output, panel
  rendering, Steam Legacy/Messages fixtures, and glibc <=2.31 checks.
- `python3 tools/linux/verify_lua_release.py`: PASS, 29 Lua files (one pinned
  native origin-replay exception) and 32 exact HUD glyphs. Manifest SHA-256:
  `069da99cba0cef16e9273300adb7efe47e7c52748e27e1de9db326df5c098d14`.
- `tools/linux/test_fpt6_pacing.py` and `tools/live_catchup_gap_test.py`: PASS
  with Lua 5.2 through clone-local lupa.
- `tools/version_gate_test.py`: PASS, 2 tests, including rejecting 0.7.0.6.
  `tools/test_lobby_limits.py`: PASS, 32 tests.
- `netpunch/lobby.py --selftest`: PASS, including ordinary-lobby letters
  remaining stable when `aaron` joins after the existing three players.
- `tools/test_steam_tunnel.py` and `tools/test_steam_tcp.py`: PASS with fake
  Steam endpoints and loopback sockets; TCP and fallback transfers were identical.
- `tools/linux/test_launcher_release.py`: PASS, 35 offline tests. All publisher
  requests are mocked; no credentials, publication or GitHub calls.
- `tools/linux/build_release.sh --build-dir native/linux/out-soldier
  --no-netpunch`: PASS. Test-only .run/tarball and native aliases built in
  `dist/linux`; this package deliberately excludes the frozen Python lobby.
  `test_installer.py dist/linux/tpf2mp-linux-0.7.0.7-native.run` and
  `test_release_assets.py dist/linux 0.7.0.7`: PASS. Installation, upgrade,
  uninstall, extraction, checksums and damaged/missing asset rejection use
  temporary fixtures inside this clone, not Steam or the lab.
- Shell syntax and `git -c core.whitespace=cr-at-eol diff --check`: PASS.
  Windows CRLF bytes are retained. No unresolved index entries; MERGE_HEAD
  still names the requested target. The compiled native menu contains v0.7.0.7.

The initial build exposed the old FPT5 size assertion; the initial publisher
suite exposed its obsolete 18-upload expectation. Both fixtures were updated
for the incoming behavior and rerun successfully. Initial installer/selftest
invocations lacked their required argument/flag; corrected invocations passed.
New Lua harness setup was corrected before its passing run.

Logs are clone-local `.git/port-*.log`. Python dependencies were installed only
in `.git/port-venv` with cache disabled and `.git/port-tmp` as temporary storage.
No system install, game run, Windows build or release publication occurred.
