"""STEAM BY DEFAULT: the join code is the host's Steam ID; CROSS-PLAY opens the classic code.

Over two fake Steam tunnels (steamtunnel.FakeTunnel), no game:

  1. a 17-digit SteamID64 (or a pasted profile URL) decodes as a Steam-only profile with no
     secret; anything else is still a classic code
  2. a host with Steam and cross-play off shows and emits its Steam ID as the code
  3. a joiner asks for the session key over the tunnel and gets exactly the host's secret
     (steamkey.py); an offer from outside the tunnel is not answered
  4. with that key the two meet only through Steam and play a SEALED session: rosters and
     a chat line cross
  5. cross-play off: a HELLO from outside Steam gets no handshake; the host's 'crossplay'
     command turns it on (the classic code is emitted, the same HELLO is answered) and off
     again (the Steam ID is back)
  6. a host without Steam emits the classic code and ignores key requests

    python tools/test_steam_code.py
"""
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
from connect import decode_code, steam_code_id, race              # noqa: E402
from punch import open_socket, _pack, _unpack, TYPE_HELLO, TYPE_ACK, TYPE_KEYX   # noqa: E402
from seal import Sealer, derive_key                                # noqa: E402
import steamkey                                                    # noqa: E402
import lobby                                                       # noqa: E402

fails = []


def check(name, cond, extra=""):
    print(("ok   " if cond else "FAIL ") + name + (f"  ({extra})" if extra else ""))
    if not cond:
        fails.append(name)


def code_events(io):
    out = []
    for e in lobby._read_events(io.out_path):
        if e.get("type") == "code":
            out.append(e)
    return out


def hello_answered(host_addr, timeout=1.0):
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.bind(("127.0.0.1", 0))
    s.settimeout(0.2)
    tok = os.urandom(8)
    deadline = time.time() + timeout
    try:
        while time.time() < deadline:
            s.sendto(_pack(TYPE_HELLO, tok), host_addr)
            try:
                data, _ = s.recvfrom(65535)
            except socket.timeout:
                continue
            ptype, payload = _unpack(data)
            if ptype == TYPE_ACK and payload == tok:
                return True
        return False
    finally:
        s.close()


# ---- 1. the code
SID = "76561198085519545"
check("a SteamID64 is a Steam code", steam_code_id(SID) == SID)
check("a pasted profile URL is too", steam_code_id("https://steamcommunity.com/profiles/%s/" % SID) == SID)
check("other digit strings are not", steam_code_id("12345678901234567") is None and steam_code_id("7656119808551954") is None)
d = decode_code(SID)
check("it decodes Steam-only, with no secret and only the steam candidate",
      d.get("steam_only") and d["secret"] is None and d["candidates"]["steam"] == SID
      and not any(d["candidates"][k] for k in ("lan_v4", "public_v4", "v6")))
check("a classic code is not taken for one", steam_code_id("XBVLDTX3JQGW243TEBGA23LTOMQHGIBKAJDYAAAE") is None)

secret = bytes(range(12))
PASSWORD = "pw"
with tempfile.TemporaryDirectory() as ta, tempfile.TemporaryDirectory() as tb:
    A = FakeTunnel(ta, 1001, "alice")
    B = FakeTunnel(tb, 2002, "bob")
    ca, cb = SteamTunnel(ta, lambda s: None), SteamTunnel(tb, lambda s: None)
    base = tempfile.mkdtemp(prefix="steam_code_")
    io_h, io_j = lobby.LobbyIO(os.path.join(base, "h")), lobby.LobbyIO(os.path.join(base, "j"))
    stop = threading.Event()
    lobby.SEAL[0] = Sealer(derive_key(secret, PASSWORD))
    hsock = open_socket(0, socket.AF_INET)
    ca.hello(hsock.getsockname()[1])
    threading.Thread(target=lobby.run_host, name="host", args=(hsock, "alice", io_h),
                     kwargs={"code": "CLASSICCODE", "stop": stop, "cross_code": "CLASSICCODE",
                             "steam_code": "1001", "steam_secret": secret, "crossplay": False},
                     daemon=True).start()

    # ---- 2. the code the host shows
    ok = lobby._wait_until(lambda: code_events(io_h), timeout=5)
    ev = code_events(io_h)[-1] if code_events(io_h) else {}
    check("cross-play off: the host emits its Steam ID as the code", ok and ev.get("code") == "1001" and ev.get("crossplay") is False,
          str(ev))
    check("... and still tells the menu the classic code", ev.get("cross_code") == "CLASSICCODE")

    # ---- 3. the key over Steam
    jsock = open_socket(0, socket.AF_INET)
    cb.hello(jsock.getsockname()[1])
    ep = cb.dial("1001")
    check("the joiner dials the host's Steam ID", ep is not None and is_tunnel_addr(ep), str(ep))
    got = lobby._steam_key_exchange(jsock, ep, 5.0)
    check("the joiner receives exactly the host's session secret over Steam", got == secret)
    outsider = socket.socket(socket.AF_INET, socket.SOCK_DGRAM); outsider.bind(("127.0.0.1", 0)); outsider.settimeout(0.8)
    outsider.sendto(_pack(TYPE_KEYX, steamkey.Offer().payload()), ("127.0.0.1", hsock.getsockname()[1]))
    try:
        outsider.recvfrom(65535); answered = True
    except socket.timeout:
        answered = False
    outsider.close()
    check("an offer from outside the tunnel is not answered", not answered)

    # ---- 4. a sealed session through Steam alone
    conn = race(jsock, {"candidates": {"steam": "1001"}, "flags": {"v6": False}}, "dial", jsock.getsockname()[1], 10,
                my_has_v6=False, late_targets=[ep])
    check("the joiner connects through the tunnel", conn is not None and is_tunnel_addr(conn.peer), str(conn and conn.peer))
    if conn is not None:
        conn.cipher = lobby.SEAL[0]
        threading.Thread(target=lobby.run_client, name="client", args=(conn, "bob", io_j),
                         kwargs={"stop": stop}, daemon=True).start()

        def rosters():
            rh, rj = lobby._latest_roster(io_h.out_path), lobby._latest_roster(io_j.out_path)
            return bool(rh and rj and sorted(rh.get("players", [])) == ["alice", "bob"] == sorted(rj.get("players", [])))
        check("both rosters list alice and bob in the sealed session", lobby._wait_until(rosters, timeout=12))
        with open(io_j.in_path, "a", encoding="utf-8") as f:
            f.write(json.dumps({"cmd": "chat", "text": "hello, steam id"}) + "\n")
        check("a chat line crosses", lobby._wait_until(lambda: lobby._has_chat(io_h.out_path, "hello, steam id"), timeout=8))

    # ---- 5. cross-play gate and the live switch
    host_addr = ("127.0.0.1", hsock.getsockname()[1])
    check("cross-play off: a HELLO from outside Steam gets no handshake", not hello_answered(host_addr))
    with open(io_h.in_path, "a", encoding="utf-8") as f:
        f.write(json.dumps({"cmd": "crossplay", "on": True}) + "\n")
    ok = lobby._wait_until(lambda: code_events(io_h)[-1].get("crossplay") is True, timeout=5)
    check("cross-play on: the classic code is emitted", ok and code_events(io_h)[-1].get("code") == "CLASSICCODE",
          str(code_events(io_h)[-1]))
    check("... and a HELLO from outside Steam is answered", hello_answered(host_addr))
    with open(io_h.in_path, "a", encoding="utf-8") as f:
        f.write(json.dumps({"cmd": "crossplay", "on": False}) + "\n")
    ok = lobby._wait_until(lambda: code_events(io_h)[-1].get("crossplay") is False, timeout=5)
    check("cross-play off again: the Steam ID is the code", ok and code_events(io_h)[-1].get("code") == "1001")

    stop.set()
    time.sleep(0.5)

    # ---- 6. a host without Steam
    io_n = lobby.LobbyIO(os.path.join(base, "n"))
    stop2 = threading.Event()
    nsock = open_socket(0, socket.AF_INET)
    threading.Thread(target=lobby.run_host, name="host2", args=(nsock, "carol", io_n),
                     kwargs={"code": "CLASSICONLY", "stop": stop2, "cross_code": "CLASSICONLY",
                             "steam_code": None, "steam_secret": None, "crossplay": False},
                     daemon=True).start()
    ok = lobby._wait_until(lambda: code_events(io_n), timeout=5)
    check("no Steam: the classic code, cross-play on regardless", ok and code_events(io_n)[-1].get("code") == "CLASSICONLY"
          and code_events(io_n)[-1].get("crossplay") is True)
    check("... and its HELLO gate stays open", hello_answered(("127.0.0.1", nsock.getsockname()[1])))
    stop2.set()
    time.sleep(0.3)
    A.close(); B.close(); ca.close(); cb.close()
    lobby.SEAL[0] = None

print()
if fails:
    print(f"{len(fails)} FAILED")
    for f in fails:
        print("  - " + f)
    raise SystemExit(1)
print("PASS: the Steam-ID code -- decoding, the key over Steam, a sealed Steam-only session, the cross-play gate and its live switch")
