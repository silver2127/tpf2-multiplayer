"""Offline checks of the peer-side survivor diff (conx.lua CM.execConX), on Lua 5.2.

A replayed CONX/CONP removes the town buildings and asset groups the
originator's world no longer has. Until 2026-09-16 the diff judged a fixed 190 m
disk (the originator gathered 200 m) and REFUSED any diff wanting more than 40
removals as 'stale' -- so exactly the largest placements left every building
standing on the peers (town-building desync), and a building beyond 190 m was
never judged, with no log line. This loads the real conx.lua with a stub engine
that builds the proposal at once, and checks:
  * 60 town buildings the list lacks are all removed -- no count cap;
  * the judged disk is the shipped gather radius minus K.SURV_INNER: a building
    500 m out goes when srad says so, one just outside the disk stays;
  * staleness is judged by what the list claims: a list whose survivors mostly
    do not exist here is refused, loudly, as a DIVERGENCE;
  * a list without its radius (srad) falls back to the corridor sweep, loudly;
  * params the reader cannot parse skip the build with a DIVERGENCE line.

    python tools/survivor_diff_test.py
"""
import os
import sys

import lupa.lua52 as lupa

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
CONX = os.path.join(REPO, "mod", "mp_lockstep_1", "res", "scripts", "mp", "conx.lua")
FILE = "station/rail/modular_station/modular_station.con"

fails = []


def check(name, cond, extra=""):
    print(("ok   " if cond else "FAIL ") + name + (f"  ({extra})" if extra else ""))
    if not cond:
        fails.append(name)


L = lupa.LuaRuntime(unpack_returned_tuples=True)
L.globals().CONX_SRC = open(CONX, encoding="utf-8").read()
L.globals().FILE = FILE
H = L.execute(r'''
local logs = {}
local world = {}      -- id -> { kind = "CONSTRUCTION"|"ASSET_GROUP", x, y, player }
local bulldozed = {}
local built = 0
local BID = 900       -- the entity every build lands as
local ME = 7
local function sink()
  return setmetatable({}, { __index = function() return sink() end, __call = function() return sink() end })
end
local CT = { CONSTRUCTION = 1, PLAYER_OWNED = 2, NAME = 3, BOUNDING_VOLUME = 4, BASE_NODE = 5, BASE_EDGE = 6 }
api = setmetatable({
  type = setmetatable({ ComponentType = CT }, { __index = function() return sink() end }),
  engine = {
    util = { getPlayer = function() return ME end },
    entityExists = function(id) return world[id] ~= nil end,
    getComponent = function(id, t)
      local w = world[id]
      if not w then return nil end
      if t == CT.CONSTRUCTION then
        local tr = {}; for i = 1, 16 do tr[i] = 0 end; tr[13], tr[14] = w.x, w.y
        return { fileName = w.file or FILE, transf = tr }
      elseif t == CT.PLAYER_OWNED then
        return w.player and { player = w.player } or nil
      elseif t == CT.BOUNDING_VOLUME then
        return { bbox = { min = { x = w.x - 50, y = w.y - 30 }, max = { x = w.x + 50, y = w.y + 30 } } }
      end
    end,
    system = { streetSystem = { getNode2TrackEdgeMap = function() return {} end } },
  },
  cmd = {
    make = { buildProposal = function(sp, ctx, ignore) return { sp = sp } end },
    sendCommand = function(cmd, cb)
      built = built + 1
      world[BID] = { kind = "CONSTRUCTION", x = 100, y = 100, player = ME, file = FILE }
      cb({ resultEntities = { BID }, resultProposalData = { costs = 1000 } }, true)
    end,
  },
}, { __index = function() return sink() end })
game = { interface = {
  getEntities = function(filter, opts)
    local out = {}
    for id, w in pairs(world) do
      if w.kind == opts.type then
        local dx, dy = w.x - filter.pos[1], w.y - filter.pos[2]
        if dx * dx + dy * dy <= filter.radius * filter.radius then out[#out + 1] = id end
      end
    end
    return out
  end,
  getEntity = function(id) local w = world[id]; return w and { position = { w.x, w.y }, params = {} } end,
  bulldoze = function(id) bulldozed[#bulldozed + 1] = id; world[id] = nil end,
} }
local K = setmetatable({ INSTANCE = "a", SURV_INNER = 10 }, { __index = function() return nil end })
local CM = setmetatable({ expectedCons = {}, cmExpectedCompany = {}, cmExpectedBal0 = {}, expectedDemolish = {}, consByKey = {}, conBal0 = {}, ticks = 0 },
  { __index = function() return nil end })
function CM.gameTime() return 100 end
function CM.scheduleLocal() end
function CM.cmLog(s) logs[#logs + 1] = s end
function CM.cmBalance() return nil end
function CM.livePeers() return 1 end
function CM.conKey(x, y) return string.format("%.1f/%.1f", x, y) end
function CM.conxContext() return nil end
function CM.unescName(s) return s end
function CM.findNodeNear() return nil end
function CM.watchSplit() end
function CM.forgetKnownCon() return false end
function CM.townCountNear() return 0 end
local log = function(s) logs[#logs + 1] = s end
-- deserParams as cons.lua defines it: a literal, or nil + a loud line
function CM.deserParams(pstr)
  if not pstr or pstr == "" then return nil end
  local chunk = load("return " .. pstr, "params", "t", {})
  if not chunk then log("params: does not parse -- REFUSED"); return nil, "parse" end
  local ok, v = pcall(chunk)
  if ok and type(v) == "table" then return v end
  return nil, "not a table"
end
assert(load(CONX_SRC, "@conx.lua"))()(CM, K, log)
local H = { CM = CM }
function H.reset() world = {}; bulldozed = {}; logs = {}; built = 0; CM.conxBusy = false end
function H.town(id, x, y, kind) world[id] = { kind = kind or "CONSTRUCTION", x = x, y = y } end
function H.conx(survivors, srad, params)
  CM.execConX({ op = "CONX", seq = 1, origin = "b", at = 1, file = FILE,
    t = "1,0,0,0,0,1,0,0,0,0,1,0,100,100,0,1", params = params or "{}",
    survivors = survivors, srad = srad, snodes = "", sedges = "", srm = "", spos = "" })
end
function H.nbulldozed() return #bulldozed end
function H.alive(id) return world[id] ~= nil end
function H.built() return built end
function H.logs() return table.concat(logs, "\n") end
return H
''')

# ---- 1. no count cap: 60 buildings the list lacks all go; the 10 listed stay
H.reset()
listed = []
for i in range(1, 71):
    x, y = 100 + (i % 10) * 15, 100 + (i // 10) * 15      # 70 town buildings within ~100 m
    H.town(1000 + i, x, y)
    if i <= 10:
        listed.append(f"{x:.1f}:{y:.1f}")
H.conx(";".join(listed), 300)
check("60 unlisted buildings removed (old cap: 40 -> refused)", H.nbulldozed() == 60, str(H.nbulldozed()))
check("the 10 listed survivors kept", all(H.alive(1000 + i) for i in range(1, 11)))
check("no 'snapshot looks stale' refusal", "REFUSED" not in H.logs(), H.logs()[-300:])
check("summary names the disk and the gather radius", "kept in the 290 m disk (gathered 300 m)" in H.logs(), H.logs()[-300:])

# ---- 2. the judged disk follows srad: 500 m out is judged when srad says so
H.reset()
H.town(2001, 600, 100)      # 500 m from the origin: beyond the old fixed 190 m
H.town(2002, 100, 100 + 595)  # 595 m: inside the 600 m gather, outside the 590 m judged disk
H.conx("", 600)
check("building 500 m out removed under a 600 m gather radius", not H.alive(2001))
check("building in the 10 m rim (never fully judged by the originator) kept", H.alive(2002))
H.reset()
H.town(2001, 600, 100)
H.conx("", 300)
check("the same building is NOT judged under a 300 m gather radius", H.alive(2001))

# ---- 3. staleness by what the list claims, not by removal count
H.reset()
for i in range(1, 21):
    H.town(3000 + i, 100 + i * 5, 100)                # 20 buildings here, none listed
foreign = ";".join(f"{9000 + i * 5:.1f}:9000.0" for i in range(20))   # 20 survivors that do not exist here
H.conx(foreign, 300)
check("foreign list (most survivors missing here): diff REFUSED, nothing removed", H.nbulldozed() == 0, str(H.nbulldozed()))
check("...logged as a DIVERGENCE with the counts", "survivor-diff REFUSED (DIVERGENCE): 20 of the 20 listed survivor(s) do not exist" in H.logs(), H.logs()[-400:])
H.reset()
for i in range(1, 21):
    H.town(3000 + i, 100 + i * 5, 100)
mostly_here = ";".join(f"{100 + i * 5:.1f}:100.0" for i in range(1, 16)) + ";9000.0:9000.0;9005.0:9000.0"
H.conx(mostly_here, 300)
check("list mostly present here (2 of 17 missing): diff runs, 5 unlisted removed", H.nbulldozed() == 5, str(H.nbulldozed()))
check("...and the missing ones are reported", "(2 not found here)" in H.logs(), H.logs()[-300:])

# ---- 4. survivors without their radius: corridor fallback, loudly
H.reset()
H.town(4001, 120, 100)
H.conx("300.0:300.0", None)
check("no srad: nothing removed by the diff", H.alive(4001))
check("no srad: logged as a fallback to report", "without their gather radius (srad)" in H.logs(), H.logs()[-300:])

# ---- 5. unreadable params: build skipped, DIVERGENCE line, queue released
H.reset()
H.conx("", 300, "{[1]=")
check("unreadable params: nothing built", H.built() == 0, str(H.built()))
check("unreadable params: DIVERGENCE logged", "shipped params could not be read -- build SKIPPED (DIVERGENCE" in H.logs(), H.logs()[-300:])
check("unreadable params: replay queue released", H.CM.conxBusy is False)

print("FAILED: " + ", ".join(fails) if fails else "all passed")
sys.exit(1 if fails else 0)
