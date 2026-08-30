#!/usr/bin/env python3
"""
connect.py -- code exchange, the connect race, and the C++-facing CLI (Phase 3)
plus named failure diagnosis (Phase 4).

This is THE interface the Transport Fever 2 mod menu drives. Two friends each
run one command and paste a short base32 code to each other over Discord:

    python connect.py host            -> prints CODE=..., waits for the joiner
    python connect.py join <CODE>     -> prints its own CODE=..., dials the host

Both write machine-readable progress to ``netpunch_status.txt`` (JSON lines):
    {"state":"waiting"}
    {"state":"connected","peer":"1.2.3.4:29471"}   (or)
    {"state":"failed","reason":"..."}
Exit code is 0 on connect, non-zero on failure. Only the single ``CODE=`` line
goes to stdout; all diagnostics go to stderr so the caller can parse cleanly.

------------------------------------------------------------------------------
Code format (base32 of a compact binary blob, ~45-55 chars)
------------------------------------------------------------------------------
    byte 0   bitfield:
             0x01 flag open        0x08 present lan_v4
             0x02 flag symmetric   0x10 present public_v4
             0x04 flag cgnat       0x20 present v6
                                   0x40 v6 is 6to4 derived from public_v4
    byte 1-4 uint32 BE unix timestamp (for the >10-min staleness warning)
    [lan_v4]     4-byte IPv4 + 2-byte port   (if present)
    [public_v4]  4-byte IPv4 + 2-byte port   (if present)
    [v6]         2-byte port + (10 bytes low-order if 6to4-derived else 16 bytes)
The 6to4 trick: a 2002:VVVV:VVVV:... address embeds the public IPv4 in its top
48 bits, so when public_v4 is also present we ship only the low 80 bits.
"""

from __future__ import annotations

import argparse
import base64
import ipaddress
import json
import os
import socket
import struct
import sys
import threading
import time

from punch import DEFAULT_PORT, Connection, open_socket
from observe import observe

STATUS_FILE = "netpunch_status.txt"
CODE_TTL = 600          # seconds; codes older than this warn "stale"
DEFAULT_TIMEOUT = 40    # seconds to keep punching before giving up

# byte-0 bit flags
_F_OPEN = 0x01
_F_SYM = 0x02
_F_CGNAT = 0x04
_P_LAN = 0x08
_P_PUB = 0x10
_P_V6 = 0x20
_V6_6TO4 = 0x40


# --------------------------------------------------------------------------- #
# small utilities
# --------------------------------------------------------------------------- #
def log(msg):
    """Diagnostics -> stderr (stdout is reserved for the CODE= line)."""
    print(msg, file=sys.stderr, flush=True)


def write_status(obj, reset=False):
    """Append (or reset) a JSON-line to the status file."""
    mode = "w" if reset else "a"
    with open(STATUS_FILE, mode, encoding="utf-8") as f:
        f.write(json.dumps(obj) + "\n")


def parse_hostport(s):
    """'1.2.3.4:29471' or '[2002::1]:29471' -> (ip, port). None-safe."""
    if not s:
        return None
    if s.startswith("["):
        ip, _, rest = s[1:].partition("]")
        return ip, int(rest.lstrip(":"))
    ip, _, port = s.rpartition(":")
    return ip, int(port)


# --------------------------------------------------------------------------- #
# code encode / decode
# --------------------------------------------------------------------------- #
def encode_profile(profile):
    """Serialize a profile dict (from observe) to a short base32 code."""
    cand = profile["candidates"]
    flags = profile["flags"]
    lan = parse_hostport(cand.get("lan_v4"))
    pub = parse_hostport(cand.get("public_v4"))
    v6 = parse_hostport(cand.get("v6"))

    b0 = 0
    if flags.get("open"):
        b0 |= _F_OPEN
    if flags.get("symmetric"):
        b0 |= _F_SYM
    if flags.get("cgnat"):
        b0 |= _F_CGNAT

    body = b""
    if lan:
        b0 |= _P_LAN
        body += socket.inet_aton(lan[0]) + struct.pack("!H", lan[1])
    if pub:
        b0 |= _P_PUB
        body += socket.inet_aton(pub[0]) + struct.pack("!H", pub[1])
    if v6:
        b0 |= _P_V6
        v6bytes = socket.inet_pton(socket.AF_INET6, v6[0])
        derived = (pub is not None and v6bytes[0:2] == b"\x20\x02"
                   and v6bytes[2:6] == socket.inet_aton(pub[0]))
        chunk = struct.pack("!H", v6[1])
        if derived:
            b0 |= _V6_6TO4
            chunk += v6bytes[6:16]        # low 80 bits only
        else:
            chunk += v6bytes              # full 128 bits
        body += chunk

    blob = bytes([b0]) + struct.pack("!I", int(time.time())) + body
    return base64.b32encode(blob).decode("ascii").rstrip("=")


def decode_code(code):
    """base32 code -> profile-like dict with candidates, flags, ts, age, stale.

    Raises ValueError on a malformed code.
    """
    code = code.strip().upper()
    pad = "=" * (-len(code) % 8)
    try:
        blob = base64.b32decode(code + pad)
    except Exception as e:                               # noqa: BLE001
        raise ValueError(f"not valid base32: {e}")
    if len(blob) < 5:
        raise ValueError("code too short")

    b0 = blob[0]
    ts = struct.unpack("!I", blob[1:5])[0]
    off = 5

    def take(n):
        nonlocal off
        chunk = blob[off:off + n]
        if len(chunk) < n:
            raise ValueError("truncated code")
        off += n
        return chunk

    lan = pub = v6 = None
    if b0 & _P_LAN:
        ip = socket.inet_ntoa(take(4))
        lan = f"{ip}:{struct.unpack('!H', take(2))[0]}"
    if b0 & _P_PUB:
        ip = socket.inet_ntoa(take(4))
        pub = f"{ip}:{struct.unpack('!H', take(2))[0]}"
    if b0 & _P_V6:
        port = struct.unpack("!H", take(2))[0]
        if b0 & _V6_6TO4:
            if not pub:
                raise ValueError("6to4-derived v6 but no public_v4 to rebuild")
            hi = b"\x20\x02" + socket.inet_aton(pub.rsplit(":", 1)[0])
            v6bytes = hi + take(10)
        else:
            v6bytes = take(16)
        v6 = f"[{socket.inet_ntop(socket.AF_INET6, v6bytes)}]:{port}"

    age = int(time.time()) - ts
    return {
        "candidates": {"lan_v4": lan, "public_v4": pub, "v6": v6},
        "flags": {
            "open": bool(b0 & _F_OPEN),
            "symmetric": bool(b0 & _F_SYM),
            "cgnat": bool(b0 & _F_CGNAT),
            "v6": v6 is not None,
        },
        "ts": ts,
        "age": age,
        "stale": age > CODE_TTL,
    }


# --------------------------------------------------------------------------- #
# role selection & diagnosis
# --------------------------------------------------------------------------- #
def _constrained(flags):
    return bool(flags.get("symmetric") or flags.get("cgnat"))


def choose_role(mine, peer):
    """Return 'listen', 'dial', or 'punch' (complementary across the two ends).

    Open side listens / constrained side dials only when exactly one end is
    constrained; otherwise both punch simultaneously (never deadlocks).
    """
    if not peer:
        return "listen"                      # host with no peer code: just wait
    my_c, peer_c = _constrained(mine), _constrained(peer["flags"])
    if my_c and not peer_c and peer["flags"].get("open"):
        return "dial"
    if not my_c and mine.get("open") and peer_c:
        return "listen"
    return "punch"


def diagnose(mine, peer, connected):
    """Return a human/machine reason string when a connect fails, else None."""
    if connected:
        return None
    if peer and peer.get("stale"):
        return "code stale -- re-exchange a fresh code"
    if mine and mine["stun"].get("timed_out") and not mine["candidates"].get("public_v4"):
        return "no UDP out -- STUN got no response; a firewall is likely blocking UDP"
    mf = mine["flags"] if mine else {}
    pf = peer["flags"] if peer else {}
    if mf.get("cgnat") and pf.get("cgnat"):
        return "both behind CGNAT -- use Tailscale tonight"
    if mf.get("symmetric") and pf.get("symmetric"):
        return "both symmetric NATs -- direct punch won't work; use Tailscale"
    if _constrained(mf) and _constrained(pf):
        return "both ends NAT-constrained -- use Tailscale"
    return "handshake timed out on all candidate pairs"


# --------------------------------------------------------------------------- #
# the race
# --------------------------------------------------------------------------- #
def _targets_v4(peer):
    out = []
    for key in ("public_v4", "lan_v4"):
        hp = parse_hostport(peer["candidates"].get(key))
        if hp:
            out.append(hp)
    return out


def race(sock_v4, peer, role, local_port, timeout, my_has_v6):
    """Fire per-family Connections at the peer's candidates; first wins.

    Returns the winning Connection (still live) or None. Losing families are
    closed. ``sock_v4`` is the shared game/STUN socket so the NAT mapping holds.
    """
    conns = []          # list of (family_label, Connection)
    listen = role == "listen"

    # --- IPv4 family (always attempted; reuses the game socket) ---
    v4_targets = [] if listen else _targets_v4(peer)
    v4 = Connection(sock_v4, v4_targets, name="v4", listen=listen,
                    log=log)
    conns.append(("v4", v4))

    # --- IPv6 family (only if both ends have v6) ---
    sock_v6 = None
    if my_has_v6 and peer["flags"].get("v6") and peer["candidates"].get("v6"):
        try:
            sock_v6 = open_socket(local_port, socket.AF_INET6)
            v6_targets = [] if listen else [parse_hostport(peer["candidates"]["v6"])]
            v6 = Connection(sock_v6, v6_targets, name="v6", listen=listen,
                            log=log)
            conns.append(("v6", v6))
        except OSError as e:
            log(f"[race] v6 socket unavailable: {e}")

    log(f"[race] role={role}  families={[c[0] for c in conns]}  "
        f"targets_v4={v4_targets}")

    # --- wait for the first family to connect ---
    deadline = time.time() + timeout
    winner = None
    while time.time() < deadline and winner is None:
        for label, c in conns:
            if c.connected.is_set():
                winner = (label, c)
                break
        time.sleep(0.05)

    # --- log outcomes, close losers ---
    for label, c in conns:
        if winner and c is winner[1]:
            log(f"[race] WON on {label} via {c.peer_str}")
        else:
            log(f"[race] {label} did not win ({'connected' if c.connected.is_set() else 'no handshake'}); closing")
            c.close()

    return winner[1] if winner else None


# --------------------------------------------------------------------------- #
# host / join commands
# --------------------------------------------------------------------------- #
def _observe_and_announce(local_port):
    """Observe on a fresh game socket, print our CODE= line, return (sock, profile, code)."""
    sock_v4 = open_socket(local_port, socket.AF_INET)
    profile = observe(local_port=local_port, sock=sock_v4, do_upnp=True,
                      keep_upnp=True)
    code = encode_profile(profile)
    log(f"[observe] flags={profile['flags']} "
        f"candidates={profile['candidates']} "
        f"stun_elapsed_ms={profile['stun']['elapsed_ms']} "
        f"stun_timed_out={profile['stun']['timed_out']}")
    # The one and only stdout line:
    print(f"CODE={code}", flush=True)
    return sock_v4, profile, code


def cmd_host(args):
    write_status({"state": "waiting"}, reset=True)
    sock_v4, profile, _ = _observe_and_announce(args.local_port)

    # Host defaults to listening (it shared first and is usually the open end).
    # If the joiner's reply code is fed on stdin, we can flip to dial/punch for
    # the awkward "host is the constrained one" case.
    peer_holder = {"peer": None}

    def stdin_reader():
        for line in sys.stdin:
            line = line.strip()
            if line.upper().startswith("CODE="):
                line = line[5:]
            if not line:
                continue
            try:
                peer_holder["peer"] = decode_code(line)
                log(f"[host] got peer code: flags={peer_holder['peer']['flags']}")
            except ValueError as e:
                log(f"[host] ignoring bad peer code: {e}")
            break

    threading.Thread(target=stdin_reader, daemon=True).start()
    # Give a brief moment for a piped reply code to arrive before deciding role.
    time.sleep(0.3)
    peer = peer_holder["peer"]
    role = choose_role(profile["flags"], peer) if peer else "listen"

    conn = race(sock_v4, peer or {"candidates": {}, "flags": {}}, role,
                args.local_port, args.timeout, profile["flags"]["v6"])
    return _finish(conn, profile, peer)


def cmd_join(args):
    write_status({"state": "waiting"}, reset=True)
    try:
        peer = decode_code(args.code)
    except ValueError as e:
        log(f"[join] bad code: {e}")
        write_status({"state": "failed", "reason": f"bad code: {e}"})
        return 2
    if peer["stale"]:
        log(f"[join] WARNING: code is {peer['age']}s old (>{CODE_TTL}s) -- may be stale")
    log(f"[join] peer flags={peer['flags']} candidates={peer['candidates']}")

    sock_v4, profile, _ = _observe_and_announce(args.local_port)
    role = choose_role(profile["flags"], peer)

    conn = race(sock_v4, peer, role, args.local_port, args.timeout,
                profile["flags"]["v6"])
    return _finish(conn, profile, peer)


def _finish(conn, profile, peer):
    if conn:
        write_status({"state": "connected", "peer": conn.peer_str})
        log(f"[connect] CONNECTED peer={conn.peer_str}")
        # Hand off: keep the process alive so the socket/mapping stays open for
        # the game layer. The mod embeds this; standalone we just idle.
        try:
            while conn.last_seen_age() < 60:
                time.sleep(1)
        except KeyboardInterrupt:
            pass
        conn.close()
        return 0
    reason = diagnose(profile, peer, connected=False)
    write_status({"state": "failed", "reason": reason})
    log(f"[connect] FAILED: {reason}")
    return 1


# --------------------------------------------------------------------------- #
# self-test: codec round-trip + a localhost dial/listen race
# --------------------------------------------------------------------------- #
def selftest():
    ok = True

    # --- codec round-trips ---
    samples = [
        {"candidates": {"lan_v4": "192.168.1.20:29471",
                        "public_v4": "198.51.100.77:29471",
                        "v6": "[2002:c633:644d:1::1000]:29471"},
         "flags": {"open": True, "symmetric": False, "cgnat": False, "v6": True}},
        {"candidates": {"lan_v4": "10.0.0.5:5000", "public_v4": None,
                        "v6": "[2001:db8:9b::1925:45e6]:5000"},
         "flags": {"open": False, "symmetric": True, "cgnat": True, "v6": True}},
        {"candidates": {"lan_v4": None, "public_v4": "203.0.113.9:1234",
                        "v6": None},
         "flags": {"open": True, "symmetric": False, "cgnat": False, "v6": False}},
    ]
    for i, s in enumerate(samples):
        code = encode_profile(s)
        d = decode_code(code)
        match = (d["candidates"] == s["candidates"]
                 and d["flags"] == s["flags"])
        print(f"[selftest] codec #{i}: len={len(code)} "
              f"{'OK' if match else 'MISMATCH'}  code={code}")
        if not match:
            ok = False
            print(f"           in ={s['candidates']} {s['flags']}")
            print(f"           out={d['candidates']} {d['flags']}")

    # --- stale detection ---
    old = encode_profile(samples[0])
    blob = bytearray(base64.b32decode(old + "=" * (-len(old) % 8)))
    struct.pack_into("!I", blob, 1, int(time.time()) - 3600)  # 1h old
    stale_code = base64.b32encode(bytes(blob)).decode().rstrip("=")
    print(f"[selftest] stale flag: {'OK' if decode_code(stale_code)['stale'] else 'FAIL'}")
    ok = ok and decode_code(stale_code)["stale"]

    # --- localhost race: a listener and a dialer must meet ---
    pa, pb = 29491, 29492
    sock_listen = open_socket(pa, socket.AF_INET)
    sock_dial = open_socket(pb, socket.AF_INET)
    peer_for_listener = {"candidates": {}, "flags": {}}
    peer_for_dialer = {"candidates": {"public_v4": f"127.0.0.1:{pa}",
                                      "lan_v4": None, "v6": None},
                       "flags": {"open": True, "v6": False}}
    res = {}
    tl = threading.Thread(target=lambda: res.__setitem__(
        "L", race(sock_listen, peer_for_listener, "listen", pa, 8, False)))
    td = threading.Thread(target=lambda: res.__setitem__(
        "D", race(sock_dial, peer_for_dialer, "dial", pb, 8, False)))
    tl.start(); td.start(); tl.join(); td.join()
    race_ok = bool(res.get("L") and res.get("D")
                   and res["L"].connected.is_set() and res["D"].connected.is_set())
    print(f"[selftest] listen/dial race: {'OK' if race_ok else 'FAIL'} "
          f"(L peer={res.get('L') and res['L'].peer_str}, "
          f"D peer={res.get('D') and res['D'].peer_str})")
    for c in res.values():
        if c:
            c.close()
    ok = ok and race_ok

    print(f"[selftest] {'PASS' if ok else 'FAIL'}")
    return ok


# --------------------------------------------------------------------------- #
# CLI
# --------------------------------------------------------------------------- #
def main(argv=None):
    ap = argparse.ArgumentParser(description="netpunch connect / code exchange")
    ap.add_argument("mode", choices=["host", "join", "selftest"])
    ap.add_argument("code", nargs="?", help="peer CODE (join mode)")
    ap.add_argument("--local-port", type=int, default=DEFAULT_PORT)
    ap.add_argument("--timeout", type=int, default=DEFAULT_TIMEOUT)
    args = ap.parse_args(argv)

    if args.mode == "selftest":
        return 0 if selftest() else 1
    if args.mode == "host":
        return cmd_host(args)
    if args.mode == "join":
        if not args.code:
            ap.error("join requires a CODE argument")
        return cmd_join(args)
    return 2


if __name__ == "__main__":
    sys.exit(main())
