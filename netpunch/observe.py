#!/usr/bin/env python3
"""
observe.py -- build a "connectivity profile" for one machine (Phase 2).

We learn everything a peer needs to reach us, and how constrained our NAT is:

* STUN, run on the SAME socket the game will use (mapping is per-socket), asked
  of several public servers. Two answers with the same mapped port => the NAT
  keeps one external port per socket (endpoint-independent = "normal"). Two
  different ports => symmetric NAT, which is very hard to punch.
* UPnP (miniupnpc, falling back to the upnpc.exe CLI): ask the router for its
  WAN IP and for an explicit port mapping on the game port. Success => "open".
* IPv6 enumeration, preferring a 2002: 6to4 address (globally routable via HE
  relays even when the ISP gives no native v6).

Flags produced:
    open       -- UPnP gave us a forwarded port on the game port
    symmetric  -- STUN mapped ports disagreed across servers
    cgnat      -- carrier-grade NAT (router WAN IP != STUN public IP, or the
                  STUN IP sits in 100.64.0.0/10)
    v6         -- we have a usable global / 6to4 IPv6 address

Profile (printed as JSON, re-runnable since mappings expire):
    {
      "candidates": {"lan_v4": "ip:port", "public_v4": "ip:port",
                     "v6": "[ip]:port" | null},
      "flags": {"open": bool, "symmetric": bool, "cgnat": bool, "v6": bool},
      ... diagnostics ...
    }

Importable:  observe(local_port=29471) -> dict
CLI:         python observe.py [--local-port 29471] [--no-upnp]
"""

from __future__ import annotations

import argparse
import ipaddress
import json
import shutil
import socket
import subprocess
import time

import stun

from punch import DEFAULT_PORT, open_socket

# STUN servers, fastest/most-reliable first so we usually satisfy the two-answer
# comparison before touching a slow one. Google + Cloudflare are the spec set;
# the rest are spares.
STUN_SERVERS = [
    ("stun.l.google.com", 19302),
    ("stun.nextcloud.com", 3478),
    ("stun.cloudflare.com", 3478),
    ("stun.services.mozilla.com", 3478),
]

STUN_PER_SERVER_TIMEOUT = 1.0    # socket timeout per attempt
STUN_TOTAL_DEADLINE = 8.0        # give up after this many seconds total
_CGNAT_NET = ipaddress.ip_network("100.64.0.0/10")


# --------------------------------------------------------------------------- #
# Local address discovery
# --------------------------------------------------------------------------- #
def preferred_ip(family=socket.AF_INET, probe=None):
    """Source IP the OS would use to reach the internet (no packets sent)."""
    if probe is None:
        probe = ("2001:4860:4860::8888", 80) if family == socket.AF_INET6 \
            else ("8.8.8.8", 80)
    s = socket.socket(family, socket.SOCK_DGRAM)
    try:
        s.connect(probe)
        return s.getsockname()[0]
    except OSError:
        return None
    finally:
        s.close()


_ULA = ipaddress.ip_network("fc00::/7")          # Tailscale ULA, etc.
_GLOBAL_UNICAST = ipaddress.ip_network("2000::/3")


def _v6_usable(addr):
    """True if a friend on the open internet could plausibly reach ``addr``.

    NB: Python's ipaddress marks 6to4 (2002::/16) as *private*, so we cannot
    lean on ``is_global`` -- it would drop the very address we want. We keep
    6to4 explicitly and otherwise accept only global unicast (2000::/3) that
    isn't ULA or documentation.
    """
    try:
        ip = ipaddress.IPv6Address(addr)
    except ValueError:
        return False
    if ip.is_link_local or ip.is_loopback or ip.is_multicast:
        return False
    if ip in _ULA:
        return False
    if addr.startswith("2002:"):        # 6to4 -- routable via HE relays
        return True
    if ip in _GLOBAL_UNICAST and not addr.startswith("2001:db8"):
        return True
    return False


def enumerate_v6():
    """Return (best, all_usable) IPv6 addresses, preferring 2002: 6to4.

    Sources: the connect-trick (the address the OS actually routes outbound --
    authoritative) plus getaddrinfo(hostname) (which is racy on Windows and may
    omit the 6to4 address on any given call).
    """
    found = []

    def add(addr):
        if addr:
            a = addr.split("%")[0]
            if a not in found and _v6_usable(a):
                found.append(a)

    add(preferred_ip(socket.AF_INET6))               # authoritative outbound v6
    try:
        for info in socket.getaddrinfo(socket.gethostname(), None,
                                       family=socket.AF_INET6):
            add(info[4][0])
    except socket.gaierror:
        pass

    # Prefer a 6to4 address; it is tied to the real public v4 and reaches any
    # other 6to4/native host through HE relays.
    sixto4 = [a for a in found if a.startswith("2002:")]
    best = sixto4[0] if sixto4 else (found[0] if found else None)
    return best, found


# --------------------------------------------------------------------------- #
# STUN
# --------------------------------------------------------------------------- #
def stun_map(sock, servers=STUN_SERVERS, want=2, deadline=STUN_TOTAL_DEADLINE):
    """Query STUN servers on ``sock`` until we have ``want`` mapped answers.

    Returns (answers, log) where answers is a list of dicts
    {server, ip, port, ms} and log is a human-readable per-server list.
    """
    stun._initialize()  # populate pystun3's msg-type tables (stun_test skips this)
    prev_timeout = sock.gettimeout()
    sock.settimeout(STUN_PER_SERVER_TIMEOUT)
    answers, log = [], []
    start = time.time()
    try:
        for host, port in servers:
            if len(answers) >= want or (time.time() - start) > deadline:
                break
            t0 = time.time()
            try:
                r = stun.stun_test(sock, host, port, "0.0.0.0", 0)
            except Exception as e:                      # noqa: BLE001
                log.append(f"{host}:{port} error {type(e).__name__} "
                           f"({time.time()-t0:.2f}s)")
                continue
            ms = (time.time() - t0) * 1000
            if r.get("Resp") and r.get("ExternalIP"):
                answers.append({"server": f"{host}:{port}",
                                "ip": r["ExternalIP"], "port": r["ExternalPort"],
                                "ms": round(ms)})
                log.append(f"{host}:{port} -> {r['ExternalIP']}:"
                           f"{r['ExternalPort']} ({ms:.0f}ms)")
            else:
                log.append(f"{host}:{port} no-response ({ms:.0f}ms)")
    finally:
        sock.settimeout(prev_timeout)
    return answers, log


# --------------------------------------------------------------------------- #
# UPnP
# --------------------------------------------------------------------------- #
def upnp_map(game_port, keep=False):
    """Try to open ``game_port`` on the router. Degrades gracefully.

    Returns dict: {open, wan_ip, lan_ip, method, detail}. Never raises.
    """
    result = {"open": False, "wan_ip": None, "lan_ip": None,
              "method": None, "detail": None}

    # --- Preferred: the miniupnpc module ---
    try:
        import miniupnpc
        u = miniupnpc.UPnP()
        u.discoverdelay = 2000
        if u.discover() > 0:
            u.selectigd()
            result["method"] = "miniupnpc"
            result["lan_ip"] = u.lanaddr
            try:
                result["wan_ip"] = u.externalipaddress()
            except Exception:                            # noqa: BLE001
                pass
            try:
                ok = u.addportmapping(game_port, "UDP", u.lanaddr, game_port,
                                      "netpunch", "")
                result["open"] = bool(ok)
                if ok and not keep:
                    # We only needed to prove it works; connect.py re-adds it.
                    try:
                        u.deleteportmapping(game_port, "UDP")
                    except Exception:                    # noqa: BLE001
                        pass
            except Exception as e:                       # noqa: BLE001
                result["detail"] = f"addportmapping failed: {e}"
            return result
        result["detail"] = "no IGD discovered"
    except ImportError:
        result["detail"] = "miniupnpc module unavailable"
    except Exception as e:                               # noqa: BLE001
        result["detail"] = f"miniupnpc error: {e}"

    # --- Fallback: the upnpc.exe CLI, if present on PATH ---
    exe = shutil.which("upnpc") or shutil.which("upnpc.exe")
    if exe:
        try:
            out = subprocess.run([exe, "-l"], capture_output=True, text=True,
                                 timeout=8).stdout
            result["method"] = "upnpc.exe"
            for line in out.splitlines():
                low = line.lower()
                if "externalipaddress" in low.replace(" ", ""):
                    result["wan_ip"] = line.split("=")[-1].strip()
                if "local lan ip address" in low:
                    result["lan_ip"] = line.split(":")[-1].strip()
            add = subprocess.run(
                [exe, "-a", result["lan_ip"] or "", str(game_port),
                 str(game_port), "UDP"],
                capture_output=True, text=True, timeout=8)
            result["open"] = "is redirected" in add.stdout.lower() \
                or add.returncode == 0
            if result["open"] and not keep:
                subprocess.run([exe, "-d", str(game_port), "UDP"],
                               capture_output=True, text=True, timeout=8)
        except Exception as e:                           # noqa: BLE001
            result["detail"] = f"upnpc.exe error: {e}"
    return result


# --------------------------------------------------------------------------- #
# Top-level profile
# --------------------------------------------------------------------------- #
def upnp_unmap(game_port):
    """Remove the UDP mapping ``upnp_map(..., keep=True)`` left on the router.
    Best-effort, never raises; returns True if a delete was issued."""
    try:
        import miniupnpc
        u = miniupnpc.UPnP()
        u.discoverdelay = 1000
        if u.discover() > 0:
            u.selectigd()
            try:
                u.deleteportmapping(game_port, "UDP")
                return True
            except Exception:                            # noqa: BLE001
                return False
    except Exception:                                    # noqa: BLE001
        pass
    exe = shutil.which("upnpc") or shutil.which("upnpc.exe")
    if exe:
        try:
            subprocess.run([exe, "-d", str(game_port), "UDP"],
                           capture_output=True, text=True, timeout=8)
            return True
        except Exception:                                # noqa: BLE001
            return False
    return False


def _fmt(ip, port):
    if ip is None:
        return None
    return f"[{ip}]:{port}" if ":" in ip else f"{ip}:{port}"


def observe(local_port=DEFAULT_PORT, sock=None, do_upnp=True, keep_upnp=False):
    """Produce a connectivity profile dict. Reusable at connect time."""
    own_sock = sock is None
    if own_sock:
        sock = open_socket(local_port, socket.AF_INET)
    try:
        t0 = time.time()
        stun_answers, stun_log = stun_map(sock)
        stun_elapsed = time.time() - t0
    finally:
        if own_sock:
            sock.close()

    # --- symmetric: do the mapped ports agree? ---
    ports = {a["port"] for a in stun_answers}
    symmetric = len(ports) > 1
    public_ip = stun_answers[0]["ip"] if stun_answers else None
    public_port = stun_answers[0]["port"] if stun_answers else None

    # --- UPnP ---
    upnp = upnp_map(local_port, keep=keep_upnp) if do_upnp else \
        {"open": False, "wan_ip": None, "lan_ip": None, "method": "skipped",
         "detail": None}

    # --- cgnat: WAN IP disagrees with STUN, or STUN IP is in 100.64/10 ---
    cgnat = False
    if upnp["wan_ip"] and public_ip and upnp["wan_ip"] != public_ip:
        cgnat = True
    if public_ip:
        try:
            if ipaddress.ip_address(public_ip) in _CGNAT_NET:
                cgnat = True
        except ValueError:
            pass

    # --- STUN down? fall back to the UPnP mapping ---
    # STUN is how the public ip:port is normally learned, but both servers can
    # time out (measured 2026-08-30: 4 s no-response from each, while UPnP was
    # perfectly healthy). Without a public candidate the shared code offers only
    # a LAN address and a 6to4 v6, so a friend on another network has nothing to
    # dial -- the lobby just never connects. A successful UPnP mapping already
    # tells us the WAN IP, and the mapping is port-preserving by construction, so
    # wan_ip:local_port IS the public candidate. Flagged in the profile so the
    # diagnosis stays honest about where the address came from.
    public_from_upnp = False
    if not public_ip and upnp["open"] and upnp["wan_ip"]:
        try:
            addr = ipaddress.ip_address(upnp["wan_ip"])
            if not (addr.is_private or addr in _CGNAT_NET):
                public_ip, public_port = upnp["wan_ip"], local_port
                public_from_upnp = True
        except ValueError:
            pass

    # --- IPv6 ---
    v6_best, v6_all = enumerate_v6()

    lan_ip = upnp["lan_ip"] or preferred_ip(socket.AF_INET)

    profile = {
        "candidates": {
            "lan_v4": _fmt(lan_ip, local_port),
            "public_v4": _fmt(public_ip, public_port),
            "v6": _fmt(v6_best, local_port),
        },
        "flags": {
            "open": bool(upnp["open"]),
            "symmetric": bool(symmetric),
            "cgnat": bool(cgnat),
            "v6": v6_best is not None,
        },
        "local_port": local_port,
        "public_from_upnp": public_from_upnp,
        "stun": {
            "answers": stun_answers,
            "log": stun_log,
            "elapsed_ms": round(stun_elapsed * 1000),
            "timed_out": len(stun_answers) < 2,
        },
        "upnp": upnp,
        "v6_all": v6_all,
    }
    return profile


def main(argv=None):
    ap = argparse.ArgumentParser(description="netpunch connectivity profiler")
    ap.add_argument("--local-port", type=int, default=DEFAULT_PORT)
    ap.add_argument("--no-upnp", action="store_true",
                    help="skip the UPnP probe (faster)")
    args = ap.parse_args(argv)
    profile = observe(local_port=args.local_port, do_upnp=not args.no_upnp)
    print(json.dumps(profile, indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
