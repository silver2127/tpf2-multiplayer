"""CM.stopsSigEqual (lines.lua) matches two stop lists within 2 m, with waypoints too.

A stop record is f1..f7[,f8,f9][^cargo][~waypoints]. The comparison stripped the
^cargo suffix but not ~waypoints, so the last field of a record with waypoints read
as nil, the arithmetic raised inside the pcall, and the lists compared unequal after
any jitter: a created line with waypoints could fail to pair with its key.

This cuts the real function out of lines.lua and runs it in Lua 5.2 (lupa).

    python tools/stop_sig_equal_test.py
"""
import os
import sys

from lupa import lua52

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
LINES = os.path.join(REPO, "mod", "mp_lockstep_1", "res", "scripts", "mp", "lines.lua")

src = open(LINES, encoding="utf-8").read()
start = src.index("function CM.stopsSigEqual(a, b)")
end = src.index("\nend\n", start) + len("\nend\n")
L = lua52.LuaRuntime()
eq = L.execute("local CM = {}\n" + src[start:end] + "\nreturn CM.stopsSigEqual")

fails = []


def check(name, cond):
    print(("ok   " if cond else "FAIL ") + name)
    if not cond:
        fails.append(name)


A9 = "100.0,200.0,3,1,0,0,60,105.0,205.0"
B9 = "100.5,200.5,3,1,0,0,60,105.5,205.5"
A7 = "100.0,200.0,3,1,0,0,60"
B7 = "100.5,200.5,3,1,0,0,60"
WP = "~5.000:1.0:2.0"

check("9 fields, 0.7 m apart: equal", eq(A9, B9))
check("9 fields with waypoints, 0.7 m apart: equal", eq(A9 + WP, B9 + WP))
check("7 fields with waypoints, 0.7 m apart: equal", eq(A7 + WP, B7 + WP))
check("with cargo and waypoints: equal", eq(A9 + "^1" + WP, B9 + "^1" + WP))
check("a different cargo filter: not equal", not eq(A9 + "^1" + WP, B9 + "^2" + WP))
check("the last field (max wait) still counts with waypoints", not eq(A7 + WP, A7[:-2] + "90" + WP))
check("10 m apart: not equal", not eq(A9, "110.0,200.0,3,1,0,0,60,115.0,205.0"))
check("a different stop count: not equal", not eq(A9, A9 + ";" + A9))

print()
if fails:
    print(f"{len(fails)} FAILED")
    sys.exit(1)
print("all stopsSigEqual checks passed")
