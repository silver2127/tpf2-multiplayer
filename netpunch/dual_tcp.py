"""A TCP link beside the UDP one, carrying every sealed frame a second time
(2026-09-17, an experiment the numbers decide).

WHAT: once a joiner and the host are connected over the punched UDP socket, a
TCP connection is made between them too -- the joiner connects to the host's
lobby port (reachable through the relay, a UPnP TCP mapping or a port
forward), and both sides also attempt a TCP simultaneous open toward the
address the other punched from, bound to their own lobby port, in case the
NAT preserves ports. Whichever lands first is the link. Every sealed frame
(type E: control messages, lockstep game frames) then goes out on BOTH: the
UDP socket as before and the TCP link. Bulk save chunks never do.

HOW THE COPY ARRIVES: a frame read from the TCP link is written into this
process's own UDP socket over loopback, prefixed with the ORIGINAL sender's
address (INJECT_MAGIC + ip + port), and DualSocket.recvfrom strips that and
returns (frame, original address). Every consumer -- the host loop, the
joiner's Connection thread, the mesh node -- therefore sees the TCP copy as
an ordinary datagram from the peer, wakes from its own select, and the seal
layer's per-sender replay window rejects whichever copy comes second. No
consumer changed.

WHAT IS MEASURED: per peer, from the nonce of every sealed frame that
arrives on either path, which path delivered it first, how much later the
other copy came (p50/p90/max), how many frames came over ONE path only
within 3 s -- `tcp_only` is a UDP loss the TCP copy covered, `udp_only` a
TCP copy that never came or came too late -- and the link's queue drops. A
line every 10 s: `[dual] <peer>: udp_first=.. tcp_first=.. tcp_only=..
udp_only=.. late p50=..ms p90=..ms max=..ms`. The lockstep bridge's own ARQ
still recovers what both paths lose.

OFF: tpf2mp_tcp_backup.txt containing 0 in the lobby's io dir, or an unsealed
session (a plaintext frame has no nonce to dedup on).
"""
import queue
import socket
import struct
import threading
import time

LINK_MAGIC = b"TPF2LINK1"
INJECT_MAGIC = b"TPF2DUAL"
MAX_FRAME = 65535
QUEUE_MAX = 1024              # frames waiting for a stalled TCP link before the oldest is dropped
DIAL_FOR = 30.0               # seconds the simultaneous open keeps trying
DIAL_EVERY = 1.0
AGE_OUT = 3.0                 # a frame seen on one path only for this long counts as udp_only / tcp_only
STATS_EVERY = 10.0
PACK_TYPE_EDATA = b"E"
MAGIC_LEN = 4                 # 'NP1:'


def _pack_addr(addr):
    ip = addr[0].encode("ascii", "replace")
    return INJECT_MAGIC + struct.pack("!BH", len(ip), int(addr[1])) + ip


def _unpack_addr(data):
    if not data.startswith(INJECT_MAGIC) or len(data) < len(INJECT_MAGIC) + 3:
        return None, None
    n, port = struct.unpack_from("!BH", data, len(INJECT_MAGIC))
    off = len(INJECT_MAGIC) + 3
    ip = data[off:off + n].decode("ascii", "replace")
    return (ip, port), data[off + n:]


class DualStats:
    """Per-peer accounting of which path delivered each sealed frame first."""

    def __init__(self, log, clock=time.time):
        self.log, self.clock = log, clock
        self.peers = {}          # key -> counters
        self.seen = {}           # (key, nonce) -> (t_first, path)
        self._last_log = clock()
        self._lock = threading.Lock()

    def _p(self, key):
        p = self.peers.get(key)
        if p is None:
            p = self.peers[key] = dict(udp_first=0, tcp_first=0, tcp_only=0, udp_only=0,
                                       tcp_late=[], udp_late=[], dropped=0)
        return p

    def arrival(self, key, nonce, path):
        now = self.clock()
        with self._lock:
            first = self.seen.get((key, nonce))
            p = self._p(key)
            if first is None:
                self.seen[(key, nonce)] = (now, path)
                p["udp_first" if path == "udp" else "tcp_first"] += 1
            else:
                t0, path0 = first
                if path0 != path:
                    lag = (now - t0) * 1000.0
                    lst = p["tcp_late"] if path == "tcp" else p["udp_late"]   # how much later the second copy came
                    lst.append(lag)
                    if len(lst) > 4096:
                        del lst[:2048]
                    del self.seen[(key, nonce)]

    def dropped(self, key):
        with self._lock:
            self._p(key)["dropped"] += 1

    def tick(self, names=None):
        """Age out single-path frames; log a line every STATS_EVERY seconds."""
        now = self.clock()
        with self._lock:
            for k, (t0, path0) in list(self.seen.items()):
                if now - t0 >= AGE_OUT:
                    del self.seen[k]
                    self._p(k[0])["udp_only" if path0 == "udp" else "tcp_only"] += 1
            if now - self._last_log < STATS_EVERY:
                return
            self._last_log = now
            for key, p in self.peers.items():
                total = p["udp_first"] + p["tcp_first"]
                if not total:
                    continue
                who = names.get(key, key) if names else key
                self.log(f"[dual] {who}: udp_first={p['udp_first']} tcp_first={p['tcp_first']} "
                         f"tcp_only={p['tcp_only']} (udp lost, tcp covered) udp_only={p['udp_only']} "
                         f"(tcp lost/late); tcp later by {self._q(p['tcp_late'])}, udp later by {self._q(p['udp_late'])}; "
                         f"queue_drops={p['dropped']}")

    @staticmethod
    def _q(values):
        if not values:
            return "-"
        s = sorted(values)
        pick = lambda f: s[min(len(s) - 1, int(f * len(s)))]
        return f"p50={pick(0.5):.0f}ms p90={pick(0.9):.0f}ms max={s[-1]:.0f}ms (n={len(s)})"


class TcpLink:
    """One established TCP link to one peer: a writer thread with a bounded
    queue (a stalled link never blocks the lobby loop), a reader thread that
    injects every frame into the UDP socket for the consumers."""

    def __init__(self, sock, peer_addr, inject, log, on_close=None, how="?"):
        self.sock, self.peer_addr, self.inject, self.log = sock, peer_addr, inject, log
        self.on_close = on_close
        self.how = how
        self.q = queue.Queue()
        self.alive = True
        self.sent = self.received = self.dropped = 0
        self.delay = 0.0              # netsim: seconds each frame waits before it leaves
        sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        threading.Thread(target=self._writer, name="dual-w", daemon=True).start()
        threading.Thread(target=self._reader, name="dual-r", daemon=True).start()

    def send(self, frame):
        if not self.alive:
            return False
        if self.q.qsize() >= QUEUE_MAX:
            try:
                self.q.get_nowait()
                self.dropped += 1
            except queue.Empty:
                pass
        # netsim: a pipeline delay -- each frame leaves `delay` after it was
        # queued, never one behind the other (a sleep per frame serialised them)
        self.q.put((time.time() + self.delay, frame))
        return True

    def close(self):
        if self.alive:
            self.alive = False
            try:
                self.sock.close()
            except OSError:
                pass
            self.q.put(None)
            if self.on_close:
                self.on_close(self)

    def _writer(self):
        try:
            while self.alive:
                item = self.q.get()
                if item is None:
                    break
                due, frame = item
                wait = due - time.time()
                if wait > 0:
                    time.sleep(wait)
                self.sock.sendall(struct.pack("!H", len(frame)) + frame)
                self.sent += 1
        except OSError:
            pass
        self.close()

    def _reader(self):
        buf = b""
        try:
            while self.alive:
                data = self.sock.recv(65536)
                if not data:
                    break
                buf += data
                while len(buf) >= 2:
                    n = struct.unpack_from("!H", buf)[0]
                    if len(buf) < 2 + n:
                        break
                    frame, buf = buf[2:2 + n], buf[2 + n:]
                    self.received += 1
                    self.inject(self.peer_addr, frame)
        except OSError:
            pass
        self.close()


class DualSocket:
    """Wraps the process's UDP socket: sendto also queues sealed frames to the
    peer's TCP link; recvfrom unwraps frames the links injected over loopback.
    Everything else is the wrapped socket's."""

    def __init__(self, udp, log, stats=None, key_of=lambda addr: addr):
        self.udp = udp
        self.log = log
        self.links = {}                       # peer addr -> TcpLink
        self.stats = stats or DualStats(log)
        self.key_of = key_of                  # addr -> the key stats are kept under
        self.enabled = True
        self.link_delay = 0.0                 # netsim: applied to every link attached
        self._inj = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self._inj.bind(("127.0.0.1", 0))
        self._inj_from = self._inj.getsockname()
        port = udp.getsockname()[1]
        self._self = ("127.0.0.1", port)
        self._lock = threading.Lock()

    # -- the socket surface the lobby uses --------------------------------------
    def __getattr__(self, name):
        return getattr(self.udp, name)

    def fileno(self):
        return self.udp.fileno()

    def sendto(self, data, addr):
        n = self.udp.sendto(data, addr)
        if self.enabled and data[MAGIC_LEN:MAGIC_LEN + 1] == PACK_TYPE_EDATA and len(data) <= MAX_FRAME:
            link = self.links.get(addr)
            if link is not None and not link.send(data):
                pass
        return n

    def recvfrom(self, n=65535):
        data, src = self.udp.recvfrom(n)
        if src == self._inj_from:
            addr, frame = _unpack_addr(data)
            if addr is not None:
                self._note(addr, frame, "tcp")
                return frame, addr
            return data, src
        if data[MAGIC_LEN:MAGIC_LEN + 1] == PACK_TYPE_EDATA:
            self._note(src, data, "udp")
        return data, src

    # -- links ------------------------------------------------------------------
    def _note(self, addr, frame, path):
        if self.enabled and len(frame) >= MAGIC_LEN + 1 + 8:
            self.stats.arrival(self.key_of(addr), frame[MAGIC_LEN + 1:MAGIC_LEN + 9], path)

    def inject(self, addr, frame):
        try:
            self._inj.sendto(_pack_addr(addr) + frame, self._self)
        except OSError:
            pass

    def attach(self, sock, addr, how):
        with self._lock:
            old = self.links.get(addr)
            if old is not None and old.alive:
                sock.close()                  # one link per peer: the first one stands
                return old
            link = TcpLink(sock, addr, self.inject, self.log, on_close=self._gone, how=how)
            link.delay = self.link_delay
            self.links[addr] = link
        self.log(f"[dual] TCP link with {addr[0]}:{addr[1]} up ({how})")
        return link

    def _gone(self, link):
        with self._lock:
            if self.links.get(link.peer_addr) is link:
                del self.links[link.peer_addr]
        self.log(f"[dual] TCP link with {link.peer_addr[0]}:{link.peer_addr[1]} closed "
                 f"(sent {link.sent}, received {link.received}, queue drops {link.dropped})")

    def has_link(self, addr):
        link = self.links.get(addr)
        return link is not None and link.alive

    def set_link_delay(self, seconds):
        """netsim: the delay every link frame waits, current links included."""
        self.link_delay = seconds
        for link in self.links.values():
            link.delay = seconds

    def tick(self, names=None):
        self.stats.tick(names)

    def close_links(self):
        for link in list(self.links.values()):
            link.close()
        try:
            self._inj.close()
        except OSError:
            pass


# -- establishing a link ---------------------------------------------------------
def hello_bytes(name, sealed_name):
    return LINK_MAGIC + b" " + name.encode("utf-8", "replace")[:64] + b" " + sealed_name.hex().encode() + b"\n"


def parse_hello(line):
    """(name, sealed bytes) from a hello line, or (None, None)."""
    parts = line.strip().split(b" ", 2)
    if len(parts) != 3 or parts[0] != LINK_MAGIC:
        return None, None
    try:
        return parts[1].decode("utf-8", "replace"), bytes.fromhex(parts[2].decode("ascii"))
    except ValueError:
        return None, None


def dial(target, local_port, hello, log, stop=None, listen_too=True, for_seconds=DIAL_FOR):
    """A thread's work: a TCP simultaneous open toward ``target`` from
    ``local_port`` -- connect attempts every second, and (listen_too) a
    listener on that same port for the other side's attempts -- for up to
    ``for_seconds``. Returns (socket, how) or (None, None). The caller sends
    ``hello`` on a connection IT made; an accepted one reads the peer's."""
    deadline = time.time() + for_seconds
    lst = None
    if listen_too:
        try:
            lst = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
            lst.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
            lst.bind(("0.0.0.0", local_port))
            lst.listen(4)
            lst.settimeout(0.2)
        except OSError as e:
            log(f"[dual] no listener on tcp/{local_port} for the simultaneous open ({e})")
            lst = None
    try:
        while time.time() < deadline and not (stop and stop.is_set()):
            c = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
            try:
                c.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
                c.bind(("0.0.0.0", local_port))
                c.settimeout(DIAL_EVERY)
                c.connect(target)
                c.sendall(hello)
                c.settimeout(None)
                return c, "connected"
            except OSError:
                c.close()
            if lst is not None:
                try:
                    a, addr = lst.accept()
                    a.settimeout(5.0)
                    return a, "accepted"
                except socket.timeout:
                    pass
                except OSError:
                    pass
            else:
                time.sleep(DIAL_EVERY)
    finally:
        if lst is not None:
            lst.close()
    return None, None


def read_hello(sock, timeout=5.0):
    """The hello line from an accepted connection, or None."""
    sock.settimeout(timeout)
    line = b""
    try:
        while not line.endswith(b"\n") and len(line) < 256:
            piece = sock.recv(256 - len(line))
            if not piece:
                return None
            line += piece
    except OSError:
        return None
    sock.settimeout(None)
    return line
