"""One malformed message from a peer must not end the host's lobby.

run_host's serve loop has only try/finally around it, and handle_data had no
guard of its own: a join whose "name" was a number, a "log" whose "lines" was a
number, a "links" whose "direct" was a number -- each raised, and the lobby
closed for everyone. In a sealed session anyone with the code can send these,
and a public listing publishes the code. (run_client already guarded handle_msg.)

This runs a plaintext host on loopback and sends each message; the host must
still be running afterwards.

    python tools/host_malformed_test.py
"""
import json
import os
import socket
import sys
import tempfile
import threading
import time

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(REPO, "netpunch"))

import lobby                                         # noqa: E402
from punch import _pack, TYPE_DATA, open_socket      # noqa: E402

lobby.BULK_TCP[0] = False
fails = []


def check(name, cond):
    print(("ok   " if cond else "FAIL ") + name)
    if not cond:
        fails.append(name)


def host_survives(msgs):
    io = lobby.LobbyIO(tempfile.mkdtemp())
    hs = open_socket(0, socket.AF_INET)
    port = hs.getsockname()[1]
    stop = threading.Event()
    died = []

    def run():
        try:
            lobby.run_host(hs, "alice", io, code="X", stop=stop, log=lambda s: None)
        except Exception as e:
            died.append(e)

    t = threading.Thread(target=run, daemon=True)
    t.start()
    time.sleep(0.3)
    c = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    c.bind(("127.0.0.1", 0))
    for m in msgs:
        c.sendto(_pack(TYPE_DATA, json.dumps(m).encode()), ("127.0.0.1", port))
        time.sleep(0.2)
    time.sleep(0.5)
    alive = t.is_alive() and not died
    stop.set()
    t.join(5)
    c.close()
    return alive


JOIN = {"t": "join", "name": "bob", "version": lobby.LOBBY_VERSION}
check("a join whose name is a number", host_survives([{"t": "join", "name": 5, "version": lobby.LOBBY_VERSION}]))
check("a log whose lines is a number", host_survives([JOIN, {"t": "log", "lines": 5}]))
check("a links whose direct is a number", host_survives([JOIN, {"t": "links", "direct": 5}]))
check("a fack whose base is text", host_survives([JOIN, {"t": "fack", "base": "x"}]))
check("an ordinary chat still works", host_survives([JOIN, {"t": "chat", "text": "hi"}]))

print()
if fails:
    print(f"{len(fails)} FAILED")
    sys.exit(1)
print("PASS: the host survives malformed peer messages")
