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
    roster  {t:roster, players[sorted], host,
             started, start_save}                 host -> all (also the ~2 s heal).
                                                  ``started`` is PER PEER: true only
                                                  if THAT joiner was included in a
                                                  start, so a joiner that missed the
                                                  start burst catches up from the
                                                  heal; ``start_save`` mirrors the
                                                  start's save flag.
    chat    {t:chat, from, text, ts, cid}        host -> all (relayed + stamped)
    chat    {t:chat, text}                        joiner -> host (host stamps it)
    ping    {t:ping}                              joiner -> host (fast keepalive)
    start   {t:start, save}                       host -> all (save=true when a save
                                                  transfer completed for that peer
                                                  this session; false = legacy
                                                  no-save start)
    status  {t:status, state, detail}             host -> one joiner (advisory; e.g.
                                                  a late joiner learns the game has
                                                  already started)
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
      {"type":"start","save":true|false}
          save=true  -> load incoming_save.* (a save transfer completed);
          save=false -> legacy no-save start.
          A JOINER emits this ONLY when save==false, or save==true AND its
          receiver completed this session (it emitted save_ready). Any other
          start is logged and ignored WITHOUT latching 'started', so a later
          retried START GAME still works.
      {"type":"transfer","role":"send|recv",...}         (progress; per-peer on
          the host: "peer","pct" or "state":"done|failed|dropped")
      {"type":"save_ready","name":"incoming_save","dir":...,"files":[...]}
      {"type":"status","state":"failed",
       "detail":"save transfer failed for <names> -- press START GAME to retry"}
          (host, when ANY peer's transfer failed: the host does NOT broadcast
          start and clears the transfer so START GAME can be pressed again)
      {"type":"status","state":"connected",
       "detail":"game already started -- ask the host to press START GAME again"}
          (a LATE joiner -- one that joins after a start -- gets this instead
          of a start)
  lobby_state.json  a SINGLE JSON object, overwritten, mirroring the latest
      roster/state for easy polling:
      {"state","code","players","you","host","started"}
  lobby_in.jsonl    the menu APPENDS command lines; the lobby TAILS it (tracks a
      byte offset, processes only new whole lines) and acts on:
      {"cmd":"chat","text":"..."}     -> send CHAT
      {"cmd":"name","name":"..."}     -> change own username, re-JOIN/broadcast
      {"cmd":"start"}                 -> host broadcasts START save=false
                                         (no-op on a client)
      {"cmd":"start","save":<path>}   -> host pushes the save to every joiner,
                                         then broadcasts START save=true; with
                                         zero joiners it emits a status ('no
                                         players to share with') and does NOT
                                         start
      {"cmd":"quit"}                  -> leave cleanly

On startup both jsonl files are TRUNCATED so stale lines aren't reprocessed. A
JOINER also DELETES any stale incoming_save.sav / .sav.lua / .jpg in its io dir
before joining, so a previous session's save can never be mistaken for this one.

GAME RELAY (lockstep frames over the punched socket)
----------------------------------------------------
The lockstep bridge (tpf2_bridge_mp.dll) speaks plain point-to-point UDP to ONE
fixed peer address. To run it between two machines behind NAT with no new NAT
code, the menu points the bridge at 127.0.0.1:<game relay port> and the lobby
carries its frames over the socket it has ALREADY punched:

  bridge --UDP--> 127.0.0.1:P (this lobby) --NP1 DATA 'g'+frame--> peer lobby
         --UDP--> 127.0.0.1:L (the peer's bridge)

Enabled with ``--game-relay-port P --game-local-port L``:
  * a UDP socket is bound to 127.0.0.1:P; every datagram arriving there (from
    the local bridge) is forwarded over the lobby transport as a BINARY DATA
    payload whose first byte is ``GAME_RELAY_MAGIC`` (b'g' -- distinct from the
    '{' of JSON messages and the 'N' of CHUNK_MAGIC save chunks). The host
    forwards to every joiner; a joiner forwards to the host only.
  * any 'g' payload arriving from the transport is unwrapped and sent to
    127.0.0.1:L from that same socket.
  * frames over ``GAME_RELAY_MAX`` bytes are dropped with a stderr warning; a
    stats line (forwarded / delivered counts) goes to stderr every 10 s.
No ARQ here -- the bridge has its own. Without the flags nothing changes.
The menu passes P=7773 for HOST / 7774 for JOIN (distinct so two instances on
one machine can both run) and L = the port the bridge reported it bound.

CLI
---
    python lobby.py host --name <username>          # observe, print CODE=, serve
    python lobby.py join <CODE> --name <username>   # dial host, participate
    python lobby.py --selftest                      # 1 host + 2 joiners, loopback
    python lobby.py --selftest-transfer             # reliable save transfer
    python lobby.py --selftest-relay                # game relay both ways
Optional: --local-port 29471, --timeout 40, --io-dir <dir>,
          --game-relay-port <P> --game-local-port <L> (see GAME RELAY).
"""

from __future__ import annotations

import argparse
import collections
import hashlib
import json
import os
import re
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
    TYPE_DATA, TYPE_EDATA, _pack, _unpack, open_socket,
)
from seal import Sealer, derive_key, SECRET_LEN
# Reuse the code exchange + the connect race + observe/announce.
from connect import decode_code, race, _observe_and_announce, encode_profile, _targets_v4
from mesh import MeshNode

# --------------------------------------------------------------------------- #
# Tunables
# --------------------------------------------------------------------------- #
CAP = 8                 # max players in a lobby, INCLUDING the host
MAX_COMPANIES = 6       # company ids 1..6 (the engine-side companies mode's limit)
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
# The ONLY names a joiner will ever write. The sender proposes names in its
# `fbegin`; anything not on this list is refused outright rather than sanitised,
# because there is no legitimate reason for a different name to arrive.
ALLOWED_INCOMING = frozenset(INCOMING_BASENAME + sfx for sfx in (".sav", ".sav.lua", ".jpg"))


def _safe_incoming_name(name):
    """True only for one of the three exact basenames we ever write.

    Rejects absolute paths, directory components, traversal, and anything else:
    the check is a whitelist of the full name, not a filter applied to it.
    """
    if not isinstance(name, str) or name not in ALLOWED_INCOMING:
        return False
    # belt and braces: a whitelisted constant can never contain these, so this
    # only ever fires if ALLOWED_INCOMING itself is edited carelessly later.
    return (os.path.basename(name) == name
            and not os.path.isabs(name)
            and ".." not in name.split("/") and ".." not in name.split("\\"))
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


# Everything below /24 is masked out of the logs. These files get pasted into
# bug reports and screenshots, and a player's home IP has no business travelling
# with them: "[host] JOIN ('203.0.113.47', 51299)" identifies a person's house,
# while "203.0.113.x" still tells you which peer a line is about and whether two
# lines are the same peer. Set TPF2MP_LOG_IPS=1 when you genuinely need the full
# address to debug a NAT problem.
_IPV4_RE = re.compile(r"\b(\d{1,3}\.\d{1,3}\.\d{1,3})\.(\d{1,3})\b")
_SHOW_IPS = os.environ.get("TPF2MP_LOG_IPS", "") == "1"


def redact(text):
    """Mask the host part of every IPv4 address in ``text``.

    Loopback and the RFC1918 ranges are left alone -- 127.0.0.1 and 192.168.x.y
    identify nobody, and masking them makes local debugging harder for no gain.
    """
    if _SHOW_IPS:
        return text

    def mask(m):
        head, tail = m.group(1), m.group(2)
        if head.startswith(("127.", "10.", "192.168.")) or head.startswith("169.254."):
            return m.group(0)
        if head.startswith("172."):
            second = int(head.split(".")[1] or 0)
            if 16 <= second <= 31:
                return m.group(0)
        return head + ".x"

    return _IPV4_RE.sub(mask, str(text))


_log_sinks = []       # callables(line): the log forwarder taps every line


def _log(msg):
    """Diagnostics go to stderr; stdout is reserved for the single CODE= line.
    Every line is also offered to the registered sinks (see LogForwarder), so a
    joiner's lobby/mesh/relay diagnostics reach the host's merged log."""
    line = redact(msg)
    print(line, file=sys.stderr, flush=True)
    for sink in list(_log_sinks):
        try:
            sink(line)
        except Exception:          # noqa: BLE001 -- logging must never raise
            pass


# --------------------------------------------------------------------------- #
# Log forwarding: every joiner ships its log lines to the host, which merges
# them (tagged per peer) into <io-dir>/lobby_peers.log. Extra files can be
# tailed with --forward-log PATH (e.g. the bridge log) and ride the same way.
# --------------------------------------------------------------------------- #
PEERS_LOG_NAME = "lobby_peers.log"
LOG_BATCH_BYTES = 1000     # keep a 'log' message under one datagram
LOG_LINE_MAX = 240
LOG_FLUSH_INTERVAL = 0.5
LOG_QUEUE_MAX = 2000       # oldest lines are dropped beyond this (never blocks)
LOG_MSGS_PER_SEC = 8       # host: inbound 'log' messages accepted per peer per second
LOG_BYTES_PER_PEER = 20 * 1024 * 1024   # host: merged-log bytes accepted per peer per session


class LogForwarder:
    """Collects log lines (own + tailed files) for shipping in small batches."""

    def __init__(self, tail_paths=()):
        self.q = collections.deque(maxlen=LOG_QUEUE_MAX)
        self.tails = []                          # [path, tag, offset]
        for path in tail_paths:
            # a file that already exists is tailed from its END (this session's
            # lines only); one that appears later is read from its start
            try:
                start = os.path.getsize(path)
            except OSError:
                start = 0
            self.tails.append([path, os.path.basename(path), start])
        self.last_flush = 0.0
        self.dropped = 0
        self._lock = threading.Lock()

    def add(self, line, tag=None):
        if tag:
            line = f"[{tag}] {line}"
        if len(line) > LOG_LINE_MAX:
            line = line[:LOG_LINE_MAX - 3] + "..."
        with self._lock:
            if len(self.q) == self.q.maxlen:
                self.dropped += 1
            self.q.append(line)

    def poll_tails(self):
        """Read whatever appended to each tailed file since last time."""
        for t in self.tails:
            path, tag, off = t
            try:
                size = os.path.getsize(path)
            except OSError:
                continue
            if size < off:
                t[2] = 0                          # truncated/rotated: restart
                off = 0
            if size == off:
                continue
            try:
                with open(path, "rb") as f:
                    f.seek(off)
                    data = f.read(min(size - off, 64 * 1024))
            except OSError:
                continue
            t[2] = off + len(data)
            for raw in data.splitlines():
                line = raw.decode("utf-8", "replace").rstrip()
                if line:
                    self.add(line, tag)

    def drain(self, now):
        """A batch of lines to send now, or [] (rate-limited, size-capped)."""
        if now - self.last_flush < LOG_FLUSH_INTERVAL:
            return []
        self.last_flush = now
        out, size = [], 0
        with self._lock:
            if self.dropped:
                out.append(f"[log] {self.dropped} lines dropped (queue full)")
                self.dropped = 0
            while self.q and size + len(self.q[0]) + 4 <= LOG_BATCH_BYTES:
                line = self.q.popleft()
                out.append(line)
                size += len(line) + 4
        return out


class PeersLog:
    """The host's merged log: one file, every line tagged with who logged it."""

    def __init__(self, directory):
        self.path = os.path.join(directory, PEERS_LOG_NAME)
        self._lock = threading.Lock()
        with open(self.path, "w", encoding="utf-8") as f:
            f.write("# merged lobby log, started " + time.strftime("%Y-%m-%d %H:%M:%S") + "\n")

    def write(self, who, lines):
        stamp = time.strftime("%H:%M:%S")
        with self._lock:
            with open(self.path, "a", encoding="utf-8") as f:
                for line in lines:
                    f.write(stamp + " [" + who + "] " + line + "\n")


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


# --------------------------------------------------------------------------- #
# Sealing: when the lobby code carried a session secret, every DATA frame this
# process sends is encrypted + authenticated (seal.py) and goes out as 'E'.
# Plain 'D' frames are then refused, except the host's plain "wrong password"
# reject so a joiner learns why it is being ignored.
# --------------------------------------------------------------------------- #
SEAL = [None]               # the process-wide seal.Sealer, or None (plaintext)
REJECT_PLAIN_EVERY = 2.0    # host: rate limit for the plain reject per address


def _pack_data(payload):
    if SEAL[0] is not None:
        return _pack(TYPE_EDATA, SEAL[0].seal(payload))
    return _pack(TYPE_DATA, payload)


def _send_data(sock, addr, msg):
    """Wrap a lobby message dict in an ``NP1:`` DATA frame and fire it at addr."""
    try:
        sock.sendto(_pack_data(json.dumps(msg).encode("utf-8")), addr)
    except OSError:
        # Windows spits ICMP-port-unreachable back as an exception when a peer
        # has gone away; the drop-timer will evict it. Ignore.
        pass


# --------------------------------------------------------------------------- #
# Game relay: carry the lockstep bridge's UDP frames over the punched socket
# --------------------------------------------------------------------------- #
GAME_RELAY_MAGIC = b"g"     # first byte of a relayed-frame DATA payload. JSON
                            # lobby messages start with '{' and save chunks with
                            # CHUNK_MAGIC ('N'), so one byte tells them apart.
GAME_RELAY_MAX = 1400       # largest bridge frame we relay (bytes). The bridge's
                            # Packet is ~1.05 KB; NP1(5)+'g'(1)+1400 stays under
                            # the 1500 MTU. Bigger frames are dropped + warned.
GAME_RELAY_DRAIN = 256      # loopback datagrams drained per ready cycle.
GAME_RELAY_STATS = 10.0     # seconds between stderr stats lines.
GAME_RELAY_WARN_EVERY = 5.0 # rate limit for the oversize warning.
GAME_LOCAL_PORT_DEFAULT = 7771   # the bridge's port if the menu passes none.

# --------------------------------------------------------------------------- #
# Mesh: joiners punch each other on their ONE observed socket (mesh.py) and
# fan their bridge frames out DIRECTLY; a pair with no direct path sends an
# envelope through a relay -- the host, or any peer that has a direct link to
# the destination. Exactly one copy reaches every participant either way.
# --------------------------------------------------------------------------- #
MESH_RELAY_MAGIC = b"r"     # 'r' + len(to) + to + len(frm) + frm + payload
MESH_LINKS_INTERVAL = 2.0   # joiner -> host: report my direct links this often
MESH_HI_INTERVAL = 0.5      # re-send mesh_hi on an unnamed connected link


def _relay_wrap(to, frm, payload):
    tb, fb = to.encode("utf-8"), frm.encode("utf-8")
    return (MESH_RELAY_MAGIC + bytes([len(tb)]) + tb + bytes([len(fb)]) + fb
            + payload)


def _relay_unwrap(data):
    """-> (to, frm, payload) or None."""
    try:
        i = 1
        lt = data[i]; i += 1
        to = data[i:i + lt].decode("utf-8"); i += lt
        lf = data[i]; i += 1
        frm = data[i:i + lf].decode("utf-8"); i += lf
        return to, frm, data[i:]
    except (IndexError, UnicodeDecodeError):
        return None


class _MeshHostLink:
    """A punch.Connection-shaped view of the HOST link inside a MeshNode, so
    run_client's host handling is unchanged. Traffic from any OTHER address is
    handed to ``on_peer(addr, payload)`` (set by run_client)."""

    def __init__(self, mesh, host_addr):
        self.mesh = mesh
        self.peer = host_addr
        self.sock = mesh.sock
        self.on_peer = lambda _a, _p: None

    @property
    def peer_str(self):
        return f"{self.peer[0]}:{self.peer[1]}"

    def send(self, data):
        if not self.mesh.send(self.peer, data):
            raise RuntimeError("host link send failed")

    def recv(self, timeout=None):
        end = None if timeout is None else time.time() + timeout
        while True:
            rem = None if end is None else max(0.0, end - time.time())
            r = self.mesh.recv(rem)
            if r is None:
                return None
            addr, payload = r
            if addr == self.peer:
                return payload
            try:
                self.on_peer(addr, payload)
            except Exception as e:                       # never die on one msg
                _log(f"[mesh] peer message error: {e!r}")
            if end is not None and time.time() >= end:
                return None

    def last_seen_age(self):
        return self.mesh.last_seen_age(self.peer)

    def close(self):
        self.mesh.close()


def _mesh_from_conn(conn, log=_log):
    """Take the connect race's socket away from its single-peer Connection
    and put a MeshNode on it, with the host link adopted as connected."""
    conn._stop.set()                       # stop the reader without closing
    if conn._thread.is_alive():
        conn._thread.join(timeout=1.0)
    mesh = MeshNode(conn.sock, log=log, name="joiner", cipher=getattr(conn, "cipher", None))
    mesh.adopt(conn.peer, "host", connected=True)
    return mesh, _MeshHostLink(mesh, conn.peer)


def _open_loopback_udp(port):
    """A non-blocking UDP socket bound to 127.0.0.1:``port`` -- loopback ONLY,
    the relay must never be reachable from the network. Windows' UDP
    'connection reset' on ICMP-unreachable is switched off where available so
    a bridge that isn't listening yet can't poison later recvs."""
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    if hasattr(socket, "SIO_UDP_CONNRESET"):  # Windows only
        try:
            s.ioctl(socket.SIO_UDP_CONNRESET, struct.pack("I", 0))
        except OSError:
            pass
    s.bind(("127.0.0.1", port))
    s.setblocking(False)
    _boost_socket_buffers(s)
    return s


class GameRelay:
    """Loopback <-> lobby-transport relay for the bridge's lockstep frames.

    Owns ONE UDP socket bound to 127.0.0.1:``relay_port``. Whoever runs the
    lobby loop (host or client) does two things with it:

      * ``pump_outbound(forward)`` -- drain frames the LOCAL bridge sent to the
        relay port and hand each, prefixed with GAME_RELAY_MAGIC, to
        ``forward(payload)`` which puts it on the transport (host: to every
        joiner; joiner: to the host). ``forward`` returns how many peers it
        reached.
      * ``deliver(payload)`` -- a 'g' payload came in from the transport: strip
        the tag and send the frame to the local bridge at 127.0.0.1:``local_port``
        (from this same socket, so the bridge sees one stable source).

    ``tick(now)`` prints a stats line every GAME_RELAY_STATS seconds. The host
    folds ``sock`` into its select(); the client (whose transport is a
    queue-fed ``Connection``, not a select loop) runs ``pump_loop`` on a
    thread. No ARQ: the bridge has its own.
    """

    def __init__(self, relay_port, local_port, log=_log):
        self.relay_port = int(relay_port)
        self.local_addr = ("127.0.0.1", int(local_port))
        self.log = log
        self.sock = _open_loopback_udp(self.relay_port)
        self.forwarded = 0          # loopback -> transport (frames)
        self.delivered = 0          # transport -> loopback (frames)
        self.no_peer = 0            # local frames with nobody to send to
        self.oversize = 0           # local frames dropped for size
        self._last_warn = 0.0
        self._last_stats = time.time()
        self._last_counts = (0, 0)
        self.log(f"[relay] game relay up: bridge -> 127.0.0.1:{self.relay_port} "
                 f"-> transport -> peer; peer -> 127.0.0.1:{self.local_addr[1]}")

    @staticmethod
    def is_game(payload):
        return payload[:1] == GAME_RELAY_MAGIC

    def pump_outbound(self, forward):
        """Drain up to GAME_RELAY_DRAIN local frames; returns how many were read."""
        n = 0
        while n < GAME_RELAY_DRAIN:
            try:
                data, _src = self.sock.recvfrom(65535)
            except BlockingIOError:
                break
            except ConnectionResetError:
                continue                     # Windows ICMP echo of our own send
            except OSError:
                break
            n += 1
            if len(data) > GAME_RELAY_MAX:
                self.oversize += 1
                now = time.time()
                if now - self._last_warn >= GAME_RELAY_WARN_EVERY:
                    self._last_warn = now
                    self.log(f"[relay] WARNING: dropped {len(data)}-byte frame "
                             f"from the bridge (cap {GAME_RELAY_MAX} bytes; "
                             f"{self.oversize} dropped so far)")
                continue
            try:
                reached = forward(GAME_RELAY_MAGIC + data)
            except (RuntimeError, OSError):
                reached = 0
            if reached:
                self.forwarded += 1
            else:
                self.no_peer += 1
        return n

    def deliver(self, payload):
        """A 'g' payload from the transport -> the local bridge."""
        try:
            self.sock.sendto(payload[1:], self.local_addr)
        except OSError:
            return
        self.delivered += 1

    def tick(self, now):
        if now - self._last_stats < GAME_RELAY_STATS:
            return
        dt = now - self._last_stats
        self._last_stats = now
        f0, d0 = self._last_counts
        self._last_counts = (self.forwarded, self.delivered)
        self.log(f"[relay] stats: forwarded={self.forwarded} "
                 f"(+{self.forwarded - f0}) delivered={self.delivered} "
                 f"(+{self.delivered - d0}) in {dt:.0f}s; "
                 f"no_peer={self.no_peer} oversize={self.oversize}")

    def pump_loop(self, forward, stop):
        """Client-side pump: select() on the loopback socket until ``stop``."""
        while not stop.is_set():
            try:
                ready, _, _ = select.select([self.sock], [], [], 0.2)
            except (OSError, ValueError):
                break
            if ready:
                self.pump_outbound(forward)

    def close(self):
        try:
            self.sock.close()
        except OSError:
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
            self.sock.sendto(_pack_data(frame), addr)
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
        elif base < p["base"]:
            # REWIND: the receiver restarted from scratch (hash mismatch ->
            # whole-file re-request). Without this the host would filter every
            # re-reported hole as "below base" and never resend -- the peer
            # would just sit there until PEER_XFER_TIMEOUT. Honour the new base,
            # re-stream from it, and drop the now-meaningless pending holes.
            self.log(f"[host] {p['name']} rewound save cursor "
                     f"{p['base']} -> {base} (receiver re-requested the file)")
            p["base"] = base
            p["next"] = base
            p["nack"] = []
            p["last_advance"] = now
            p["last_pct"] = -1
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
                # Per-peer outcome is a 'transfer' event (like "done"); the
                # single failed STATUS comes from the host loop once every
                # peer has resolved, naming all failures.
                self.io.emit({"type": "transfer", "role": "send",
                              "peer": p["name"], "state": "failed",
                              "detail": "could not verify the file"})
                self.log(f"[host] {p['name']} FAILED save (receiver gave up)")
        # A non-final ok:false means the receiver is retrying -> keep serving.

    def on_peer_dropped(self, addr):
        p = self.peers.get(addr)
        if p and p["state"] == "active":
            p["state"] = "dropped"
            self.io.emit({"type": "transfer", "role": "send",
                          "peer": p["name"], "state": "dropped",
                          "detail": "dropped mid-transfer"})
            self.log(f"[host] {p['name']} dropped mid-transfer -- skipping")

    # -- the pump (one slice of work per peer) ----------------------------- #
    def pump(self, now):
        for addr, p in self.peers.items():
            if p["state"] != "active":
                continue
            if now - p["last_advance"] > PEER_XFER_TIMEOUT:
                p["state"] = "failed"
                self.io.emit({"type": "transfer", "role": "send",
                              "peer": p["name"], "state": "failed",
                              "detail": "timed out"})
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

    def failed_names(self):
        """Names of peers whose transfer FAILED (verify/timeout). 'dropped'
        peers have already left the lobby and never block a start."""
        return [p["name"] for p in self.peers.values()
                if p["state"] == "failed"]

    def done_count(self):
        return sum(1 for p in self.peers.values() if p["state"] == "done")


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

    def _fail(self, detail):
        """Give up on this session: tell the menu AND the host (fdone ok:false
        final) so the host resolves us as failed NOW rather than after
        PEER_XFER_TIMEOUT."""
        self.failed = True
        self.io.emit({"type": "status", "state": "failed",
                      "detail": f"save transfer failed: {detail}"})
        self._send({"t": "fdone", "sid": self.sid, "ok": False, "final": True})

    # -- inbound ----------------------------------------------------------- #
    def on_begin(self, msg):
        sid = msg.get("sid")
        if sid == self.sid:
            if self.failed:
                # The host missed our final fdone and is still re-sending
                # fbegin: repeat the verdict (never re-ack, or it would start
                # streaming at a receiver that can't take the file).
                self._send({"t": "fdone", "sid": sid, "ok": False,
                            "final": True})
            else:
                self._send({"t": "fbegin_ack", "sid": sid})  # duplicate -> re-ack
            return
        # A brand-new session (first ever, or a later transfer): (re)allocate.
        self.sid = sid
        self.total_bytes = int(msg.get("total_bytes", 0))
        self.chunk = int(msg.get("chunk", CHUNK_DATA)) or CHUNK_DATA
        self.total_chunks = int(msg.get("total_chunks", 0))
        self.files = msg.get("files", [])
        # Validate the proposed names BEFORE allocating or acking: a rejected
        # transfer must cost the joiner nothing.
        bad = [m.get("name") for m in (self.files or [])
               if not _safe_incoming_name(m.get("name"))]
        if bad:
            self._fail(f"refused: sender proposed unexpected filename(s) {bad}")
            return
        self.overall_sha = msg.get("sha256")
        self.buf = None
        self.have = None
        self.complete = False
        try:
            self.buf = bytearray(self.total_bytes)
            # inside the SAME guard: a host-declared huge total_chunks used to
            # crash the joiner here instead of failing cleanly through _fail
            self.have = bytearray(self.total_chunks)
        except (MemoryError, OverflowError):
            self._fail(f"cannot allocate {self.total_bytes} bytes "
                       f"/ {self.total_chunks} chunks")
            return
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
        # bytearray slice-assignment GROWS the buffer when the slice runs past
        # the end, so an oversized host-controlled chunk silently changed
        # total_bytes out from under the hash check. Refuse instead.
        if off < 0 or off + len(data) > self.total_bytes:
            self._fail(f"refused: chunk {seq} would write "
                       f"{off}..{off + len(data)} past {self.total_bytes}")
            return
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
            self._fail("hash mismatch")
            return
        written = []
        try:
            for meta in self.files:
                name = meta.get("name")
                if not _safe_incoming_name(name):
                    self._fail(f"refused: unexpected filename {name!r}")
                    return
                with open(os.path.join(self.io.dir, name), "wb") as f:
                    f.write(parts[name])
                written.append(name)
        except OSError as e:
            self._fail(f"write error: {e}")
            return
        self.complete = True
        self.io.emit({"type": "transfer", "role": "recv", "pct": 100})
        self.io.emit({"type": "save_ready", "name": INCOMING_BASENAME,
                      "dir": os.path.abspath(self.io.dir), "files": written})
        self.log(f"[client] save ready: {written} in {self.io.dir}")
        self._maybe_send_done(force=True)


def _clear_stale_incoming(directory, log=_log):
    """Delete a previous session's incoming_save.* from ``directory``.

    A joiner runs this BEFORE joining: a stale file from an earlier lobby must
    never be picked up as this session's save (the DLL loads incoming_save.*
    on a start with save=true, and we only emit that after save_ready).
    """
    for suffix in (".sav", ".sav.lua", ".jpg"):
        path = os.path.join(directory, INCOMING_BASENAME + suffix)
        try:
            os.remove(path)
            log(f"[client] removed stale {path}")
        except FileNotFoundError:
            pass
        except OSError as e:
            log(f"[client] WARNING: could not remove stale {path}: {e}")


# --------------------------------------------------------------------------- #
# HOST: single socket, N peers, authority for roster + chat relay
# --------------------------------------------------------------------------- #
def run_host(sock, my_name, io, code=None, stop=None, drop_after=DROP_AFTER,
             log=_log, relay=None, forward_logs=()):
    """Run the lobby server forever on ``sock`` (blocks until ``stop`` is set).

    ``sock`` is a bound UDP socket (the observe/game socket for the real CLI, a
    plain loopback socket for the self-test). ``io`` is a :class:`LobbyIO`.
    ``relay`` is an optional :class:`GameRelay`: local bridge frames fan out
    to every joiner, joiners' 'g' frames go to the local bridge.
    """
    stop = stop or threading.Event()
    sock.setblocking(False)
    _boost_socket_buffers(sock)             # help bursty save-transfer traffic

    host_name = _dedupe(my_name, set())     # reassigned by the 'name' command
    peers = collections.OrderedDict()       # addr -> {"name":str, "last":float,
                                            #          "started":bool,
                                            #          "profile":code|None,
                                            #          "links":[names],
                                            #          "company":1..6}
    host_company = [1]                      # the host's own company id
    cid_counter = [0]                       # host-authoritative chat id
    started = [False]
    start_save = [False]                    # save flag of the last broadcast start
    last_emitted_roster = [None]
    transfer = [None]                       # the active _HostSaveTransfer, or None

    if code:
        io.emit({"type": "code", "code": code})

    # merged log: our own lines + every joiner's, tagged; extra files tailed
    peers_log = PeersLog(io.dir)
    own_fwd = LogForwarder(forward_logs)
    _log_sinks.append(own_fwd.add)
    log(f"[host] merged log -> {peers_log.path}")

    # ---- roster helpers ---------------------------------------------------- #
    def all_names(exclude_addr=None):
        names = {host_name}
        for a, p in peers.items():
            if a != exclude_addr:
                names.add(p["name"])
        return names

    def roster_players():
        return sorted([host_name] + [p["name"] for p in peers.values()])

    def roster_companies():
        """name -> company id. Same id = same company (co-op); different ids =
        separate companies. Everyone starts on 1, so nothing changes until
        someone clicks a chip."""
        m = {host_name: host_company[0]}
        for p in peers.values():
            m[p["name"]] = int(p.get("company", 1))
        return m

    def set_company(name, cid):
        try:
            cid = int(cid)
        except (TypeError, ValueError):
            return False
        if not 1 <= cid <= MAX_COMPANIES:
            return False
        if name == host_name:
            host_company[0] = cid
            return True
        for p in peers.values():
            if p["name"] == name:
                p["company"] = cid
                return True
        return False

    def send_roster_packets():
        # Doubles as the start self-heal: a joiner that lost the whole start
        # burst sees started:true here (~2 s later) and starts. The flag is
        # PER PEER so a late joiner (not included in the start) never starts
        # off a heal -- it needs the host to press START GAME again.
        players = roster_players()
        profiles = {p["name"]: p["profile"] for p in peers.values()
                    if p.get("profile")}
        links = {p["name"]: p.get("links", []) for p in peers.values()}
        companies = roster_companies()
        for a, p in list(peers.items()):
            _send_data(sock, a, {"t": "roster", "players": players,
                                 "host": host_name,
                                 "started": bool(p.get("started")),
                                 "start_save": start_save[0],
                                 "profiles": profiles, "links": links,
                                 "companies": companies})

    def emit_roster():
        players = roster_players()
        io.emit({"type": "roster", "players": players,
                 "you": host_name, "host": host_name,
                 "companies": roster_companies()})
        io.write_state(state="connected", code=code, players=players,
                       you=host_name, host=host_name, started=started[0])

    def roster_changed(broadcast=True):
        """Push the roster to peers, and emit an event only if it changed."""
        if broadcast:
            send_roster_packets()
        key = (tuple(roster_players()), tuple(sorted(roster_companies().items())))
        if last_emitted_roster[0] != key:
            last_emitted_roster[0] = key
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

    def broadcast_start(save):
        """Start everyone currently in the lobby. ``save`` is True when a save
        transfer just completed for every peer (they load incoming_save.*),
        False for a legacy no-save start."""
        started[0] = True
        start_save[0] = bool(save)
        msg = {"t": "start", "save": start_save[0]}
        for a in list(peers):
            peers[a]["started"] = True      # heal roster carries started:true
        for _ in range(CHAT_BURST):
            for a in list(peers):
                _send_data(sock, a, msg)
        io.emit({"type": "start", "save": start_save[0]})
        io.write_state(started=True)
        log(f"[host] START broadcast (save={start_save[0]}) to "
            f"{len(peers)} peer(s)")

    # ---- inbound lobby messages -------------------------------------------- #
    def do_join(addr, name, profile=None, is_mesh=False):
        late = False
        if addr in peers:                                   # rename in place
            peers[addr]["name"] = _dedupe(name, all_names(exclude_addr=addr))
            if profile:
                peers[addr]["profile"] = profile
            peers[addr]["mesh"] = bool(is_mesh)
        else:                                               # brand-new joiner
            if len(peers) + 1 >= CAP:
                _send_data(sock, addr, {"t": "reject", "reason": "lobby full"})
                log(f"[host] rejected {addr} (lobby full)")
                return
            assigned = _dedupe(name, all_names())
            peers[addr] = {"name": assigned, "last": time.time(),
                           "started": False, "profile": profile,
                           "links": [], "mesh": bool(is_mesh), "company": 1}
            late = started[0]
            log(f"[host] JOIN {addr} as {assigned!r}"
                + (" (late -- game already started)" if late else ""))
        peers[addr]["last"] = time.time()
        _send_data(sock, addr, {"t": "welcome",
                                "you": peers[addr]["name"], "host": host_name})
        roster_changed()
        if late:
            # A late joiner is NOT started: it has no save (a save start) and
            # nobody is in the lobby to sync with. Tell it why; the host has to
            # press START GAME again to bring it in.
            _send_data(sock, addr, {"t": "status", "state": "connected",
                                    "detail": "game already started -- ask the "
                                              "host to press START GAME again"})

    def relay_forward(payload):
        """Host side of the game relay: a local bridge frame -> every joiner.
        Returns how many joiners it was sent to."""
        frame = _pack_data(payload)
        n = 0
        for a in list(peers):
            try:
                sock.sendto(frame, a)
                n += 1
            except OSError:
                pass
        return n

    def handle_data(addr, payload):
        if payload[:1] == MESH_RELAY_MAGIC:
            # A relay envelope: for us -> deliver the inner frame; for another
            # joiner -> forward verbatim (the host is the default relay).
            if addr not in peers:
                return
            peers[addr]["last"] = time.time()
            env = _relay_unwrap(payload)
            if env is None:
                return
            to, _frm, inner = env
            if to == host_name:
                if GameRelay.is_game(inner) and relay is not None:
                    relay.deliver(inner)
                return
            for a, p in peers.items():
                if p["name"] == to:
                    # a mesh joiner unwraps envelopes itself; a legacy (star)
                    # joiner only understands plain frames
                    out = payload if p.get("mesh") else inner
                    try:
                        sock.sendto(_pack_data(out), a)
                    except OSError:
                        pass
                    return
            return
        if GameRelay.is_game(payload):
            # A joiner's bridge frame (binary, never JSON): straight to our
            # bridge. Only from a peer that has joined -- strays are dropped.
            if addr in peers:
                peers[addr]["last"] = time.time()
                if relay is not None:
                    relay.deliver(payload)
                if not peers[addr].get("mesh"):
                    # legacy star joiner: it cannot reach the others itself
                    frame = _pack_data(payload)
                    for a in list(peers):
                        if a != addr:
                            try:
                                sock.sendto(frame, a)
                            except OSError:
                                pass
            return
        try:
            msg = json.loads(payload.decode("utf-8"))
        except (ValueError, UnicodeDecodeError):
            return
        t = msg.get("t")
        if addr in peers:
            peers[addr]["last"] = time.time()
        if t == "join":
            do_join(addr, msg.get("name", "player"), msg.get("profile"),
                    msg.get("mesh", False))
        elif t == "links":
            if addr in peers:
                new = [str(x) for x in msg.get("direct", [])][:CAP]
                if new != peers[addr].get("links"):
                    peers[addr]["links"] = new
                    send_roster_packets()          # let everyone re-plan relays
        elif t == "company":
            # a joiner may set ITS OWN company; only the host sets anyone's
            if addr in peers:
                target = str(msg.get("player") or peers[addr]["name"])
                if target == peers[addr]["name"] and set_company(target, msg.get("id")):
                    log(f"[host] {target} -> company {msg.get('id')}")
                    roster_changed()
        elif t == "mesh_hi":
            pass                                    # names are host-assigned
        elif t == "log":
            # Untrusted input from a peer, written to our disk: cap the volume
            # per peer (bytes per session, messages per second) and strip
            # control characters so a line cannot forge another peer's tag or
            # fill the drive.
            if addr in peers:
                p = peers[addr]
                nowt = time.time()
                if nowt - p.get("log_win", 0.0) >= 1.0:
                    p["log_win"], p["log_n"] = nowt, 0
                p["log_n"] = p.get("log_n", 0) + 1
                if p["log_n"] > LOG_MSGS_PER_SEC or p.get("log_bytes", 0) > LOG_BYTES_PER_PEER:
                    return
                lines = []
                for x in msg.get("lines", [])[:64]:
                    x = "".join(ch if ch >= " " else " " for ch in str(x))[:LOG_LINE_MAX]
                    lines.append(x)
                    p["log_bytes"] = p.get("log_bytes", 0) + len(x) + 1
                if lines:
                    peers_log.write(p["name"], lines)
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
        Once every peer has verified (or dropped), the loop broadcasts
        'start' with save=true. If any peer FAILED, or on a read error, we emit
        a failed status and do NOT start (START GAME can be pressed again).
        """
        if transfer[0] is not None:
            log("[host] start(save) ignored -- a transfer is already running")
            return
        if not peers:
            # Nobody to share the save with: do NOT latch 'started' (that would
            # turn every subsequent joiner into a 'late' one). Just say so.
            log("[host] start(save): no joiners connected -- not starting")
            io.emit({"type": "status", "state": "connected",
                     "detail": "no players to share with -- wait for a player "
                               "to join, then press START GAME"})
            return
        try:
            blob, files_meta = _read_save_files(save_path)
        except (OSError, ValueError) as e:
            io.emit({"type": "status", "state": "failed",
                     "detail": f"save transfer failed: {e}"})
            log(f"[host] save read failed: {e}")
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
        elif c == "company":
            target = str(cmd.get("player") or host_name)
            if set_company(target, cmd.get("id")):
                log(f"[host] {target} -> company {cmd.get('id')} (set by host)")
                roster_changed()
        elif c == "start":
            save = cmd.get("save")
            if transfer[0] is not None:
                log("[host] start ignored -- a save transfer is in progress")
            elif save:
                begin_save_transfer(save)         # start(save=True) when done
            else:
                broadcast_start(save=False)       # legacy start, no transfer
        elif c == "quit":
            stop.set()

    # ---- serve ------------------------------------------------------------- #
    io.emit({"type": "status", "state": "connected",
             "detail": f"lobby ready on {sock.getsockname()[1]}"})
    emit_roster()                                           # initial: just host
    log(f"[host] serving as {host_name!r} on udp/{sock.getsockname()[1]}")

    last_heal = last_drop = 0.0
    reject_sent = {}                        # addr -> when we last sent a plain reject
    # The game relay's loopback socket joins the select set so a bridge frame
    # wakes the loop immediately (lockstep latency) instead of on the next tick.
    rlist = [sock] if relay is None else [sock, relay.sock]
    try:
        while not stop.is_set():
            # While a transfer runs, poll fast so we pump chunks + absorb ACKs
            # promptly; otherwise idle at 0.2 s to keep the loop cheap.
            timeout = XFER_SELECT_TIMEOUT if transfer[0] is not None else 0.2
            try:
                ready, _, _ = select.select(rlist, [], [], timeout)
            except (OSError, ValueError):
                break
            now = time.time()

            # Drain up to HOST_DRAIN datagrams this cycle -- a busy transfer can
            # deliver a burst of facks/pings, and one-per-iteration would let the
            # kernel recv buffer overflow (self-inflicted loss).
            if sock in ready:
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
                    elif ptype == TYPE_EDATA:
                        if SEAL[0] is None:
                            continue                        # we run plaintext
                        plain = SEAL[0].open(payload)
                        if plain is not None:
                            handle_data(addr, plain)
                        elif addr not in peers and now - reject_sent.get(addr, 0.0) >= REJECT_PLAIN_EVERY:
                            # wrong password (or wrong code): say so in the clear
                            reject_sent[addr] = now
                            try:
                                sock.sendto(_pack(TYPE_DATA, json.dumps(
                                    {"t": "reject", "reason": "wrong password"}).encode("utf-8")), addr)
                            except OSError:
                                pass
                    elif ptype == TYPE_DATA:
                        if SEAL[0] is None:
                            handle_data(addr, payload)      # plaintext session

            # Game relay: local bridge frames -> every joiner.
            if relay is not None and relay.sock in ready:
                relay.pump_outbound(relay_forward)

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

            if relay is not None:
                relay.tick(now)                             # 10 s stats line

            own_fwd.poll_tails()
            own_lines = own_fwd.drain(now)
            if own_lines:
                peers_log.write(host_name, own_lines)

            # Pump the save transfer (if any). Once every peer has resolved:
            #   all done (dropped peers don't block) -> start with save=true;
            #   any FAILED -> failed status naming them, NO start, and the
            #   transfer is cleared so START GAME can simply be pressed again.
            if transfer[0] is not None:
                try:
                    transfer[0].pump(now)
                    if transfer[0].all_resolved():
                        xfer, transfer[0] = transfer[0], None
                        failed = xfer.failed_names()
                        if failed:
                            detail = (f"save transfer failed for "
                                      f"{', '.join(failed)} -- press START "
                                      f"GAME to retry")
                            log(f"[host] {detail}")
                            io.emit({"type": "status", "state": "failed",
                                     "detail": detail})
                        elif xfer.done_count() == 0:
                            # Every target dropped mid-transfer: nobody holds
                            # the save, so (as with zero joiners) don't start.
                            log("[host] every peer dropped mid-transfer -- "
                                "not starting")
                            io.emit({"type": "status", "state": "connected",
                                     "detail": "no players to share with -- "
                                               "wait for a player to join, "
                                               "then press START GAME"})
                        else:
                            log("[host] all save transfers resolved -- "
                                "starting")
                            broadcast_start(save=True)
                except Exception as e:                     # never crash the lobby
                    log(f"[host] save transfer error: {e!r}")
                    io.emit({"type": "status", "state": "failed",
                             "detail": f"save transfer failed: {e}"})
                    transfer[0] = None
    except KeyboardInterrupt:
        pass
    finally:
        try:
            _log_sinks.remove(own_fwd.add)
        except ValueError:
            pass
        for a in list(peers):
            _send_data(sock, a, {"t": "bye"})
        io.emit({"type": "status", "state": "failed",
                 "detail": "lobby closed"})
        io.write_state(state="failed")
        if relay is not None:
            relay.close()
        try:
            sock.close()
        except OSError:
            pass


# --------------------------------------------------------------------------- #
# CLIENT: one Connection to the host; participate
# --------------------------------------------------------------------------- #
def run_client(conn, my_name, io, stop=None, host_gone_after=HOST_GONE_AFTER,
               log=_log, receiver_cls=_ClientSaveReceiver, relay=None,
               mesh=None, profile_code=None, forward_logs=()):
    """Participate in the lobby over a connected ``punch.Connection`` (blocks).

    ``receiver_cls`` is the save-receive implementation (the self-test swaps in
    a deliberately corrupting one to prove the failure path). ``relay`` is an
    optional :class:`GameRelay`: local bridge frames go to the host only, the
    host's 'g' frames go to the local bridge.
    """
    stop = stop or threading.Event()

    desired = [my_name]     # what we asked to be called
    assigned = [my_name]    # what the host actually named us (from 'welcome')
    started = [False]
    last_roster = [None]
    host_name = [None]
    participants = [[]]     # every player name, from the roster
    roster_links = [{}]     # name -> [names it has direct links to]
    roster_profiles = [{}]  # name -> profile code (how to punch it)
    last_hi = {}            # addr -> when we last sent mesh_hi on it
    last_links_report = [0.0]
    reported_links = [None]
    last_ignored_start = [None]     # (save, sid) of the last start we refused
    seen_cids = collections.deque(maxlen=512)
    seen_set = set()

    try:
        _boost_socket_buffers(conn.sock)    # help bursty save-transfer traffic
    except AttributeError:
        pass
    _clear_stale_incoming(io.dir, log)      # never trust a previous session's save
    receiver = receiver_cls(conn, io, log)  # save-transfer receive side
    fwd = LogForwarder(forward_logs)        # our log lines -> the host's merged log
    _log_sinks.append(fwd.add)

    io.emit({"type": "status", "state": "connected",
             # no address in the panel: a joiner's screen (or a stream of it)
             # must not show the host's IP. The redacted log still has it.
             "detail": "joined the host"})
    io.write_state(state="connected", you=desired[0], started=False)

    def send(msg):
        try:
            conn.send(json.dumps(msg).encode("utf-8"))
        except (RuntimeError, OSError):
            pass

    def apply_start(save, via):
        """The save-flag rule: emit start only if save==false, or save==true
        AND our receiver completed this session (it emitted save_ready). An
        unsatisfiable start is ignored WITHOUT latching started, so a retried
        START GAME (after the host re-sends the save) still works."""
        if started[0]:
            return
        save = bool(save)
        if save and not receiver.complete:
            key = (save, receiver.sid)
            if last_ignored_start[0] != key:      # log once per situation
                last_ignored_start[0] = key
                log(f"[client] start(save=True) via {via} ignored -- no "
                    f"verified save this session (receiver "
                    f"{'failed' if receiver.failed else 'incomplete'})")
            return
        started[0] = True
        io.emit({"type": "start", "save": save})
        io.write_state(started=True)
        log(f"[client] START (save={save}) via {via}")

    # ---- mesh: direct links to the other joiners, relay for the rest -------- #
    def mesh_plan_dials():
        """Dial every other joiner we have a profile for and no link to yet."""
        if mesh is None:
            return
        me = assigned[0]
        for nm, code in roster_profiles[0].items():
            if nm == me or nm == host_name[0] or not code:
                continue
            if mesh.by_name(nm) or mesh.dial_pending(nm) or mesh.dial_failed(nm):
                continue
            try:
                prof = decode_code(code)
            except ValueError:
                continue
            mesh.dial(nm, _targets_v4(prof))

    def mesh_send_hi(addr):
        try:
            mesh.send(addr, json.dumps({"t": "mesh_hi", "name": assigned[0]}).encode("utf-8"))
        except Exception:
            pass

    def mesh_housekeeping(now):
        if mesh is None:
            return
        # name freshly connected links (both sides say hi; duplicates harmless)
        for ln in list(mesh.links.values()):
            if ln.connected and ln.name is None and now - last_hi.get(ln.addr, 0.0) >= MESH_HI_INTERVAL:
                last_hi[ln.addr] = now
                mesh_send_hi(ln.addr)
        # a failed dial may succeed later (NAT state changes): retry per roster
        # heal by forgetting failures every 30 s
        for nm in list(mesh.dials):
            d = mesh.dials[nm]
            if d.failed and now - d.started > 30.0:
                mesh.forget_dial(nm)
        mesh_plan_dials()
        # tell the host who we reach directly (it goes into everyone's roster)
        if now - last_links_report[0] >= MESH_LINKS_INTERVAL:
            last_links_report[0] = now
            direct = [n for n in mesh.direct_names() if n != "host"]
            if direct != reported_links[0]:
                reported_links[0] = direct
                send({"t": "links", "direct": direct})

    def mesh_relay_via(dest):
        """Who forwards our envelope to ``dest``: the host if it is alive, else
        any direct peer that reports a direct link to ``dest``."""
        if conn.last_seen_age() < host_gone_after:
            return conn.peer
        for nm in mesh.direct_names():
            if dest in roster_links[0].get(nm, []):
                ln = mesh.by_name(nm)
                if ln:
                    return ln.addr
        return None

    def mesh_forward(payload):
        """A local bridge frame -> every OTHER participant exactly once:
        direct where we have a link, else one envelope via a relay."""
        me = assigned[0]
        n = 0
        for nm in participants[0]:
            if nm == me:
                continue
            if nm == host_name[0]:
                if mesh.send(conn.peer, payload):
                    n += 1
                continue
            if mesh.by_name(nm):
                if mesh.send(nm, payload):
                    n += 1
                continue
            via = mesh_relay_via(nm)
            if via is not None and mesh.send(via, _relay_wrap(nm, me, payload)):
                n += 1
        return n

    def handle_peer(addr, payload):
        """Traffic from a NON-host address: a direct peer's bridge frame, a
        relay envelope (for us, or to forward), or a mesh_hi."""
        if payload[:1] == MESH_RELAY_MAGIC:
            env = _relay_unwrap(payload)
            if env is None:
                return
            to, frm, inner = env
            if to == assigned[0]:
                if GameRelay.is_game(inner) and relay is not None:
                    relay.deliver(inner)
            elif mesh.by_name(to):
                mesh.send(to, payload)          # we are the relay for this pair
            return
        if GameRelay.is_game(payload):
            ln = mesh.link(addr)
            if ln is not None and ln.connected and relay is not None:
                relay.deliver(payload)
            return
        try:
            m = json.loads(payload.decode("utf-8"))
        except (ValueError, UnicodeDecodeError):
            return
        if m.get("t") == "mesh_hi":
            nm = str(m.get("name", ""))
            if nm and nm != assigned[0]:
                ln = mesh.link(addr)
                fresh = ln is None or ln.name != nm
                mesh.name_link(addr, nm)
                mesh.forget_dial(nm)
                if fresh:
                    mesh_send_hi(addr)          # make sure they can name us too
                    last_links_report[0] = 0.0  # report the new link promptly

    if mesh is not None and hasattr(conn, "on_peer"):
        conn.on_peer = handle_peer

    def handle_msg(raw):
        # Two binary payload kinds are NOT JSON: a relayed bridge frame (first
        # byte GAME_RELAY_MAGIC) and a save chunk (CHUNK_MAGIC); everything
        # else is a JSON lobby message.
        if GameRelay.is_game(raw):
            if relay is not None:
                relay.deliver(raw)
            return
        if raw[:1] == MESH_RELAY_MAGIC:
            if mesh is not None:
                handle_peer(conn.peer, raw)
            else:
                env = _relay_unwrap(raw)
                if env and env[0] == assigned[0] and GameRelay.is_game(env[2]) and relay is not None:
                    relay.deliver(env[2])
            return
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
            host_name[0] = m.get("host")
            io.write_state(you=assigned[0], host=m.get("host"))
            log(f"[client] host named us {assigned[0]!r}")
        elif t == "roster":
            players = m.get("players", [])
            host_name[0] = m.get("host", host_name[0])
            participants[0] = list(players)
            roster_links[0] = m.get("links", {}) or {}
            roster_profiles[0] = m.get("profiles", {}) or {}
            mesh_plan_dials()
            companies = m.get("companies", {}) or {}
            key = (tuple(players), tuple(sorted(companies.items())))
            if key != last_roster[0]:
                last_roster[0] = key
                io.emit({"type": "roster", "players": players,
                         "you": assigned[0], "host": m.get("host"),
                         "companies": companies})
                io.write_state(state="connected", players=players,
                               you=assigned[0], host=m.get("host"),
                               started=started[0])
            # Start self-heal: the host's roster carries started:true for us
            # once we were included in a start -- catches a lost start burst.
            if m.get("started") is True:
                apply_start(m.get("start_save", False), via="roster")
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
            apply_start(m.get("save", False), via="start")
        elif t == "status":
            # Advisory from the host (e.g. late joiner: game already started).
            io.emit({"type": "status",
                     "state": str(m.get("state", "connected")),
                     "detail": str(m.get("detail", ""))})
            log(f"[client] host says: {m.get('detail', '')}")
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
        elif c == "company":
            send({"t": "company", "player": assigned[0], "id": cmd.get("id")})
        elif c == "name":
            desired[0] = str(cmd.get("name", "player"))
            m2 = {"t": "join", "name": desired[0], "mesh": mesh is not None}
            if profile_code:
                m2["profile"] = profile_code
            send(m2)
        elif c == "start":
            log("[client] 'start' ignored -- only the host can start")
        elif c == "quit":
            send({"t": "leave"})
            io.emit({"type": "status", "state": "failed", "detail": "left lobby"})
            stop.set()

    # Game relay, outbound half: the client's transport is a queue-fed
    # Connection (no select loop to join), so the loopback socket gets its own
    # select() thread that wraps each local bridge frame and sends it to the
    # host. The inbound half rides the normal inbox path (handle_msg above).
    relay_stop = threading.Event()
    relay_thread = None
    if relay is not None:
        def relay_forward(payload):
            if mesh is not None:
                return mesh_forward(payload)
            conn.send(payload)          # RuntimeError (no peer yet) -> pump
            return 1

        relay_thread = threading.Thread(target=relay.pump_loop,
                                        name="game-relay",
                                        args=(relay_forward, relay_stop),
                                        daemon=True)
        relay_thread.start()

    join_msg = {"t": "join", "name": desired[0], "mesh": mesh is not None}
    if profile_code:
        join_msg["profile"] = profile_code
    send(join_msg)                                          # announce ourselves
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
            mesh_housekeeping(now)
            receiver.tick(now)                              # facks / fdone cadence
            if now - last_ping >= PING_INTERVAL:
                last_ping = now
                send({"t": "ping"})
            for cmd in io.poll_commands():
                handle_command(cmd)
            if relay is not None:
                relay.tick(now)                             # 10 s stats line
            fwd.poll_tails()
            lines = fwd.drain(now)
            if lines:
                send({"t": "log", "lines": lines})
    except KeyboardInterrupt:
        send({"t": "leave"})
    finally:
        lines = fwd.drain(time.time() + LOG_FLUSH_INTERVAL)   # last words
        if lines:
            send({"t": "log", "lines": lines})
        try:
            _log_sinks.remove(fwd.add)
        except ValueError:
            pass
        relay_stop.set()
        if relay_thread is not None:
            relay_thread.join(timeout=1.0)  # its select() wakes within 0.2 s
        if relay is not None:
            relay.close()
        conn.close()


# --------------------------------------------------------------------------- #
# CLI commands
# --------------------------------------------------------------------------- #
def _make_relay(args):
    """Build the :class:`GameRelay` from --game-relay-port / --game-local-port,
    or None when the menu didn't ask for one (legacy behaviour, unchanged).

    A bind failure does NOT kill the lobby -- roster/chat/save still work --
    but it is shouted on stderr because lockstep will not connect.
    """
    port = getattr(args, "game_relay_port", None)
    if not port:
        return None
    local = getattr(args, "game_local_port", None) or GAME_LOCAL_PORT_DEFAULT
    try:
        return GameRelay(port, local)
    except OSError as e:
        _log(f"[relay] WARNING: cannot bind game relay 127.0.0.1:{port}: {e} "
             f"-- bridge frames will NOT be relayed this session")
        return None


def cmd_host(args):
    io = LobbyIO(args.io_dir or os.getcwd())
    io.emit({"type": "status", "state": "waiting", "detail": "observing NAT"})
    io.write_state(state="waiting")
    relay = _make_relay(args)               # bind early so a clash shows up now
    # observe + print the single CODE= line (reused from connect.py). The code
    # carries a fresh session secret: whoever has the code can talk to us,
    # nobody else can read or inject; --password layers on top of it.
    secret = os.urandom(SECRET_LEN)
    SEAL[0] = Sealer(derive_key(secret, args.password or ""))
    sock, _profile, code = _observe_and_announce(args.local_port, secret=secret,
                                                 password=args.password or None)
    if args.password:
        _log("[host] the code is LOCKED: without the password it reveals nothing")
    else:
        _log("[host] the code is plain: anyone who sees it can read your address "
             "-- set a password to lock it")
    _log("[host] frames are sealed (session key from the code"
         + (" + password)" if args.password else ")"))
    try:
        run_host(sock, args.name, io, code=code, relay=relay,
                 forward_logs=args.forward_log or ())
    finally:
        try:
            from observe import upnp_unmap
            if upnp_unmap(args.local_port):
                _log(f"[host] UPnP mapping for udp/{args.local_port} removed")
        except Exception as e:                            # noqa: BLE001
            _log(f"[host] UPnP unmap skipped: {e}")
    return 0


def cmd_join(args):
    io = LobbyIO(args.io_dir or os.getcwd())
    io.emit({"type": "status", "state": "waiting", "detail": "dialing host"})
    io.write_state(state="waiting")
    try:
        peer = decode_code(args.code, password=args.password or None)
    except ValueError as e:
        io.emit({"type": "status", "state": "failed", "detail": f"bad code: {e}"})
        io.write_state(state="failed")
        _log(f"[join] bad code: {e}")
        return 2
    if peer.get("stale"):
        _log(f"[join] WARNING: code is {peer['age']}s old -- may be stale")
    if peer.get("secret"):
        SEAL[0] = Sealer(derive_key(peer["secret"], args.password or ""))
        _log("[join] frames are sealed (session key from the code"
             + (" + password)" if args.password else ")"))
    else:
        _log("[join] WARNING: this code carries no session secret -- the lobby "
             "is PLAINTEXT and unauthenticated (old host?)")
    relay = _make_relay(args)               # bind early so a clash shows up now
    # Star model: the host is open, so we simply DIAL its v4 candidates. This
    # reuses connect.py's race (role='dial', v4 only -> single socket).
    sock = open_socket(args.local_port, socket.AF_INET)
    # Mesh: learn our OWN public mapping on this same socket first (STUN, no
    # UPnP), so the host can hand the other joiners a code that punches us.
    profile_code = None
    if not getattr(args, "no_mesh", False):
        try:
            from observe import observe
            prof = observe(args.local_port, sock=sock, do_upnp=False)
            profile_code = encode_profile(prof)
            _log(f"[join] self-observed candidates={prof['candidates']} "
                 f"flags={prof['flags']}")
        except Exception as e:                            # noqa: BLE001
            _log(f"[join] self-observe failed: {e} -- peers will reach us via relay")
    conn = race(sock, peer, "dial", args.local_port, args.timeout,
                my_has_v6=False)
    if not conn:
        io.emit({"type": "status", "state": "failed",
                 "detail": "could not reach host"})
        io.write_state(state="failed")
        _log("[join] FAILED to reach host")
        if relay is not None:
            relay.close()
        return 1
    _log(f"[join] connected to host {conn.peer_str}")
    conn.cipher = SEAL[0]
    mesh = None
    if not getattr(args, "no_mesh", False):
        mesh, conn = _mesh_from_conn(conn)
        _log("[join] mesh: direct links to other joiners enabled")
    run_client(conn, args.name, io, relay=relay, mesh=mesh,
               profile_code=profile_code, forward_logs=args.forward_log or ())
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


def _has_start(path, save=None):
    """True if lobby_out.jsonl at ``path`` has a start event (optionally with
    the given ``save`` flag)."""
    return any(e.get("type") == "start"
               and (save is None or e.get("save") is save)
               for e in _read_events(path))


def _dial_loopback(port, host_port, timeout):
    """Dial a loopback host with the connect race; returns a Connection/None."""
    peer = {"candidates": {"public_v4": f"127.0.0.1:{host_port}",
                           "lan_v4": None, "v6": None},
            "flags": {"open": True, "v6": False}}
    s = open_socket(port, socket.AF_INET)
    return race(s, peer, "dial", port, timeout, my_has_v6=False)


def selftest():
    HP, P1, P2, P3 = 29520, 29521, 29522, 29523
    names = {"host": "alice", "j1": "bob", "j2": "carol"}
    late_name = "dave"                        # joins AFTER the start (case d)
    expected = sorted(names.values())
    base = tempfile.mkdtemp(prefix="lobby_selftest_")
    dirs = {k: os.path.join(base, k) for k in list(names) + ["j3"]}
    ios = {k: LobbyIO(dirs[k]) for k in names}
    stop = threading.Event()
    conns = []
    print(f"[selftest] scratch dir: {base}")
    print(f"[selftest] host udp/{HP}  joiners udp/{P1},{P2}  late udp/{P3}")

    # A stale save from "a previous session" in j1's io dir: the joiner must
    # delete it before joining (case e).
    stale = os.path.join(dirs["j1"], INCOMING_BASENAME + ".sav")
    with open(stale, "wb") as f:
        f.write(b"stale save from an earlier lobby")

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

        # (a1) bob picks company 2: every roster carries companies {bob: 2}
        with open(ios["j1"].in_path, "a", encoding="utf-8") as f:
            f.write(json.dumps({"cmd": "company", "id": 2}) + "\n")
        def companies_ok():
            for k in names:
                r = _latest_roster(ios[k].out_path)
                if not r or (r.get("companies") or {}).get("bob") != 2:
                    return False
            return True
        if not _wait_until(companies_ok, timeout=8):
            ok = False
            print("[selftest] FAIL (a1): company choice did not reach every roster")
        else:
            print("[selftest] OK (a1): bob -> company 2 visible in every roster")

        # (a2) every joiner's log lines reach the host's merged log, tagged
        merged = os.path.join(dirs["host"], PEERS_LOG_NAME)
        def merged_has_all():
            try:
                txt = open(merged, encoding="utf-8").read()
            except OSError:
                return False
            return all(("[" + names[k] + "] ") in txt for k in ("j1", "j2", "host"))
        if not _wait_until(merged_has_all, timeout=8):
            ok = False
            print(f"[selftest] FAIL (a2): merged log lacks a peer's lines: {merged}")
        else:
            print("[selftest] OK (a2): host merged log has lines from bob, carol and itself")

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

        # (c) a legacy (no-save) START from the host -> everyone emits
        #     {"type":"start","save":false}
        with open(ios["host"].in_path, "a", encoding="utf-8") as f:
            f.write(json.dumps({"cmd": "start"}) + "\n")

        def started_everywhere():
            return all(_has_start(ios[k].out_path, save=False) for k in names)

        if not _wait_until(started_everywhere, timeout=12):
            ok = False
            for k in names:
                print(f"[selftest]   {k} start(save=false): "
                      f"{_has_start(ios[k].out_path, save=False)}")
            print("[selftest] FAIL (c): legacy start did not reach all three")
        else:
            print("[selftest] OK (c): legacy start save=false reached all three")

        # (d) a LATE joiner (after the start) must NOT be started -- not by a
        #     start message, and not by the periodic roster heal either -- and
        #     must be told why.
        ios["j3"] = LobbyIO(dirs["j3"])
        conn = _dial_loopback(P3, HP, 10)
        if not conn:
            print(f"[selftest] FAIL (d): late joiner {late_name} could not connect")
            ok = False
        else:
            conns.append(conn)
            threading.Thread(target=run_client, name="j3",
                             args=(conn, late_name, ios["j3"]),
                             kwargs={"stop": stop}, daemon=True).start()

            def late_notified():
                return any(e.get("type") == "status"
                           and "game already started" in e.get("detail", "")
                           for e in _read_events(ios["j3"].out_path))

            if not _wait_until(late_notified, timeout=12):
                ok = False
                print("[selftest] FAIL (d): late joiner never got the "
                      "'game already started' status")
            else:
                # Outlast a couple of roster heals to prove they don't start it.
                time.sleep(ROSTER_HEAL * 1.5)
                if _has_start(ios["j3"].out_path):
                    ok = False
                    print("[selftest] FAIL (d): late joiner was started")
                elif not (_latest_roster(ios["host"].out_path) and late_name in
                          _latest_roster(ios["host"].out_path)["players"]):
                    ok = False
                    print("[selftest] FAIL (d): late joiner not in host roster")
                else:
                    print("[selftest] OK (d): late joiner told 'game already "
                          "started', not started, in roster")

        # (e) the stale incoming_save.sav in j1's io dir was deleted on startup
        if os.path.exists(stale):
            ok = False
            print("[selftest] FAIL (e): stale incoming_save.sav survived join")
        else:
            print("[selftest] OK (e): stale incoming_save.sav removed before join")
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

        # (3) host per-peer done x2, and start(save=true) AFTER both done events
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
        elif hev[start_idx[-1]].get("save") is not True:
            print(f"[xfer:{tag}] FAIL: host start lacks save:true: "
                  f"{hev[start_idx[-1]]}")
            ok = False
        if done_idx and start_idx and start_idx[-1] < max(done_idx):
            print(f"[xfer:{tag}] FAIL: start was broadcast BEFORE transfers done")
            ok = False

        # (4) both joiners actually started, with save:true
        for k in ("j1", "j2"):
            if not _has_start(ios[k].out_path, save=True):
                print(f"[xfer:{tag}] FAIL: {k} never received start(save=true)")
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


def _run_transfer_failure(tag, size_bytes=4 * 1024 * 1024):
    # NB: size must exceed SEND_WINDOW chunks (~2.4 MB) so the host has SEEN
    # an advanced base before the receiver re-requests from 0 -- that is the
    # only way the rewind path is actually exercised.
    """One joiner's receiver FAILS (its copy of chunk 0 is corrupted, so every
    hash check fails until it gives up with fdone ok:false final); then the
    host is asked to START again with the corruption gone.

    Asserts, round 1: the failing joiner emits status failed and NO start; the
    healthy joiner (verified its copy) gets NO start either; the host emits
    the 'save transfer failed for <name> -- press START GAME to retry' status
    and NO start -- all well inside PEER_XFER_TIMEOUT, which proves the host
    honoured the receiver's base REWIND on each re-request.
    Round 2 (retry): both joiners save_ready + start(save=true), host start.
    """
    HP, P1, P2 = 29520, 29521, 29522
    names = {"host": "alice", "j1": "bob", "j2": "carol"}
    base = tempfile.mkdtemp(prefix="lobby_xferfail_")
    dirs = {k: os.path.join(base, k) for k in names}
    ios = {k: LobbyIO(dirs[k]) for k in names}
    stop = threading.Event()
    conns = []
    corrupt = [True]                          # flipped off before the retry

    class _CorruptingReceiver(_ClientSaveReceiver):
        """Zeroes chunk 0 while ``corrupt`` is set -> per-file SHA mismatch."""
        def on_chunk(self, sid, seq, data):
            if seq == 0 and corrupt[0]:
                data = bytes(len(data))
            super().on_chunk(sid, seq, data)

    save_path = os.path.join(base, "world.sav")
    with open(save_path, "wb") as f:
        f.write(os.urandom(size_bytes))
    src_sha = _sha256_file(save_path)

    ok = True
    t0 = time.time()
    try:
        hsock = open_socket(HP, socket.AF_INET)
        threading.Thread(target=run_host, name="xferfail-host",
                         args=(hsock, names["host"], ios["host"]),
                         kwargs={"code": "XFERFAIL", "stop": stop},
                         daemon=True).start()
        time.sleep(0.4)

        for port, key, cls in ((P1, "j1", _ClientSaveReceiver),
                               (P2, "j2", _CorruptingReceiver)):
            conn = _dial_loopback(port, HP, 12)
            if not conn:
                print(f"[xfer:{tag}] FAIL: joiner {names[key]} could not connect")
                return False
            conns.append(conn)
            threading.Thread(target=run_client, name=key,
                             args=(conn, names[key], ios[key]),
                             kwargs={"stop": stop, "receiver_cls": cls},
                             daemon=True).start()

        def all_joined():
            for k in names:
                r = _latest_roster(ios[k].out_path)
                if not r or len(r.get("players", [])) < 3:
                    return False
            return True

        if not _wait_until(all_joined, timeout=15):
            print(f"[xfer:{tag}] FAIL: not all three joined")
            return False

        def host_failed_status():
            return [e for e in _read_events(ios["host"].out_path)
                    if e.get("type") == "status" and e.get("state") == "failed"
                    and "press START GAME to retry" in e.get("detail", "")]

        # ---- round 1: carol's receiver cannot verify -> nobody starts ------ #
        with open(ios["host"].in_path, "a", encoding="utf-8") as f:
            f.write(json.dumps({"cmd": "start", "save": save_path}) + "\n")

        # Must resolve WELL before PEER_XFER_TIMEOUT (30 s): each re-request
        # only completes because the host rewinds its cursor to the new base.
        if not _wait_until(host_failed_status, timeout=PEER_XFER_TIMEOUT - 10):
            print(f"[xfer:{tag}] FAIL: host never emitted the retry status "
                  f"(receiver rewind not honoured?)")
            ok = False
        else:
            detail = host_failed_status()[-1]["detail"]
            if detail != ("save transfer failed for carol -- press START GAME "
                          "to retry"):
                print(f"[xfer:{tag}] FAIL: unexpected failed detail: {detail!r}")
                ok = False
        time.sleep(0.5)                       # let any stray start land (none may)
        if _has_start(ios["host"].out_path):
            print(f"[xfer:{tag}] FAIL: host broadcast start despite a failure")
            ok = False
        for k in ("j1", "j2"):
            if _has_start(ios[k].out_path):
                print(f"[xfer:{tag}] FAIL: {k} started despite a failed transfer")
                ok = False
        if not any(e.get("type") == "status" and e.get("state") == "failed"
                   and "hash mismatch" in e.get("detail", "")
                   for e in _read_events(ios["j2"].out_path)):
            print(f"[xfer:{tag}] FAIL: j2 never reported the hash mismatch")
            ok = False
        if any(e.get("type") == "save_ready"
               for e in _read_events(ios["j2"].out_path)):
            print(f"[xfer:{tag}] FAIL: j2 emitted save_ready for a bad file")
            ok = False
        if not any(e.get("type") == "save_ready"
                   for e in _read_events(ios["j1"].out_path)):
            print(f"[xfer:{tag}] FAIL: j1 (healthy) never emitted save_ready")
            ok = False
        if ok:
            print(f"[xfer:{tag}] round 1 OK: failure surfaced, nobody started "
                  f"({time.time() - t0:.1f}s)")

        # ---- round 2: fix carol, press START GAME again -> all start ------- #
        corrupt[0] = False
        with open(ios["host"].in_path, "a", encoding="utf-8") as f:
            f.write(json.dumps({"cmd": "start", "save": save_path}) + "\n")

        def retry_settled():
            return (_has_start(ios["host"].out_path, save=True)
                    and all(_has_start(ios[k].out_path, save=True)
                            for k in ("j1", "j2")))

        if not _wait_until(retry_settled, timeout=30):
            print(f"[xfer:{tag}] FAIL: retry did not start everyone")
            for k in names:
                print(f"[xfer:{tag}]   {k} start(save=true): "
                      f"{_has_start(ios[k].out_path, save=True)}")
            ok = False
        for k in ("j1", "j2"):
            p = os.path.join(ios[k].dir, INCOMING_BASENAME + ".sav")
            if not os.path.isfile(p) or _sha256_file(p) != src_sha:
                print(f"[xfer:{tag}] FAIL: {k} incoming_save.sav wrong after retry")
                ok = False
    finally:
        stop.set()
        time.sleep(0.5)
        for c in conns:
            try:
                c.close()
            except Exception:
                pass
        shutil.rmtree(base, ignore_errors=True)

    print(f"[xfer:{tag}] {'OK' if ok else 'FAIL'}  "
          f"(failed peer -> retry status, then successful retry, "
          f"{time.time() - t0:.1f}s)")
    return ok


def selftest_transfer():
    """Run the save-transfer self-test several times (clean + lossy) so it
    proves both correctness and non-flakiness, and exercises retransmit;
    then the failure + retry case."""
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
    allok = _run_transfer_failure("fail-retry") and allok
    print(f"[selftest-transfer] {'PASS' if allok else 'FAIL'}")
    return allok


# --------------------------------------------------------------------------- #
# Self-test: GAME RELAY -- two stand-in bridges exchange frames via host+joiner
# --------------------------------------------------------------------------- #
def selftest_relay():
    """1 host + 1 joiner on loopback, each with a game relay; two plain UDP
    sockets stand in for the two bridges (bound to the --game-local-ports).

    (a) 200 numbered 900-byte frames go EACH way in through the relay ports and
        must all arrive, byte-exact, at the OTHER stand-in (reordering allowed;
        loopback loses nothing).
    (b) an oversize frame is dropped (counted + warned), never delivered.
    (c) lobby chat still flows after the relay traffic.
    (d) the relay counters agree with (a).
    """
    HP, P1 = 29530, 29531                       # lobby transport ports
    RELAY_HOST, RELAY_JOIN = 7783, 7784         # --game-relay-port (per role)
    LOCAL_HOST, LOCAL_JOIN = 7781, 7782         # --game-local-port (the bridges)
    N, SIZE = 200, 900
    names = {"host": "alice", "j1": "bob"}
    base = tempfile.mkdtemp(prefix="lobby_relay_")
    ios = {k: LobbyIO(os.path.join(base, k)) for k in names}
    stop = threading.Event()
    conns, relays, standins = [], [], []
    ok = True
    print(f"[selftest-relay] host udp/{HP}: relay {RELAY_HOST} -> bridge "
          f"{LOCAL_HOST};  joiner udp/{P1}: relay {RELAY_JOIN} -> bridge "
          f"{LOCAL_JOIN}")

    def frame(tag, i):
        head = tag + struct.pack("!I", i)
        return head + bytes((i * 7 + j) & 0xFF for j in range(SIZE - len(head)))

    try:
        # The two "bridges": plain UDP sockets on the local ports.
        bridge_a = _open_loopback_udp(LOCAL_HOST)     # host machine's bridge
        bridge_b = _open_loopback_udp(LOCAL_JOIN)     # joiner machine's bridge
        standins += [bridge_a, bridge_b]
        relay_h = GameRelay(RELAY_HOST, LOCAL_HOST)
        relay_j = GameRelay(RELAY_JOIN, LOCAL_JOIN)
        relays += [relay_h, relay_j]

        hsock = open_socket(HP, socket.AF_INET)
        threading.Thread(target=run_host, name="relay-host",
                         args=(hsock, names["host"], ios["host"]),
                         kwargs={"code": "RELAYCODE", "stop": stop,
                                 "relay": relay_h}, daemon=True).start()
        time.sleep(0.4)
        conn = _dial_loopback(P1, HP, 10)
        if not conn:
            print("[selftest-relay] FAIL: joiner could not connect")
            return False
        conns.append(conn)
        threading.Thread(target=run_client, name="relay-j1",
                         args=(conn, names["j1"], ios["j1"]),
                         kwargs={"stop": stop, "relay": relay_j},
                         daemon=True).start()

        def both_joined():
            for k in names:
                r = _latest_roster(ios[k].out_path)
                if not r or sorted(r.get("players", [])) != sorted(names.values()):
                    return False
            return True

        if not _wait_until(both_joined, timeout=12):
            print("[selftest-relay] FAIL: lobby did not form")
            return False
        print("[selftest-relay] lobby formed; sending frames")

        # (a) N frames each way, interleaved, lightly paced (bursts are what a
        #     real bridge does too, but we're proving routing, not buffers).
        for i in range(N):
            bridge_a.sendto(frame(b"A", i), ("127.0.0.1", RELAY_HOST))
            bridge_b.sendto(frame(b"B", i), ("127.0.0.1", RELAY_JOIN))
            if i % 20 == 19:
                time.sleep(0.002)

        got = {b"A": set(), b"B": set()}          # tag -> indices received
        bad = 0
        deadline = time.time() + 15.0
        while (time.time() < deadline
               and (len(got[b"A"]) < N or len(got[b"B"]) < N)):
            try:
                ready, _, _ = select.select([bridge_a, bridge_b], [], [], 0.2)
            except (OSError, ValueError):
                break
            for s in ready:
                for _ in range(GAME_RELAY_DRAIN):
                    try:
                        data, _src = s.recvfrom(65535)
                    except (BlockingIOError, ConnectionResetError, OSError):
                        break
                    tag = data[:1]
                    if tag not in got or len(data) < 5:
                        bad += 1
                        continue
                    idx = struct.unpack("!I", data[1:5])[0]
                    # A-frames left bridge_a and must land on bridge_b; B the
                    # other way. Anything else is misrouted or corrupt.
                    expect = bridge_b if tag == b"A" else bridge_a
                    if s is not expect or data != frame(tag, idx):
                        bad += 1
                        continue
                    got[tag].add(idx)

        for tag, dst in ((b"A", "joiner"), (b"B", "host")):
            missing = sorted(set(range(N)) - got[tag])
            if missing:
                ok = False
                print(f"[selftest-relay] FAIL (a): {len(missing)}/{N} "
                      f"{tag.decode()} frames never reached the {dst}'s bridge "
                      f"(first missing {missing[:5]})")
            else:
                print(f"[selftest-relay] OK (a): all {N} {tag.decode()} frames "
                      f"reached the {dst}'s bridge")
        if bad:
            ok = False
            print(f"[selftest-relay] FAIL (a): {bad} corrupt/misrouted frames")

        # (b) an oversize frame is dropped, never delivered.
        bridge_a.sendto(b"X" * (GAME_RELAY_MAX + 100), ("127.0.0.1", RELAY_HOST))
        time.sleep(0.5)
        leaked = False
        while True:
            try:
                data, _src = bridge_b.recvfrom(65535)
            except (BlockingIOError, ConnectionResetError, OSError):
                break
            if data[:1] == b"X":
                leaked = True
        if leaked or relay_h.oversize != 1:
            ok = False
            print(f"[selftest-relay] FAIL (b): oversize frame leaked={leaked} "
                  f"oversize_count={relay_h.oversize}")
        else:
            print("[selftest-relay] OK (b): oversize frame dropped (warned)")

        # (c) the lobby still works around the relay traffic: chat from joiner.
        chat_text = "still chatting after relay"
        with open(ios["j1"].in_path, "a", encoding="utf-8") as f:
            f.write(json.dumps({"cmd": "chat", "text": chat_text}) + "\n")
        if not _wait_until(lambda: all(_has_chat(ios[k].out_path, chat_text)
                                       for k in names), timeout=10):
            ok = False
            print("[selftest-relay] FAIL (c): chat broken after relay traffic")
        else:
            print("[selftest-relay] OK (c): chat still flows")

        # (d) counters: each relay forwarded N and delivered N.
        counts = (relay_h.forwarded, relay_h.delivered,
                  relay_j.forwarded, relay_j.delivered)
        if counts != (N, N, N, N):
            ok = False
            print(f"[selftest-relay] FAIL (d): counters host(fwd,del)="
                  f"{counts[:2]} joiner(fwd,del)={counts[2:]} (want {N} each)")
        else:
            print(f"[selftest-relay] OK (d): counters host/joiner fwd={N} "
                  f"delivered={N}")
    finally:
        stop.set()
        time.sleep(0.5)
        for c in conns:
            try:
                c.close()
            except Exception:
                pass
        for r in relays:
            r.close()                     # idempotent; the loops close too
        for s in standins:
            try:
                s.close()
            except OSError:
                pass
        shutil.rmtree(base, ignore_errors=True)

    print(f"[selftest-relay] {'PASS' if ok else 'FAIL'}")
    return ok


# --------------------------------------------------------------------------- #
# Self-test: host + 3 joiners; j1<->j2 punch directly, j3 is relay-only
# --------------------------------------------------------------------------- #
def selftest_mesh():
    """Every participant's bridge frames must reach every OTHER participant's
    bridge EXACTLY once. j1 and j2 carry loopback profiles, so they must end up
    with a direct link; j3 joins with no mesh (the old star), so everything to
    and from it is relayed through the host. Also proves the host still gets
    one copy (not a duplicate) of each joiner frame."""
    HP = 29570
    JP = {"j1": 29571, "j2": 29572, "j3": 29573}
    RELAY = {"host": 7793, "j1": 7794, "j2": 7795, "j3": 7796}   # bridge -> lobby
    LOCAL = {"host": 7783, "j1": 7784, "j2": 7785, "j3": 7786}   # lobby -> bridge
    names = {"host": "alice", "j1": "bob", "j2": "carol", "j3": "dave"}
    N, SIZE = 60, 600
    base = tempfile.mkdtemp(prefix="lobby_mesh_")
    ios = {k: LobbyIO(os.path.join(base, k)) for k in names}
    stop = threading.Event()
    conns, relays, standins, meshes = [], [], [], {}
    ok = True
    # the whole test runs SEALED: one process-wide sealer stands in for the key
    # every member would derive from the code
    KEY = derive_key(b"selftest-secret!"[:SECRET_LEN], "pw")
    SEAL[0] = Sealer(KEY)                # the host's instance (own replay window)

    def frame(tag, i):
        head = tag + struct.pack("!I", i)
        return head + bytes((i * 3 + j) & 0xFF for j in range(SIZE - len(head)))

    try:
        bridges = {k: _open_loopback_udp(LOCAL[k]) for k in names}
        standins += list(bridges.values())
        rel = {k: GameRelay(RELAY[k], LOCAL[k]) for k in names}
        relays += list(rel.values())

        hsock = open_socket(HP, socket.AF_INET)
        threading.Thread(target=run_host, name="mesh-host",
                         args=(hsock, names["host"], ios["host"]),
                         kwargs={"code": "MESHCODE", "stop": stop,
                                 "relay": rel["host"]}, daemon=True).start()
        time.sleep(0.4)

        for key in ("j1", "j2", "j3"):
            port = JP[key]
            peer = {"candidates": {"public_v4": f"127.0.0.1:{HP}",
                                   "lan_v4": None, "v6": None},
                    "flags": {"open": True, "v6": False}}
            s = open_socket(port, socket.AF_INET)
            conn = race(s, peer, "dial", port, 10, my_has_v6=False)
            if not conn:
                print(f"[selftest-mesh] FAIL: {names[key]} could not connect")
                return False
            conns.append(conn)
            conn.cipher = Sealer(KEY)    # one instance per member, like real processes
            mesh = None
            code = None
            if key != "j3":
                code = encode_profile({"candidates": {"lan_v4": f"127.0.0.1:{port}",
                                                      "public_v4": None, "v6": None},
                                       "flags": {"open": True}})
                mesh, conn = _mesh_from_conn(conn, log=lambda m, k=key: print(f"  [{k}] {m}"))
                meshes[key] = mesh
            threading.Thread(target=run_client, name=f"mesh-{key}",
                             args=(conn, names[key], ios[key]),
                             kwargs={"stop": stop, "relay": rel[key],
                                     "mesh": mesh, "profile_code": code},
                             daemon=True).start()

        def all_joined():
            for k in names:
                r = _latest_roster(ios[k].out_path)
                if not r or sorted(r.get("players", [])) != sorted(names.values()):
                    return False
            return True

        if not _wait_until(all_joined, timeout=15):
            print("[selftest-mesh] FAIL: lobby did not form")
            return False

        # (a) j1 <-> j2 must become a direct link; j3 has none
        def linked():
            return (meshes["j1"].by_name(names["j2"]) is not None
                    and meshes["j2"].by_name(names["j1"]) is not None)
        if not _wait_until(linked, timeout=12):
            ok = False
            print("[selftest-mesh] FAIL (a): j1<->j2 did not link directly")
        else:
            print("[selftest-mesh] OK (a): j1<->j2 direct link on their own sockets")
        time.sleep(1.0)                      # let 'links' reports reach the roster

        # (b) frames from every bridge reach every other bridge exactly once
        tags = {"host": b"H", "j1": b"1", "j2": b"2", "j3": b"3"}
        for i in range(N):
            for k in names:
                bridges[k].sendto(frame(tags[k], i), ("127.0.0.1", RELAY[k]))
            if i % 10 == 9:
                time.sleep(0.003)
        got = {k: {t: collections.Counter() for t in tags.values()} for k in names}
        deadline = time.time() + 15.0
        def complete():
            for k in names:
                for src, t in tags.items():
                    if src != k and len(got[k][t]) < N:
                        return False
            return True
        while time.time() < deadline and not complete():
            try:
                ready, _, _ = select.select(list(bridges.values()), [], [], 0.2)
            except (OSError, ValueError):
                break
            for s in ready:
                k = next(kk for kk, ss in bridges.items() if ss is s)
                for _ in range(GAME_RELAY_DRAIN):
                    try:
                        data, _src = s.recvfrom(65535)
                    except (BlockingIOError, ConnectionResetError, OSError):
                        break
                    t = data[:1]
                    if t in got[k] and len(data) >= 5:
                        got[k][t][struct.unpack("!I", data[1:5])[0]] += 1
        time.sleep(0.5)                      # catch late duplicates, if any
        for s in bridges.values():
            while True:
                try:
                    data, _src = s.recvfrom(65535)
                except (BlockingIOError, ConnectionResetError, OSError):
                    break
                k = next(kk for kk, ss in bridges.items() if ss is s)
                t = data[:1]
                if t in got[k] and len(data) >= 5:
                    got[k][t][struct.unpack("!I", data[1:5])[0]] += 1
        for k in names:
            for src, t in tags.items():
                if src == k:
                    if got[k][t]:
                        ok = False
                        print(f"[selftest-mesh] FAIL (b): {k} received its own frames")
                    continue
                c = got[k][t]
                missing = N - len(c)
                dups = sum(v - 1 for v in c.values() if v > 1)
                if missing or dups:
                    ok = False
                    print(f"[selftest-mesh] FAIL (b): {src}->{k}: missing={missing} duplicates={dups}")
        if ok:
            print(f"[selftest-mesh] OK (b): {N} frames from each of 4 bridges reached "
                  f"the other 3 exactly once (direct j1<->j2, relayed j3)")
    finally:
        stop.set()
        time.sleep(0.5)
        for c in conns:
            try:
                c.close()
            except Exception:
                pass
        for r in relays:
            r.close()
        for s in standins:
            try:
                s.close()
            except OSError:
                pass
        shutil.rmtree(base, ignore_errors=True)
        SEAL[0] = None
    print(f"[selftest-mesh] {'PASS' if ok else 'FAIL'}"
          + (" (all frames sealed)" if ok else ""))
    return ok


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
    ap.add_argument("--password", default="",
                    help="optional lobby password: mixed into the session key, "
                         "so everyone must enter the same one (host + joiners)")
    ap.add_argument("--forward-log", action="append", default=None,
                    metavar="PATH",
                    help="also tail this file and ship its new lines to the "
                         "host's merged lobby_peers.log (repeatable; e.g. the "
                         "bridge log)")
    ap.add_argument("--no-mesh", action="store_true",
                    help="joiner: do not punch other joiners directly; keep "
                         "every frame on the host relay (the pre-mesh star)")
    ap.add_argument("--selftest-mesh", action="store_true",
                    help="run the mesh self-test (host + 3 joiners, two of "
                         "them directly linked, one relay-only) and exit")
    ap.add_argument("--selftest-relay", action="store_true",
                    help="run the game-relay self-test (host + joiner, two "
                         "stand-in bridges, frames both ways) and exit")
    ap.add_argument("--game-relay-port", type=int, default=None,
                    help="bind 127.0.0.1:<port> and relay the lockstep "
                         "bridge's UDP frames over the lobby transport "
                         "(the menu passes 7773 for host, 7774 for join)")
    ap.add_argument("--game-local-port", type=int,
                    default=GAME_LOCAL_PORT_DEFAULT,
                    help="the local bridge's UDP port that relayed frames are "
                         "delivered to (the menu reads it from "
                         "tpf2_instance.txt; default %(default)s)")
    args = ap.parse_args(argv)

    if args.selftest:
        return 0 if selftest() else 1
    if args.selftest_transfer:
        return 0 if selftest_transfer() else 1
    if args.selftest_relay:
        return 0 if selftest_relay() else 1
    if args.selftest_mesh:
        return 0 if selftest_mesh() else 1
    if args.mode == "host":
        return cmd_host(args)
    if args.mode == "join":
        if not args.code:
            ap.error("join requires a CODE argument")
        return cmd_join(args)
    ap.error("give a mode: host / join / --selftest / --selftest-transfer / "
             "--selftest-relay")
    return 2


if __name__ == "__main__":
    sys.exit(main())
