"""Offline checks for the SPARE line (lines.lua CM.spareTick / LSPARE / the claim, inject.lua
LCREATEX spare=), on Lua 5.2.

A strict create lands at its stamp, so the editor opened a new line 1-2.6 s after the
click; creating it natively at the click shifts every entity id allocated after it on
that game (3-game rig, 2026-09-12). So every player owns one spare line, created in
lockstep and owned by a hidden pool company; the slice opens the editor on it at the
click and the LCREATEX carries spare=<id>. This loads the real inject.lua and lines.lua
into a lupa.lua52 runtime with stub CM/K/api and drives:
  - spareTick with no spare: one LSPARE scheduled, not another inside the retry window
  - LSPARE replay: the pool company is made once (addPlayer), a grey empty line is
    created for the pool, and once it appears it is keyed spare:<origin>; our own spare
    is written to lockstep_lspare_<x>.txt and the file is kept fresh
  - LCREATEX spare=<ours>: the spare is ours at once (cmSetPlayer), the LCREATE ships
    with spare=<key>, the line is re-keyed to origin:seq here, the file is blanked
  - LCREATEX spare=<not ours>: the ordinary strict create, with a loud log line
  - the claim on a peer: re-owned to the origin's player, renamed, recoloured, re-keyed,
    the origin's next spare created on the same step; with stops, updateLine too
  - the claim before the spare is bound: retried on the deterministic step grid
  - the pool company rides in the line-key save state

    python tools/line_spare_test.py
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


def runtime(inject_path, base, instance):
    L = lupa.LuaRuntime(unpack_returned_tuples=True)
    L.globals().package.path = os.path.join(REPO, "mod/mp_lockstep_1/res/scripts/?.lua").replace("\\", "/") + ";" + L.globals().package.path
    g = L.globals()
    g.INJECT_SRC = open(os.path.join(MP, "inject.lua"), encoding="utf-8").read()
    g.LINES_SRC = open(os.path.join(MP, "lines.lua"), encoding="utf-8").read()
    g.INJECT = inject_path.replace("\\", "/")
    g.BASE = base.replace("\\", "/") + "/"
    g.INSTANCE = instance
    H = L.execute(r'''
local logs, sched, sent, owners, players, names, lines, snaps = {}, {}, {}, {}, {}, {}, {}, {}
local function sink()
  return setmetatable({}, { __index = function() return sink() end, __call = function() return nil end })
end
api = setmetatable({}, { __index = function() return sink() end })
api.type = setmetatable({
  Line = { new = function() return { stops = {} } end, Stop = { new = function() return {} end } },
  Vec3f = { new = function(r, g, b) return { r, g, b } end },
  ComponentType = { STATION_GROUP = 1, LINE = 2 },
}, { __index = function() return sink() end })
api.engine = setmetatable({
  util = { getPlayer = function() return 1 end },
  system = { lineSystem = { getLines = function() return lines end } },
  entityExists = function(id) return snaps[id] ~= nil end,
}, { __index = function() return sink() end })
local nextCb = {}
api.cmd = {
  make = {
    createLine = function(name, color, player, line) return { what = "createLine", name = name, color = color, player = player, n = #line.stops } end,
    setColor = function(id, color) return { what = "setColor", id = id, color = color } end,
    setName = function(id, name) return { what = "setName", id = id, name = name } end,
    updateLine = function(id, line) return { what = "updateLine", id = id, n = #line.stops } end,
  },
  sendCommand = function(cmd, cb) sent[#sent + 1] = cmd; if cb then nextCb[#nextCb + 1] = cb end end,
}
local addPlayers = 0
game = setmetatable({ interface = setmetatable({
  addPlayer = function() addPlayers = addPlayers + 1; return 900 + addPlayers end,
  setMaximumLoan = function() end,
}, { __index = function() return sink() end }) }, { __index = function() return sink() end })
local K = setmetatable({ INSTANCE = INSTANCE, PEER = "b", INJECT_FILE = INJECT, BASE = BASE, VLINE_RETRY_STEPS = 1,
                         STRICT_OPS = { LCREATE = true, LUPDATE = true, LDELETE = true, LSPARE = true } },
  { __index = function() return nil end })
local CM = { peerSeen = true, injectOffset = 0, seqNo = 8, ticks = 0, retryQueue = {} }
local now = 100
function CM.gameTime() return now end
function CM.stepOf(t) return math.floor((t or 0) / 0.2 + 0.5) end
function CM.scheduleLocal(op, args)
  CM.seqNo = CM.seqNo + 1
  args.op, args.seq, args.at = op, CM.seqNo, now + 0.8
  sched[#sched + 1] = { op = op, args = args }
end
function CM.escName(s) return (tostring(s or ""):gsub("[ %%]", function(c) return string.format("%%%02X", c:byte()) end)) end
function CM.unescName(s) return (tostring(s or ""):gsub("%%(%x%x)", function(h) return string.char(tonumber(h, 16)) end)) end
function CM.cmOwnerOf(id) return owners[id] end
function CM.cmSetPlayer(id, pid) owners[id] = pid; players[#players + 1] = { id = id, pid = pid } end
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
function CM.lineSnapshot(lid) return snaps[lid] end
function CM.stationGroupPos(sg) if sg == 5000 then return 100, 200 end end
function CM.stationPosInGroup() return nil end
function CM.stopsSigEqual(a, b) return (a or "") == (b or "") end
local H = { CM = CM }
function H.poll() CM.pollInject() end
function H.tick(n) for _ = 1, (n or 1) do CM.ticks = CM.ticks + 1; CM.spareTick() end end
function H.advance(t) now = now + t end
function H.pollKeys() CM.pollLineKeys() end
function H.nsched() return #sched end
function H.op(i) local s = sched[i]; return s and s.op end
function H.arg(i, k) local s = sched[i]; return s and s.args[k] end
function H.clearSched() sched = {} end
function H.nsent() return #sent end
function H.sent(i, k) local c = sent[i]; return c and c[k] end
function H.sentColor(i, j) local c = sent[i]; return c and c.color and c.color[j] end
function H.clearSent() sent = {} end
function H.completeNext(ok) local cb = table.remove(nextCb, 1); if cb then cb({}, ok) end end
function H.appear(lid, name) lines[#lines + 1] = lid; snaps[lid] = { name = CM.escName(name), color = "0.5,0.5,0.5", wait = 180, stops = "", alts = "" }; owners[lid] = owners[lid] or CM.poolPid end
function H.vanish(lid) snaps[lid] = nil; for i = #lines, 1, -1 do if lines[i] == lid then table.remove(lines, i) end end end
function H.keyOf(lid) return CM.lineKeyOf[lid] end
function H.idFor(key) return CM.lineIdFor(key) end
function H.owner(id) return owners[id] end
function H.nplayers() return #players end
function H.player(i) local p = players[i]; return p and p.pid end
function H.pool() return CM.poolPid end
function H.addPlayers() return addPlayers end
function H.spareFile()
  local f = io.open(CM.spareFile(), "r"); if not f then return nil end
  local s = f:read("*a"); f:close(); return s
end
function H.logs() return table.concat(logs, "\n") end
function H.nretry() return #CM.retryQueue end
function H.exec(c) CM.execLine(c) end
function H.saveState() return CM.lineKeysSaveState() end
function H.loadState(st) CM.lineKeysLoadState(st); CM.primeLineKeys() end
return H
''')

    class Harness:
        # python dicts index by KeyError in Lua; hand execLine a real Lua table
        def __init__(self, H):
            self._H = H

        def __getattr__(self, k):
            return getattr(self._H, k)

        def exec(self, c):
            return self._H.exec(L.table_from(c))

        def loadState(self, d):
            t = L.table_from({k: (L.table_from(v) if isinstance(v, dict) else v) for k, v in d.items()})
            return self._H.loadState(t)

        def saveState(self):
            st = self._H.saveState()
            return {k: (dict(v.items()) if lupa.lua_type(v) == "table" else v) for k, v in st.items()}

    return Harness(H)


def main():
    tmp = tempfile.mkdtemp()
    inject = os.path.join(tmp, "lockstep_inject_a.txt")
    open(inject, "w").close()
    H = runtime(inject, tmp, "a")

    def write(*rows):
        with open(inject, "a", encoding="utf-8", newline="\n") as f:
            for r in rows:
                f.write(r + "\n")

    print("== asking for a spare")
    H.tick(1)
    check("no spare: one LSPARE scheduled", H.nsched() == 1 and H.op(1) == "LSPARE", H.logs()[-200:])
    H.tick(5)
    check("no second ask inside the retry window", H.nsched() == 1)
    check("the slice's file is absent or blank", not H.spareFile())

    print("== the LSPARE replay, on every instance alike")
    H.exec({"op": "LSPARE", "origin": "a", "seq": 9, "at": 100.8})
    check("pool company made once", H.addPlayers() == 1 and H.pool() == 901, str(H.pool()))
    check("a grey empty line for the pool", H.nsent() == 1 and H.sent(1, "what") == "createLine" and H.sent(1, "player") == 901
          and H.sent(1, "n") == 0 and H.sentColor(1, 1) == 0.5, str(H.sent(1, "what")))
    check("named for its origin", H.sent(1, "name") == "spare a", str(H.sent(1, "name")))
    H.completeNext(True)
    H.appear(500, "spare a")
    H.pollKeys()
    check("the new line is keyed spare:a", H.keyOf(500) == "spare:a" and H.idFor("spare:a") == 500, str(H.keyOf(500)))
    check("our spare is handed to the slice", H.spareFile() == "500", str(H.spareFile()))
    H.tick(1)
    check("with a spare, nothing more is asked", H.nsched() == 1)
    H.exec({"op": "LSPARE", "origin": "a", "seq": 10, "at": 101.0})
    check("a second LSPARE for the same origin does nothing", H.nsent() == 1 and H.addPlayers() == 1)

    print("== the click: LCREATEX spare=<ours>")
    H.clearSched(); H.clearSent()
    write("ARMED 1", "LCREATEX 0.1 0.2 0.3 180 0 spare=500 name=Bus%20Line%201")
    H.poll()
    check("ours at once", H.nplayers() == 1 and H.player(1) == 1 and H.owner(500) == 1, str(H.owner(500)))
    check("the create ships with the spare's key", H.nsched() == 1 and H.op(1) == "LCREATE" and H.arg(1, "spare") == "spare:a", str(H.arg(1, "spare")))
    check("name and colour kept", H.arg(1, "name") == "Bus%20Line%201" and H.arg(1, "color") == "0.1,0.2,0.3", str(H.arg(1, "color")))
    seq = H.arg(1, "seq")
    check("re-keyed here to origin:seq", H.keyOf(500) == f"a:{seq}" and H.idFor(f"a:{seq}") == 500 and H.idFor("spare:a") is None, str(H.keyOf(500)))
    check("the slice's file is blank again", H.spareFile() == "", repr(H.spareFile()))
    check("the read-back path is not used", len(list(H.CM.pendingLineCreates.values())) == 0)

    print("== the claim replays on the originator too")
    H.clearSent()
    H.exec({"op": "LCREATE", "origin": "a", "seq": seq, "at": 100.8, "armed": 1, "spare": "spare:a",
            "name": "Bus%20Line%201", "color": "0.1,0.2,0.3", "wait": 180, "stops": "", "alts": ""})
    check("owner set again (idempotent), named, coloured, no updateLine for an empty create",
          H.nplayers() == 2 and H.sent(1, "what") == "setName" and H.sent(1, "name") == "Bus Line 1"
          and H.sent(2, "what") == "setColor" and H.sentColor(2, 1) == 0.1 and H.sent(3, "what") == "createLine",
          " ".join(str(H.sent(i, "what")) for i in range(1, H.nsent() + 1)))
    check("the next spare is created on the same step, for the pool", H.sent(3, "player") == 901 and H.sent(3, "name") == "spare a")
    H.completeNext(True); H.completeNext(True); H.completeNext(True)
    H.appear(501, "spare a")
    H.pollKeys()
    check("the successor is keyed spare:a and handed to the slice", H.keyOf(501) == "spare:a" and H.spareFile() == "501", str(H.spareFile()))
    check("the claimed line keeps its key", H.keyOf(500) == f"a:{seq}")

    print("== a spare that is not ours")
    H.clearSched(); H.clearSent()
    write("ARMED 1", "LCREATEX 0.1 0.2 0.3 180 0 spare=777 name=L2")
    H.poll()
    check("the ordinary strict create", H.nsched() == 1 and H.op(1) == "LCREATE" and H.arg(1, "spare") is None)
    check("said loudly", "not ours here" in H.logs())

    print("== the file is kept fresh while a spare exists")
    open(os.path.join(tmp, "lockstep_lspare_a.txt"), "w").close()
    H.tick(30)
    check("rewritten within the touch interval", H.spareFile() == "501", repr(H.spareFile()))
    H.vanish(501)
    H.clearSched()
    H.advance(40)
    H.tick(2)   # one tick forgets it, the next asks
    check("a vanished spare is forgotten and a new one asked for", H.idFor("spare:a") is None and H.nsched() == 1 and H.op(1) == "LSPARE", H.logs()[-200:])

    print("== the peer's view: instance b claims a's spare")
    tmpb = tempfile.mkdtemp()
    injb = os.path.join(tmpb, "lockstep_inject_b.txt")
    open(injb, "w").close()
    B = runtime(injb, tmpb, "b")
    B.exec({"op": "LSPARE", "origin": "a", "seq": 9, "at": 100.8})
    B.completeNext(True)
    B.appear(500, "spare a")
    B.pollKeys()
    check("b binds a's spare without touching its own file", B.keyOf(500) == "spare:a" and not B.spareFile())
    B.clearSent()
    B.exec({"op": "LCREATE", "origin": "a", "seq": 11, "at": 100.8, "armed": 1, "spare": "spare:a",
            "name": "Bus%20Line%201", "color": "0.1,0.2,0.3", "wait": 120, "stops": "", "alts": ""})
    check("b re-owns it to a's player", B.nplayers() == 1 and B.player(1) == 1 and B.owner(500) == 1)
    check("re-keyed to a:11", B.keyOf(500) == "a:11" and B.idFor("spare:a") is None, str(B.keyOf(500)))
    what = [B.sent(i, "what") for i in range(1, B.nsent() + 1)]
    check("renamed, recoloured, the changed wait applied, successor created", what == ["setName", "setColor", "updateLine", "createLine"], str(what))
    check("the successor is the origin's, for the pool", B.sent(4, "name") == "spare a" and B.sent(4, "player") == 901)

    print("== the claim before the spare is bound")
    B.clearSent()
    B.exec({"op": "LCREATE", "origin": "c", "seq": 3, "at": 100.8, "armed": 1, "spare": "spare:c",
            "name": "L9", "color": "0.1,0.2,0.3", "wait": 180, "stops": "", "alts": ""})
    check("retried on the step grid, nothing sent", B.nretry() == 1 and B.nsent() == 0, B.logs()[-160:])

    print("== the pool company in the save state")
    st = B.saveState()
    check("pool pid saved", st["pool"] == 901, str(st["pool"]))
    C = runtime(injb, tmpb, "c")
    C.appear(500, "Bus Line 1")   # the save holds the line; the keys are adopted only for lines that exist
    C.loadState(st)
    check("pool pid adopted from the save", C.pool() == 901, str(C.pool()))
    check("the saved spare key came with it", C.idFor("a:11") == 500)

    print()
    if fails:
        print(f"{len(fails)} FAILED")
        for f in fails:
            print("  -", f)
        sys.exit(1)
    print("all passed")


if __name__ == "__main__":
    main()
