"""Offline check (Lua 5.2, real inject.lua): a captured edit of a FOREIGN entity is not shipped.

Read-only foreign windows (slice foreignwindows) let a player click another company's
vehicle/station window, whose native controls have no owner gate. inject.lua's capture
must refuse to replicate an edit of a foreign entity, on the originator, so the control
is a no-op. This drives the real CM.pollInject over VNAME/VCOLOR/VREV/VDEPOT/VLINE/VSELL/
LUPDATE lines and checks: a foreign target ships nothing; an own target ships as before;
a VSELL batch drops only the foreign ids.

    python tools/foreign_edit_guard_test.py
"""
import os
import sys
import tempfile

import lupa.lua52 as lupa

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
INJECT = os.path.join(REPO, "mod", "mp_lockstep_1", "res", "scripts", "mp", "inject.lua")

fails = []


def check(name, cond, extra=""):
    print(("ok   " if cond else "FAIL ") + name + (f"  ({extra})" if extra else ""))
    if not cond:
        fails.append(name)


BASE = tempfile.mkdtemp().replace("\\", "/") + "/"
INJ = BASE + "lockstep_inject_a.txt"
open(INJ, "w").close()

L = lupa.LuaRuntime(unpack_returned_tuples=True)
L.globals().SRC = open(INJECT, encoding="utf-8").read()
L.globals().INJ = INJ
H = L.execute(r'''
local sched = {}
local function sink() return setmetatable({}, { __index = function() return sink() end, __call = function() return sink() end }) end
api = sink(); game = sink()
local K = setmetatable({ INSTANCE = "a", BASE = "", INJECT_FILE = INJ, STRICT_OPS = {} }, { __index = function() return nil end })
-- fields pollInject reads AS VALUES must be nil/tables, not the method sink
local NILF = { injectCarry = true, actionsOff = true, injectNext = true, lastArmed = true, dropNextCmd = true,
               geomDepth = true, actionsBlockTick = true, lclaimSeq = true, injForeignSaid = true }
local CM = setmetatable({ ticks = 0, injectOffset = 0, peerSeen = true, cmMode = "companies", cmMyCompany = 1,
                          actionsHeld = {}, injectCarry = nil, actionsOff = false, lastArmed = 0 },
  { __index = function(_, k) if NILF[k] then return nil end return function() return nil end end })
local log = function(s) end
assert(load(SRC, "@inject.lua"))()(CM, K, log)
-- vehicles/lines the capture resolves to keys
local vkey = { [500] = "a:5", [600] = "b:9", [510] = "a:6" }   -- 500/510 ours, 600 foreign
local lkey = { [700] = "a:1", [800] = "b:2" }                  -- 700 ours, 800 foreign
CM.vehKeyFor = function(id) return vkey[id] end
CM.lineKeyFor = function(id) return lkey[id] end
CM.vehKeyOf = { [500] = true, [510] = true, [600] = true }
CM.lineKeyOf = { [700] = true }; CM.primedLines = {}; CM.pollLineKeys = function() end
CM.takeColorEcho = function() return false end
CM.gameTime = function() return 100 end
CM.readFrom = function(path, offset)
  local f = io.open(path, "r"); if not f then return nil, offset end
  local size = f:seek("end"); if offset >= size then f:close(); return nil, offset end
  f:seek("set", offset); local data = f:read("*a") or ""; f:close()
  local last = #data; while last > 0 and data:byte(last) ~= 10 do last = last - 1 end
  if last == 0 then return nil, offset end
  data = data:sub(1, last); return data, offset + #data
end
CM.waitNum = function(v, d) return tonumber(v) or d end
CM.deferVehCap = function(c) sched[#sched + 1] = { op = c.kind, c = c } end
CM.scheduleLocal = function(op, args) sched[#sched + 1] = { op = op, args = args } end
-- ownership: player 7 = company 1 (us), player 8 = company 2 (them)
local owner = { [500] = 7, [510] = 7, [600] = 8, [700] = 7, [800] = 8 }
CM.cmForeignOwner = function(eid)
  if CM.cmMode ~= "companies" then return false end
  local o = owner[eid]; if not o or o == 7 then return false end
  return true, 2, o
end
CM.cmEnsure = function() end
local H = { CM = CM }
function H.feed(line) local f = io.open(INJ, "a"); f:write(line .. "\n"); f:close(); CM.pollInject() end
function H.ops() local t = {} for i, s in ipairs(sched) do t[i] = s.op end return table.concat(t, ",") end
function H.last() return sched[#sched] end
function H.clear() for k in pairs(sched) do sched[k] = nil end end
function H.count() return #sched end
return H
''')

CM = H.CM

# own vehicle: ships
H.feed("VNAME 500 Bus%20One")
check("own VNAME ships", H.ops() == "VNAME", H.ops())
H.clear()
# foreign vehicle: nothing
H.feed("VNAME 600 Theirs")
check("foreign VNAME is not shipped", H.count() == 0)
H.clear()
H.feed("VCOLOR 600 0.1 0.2 0.3")
check("foreign VCOLOR is not shipped", H.count() == 0)
H.clear()
H.feed("VCOLOR 500 0.1 0.2 0.3")
check("own VCOLOR ships", H.ops() == "VCOLOR", H.ops())
H.clear()
H.feed("VREV 600")
check("foreign VREV is not shipped", H.count() == 0)
H.feed("VREV 500")
check("own VREV ships", H.ops() == "VREV", H.ops())
H.clear()
H.feed("VDEPOT 600 1")
check("foreign VDEPOT is not shipped", H.count() == 0)
H.feed("VDEPOT 500 1")
check("own VDEPOT ships", H.ops() == "VDEPOT", H.ops())
H.clear()
H.feed("VLINE 600 12 0")
check("foreign VLINE is not shipped", H.count() == 0)
H.feed("VLINE 500 12 0")
check("own VLINE is deferred (ships)", H.ops() == "VLINE", H.ops())
H.clear()
# VSELL batch: own + foreign -> only own kept
H.feed("VSELL 2 500 600")
last = H.last()
ids = last and last.c and last.c.ids
kept = [ids[i] for i in range(1, len(ids) + 1)] if ids else []
check("VSELL drops the foreign id, keeps the own one", kept == [500], str(kept))
H.clear()
H.feed("LUPDATE 800 180 0")
check("foreign LUPDATE is not shipped", H.count() == 0)
H.feed("LUPDATE 700 180 0")
check("own LUPDATE ships", H.count() >= 1, H.ops())

# coop mode: nothing is foreign, everything ships
CM.cmMode = "coop"
H.clear()
H.feed("VNAME 600 X")
check("coop: even another player's VNAME ships (no companies -> no foreign)", H.count() == 1)

print("FAILED: " + ", ".join(fails) if fails else "ALL PASS")
sys.exit(1 if fails else 0)
