# Multiplayer version checking

Starting with 0.4.23, lobby admission requires an exact `LOBBY_VERSION` match.
The join request, welcome, and periodic roster carry the release version.
The host rejects mismatches before adding a player, sharing a save, or relaying
game traffic. The client checks the host before accepting saves, starts, or
game traffic. A roster can confirm the version if the UDP welcome was lost.
Missing versions from pre-0.4.23 builds are incompatible. The menu reports the
mismatch and tells players to install the same multiplayer version.

Dedicated relay servers enforce the same rule and must be updated alongside
clients. This checks the packaged lobby release, not file hashes or the versions
of third-party mods. Bump `netpunch/lobby.py`'s `LOBBY_VERSION` and
`installer/VERSION` together for each release. Two old clients without this
check cannot be prevented from connecting to each other.

Validation: `python tools/version_gate_test.py` covers real host/relay admission
and client rejection of older, newer, missing, and malformed version fields.
`python netpunch/lobby.py --selftest` and `--selftest-mesh` cover matching-version
lobbies, late joins, and direct/relayed gameplay traffic.
