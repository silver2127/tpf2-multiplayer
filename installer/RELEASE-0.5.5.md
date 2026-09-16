A hot join gets the host's fresh autosave

- Lobby: a player who hot-joined a running game was sent the save from START GAME instead of the autosave the host had just taken for them. The lobby's "serve waiting peers again" pass (meant for someone who joined during a transfer) fired within a second of the join, and the host's fresh save then arrived to "start ignored -- a save transfer is in progress"; the newcomer loaded a world minutes or hours old and had to catch up from there or desynced. The host's menu now tells its lobby the moment it takes a hot-join save, the lobby holds that pass until the save's start arrives (120 s fallback), and a host start that still meets a running transfer is queued, never dropped.
- Everything in 0.5.4 (a joiner whose game restarted can rejoin; the lobby follows a completed resync's world; a first-time player can load a shared save).

Every player must update (the lobby version gate is exact) and the dedicated relay must run this version.

Validation: on the two-instance rig the hot join now transfers the autosave; offline, the transport, lobby recovery and readiness tests as in 0.5.4.
