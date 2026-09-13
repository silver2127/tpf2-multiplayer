"""Offline checks for quick line-editor clicks (inject.lua LUPDATE merge, lines.lua CM.lineBaseFor), on Lua 5.2.

A quick click in the line editor is built from the list the EDITOR last saw. With strict
line updates an earlier click can apply between the click and the moment the Lua reads
it, so diffing the click against the line's current list read "stop 1 removed, stop 3
added" for a click that only added stop 3 -- the merge deleted stop 1 (b:6, 2026-09-12:
three quick stations applied as 1, 2, 2 stops on every instance).

This loads the real inject.lua and lines.lua with stub CM/api and replays that session's
shape: each click is a decoded LUPDATE built from the editor's (possibly stale) list, and
an update "applies" the way execLine records it (the list before and after it).
  - click 2 while click 1 waits: both stops
  - click 3 built from the EMPTY list after click 1 applied: all three stops, none lost
  - click 4 built from the one-stop list: four stops
  - removing the stop just added (nothing waiting): a removal, not a no-op

    python tools/line_rapid_edit_test.py
"""
import os
import sys
import tempfile

import lupa.lua52 as lupa

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
MP = os.path.join(REPO, "mod", "mp_lockstep_1", "res", "scripts", "mp")
LID = 108664
POS = {107158: 10, 107161: 20, 107164: 30, 107167: 40}

fails = []


def check(name, cond, extra=""):
    print(("ok   " if cond else "FAIL ") + name + (f"  ({extra})" if extra else ""))
    if not cond:
        fails.append(name)


def stop_str(sg):
    return f"{POS[sg]:.2f},0.00,0,0,0,0,180"


def click_line(sgs):
    parts = ["LUPDATE", str(LID), "180", str(len(sgs))]
    for sg in sgs:
        parts += [str(sg), "0", "0", "0", "0", "180", "0"]
    return " ".join(parts)


def runtime(inject_path):
    L = lupa.LuaRuntime(unpack_returned_tuples=True)
    g = L.globals()
    g.INJECT_SRC = open(os.path.join(MP, "inject.lua"), encoding="utf-8").read()
    g.LINES_SRC = open(os.path.join(MP, "lines.lua"), encoding="utf-8").read()
    g.INJECT = inject_path.replace("\\", "/")
    return L.execute(r'''
local logs, sched = {}, {}
local function sink()
  return setmetatable({}, { __index = function() return sink() end, __call = function() return nil end })
end
api = setmetatable({}, { __index = function() return sink() end })
game = setmetatable({}, { __index = function() return sink() end })
local K = setmetatable({ INSTANCE = "b", INJECT_FILE = INJECT, BASE = "",
                         STRICT_OPS = { LCREATE = true, LUPDATE = true, LDELETE = true } },
  { __index = function() return nil end })
local CM = { peerSeen = true, injectOffset = 0, seqNo = 6, ticks = 0, queue = {} }
local now = 100
function CM.gameTime() return now end
function CM.stepOf(t) return math.floor((t or 0) / 0.2 + 0.5) end
function CM.scheduleLocal(op, args)
  CM.seqNo = CM.seqNo + 1
  args.op, args.seq, args.at = op, CM.seqNo, now + 0.8
  sched[#sched + 1] = args
  CM.queue[#CM.queue + 1] = args
end
function CM.escName(s) return s end
function CM.unescName(s) return s end
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
-- the line as the engine holds it
local entity = ""
CM.lineKeyOf[108664] = "b:6"
function CM.lineKeyFor(lid) return lid == 108664 and "b:6" or nil end
function CM.lineIdFor(k) return k == "b:6" and 108664 or nil end
function CM.lineSnapshot(lid) return { name = "Line%201", color = "0.9,0.2,0.2", wait = 180, stops = entity, alts = "" } end
local P = { [107158] = 10, [107161] = 20, [107164] = 30, [107167] = 40 }
function CM.stationGroupPos(sg) if P[sg] then return P[sg], 0 end end
function CM.stationPosInGroup() return nil end
local H = {}
function H.poll() CM.pollInject() end
-- apply the oldest waiting update the way execLine records it: the list before, then after
function H.applyNext()
  local c = table.remove(CM.queue, 1)
  if not c then return nil end
  CM.lineHistNote(c.key, entity, "")
  CM.lineHistNote(c.key, c.stops or "", c.alts or "")
  entity = c.stops or ""
  now = now + 0.4
  return c.seq
end
function H.lastStops() local s = sched[#sched]; return s and s.stops end
function H.nsched() return #sched end
function H.entity() return entity end
function H.logs() return table.concat(logs, "\n") end
return H
''')


def main():
    d = tempfile.mkdtemp()
    path = os.path.join(d, "lockstep_inject_b.txt")
    open(path, "wb").close()
    H = runtime(path)

    def click(sgs):
        with open(path, "ab") as f:
            f.write(("ARMED 1\n" + click_line(sgs) + "\n").encode())
        H.poll()

    s158, s161, s164, s167 = (stop_str(x) for x in (107158, 107161, 107164, 107167))

    # 1. first station
    click([107158])
    check("click 1: one stop scheduled", H.lastStops() == s158, str(H.lastStops()))

    # 2. second station while click 1 still waits: the editor still shows the empty list
    click([107161])
    check("click 2 (click 1 waiting): both stops", H.lastStops() == f"{s158};{s161}", str(H.lastStops()))

    # click 1 applies before the next click is read
    H.applyNext()
    check("click 1 applied: the line holds stop 1", H.entity() == s158)

    # 3. third station, built by the editor from the EMPTY list it last saw
    click([107164])
    check("click 3 (built from the empty list): all three stops, stop 1 kept",
          H.lastStops() == f"{s158};{s161};{s164}", str(H.lastStops()) + "\n" + H.logs()[-400:])

    H.applyNext()   # click 2's update
    # 4. fourth station, built from the one-stop list
    click([107158, 107167])
    check("click 4 (built from the one-stop list): four stops",
          H.lastStops() == f"{s158};{s161};{s164};{s167}", str(H.lastStops()))

    # everything applies
    while H.applyNext():
        pass
    check("all applied: the line holds four stops", H.entity() == f"{s158};{s161};{s164};{s167}", H.entity())

    # 5. remove the stop just added, nothing waiting: a removal, not a no-op
    click([107158, 107161, 107164])
    check("remove the last stop: three stops scheduled", H.lastStops() == f"{s158};{s161};{s164}", str(H.lastStops()))

    print()
    if fails:
        print(f"{len(fails)} FAILED")
        sys.exit(1)
    print("all passed")


if __name__ == "__main__":
    main()
