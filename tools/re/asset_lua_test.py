"""Offline test of mod/.../scripts/mp/assets.lua on Lua 5.2 (no game needed).

Stubs game.interface (asset groups with position/count), api.engine, api.cmd and
CM.scheduleLocal, then runs the real module: the originator's ASSETCAP turns
removed ids into position+count signatures, the wire keeps the fields intact,
and a peer's execAssets finds ITS OWN group ids for them (different ids, same
positions), writes the inject file and sends the carrier.

    python tools/re/asset_lua_test.py
"""
import os, sys, tempfile
import lupa.lua52 as lupa

REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
SRC = open(os.path.join(REPO, "mod", "mp_lockstep_1", "res", "scripts", "mp", "assets.lua"), encoding="utf-8").read()
NET = open(os.path.join(REPO, "mod", "mp_lockstep_1", "res", "scripts", "mp", "net.lua"), encoding="utf-8").read()
codec = "return function()\n" + NET[NET.index("local function encodeCmd"):NET.index("function CM.scheduleLocal")] + "\nreturn encodeCmd, decodeCmd\nend"

L = lupa.LuaRuntime(unpack_returned_tuples=True)
enc, dec = L.execute(codec)()
base = tempfile.mkdtemp(prefix="asset_lua_") + os.sep
L.globals().BASE = base.replace("\\", "/")
make = L.execute(r'''
return function(src, letter, groups)
  local CM, K, logs = {}, { INSTANCE = letter, BASE = BASE }, {}
  local function log(s) logs[#logs + 1] = s end
  local sched, sent = nil, {}
  CM.scheduleLocal = function(op, args)
    local c = { op = op, at = 50.2, origin = "a", seq = 3 }
    for k, v in pairs(args) do c[k] = v end
    sched = c
  end
  local myGame, myApi
  -- game/api are globals shared by both instances: every entry point below
  -- puts this instance's world back before it runs
  local function use() game, api = myGame, myApi end
  myGame = { interface = {
    getEntity = function(id) local g = groups[id]; if not g then return nil end
      return { id = id, type = "ASSET_GROUP", count = g.count, position = { g.x, g.y, g.z }, models = { ["tree/x.mdl"] = g.count } } end,
    getEntities = function(q, f)
      local out = {}
      for id, g in pairs(groups) do
        local dx, dy = g.x - q.pos[1], g.y - q.pos[2]
        if dx * dx + dy * dy <= q.radius * q.radius then out[#out + 1] = id end
      end
      return out
    end } }
  myApi = { engine ={ entityExists = function(id) return groups[id] ~= nil end, util = { getPlayer = function() return 1 end } },
          type = { Context = { new = function() return {} end }, SimpleProposal = { new = function() return {} end } },
          cmd = { make = { buildProposal = function(p, ctx) return { ctx = ctx } end },
                  sendCommand = function(cmd, cb) sent[#sent + 1] = cmd; if cb then cb(nil, true) end end } }
  assert(load(src, "@assets.lua"))()(CM, K, log)
  local H = {}
  use()
  function H.cap(w) use(); sched = nil; CM.lastArmed = 1; CM.assetCapture(w); return sched end
  function H.capUnarmed(w) use(); sched = nil; CM.lastArmed = 0; CM.assetCapture(w); return sched end
  function H.exec(c) use(); sent = {}; CM.execAssets(c); return #sent end
  function H.file() local f = io.open(BASE .. "asset_inject_" .. letter .. ".txt", "rb"); if not f then return nil end local s = f:read("*a"); f:close(); return s end
  function H.logs() return table.concat(logs, " | ") end
  return H
end
''')

fails = []
def check(name, cond, extra=""):
    print(("ok   " if cond else "FAIL ") + name + (f"  ({extra})" if extra else ""))
    if not cond:
        fails.append(name)

import base64
blob = b"TPAS" + b"\x01\x00\x00\x00" + b"\x00" * 8
b64 = base64.b64encode(blob).decode()
# A's world: groups 100 and 101; B has the SAME groups under different ids
A = L.table_from({100: L.table_from({"x": -4386.03125, "y": 11038.850585938, "z": 89.872802734375, "count": 43}),
                  101: L.table_from({"x": 10.5, "y": -20.25, "z": 3.0, "count": 2})})
B = L.table_from({7001: L.table_from({"x": -4386.03125, "y": 11038.850585938, "z": 89.872802734375, "count": 43}),
                  7002: L.table_from({"x": 10.5, "y": -20.25, "z": 3.0, "count": 2}),
                  7003: L.table_from({"x": 10.5, "y": -20.25, "z": 3.0, "count": 5})})
ha = make(SRC, "a", A)
hb = make(SRC, "b", B)

c = ha.cap(L.table_from({1: "ASSETCAP", 2: str(len(blob)), 3: b64, 4: "2", 5: "100,101"}))
check("originator schedules ASSETS for everyone (armed)", c is not None and c["op"] == "ASSETS" and c["skipOrigin"] is None, ha.logs())
check("removals travel as position+count signatures", c is not None and c["rm"] == "-4386.031,11038.851,89.873,43;10.500,-20.250,3.000,2", str(c and c["rm"]))
wire = enc(c)
c2 = dec(wire)
check("the command survives encode/decode", c2["d"] == b64 and c2["rm"] == c["rm"] and int(c2["n"]) == len(blob), wire[:120])
n = hb.exec(c2)
f = hb.file()
check("peer matches ITS OWN ids by position and count", f is not None and f.startswith("rm 7001,7002\n"), repr(f[:40] if f else f))
check("peer file carries the stroke", f is not None and f.endswith(b64), "")
check("peer sends exactly one carrier (not the originator)", n == 1, str(n))
n = ha.exec(c2)
check("originator replays too and sends the release marker", n == 2 and ha.file().startswith("rm 100,101\n"), str(n))

c = ha.cap(L.table_from({1: "ASSETCAP", 2: str(len(blob)), 3: b64, 4: "1", 5: "999"}))
check("a removed group that no longer stands: stroke not shipped", c is None and "no longer stand" in ha.logs())
c = ha.cap(L.table_from({1: "ASSETCAP", 2: str(len(blob)), 3: b64, 4: "0", 5: "-"}))
check("an add-only stroke ships with no rm field", c is not None and c["rm"] is None)
c = ha.capUnarmed(L.table_from({1: "ASSETCAP", 2: str(len(blob)), 3: b64, 4: "0", 5: "-"}))
check("not cancelled here: skipOrigin", c is not None and c["skipOrigin"] == 1)
check("the originator skips its own skipOrigin command", ha.exec(c) == 0)
c = ha.cap(L.table_from({1: "ASSETCAP", 2: "999", 3: b64, 4: "0", 5: "-"}))
check("a damaged line is refused", c is None and "damaged" in ha.logs())
print("FAILED: " + ", ".join(fails) if fails else "ALL OK")
sys.exit(1 if fails else 0)
