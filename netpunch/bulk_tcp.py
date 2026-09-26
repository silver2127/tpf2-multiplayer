"""A TCP side channel for the save and mod transfers (2026-09-17).

The lobby's own transfer is a receiver-driven selective-repeat ARQ over the
punched UDP socket: one Python pack and sendto per 1,350-byte chunk, a
2.76 MB window (so throughput = window / round trip: ~15 MB/s at 185 ms),
and a whole-window rewind after half a second without feedback. Measured on
loopback: ~11 MB/s per peer clean, ~5 MB/s at 15% loss, against ~2 GB/s
for a plain TCP stream on the same machine. Over the internet the kernel's
own window and congestion control reach the link's speed; the ARQ often
does not.

So a transfer opens ONE extra TCP connection per receiver and streams the
file down it, and everything else -- fbegin, the feedback the receiver sends
every 50 ms (its base still advances, so the host's progress, stage and
timeouts see nothing new), verify by hash, fdone, start -- stays as it is.
The side behind NAT always makes the connection (a joiner to the host or
relay; the leader to the relay for its upload), so no TCP hole punching is
attempted; a host that is not reachable on TCP costs one failed connect
and the transfer runs over UDP as before. If a stream breaks half way the
receiver's feedback names the holes and the UDP path resumes from there.

Wire: the connecting side writes one hello line, `TPF2BULK1 <role> <sid>
<token> <name>\\n`; the listener answers `OK\\n` and the sender then writes
the file bytes, in order from offset 0, nothing else. The token is 16
random bytes the sender put in its fbegin (sealed like every control
message), so an unrelated connection cannot claim or feed a transfer.
"""
import socket
import sys
import threading
import time

BULK_MAGIC = b"TPF2BULK1"
HELLO_TIMEOUT = 5.0          # a connection that has not said hello by then is dropped
CONNECT_TIMEOUT = 3.0        # a host not reachable on TCP costs this once, then UDP
SEND_BLOCK = 1 << 20         # sendall() slices
RECV_BLOCK = 4 << 20         # recv() size
PENDING_MAX = 32             # connections waiting to say hello at once


class BulkListener:
    """The host's (or relay's) TCP listener on the lobby port. Never raises
    out of the accept thread; a listener that cannot bind is None to callers."""

    def __init__(self, port, log=lambda _: None, bind="0.0.0.0"):
        self.port = int(port)
        self.log = log
        self._expect = {}        # (sid, role) -> (token, handler)
        self._lock = threading.Lock()
        self._stop = threading.Event()
        self._pending = 0
        self.accepted = self.refused = 0
        self.link_handler = None   # dual_tcp: a "TPF2LINK1 ..." hello is a peer's TCP link, not a transfer
        s = self._listen(socket.AF_INET, bind, self.port)
        self.sock = s
        self.port = s.getsockname()[1]
        self.sockets = [s]
        # Separate sockets preserve IPv4 peer addresses and keep IPv4 working
        # even on machines without IPv6. Never widen an explicit IPv4 bind.
        if bind == "0.0.0.0":
            try:
                self.sockets.append(self._listen(socket.AF_INET6, "::", self.port))
            except OSError as e:
                log(f"[bulk] IPv6 TCP unavailable on tcp/{self.port}: {e}; IPv4 remains available")
        for listener in self.sockets:
            threading.Thread(target=self._accept_loop, args=(listener,), name="bulk-accept", daemon=True).start()

    @staticmethod
    def _listen(family, bind, port):
        s = socket.socket(family, socket.SOCK_STREAM)
        try:
            s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
            # Bound Linux dial sockets need SO_REUSEPORT on both ends.
            if sys.platform.startswith("linux"):
                s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEPORT, 1)
            if family == socket.AF_INET6:
                s.setsockopt(socket.IPPROTO_IPV6, socket.IPV6_V6ONLY, 1)
            s.bind((bind, port))
            s.listen(16)
            s.settimeout(0.5)
            return s
        except OSError:
            s.close()
            raise

    @classmethod
    def open(cls, port, log=lambda _: None):
        """A listener, or None (logged) when the port cannot be bound."""
        try:
            lst = cls(port, log)
        except OSError as e:
            log(f"[bulk] no TCP listener on {port} ({e}); transfers use UDP only")
            return None
        families = "IPv4 + IPv6" if len(lst.sockets) > 1 else "IPv4"
        log(f"[bulk] TCP transfers accepted on tcp/{lst.port} ({families})")
        return lst

    def expect(self, sid, role, token, handler):
        """Accept a hello for (sid, role) carrying ``token``; ``handler(sock,
        addr, name)`` then owns the socket on the accept thread's helper."""
        with self._lock:
            self._expect[(int(sid), role)] = (str(token), handler)

    def forget(self, sid):
        with self._lock:
            for key in [k for k in self._expect if k[0] == int(sid)]:
                del self._expect[key]

    def close(self):
        self._stop.set()
        for listener in self.sockets:
            try:
                listener.close()
            except OSError:
                pass

    def _accept_loop(self, listener):
        while not self._stop.is_set():
            try:
                c, addr = listener.accept()
            except socket.timeout:
                continue
            except OSError:
                break
            with self._lock:
                if self._pending >= PENDING_MAX:
                    c.close()
                    continue
                self._pending += 1
            threading.Thread(target=self._hello, args=(c, addr), name="bulk-hello", daemon=True).start()

    def _hello(self, c, addr):
        try:
            c.settimeout(HELLO_TIMEOUT)
            line = b""
            while not line.endswith(b"\n") and len(line) < 256:
                piece = c.recv(256 - len(line))
                if not piece:
                    break
                line += piece
            if line.startswith(b"TPF2LINK1 ") and self.link_handler is not None:
                c.settimeout(None)
                with self._lock:
                    self._pending -= 1
                self.link_handler(c, addr, line)
                return
            parts = line.strip().split(b" ", 4)
            if len(parts) < 4 or parts[0] != BULK_MAGIC:
                raise ValueError("bad hello")
            role, sid, token = parts[1].decode("ascii", "replace"), int(parts[2]), parts[3].decode("ascii", "replace")
            name = parts[4].decode("utf-8", "replace") if len(parts) > 4 else ""
            with self._lock:
                want = self._expect.get((sid, role))
            if not want or want[0] != token:
                raise ValueError("unknown transfer or wrong token")
            c.sendall(b"OK\n")
            c.settimeout(None)
            self.accepted += 1
        except (OSError, ValueError, UnicodeDecodeError) as e:
            self.refused += 1
            self.log(f"[bulk] refused a TCP connection from {addr[0]}: {e}")
            try:
                c.close()
            except OSError:
                pass
            with self._lock:
                self._pending -= 1
            return
        with self._lock:
            self._pending -= 1
        try:
            want[1](c, addr, name)
        except Exception as e:                       # noqa: BLE001 -- a handler bug must not kill the thread pool
            self.log(f"[bulk] handler error for sid={sid} {role}: {e!r}")
            try:
                c.close()
            except OSError:
                pass


def _why(e):
    """A connect failure in words: a timeout means something dropped the SYN (a
    firewall or a router without a mapping), a refusal that nothing listens."""
    if isinstance(e, socket.timeout):
        return "timed out (blocked: firewall or no port mapping)"
    if isinstance(e, ConnectionRefusedError):
        return "refused (nothing listening on that port)"
    return f"{type(e).__name__}: {e}"


def bulk_connect(host, port, role, sid, token, name="", timeout=CONNECT_TIMEOUT, errors=None):
    """Connect to a listener and say hello. A socket ready for the stream, or
    None (the caller falls back to UDP). ``errors``, a list, gets one
    'host: why' line per failure (the logs said only "no TCP stream")."""
    try:
        c = socket.create_connection((host, int(port)), timeout=timeout)
        c.sendall(BULK_MAGIC + b" " + role.encode() + b" " + str(int(sid)).encode() + b" " + str(token).encode()
                  + b" " + name.encode("utf-8", "replace")[:64] + b"\n")
        c.settimeout(timeout)
        # EXACTLY the three bytes of "OK\n", never more: the sender's stream
        # follows its OK at once, and a read of up to 8 bytes took the first
        # payload bytes with it whenever they arrived in the same segment. The
        # reply then was not "OK", this end closed, and the host saw "the TCP
        # stream broke after 1048576 B -- UDP takes over": 200 MB at 1350 B a
        # datagram, 13 s instead of 0.6 (twice in a 140-batch round, 2026-09-20).
        ok = b""
        while len(ok) < 3:
            piece = c.recv(3 - len(ok))
            if not piece:
                break
            ok += piece
        if ok != b"OK\n":
            c.close()
            if errors is not None:
                errors.append(f"{host}: connected, but the listener refused the hello")
            return None
        c.settimeout(None)
        return c
    except (OSError, UnicodeEncodeError) as e:
        if errors is not None:
            errors.append(f"{host}: {_why(e)}")
        return None


def stream_send(sock, blob, progress=None, block=SEND_BLOCK):
    """Write ``blob`` down the socket in order. True when all of it went."""
    sent = 0
    try:
        view = memoryview(blob)
        while sent < len(view):
            piece = view[sent:sent + block]
            sock.sendall(piece)
            sent += len(piece)
            if progress:
                progress(sent)
        return True
    except OSError:
        return False
    finally:
        try:
            sock.close()
        except OSError:
            pass


def stream_recv(sock, total, sink, block=RECV_BLOCK):
    """Read ``total`` bytes, handing each block to ``sink(bytes)`` (on this
    thread). True when all of it came."""
    got = 0
    try:
        while got < total:
            data = sock.recv(min(block, total - got))
            if not data:
                return False
            got += len(data)
            sink(data)
        return True
    except OSError:
        return False
    finally:
        try:
            sock.close()
        except OSError:
            pass


def rate_text(nbytes, seconds):
    return f"{nbytes / 1e6:.1f} MB in {seconds:.1f} s ({nbytes / max(seconds, 1e-6) / 1e6:.1f} MB/s)"


if __name__ == "__main__":
    # loopback self-check: one listener, one connect, a 64 MB stream both ways
    import os
    log = print
    lst = BulkListener.open(0, log)
    blob = os.urandom(64 << 20)
    got = bytearray()
    done = threading.Event()

    def serve(c, addr, name):
        t = time.perf_counter()
        ok = stream_send(c, blob)
        log(f"served {name!r}: {ok} {rate_text(len(blob), time.perf_counter() - t)}")

    lst.expect(7, "recv", "tok", serve)
    c = bulk_connect("127.0.0.1", lst.port, "recv", 7, "tok", "me")
    assert c is not None
    t = time.perf_counter()
    assert stream_recv(c, len(blob), got.extend)
    log(f"received {rate_text(len(got), time.perf_counter() - t)}; equal={bytes(got) == blob}")
    assert bulk_connect("127.0.0.1", lst.port, "recv", 7, "wrong", "me") is None
    lst.close()
    print("bulk_tcp self-check OK")
