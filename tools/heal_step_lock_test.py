"""Offline checks that orphaned-split heals run on a fixed sim step (cons.lua CM.watchSplit), on Lua 5.2.

A replayed construction that splits a road leaves a scar when it is demolished, and the
scar is healed (the two halves merged back into one road). That heal used to be decided by
a frame-tick sweep (CM.ticks), so every game healed on its own step: the rebuilt road
rerouted passengers at different moments, people counts split and buses drifted into a
vehicle desync on all three games (2026-09-12, t=3024 -> t=3156).

Each watched split is now a HEALCHK in the step-locked queue, due a fixed number of steps
after the stamp of the command that caused it. This loads the real cons.lua twice, with
very different frame-tick counts, and checks:
  - the same command queues an identical check (key and due step) on both
  - arming the same site twice from one command queues it once
  - a split without a command stamp is not watched
  - the check heals a scar (2 edges), keeps an in-use split (3 edges) as a site
  - a demolish near a kept site queues a new check from the demolish's stamp

    python tools/heal_step_lock_test.py
"""
import os
import sys

import lupa.lua52 as lupa

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
CONS = os.path.join(REPO, "mod", "mp_lockstep_1", "res", "scripts", "mp", "cons.lua")

fails = []


def check(name, cond, extra=""):
    print(("ok   " if cond else "FAIL ") + name + (f"  ({extra})" if extra else ""))
    if not cond:
        fails.append(name)


def runtime(ticks):
    L = lupa.LuaRuntime(unpack_returned_tuples=True)
    L.globals().SRC = open(CONS, encoding="utf-8").read()
    L.globals().TICKS = ticks
    return L.execute(r'''
local logs, heals = {}, {}
local function sink() return setmetatable({}, { __index = function() return sink() end, __call = function() return sink() end }) end
local edgesAt = {}   -- node id -> number of street edges
local nodeAt = {}    -- "x/y" -> node id
api = setmetatable({
  engine = setmetatable({
    system = { streetSystem = { getNode2StreetEdgeMap = function()
      local m = {}
      for nid, n in pairs(edgesAt) do local t = {}; for i = 1, n do t[i] = nid * 10 + i end; m[nid] = t end
      return m
    end } },
  }, { __index = function() return sink() end }),
}, { __index = function() return sink() end })
game = setmetatable({}, { __index = function() return sink() end })
local K = setmetatable({ INSTANCE = "a", SIM_STEP = 0.2 }, { __index = function() return nil end })
local CM = setmetatable({ ticks = TICKS }, { __index = function() return function() return sink() end end })
local log = function(s) logs[#logs + 1] = s end
assert(load(SRC, "@cons.lua"))()(CM, K, log)
CM.retryQueue, CM.splitSites = {}, {}
function CM.stepOf(t) return math.floor((t or 0) / 0.2 + 0.5) end
local now = 100
function CM.gameTime() return now end
function CM.findNodeNear(isTrack, x, y) return nodeAt[string.format("%.1f/%.1f", x, y)] end
function CM.healNodeAt(x, y, why) heals[#heals + 1] = string.format("%.1f,%.1f", x, y); return true end
local H = {}
function H.setNode(x, y, nid, nedges) nodeAt[string.format("%.1f/%.1f", x, y)] = nid; edgesAt[nid] = nedges end
function H.watch(x, y, at, origin, seq) CM.watchSplit(x, y, at and { at = at, origin = origin, seq = seq } or nil) end
function H.rearm(x, y, at, origin, seq) CM.rearmSplitsNear(x, y, { at = at, origin = origin, seq = seq }) end
function H.nq() return #CM.retryQueue end
function H.q(i, k) local e = CM.retryQueue[i]; return e and e[k] end
function H.key(i) local e = CM.retryQueue[i]; return e and string.format("%.4f|%s|%.6f|%d", e.at, e.origin, e.seq, e.notBeforeStep) end
function H.run(i) CM.execHealCheck(CM.retryQueue[i]) end
function H.clearQ() CM.retryQueue = {} end
function H.nheals() return #heals end
function H.nsites() local n = 0; for _ in pairs(CM.splitSites or {}) do n = n + 1 end; return n end
function H.logs() return table.concat(logs, "\n") end
return H
''')


def main():
    A = runtime(120)       # a game that has run few frames
    B = runtime(98765)     # one that has run many more

    # 1. one CONX (at 3000.4, origin b, seq 35) cuts a road at -3189.7,10143.1
    for R in (A, B):
        R.watch(-3189.7, 10143.1, 3000.4, "b", 35)
    check("both games queue one check", A.nq() == 1 and B.nq() == 1)
    check("identical key and due step, whatever the frame count", A.key(1) == B.key(1), f"{A.key(1)} vs {B.key(1)}")
    check("due 25 steps after the CONX stamp (step 15002 + 25)", A.q(1, "notBeforeStep") == 15027, str(A.q(1, "notBeforeStep")))
    check("a HEALCHK op", A.q(1, "op") == "HEALCHK")

    # 2. the capture and the exec of the same CONX arm the same site: queued once
    A.watch(-3189.7, 10143.1, 3000.4, "b", 35)
    check("arming the same site twice from one command queues it once", A.nq() == 1, str(A.nq()))

    # 3. no stamp, no watch
    A.watch(10.0, 20.0, None, None, None)
    check("a split with no command stamp is not watched", A.nq() == 1 and "no command stamp" in A.logs())

    # 4. the check: a scar (2 edges) heals; an in-use split (3 edges) is kept as a site
    A.setNode(-3189.7, 10143.1, 110682, 2)
    A.run(1)
    check("a scar with 2 edges is healed", A.nheals() == 1)
    B.setNode(-3189.7, 10143.1, 110682, 3)
    B.run(1)
    check("an in-use split (3 edges) is not healed", B.nheals() == 0)
    check("an in-use split is kept as a site", B.nsites() == 1)

    # 5. the construction using it is demolished (DEMOLISH at 3005.6, origin b, seq 36)
    B.clearQ()
    B.rearm(-3170.0, 10150.0, 3005.6, "b", 36)
    check("a demolish within 60 m queues a new check", B.nq() == 1)
    check("due 25 steps after the DEMOLISH stamp (step 15028 + 25)", B.q(1, "notBeforeStep") == 15053, str(B.q(1, "notBeforeStep")))
    B.setNode(-3189.7, 10143.1, 110682, 2)
    B.run(1)
    check("the scar the demolish left is healed", B.nheals() == 1)

    print()
    if fails:
        print(f"{len(fails)} FAILED")
        sys.exit(1)
    print("all passed")


if __name__ == "__main__":
    main()
