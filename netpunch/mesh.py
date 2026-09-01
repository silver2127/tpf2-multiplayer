#!/usr/bin/env python3
"""
mesh.py -- many hole-punched peers on ONE socket (by-source demux).

``punch.Connection`` owns its socket for exactly one peer: it locks onto
whichever address spoke last. A joiner therefore had a single link (to the
host) and everything else was relayed through the host. A NAT mapping is
per-socket, so a joiner cannot open a second socket to punch a second peer --
the advertised (observed) mapping only exists for the socket it was observed
on. The way out is what the host already does: keep the one observed socket and
demultiplex inbound frames BY SOURCE ADDRESS into per-peer state.

``MeshNode`` does that. Same NP1 wire as punch.py (HELLO/ACK/CONNECTED/
KEEPALIVE/DATA, identical framing and handshake), so a stock lobby host that
just answers HELLOs with ACKs interoperates unchanged, and two MeshNodes punch
each other with the usual simultaneous-send.

    node = MeshNode(sock)                     # the observed, already-bound socket
    node.adopt(host_addr, "host")             # a link the connect race already made
    node.dial("bob", [(ip, port), ...])       # HELLO bursts at every candidate
    node.send("bob", payload) / node.send(addr, payload)
    addr, payload = node.recv(timeout)        # any DATA, from any peer
    node.link(addr).name / node.by_name("bob")

Naming: a HELLO/ACK carries no identity, and NAT can rewrite the address we
dialled, so the lobby layer names a link out-of-band: right after a link
connects, each side sends a ``{t:"mesh_hi", name}`` DATA and the receiver calls
``node.name_link(addr, name)``. A pending dial to that name is then resolved.
"""

from __future__ import annotations

import os
import queue
import select
import socket
import threading
import time

from punch import (
    TYPE_HELLO, TYPE_ACK, TYPE_CONNECTED, TYPE_KEEPALIVE, TYPE_DATA,
    _pack, _unpack, HELLO_INTERVAL, KEEPALIVE_INTERVAL, TOKEN_LEN,
)

DIAL_TIMEOUT = 8.0          # give up on a direct punch after this; relay instead
LINK_DEAD_AFTER = 30.0      # a connected link unheard-from this long is dropped


class Link:
    __slots__ = ("addr", "name", "connected", "last_seen", "last_keepalive",
                 "since")

    def __init__(self, addr, name=None, connected=False):
        self.addr = addr
        self.name = name
        self.connected = connected
        now = time.time()
        self.last_seen = now if connected else 0.0
        self.last_keepalive = now
        self.since = now


class Dial:
    __slots__ = ("name", "targets", "started", "done", "failed")

    def __init__(self, name, targets):
        self.name = name
        self.targets = list(targets)
        self.started = time.time()
        self.done = False
        self.failed = False


class MeshNode:
    def __init__(self, sock, token=None, log=None, name="mesh"):
        self.sock = sock
        self.token = token or os.urandom(TOKEN_LEN)
        self.log = log or (lambda *_a, **_k: None)
        self.name = name
        self._lock = threading.Lock()
        self.links = {}            # addr -> Link
        self.dials = {}            # name -> Dial
        self.inbox = queue.Queue() # (addr, payload)
        self._stop = threading.Event()
        self._thread = threading.Thread(target=self._run, name=f"mesh-{name}",
                                        daemon=True)
        self.sock.setblocking(False)
        self._thread.start()

    # -- link bookkeeping ------------------------------------------------- #
    def adopt(self, addr, name, connected=True):
        """Register a link the connect race already established (the host)."""
        with self._lock:
            ln = self.links.get(addr)
            if ln is None:
                ln = Link(addr, name, connected)
                self.links[addr] = ln
            else:
                ln.name = name
                ln.connected = ln.connected or connected
            if connected:
                ln.last_seen = time.time()
            return ln

    def name_link(self, addr, name):
        """Attach a name to a connected link (from a mesh_hi). Resolves a
        pending dial of that name, even if NAT rewrote the address."""
        with self._lock:
            ln = self.links.get(addr)
            if ln is None:
                ln = Link(addr, name, True)
                self.links[addr] = ln
            ln.name = name
            d = self.dials.get(name)
            if d is not None and not d.done:
                d.done = True
                self.log(f"[mesh] {name} direct via {addr[0]}:{addr[1]}")
            return ln

    def link(self, addr):
        return self.links.get(addr)

    def by_name(self, name):
        with self._lock:
            for ln in self.links.values():
                if ln.name == name and ln.connected:
                    return ln
        return None

    def direct_names(self):
        with self._lock:
            return sorted({ln.name for ln in self.links.values()
                           if ln.connected and ln.name})

    def dial(self, name, targets):
        """Start (or restart) HELLO bursts at ``targets`` [(ip, port), ...]
        until a link named ``name`` connects or DIAL_TIMEOUT passes."""
        if not targets:
            return
        with self._lock:
            if self.by_name_locked(name):
                return
            self.dials[name] = Dial(name, targets)
        self.log(f"[mesh] dialing {name} at {targets}")

    def by_name_locked(self, name):
        for ln in self.links.values():
            if ln.name == name and ln.connected:
                return ln
        return None

    def dial_failed(self, name):
        d = self.dials.get(name)
        return bool(d and d.failed)

    def dial_pending(self, name):
        d = self.dials.get(name)
        return bool(d and not d.done and not d.failed)

    def forget_dial(self, name):
        with self._lock:
            self.dials.pop(name, None)

    # -- I/O ---------------------------------------------------------------- #
    def _send_to(self, ptype, payload, addr):
        try:
            self.sock.sendto(_pack(ptype, payload), addr)
            return True
        except (ConnectionResetError, OSError):
            return False

    def send(self, to, payload):
        """``to`` is a peer name or an (ip, port). Returns True if sent."""
        if isinstance(to, str):
            ln = self.by_name(to)
            if ln is None:
                return False
            addr = ln.addr
        else:
            addr = to
        return self._send_to(TYPE_DATA, payload, addr)

    def recv(self, timeout=None):
        try:
            return self.inbox.get(timeout=timeout)
        except queue.Empty:
            return None

    def last_seen_age(self, addr):
        ln = self.links.get(addr)
        if ln is None or not ln.last_seen:
            return float("inf")
        return time.time() - ln.last_seen

    # -- reader / puncher loop --------------------------------------------- #
    def _run(self):
        last_hello = 0.0
        while not self._stop.is_set():
            now = time.time()
            if now - last_hello >= HELLO_INTERVAL:
                last_hello = now
                with self._lock:
                    for d in list(self.dials.values()):
                        if d.done or d.failed:
                            continue
                        if now - d.started > DIAL_TIMEOUT:
                            d.failed = True
                            self.log(f"[mesh] {d.name}: no direct path "
                                     f"(timeout) -- relaying")
                            continue
                        for t in d.targets:
                            self._send_to(TYPE_HELLO, self.token, t)
                    for ln in self.links.values():
                        if ln.connected and now - ln.last_keepalive >= KEEPALIVE_INTERVAL:
                            ln.last_keepalive = now
                            self._send_to(TYPE_KEEPALIVE, self.token, ln.addr)
                        if ln.connected and ln.last_seen and now - ln.last_seen > LINK_DEAD_AFTER:
                            ln.connected = False
                            self.log(f"[mesh] {ln.name or ln.addr}: link dead")
            try:
                ready, _, _ = select.select([self.sock], [], [], HELLO_INTERVAL)
            except (OSError, ValueError):
                break
            if not ready:
                continue
            for _ in range(256):
                try:
                    data, addr = self.sock.recvfrom(65535)
                except BlockingIOError:
                    break
                except (ConnectionResetError, OSError):
                    break
                ptype, payload = _unpack(data)
                if ptype is None:
                    continue
                with self._lock:
                    ln = self.links.get(addr)
                    if ln is None:
                        ln = Link(addr)
                        self.links[addr] = ln
                    ln.last_seen = now
                if ptype == TYPE_HELLO:
                    self._send_to(TYPE_ACK, payload, addr)     # echo THEIR token
                elif ptype == TYPE_ACK:
                    if payload == self.token and not ln.connected:
                        ln.connected = True
                        self._send_to(TYPE_CONNECTED, self.token, addr)
                        self.log(f"[mesh] connected {addr[0]}:{addr[1]}")
                elif ptype == TYPE_CONNECTED:
                    if not ln.connected:
                        ln.connected = True
                        self.log(f"[mesh] connected (peer-driven) {addr[0]}:{addr[1]}")
                elif ptype == TYPE_KEEPALIVE:
                    pass
                elif ptype == TYPE_DATA:
                    self.inbox.put((addr, payload))

    def close(self):
        self._stop.set()
        if self._thread.is_alive() and threading.current_thread() is not self._thread:
            self._thread.join(timeout=1.0)
        try:
            self.sock.close()
        except OSError:
            pass


# --------------------------------------------------------------------------- #
# Self-test: three nodes on loopback punch each other pairwise on ONE socket each
# --------------------------------------------------------------------------- #
def selftest(base_port=29560):
    from punch import open_socket
    ports = {"a": base_port, "b": base_port + 1, "c": base_port + 2}
    nodes = {n: MeshNode(open_socket(p, socket.AF_INET), name=n,
                         log=lambda m: print("  " + m)) for n, p in ports.items()}
    # everyone dials everyone (simultaneous open)
    for me in nodes:
        for other, p in ports.items():
            if other != me:
                nodes[me].dial(other, [("127.0.0.1", p)])
    # name links via mesh_hi once connected
    deadline = time.time() + 6
    ok = True
    named = {n: set() for n in nodes}
    while time.time() < deadline and any(len(named[n]) < 2 for n in nodes):
        for me, node in nodes.items():
            for ln in list(node.links.values()):
                if ln.connected and ln.name is None:
                    node._send_to(TYPE_DATA, b'{"t":"mesh_hi","name":"%s"}' % me.encode(), ln.addr)
                    # crude: also sends when the far side has named us already;
                    # harmless duplicate
            r = node.recv(timeout=0.01)
            while r is not None:
                addr, payload = r
                if payload.startswith(b'{"t":"mesh_hi"'):
                    import json
                    nm = json.loads(payload)["name"]
                    node.name_link(addr, nm)
                    named[me].add(nm)
                    node.forget_dial(nm)
                    node._send_to(TYPE_DATA, b'{"t":"mesh_hi","name":"%s"}' % me.encode(), addr)
                r = node.recv(timeout=0.0)
    for n in nodes:
        if len(named[n]) < 2:
            ok = False
            print(f"[mesh] FAIL: {n} named links = {sorted(named[n])}")
    if ok:
        # data both ways by name
        nodes["a"].send("c", b"hello-c")
        got = None
        t1 = time.time() + 2
        while time.time() < t1:
            r = nodes["c"].recv(timeout=0.2)
            if r and r[1] == b"hello-c":
                got = r; break
        ok = got is not None and nodes["c"].link(got[0]).name == "a"
    for node in nodes.values():
        node.close()
    print(f"[mesh] {'PASS' if ok else 'FAIL'}: 3 nodes, each with 2 direct links on one socket")
    return ok


if __name__ == "__main__":
    import sys
    sys.exit(0 if selftest() else 1)
