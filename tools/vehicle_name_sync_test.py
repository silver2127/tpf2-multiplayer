"""Offline checks: a bought vehicle's engine-given NAME travels to the peers (vehicles.lua, Lua 5.2).

Measured 2026-09-18 between an English and a German game: every new train produced a
"DESYNC (train names)" a stamp after leaving its depot, with equal geometry. No instance
names a bought vehicle -- the engine does, in its own language and with its own counter --
and the native reservation-order patch ranks trains by name. Now the ORIGINATOR ships the
name the engine gave its copy as a VNAME once the buy's key binds (pollVehKeys), and a
peer whose key is not bound yet retries the VNAME on the step grid.

This loads the real mod/.../scripts/mp/vehicles.lua into lupa.lua52 runtimes with stub
CM/K/api and drives:
  - the originator binds a:7 through shipParkedBuys -> pollVehKeys and schedules exactly
    one VNAME {kind=veh, key=a:7, name=<percent-escaped>, skipOrigin=1}
  - a non-ASCII name ("Strassenbahn 3" with the sharp s) escapes byte-wise and a peer's
    execSetName sends make.setName with the exact original string
  - a key that is not ours (a:7 on instance b) ships nothing
  - a vehicle without a NAME component ships nothing and says so
  - without cons.lua's escaper the local fallback escapes the same way
  - a peer's VNAME for a key that is not bound yet goes to the retry queue on the step
    grid, applies once the key binds, and gives up after the retry budget
  - the originator's own copy of its VNAME (skipOrigin=1) is not applied again

    python tools/vehicle_name_sync_test.py
"""
import os

import lupa.lua52 as lupa

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
MP = os.path.join(REPO, "mod", "mp_lockstep_1", "res", "scripts", "mp")

fails = []


def check(name, cond, extra=""):
    print(("ok   " if cond else "FAIL ") + name + (f"  ({extra})" if extra else ""))
    if not cond:
        fails.append(name)


def runtime(letter, seq, with_esc=True):
    L = lupa.LuaRuntime(unpack_returned_tuples=True)
    g = L.globals()
    g.VEH_SRC = open(os.path.join(MP, "vehicles.lua"), encoding="utf-8").read()
    g.LETTER = letter
    g.SEQ = seq
    g.WITH_ESC = with_esc
    return L.execute(r'''
-- Python sets the process locale from Windows; the game's Lua runs in "C". %w is
-- isalnum() and locale-bound, so the escaper is tested under the locale it ships in.
os.setlocale("C")
local logs = {}
local WORLD, PARKED, PT, NAMES = {}, {}, {}, {}
local function sink()
  return setmetatable({}, { __index = function() return sink() end, __call = function() return nil end })
end
api = setmetatable({}, { __index = function() return sink() end })
api.type = { ComponentType = { CONSTRUCTION = 1, TRANSPORT_VEHICLE = 2, NAME = 3 },
             enum = { TransportVehicleState = { IN_DEPOT = 1 } } }
api.engine = setmetatable({
  entityExists = function(id) return WORLD[id] == true end,
  getComponent = function(id, t)
    if t == 1 and id == 900 then return { depots = { 901 } } end
    if t == 2 and PARKED[id] then
      return { depot = 900, transportVehicleConfig = { vehicles = { { purchaseTime = PT[id] or 0 } } } }
    end
    if t == 3 then return NAMES[id] end
    return nil
  end,
  system = {
    transportVehicleSystem = { getVehiclesWithState = function()
      local out = {}
      for id in pairs(PARKED) do out[#out + 1] = id end
      table.sort(out)
      return out
    end },
  },
  util = { getPlayer = function() return 1 end },
}, { __index = function() return sink() end })
local sent = {}
api.cmd = {
  make = { setName = function(id, name) return { what = "setName", id = id, name = name } end },
  sendCommand = function(cmd, cb) sent[#sent + 1] = cmd; if cb then cb({}, true) end end,
}
game = { interface = setmetatable({ getEntities = function() return {} end },
  { __index = function() return sink() end }) }
local K = setmetatable({ INSTANCE = LETTER, STRICT_OPS = {}, BIND_GUARD_STEPS = 10, VLINE_RETRY_STEPS = 5, VCOLOR_RETRY_MAX = 3 },
  { __index = function() return nil end })
local scheduled = {}
local CM = { seqNo = SEQ, ticks = 0, consPrimed = true, primeQueue = {}, parkedBuys = {},
             cmMode = "coop", cmCompanyPid = {}, consByKey = {} }
local T = 100
function CM.gameTime() return T end
function CM.stepOf(t) return math.floor((t or 0) / 0.2 + 0.5) end
function CM.scheduleLocal(op, args) CM.seqNo = CM.seqNo + 1; scheduled[#scheduled + 1] = { op = op, args = args, seq = CM.seqNo } end
function CM.cmBalance() return 0 end
function CM.cmLog(s) logs[#logs + 1] = s end
if WITH_ESC then
  -- cons.lua's pair, verbatim
  function CM.escName(s)
    return (tostring(s or ""):gsub("[^%w%-%._~]", function(c) return string.format("%%%02X", c:byte()) end))
  end
  function CM.unescName(s)
    return (tostring(s or ""):gsub("%%(%x%x)", function(h) return string.char(tonumber(h, 16)) end))
  end
end
local log = function(s) logs[#logs + 1] = s end
assert(load(VEH_SRC, "@vehicles.lua"))()(CM, K, log)
local H = { CM = CM }
function H.park(id, pt, name) WORLD[id] = true; PARKED[id] = true; PT[id] = pt; if name then NAMES[id] = { name = name } end end
function H.parkedBuy() CM.parkedBuys[#CM.parkedBuys + 1] = { depot = 900, args = {}, since = T } end
function H.tick() T = T + 1; CM.ticks = CM.ticks + 1; CM.primeVehKeys(); CM.shipParkedBuys(); CM.pollVehKeys() end
function H.vehId(key) return CM.vehIdForKey(key) end
function H.ship(key, vid) CM.shipVehicleName(key, vid) end
function H.nsched() return #scheduled end
function H.schedOp(i) local s = scheduled[i]; return s and s.op end
function H.schedArgs(i) local s = scheduled[i]; return s and s.args end
function H.setName(origin, key, name, skip) CM.execSetName({ op = "VNAME", origin = origin, seq = 50, at = T, kind = "veh", key = key, name = name, skipOrigin = skip or 0 }) end
function H.retry() local q = CM.retryQueue or {}; CM.retryQueue = {}; for _, c in ipairs(q) do CM.execSetName(c) end; return #q end
function H.nretry() return CM.retryQueue and #CM.retryQueue or 0 end
function H.retryStep(i) return CM.retryQueue[i].notBeforeStep end
function H.nsent() return #sent end
function H.sentId(i) local c = sent[i]; return c and c.id end
function H.sentName(i) local c = sent[i]; return c and c.name end
function H.logs() return table.concat(logs, "\n") end
function H.count(pat) local n = 0; for _, l in ipairs(logs) do if l:find(pat, 1, true) then n = n + 1 end end; return n end
-- one tick with an empty world: everything parked AFTER this is a new purchase, not a primed s:<id>
H.tick()
return H
''')


def vnames(h):
    return [h.schedArgs(i + 1) for i in range(h.nsched()) if h.schedOp(i + 1) == "VNAME"]


SHARP_S_NAME = "Straßenbahn 3"

# --- the originator ships the engine's name once the key binds ------------------------
h = runtime("a", 6)
h.park(189155, 954.6, "Zug 7")
h.parkedBuy()
h.tick(); h.tick()
check("originator: a:7 bound to 189155 through shipParkedBuys + pollVehKeys", h.vehId("a:7") == 189155, str(h.vehId("a:7")))
vn = vnames(h)
check("originator: exactly one VNAME scheduled", len(vn) == 1, str(len(vn)))
if vn:
    a = vn[0]
    check("originator: VNAME names the buy's key", a.kind == "veh" and a.key == "a:7", f"{a.kind} {a.key}")
    check("originator: VNAME carries the engine's name percent-escaped", a.name == "Zug%207", str(a.name))
    check("originator: VNAME skips its own apply (skipOrigin=1)", a.skipOrigin == 1, str(a.skipOrigin))
check("originator: the ship is logged with the key", h.count("VNAME: vehicle a:7 = Zug%207") == 1)
check("originator: a second poll ships nothing more", (h.tick(), len(vnames(h)))[1] == 1)

# --- non-ASCII: byte-wise escape, exact string on the receiving side ------------------
h = runtime("a", 10)
h.park(5, 1.0, SHARP_S_NAME.encode("utf-8"))   # bytes: the engine hands Lua raw UTF-8
h.parkedBuy()
h.tick(); h.tick()
vn = vnames(h)
check("non-ASCII name escapes byte-wise", len(vn) == 1 and vn[0].name == "Stra%C3%9Fenbahn%203", str(vn and vn[0].name))
p = runtime("b", 3)
p.park(77, 1.0)
p.parkedBuy()
p.tick(); p.tick()
check("peer: b:4 bound to 77", p.vehId("b:4") == 77)
p.setName("a", "b:4", "Stra%C3%9Fenbahn%203")
check("peer: make.setName carries the exact original string",
      p.nsent() == 1 and p.sentId(1) == 77 and p.sentName(1) == SHARP_S_NAME, f"{p.sentId(1)} {p.sentName(1)!r}")

# --- not our key, no name: nothing ships -----------------------------------------------
p = runtime("b", 3)
p.park(77, 1.0, "Train 4")
p.ship("a:7", 77)
check("a key of another instance ships nothing", p.nsched() == 0, str(p.nsched()))
h = runtime("a", 6)
h.park(189155, 954.6)   # no NAME component
h.parkedBuy()
h.tick(); h.tick()
check("a vehicle without a NAME component ships nothing", len(vnames(h)) == 0)
check("...and says so", h.count("has no name to share") == 1)
h = runtime("a", 6, with_esc=False)
h.park(1, 1.0, "Zug 7")
h.parkedBuy()
h.tick(); h.tick()
vn = vnames(h)
check("without cons.lua's escaper the local fallback escapes the same way", len(vn) == 1 and vn[0].name == "Zug%207", str(vn and vn[0].name))

# --- the peer's VNAME waits for the key, then applies; gives up after the budget --------
p = runtime("b", 3)
p.setName("a", "b:4", "Zug%207")
check("peer: unbound key -> nothing sent, one retry queued", p.nsent() == 0 and p.nretry() == 1, f"sent={p.nsent()} retry={p.nretry()}")
check("peer: retry sits on the step grid (stepOf(at=101) + VLINE_RETRY_STEPS)", p.retryStep(1) == 505 + 5, str(p.retryStep(1)))
check("peer: first retry is logged", p.count("VNAME seq=50: vehicle key b:4 not bound yet -- retry 1") == 1)
p.park(77, 1.0)
p.parkedBuy()
p.tick(); p.tick()
n = p.retry()
check("peer: the queued VNAME applies once the key binds",
      n == 1 and p.nsent() == 1 and p.sentId(1) == 77 and p.sentName(1) == "Zug 7", f"{p.sentId(1)} {p.sentName(1)!r}")
check("peer: nothing left in the retry queue", p.nretry() == 0)
p = runtime("b", 3)
p.setName("a", "a:9", "Zug%207")
for _ in range(5):
    p.retry()
check("peer: gives up once K.VCOLOR_RETRY_MAX retries are spent, nothing sent", p.nretry() == 0 and p.nsent() == 0, f"retry={p.nretry()} sent={p.nsent()}")
check("peer: the give-up names the retries", p.count("no local veh for key a:9 -- skipped after 4 retries") == 1)

# --- the originator does not apply its own copy ----------------------------------------
h = runtime("a", 6)
h.park(1, 1.0, "Zug 7")
h.parkedBuy()
h.tick(); h.tick()
h.setName("a", "a:7", "Zug%207", 1)
check("originator: its own VNAME (skipOrigin=1) sends nothing", h.nsent() == 0 and h.nretry() == 0)

print()
if fails:
    print("FAILED: " + ", ".join(fails))
    raise SystemExit(1)
print("all vehicle-name sync checks passed")
