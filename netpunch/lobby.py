#!/usr/bin/env python3
"""
lobby.py -- N-player LOBBY (usernames + roster + chat) over the netpunch engine.

This sits on top of the Phase 1-4 transport (``punch.py`` / ``observe.py`` /
``connect.py``) and turns a pile of hole-punched UDP links into a small,
host-authoritative lobby that the in-game Transport Fever 2 menu drives through
flat files.

TOPOLOGY -- host-as-relay star
------------------------------
The HOST is assumed reachable (open NAT). Every JOINER dials the host with the
existing connect race, so each joiner ends up with one ``punch.Connection`` to
the host. The host owns a SINGLE UDP socket and receives from ALL joiners on it;
it demultiplexes by source address (each distinct source == one peer). The host
is the authority for the roster and relays chat to everyone. No mesh, so it
scales to a handful of players without N^2 links.

Two layers ride the same ``NP1:`` wire:
  * the transport handshake (HELLO/ACK/CONNECTED/KEEPALIVE) -- reused verbatim
    from punch.py; the host replies to HELLO with ACK so joiners connect, and
    the joiner side is a stock ``Connection``.
  * the lobby protocol -- carried INSIDE ``TYPE_DATA`` payloads as one UTF-8
    JSON object with a ``"t"`` (type) field. This is the "extend NP1 with new
    types" the spec asks for: every lobby message is a real ``NP1:`` DATA frame,
    so ``Connection.send``/``recv`` on the joiner and ``_unpack`` on the host
    both handle it unchanged.

Lobby message types (the ``"t"`` field):
    join    {t:join, name}                       joiner -> host
    welcome {t:welcome, you, host}               host -> one joiner (your final,
                                                  de-duplicated, username)
    roster  {t:roster, players[sorted], host}    host -> all
    chat    {t:chat, from, text, ts, cid}        host -> all (relayed + stamped)
    chat    {t:chat, text}                        joiner -> host (host stamps it)
    ping    {t:ping}                              joiner -> host (fast keepalive)
    start   {t:start}                             host -> all
    leave   {t:leave}                             joiner -> host
    reject  {t:reject, reason}                    host -> one joiner (lobby full)
    bye     {t:bye}                               host -> all (lobby closing)

FILE-IPC CONTRACT (how the menu drives it -- kept EXACT)
--------------------------------------------------------
All three files live in the process CWD (override with --io-dir).

  lobby_out.jsonl   the lobby APPENDS newline-delimited JSON events:
      {"type":"code","code":"<base32>"}                 (host, once)
      {"type":"status","state":"waiting|connected|failed","detail":"..."}
      {"type":"roster","players":["alice","bob"],"you":"alice","host":"alice"}
      {"type":"chat","from":"bob","text":"hi","ts":<unix>}
      {"type":"start"}
  lobby_state.json  a SINGLE JSON object, overwritten, mirroring the latest
      roster/state for easy polling:
      {"state","code","players","you","host","started"}
  lobby_in.jsonl    the menu APPENDS command lines; the lobby TAILS it (tracks a
      byte offset, processes only new whole lines) and acts on:
      {"cmd":"chat","text":"..."}     -> send CHAT
      {"cmd":"name","name":"..."}     -> change own username, re-JOIN/broadcast
      {"cmd":"start"}                 -> host broadcasts START (no-op on a client)
      {"cmd":"quit"}                  -> leave cleanly

On startup both jsonl files are TRUNCATED so stale lines aren't reprocessed.

CLI
---
    python lobby.py host --name <username>          # observe, print CODE=, serve
    python lobby.py join <CODE> --name <username>   # dial host, participate
    python lobby.py --selftest                      # 1 host + 2 joiners, loopback
Optional: --local-port 29471, --timeout 40, --io-dir <dir>.
"""

from __future__ import annotations

import argparse
import collections
import hashlib
import json
import os
import random
import select
import shutil
import socket
import struct
import sys
import tempfile
import threading
import time

# Reuse the transport verbatim -- do NOT reinvent the framing/handshake.
from punch import (
    DEFAULT_PORT, TYPE_HELLO, TYPE_ACK, TYPE_CONNECTED, TYPE_KEEPALIVE,
    TYPE_DATA, _pack, _unpack, open_socket,
)
# Reuse the code exchange + the connect race + observe/announce.
from connect import decode_code, race, _observe_and_announce

# --------------------------------------------------------------------------- #
# Tunables
# --------------------------------------------------------------------------- #
CAP = 8                 # max players in a lobby, INCLUDING the host
PING_INTERVAL = 3.0     # joiner -> host lobby keepalive cadence
DROP_AFTER = 10.0       # host drops a peer unheard-from for this long
ROSTER_HEAL = 2.0       # host re-sends the roster this often (UDP self-heal +
                        # doubles as a host -> joiner keepalive)
HOST_GONE_AFTER = 12.0  # joiner declares the host dead after this much silence
CHAT_BURST = 3          # copies of a chat/start packet (best-effort redundancy;
                        # clients de-dupe by cid, so extras are harmless)

# --------------------------------------------------------------------------- #
# Save-transfer tunables (reliable host -> all-joiners file push)
# --------------------------------------------------------------------------- #
# The transport is best-effort UDP (loss/dup/reorder), so a reliability layer is
# built on top: chunk the file(s), sequence them, and let each RECEIVER drive
# recovery (cumulative base + selective NACKs) while the host streams within a
# flow-control window and retransmits on request. Correct under any loss pattern
# and pipelined (never stop-and-wait), so a ~200 MB save moves at link speed.
CHUNK_DATA = 1200           # bytes of file data per chunk. Datagram on the wire =
                            # NP1 frame(5) + chunk header(12) + 1200 = 1217 bytes,
                            # under the 1280 IPv6 min-MTU and 1500 v4 MTU (even
                            # through PPPoE/VPN overhead) -- no fragmentation.
CHUNK_MAGIC = b"NPF1"       # 4-byte tag: a DATA payload starting with this is a
                            # binary chunk, not a JSON lobby message ('{' != 'N').
SEND_WINDOW = 2048          # chunks a peer may have in flight (~2.4 MB); pipelines
                            # the link so RTT doesn't throttle a big transfer.
SEND_BUDGET = 256           # max datagrams sent per peer per pump() -- bounds the
                            # time one host loop iteration spends, so pings/roster
                            # for OTHER peers keep being serviced during a send.
FEEDBACK_INTERVAL = 0.05    # receiver 'fack' (base + NACKs) cadence.
BEGIN_INTERVAL = 0.2        # host re-sends 'fbegin' this often until a peer is ready.
RESEND_AFTER = 0.5          # if a peer's facks go silent this long, rewind its send
                            # cursor and re-stream the window (recovers lost facks).
PEER_XFER_TIMEOUT = 30.0    # no forward progress for this long -> skip that peer.
MAX_NACK = 128              # holes a receiver reports per fack (rest next round).
DRAIN_CAP = 2048            # inbound datagrams a joiner drains per loop iteration.
HOST_DRAIN = 128            # inbound datagrams the host drains per ready cycle.
XFER_SELECT_TIMEOUT = 0.002 # host select() timeout while a transfer is active.
MAX_FILE_RETRIES = 3        # whole-file re-request attempts on a hash mismatch.
INCOMING_BASENAME = "incoming_save"   # joiner writes incoming_save.sav[.lua/.jpg]
XFER_BUF_BYTES = 4 * 1024 * 1024      # best-effort SO_RCVBUF/SO_SNDBUF for bursts.


def _boost_socket_buffers(sock):
    """Best-effort: enlarge the socket's send/recv buffers so bursty chunk
    traffic during a big save transfer isn't dropped in the kernel. Silently
    ignored where the OS clamps or rejects it -- the ARQ layer copes with loss."""
    for opt in (socket.SO_RCVBUF, socket.SO_SNDBUF):
        try:
            sock.setsockopt(socket.SOL_SOCKET, opt, XFER_BUF_BYTES)
        except (OSError, AttributeError):
            pass


def _log(msg):
    """Diagnostics go to stderr; stdout is reserved for the single CODE= line."""
    print(msg, file=sys.stderr, flush=True)


def _dedupe(name, taken):
    """Return ``name`` unless it collides, then ``name#2``, ``name#3``, ..."""
    name = (name or "").strip() or "player"
    if name not in taken:
        return name
    i = 2
    while f"{name}#{i}" in taken:
        i += 1
    return f"{name}#{i}"


# --------------------------------------------------------------------------- #
# File IPC: the flat-file surface the in-game menu reads/writes
# --------------------------------------------------------------------------- #
class LobbyIO:
    """Owns lobby_out.jsonl / lobby_in.jsonl / lobby_state.json in one dir.

    * ``emit`` appends an event to lobby_out.jsonl.
    * ``write_state`` overwrites lobby_state.json atomically (merged fields).
    * ``poll_commands`` tails lobby_in.jsonl, returning only new whole lines.
    Both jsonl files are truncated on construction so stale lines don't replay.
    """

    def __init__(self, directory):
        self.dir = directory
        os.makedirs(directory, exist_ok=True)
        self.out_path = os.path.join(directory, "lobby_out.jsonl")
        self.in_path = os.path.join(directory, "lobby_in.jsonl")
        self.state_path = os.path.join(directory, "lobby_state.json")
        # Truncate the event log and the command inbox on startup.
        open(self.out_path, "w", encoding="utf-8").close()
        open(self.in_path, "w", encoding="utf-8").close()
        self._in_offset = 0
        self._state = {}
        self._lock = threading.Lock()

    def emit(self, event):
        with self._lock:
            with open(self.out_path, "a", encoding="utf-8") as f:
                f.write(json.dumps(event) + "\n")
                f.flush()

    def write_state(self, **fields):
        with self._lock:
            self._state.update(fields)
            data = json.dumps(self._state)
            # The state mirror is advisory -- the menu reads lobby_out.jsonl, not
            # this file -- so a failed write must NEVER crash the client. Sandboxie
            # (and some overlay filesystems) reject the temp-file + os.replace dance
            # with FileNotFoundError; fall back to a direct write, and swallow even
            # that if the sandbox blocks it. Without this, a sandboxed joiner died
            # on the first 'welcome' message and got dropped by the host.
            try:
                tmp = self.state_path + ".tmp"
                with open(tmp, "w", encoding="utf-8") as f:
                    f.write(data)
                    f.flush()
                os.replace(tmp, self.state_path)
            except OSError:
                try:
                    with open(self.state_path, "w", encoding="utf-8") as f:
                        f.write(data)
                        f.flush()
                except OSError:
                    pass

    def poll_commands(self):
        """Return a list of newly-appended command dicts (whole lines only)."""
        cmds = []
        try:
            with open(self.in_path, "rb") as f:
                f.seek(self._in_offset)
                chunk = f.read()
        except FileNotFoundError:
            return cmds
        if not chunk:
            return cmds
        nl = chunk.rfind(b"\n")
        if nl == -1:
            return cmds                       # no complete line yet; wait
        complete = chunk[:nl + 1]
        self._in_offset += len(complete)      # byte-accurate advance
        for line in complete.split(b"\n"):
            line = line.strip()
            if not line:
                continue
            try:
                cmds.append(json.loads(line.decode("utf-8")))
            except (ValueError, UnicodeDecodeError):
                pass                          # ignore malformed command lines
        return cmds


def _send_data(sock, addr, msg):
    """Wrap a lobby message dict in an ``NP1:`` DATA frame and fire it at addr."""
    try:
        sock.sendto(_pack(TYPE_DATA, json.dumps(msg).encode("utf-8")), addr)
    except OSError:
        # Windows spits ICMP-port-unreachable back as an exception when a peer
        # has gone away; the drop-timer will evict it. Ignore.
        pass


# --------------------------------------------------------------------------- #
# Reliable save transfer (host -> every joiner), layered on the DATA channel
# --------------------------------------------------------------------------- #
# Wire additions (all inside NP1 DATA frames):
#   JSON control messages (keep the "t" convention):
#     fbegin      host  -> joiner : {t, sid, total_bytes, chunk, total_chunks,
#                                    files:[{name,size,sha256}], sha256}
#     fbegin_ack  joiner-> host   : {t, sid}                (receiver allocated)
#     fack        joiner-> host   : {t, sid, base, nack:[seq...]}  (feedback)
#     fdone       joiner-> host   : {t, sid, ok[, final]}   (verified / gave up)
#   BINARY chunk (host -> joiner), NOT JSON, to avoid base64 bloat:
#     CHUNK_MAGIC(4) + sid(uint32 BE) + seq(uint32 BE) + up-to-CHUNK_DATA bytes
#
# All files (.sav + optional .sav.lua + .jpg) are concatenated into ONE byte
# stream with a single sequence space; the receiver splits them back out using
# the per-file sizes in `fbegin`. Integrity is SHA-256 per file AND overall.
def _read_save_files(save_path):
    """Read the .sav and any sidecars; return (blob, files_meta).

    ``blob`` is the concatenation (order: .sav, .sav.lua, .jpg). ``files_meta``
    is the list the host advertises in ``fbegin`` -- each entry names the file
    with the joiner's target basename (incoming_save.*) plus its size + sha256.
    Raises on a missing/unreadable .sav.
    """
    save_path = os.path.abspath(save_path)
    if not os.path.isfile(save_path):
        raise FileNotFoundError(f"save not found: {save_path}")
    if save_path.lower().endswith(".sav"):
        stem = save_path[:-4]
    else:
        stem = os.path.splitext(save_path)[0]
    sources = [(save_path, INCOMING_BASENAME + ".sav")]
    lua = save_path + ".lua"                      # <name>.sav.lua
    jpg = stem + ".jpg"                           # <name>.jpg
    if os.path.isfile(lua):
        sources.append((lua, INCOMING_BASENAME + ".sav.lua"))
    if os.path.isfile(jpg):
        sources.append((jpg, INCOMING_BASENAME + ".jpg"))
    blob = bytearray()
    files_meta = []
    for src, logical in sources:
        with open(src, "rb") as f:
            data = f.read()
        files_meta.append({"name": logical, "size": len(data),
                           "sha256": hashlib.sha256(data).hexdigest()})
        blob += data
    # Return the bytearray as-is (no bytes() copy) -- for a ~200 MB save that
    # avoids a transient second 200 MB allocation; slicing it per chunk works.
    return blob, files_meta


class _HostSaveTransfer:
    """Reliable host -> all-joiners file push, PUMPED from the host's main loop.

    No hard size cap (real TF2 saves are ~200 MB); the whole concatenation is
    held in memory once and fanned out to every peer. Reliability is
    receiver-driven selective repeat:

      * the host streams chunks within a flow-control window (SEND_WINDOW);
      * each receiver periodically reports its cumulative ``base`` (every chunk
        below base is in hand) plus an explicit ``nack`` list of holes;
      * the host drops everything below base, retransmits NACKed chunks first,
        then sends new in-order chunks up to base+window;
      * if a receiver goes silent (its facks were lost) the host rewinds and
        re-streams the window, so it self-heals under ANY loss pattern.

    ``pump()`` sends at most SEND_BUDGET datagrams PER PEER, so it never hogs the
    single demux loop -- keepalives/roster for other peers keep flowing during a
    transfer. All ACK-side messages arrive through the host's normal recv path
    and are routed in via on_begin_ack / on_fack / on_fdone.
    """

    def __init__(self, sock, sid, blob, files_meta, targets, io, log):
        self.sock = sock
        self.sid = sid
        self.blob = blob
        self.total_bytes = len(blob)
        self.chunk = CHUNK_DATA
        self.total_chunks = (self.total_bytes + self.chunk - 1) // self.chunk
        self.files_meta = files_meta
        self.overall_sha = hashlib.sha256(blob).hexdigest()
        self.io = io
        self.log = log
        self.begin_msg = {"t": "fbegin", "sid": sid,
                          "total_bytes": self.total_bytes, "chunk": self.chunk,
                          "total_chunks": self.total_chunks,
                          "files": files_meta, "sha256": self.overall_sha}
        now = time.time()
        self.peers = {}          # addr -> per-peer send state
        for addr, name in targets:
            self.peers[addr] = {
                "name": name, "ready": False, "base": 0, "next": 0,
                "nack": [], "last_fack": now, "last_begin": 0.0,
                "last_resend": 0.0, "last_advance": now,
                "state": "active", "last_pct": -1,
            }
        self.log(f"[host] save transfer sid={sid} {self.total_bytes}B in "
                 f"{self.total_chunks} chunks -> {len(self.peers)} peer(s)")

    # -- progress ---------------------------------------------------------- #
    def _emit_pct(self, p):
        if self.total_bytes == 0:
            pct = 100
        else:
            done = min(p["base"] * self.chunk, self.total_bytes)
            pct = int(done * 100 // self.total_bytes)
        if pct // 10 > p["last_pct"] // 10:
            p["last_pct"] = pct
            self.io.emit({"type": "transfer", "role": "send",
                          "peer": p["name"], "pct": pct})

    # -- outbound chunk ---------------------------------------------------- #
    def _send_chunk(self, addr, seq):
        off = seq * self.chunk
        data = self.blob[off:off + self.chunk]
        frame = CHUNK_MAGIC + struct.pack("!II", self.sid, seq) + data
        try:
            self.sock.sendto(_pack(TYPE_DATA, frame), addr)
        except OSError:
            pass                      # kernel buffer full etc.; ARQ will re-send

    # -- inbound ACK routing (called from the host recv loop) -------------- #
    def on_begin_ack(self, addr, msg):
        p = self.peers.get(addr)
        if not p or msg.get("sid") != self.sid:
            return
        if not p["ready"]:
            p["ready"] = True
            self.log(f"[host] {p['name']} ready for save")

    def on_fack(self, addr, msg):
        p = self.peers.get(addr)
        if not p or msg.get("sid") != self.sid or p["state"] != "active":
            return
        now = time.time()
        p["ready"] = True
        p["last_fack"] = now
        base = int(msg.get("base", 0))
        if base > p["base"]:
            p["base"] = base
            p["last_advance"] = now
            if p["next"] < base:
                p["next"] = base
        # Merge the reported holes with any still-pending ones (all >= base).
        holes = {s for s in msg.get("nack", [])
                 if isinstance(s, int) and p["base"] <= s < self.total_chunks}
        holes |= {s for s in p["nack"] if s >= p["base"]}
        p["nack"] = sorted(holes)
        self._emit_pct(p)

    def on_fdone(self, addr, msg):
        p = self.peers.get(addr)
        if not p or msg.get("sid") != self.sid:
            return
        if msg.get("ok"):
            if p["state"] == "active":
                p["state"] = "done"
                self.io.emit({"type": "transfer", "role": "send",
                              "peer": p["name"], "state": "done"})
                self.log(f"[host] {p['name']} verified save transfer")
        elif msg.get("final"):
            if p["state"] == "active":
                p["state"] = "failed"
                self.io.emit({"type": "status", "state": "failed",
                              "detail": f"save transfer: {p['name']} could "
                                        f"not verify the file"})
                self.log(f"[host] {p['name']} FAILED save (receiver gave up)")
        # A non-final ok:false means the receiver is retrying -> keep serving.

    def on_peer_dropped(self, addr):
        p = self.peers.get(addr)
        if p and p["state"] == "active":
            p["state"] = "dropped"
            self.io.emit({"type": "status", "state": "failed",
                          "detail": f"save transfer: {p['name']} dropped "
                                    f"mid-transfer"})
            self.log(f"[host] {p['name']} dropped mid-transfer -- skipping")

    # -- the pump (one slice of work per peer) ----------------------------- #
    def pump(self, now):
        for addr, p in self.peers.items():
            if p["state"] != "active":
                continue
            if now - p["last_advance"] > PEER_XFER_TIMEOUT:
                p["state"] = "failed"
                self.io.emit({"type": "status", "state": "failed",
                              "detail": f"save transfer: {p['name']} timed out"})
                self.log(f"[host] {p['name']} save transfer TIMED OUT")
                continue
            if not p["ready"] or self.total_chunks == 0:
                if now - p["last_begin"] >= BEGIN_INTERVAL:
                    _send_data(self.sock, addr, self.begin_msg)
                    p["last_begin"] = now
                continue
            # Receiver silent for too long? Its facks were lost -- rewind and
            # re-stream the window so it (and its facks) can catch up.
            if (now - p["last_fack"] > RESEND_AFTER
                    and now - p["last_resend"] > RESEND_AFTER):
                p["next"] = p["base"]
                p["last_resend"] = now
            budget = SEND_BUDGET
            # 1) selective retransmits (explicit holes) first
            while budget > 0 and p["nack"]:
                seq = p["nack"].pop(0)
                if seq < p["base"] or seq >= self.total_chunks:
                    continue
                self._send_chunk(addr, seq)
                budget -= 1
            # 2) new in-order chunks, capped by the flow-control window
            limit = min(self.total_chunks, p["base"] + SEND_WINDOW)
            while budget > 0 and p["next"] < limit:
                self._send_chunk(addr, p["next"])
                p["next"] += 1
                budget -= 1

    def all_resolved(self):
        return all(p["state"] in ("done", "failed", "dropped")
                   for p in self.peers.values())


class _ClientSaveReceiver:
    """Receive + verify a save on a joiner; drives recovery via facks/NACKs.

    Holds the whole file set in memory (a bytearray sized from ``fbegin``),
    writes each received chunk at its exact offset (so reorder/dupes are free),
    tracks the contiguous ``base``, and periodically reports base + holes. On
    completion it verifies SHA-256 per file and overall, writes the files into
    the lobby IO directory as incoming_save.*, and emits ``save_ready``.
    """

    def __init__(self, conn, io, log):
        self.conn = conn
        self.io = io
        self.log = log
        self.sid = None
        self.buf = None
        self.have = None
        self.total_bytes = 0
        self.chunk = CHUNK_DATA
        self.total_chunks = 0
        self.files = []
        self.overall_sha = None
        self.base = 0
        self.recv_count = 0
        self.complete = False
        self.failed = False
        self.retries = 0
        self.last_fack = 0.0
        self.last_pct = -1
        self.done_sends = 0
        self.last_done = 0.0

    def active(self):
        """True while a transfer is in progress (steer the loop to poll fast)."""
        return self.sid is not None and not self.complete and not self.failed

    def _send(self, msg):
        try:
            self.conn.send(json.dumps(msg).encode("utf-8"))
        except (RuntimeError, OSError):
            pass

    # -- inbound ----------------------------------------------------------- #
    def on_begin(self, msg):
        sid = msg.get("sid")
        if sid == self.sid:
            self._send({"t": "fbegin_ack", "sid": sid})   # duplicate -> re-ack
            return
        # A brand-new session (first ever, or a later transfer): (re)allocate.
        self.sid = sid
        self.total_bytes = int(msg.get("total_bytes", 0))
        self.chunk = int(msg.get("chunk", CHUNK_DATA)) or CHUNK_DATA
        self.total_chunks = int(msg.get("total_chunks", 0))
        self.files = msg.get("files", [])
        self.overall_sha = msg.get("sha256")
        try:
            self.buf = bytearray(self.total_bytes)
        except (MemoryError, OverflowError):
            self.failed = True
            self.io.emit({"type": "status", "state": "failed",
                          "detail": "save transfer failed: cannot allocate "
                                    f"{self.total_bytes} bytes"})
            return
        self.have = bytearray(self.total_chunks)
        self.base = 0
        self.recv_count = 0
        self.complete = False
        self.failed = False
        self.retries = 0
        self.last_pct = -1
        self.done_sends = 0
        self.log(f"[client] save incoming sid={sid} {self.total_bytes}B "
                 f"{self.total_chunks} chunks")
        self._send({"t": "fbegin_ack", "sid": sid})
        self.io.emit({"type": "transfer", "role": "recv", "pct": 0})
        if self.total_chunks == 0:
            self._finalize()

    def on_chunk(self, sid, seq, data):
        if self.sid is None or sid != self.sid:
            return
        if self.complete:
            self._maybe_send_done(force=True)   # nudge host to stop resending
            return
        if self.failed or seq < 0 or seq >= self.total_chunks:
            return
        if self.have[seq]:
            return                               # dup / reorder -- already have it
        off = seq * self.chunk
        self.buf[off:off + len(data)] = data
        self.have[seq] = 1
        self.recv_count += 1
        while self.base < self.total_chunks and self.have[self.base]:
            self.base += 1
        self._emit_pct()
        if self.base >= self.total_chunks:
            self._finalize()

    def _emit_pct(self):
        if self.total_chunks == 0:
            pct = 100
        else:
            pct = int(self.recv_count * 100 // self.total_chunks)
        if pct // 10 > self.last_pct // 10:
            self.last_pct = pct
            self.io.emit({"type": "transfer", "role": "recv", "pct": pct})

    # -- periodic (called from the client loop) ---------------------------- #
    def tick(self, now):
        if self.sid is None or self.failed:
            return
        if self.complete:
            self._maybe_send_done(now=now)
            return
        if now - self.last_fack >= FEEDBACK_INTERVAL:
            self.last_fack = now
            self._send_fack()

    def _send_fack(self):
        nack = []
        limit = min(self.total_chunks, self.base + SEND_WINDOW)
        s = self.base
        while s < limit and len(nack) < MAX_NACK:
            if not self.have[s]:
                nack.append(s)
            s += 1
        self._send({"t": "fack", "sid": self.sid, "base": self.base,
                    "nack": nack})

    def _maybe_send_done(self, now=None, force=False):
        # After completion the host may not have heard our fdone (it can be
        # lost), so we keep re-announcing it on a timer AND force a reply to any
        # chunk the host retransmits -- either way the host learns we're done.
        now = now or time.time()
        if force or (self.done_sends < 40 and now - self.last_done >= 0.2):
            self.last_done = now
            self.done_sends += 1
            self._send({"t": "fdone", "sid": self.sid, "ok": True})

    # -- assemble + verify + write ----------------------------------------- #
    def _finalize(self):
        ok = True
        off = 0
        parts = {}
        for meta in self.files:
            size = int(meta.get("size", 0))
            part = bytes(self.buf[off:off + size])
            off += size
            if hashlib.sha256(part).hexdigest() != meta.get("sha256"):
                ok = False
                break
            parts[meta.get("name")] = part
        if ok and self.overall_sha:
            if hashlib.sha256(bytes(self.buf)).hexdigest() != self.overall_sha:
                ok = False
        if not ok:
            self.retries += 1
            if self.retries <= MAX_FILE_RETRIES:
                self.log(f"[client] save hash mismatch -- re-request "
                         f"(attempt {self.retries})")
                self.have = bytearray(self.total_chunks)   # request everything
                self.base = 0
                self.recv_count = 0
                self.last_pct = -1
                self._send_fack()
                return
            self.failed = True
            self.io.emit({"type": "status", "state": "failed",
                          "detail": "save transfer failed: hash mismatch"})
            self._send({"t": "fdone", "sid": self.sid, "ok": False,
                        "final": True})
            return
        written = []
        try:
            for meta in self.files:
                name = meta.get("name")
                with open(os.path.join(self.io.dir, name), "wb") as f:
                    f.write(parts[name])
                written.append(name)
        except OSError as e:
            self.failed = True
            self.io.emit({"type": "status", "state": "failed",
                          "detail": f"save transfer failed: write error: {e}"})
            self._send({"t": "fdone", "sid": self.sid, "ok": False,
                        "final": True})
            return
        self.complete = True
        self.io.emit({"type": "transfer", "role": "recv", "pct": 100})
        self.io.emit({"type": "save_ready", "name": INCOMING_BASENAME,
                      "dir": os.path.abspath(self.io.dir), "files": written})
        self.log(f"[client] save ready: {written} in {self.io.dir}")
        self._maybe_send_done(force=True)


# --------------------------------------------------------------------------- #
# HOST: single socket, N peers, authority for roster + chat relay
# --------------------------------------------------------------------------- #
def run_host(sock, my_name, io, code=None, stop=None, drop_after=DROP_AFTER,
             log=_log):
    """Run the lobby server forever on ``sock`` (blocks until ``stop`` is set).

    ``sock`` is a bound UDP socket (the observe/game socket for the real CLI, a
    plain loopback socket for the self-test). ``io`` is a :class:`LobbyIO`.
    """
    stop = stop or threading.Event()
    sock.setblocking(False)
    _boost_socket_buffers(sock)             # help bursty save-transfer traffic

    host_name = _dedupe(my_name, set())     # reassigned by the 'name' command
    peers = collections.OrderedDict()       # addr -> {"name":str, "last":float}
    cid_counter = [0]                       # host-authoritative chat id
    started = [False]
    last_emitted_roster = [None]
    transfer = [None]                       # the active _HostSaveTransfer, or None

    if code:
        io.emit({"type": "code", "code": code})

    # ---- roster helpers ---------------------------------------------------- #
    def all_names(exclude_addr=None):
        names = {host_name}
        for a, p in peers.items():
            if a != exclude_addr:
                names.add(p["name"])
        return names

    def roster_players():
        return sorted([host_name] + [p["name"] for p in peers.values()])

    def send_roster_packets():
        msg = {"t": "roster", "players": roster_players(), "host": host_name}
        for a in list(peers):
            _send_data(sock, a, msg)

    def emit_roster():
        players = roster_players()
        io.emit({"type": "roster", "players": players,
                 "you": host_name, "host": host_name})
        io.write_state(state="connected", code=code, players=players,
                       you=host_name, host=host_name, started=started[0])
        last_emitted_roster[0] = tuple(players)

    def roster_changed(broadcast=True):
        """Push the roster to peers, and emit an event only if it changed."""
        if broadcast:
            send_roster_packets()
        if last_emitted_roster[0] != tuple(roster_players()):
            emit_roster()

    def broadcast_chat(frm, text):
        cid_counter[0] += 1
        ts = int(time.time())
        msg = {"t": "chat", "from": frm, "text": text, "ts": ts,
               "cid": cid_counter[0]}
        for _ in range(CHAT_BURST):
            for a in list(peers):
                _send_data(sock, a, msg)
        io.emit({"type": "chat", "from": frm, "text": text, "ts": ts})

    def broadcast_start():
        started[0] = True
        for _ in range(CHAT_BURST):
            for a in list(peers):
                _send_data(sock, a, {"t": "start"})
        io.emit({"type": "start"})
        io.write_state(started=True)

    # ---- inbound lobby messages -------------------------------------------- #
    def do_join(addr, name):
        if addr in peers:                                   # rename in place
            peers[addr]["name"] = _dedupe(name, all_names(exclude_addr=addr))
        else:                                               # brand-new joiner
            if len(peers) + 1 >= CAP:
                _send_data(sock, addr, {"t": "reject", "reason": "lobby full"})
                log(f"[host] rejected {addr} (lobby full)")
                return
            assigned = _dedupe(name, all_names())
            peers[addr] = {"name": assigned, "last": time.time()}
            log(f"[host] JOIN {addr} as {assigned!r}")
        peers[addr]["last"] = time.time()
        _send_data(sock, addr, {"t": "welcome",
                                "you": peers[addr]["name"], "host": host_name})
        roster_changed()
        if started[0]:                                      # late joiner catch-up
            _send_data(sock, addr, {"t": "start"})

    def handle_data(addr, payload):
        try:
            msg = json.loads(payload.decode("utf-8"))
        except (ValueError, UnicodeDecodeError):
            return
        t = msg.get("t")
        if addr in peers:
            peers[addr]["last"] = time.time()
        if t == "join":
            do_join(addr, msg.get("name", "player"))
        elif t == "chat":
            if addr in peers:
                broadcast_chat(peers[addr]["name"], str(msg.get("text", "")))
        elif t == "ping":
            pass                                            # last-seen refreshed
        elif t == "leave":
            if addr in peers:
                log(f"[host] LEAVE {addr} ({peers[addr]['name']})")
                del peers[addr]
                if transfer[0] is not None:
                    transfer[0].on_peer_dropped(addr)
                roster_changed()
        # ---- reliable save-transfer feedback (receiver -> host) ---------- #
        elif t == "fbegin_ack":
            if transfer[0] is not None:
                transfer[0].on_begin_ack(addr, msg)
        elif t == "fack":
            if transfer[0] is not None:
                transfer[0].on_fack(addr, msg)
        elif t == "fdone":
            if transfer[0] is not None:
                transfer[0].on_fdone(addr, msg)

    # ---- save transfer: read the file(s), fan out reliably, THEN start ----- #
    def begin_save_transfer(save_path):
        """Kick off a reliable push of ``save_path`` (+ sidecars) to all peers.

        Runs entirely off the main loop: this only builds the transfer object;
        pump()/ACK-routing happen in the serve loop so pings/roster keep flowing.
        Once every peer has verified (or dropped/timed out), the loop broadcasts
        the normal 'start'. On a read error we emit failed and do NOT start.
        """
        if transfer[0] is not None:
            log("[host] start(save) ignored -- a transfer is already running")
            return
        try:
            blob, files_meta = _read_save_files(save_path)
        except (OSError, ValueError) as e:
            io.emit({"type": "status", "state": "failed",
                     "detail": f"save transfer failed: {e}"})
            log(f"[host] save read failed: {e}")
            return
        if not peers:
            log("[host] start(save): no joiners connected -- starting immediately")
            broadcast_start()
            return
        sid = int(time.time() * 1000) & 0xFFFFFFFF
        targets = [(a, peers[a]["name"]) for a in peers]
        transfer[0] = _HostSaveTransfer(sock, sid, blob, files_meta, targets,
                                        io, log)

    # ---- local (host's own menu) commands ---------------------------------- #
    def handle_command(cmd):
        nonlocal host_name
        c = cmd.get("cmd")
        if c == "chat":
            broadcast_chat(host_name, str(cmd.get("text", "")))
        elif c == "name":
            host_name = _dedupe(str(cmd.get("name", "player")),
                                {p["name"] for p in peers.values()})
            roster_changed()
        elif c == "start":
            save = cmd.get("save")
            if transfer[0] is not None:
                log("[host] start ignored -- a save transfer is in progress")
            elif save:
                begin_save_transfer(save)         # broadcasts start when done
            else:
                broadcast_start()                 # legacy start, no transfer
        elif c == "quit":
            stop.set()

    # ---- serve ------------------------------------------------------------- #
    io.emit({"type": "status", "state": "connected",
             "detail": f"lobby ready on {sock.getsockname()[1]}"})
    emit_roster()                                           # initial: just host
    log(f"[host] serving as {host_name!r} on udp/{sock.getsockname()[1]}")

    last_heal = last_drop = 0.0
    try:
        while not stop.is_set():
            # While a transfer runs, poll fast so we pump chunks + absorb ACKs
            # promptly; otherwise idle at 0.2 s to keep the loop cheap.
            timeout = XFER_SELECT_TIMEOUT if transfer[0] is not None else 0.2
            try:
                ready, _, _ = select.select([sock], [], [], timeout)
            except (OSError, ValueError):
                break
            now = time.time()

            # Drain up to HOST_DRAIN datagrams this cycle -- a busy transfer can
            # deliver a burst of facks/pings, and one-per-iteration would let the
            # kernel recv buffer overflow (self-inflicted loss).
            if ready:
                for _ in range(HOST_DRAIN):
                    try:
                        data, addr = sock.recvfrom(65535)
                    except BlockingIOError:
                        break                               # nothing left to read
                    except (ConnectionResetError, OSError):
                        break
                    if not data:
                        break
                    ptype, payload = _unpack(data)
                    if ptype == TYPE_HELLO:
                        # Complete the joiner's handshake: echo THEIR token.
                        try:
                            sock.sendto(_pack(TYPE_ACK, payload), addr)
                        except OSError:
                            pass
                    elif ptype in (TYPE_ACK, TYPE_CONNECTED):
                        pass                                # informational
                    elif ptype == TYPE_KEEPALIVE:
                        if addr in peers:
                            peers[addr]["last"] = now
                    elif ptype == TYPE_DATA:
                        handle_data(addr, payload)

            if now - last_drop >= 1.0:
                last_drop = now
                dead = [a for a, p in peers.items()
                        if now - p["last"] > drop_after]
                for a in dead:
                    log(f"[host] DROP {a} ({peers[a]['name']}) -- silent")
                    del peers[a]
                    if transfer[0] is not None:
                        transfer[0].on_peer_dropped(a)     # skip it, keep going
                if dead:
                    roster_changed()

            if now - last_heal >= ROSTER_HEAL:
                last_heal = now
                send_roster_packets()

            for cmd in io.poll_commands():
                handle_command(cmd)

            # Pump the save transfer (if any) and, once every peer has verified
            # or been skipped, broadcast the normal 'start' so all begin together.
            if transfer[0] is not None:
                try:
                    transfer[0].pump(now)
                    if transfer[0].all_resolved():
                        log("[host] all save transfers resolved -- starting")
                        transfer[0] = None
                        broadcast_start()
                except Exception as e:                     # never crash the lobby
                    log(f"[host] save transfer error: {e!r}")
                    io.emit({"type": "status", "state": "failed",
                             "detail": f"save transfer failed: {e}"})
                    transfer[0] = None
    except KeyboardInterrupt:
        pass
    finally:
        for a in list(peers):
            _send_data(sock, a, {"t": "bye"})
        io.emit({"type": "status", "state": "failed",
                 "detail": "lobby closed"})
        io.write_state(state="failed")
        try:
            sock.close()
        except OSError:
            pass


# --------------------------------------------------------------------------- #
# CLIENT: one Connection to the host; participate
# --------------------------------------------------------------------------- #
def run_client(conn, my_name, io, stop=None, host_gone_after=HOST_GONE_AFTER,
               log=_log):
    """Participate in the lobby over a connected ``punch.Connection`` (blocks)."""
    stop = stop or threading.Event()

    desired = [my_name]     # what we asked to be called
    assigned = [my_name]    # what the host actually named us (from 'welcome')
    started = [False]
    last_roster = [None]
    seen_cids = collections.deque(maxlen=512)
    seen_set = set()

    try:
        _boost_socket_buffers(conn.sock)    # help bursty save-transfer traffic
    except AttributeError:
        pass
    receiver = _ClientSaveReceiver(conn, io, log)   # save-transfer receive side

    io.emit({"type": "status", "state": "connected",
             "detail": f"joined host {conn.peer_str}"})
    io.write_state(state="connected", you=desired[0], started=False)

    def send(msg):
        try:
            conn.send(json.dumps(msg).encode("utf-8"))
        except (RuntimeError, OSError):
            pass

    def handle_msg(raw):
        # Binary save-transfer chunks are NOT JSON: a DATA payload starting with
        # CHUNK_MAGIC is a chunk frame; everything else is a JSON lobby message.
        if raw[:4] == CHUNK_MAGIC:
            if len(raw) >= 12:
                sid, seq = struct.unpack("!II", raw[4:12])
                receiver.on_chunk(sid, seq, raw[12:])
            return
        try:
            m = json.loads(raw.decode("utf-8"))
        except (ValueError, UnicodeDecodeError):
            return
        t = m.get("t")
        if t == "fbegin":
            receiver.on_begin(m)
            return
        if t == "welcome":
            assigned[0] = m.get("you", desired[0])
            io.write_state(you=assigned[0], host=m.get("host"))
            log(f"[client] host named us {assigned[0]!r}")
        elif t == "roster":
            players = m.get("players", [])
            if tuple(players) != last_roster[0]:
                last_roster[0] = tuple(players)
                io.emit({"type": "roster", "players": players,
                         "you": assigned[0], "host": m.get("host")})
                io.write_state(state="connected", players=players,
                               you=assigned[0], host=m.get("host"),
                               started=started[0])
        elif t == "chat":
            cid = m.get("cid")
            if cid in seen_set:
                return                                      # de-dupe the burst
            if len(seen_cids) == seen_cids.maxlen:
                seen_set.discard(seen_cids.popleft())
            seen_cids.append(cid)
            seen_set.add(cid)
            io.emit({"type": "chat", "from": m.get("from"),
                     "text": m.get("text"), "ts": m.get("ts")})
        elif t == "start":
            if not started[0]:
                started[0] = True
                io.emit({"type": "start"})
                io.write_state(started=True)
        elif t == "reject":
            io.emit({"type": "status", "state": "failed",
                     "detail": m.get("reason", "rejected")})
            stop.set()
        elif t == "bye":
            io.emit({"type": "status", "state": "failed",
                     "detail": "host closed the lobby"})
            stop.set()

    def handle_command(cmd):
        c = cmd.get("cmd")
        if c == "chat":
            send({"t": "chat", "text": str(cmd.get("text", ""))})
        elif c == "name":
            desired[0] = str(cmd.get("name", "player"))
            send({"t": "join", "name": desired[0]})         # host re-dedupes
        elif c == "start":
            log("[client] 'start' ignored -- only the host can start")
        elif c == "quit":
            send({"t": "leave"})
            io.emit({"type": "status", "state": "failed", "detail": "left lobby"})
            stop.set()

    send({"t": "join", "name": desired[0]})                 # announce ourselves
    last_ping = 0.0
    try:
        while not stop.is_set():
            if conn.last_seen_age() > host_gone_after:
                io.emit({"type": "status", "state": "failed",
                         "detail": "host unreachable"})
                io.write_state(state="failed")
                break
            # Drain a burst of inbound datagrams (chunks arrive fast during a
            # transfer). Block briefly on the first recv so we don't spin when
            # idle; then pull whatever else is already queued, up to DRAIN_CAP.
            busy = receiver.active()
            raw = conn.recv(timeout=0.02 if busy else 0.2)
            drained = 0
            while raw is not None:
                try:
                    handle_msg(raw)
                except Exception as e:                      # never die on one msg
                    log(f"[client] message error: {e!r}")
                drained += 1
                if drained >= DRAIN_CAP:
                    break
                raw = conn.recv(timeout=0.0)
            now = time.time()
            receiver.tick(now)                              # facks / fdone cadence
            if now - last_ping >= PING_INTERVAL:
                last_ping = now
                send({"t": "ping"})
            for cmd in io.poll_commands():
                handle_command(cmd)
    except KeyboardInterrupt:
        send({"t": "leave"})
    finally:
        conn.close()


# --------------------------------------------------------------------------- #
# CLI commands
# --------------------------------------------------------------------------- #
def cmd_host(args):
    io = LobbyIO(args.io_dir or os.getcwd())
    io.emit({"type": "status", "state": "waiting", "detail": "observing NAT"})
    io.write_state(state="waiting")
    # observe + print the single CODE= line (reused from connect.py).
    sock, _profile, code = _observe_and_announce(args.local_port)
    run_host(sock, args.name, io, code=code)
    return 0


def cmd_join(args):
    io = LobbyIO(args.io_dir or os.getcwd())
    io.emit({"type": "status", "state": "waiting", "detail": "dialing host"})
    io.write_state(state="waiting")
    try:
        peer = decode_code(args.code)
    except ValueError as e:
        io.emit({"type": "status", "state": "failed", "detail": f"bad code: {e}"})
        io.write_state(state="failed")
        _log(f"[join] bad code: {e}")
        return 2
    if peer.get("stale"):
        _log(f"[join] WARNING: code is {peer['age']}s old -- may be stale")
    # Star model: the host is open, so we simply DIAL its v4 candidates. This
    # reuses connect.py's race (role='dial', v4 only -> single socket).
    sock = open_socket(args.local_port, socket.AF_INET)
    conn = race(sock, peer, "dial", args.local_port, args.timeout,
                my_has_v6=False)
    if not conn:
        io.emit({"type": "status", "state": "failed",
                 "detail": "could not reach host"})
        io.write_state(state="failed")
        _log("[join] FAILED to reach host")
        return 1
    _log(f"[join] connected to host {conn.peer_str}")
    run_client(conn, args.name, io)
    return 0


# --------------------------------------------------------------------------- #
# Self-test: 1 host + 2 joiners on loopback; roster + chat must reach all three
# --------------------------------------------------------------------------- #
def _read_events(path):
    events = []
    try:
        with open(path, "r", encoding="utf-8") as f:
            for line in f:
                line = line.strip()
                if line:
                    try:
                        events.append(json.loads(line))
                    except ValueError:
                        pass
    except FileNotFoundError:
        pass
    return events


def _latest_roster(path):
    rosters = [e for e in _read_events(path) if e.get("type") == "roster"]
    return rosters[-1] if rosters else None


def _has_chat(path, text):
    return any(e.get("type") == "chat" and e.get("text") == text
               for e in _read_events(path))


def _wait_until(predicate, timeout=12.0, interval=0.15):
    deadline = time.time() + timeout
    while time.time() < deadline:
        if predicate():
            return True
        time.sleep(interval)
    return predicate()


def selftest():
    HP, P1, P2 = 29520, 29521, 29522
    names = {"host": "alice", "j1": "bob", "j2": "carol"}
    expected = sorted(names.values())
    base = tempfile.mkdtemp(prefix="lobby_selftest_")
    dirs = {k: os.path.join(base, k) for k in names}
    ios = {k: LobbyIO(dirs[k]) for k in names}
    stop = threading.Event()
    conns = []
    print(f"[selftest] scratch dir: {base}")
    print(f"[selftest] host udp/{HP}  joiners udp/{P1},{P2}")

    ok = True
    try:
        # --- host ---
        hsock = open_socket(HP, socket.AF_INET)
        threading.Thread(target=run_host, name="host",
                         args=(hsock, names["host"], ios["host"]),
                         kwargs={"code": "SELFTESTCODE", "stop": stop},
                         daemon=True).start()
        time.sleep(0.4)                       # let the server bind + listen

        # --- two joiners dial the host over loopback (reuse the connect race) --
        for port, key in ((P1, "j1"), (P2, "j2")):
            peer = {"candidates": {"public_v4": f"127.0.0.1:{HP}",
                                   "lan_v4": None, "v6": None},
                    "flags": {"open": True, "v6": False}}
            s = open_socket(port, socket.AF_INET)
            conn = race(s, peer, "dial", port, 10, my_has_v6=False)
            if not conn:
                print(f"[selftest] FAIL: joiner {names[key]} could not connect")
                return False
            conns.append(conn)
            threading.Thread(target=run_client, name=key,
                             args=(conn, names[key], ios[key]),
                             kwargs={"stop": stop}, daemon=True).start()

        # (a) every roster must list all three usernames
        def rosters_full():
            for k in names:
                r = _latest_roster(ios[k].out_path)
                if not r or sorted(r.get("players", [])) != expected:
                    return False
            return True

        if not _wait_until(rosters_full, timeout=12):
            ok = False
            for k in names:
                print(f"[selftest]   {k} roster = {_latest_roster(ios[k].out_path)}")
            print("[selftest] FAIL (a): all three not in every roster")
        else:
            print(f"[selftest] OK (a): every roster = {expected}")

        # (b) a chat from j1 must reach all three via their lobby_out.jsonl
        chat_text = "hello lobby from bob"
        with open(ios["j1"].in_path, "a", encoding="utf-8") as f:
            f.write(json.dumps({"cmd": "chat", "text": chat_text}) + "\n")

        def chat_everywhere():
            return all(_has_chat(ios[k].out_path, chat_text) for k in names)

        if not _wait_until(chat_everywhere, timeout=12):
            ok = False
            for k in names:
                got = _has_chat(ios[k].out_path, chat_text)
                print(f"[selftest]   {k} has chat: {got}")
            print("[selftest] FAIL (b): chat did not reach all three")
        else:
            print(f"[selftest] OK (b): chat reached host + both joiners")
    finally:
        stop.set()
        time.sleep(0.3)
        for c in conns:
            try:
                c.close()
            except Exception:
                pass
        try:
            shutil.rmtree(base, ignore_errors=True)
        except Exception:
            pass

    print(f"[selftest] {'PASS' if ok else 'FAIL'}")
    return ok


# --------------------------------------------------------------------------- #
# Self-test: reliable SAVE TRANSFER host -> 2 joiners, with packet-loss injection
# --------------------------------------------------------------------------- #
class _LossySocket:
    """Wrap a UDP socket and randomly DROP a fraction of outbound datagrams.

    Used only by the transfer self-test to prove the ARQ retransmit path on
    loopback (loopback itself never loses). It forwards ``fileno`` so select()
    still works, exposes ``family``, and delegates everything else to the real
    socket; only ``sendto`` is intercepted."""

    def __init__(self, sock, loss=0.0):
        self._sock = sock
        self.loss = loss

    def sendto(self, data, addr):
        if self.loss and random.random() < self.loss:
            return len(data)                      # pretend sent; silently dropped
        return self._sock.sendto(data, addr)

    def fileno(self):
        return self._sock.fileno()

    @property
    def family(self):
        return self._sock.family

    def __getattr__(self, name):                  # recvfrom/setblocking/close/...
        return getattr(self._sock, name)


def _sha256_file(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for block in iter(lambda: f.read(1 << 20), b""):
            h.update(block)
    return h.hexdigest()


def _run_transfer_once(loss, size_bytes, tag):
    """One full host + 2-joiner save transfer over loopback. Returns True/False.

    Asserts: each joiner's incoming_save.* is byte-identical (SHA-256) to the
    source, each joiner emitted save_ready + start, the host emitted a per-peer
    state:"done", and the host's {"type":"start"} came AFTER both done events.
    """
    HP, P1, P2 = 29520, 29521, 29522
    names = {"host": "alice", "j1": "bob", "j2": "carol"}
    base = tempfile.mkdtemp(prefix="lobby_xfer_")
    dirs = {k: os.path.join(base, k) for k in names}
    ios = {k: LobbyIO(dirs[k]) for k in names}
    stop = threading.Event()
    conns = []

    # Build the "save": a realistic .sav plus the two small sidecars.
    save_path = os.path.join(base, "world.sav")
    with open(save_path, "wb") as f:
        f.write(os.urandom(size_bytes))
    with open(save_path + ".lua", "wb") as f:            # world.sav.lua sidecar
        f.write(b"-- meta\n" + os.urandom(2048))
    with open(os.path.join(base, "world.jpg"), "wb") as f:  # world.jpg sidecar
        f.write(os.urandom(4096))
    src_sha = {
        "incoming_save.sav": _sha256_file(save_path),
        "incoming_save.sav.lua": _sha256_file(save_path + ".lua"),
        "incoming_save.jpg": _sha256_file(os.path.join(base, "world.jpg")),
    }

    ok = True
    t0 = time.time()
    lossy_socks = []
    try:
        # Establish the lobby with NO loss first: the base lobby sends 'join'
        # only once (roster/chat self-heal, but join does not retransmit), so
        # dropping the handshake/join is a separate pre-existing concern. We
        # switch loss ON only for the transfer, which is what we're validating.
        hsock = _LossySocket(open_socket(HP, socket.AF_INET), 0.0)
        lossy_socks.append(hsock)
        threading.Thread(target=run_host, name="xfer-host",
                         args=(hsock, names["host"], ios["host"]),
                         kwargs={"code": "XFERCODE", "stop": stop},
                         daemon=True).start()
        time.sleep(0.4)

        for port, key in ((P1, "j1"), (P2, "j2")):
            peer = {"candidates": {"public_v4": f"127.0.0.1:{HP}",
                                   "lan_v4": None, "v6": None},
                    "flags": {"open": True, "v6": False}}
            s = _LossySocket(open_socket(port, socket.AF_INET), 0.0)
            lossy_socks.append(s)
            conn = race(s, peer, "dial", port, 12, my_has_v6=False)
            if not conn:
                print(f"[xfer:{tag}] FAIL: joiner {names[key]} could not connect")
                return False
            conns.append(conn)
            threading.Thread(target=run_client, name=key,
                             args=(conn, names[key], ios[key]),
                             kwargs={"stop": stop}, daemon=True).start()

        # Wait until both joiners are in the roster (fully joined) before start.
        def all_joined():
            for k in names:
                r = _latest_roster(ios[k].out_path)
                if not r or len(r.get("players", [])) < 3:
                    return False
            return True

        if not _wait_until(all_joined, timeout=15):
            print(f"[xfer:{tag}] FAIL: not all three joined")
            return False

        # NOW turn on packet loss to exercise the transfer's retransmit path.
        for ls in lossy_socks:
            ls.loss = loss

        # Host issues the save-transfer start (the new command).
        with open(ios["host"].in_path, "a", encoding="utf-8") as f:
            f.write(json.dumps({"cmd": "start", "save": save_path}) + "\n")

        # Wait for the WHOLE handshake to settle: both joiners save_ready AND the
        # host recorded a per-peer done for both AND broadcast start. (save_ready
        # fires the instant a joiner assembles the file, which is BEFORE its
        # fdone reaches the host -- so we must not check host state on save_ready
        # alone, or we'd race the in-flight fdone.)
        def transfer_settled():
            for k in ("j1", "j2"):
                if not any(e.get("type") == "save_ready"
                           for e in _read_events(ios[k].out_path)):
                    return False
            hev = _read_events(ios["host"].out_path)
            done = sum(1 for e in hev if e.get("type") == "transfer"
                       and e.get("role") == "send" and e.get("state") == "done")
            started_ev = any(e.get("type") == "start" for e in hev)
            return done >= 2 and started_ev

        if not _wait_until(transfer_settled, timeout=120):
            print(f"[xfer:{tag}] FAIL: transfer did not settle in time")
            ok = False

        # (1) byte-identical files on each joiner
        for k in ("j1", "j2"):
            for fname, sha in src_sha.items():
                p = os.path.join(ios[k].dir, fname)
                if not os.path.isfile(p):
                    print(f"[xfer:{tag}] FAIL: {k} missing {fname}")
                    ok = False
                    continue
                if _sha256_file(p) != sha:
                    print(f"[xfer:{tag}] FAIL: {k} {fname} SHA mismatch")
                    ok = False

        # (2) save_ready shape check (dir + files list)
        for k in ("j1", "j2"):
            sr = [e for e in _read_events(ios[k].out_path)
                  if e.get("type") == "save_ready"]
            if sr and (sr[-1].get("name") != INCOMING_BASENAME
                       or "incoming_save.sav" not in sr[-1].get("files", [])):
                print(f"[xfer:{tag}] FAIL: {k} save_ready shape = {sr[-1]}")
                ok = False

        # (3) host per-peer done x2, and start AFTER both done events
        hev = _read_events(ios["host"].out_path)
        done_idx = [i for i, e in enumerate(hev)
                    if e.get("type") == "transfer" and e.get("role") == "send"
                    and e.get("state") == "done"]
        start_idx = [i for i, e in enumerate(hev) if e.get("type") == "start"]
        if len(done_idx) < 2:
            print(f"[xfer:{tag}] FAIL: host emitted {len(done_idx)} done (want 2)")
            ok = False
        if not start_idx:
            print(f"[xfer:{tag}] FAIL: host never broadcast start")
            ok = False
        if done_idx and start_idx and start_idx[-1] < max(done_idx):
            print(f"[xfer:{tag}] FAIL: start was broadcast BEFORE transfers done")
            ok = False

        # (4) both joiners actually started
        for k in ("j1", "j2"):
            if not any(e.get("type") == "start"
                       for e in _read_events(ios[k].out_path)):
                print(f"[xfer:{tag}] FAIL: {k} never received start")
                ok = False
    finally:
        stop.set()
        time.sleep(0.5)                           # let the host release its port
        for c in conns:
            try:
                c.close()
            except Exception:
                pass
        shutil.rmtree(base, ignore_errors=True)

    dt = time.time() - t0
    mb = size_bytes / (1024 * 1024)
    rate = mb / dt if dt else 0
    print(f"[xfer:{tag}] {'OK' if ok else 'FAIL'}  "
          f"({mb:.0f} MB, loss={loss:.0%}, {dt:.1f}s, ~{rate:.1f} MB/s to 2 peers)")
    return ok


def selftest_transfer():
    """Run the save-transfer self-test several times (clean + lossy) so it
    proves both correctness and non-flakiness, and exercises retransmit."""
    print("[selftest-transfer] reliable host -> 2-joiner save transfer")
    runs = [
        ("clean-1", 0.00, 20 * 1024 * 1024),
        ("clean-2", 0.00, 16 * 1024 * 1024),
        ("lossy-8pct", 0.08, 12 * 1024 * 1024),
        ("lossy-15pct", 0.15, 10 * 1024 * 1024),
    ]
    allok = True
    for tag, loss, size in runs:
        allok = _run_transfer_once(loss, size, tag) and allok
    print(f"[selftest-transfer] {'PASS' if allok else 'FAIL'}")
    return allok


# --------------------------------------------------------------------------- #
# CLI
# --------------------------------------------------------------------------- #
def main(argv=None):
    ap = argparse.ArgumentParser(description="netpunch N-player lobby")
    ap.add_argument("mode", nargs="?", choices=["host", "join"],
                    help="host a lobby or join one with a CODE")
    ap.add_argument("code", nargs="?", help="peer CODE (join mode)")
    ap.add_argument("--name", default="player", help="your username")
    ap.add_argument("--local-port", type=int, default=DEFAULT_PORT)
    ap.add_argument("--timeout", type=int, default=40,
                    help="seconds to keep dialing the host (join)")
    ap.add_argument("--io-dir", default=None,
                    help="directory for the lobby_*.json[l] files (default: cwd)")
    ap.add_argument("--selftest", action="store_true",
                    help="run the loopback 1-host + 2-joiner self-test and exit")
    ap.add_argument("--selftest-transfer", action="store_true",
                    help="run the reliable save-transfer self-test (clean + "
                         "lossy) and exit")
    args = ap.parse_args(argv)

    if args.selftest:
        return 0 if selftest() else 1
    if args.selftest_transfer:
        return 0 if selftest_transfer() else 1
    if args.mode == "host":
        return cmd_host(args)
    if args.mode == "join":
        if not args.code:
            ap.error("join requires a CODE argument")
        return cmd_join(args)
    ap.error("give a mode: host / join / --selftest")
    return 2


if __name__ == "__main__":
    sys.exit(main())
