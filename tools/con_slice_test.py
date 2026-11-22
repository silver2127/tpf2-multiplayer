"""Offline checks (Lua 5.2, real cons.lua): the construction checks run in slices, not all at once.

Live, 2026-09-24 (PERF lanes, a 409-construction map): the edit scan read and serialized every
tracked construction in ONE update every 30 ticks -- a ~525 ms freeze of the simulation every
~5.5 s -- and the removal poll walked them all every 3rd tick (~21 ms). Together 23 of the
update's 24 ms. update() now calls CM.scanConstructionEditsSlice and
CM.pollConstructionRemovalsSlice every tick; each works through ceil(n / every) constructions
per tick. This drives the real cons.lua and checks that nothing is lost:
  - every construction is checked exactly once per cycle, and no tick does more than its share
  - over one cycle the sliced edit scan ships exactly what the full scan ships
  - demolishes, replacements on the spot, recycled ids and our own replays' demolishes behave
    as in the full removal poll, within two cycles (the same two-miss rule)
  - a construction added mid-cycle is checked in the next cycle; one removed is skipped
  - one construction whose read throws does not stop the others

    python tools/con_slice_test.py
"""
import math
import os
import random
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
local logs, sched = {}, {}
local world = {}          -- id -> { file=, x=, y=, player=, params=, con=, boom= }
local reads = {}          -- id -> getEntity calls
local tickReads = 0
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
        if not w.con then return nil end
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
  getEntity = function(id)
    reads[id] = (reads[id] or 0) + 1
    tickReads = tickReads + 1
    local w = world[id]
    if w and w.boom then error("unreadable entity " .. id) end
    return w and { params = w.params or {} }
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
local H = { CM = CM, K = K }
function H.con(id, x, y, n) world[id] = { file = FILE, x = x, y = y, player = ME, params = { modules = n or 1 }, con = true } end
function H.record(id, x, y) CM.consByKey[CM.conKey(x, y)] = { id = id, file = FILE, params = CM.ser(world[id].params) } end
function H.edit(id, n) world[id].params = { modules = n } end
function H.recycle(id) world[id].con = false end
function H.drop(id) world[id] = nil end
function H.boom(id) world[id].boom = true end
function H.rec(x, y) return CM.consByKey[CM.conKey(x, y)] end
function H.forget(x, y) CM.consByKey[CM.conKey(x, y)] = nil end
function H.expectDemolish(x, y) CM.expectedDemolish[CM.conKey(x, y)] = true end
function H.editsTick() tickReads = 0; CM.scanConstructionEditsSlice(); return tickReads end
function H.removalsTick() CM.pollConstructionRemovalsSlice() end
function H.fullScan() CM.scanConstructionEdits() end
function H.reads(id) return reads[id] or 0 end
function H.resetReads() reads = {} end
function H.shipped()
  local t = {}
  for _, s in ipairs(sched) do t[#t + 1] = s.op .. "@" .. tostring(s.args.x) .. "," .. tostring(s.args.y) .. ":" .. tostring(s.args.params) end
  table.sort(t)
  return table.concat(t, ";")
end
function H.ops() local t = {} for i, s in ipairs(sched) do t[i] = s.op end return table.concat(t, ",") end
function H.clear() logs = {}; sched = {} end
function H.logs() return table.concat(logs, "\n") end
return H
''')


def fill(H, n, rnd=None):
    ids = []
    for i in range(n):
        idn = 1000 + i
        x, y = 10.0 * (i % 40), 10.0 * (i // 40)
        H.con(idn, x, y, 1)
        H.record(idn, x, y)
        ids.append((idn, x, y))
    return ids


def main():
    N = 409
    every_e = 30
    every_r = 3

    # 1. every construction exactly once per cycle, no tick over its share
    H = runtime()
    check("the cadences are the old ones (edit 30, removal 3 ticks)",
          H.K.CON_EDIT_SCAN_EVERY == every_e and H.K.REMOVAL_POLL_EVERY == every_r)
    ids = fill(H, N)
    per = math.ceil(N / every_e)
    worst = max(H.editsTick() for _ in range(every_e))
    counts = {H.reads(i) for i, _, _ in ids}
    check(f"one edit cycle ({every_e} ticks) reads each of {N} constructions exactly once", counts == {1}, str(counts))
    check(f"no tick reads more than ceil({N}/{every_e}) = {per}", worst <= per, str(worst))
    H.resetReads()
    for _ in range(every_e * 3):
        H.editsTick()
    counts = {H.reads(i) for i, _, _ in ids}
    check("three cycles read each exactly three times", counts == {3}, str(counts))

    # 2. the sliced scan ships exactly what the full scan ships
    rnd = random.Random(924)
    for trial in range(5):
        A, B = runtime(), runtime()
        ia, ib = fill(A, N), fill(B, N)
        for _ in range(every_e):     # align B to a cycle start
            B.editsTick()
        B.clear()
        picks = rnd.sample(range(N), 25)
        for k in picks:
            A.edit(ia[k][0], 2 + k)
            B.edit(ib[k][0], 2 + k)
        A.fullScan()
        for _ in range(every_e):
            B.editsTick()
        ok = A.shipped() == B.shipped() and A.ops() == ",".join(["CONU"] * 25)
        if not ok:
            break
    check("over one cycle the sliced edit scan ships exactly the full scan's CONUs (5 x 25 random edits)", ok)
    B.clear()
    for _ in range(every_e):
        B.editsTick()
    check("an edit is shipped once, not again next cycle", B.ops() == "", B.ops())

    # 3. removals: demolish, replacement on the spot, recycled id, our own replay
    X, Y = 1998.6, 5462.0

    def removal_case(setup):
        H = runtime()
        fill(H, N)
        H.con(9, X, Y); H.record(9, X, Y)
        for _ in range(every_r):
            H.removalsTick()
        H.clear()
        setup(H)
        for _ in range(every_r * 2):
            H.removalsTick()
        return H

    H = removal_case(lambda H: H.drop(9))
    check("a demolished construction ships one DEMOLISH within two removal cycles", H.ops() == "DEMOLISH" and H.rec(X, Y) is None, H.ops())
    H = removal_case(lambda H: (H.drop(9), H.con(10, X + 0.05, Y)))
    check("a replacement on the spot is not a demolish", H.ops() == "" and H.rec(X, Y) is not None, H.ops())
    H = removal_case(lambda H: H.recycle(9))
    check("a recycled id ships the DEMOLISH", H.ops() == "DEMOLISH", H.ops())
    H = removal_case(lambda H: (H.expectDemolish(X, Y), H.drop(9)))
    check("our own replay's demolish is cleared quietly", H.ops() == "" and H.rec(X, Y) is None and "not echoed" in H.logs(), H.ops())
    H = runtime()
    fill(H, N)
    H.con(9, X, Y); H.record(9, X, Y)
    for _ in range(every_r):
        H.removalsTick()
    H.clear()
    H.drop(9)
    for _ in range(every_r):
        H.removalsTick()
    check("one miss alone is not a demolish (the two-miss rule holds)", H.ops() == "" and H.rec(X, Y) is not None, H.ops())

    # 4. added mid-cycle -> next cycle; removed mid-cycle -> skipped
    H = runtime()
    fill(H, N)
    H.editsTick()                         # a cycle is under way
    H.con(5000, -50.0, -50.0); H.record(5000, -50.0, -50.0)
    for _ in range(every_e - 1):
        H.editsTick()
    first = H.reads(5000)
    for _ in range(every_e):
        H.editsTick()
    check("a construction added mid-cycle is checked in the next cycle", first == 0 and H.reads(5000) == 1, f"{first}, {H.reads(5000)}")
    H = runtime()
    ids = fill(H, N)
    H.editsTick()
    last = ids[-1]
    H.forget(last[1], last[2])
    H.clear()
    for _ in range(every_e - 1):
        H.editsTick()
    check("a construction forgotten mid-cycle is skipped without an error", H.reads(last[0]) == 0 and "error" not in H.logs(), H.logs()[-200:])

    # 5. one throwing read does not stop the rest
    H = runtime()
    ids = fill(H, N)
    H.boom(ids[5][0])
    H.edit(ids[6][0], 99)
    H.edit(ids[N - 1][0], 98)
    for _ in range(every_e):
        H.editsTick()
    check("an unreadable construction is logged and the others are still scanned",
          "edit scan error" in H.logs() and H.ops() == "CONU,CONU", H.ops())

    print()
    if fails:
        print(f"{len(fails)} FAILED")
        sys.exit(1)
    print("all passed")


if __name__ == "__main__":
    main()
