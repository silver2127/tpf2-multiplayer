"""A bought vehicle's turned parts stay turned on every instance (Lua 5.2, offline).

The slice writes each part's reversed flag (VehiclePart+0x04, from the member
offsets RegisterUsertypesVehicle hands the Lua usertype) right behind the model
id; inject.lua carries it to the peers as the part spec's fifth field; and
buildVehConfig (vehicles.lua) sets part.reversed from it. Before, the flag was
never decoded, so a train bought with a turned car (an ICE's tail head, a cab
car) came out facing forward on every instance, the originator's included
(2026-09-20).

  1. inject.lua: a VBUY with parts 0/1/0 turned -> the scheduled VBUY's part
     specs end in ~0, ~1, ~0; a VREPL the same
  2. buildVehConfig: those specs give VehicleParts with reversed false/true/false;
     a four-field spec (no flag) is a forward part; a malformed spec still raises

    python tools/vehicle_reversed_test.py
"""
import os
import sys
import tempfile

import lupa.lua52 as lupa

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
INJ = os.path.join(REPO, "mod", "mp_lockstep_1", "res", "scripts", "mp", "inject.lua")
VEH = os.path.join(REPO, "mod", "mp_lockstep_1", "res", "scripts", "mp", "vehicles.lua")

fails = []


def check(name, cond, extra=""):
    print(("ok   " if cond else "FAIL ") + name + (f"  ({extra})" if extra else ""))
    if not cond:
        fails.append(name)


# WriteVehicleConfig: <n> { <model> <rev> <nLoad> <load..> <r> <g> <b> <nAuto> <auto..> }* <ng> <group..>
def vehicle_config(revs):
    parts = [f"{3000 + k} {rev} 1 0 -1.0000 -1.0000 -1.0000 1 1" for k, rev in enumerate(revs)]
    return f"{len(revs)} " + " ".join(parts) + " 0"


def run_inject(*lines):
    d = tempfile.mkdtemp()
    path = os.path.join(d, "lockstep_inject_a.txt")
    with open(path, "wb") as f:
        for ln in lines:
            f.write((ln + "\n").encode())
    L = lupa.LuaRuntime(unpack_returned_tuples=True)
    L.globals().INJ_SRC = open(INJ, encoding="utf-8").read()
    L.globals().INJECT = path.replace("\\", "/")
    return L.execute(r'''
local logs, sched = {}, {}
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
    entityExists = function(id) return id == 900 end,
    getComponent = function(id, t) if t == CT.CONSTRUCTION and id == 900 then return con(id) end end,
  }, { __index = function() return sink() end }),
}, { __index = function() return sink() end })
game = setmetatable({}, { __index = function() return sink() end })
local K = setmetatable({ INSTANCE = "a", PEER = "b", INJECT_FILE = INJECT,
                         STRICT_OPS = { VBUY = true, VREPL = true } },
  { __index = function() return nil end })
local CM = { peerSeen = true, injectOffset = 0, consByKey = { d = { id = 900 } }, seqNo = 0, ticks = 0,
             vehKeyOf = { [777] = "a:5" }, lineKeyOf = {}, primedLines = {}, primedVeh = {}, pendingLineCreates = {} }
function CM.gameTime() return 100 end
function CM.stepOf(t) return math.floor((t or 0) / 0.2 + 0.5) end
function CM.scheduleLocal(op, args) sched[#sched + 1] = { op = op, args = args } end
function CM.vehKeyFor(vid) return CM.vehKeyOf[vid] end
function CM.lineKeyFor(lid) return nil end
function CM.autoLoadFlags(words, n) return words end
function CM.conKey(x, y) return string.format("%.1f/%.1f", x, y) end
function CM.unescName(s) return s end
function CM.readFrom(p, offset)
  local f = io.open(p, "rb")
  if not f then return nil, offset end
  local size = f:seek("end")
  if offset >= size then f:close(); return nil, offset end
  f:seek("set", offset)
  local data = f:read("*a") or ""
  f:close()
  return data, offset + #data
end
local log = function(s) logs[#logs + 1] = s end
assert(load(INJ_SRC, "@inject.lua"))()(CM, K, log)
CM.pollInject()
CM.pollInject()
local H = {}
function H.nsched() return #sched end
function H.op(i) local s = sched[i]; return s and s.op end
function H.arg(i, k) local s = sched[i]; return s and s.args[k] end
function H.logs() return table.concat(logs, "\n") end
return H
''')


def run_build(parts):
    L = lupa.LuaRuntime(unpack_returned_tuples=True)
    L.globals().VEH_SRC = open(VEH, encoding="utf-8").read()
    L.globals().PARTS = parts
    return L.execute(r'''
local function vec() return setmetatable({}, { __index = function() return nil end }) end
api = {
  type = {
    ComponentType = { CONSTRUCTION = 1 },
    TransportVehicleConfig = { new = function() return { vehicles = {}, vehicleGroups = vec() } end },
    VehiclePart = { new = function() return { loadConfig = vec() } end },
    TransportVehiclePart = { new = function() return { autoLoadConfig = vec() } end },
    Vec3f = { new = function(r, g, b) return { r, g, b } end },
  },
  res = { modelRep = { find = function(name) return name:match("wagon_(%d+)") and tonumber(name:match("wagon_(%d+)")) or -1 end } },
  engine = { util = { getPlayer = function() return 1 end } },
  cmd = { make = {}, sendCommand = function() end },
}
game = { interface = {} }
local CM = { consByKey = {}, ticks = 10 }
local K = { INSTANCE = "a", STRICT_OPS = { VBUY = true } }
function CM.autoLoadFlags(words, n) return words end
assert(load(VEH_SRC, "@vehicles.lua"))()(CM, K, function() end)
local ok, cfg = pcall(buildVehConfig, { parts = PARTS, at = 100 })
if not ok then return { err = tostring(cfg) } end
local out = {}
for i, tvp in ipairs(cfg.vehicles) do out[i] = tvp.part.reversed and 1 or 0 end
return { revs = table.concat(out, ",") }
''')


def main():
    print("== inject.lua carries the flag")
    H = run_inject("ARMED 1", "VBUY 241897 " + vehicle_config([0, 1, 0]), "VBUYLINE -1")
    check("one VBUY scheduled", H.nsched() == 1 and H.op(1) == "VBUY", H.logs()[-300:])
    parts = (H.arg(1, "parts") or "").split(";")
    check("three parts, the turned one flagged", [p.split("~")[-1] for p in parts] == ["0", "1", "0"], str(parts))
    check("model names still lead the spec", parts[1].startswith("vehicle/train/wagon_3001.mdl~"), parts[1])
    H = run_inject("ARMED 1", "VREPL 777 " + vehicle_config([1, 1]))
    check("VREPL: the same parser, both turned", H.op(1) == "VREPL"
          and [p.split("~")[-1] for p in (H.arg(1, "parts") or "").split(";")] == ["1", "1"], H.logs()[-300:])
    H = run_inject("ARMED 1", "VBUY 241897 1 3000 2 1 0 -1.0000 -1.0000 -1.0000 1 1 0")
    check("any non-zero flag reads as turned", H.nsched() == 1 and (H.arg(1, "parts") or "").endswith("~1"), H.logs()[-300:])

    print("== buildVehConfig sets part.reversed")
    r = run_build("vehicle/train/wagon_1.mdl~0~-1,-1,-1~1~0;vehicle/train/wagon_2.mdl~0~-1,-1,-1~1~1;vehicle/train/wagon_3.mdl~0~-1,-1,-1~1~0")
    check("false / true / false from the specs", r["revs"] == "0,1,0", str(dict(r)))
    r = run_build("vehicle/train/wagon_1.mdl~0~-1,-1,-1~1")
    check("a four-field spec is a forward part", r["revs"] == "0", str(dict(r)))
    r = run_build("vehicle/train/wagon_1.mdl~0~-1,-1,-1")
    check("a malformed spec still raises", r["err"] is not None and "bad part spec" in r["err"], str(dict(r)))

    print()
    if fails:
        print(f"{len(fails)} FAILED")
        for f in fails:
            print("  - " + f)
        sys.exit(1)
    print("all passed")


if __name__ == "__main__":
    main()
