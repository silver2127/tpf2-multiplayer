"""The host-capacity cap (pacing.lua CM.hostCapacityCap, 2026-09-18).

The engine stretches its 200 ms batch interval when a batch of `lever`
iterations costs more; the lever still reads 4 while the world runs at 2. The
bridge publishes the interval (tpf2_engine_pace.txt) and the leader caps the
session speed at what the host sustains: three stretched readings in a row (one
is a hash stamp), a whole number, one step up after an unstretched while. Real
Lua 5.2 over the sliced functions, a temp data dir, no engine.

    python tools/host_capacity_test.py
"""
import os
import re
import tempfile

import lupa.lua52 as lupa

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
PACING = open(os.path.join(REPO, "mod", "mp_lockstep_1", "res", "scripts", "mp", "pacing.lua"), encoding="utf-8").read()

fails = []


def check(name, cond, extra=""):
    print(("ok   " if cond else "FAIL ") + name + (f"  ({extra})" if extra else ""))
    if not cond:
        fails.append(name)


check("the leader caps eff by the host's capacity before the governor",
      "local capped, capAt = CM.hostCapacityCap(eff)" in PACING
      and PACING.index("local capped, capAt = CM.hostCapacityCap(eff)") < PACING.index("local governed = CM.governSpeed(now, eff)"))

m1 = re.search(r"^function CM\.hostPace\(\)\n.*?\n^end\n", PACING, re.S | re.M)
m2 = re.search(r"^function CM\.hostCapacityCap\(eff\)\n.*?\n^end\n", PACING, re.S | re.M)
consts = "\n".join(l for l in PACING.splitlines() if l.startswith("K.CAP_"))
check("pacing.lua defines CM.hostPace and CM.hostCapacityCap", bool(m1 and m2))

with tempfile.TemporaryDirectory() as td:
    L = lupa.LuaRuntime(unpack_returned_tuples=True)
    L.globals().SRC = consts + "\n" + (m1.group(0) if m1 else "") + (m2.group(0) if m2 else "")
    L.globals().BASE = td.replace("\\", "/") + "/"
    T = L.execute(r'''
local T = { log = {} }
local CM, K = { ticks = 0 }, { BASE = BASE }
local function log(s) T.log[#T.log + 1] = s end
assert(load("local CM, K, log = ...\n" .. SRC, "@cap"))(CM, K, log)
T.CM, T.K = CM, K
function T.run(n, eff)   -- n ticks; returns the last (eff, capAt)
  local e, at
  for _ = 1, n do CM.ticks = CM.ticks + 1; e, at = CM.hostCapacityCap(eff) end
  return e, at
end
return T
''')
    pace = os.path.join(td, "tpf2_engine_pace.txt")

    def write(base, lever):
        open(pace, "w").write(f"base={base} lever={lever}\n")

    e, at = T.run(30, 4)
    check("no pace file: eff stands", e == 4 and at is None)
    write(400000, 4)
    e, at = T.run(25, 4)
    check("one stretched reading (a hash stamp) does nothing", e == 4 and at is None)
    e, at = T.run(50, 4)
    check("three stretched readings in a row: capped at what the host sustains (4 x 200/400 = 2)", e == 2 and at == 2, f"{e} {at}")
    check("  ... logged once", sum("keeps up with 2x" in str(T.log[i + 1]) for i in range(len(T.log))) == 1)
    write(200000, 2)   # the lever followed the cap; the engine keeps up now
    e, at = T.run(100, 4)
    check("unstretched at the cap for a short while: the cap holds", e == 2 and at == 2)
    e, at = T.run(T.K.CAP_UP_TICKS + 30, 4)
    check("after an unstretched while it tries one step up", e == 3 and at == 3, f"{e} {at}")
    write(300000, 3)   # 3 iterations cost 300 ms: back down
    e, at = T.run(80, 4)
    check("stretched again at 3: back to 2", e == 2 and at == 2, f"{e} {at}")
    write(200000, 2)
    e, at = T.run(T.K.CAP_UP_TICKS + 30, 3)
    check("with the votes at 3 the step up reaches them and the cap clears", e == 3 and at is None and T.CM.hostCap is None, f"{e} {at}")
    # a lever below the votes that keeps up is never capped
    write(200000, 4)
    e, at = T.run(100, 4)
    check("keeping up at the votes: no cap", e == 4 and at is None)
    # the governor's off switch turns the cap off too
    write(500000, 4)
    T.run(80, 4)
    T.CM.governorOff = L.eval("function() return true end")
    e, at = T.run(1, 4)
    check("tpf2mp_governor_off.txt: no cap", e == 4 and at is None and T.CM.hostCap is None)
    T.CM.governorOff = None
    # a pause or no eff: untouched
    e, at = T.run(1, 0)
    check("eff 0 (paused): untouched", e == 0 and at is None)

if fails:
    print("FAIL:", len(fails), "check(s):", "; ".join(fails))
    raise SystemExit(1)
print("PASS: the session speed is capped at what the host's machine sustains, whole numbers, one step up after an unstretched while")
