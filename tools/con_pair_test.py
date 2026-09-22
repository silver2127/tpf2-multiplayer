"""Offline checks of construction / street-payload pairing (cons.lua CM.flushConPairs), on Lua 5.2.

Until 2026-09-16 a captured construction took the nearest parked ROADC within
150 m and a ROADC nothing claimed inside 15 units was dropped with one log line
("street payload dropped"), so a big station -- its origin further than 150 m
from its road mouth -- shipped as CONP and the peers built it WITHOUT its
street. Pairing is by identity now. This loads the real cons.lua with a stub
world and checks:
  * a native build pairs with the ROADC whose nodes are its frozen nodes, even
    with the mouth 470 m from the origin, and the CONX carries the survivor
    gather radius derived from the entity's bbox + street payload (srad);
  * a cancelled placement (CONXP) pairs with the ROADC carrying the same
    placement serial (ps=), whatever else is parked and however many ticks
    apart the two were read; one shipped with rc=1 whose ROADC is missing is
    refused loudly (never built anywhere without its street); rc=0 ships as
    CONP at once; a ROADC whose CONXP was dropped (actions off) is released;
  * a CONXP with no serial (an older slice) pairs with the ROADC parked right
    before it;
  * a CONXP after a ROADC parked on an older tick (a build whose cancel did
    not land) is free-standing -> CONP with the reason, the ROADC stays parked;
  * a parked ROADC whose construction the poll never captured is rescued by
    identity even when the entity's origin lies outside the local search disk
    (the once-only whole-map search at the deadline);
  * a ROADC no construction ever claims is declared a DIVERGENCE, loudly.

    python tools/con_pair_test.py
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


L = lupa.LuaRuntime(unpack_returned_tuples=True)
L.globals().CONS_SRC = open(CONS, encoding="utf-8").read()
L.globals().FILE = FILE
H = L.execute(r'''
local logs, sched = {}, {}
local world = {}   -- id -> { file, x, y, player, frozen = {nodeId...}, bbox = {x0,y0,x1,y1} }
local nodes = {}   -- node id -> {x, y, z}
local ME = 7
local now = 100
local CT = { CONSTRUCTION = 1, PLAYER_OWNED = 2, NAME = 3, BOUNDING_VOLUME = 4, BASE_NODE = 5 }
api = {
  type = { ComponentType = CT },
  engine = {
    util = { getPlayer = function() return ME end },
    entityExists = function(id) return world[id] ~= nil end,
    getComponent = function(id, t)
      if t == CT.BASE_NODE then
        local n = nodes[id]
        return n and { position = { x = n[1], y = n[2], z = n[3] } } or nil
      end
      local w = world[id]
      if not w then return nil end
      if t == CT.CONSTRUCTION then
        local tr = {}; for i = 1, 16 do tr[i] = 0 end; tr[1], tr[6], tr[11], tr[16] = 1, 1, 1, 1; tr[13], tr[14] = w.x, w.y
        return { fileName = w.file, transf = tr, frozenNodes = w.frozen or {} }
      elseif t == CT.PLAYER_OWNED then
        return w.player and { player = w.player } or nil
      elseif t == CT.BOUNDING_VOLUME then
        local b = w.bbox
        return b and { bbox = { min = { x = b[1], y = b[2] }, max = { x = b[3], y = b[4] } } } or nil
      end
    end,
  },
}
game = { interface = {
  getEntities = function(filter, opts)
    local out = {}
    if opts.type ~= "CONSTRUCTION" then return out end
    for id, w in pairs(world) do
      local dx, dy = w.x - filter.pos[1], w.y - filter.pos[2]
      if dx * dx + dy * dy <= filter.radius * filter.radius then out[#out + 1] = id end
    end
    return out
  end,
  getEntity = function(id) local w = world[id]; return w and { params = {} } end,
} }
local K = setmetatable({ INSTANCE = "a" }, { __index = function() return nil end })
local CM = setmetatable({ expectedCons = {}, cmExpectedCompany = {}, cmExpectedBal0 = {}, expectedDemolish = {}, ticks = 0, seqNo = 0 },
  { __index = function() return nil end })
function CM.gameTime() return now end
function CM.scheduleLocal(op, args) CM.seqNo = CM.seqNo + 1; CM.lastSchedAt = now; sched[#sched + 1] = { op = op, args = args } end
function CM.cmLog(s) logs[#logs + 1] = s end
function CM.cmBalance() return nil end
function CM.livePeers() return 1 end
local log = function(s) logs[#logs + 1] = s end
assert(load(CONS_SRC, "@cons.lua"))()(CM, K, log)
local H = { CM = CM }
function H.put(id, x, y, player, frozen, bbox) world[id] = { file = (player and FILE or "building/town.con"), x = x, y = y, player = player, frozen = frozen, bbox = bbox } end
function H.node(id, x, y) nodes[id] = { x, y, 0 } end
function H.tick(t) CM.ticks = t end
function H.time(t) now = t end
-- a ROADC as inject.lua parks it: new nodes with positions, existing nodes' positions, one edge between each pair
function H.roadc(newPts, oldPts, ps)
  local posOf, spos, adds = {}, {}, {}
  local ids = {}
  for i, p in ipairs(newPts) do posOf[-i] = { p[1], p[2], 0 }; ids[#ids + 1] = -i end
  for i, p in ipairs(oldPts) do spos[1000 + i] = { p[1], p[2], 0 }; ids[#ids + 1] = 1000 + i end
  for i = 1, #ids - 1 do adds[#adds + 1] = { ids[i], ids[i + 1], { 1, 0, 0, 1, 0, 0 }, 0, -1 } end
  CM.pendingRoadc[#CM.pendingRoadc + 1] = { at = now, posOf = posOf, adds = adds, rms = {}, spos = spos,
    etype = 0, stype = 16, ttype = 1, cat = 0, bal0 = nil, ps = ps }
end
-- a captured native build (queueConCapture's record shape)
function H.native(id)
  local w = world[id]
  CM.pendingCons[#CM.pendingCons + 1] = { at = now, file = w.file, key = CM.conKey(w.x, w.y), t = "1,0,0,0,0,1,0,0,0,0,1,0," .. w.x .. "," .. w.y .. ",0,1",
    params = "{}", x = w.x, y = w.y, name = "", id = id }
end
-- a cancelled placement (inject.lua's CONXP record shape)
function H.conxp(x, y, ps, rc)
  CM.pendingCons[#CM.pendingCons + 1] = { at = now, file = FILE, t = "1,0,0,0,0,1,0,0,0,0,1,0," .. x .. "," .. y .. ",0,1",
    params = "{}", name = "x", x = x, y = y, id = nil, cancelled = 1, ps = ps, hadRoadc = rc }
end
function H.drop(ps) CM.droppedConxp[ps] = true end
function H.flush() CM.flushConPairs() end
function H.nsched() return #sched end
function H.sched(i) return sched[i].op, sched[i].args end
function H.parked() return #CM.pendingRoadc, #CM.pendingCons end
function H.logs() return table.concat(logs, "\n") end
function H.clear() logs = {}; sched = {} end
function H.radius(x, y, id, pts) return CM.survivorRadius(x, y, id, pts) end
return H
''')

# ---- 1. native build, mouth 470 m from the origin: identity pairs it, distance never could
H.tick(10)
H.node(1, 400, 5)
H.node(2, 405, 5)
H.put(500, 0, 0, 7, L.table(1, 2), L.table(-300, -200, 420, 200))
H.put(601, 0, 300, None)     # town buildings: one inside the derived disk, one beyond it
H.put(602, 0, 800, None)
H.roadc(L.table(L.table(400, 5), L.table(405, 5), L.table(430, 5)), L.table(L.table(470, 5)))
H.native(500)
H.flush()
op, args = H.sched(1)
check("native build 470 m from its mouth pairs by identity -> CONX", H.nsched() == 1 and op == "CONX", f"n={H.nsched()} op={op}")
check("CONX carries the street payload", args and args["snodes"] and args["snodes"].count(",") >= 9, args and args["snodes"])
srad = args and args["srad"]
check("srad derived from the extent: 470 m street reach + 100 m margin", srad is not None and 569 < srad < 571, str(srad))
check("survivors gathered in that disk: 300 m building listed, 800 m one not",
      args and args["survivors"] == "0.0:300.0", args and args["survivors"])
check("pairing reason logged", "paired with its street payload: payload shares 2 node(s)" in H.logs(), H.logs()[-300:])
check("buffers empty", H.parked() == (0, 0), str(H.parked()))

# ---- 2. cancelled placement: the ROADC parked right before it, same poll
H.clear()
H.tick(20)
H.roadc(L.table(L.table(1000, 1000), L.table(1010, 1000)), L.table(L.table(1050, 1000)))
H.conxp(1000, 980)
H.flush()
op, args = H.sched(1)
check("CONXP pairs with the ROADC parked right before it -> CONX cancelled=1",
      H.nsched() == 1 and op == "CONX" and args["cancelled"] == 1, f"n={H.nsched()} op={op}")
check("cancelled placement: radius from the street payload alone (54 m reach + 100)", 153 < args["srad"] < 155, str(args["srad"]))

# ---- 3. a ROADC from an older tick is not this placement's: CONP, and the payload stays parked
H.clear()
H.tick(30)
H.roadc(L.table(L.table(2000, 2000), L.table(2010, 2000)), L.table(L.table(2050, 2000)))
H.tick(60)
H.conxp(2000, 1980)
H.flush()
op, args = H.sched(1)
check("CONXP after a 30-tick-old ROADC ships as CONP", H.nsched() == 1 and op == "CONP" and args["cancelled"] == 1, f"n={H.nsched()} op={op}")
check("...with the reason in the log", "30 tick(s) older -- a build whose cancel did not land" in H.logs(), H.logs()[-300:])
check("...and the ROADC still parked", H.parked() == (1, 0), str(H.parked()))
check("CONP carries survivors radius too", args["srad"] == 100, str(args["srad"]))

# ---- 4. that ROADC's real owner: an untracked native entity whose origin is 900 m from the payload
#         (outside any local search disk) -> the whole-map search at the deadline finds it by identity
H.clear()
H.node(3, 2000, 2000)
H.put(700, 2000, 2900, 7, L.table(3), L.table(1900, 2800, 2100, 3000))
H.time(102)      # past the 1-unit grace: local rescue disk = 25 m extent + 100 m, origin is 900 m away
H.tick(65)
H.flush()
check("local rescue disk (payload extent + margin) does not reach the entity", H.nsched() == 0 and H.parked() == (1, 0), f"n={H.nsched()} parked={H.parked()}")
H.time(116)      # past K.ROADC_ORPHAN_UNITS: one whole-map search
H.flush()
op, args = H.sched(1)
check("whole-map search rescues it by identity -> CONX", H.nsched() == 1 and op == "CONX", f"n={H.nsched()} op={op}")
check("...logged as a rescue", "rescued its construction" in H.logs() and "whole-map" in H.logs(), H.logs()[-300:])
check("srad covers bbox + 900 m payload reach", args["srad"] > 1000, str(args["srad"]))

# ---- 5. a payload nothing ever claims: never dropped in silence
H.clear()
H.time(200)
H.roadc(L.table(L.table(5000, 5000), L.table(5010, 5000)), L.table(L.table(5050, 5000)))
H.time(216)
H.flush()
check("orphan payload: DIVERGENCE logged loudly", "ROADC: DIVERGENCE -- no construction claimed this street payload" in H.logs()
      and "report this line" in H.logs(), H.logs()[-300:])
check("orphan payload: gone from the buffer, nothing shipped", H.parked() == (0, 0) and H.nsched() == 0, f"parked={H.parked()} n={H.nsched()}")

# ---- 6. the radius helper alone
r, src = H.radius(0, 0, 500, None)
check("survivorRadius: bbox corner (420,200) -> 465 + 100", 564 < r < 566, f"{r} ({src})")
r, src = H.radius(0, 0, None, None)
check("survivorRadius: nothing known -> margin only, says so", r == 100 and "origin only" in src, f"{r} ({src})")

# ---- 7. IDENTITY by placement serial: two ROADCs parked (ps 41, 42), the CONXP for 41 read three
#         ticks later -- it takes ITS payload, not the newest one, not the same-tick one
H.clear()
H.time(300)
H.tick(300)
H.roadc(L.table(L.table(6000, 6000), L.table(6010, 6000)), L.table(L.table(6050, 6000)), 41)
H.roadc(L.table(L.table(7000, 7000), L.table(7010, 7000)), L.table(L.table(7050, 7000)), 42)
H.tick(303)
H.conxp(6000, 5980, 41, 1)
H.flush()
op, args = H.sched(1)
check("CONXP ps=41 pairs with the ROADC ps=41 across 3 ticks and past a newer payload -> CONX",
      H.nsched() == 1 and op == "CONX" and args["cancelled"] == 1, f"n={H.nsched()} op={op}")
check("...the payload it took is the ps=41 one (its nodes at 6000)", args["snodes"] and "6000.0000" in args["snodes"], args and args["snodes"])
check("...reason names the serial", "same placement serial ps=41" in H.logs(), H.logs()[-300:])
check("...ROADC ps=42 still parked", H.parked() == (1, 0), str(H.parked()))
H.clear()
H.conxp(7000, 6980, 42, 1)
H.flush()
op, args = H.sched(1)
check("CONXP ps=42 then takes the ps=42 payload", H.nsched() == 1 and op == "CONX" and "7000.0000" in args["snodes"], f"n={H.nsched()} op={op}")
check("buffers empty", H.parked() == (0, 0), str(H.parked()))

# ---- 8. rc=1 but no payload with that serial parked: REFUSED loudly, nothing shipped, nothing waits
H.clear()
H.roadc(L.table(L.table(8000, 8000), L.table(8010, 8000)), L.table(L.table(8050, 8000)), 50)   # someone else's
H.conxp(9000, 9000, 51, 1)
H.flush()
check("CONXP rc=1 with its ROADC missing is refused: nothing scheduled", H.nsched() == 0, f"n={H.nsched()}")
check("...loudly, naming the serial and the fix", "CONXP: REFUSED" in H.logs() and "ps=51" in H.logs() and "place it again" in H.logs(), H.logs()[-300:])
check("...the other payload untouched, the CONXP gone", H.parked() == (1, 0), str(H.parked()))

# ---- 9. rc=0: free-standing, ships as CONP at once with the reason
H.clear()
H.conxp(9500, 9500, 52, 0)
H.flush()
op, args = H.sched(1)
check("CONXP rc=0 ships as CONP at once", H.nsched() == 1 and op == "CONP" and args["cancelled"] == 1, f"n={H.nsched()} op={op}")
check("...saying it is free-standing by serial", "free-standing, the slice shipped no street payload (ps=52 rc=0)" in H.logs(), H.logs()[-300:])
check("...and did not take the stray ps=50 payload", H.parked() == (1, 0), str(H.parked()))

# ---- 10. the stray payload's CONXP was dropped (actions off): released, no search, no DIVERGENCE
H.clear()
H.drop(50)
H.time(320)
H.flush()
check("ROADC of a dropped placement is released", H.parked() == (0, 0) and H.nsched() == 0, f"parked={H.parked()} n={H.nsched()}")
check("...with the reason, not a DIVERGENCE", "ROADC: released -- its placement ps=50 was dropped" in H.logs() and "DIVERGENCE" not in H.logs(), H.logs()[-300:])

print("FAILED: " + ", ".join(fails) if fails else "all passed")
sys.exit(1 if fails else 0)
