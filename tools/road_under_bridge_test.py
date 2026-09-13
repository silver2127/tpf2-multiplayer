"""Offline checks for building a road under a bridge (slice in-place removals, inject.lua ROADE), on Lua 5.2.

Building a road under a bridge makes the engine remove the bridge span and add it again
between the same two existing nodes. The slice shipped removals for upgrades only, so the
road tool's capture carried re=0: every instance laid a second span over the old one and
the build failed critical everywhere (2026-09-12, ROADP seq 89/90). That bridge was a ROAD
bridge (B built it with the street tool, captures #29/#30).

PR #2 (tearded) carries an unchanged bridge span as a COMPANION rebuilt with its own kind
and properties (br, a span of the other network). The same model now covers the road's own
network (bs): as a link the span replayed with the NEW road's street type. The slice still
ships the in-place removal (re=1); a companion carries it, so it is not shipped in rm too.

This loads the real inject.lua with stub CM/api and feeds it B's actual capture (record
287 of lockstep_inject_b.txt), as captured (re=0) and as the fixed slice writes it (re=1),
under the road bridge it really was and under a rail bridge:
  - road bridge: the span travels as bs, never in rm
  - rail bridge: the span travels as br, never in rm
  - a vertex really on the edge's surface (a true split): the removal stays with the
    peer's own split, as before

    python tools/road_under_bridge_test.py
"""
import os
import sys
import tempfile

import lupa.lua52 as lupa

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
INJ = os.path.join(REPO, "mod", "mp_lockstep_1", "res", "scripts", "mp", "inject.lua")

NODES = ("-1 -2073.6184 7777.4478 8.8026 -2 -2036.7944 7865.6411 7.0911 -3 -2049.1404 7963.3574 5.2754")
EDGES = ("111894 -1 96.2297 39.5401 2.8432 58.8002 77.1826 -0.8596 "
         "-1 -2 58.8002 77.1826 -0.8596 13.5436 96.0791 -2.1634 "
         "-2 -3 13.5436 96.0790 -2.1634 -39.5403 96.2296 -1.0682 "
         "111672 111711 -62.9859 -34.9911 7.1695 -62.9859 -34.9911 0.0000")
TAIL = "0 -1 0 -1 0 -1 1 4"
RM = "111672 111711 -62.9859 -34.9911 7.1695 -62.9859 -34.9911 0.0000"
AS_CAPTURED = f"ROADE 3 0 12 1 0 4 0 0 {NODES} {EDGES} {TAIL}"
FIXED = f"ROADE 3 0 12 1 0 4 0 1 {NODES} {EDGES} {RM} {TAIL}"
SPAN = "-2039.3987,7927.0957,20.7942,-2102.3845,7892.1045,20.7942,4"

fails = []


def check(name, cond, extra=""):
    print(("ok   " if cond else "FAIL ") + name + (f"  ({extra})" if extra else ""))
    if not cond:
        fails.append(name)


def run(record, edge_z, rail_bridge=False):
    d = tempfile.mkdtemp()
    path = os.path.join(d, "lockstep_inject_b.txt")
    with open(path, "wb") as f:
        f.write(("ARMED 1\nSTREETP 1 2\n" + record + "\n").encode())
    L = lupa.LuaRuntime(unpack_returned_tuples=True)
    L.globals().INJ_SRC = open(INJ, encoding="utf-8").read()
    L.globals().INJECT = path.replace("\\", "/")
    L.globals().EDGE_Z = edge_z
    L.globals().RAIL_BRIDGE = rail_bridge
    return L.execute(r'''
local logs, sched = {}, {}
local function sink()
  return setmetatable({}, { __index = function() return sink() end, __call = function() return nil end })
end
local CT = { BASE_EDGE = 1, BASE_NODE = 2 }
local BRIDGE = 900001
-- the span as the world holds it: the capture re-adds it unchanged (same tangents, model 4)
local edges = { [BRIDGE] = { node0 = 111672, node1 = 111711, type = 1, typeIndex = 4,
  tangent0 = { x = -62.9859, y = -34.9911, z = 7.1695 }, tangent1 = { x = -62.9859, y = -34.9911, z = 0.0 } } }
local nodes = {
  [111894] = { -2151.7856, 7717.5244, 8.0107 },
  [111672] = { -2039.3987, 7927.0957, 20.7942 },
  [111711] = { -2102.3845, 7892.1045, 20.7942 },
}
api = setmetatable({
  type = { ComponentType = CT },
  engine = setmetatable({
    getComponent = function(id, t)
      if t == CT.BASE_EDGE then return edges[id] end
      if t == CT.BASE_NODE then local p = nodes[id]; return p and { position = { x = p[1], y = p[2], z = p[3] } } end
    end,
  }, { __index = function() return sink() end }),
}, { __index = function() return sink() end })
game = setmetatable({}, { __index = function() return sink() end })
local K = setmetatable({ INSTANCE = "b", INJECT_FILE = INJECT }, { __index = function() return nil end })
local CM = { peerSeen = true, injectOffset = 0, seqNo = 88, ticks = 0 }
function CM.gameTime() return 5389.4 end
function CM.scheduleLocal(op, args) sched[#sched + 1] = { op = op, args = args } end
function CM.readFrom(p, offset)
  local f = io.open(p, "rb")
  if not f then return nil, offset end
  local size = f:seek("end")
  if offset >= size then f:close(); return nil, offset end
  f:seek("set", offset)
  local data = f:read("*a") or ""
  f:close()
  local last = #data
  while last > 0 and data:byte(last) ~= 10 do last = last - 1 end
  if last == 0 then return nil, offset end
  data = data:sub(1, last)
  return data, offset + #data
end
function CM.geomScopeBegin() end
function CM.geomScopeEnd() end
-- the bridge span is in the track network's map when it is a rail bridge, else in the
-- street map (the road's own network)
function CM.netMap(track)
  if track == RAIL_BRIDGE then return { [111672] = { BRIDGE }, [111711] = { BRIDGE } } end
  return {}
end
-- vertex -2 lies inside the bridge's plan-view footprint
function CM.findEdgeContaining(isTrack, x, y)
  if math.abs(x - -2036.7944) < 0.01 and math.abs(y - 7865.6411) < 0.01 then return BRIDGE, 0.5 end
end
function CM.edgeZAt(eid, u) if eid == BRIDGE then return EDGE_Z end end
function CM.execPolyline(c, planOnly) return nil, nil end
local log = function(s) logs[#logs + 1] = s end
assert(load(INJ_SRC, "@inject.lua"))()(CM, K, log)
CM.pollInject()
local s = sched[1]
return (s and s.op or "none"), (s and s.args.rm or "nil"), table.concat(logs, "\n"),
  (s and s.args.br or "nil"), (s and s.args.bs or "nil")
''')


def main():
    # the road bridge it really was
    op, rm, logs, br, bs = run(AS_CAPTURED, 20.7942)
    check("road bridge, re=0: the road is scheduled", op == "ROADP", logs[-300:])
    check("road bridge, re=0: the span travels as a same-network companion (bs)", bs == SPAN and br == "nil", f"bs={bs} br={br}")
    check("road bridge, re=0: nothing in rm", rm == "nil", rm)

    op, rm, logs, br, bs = run(FIXED, 20.7942)
    check("road bridge, re=1: the road is scheduled", op == "ROADP", logs[-300:])
    check("road bridge, re=1, vertex 13 m under the bridge: the span travels as bs", bs == SPAN and br == "nil", f"bs={bs} br={br}")
    check("road bridge, re=1: its in-place removal is not shipped in rm as well", rm == "nil", rm)
    check("road bridge, re=1: logged", "1 in-place removal(s) from the slice travel with those replacements" in logs, logs[-300:])

    op, rm, logs, br, bs = run(FIXED, 7.2)
    check("control: a vertex really on the edge is still a split -- removal left to the peer",
          rm == "nil" and "0 removal(s) shipped as positions, 1 left" in logs, rm)

    # the same capture under a rail bridge (PR #2's case)
    op, rm, logs, br, bs = run(AS_CAPTURED, 20.7942, rail_bridge=True)
    check("rail bridge, re=0: the road is scheduled", op == "ROADP", logs[-300:])
    check("rail bridge, re=0: the span travels as br, nothing in rm", br == SPAN and bs == "nil" and rm == "nil", f"br={br} bs={bs} rm={rm}")

    op, rm, logs, br, bs = run(FIXED, 20.7942, rail_bridge=True)
    check("rail bridge, re=1: the road is scheduled", op == "ROADP", logs[-300:])
    check("rail bridge, re=1: the span travels as br", br == SPAN and bs == "nil", f"br={br} bs={bs}")
    check("rail bridge, re=1: its in-place removal is not shipped in rm as well", rm == "nil", rm)
    check("rail bridge, re=1: logged", "1 in-place removal(s) from the slice travel with those replacements" in logs, logs[-300:])

    print()
    if fails:
        print(f"{len(fails)} FAILED")
        sys.exit(1)
    print("all passed")


if __name__ == "__main__":
    main()
