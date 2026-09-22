"""A WHOLE SAVE THROUGH THE STEAM TUNNEL: host and joiner meet only through two fake
tunnels (steamtunnel.FakeTunnel) and the host shares a 48 MB save, which goes at
CHUNK_STEAM (32 KB) with SEND_WINDOW_STEAM.

The fakes relay over loopback UDP like the native tunnel does, so the loopback
leg is real: the lobby hands its window to an endpoint socket in one burst, and
an endpoint with the OS default receive buffer drops most of it. That is what
stalled the live 0.6.1.21 transfer (base 132/4199, TIMED OUT); the native
tunnel now gives its endpoints 16 MB buffers, as the fakes do by default.

    python tools/test_steam_save_transfer.py            # 16 MB endpoint buffers: must finish
    python tools/test_steam_save_transfer.py --default  # the OS default: shows the stall (not asserted)
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

DEFAULT = "--default" in sys.argv
SIZE = 48 * 1024 * 1024
buf = None if DEFAULT else 16 * 1024 * 1024

with tempfile.TemporaryDirectory() as ta, tempfile.TemporaryDirectory() as tb:
    A = FakeTunnel(ta, 1001, "alice", endpoint_buf=buf)
    B = FakeTunnel(tb, 2002, "bob", endpoint_buf=buf)
    ca, cb = SteamTunnel(ta), SteamTunnel(tb)
    base = tempfile.mkdtemp(prefix="steam_xfer_")
    io_h, io_j = lobby.LobbyIO(os.path.join(base, "h")), lobby.LobbyIO(os.path.join(base, "j"))
    save = os.path.join(base, "world.sav")
    with open(save, "wb") as f:
        f.write(os.urandom(SIZE))
    with open(save + ".lua", "wb") as f:
        f.write(b'["lockstep.lua"] = { }\n')
    want = hashlib.sha256(open(save, "rb").read()).hexdigest()
    stop = threading.Event()
    hsock = open_socket(0, socket.AF_INET)
    lobby._boost_socket_buffers(hsock)
    ca.hello(hsock.getsockname()[1])
    threading.Thread(target=lobby.run_host, args=(hsock, "alice", io_h),
                     kwargs={"code": "STEAMXFER", "stop": stop}, daemon=True).start()
    time.sleep(0.4)
    jsock = open_socket(0, socket.AF_INET)
    cb.hello(jsock.getsockname()[1])
    ep = cb.dial("1001")
    conn = race(jsock, {"candidates": {"steam": "1001"}, "flags": {"v6": False}}, "dial", jsock.getsockname()[1], 10,
                my_has_v6=False, late_targets=[ep])
    assert conn is not None and is_tunnel_addr(conn.peer), "no connection through the fake tunnel"
    threading.Thread(target=lobby.run_client, args=(conn, "bob", io_j), kwargs={"stop": stop}, daemon=True).start()
    lobby._wait_until(lambda: (lobby._latest_roster(io_h.out_path) or {}).get("players", []) == ["alice", "bob"]
                      or sorted((lobby._latest_roster(io_h.out_path) or {}).get("players", [])) == ["alice", "bob"], timeout=12)
    t0 = time.time()
    with open(io_h.in_path, "a", encoding="utf-8") as f:
        f.write(json.dumps({"cmd": "start", "save": save}) + "\n")

    def done():
        return any(e.get("type") == "save_ready" for e in lobby._read_events(io_j.out_path))

    def failed():
        return any(e.get("type") == "transfer" and e.get("state") == "failed" for e in lobby._read_events(io_h.out_path))
    ok = lobby._wait_until(lambda: done() or failed(), timeout=90) and done()
    dt = time.time() - t0
    got = os.path.join(io_j.dir, "incoming_save.sav")
    same = ok and os.path.isfile(got) and hashlib.sha256(open(got, "rb").read()).hexdigest() == want
    print(f"{'endpoint buffers: OS default' if DEFAULT else 'endpoint buffers: 16 MB'} -- "
          + (f"{SIZE / 1e6:.0f} MB in {dt:.1f} s ({SIZE / 1e6 / dt:.1f} MB/s), identical={same}" if ok else
             f"did NOT finish ({'failed' if failed() else 'timed out'} after {dt:.0f} s)")
          + f"; tunnel datagrams {A.sent} out of the host's side")
    stop.set()
    time.sleep(0.5)
    A.close(); B.close(); ca.close(); cb.close()

if DEFAULT:
    sys.exit(0)
print("PASS: a 48 MB save crossed the Steam tunnel at 32 KB chunks" if same else "FAIL")
sys.exit(0 if same else 1)
