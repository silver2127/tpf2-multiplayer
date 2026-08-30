#!/usr/bin/env python3
"""
punch.py -- serverless UDP hole-punch engine (Phase 1 of netpunch).

This is the core transport used by the Transport Fever 2 co-op mod. Two peers
each bind a single UDP socket to a fixed local port and blast HELLO packets at
each other simultaneously. Whichever NAT sees the outbound packet first opens a
pinhole; the returning packet then flows through it. No server is involved.

Design notes
------------
* ONE socket per address family, bound to a fixed local port (default 29471).
  The public NAT mapping is per-socket, so everything (STUN, punch, gameplay)
  must run on the same socket for the mapping to line up. That's why
  ``Connection`` can adopt an already-bound socket handed in by observe.py.
* Simultaneous-send: send HELLO every 100 ms until connected. On any inbound
  packet, record its source as "the peer" -- always the *most recent* source, so
  we survive a NAT re-mapping mid-handshake.
* Handshake:  HELLO(token=ours) -> ACK(echoes sender's token) -> CONNECTED.
  A side declares success once it receives an ACK echoing ITS OWN token, which
  proves the peer actually heard us. Both sides independently reach that.
* Every packet is framed  MAGIC + TYPE + PAYLOAD  so stray UDP is ignored.
* After connect: KEEPALIVE every 15 s, and we track peer last-seen.

The whole packet lifecycle (handshake + steady state) lives in one reader
thread that owns the socket, so there's never a second reader stealing packets.

CLI:
    python punch.py --local-port 29471 --peer IP:PORT [--name A]
    python punch.py --selftest      # loopback: two instances must both CONNECT
"""

from __future__ import annotations

import argparse
import os
import queue
import select
import socket
import struct
import sys
import threading
import time

# --------------------------------------------------------------------------- #
# Wire format
# --------------------------------------------------------------------------- #
MAGIC = b"NP1:"                      # 4-byte guard so stray UDP is dropped
TYPE_HELLO = b"H"                    # payload = our 8-byte session token
TYPE_ACK = b"A"                      # payload = the token we just received
TYPE_CONNECTED = b"C"               # payload = our token (informational)
TYPE_KEEPALIVE = b"K"              # payload = our token
TYPE_DATA = b"D"                     # payload = application bytes

DEFAULT_PORT = 29471
HELLO_INTERVAL = 0.10                # seconds between HELLO bursts
KEEPALIVE_INTERVAL = 15.0            # seconds between keepalives once connected
TOKEN_LEN = 8


def _pack(ptype: bytes, payload: bytes = b"") -> bytes:
    """Frame a packet: MAGIC + 1-byte type + payload."""
    return MAGIC + ptype + payload


def _unpack(data: bytes):
    """Return (type, payload) or (None, None) if the frame isn't ours."""
    if len(data) < len(MAGIC) + 1 or not data.startswith(MAGIC):
        return None, None
    return data[len(MAGIC):len(MAGIC) + 1], data[len(MAGIC) + 1:]


# --------------------------------------------------------------------------- #
# Socket helper
# --------------------------------------------------------------------------- #
def open_socket(local_port: int = DEFAULT_PORT,
                family: int = socket.AF_INET) -> socket.socket:
    """Create a UDP socket bound to ``local_port`` on all interfaces.

    On Windows, sending a datagram to a port with no listener can make a later
    recv raise WSAECONNRESET; SIO_UDP_CONNRESET disables that where available.
    """
    s = socket.socket(family, socket.SOCK_DGRAM)
    s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    if family == socket.AF_INET6:
        # Keep the v6 socket v6-only so a v4 and a v6 socket can share the same
        # port during the connect race without a bind clash.
        try:
            s.setsockopt(socket.IPPROTO_IPV6, socket.IPV6_V6ONLY, 1)
        except OSError:
            pass
    if hasattr(socket, "SIO_UDP_CONNRESET"):  # Windows only
        try:
            s.ioctl(socket.SIO_UDP_CONNRESET, struct.pack("I", 0))
        except OSError:
            pass
    bind_addr = "::" if family == socket.AF_INET6 else "0.0.0.0"
    s.bind((bind_addr, local_port))
    return s


# --------------------------------------------------------------------------- #
# Connection object
# --------------------------------------------------------------------------- #
class Connection:
    """A live (or forming) peer link. Owns the socket via one reader thread.

    Public surface used by callers/mod:
        .send(bytes)         send an application datagram to the peer
        .recv(timeout=None)  -> bytes | None   pop next application datagram
        .close()             stop the thread and close the socket
        .peer                resolved (ip, port) of the peer, or None yet
        .connected           threading.Event, set once we're confirmed
        .name                friendly label for logging
    """

    def __init__(self, sock, targets, *, token=None, name="peer",
                 listen=False, stop_event=None, log=None):
        self.sock = sock
        self.family = sock.family
        # targets: list of (ip, port) we proactively HELLO at (empty in listen
        # mode until the peer reveals itself).
        self.targets = list(targets or [])
        self.token = token or os.urandom(TOKEN_LEN)
        self.name = name
        self.listen = listen
        self.log = log or (lambda *_a, **_k: None)

        self.peer = None                       # most-recent source address
        self.connected = threading.Event()     # set when our token is echoed
        self._stop = stop_event or threading.Event()
        self._owns_stop = stop_event is None
        self._inbox = queue.Queue()            # decoded DATA payloads
        self._last_seen = 0.0
        self._thread = threading.Thread(
            target=self._run, name=f"punch-{name}", daemon=True)
        self._thread.start()

    # -- internal reader / sender loop ------------------------------------- #
    def _send_to(self, ptype, payload, addr):
        try:
            self.sock.sendto(_pack(ptype, payload), addr)
        except (ConnectionResetError, OSError):
            # Windows spits ICMP-port-unreachable back as an exception when the
            # peer isn't listening yet. Ignore; the punch loop keeps retrying.
            pass

    def _hello_destinations(self):
        """Where to aim HELLOs right now."""
        if self.listen:
            # An "open" side stays quiet until it hears the dialer, then aims
            # its HELLOs straight back at that source so its token gets echoed.
            return [self.peer] if self.peer else []
        return self.targets

    def _run(self):
        self.sock.setblocking(False)
        last_hello = 0.0
        last_keepalive = time.time()
        while not self._stop.is_set():
            now = time.time()

            if not self.connected.is_set():
                if now - last_hello >= HELLO_INTERVAL:
                    for dst in self._hello_destinations():
                        self._send_to(TYPE_HELLO, self.token, dst)
                    last_hello = now
            else:
                if now - last_keepalive >= KEEPALIVE_INTERVAL and self.peer:
                    self._send_to(TYPE_KEEPALIVE, self.token, self.peer)
                    last_keepalive = now

            try:
                ready, _, _ = select.select([self.sock], [], [], HELLO_INTERVAL)
            except (OSError, ValueError):
                break
            if not ready:
                continue
            try:
                data, addr = self.sock.recvfrom(65535)
            except (ConnectionResetError, OSError):
                continue

            ptype, payload = _unpack(data)
            if ptype is None:
                continue  # not one of ours -- ignore stray UDP

            # Lock onto the most-recent source (survives NAT re-mapping).
            self.peer = addr
            self._last_seen = now

            if ptype == TYPE_HELLO:
                # Echo THEIR token so they can confirm us.
                self._send_to(TYPE_ACK, payload, addr)
            elif ptype == TYPE_ACK:
                if payload == self.token and not self.connected.is_set():
                    # Proof the peer heard us -> we're connected.
                    self._send_to(TYPE_CONNECTED, self.token, addr)
                    self.connected.set()
                    self.log(f"[{self.name}] CONNECTED to {addr[0]}:{addr[1]}")
            elif ptype == TYPE_CONNECTED:
                # Peer says it's done; make sure we've flagged ourselves too.
                if not self.connected.is_set():
                    self.connected.set()
                    self.log(f"[{self.name}] CONNECTED (peer-driven) "
                             f"{addr[0]}:{addr[1]}")
            elif ptype == TYPE_KEEPALIVE:
                pass  # last_seen already refreshed above
            elif ptype == TYPE_DATA:
                self._inbox.put(payload)

    # -- public API -------------------------------------------------------- #
    def wait(self, timeout=None) -> bool:
        """Block until connected. Returns True on success."""
        return self.connected.wait(timeout)

    def send(self, data: bytes):
        if self.peer is None:
            raise RuntimeError("no peer resolved yet")
        self._send_to(TYPE_DATA, data, self.peer)

    def recv(self, timeout=None):
        """Return the next application datagram, or None on timeout/close."""
        try:
            return self._inbox.get(timeout=timeout)
        except queue.Empty:
            return None

    def last_seen_age(self):
        """Seconds since we last heard anything from the peer."""
        return time.time() - self._last_seen if self._last_seen else float("inf")

    @property
    def peer_str(self):
        return f"{self.peer[0]}:{self.peer[1]}" if self.peer else None

    def close(self):
        if self._owns_stop:
            self._stop.set()
        if self._thread.is_alive() and threading.current_thread() is not self._thread:
            self._thread.join(timeout=1.0)
        try:
            self.sock.close()
        except OSError:
            pass


# --------------------------------------------------------------------------- #
# Dial helpers
# --------------------------------------------------------------------------- #
def dial(targets, timeout=30, *, local_port=DEFAULT_PORT, family=socket.AF_INET,
         sock=None, name="peer", listen=False, stop_event=None, token=None,
         log=None):
    """Punch at one or more target (ip, port) pairs from a single socket.

    Returns a connected ``Connection`` on success, else None (socket closed).
    Pass an existing ``sock`` (e.g. the STUN socket) to keep the NAT mapping.
    """
    if isinstance(targets, tuple):
        targets = [targets]
    own_sock = sock is None
    if own_sock:
        sock = open_socket(local_port, family)
    conn = Connection(sock, targets, token=token, name=name, listen=listen,
                      stop_event=stop_event, log=log)
    if conn.wait(timeout):
        return conn
    conn.close()
    return None


def punch(target_ip, target_port, timeout=30, *, local_port=DEFAULT_PORT,
          name="peer", sock=None, stop_event=None, token=None, log=None):
    """Convenience single-target punch (the Phase-1 spec signature).

    Returns a connected ``Connection`` or None.
    """
    family = socket.AF_INET6 if ":" in target_ip else socket.AF_INET
    return dial((target_ip, int(target_port)), timeout, local_port=local_port,
                family=family, sock=sock, name=name, stop_event=stop_event,
                token=token, log=log)


# --------------------------------------------------------------------------- #
# Self-test: two loopback instances must both reach CONNECTED
# --------------------------------------------------------------------------- #
def selftest(base_port=29471, timeout=10) -> bool:
    """Spawn two Connections on 127.0.0.1 and assert both connect."""
    pa, pb = base_port, base_port + 1
    print(f"[selftest] A=127.0.0.1:{pa}  B=127.0.0.1:{pb}")

    log = lambda m: print("  " + m)
    results = {}

    def run(name, my_port, peer_port):
        conn = dial(("127.0.0.1", peer_port), timeout=timeout,
                    local_port=my_port, name=name, log=log)
        results[name] = conn

    ta = threading.Thread(target=run, args=("A", pa, pb))
    tb = threading.Thread(target=run, args=("B", pb, pa))
    ta.start(); tb.start()
    ta.join(); tb.join()

    a, b = results.get("A"), results.get("B")
    ok = bool(a and b and a.connected.is_set() and b.connected.is_set())

    if ok:
        # Prove the data path in both directions too.
        a.send(b"ping-from-A")
        b.send(b"ping-from-B")
        got_b = b.recv(timeout=2)
        got_a = a.recv(timeout=2)
        print(f"[selftest] A peer={a.peer_str}  B peer={b.peer_str}")
        print(f"[selftest] B received {got_b!r}   A received {got_a!r}")
        ok = got_b == b"ping-from-A" and got_a == b"ping-from-B"

    for c in (a, b):
        if c:
            c.close()

    print(f"[selftest] {'PASS' if ok else 'FAIL'}: both reached CONNECTED"
          f"{' and exchanged data' if ok else ''}")
    return ok


# --------------------------------------------------------------------------- #
# CLI
# --------------------------------------------------------------------------- #
def _chat_cli(conn: Connection):
    """Tiny interactive proof: type lines to send, incoming lines print."""
    print(f"[{conn.name}] connected to {conn.peer_str}. Type to send, "
          f"Ctrl-C to quit.")

    def reader():
        while True:
            msg = conn.recv(timeout=1)
            if msg is not None:
                print(f"\n<peer> {msg.decode('utf-8', 'replace')}")

    threading.Thread(target=reader, daemon=True).start()
    try:
        for line in sys.stdin:
            conn.send(line.rstrip("\n").encode("utf-8"))
    except KeyboardInterrupt:
        pass
    finally:
        conn.close()


def main(argv=None):
    ap = argparse.ArgumentParser(description="netpunch UDP hole-punch engine")
    ap.add_argument("--local-port", type=int, default=DEFAULT_PORT)
    ap.add_argument("--peer", help="IP:PORT of the peer to punch")
    ap.add_argument("--name", default="peer")
    ap.add_argument("--timeout", type=int, default=30)
    ap.add_argument("--selftest", action="store_true",
                    help="run the loopback two-instance self-test and exit")
    args = ap.parse_args(argv)

    if args.selftest:
        return 0 if selftest() else 1

    if not args.peer:
        ap.error("--peer IP:PORT is required (or use --selftest)")

    ip, _, port = args.peer.rpartition(":")
    ip = ip.strip("[]")  # allow [v6]:port
    conn = punch(ip, int(port), timeout=args.timeout,
                 local_port=args.local_port, name=args.name,
                 log=lambda m: print(m))
    if not conn:
        print(f"[{args.name}] FAILED to connect within {args.timeout}s")
        return 1
    _chat_cli(conn)
    return 0


if __name__ == "__main__":
    sys.exit(main())
