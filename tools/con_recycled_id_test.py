"""Offline checks (Lua 5.2, real cons.lua): a recycled entity id is not our construction.

Live, 2026-09-19: a Snowball Fences segment was bulldozed (strict DEMOLISH on every
instance). Its record stayed in consByKey because the NEXT segment, 4 m away, passed the
removal poll's "something is still there" check (6 m). The engine then handed the dead id
to another entity; entityExists said yes, getEntity().params read {} and the edit scan
shipped a CONU with empty params -- upgradeConstruction on that id was a fatal
GetComponentDataIndex assert on every instance. This drives the real cons.lua and checks:
  - the edit scan ignores a record whose id no longer carries the construction
  - the removal poll treats such a record as gone (a neighbour 4 m away does not keep it)
    and clears it quietly when the demolish was our own replay
  - execConU refuses to upgrade a recycled id, and still adopts a live replacement ON the spot

    python tools/con_recycled_id_test.py
"""
import os
import sys

import lupa.lua52 as lupa

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
CONS = os.path.join(REPO, "mod", "mp_lockstep_1", "res", "scripts", "mp", "cons.lua")
FILE = "asset/snowball_fences_fence_on.con"

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
local logs, upgrades, sched = {}, {}, {}
local world = {}          -- id -> { file=, x=, y=, player=, params=, con= }
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
        if not w.con then return nil end        -- a recycled id: some other entity kind
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
      if w.con then
        if not filter.pos then out[#out + 1] = id
        else
          local dx, dy = w.x - filter.pos[1], w.y - filter.pos[2]
          if dx * dx + dy * dy <= filter.radius * filter.radius then out[#out + 1] = id end
        end
      end
    end
    return out
  end,
  getEntity = function(id) local w = world[id]; return w and { params = w.params or {} } end,
  upgradeConstruction = function(id, file, params)
    assert(world[id] and world[id].con, "upgradeConstruction on a non-construction id (fatal assert in the game)")
    upgrades[#upgrades + 1] = id; return id
  end,
}, { __index = function() return sink() end }) }
local K = setmetatable({ INSTANCE = "a" }, { __index = function() return nil end })
local CM = setmetatable({ expectedCons = {}, cmExpectedCompany = {}, cmExpectedBal0 = {}, expectedDemolish = {} },
  { __index = function(t, k) return nil end })
function CM.gameTime() return 100 end
function CM.scheduleLocal(op, args) sched[#sched + 1] = { op = op, args = args } end
function CM.cmLog() end
function CM.cmBalance() return nil end
function CM.rearmSplitsNear() end
local log = function(s) logs[#logs + 1] = s end
assert(load(CONS_SRC, "@cons.lua"))()(CM, K, log)
local H = { CM = CM }
-- a player construction of FILE (a fence segment)
function H.fence(id, x, y) world[id] = { file = FILE, x = x, y = y, player = ME, params = { result = { models = {} } }, con = true } end
-- the same id, recycled by an entity that is not a construction
function H.recycle(id, x, y) world[id] = { x = x, y = y, con = false } end
function H.drop(id) world[id] = nil end
-- pstr nil: the record matches the entity's own params (a construction nobody edited)
function H.record(id, x, y, pstr) CM.consByKey[CM.conKey(x, y)] = { id = id, file = FILE, params = pstr or CM.ser(world[id].params) } end
function H.rec(x, y) return CM.consByKey[CM.conKey(x, y)] end
function H.expectDemolish(x, y) CM.expectedDemolish[CM.conKey(x, y)] = true end
function H.scan() CM.scanConstructionEdits() end
function H.removals() CM.pollConstructionRemovals() end
function H.conu(x, y) CM.execConU({ origin = "b", seq = 1, file = FILE, x = x, y = y, params = "{}", diff = 0 }) end
function H.ops() local t = {} for i, s in ipairs(sched) do t[i] = s.op end return table.concat(t, ",") end
function H.nup() return #upgrades end
function H.up(i) return upgrades[i] end
function H.clear() logs = {}; upgrades = {}; sched = {} end
function H.logs() return table.concat(logs, "\n") end
return H
''')


def main():
    X, Y = 1998.6, 5462.0
    PSTR = "{result={models={{id=\"a.mdl\"}}},seed=0}"

    # the live case: the segment's id was recycled, the next segment is 4 m away
    H = runtime()
    H.fence(117955, X, Y)
    H.record(117955, X, Y, PSTR)          # edited/registered by the FENCE replay
    H.fence(117956, X + 4, Y)
    H.record(117956, X + 4, Y)
    H.expectDemolish(X, Y)                 # our own strict replay bulldozed it
    H.recycle(117955, X, Y)                # ... and the engine reused the id
    H.clear()
    H.scan()
    check("edit scan: a recycled id ships no CONU", H.ops() == "", H.ops())
    check("edit scan: the record keeps its params", H.rec(X, Y).params == PSTR)
    H.removals(); H.removals()
    check("removal poll: the neighbour 4 m away does not keep the record alive", H.rec(X, Y) is None, H.logs()[-200:])
    check("removal poll: our own replay's demolish is not echoed", H.ops() == "" and "not echoed" in H.logs(), H.ops())
    check("removal poll: the neighbour's record stays", H.rec(X + 4, Y) is not None)

    # a native bulldoze the slice let run: the same recycled id still ships the demolish
    H = runtime()
    H.fence(500, X, Y); H.record(500, X, Y, PSTR)
    H.recycle(500, X, Y)
    H.clear()
    H.removals(); H.removals()
    check("removal poll: recycled id -> DEMOLISH captured", H.ops() == "DEMOLISH", H.ops())

    # an upgrade replacement ON the spot still counts as "still there"
    H = runtime()
    H.fence(600, X, Y); H.record(600, X, Y, PSTR)
    H.drop(600)
    H.fence(601, X + 0.05, Y)              # the replacement, within the key's rounding
    H.clear()
    H.removals(); H.removals()
    check("removal poll: a replacement on the spot is not a demolish", H.ops() == "" and H.rec(X, Y) is not None, H.ops())

    # execConU: a recycled id is refused, never upgraded (the fatal assert)
    H = runtime()
    H.fence(700, X, Y); H.record(700, X, Y, PSTR)
    H.recycle(700, X, Y)
    H.clear()
    H.conu(X, Y)
    check("CONU: nothing upgraded on a recycled id", H.nup() == 0, H.logs()[-200:])
    check("CONU: says it ignored the edit", "ignoring" in H.logs())
    # ... but a live construction on the spot is adopted and upgraded as before
    H.fence(701, X, Y)
    H.clear()
    H.conu(X, Y)
    check("CONU: a live replacement on the spot is adopted and upgraded", H.nup() == 1 and H.up(1) == 701, H.logs()[-300:])

    print()
    if fails:
        print(f"{len(fails)} FAILED")
        sys.exit(1)
    print("all passed")


if __name__ == "__main__":
    main()
