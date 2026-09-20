#!/usr/bin/env python3
"""tpf2mp master server -- the OpenTTD-style public game list, and desync reports.

A host that ticks PUBLIC announces its lobby here every 10 s; the title-menu
panel lists what is announced and pasting a row's code joins it. Nothing is
brokered: the code IS the join mechanism (it carries the host's address and
the session secret), this server only stores and repeats it. No accounts,
no auth: a public lobby is public by choice, and a password-locked lobby
shows as locked (its code alone does not get anyone in).

    POST /announce   {"id","name","code","players","max","type","version","locked"}
    POST /leave      {"id"}
    GET  /list       {"servers":[{... , "age": seconds since last announce}], "now": unix}
    GET  /health     "ok"
    POST /ping       {"sid", "players", "max", "type", "version", "public"}
                     -- a session heartbeat every PING_EVERY s from EVERY lobby, listed
                     or not: an anonymous count (a random per-run id; no name, no code,
                     no address kept). report_sessions=0 / --no-ping turns it off.
    GET  /stats      {"sessions", "players", "public", "private", "by_version",
                     "by_type", "listed", "hours": [...]} -- aggregates only
    POST /desync     a zip of a player's scrubbed logs (netpunch/desynclogs.py), metadata
                     as JSON in the X-Tpf2mp-Meta header -> {"ok":true,"id":...}
    POST /knock      {"s": session tag, "blob": base64[, "relay": 1, "j": nonce]}
                     -- a joiner's sealed address note; with "relay" the reply carries
                     a UDP relay allocation {"relay": {"ip", "port", "id"}}
    GET  /knock?s=<tag>&since=<unix>   {"knocks":[{"t","blob"[, "relay": {...}]}], "now": unix}

KNOCKS are the rendezvous for hole punching (lobby.py _Rendezvous*). A joiner
posts its STUN-observed address, sealed with the session key, under a tag derived
from the code's secret; the host polls its tag and fires packets back at the
joiner, which opens the host's own NAT for the joiner's HELLOs. This server can
neither read a note nor tell which lobby a tag belongs to; notes live KNOCK_TTL
seconds.

RELAY (--relay-ip): the fallback when the punch cannot work (a host behind CGNAT,
a joiner behind a symmetric NAT). A joiner still unanswered asks for it in its
knock; the server binds a UDP port of --relay-ports for that joiner and tells
both ends (the joiner in the reply, the host with the note). Each end sends a
bind packet TRLB|id(16)|role to that port, after which the port swaps every
datagram between the two addresses it learned, verbatim: the frames stay sealed
with the session key, this server forwards bytes it cannot read. An allocation
dies after RELAY_IDLE seconds without traffic.

Entries expire TTL seconds after their last announce. Desync reports are kept in
--desync-dir (disabled without it): at most UPLOAD_MAX bytes each, a few per
address per hour, and the oldest are deleted beyond UPLOAD_DISK_CAP. The
announcer's address is never stored with a report.

Bound to localhost; nginx proxies https://<host>/tpf2mp/ to it. Stdlib only, one
file, runs as a systemd service (see the deploy step in tools/masterserver_deploy.sh).
"""
import argparse, io, json, os, re, secrets, select, socket, sys, time, threading, zipfile
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

TTL = 30.0           # seconds an entry lives without a fresh announce (lobbies announce every 10 s)
MAX_BODY = 4096
MAX_ENTRIES = 500
SESSION_TTL = 180.0  # seconds a session counts without a fresh ping (lobbies ping every 60 s)
PING_EVERY = 60
STATS_FILE = None    # --stats-file: one JSON line per finished hour
_sessions = {}       # sid -> {"players","max","type","version","public","at","first"}
_hours = {}          # hour (unix // 3600) -> {"sids": set, "peak_sessions", "peak_players", "versions": {}}
_SID_RE = re.compile(r"^[0-9a-f]{8,32}$")
FIELDS = ("id", "name", "code", "players", "max", "game", "type", "version", "locked")
# The list shows what KIND of server a row is, never the host's save name
# (2026-09-10): a save's file name ("multi Balage", "autosave 3") read as
# nonsense and was not even the world START GAME ends up sharing.
TYPE_LABELS = {"relay": "dedicated server", "dedicated": "dedicated server", "host": "player hosted"}

UPLOAD_MAX = 16 << 20          # nginx's client_max_body_size for /tpf2mp/desync matches
UPLOAD_PER_IP_HOUR = 6
UPLOAD_PER_DAY = 300
UPLOAD_DISK_CAP = 2 << 30
UPLOAD_MAX_ENTRIES = 500       # files inside one zip
DESYNC_DIR = None              # --desync-dir

KNOCK_TTL = 60.0              # seconds a joiner's note waits for the host's poll
KNOCK_PER_TAG = 16
KNOCK_MAX_TAGS = 2000
KNOCK_BLOB_MAX = 1024          # base64 characters
KNOCK_PER_IP_MIN = 40          # posts per address per minute (a joiner posts every 2 s)
_TAG_RE = re.compile(r"^[0-9a-f]{24}$")
_NONCE_RE = re.compile(r"^[0-9a-f]{8,16}$")

RELAY_IP = None                # --relay-ip: the address joiners and hosts send to; None = no relay
RELAY_BIND = "0.0.0.0"
RELAY_PORTS = (29600, 29699)   # --relay-ports a-b: one UDP port per relayed joiner
RELAY_IDLE = 90.0              # seconds without a datagram before an allocation is closed
RELAY_LIFE = 12 * 3600.0
RELAY_MAGIC = b"TRLB"          # bind: TRLB | id(16) | role (b"H" host, b"J" joiner); the reply is TRLA | id
RELAY_ACK = b"TRLA"

_lock = threading.Lock()
_servers = {}        # id -> dict(fields..., "at": last announce, "ip": announcer)
_knock_lock = threading.Lock()
_knocks = {}         # tag -> [(unix, blob, relay dict or None), ...] newest last
_knock_by_ip = {}    # ip -> [unix times of posts in the last minute]
_up_lock = threading.Lock()
_up_by_ip = {}       # ip -> [unix times of accepted uploads in the last hour]
_up_all = []         # unix times of accepted uploads in the last day


def _clean_sessions(now):
    dead = [k for k, v in _sessions.items() if now - v["at"] > SESSION_TTL]
    for k in dead:
        del _sessions[k]
    # finished hours go to the stats file once, and stay in memory for a day
    h = int(now // 3600)
    for k in [k for k in _hours if k < h and not _hours[k].get("flushed")]:
        row = _hours[k]
        row["flushed"] = True
        if STATS_FILE:
            try:
                with open(STATS_FILE, "a", encoding="utf-8") as f:
                    f.write(json.dumps({"hour": k * 3600, "sessions": len(row["sids"]), "peak_sessions": row["peak_sessions"],
                                        "peak_players": row["peak_players"], "by_version": row["versions"]}) + "\n")
            except OSError as e:
                sys.stderr.write("stats file: %s\n" % e)
    for k in [k for k in _hours if k < h - 24]:
        del _hours[k]


def _stats(now):
    """Aggregates only: how many sessions and players right now, split public/private,
    by version and type, and the last 24 finished hours."""
    _clean_sessions(now)
    by_version, by_type = {}, {}
    players = public = 0
    for s in _sessions.values():
        players += s["players"]
        public += 1 if s["public"] else 0
        by_version[s["version"]] = by_version.get(s["version"], 0) + 1
        by_type[s["type"]] = by_type.get(s["type"], 0) + 1
    hours = [{"hour": k * 3600, "sessions": len(v["sids"]), "peak_sessions": v["peak_sessions"], "peak_players": v["peak_players"]}
             for k, v in sorted(_hours.items())]
    return {"now": int(now), "sessions": len(_sessions), "players": players, "public": public,
            "private": len(_sessions) - public, "by_version": by_version, "by_type": by_type,
            "listed": len(_servers), "hours": hours, "ttl": int(SESSION_TTL)}


def _clean(now):
    dead = [k for k, v in _servers.items() if now - v["at"] > TTL]
    for k in dead:
        del _servers[k]


def _s(v, n):
    return str(v)[:n] if v is not None else ""


def _safe(v, n):
    return re.sub(r"[^A-Za-z0-9._-]", "_", str(v or ""))[:n] or "x"


def _upload_allowed(ip, now):
    """Counts the upload when it is allowed."""
    with _up_lock:
        for k in list(_up_by_ip):
            _up_by_ip[k] = [t for t in _up_by_ip[k] if now - t < 3600]
            if not _up_by_ip[k]:
                del _up_by_ip[k]
        _up_all[:] = [t for t in _up_all if now - t < 86400]
        if len(_up_by_ip.get(ip, ())) >= UPLOAD_PER_IP_HOUR or len(_up_all) >= UPLOAD_PER_DAY:
            return False
        _up_by_ip.setdefault(ip, []).append(now)
        _up_all.append(now)
        return True


def _knock_post(tag, blob, ip, now, relay=None):
    """-> None when stored, else an (http status, error) pair."""
    with _knock_lock:
        for k in list(_knocks):
            _knocks[k] = [e for e in _knocks[k] if now - e[0] < KNOCK_TTL]
            if not _knocks[k]:
                del _knocks[k]
        for k in list(_knock_by_ip):
            _knock_by_ip[k] = [t for t in _knock_by_ip[k] if now - t < 60]
            if not _knock_by_ip[k]:
                del _knock_by_ip[k]
        if len(_knock_by_ip.get(ip, ())) >= KNOCK_PER_IP_MIN:
            return 429, "too many knocks"
        if tag not in _knocks and len(_knocks) >= KNOCK_MAX_TAGS:
            return 503, "full"
        _knock_by_ip.setdefault(ip, []).append(now)
        lst = _knocks.setdefault(tag, [])
        lst.append((now, blob, relay))
        del lst[:-KNOCK_PER_TAG]
    return None


def _knock_get(tag, since, now):
    with _knock_lock:
        out = []
        for t, b, r in _knocks.get(tag, ()):
            if t > since and now - t < KNOCK_TTL:
                e = {"t": t, "blob": b}
                if r:
                    e["relay"] = r
                out.append(e)
        return out


# --------------------------------------------------------------------------- #
# UDP relay: one port per relayed joiner, swapping datagrams between two addresses
# --------------------------------------------------------------------------- #
class _Alloc:
    __slots__ = ("sock", "port", "id", "key", "joiner", "host", "last", "created", "packets")

    def __init__(self, sock, port, aid, key, now):
        self.sock, self.port, self.id, self.key = sock, port, aid, key
        self.joiner = self.host = None
        self.last = self.created = now
        self.packets = 0

    def public(self):
        return {"ip": RELAY_IP, "port": self.port, "id": self.id.hex()}


_relay_lock = threading.Lock()
_allocs = {}          # id bytes -> _Alloc
_alloc_by_key = {}    # (tag, nonce) -> id bytes
_relay_wake = None    # a socketpair-less wakeup: the loop polls every 0.5 s instead


def _relay_alloc(tag, nonce, now):
    """-> the allocation's public dict, or None when the relay is off or full."""
    if not RELAY_IP:
        return None
    with _relay_lock:
        aid = _alloc_by_key.get((tag, nonce))
        a = _allocs.get(aid) if aid else None
        if a is not None:
            a.last = now
            return a.public()
        used = {a.port for a in _allocs.values()}
        for port in range(RELAY_PORTS[0], RELAY_PORTS[1] + 1):
            if port in used:
                continue
            s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
            try:
                s.bind((RELAY_BIND, port))
            except OSError:
                s.close()
                continue
            s.setblocking(False)
            aid = secrets.token_bytes(16)
            a = _Alloc(s, port, aid, (tag, nonce), now)
            _allocs[aid] = a
            _alloc_by_key[(tag, nonce)] = aid
            sys.stderr.write("relay: port %d allocated for tag %s..\n" % (port, tag[:8]))
            return a.public()
    return None


def _relay_close(a, why):
    with _relay_lock:
        _allocs.pop(a.id, None)
        if _alloc_by_key.get(a.key) == a.id:
            del _alloc_by_key[a.key]
    try:
        a.sock.close()
    except OSError:
        pass
    sys.stderr.write("relay: port %d closed (%s, %d datagrams)\n" % (a.port, why, a.packets))


def _relay_step(a, now):
    """Drain one allocation's socket: binds, then forwarding between its two ends."""
    for _ in range(256):
        try:
            data, addr = a.sock.recvfrom(65535)
        except (BlockingIOError, InterruptedError):
            return
        except (ConnectionResetError, OSError):
            return
        a.last = now
        if len(data) == 21 and data[:4] == RELAY_MAGIC:
            if data[4:20] != a.id:
                continue
            role = data[20:21]
            if role == b"H":
                if a.host != addr:
                    sys.stderr.write("relay: port %d host bound from %s:%d\n" % (a.port, addr[0], addr[1]))
                a.host = addr
            elif role == b"J":
                if a.joiner != addr:
                    sys.stderr.write("relay: port %d joiner bound from %s:%d\n" % (a.port, addr[0], addr[1]))
                a.joiner = addr
            else:
                continue
            try:
                a.sock.sendto(RELAY_ACK + a.id, addr)
            except OSError:
                pass
            continue
        dst = a.host if addr == a.joiner else a.joiner if addr == a.host else None
        if dst is None:
            continue
        a.packets += 1
        try:
            a.sock.sendto(data, dst)
        except OSError:
            pass


def _relay_loop():
    while True:
        with _relay_lock:
            allocs = list(_allocs.values())
        now = time.time()
        for a in allocs:
            if now - a.last > RELAY_IDLE:
                _relay_close(a, "idle")
            elif now - a.created > RELAY_LIFE:
                _relay_close(a, "lifetime")
        with _relay_lock:
            socks = {a.sock: a for a in _allocs.values()}
        if not socks:
            time.sleep(0.5)
            continue
        try:
            ready, _, _ = select.select(list(socks), [], [], 0.5)
        except (OSError, ValueError):
            time.sleep(0.1)
            continue
        now = time.time()
        for s in ready:
            a = socks.get(s)
            if a is not None:
                _relay_step(a, now)


def start_relay():
    threading.Thread(target=_relay_loop, name="relay", daemon=True).start()


def _prune(directory):
    try:
        entries = [e for e in os.scandir(directory) if e.is_file()]
    except OSError:
        return
    entries.sort(key=lambda e: e.stat().st_mtime)
    total = sum(e.stat().st_size for e in entries)
    while entries and total > UPLOAD_DISK_CAP:
        e = entries.pop(0)
        try:
            total -= e.stat().st_size
            os.remove(e.path)
        except OSError:
            pass


class H(BaseHTTPRequestHandler):
    server_version = "tpf2mp-master/1"

    def _send(self, code, obj):
        body = (obj if isinstance(obj, (bytes, bytearray)) else json.dumps(obj).encode("utf-8"))
        self.send_response(code)
        self.send_header("Content-Type", "application/json; charset=utf-8" if not isinstance(obj, bytes) else "text/plain")
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Cache-Control", "no-store")
        self.send_header("Access-Control-Allow-Origin", "*")
        self.end_headers()
        self.wfile.write(body)

    def _body(self):
        n = int(self.headers.get("Content-Length") or 0)
        if n <= 0 or n > MAX_BODY:
            return None
        try:
            return json.loads(self.rfile.read(n).decode("utf-8"))
        except (ValueError, UnicodeDecodeError):
            return None

    def do_GET(self):
        path = self.path.split("?", 1)[0].rstrip("/") or "/"
        if path.endswith("/health"):
            return self._send(200, b"ok")
        if path.endswith("/knock"):
            from urllib.parse import parse_qs
            q = parse_qs(self.path.split("?", 1)[1] if "?" in self.path else "")
            tag = (q.get("s") or [""])[0]
            if not _TAG_RE.match(tag):
                return self._send(400, {"error": "bad tag"})
            try:
                since = float((q.get("since") or ["0"])[0])
            except ValueError:
                since = 0.0
            now = time.time()
            return self._send(200, {"knocks": _knock_get(tag, since, now), "now": now})
        if path.endswith("/stats"):
            with _lock:
                return self._send(200, _stats(time.time()))
        if path.endswith("/list") or path == "/":
            now = time.time()
            with _lock:
                _clean(now)
                rows = []
                for v in sorted(_servers.values(), key=lambda e: (-e["at"])):
                    row = {k: v[k] for k in FIELDS}
                    row["age"] = int(now - v["at"])
                    rows.append(row)
            return self._send(200, {"servers": rows, "now": int(now), "ttl": int(TTL)})
        return self._send(404, {"error": "not found"})

    def _desync(self):
        # The body is not read before every check that can refuse it has passed;
        # a refused request closes the connection instead of draining megabytes.
        self.close_connection = True
        if not DESYNC_DIR:
            return self._send(503, {"error": "reports are not accepted here"})
        try:
            n = int(self.headers.get("Content-Length") or 0)
        except ValueError:
            n = 0
        if n <= 0 or n > UPLOAD_MAX:
            return self._send(413, {"error": "a report is at most %d bytes" % UPLOAD_MAX})
        ip = self.headers.get("X-Real-IP") or self.client_address[0]
        now = time.time()
        if not _upload_allowed(ip, now):
            return self._send(429, {"error": "too many reports, try again later"})
        meta = {}
        raw = self.headers.get("X-Tpf2mp-Meta") or ""
        if 0 < len(raw) <= 2048:
            try:
                m = json.loads(raw)
                if isinstance(m, dict):
                    meta = {str(k)[:32]: (v if isinstance(v, (int, float, bool)) else str(v)[:200])
                            for k, v in list(m.items())[:32]}
            except ValueError:
                pass
        body = self.rfile.read(n)
        if len(body) != n or not body.startswith(b"PK\x03\x04"):
            return self._send(400, {"error": "not a zip"})
        try:
            with zipfile.ZipFile(io.BytesIO(body)) as z:
                count = len(z.infolist())
        except zipfile.BadZipFile:
            return self._send(400, {"error": "not a zip"})
        if count > UPLOAD_MAX_ENTRIES:
            return self._send(400, {"error": "too many files"})
        rid = time.strftime("%Y%m%d-%H%M%S", time.gmtime(now)) + "-" + secrets.token_hex(3)
        base = os.path.join(DESYNC_DIR, "%s-%s-%s" % (rid, _safe(meta.get("version"), 16), _safe(meta.get("instance"), 4)))
        try:
            with open(base + ".zip.tmp", "wb") as f:
                f.write(body)
            os.replace(base + ".zip.tmp", base + ".zip")
            with open(base + ".json", "w", encoding="utf-8") as f:
                json.dump({"id": rid, "received": int(now), "bytes": n, "files": count, "meta": meta}, f, indent=1)
        except OSError as e:
            sys.stderr.write("desync report %s not stored: %s\n" % (rid, e))
            return self._send(500, {"error": "could not store the report"})
        _prune(DESYNC_DIR)
        sys.stderr.write("desync report %s: %d B, %d file(s), %s\n" % (rid, n, count, json.dumps(meta)[:300]))
        return self._send(200, {"ok": True, "id": rid})

    def do_POST(self):
        path = self.path.split("?", 1)[0].rstrip("/")
        if path.endswith("/desync"):
            return self._desync()
        d = self._body()
        if path.endswith("/knock"):
            if not isinstance(d, dict):
                return self._send(400, {"error": "bad request"})
            tag, blob = str(d.get("s") or ""), str(d.get("blob") or "")
            if not _TAG_RE.match(tag) or not blob or len(blob) > KNOCK_BLOB_MAX \
                    or not re.match(r"^[A-Za-z0-9+/=]+$", blob):
                return self._send(400, {"error": "bad knock"})
            ip = self.headers.get("X-Real-IP") or self.client_address[0]
            now = time.time()
            relay = None
            if d.get("relay") and RELAY_IP:
                nonce = str(d.get("j") or "")
                if not _NONCE_RE.match(nonce):
                    return self._send(400, {"error": "bad nonce"})
                relay = _relay_alloc(tag, nonce, now)
                if relay is None:
                    return self._send(503, {"error": "relay full"})
            err = _knock_post(tag, blob, ip, now, relay)
            if err:
                return self._send(err[0], {"error": err[1]})
            reply = {"ok": True, "ttl": int(KNOCK_TTL)}
            if relay:
                reply["relay"] = relay
            return self._send(200, reply)
        if path.endswith("/ping"):
            if not isinstance(d, dict):
                return self._send(400, {"error": "bad request"})
            now = time.time()
            # the anonymous session count: nothing here identifies a lobby or a
            # player, and the address is deliberately not kept
            psid = _s(d.get("sid"), 32)
            if not _SID_RE.match(psid):
                return self._send(400, {"error": "bad sid"})
            kind = _s(d.get("type"), 16)
            if kind not in TYPE_LABELS:
                kind = "host"
            try:
                players = max(0, min(int(d.get("players") or 0), 1000))
            except (TypeError, ValueError):
                players = 0
            with _lock:
                _clean_sessions(now)
                if psid not in _sessions and len(_sessions) >= MAX_ENTRIES:
                    return self._send(503, {"error": "full"})
                prev = _sessions.get(psid)
                _sessions[psid] = {"players": players, "max": int(d.get("max") or 0), "type": kind,
                                   "version": _s(d.get("version"), 20), "public": bool(d.get("public")),
                                   "at": now, "first": prev["first"] if prev else now}
                h = _hours.setdefault(int(now // 3600), {"sids": set(), "peak_sessions": 0, "peak_players": 0, "versions": {}})
                if psid not in h["sids"]:
                    h["sids"].add(psid)
                    v = _sessions[psid]["version"]
                    h["versions"][v] = h["versions"].get(v, 0) + 1
                h["peak_sessions"] = max(h["peak_sessions"], len(_sessions))
                h["peak_players"] = max(h["peak_players"], sum(s["players"] for s in _sessions.values()))
            return self._send(200, {"ok": True, "every": PING_EVERY})
        if not isinstance(d, dict) or not _s(d.get("id"), 64):
            return self._send(400, {"error": "bad request"})
        sid = _s(d.get("id"), 64)
        now = time.time()
        if path.endswith("/announce"):
            code = _s(d.get("code"), 400)
            if not code:
                return self._send(400, {"error": "code required"})
            kind = _s(d.get("type"), 16)
            if kind not in TYPE_LABELS:
                # lobbies before the type field send none: the relay is known by
                # the game string its service passes, anything else is a player
                kind = "relay" if _s(d.get("game"), 60) == "dedicated relay" else "host"
            e = {
                "id": sid,
                "name": _s(d.get("name"), 40) or "unnamed",
                "code": code,
                "players": int(d.get("players") or 0),
                "max": int(d.get("max") or 8),
                "type": kind,
                # "game" carries the type's label: 0.4.11-and-older panels show
                # this field, so they list the type too instead of a save name
                "game": TYPE_LABELS[kind],
                "version": _s(d.get("version"), 20),
                "locked": bool(d.get("locked")),
                "at": now,
                "ip": self.client_address[0],
            }
            with _lock:
                _clean(now)
                if sid not in _servers and len(_servers) >= MAX_ENTRIES:
                    return self._send(503, {"error": "full"})
                _servers[sid] = e
            return self._send(200, {"ok": True, "ttl": int(TTL)})
        if path.endswith("/leave"):
            with _lock:
                _servers.pop(sid, None)
            return self._send(200, {"ok": True})
        return self._send(404, {"error": "not found"})

    def log_message(self, fmt, *args):
        sys.stderr.write("%s %s\n" % (self.client_address[0], fmt % args))


def main(argv=None):
    global DESYNC_DIR, RELAY_IP, RELAY_PORTS, RELAY_BIND, STATS_FILE
    ap = argparse.ArgumentParser(description="tpf2mp master server")
    ap.add_argument("port", nargs="?", type=int, default=8471)
    ap.add_argument("--desync-dir", default=None, help="accept desync reports and keep them here")
    ap.add_argument("--stats-file", default=None, help="append one JSON line per finished hour of session counts")
    ap.add_argument("--bind", default="127.0.0.1",
                    help="listen address (default 127.0.0.1, behind nginx; tools/nat_lab binds 0.0.0.0)")
    ap.add_argument("--relay-ip", default=None,
                    help="offer the UDP relay fallback: the public address joiners and hosts send to (off without it)")
    ap.add_argument("--relay-ports", default="%d-%d" % RELAY_PORTS, help="UDP port range for relay allocations, a-b")
    ap.add_argument("--relay-bind", default="0.0.0.0", help="address the relay ports bind (default 0.0.0.0)")
    a = ap.parse_args(argv)
    if a.desync_dir:
        os.makedirs(a.desync_dir, exist_ok=True)
        DESYNC_DIR = a.desync_dir
    if a.relay_ip:
        lo, hi = (int(x) for x in a.relay_ports.split("-", 1))
        RELAY_IP, RELAY_PORTS, RELAY_BIND = a.relay_ip, (lo, hi), a.relay_bind
        start_relay()
    STATS_FILE = a.stats_file
    srv = ThreadingHTTPServer((a.bind, a.port), H)
    sys.stderr.write("tpf2mp master server on %s:%d (ttl %ds, desync reports %s, relay %s)\n"
                     % (a.bind, a.port, TTL, DESYNC_DIR or "off",
                        ("%s udp/%d-%d" % (RELAY_IP, RELAY_PORTS[0], RELAY_PORTS[1])) if RELAY_IP else "off"))
    srv.serve_forever()


if __name__ == "__main__":
    main()
