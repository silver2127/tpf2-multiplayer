"""Offline test of the anonymous session count: masterserver.py /ping + /stats and
the lobby's _Publisher heartbeat, on a loopback master.

  1. two pings (one public, one private) -> /stats counts 2 sessions, 3 players, 1 public
  2. a repeat ping updates, never duplicates; a bad sid is refused
  3. the hour row carries the peak; an expired session leaves the count
  4. a _Publisher with ping on reports without being listed; --no-ping sends nothing
"""
import json, os, sys, threading, time, urllib.request
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "netpunch"))
import masterserver                                          # noqa: E402
import lobby                                                 # noqa: E402

fails = []
def check(name, cond, extra=""):
    print(("PASS " if cond else "FAIL ") + name + (f"  ({extra})" if extra and not cond else ""))
    if not cond:
        fails.append(name)

masterserver.H.log_message = lambda *a, **k: None
srv = masterserver.ThreadingHTTPServer(("127.0.0.1", 0), masterserver.H)
threading.Thread(target=srv.serve_forever, daemon=True).start()
url = "http://127.0.0.1:%d" % srv.server_address[1]

def post(path, body):
    req = urllib.request.Request(url + path, data=json.dumps(body).encode(), headers={"Content-Type": "application/json"})
    try:
        with urllib.request.urlopen(req, timeout=5) as r:
            return r.status, json.loads(r.read() or b"{}")
    except urllib.error.HTTPError as e:
        return e.code, json.loads(e.read() or b"{}")

def stats():
    with urllib.request.urlopen(url + "/stats", timeout=5) as r:
        return json.loads(r.read())

# 1
post("/ping", {"sid": "a" * 16, "players": 2, "max": 200, "type": "host", "version": "0.6.1.10", "public": True})
post("/ping", {"sid": "b" * 16, "players": 1, "max": 200, "type": "host", "version": "0.6.1.10", "public": False})
s = stats()
check("two sessions counted", s["sessions"] == 2 and s["players"] == 3, json.dumps(s))
check("one public, one private", s["public"] == 1 and s["private"] == 1)
check("by version", s["by_version"] == {"0.6.1.10": 2}, json.dumps(s["by_version"]))
check("nothing listed publicly", s["listed"] == 0)

# 2
post("/ping", {"sid": "a" * 16, "players": 4, "max": 200, "type": "host", "version": "0.6.1.10", "public": True})
s = stats()
check("a repeat ping updates, never duplicates", s["sessions"] == 2 and s["players"] == 5, json.dumps(s))
code, _ = post("/ping", {"sid": "not-hex!", "players": 1})
check("a bad sid is refused", code == 400)
check("the stats carry no ids, names or addresses", not any(k in json.dumps(s) for k in ("aaaaaaaa", "ip", "code", "name")), json.dumps(s))

# 3
h = s["hours"]
check("the hour row carries the peak", h and h[-1]["peak_sessions"] == 2 and h[-1]["peak_players"] == 5 and h[-1]["sessions"] == 2, json.dumps(h))
real_ttl, masterserver.SESSION_TTL = masterserver.SESSION_TTL, 0.2
time.sleep(0.4)
s = stats()
check("an expired session leaves the count", s["sessions"] == 0 and s["players"] == 0, json.dumps(s))
masterserver.SESSION_TTL = real_ttl

# 4
logs = []
real_every, lobby.PING_EVERY = lobby.PING_EVERY, 0.1
real_pub, lobby.PUBLISH_EVERY = lobby.PUBLISH_EVERY, 0.1      # the loop wakes on the announce cadence
p = lobby._Publisher(url, "CODE", "host", False, logs.append, ping=True)
p.update("secret lobby", 3)
deadline = time.time() + 5
while time.time() < deadline and not (stats()["sessions"] == 1 and stats()["players"] == 3):
    time.sleep(0.1)
s = stats()
check("a private lobby's publisher reports a session", s["sessions"] == 1 and s["players"] == 3 and s["private"] == 1, json.dumps(s))
check("... and is not listed", s["listed"] == 0)
check("the log says so once", sum("counted anonymously" in l for l in logs) == 1, str(logs))
p.set(True)
deadline = time.time() + 5
while time.time() < deadline and stats()["public"] < 1:
    time.sleep(0.1)
s = stats()
check("listing makes the same session public", s["sessions"] == 1 and s["public"] == 1 and s["listed"] == 1, json.dumps(s))
p.close()
masterserver.SESSION_TTL = 0.2
time.sleep(0.4)
with masterserver._lock:
    masterserver._sessions.clear()
masterserver.SESSION_TTL = real_ttl
q = lobby._Publisher(url, "CODE", "host", False, logs.append, ping=False)
q.update("quiet lobby", 2)
time.sleep(0.5)
s = stats()
check("--no-ping sends nothing", s["sessions"] == 0, json.dumps(s))
q.close()
lobby.PING_EVERY, lobby.PUBLISH_EVERY = real_every, real_pub
srv.shutdown()
print("session_stats_test:", "FAIL " + ", ".join(fails) if fails else "PASS")
sys.exit(1 if fails else 0)
