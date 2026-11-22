"""Small guards in the lobby's network modules.

1. seal.py: a counter far ahead of the replay window used to shift the window
   bitmap by the whole jump before masking it -- one frame with counter
   0xFFFFFFFF after counter 1 built a ~512 MB integer.
2. bulk_tcp.py: a TCP link handler that raised took the listener's pending
   count down twice (below zero), loosening its PENDING_MAX cap.

    python tools/net_guards_test.py
"""
import os
import socket
import struct
import sys
import time

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(REPO, "netpunch"))

import bulk_tcp   # noqa: E402
import seal       # noqa: E402

fails = []


def check(name, cond, extra=""):
    print(("ok   " if cond else "FAIL ") + name + (f"  ({extra})" if extra else ""))
    if not cond:
        fails.append(name)


# ---- 1. the replay window ----
s = seal.Sealer(seal.derive_key(b"k" * 16))
salt = b"abcd"
accept = lambda c: s._accept_nonce(salt + struct.pack("!I", c))   # noqa: E731
t0 = time.time()
results = [accept(c) for c in (1, 2, 0xFFFFFFFF, 5, 0xFFFFFFFF, 0xFFFFFFFE)]
took = time.time() - t0
check("a jump to the top counter is accepted, then replays and old ones refused, a close one accepted",
      results == [True, True, True, False, False, True], str(results))
check("...without building a huge bitmap", took < 0.05 and s._windows[salt][1].bit_length() <= seal.WINDOW, f"{took:.3f} s")

# ---- 2. the bulk listener's pending count ----
L = bulk_tcp.BulkListener(0, lambda _: None, bind="127.0.0.1")


def failing_handler(c, addr, line):
    raise OSError("boom")


L.link_handler = failing_handler
for _ in range(3):
    c = socket.create_connection(("127.0.0.1", L.port))
    c.sendall(b"TPF2LINK1 bob 00\n")
    time.sleep(0.3)
    c.close()
check("a link handler that raises leaves the pending count at 0", L._pending == 0, str(L._pending))

# ---- 3. connect.py's lines, when the lobby runs them, are redacted and forwarded ----
import connect   # noqa: E402
import lobby     # noqa: E402

got = []
lobby._log_sinks.append(got.append)
connect.log("[race] WON on v4 via 203.0.113.9:4000")
lobby._log_sinks.remove(got.append)
check("a connect.py line run by the lobby reaches the merged log, its IP masked",
      got == ["[race] WON on v4 via 203.0.113.x:4000"], str(got))

print()
if fails:
    print(f"{len(fails)} FAILED")
    sys.exit(1)
print("PASS: the replay window and the pending count hold")
