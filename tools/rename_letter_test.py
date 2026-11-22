"""A joiner who renames keeps its origin letter.

Letters are fixed per player so nobody is renumbered while the game runs
(letter_for, 2026-09-26), but they were keyed by name: a rename in place (a
repeat join with a new name) handed out a NEW letter, and the worlds desync
when a player's commands change origin mid-game.

This runs a plaintext host on loopback: bob joins (letter b), renames to carl,
and the roster must say carl is b.

    python tools/rename_letter_test.py
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

import lobby                                                  # noqa: E402
from punch import _pack, _unpack, TYPE_DATA, open_socket      # noqa: E402

lobby.BULK_TCP[0] = False
fails = []


def check(name, cond, extra=""):
    print(("ok   " if cond else "FAIL ") + name + (f"  ({extra})" if extra else ""))
    if not cond:
        fails.append(name)


io = lobby.LobbyIO(tempfile.mkdtemp())
hs = open_socket(0, socket.AF_INET)
port = hs.getsockname()[1]
stop = threading.Event()
t = threading.Thread(target=lambda: lobby.run_host(hs, "alice", io, code="X", stop=stop, log=lambda s: None),
                     daemon=True)
t.start()
time.sleep(0.3)

c = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
c.bind(("127.0.0.1", 0))
c.settimeout(0.2)


def send(m):
    c.sendto(_pack(TYPE_DATA, json.dumps(m).encode()), ("127.0.0.1", port))


def last_letters(seconds=1.5):
    seen = None
    end = time.time() + seconds
    while time.time() < end:
        try:
            data, _ = c.recvfrom(65536)
        except socket.timeout:
            continue
        ptype, payload = _unpack(data)
        if ptype != TYPE_DATA:
            continue
        try:
            m = json.loads(payload.decode("utf-8"))
        except ValueError:
            continue
        if m.get("t") == "roster" and m.get("letters"):
            seen = m["letters"]
    return seen or {}


send({"t": "join", "name": "bob", "version": lobby.LOBBY_VERSION})
before = last_letters()
check("bob joins as b", before.get("bob") == "b", str(before))
send({"t": "join", "name": "carl", "version": lobby.LOBBY_VERSION})
after = last_letters()
check("renamed to carl, the letter is still b", after.get("carl") == "b", str(after))
check("...and no letter is left for the old name", "bob" not in after, str(after))

stop.set()
t.join(5)
print()
if fails:
    print(f"{len(fails)} FAILED")
    sys.exit(1)
print("PASS: a rename keeps the origin letter")
