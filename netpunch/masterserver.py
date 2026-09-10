#!/usr/bin/env python3
"""tpf2mp master server -- the OpenTTD-style public game list.

A host that ticks PUBLIC announces its lobby here every ~30 s; the title-menu
panel lists what is announced and pasting a row's code joins it. Nothing is
brokered: the code IS the join mechanism (it carries the host's address and
the session secret), this server only stores and repeats it. No accounts,
no auth: a public lobby is public by choice, and a password-locked lobby
shows as locked (its code alone does not get anyone in).

    POST /announce   {"id","name","code","players","max","game","version","locked"}
    POST /leave      {"id"}
    GET  /list       {"servers":[{... , "age": seconds since last announce}], "now": unix}
    GET  /health     "ok"

Entries expire TTL seconds after their last announce. Bound to localhost;
nginx proxies https://<host>/tpf2mp/ to it. Stdlib only, one file, runs as a
systemd service (see the deploy step in tools/masterserver_deploy.sh).
"""
import json, sys, time, threading
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

TTL = 30.0           # seconds an entry lives without a fresh announce (lobbies announce every 10 s)
MAX_BODY = 4096
MAX_ENTRIES = 500
FIELDS = ("id", "name", "code", "players", "max", "game", "version", "locked")

_lock = threading.Lock()
_servers = {}        # id -> dict(fields..., "at": last announce, "ip": announcer)


def _clean(now):
    dead = [k for k, v in _servers.items() if now - v["at"] > TTL]
    for k in dead:
        del _servers[k]


def _s(v, n):
    return str(v)[:n] if v is not None else ""


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

    def do_POST(self):
        path = self.path.split("?", 1)[0].rstrip("/")
        d = self._body()
        if not isinstance(d, dict) or not _s(d.get("id"), 64):
            return self._send(400, {"error": "bad request"})
        sid = _s(d.get("id"), 64)
        now = time.time()
        if path.endswith("/announce"):
            code = _s(d.get("code"), 400)
            if not code:
                return self._send(400, {"error": "code required"})
            e = {
                "id": sid,
                "name": _s(d.get("name"), 40) or "unnamed",
                "code": code,
                "players": int(d.get("players") or 0),
                "max": int(d.get("max") or 8),
                "game": _s(d.get("game"), 60),
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


def main():
    port = int(sys.argv[1]) if len(sys.argv) > 1 else 8471
    srv = ThreadingHTTPServer(("127.0.0.1", port), H)
    sys.stderr.write("tpf2mp master server on 127.0.0.1:%d (ttl %ds)\n" % (port, TTL))
    srv.serve_forever()


if __name__ == "__main__":
    main()
