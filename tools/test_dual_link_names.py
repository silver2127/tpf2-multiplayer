"""THE TCP BACKUP LINK MUST SURVIVE A RENAMED JOINER, AND A STEAM JOINER MUST BE DIALLABLE.

2026-09-22, two instances on one PC and one Steam account ('ComradeSilver'):
the host renamed the joiner 'ComradeSilver#2', the joiner's link hello (sent
before the host named it) still said 'ComradeSilver', and the host logged
"[dual] TCP link hello from 192.168.0.141 ('ComradeSilver') matches no joiner
-- closed" and then `tcp_first=0 tcp_only=0` for the whole session.

  1. _match_link_hello: assigned name, then the asked name; several joiners that
     asked for one name are told apart by the connection's address; still
     ambiguous -> refused; a joiner that already has a link is skipped
  2. end to end (lobby._run_dual_round): a joiner asking for the HOST's name gets
     'alice#2' and its TCP link still comes up and covers 30% UDP loss
  3. a joiner in through Steam opens its listener and maps it (fake UPnP): the
     router's WAN IP is offered first (MY_TCP_ADDRS); a failed mapping is logged
  4. bulk_connect says why a dial failed (refused vs timed out)

    python tools/test_dual_link_names.py
"""
import os
import socket
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "netpunch"))

import bulk_tcp   # noqa: E402
import lobby      # noqa: E402

fails = []


def check(name, cond, extra=""):
    print(("ok   " if cond else "FAIL ") + name + (f"  ({extra})" if extra else ""))
    if not cond:
        fails.append(name)


# ---- 1. matching ---------------------------------------------------------- #
M = lobby._match_link_hello
no_link = lambda a: False   # noqa: E731
J1, J2 = ("192.168.0.141", 59752), ("192.168.0.141", 60001)
peers = {J1: {"name": "ComradeSilver#2", "asked": "ComradeSilver"}}
check("1a. the asked name finds the renamed joiner",
      M(peers, "ComradeSilver", ("192.168.0.141", 59752), no_link)[0] == J1)
check("1b. the assigned name still matches",
      M(peers, "ComradeSilver#2", ("192.168.0.141", 5), no_link)[0] == J1)
check("1c. an unknown name matches nobody", M(peers, "mallory", J1, no_link)[0] is None)
peers2 = {J1: {"name": "ComradeSilver#2", "asked": "ComradeSilver"},
          J2: {"name": "ComradeSilver#3", "asked": "ComradeSilver"}}
check("1d. two joiners asked for one name: the connection's exact address decides",
      M(peers2, "ComradeSilver", J2, no_link)[0] == J2)
f, why = M(peers2, "ComradeSilver", ("192.168.0.141", 1234), no_link)
check("1e. same IP, neither address: refused as ambiguous", f is None and "2 joiners" in (why or ""), why)
check("1f. a joiner that already has a link is skipped",
      M(peers2, "ComradeSilver", ("192.168.0.141", 1234), lambda a: a == J1)[0] == J2)
old = {J1: {"name": "bob"}}                       # a peer entry from before 'asked' existed
check("1g. entries without 'asked' still match by name", M(old, "bob", J1, no_link)[0] == J1)

# ---- 2. end to end -------------------------------------------------------- #
ok, delivered = lobby._run_dual_round("renamed-joiner-30pct-loss", 0.30, 0.05, True, joiner_name="alice")
check("2. a joiner renamed 'alice#2' gets its TCP link and every frame arrives", ok and delivered == 1.0,
      f"delivered {delivered:.0%}")

# ---- 3. a Steam joiner maps its listener ---------------------------------- #
logs = []
lobby.MY_TCP_ADDRS[0] = ["192.168.0.141", "25.37.69.230"]
lobby.JOINER_BULK[0] = None
lobby.JOINER_UPNP["done"].clear()
mapped = []
lobby._open_steam_joiner_tcp(0, logs.append, mapper=lambda port: (mapped.append(port) or True, "203.0.113.7", None))
lst = lobby.JOINER_BULK[0]
check("3a. the listener opened and its port was mapped", lst is not None and mapped == [lst.port], str(mapped))
check("3b. the router's WAN IP is offered first", lobby.MY_TCP_ADDRS[0][:1] == ["203.0.113.7"], str(lobby.MY_TCP_ADDRS[0]))
check("3c. the offer keeps the LAN/VPN addresses", "192.168.0.141" in lobby.MY_TCP_ADDRS[0])
check("3d. done is signalled", lobby.JOINER_UPNP["done"].is_set())
check("3e. the listener is reused, never bound twice", lobby._joiner_bulk_listener(0, logs.append) is lst)
lobby.JOINER_UPNP["port"] = None                  # the fake mapping has nothing to remove
if lst is not None:
    lst.close()
lobby.JOINER_BULK[0] = None
logs.clear()
lobby.JOINER_UPNP["done"].clear()
lobby.MY_TCP_ADDRS[0] = ["192.168.0.141"]
lobby._open_steam_joiner_tcp(0, logs.append, mapper=lambda port: (False, None, "no IGD discovered"))
check("3f. a failed mapping keeps the offers and says why",
      lobby.MY_TCP_ADDRS[0] == ["192.168.0.141"] and any("no IGD discovered" in l for l in logs), str(logs))
if lobby.JOINER_BULK[0] is not None:
    lobby.JOINER_BULK[0].close()
lobby.JOINER_BULK[0] = None
lobby.MY_TCP_ADDRS[0] = []

# ---- 4. why a dial failed ------------------------------------------------- #
s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
s.bind(("127.0.0.1", 0))
closed_port = s.getsockname()[1]
s.close()
errs = []
check("4a. a closed port: no socket", bulk_tcp.bulk_connect("127.0.0.1", closed_port, "recv", 1, "t", errors=errs) is None)
check("4b. ... and the reason says refused (or timed out where the stack drops it)",
      len(errs) == 1 and ("refused" in errs[0] or "timed out" in errs[0]), str(errs))
errs = []
bulk_tcp.bulk_connect("192.0.2.1", 9, "recv", 1, "t", timeout=0.5, errors=errs)
check("4c. an address that never answers: the reason is recorded", len(errs) == 1 and errs[0].startswith("192.0.2.1: "), str(errs))

print()
if fails:
    print(f"{len(fails)} FAILED")
    raise SystemExit(1)
print("PASS: a renamed joiner keeps its TCP link; a Steam joiner offers a mapped address; dial failures say why")
