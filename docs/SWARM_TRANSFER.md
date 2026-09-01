# Swarm save transfer (peer-to-peer piece distribution)

Goal: stop the host uploading N copies of a ~230 MB save. Distribute it as
pieces traded between peers, so host upload approaches one copy and finish time
is bounded by the swarm's aggregate bandwidth. Requested as three parts: (1)
piece protocol, (2) pairwise peer connections, (3) relay fallback.

## Status

- **Core algorithm: DONE and tested** in `netpunch/swarm.py` (`python swarm.py`
  = PASS). Standalone, no game, no network needed.
  - **(1) Piece protocol** — `PieceManifest` / `PieceStore`: the blob is split
    into chunk-aligned hashed pieces (a piece is a whole number of 1200-byte
    chunks, so no chunk ever straddles two pieces). A piece is verified the
    moment its chunks are all in; only a verified piece is announced and served,
    so corruption never propagates. Chunk frame is lobby.py's exact
    `CHUNK_MAGIC + sid + seq` — the reliability primitive is unchanged.
  - **(2) Peer scheduler** — `SwarmState`: rarest-first with a random warmup, a
    per-member "spread" so leechers diverge on which pieces they pull first
    (then trade the difference), an endgame that asks every holder for the last
    few pieces, and a per-peer request pipeline. `PeerLink` is one logical link,
    direct or relayed.
  - **(3) Relay fallback** — a `PeerLink` in `relay` mode meters bytes and stops
    using the host relay past `RELAY_BYTE_BUDGET` (8 MB); direct peers and the
    seed keep serving it. So a strict-NAT mesh degrades to "a little more host
    load", never a silent collapse back to the full star.
  - Loopback test (1 seed + 3 leechers, mesh and relay topologies): all three
    assemble a byte-identical blob; seed upload drops from 3.0 copies to **1.78**
    with the majority of data traded peer-to-peer.

## Lobby integration of the swarm (designed; mesh transport now available)

Wire additions, all inside NP1 DATA and all gated behind a `--swarm` flag (off
= today's star transfer, byte-identical):

- `join` gains `"profile": <base32>` — the joiner encodes its OWN observed
  profile (candidates + flags, via `connect.encode_profile`) so other joiners
  can punch it. Requires the joiner to `observe()` its own socket first; today
  `cmd_join` only dials, it does not self-observe.
- Host, on a save-swarm start: build a `PieceManifest` from the blob, seed a
  complete `PieceStore`, and broadcast `sbegin {sid, manifest_meta,
  peers:[{name, profile}]}`. The host is a `SwarmMember` whose links to each
  joiner are the existing star socket (`sock.sendto` to that addr). It watches
  each joiner's `have` gossip; when every joiner's bitfield is complete it does
  the normal `broadcast_start(save=True)`.
- Joiner, on `sbegin`: build the manifest, an empty store, and a `SwarmMember`
  with a link to the host (seed, over its `conn`) plus a link to every OTHER
  joiner. On store-complete it writes `incoming_save.*` and emits `save_ready`
  exactly like `_ClientSaveReceiver`, so the START/Continue flow downstream is
  unchanged.
- `srelay` — `SWARM_RELAY_MAGIC ('s')` + a JSON `{to, frm, ...}` envelope (or a
  wrapped chunk). The host forwards it between two joiners that could not punch.

## The direct joiner<->joiner socket problem -- SOLVED by netpunch/mesh.py (2026-09-01)

`MeshNode` keeps the joiner's ONE observed socket and demultiplexes by source
address into per-peer links with the stock NP1 handshake, i.e. option (A)
below. Joiners self-observe (STUN, no UPnP) in `cmd_join`, send their profile in
`join`, the host puts every profile + each joiner's direct-link list in the
roster, and joiners punch each other pairwise. Bridge frames now fan out
DIRECTLY, with an `'r'` envelope via the host (or any peer that has a direct
link to the destination) for pairs that fail to punch. Legacy `--no-mesh`
joiners keep the star. `python lobby.py --selftest-mesh` = PASS. The swarm's
`PeerLink.raw_send` can now be `mesh.send(name, payload)` -- that wiring is the
remaining swarm step.

### (historical) the blocker as first identified

`punch.Connection` owns its socket exclusively (one reader thread). A joiner's
main socket is already owned by its link to the host. A NAT mapping is
**per-socket**, so a direct joiner<->joiner punch must either:

- **(A) multiplex** peer punch + peer piece traffic on the joiner's ONE existing
  socket (the same one it observed on, so the advertised profile matches). This
  needs a small demux in front of `Connection` that routes datagrams by source
  address to per-peer state — the host already does exactly this by-source
  demux, so the pattern exists. Preferred: one socket, mapping matches the
  advertised profile, no re-observe.
- **(B) one observed socket per peer**: bind a fresh socket per peer link,
  `observe()` it (fresh STUN + a fresh public mapping), exchange THOSE codes.
  Simpler transport, but O(peers) STUN round-trips at start and more sockets.

Until (A) or (B) lands, only the **relay** links work between joiners, and
relayed piece traffic still crosses the host — so the bandwidth win is not yet
realized on real NAT even though the algorithm and the relay fallback are done.
On loopback (no NAT) direct links work trivially, which is what the swarm.py
self-test covers.

**Recommendation:** implement (A) — a by-source demux over the joiner's existing
socket — so direct links reuse the already-observed mapping. Measure real
joiner<->joiner punch success on the actual play machines first (a cheap probe);
if most pairs punch, the swarm delivers its full gain, and any pair that fails
uses the relay fallback that already exists.

## Also worth doing (independent of the swarm)

Compression is NOT available: the .sav starts with the zstd magic (`28 b5 2f fd`)
and is already compressed (zlib gets ~9%). For rejoins, a binary delta against
the joiner's previous copy is a big win and stacks with the swarm.

## Open item after the mesh: is the bridge N-way?

The transport now delivers every participant's frames to every other
participant exactly once. The lockstep bridge DLL still speaks to ONE local
port and the Lua layer keys commands by `origin`, so the Lua side is N-aware;
whether the DLL's UDP framing/ARQ copes with frames from several origins on
one port is unverified live. Two-instance play is unchanged (host<->joiner is
the same path as before).
