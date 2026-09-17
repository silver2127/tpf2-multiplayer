"""Offline checks for the vehicle clone replay (inject.lua VBUY/VBUYLINE, vehicles.lua), on Lua 5.2.

The game's clone buys through the depot window's buy function, and the buy's completion
callback then SetLines the new vehicle onto the original's line. The slice cancels the
buy, so it writes VBUY at capture and VBUYLINE <line> at CommandList::Add. This loads the
real mod/.../scripts/mp/inject.lua and vehicles.lua into a lupa.lua52 runtime with stub
CM/K/api and drives:
  - VBUYLINE in the same read as its VBUY: the scheduled VBUY carries cline=<line key>
  - VBUYLINE in the NEXT read: the VBUY waits one poll and still carries it
  - no VBUYLINE: the VBUY that ended a read ships bare on the next poll
  - VBUYLINE -1 (a depot buy) and a line with no cross-peer key: no cline
  - CM.cloneOntoLine: setLine(vehicle, line, 0) sent; an unknown line sends nothing

    python tools/clone_buy_test.py
"""
import os
import sys
import tempfile

import lupa.lua52 as lupa

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
MP = os.path.join(REPO, "mod", "mp_lockstep_1", "res", "scripts", "mp")

# a real record from lockstep_inject_a.txt (depot child 241897, one bus part)
VBUY = "VBUY 241897 1 3320 1 0 -1.0000 -1.0000 -1.0000 1 1 1 1"

fails = []


def check(name, cond, extra=""):
    print(("ok   " if cond else "FAIL ") + name + (f"  ({extra})" if extra else ""))
    if not cond:
        fails.append(name)


def runtime(inject_path):
    L = lupa.LuaRuntime(unpack_returned_tuples=True)
    L.globals().package.path = os.path.join(REPO, "mod/mp_lockstep_1/res/scripts/?.lua").replace("\\", "/") + ";" + L.globals().package.path
    g = L.globals()
    g.INJECT_SRC = open(os.path.join(MP, "inject.lua"), encoding="utf-8").read()
    g.VEH_SRC = open(os.path.join(MP, "vehicles.lua"), encoding="utf-8").read()
    g.INJECT = inject_path.replace("\\", "/")
    return L.execute(r'''
local logs, sched, sent = {}, {}, {}
-- anything the modules touch at load or in unrelated branches answers harmlessly
local function sink()
  return setmetatable({}, { __index = function() return sink() end, __call = function() return nil end })
end
api = setmetatable({}, { __index = function() return sink() end })
api.type = { ComponentType = { CONSTRUCTION = 1 } }
api.res = { modelRep = { getName = function(id) return "vehicle/bus/test_" .. tostring(id) .. ".mdl" end } }
api.engine = setmetatable({
  getComponent = function(id, t)
    if id == 900 and t == 1 then
      local tr = {}; for i = 1, 16 do tr[i] = 0 end; tr[13], tr[14] = 2857.1, 3550.2
      return { depots = { 241897 }, transf = tr, fileName = "depot/road_depot_era_a.con" }
    end
  end,
}, { __index = function() return sink() end })
api.cmd = {
  make = { setLine = function(v, l, s) return { what = "setLine", v = v, l = l, s = s } end },
  sendCommand = function(cmd, cb) sent[#sent + 1] = cmd; if cb then cb({}, true) end end,
}
game = setmetatable({}, { __index = function() return sink() end })
local K = setmetatable({ INSTANCE = "a", PEER = "b", INJECT_FILE = INJECT, STRICT_OPS = { VBUY = true },
                         BIND_GUARD_STEPS = 10, VLINE_RETRY_STEPS = 5 },
  { __index = function() return nil end })
local CM = { peerSeen = true, injectOffset = 0, consByKey = { d = { id = 900 } }, seqNo = 0, ticks = 0 }
function CM.gameTime() return 100 end
function CM.stepOf(t) return math.floor((t or 0) / 0.2 + 0.5) end
function CM.scheduleLocal(op, args) CM.seqNo = CM.seqNo + 1; sched[#sched + 1] = { op = op, args = args, seq = CM.seqNo } end
-- companies.lua's paint, as the strict buy calls it (2026-09-16): records the key it was given
local keys = { [170607] = "a:87" }
function CM.lineKeyFor(lid) return keys[lid] end
local ids = { ["a:87"] = 170607 }
function CM.lineIdFor(k) return ids[k] end
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
assert(load(VEH_SRC, "@vehicles.lua"))()(CM, K, log)
local H = { CM = CM }
function H.poll() CM.pollInject() end
function H.nsched() return #sched end
function H.cline(i) local s = sched[i]; return s and s.args.cline end
function H.op(i) local s = sched[i]; return s and s.op end
local paints = {}
function CM.cmColorNewVehicle(key) paints[#paints + 1] = key end
function H.paints() local t = {}; for i, k in ipairs(paints) do t[i] = k end; return t end
function H.clearPaints() paints = {} end
function H.arg(i, k) local s = sched[i]; return s and s.args and s.args[k] end
function H.seq(i) local s = sched[i]; return s and s.seq end
function H.clearSched() sched = {} end
function H.nsent() return #sent end
function H.sentAt(i) local c = sent[i]; return c and string.format("%s %s %s %s", c.what, c.v, c.l, c.s) end
function H.clearSent() sent = {} end
function H.logs() return table.concat(logs, "\n") end
function H.clone(cline, key) CM.queueCloneAssign({ seq = 7, origin = "a", at = 100, cline = cline }, key) end
function H.nretry() return #(CM.retryQueue or {}) end
function H.retry(i, k) local r = (CM.retryQueue or {})[i]; return r and r[k] end
-- the originator's own strict VLINE for a key that has not bound yet
function H.execVLineUnbound()
  K.STRICT_OPS.VLINE = true
  local before = #(CM.retryQueue or {})
  CM.execVehCmd({ op = "VLINE", origin = "a", seq = 40.5, at = 100, key = "a:40", line = "a:87", stop = 0, armed = 1 })
  local q = CM.retryQueue or {}
  local last = q[#q]
  if #q == before + 1 and last and last.key == "a:40" and last.tries == 1 and last.notBeforeStep == 505 then return "retried" end
  return "dropped"
end
return H
''')


def main():
    d = tempfile.mkdtemp()
    path = os.path.join(d, "lockstep_inject_a.txt")
    open(path, "wb").close()
    H = runtime(path)

    def write(*lines):
        with open(path, "ab") as f:
            for ln in lines:
                f.write((ln + "\n").encode())

    # 1. VBUY and its VBUYLINE in one read
    write("ARMED 1", VBUY, "VBUYLINE 170607")
    H.poll()
    check("same read: one VBUY scheduled", H.nsched() == 1 and H.op(1) == "VBUY", H.logs()[-300:])
    check("same read: the VBUY carries the clone line key", H.cline(1) == "a:87", str(H.cline(1)))
    H.clearSched()

    # 2. the VBUYLINE lands in the next read
    write("ARMED 1", VBUY)
    H.poll()
    check("split read: the VBUY that ends the read waits", H.nsched() == 0)
    write("VBUYLINE 170607")
    H.poll()
    check("split read: scheduled on the next poll", H.nsched() == 1)
    check("split read: still carries the clone line", H.cline(1) == "a:87", str(H.cline(1)))
    H.clearSched()

    # 2b. the strict buy paints its vehicle: a VCOLOR keyed by the buy's own key follows (2026-09-16)
    H.clearPaints()
    write("ARMED 1", VBUY, "VBUYLINE -1")
    H.poll()
    paints = [H.paints()[i] for i in range(1, len(H.paints()) + 1)]
    check("strict buy: the company paint is asked for once, right behind the VBUY", H.nsched() == 1 and len(paints) == 1, str(paints))
    check("strict buy: the paint names the buy's own key", paints and paints[0] == "a:" + str(H.seq(1)), str(paints))
    H.clearSched(); H.clearPaints()

    # 3. no VBUYLINE ever: a plain buy, one poll late
    write("ARMED 1", VBUY)
    H.poll()
    check("plain buy: waits one poll", H.nsched() == 0)
    H.poll()
    check("plain buy: ships on the next poll with no new data", H.nsched() == 1 and H.cline(1) is None, str(H.cline(1)))
    H.clearSched()

    # 4. two buys in a row, the first followed by more records: no hold for it
    write("ARMED 1", VBUY, "ARMED 1", VBUY, "VBUYLINE 170607")
    H.poll()
    check("burst: both scheduled in one poll", H.nsched() == 2)
    check("burst: only the second is a clone", H.cline(1) is None and H.cline(2) == "a:87",
          f"{H.cline(1)} / {H.cline(2)}")
    H.clearSched()

    # 5. -1 is the depot window's own buy; an unkeyed line cannot be named
    write("ARMED 1", VBUY, "VBUYLINE -1", "ARMED 1", VBUY, "VBUYLINE 555")
    H.poll()
    check("-1 and unkeyed line: both buys scheduled", H.nsched() == 2)
    check("-1 and unkeyed line: neither carries a clone line", H.cline(1) is None and H.cline(2) is None)
    check("unkeyed line: says the vehicle stays in the depot", "no cross-peer key" in H.logs())
    H.clearSched()

    # 6. the replay's assignment: a VLINE queued at the buy's stamp, same step everywhere
    H.clone("a:87", "a:7")
    check("queueCloneAssign: one VLINE queued", H.nretry() == 1 and H.retry(1, "op") == "VLINE")
    check("queueCloneAssign: for the bought vehicle's key and the clone line",
          H.retry(1, "key") == "a:7" and H.retry(1, "line") == "a:87", f"{H.retry(1, 'key')} {H.retry(1, 'line')}")
    check("queueCloneAssign: stop 0, armed, its own seq",
          H.retry(1, "stop") == 0 and H.retry(1, "armed") == 1 and H.retry(1, "seq") == 7.5, str(H.retry(1, "seq")))
    check("queueCloneAssign: due BIND_GUARD_STEPS after the stamp (step 500 + 10)",
          H.retry(1, "notBeforeStep") == 510, str(H.retry(1, "notBeforeStep")))
    check("queueCloneAssign: nothing sent directly", H.nsent() == 0)

    # 7. the ORIGINATOR's strict assignment with a key not bound yet retries like a peer's.
    # It used to fall through to "unknown vehicle key" and drop the assignment: seven cloned
    # trucks stayed parked on the host and ran on the joiner (2026-09-16, a:40..a:46).
    H.clearSent()
    n0 = H.nretry()
    check("originator with an unbound key: retried, not dropped",
          H.execVLineUnbound() == "retried" and H.nretry() == n0 + 1 and "unknown vehicle key" not in H.logs())
    check("originator with an unbound key: nothing sent to the engine", H.nsent() == 0)

    print()
    if fails:
        print(f"{len(fails)} FAILED")
        sys.exit(1)
    print("all passed")


if __name__ == "__main__":
    main()
