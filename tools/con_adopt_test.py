"""Offline checks for a construction that reuses a known entity id (cons.lua), on Lua 5.2.

A station our replay builds can take the id of a town building it just demolished. That id
is already in knownCons, so pollNewConstructions never adopts the station and every module
edit on it logged "CONU: no ... within 10 m" and was lost on every instance (station 45198,
2026-09-11). This loads the real mod/.../scripts/mp/cons.lua into a lupa.lua52 runtime with a
stub world and drives:
  - the bug: prime the poll with a town building, replace it by a station with the SAME id,
    poll again -> the station is not in consByKey
  - CM.forgetKnownCon: the builder forgets the id, the next poll adopts it ("landed as id")
  - CM.execConU fallback: with the table still missing it, the edit adopts the station from
    the world and upgrades it
  - a dead id left in the table is replaced by the live station at that spot

    python tools/con_adopt_test.py
"""
import os
import sys

import lupa.lua52 as lupa

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
CONS = os.path.join(REPO, "mod", "mp_lockstep_1", "res", "scripts", "mp", "cons.lua")
FILE = "station/rail/modular_station/modular_station.con"

fails = []


def check(name, cond, extra=""):
    print(("ok   " if cond else "FAIL ") + name + (f"  ({extra})" if extra else ""))
    if not cond:
        fails.append(name)


def runtime():
    L = lupa.LuaRuntime(unpack_returned_tuples=True)
    L.globals().CONS_SRC = open(CONS, encoding="utf-8").read()
    L.globals().FILE = FILE
    return L.execute(r'''
local logs, upgrades = {}, {}
local world = {}          -- id -> { file=, x=, y=, player=, params= }
local ME = 7
local function sink()
  return setmetatable({}, { __index = function() return sink() end, __call = function() return nil end })
end
local CT = { CONSTRUCTION = 1, PLAYER_OWNED = 2, NAME = 3 }
api = setmetatable({
  type = { ComponentType = CT },
  engine = {
    util = { getPlayer = function() return ME end },
    entityExists = function(id) return world[id] ~= nil end,
    getComponent = function(id, t)
      local w = world[id]
      if not w then return nil end
      if t == CT.CONSTRUCTION then
        local tr = {}; for i = 1, 16 do tr[i] = 0 end; tr[13], tr[14] = w.x, w.y
        return { fileName = w.file, transf = tr }
      elseif t == CT.PLAYER_OWNED then
        return w.player and { player = w.player } or nil
      end
    end,
  },
}, { __index = function() return sink() end })
game = { interface = setmetatable({
  getEntities = function(filter, opts)
    local out = {}
    for id, w in pairs(world) do
      if not filter.pos then out[#out + 1] = id
      else
        local dx, dy = w.x - filter.pos[1], w.y - filter.pos[2]
        if dx * dx + dy * dy <= filter.radius * filter.radius then out[#out + 1] = id end
      end
    end
    return out
  end,
  getEntity = function(id) local w = world[id]; return w and { params = w.params or {} } end,
  upgradeConstruction = function(id, file, params) upgrades[#upgrades + 1] = id; return id end,
}, { __index = function() return sink() end }) }
local K = setmetatable({ INSTANCE = "a" }, { __index = function() return nil end })
local CM = setmetatable({ expectedCons = {}, cmExpectedCompany = {}, cmExpectedBal0 = {}, expectedDemolish = {} },
  { __index = function(t, k) return nil end })
function CM.gameTime() return 100 end
function CM.scheduleLocal() end
function CM.cmLog() end
function CM.cmBalance() return nil end
local log = function(s) logs[#logs + 1] = s end
assert(load(CONS_SRC, "@cons.lua"))()(CM, K, log)
local H = { CM = CM }
function H.put(id, x, y, player) world[id] = { file = (player and FILE or "building/town.con"), x = x, y = y, player = player, params = {} } end
function H.drop(id) world[id] = nil end
function H.poll() CM.pollNewConstructions() end
function H.keyOf(x, y) return CM.conKey(x, y) end
function H.recId(x, y) local r = CM.consByKey[CM.conKey(x, y)]; return r and r.id end
function H.setRec(x, y, id) CM.consByKey[CM.conKey(x, y)] = { id = id, file = FILE, params = "{}" } end
function H.expect(x, y) CM.expectedCons[CM.conKey(x, y)] = true end
function H.conu(x, y) CM.execConU({ origin = "b", seq = 1, file = FILE, x = x, y = y, params = "{}", diff = 0 }) end
function H.nup() return #upgrades end
function H.up(i) return upgrades[i] end
function H.clear() logs = {}; upgrades = {} end
function H.logs() return table.concat(logs, "\n") end
return H
''')


def main():
    X, Y = 3116.3, 3629.3

    # the bug, reproduced
    H = runtime()
    H.put(45198, X + 3, Y - 2, None)       # a town building
    H.poll()                               # first poll primes knownCons
    H.drop(45198)
    H.put(45198, X, Y, 7)                  # our replay's station takes the same id
    H.expect(X, Y)
    H.poll()
    check("bug: a station on a reused id is never adopted by the poll", H.recId(X, Y) is None, str(H.recId(X, Y)))

    # the builder forgets the stale id, the poll adopts it through the replay branch
    was = H.CM.forgetKnownCon(45198)
    check("forgetKnownCon: says the id was stale", was is True)
    H.poll()
    check("forgetKnownCon: the next poll adopts the station", H.recId(X, Y) == 45198, str(H.recId(X, Y)))
    check("forgetKnownCon: through the replay branch", "landed as id 45198" in H.logs())
    check("forgetKnownCon: a fresh id reports not stale", H.CM.forgetKnownCon(999999) is False)

    # the edit replay's own fallback, with the table still missing the station
    H = runtime()
    H.put(45198, X + 3, Y - 2, None)
    H.poll()
    H.drop(45198)
    H.put(45198, X, Y, 7)
    H.poll()
    H.clear()
    H.conu(X, Y)
    check("CONU fallback: the edit upgrades the station anyway", H.nup() == 1 and H.up(1) == 45198, H.logs()[-300:])
    check("CONU fallback: adopted and logged", "con: adopted" in H.logs() and H.recId(X, Y) == 45198)
    check("CONU fallback: no 'ignoring'", "ignoring" not in H.logs())

    # a dead id in the table: the upgrade replaced the entity with a reused id
    H.drop(45198)
    H.put(22943, X, Y, 7)
    H.clear()
    H.conu(X, Y)
    check("dead id: the edit lands on the live station", H.nup() == 1 and H.up(1) == 22943, H.logs()[-300:])
    check("dead id: the table now names it", H.recId(X, Y) == 22943)

    # nothing there at all: still refuses, and does not upgrade a town building
    H.drop(22943)
    H.put(5000, X, Y, None)
    H.clear()
    H.conu(X, Y)
    check("empty spot: nothing upgraded", H.nup() == 0)
    check("empty spot: says so", "ignoring" in H.logs())

    print()
    if fails:
        print(f"{len(fails)} FAILED")
        sys.exit(1)
    print("all passed")


if __name__ == "__main__":
    main()
