"""TCP FOR STEAM PEERS: host and joiner meet only through two fake Steam tunnels,
and over the sealed link each names its own addresses so the save can take the
bulk TCP channel (lobby.MY_TCP_ADDRS):

  1. the host's listener is reachable: the joiner dials it and the save streams
  2. the host has no listener: the joiner's offer (fbegin_ack tcp_addrs/tcp_port)
     lets the HOST dial the JOINER, and the save streams that way
  3. no TCP at all: Steam carries the save, complete and identical

Addresses are this machine's LAN IP (loopback is filtered out of offers on purpose).

    python tools/test_steam_tcp.py
"""
import hashlib
import json
import os
import socket
import sys
import tempfile
import threading
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "netpunch"))

from steamtunnel import FakeTunnel, SteamTunnel, is_tunnel_addr   # noqa: E402
from connect import race                                          # noqa: E402
from punch import open_socket                                     # noqa: E402
import lobby                                                      # noqa: E402

fails = []


def check(name, cond, extra=""):
    print(("ok   " if cond else "FAIL ") + name + (f"  ({extra})" if extra else ""))
    if not cond:
        fails.append(name)


def lan_ip():
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    try:
        s.connect(("192.0.2.1", 9))           # no packet is sent: this only picks the outbound interface
        return s.getsockname()[0]
    finally:
        s.close()


LAN = lan_ip()
SIZE = 24 * 1024 * 1024


def run(label, addrs, host_listener=True):
    lobby.MY_TCP_ADDRS[0] = list(addrs)
    lobby.JOINER_BULK[0] = None
    with tempfile.TemporaryDirectory() as ta, tempfile.TemporaryDirectory() as tb:
        A, B = FakeTunnel(ta, 1001, "alice"), FakeTunnel(tb, 2002, "bob")
        ca, cb = SteamTunnel(ta), SteamTunnel(tb)
        base = tempfile.mkdtemp(prefix="steam_tcp_")
        io_h, io_j = lobby.LobbyIO(os.path.join(base, "h")), lobby.LobbyIO(os.path.join(base, "j"))
        save = os.path.join(base, "world.sav")
        with open(save, "wb") as f:
            f.write(os.urandom(SIZE))
        with open(save + ".lua", "wb") as f:
            f.write(b'["lockstep.lua"] = { }\n')
        want = hashlib.sha256(open(save, "rb").read()).hexdigest()
        stop = threading.Event()
        hsock = open_socket(0, socket.AF_INET)
        ca.hello(hsock.getsockname()[1])
        threading.Thread(target=lobby.run_host, args=(hsock, "alice", io_h),
                         kwargs={"code": "STEAMTCP", "stop": stop}, daemon=True).start()
        time.sleep(0.5)
        if not host_listener and lobby.BULK[0] is not None:
            lobby.BULK[0].close() if hasattr(lobby.BULK[0], "close") else None
            lobby.BULK[0] = None
        jsock = open_socket(0, socket.AF_INET)
        cb.hello(jsock.getsockname()[1])
        ep = cb.dial("1001")
        conn = race(jsock, {"candidates": {"steam": "1001"}, "flags": {"v6": False}}, "dial", jsock.getsockname()[1], 10,
                    my_has_v6=False, late_targets=[ep])
        if not (conn is not None and is_tunnel_addr(conn.peer)):
            check(f"{label}: connected through the fake tunnel", False)
            return None
        threading.Thread(target=lobby.run_client, args=(conn, "bob", io_j), kwargs={"stop": stop}, daemon=True).start()
        lobby._wait_until(lambda: sorted((lobby._latest_roster(io_h.out_path) or {}).get("players", [])) == ["alice", "bob"], timeout=12)
        t0 = time.time()
        with open(io_h.in_path, "a", encoding="utf-8") as f:
            f.write(json.dumps({"cmd": "start", "save": save}) + "\n")
        done = lambda: any(e.get("type") == "save_ready" for e in lobby._read_events(io_j.out_path))  # noqa: E731
        lobby._wait_until(done, timeout=90)
        dt = time.time() - t0
        got = os.path.join(io_j.dir, "incoming_save.sav")
        same = done() and os.path.isfile(got) and hashlib.sha256(open(got, "rb").read()).hexdigest() == want
        via_tcp = any(e.get("type") == "transfer" and e.get("state") == "tcp" for e in lobby._read_events(io_j.out_path))
        print(f"     {label}: {SIZE / 1e6:.0f} MB in {dt:.1f} s, tcp={via_tcp}, identical={same}")
        stop.set()
        time.sleep(0.6)
        A.close(); B.close(); ca.close(); cb.close()
        if lobby.JOINER_BULK[0] is not None and hasattr(lobby.JOINER_BULK[0], "close"):
            lobby.JOINER_BULK[0].close()
        lobby.JOINER_BULK[0] = None
        return same, via_tcp


print(f"LAN address used for the offers: {LAN}")
r = run("host listener reachable", [LAN])
check("1. the joiner dials the host's address and the save takes TCP", r and r[0] and r[1], str(r))
r = run("no host listener, joiner's offered", [LAN], host_listener=False)
check("2. the host dials the joiner's listener and the save takes TCP", r and r[0] and r[1], str(r))
r = run("no addresses at all", [])
check("3. no TCP either way: Steam carries the save, identical", r and r[0] and not r[1], str(r))

print()
if fails:
    print(f"{len(fails)} FAILED")
    raise SystemExit(1)
print("PASS: Steam peers swap addresses over the sealed link; either end's dial carries the save over TCP, Steam otherwise")
