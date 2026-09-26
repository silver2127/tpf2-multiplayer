"""Offline check (Lua 5.2): the hotseat switch a loaded save asks for waits for a
world that answers, and a company whose saved player entity is still there is never
replaced by a fresh one.

2026-09-22, the 49,000-tile dedicated-server world: the joiner's load-time switch ran
on the first tick after the load, `getEntities` and the wallet read answered nothing,
and it moved "0 + 0 entities". Every company's assets stayed on the player entity the
save had, the joiner played an empty company, and `cmEnsure` then made a fresh AI
player for the company it had lost -- so the player could not reach his own company
again. Only the dash note recorded it.

    python tools/company_load_switch_test.py
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


L = lupa.LuaRuntime(unpack_returned_tuples=True)
L.globals().SRC = open(os.path.join(MP, "companies.lua"), encoding="utf-8").read()
L.globals().SHARED = open(os.path.join(MP, "shared_infra.lua"), encoding="utf-8").read()
T = L.execute(r'''
local function sink() return setmetatable({}, { __index = function() return sink() end, __call = function() return sink() end }) end
api = sink(); game = sink()
package.preload["mp.shared_infra"] = function() return assert(load(SHARED, "@shared_infra.lua"))() end
local K = setmetatable({ INSTANCE = "b", BASE = "" }, { __index = function() return nil end })
local CM = { ticks = 0 }
local logs = {}
local log = function(s) logs[#logs + 1] = s end
assert(load(SRC, "@companies.lua"))()(CM, K, log)
CM.cmLog = function(s) logs[#logs + 1] = s end
CM.cmApplyNames = function() end
CM.cmWritePerms = function() end
CM.cmOpenFromCode = function() end
CM.cmRosterHas = function(cid) for _, c in ipairs(CM.cmRoster or {}) do if c == cid then return true end end return false end
CM.cmCarried = false; CM.cmCarriedNoted = false; CM.cmMode = "coop"; CM.cmMyCompany = nil; CM.cmSaved = false
CM.cmOriginCompany = {}; CM.cmPw = {}; CM.cmCompanyPid = {}; CM.cmName = {}; CM.cmOpen = {}
CM.cmFounded = {}; CM.cmFoundedCount = {}; CM.cmRoster = {}; CM.cmLive = false; CM.cmReady = false
CM.cmSwitchWanted = false; CM.cmSwitchTries = 0; CM.cmSavedPid = false

-- what the switch did, and whether the world answers queries yet
local switched = {}
CM.cmLocalSwitch = function(cid) switched[#switched + 1] = cid; CM.cmMyCompany = cid; return true end
local answers = false
local added = 0
local alive = {}
local function world()
  api = { engine = {
            util = { getPlayer = function() return 227011 end },
            entityExists = function(eid) return alive[eid] == true end,
            system = { lineSystem = { getLines = function() return {} end } },
          } }
  game = { interface = {
             getEntities = function() return answers and { 5001 } or {} end,
             addPlayer = function() added = added + 1; return 900000 + added end,
             setMaximumLoan = function() end,
           } }
end

-- the record a co1 machine (the host) wrote: we are letter b, so company 2 is ours
local saved = { v = 1, mode = "companies", mine = 1, roster = { 1, 2 },
                origin = { a = 1, b = 2 }, pid = { ["1"] = 227011, ["2"] = 339916 },
                pw = {}, names = {}, open = {}, founded = {}, foundedCount = {} }

local T = {}
-- the load: the switch is wanted but must not have happened yet
function T.applied()
  world(); answers = false
  CM.cmLoadState(saved)
  CM.cmMode = "coop"; CM.cmMyCompany = nil; CM.cmLive = false; CM.cmReady = false
  CM.cmApplySaved()
  return #switched, CM.cmSwitchWanted, CM.cmMyCompany
end
-- ticks while the world answers nothing change nothing
function T.waits(n)
  for _ = 1, n do CM.cmLoadSwitchTick() end
  return #switched, CM.cmSwitchWanted
end
-- the tick after the world answers does the switch, once
function T.answers()
  answers = true
  CM.cmLoadSwitchTick()
  CM.cmLoadSwitchTick()
  return #switched, switched[1], CM.cmSwitchWanted
end
-- a world that never answers is not held for ever
function T.giveUp()
  switched = {}; answers = false
  CM.cmMyCompany = 1; CM.cmSwitchWanted = 2; CM.cmSwitchTries = CM.CM_SWITCH_WAIT_TICKS - 1
  CM.cmLoadSwitchTick()
  return #switched, CM.cmSwitchWanted
end
-- cmEnsure: the saved player entity is taken, not replaced by a fresh one
function T.keepsSavedPid()
  world(); alive = { [339916] = true }
  added = 0
  CM.cmReadConfig = function() end
  CM.cmMode = "companies"; CM.cmMyCompany = 2; CM.cmRoster = { 1, 2 }
  CM.cmCompanyPid = {}; CM.cmSavedPid = { [1] = 339916 }; CM.cmReady = false
  CM.cmEnsure()
  return CM.cmCompanyPid[1], added, CM.cmReady
end
-- a saved entity that is gone: a fresh player is still made
function T.goneSavedPid()
  world(); alive = {}
  added = 0
  CM.cmMode = "companies"; CM.cmMyCompany = 2; CM.cmRoster = { 1, 2 }
  CM.cmCompanyPid = {}; CM.cmSavedPid = { [1] = 339916 }; CM.cmReady = false
  CM.cmEnsure()
  return CM.cmCompanyPid[1], added
end
-- The secondary readiness signal must work even while construction queries fail.
function T.lineAnswer()
  world(); switched = {}; CM.cmMyCompany = 1; CM.cmSwitchWanted = 2; CM.cmSwitchTries = 0
  game.interface.getEntities = function() error("world loading") end
  api.engine.system.lineSystem.getLines = function() return { 7001 } end
  CM.cmLoadSwitchTick()
  return #switched, CM.cmSwitchWanted
end
function T.queryFailures()
  world(); switched = {}; CM.cmMyCompany = 1; CM.cmSwitchWanted = 2; CM.cmSwitchTries = 0
  game.interface.getEntities = function() error("world loading") end
  api.engine.system.lineSystem.getLines = function() error("world loading") end
  CM.cmLoadSwitchTick()
  return #switched, CM.cmSwitchWanted
end
function T.beforeTimeout()
  world(); answers = false; switched = {}
  CM.cmMyCompany = 1; CM.cmSwitchWanted = 2; CM.cmSwitchTries = CM.CM_SWITCH_WAIT_TICKS - 2
  CM.cmLoadSwitchTick()
  return #switched, CM.cmSwitchWanted
end
function T.alreadySelected()
  switched = {}; CM.cmMyCompany = 2; CM.cmSwitchWanted = 2
  CM.cmLoadSwitchTick()
  return #switched, CM.cmSwitchWanted
end
function T.logs() return table.concat(logs, "\n") end
return T
''')

n, wanted, mine = T.applied()
check("the load does not switch on the spot", n == 0, f"{n} switches")
check("... it asks for the switch instead", wanted == 2, str(wanted))
check("... and the session is the company the save was written for until then", mine == 1, str(mine))

n, wanted = T.waits(5)
check("a world that answers nothing does not get the switch", n == 0 and wanted == 2, f"{n} {wanted}")
check("the wait is logged", "waits for a world that answers" in T.logs())

n, first, wanted = T.answers()
check("the tick after the world answers switches", n == 1 and first == 2, f"{n} {first}")
check("... and the request is cleared", wanted is None, str(wanted))

n, wanted = T.giveUp()
check("a world that never answers is not held for ever", n == 1, f"{n}")
check("... and says so", "answered nothing" in T.logs())

pid, added, ready = T.keepsSavedPid()
check("a company whose saved entity is alive keeps it", pid == 339916, str(pid))
check("... with no new player entity made", added == 0, f"{added} added")
check("... and companies mode comes up ready", ready is True, str(ready))

pid, added = T.goneSavedPid()
check("a saved entity that is gone is replaced", pid == 900001 and added == 1, f"{pid} {added}")

n, wanted = T.lineAnswer()
check("a line answers even when the construction query throws", n == 1 and wanted is None)
n, wanted = T.queryFailures()
check("failed queries keep the switch pending", n == 0 and wanted == 2)
n, wanted = T.beforeTimeout()
check("the tick before timeout still waits", n == 0 and wanted == 2)
n, wanted = T.alreadySelected()
check("an already selected company clears the request without switching", n == 0 and wanted is None)

print("FAILED: " + ", ".join(fails) if fails else "ALL PASS: the load-time switch waits for the world, and a live saved company entity is kept")
raise SystemExit(1 if fails else 0)
