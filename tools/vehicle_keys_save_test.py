"""Offline checks: vehicle and line KEYS travel in the save (vehicles.lua, lines.lua), on Lua 5.2.

Measured 2026-09-16 on the two-instance rig: the host bought vehicles keyed a:7 / a:8; a
joiner that later loaded the host's 954.6 save primed the same entity 189156 as s:189156,
so the VPOS pairing said '(nearest)' instead of '(same key)' and any host command naming
a:8 would have been "unknown vehicle key" on the joiner. Now CM.vehKeysSaveState /
CM.lineKeysSaveState ride in the game script's save and are adopted on the first tick.

This loads the real mod/.../scripts/mp/vehicles.lua and lines.lua into lupa.lua52 runtimes
with stub CM/K/api and drives:
  - the HOST binds a:7 / a:8 through its real path (shipParkedBuys -> pollVehKeys) and a
    line a:9 through pollLineKeys; the save state names them by entity id
  - the state is copied through Python (plain strings/numbers only, like the engine's
    serializer) into a FRESH runtime whose world holds those entities: a:8 resolves to
    189156 and 189156 reads back as a:8; a line the same way
  - a keyed entity absent from the loaded world is not adopted
  - a vehicle the save carried no key for still primes as s:<id>
  - the per-origin counters restore as max(saved, local): a letter the save already used
    never mints a key the save holds, and a higher local seq is kept
  - an echoed load (the same state again, as the GUI state sees every frame) changes nothing
  - a save with no key state (an older build) primes s:<id> for everything, as before
  - a re-save after a sale drops the sold key but its counter never goes backwards

    python tools/vehicle_keys_save_test.py
"""
import os
import sys

import lupa.lua52 as lupa

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
MP = os.path.join(REPO, "mod", "mp_lockstep_1", "res", "scripts", "mp")

fails = []


def check(name, cond, extra=""):
    print(("ok   " if cond else "FAIL ") + name + (f"  ({extra})" if extra else ""))
    if not cond:
        fails.append(name)


def runtime(letter, seq):
    L = lupa.LuaRuntime(unpack_returned_tuples=True)
    L.globals().package.path = os.path.join(REPO, "mod/mp_lockstep_1/res/scripts/?.lua").replace("\\", "/") + ";" + L.globals().package.path
    g = L.globals()
    g.VEH_SRC = open(os.path.join(MP, "vehicles.lua"), encoding="utf-8").read()
    g.LINE_SRC = open(os.path.join(MP, "lines.lua"), encoding="utf-8").read()
    g.LETTER = letter
    g.SEQ = seq
    H = L.execute(r'''
local logs = {}
local WORLD, PARKED, PT, LINES = {}, {}, {}, {}
local function sink()
  return setmetatable({}, { __index = function() return sink() end, __call = function() return nil end })
end
api = setmetatable({}, { __index = function() return sink() end })
api.type = { ComponentType = { CONSTRUCTION = 1, TRANSPORT_VEHICLE = 2 },
             enum = { TransportVehicleState = { IN_DEPOT = 1 } } }
api.engine = setmetatable({
  entityExists = function(id) return WORLD[id] == true end,
  getComponent = function(id, t)
    if t == 1 and id == 900 then return { depots = { 901 } } end
    if t == 2 and PARKED[id] then
      return { depot = 900, transportVehicleConfig = { vehicles = { { purchaseTime = PT[id] or 0 } } } }
    end
    return nil
  end,
  system = {
    transportVehicleSystem = { getVehiclesWithState = function()
      local out = {}
      for id in pairs(PARKED) do out[#out + 1] = id end
      table.sort(out)
      return out
    end },
    lineSystem = { getLines = function()
      local out = {}
      for id in pairs(LINES) do out[#out + 1] = id end
      table.sort(out)
      return out
    end },
  },
  util = { getPlayer = function() return 1 end },
}, { __index = function() return sink() end })
local sent = {}
api.cmd = {
  make = { sellVehicle = function(id) return { what = "sell", id = id } end },
  sendCommand = function(cmd, cb) sent[#sent + 1] = cmd; if cb then cb({}, true) end end,
}
game = { interface = setmetatable({
  getEntities = function()
    local out = {}
    for id in pairs(WORLD) do if not PARKED[id] and not LINES[id] then out[#out + 1] = id end end
    table.sort(out)
    return out
  end,
}, { __index = function() return sink() end }) }
local K = setmetatable({ INSTANCE = LETTER, STRICT_OPS = {}, BIND_GUARD_STEPS = 10, VLINE_RETRY_STEPS = 5 },
  { __index = function() return nil end })
local CM = { seqNo = SEQ, ticks = 0, consPrimed = true, primeQueue = {}, parkedBuys = {},
             cmMode = "coop", cmCompanyPid = {}, consByKey = {} }
local T = 100
function CM.gameTime() return T end
function CM.stepOf(t) return math.floor((t or 0) / 0.2 + 0.5) end
function CM.scheduleLocal(op, args) CM.seqNo = CM.seqNo + 1 end
function CM.cmBalance() return 0 end
function CM.cmLog(s) logs[#logs + 1] = s end
local log = function(s) logs[#logs + 1] = s end
assert(load(VEH_SRC, "@vehicles.lua"))()(CM, K, log)
assert(load(LINE_SRC, "@lines.lua"))()(CM, K, log)
-- the line read-back is the engine's; a fixed snapshot is enough to bind a key
CM.lineSnapshot = function(lid) return { name = "L" .. lid, color = { 1, 0, 0 }, wait = 180, stops = {} } end
local H = { CM = CM }
function H.addVehicle(id) WORLD[id] = true end
function H.park(id, pt) WORLD[id] = true; PARKED[id] = true; PT[id] = pt end
function H.addLine(id) WORLD[id] = true; LINES[id] = true end
function H.parkedBuy() CM.parkedBuys[#CM.parkedBuys + 1] = { depot = 900, args = {}, since = T } end
function H.lineCreate() CM.pendingLineCreates[#CM.pendingLineCreates + 1] = { since = T } end
function H.tick() T = T + 1; CM.ticks = CM.ticks + 1; CM.primeVehKeys(); CM.shipParkedBuys(); CM.pollVehKeys(); CM.primeLineKeys(); CM.pollLineKeys() end
function H.vehKey(vid) return CM.vehKeyOf[vid] end
function H.vehKeyFor(vid) return CM.vehKeyFor(vid) end
function H.vehId(key) return CM.vehIdForKey(key) end
function H.lineKey(lid) return CM.lineKeyOf[lid] end
function H.lineId(key) return CM.lineIdFor(key) end
function H.seq() return CM.seqNo end
function H.vehNext(o) return CM.vehKeyNext[o] end
-- a host VSELL naming a key, replayed here through the real apply path (sells on success -> forgetVehicle)
function H.hostSell(key) CM.execVehCmd({ op = "VSELL", origin = "a", seq = 99, at = T, keys = key, armed = 1 }) end
function H.nsent() return #sent end
function H.sentId(i) local c = sent[i]; return c and c.id end
function H.logs() return table.concat(logs, "\n") end
function H.count(pat) local n = 0; for _, l in ipairs(logs) do if l:find(pat, 1, true) then n = n + 1 end end; return n end
return H
''')
    return L, H


def lua_to_py(v):
    """the engine serializes plain tables: strings, numbers, booleans, nested tables only"""
    if lupa.lua_type(v) == "table":
        out = {}
        for k, x in v.items():
            assert isinstance(k, (str, int, float)), f"table key {k!r}"
            out[k] = lua_to_py(x)
        return out
    assert v is None or isinstance(v, (str, int, float, bool)), f"value {v!r}"
    return v


def py_to_lua(L, v):
    if isinstance(v, dict):
        t = L.table()
        for k, x in v.items():
            t[k] = py_to_lua(L, x)
        return t
    return v


def main():
    # ---------- the HOST: a:7, a:8 bought natively, line a:9 created natively ----------
    L, host = runtime("a", 6)
    host.addVehicle(777)             # in the save before this session: s:777
    host.tick()                      # primes 777
    host.park(189155, 10)
    host.park(189156, 20)
    host.parkedBuy()
    host.parkedBuy()
    host.tick()
    check("host: a:7 and a:8 bound to 189155 / 189156 through shipParkedBuys + pollVehKeys",
          host.vehKey(189155) == "a:7" and host.vehKey(189156) == "a:8",
          f"{host.vehKey(189155)} {host.vehKey(189156)}\n{host.logs()[-500:]}")
    host.addLine(5001)
    host.lineCreate()
    host.tick()
    check("host: line a:9 bound to 5001", host.lineKey(5001) == "a:9", str(host.lineKey(5001)))
    check("host: the save vehicle stays s:777", host.vehKeyFor(777) == "s:777")
    # a peer's key seen earlier: its counter is remembered even with no live vehicle
    host.CM.vehKeyNext["b"] = 30

    st = lua_to_py(host.CM.vehKeysSaveState())
    lst = lua_to_py(host.CM.lineKeysSaveState())
    check("save: vehicle keys by entity id, bound keys only (no s:)",
          st["keys"] == {"189155": "a:7", "189156": "a:8"}, str(st["keys"]))
    check("save: counters carry a's seq and b's high-water", st["next"]["a"] == 9 and st["next"]["b"] == 30, str(st["next"]))
    check("save: line keys by line id", lst["keys"] == {"5001": "a:9"}, str(lst["keys"]))
    # a key for a vehicle / line that will be gone on the joiner
    st["keys"]["4242"] = "a:5"
    lst["keys"]["6000"] = "a:3"

    # ---------- the JOINER: a fresh runtime loads that state with those entities present ----------
    L2, j = runtime("b", 0)
    j.addVehicle(777)                # no key in the save: primes s:777
    j.park(189155, 10)
    j.park(189156, 20)
    j.addLine(5001)
    j.CM.vehKeysLoadState(py_to_lua(L2, st))
    j.CM.lineKeysLoadState(py_to_lua(L2, lst))
    j.CM.vehKeysLoadState(py_to_lua(L2, st))     # the GUI-state echo before the first tick
    check("joiner: nothing adopted before the first tick", j.vehKey(189156) is None)
    j.tick()
    check("joiner: a:8 resolves to 189156", j.vehId("a:8") == 189156, str(j.vehId("a:8")))
    check("joiner: 189156 reads back as a:8, 189155 as a:7",
          j.vehKey(189156) == "a:8" and j.vehKeyFor(189155) == "a:7", f"{j.vehKey(189156)} {j.vehKeyFor(189155)}")
    check("joiner: the vehicle the save had no key for primes s:777", j.vehKeyFor(777) == "s:777" and j.vehId("s:777") == 777)
    check("joiner: a key for a vehicle absent from this world is not adopted",
          j.vehId("a:5") is None and j.vehKey(4242) is None)
    check("joiner: line a:9 resolves to 5001 and back", j.lineId("a:9") == 5001 and j.lineKey(5001) == "a:9")
    check("joiner: a key for a line absent from this world is not adopted", j.lineId("a:3") is None)
    check("joiner: adoption logged once, with the counts",
          j.count("veh: adopted 2 key(s) from the save, 1 for vehicles no longer there") == 1
          and j.count("line: adopted 1 key(s) from the save, 1 for lines no longer there") == 1, j.logs()[-400:])
    check("joiner (letter b): seq lifted to the save's b high-water, so b:1..b:30 are never minted again",
          j.seq() == 30, str(j.seq()))
    check("joiner: a's counter kept for the next save", j.vehNext("a") == 9, str(j.vehNext("a")))

    # an echoed load after adoption: nothing changes
    j.CM.vehKeysLoadState(py_to_lua(L2, st))
    j.CM.lineKeysLoadState(py_to_lua(L2, lst))
    j.tick()
    check("echo: the same state again adopts nothing and moves nothing",
          j.count("veh: adopted") == 1 and j.count("line: adopted") == 1 and j.seq() == 30
          and j.vehKey(189156) == "a:8" and j.vehKeyFor(777) == "s:777")

    # the bug itself: a host VSELL naming a pre-join key now resolves here and sells that vehicle
    j.hostSell("a:7")
    check("joiner: a host VSELL naming a:7 resolves to 189155 and sells it (used to be 'unknown vehicle key')",
          j.nsent() == 1 and j.sentId(1) == 189155 and j.count("unknown vehicle key") == 0,
          f"sent={j.nsent()} id={j.sentId(1)}")
    # the joiner's own save: adopted keys and counters ride on; a sold vehicle's key does not
    st2 = lua_to_py(j.CM.vehKeysSaveState())
    check("re-save: the adopted key rides on, the sold one is gone, s: keys are not saved",
          st2["keys"] == {"189156": "a:8"}, str(st2["keys"]))
    check("re-save: counters never go backwards (a stays 9 without a:7; b stays 30 with no b key)",
          st2["next"]["a"] == 9 and st2["next"]["b"] == 30, str(st2["next"]))

    # ---------- a local seq already past the save's: kept ----------
    L3, k = runtime("b", 40)
    k.park(189156, 20)
    k.CM.vehKeysLoadState(py_to_lua(L3, st))
    k.tick()
    check("higher local seq: max(saved, local) keeps 40", k.seq() == 40 and k.vehKey(189156) == "a:8", str(k.seq()))

    # ---------- the host's letter reused from the save (a restart of the host): mints past a:9 ----------
    L4, a2 = runtime("a", 0)
    a2.park(189156, 20)
    a2.addLine(5001)
    a2.CM.vehKeysLoadState(py_to_lua(L4, st))
    a2.CM.lineKeysLoadState(py_to_lua(L4, lst))
    a2.tick()
    check("host restarted as 'a' with seq 0: seq lifted to 9, the next key is a:10, not a:7", a2.seq() == 9, str(a2.seq()))
    a2.addLine(5002)
    a2.lineCreate()
    a2.tick()
    check("host restarted: the next line minted is a:10", a2.lineKey(5002) == "a:10", str(a2.lineKey(5002)))

    # ---------- an older save with no key state: everything primes s:<id> as before ----------
    L5, o = runtime("b", 0)
    o.addVehicle(777)
    o.park(189156, 20)
    o.addLine(5001)
    o.tick()
    check("no key state: 189156 is s:189156 and 5001 is s:5001, as before",
          o.vehKeyFor(189156) == "s:189156" and o.vehId("s:189156") == 189156 and o.lineId("s:5001") == 5001)
    check("no key state: seq untouched", o.seq() == 0)
    o.hostSell("a:7")
    check("no key state: the measured failure -- a host VSELL naming a:7 is 'unknown vehicle key' here",
          o.nsent() == 0 and o.count("unknown vehicle key a:7") == 1, o.logs()[-200:])
    check("no key state: no adoption logged", o.count("adopted") == 0)
    st5 = lua_to_py(o.CM.vehKeysSaveState())
    check("no key state: its own save carries no keys and no counters yet", st5["keys"] == {} and st5["next"] == {}, str(st5))

    print()
    if fails:
        print(f"{len(fails)} FAILED")
        sys.exit(1)
    print("all passed")


if __name__ == "__main__":
    main()
