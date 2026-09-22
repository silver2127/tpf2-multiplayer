"""Offline check (Lua 5.2): a save's companies record survives a session that
does not apply it. A world with four companies loaded in single player, or on a
machine whose player entity did not match the record, used to be re-saved as
{ mode = "coop" } -- every company gone from the save for good (2026-09-18).
companies.lua now keeps the loaded record (cmCarried) and cmSaveState writes it
back whenever the session itself is not in companies mode.

    python tools/company_carry_test.py
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
local K = setmetatable({ INSTANCE = "a", BASE = "" }, { __index = function() return nil end })
local CM = setmetatable({ ticks = 0 }, { __index = function() return function() return nil end end })
local logs = {}
local log = function(s) logs[#logs + 1] = s end
assert(load(SRC, "@companies.lua"))()(CM, K, log)
CM.cmLog = function(s) logs[#logs + 1] = s end
CM.cmApplyNames = function() end
CM.cmLocalSwitch = function() end
CM.cmRosterHas = function(cid) for _, c in ipairs(CM.cmRoster or {}) do if c == cid then return true end end return false end
CM.cmOpenFromCode = function() end
-- the stub CM answers every unknown key with a function: pin the fields the code tests for nil
CM.cmCarried = false; CM.cmCarriedNoted = false; CM.cmMode = "coop"; CM.cmMyCompany = nil; CM.cmSaved = false
CM.cmOriginCompany = {}; CM.cmPw = {}; CM.cmCompanyPid = {}; CM.cmName = {}; CM.cmOpen = {}; CM.cmFounded = {}; CM.cmFoundedCount = {}; CM.cmRoster = {}; CM.cmLive = false; CM.cmReady = false
-- the record a companies-mode session wrote into the save
local saved = { v = 1, mode = "companies", mine = 1, roster = { 1, 2, 3, 4 },
                origin = { a = 1, b = 2, c = 4 }, pid = { ["1"] = 227011, ["2"] = 339916, ["3"] = 363279, ["4"] = 363339 },
                pw = {}, names = { ["2"] = "Indomitable's company" }, open = {}, founded = {}, foundedCount = {} }
local T = {}
function T.fresh() return CM.cmSaveState().mode end
function T.load() CM.cmLoadState(saved) return CM.cmSaved ~= nil end
-- single player: no lobby file, and this machine's player entity is another one -> the record cannot be applied
function T.mismatch()
  api = { engine = { util = { getPlayer = function() return 111 end } } }
  CM.cmMode = "coop"; CM.cmMyCompany = nil
  CM.cmApplySaved()
  local st = CM.cmSaveState()
  return st.mode, #(st.roster or {}), st.names and st.names["2"] or "?", CM.cmMode
end
-- the machine whose player entity matches applies it: the live state is what is saved
function T.match()
  CM.cmLoadState(saved)
  api = { engine = { util = { getPlayer = function() return 227011 end } } }
  CM.cmMode = "coop"; CM.cmMyCompany = nil
  CM.cmApplySaved()
  local st = CM.cmSaveState()
  return st.mode, #(st.roster or {}), CM.cmMode, CM.cmMyCompany
end
-- a world that never had companies still saves coop
function T.plain()
  CM.cmCarried = false; CM.cmMode = "coop"; CM.cmMyCompany = nil
  return CM.cmSaveState().mode
end
function T.logs() return table.concat(logs, "\n") end
return T
''')
check("a world with no companies record saves coop", T.fresh() == "coop")
check("loading a companies record keeps it", T.load())
mode, n, name, live = T.mismatch()
check("player entity mismatch: the session stays coop", live == "coop")
check("... but the save keeps the four companies", mode == "companies" and n == 4 and name == "Indomitable's company", f"{mode} {n} {name}")
check("the mismatch is logged with the keep", "kept for the next save" in T.logs())
mode, n, live, mine = T.match()
check("a matching player entity applies the record", live == "companies" and mine == 1 and mode == "companies" and n == 4, f"{live} {mine} {mode} {n}")
check("a plain coop world still saves coop", T.plain() == "coop")
print("FAILED: " + ", ".join(fails) if fails else "ALL OK")
raise SystemExit(1 if fails else 0)
