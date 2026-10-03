"""Offline checks for the per-job PERF lanes and the train/depot watch switch, on Lua 5.2.

Since 2026-09-23 the update's cost is logged per job ("PERF lanes: ..."), and the two
log-only watchers (watchDepartures, watchTrains) run only with watch_trains=1 in
tpf2_slice.cfg: they read every train's path each update and every vehicle every 300
ticks, and nothing they read feeds a command, the queue or the hash.

This runs the REAL CM.perfLane / CM.perfLanesText cut out of lockstep.lua and the REAL
CM.vehWatchOn cut out of vehicles.lua, and checks:
  - lanes accumulate sum and max per name, and the text lists the dearest first
  - the watchers are called only behind CM.vehWatchOn() in update()
  - the switch is off by default, on with watch_trains=1, and switching off drops
    their state so switching on again starts clean

    python tools/perf_lanes_test.py
"""
import os
import re
import sys

import lupa.lua52 as lupa

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
LOCKSTEP = os.path.join(REPO, "mod", "mp_lockstep_1", "res", "config", "game_script", "lockstep.lua")
VEHICLES = os.path.join(REPO, "mod", "mp_lockstep_1", "res", "scripts", "mp", "vehicles.lua")

fails = []


def check(name, cond, extra=""):
    print(("ok   " if cond else "FAIL ") + name + (f"  ({extra})" if extra else ""))
    if not cond:
        fails.append(name)


def cut(src, name):
    m = re.search(r"^function CM\." + name + r"\(.*?^end\r?$", src, re.S | re.M)
    if not m:
        raise SystemExit(f"FAIL cannot find CM.{name}")
    return m.group(0)


with open(LOCKSTEP, encoding="utf-8") as f:
    ls = f.read()
with open(VEHICLES, encoding="utf-8") as f:
    veh = f.read()

lua = lupa.LuaRuntime(unpack_returned_tuples=True)
lua.execute("CM = {}; K = {}; fakeClock = 0; os.clock = function() return fakeClock end")
lua.execute(cut(ls, "perfLane"))
lua.execute(cut(ls, "perfLanesText"))

# 1. lanes
lua.execute("""
  local t = 0
  fakeClock = 0.000; t = CM.perfLane("a", 0)          -- a: 0 ms
  fakeClock = 0.004; t = CM.perfLane("b", t)          -- b: 4 ms
  fakeClock = 0.005; t = CM.perfLane("a", t)          -- a: 1 ms
  fakeClock = 0.015; t = CM.perfLane("c", t)          -- c: 10 ms
  fakeClock = 0.017; t = CM.perfLane("b", t)          -- b: 2 ms
""")
b = lua.eval("CM.perfLanes.b")
check("a lane sums its laps and keeps the worst", abs(b.sum - 6) < 1e-9 and abs(b.max - 4) < 1e-9, f"sum={b.sum} max={b.max}")
text = lua.eval("CM.perfLanesText(2)")
check("the lanes text lists the dearest lane first, avg per update / max", text == "c 5.00/10, b 3.00/4, a 0.50/1", text)
check("an empty window gives an empty text", lua.eval("(function() CM.perfLanes = nil; return CM.perfLanesText(0) end)()") == "")

# 2. the watchers are gated in update()
calls = [m.start() for m in re.finditer(r"CM\.watch(Departures|Trains)\(\)", ls)]
gate = ls.find("if CM.vehWatchOn() then")
check("update() calls both watchers exactly once", len(calls) == 2, str(len(calls)))
check("both watcher calls sit inside the CM.vehWatchOn() block",
      gate != -1 and all(gate < c < ls.find("end", gate) for c in calls))
check("the PERF log prints the lanes and the window resets them",
      'log("PERF lanes: " .. CM.perfLanesText(u.n))' in ls and "CM.perfLanes = nil" in ls)

# 3. the switch
lua.execute("cfg = {}; CM.cfgFlag = function(k, d) local v = cfg[k]; if v == nil then return d end; return v end")
lua.execute(cut(veh, "vehWatchOn"))
check("off by default (no watch_trains in the cfg)", lua.eval("CM.vehWatchOn()") is False)
lua.execute("cfg.watch_trains = true")
check("on with watch_trains=1", lua.eval("CM.vehWatchOn()") is True)
lua.execute("CM.depotSince = { [7] = 3 }; CM.trainWatch = { list = { 1 }, last = { [1] = {} }, at = 5, off = true }")
lua.execute("cfg.watch_trains = false")
check("switching off reports off", lua.eval("CM.vehWatchOn()") is False)
check("switching off drops the departure state", lua.eval("CM.depotSince") is None)
check("switching off resets the train watch (empty, due at once, not off)",
      lua.eval("#CM.trainWatch.list == 0 and next(CM.trainWatch.last) == nil and CM.trainWatch.at < -1e8 and CM.trainWatch.off == false"))

if fails:
    print(f"\n{len(fails)} check(s) FAILED")
    sys.exit(1)
print("\nall checks passed")
