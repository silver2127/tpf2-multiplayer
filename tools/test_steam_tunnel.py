#!/usr/bin/env python3
"""The Steam transport, offline: the code's SteamID tail, the tunnel client against a
fake tunnel, and a host and a joiner that meet ONLY through the tunnel (no other
candidate), run through the real run_host/race/run_client (netpunch/steamtunnel.py,
connect.py, lobby.py; native/src/steam_tunnel.cpp is the other half).

    python tools/test_steam_tunnel.py
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

import steamtunnel                                    # noqa: E402
from steamtunnel import FakeTunnel, SteamTunnel, TUNNEL_IP, read_identity, is_tunnel_addr   # noqa: E402
from connect import encode_profile, decode_code, race   # noqa: E402
import lobby                                          # noqa: E402
from punch import open_socket                         # noqa: E402

fails = []


def check(name, cond, extra=""):
    print(("ok   " if cond else "FAIL ") + name + (f"  ({extra})" if extra else ""))
    if not cond:
        fails.append(name)


# ---- the code's tail
prof = {"candidates": {"lan_v4": "192.168.1.20:29471", "public_v4": "198.51.100.77:29471", "v6": None,
                       "vpn_v4": None, "vpn2_v4": None, "steam": "76561198012345678"},
        "flags": {"open": True, "symmetric": False, "cgnat": False, "v6": False}}
d = decode_code(encode_profile(prof, secret=bytes(range(12))))
check("the SteamID rides in the code and comes back", d["candidates"]["steam"] == "76561198012345678" and d["secret"] == bytes(range(12)))
check("without one the field is None", decode_code(encode_profile(dict(prof, candidates=dict(prof["candidates"], steam=None))))["candidates"]["steam"] is None)
check("a non-numeric steam value is not encoded", decode_code(encode_profile(dict(prof, candidates=dict(prof["candidates"], steam="nope"))))["candidates"]["steam"] is None)

# ---- identity file + client
with tempfile.TemporaryDirectory() as td:
    check("no identity file -> None", read_identity(td) is None)
    open(os.path.join(td, "tpf2_steam.txt"), "w").write("id=0\nport=5\n")
    check("id=0 -> None", read_identity(td) is None)
    open(os.path.join(td, "tpf2_steam.txt"), "w").write("id=76561198000000001\nport=40000\nname=Al\n")
    check("a valid identity parses", read_identity(td) == ("76561198000000001", 40000, "Al"))
    open(os.path.join(td, "tpf2mp_steam_off.txt"), "w").write("")
    t = SteamTunnel(td, lambda s: None)
    check("the kill switch makes the client unavailable", not t.available and t.id is None and t.dial("1") is None)
    os.remove(os.path.join(td, "tpf2mp_steam_off.txt"))

check("is_tunnel_addr", is_tunnel_addr((TUNNEL_IP, 62100)) and is_tunnel_addr((TUNNEL_IP, 62199)) and not is_tunnel_addr(("127.0.0.1", 29471))
      and not is_tunnel_addr(("10.0.0.1", 62100)) and not is_tunnel_addr(None))

# ---- two fake tunnels: control protocol and delivery
with tempfile.TemporaryDirectory() as ta, tempfile.TemporaryDirectory() as tb:
    A = FakeTunnel(ta, 1001, "alice")
    B = FakeTunnel(tb, 2002, "bob")
    la = socket.socket(socket.AF_INET, socket.SOCK_DGRAM); la.bind(("0.0.0.0", 0)); la.settimeout(2)
    lb = socket.socket(socket.AF_INET, socket.SOCK_DGRAM); lb.bind(("0.0.0.0", 0)); lb.settimeout(2)
    ca = SteamTunnel(ta, lambda s: None)
    cb = SteamTunnel(tb, lambda s: None)
    check("the clients read the fakes' identities", ca.id == "1001" and cb.id == "2002" and ca.name == "alice")
    check("hello", ca.hello(la.getsockname()[1]) and cb.hello(lb.getsockname()[1]))
    check("dialling oneself is refused", ca.dial("1001") is None)
    ep_b = ca.dial("2002")
    check("DIAL answers an endpoint in the tunnel port range", ep_b is not None and is_tunnel_addr(ep_b), str(ep_b))
    la.sendto(b"ping", ep_b)
    data, frm = lb.recvfrom(65535)
    check("a datagram to A's endpoint for B reaches B's lobby from B's endpoint for A", data == b"ping" and is_tunnel_addr(frm), str(frm))
    lb.sendto(b"pong", frm)
    data, frm2 = la.recvfrom(65535)
    check("and the reply comes back to A from the endpoint it dialled", data == b"pong" and frm2 == ep_b, f"{frm2} vs {ep_b}")
    big = bytes(1400)
    la.sendto(big, ep_b)
    data, _ = lb.recvfrom(65535)
    check("a 1,400-byte datagram (over the unreliable limit) arrives whole", data == big)
    check("STATUS answers", "endpoints=" in ca.status())
    ca.close_peer("2002")
    check("CLOSE drops the endpoint", "endpoints=0" in ca.status(), ca.status())

    # ---- a real host and joiner meeting only through the tunnel
    base = tempfile.mkdtemp(prefix="steam_lobby_")
    io_h, io_j = lobby.LobbyIO(os.path.join(base, "h")), lobby.LobbyIO(os.path.join(base, "j"))
    stop = threading.Event()
    hsock = open_socket(0, socket.AF_INET)
    ca.hello(hsock.getsockname()[1])
    threading.Thread(target=lobby.run_host, name="host", args=(hsock, "alice", io_h),
                     kwargs={"code": "STEAMTESTCODE", "stop": stop}, daemon=True).start()
    time.sleep(0.4)
    jsock = open_socket(0, socket.AF_INET)
    cb.hello(jsock.getsockname()[1])
    late = []
    peer = {"candidates": {"public_v4": None, "lan_v4": None, "v6": None, "vpn_v4": None, "vpn2_v4": None, "steam": "1001"},
            "flags": {"open": False, "v6": False}}
    def dial_late():
        ep = cb.dial("1001")
        if ep:
            late.append(ep)
    threading.Thread(target=dial_late, daemon=True).start()
    conn = race(jsock, peer, "dial", jsock.getsockname()[1], 10, my_has_v6=False, late_targets=late)
    check("the joiner connects to the host with the Steam endpoint as its only candidate",
          conn is not None and is_tunnel_addr(conn.peer), str(conn and conn.peer))
    if conn is not None:
        threading.Thread(target=lobby.run_client, name="client", args=(conn, "bob", io_j),
                         kwargs={"stop": stop}, daemon=True).start()
        def rosters():
            rh, rj = lobby._latest_roster(io_h.out_path), lobby._latest_roster(io_j.out_path)
            return bool(rh and rj and sorted(rh.get("players", [])) == ["alice", "bob"] == sorted(rj.get("players", [])))
        check("both rosters list alice and bob (roster traffic crossed the tunnel)", lobby._wait_until(rosters, timeout=12),
              f"h={lobby._latest_roster(io_h.out_path)} j={lobby._latest_roster(io_j.out_path)}")
        with open(io_j.in_path, "a", encoding="utf-8") as f:
            f.write(json.dumps({"cmd": "chat", "text": "hello over steam"}) + "\n")
        check("a chat line crosses the tunnel", lobby._wait_until(lambda: lobby._has_chat(io_h.out_path, "hello over steam"), timeout=8))
        check("the host's tunnel delivered the joiner's packets", A.received > 0 and "2002" in A.endpoints)
    stop.set()
    time.sleep(0.5)
    A.close(); B.close(); ca.close(); cb.close()

# ---- the save transfer's view of a tunnel peer: not loopback
pick, win = lobby._HostSaveTransfer._pick_chunk, lobby._HostSaveTransfer._pick_window
big = lobby.CHUNK_STEAM if lobby.STEAM_BIG_CHUNKS else lobby.CHUNK_STEAM_MIXED
check("tunnel peers (and loopback ones) get the Steam chunk (big only while STEAM_BIG_CHUNKS is on)",
      pick([((TUNNEL_IP, 62100), "bob")]) == big
      and pick([(("127.0.0.1", 29521), "carol"), ((TUNNEL_IP, 62105), "bob")]) == big)
check("a tunnel peer beside an internet peer: the small size that fits both",
      pick([(("198.51.100.7", 29471), "dave"), ((TUNNEL_IP, 62105), "bob")]) == lobby.CHUNK_STEAM_MIXED
      and lobby.CHUNK_STEAM_MIXED + 17 + 28 < 1200 and win(lobby.CHUNK_STEAM_MIXED) == lobby.SEND_WINDOW_REMOTE)
check("loopback-only stays local, an internet peer stays internet-safe",
      pick([(("127.0.0.1", 29521), "carol")]) == lobby.CHUNK_LOCAL and pick([(("198.51.100.7", 29471), "dave")]) == lobby.CHUNK_DATA)
check("big Steam chunks: a byte-bounded window under Steam's 8 MB send buffer, one loopback datagram each",
      win(lobby.CHUNK_STEAM) == lobby.SEND_WINDOW_STEAM and lobby.CHUNK_STEAM * lobby.SEND_WINDOW_STEAM <= 4 * 1024 * 1024
      and lobby.CHUNK_STEAM + 17 + 28 + 64 < 65507)

# ---- the C++ half, by anchor
src = open(os.path.join(ROOT, "native", "src", "steam_tunnel.cpp"), encoding="utf-8").read()
bridge = open(os.path.join(ROOT, "native", "src", "bridge_main.cpp"), encoding="utf-8").read()
bat = open(os.path.join(ROOT, "native", "build.bat"), encoding="utf-8").read()
check("the tunnel resolves the legacy P2P API from the game's own steam_api64.dll",
      'GetModuleHandleW(L"steam_api64.dll")' in src and '"SteamAPI_SteamNetworking_v006"' in src and '"SteamAPI_ISteamNetworking_SendP2PPacket"' in src)
check("relay through Valve is allowed and sessions are accepted from the callback", "g_api.allowRelay(g_api.net, true);" in src and "CB_SESSION_REQUEST = 1202" in src and "g_api.accept(g_api.net, id)" in src)
check("packets over 1,200 bytes go reliable", "UNRELIABLE_MAX = 1200" in src and "SEND_RELIABLE : SEND_UNRELIABLE" in src)
check("endpoints live on 127.0.0.1 ports 62100-62199 and the identity file is tpf2_steam.txt",
      'TUNNEL_PORT_LO = 62100, TUNNEL_PORT_HI = 62199' in src and 'L"tpf2_steam.txt"' in src and list(steamtunnel.TUNNEL_PORTS) == list(range(62100, 62200)))
check("the kill switch", 'L"tpf2mp_steam_off.txt"' in src)
check("Steam's send rate and buffers are raised by the right ids (SendRateMin 10, SendRateMax 11, SendBufferSize 9, RecvBufferSize 47; "
      "23/24 are IP_AllowWithoutAuth/TimeoutInitial and must not be touched)",
      '{ "SendRateMin",    10,' in src and '{ "SendRateMax",    11,' in src
      and '{ "SendRateMin",    23,' not in src and '{ "SendRateMax",    24,' not in src and '{ "SendBufferSize",  9,' in src and '{ "RecvBufferSize", 47,' in src
      and '"SteamAPI_ISteamNetworkingUtils_SetConfigValue"' in src)
check("the bridge starts it after its identity is written", "SteamTunnel_Start(g_dataDir, Log);" in bridge and bridge.index("SteamTunnel_Start") > bridge.index("SetPlayerPatch_Install(Log);"))
check("build.bat compiles and links it into the bridge", "src\\steam_tunnel.cpp" in bat and "out\\steam_tunnel_mp.obj ||" in bat)
lob = open(os.path.join(ROOT, "netpunch", "lobby.py"), encoding="utf-8").read()
check("the lobby skips the TCP side channels for tunnel peers", lob.count("steamtunnel.is_tunnel_addr(") >= 4)

if fails:
    raise SystemExit("FAIL: " + ", ".join(fails))
print("PASS: the Steam transport -- code tail, tunnel client, delivery through two fake tunnels, a host and a joiner meeting only through Steam")
