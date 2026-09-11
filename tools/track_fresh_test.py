"""Offline checks for the parallel-track-through-a-crossing fix (inject.lua ROADE -> fv), on Lua 5.2.

A rail vertex a few metres off a road's centreline near the road's end counted as "on" the
road: the replay snapped it to the road's end node, the same-pair dedup then dropped the
rail edge into the crossing, and the build failed critical on every instance (2026-09-11,
ROADP seq 328/329). The originator's capture already says which new nodes its engine
attached to an existing edge -- exactly those with split halves -- so inject.lua now ships
every OTHER new node's vertex index as fv=, and execPolyline gives those a plain node.

This loads the real mod/.../scripts/mp/inject.lua with stub CM/api and feeds it the two
captures from that session (lockstep_inject_a.txt):
  - seq 328's ROADE: the vertex beside the road is fresh; the crossing node that split the
    road is not; the plan pass receives the same fv
  - seq 339's ROADE (a crossing that built): its crossing node is not fresh either

    python tools/track_fresh_test.py
"""
import os
import sys
import tempfile

import lupa.lua52 as lupa

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
INJ = os.path.join(REPO, "mod", "mp_lockstep_1", "res", "scripts", "mp", "inject.lua")

# seq 328: rail 263119 -> -1 -> -2 -> -3 -> -4 -> -5 -> -6; the road 260895-260413 is split at -3
ROADE_328 = ("ROADE 6 1 -1 0 1 8 0 0 -1 5122.6934 5278.9854 12.4299 -2 5082.7148 5222.0713 11.9300 "
             "-3 5075.9932 5212.3188 11.8623 -4 5043.1309 5163.8120 11.5989 -5 5004.5566 5104.8823 11.4408 "
             "-6 4995.3682 5090.5225 11.4260 263119 -1 -41.3972 -55.8798 -0.8019 -40.4706 -56.5545 -0.5986 "
             "-1 -2 -40.4764 -56.5625 -0.5987 -39.5399 -57.2211 -0.4111 -2 -3 -6.7334 -9.7444 -0.0700 -6.7100 -9.7604 -0.0655 "
             "-3 -4 -33.1927 -48.2823 -0.3238 -32.4973 -48.7532 -0.1992 -4 -5 -39.0653 -58.6068 -0.2395 -38.0821 -59.2504 -0.0792 "
             "-5 -6 -9.2176 -14.3413 -0.0192 -9.1593 -14.3778 -0.0104 260895 -3 7.3531 33.3463 -0.8127 6.9226 33.4387 0.6622 "
             "-3 260413 2.6078 12.5964 0.2494 2.6171 12.5943 -0.3368 0 -1 0 -1 0 -1 0 -1 0 -1 0 -1 0 -1 0 -1")
# seq 339: rail 260477 -> -1 -> -2 -> -3 -> -4; the road 260413-236480 is split at -3
ROADE_339 = ("ROADE 4 1 -1 0 1 6 0 0 -1 5159.0835 5340.9458 12.6459 -2 5097.0903 5268.5469 10.2788 "
             "-3 5085.1343 5255.4658 9.7853 -4 5031.2119 5199.6152 7.5298 260477 -1 -55.9109 -77.3026 -1.0967 "
             "-60.0044 -74.0785 -2.0493 -1 -2 -60.0044 -74.0785 -2.0493 -63.9589 -70.6923 -2.6215 -2 -3 -11.8892 -13.1409 "
             "-0.4873 -12.0230 -13.0205 -0.4993 -3 -4 -52.6557 -57.0244 -2.1866 -55.1762 -54.6627 -2.2902 260413 -3 6.3613 "
             "30.6129 -0.8187 6.7966 30.4641 -2.1023 -3 236480 6.5670 29.4351 -2.0313 7.4717 29.1401 -0.5149 "
             "0 -1 0 -1 0 -1 0 -1 0 -1 0 -1")

fails = []


def check(name, cond, extra=""):
    print(("ok   " if cond else "FAIL ") + name + (f"  ({extra})" if extra else ""))
    if not cond:
        fails.append(name)


def runtime(path):
    L = lupa.LuaRuntime(unpack_returned_tuples=True)
    L.globals().INJ_SRC = open(INJ, encoding="utf-8").read()
    L.globals().INJECT = path.replace("\\", "/")
    return L.execute(r'''
local logs, sched, plans = {}, {}, {}
local function sink()
  return setmetatable({}, { __index = function() return sink() end, __call = function() return nil end })
end
local CT = { BASE_EDGE = 1, BASE_NODE = 2 }
local edges = {
  [157333] = { node0 = 260895, node1 = 260413 },
  [253998] = { node0 = 260413, node1 = 236480 },
}
local nodes = {
  [263119] = { 5163.6284, 5335.2031, 13.1297 }, [260895] = { 5068.64, 5178.97, 12.67 },
  [260413] = { 5078.60, 5224.91, 11.53 },     [260477] = { 5215.0, 5418.2, 13.7 },
  [236480] = { 5091.70, 5284.90, 9.30 },
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
local K = setmetatable({ INSTANCE = "a", INJECT_FILE = INJECT }, { __index = function() return nil end })
local CM = { peerSeen = true, injectOffset = 0, seqNo = 0, ticks = 0 }
function CM.gameTime() return 100 end
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
-- the loose on-road test that misfired: both the crossing node AND the vertex beside the
-- road count as "on" the road's edge (seq 328 at -2/-3, seq 339 at -3)
local onRoad = {
  { 5082.7148, 5222.0713, 157333 }, { 5075.9932, 5212.3188, 157333 }, { 5085.1343, 5255.4658, 253998 },
}
function CM.findEdgeContaining(isTrack, x, y)
  if isTrack then return nil end
  for _, r in ipairs(onRoad) do
    if math.abs(r[1] - x) < 0.01 and math.abs(r[2] - y) < 0.01 then return r[3], 0.5 end
  end
end
function CM.execPolyline(c, planOnly) plans[#plans + 1] = c; return nil, nil end
local log = function(s) logs[#logs + 1] = s end
assert(load(INJ_SRC, "@inject.lua"))()(CM, K, log)
local H = {}
function H.poll() CM.pollInject() end
function H.n() return #sched end
function H.arg(i, k) local s = sched[i]; return s and s.args[k] end
function H.planFv(i) local p = plans[i]; return p and p.fv end
function H.pts(i) return sched[i] and sched[i].args.pts end
function H.logs() return table.concat(logs, "\n") end
return H
''')


def fresh_set(fv):
    return set(int(t) for t in str(fv or "").split(",") if t.strip())


def vertex_of(pts, x, y):
    vals = [float(t) for t in pts.split(",")]
    for i in range(len(vals) // 3):
        if abs(vals[i * 3] - x) < 0.01 and abs(vals[i * 3 + 1] - y) < 0.01:
            return i + 1
    return None


def main():
    d = tempfile.mkdtemp()
    path = os.path.join(d, "lockstep_inject_a.txt")
    with open(path, "wb") as f:
        f.write(("ARMED 1\n" + ROADE_328 + "\nARMED 1\n" + ROADE_339 + "\n").encode())
    H = runtime(path)
    H.poll()
    check("both captures scheduled as ROADP", H.n() == 2, H.logs()[-400:])

    pts, fv = H.pts(1), H.arg(1, "fv")
    fr = fresh_set(fv)
    beside = vertex_of(pts, 5082.7148, 5222.0713)
    xing = vertex_of(pts, 5075.9932, 5212.3188)
    start = vertex_of(pts, 5163.6284, 5335.2031)
    check("seq 328: the rail vertex beside the road is fresh", beside in fr, f"fv={fv} beside={beside}")
    check("seq 328: the crossing node that split the road is NOT fresh", xing is not None and xing not in fr, f"xing={xing}")
    check("seq 328: the existing start node is not listed", start not in fr, f"start={start}")
    check("seq 328: every other new rail vertex is fresh", len(fr) == 5, f"fv={fv}")
    check("seq 328: the plan pass gets the same fv", H.planFv(1) == fv, f"{H.planFv(1)} vs {fv}")
    check("seq 328: its two road halves are still dropped", "dropped 2 split half/halves" in H.logs())

    pts2, fv2 = H.pts(2), H.arg(2, "fv")
    fr2 = fresh_set(fv2)
    xing2 = vertex_of(pts2, 5085.1343, 5255.4658)
    check("seq 339: the crossing node is NOT fresh", xing2 is not None and xing2 not in fr2, f"fv={fv2} xing={xing2}")
    check("seq 339: its other new rail vertices are", len(fr2) == 3, f"fv={fv2}")

    print()
    if fails:
        print(f"{len(fails)} FAILED")
        sys.exit(1)
    print("all passed")


if __name__ == "__main__":
    main()
