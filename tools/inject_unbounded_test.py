"""Offline checks that the inject reader takes a capture at ANY size (inject.lua), on Lua 5.2.

The slice (native/src/slice_hook.cpp) used to cut what it shipped at fixed sizes: 64 parts
per bought or replaced vehicle, 256 sold vehicles, 16 demolished constructions, 512 road
edges, 255 characters of a line or vehicle name. Each cut let the player's command run
natively on one instance and nowhere else. The slice now writes every record whole, so
the Lua side must parse them whole: this loads the real mod/.../scripts/mp/inject.lua
into a lupa.lua52 runtime with stub CM/K/api and feeds it captures at sizes past every
old cut, exactly as the slice's writers format them:
  - VBUY with 65 parts, 300 vehicle groups: one VBUY scheduled carrying all 65 parts
  - VREPL with 65 parts: the same parser, all 65
  - VSELL with 600 ids: every id reaches CM.deferVehCap
  - CDEMO with 40 ids: 40 DEMOLISH scheduled
  - LCREATEX whose name is 3,000 characters percent-encoded: the LCREATE carries it whole
  - VNAME with a 1,000-character name: the VNAME carries it whole
  - ROADE with 701 new nodes and 700 edges (plus the bridge tail): a ROADP is scheduled
    with every node in it

    python tools/inject_unbounded_test.py
"""
import os
import sys
import tempfile

import lupa.lua52 as lupa

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
INJ = os.path.join(REPO, "mod", "mp_lockstep_1", "res", "scripts", "mp", "inject.lua")

fails = []


def check(name, cond, extra=""):
    print(("ok   " if cond else "FAIL ") + name + (f"  ({extra})" if extra else ""))
    if not cond:
        fails.append(name)


def percent_encode(s):
    # slice_hook.cpp PercentEncode: printable ASCII other than '%' and '=' as is, else %XX
    out = []
    for ch in s.encode("utf-8"):
        if 32 < ch < 127 and ch not in (0x25, 0x3D):
            out.append(chr(ch))
        else:
            out.append("%%%02X" % ch)
    return "".join(out)


def run(*lines):
    d = tempfile.mkdtemp()
    path = os.path.join(d, "lockstep_inject_a.txt")
    with open(path, "wb") as f:
        for ln in lines:
            f.write((ln + "\n").encode())
    L = lupa.LuaRuntime(unpack_returned_tuples=True)
    L.globals().INJ_SRC = open(INJ, encoding="utf-8").read()
    L.globals().INJECT = path.replace("\\", "/")
    return L.execute(r'''
local logs, sched, sold = {}, {}, {}
local function sink()
  return setmetatable({}, { __index = function() return sink() end, __call = function() return nil end })
end
local CT = { CONSTRUCTION = 1, BASE_EDGE = 2, BASE_NODE = 3 }
local function con(id)
  local tr = {}; for i = 1, 16 do tr[i] = 0 end; tr[13], tr[14] = 1000 + id, 2000 + id
  return { depots = { 241897 }, transf = tr, fileName = "depot/road_depot_era_a.con" }
end
api = setmetatable({
  type = { ComponentType = CT },
  res = { modelRep = { getName = function(id) return "vehicle/train/wagon_" .. tostring(id) .. ".mdl" end } },
  engine = setmetatable({
    entityExists = function(id) return id >= 5000 and id < 6000 end,
    getComponent = function(id, t)
      if t == CT.CONSTRUCTION and (id == 900 or (id >= 5000 and id < 6000)) then return con(id) end
    end,
  }, { __index = function() return sink() end }),
}, { __index = function() return sink() end })
game = setmetatable({}, { __index = function() return sink() end })
local K = setmetatable({ INSTANCE = "a", PEER = "b", INJECT_FILE = INJECT,
                         STRICT_OPS = { VBUY = true, VREPL = true, VSELL = true } },
  { __index = function() return nil end })
local CM = { peerSeen = true, injectOffset = 0, consByKey = { d = { id = 900 } }, seqNo = 0, ticks = 0,
             vehKeyOf = { [777] = "a:5", [4242] = "a:9" }, lineKeyOf = {}, primedLines = {}, primedVeh = {},
             pendingLineCreates = {} }
function CM.gameTime() return 100 end
function CM.stepOf(t) return math.floor((t or 0) / 0.2 + 0.5) end
function CM.scheduleLocal(op, args) sched[#sched + 1] = { op = op, args = args } end
function CM.vehKeyFor(vid) return CM.vehKeyOf[vid] end
function CM.lineKeyFor(lid) return nil end
function CM.deferVehCap(entry) sold[#sold + 1] = entry end
function CM.autoLoadFlags(words, n) return words end
function CM.groundAt(x, y) return 0 end
function CM.conKey(x, y) return string.format("%.1f/%.1f", x, y) end
function CM.stationGroupPos(sg) if sg > 0 then return 100 + sg, 200 + sg end end
function CM.stationPosInGroup(sg, idx) return nil end
function CM.unescName(s) return (s:gsub("%%(%x%x)", function(h) return string.char(tonumber(h, 16)) end)) end
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
function CM.netMap(track) return {} end
function CM.findEdgeContaining(isTrack, x, y) return nil end
function CM.edgeZAt(eid, u) return nil end
function CM.execPolyline(c, planOnly) return nil, nil end
local log = function(s) logs[#logs + 1] = s end
assert(load(INJ_SRC, "@inject.lua"))()(CM, K, log)
CM.pollInject()
CM.pollInject()   -- a VBUY that ends a read waits one poll for its VBUYLINE
local H = {}
function H.nsched() return #sched end
function H.op(i) local s = sched[i]; return s and s.op end
function H.arg(i, k) local s = sched[i]; return s and s.args[k] end
function H.nsold() return #sold end
function H.soldIds(i) return sold[i] and #sold[i].ids or -1 end
function H.logs() return table.concat(logs, "\n") end
return H
''')


def vehicle_config(nparts, ngroups):
    # WriteVehicleConfig: <n> { <model> <nLoad> <load..> <r> <g> <b> <nAuto> <auto..> }* <ng> <group..>
    parts = []
    for k in range(nparts):
        parts.append(f"{3000 + k} 3 0 1 2 -1.0000 -1.0000 -1.0000 2 1 0")
    groups = " ".join(str(g) for g in range(ngroups))
    return f"{nparts} " + " ".join(parts) + f" {ngroups}" + (" " + groups if ngroups else "")


def main():
    # 1. VBUY: a 65-wagon train (the old cut was 64 parts, 256 ints per vector)
    H = run("ARMED 1", "VBUY 241897 " + vehicle_config(65, 300))
    check("VBUY 65 parts: one VBUY scheduled", H.nsched() == 1 and H.op(1) == "VBUY", H.logs()[-300:])
    parts = (H.arg(1, "parts") or "").split(";")
    check("VBUY 65 parts: all 65 parts on the wire", len(parts) == 65 and parts[64].startswith("vehicle/train/wagon_3064.mdl"),
          f"{len(parts)} parts, last={parts[-1][:50]}")
    groups = (H.arg(1, "groups") or "").split("/")
    check("VBUY 65 parts: all 300 vehicle groups on the wire", len(groups) == 300, f"{len(groups)} groups")

    # 2. VREPL: the same config on a replace
    H = run("ARMED 1", "VREPL 777 " + vehicle_config(65, 0))
    check("VREPL 65 parts: one VREPL scheduled", H.nsched() == 1 and H.op(1) == "VREPL", H.logs()[-300:])
    check("VREPL 65 parts: all 65 parts on the wire", len((H.arg(1, "parts") or "").split(";")) == 65)

    # 3. VSELL: a fleet of 600 (the old cut was 256 ids)
    ids = list(range(10000, 10600))
    H = run("ARMED 1", f"VSELL {len(ids)} " + " ".join(str(i) for i in ids))
    check("VSELL 600 ids: deferred once with every id", H.nsold() == 1 and H.soldIds(1) == 600, f"n={H.nsold()} ids={H.soldIds(1)}")

    # 4. CDEMO: 40 constructions in one bulldoze (the old cut was 16)
    cids = list(range(5000, 5040))
    H = run("ARMED 1", f"CDEMO {len(cids)} " + " ".join(str(i) for i in cids))
    check("CDEMO 40 ids: 40 DEMOLISH scheduled", H.nsched() == 40 and all(H.op(i + 1) == "DEMOLISH" for i in range(40)),
          f"{H.nsched()} scheduled; " + H.logs()[-200:])
    check("CDEMO 40 ids: the last one is the 40th construction", H.arg(40, "x") == 1000 + 5039, str(H.arg(40, "x")))

    # 5. LCREATEX: a 3,000-character name (the old cut was 255 raw / 767 encoded)
    name = ("Coal Line " + "é" * 5 + "=%") * 250   # 3,000 bytes with spaces, UTF-8, '=' and '%'
    enc = percent_encode(name)
    H = run("ARMED 1", f"LCREATEX 0.5 0.25 1 180 1 12 0 0 0 0 180 0 name={enc}")
    check("LCREATEX long name: one LCREATE scheduled", H.nsched() == 1 and H.op(1) == "LCREATE", H.logs()[-300:])
    check("LCREATEX long name: the whole encoded name travels", H.arg(1, "name") == enc,
          f"{len(H.arg(1, 'name') or '')} of {len(enc)} chars")

    # 6. VNAME: a 1,000-character vehicle name
    vname = ("Express " + "ü" * 3) * 90   # over 1,000 bytes
    venc = percent_encode(vname)
    H = run("ARMED 1", f"VNAME 4242 {venc}")
    check("VNAME long name: one VNAME scheduled", H.nsched() == 1 and H.op(1) == "VNAME", H.logs()[-300:])
    check("VNAME long name: the whole encoded name travels", H.arg(1, "name") == venc,
          f"{len(H.arg(1, 'name') or '')} of {len(venc)} chars")

    # 7. ROADE: 701 new nodes, 700 edges, bridge tail (the old arrays held 256 / 512)
    n, m = 701, 700
    nodes = " ".join(f"{-(i + 1)} {100.0 + i * 10:.4f} {200.0:.4f} {5.0:.4f}" for i in range(n))
    edges = " ".join(f"{-(i + 1)} {-(i + 2)} 10.0000 0.0000 0.0000 10.0000 0.0000 0.0000" for i in range(m))
    tail = " ".join("0 -1" for _ in range(m))
    H = run("ARMED 1", "STREETP 0 0", f"ROADE {n} 0 12 1 0 {m} 0 0 {nodes} {edges} {tail}")
    check("ROADE 700 edges: a ROADP is scheduled", H.nsched() == 1 and H.op(1) == "ROADP", H.logs()[-400:])
    pts = str(H.arg(1, "pts") or "")
    check("ROADE 700 edges: every node travels (x,y,z each)", pts.count(",") == n * 3 - 1 and pts.endswith("7100.0000,200.0000,5.0000"),
          f"{pts.count(',') + 1} numbers, tail={pts[-40:]}")

    print()
    if fails:
        print(f"{len(fails)} FAILED")
        sys.exit(1)
    print("all passed")


if __name__ == "__main__":
    main()
