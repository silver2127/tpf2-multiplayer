"""A sealed punch session only moves its peer address on a proven frame.

CONNECTED carries no proof, and the plaintext carve-outs of a sealed session
(save chunks b"NPF1..." and the host's "wrong password" reject) are accepted
from anyone. All three used to set the session's peer to their sender, so one
spoofed datagram redirected every later sealed frame to it. Now they are still
delivered where they were, but only a sealed/signed frame (or the handshake)
moves the peer.

    python tools/punch_peer_lock_test.py
"""
import os
import socket
import sys
import threading
import time

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(REPO, "netpunch"))

import punch   # noqa: E402
import seal    # noqa: E402

fails = []


def check(name, cond, extra=""):
    print(("ok   " if cond else "FAIL ") + name + (f"  ({extra})" if extra else ""))
    if not cond:
        fails.append(name)


def free_port():
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.bind(("127.0.0.1", 0))
    p = s.getsockname()[1]
    s.close()
    return p


pa, pb = free_port(), free_port()
res = {}
threads = [threading.Thread(target=lambda n, me, other: res.__setitem__(n, punch.dial(("127.0.0.1", other), timeout=10, local_port=me, name=n)),
                            args=(n, me, other)) for n, me, other in (("A", pa, pb), ("B", pb, pa))]
for t in threads:
    t.start()
for t in threads:
    t.join()
a, b = res.get("A"), res.get("B")
check("the pair connects", bool(a and b))
if a and b:
    key = seal.derive_key(b"k" * 16)
    a.cipher, b.cipher = seal.Sealer(key), seal.Sealer(key)
    a.send(b"hello")
    got = b.recv(timeout=2)
    real = b.peer
    check("sealed data flows", got == b"hello" and real is not None)

    evil = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    evil.bind(("127.0.0.1", 0))
    evil.sendto(punch._pack(punch.TYPE_CONNECTED, os.urandom(8)), ("127.0.0.1", pb))
    time.sleep(0.3)
    check("a spoofed CONNECTED does not move the peer", b.peer == real, f"{b.peer} vs {real}")
    evil.sendto(punch._pack(punch.TYPE_DATA, punch.CHUNK_PREFIX + b"junk"), ("127.0.0.1", pb))
    delivered = b.recv(timeout=1)
    check("a plain save chunk is still delivered", delivered == punch.CHUNK_PREFIX + b"junk", repr(delivered))
    check("...and does not move the peer", b.peer == real, f"{b.peer} vs {real}")
    evil.sendto(punch._pack(punch.TYPE_DATA, b'{"t": "reject", "reason": "x"}'), ("127.0.0.1", pb))
    rej = b.recv(timeout=1)
    check("a plain reject is still delivered", rej is not None and rej.startswith(b'{"t": "reject"'), repr(rej))
    check("...but does not move the peer", b.peer == real, f"{b.peer} vs {real}")
    a.send(b"still")
    check("sealed data still flows from the real peer", b.recv(timeout=2) == b"still" and b.peer == real)
    evil.close()
for c in (a, b):
    if c:
        c.close()

print()
if fails:
    print(f"{len(fails)} FAILED")
    sys.exit(1)
print("PASS: only proven frames move the peer")
