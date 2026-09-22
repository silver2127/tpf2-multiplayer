"""The lobby's side of the Steam transport (native/src/steam_tunnel.cpp).

The bridge DLL, inside the game, sends and receives Steam P2P packets keyed by
SteamID and presents every Steam peer to this process as a loopback UDP
endpoint (127.0.0.1, a port in TUNNEL_PORTS). This module reads the tunnel's identity file, talks its
control protocol, and gives the lobby the endpoint address of a SteamID to dial
or punch toward exactly like any other candidate.

    t = SteamTunnel(data_dir, log)          # t.id is None without a tunnel
    t.hello(lobby_udp_port)                 # where inbound packets go
    ep = t.dial(host_steamid)               # ("127.0.0.1", port in the tunnel range) or None

The join code carries the host's SteamID (connect.py, the trailing field), and
the joiner's knock at the master carries its own, so the host can open the
session from its side too (the tunnel's OPEN implicitly accepts).

FakeTunnel below implements the same control protocol in-process for tests: two
fakes with different ids deliver to each other, so a host and a joiner can be
run through the Steam path on one machine with no Steam at all.
"""
from __future__ import annotations

import os
import socket
import threading
import time

TUNNEL_IP = "127.0.0.1"
TUNNEL_PORTS = range(62100, 62200)   # endpoints live on 127.0.0.1 in this range: the lobby tells them from
                                     # real peers by the port. (A loopback alias, 127.0.0.77, was the first
                                     # design: Windows binds it and then drops everything it sends.)
IDENTITY_FILE = "tpf2_steam.txt"
OFF_FILE = "tpf2mp_steam_off.txt"


def read_identity(data_dir):
    """(steamid64 as str, control port, persona) from <data_dir>/tpf2_steam.txt,
    or None when the tunnel is off, not up yet, or the file is unreadable."""
    if not data_dir:
        return None
    try:
        with open(os.path.join(data_dir, IDENTITY_FILE), "r", encoding="utf-8", errors="replace") as f:
            text = f.read()
    except OSError:
        return None
    fields = {}
    for line in text.splitlines():
        k, sep, v = line.partition("=")
        if sep:
            fields[k.strip()] = v.strip()
    sid, port = fields.get("id", ""), fields.get("port", "")
    if not sid.isdigit() or int(sid) == 0 or not port.isdigit() or not 0 < int(port) < 65536:
        return None
    return sid, int(port), fields.get("name", "")


def is_tunnel_addr(addr):
    """True for a peer address that is a Steam tunnel endpoint (no TCP there)."""
    try:
        return addr[0] == TUNNEL_IP and int(addr[1]) in TUNNEL_PORTS
    except (TypeError, IndexError, ValueError):
        return False


class SteamTunnel:
    """Control-protocol client. Cheap to construct; ``id`` is None when there is
    no tunnel (no data dir, kill switch, Steam not up), and every method is then
    a no-op returning None."""

    def __init__(self, data_dir, log=lambda s: None, identity=None, wait=0.0):
        self.data_dir, self.log = data_dir, log
        self.id = self.port = self.name = None
        self._sock = None
        self._lock = threading.Lock()
        if data_dir and os.path.exists(os.path.join(data_dir, OFF_FILE)):
            log("[steam] tpf2mp_steam_off.txt present -- the Steam transport is off")
            return
        ident = identity or read_identity(data_dir)
        deadline = time.time() + wait
        while ident is None and time.time() < deadline:
            time.sleep(0.25)
            ident = read_identity(data_dir)
        if ident is None:
            return
        self.id, self.port, self.name = ident
        try:
            self._sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
            self._sock.bind(("127.0.0.1", 0))
            self._sock.settimeout(1.0)
        except OSError as e:
            log(f"[steam] control socket failed: {e}")
            self.id = None

    @property
    def available(self):
        return self.id is not None and self._sock is not None

    def _ask(self, text, tries=3):
        if not self.available:
            return None
        with self._lock:
            for _ in range(tries):
                try:
                    self._sock.sendto(text.encode("ascii"), ("127.0.0.1", self.port))
                    data, _ = self._sock.recvfrom(65535)
                    return data.decode("utf-8", "replace")
                except socket.timeout:
                    continue
                except OSError as e:
                    self.log(f"[steam] control {text.split()[0]} failed: {e}")
                    return None
        self.log(f"[steam] the tunnel did not answer {text.split()[0]} -- Steam transport unavailable")
        return None

    def hello(self, lobby_port):
        """Tell the tunnel where this lobby's game socket listens."""
        r = self._ask(f"LOBBY {int(lobby_port)}")
        if r is not None and r.startswith("OK"):
            self.log(f"[steam] transport ready: this is {self.id}" + (f" ({self.name})" if self.name else "")
                     + f", inbound to udp/{lobby_port}")
            return True
        return False

    def dial(self, steamid):
        """The loopback endpoint for ``steamid`` -> (ip, port), or None."""
        steamid = str(steamid or "").strip()
        if not steamid.isdigit() or steamid == self.id:
            return None
        r = self._ask(f"DIAL {steamid}")
        if not r:
            return None
        parts = r.split()
        if len(parts) == 4 and parts[0] == "EP" and parts[1] == steamid and parts[3].isdigit():
            return parts[2], int(parts[3])
        self.log(f"[steam] DIAL {steamid}: {r.strip()}")
        return None

    def close_peer(self, steamid):
        if str(steamid or "").isdigit():
            self._ask(f"CLOSE {steamid}", tries=1)

    def status(self):
        return self._ask("STATUS", tries=1) or ""

    def close(self):
        if self._sock is not None:
            try:
                self._sock.close()
            except OSError:
                pass
            self._sock = None


# --------------------------------------------------------------------------- #
# a fake tunnel for tests: the same control protocol, delivery in-process
# --------------------------------------------------------------------------- #
class FakeTunnel:
    """Stands in for the native tunnel. Every FakeTunnel registers in a class
    table by id; a datagram the lobby sends to fake A's endpoint for B is handed
    to fake B, which delivers it to B's lobby from B's endpoint for A. Writes the
    identity file into ``data_dir`` so the real SteamTunnel client finds it."""

    _by_id = {}
    _table_lock = threading.Lock()

    def __init__(self, data_dir, steamid, name="fake", drop=None):
        self.data_dir, self.id, self.name = data_dir, str(steamid), name
        self.drop = drop                    # optional predicate(bytes) -> True to lose a datagram
        self.lobby_port = None
        self.endpoints = {}                 # peer id -> socket
        self._ctl = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self._ctl.bind(("127.0.0.1", 0))
        self.port = self._ctl.getsockname()[1]
        self._stop = threading.Event()
        self.sent = self.received = 0
        with FakeTunnel._table_lock:
            FakeTunnel._by_id[self.id] = self
        os.makedirs(data_dir, exist_ok=True)
        with open(os.path.join(data_dir, IDENTITY_FILE), "w", encoding="utf-8") as f:
            f.write(f"id={self.id}\nport={self.port}\nname={name}\n")
        self._t = threading.Thread(target=self._run, daemon=True, name=f"faketunnel-{steamid}")
        self._t.start()

    def endpoint_for(self, peer):
        peer = str(peer)
        s = self.endpoints.get(peer)
        if s is None:
            s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
            for port in TUNNEL_PORTS:            # the first free port of the range, as the native tunnel does
                try:
                    s.bind((TUNNEL_IP, port))
                    break
                except OSError:
                    continue
            else:
                raise OSError("no free endpoint port in the tunnel range")
            s.setblocking(False)
            self.endpoints[peer] = s
        return s

    def deliver(self, sender, data):
        """A packet from ``sender`` (a fake id) arrives: to my lobby, from my endpoint for the sender."""
        self.received += 1
        if self.lobby_port is None:
            return
        try:
            self.endpoint_for(sender).sendto(data, ("127.0.0.1", self.lobby_port))
        except OSError:
            pass

    def _run(self):
        import select
        while not self._stop.is_set():
            socks = [self._ctl] + list(self.endpoints.values())
            try:
                ready, _, _ = select.select(socks, [], [], 0.05)
            except (OSError, ValueError):
                continue
            for s in ready:
                if s is self._ctl:
                    try:
                        data, addr = s.recvfrom(65535)
                    except OSError:
                        continue
                    line = data.decode("ascii", "replace").strip()
                    parts = line.split()
                    reply = "ERR unknown"
                    if len(parts) == 2 and parts[0] == "LOBBY" and parts[1].isdigit():
                        self.lobby_port = int(parts[1]); reply = "OK"
                    elif len(parts) == 2 and parts[0] == "DIAL" and parts[1].isdigit():
                        if parts[1] == self.id:
                            reply = "ERR self"
                        else:
                            ep = self.endpoint_for(parts[1])
                            reply = f"EP {parts[1]} {ep.getsockname()[0]} {ep.getsockname()[1]}"
                    elif len(parts) == 2 and parts[0] == "CLOSE":
                        ep = self.endpoints.pop(parts[1], None)
                        if ep is not None:
                            ep.close()
                        reply = "OK"
                    elif line == "STATUS":
                        reply = f"id={self.id} endpoints={len(self.endpoints)}"
                    try:
                        s.sendto(reply.encode("ascii"), addr)
                    except OSError:
                        pass
                else:
                    peer = next((p for p, e in self.endpoints.items() if e is s), None)
                    for _ in range(64):
                        try:
                            data, _ = s.recvfrom(65535)
                        except (BlockingIOError, OSError):
                            break
                        if peer is None or (self.drop and self.drop(data)):
                            continue
                        self.sent += 1
                        with FakeTunnel._table_lock:
                            other = FakeTunnel._by_id.get(peer)
                        if other is not None:
                            other.deliver(self.id, data)

    def close(self):
        self._stop.set()
        self._t.join(timeout=2)
        with FakeTunnel._table_lock:
            FakeTunnel._by_id.pop(self.id, None)
        for e in self.endpoints.values():
            e.close()
        self._ctl.close()
        try:
            with open(os.path.join(self.data_dir, IDENTITY_FILE), "w", encoding="utf-8"):
                pass
        except OSError:
            pass
