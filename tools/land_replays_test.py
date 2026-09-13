"""Offline checks that replayed constructions land by lookup (cons.lua CM.landReplays), on Lua 5.2.

The mod used to find every construction -- ours, a peer's replay, an upgrade's
replacement -- by scanning the whole world every 10 sim steps. On a big map that scan
froze the simulation ~300 ms each time (2026-09-12), so it now runs only at load and as
a one-shot catch-up. A replay knows where it builds, so the landing is a lookup at that
position instead. This loads the real cons.lua and checks:
  - the load-time prime marks what the save holds as known
  - a replayed construction lands: registered in consByKey, its flag cleared
  - companies mode: the landed replay is handed to the origin company, cost moved
  - an edit's replacement lands, but not the old entity still standing on the spot
  - a construction a few metres off the expected spot is not taken for it
  - a replay that never lands is let go after K.LAND_TIMEOUT_TICKS
  - with nothing expected, the lookup does not touch the world at all

    python tools/land_replays_test.py
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


def runtime():
    L = lupa.LuaRuntime(unpack_returned_tuples=True)
    L.globals().SRC = open(CONS, encoding="utf-8").read()
    return L.execute(r'''
local logs, lookups, handed, moved = {}, 0, {}, {}
local world = {}   -- id -> { file, x, y, owned, params }
local function sink() return setmetatable({}, { __index = function() return sink() end, __call = function() return sink() end }) end
api = setmetatable({
  type = { ComponentType = { CONSTRUCTION = 1, PLAYER_OWNED = 2 } },
  engine = setmetatable({
    entityExists = function(id) return world[id] ~= nil end,
    getComponent = function(id, t)
      local w = world[id]
      if not w then return nil end
      if t == 1 then
        local tr = {}
        for i = 1, 16 do tr[i] = 0 end
        tr[13], tr[14] = w.x, w.y
        return { fileName = w.file, transf = tr }
      end
      if t == 2 then return w.owned and {} or nil end
      return nil
    end,
    util = { getPlayer = function() return 7 end },
  }, { __index = function() return sink() end }),
}, { __index = function() return sink() end })
game = { interface = {
  getEntities = function(filter, opts)
    lookups = lookups + 1
    local out, p, r = {}, filter.pos, filter.radius or 1e9
    for id, w in pairs(world) do
      if not p or (w.x - p[1]) ^ 2 + (w.y - p[2]) ^ 2 <= r * r then out[#out + 1] = id end
    end
    return out
  end,
  getEntity = function(id) local w = world[id]; return w and { params = w.params or {}, balance = 1000 } or nil end,
} }
local K = setmetatable({ INSTANCE = "a" }, { __index = function() return nil end })
local CM = setmetatable({ ticks = 0 }, { __index = function() return function() return sink() end end })
local log = function(s) logs[#logs + 1] = s end
assert(load(SRC, "@cons.lua"))()(CM, K, log)
CM.ser = function(t) return "{}" end
CM.isPlayerConstruction = function(id, file) local w = world[id]; return w ~= nil and w.owned end
CM.cmExpectedCompany, CM.cmExpectedBal0, CM.cmCompanyPid = {}, {}, { [1] = 7 }
CM.cmMyCompany = 1
CM.cmReassignConstruction = function(id, cid) handed[#handed + 1] = id .. ">" .. cid end
CM.cmBalance = function(pid) return 900 end
CM.cmTransferCost = function(cid, amount, what) moved[#moved + 1] = cid .. ":" .. amount end
local H = {}
function H.put(id, file, x, y, owned) world[id] = { file = file, x = x, y = y, owned = owned } end
function H.remove(id) world[id] = nil end
function H.prime() CM.pollNewConstructions() end
function H.expect(x, y) CM.expectedCons[CM.conKey(x, y)] = true end
function H.expectCompany(x, y, cid, bal0) local k = CM.conKey(x, y); CM.cmExpectedCompany[k] = cid; CM.cmExpectedBal0[k] = bal0 end
function H.tick(n) for _ = 1, n or 1 do CM.ticks = CM.ticks + 1; CM.landReplays() end end
function H.expected(x, y) return CM.expectedCons[CM.conKey(x, y)] == true end
function H.rec(x, y) local r = CM.consByKey[CM.conKey(x, y)]; return r and r.id or nil end
function H.lookups() return lookups end
function H.handed() return table.concat(handed, ",") end
function H.moved() return table.concat(moved, ",") end
function H.logs() return table.concat(logs, "\n") end
function H.timeout() return K.LAND_TIMEOUT_TICKS end
return H
''')


def main():
    R = runtime()
    R.put(10, "station/rail/modular_station.con", 100.0, 200.0, True)   # in the save
    R.put(11, "town/house.con", 300.0, 300.0, False)
    R.prime()
    n0 = R.lookups()
    R.tick(9)
    check("nothing expected: the lookup never touches the world", R.lookups() == n0, f"{R.lookups() - n0} lookups")

    # 1. a replayed depot lands at its spot
    R.expect(500.0, 500.0)
    R.tick(3)
    check("a replay not built yet keeps its flag", R.expected(500.0, 500.0))
    R.put(20, "depot/road_depot.con", 500.0, 500.0, True)
    R.tick(3)
    check("the replay lands: flag cleared", not R.expected(500.0, 500.0))
    check("...and registered at its position", R.rec(500.0, 500.0) == 20, str(R.rec(500.0, 500.0)))

    # 2. companies mode: handed to the origin company, cost moved
    R.expect(600.0, 600.0)
    R.expectCompany(600.0, 600.0, 3, 1000)
    R.put(30, "depot/road_depot.con", 600.0, 600.0, True)
    R.tick(3)
    check("companies: handed to company 3", R.handed() == "30>3", R.handed())
    check("companies: the build cost moved to it", R.moved() == "3:100", R.moved())

    # 3. an owned construction a few metres away is not the replay
    R.expect(700.0, 700.0)
    R.put(40, "depot/road_depot.con", 703.0, 700.0, True)
    R.tick(3)
    check("a construction 3 m off the spot is not taken for it", R.expected(700.0, 700.0) and R.rec(700.0, 700.0) is None)

    # 4. not owned yet (ownership lands a tick later): waits, then lands
    R.put(41, "depot/road_depot.con", 700.0, 700.0, False)
    R.tick(3)
    check("an unowned construction on the spot is not landed yet", R.expected(700.0, 700.0))
    R.put(41, "depot/road_depot.con", 700.0, 700.0, True)
    R.tick(3)
    check("...and lands once it is owned", not R.expected(700.0, 700.0) and R.rec(700.0, 700.0) == 41)

    # 5. an edit replay: the old entity (known) stands until its replacement appears
    R.expect(100.0, 200.0)        # the CONU path expects the replaced entity at the station's spot
    R.tick(3)
    check("the edit's old entity is not taken for its replacement", R.expected(100.0, 200.0))
    R.remove(10)
    R.put(50, "station/rail/modular_station.con", 100.0, 200.0, True)
    R.tick(3)
    check("the replacement lands under the same key", not R.expected(100.0, 200.0) and R.rec(100.0, 200.0) == 50)

    # 6. a replay that never lands is let go
    R.expect(900.0, 900.0)
    R.tick(R.timeout() + 6)
    check("a replay that never lands drops its flag after the timeout", not R.expected(900.0, 900.0))
    check("...and says so", "never landed" in R.logs())

    print("all passed" if not fails else f"{len(fails)} FAILED")
    sys.exit(1 if fails else 0)


if __name__ == "__main__":
    main()
