#!/usr/bin/env python3
"""Local-only source test of Linux host shutdown, including release-0.4.22 rendezvous.

    <python with netpunch dependencies> tools/linux/test_lobby_stop.py

The child runs lobby.main(['host', ...]) with the network parts replaced:
_observe_and_announce binds a loopback socket and makes a code without STUN or
UPnP; observe.upnp_unmap only records that it ran (it takes UNMAP_SLEEP s). A
local stand-in master answers /announce at once and /leave after LEAVE_SLEEP s.
  (h) group SIGTERM while observing -> exit 130, 'stopped before the lobby was
      up', the unmap ran to the end first, no traceback
  (i) quit, then 1.5 s later group SIGTERM (lobby_linux.cpp Teardown) while
      publisher.close() waits for the slow /leave -> /leave arrives, the unmap
      runs to the end, exit 0 within the launcher's 2 s SIGTERM wait, no traceback
  (j) group SIGTERM while the lobby runs, a second one 1 s later -> the same
"""
import http.server
import json
import os
import signal
import subprocess
import sys
import tempfile
import threading
import time
import atexit
import shutil

from pathlib import Path
NP = str(Path(__file__).resolve().parents[2] / "netpunch")
LEAVE_SLEEP = 2.5

CHILD = r'''
import os, socket, sys, time, urllib.request
sys.dont_write_bytecode = True
sys.path.insert(0, os.environ["NP"])
urllib.request.install_opener(urllib.request.build_opener(urllib.request.ProxyHandler({})))
import lobby, observe
from connect import encode_profile
calls = os.environ["CALLS"]
def note(s):
    with open(calls, "a") as f:
        f.write("%.3f %s\n" % (time.time(), s))
def fake_unmap(port):
    note("unmap-start")
    time.sleep(float(os.environ.get("UNMAP_SLEEP", "0.5")))
    note("unmap-done")
    return True
observe.upnp_unmap = fake_unmap
def fake_observe(local_port, secret=None, password=None):
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.bind(("127.0.0.1", local_port))
    sock.setblocking(False)
    note("observe-start")
    time.sleep(float(os.environ.get("OBSERVE_SLEEP", "0")))
    prof = {"candidates": {"lan_v4": None, "public_v4": "127.0.0.1:%d" % sock.getsockname()[1], "v6": None},
            "flags": {"open": True}}
    note("observe-done")
    return sock, prof, encode_profile(prof, secret=secret, password=password)
lobby._observe_and_announce = fake_observe
rc = lobby.main(sys.argv[1:])
note("main-returned %s" % rc)
sys.exit(rc)
'''

results = []
events = []


def check(name, cond, detail=""):
    results.append(bool(cond))
    print(("ok   " if cond else "FAIL ") + name + ("" if cond else f"   ({detail})"), flush=True)


def wait_until(pred, timeout):
    end = time.time() + timeout
    while time.time() < end:
        if pred():
            return True
        time.sleep(0.05)
    return pred()


class Master(http.server.BaseHTTPRequestHandler):
    def do_GET(self):
        # Release .22 added rendezvous polling. A stalled poll must not consume
        # the launcher's shutdown budget before /leave and UPnP cleanup run.
        if self.path.startswith("/slow/"):
            events.append(("poll-start", time.time()))
            time.sleep(6)
        body = b'{"knocks":[]}'
        try:
            self.send_response(200)
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)
        except (BrokenPipeError, ConnectionResetError):
            pass

    def do_POST(self):
        self.rfile.read(int(self.headers.get("Content-Length") or 0))
        if self.path.endswith("/leave"):
            events.append(("leave-start", time.time()))
            time.sleep(LEAVE_SLEEP)
            events.append(("leave-done", time.time()))
        else:
            events.append((self.path, time.time()))
        self.send_response(200)
        self.send_header("Content-Length", "2")
        self.end_headers()
        self.wfile.write(b"{}")

    def log_message(self, *_a):
        pass


master = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Master)
threading.Thread(target=master.serve_forever, daemon=True).start()
MURL = f"http://127.0.0.1:{master.server_address[1]}"


def start(tag, extra=(), env_extra=None):
    d = tempfile.mkdtemp(prefix=f"np_hoststop_{tag}_")
    atexit.register(shutil.rmtree, d, ignore_errors=True)
    env = dict(os.environ, NP=NP, CALLS=os.path.join(d, "calls.txt"), PYTHONDONTWRITEBYTECODE="1", **(env_extra or {}))
    with open(os.path.join(d, "proc.log"), "wb") as logf:
        p = subprocess.Popen([sys.executable, "-c", CHILD, "host", "--name", "h", "--local-port", "0",
                              "--io-dir", d, "--rendezvous", MURL + "/slow"] + list(extra),
                             cwd=d, env=env, stdin=subprocess.DEVNULL, stdout=logf, stderr=subprocess.STDOUT,
                             start_new_session=True)
    return p, d


def calls(d):
    try:
        with open(os.path.join(d, "calls.txt")) as f:
            return [(float(line.split(" ", 1)[0]), line.split(" ", 1)[1].strip()) for line in f if line.strip()]
    except OSError:
        return []


def names(d):
    return [c[1] for c in calls(d)]


def text(d, name):
    try:
        with open(os.path.join(d, name), errors="replace") as f:
            return f.read()
    except OSError:
        return ""


# (h) SIGTERM while observing
p, d = start("h", env_extra={"OBSERVE_SLEEP": "30"})
check("(h) the host is observing", wait_until(lambda: "observe-start" in names(d), 15), text(d, "proc.log")[-600:])
t0 = time.time()
os.killpg(p.pid, signal.SIGTERM)
rc = p.wait(timeout=15)
took = time.time() - t0
log = text(d, "proc.log")
n = names(d)
check("(h) exit 130", rc == 130, (rc, log[-800:]))
check("(h) the UPnP unmap ran to the end before exit", n[-3:] == ["unmap-start", "unmap-done", "main-returned 130"], n)
check("(h) 'stopped before the lobby was up', no traceback",
      "stopped before the lobby was up" in log and "Traceback" not in log, log[-800:])
print(f"      exit {took:.2f} s after SIGTERM (includes the 0.5 s stand-in unmap)")

# (i) quit, then SIGTERM 1.5 s later, while /leave is slow
events.clear()
p, d = start("i", ["--publish", MURL, "--public", "--lobby-name", "stop test"])
ready = wait_until(lambda: "lobby ready" in text(d, "lobby_out.jsonl") and any(e[0] == "/announce" for e in events) and any(e[0] == "poll-start" for e in events), 20)
check("(i) the lobby is up and announced", ready, (text(d, "proc.log")[-600:], events))
with open(os.path.join(d, "lobby_in.jsonl"), "a") as f:
    f.write(json.dumps({"cmd": "quit"}) + "\n")
tq = time.time()
time.sleep(1.5)
check("(i) still cleaning up 1.5 s after quit (waiting for /leave)", p.poll() is None, p.returncode)
tt = time.time()
os.killpg(p.pid, signal.SIGTERM)
rc = p.wait(timeout=15)
te = time.time()
log = text(d, "proc.log")
n = names(d)
check("(i) exit 0", rc == 0, (rc, log[-800:]))
check("(i) /leave reached the master", any(e[0] == "leave-done" for e in events), events)
check("(i) the unmap ran to the end", n[-3:] == ["unmap-start", "unmap-done", "main-returned 0"], n)
check("(i) no traceback, no 'stopped before'", "Traceback" not in log and "stopped before" not in log, log[-800:])
check("(i) out within the launcher's 2 s SIGTERM wait", te - tt < 2.0, round(te - tt, 2))
print(f"      quit -> exit {te - tq:.2f} s; SIGTERM -> exit {te - tt:.2f} s")

# (j) SIGTERM while running, a second one 1 s later
events.clear()
p, d = start("j", ["--publish", MURL, "--public"])
ready = wait_until(lambda: "lobby ready" in text(d, "lobby_out.jsonl") and any(e[0] == "/announce" for e in events) and any(e[0] == "poll-start" for e in events), 20)
check("(j) the lobby is up and announced", ready, (text(d, "proc.log")[-600:], events))
tt = time.time()
os.killpg(p.pid, signal.SIGTERM)
time.sleep(1.0)
if p.poll() is None:
    os.killpg(p.pid, signal.SIGTERM)
rc = p.wait(timeout=15)
te = time.time()
log = text(d, "proc.log")
n = names(d)
check("(j) exit 0", rc == 0, (rc, log[-800:]))
check("(j) /leave reached the master", any(e[0] == "leave-done" for e in events), events)
check("(j) the unmap ran to the end", n[-3:] == ["unmap-start", "unmap-done", "main-returned 0"], n)
check("(j) no traceback", "Traceback" not in log, log[-800:])
print(f"      first SIGTERM -> exit {te - tt:.2f} s")

print(f"{sum(results)}/{len(results)} checks passed -> {'PASS' if all(results) else 'FAIL'}", flush=True)
sys.exit(0 if all(results) else 1)
