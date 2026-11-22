# Windows dev 4e857780 integration: release 0.7.0.6

## Merged

Windows target: `4e85778017880eb29463767e884d7e5ef464314c`.
Linux parent: `d8bf0aa6a1fa63fa2130bf2aa8f12ebaca5fc099`.
One incoming commit, no conflicts. Merge staged and uncommitted.

Retain the two upstream release notes unchanged (`docs/releases/0.7.0.6.md`
and `installer/RELEASE-0.7.0.6.md`), the `installer/VERSION` bump and the
shared lobby's `LOBBY_VERSION` bump to 0.7.0.6. All participants, including
dedicated servers, must update.

## Ported (what and how)

The native package already reads `installer/VERSION`, and native and Windows
lobbies share the version gate. Advance the Lua verifier and package BUILDINFO
provenance to this release, preserving the pinned native origin-replay change
in `inject.lua`. Package this record and link it from README and Linux INSTALL.
Extend existing host/client version tests to reject the previous 0.7.0.5
release, including dedicated relay admission and welcome/roster rejection
before processing game/save state.

This commit contains no new native or Windows hooks, offsets, addresses,
byte patterns, MSVC code or lifetime contracts. No new RE is required.
Its release notes summarize earlier integrations:

- Cargo filters: [0047c19f](UPSTREAM_dev_0047c19f.md),
  [f9d34252](UPSTREAM_dev_f9d34252.md) and
  [ad36a976](UPSTREAM_dev_ad36a976.md). Native capture/replay remains absent.
- Repeated big-map loading: [363c38cc](UPSTREAM_dev_363c38cc.md).
  Native sidecar capture/serving remains absent; the native pager is separate.
- Catch-up command stamps: shared Lua [06188ea5](UPSTREAM_dev_06188ea5.md).
- Compressed terrain edits: [2f65bae3](UPSTREAM_dev_2f65bae3.md).
- Chat wrapping: [ceee11b1](UPSTREAM_dev_ceee11b1.md).
- Previous-run logs/dumps: [616191b1](UPSTREAM_dev_616191b1.md) and
  [bef70213](UPSTREAM_dev_bef70213.md).
- Bridge admission check: [ba1fa26e](UPSTREAM_dev_ba1fa26e.md).
- Release layout: [8e0a0c00](UPSTREAM_dev_8e0a0c00.md).

## Not ported

None introduced by this commit. This release stamp does not close inherited
native cargo-filter capture/replay or terrain-sidecar gaps. Their static RE,
failed lab launches and missing ownership/lifetime evidence remain documented
in [DEV_AD36A976](../re/linux/DEV_AD36A976.md),
[DEV_F9D34252](../re/linux/DEV_F9D34252.md) and
[DEV_363C38CC](../re/linux/DEV_363C38CC.md).
No new attempt or gameplay fix for those earlier gaps is claimed here.
In particular, upstream's successful two-instance cargo-filter test and
repeated-load fix must not be read as verified native Linux behavior.

## Live testing

No game launched, debugger attached or desktop input sent. No lab actor,
save, Steam installation or user mod changed; no backup/restoration required.
No GPU, rendering, loaded-world or cross-platform gameplay observation is
claimed. Upstream release-note measurements were not repeated.

The version-gate test exercised real Linux loopback host admission in normal
and dedicated relay modes: previous-release clients were rejected and the
current version admitted. Client welcome/roster rejection used fake peers.

## Tests

- `tools/linux/build_native.sh`: PASS, pinned soldier SDK, 70/70 CTests
  and glibc <=2.31 checks.
- `python3 tools/linux/verify_lua_release.py`: PASS, 29 Lua files with one
  pinned native exception and 32 exact HUD glyphs. Manifest SHA-256:
  `395f7dc46343862d04ee8b0b33151b4a1799c445c16ef5c5081f7eebd76faca1`.
- `.git/port-venv/bin/python tools/version_gate_test.py`: PASS, 2 tests.
- `bash -n tools/linux/build_release.sh`, upstream release-file byte equality,
  package/lobby version consistency and staged whitespace checks: PASS.
  No unresolved index entries; MERGE_HEAD retained.

Logs: `.git/port-4e857780-{build,lua,version,deps}.log`. System Python lacked
`stun`; installed pystun3 and zstandard in clone-local `.git/port-venv`, with
pip caching disabled and temporary files in `.git/port-tmp`. No system install
or release publication performed. No frozen lobby, installer package, Windows
executable or MSI build was attempted. DONE for this release-only commit;
existing native feature and live-validation limits remain.
