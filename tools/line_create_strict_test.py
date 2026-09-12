"""Offline checks for strict line creation (inject.lua LCREATEX, lines.lua execLine), on Lua 5.2.

The line editor's CreateLine used to run natively on the player's game one command delay
before the peers' replay, and the new line took a different entity id there. The slice now
decodes it (name, colour, component::Line), writes ARMED 1 + LCREATEX and cancels it; the
editor's callback is held and rides on the originator's own replay, which the Lua claims
through lockstep_lclaim_<x>.txt. This loads the real inject.lua and lines.lua into a
lupa.lua52 runtime with stub CM/K/api and drives:
  - ARMED 1 + LCREATEX (the editor's empty line): scheduled LCREATE, armed=1, name/colour kept
  - a decoded stop with an alternative platform: stops/alts strings as LUPDATE builds them
  - ARMED 0: nothing scheduled, the old read-back path takes it (pendingLineCreates)
  - a stop that is not a station group: rejected loudly, nothing scheduled
  - execLine on the originator, armed=1: claim written (fresh value each time), createLine sent
  - execLine on the originator, armed=0: skipped; on a peer: sent, no claim

    python tools/line_create_strict_test.py
"""
import os
import sys
import tempfile

import lupa.lua52 as lupa

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
MP = os.path.join(REPO, "mod", "mp_lockstep_1", "res", "scripts", "mp")

fails = []


def check(name, cond, extra=""):
    print(("ok   " if cond else "FAIL ") + name + (f"  ({extra})" if extra else ""))
    if not cond:
        fails.append(name)


def runtime(inject_path, base):
    L = lupa.LuaRuntime(unpack_returned_tuples=True)
    g = L.globals()
    g.INJECT_SRC = open(os.path.join(MP, "inject.lua"), encoding="utf-8").read()
    g.LINES_SRC = open(os.path.join(MP, "lines.lua"), encoding="utf-8").read()
    g.INJECT = inject_path.replace("\\", "/")
    g.BASE = base.replace("\\", "/") + "/"
    return L.execute(r'''
local logs, sched, sent = {}, {}, {}
local function sink()
  return setmetatable({}, { __index = function() return sink() end, __call = function() return nil end })
end
api = setmetatable({}, { __index = function() return sink() end })
api.type = setmetatable({
  Line = { new = function() return { stops = {} } end, Stop = { new = function() return {} end } },
  Vec3f = { new = function(r, g, b) return { r, g, b } end },
  ComponentType = { STATION_GROUP = 1 },
}, { __index = function() return sink() end })
api.engine = setmetatable({ util = { getPlayer = function() return 1 end },
                            system = { lineSystem = { getLines = function() return {} end } } },
  { __index = function() return sink() end })
api.cmd = {
  make = { createLine = function(name, color, player, line)
    return { what = "createLine", name = name, color = color, n = #line.stops } end },
  sendCommand = function(cmd, cb) sent[#sent + 1] = cmd end,
}
game = setmetatable({}, { __index = function() return sink() end })
local K = setmetatable({ INSTANCE = "a", PEER = "b", INJECT_FILE = INJECT, BASE = BASE,
                         STRICT_OPS = { LCREATE = true, LUPDATE = true, LDELETE = true } },
  { __index = function() return nil end })
local CM = { peerSeen = true, injectOffset = 0, seqNo = 0, ticks = 0 }
function CM.gameTime() return 100 end
function CM.stepOf(t) return math.floor((t or 0) / 0.2 + 0.5) end
function CM.scheduleLocal(op, args) sched[#sched + 1] = { op = op, args = args } end
function CM.unescName(s)
  return (tostring(s or ""):gsub("%%(%x%x)", function(h) return string.char(tonumber(h, 16)) end))
end
function CM.readFrom(path, offset)
  local f = io.open(path, "rb")
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
local log = function(s) logs[#logs + 1] = s end
assert(load(INJECT_SRC, "@inject.lua"))()(CM, K, log)
assert(load(LINES_SRC, "@lines.lua"))()(CM, K, log)
-- station group 5000 sits at 100,200; its station 1 at 101.5,202.5
function CM.stationGroupPos(sg) if sg == 5000 then return 100, 200 end end
function CM.stationPosInGroup(sg, st) if sg == 5000 and st == 1 then return 101.5, 202.5 end end
local H = { CM = CM }
function H.poll() CM.pollInject() end
function H.nsched() return #sched end
function H.op(i) local s = sched[i]; return s and s.op end
function H.arg(i, k) local s = sched[i]; return s and s.args[k] end
function H.clearSched() sched = {} end
function H.npending() return #CM.pendingLineCreates end
function H.clearPending() CM.pendingLineCreates = {} end
function H.nsent() return #sent end
function H.sentName(i) local c = sent[i]; return c and c.name end
function H.clearSent() sent = {} end
function H.logs() return table.concat(logs, "\n") end
function H.exec(origin, armed, seq)
  CM.execLine({ op = "LCREATE", origin = origin, seq = seq, at = 100, armed = armed,
                name = "Bus%20Line%201", color = "0.1,0.2,0.3", wait = 180, stops = "", alts = "" })
end
return H
''')


def main():
    d = tempfile.mkdtemp()
    path = os.path.join(d, "lockstep_inject_a.txt")
    claim = os.path.join(d, "lockstep_lclaim_a.txt")
    open(path, "wb").close()
    H = runtime(path, d)

    def write(*lines):
        with open(path, "ab") as f:
            for ln in lines:
                f.write((ln + "\n").encode())

    # 1. the editor's create: an empty line, a name with a space
    write("ARMED 1", "LCREATEX 0.1000 0.2000 0.3000 180 0 name=Bus%20Line%201")
    H.poll()
    check("empty create: one LCREATE scheduled", H.nsched() == 1 and H.op(1) == "LCREATE", H.logs()[-300:])
    check("empty create: armed=1", H.arg(1, "armed") == 1)
    check("empty create: name stays escaped for the wire", H.arg(1, "name") == "Bus%20Line%201", str(H.arg(1, "name")))
    check("empty create: colour and wait", H.arg(1, "color") == "0.1000,0.2000,0.3000" and H.arg(1, "wait") == 180,
          f"{H.arg(1, 'color')} {H.arg(1, 'wait')}")
    check("empty create: no stops", H.arg(1, "stops") == "" and H.arg(1, "alts") == "")
    check("empty create: the read-back path is not used", H.npending() == 0)
    H.clearSched()

    # 2. a decoded stop with one alternative platform
    write("ARMED 1", "LCREATEX 0.5000 0.5000 0.5000 120 1 5000 1 2 0 0 180 1 1 3 name=L2")
    H.poll()
    check("one stop: scheduled", H.nsched() == 1)
    check("one stop: stops string as LUPDATE builds it",
          H.arg(1, "stops") == "100.00,200.00,1,2,0,0,180,101.5,202.5", str(H.arg(1, "stops")))
    check("one stop: alternatives aligned", H.arg(1, "alts") == "1:3", str(H.arg(1, "alts")))
    H.clearSched()

    # 3. not cancelled (no live session): the old read-back path
    write("ARMED 0", "LCREATEX 0.1000 0.2000 0.3000 180 0 name=L3")
    H.poll()
    check("ARMED 0: nothing scheduled", H.nsched() == 0)
    check("ARMED 0: waits to read the native line back", H.npending() == 1)
    H.clearPending()

    # 4. a stop that is not a station group
    write("ARMED 1", "LCREATEX 0.1000 0.2000 0.3000 180 1 777 0 0 0 0 180 0 name=L4")
    H.poll()
    check("bad stop: nothing scheduled", H.nsched() == 0 and H.npending() == 0)
    check("bad stop: says the create is lost", "REJECTED" in H.logs() and "LOST" in H.logs())

    # 5. the originator's replay claims its createLine
    H.exec("a", 1, 11)
    check("originator armed=1: createLine sent", H.nsent() == 1 and H.sentName(1) == "Bus Line 1", str(H.sentName(1)))
    c1 = open(claim).read().strip() if os.path.exists(claim) else ""
    check("originator armed=1: claim written", c1.isdigit() and int(c1) > 0, c1)
    H.exec("a", 1, 12)
    c2 = open(claim).read().strip() if os.path.exists(claim) else ""
    check("second claim differs from the first", c2.isdigit() and c2 != c1, f"{c1} -> {c2}")
    check("originator: logged as created at the stamp", "created at the stamp here too" in H.logs())
    H.clearSent()

    # 6. armed=0 on the originator is the old native path: skipped
    H.exec("a", 0, 13)
    check("originator armed=0: skipped", H.nsent() == 0)

    # 7. a peer's line: sent, no claim
    H.exec("b", 1, 14)
    c3 = open(claim).read().strip()
    check("peer line: createLine sent", H.nsent() == 1)
    check("peer line: claim untouched", c3 == c2, f"{c2} -> {c3}")

    print()
    if fails:
        print(f"{len(fails)} FAILED")
        sys.exit(1)
    print("all passed")


if __name__ == "__main__":
    main()
