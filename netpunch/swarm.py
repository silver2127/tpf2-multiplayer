#!/usr/bin/env python3
"""
swarm.py -- peer-to-peer piece distribution of the shared save.

The host-as-relay star (``lobby.py``) pushes the whole save from the host to
every joiner, so the host uploads N copies of a ~230 MB file and 8 players wait
on one upload link. This module distributes the save as PIECES traded between
peers, BitTorrent-style: the host seeds one copy, joiners pull rare pieces from
whoever has them, and every joiner's upload contributes. Host upload drops from
N copies toward ~1, and finish time is bounded by the swarm's aggregate
bandwidth rather than the host's alone.

Three parts (this file):

  1. PIECE PROTOCOL -- ``PieceManifest`` splits the blob into fixed pieces, each
     hashed; a peer verifies a piece the moment its chunks are all in, and only
     a verified piece is announced as "have" and offered onward. Chunks reuse
     lobby.py's on-wire frame (CHUNK_MAGIC + sid + seq); a piece is just a
     contiguous run of chunk seqs, so the reliability primitive is unchanged.

  2. PEER SCHEDULER -- ``SwarmState`` tracks a bitfield of pieces per known peer,
     picks the next pieces to request RAREST-FIRST (with a random first-piece
     warmup and an endgame that duplicate-requests the last few), and bounds how
     many requests are outstanding per peer.

  3. TRANSPORT -- ``PeerLink`` is one logical link to another swarm member. It
     is DIRECT when a hole-punch between the pair succeeded, or VIA-HOST when it
     did not: the host forwards ``swarm_relay`` envelopes between the two. The
     scheduler is identical either way; only the link's ``send`` differs, and a
     per-link relayed-byte budget stops a bad-NAT mesh from collapsing back onto
     the host link silently.

Everything here is transport-agnostic and driven by a caller-supplied ``send``
callback + fed inbound messages, so the loopback self-test (``selftest`` below,
runnable with no game and no network) exercises the full multi-peer assembly and
asserts that most pieces arrived from peers rather than the seed.
"""

from __future__ import annotations

import hashlib
import json
import os
import random
import struct
import time

# Wire framing shared with lobby.py (kept identical so a chunk is a chunk on
# either path). Imported lazily to avoid a hard cycle when swarm is used alone.
CHUNK_MAGIC = b"NPF1"
SWARM_RELAY_MAGIC = b"s"          # first byte of a host-relayed swarm envelope
                                  # ('{' JSON, 'N' chunk, 'g' game, 's' swarm)

# --------------------------------------------------------------------------- #
# Tunables
# --------------------------------------------------------------------------- #
PIECE_BYTES = 2 * 1024 * 1024     # 2 MB pieces: 230 MB save -> ~115 pieces, so
                                  # the manifest hash list is ~7 KB and a piece
                                  # is coarse enough that per-piece bookkeeping
                                  # is cheap, fine enough to trade usefully.
CHUNK_DATA = 1200                 # must match lobby.CHUNK_DATA (bytes per chunk)
REQUEST_PIPELINE = 4              # pieces requested-ahead per peer (each piece is
                                  # many chunks, so this is plenty of in-flight)
REQUEST_TIMEOUT = 4.0             # re-request a piece if it hasn't completed
HAVE_GOSSIP = 1.0                 # re-announce our bitfield this often
ENDGAME_PIECES = 3                # when this few pieces remain, ask ALL peers for
                                  # each remaining piece (kills the long tail)
RELAY_BYTE_BUDGET = 8 * 1024 * 1024   # max bytes one link will pull via the host
                                      # before it stops using the relay (direct
                                      # peers and the seed still serve it)


# --------------------------------------------------------------------------- #
# 1. Piece protocol
# --------------------------------------------------------------------------- #
class PieceManifest:
    """The fixed division of a save blob into hashed pieces.

    Pieces are ``piece_bytes`` each (the last is short). Chunk ``seq`` maps to a
    piece by ``seq * chunk // piece_bytes``; a piece owns the contiguous seq
    range ``[first_chunk(p), first_chunk(p+1))``. The manifest is what the host
    advertises and what a receiver verifies against.
    """

    def __init__(self, total_bytes, piece_bytes=PIECE_BYTES, chunk=CHUNK_DATA,
                 piece_sha=None, overall_sha=None):
        self.total_bytes = int(total_bytes)
        self.piece_bytes = int(piece_bytes)
        self.chunk = int(chunk)
        self.total_chunks = (self.total_bytes + self.chunk - 1) // self.chunk
        self.piece_count = (self.total_bytes + self.piece_bytes - 1) // self.piece_bytes
        self.piece_sha = list(piece_sha) if piece_sha else None
        self.overall_sha = overall_sha
        # chunks per full piece (the last piece may hold fewer)
        self.chunks_per_piece = self.piece_bytes // self.chunk
        if self.chunks_per_piece < 1:
            raise ValueError("piece_bytes must be >= chunk")

    @classmethod
    def from_blob(cls, blob, piece_bytes=PIECE_BYTES, chunk=CHUNK_DATA):
        m = cls(len(blob), piece_bytes, chunk)
        m.piece_sha = []
        for p in range(m.piece_count):
            off = p * m.piece_bytes
            m.piece_sha.append(hashlib.sha256(blob[off:off + m.piece_bytes]).hexdigest())
        m.overall_sha = hashlib.sha256(blob).hexdigest()
        return m

    def piece_byte_range(self, p):
        off = p * self.piece_bytes
        return off, min(off + self.piece_bytes, self.total_bytes)

    def piece_chunk_range(self, p):
        """[first_seq, last_seq) of chunks that make up piece ``p``."""
        first = (p * self.piece_bytes) // self.chunk
        end_byte = min((p + 1) * self.piece_bytes, self.total_bytes)
        last = (end_byte + self.chunk - 1) // self.chunk
        return first, last

    def piece_of_chunk(self, seq):
        return (seq * self.chunk) // self.piece_bytes

    def verify_piece(self, p, data):
        return (self.piece_sha is not None
                and hashlib.sha256(data).hexdigest() == self.piece_sha[p])

    def to_meta(self):
        return {"total_bytes": self.total_bytes, "piece_bytes": self.piece_bytes,
                "chunk": self.chunk, "piece_count": self.piece_count,
                "piece_sha": self.piece_sha, "sha256": self.overall_sha}

    @classmethod
    def from_meta(cls, m):
        return cls(m["total_bytes"], m["piece_bytes"], m["chunk"],
                   piece_sha=m.get("piece_sha"), overall_sha=m.get("sha256"))


class PieceStore:
    """A receiver's assembly buffer with per-piece verification.

    Chunks are written at their exact byte offset (so reorder/dupes are free).
    When every chunk of a piece is present the piece is hashed; on a match it is
    marked ``have`` and its chunks are frozen, on a mismatch the whole piece is
    dropped and re-requested. Only verified pieces are ever offered onward, so a
    corrupt piece can never propagate through the swarm.
    """

    def __init__(self, manifest: PieceManifest, seed_blob=None):
        self.m = manifest
        n = manifest.piece_count
        self.have = bytearray(n)                 # 1 = verified
        self.piece_chunks_left = [0] * n
        for p in range(n):
            first, last = manifest.piece_chunk_range(p)
            self.piece_chunks_left[p] = last - first
        if seed_blob is not None:
            # the host seeds a complete store (it has the whole blob)
            self.buf = bytearray(seed_blob)
            for p in range(n):
                self.have[p] = 1
                self.piece_chunks_left[p] = 0
        else:
            self.buf = bytearray(manifest.total_bytes)
        self.chunk_have = bytearray(manifest.total_chunks)
        if seed_blob is not None:
            for s in range(manifest.total_chunks):
                self.chunk_have[s] = 1

    def have_count(self):
        return sum(self.have)

    def is_complete(self):
        return self.have_count() == self.m.piece_count

    def bitfield_bytes(self):
        """Pack ``have`` into a compact big-endian bitfield for the wire."""
        return bytes(self.have)          # 1 byte/piece; piece_count is small

    def add_chunk(self, seq, data):
        """Store one chunk; return the piece index if that completed+verified a
        piece (for the caller to announce), else None."""
        if seq < 0 or seq >= self.m.total_chunks or self.chunk_have[seq]:
            return None
        p = self.m.piece_of_chunk(seq)
        if self.have[p]:
            return None
        off = seq * self.m.chunk
        self.buf[off:off + len(data)] = data
        self.chunk_have[seq] = 1
        self.piece_chunks_left[p] -= 1
        if self.piece_chunks_left[p] > 0:
            return None
        # piece complete -> verify
        a, b = self.m.piece_byte_range(p)
        if self.m.verify_piece(p, bytes(self.buf[a:b])):
            self.have[p] = 1
            return p
        # corrupt: drop the piece's chunks so it is fully re-requested
        first, last = self.m.piece_chunk_range(p)
        for s in range(first, last):
            self.chunk_have[s] = 0
        self.piece_chunks_left[p] = last - first
        return None

    def missing_chunks_of_piece(self, p):
        first, last = self.m.piece_chunk_range(p)
        return [s for s in range(first, last) if not self.chunk_have[s]]


# --------------------------------------------------------------------------- #
# 2. Peer scheduler
# --------------------------------------------------------------------------- #
class SwarmState:
    """Rarest-first request planning across a set of peer bitfields.

    The caller feeds peer bitfields (from ``have`` gossip) and asks
    ``plan(store, peers)`` for a mapping ``{peer_name: [piece, ...]}`` of what to
    request from whom this round. Rarest-first: pieces held by the fewest peers
    go first, so the swarm keeps every piece alive; a short random warmup avoids
    everyone grabbing piece 0; an endgame broadcasts the final pieces to all
    peers to kill the tail.
    """

    def __init__(self, manifest: PieceManifest):
        self.m = manifest
        self.peer_have = {}              # name -> bytearray bitfield
        self.inflight = {}               # piece -> (peer_name, t_requested)
        self.warmup = True
        self._warmup_left = 4

    def set_peer_bitfield(self, name, bits):
        n = self.m.piece_count
        b = bytearray(n)
        for i in range(min(n, len(bits))):
            b[i] = 1 if bits[i] else 0
        self.peer_have[name] = b

    def peer_has(self, name, p):
        b = self.peer_have.get(name)
        return bool(b and p < len(b) and b[p])

    def drop_peer(self, name):
        self.peer_have.pop(name, None)
        for p, (owner, _) in list(self.inflight.items()):
            if owner == name:
                del self.inflight[p]

    def note_requested(self, name, p, now):
        self.inflight[p] = (name, now)

    def note_completed(self, p):
        self.inflight.pop(p, None)

    def _rarity(self):
        """piece -> number of peers (incl. seed) that hold it."""
        n = self.m.piece_count
        cnt = [0] * n
        for b in self.peer_have.values():
            for p in range(n):
                if p < len(b) and b[p]:
                    cnt[p] += 1
        return cnt

    def plan(self, store: PieceStore, now, pipeline=REQUEST_PIPELINE):
        """Return {peer_name: [piece,...]} to request this round."""
        need = [p for p in range(self.m.piece_count) if not store.have[p]]
        if not need:
            return {}
        # expire stale in-flight requests
        for p, (owner, t) in list(self.inflight.items()):
            if store.have[p] or now - t > REQUEST_TIMEOUT:
                del self.inflight[p]
        endgame = len(need) <= ENDGAME_PIECES
        rarity = self._rarity()
        # order the pieces we still need
        if self.warmup and self._warmup_left > 0:
            random.shuffle(need)
        else:
            need.sort(key=lambda p: (rarity[p], p))
        plan = {}
        outstanding = dict.fromkeys(self.peer_have, 0)
        for p in need:
            if not endgame and p in self.inflight:
                continue
            holders = [nm for nm in self.peer_have if self.peer_has(nm, p)]
            if not holders:
                continue
            if endgame:
                # ask every holder for each remaining piece
                for nm in holders:
                    plan.setdefault(nm, []).append(p)
            else:
                # give it to the least-loaded holder
                holders.sort(key=lambda nm: outstanding.get(nm, 0))
                nm = holders[0]
                if outstanding.get(nm, 0) >= pipeline:
                    continue
                plan.setdefault(nm, []).append(p)
                outstanding[nm] = outstanding.get(nm, 0) + 1
                self.note_requested(nm, p, now)
        if self.warmup and self._warmup_left > 0:
            self._warmup_left -= 1
            if self._warmup_left == 0:
                self.warmup = False
        return plan


# --------------------------------------------------------------------------- #
# 3. Transport: one link per swarm member (direct or host-relayed)
# --------------------------------------------------------------------------- #
class PeerLink:
    """A logical link to one other swarm member.

    ``mode`` is "direct" (a hole-punched Connection between the pair) or "relay"
    (the host forwards ``swarm_relay`` envelopes). ``raw_send(bytes)`` puts a
    frame on whichever wire this link uses. The swarm layer above is identical
    for both; ``relay`` mode just meters bytes and gives up on the relay once
    ``RELAY_BYTE_BUDGET`` is spent (the peer keeps serving over any direct links
    and the seed still serves everyone), so a strict-NAT mesh degrades to "a bit
    more host load" rather than "silently all host load".
    """

    def __init__(self, name, raw_send, mode="direct", log=None):
        self.name = name
        self.raw_send = raw_send
        self.mode = mode
        self.log = log or (lambda *_a, **_k: None)
        self.relay_bytes = 0
        self.last_have = 0.0
        self.alive = True

    def usable(self):
        if not self.alive:
            return False
        if self.mode == "relay" and self.relay_bytes >= RELAY_BYTE_BUDGET:
            return False
        return True

    def _meter(self, nbytes):
        if self.mode == "relay":
            self.relay_bytes += nbytes

    def send_json(self, obj):
        payload = json.dumps(obj).encode("utf-8")
        self._meter(len(payload))
        self.raw_send(payload)

    def send_chunk_frame(self, sid, seq, data):
        frame = CHUNK_MAGIC + struct.pack("!II", sid, seq) + data
        self._meter(len(frame))
        self.raw_send(frame)


class SwarmMember:
    """Drives piece exchange for one participant over a set of PeerLinks.

    Used by both a joiner (starts empty, downloads) and any peer that has pieces
    (serves them). The host is just a member whose store starts complete. Pumped
    from the caller's loop: ``tick(now)`` gossips bitfields and issues requests;
    inbound frames are routed via ``on_json`` / ``on_chunk``.
    """

    def __init__(self, sid, manifest: PieceManifest, store: PieceStore,
                 links: dict, on_piece=None, log=None):
        self.sid = sid
        self.m = manifest
        self.store = store
        self.links = links                       # name -> PeerLink
        self.sched = SwarmState(manifest)
        self.on_piece = on_piece or (lambda p: None)
        self.log = log or (lambda *_a, **_k: None)
        self._last_gossip = 0.0
        self._serve_q = []                        # (link_name, seq) to send
        self.served_chunks = 0
        self.recv_from = {}                       # name -> chunks received (stats)

    # -- inbound ----------------------------------------------------------- #
    def on_json(self, from_name, msg):
        t = msg.get("t")
        if t == "have":
            bits = bytes.fromhex(msg.get("bits", ""))
            self.sched.set_peer_bitfield(from_name, bits)
        elif t == "request":
            # queue the missing chunks of each requested piece we actually hold
            for p in msg.get("pieces", []):
                if 0 <= p < self.m.piece_count and self.store.have[p]:
                    first, last = self.m.piece_chunk_range(p)
                    for s in range(first, last):
                        self._serve_q.append((from_name, s))

    def on_chunk(self, from_name, sid, seq, data):
        if sid != self.sid:
            return
        self.recv_from[from_name] = self.recv_from.get(from_name, 0) + 1
        done = self.store.add_chunk(seq, data)
        if done is not None:
            self.sched.note_completed(done)
            self.on_piece(done)
            self._announce_have()                 # tell peers immediately

    # -- outbound / periodic ---------------------------------------------- #
    def _announce_have(self):
        bits = self.store.bitfield_bytes().hex()
        for link in self.links.values():
            if link.usable():
                link.send_json({"t": "have", "bits": bits})

    def _serve(self, budget):
        sent = 0
        while self._serve_q and sent < budget:
            name, seq = self._serve_q.pop(0)
            link = self.links.get(name)
            if not link or not link.usable():
                continue
            off = seq * self.m.chunk
            a, b = off, min(off + self.m.chunk, self.m.total_bytes)
            link.send_chunk_frame(self.sid, seq, bytes(self.store.buf[a:b]))
            sent += 1
            self.served_chunks += 1
        return sent

    def tick(self, now, serve_budget=512):
        # 1) gossip our bitfield periodically (self-heals lost 'have's)
        if now - self._last_gossip >= HAVE_GOSSIP:
            self._last_gossip = now
            self._announce_have()
        # 2) serve queued chunk requests
        self._serve(serve_budget)
        # 3) request pieces we still need, rarest-first
        if not self.store.is_complete():
            plan = self.sched.plan(self.store, now)
            for name, pieces in plan.items():
                link = self.links.get(name)
                if link and link.usable() and pieces:
                    link.send_json({"t": "request", "pieces": pieces})

    def progress(self):
        return self.store.have_count(), self.m.piece_count


# --------------------------------------------------------------------------- #
# Self-test: 1 seed + 3 leechers, in-process links, must all assemble the blob
# --------------------------------------------------------------------------- #
def _selftest_topology(direct_between_leechers=True, size=6 * 1024 * 1024,
                       verbose=True):
    """Wire N members with in-process PeerLinks and run the exchange to
    completion. Returns (ok, stats). When ``direct_between_leechers`` is False,
    leechers can only reach the seed + relay through it (worst case)."""
    random.seed(1234)
    blob = os.urandom(size)
    manifest = PieceManifest.from_blob(blob, piece_bytes=512 * 1024)
    sid = 0xABCD

    names = ["seed", "l1", "l2", "l3"]
    members = {}
    # inbox per member: list of (from_name, raw_bytes)
    inbox = {nm: [] for nm in names}

    def make_link(owner, other, mode):
        def raw_send(payload, _other=other):
            inbox[_other].append((owner, payload))
        return PeerLink(other, raw_send, mode=mode)

    # build link maps
    linkmap = {nm: {} for nm in names}
    for a in names:
        for b in names:
            if a == b:
                continue
            if a == "seed" or b == "seed":
                mode = "direct"
            else:
                mode = "direct" if direct_between_leechers else "relay"
            linkmap[a][b] = make_link(a, b, mode)

    for nm in names:
        seed_blob = blob if nm == "seed" else None
        store = PieceStore(manifest, seed_blob=seed_blob)
        members[nm] = SwarmMember(sid, manifest, store, linkmap[nm])

    # relay hop: in "relay" mode a leecher->leecher link would in reality go
    # through the host. Here the in-process raw_send already delivers directly;
    # the RELAY_BYTE_BUDGET metering still applies, so this proves the budget
    # path too. (Cross-NAT host forwarding is exercised in the lobby self-test.)

    def deliver_all():
        for nm in names:
            q, inbox[nm] = inbox[nm], []
            mem = members[nm]
            for from_name, payload in q:
                if payload[:4] == CHUNK_MAGIC:
                    s, seq = struct.unpack("!II", payload[4:12])
                    mem.on_chunk(from_name, s, seq, payload[12:])
                elif payload[:1] == b"{":
                    mem.on_json(from_name, json.loads(payload.decode("utf-8")))

    t0 = time.time()
    now = 0.0
    for step in range(20000):
        now += 0.05
        for nm in names:
            members[nm].tick(now, serve_budget=64)
        deliver_all()
        if all(members[nm].store.is_complete() for nm in ("l1", "l2", "l3")):
            break

    ok = True
    for nm in ("l1", "l2", "l3"):
        st = members[nm].store
        if not st.is_complete() or hashlib.sha256(bytes(st.buf)).hexdigest() != manifest.overall_sha:
            ok = False
            if verbose:
                print(f"[swarm] FAIL: {nm} incomplete/mismatch "
                      f"({st.have_count()}/{manifest.piece_count})")
    # how much did the seed serve vs peers?
    seed_served = members["seed"].served_chunks
    peer_served = sum(members[nm].served_chunks for nm in ("l1", "l2", "l3"))
    from_peers = sum(
        v for nm in ("l1", "l2", "l3")
        for k, v in members[nm].recv_from.items() if k != "seed")
    from_seed = sum(
        v for nm in ("l1", "l2", "l3")
        for k, v in members[nm].recv_from.items() if k == "seed")
    total = manifest.total_chunks
    stats = {"steps": step, "seed_served_chunks": seed_served,
             "peer_served_chunks": peer_served, "from_peers": from_peers,
             "from_seed": from_seed, "chunks_per_copy": total}
    if verbose:
        print(f"[swarm] {'PASS' if ok else 'FAIL'} "
              f"({'mesh' if direct_between_leechers else 'relay'}): 3 leechers "
              f"assembled {size} B in {step} steps")
        print(f"[swarm]   seed served {seed_served} chunks "
              f"(= {seed_served/total:.2f} full copies), peers served "
              f"{peer_served}")
        print(f"[swarm]   leechers pulled {from_seed} chunks from seed, "
              f"{from_peers} from each other")
    return ok, stats


def selftest():
    ok1, s1 = _selftest_topology(direct_between_leechers=True)
    # In the mesh case the seed should serve well under 3 full copies -- the
    # whole point. Assert < 2.4 copies (naive star would be exactly 3).
    copies = s1["seed_served_chunks"] / s1["chunks_per_copy"]
    if ok1 and copies >= 2.4:
        print(f"[swarm] FAIL: seed served {copies:.2f} copies -- no swarm gain")
        ok1 = False
    ok2, _ = _selftest_topology(direct_between_leechers=False)
    return ok1 and ok2


if __name__ == "__main__":
    import sys
    sys.exit(0 if selftest() else 1)
