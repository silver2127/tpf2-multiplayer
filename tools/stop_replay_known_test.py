"""A catch-up scan must not re-ship edge objects that a replay made or removed.

Strict stop/signal placement (STOPX/STOPXDEL) replays on every instance through
CM.nativeStopProposal at the stamp. The stop poll (CM.pollStops) runs only at load
and as a one-shot catch-up scan, so before 2026-09-25 it never learned the
replayed objects: the 8-unit expectations had expired by the next scan and it
shipped every signal replayed since the last scan as a NEW STOPADD (measured
2026-09-24, nine signals re-shipped 2000 units after they were built).

This drives the real stops.lua (lupa, Lua 5.2 like the game) against a small
fake street system whose sendCommand applies the proposal the way the engine
does (survivors keep ids, -1 becomes a new entity, edgeObjectsToRemove die):

  1. a replayed add, then a catch-up scan 100 units later: nothing shipped;
  2. a replayed removal, then a scan: nothing shipped;
  3. a NATIVE object (the slice's fallback) is still captured as STOPADD;
  4. a native object already on the edge when a replay rebuilds it is not
     swallowed by the registration: the scan still ships it;
  5. a failed replay registers nothing.

    python tools/stop_replay_known_test.py            # the worktree's stops.lua
    python tools/stop_replay_known_test.py OLD.lua    # expect FAIL on the old file
"""
from pathlib import Path
import sys
from lupa.lua52 import LuaRuntime

root = Path(__file__).resolve().parents[1]
scripts = root / 'mod/mp_lockstep_1/res/scripts'
stops_src = Path(sys.argv[1]).read_text(encoding='utf-8') if len(sys.argv) > 1 else None

lua = LuaRuntime(unpack_returned_tuples=True)
lua.globals().MP = scripts.as_posix()
lua.globals().STOPS_SRC = stops_src
lua.execute(r'''
package.path = MP .. '/?.lua;' .. package.path
local CT = {BASE_EDGE=1, BASE_EDGE_TRACK=2, MODEL_INSTANCE_LIST=3, NAME=4, STATION=5,
  SIGNAL_LIST=6, PLAYER_OWNED=7, BASE_EDGE_STREET=8}
-- world: edges {id -> {node0, node1, objects={{id, side}}}}, objects {id -> {x, y, model}}
W = {edges = {[201] = {node0 = 11, node1 = 12, objects = {}}}, objs = {}, nextId = 1000}
W.fail = false
local function newId() W.nextId = W.nextId + 1; return W.nextId end
local function objPos(u) return u * 2000, 3 end    -- the model stands 3 m off the centreline

api = {type = {ComponentType = CT,
    SimpleProposal = {new = function() return {streetProposal = {edgesToAdd = {}, edgesToRemove = {},
      edgeObjectsToAdd = {}, edgeObjectsToRemove = {}}} end},
    SimpleStreetProposal = {EdgeObject = {new = function() return {} end}}},
  res = {modelRep = {getName = function(m) return m end}},
  engine = {util = {getPlayer = function() return 99 end},
    system = {streetSystem = {getEdgeObject2EdgeMap = function()
      local m = {}
      for eid, e in pairs(W.edges) do for _, o in ipairs(e.objects) do m[o[1]] = eid end end
      return m
    end}},
    getComponent = function(id, kind)
      assert(id, 'nil entity passed to the engine')
      if kind == CT.BASE_EDGE then return W.edges[id] end
      if kind == CT.BASE_EDGE_TRACK then return W.edges[id] and {} or nil end
      local o = W.objs[id]
      if not o then return nil end
      if kind == CT.MODEL_INSTANCE_LIST then
        local t = {}; for i = 1, 16 do t[i] = 0 end
        t[13], t[14], t[15] = o.x, o.y, 0
        return {fatInstances = {{transf = t, modelId = o.model}}}
      end
      if kind == CT.NAME then return {name = ''} end
      if kind == CT.SIGNAL_LIST then return {signals = {{type = 0}}} end
      if kind == CT.PLAYER_OWNED then return {player = 99} end
      return nil
    end},
  cmd = {make = {buildProposal = function(sp) return sp end},
    sendCommand = function(sp, cb)
      if W.fail then cb({}, false); return end
      local s = sp.streetProposal
      local removed = {}
      for _, id in ipairs(s.edgeObjectsToRemove) do removed[id] = true; W.objs[id] = nil end
      local newEdge = {}
      for k, e in ipairs(s.edgesToAdd) do
        local old = s.edgesToRemove[k]
        W.edges[old] = nil
        local id = newId()
        local objs = {}
        for _, o in ipairs(e.comp.objects) do
          if o[1] >= 0 then
            assert(not removed[o[1]], 'a removed object was carried')
            objs[#objs + 1] = {o[1], o[2]}
          else
            local add = s.edgeObjectsToAdd[-o[1]]
            local oid = newId()
            local x, y = objPos(add.param)
            W.objs[oid] = {x = x, y = y, model = add.model}
            objs[#objs + 1] = {oid, o[2]}
          end
        end
        W.edges[id] = {node0 = e.comp.node0, node1 = e.comp.node1, objects = objs}
        newEdge[#newEdge + 1] = id
      end
      W.lastEdge = newEdge[1]
      cb({}, true)
    end}}
game = {interface = {getEntities = function() return {} end}}

local now = 0
CM = {ticks = 0, conxQueue = {}, gameTime = function() return now end,
  escName = function(s) return s end, unescName = function(s) return s end,
  buildContext = function() return {} end, linesUsingStation = function() return {} end}
function advance(dt) now = now + dt end
shipped = {}
CM.scheduleLocal = function(op, f) shipped[#shipped + 1] = {op = op, f = f} end
local geom = require('mp/geom')(CM, {}, function() end)
CM.hermitePos, CM.hermiteTangent = geom.hermitePos, geom.hermiteTangent
local K = {INSTANCE = 'a', STOP_EDGE_EPS = 14}
local logs = {}
local log = function(s) logs[#logs + 1] = s end
LOGS = logs
local factory = STOPS_SRC and assert(load(STOPS_SRC, 'stops.lua')) () or require('mp/stops')
factory(CM, K, log)
CM.edgeGeomT = function(eid)
  local e = W.edges[eid]
  if not e then return nil end
  return e, {0, 0, 0}, {2000, 0, 0}, {2000, 0, 0}, {2000, 0, 0}
end
CM.uOnEdge = function(eid, x, y) return x / 2000, math.abs(y) end
CM.frozenOwnerOf = function() return nil end
CM.nativeEdgeCopy = function(eid, isTrack, ent)
  local e = W.edges[eid]
  return {entity = ent, comp = {node0 = e.node0, node1 = e.node1}}
end
function edgeNow()
  for eid, e in pairs(W.edges) do if e.node0 == 11 then return eid end end
end
function nativeAdd(u, model)
  local oid = newId()
  local x, y = objPos(u)
  W.objs[oid] = {x = x, y = y, model = model}
  local e = W.edges[edgeNow()]
  e.objects[#e.objects + 1] = {oid, 2}
  return oid
end
function replayAdd(u, model)
  local x, y = objPos(u)
  local okS, why = CM.nativeStopProposal({eid = edgeNow(), u = u, left = true, side = 2, model = model,
    name = '', x = x, y = y}, nil, 'test add', nil)
  assert(okS, why)
end
function replayRemove(oid)
  local o = W.objs[oid]
  local okS, why = CM.nativeStopProposal(nil, {eo = oid, eid = edgeNow(), x = o.x, y = o.y}, 'test rm', nil)
  assert(okS, why)
end
function ops()
  local t = {}
  for _, s in ipairs(shipped) do t[#t + 1] = s.op .. '@' .. string.format('%.0f', s.f.x) end
  return table.concat(t, ',')
end
''')
g = lua.globals()
run = lua.eval

# prime with one signal from the save
lua.execute("nativeAdd(0.1, 'sig'); CM.pollStops(); assert(CM.stopPrimed)")
assert lua.eval("#shipped") == 0

# 1. a replayed add, then a catch-up scan long after the expectations expired
lua.execute("replayAdd(0.5, 'sig'); replayAdd(0.7, 'sig'); advance(100); CM.pollStops()")
got = lua.eval("ops()")
assert got == '', 'catch-up scan re-shipped replayed objects: ' + got

# 2. a replayed removal, then a scan
lua.execute("""
local victim
for oid, o in pairs(W.objs) do if math.abs(o.x - 1000) < 1 then victim = oid end end
replayRemove(victim); advance(100); CM.pollStops()""")
got = lua.eval("ops()")
assert got == '', 'catch-up scan re-shipped a replayed removal: ' + got

# 3. the fallback: a native (uncancelled) object is still captured
lua.execute("nativeAdd(0.3, 'sig'); advance(1); CM.pollStops()")
got = lua.eval("ops()")
assert got == 'STOPADD@600', 'native object not captured: ' + got

# 4. a native object waiting for its catch-up survives a replay's registration
lua.execute("shipped = {}; nativeAdd(0.9, 'sig'); replayAdd(0.2, 'sig'); advance(100); CM.pollStops()")
got = lua.eval("ops()")
assert got == 'STOPADD@1800', 'registration swallowed a native object on the replayed edge: ' + got

# 5. a failed replay registers nothing (and its object does not exist)
lua.execute("shipped = {}; W.fail = true; replayAdd(0.4, 'sig'); W.fail = false; advance(100); CM.pollStops()")
got = lua.eval("ops()")
assert got == '', 'failed replay: ' + got
assert any('registered as known' in s for s in lua.eval("LOGS").values()), 'EXEC log does not report the registration'

print('PASS: replayed adds and removals are known to the catch-up scan; native objects still ship; failed replays register nothing')
