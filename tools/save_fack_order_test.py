"""The host's save transfer: a late fack is not a restart; a base of 0 is.

A receiver only ever moves its base back to 0 (a whole-file re-request after a
hash mismatch). The host treated ANY smaller base as that restart, so an older
fack that arrived late (a TCP copy landing after a newer UDP one) re-streamed
from there and reset the Steam window.

    python tools/save_fack_order_test.py
"""
import os
import sys

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(REPO, "netpunch"))

import lobby   # noqa: E402

lobby.BULK_TCP[0] = False
fails = []


def check(name, cond, extra=""):
    print(("ok   " if cond else "FAIL ") + name + (f"  ({extra})" if extra else ""))
    if not cond:
        fails.append(name)


class Sock:
    def sendto(self, data, addr):
        pass


class IO:
    def emit(self, ev):
        pass


A = ("10.1.1.1", 5000)


def transfer():
    logs = []
    size = 1350 * 5000
    x = lobby._HostSaveTransfer(Sock(), 1, bytearray(size), [{"name": "incoming_save.sav", "size": size, "sha256": ""}],
                                [(A, "bob")], IO(), logs.append)
    x.on_begin_ack(A, {"t": "fbegin_ack", "sid": 1})
    x.on_fack(A, {"sid": 1, "base": 300, "nack": []})
    return x, logs


x, logs = transfer()
x.on_fack(A, {"sid": 1, "base": 200, "nack": []})
p = x.peers[A]
check("a late fack (base 200 after 300) leaves the cursor at 300", p["base"] == 300 and p["next"] >= 300, f"base {p['base']} next {p['next']}")
check("...and logs no rewind", not any("rewound" in l for l in logs))

x, logs = transfer()
x.on_fack(A, {"sid": 1, "base": 0, "nack": []})
p = x.peers[A]
check("a re-request (base 0) rewinds the cursor to 0", p["base"] == 0 and p["next"] == 0, f"base {p['base']} next {p['next']}")
check("...and says so", any("rewound save cursor 300 -> 0" in l for l in logs))

print()
if fails:
    print(f"{len(fails)} FAILED")
    sys.exit(1)
print("PASS: late facks are ignored, a re-request rewinds")
