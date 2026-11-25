# Multiplayer version checking

Starting with 0.4.23, lobby admission requires an exact `LOBBY_VERSION` match.
The join request, welcome, and periodic roster carry `LOBBY_VERSION` as
`version` and, from the release after 0.7.1.6, the release (`RELEASE_VERSION`,
equal to `installer/VERSION`) as `release`. A release that changes nothing on
the wire keeps `LOBBY_VERSION` (0.7.1.6 speaks 0.7.1.5), so the gate compares
`version` while the mismatch message names releases: "you have 0.7.1.6; the
host has 0.7.1.4". A peer without `release` is named by its `version`, which
was its release up to 0.7.1.5. A reject from such an older host that names the
joiner's lobby version is reworded to name the joiner's release.
The host rejects mismatches before adding a player, sharing a save, or relaying
game traffic. The client checks the host before accepting saves, starts, or
game traffic. A roster can confirm the version if the UDP welcome was lost.
Missing versions from pre-0.4.23 builds are incompatible. The menu reports the
mismatch and tells players to install the same multiplayer version.

Dedicated relay servers enforce the same rule and must be updated alongside
clients. This checks the packaged lobby release, not file hashes or the versions
of third-party mods. Bump `installer/VERSION` and `netpunch/lobby.py`'s
`RELEASE_VERSION` together for each release, and `LOBBY_VERSION` with every
release that changes the protocol or the world. Two old clients without this
check cannot be prevented from connecting to each other.

Validation: `python tools/version_gate_test.py` covers real host/relay admission
and client rejection of older, newer, missing, and malformed version fields.
`python netpunch/lobby.py --selftest` and `--selftest-mesh` cover matching-version
lobbies, late joins, and direct/relayed gameplay traffic.
