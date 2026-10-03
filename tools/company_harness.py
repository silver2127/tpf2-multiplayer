"""Shared offline harness for the companies tests (Lua 5.2): machines running the
real companies.lua / shared_infra.lua / companies_gui.lua against a small fake
engine, and a lockstep in miniature that applies the same commands everywhere.
Imported by company_registry_test.py and company_rules_test.py.
"""
import os
import tempfile

import lupa.lua52 as lupa

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
MP = os.path.join(REPO, "mod", "mp_lockstep_1", "res", "scripts", "mp")
SRC = open(os.path.join(MP, "companies.lua"), encoding="utf-8").read()
SHARED = open(os.path.join(MP, "shared_infra.lua"), encoding="utf-8").read()
GUI = open(os.path.join(MP, "companies_gui.lua"), encoding="utf-8").read()
fails = []


def check(name, cond, extra=""):
    print(("ok   " if cond else "FAIL ") + name + (f"  ({extra})" if extra else ""))
    if not cond:
        fails.append(name)


# ---------------------------------------------------------------------------
# a machine: one Lua state, the real module, a fake engine
# ---------------------------------------------------------------------------
HARNESS = r'''
local SRC, SHARED, GUI, INSTANCE, BASE = ...
local W = { ents = {}, players = {}, nextPid = 900, human = nil, cmds = {}, names = {}, colors = {} }
local T = { W = W }
local CT = { PLAYER_OWNED = "PO", CONSTRUCTION = "CON", LINE = "LINE", TRANSPORT_VEHICLE = "TV", NAME = "NAME", STATION_GROUP = "SG", BASE_EDGE = "BE" }
local function owner(eid) local e = W.ents[eid]; return e and e.owner end
api = {
  type = {
    ComponentType = CT,
    JournalEntryCategory = { new = function() return {} end },
    JournalEntry = { new = function() return {} end },
    Vec3f = { new = function(x, y, z) return { x, y, z } end },
    SimpleProposal = { new = function() return { streetProposal = { edgesToRemove = {}, nodesToRemove = {} } } end },
    Context = { new = function() return {} end },
    enum = { TransportVehicleState = { IN_DEPOT = 2 } },
  },
  engine = {
    util = { getPlayer = function() return W.human end },
    entityExists = function(eid) return W.ents[eid] ~= nil or W.players[eid] ~= nil end,
    getComponent = function(eid, t)
      local e = W.ents[eid]
      if t == "PO" then return e and e.owner and { player = e.owner } or nil end
      if t == "CON" then return (e and e.kind == "CONSTRUCTION") and { depots = {}, fileName = e.file or "", transf = { [13] = e.x or 0, [14] = e.y or 0 } } or nil end
      if t == "LINE" then return (e and e.kind == "LINE") and {} or nil end
      if t == "TV" then return (e and e.kind == "VEHICLE") and { line = e.line or -1 } or nil end
      if t == "NAME" then return W.names[eid] and { name = W.names[eid] } or nil end
      if t == "BE" then return (e and e.kind == "BASE_EDGE") and { node0 = e.n0, node1 = e.n1 } or nil end
      return nil
    end,
    system = {
      lineSystem = { getLines = function() local t = {}; for id, e in pairs(W.ents) do if e.kind == "LINE" then t[#t + 1] = id end end; table.sort(t); return t end },
      streetSystem = { getNode2StreetEdgeMap = function() return {} end, getNode2TrackEdgeMap = function() return {} end },
      transportVehicleSystem = { getVehiclesWithState = function()
        local t = {}; for id, e in pairs(W.ents) do if e.kind == "VEHICLE" and e.parked then t[#t + 1] = id end end; table.sort(t); return t end },
    },
  },
  cmd = {
    make = {
      bookJournalEntry = function(pid, entry) return { op = "journal", pid = pid, amount = entry.amount, type = entry.category.type } end,
      setName = function(pid, name) return { op = "name", pid = pid, name = name } end,
      setColor = function(id, c) return { op = "color", id = id, c = c } end,
      sellVehicle = function(id) return { op = "sell", id = id } end,
      deleteLine = function(id) return { op = "dline", id = id } end,
      buildProposal = function(sp) return { op = "proposal", sp = sp } end,
    },
    sendCommand = function(cmd, cb)
      W.cmds[#W.cmds + 1] = cmd
      if cmd.op == "journal" then
        local p = W.players[cmd.pid]
        if p then
          p.balance = p.balance + cmd.amount
          if cmd.type == 0 then p.loan = p.loan + cmd.amount end
        end
      elseif cmd.op == "name" then W.names[cmd.pid] = cmd.name
      elseif cmd.op == "color" then W.colors[cmd.id] = cmd.c
      elseif cmd.op == "sell" then
        local e = W.ents[cmd.id]
        if not e or e.stuck then if cb then cb({}, false) end; return end
        local p = W.players[e.owner]; if p then p.balance = p.balance + (e.refund or 0) end
        W.ents[cmd.id] = nil
      elseif cmd.op == "dline" then W.ents[cmd.id] = nil
      elseif cmd.op == "proposal" then
        for _, eid in ipairs(cmd.sp.streetProposal.edgesToRemove) do W.ents[eid] = nil end
      end
      if cb then cb({}, true) end
    end,
  },
}
game = { interface = {
  getEntities = function(_, filter)
    local t = {}
    for id, e in pairs(W.ents) do if e.kind == filter.type and not e.parked then t[#t + 1] = id end end
    table.sort(t)
    return t
  end,
  addPlayer = function() W.nextPid = W.nextPid + 1; W.players[W.nextPid] = { balance = 0, loan = 0 }; return W.nextPid end,
  setMaximumLoan = function() end,
  getEntity = function(pid) return W.players[pid] end,
  setPlayer = function(eid, pid)
    if pid >= 1610612736 then pid = pid - 1610612736 end
    if W.ents[eid] then W.ents[eid].owner = pid end
  end,
  setBulldozeable = function() end,
  bulldoze = function(id)
    local e = W.ents[id]
    if not e then error("no entity") end
    if e.stuck then error("in use") end
    local p = W.players[e.owner]
    if p then p.balance = p.balance + (e.refund or 0) end
    W.ents[id] = nil
    return true
  end,
} }
local function esc(s) return (tostring(s or ""):gsub("[^%w%-%._~]", function(c) return string.format("%%%02X", c:byte()) end)) end
local function unesc(s) return (tostring(s or ""):gsub("%%(%x%x)", function(h) return string.char(tonumber(h, 16)) end)) end
package.preload["mp.shared_infra"] = function() return assert(load(SHARED, "@shared_infra.lua"))() end
local K = { INSTANCE = INSTANCE, BASE = BASE, IDENTITY_FILE = BASE .. "tpf2_instance.txt", JOURNAL_TRANSFER = 6, JOURNAL_LOAN = 0 }
local CM = { ticks = 0, peerSeen = true, escName = esc, unescName = unesc, expectedDemolish = {},
             conKey = function(x, y) return string.format("%.1f/%.1f", x, y) end }
local logs = {}
local log = function(s) logs[#logs + 1] = s end
T.CM, T.K, T.logs = CM, K, logs
T.sent = {}
CM.scheduleLocal = function(op, args)
  local c = { op = op }
  for k, v in pairs(args) do c[k] = v end
  T.sent[#T.sent + 1] = c
end
CM.expectColorEcho = function() end
assert(load(SRC, "@companies.lua"))()(CM, K, log)
assert(load(GUI, "@companies_gui.lua"))()(CM, K, log)
CM.cmLog = function(s) logs[#logs + 1] = s end

-- the world a save holds: players (pid -> balance/loan), entities (eid -> kind/owner), the save's human
function T.world(players, ents, human)
  for pid, p in pairs(players) do W.players[pid] = { balance = p.balance or 0, loan = p.loan or 0 } end
  for eid, e in pairs(ents) do W.ents[eid] = { kind = e.kind, owner = e.owner, line = e.line, parked = e.parked, file = e.file, x = e.x, y = e.y, refund = e.refund, stuck = e.stuck, n0 = e.n0, n1 = e.n1 } end
  W.human = human
end
function T.apply(c) CM.cmAttribute(c); if c.op:sub(1, 2) == "CM" then CM.execCompanyCmd(c) end; CM.cmLoadSwitchTick(); return c.company end
function T.take() local s = T.sent; T.sent = {}; return s end
function T.companyOf(letter) return CM.cmCompanyOfOrigin(letter) end
-- what company cid owns here and its wallet (by the local pid map)
function T.holdings(cid)
  local pid = CM.cmCompanyPid[cid]
  local owned = {}
  for eid, e in pairs(W.ents) do if e.owner == pid then owned[#owned + 1] = eid end end
  table.sort(owned)
  local p = W.players[pid] or {}
  return table.concat(owned, ","), p.balance, p.loan, pid
end
function T.dash() return table.concat(CM.cmDashLines(), "\n") end
function T.parseDash()
  local kv = {}
  for line in T.dash():gmatch("[^\n]+") do
    local k, v = line:match("^(%w+)=(.*)$")
    if k == "co" then kv.coList = kv.coList or {}; kv.coList[#kv.coList + 1] = v elseif k then kv[k] = v end
  end
  return CM.coGuiParse(kv)
end
return T
'''


def encode(c):
    """net.lua encodeCmd / decodeCmd, as a round trip"""
    parts = [f"op={c['op']}", f"origin={c['origin']}"]
    for k in sorted(c):
        if k in ("op", "origin"):
            continue
        v = c[k]
        parts.append(f"{k}=%.4f" % v if isinstance(v, (int, float)) and not isinstance(v, bool) else f"{k}={v}")
    out = {}
    for tok in " ".join(parts).split(" "):
        k, v = tok.split("=", 1)
        if k not in ("op", "origin", "name"):
            try:
                v = float(v)
            except ValueError:
                pass
        out[k] = v
    return out


class Machine:
    def __init__(self, letter, steam, name, players, lobby_mode="companies", chip=1):
        self.dir = tempfile.mkdtemp(prefix="cmtest_")
        base = self.dir + os.sep
        with open(base + "tpf2_instance.txt", "w") as f:
            f.write(f"{letter}\npid=1\nentity_owner_v1=1\n")
        if steam:
            with open(base + "tpf2_steam.txt", "w") as f:
                f.write(f"id={steam}\nport=1\nname={name}\n")
        with open(base + "mp_players.txt", "w") as f:
            f.write("".join(f"{l}={n}\n" for l, n in players.items()))
        with open(base + "mp_company_cfg.txt", "w") as f:
            f.write(f"{lobby_mode}\n{chip}\n")
        self.L = lupa.LuaRuntime(unpack_returned_tuples=True)
        self.T = self.L.execute(HARNESS, SRC, SHARED, GUI, letter, base)
        self.letter = letter

    def world(self, players, ents, human):
        lp = self.L.table_from({k: self.L.table_from(v) for k, v in players.items()})
        le = self.L.table_from({k: self.L.table_from(v) for k, v in ents.items()})
        self.T.world(lp, le, human)

    def load(self, record):
        self.T.CM.cmLoadState(to_lua(self.L, record))

    def boot(self):
        self.T.CM.cmEnsure()

    def join(self):
        self.T.CM.cmJoinTick(True)

    def cm(self):
        return self.T.CM

    def holdings(self, cid):
        return tuple(self.T.holdings(cid))


def to_lua(L, v):
    if isinstance(v, dict):
        t = L.table()
        for k, x in v.items():
            t[k] = to_lua(L, x)
        return t
    if isinstance(v, list):
        t = L.table()
        for i, x in enumerate(v):
            t[i + 1] = to_lua(L, x)
        return t
    return v


TOWN = {1: {"kind": "CONSTRUCTION"}}   # a town building: the world answers entity queries


def from_lua(v):
    """a Lua table -> Python (a save record crossing to another machine)"""
    if lupa.lua_type(v) == "table":
        keys = list(v.keys())
        if keys and all(isinstance(k, (int, float)) for k in keys) and sorted(keys) == list(range(1, len(keys) + 1)):
            return [from_lua(v[k]) for k in sorted(keys)]
        return {k: from_lua(v[k]) for k in keys}
    return v


class Session:
    """lockstep in miniature: every machine applies every command, in one order"""

    def __init__(self, machines):
        self.m = machines

    def pump(self, order=None):
        """collect what the machines scheduled, apply it everywhere (in `order` of
        letters when given), return the applied commands"""
        batch = []
        for mach in self.m:
            for c in mach.T.take().values():
                d = {k: v for k, v in c.items()}
                d["origin"] = mach.letter
                batch.append(encode(d))
        if order:
            batch.sort(key=lambda c: order.index(c["origin"]))
        companies = []
        for c in batch:
            per = []
            for mach in self.m:
                per.append(mach.T.apply(to_lua(mach.L, c)))
            companies.append(per)
        return batch, companies

    def request(self, mach, line):
        mach.cm().cmRequest(to_lua(mach.L, line.split(" ")))


def same_company_state(sess, label, cids):
    """every machine: the same owned entities (by company, through its own pid map) and wallet"""
    for cid in cids:
        views = []
        for mach in sess.m:
            owned, bal, loan, pid = mach.holdings(cid)
            views.append((owned, bal, loan))
        check(f"{label}: company {cid} owns the same and has the same wallet on every machine", len(set(views)) == 1, str(views))
